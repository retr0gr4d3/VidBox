#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <string>

#include "kernels.h"

struct cudaGraphicsResource;

class Renderer {
 public:
  bool Init(HWND hwnd, std::string* err);
  void Shutdown();
  void Resize(int w, int h);

  // Blocks until the swap chain can accept a new frame (keeps latency at one frame).
  void WaitForNextFrame();
  void BeginFrame(float r, float g, float b);
  void Present(bool vsync);

  // Copies a CUDA image into the display texture (recreating it on size change).
  bool UploadVideo(const GpuImage& img, cudaStream_t stream);
  ID3D11ShaderResourceView* VideoSrv() const { return videoSrv_; }
  int VideoWidth() const { return videoW_; }
  int VideoHeight() const { return videoH_; }
  bool HasVideo() const { return hasVideo_; }
  void ClearVideo() { hasVideo_ = false; }

  ID3D11Device* Device() const { return device_; }
  ID3D11DeviceContext* Context() const { return context_; }
  int CudaDevice() const { return cudaDevice_; }
  const std::wstring& AdapterName() const { return adapterName_; }
  bool TearingSupported() const { return tearing_; }
  // Refresh rate of the monitor the window is on (Hz).
  double RefreshRate();

 private:
  void CreateRenderTarget();
  void CleanupRenderTarget();
  bool EnsureVideoTexture(int w, int h);

  HWND hwnd_ = nullptr;
  IDXGIFactory6* factory_ = nullptr;
  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;
  IDXGISwapChain2* swap_ = nullptr;
  HANDLE frameWait_ = nullptr;
  ID3D11RenderTargetView* rtv_ = nullptr;
  bool tearing_ = false;
  int cudaDevice_ = 0;
  std::wstring adapterName_;

  ID3D11Texture2D* videoTex_ = nullptr;
  ID3D11ShaderResourceView* videoSrv_ = nullptr;
  cudaGraphicsResource* videoRes_ = nullptr;
  int videoW_ = 0, videoH_ = 0;
  bool hasVideo_ = false;

  double refreshHz_ = 60.0;
  double refreshQueried_ = 0.0;
};
