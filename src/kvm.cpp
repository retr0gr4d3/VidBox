#include "kvm.h"
#include "common.h"

#include <algorithm>

using nlohmann::json;

// Stream quality presets, identical to the GLKVM web UI (index -> h264 bitrate kbps / GOP).
// 0 ExtremeLow, 1 Low, 2 Medium, 3 High, 4 Lossless, 5 Auto
static const struct { int bitrate, gop; } kQualityPresets[] = {
  {500, 30}, {2000, 30}, {5000, 60}, {8000, 60}, {20000, 60}, {0, 0}};

static void DeepMerge(json& dst, const json& src) {
  if (!dst.is_object() || !src.is_object()) { dst = src; return; }
  for (auto it = src.begin(); it != src.end(); ++it) {
    if (it->is_object() && dst.contains(it.key()) && dst[it.key()].is_object()) DeepMerge(dst[it.key()], *it);
    else dst[it.key()] = *it;
  }
}

template <typename T>
static T JGet(const json& j, std::initializer_list<const char*> path, T def) {
  const json* cur = &j;
  for (const char* k : path) {
    if (!cur->is_object()) return def;
    auto it = cur->find(k);
    if (it == cur->end() || it->is_null()) return def;
    cur = &*it;
  }
  try { return cur->get<T>(); } catch (...) { return def; }
}

KvmClient::KvmClient() {}
KvmClient::~KvmClient() { Stop(); }

HttpResponse KvmClient::Api(const wchar_t* method, const std::string& path, const std::string& body,
                            const std::string& ct) {
  HttpResponse r = http_.Request(method, path, body, ct, token_);
  if (!r.error.empty()) LogF("API %s -> %s", path.c_str(), r.error.c_str());
  else if (r.status != 200) LogF("API %s -> HTTP %d %s", path.c_str(), r.status, r.body.substr(0, 200).c_str());
  return r;
}

bool KvmClient::Login(const std::string& host, const std::string& user, const std::string& pass, std::string* err,
                      bool* certMismatch) {
  if (certMismatch) *certMismatch = false;
  host_ = host;
  token_.clear();
  if (!http_.SetHost(host)) { if (err) *err = "Invalid host"; return false; }
  std::string body = "user=" + UrlEncode(user) + "&passwd=" + UrlEncode(pass);
  HttpResponse r = http_.Request(L"POST", "/api/auth/login", body, "application/x-www-form-urlencoded");
  if (r.certMismatch) { if (certMismatch) *certMismatch = true; if (err) *err = r.error; return false; }
  if (!r.error.empty()) { if (err) *err = "Can't reach the KVM at " + host + " (" + r.error + ")"; return false; }
  json j = json::parse(r.body, nullptr, false);
  if (r.status == 401 || r.status == 403) {
    if (err) *err = "Wrong username or password";
    return false;
  }
  if (r.status != 200 || j.is_discarded() || !JGet<bool>(j, {"ok"}, false)) {
    std::string msg = j.is_discarded() ? std::string() : JGet<std::string>(j, {"result", "error_msg"}, "");
    if (err) *err = "Login failed (HTTP " + std::to_string(r.status) + (msg.empty() ? "" : ": " + msg) + ")";
    return false;
  }
  token_ = JGet<std::string>(j, {"result", "token"}, "");
  if (token_.empty()) token_ = JGet<std::string>(j, {"token"}, "");
  if (token_.empty()) {
    auto p = r.setCookie.find("auth_token=");
    if (p != std::string::npos) {
      p += 11;
      auto e = r.setCookie.find(';', p);
      token_ = r.setCookie.substr(p, e == std::string::npos ? std::string::npos : e - p);
    }
  }
  if (token_.empty()) { if (err) *err = "Login succeeded but the KVM returned no session token"; return false; }
  LogF("Logged in to %s", host.c_str());
  return true;
}

bool KvmClient::ResumeSession(const std::string& host, const std::string& token) {
  host_ = host;
  token_ = token;
  if (!http_.SetHost(host)) return false;
  HttpResponse r = Api(L"GET", "/api/auth/check");
  return r.error.empty() && r.status == 200;
}

