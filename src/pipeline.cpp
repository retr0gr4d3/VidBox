#include "pipeline.h"
#include "common.h"
#include "settings.h"

#include <algorithm>
#include <cmath>

bool Pipeline::Init(int cudaDevice, std::string* err) {
  device_ = cudaDevice;
  cudaSetDevice(device_);
  if (cudaStreamCreate(&decStream_) != cudaSuccess) { if (err) *err = "cudaStreamCreate failed"; return false; }
  cudaMalloc(&dDiff_, sizeof(unsigned long long) * 2);
  cudaMallocHost(&hDiff_, sizeof(unsigned long long) * 2);
  return decoder_.Init(device_, decStream_, err);
}

void Pipeline::Shutdown() {
  if (srPending_.valid()) srPending_.wait();
  sr_.reset();
  decoder_.Shutdown();
  mjpeg_.Shutdown();
  if (rawStage_) cudaFree(rawStage_);
  rawStage_ = nullptr;
  rawStageSize_ = 0;
  raw_.Free();
  lastRaw_.Free();
  for (auto& s : slots_) s.Free();
  if (dDiff_) cudaFree(dDiff_);
  if (hDiff_) cudaFreeHost(hDiff_);
  dDiff_ = nullptr;
  hDiff_ = nullptr;
  if (decStream_) cudaStreamDestroy(decStream_);
  decStream_ = nullptr;
}

void Pipeline::Reset() {
  std::lock_guard<std::mutex> lk(mu_);
  haveTs_ = false;
  lastRawValid_ = false;
}

int Pipeline::AcquireFreeSlotLocked() {
  for (;;) {
    for (int i = 0; i < kSlots; ++i) {
      if (pins_[i] != 0) continue;
      bool used = false;
      for (auto& f : history_) if (f.slot == i) { used = true; break; }
      if (!used) return i;
    }
    // Everything is in use: drop the oldest unpinned history entry.
    auto it = std::find_if(history_.begin(), history_.end(), [&](const Frame& f) { return pins_[f.slot] == 0; });
    if (it == history_.end()) return -1;
    history_.erase(it);
  }
}

void Pipeline::UpdateSuperRes(int inW, int inH) {
  const Settings& s = GetSettings();
  // Collect a finished async init.
  if (srPending_.valid() && srPending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto sr = srPending_.get();
    if (sr && sr->Ready()) { sr_ = std::move(sr); SetSrState("Active"); }
    else {
      memcpy(srFailKey_, srPendingKey_, sizeof(srFailKey_));
      SetSrState("Failed to start (see log)");
    }
  }
  if (!s.srEnabled) {
    if (sr_) { sr_.reset(); }
    SetSrState({});
    return;
  }
  int mode = kVsrModes[std::clamp(s.srMode, 0, kVsrModeCount - 1)].value;
  bool sameSize = kVsrModes[std::clamp(s.srMode, 0, kVsrModeCount - 1)].sameSize;
  int outH = inH;
  if (!sameSize) {
    switch (s.srTarget) {
      case SrTarget::P1440: outH = 1440; break;
      case SrTarget::P2160: outH = 2160; break;
      case SrTarget::Window: outH = 2160; break;
    }
    if (outH <= inH) outH = inH;  // never downscale
  }
  int outW = sameSize ? inW : int(std::lround(double(inW) * outH / inH)) & ~1;
  int key[5] = {inW, inH, outW, outH, mode};
  if (sr_ && sr_->Matches(inW, inH, outW, outH, mode)) return;
  if (memcmp(key, srFailKey_, sizeof(key)) == 0) return;  // already failed with these settings
  if (srPending_.valid()) return;                          // init in progress
  sr_.reset();
  memcpy(srPendingKey_, key, sizeof(key));
  SetSrState("Preparing...");
  cudaStream_t stream = decStream_;
  int dev = device_;
  srPending_ = std::async(std::launch::async, [=]() {
    cudaSetDevice(dev);
    auto sr = std::make_unique<SuperRes>();
    std::string err;
    if (!sr->Init(inW, inH, outW, outH, mode, stream, &err)) {
      LogF("super resolution init failed: %s", err.c_str());
      return std::unique_ptr<SuperRes>();
    }
    return sr;
  });
}

