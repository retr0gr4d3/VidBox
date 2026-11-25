#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AudioDevice {
  std::string name;  // friendly name (UTF-8)
  std::string id;    // endpoint id (UTF-8)
};

struct AudioStatus {
  bool running = false;
  bool active = false;       // capture and playback both open
  std::string input, output; // device names in use
  std::string error;
  float peak = 0;            // recent input level 0..1 (float formats only)
  double latencyMs = 0;      // queued audio (capture -> speakers)
  double silentSec = 0;      // how long the input has delivered only digital silence
  int sampleRate = 0, channels = 0;
};

// Plays a capture card's audio input on the default output device (WASAPI shared mode). The output stream is
// opened in the input's format and Windows resamples, so any rate/channel layout works.
class AudioPassthrough {
 public:
  ~AudioPassthrough() { Stop(); }

  static std::vector<AudioDevice> ListInputs();
  // Best audio input for a video capture device name ("Razer Ripsaw HD HDMI" -> "Microphone (Razer Ripsaw HD HDMI)").
  static int MatchInput(const std::vector<AudioDevice>& inputs, const std::string& videoName);

  void Start(const AudioDevice& input);
  void Stop();
  bool Running() const { return running_; }

  void SetVolume(float v) { volume_ = v; }       // 0..1
  void SetMuted(bool m) { muted_ = m; }
  void SetExtraDelayMs(double ms) { extraDelayMs_ = ms; }  // to stay in sync with delayed video
  AudioStatus Status();

 private:
  void Thread(AudioDevice input);
  bool RunOnce(const AudioDevice& input);  // returns false to stop retrying
  void SetError(const std::string& e);

  std::atomic<bool> running_{false};
  std::thread thread_;
  void* stopEvent_ = nullptr;
  std::atomic<float> volume_{1.0f};
  std::atomic<bool> muted_{false};
  std::atomic<double> extraDelayMs_{0};

  std::mutex mu_;
  AudioStatus status_;
};

// Plays pushed PCM (interleaved float) on the default output, keeping a steady queue that absorbs network jitter.
// Used for the GLKVM's WebRTC audio.
class PcmPlayer {
 public:
  ~PcmPlayer() { Stop(); }
  void Start(int rate = 48000, int channels = 2);
  void Stop();
  bool Running() const { return running_; }
  // Thread-safe. Samples in another rate/channel count than Start()'s are dropped.
  void Push(const float* samples, int frames, int rate, int channels);

  void SetVolume(float v) { volume_ = v; }
  void SetMuted(bool m) { muted_ = m; }
  void SetExtraDelayMs(double ms) { extraDelayMs_ = ms; }
  AudioStatus Status();

 private:
  void Thread();
  bool RunOnce();
  void SetError(const std::string& e);

  int rate_ = 48000, channels_ = 2;
  std::atomic<bool> running_{false};
  std::thread thread_;
  void* stopEvent_ = nullptr;
  std::atomic<float> volume_{1.0f};
  std::atomic<bool> muted_{false};
  std::atomic<double> extraDelayMs_{0};

  std::mutex fifoMu_;
  std::vector<float> fifo_;
  float pushPeak_ = 0;
  double lastPush_ = 0;

  std::mutex mu_;
  AudioStatus status_;
};
