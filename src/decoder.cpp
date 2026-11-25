#include "decoder.h"
#include "common.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
}

static enum AVPixelFormat GetHwFormat(AVCodecContext*, const enum AVPixelFormat* fmts) {
  for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
    if (*p == AV_PIX_FMT_CUDA) return *p;
  LogF("decoder: CUDA surface format not offered, using software");
  return fmts[0];
}

VideoDecoder::~VideoDecoder() { Shutdown(); }

void VideoDecoder::Shutdown() {
  if (ctx_) avcodec_free_context(&ctx_);
  if (hwDevice_) av_buffer_unref(&hwDevice_);
  if (frame_) av_frame_free(&frame_);
  if (pkt_) av_packet_free(&pkt_);
  if (stage_) { cudaFree(stage_); stage_ = nullptr; stageSize_ = 0; }
}

bool VideoDecoder::Init(int cudaDevice, cudaStream_t stream, std::string* err, Codec codec) {
  device_ = cudaDevice;
  stream_ = stream;
  codec_ = codec;
  if (!frame_) frame_ = av_frame_alloc();
  if (!pkt_) pkt_ = av_packet_alloc();
  if (codec == Codec::Mjpeg) {
    // MJPEG from capture cards is often 4:2:2, which NVDEC paths don't reliably return as NV12.
    if (OpenCodec(false)) return true;
    if (err) *err = "Could not open an MJPEG decoder";
    return false;
  }
  if (OpenCodec(true)) return true;
  LogF("decoder: NVDEC unavailable, falling back to software decode");
  if (OpenCodec(false)) return true;
  if (err) *err = "Could not open an H.264 decoder";
  return false;
}

bool VideoDecoder::OpenCodec(bool useHw) {
  if (ctx_) avcodec_free_context(&ctx_);
  if (hwDevice_) av_buffer_unref(&hwDevice_);
  hw_ = false;

  const AVCodec* codec = avcodec_find_decoder(codec_ == Codec::Mjpeg ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_H264);
  if (!codec) return false;
  ctx_ = avcodec_alloc_context3(codec);
  ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
  ctx_->err_recognition = 0;

  if (useHw) {
    static bool listed = false;
    if (!listed) {
      listed = true;
      std::string types;
      for (AVHWDeviceType t = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE); t != AV_HWDEVICE_TYPE_NONE;
           t = av_hwdevice_iterate_types(t))
        types += std::string(av_hwdevice_get_type_name(t)) + " ";
      LogF("decoder: FFmpeg %s, hw device types: %s", av_version_info(), types.c_str());
    }
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "primary_ctx", "1", 0);  // share the CUDA primary context with our kernels
    std::string dev = std::to_string(device_);
    int r = av_hwdevice_ctx_create(&hwDevice_, AV_HWDEVICE_TYPE_CUDA, dev.c_str(), opts, 0);
    av_dict_free(&opts);
    if (r < 0) {
      // A separate context would make NVDEC surfaces unreadable from our kernels; use software instead.
      char e[128];
      av_strerror(r, e, sizeof(e));
      LogF("decoder: CUDA primary context unavailable (%s)", e);
      avcodec_free_context(&ctx_);
      return false;
    }
    ctx_->hw_device_ctx = av_buffer_ref(hwDevice_);
    ctx_->get_format = GetHwFormat;
    ctx_->extra_hw_frames = 2;
    ctx_->thread_count = 1;
  } else {
    ctx_->thread_count = 4;
    ctx_->thread_type = FF_THREAD_SLICE;  // frame threading would add latency
  }
  if (avcodec_open2(ctx_, codec, nullptr) < 0) {
    avcodec_free_context(&ctx_);
    if (hwDevice_) av_buffer_unref(&hwDevice_);
    return false;
  }
  hw_ = useHw;
  LogF("decoder: opened %s", useHw ? "NVDEC (CUDA)" : "software");
  return true;
}

bool VideoDecoder::Decode(const uint8_t* data, size_t len, GpuImage& out) {
  if (!ctx_) return false;
  pkt_->data = const_cast<uint8_t*>(data);
  pkt_->size = int(len);
  int r = avcodec_send_packet(ctx_, pkt_);
  av_packet_unref(pkt_);
  if (r < 0 && r != AVERROR(EAGAIN)) {
    if (++errorsInRow_ % 30 == 1) LogF("decoder: send_packet error %d", r);
  }
  bool got = false;
  for (;;) {
    r = avcodec_receive_frame(ctx_, frame_);
    if (r < 0) break;
    errorsInRow_ = 0;
    got = ConvertFrame(frame_, out) || got;
    av_frame_unref(frame_);
  }
  return got;
}

