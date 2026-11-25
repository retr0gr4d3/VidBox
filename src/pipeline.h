#pragma once
#include "common.h"
#include "decoder.h"
#include "kernels.h"
#include "vfx.h"

#include <array>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// An uncompressed or MJPEG frame from a capture card, in host memory.
enum class RawFormat { NV12, YUY2, UYVY, I420, RGB32, MJPG };

struct RawFrame {
  RawFormat format = RawFormat::NV12;
  const uint8_t* data = nullptr;
  size_t size = 0;
  int pitch = 0;            // bytes per row of the first plane (negative for bottom-up RGB32)
  int width = 0, height = 0;
  int fullRange = -1;       // -1 unknown, 0 limited, 1 full
  int bt709 = -1;           // -1 unknown, 0 BT.601, 1 BT.709
};

struct PipelineStats {
  double kvmFps = 0;        // frames received from the source (including repeats)
  double uniqueFps = 0;     // distinct frames (the game's real frame rate)
  double decodeMs = 0;
  double srMs = 0;
  int decodedW = 0, decodedH = 0;
  int outW = 0, outH = 0;
  bool hwDecode = false;
  std::string decoderName;  // "NVDEC", "software", "MJPEG (CPU)", "YUY2 (GPU)", ...
  std::string srState;      // "", "Preparing...", "Active", or an error
  double uniqueInterval = 0;
  double kvmInterval = 0;
  float lastDiff = 0;       // last mean luma difference (for tuning duplicate detection)
};

// Receives H.264 on the media thread, decodes on the GPU, drops repeated frames, optionally upscales,
// and keeps a short timestamped history of distinct frames for the renderer.
class Pipeline {
 public:
  static constexpr int kSlots = 10;

  struct Frame {
    int slot = -1;
    uint64_t id = 0;
    double ts = 0;        // smoothed presentation timestamp (seconds, NowSec clock)
    double arrival = 0;
    int width = 0, height = 0;
  };

  struct Pick {
    Frame a, b;
    bool hasA = false, hasB = false;
    float t = 0;          // 0..1 between a and b
    bool interpolate = false;
    bool starved = false; // content time ran past the newest frame
  };

  bool Init(int cudaDevice, std::string* err);
  void Shutdown();

  // Media thread.
  void OnPacket(const uint8_t* data, size_t len, bool keyframe, double arrival);
  // Capture thread (capture card mode). Same processing as OnPacket after decoding.
  void OnRawFrame(const RawFrame& f, double arrival);
  // Colour override for capture cards: range 0 auto, 1 limited, 2 full.
  void SetCaptureColorRange(int mode) { captureRange_ = mode; }
  void Reset();  // new stream: forget history timing

  // Render thread. Picked frames are pinned until Release().
  Pick PickAt(double contentTime, double maxGap);
  bool PickLatest(Frame* out);
  void Release(const Pick& p);
  void Release(const Frame& f);
  const GpuImage& SlotImage(int slot) const { return slots_[slot]; }

  PipelineStats Stats();
  double UniqueInterval();

 private:
  void ProcessDecoded(bool keyframe, double arrival, double decodeStart);
  bool ConvertRaw(const RawFrame& f, std::string* name);
  int AcquireFreeSlotLocked();
  void UpdateSuperRes(int inW, int inH);
  void SetSrState(const std::string& s) { std::lock_guard<std::mutex> l(statMu_); srState_ = s; }

  int device_ = 0;
  cudaStream_t decStream_ = nullptr;
  VideoDecoder decoder_;
  VideoDecoder mjpeg_;           // opened on first MJPEG frame
  uint8_t* rawStage_ = nullptr;  // device copy of an uncompressed capture frame
  size_t rawStageSize_ = 0;
  std::atomic<int> captureRange_{0};
  std::string decoderName_;
  GpuImage raw_, lastRaw_;
  bool lastRawValid_ = false;
  unsigned long long* dDiff_ = nullptr;
  unsigned long long* hDiff_ = nullptr;

  std::array<GpuImage, kSlots> slots_;
  std::array<int, kSlots> pins_{};
  std::vector<Frame> history_;   // sorted by ts, newest last
  std::mutex mu_;
  uint64_t nextId_ = 1;

  // cadence
  double lastAnyArrival_ = 0, lastUniqueArrival_ = 0, lastTs_ = 0;
  double kvmInterval_ = 1.0 / 60, uniqueInterval_ = 1.0 / 30;
  bool haveTs_ = false;

  // super resolution (initialized asynchronously; model load can take a while)
  std::unique_ptr<SuperRes> sr_;
  std::future<std::unique_ptr<SuperRes>> srPending_;
  int srPendingKey_[5] = {};
  std::string srState_;
  int srFailKey_[5] = {-1, -1, -1, -1, -1};

  // stats (guarded by statMu_: written on the media thread, read on the render thread)
  std::mutex statMu_;
  RateCounter kvmRate_, uniqueRate_;
  Ema decodeMs_, srMs_;
  int decodedW_ = 0, decodedH_ = 0, outW_ = 0, outH_ = 0;
  float lastDiff_ = 0;
};