void KvmClient::Start(VideoCallback onVideo, ResetCallback onReset) {
  Stop();
  onVideo_ = std::move(onVideo);
  onReset_ = std::move(onReset);
  running_ = true;
  workerThread_ = std::thread(&KvmClient::WorkerThread, this);
  eventsThread_ = std::thread(&KvmClient::EventsThread, this);
  mediaThread_ = std::thread(&KvmClient::MediaThread, this);
}

void KvmClient::StartAudio(KvmAudioReceiver::PcmCallback onPcm) {
  StopAudio();
  if (!running_) return;
  onPcm_ = std::move(onPcm);
  audioWanted_ = true;
  audioThread_ = std::thread(&KvmClient::AudioThread, this);
}

void KvmClient::StopAudio() {
  audioWanted_ = false;
  {
    std::lock_guard<std::mutex> lk(wsMu_);
    if (audioWs_) audioWs_->Close();
  }
  if (audioThread_.joinable()) audioThread_.join();
  SetAudioState("", "", false);
}

void KvmClient::SetAudioState(const std::string& state, const std::string& err, bool connected) {
  std::lock_guard<std::mutex> lk(stateMu_);
  status_.audioState = state;
  if (!err.empty() || state.empty()) status_.audioError = err;
  status_.audioConnected = connected;
}

void KvmClient::AudioThread() {
  int backoffMs = 1000;
  while (running_ && audioWanted_) {
    SetAudioState("connecting", "", false);
    std::string err;
    bool mismatch = false;
    HINTERNET h = http_.OpenWebSocket("/pion/ws?auth_token=" + UrlEncode(token_), &err, &mismatch);
    if (!h) {
      LogF("kvm audio ws: %s", err.c_str());
      SetAudioState("waiting", "Could not reach the KVM's audio service: " + err, false);
    } else {
      auto ws = std::make_shared<WebSocket>(h);
      { std::lock_guard<std::mutex> lk(wsMu_); audioWs_ = ws; }
      LogF("kvm audio ws connected");
      double t0 = NowSec();
      bool ok = KvmAudioReceiver::Run(
          ws,
          [this](const float* s, int frames, int rate, int ch) {
            {
              std::lock_guard<std::mutex> lk(stateMu_);
              if (!status_.audioConnected) { status_.audioConnected = true; status_.audioError.clear(); }
            }
            if (onPcm_) onPcm_(s, frames, rate, ch);
          },
          [this](const std::string& st) {
            std::lock_guard<std::mutex> lk(stateMu_);
            status_.audioState = st;
          },
          &err);
      { std::lock_guard<std::mutex> lk(wsMu_); audioWs_.reset(); }
      ws->Close();
      if (!running_ || !audioWanted_) break;
      SetAudioState("waiting", ok ? "Audio connection lost; reconnecting..." : err, false);
      if (ok && NowSec() - t0 > 10) backoffMs = 1000;  // it worked for a while: retry quickly
    }
    for (int i = 0; i < backoffMs / 50 && running_ && audioWanted_; ++i) Sleep(50);
    backoffMs = std::min(backoffMs * 2, 10000);
  }
}

void KvmClient::Stop() {
  StopAudio();
  if (!running_ && !eventsThread_.joinable()) return;
  running_ = false;
  {
    std::lock_guard<std::mutex> lk(wsMu_);
    if (eventsWs_) eventsWs_->Close();
    if (mediaWs_) mediaWs_->Close();
  }
  workCv_.notify_all();
  if (eventsThread_.joinable()) eventsThread_.join();
  if (mediaThread_.joinable()) mediaThread_.join();
  if (workerThread_.joinable()) workerThread_.join();
  std::lock_guard<std::mutex> lk(stateMu_);
  status_ = KvmStatus{};
  streamer_ = hid_ = atx_ = json();
}

void KvmClient::Logout() {
  ReleaseAllKeys();
  Stop();
  if (!token_.empty()) http_.Request(L"POST", "/api/auth/logout", {}, {}, token_);
  token_.clear();
}

KvmStatus KvmClient::Status() {
  std::lock_guard<std::mutex> lk(stateMu_);
  return status_;
}

