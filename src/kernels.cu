#include "kernels.h"

bool GpuImage::Alloc(int w, int h) {
  if (ptr && width == w && height == h) return true;
  Free();
  if (cudaMallocPitch(reinterpret_cast<void**>(&ptr), &pitch, size_t(w) * 4, size_t(h)) != cudaSuccess) {
    ptr = nullptr;
    return false;
  }
  width = w;
  height = h;
  return true;
}

void GpuImage::Free() {
  if (ptr) cudaFree(ptr);
  ptr = nullptr;
  pitch = 0;
  width = height = 0;
}

struct Coeffs { float yScale, yOff, rv, gu, gv, bu, cScale; };

__host__ __device__ static Coeffs MakeCoeffs(ColorParams cp) {
  Coeffs c;
  if (cp.fullRange) { c.yScale = 1.0f; c.yOff = 0.0f; c.cScale = 1.0f; }
  else { c.yScale = 255.0f / 219.0f; c.yOff = 16.0f; c.cScale = 255.0f / 224.0f; }
  if (cp.bt709) { c.rv = 1.5748f; c.gu = -0.1873f; c.gv = -0.4681f; c.bu = 1.8556f; }
  else { c.rv = 1.402f; c.gu = -0.344136f; c.gv = -0.714136f; c.bu = 1.772f; }
  return c;
}

__device__ __forceinline__ uchar4 YuvToRgba(float y, float u, float v, const Coeffs& c) {
  float Y = (y - c.yOff) * c.yScale;
  float U = (u - 128.0f) * c.cScale;
  float V = (v - 128.0f) * c.cScale;
  float r = Y + c.rv * V;
  float g = Y + c.gu * U + c.gv * V;
  float b = Y + c.bu * U;
  return make_uchar4((unsigned char)(fminf(fmaxf(r, 0.f), 255.f) + 0.5f),
                     (unsigned char)(fminf(fmaxf(g, 0.f), 255.f) + 0.5f),
                     (unsigned char)(fminf(fmaxf(b, 0.f), 255.f) + 0.5f), 255);
}

// Each thread converts a 2x2 block (one chroma sample).
__global__ void Nv12Kernel(const uint8_t* __restrict__ y, int yPitch, const uint8_t* __restrict__ uv, int uvPitch,
                           uint8_t* out, size_t outPitch, int w, int h, Coeffs c) {
  int cx = blockIdx.x * blockDim.x + threadIdx.x;
  int cy = blockIdx.y * blockDim.y + threadIdx.y;
  int x = cx * 2, yy = cy * 2;
  if (x >= w || yy >= h) return;
  float u = uv[cy * uvPitch + cx * 2];
  float v = uv[cy * uvPitch + cx * 2 + 1];
#pragma unroll
  for (int dy = 0; dy < 2; ++dy) {
    int row = yy + dy;
    if (row >= h) break;
    uchar4* o = reinterpret_cast<uchar4*>(out + row * outPitch);
#pragma unroll
    for (int dx = 0; dx < 2; ++dx) {
      int col = x + dx;
      if (col >= w) break;
      o[col] = YuvToRgba(y[row * yPitch + col], u, v, c);
    }
  }
}

__global__ void I420Kernel(const uint8_t* __restrict__ y, int yPitch, const uint8_t* __restrict__ up, int uPitch,
                           const uint8_t* __restrict__ vp, int vPitch, uint8_t* out, size_t outPitch, int w, int h,
                           Coeffs c) {
  int cx = blockIdx.x * blockDim.x + threadIdx.x;
  int cy = blockIdx.y * blockDim.y + threadIdx.y;
  int x = cx * 2, yy = cy * 2;
  if (x >= w || yy >= h) return;
  float u = up[cy * uPitch + cx];
  float v = vp[cy * vPitch + cx];
#pragma unroll
  for (int dy = 0; dy < 2; ++dy) {
    int row = yy + dy;
    if (row >= h) break;
    uchar4* o = reinterpret_cast<uchar4*>(out + row * outPitch);
#pragma unroll
    for (int dx = 0; dx < 2; ++dx) {
      int col = x + dx;
      if (col >= w) break;
      o[col] = YuvToRgba(y[row * yPitch + col], u, v, c);
    }
  }
}

void LaunchNv12ToRgba(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, GpuImage& out, ColorParams cp,
                      cudaStream_t s) {
  dim3 block(16, 16);
  dim3 grid(((out.width + 1) / 2 + 15) / 16, ((out.height + 1) / 2 + 15) / 16);
  Nv12Kernel<<<grid, block, 0, s>>>(y, yPitch, uv, uvPitch, out.ptr, out.pitch, out.width, out.height,
                                    MakeCoeffs(cp));
}

void LaunchI420ToRgba(const uint8_t* y, int yPitch, const uint8_t* u, int uPitch, const uint8_t* v, int vPitch,
                      GpuImage& out, ColorParams cp, cudaStream_t s) {
  dim3 block(16, 16);
  dim3 grid(((out.width + 1) / 2 + 15) / 16, ((out.height + 1) / 2 + 15) / 16);
  I420Kernel<<<grid, block, 0, s>>>(y, yPitch, u, uPitch, v, vPitch, out.ptr, out.pitch, out.width, out.height,
                                    MakeCoeffs(cp));
}

__global__ void I422Kernel(const uint8_t* __restrict__ y, int yPitch, const uint8_t* __restrict__ up, int uPitch,
                           const uint8_t* __restrict__ vp, int vPitch, uint8_t* out, size_t outPitch, int w, int h,
                           Coeffs c) {
  int cx = blockIdx.x * blockDim.x + threadIdx.x;
  int row = blockIdx.y * blockDim.y + threadIdx.y;
  int x = cx * 2;
  if (x >= w || row >= h) return;
  float u = up[row * uPitch + cx];
  float v = vp[row * vPitch + cx];
  uchar4* o = reinterpret_cast<uchar4*>(out + row * outPitch);
  o[x] = YuvToRgba(y[row * yPitch + x], u, v, c);
  if (x + 1 < w) o[x + 1] = YuvToRgba(y[row * yPitch + x + 1], u, v, c);
}