void Pipeline::OnPacket(const uint8_t* data, size_t len, bool keyframe, double arrival) {
  cudaSetDevice(device_);
  double t0 = NowSec();
  if (!decoder_.Decode(data, len, raw_)) return;
  {
    std::lock_guard<std::mutex> sl(statMu_);
    decoderName_ = decoder_.HardwareAccelerated() ? "NVDEC" : "software";
  }
  ProcessDecoded(keyframe, arrival, t0);
}

bool Pipeline::ConvertRaw(const RawFrame& f, std::string* name) {
  int w = f.width, h = f.height;
  int mode = captureRange_.load();
  int range = mode == 1 ? 0 : mode == 2 ? 1 : f.fullRange;
  if (f.format == RawFormat::MJPG) {
    if (!mjpeg_.IsOpen()) {
      std::string err;
      if (!mjpeg_.Init(device_, decStream_, &err, VideoDecoder::Codec::Mjpeg)) {
        LogF("capture: %s", err.c_str());
        return false;
      }
    }
    mjpeg_.SetColorOverride(mode == 0 ? -1 : range, -1);
    *name = "MJPEG (CPU decode)";
    return mjpeg_.Decode(f.data, f.size, raw_);
  }
  if (!raw_.Alloc(w, h)) return false;
  ColorParams cp;
  cp.fullRange = range == 1;
  cp.bt709 = f.bt709 >= 0 ? f.bt709 != 0 : h >= 720;
  int pitch = f.pitch < 0 ? -f.pitch : f.pitch;
  size_t need = f.size;
  if (rawStageSize_ < need) {
    if (rawStage_) cudaFree(rawStage_);
    if (cudaMalloc(&rawStage_, need) != cudaSuccess) { rawStage_ = nullptr; rawStageSize_ = 0; return false; }
    rawStageSize_ = need;
  }
  cudaMemcpyAsync(rawStage_, f.data, need, cudaMemcpyHostToDevice, decStream_);
  const uint8_t* d = rawStage_;
  switch (f.format) {
    case RawFormat::NV12:
      if (need < size_t(pitch) * h * 3 / 2) return false;
      LaunchNv12ToRgba(d, pitch, d + size_t(pitch) * h, pitch, raw_, cp, decStream_);
      *name = "NV12 (GPU)";
      break;
    case RawFormat::I420: {
      if (need < size_t(pitch) * h * 3 / 2) return false;
      int cp2 = pitch / 2;
      const uint8_t* u = d + size_t(pitch) * h;
      const uint8_t* v = u + size_t(cp2) * ((h + 1) / 2);
      LaunchI420ToRgba(d, pitch, u, cp2, v, cp2, raw_, cp, decStream_);
      *name = "I420 (GPU)";
      break;
    }
    case RawFormat::YUY2:
    case RawFormat::UYVY:
      if (need < size_t(pitch) * h) return false;
      LaunchPacked422ToRgba(d, pitch, f.format == RawFormat::UYVY, raw_, cp, decStream_);
      *name = f.format == RawFormat::UYVY ? "UYVY (GPU)" : "YUY2 (GPU)";
      break;
    case RawFormat::RGB32:
      if (need < size_t(pitch) * h) return false;
      LaunchBgraToRgba(d, pitch, f.pitch < 0, raw_, decStream_);
      *name = "RGB32 (GPU)";
      break;
    default:
      return false;
  }
  cudaStreamSynchronize(decStream_);
  return true;
}