bool VideoDecoder::ConvertFrame(AVFrame* f, GpuImage& out) {
  int w = f->width, h = f->height;
  if (!out.Alloc(w, h)) return false;
  ColorParams cp;
  cp.fullRange = f->color_range == AVCOL_RANGE_JPEG;
  switch (f->colorspace) {
    case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: case AVCOL_SPC_FCC: cp.bt709 = false; break;
    case AVCOL_SPC_BT709: cp.bt709 = true; break;
    default: cp.bt709 = h >= 720; break;
  }
  bool jpegRange = f->format == AV_PIX_FMT_YUVJ420P || f->format == AV_PIX_FMT_YUVJ422P;
  if (jpegRange) cp.fullRange = true;
  if (codec_ == Codec::Mjpeg && f->colorspace == AVCOL_SPC_UNSPECIFIED) cp.bt709 = false;  // JFIF is BT.601
  if (forceFull_ >= 0) cp.fullRange = forceFull_ != 0;
  if (force709_ >= 0) cp.bt709 = force709_ != 0;

  if (f->format == AV_PIX_FMT_CUDA) {
    AVHWFramesContext* fc = reinterpret_cast<AVHWFramesContext*>(f->hw_frames_ctx->data);
    if (fc->sw_format != AV_PIX_FMT_NV12) {
      static bool warned = false;
      if (!warned) { LogF("decoder: unsupported CUDA sw_format %s", av_get_pix_fmt_name(fc->sw_format)); warned = true; }
      return false;
    }
    LaunchNv12ToRgba(f->data[0], f->linesize[0], f->data[1], f->linesize[1], out, cp, stream_);
    // The surface goes back to FFmpeg's pool after this returns, so finish reading it now.
    cudaStreamSynchronize(stream_);
    return true;
  }

  // Software frame: upload planes, then convert on the GPU.
  if (f->format == AV_PIX_FMT_YUV422P || f->format == AV_PIX_FMT_YUVJ422P) {
    int cw = (w + 1) / 2;
    size_t ySize = size_t(w) * h, cSize = size_t(cw) * h;
    size_t need = ySize + cSize * 2;
    if (stageSize_ < need) {
      if (stage_) cudaFree(stage_);
      if (cudaMalloc(&stage_, need) != cudaSuccess) { stage_ = nullptr; stageSize_ = 0; return false; }
      stageSize_ = need;
    }
    uint8_t* dy = stage_;
    uint8_t* du = stage_ + ySize;
    uint8_t* dv = du + cSize;
    cudaMemcpy2DAsync(dy, w, f->data[0], f->linesize[0], w, h, cudaMemcpyHostToDevice, stream_);
    cudaMemcpy2DAsync(du, cw, f->data[1], f->linesize[1], cw, h, cudaMemcpyHostToDevice, stream_);
    cudaMemcpy2DAsync(dv, cw, f->data[2], f->linesize[2], cw, h, cudaMemcpyHostToDevice, stream_);
    LaunchI422ToRgba(dy, w, du, cw, dv, cw, out, cp, stream_);
    cudaStreamSynchronize(stream_);
    return true;
  }
  if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P || f->format == AV_PIX_FMT_NV12) {
    bool nv12 = f->format == AV_PIX_FMT_NV12;
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    size_t ySize = size_t(w) * h;
    size_t cSize = nv12 ? size_t(cw) * 2 * ch : size_t(cw) * ch;
    size_t need = ySize + cSize * (nv12 ? 1 : 2);
    if (stageSize_ < need) {
      if (stage_) cudaFree(stage_);
      if (cudaMalloc(&stage_, need) != cudaSuccess) { stage_ = nullptr; stageSize_ = 0; return false; }
      stageSize_ = need;
    }
    uint8_t* dy = stage_;
    uint8_t* d1 = stage_ + ySize;
    cudaMemcpy2DAsync(dy, w, f->data[0], f->linesize[0], w, h, cudaMemcpyHostToDevice, stream_);
    if (nv12) {
      cudaMemcpy2DAsync(d1, cw * 2, f->data[1], f->linesize[1], cw * 2, ch, cudaMemcpyHostToDevice, stream_);
      LaunchNv12ToRgba(dy, w, d1, cw * 2, out, cp, stream_);
    } else {
      uint8_t* d2 = d1 + cSize;
      cudaMemcpy2DAsync(d1, cw, f->data[1], f->linesize[1], cw, ch, cudaMemcpyHostToDevice, stream_);
      cudaMemcpy2DAsync(d2, cw, f->data[2], f->linesize[2], cw, ch, cudaMemcpyHostToDevice, stream_);
      LaunchI420ToRgba(dy, w, d1, cw, d2, cw, out, cp, stream_);
    }
    cudaStreamSynchronize(stream_);
    return true;
  }
  static bool warned = false;
  if (!warned) { LogF("decoder: unsupported pixel format %s", av_get_pix_fmt_name(AVPixelFormat(f->format))); warned = true; }
  return false;
}
