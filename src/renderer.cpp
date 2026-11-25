#include "renderer.h"
#include "common.h"

#include <cuda_d3d11_interop.h>
#include <cuda_runtime.h>

template <typename T>
static void SafeRelease(T*& p) {
  if (p) { p->Release(); p = nullptr; }
}

bool Renderer::Init(HWND hwnd, std::string* err) {
  hwnd_ = hwnd;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) { if (err) *err = "CreateDXGIFactory2 failed"; return false; }

  // Pick the NVIDIA GPU explicitly (the monitor may be attached to another card).
  IDXGIAdapter1* chosen = nullptr;
  for (UINT i = 0;; ++i) {
    IDXGIAdapter1* a = nullptr;
    if (factory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) != S_OK) break;
    DXGI_ADAPTER_DESC1 d;
    a->GetDesc1(&d);
    LogF("adapter %u: %s (vendor %04x)", i, Narrow(d.Description).c_str(), d.VendorId);
    if (!chosen && d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { chosen = a; continue; }
    a->Release();
  }
  if (!chosen) {
    if (err) *err = "No NVIDIA GPU found. VidBox needs an RTX 40 or 50 series card for frame generation.";
    return false;
  }
  DXGI_ADAPTER_DESC1 desc;
  chosen->GetDesc1(&desc);
  adapterName_ = desc.Description;

  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                 &device_, nullptr, &context_);
  if (FAILED(hr)) { chosen->Release(); if (err) *err = "D3D11CreateDevice failed"; return false; }

  if (cudaD3D11GetDevice(&cudaDevice_, chosen) != cudaSuccess) {
    chosen->Release();
    if (err) *err = "CUDA is not available on " + Narrow(adapterName_) + " (driver too old?)";
    return false;
  }
  chosen->Release();
  cudaSetDevice(cudaDevice_);
  // FFmpeg's NVDEC shares this primary context and requires blocking-sync scheduling flags.
  cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
  cudaFree(nullptr);  // create the primary context now

  BOOL allowTearing = FALSE;
  factory_->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
  tearing_ = allowTearing != FALSE;

  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 3;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Scaling = DXGI_SCALING_NONE;
  sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  IDXGISwapChain1* sc1 = nullptr;
  hr = factory_->CreateSwapChainForHwnd(device_, hwnd, &sd, nullptr, nullptr, &sc1);
  if (FAILED(hr)) { if (err) *err = "CreateSwapChainForHwnd failed"; return false; }
  sc1->QueryInterface(IID_PPV_ARGS(&swap_));
  sc1->Release();
  factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  swap_->SetMaximumFrameLatency(1);
  frameWait_ = swap_->GetFrameLatencyWaitableObject();
  CreateRenderTarget();
  LogF("renderer: %s, CUDA device %d, tearing %d", Narrow(adapterName_).c_str(), cudaDevice_, int(tearing_));
  return true;
}

void Renderer::Shutdown() {
  if (videoRes_) { cudaGraphicsUnregisterResource(videoRes_); videoRes_ = nullptr; }
  SafeRelease(videoSrv_);
  SafeRelease(videoTex_);
  CleanupRenderTarget();
  if (frameWait_) { CloseHandle(frameWait_); frameWait_ = nullptr; }
  SafeRelease(swap_);
  SafeRelease(context_);
  SafeRelease(device_);
  SafeRelease(factory_);
}

void Renderer::CreateRenderTarget() {
  ID3D11Texture2D* back = nullptr;
  swap_->GetBuffer(0, IID_PPV_ARGS(&back));
  if (back) {
    device_->CreateRenderTargetView(back, nullptr, &rtv_);
    back->Release();
  }
}

void Renderer::CleanupRenderTarget() { SafeRelease(rtv_); }

void Renderer::Resize(int w, int h) {
  if (!swap_ || w <= 0 || h <= 0) return;
  CleanupRenderTarget();
  context_->ClearState();
  context_->Flush();
  swap_->ResizeBuffers(0, UINT(w), UINT(h), DXGI_FORMAT_UNKNOWN,
                       DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                           (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
  CreateRenderTarget();
}

void Renderer::WaitForNextFrame() {
  if (frameWait_) WaitForSingleObjectEx(frameWait_, 100, TRUE);
}

void Renderer::BeginFrame(float r, float g, float b) {
  const float c[4] = {r, g, b, 1.0f};
  context_->OMSetRenderTargets(1, &rtv_, nullptr);
  context_->ClearRenderTargetView(rtv_, c);
}

void Renderer::Present(bool vsync) {
  UINT flags = 0;
  if (!vsync && tearing_) flags |= DXGI_PRESENT_ALLOW_TEARING;
  swap_->Present(vsync ? 1 : 0, flags);
}

bool Renderer::EnsureVideoTexture(int w, int h) {
  if (videoTex_ && videoW_ == w && videoH_ == h) return true;
  if (videoRes_) { cudaGraphicsUnregisterResource(videoRes_); videoRes_ = nullptr; }
  SafeRelease(videoSrv_);
  SafeRelease(videoTex_);
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = UINT(w);
  td.Height = UINT(h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(device_->CreateTexture2D(&td, nullptr, &videoTex_))) return false;
  device_->CreateShaderResourceView(videoTex_, nullptr, &videoSrv_);
  if (cudaGraphicsD3D11RegisterResource(&videoRes_, videoTex_, cudaGraphicsRegisterFlagsNone) != cudaSuccess) {
    LogF("renderer: cudaGraphicsD3D11RegisterResource failed");
    return false;
  }
  cudaGraphicsResourceSetMapFlags(videoRes_, cudaGraphicsMapFlagsWriteDiscard);
  videoW_ = w;
  videoH_ = h;
  return true;
}

bool Renderer::UploadVideo(const GpuImage& img, cudaStream_t stream) {
  if (!img.Valid() || !EnsureVideoTexture(img.width, img.height)) return false;
  if (cudaGraphicsMapResources(1, &videoRes_, stream) != cudaSuccess) return false;
  cudaArray_t arr = nullptr;
  cudaGraphicsSubResourceGetMappedArray(&arr, videoRes_, 0, 0);
  cudaError_t e = cudaMemcpy2DToArrayAsync(arr, 0, 0, img.ptr, img.pitch, size_t(img.width) * 4, size_t(img.height),
                                           cudaMemcpyDeviceToDevice, stream);
  cudaGraphicsUnmapResources(1, &videoRes_, stream);
  if (e == cudaSuccess) hasVideo_ = true;
  return e == cudaSuccess;
}

double Renderer::RefreshRate() {
  double now = NowSec();
  if (now - refreshQueried_ > 2.0) {
    refreshQueried_ = now;
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
      DEVMODEW dm = {};
      dm.dmSize = sizeof(dm);
      if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        refreshHz_ = dm.dmDisplayFrequency == 59 ? 59.94 : double(dm.dmDisplayFrequency);
    }
  }
  return refreshHz_;
}
