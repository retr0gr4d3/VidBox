#pragma once
#include "kvmaudio.h"
#include "net.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"

struct KvmStatus {
  // connection
  bool eventsConnected = false;
  bool mediaConnected = false;
  std::string mediaError;
  // streamer
  bool sourceOnline = false;
  int width = 0, height = 0;
  int capturedFps = 0;
  int h264Bitrate = 0, h264Gop = 0;
  // hid
  bool mouseAbsolute = true;
  std::vector<std::string> mouseOutputs;
  std::string mouseOutput;
  bool capsLock = false;
  bool keyboardOnline = false, mouseOnline = false;
  // atx
  bool atxEnabled = false;
  bool atxPowerLed = false, atxHddLed = false;
  // audio (WebRTC session on /pion/ws)
  bool audioConnected = false;   // audio packets are arriving
  std::string audioState;        // "", "connecting", peer state, ...
  std::string audioError;
};

class KvmClient {
 public:
  // Called on the media thread for every H.264 access unit (Annex-B).
  using VideoCallback = std::function<void(const uint8_t* data, size_t len, bool keyframe, double arrival)>;
  using ResetCallback = std::function<void()>;

  KvmClient();
  ~KvmClient();

  // Blocking. Returns the auth token or empty on failure.
  bool Login(const std::string& host, const std::string& user, const std::string& pass, std::string* err,
             bool* certMismatch);
  bool ResumeSession(const std::string& host, const std::string& token);
  void Start(VideoCallback onVideo, ResetCallback onReset);
  void Stop();
  void Logout();

  const std::string& Token() const { return token_; }
  std::string SeenCert() { return http_.SeenCert(); }
  void SetPinnedCert(const std::string& pin) { http_.SetPinnedCert(pin); }

  KvmStatus Status();
  bool Running() const { return running_; }

  // HID (non-blocking, sent over /api/ws).
  void SendKey(const char* code, bool down);
  void SendMouseButton(const char* button, bool down);
  void SendMouseMove(int x, int y);            // -32768..32767
  void SendMouseRelative(int dx, int dy);      // accumulated; flushed by FlushRelative
  void FlushRelative();
  void SendWheel(int dx, int dy);
  void ReleaseAllKeys();

  // REST calls, executed on a background worker.
  void ApplyStreamQuality(int quality, bool zeroDelay, bool vencSmart);
  void SetStreamerParam(const std::string& query);
  void AtxClick(const std::string& button);
  void PasteText(const std::string& text);
  void SetMouseOutput(const std::string& output);
  void RequestKeyframe();

  // Speaker audio over WebRTC (/pion/ws). Runs while the client runs; reconnects on its own.
  void StartAudio(KvmAudioReceiver::PcmCallback onPcm);
  void StopAudio();

 private:
  void EventsThread();
  void MediaThread();
  void WorkerThread();
  void AudioThread();
  void SetAudioState(const std::string& state, const std::string& err, bool connected);
  void Post(std::function<void()> fn);
  void HandleEvent(const nlohmann::json& j);
  void SendHid(const void* data, size_t len);
  HttpResponse Api(const wchar_t* method, const std::string& path, const std::string& body = {},
                   const std::string& ct = {});

  HttpClient http_;
  std::string host_;
  std::string token_;

  std::atomic<bool> running_{false};
  std::thread eventsThread_, mediaThread_, workerThread_;
  VideoCallback onVideo_;
  ResetCallback onReset_;

  std::mutex wsMu_;
  std::shared_ptr<WebSocket> eventsWs_, mediaWs_, audioWs_;

  std::thread audioThread_;
  std::atomic<bool> audioWanted_{false};
  KvmAudioReceiver::PcmCallback onPcm_;

  std::mutex stateMu_;
  nlohmann::json streamer_, hid_, atx_;
  KvmStatus status_;

  std::mutex workMu_;
  std::condition_variable workCv_;
  std::deque<std::function<void()>> work_;

  std::mutex keysMu_;
  std::vector<std::string> downKeys_;

  std::mutex relMu_;
  std::vector<std::pair<int, int>> relDeltas_;
};
