#pragma once
#include <cuda_runtime.h>
#include <cstdint>

// A pitch-linear RGBA8 image in CUDA device memory.
struct GpuImage {
  uint8_t* ptr = nullptr;
  size_t pitch = 0;
  int width = 0, height = 0;
  bool Alloc(int w, int h);
  void Free();
  bool Valid() const { return ptr != nullptr; }
};

struct ColorParams {
  bool bt709 = true;      // else BT.601
  bool fullRange = false;
};

// NV12 (Y plane + interleaved UV plane) -> RGBA.
void LaunchNv12ToRgba(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, GpuImage& out, ColorParams cp,
                      cudaStream_t s);
// I420 (three planes) -> RGBA.
void LaunchI420ToRgba(const uint8_t* y, int yPitch, const uint8_t* u, int uPitch, const uint8_t* v, int vPitch,
                      GpuImage& out, ColorParams cp, cudaStream_t s);
// I422 (three planes, chroma at full height) -> RGBA. Used for software-decoded MJPEG.
void LaunchI422ToRgba(const uint8_t* y, int yPitch, const uint8_t* u, int uPitch, const uint8_t* v, int vPitch,
                      GpuImage& out, ColorParams cp, cudaStream_t s);
// Packed 4:2:2 (YUY2 when uyvy=false, UYVY when true) -> RGBA.
void LaunchPacked422ToRgba(const uint8_t* src, int pitch, bool uyvy, GpuImage& out, ColorParams cp, cudaStream_t s);
// 32-bit BGRX (Media Foundation RGB32) -> RGBA. bottomUp flips rows.
void LaunchBgraToRgba(const uint8_t* src, int pitch, bool bottomUp, GpuImage& out, cudaStream_t s);

// Compares two RGBA images on a sparse grid. Results are written to device memory `result`
// (2 x uint64: sum of |luma diff|, number of samples whose diff > 12), plus the sample count.
int LaunchFrameDiff(const GpuImage& a, const GpuImage& b, unsigned long long* result, cudaStream_t s);