// Each thread converts one 4-byte macropixel (two pixels).
__global__ void Packed422Kernel(const uint8_t* __restrict__ src, int pitch, int yOff, int uOff, int vOff, uint8_t* out,
                                size_t outPitch, int w, int h, Coeffs c) {
  int cx = blockIdx.x * blockDim.x + threadIdx.x;
  int row = blockIdx.y * blockDim.y + threadIdx.y;
  int x = cx * 2;
  if (x >= w || row >= h) return;
  const uint8_t* m = src + size_t(row) * pitch + cx * 4;
  float u = m[uOff], v = m[vOff];
  uchar4* o = reinterpret_cast<uchar4*>(out + row * outPitch);
  o[x] = YuvToRgba(m[yOff], u, v, c);
  if (x + 1 < w) o[x + 1] = YuvToRgba(m[yOff + 2], u, v, c);
}

__global__ void BgraKernel(const uint8_t* __restrict__ src, int pitch, bool bottomUp, uint8_t* out, size_t outPitch,
                           int w, int h) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int row = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= w || row >= h) return;
  int srow = bottomUp ? h - 1 - row : row;
  uchar4 p = reinterpret_cast<const uchar4*>(src + size_t(srow) * pitch)[x];
  reinterpret_cast<uchar4*>(out + row * outPitch)[x] = make_uchar4(p.z, p.y, p.x, 255);
}

void LaunchI422ToRgba(const uint8_t* y, int yPitch, const uint8_t* u, int uPitch, const uint8_t* v, int vPitch,
                      GpuImage& out, ColorParams cp, cudaStream_t s) {
  dim3 block(16, 16);
  dim3 grid(((out.width + 1) / 2 + 15) / 16, (out.height + 15) / 16);
  I422Kernel<<<grid, block, 0, s>>>(y, yPitch, u, uPitch, v, vPitch, out.ptr, out.pitch, out.width, out.height,
                                    MakeCoeffs(cp));
}

void LaunchPacked422ToRgba(const uint8_t* src, int pitch, bool uyvy, GpuImage& out, ColorParams cp, cudaStream_t s) {
  dim3 block(16, 16);
  dim3 grid(((out.width + 1) / 2 + 15) / 16, (out.height + 15) / 16);
  // YUY2: Y0 U Y1 V    UYVY: U Y0 V Y1
  int yOff = uyvy ? 1 : 0, uOff = uyvy ? 0 : 1, vOff = uyvy ? 2 : 3;
  Packed422Kernel<<<grid, block, 0, s>>>(src, pitch, yOff, uOff, vOff, out.ptr, out.pitch, out.width, out.height,
                                         MakeCoeffs(cp));
}

void LaunchBgraToRgba(const uint8_t* src, int pitch, bool bottomUp, GpuImage& out, cudaStream_t s) {
  dim3 block(16, 16);
  dim3 grid((out.width + 15) / 16, (out.height + 15) / 16);
  BgraKernel<<<grid, block, 0, s>>>(src, pitch, bottomUp, out.ptr, out.pitch, out.width, out.height);
}

static constexpr int kDiffStep = 4;  // sample every 4th pixel in x and y

__global__ void DiffKernel(const uint8_t* a, size_t aPitch, const uint8_t* b, size_t bPitch, int sw, int sh,
                           unsigned long long* result) {
  __shared__ unsigned long long sSum[256];
  __shared__ unsigned int sCnt[256];
  int tid = threadIdx.y * blockDim.x + threadIdx.x;
  int sx = blockIdx.x * blockDim.x + threadIdx.x;
  int sy = blockIdx.y * blockDim.y + threadIdx.y;
  unsigned int d = 0, big = 0;
  if (sx < sw && sy < sh) {
    int x = sx * kDiffStep, y = sy * kDiffStep;
    uchar4 pa = reinterpret_cast<const uchar4*>(a + y * aPitch)[x];
    uchar4 pb = reinterpret_cast<const uchar4*>(b + y * bPitch)[x];
    int la = (pa.x * 77 + pa.y * 150 + pa.z * 29) >> 8;
    int lb = (pb.x * 77 + pb.y * 150 + pb.z * 29) >> 8;
    d = (unsigned)abs(la - lb);
    big = d > 12 ? 1u : 0u;
  }
  sSum[tid] = d;
  sCnt[tid] = big;
  __syncthreads();
  for (int st = 128; st > 0; st >>= 1) {
    if (tid < st) { sSum[tid] += sSum[tid + st]; sCnt[tid] += sCnt[tid + st]; }
    __syncthreads();
  }
  if (tid == 0) {
    atomicAdd(&result[0], sSum[0]);
    atomicAdd(&result[1], (unsigned long long)sCnt[0]);
  }
}

int LaunchFrameDiff(const GpuImage& a, const GpuImage& b, unsigned long long* result, cudaStream_t s) {
  int sw = a.width / kDiffStep, sh = a.height / kDiffStep;
  cudaMemsetAsync(result, 0, sizeof(unsigned long long) * 2, s);
  dim3 block(16, 16);
  dim3 grid((sw + 15) / 16, (sh + 15) / 16);
  DiffKernel<<<grid, block, 0, s>>>(a.ptr, a.pitch, b.ptr, b.pitch, sw, sh, result);
  return sw * sh;
}
