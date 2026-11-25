#pragma once
#include "audio.h"
#include "capture.h"
#include "common.h"
#include "kvm.h"
#include "pipeline.h"
#include "renderer.h"
#include "settings.h"
#include "vfx.h"

#include <future>
#include <memory>
#include <string>

enum class Screen { Login, Connecting, Main };

enum class Panel { None, Video, Audio, FrameGen, SuperRes, Keyboard, Mouse, Power, Settings };

struct LoginResult {
  bool ok = false;
  bool certMismatch = false;
  std::string error;
};

struct PresentStats {
  double outputFps = 0;
  double fgMs = 0;
  double addedLatencyMs = 0;
  double refreshHz = 60;
  int multiplier = 1;       // effective output frames per game frame
  int starvations = 0;
  double extraBufferMs = 0;
  std::string fgState;      // "", "Preparing...", "Active", "Waiting for video", error text
};

struct App {
  HWND hwnd = nullptr;
  Renderer renderer;
  Pipeline pipeline;
  KvmClient kvm;
  CaptureClient capture;
  AudioPassthrough audio;      // capture card audio
  PcmPlayer kvmPlayer;         // GLKVM WebRTC audio
  bool pipelineReady = false;
  std::string fatalError;

  Screen screen = Screen::Login;
  Panel panel = Panel::None;

  // login form
  char host[128] = {};
  char user[64] = {};
  char pass[128] = {};
  bool remember = false;
  std::string pendingPassword;  // DPAPI-protected, committed on successful login
  std::string loginError;
  bool loginCertMismatch = false;
  std::future<LoginResult> loginTask;

  // capture card mode (start screen + video panel)
  std::vector<CaptureDevice> captureDevices;
  bool devicesScanned = false;
  std::vector<CaptureFormat> captureFormats;   // formats of the selected device (start screen)
  std::string captureFormatsFor;               // device link the list belongs to
  std::string captureFormatsError;
  std::future<std::pair<std::vector<CaptureFormat>, std::string>> formatTask;
  std::string formatTaskFor;
  CaptureStatus cstatus;
  std::vector<AudioDevice> audioInputs;
  std::string audioInputName;   // what is (or would be) used
  std::string audioNote;        // e.g. "no audio input found"
  AudioStatus astatus;

  // frame generation runtime
  cudaStream_t renderStream = nullptr;
  std::unique_ptr<FrameGen> fg;
  std::future<std::unique_ptr<FrameGen>> fgPending;
  int fgPendingKey[3] = {};
  int fgFailKey[3] = {-1, -1, -1};
  double vclock = 0;
  double extraBufferMs = 0;
  double lastStarveTime = 0;
  uint64_t lastStarveId = 0;
  uint64_t shownA = 0, shownB = 0;
  int shownStep = -1;
  RateCounter outRate;
  Ema fgMs;
  PresentStats pstats;

  // status snapshots (refreshed each frame)
  KvmStatus status;
  PipelineStats stats;

  // window
  bool fullscreen = false;
  WINDOWPLACEMENT savedPlacement = {sizeof(WINDOWPLACEMENT)};
  LONG_PTR savedStyle = 0;

  // input
  float videoX0 = 0, videoY0 = 0, videoX1 = 0, videoY1 = 0;  // where the video is drawn (client px)
  bool relativeCaptured = false;
  bool absPending = false;
  int absX = 0, absY = 0;
  double lastAbsSend = 0;
  int wheelAccum = 0;
  unsigned buttonsDown = 0;  // bitmask of mouse buttons we sent "down" for
  bool cursorHidden = false;

  // UI state
  std::string toast;
  double toastUntil = 0;
  char pasteBuf[4096] = {};
  int customBitrate = 5000;
  int customGop = 60;

  void Toast(const std::string& msg, double seconds = 3.0);
  void StartLogin(bool useSavedPassword);
  void OnLoggedIn();
  void Logout();               // KVM: log out. Capture: stop and return to the start screen.
  bool CaptureMode() const { return GetSettings().mode == SourceMode::Capture; }
  void RefreshCaptureDevices();
  void LoadCaptureFormats();   // async, for the device in settings
  void StartCapture();         // (re)starts with the device/format in settings
  void StartAudio();           // (re)starts audio for the current mode per settings (or stops it)
  void ToggleFullscreen();
  void ReleaseMouseCapture();
};

// ui.cpp
void SetupImGuiStyle(float dpiScale);
void DrawUi(App& app);