void KvmClient::Post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lk(workMu_);
    work_.push_back(std::move(fn));
  }
  workCv_.notify_one();
}

void KvmClient::WorkerThread() {
  while (running_) {
    std::function<void()> fn;
    {
      std::unique_lock<std::mutex> lk(workMu_);
      workCv_.wait(lk, [&] { return !work_.empty() || !running_; });
      if (!running_) break;
      fn = std::move(work_.front());
      work_.pop_front();
    }
    fn();
  }
}

// ---------------------------------------------------------------- events (/api/ws)

void KvmClient::HandleEvent(const json& j) {
  std::string type = JGet<std::string>(j, {"event_type"}, "");
  const json ev = j.contains("event") ? j["event"] : json();
  std::lock_guard<std::mutex> lk(stateMu_);
  if (type == "streamer") {
    if (ev.is_object() && ev.contains("streamer") && ev["streamer"].is_null()) {
      status_.sourceOnline = false;
      return;
    }
    DeepMerge(streamer_, ev);
    status_.sourceOnline = JGet<bool>(streamer_, {"streamer", "source", "online"}, false);
    status_.width = JGet<int>(streamer_, {"streamer", "source", "resolution", "width"}, 0);
    status_.height = JGet<int>(streamer_, {"streamer", "source", "resolution", "height"}, 0);
    status_.capturedFps = JGet<int>(streamer_, {"streamer", "source", "captured_fps"}, 0);
    status_.h264Bitrate = JGet<int>(streamer_, {"params", "h264_bitrate"}, 0);
    status_.h264Gop = JGet<int>(streamer_, {"params", "h264_gop"}, 0);
  } else if (type == "hid") {
    DeepMerge(hid_, ev);
    status_.mouseAbsolute = JGet<bool>(hid_, {"mouse", "absolute"}, true);
    status_.keyboardOnline = JGet<bool>(hid_, {"keyboard", "online"}, false);
    status_.mouseOnline = JGet<bool>(hid_, {"mouse", "online"}, false);
    status_.capsLock = JGet<bool>(hid_, {"keyboard", "leds", "caps"}, false);
    status_.mouseOutput = JGet<std::string>(hid_, {"mouse", "outputs", "active"}, "");
    status_.mouseOutputs = JGet<std::vector<std::string>>(hid_, {"mouse", "outputs", "available"}, {});
  } else if (type == "atx") {
    DeepMerge(atx_, ev);
    status_.atxEnabled = JGet<bool>(atx_, {"enabled"}, false);
    status_.atxPowerLed = JGet<bool>(atx_, {"leds", "power"}, false);
    status_.atxHddLed = JGet<bool>(atx_, {"leds", "hdd"}, false);
  }
}

void KvmClient::EventsThread() {
  int backoffMs = 500;
  while (running_) {
    std::string err;
    bool mismatch = false;
    HINTERNET h = http_.OpenWebSocket("/api/ws?auth_token=" + UrlEncode(token_), &err, &mismatch);
    if (!h) {
      LogF("events ws: %s", err.c_str());
      for (int i = 0; i < backoffMs / 50 && running_; ++i) Sleep(50);
      backoffMs = std::min(backoffMs * 2, 5000);
      continue;
    }
    backoffMs = 500;
    auto ws = std::make_shared<WebSocket>(h);
    { std::lock_guard<std::mutex> lk(wsMu_); eventsWs_ = ws; }
    { std::lock_guard<std::mutex> lk(stateMu_); status_.eventsConnected = true; }
    LogF("events ws connected");

    std::atomic<bool> alive{true};
    std::thread pinger([&] {
      while (alive && running_) {
        for (int i = 0; i < 40 && alive && running_; ++i) Sleep(50);
        if (!alive || !running_) break;
        ws->SendText(R"({"event_type":"ping","event":{}})");
        FlushRelative();
      }
    });

    std::vector<uint8_t> buf;
    while (running_) {
      auto m = ws->Receive(buf);
      if (m == WebSocket::Msg::Text) {
        json j = json::parse(buf.begin(), buf.end(), nullptr, false);
        if (!j.is_discarded()) HandleEvent(j);
      } else if (m == WebSocket::Msg::Binary) {
        // not used on this socket
      } else {
        break;
      }
    }
    alive = false;
    ws->Close();
    pinger.join();
    { std::lock_guard<std::mutex> lk(wsMu_); eventsWs_.reset(); }
    { std::lock_guard<std::mutex> lk(stateMu_); status_.eventsConnected = false; }
    LogF("events ws disconnected");
  }
}