void Pipeline::OnRawFrame(const RawFrame& f, double arrival) {
  cudaSetDevice(device_);
  double t0 = NowSec();
  std::string name;
  if (!ConvertRaw(f, &name)) return;
  {
    std::lock_guard<std::mutex> sl(statMu_);
    decoderName_ = name;
  }
  // Uncompressed frames carry no coding noise, so every frame is treated like a non-keyframe.
  ProcessDecoded(false, arrival, t0);
}

void Pipeline::ProcessDecoded(bool keyframe, double arrival, double t0) {
  double t1 = NowSec();
  {
    std::lock_guard<std::mutex> sl(statMu_);
    decodeMs_.Add((t1 - t0) * 1000, 0.05);
    kvmRate_.Tick(t1);
    decodedW_ = raw_.width;
    decodedH_ = raw_.height;
  }

  // KVM capture cadence (all frames).
  if (lastAnyArrival_ > 0) {
    double d = arrival - lastAnyArrival_;
    if (d > 0.002 && d < 0.2) kvmInterval_ += 0.02 * (d - kvmInterval_);
  }
  lastAnyArrival_ = arrival;

  const Settings& s = GetSettings();
  bool sizeChanged = !lastRawValid_ || lastRaw_.width != raw_.width || lastRaw_.height != raw_.height;
  if (s.dupDetection && !sizeChanged) {
    int samples = LaunchFrameDiff(raw_, lastRaw_, dDiff_, decStream_);
    cudaMemcpyAsync(hDiff_, dDiff_, sizeof(unsigned long long) * 2, cudaMemcpyDeviceToHost, decStream_);
    cudaStreamSynchronize(decStream_);
    double mean = samples ? double(hDiff_[0]) / samples : 0.0;
    double changed = samples ? double(hDiff_[1]) / samples : 0.0;
    lastDiff_ = float(mean);
    // Keyframes of a repeated picture are re-quantized, so allow them a little more difference.
    double thr = keyframe ? s.dupThreshold * 2.0 : s.dupThreshold;
    if (mean < thr && changed < (keyframe ? 0.01 : 0.002)) return;  // repeated frame: the game didn't render a new one
  }
  std::swap(raw_, lastRaw_);
  lastRawValid_ = true;
  { std::lock_guard<std::mutex> sl(statMu_); uniqueRate_.Tick(t1); }

  UpdateSuperRes(lastRaw_.width, lastRaw_.height);
  int outW = lastRaw_.width, outH = lastRaw_.height;
  bool useSr = sr_ && sr_->Ready();
  if (useSr) {
    // The SR object knows its output size; derive it from the settings key it was created with.
    outW = srPendingKey_[2];
    outH = srPendingKey_[3];
  }

  int slot;
  {
    std::lock_guard<std::mutex> lk(mu_);
    slot = AcquireFreeSlotLocked();
  }
  if (slot < 0) return;
  GpuImage& dst = slots_[slot];
  if (!dst.Alloc(outW, outH)) return;

  double t2 = NowSec();
  bool srOk = false;
  if (useSr) srOk = sr_->Run(lastRaw_, dst);
  if (!srOk) {
    if (dst.width != lastRaw_.width || dst.height != lastRaw_.height) {
      if (!dst.Alloc(lastRaw_.width, lastRaw_.height)) return;
    }
    cudaMemcpy2DAsync(dst.ptr, dst.pitch, lastRaw_.ptr, lastRaw_.pitch, size_t(dst.width) * 4, dst.height,
                      cudaMemcpyDeviceToDevice, decStream_);
  }
  cudaStreamSynchronize(decStream_);
  {
    std::lock_guard<std::mutex> sl(statMu_);
    if (srOk) srMs_.Add((NowSec() - t2) * 1000, 0.05);
    outW_ = dst.width;
    outH_ = dst.height;
  }

  std::lock_guard<std::mutex> lk(mu_);
  // Smoothed timestamp locked to the KVM capture cadence, so 30 fps content lands on exact 2-frame steps
  // even though packets arrive with network jitter.
  double ts;
  double rawGap = arrival - lastUniqueArrival_;
  if (!haveTs_ || rawGap > 0.25 || rawGap <= 0) {
    ts = arrival;
    haveTs_ = true;
  } else {
    double period = std::clamp(kvmInterval_, 1.0 / 240, 1.0 / 15);
    double n = std::max(1.0, std::round(rawGap / period));
    double step = n * period;
    ts = lastTs_ + step;
    double e = arrival - ts;
    if (std::fabs(e) > 0.06) ts = arrival;  // lost lock: resync
    else ts += 0.05 * e;                    // slow drift correction
    if (ts <= lastTs_) ts = lastTs_ + 0.001;
    uniqueInterval_ += 0.1 * (std::min(step, 0.1) - uniqueInterval_);
  }
  lastTs_ = ts;
  lastUniqueArrival_ = arrival;

  Frame f;
  f.slot = slot;
  f.id = nextId_++;
  f.ts = ts;
  f.arrival = arrival;
  f.width = dst.width;
  f.height = dst.height;
  history_.push_back(f);
  while (history_.size() > 6) {
    auto it = std::find_if(history_.begin(), history_.end(), [&](const Frame& x) { return pins_[x.slot] == 0; });
    if (it == history_.end()) break;
    history_.erase(it);
  }
}

