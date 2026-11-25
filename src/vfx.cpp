#include "vfx.h"
#include "common.h"

#include <windows.h>

#include "nvVideoEffects.h"
#include "nvCVStatus.h"
#include "nvVFXVideoFrameGeneration.h"
#include "nvVFXVideoSuperRes.h"

// Used by the SDK's nvVideoEffectsProxy.cpp to locate NVVideoEffects.dll.
char* g_nvVFXSDKPath = nullptr;

static std::string g_sdkDir;
static std::string g_binDir;

const std::string& VfxSdkDir() { return g_sdkDir; }

bool VfxInitLibrary(std::string* sdkDirOut) {
  static int state = 0;  // 0 untried, 1 ok, 2 failed
  if (state == 0) {
    // Prefer a copy shipped next to the exe (vfx\bin), else the SDK the app was built against.
    std::string candidates[] = {Narrow(ExeDir()) + "\\vfx", VIDBOX_VFX_SDK_DIR};
    for (auto& c : candidates) {
      if (c.empty()) continue;  // release builds have no SDK fallback
      std::string bin = c + "\\bin";
      if (GetFileAttributesA((bin + "\\NVVideoEffects.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_sdkDir = c;
        g_binDir = bin;
        break;
      }
    }
    if (g_binDir.empty()) {
      LogF("vfx: NVIDIA Video Effects SDK not found");
      state = 2;
    } else {
      for (auto& ch : g_binDir) if (ch == '/') ch = '\\';
      g_nvVFXSDKPath = g_binDir.data();
      SetDllDirectoryA(g_binDir.c_str());
      unsigned int ver = 0;
      NvCV_Status s = NvVFX_GetVersion(&ver);
      if (s == NVCV_SUCCESS) {
        LogF("vfx: SDK %u.%u.%u at %s", ver >> 24, (ver >> 16) & 255, (ver >> 8) & 255, g_sdkDir.c_str());
        NvVFX_ConfigureLogger(NVCV_LOG_WARNING, Narrow(AppDataDir() + L"\\nvvfx.log").c_str(), nullptr, nullptr);
        state = 1;
      } else {
        HMODULE h = LoadLibraryExA((g_binDir + "\\NVVideoEffects.dll").c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        LogF("vfx: failed to load NVVideoEffects.dll (%d), direct load %s, GetLastError %lu", int(s),
             h ? "ok" : "failed", GetLastError());
        state = 2;
      }
    }
  }
  if (sdkDirOut) *sdkDirOut = g_sdkDir;
  return state == 1;
}

std::string VfxStatusString(int status) {
  const char* s = NvCV_GetErrorStringFromCode(NvCV_Status(status));
  return std::string(s ? s : "unknown") + " (" + std::to_string(status) + ")";
}

static NvCVImage Wrap(const GpuImage& g) {
  NvCVImage im{};
  NvCVImage_Init(&im, unsigned(g.width), unsigned(g.height), int(g.pitch), g.ptr, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY,
                 NVCV_GPU);
  return im;
}

#define VFX_CHECK(call, what)                                         \
  do {                                                                \
    NvCV_Status _s = (call);                                          \
    if (_s != NVCV_SUCCESS) {                                         \
      if (err) *err = std::string(what) + ": " + VfxStatusString(_s); \
      LogF("vfx: %s failed: %s", what, VfxStatusString(_s).c_str()); \
      Shutdown();                                                     \
      return false;                                                   \
    }                                                                 \
  } while (0)

// ------------------------------------------------------------------ Frame generation

FrameGen::~FrameGen() { Shutdown(); }

void FrameGen::Shutdown() {
  if (fx_) NvVFX_DestroyEffect(fx_);
  fx_ = nullptr;
  out_.Free();
  width_ = height_ = 0;
  quality_ = -1;
}

bool FrameGen::Init(int width, int height, int quality, cudaStream_t stream, std::string* err) {
  Shutdown();
  if (!VfxInitLibrary(nullptr)) { if (err) *err = "NVIDIA Video Effects SDK not found"; return false; }
  stream_ = stream;
  VFX_CHECK(NvVFX_CreateEffect(NVVFX_FX_VIDEO_FRAME_GENERATION, &fx_), "Create frame generation");
  std::string models = g_binDir + "\\models";
  NvVFX_SetString(fx_, NVVFX_MODEL_DIRECTORY, models.c_str());
  VFX_CHECK(NvVFX_SetCudaStream(fx_, NVVFX_CUDA_STREAM, stream), "Set stream");
  VFX_CHECK(NvVFX_SetU32(fx_, NVVFX_INPUT_WIDTH, unsigned(width)), "Set width");
  VFX_CHECK(NvVFX_SetU32(fx_, NVVFX_INPUT_HEIGHT, unsigned(height)), "Set height");
  VFX_CHECK(NvVFX_SetU32(fx_, NVVFXVIDEOFRAMEGENERATION_MODE, unsigned(quality)), "Set quality");
  NvVFX_SetU32(fx_, NVVFXVIDEOFRAMEGENERATION_AUTOMATIC_SHOT_CHANGE_DETECTION_ENABLED, 1u);
  double t0 = NowSec();
  VFX_CHECK(NvVFX_Load(fx_), "Load frame generation model");

  // Same order as NVIDIA's sample: output image and timestep mode are set after Load.
  if (!out_.Alloc(width, height)) { if (err) *err = "Out of GPU memory"; Shutdown(); return false; }
  outIm_ = Wrap(out_);
  VFX_CHECK(NvVFX_SetImage(fx_, NVVFX_OUTPUT_IMAGE, &outIm_), "Set output");
  // Timestep mode: we pick the exact instant to synthesize for each display refresh.
  VFX_CHECK(NvVFX_SetU32(fx_, NVVFXVIDEOFRAMEGENERATION_FRAME_MULTIPLIER, 0u), "Set timestep mode");
  LogF("vfx: frame generation %dx%d quality %d loaded in %.0f ms", width, height, quality, (NowSec() - t0) * 1000);
  width_ = width;
  height_ = height;
  quality_ = quality;
  failures_ = 0;
  return true;
}

const GpuImage* FrameGen::Run(const GpuImage& prev, const GpuImage& next, float t, bool shotChange) {
  if (!fx_) return nullptr;
  in0_ = Wrap(prev);
  in1_ = Wrap(next);
  NvVFX_SetImage(fx_, NVVFX_INPUT_IMAGE_0, &in0_);
  NvVFX_SetImage(fx_, NVVFX_INPUT_IMAGE_1, &in1_);
  NvVFX_SetU32(fx_, NVVFXVIDEOFRAMEGENERATION_SHOT_CHANGE, shotChange ? 1u : 0u);
  NvVFX_SetF32(fx_, NVVFXVIDEOFRAMEGENERATION_TIMESTEP, t);
  NvCV_Status s = NvVFX_Run(fx_, 1);
  if (s != NVCV_SUCCESS) {
    if (failures_++ < 5) LogF("vfx: frame generation run failed: %s", VfxStatusString(s).c_str());
    return nullptr;
  }
  return &out_;
}

static void LogImage(const char* tag, const NvCVImage& im) {
  LogF("probe %s: %ux%u pitch %d fmt %d type %d pixB %u compB %u nComp %u planar %u gpu %u cs %u bufBytes %llu",
       tag, im.width, im.height, im.pitch, int(im.pixelFormat), int(im.componentType), im.pixelBytes,
       im.componentBytes, im.numComponents, im.planar, im.gpuMem, im.colorspace, im.bufferBytes);
}

// Diagnoses which image descriptors VFG accepts (used by --selftest).
void VfxProbeFrameGen(int W, int H, cudaStream_t stream) {
  GpuImage g;
  g.Alloc(W, H);
  NvCVImage wrapped = Wrap(g);
  LogImage("wrapped", wrapped);
  NvCVImage a{}, b{}, o{};
  NvCVImage_Alloc(&a, W, H, NVCV_RGBA, NVCV_U8, NVCV_INTERLEAVED, NVCV_GPU, 32);
  NvCVImage_Alloc(&b, W, H, NVCV_RGBA, NVCV_U8, NVCV_INTERLEAVED, NVCV_GPU, 32);
  NvCVImage_Alloc(&o, W, H, NVCV_RGBA, NVCV_U8, NVCV_INTERLEAVED, NVCV_GPU, 32);
  LogImage("alloc", a);

  NvVFX_Handle fx = nullptr;
  NvVFX_CreateEffect(NVVFX_FX_VIDEO_FRAME_GENERATION, &fx);
  NvVFX_SetString(fx, NVVFX_MODEL_DIRECTORY, (g_binDir + "\\models").c_str());
  NvVFX_SetCudaStream(fx, NVVFX_CUDA_STREAM, stream);
  NvVFX_SetU32(fx, NVVFX_INPUT_WIDTH, W);
  NvVFX_SetU32(fx, NVVFX_INPUT_HEIGHT, H);
  NvVFX_SetU32(fx, NVVFXVIDEOFRAMEGENERATION_MODE, 1);
  NvCV_Status s = NvVFX_Load(fx);
  LogF("probe load: %s", VfxStatusString(s).c_str());
  auto tryRun = [&](const char* name, NvCVImage* i0, NvCVImage* i1, NvCVImage* out, bool timestep) {
    NvVFX_SetImage(fx, NVVFX_OUTPUT_IMAGE, out);
    NvVFX_SetU32(fx, NVVFXVIDEOFRAMEGENERATION_FRAME_MULTIPLIER, timestep ? 0u : 2u);
    NvVFX_SetImage(fx, NVVFX_INPUT_IMAGE_0, i0);
    NvVFX_SetImage(fx, NVVFX_INPUT_IMAGE_1, i1);
    NvVFX_SetU32(fx, NVVFXVIDEOFRAMEGENERATION_SHOT_CHANGE, 0u);
    if (timestep) NvVFX_SetF32(fx, NVVFXVIDEOFRAMEGENERATION_TIMESTEP, 0.5f);
    else NvVFX_SetU32(fx, NVVFXVIDEOFRAMEGENERATION_FRAME_INDEX, 1u);
    NvCV_Status r = NvVFX_Run(fx, 0);
    LogF("probe run %-28s: %s", name, VfxStatusString(r).c_str());
  };
  tryRun("alloc / multiplier", &a, &b, &o, false);
  tryRun("alloc / timestep", &a, &b, &o, true);
  NvCVImage w2 = Wrap(g);
  tryRun("wrapped in / alloc out", &w2, &w2, &o, true);
  NvCVImage w3 = Wrap(g);
  w3.bufferBytes = (unsigned long long)w3.pitch * w3.height;
  tryRun("wrapped+bufBytes", &w3, &w3, &w3, true);
  NvVFX_DestroyEffect(fx);
  NvCVImage_Dealloc(&a);
  NvCVImage_Dealloc(&b);
  NvCVImage_Dealloc(&o);
  g.Free();
}

// ------------------------------------------------------------------ Super resolution

const VsrModeInfo kVsrModes[] = {
  {0, "Bicubic (no AI)", false},
  {1, "Low", false},
  {2, "Medium", false},
  {3, "High", false},
  {4, "Ultra", false},
  {16, "High bitrate - Low", false},
  {17, "High bitrate - Medium", false},
  {18, "High bitrate - High", false},
  {19, "High bitrate - Ultra", false},
  {21, "Streaming artifacts - Medium", false},
  {23, "Streaming artifacts - Ultra", false},
  {8, "Denoise only - Low", true},
  {9, "Denoise only - Medium", true},
  {10, "Denoise only - High", true},
  {11, "Denoise only - Ultra", true},
  {12, "Deblur only - Low", true},
  {13, "Deblur only - Medium", true},
  {14, "Deblur only - High", true},
  {15, "Deblur only - Ultra", true},
};
const int kVsrModeCount = int(sizeof(kVsrModes) / sizeof(kVsrModes[0]));

SuperRes::~SuperRes() { Shutdown(); }

void SuperRes::Shutdown() {
  if (fx_) NvVFX_DestroyEffect(fx_);
  fx_ = nullptr;
  mode_ = -1;
}

bool SuperRes::Init(int inW, int inH, int outW, int outH, int mode, cudaStream_t stream, std::string* err) {
  Shutdown();
  if (!VfxInitLibrary(nullptr)) { if (err) *err = "NVIDIA Video Effects SDK not found"; return false; }
  stream_ = stream;
  VFX_CHECK(NvVFX_CreateEffect(NVVFX_FX_VIDEO_SUPER_RES, &fx_), "Create super resolution");
  std::string models = g_binDir + "\\models";
  NvVFX_SetString(fx_, NVVFX_MODEL_DIRECTORY, models.c_str());
  VFX_CHECK(NvVFX_SetCudaStream(fx_, NVVFX_CUDA_STREAM, stream), "Set stream");
  NvVFX_SetU32(fx_, NVVFX_IMAGE_ENCODING_MODE, NVVFX_IMAGE_ENCODING_RGB8);
  NvCV_Status s = NvVFX_SetU32(fx_, NVVFX_QUALITY_LEVEL, unsigned(mode));
  if (s == NVCV_ERR_SELECTOR) s = NvVFX_SetU32(fx_, NVVFX_MODE, unsigned(mode));
  VFX_CHECK(s, "Set super resolution mode");
  // Placeholder images with the right geometry so Load can size its buffers.
  GpuImage tmpIn, tmpOut;
  if (!tmpIn.Alloc(inW, inH) || !tmpOut.Alloc(outW, outH)) {
    if (err) *err = "Out of GPU memory";
    Shutdown();
    return false;
  }
  NvCVImage i = Wrap(tmpIn), o = Wrap(tmpOut);
  NvVFX_SetImage(fx_, NVVFX_INPUT_IMAGE, &i);
  NvVFX_SetImage(fx_, NVVFX_OUTPUT_IMAGE, &o);
  NvCV_Status ls = NvVFX_Load(fx_);
  tmpIn.Free();
  tmpOut.Free();
  VFX_CHECK(ls, "Load super resolution");
  inW_ = inW; inH_ = inH; outW_ = outW; outH_ = outH; mode_ = mode;
  LogF("vfx: super resolution %dx%d -> %dx%d mode %d", inW, inH, outW, outH, mode);
  return true;
}

bool SuperRes::Run(const GpuImage& in, GpuImage& out) {
  if (!fx_) return false;
  inIm_ = Wrap(in);
  outIm_ = Wrap(out);
  NvVFX_SetImage(fx_, NVVFX_INPUT_IMAGE, &inIm_);
  NvVFX_SetImage(fx_, NVVFX_OUTPUT_IMAGE, &outIm_);
  NvCV_Status s = NvVFX_Run(fx_, 1);
  if (s != NVCV_SUCCESS) {
    static int n = 0;
    if (n++ < 5) LogF("vfx: super resolution run failed: %s", VfxStatusString(s).c_str());
    return false;
  }
  return true;
}
