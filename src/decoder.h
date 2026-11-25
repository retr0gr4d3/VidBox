#pragma once
#include "kernels.h"

#include <string>

struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;
struct AVPacket;

// Video decoder: FFmpeg + NVDEC (CUDA hwaccel) for H.264 from the KVM, software MJPEG for capture cards.
// Output is always an RGBA GpuImage on the CUDA device shared with D3D11.
class VideoDecoder {
 public:
  enum class Codec { H264, Mjpeg };
  ~VideoDecoder();
  bool Init(int cudaDevice, cudaStream_t stream, std::string* err, Codec codec = Codec::H264);
  bool IsOpen() const { return ctx_ != nullptr; }
  // Overrides the stream's colour range/matrix (capture devices often don't signal them). <0 = keep.
  void SetColorOverride(int fullRange, int bt709) { forceFull_ = fullRange; force709_ = bt709; }
  void Shutdown();
  // Feeds one access unit. Returns true if a picture was written to `out`.
  bool Decode(const uint8_t* data, size_t len, GpuImage& out);
  bool HardwareAccelerated() const { return hw_; }

 private:
  bool OpenCodec(bool useHw);
  bool ConvertFrame(AVFrame* f, GpuImage& out);

  Codec codec_ = Codec::H264;
  int forceFull_ = -1, force709_ = -1;
  int device_ = 0;
  cudaStream_t stream_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVBufferRef* hwDevice_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  bool hw_ = false;
  int errorsInRow_ = 0;
  // Staging planes for software-decoded frames.
  uint8_t* stage_ = nullptr;
  size_t stageSize_ = 0;
};