// ---------------------------------------------------------------- media (/api/media/ws)

void KvmClient::MediaThread() {
  // Direct H.264 requires the H.264 video format (same call the web UI makes).
  Api(L"POST", "/api/streamer/set_params?video_format=0");
  Api(L"POST", "/api/streamer/set_params?gl_webrtc=false");

  int backoffMs = 500;
  while (running_) {
    std::string err;
    bool mismatch = false;
    HINTERNET h = http_.OpenWebSocket("/api/media/ws?auth_token=" + UrlEncode(token_), &err, &mismatch);
    if (!h) {
      { std::lock_guard<std::mutex> lk(stateMu_); status_.mediaError = err; }
      LogF("media ws: %s", err.c_str());
      for (int i = 0; i < backoffMs / 50 && running_; ++i) Sleep(50);
      backoffMs = std::min(backoffMs * 2, 5000);
      continue;
    }
    backoffMs = 500;
    auto ws = std::make_shared<WebSocket>(h);
    { std::lock_guard<std::mutex> lk(wsMu_); mediaWs_ = ws; }
    { std::lock_guard<std::mutex> lk(stateMu_); status_.mediaConnected = true; status_.mediaError.clear(); }
    LogF("media ws connected");
    if (onReset_) onReset_();

    std::atomic<bool> alive{true};
    std::atomic<int> missed{0};
    std::thread pinger([&] {
      const uint8_t ping = 0;
      while (alive && running_) {
        for (int i = 0; i < 20 && alive && running_; ++i) Sleep(50);
        if (!alive || !running_) break;
        if (++missed >= 6) { LogF("media ws: heartbeat lost"); ws->Close(); break; }
        ws->SendBinary(&ping, 1);
      }
    });

    std::vector<uint8_t> buf;
    bool started = false;
    while (running_) {
      auto m = ws->Receive(buf);
      if (m == WebSocket::Msg::Text) {
        json j = json::parse(buf.begin(), buf.end(), nullptr, false);
        if (j.is_discarded()) continue;
        if (JGet<std::string>(j, {"event_type"}, "") == "media") {
          const json& video = j["event"]["video"];
          if (video.is_object() && video.contains("h264")) {
            LogF("media: h264 profile %s", JGet<std::string>(video, {"h264", "profile_level_id"}, "?").c_str());
            ws->SendText(R"({"event_type":"start","event":{"type":"video","format":"h264"}})");
            started = true;
          } else {
            std::lock_guard<std::mutex> lk(stateMu_);
            status_.mediaError = "The KVM offers no H.264 stream";
          }
        }
      } else if (m == WebSocket::Msg::Binary) {
        if (buf.empty()) continue;
        if (buf[0] == 255) { missed = 0; continue; }
        if (buf[0] == 1 && buf.size() > 2 && started) {
          missed = 0;
          if (onVideo_) onVideo_(buf.data() + 2, buf.size() - 2, buf[1] != 0, NowSec());
        }
      } else {
        break;
      }
    }
    alive = false;
    ws->Close();
    pinger.join();
    { std::lock_guard<std::mutex> lk(wsMu_); mediaWs_.reset(); }
    { std::lock_guard<std::mutex> lk(stateMu_); status_.mediaConnected = false; }
    LogF("media ws disconnected");
  }
}

void KvmClient::RequestKeyframe() {
  // Reconnecting the media socket makes the KVM start with a keyframe.
  std::lock_guard<std::mutex> lk(wsMu_);
  if (mediaWs_) mediaWs_->Close();
}

// ---------------------------------------------------------------- HID (binary protocol, as in the web UI)

void KvmClient::SendHid(const void* data, size_t len) {
  std::shared_ptr<WebSocket> ws;
  { std::lock_guard<std::mutex> lk(wsMu_); ws = eventsWs_; }
  if (ws) ws->SendBinary(data, len);
}

