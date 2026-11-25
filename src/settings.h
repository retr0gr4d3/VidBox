#pragma once
#include <string>

enum class VideoLayout { Fit = 0, Stretch = 1, Original = 2 };
enum class FgTarget { MatchDisplay = 0, X2 = 1, X3 = 2, X4 = 3, Custom = 4 };
enum class SrTarget { P1440 = 0, P2160 = 1, Window = 2 };
enum class SourceMode { Kvm = 0, Capture = 1 };   // GLKVM over the network, or a local capture card

struct Settings {
  SourceMode mode = SourceMode::Kvm;

  // Capture card (CCBox mode)
  std::string captureLink;         // device symbolic link
  std::string captureName;
  std::string captureSubtype;      // saved format ("" = pick automatically)
  int captureW = 0, captureH = 0;
  unsigned captureFpsNum = 0, captureFpsDen = 1;
  int captureRange = 0;            // 0 auto, 1 limited (16-235), 2 full (0-255)
  bool captureAutoStart = true;
  bool audioEnabled = true;        // play audio (capture card input or the KVM's WebRTC audio)
  std::string captureAudioId;      // audio endpoint id ("" = match the video device automatically)
  float audioVolume = 1.0f;
  bool audioMuted = false;
  bool audioSync = true;           // delay audio by frame generation's added latency
  int audioOffsetMs = 0;           // extra manual delay

  // Connection
  std::string host = "192.168.8.138";
  std::string user = "admin";
  bool rememberPassword = false;
  std::string passwordProtected;   // DPAPI blob (base64)
  std::string certPin;             // SHA-256 of the KVM TLS cert (trust on first use)
  bool autoConnect = true;

  // Stream (applied to the KVM)
  int quality = 5;                 // 0..5: ExtremeLow, Low, Medium, High, Lossless, Auto (web UI order)
  bool zeroDelay = true;           // "Fast response" vs "Smooth display"
  bool vencSmart = true;           // encoder mode smart/normal

  // Display
  VideoLayout layout = VideoLayout::Fit;
  bool vsync = true;               // only relevant while frame gen is off
  bool showStatusBar = true;
  bool showStatsOverlay = false;
  bool toolbarCollapsed = false;

  // Frame generation (NVIDIA VFG)
  bool fgEnabled = false;
  FgTarget fgTarget = FgTarget::MatchDisplay;
  int fgCustomFps = 120;
  int fgQuality = 1;               // 0 Low, 1 Medium, 2 High
  int fgBufferMs = 4;              // jitter buffer on top of one source interval
  bool fgAdaptiveBuffer = true;
  float dupThreshold = 0.6f;       // mean abs luma difference (0..255) below which a frame is a duplicate
  bool dupDetection = true;
  int fgMaxGapMs = 120;            // don't interpolate across gaps longer than this

  // Super resolution (NVIDIA VSR)
  bool srEnabled = false;
  SrTarget srTarget = SrTarget::P1440;
  int srMode = 3;                  // VSR mode index (see vfx.h)

  // Input
  bool keyboardEnabled = true;
  bool mouseEnabled = true;
  int scrollRate = 5;
  bool reverseScroll = false;
  int relativeSense = 10;          // x/10
  bool showCursor = true;

  bool Load();
  bool Save() const;
};

Settings& GetSettings();