Pipeline::Pick Pipeline::PickAt(double s, double maxGap) {
  Pick p;
  std::lock_guard<std::mutex> lk(mu_);
  if (history_.empty()) return p;
  int ia = -1;
  for (int i = 0; i < int(history_.size()); ++i) {
    if (history_[i].ts <= s) ia = i;
    else break;
  }
  if (ia < 0) {
    // Content time is before everything we have (just started / resynced): show the oldest.
    p.a = history_.front();
    p.hasA = true;
  } else {
    p.a = history_[ia];
    p.hasA = true;
    if (ia + 1 < int(history_.size())) {
      p.b = history_[ia + 1];
      p.hasB = true;
      double span = p.b.ts - p.a.ts;
      p.t = float(std::clamp((s - p.a.ts) / span, 0.0, 1.0));
      p.interpolate = span <= maxGap && p.a.width == p.b.width && p.a.height == p.b.height;
    } else {
      p.starved = true;
    }
  }
  if (p.hasA) pins_[p.a.slot]++;
  if (p.hasB) pins_[p.b.slot]++;
  return p;
}

bool Pipeline::PickLatest(Frame* out) {
  std::lock_guard<std::mutex> lk(mu_);
  if (history_.empty()) return false;
  *out = history_.back();
  pins_[out->slot]++;
  return true;
}

void Pipeline::Release(const Pick& p) {
  std::lock_guard<std::mutex> lk(mu_);
  if (p.hasA) pins_[p.a.slot]--;
  if (p.hasB) pins_[p.b.slot]--;
}

void Pipeline::Release(const Frame& f) {
  std::lock_guard<std::mutex> lk(mu_);
  if (f.slot >= 0) pins_[f.slot]--;
}

double Pipeline::UniqueInterval() {
  std::lock_guard<std::mutex> lk(mu_);
  return uniqueInterval_;
}

PipelineStats Pipeline::Stats() {
  double now = NowSec();
  PipelineStats st;
  std::lock_guard<std::mutex> sl(statMu_);
  st.kvmFps = kvmRate_.Rate(now);
  st.uniqueFps = uniqueRate_.Rate(now);
  st.decodeMs = decodeMs_.value;
  st.srMs = srMs_.value;
  st.decodedW = decodedW_;
  st.decodedH = decodedH_;
  st.outW = outW_;
  st.outH = outH_;
  st.hwDecode = decoder_.HardwareAccelerated();
  st.decoderName = decoderName_;
  st.srState = srState_;
  st.lastDiff = lastDiff_;
  std::lock_guard<std::mutex> lk(mu_);
  st.uniqueInterval = uniqueInterval_;
  st.kvmInterval = kvmInterval_;
  return st;
}