void KvmClient::SendKey(const char* code, bool down) {
  {
    std::lock_guard<std::mutex> lk(keysMu_);
    auto it = std::find(downKeys_.begin(), downKeys_.end(), code);
    if (down) {
      if (it != downKeys_.end()) return;  // ignore auto-repeat; the KVM repeats itself
      downKeys_.push_back(code);
    } else {
      if (it == downKeys_.end()) return;
      downKeys_.erase(it);
    }
  }
  std::string msg;
  msg += char(1);
  msg += char(down ? 1 : 0);
  msg += code;
  SendHid(msg.data(), msg.size());
}

void KvmClient::ReleaseAllKeys() {
  std::vector<std::string> keys;
  { std::lock_guard<std::mutex> lk(keysMu_); keys = downKeys_; }
  for (auto& k : keys) SendKey(k.c_str(), false);
}

void KvmClient::SendMouseButton(const char* button, bool down) {
  std::string msg;
  msg += char(2);
  msg += char(down ? 1 : 0);
  msg += button;
  SendHid(msg.data(), msg.size());
}

void KvmClient::SendMouseMove(int x, int y) {
  x = std::clamp(x, -32768, 32767);
  y = std::clamp(y, -32768, 32767);
  uint8_t m[5] = {3, uint8_t((x >> 8) & 255), uint8_t(x & 255), uint8_t((y >> 8) & 255), uint8_t(y & 255)};
  SendHid(m, sizeof(m));
}

void KvmClient::SendMouseRelative(int dx, int dy) {
  std::lock_guard<std::mutex> lk(relMu_);
  // Split large moves into int8 steps.
  while (dx != 0 || dy != 0) {
    int sx = std::clamp(dx, -127, 127), sy = std::clamp(dy, -127, 127);
    relDeltas_.push_back({sx, sy});
    dx -= sx;
    dy -= sy;
  }
}

void KvmClient::FlushRelative() {
  std::vector<std::pair<int, int>> d;
  { std::lock_guard<std::mutex> lk(relMu_); d.swap(relDeltas_); }
  if (d.empty()) return;
  std::vector<int8_t> m(2 + 2 * d.size());
  m[0] = 4;
  m[1] = 1;  // squash
  for (size_t i = 0; i < d.size(); ++i) { m[2 + 2 * i] = int8_t(d[i].first); m[3 + 2 * i] = int8_t(d[i].second); }
  SendHid(m.data(), m.size());
}

void KvmClient::SendWheel(int dx, int dy) {
  int8_t m[4] = {5, 0, int8_t(std::clamp(dx, -127, 127)), int8_t(std::clamp(dy, -127, 127))};
  SendHid(m, sizeof(m));
}

// ---------------------------------------------------------------- REST

void KvmClient::SetStreamerParam(const std::string& query) {
  Post([this, query] { Api(L"POST", "/api/streamer/set_params?" + query); });
}

void KvmClient::ApplyStreamQuality(int quality, bool zeroDelay, bool vencSmart) {
  quality = std::clamp(quality, 0, 5);
  auto p = kQualityPresets[quality];
  std::string q = "h264_bitrate=" + std::to_string(p.bitrate);
  if (p.gop) q += "&h264_gop=" + std::to_string(p.gop);
  SetStreamerParam(q);
  SetStreamerParam(std::string("zero_delay=") + (zeroDelay ? "true" : "false"));
  SetStreamerParam(std::string("venc_mode=") + (vencSmart ? "smart" : "normal"));
}

void KvmClient::AtxClick(const std::string& button) {
  Post([this, button] { Api(L"POST", "/api/atx/click?button=" + UrlEncode(button)); });
}

void KvmClient::PasteText(const std::string& text) {
  Post([this, text] { Api(L"POST", "/api/hid/print?limit=0&keymap=en-us", text, "text/plain"); });
}

void KvmClient::SetMouseOutput(const std::string& output) {
  Post([this, output] { Api(L"POST", "/api/hid/set_params?mouse_output=" + UrlEncode(output)); });
}
