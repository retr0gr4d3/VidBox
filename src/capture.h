#pragma once
#include "pipeline.h"

#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct IMFSourceReader;
struct IMFMediaSource;
class CaptureReaderCallback;

struct CaptureDevice {
  std::string name;  // friendly name (UTF-8)
  std::string link;  // symbolic link (UTF-8), stable across launches
};

struct CaptureFormat {
  std::string subtype;      // FourCC ("NV12", "YUY2", "MJPG", ...)
  int width = 0, height = 0;
  unsigned fpsNum = 0, fpsDen = 1;
  double Fps() const { return fpsDen ? double(fpsNum) / fpsDen : 0.0; }
  bool Native() const;      // converted on the GPU / decoded by us, vs. converted by Windows
  std::string Label() const;
  bool Same(const CaptureFormat& o) const {
    return subtype == o.subtype && width == o.width && height == o.height && fpsNum == o.fpsNum && fpsDen == o.fpsDen;
  }
};

struct CaptureStatus {
  bool running = false;     // Start() called and not stopped
  bool opened = false;      // device opened and streaming requested
  bool signal = false;      // frames arrived recently
  std::string error;
  std::string deviceName;
  CaptureFormat format;     // what the device is delivering
  std::string outputSubtype;// what we receive (differs when Windows converts)
  double fps = 0;           // measured
  std::vector<CaptureFormat> formats;
};

// Capture card (UVC / DirectShow-class video device) source using the Media Foundation source reader in
// asynchronous mode. Frames are delivered on a Media Foundation worker thread, one at a time.
class CaptureClient {
 public:
  using FrameCallback = std::function<void(const RawFrame& f, double arrival)>;
  using ResetCallback = std::function<void()>;

  ~CaptureClient();

  static std::vector<CaptureDevice> ListDevices();
  static std::vector<CaptureFormat> ListFormats(const std::string& link, std::string* err);
  // Highest frame rate (>= 50 preferred), then largest picture, then cheapest pixel format.
  static int PickDefault(const std::vector<CaptureFormat>& formats);

  // Opens the device on a background thread. `want` is matched against the device's formats; if it isn't
  // offered, PickDefault is used.
  void Start(const CaptureDevice& dev, const CaptureFormat& want, FrameCallback onFrame, ResetCallback onReset);
  void Stop();
  CaptureStatus Status();
  bool Running() const { return running_; }

  // Called by the reader callback.
  void OnSample(long hr, unsigned long flags, long long timestamp, struct IMFSample* sample);
  void OnFlushed();

 private:
  void OpenThread(CaptureDevice dev, CaptureFormat want);
  bool ReadCurrentType();
  void RequestNext();
  void Fail(const std::string& msg);

  std::atomic<bool> running_{false};
  std::thread openThread_;
  FrameCallback onFrame_;
  ResetCallback onReset_;

  std::mutex mu_;              // guards reader_/source_/callback_ and the delivery path
  IMFSourceReader* reader_ = nullptr;
  IMFMediaSource* source_ = nullptr;
  CaptureReaderCallback* callback_ = nullptr;
  bool stopping_ = false;
  HANDLE flushEvent_ = nullptr;

  // current output type
  RawFormat rawFormat_ = RawFormat::NV12;
  int width_ = 0, height_ = 0;
  int stride_ = 0;
  int fullRange_ = -1, bt709_ = -1;

  // timestamp mapping (device clock -> NowSec)
  double tsOffset_ = 0;
  bool tsInit_ = false;
  long long lastTs_ = -1;

  std::mutex statMu_;
  CaptureStatus status_;
  RateCounter rate_;
  double lastFrameTime_ = 0;
};
