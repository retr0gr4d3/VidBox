#pragma once
#include "kernels.h"
#include "nvCVImage.h"

#include <string>

typedef struct NvVFX_Object* NvVFX_Handle;

// Loads NVVideoEffects.dll from the VFX SDK. Returns false if the SDK is missing.
bool VfxInitLibrary(std::string* sdkDirOut);
const std::string& VfxSdkDir();
std::string VfxStatusString(int status);
void VfxProbeFrameGen(int W, int H, cudaStream_t stream);

// NVIDIA Video Frame Generation: synthesizes a frame between two inputs at timestep t in (0,1).
class FrameGen {
 public:
  ~FrameGen();
  bool Init(int width, int height, int quality, cudaStream_t stream, std::string* err);
  void Shutdown();
  bool Ready() const { return fx_ != nullptr; }
  int Width() const { return width_; }
  int Height() const { return height_; }
  int Quality() const { return quality_; }
  // Generates into the internal output buffer; returns a pointer to it (valid until the next Run).
  const GpuImage* Run(const GpuImage& prev, const GpuImage& next, float t, bool shotChange);

 private:
  NvVFX_Handle fx_ = nullptr;
  cudaStream_t stream_ = nullptr;
  GpuImage out_;
  // VFG keeps pointers to these descriptors, so they must outlive every Run.
  NvCVImage in0_{}, in1_{}, outIm_{};
  int width_ = 0, height_ = 0, quality_ = -1;
  int failures_ = 0;
};

// NVIDIA Video Super Resolution (upscaling + optional artifact reduction modes).
struct VsrModeInfo { int value; const char* name; bool sameSize; };
extern const VsrModeInfo kVsrModes[];
extern const int kVsrModeCount;

class SuperRes {
 public:
  ~SuperRes();
  bool Init(int inW, int inH, int outW, int outH, int mode, cudaStream_t stream, std::string* err);
  void Shutdown();
  bool Ready() const { return fx_ != nullptr; }
  bool Matches(int inW, int inH, int outW, int outH, int mode) const {
    return fx_ && inW == inW_ && inH == inH_ && outW == outW_ && outH == outH_ && mode == mode_;
  }
  bool Run(const GpuImage& in, GpuImage& out);

 private:
  NvVFX_Handle fx_ = nullptr;
  cudaStream_t stream_ = nullptr;
  int inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0, mode_ = -1;
  NvCVImage inIm_{}, outIm_{};
};
