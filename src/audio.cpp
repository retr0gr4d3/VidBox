#include "audio.h"
#include "common.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
// Define PKEY_Device_FriendlyName in this file (WIN32_LEAN_AND_MEAN leaves propkeydef out).
#include <initguid.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

static constexpr double kBaseLatencyMs = 40.0;   // queue kept between the card and the speakers
static constexpr double kSlackMs = 30.0;         // drift allowed before dropping / padding

static std::string HrHex(HRESULT hr) {
  char b[32];
  snprintf(b, sizeof(b), "0x%08X", unsigned(hr));
  return b;
}

static std::string DeviceName(IMMDevice* d) {
  ComPtr<IPropertyStore> ps;
  if (FAILED(d->OpenPropertyStore(STGM_READ, &ps))) return "Audio device";
  PROPVARIANT v;
  PropVariantInit(&v);
  std::string n = "Audio device";
  if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) n = Narrow(v.pwszVal);
  PropVariantClear(&v);
  return n;
}

std::vector<AudioDevice> AudioPassthrough::ListInputs() {
  std::vector<AudioDevice> out;
  bool uninit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDeviceCollection> col;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) {
      UINT n = 0;
      col->GetCount(&n);
      for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(col->Item(i, &d))) continue;
        LPWSTR id = nullptr;
        if (FAILED(d->GetId(&id))) continue;
        out.push_back({DeviceName(d.Get()), Narrow(id)});
        CoTaskMemFree(id);
      }
    }
  }
  if (uninit) CoUninitialize();
  return out;
}

static std::vector<std::string> Words(const std::string& s) {
  std::vector<std::string> w;
  std::string cur;
  for (char c : s + " ") {
    if (std::isalnum(static_cast<unsigned char>(c))) cur += char(std::tolower(static_cast<unsigned char>(c)));
    else if (!cur.empty()) { w.push_back(cur); cur.clear(); }
  }
  return w;
}

int AudioPassthrough::MatchInput(const std::vector<AudioDevice>& inputs, const std::string& videoName) {
  static const char* generic[] = {"hdmi", "video", "audio", "usb", "capture", "device", "microphone", "line", "input",
                                  "digital", "interface", "the"};
  std::vector<std::string> vw = Words(videoName);
  bool videoHdmi = std::find(vw.begin(), vw.end(), "hdmi") != vw.end();
  int best = -1, bestScore = 0;
  for (int i = 0; i < int(inputs.size()); ++i) {
    std::vector<std::string> aw = Words(inputs[i].name);
    int score = 0;
    for (auto& w : vw) {
      if (w.size() < 3 || std::find(std::begin(generic), std::end(generic), w) != std::end(generic)) continue;
      if (std::find(aw.begin(), aw.end(), w) != aw.end()) score += 10;
    }
    bool audioHdmi = std::find(aw.begin(), aw.end(), "hdmi") != aw.end();
    if (score > 0 && audioHdmi) score += videoHdmi ? 5 : 3;  // HDMI audio over a line/mic input on the same card
    if (score == 0) {
      // Unrelated names: fall back to inputs that look like a capture card's.
      if (audioHdmi || std::find(aw.begin(), aw.end(), "capture") != aw.end()) score = 2;
    }
    if (score > bestScore) { bestScore = score; best = i; }
  }
  return best;
}

void AudioPassthrough::SetError(const std::string& e) {
  std::lock_guard<std::mutex> l(mu_);
  if (status_.error != e && !e.empty()) LogF("audio: %s", e.c_str());
  status_.error = e;
  status_.active = false;
}

void AudioPassthrough::Start(const AudioDevice& input) {
  Stop();
  if (!stopEvent_) stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ResetEvent(stopEvent_);
  {
    std::lock_guard<std::mutex> l(mu_);
    status_ = AudioStatus();
    status_.running = true;
    status_.input = input.name;
  }
  running_ = true;
  thread_ = std::thread(&AudioPassthrough::Thread, this, input);
}

void AudioPassthrough::Stop() {
  if (thread_.joinable()) {
    SetEvent(stopEvent_);
    thread_.join();
  }
  running_ = false;
  std::lock_guard<std::mutex> l(mu_);
  status_.running = false;
  status_.active = false;
}

AudioStatus AudioPassthrough::Status() {
  std::lock_guard<std::mutex> l(mu_);
  return status_;
}

void AudioPassthrough::Thread(AudioDevice input) {
  bool uninit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  LogF("audio: input \"%s\"", input.name.c_str());
  while (RunOnce(input)) {
    // Device lost or not ready (unplugged, default output changed): try again shortly.
    if (WaitForSingleObject(stopEvent_, 1000) == WAIT_OBJECT_0) break;
  }
  if (uninit) CoUninitialize();
}

bool AudioPassthrough::RunOnce(const AudioDevice& input) {
  ComPtr<IMMDeviceEnumerator> en;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
  if (FAILED(hr)) { SetError("Windows audio is unavailable (" + HrHex(hr) + ")."); return true; }
  ComPtr<IMMDevice> inDev, outDev;
  std::wstring wid = Widen(input.id);
  if (FAILED(en->GetDevice(wid.c_str(), &inDev))) { SetError("The capture card's audio input isn't connected."); return true; }
  DWORD state = 0;
  if (SUCCEEDED(inDev->GetState(&state)) && state != DEVICE_STATE_ACTIVE) {
    SetError("The capture card's audio input is disabled or unplugged.");
    return true;
  }
  if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &outDev))) { SetError("No audio output device."); return true; }
  std::string outName = DeviceName(outDev.Get());

  ComPtr<IAudioClient> cap, ren;
  if (FAILED(hr = inDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &cap))) {
    SetError("Could not open the audio input (" + HrHex(hr) + ")."); return true;
  }
  WAVEFORMATEX* fmt = nullptr;
  if (FAILED(hr = cap->GetMixFormat(&fmt))) { SetError("Could not read the audio input format (" + HrHex(hr) + ")."); return true; }
  struct FmtFree { WAVEFORMATEX* f; ~FmtFree() { CoTaskMemFree(f); } } fmtFree{fmt};

  hr = cap->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, fmt, nullptr);
  if (FAILED(hr)) {
    if (hr == E_ACCESSDENIED)
      SetError("Windows blocked microphone access. Turn on Settings > Privacy & security > Microphone > "
               "\"Let desktop apps access your microphone\".");
    else
      SetError("Could not start the audio input (" + HrHex(hr) + ").");
    return true;
  }
  HANDLE capEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  struct EvFree { HANDLE h; ~EvFree() { CloseHandle(h); } } evFree{capEvent};
  cap->SetEventHandle(capEvent);
  ComPtr<IAudioCaptureClient> cc;
  if (FAILED(hr = cap->GetService(IID_PPV_ARGS(&cc)))) { SetError("Audio input service error (" + HrHex(hr) + ")."); return true; }

  if (FAILED(hr = outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &ren))) {
    SetError("Could not open the audio output (" + HrHex(hr) + ")."); return true;
  }
  // Same format as the input; Windows converts rate and channel layout to the output's mix format.
  hr = ren->Initialize(AUDCLNT_SHAREMODE_SHARED,
                       AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 2000000, 0, fmt,
                       nullptr);
  if (FAILED(hr)) { SetError("Could not start the audio output (" + HrHex(hr) + ")."); return true; }
  ComPtr<IAudioRenderClient> rc;
  ComPtr<ISimpleAudioVolume> vol;
  if (FAILED(hr = ren->GetService(IID_PPV_ARGS(&rc)))) { SetError("Audio output service error (" + HrHex(hr) + ")."); return true; }
  ren->GetService(IID_PPV_ARGS(&vol));
  UINT32 renderFrames = 0;
  ren->GetBufferSize(&renderFrames);

  const UINT32 block = fmt->nBlockAlign;
  const double rate = fmt->nSamplesPerSec;
  bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                 (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                  reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  isFloat = isFloat && fmt->wBitsPerSample == 32;
  bool isPcm16 = !isFloat && fmt->wBitsPerSample == 16;
  auto msToFrames = [&](double ms) { return int64_t(ms * rate / 1000.0); };

  std::vector<uint8_t> fifo;  // pending frames, oldest first
  fifo.assign(size_t(msToFrames(kBaseLatencyMs)) * block, 0);

  if (FAILED(hr = cap->Start())) { SetError("Could not start the audio input (" + HrHex(hr) + ")."); return true; }
  ren->Start();
  {
    std::lock_guard<std::mutex> l(mu_);
    status_.active = true;
    status_.error.clear();
    status_.output = outName;
    status_.sampleRate = int(rate);
    status_.channels = fmt->nChannels;
  }
  LogF("audio: %s -> %s, %d Hz %d ch %d-bit%s", input.name.c_str(), outName.c_str(), int(rate), fmt->nChannels,
       fmt->wBitsPerSample, isFloat ? " float" : "");

  float lastVol = -1;
  float peak = 0;
  bool keepGoing = true;
  const double openedAt = NowSec();
  double lastSound = openedAt;  // last time a sample above ~-70 dBFS arrived
  bool silenceLogged = false;
  for (;;) {
    HANDLE hs[2] = {stopEvent_, capEvent};
    DWORD w = WaitForMultipleObjects(2, hs, FALSE, 200);
    if (w == WAIT_OBJECT_0) { keepGoing = false; break; }

    // 1. Drain the capture buffer.
    UINT32 packet = 0;
    float pk = 0;
    while (SUCCEEDED(hr = cc->GetNextPacketSize(&packet)) && packet) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(hr = cc->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
      size_t bytes = size_t(frames) * block;
      size_t at = fifo.size();
      fifo.resize(at + bytes);
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) memset(fifo.data() + at, 0, bytes);
      else {
        memcpy(fifo.data() + at, data, bytes);
        if (isFloat) {
          const float* f = reinterpret_cast<const float*>(data);
          for (size_t i = 0, n = bytes / 4; i < n; ++i) pk = std::max(pk, std::fabs(f[i]));
        } else if (isPcm16) {
          const int16_t* s = reinterpret_cast<const int16_t*>(data);
          for (size_t i = 0, n = bytes / 2; i < n; ++i) pk = std::max(pk, std::abs(int(s[i])) / 32768.0f);
        }
      }
      cc->ReleaseBuffer(frames);
    }
    if (FAILED(hr)) {
      SetError(hr == AUDCLNT_E_DEVICE_INVALIDATED ? "The capture card's audio input was disconnected."
                                                  : "Audio input error (" + HrHex(hr) + ").");
      break;
    }

    // 2. Hold the total queue at the target latency: drop on overflow (clock drift), pad on underflow or when
    //    the video delay grows.
    UINT32 pad = 0;
    if (FAILED(hr = ren->GetCurrentPadding(&pad))) {
      SetError(hr == AUDCLNT_E_DEVICE_INVALIDATED ? "The audio output changed; reconnecting..."
                                                  : "Audio output error (" + HrHex(hr) + ").");
      break;
    }
    int64_t fifoFrames = int64_t(fifo.size() / block);
    int64_t target = msToFrames(kBaseLatencyMs + std::clamp(extraDelayMs_.load(), 0.0, 500.0));
    int64_t total = fifoFrames + pad;
    int64_t slack = msToFrames(kSlackMs);
    if (total > target + slack) {
      int64_t drop = std::min(fifoFrames, total - target);
      fifo.erase(fifo.begin(), fifo.begin() + size_t(drop) * block);
      fifoFrames -= drop;
    } else if (total + slack < target) {
      int64_t add = target - total;
      fifo.insert(fifo.begin(), size_t(add) * block, 0);
      fifoFrames += add;
    }

    // 3. Feed the output, keeping at most `target` frames queued in the device (the rest waits in the FIFO).
    int64_t keep = std::min<int64_t>(target, renderFrames);
    int64_t n = std::min<int64_t>(fifoFrames, std::max<int64_t>(0, keep - pad));
    n = std::min<int64_t>(n, int64_t(renderFrames) - pad);
    if (n > 0) {
      BYTE* dst = nullptr;
      if (SUCCEEDED(rc->GetBuffer(UINT32(n), &dst))) {
        memcpy(dst, fifo.data(), size_t(n) * block);
        rc->ReleaseBuffer(UINT32(n), 0);
        fifo.erase(fifo.begin(), fifo.begin() + size_t(n) * block);
      }
    }

    float v = muted_ ? 0.0f : std::clamp(volume_.load(), 0.0f, 1.0f);
    if (vol && v != lastVol) { vol->SetMasterVolume(v, nullptr); lastVol = v; }

    peak = std::max(peak * 0.85f, pk);
    double now = NowSec();
    // Below -54 dBFS for seconds on end is silence in practice: some cards flicker the lowest bit of an
    // otherwise silent stream (seen on the Razer Ripsaw HD), which a "nonzero" test would count as sound.
    if (pk > 0.002f || !(isFloat || isPcm16)) lastSound = now;
    if (!silenceLogged && now - lastSound > 5.0) {
      silenceLogged = true;
      LogF("audio: %s has delivered only digital silence for 5 s", input.name.c_str());
    }
    if (now - lastSound < 1.0) silenceLogged = false;
    std::lock_guard<std::mutex> l(mu_);
    status_.peak = peak;
    status_.latencyMs = double(fifo.size() / block + pad) * 1000.0 / rate;
    status_.silentSec = now - lastSound;
  }
  cap->Stop();
  ren->Stop();
  std::lock_guard<std::mutex> l(mu_);
  status_.active = false;
  return keepGoing;
}

// ------------------------------------------------------------------ PcmPlayer

static constexpr double kNetBaseLatencyMs = 60.0;  // network audio arrives in 20 ms bursts with jitter

void PcmPlayer::SetError(const std::string& e) {
  std::lock_guard<std::mutex> l(mu_);
  if (status_.error != e && !e.empty()) LogF("audio out: %s", e.c_str());
  status_.error = e;
  status_.active = false;
}

void PcmPlayer::Start(int rate, int channels) {
  Stop();
  rate_ = rate;
  channels_ = channels;
  if (!stopEvent_) stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ResetEvent(stopEvent_);
  {
    std::lock_guard<std::mutex> l(fifoMu_);
    fifo_.clear();
    pushPeak_ = 0;
    lastPush_ = 0;
  }
  {
    std::lock_guard<std::mutex> l(mu_);
    status_ = AudioStatus();
    status_.running = true;
    status_.sampleRate = rate;
    status_.channels = channels;
  }
  running_ = true;
  thread_ = std::thread(&PcmPlayer::Thread, this);
}

void PcmPlayer::Stop() {
  if (thread_.joinable()) {
    SetEvent(stopEvent_);
    thread_.join();
  }
  running_ = false;
  std::lock_guard<std::mutex> l(mu_);
  status_.running = false;
  status_.active = false;
}

void PcmPlayer::Push(const float* s, int frames, int rate, int channels) {
  if (!running_ || rate != rate_ || channels != channels_ || frames <= 0) return;
  size_t n = size_t(frames) * channels;
  float pk = 0;
  for (size_t i = 0; i < n; ++i) pk = std::max(pk, std::fabs(s[i]));
  std::lock_guard<std::mutex> l(fifoMu_);
  // Never let a stalled output grow the queue without bound (2 s).
  if (fifo_.size() + n > size_t(rate_) * channels_ * 2) fifo_.clear();
  fifo_.insert(fifo_.end(), s, s + n);
  pushPeak_ = std::max(pushPeak_, pk);
  lastPush_ = NowSec();
}

AudioStatus PcmPlayer::Status() {
  std::lock_guard<std::mutex> l(mu_);
  return status_;
}

void PcmPlayer::Thread() {
  bool uninit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  while (RunOnce()) {
    if (WaitForSingleObject(stopEvent_, 1000) == WAIT_OBJECT_0) break;
  }
  if (uninit) CoUninitialize();
}

bool PcmPlayer::RunOnce() {
  ComPtr<IMMDeviceEnumerator> en;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
  if (FAILED(hr)) { SetError("Windows audio is unavailable (" + HrHex(hr) + ")."); return true; }
  ComPtr<IMMDevice> outDev;
  if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &outDev))) { SetError("No audio output device."); return true; }
  std::string outName = DeviceName(outDev.Get());
  ComPtr<IAudioClient> ren;
  if (FAILED(hr = outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &ren))) {
    SetError("Could not open the audio output (" + HrHex(hr) + ")."); return true;
  }
  WAVEFORMATEXTENSIBLE wf = {};
  wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wf.Format.nChannels = WORD(channels_);
  wf.Format.nSamplesPerSec = DWORD(rate_);
  wf.Format.wBitsPerSample = 32;
  wf.Format.nBlockAlign = WORD(4 * channels_);
  wf.Format.nAvgBytesPerSec = wf.Format.nSamplesPerSec * wf.Format.nBlockAlign;
  wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  wf.Samples.wValidBitsPerSample = 32;
  wf.dwChannelMask = channels_ == 1 ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
  wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  hr = ren->Initialize(AUDCLNT_SHAREMODE_SHARED,
                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                           AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                       2000000, 0, &wf.Format, nullptr);
  if (FAILED(hr)) { SetError("Could not start the audio output (" + HrHex(hr) + ")."); return true; }
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  struct EvFree { HANDLE h; ~EvFree() { CloseHandle(h); } } evFree{ev};
  ren->SetEventHandle(ev);
  ComPtr<IAudioRenderClient> rc;
  ComPtr<ISimpleAudioVolume> vol;
  if (FAILED(hr = ren->GetService(IID_PPV_ARGS(&rc)))) { SetError("Audio output service error (" + HrHex(hr) + ")."); return true; }
  ren->GetService(IID_PPV_ARGS(&vol));
  UINT32 renderFrames = 0;
  ren->GetBufferSize(&renderFrames);
  ren->Start();
  {
    std::lock_guard<std::mutex> l(mu_);
    status_.active = true;
    status_.error.clear();
    status_.output = outName;
  }
  LogF("audio out: %s, %d Hz %d ch", outName.c_str(), rate_, channels_);

  const double rate = rate_;
  const int ch = channels_;
  auto msToFrames = [&](double ms) { return int64_t(ms * rate / 1000.0); };
  float lastVol = -1, peak = 0;
  bool keepGoing = true;
  for (;;) {
    HANDLE hs[2] = {stopEvent_, ev};
    if (WaitForMultipleObjects(2, hs, FALSE, 100) == WAIT_OBJECT_0) { keepGoing = false; break; }
    UINT32 pad = 0;
    if (FAILED(hr = ren->GetCurrentPadding(&pad))) {
      SetError(hr == AUDCLNT_E_DEVICE_INVALIDATED ? "The audio output changed; reconnecting..."
                                                  : "Audio output error (" + HrHex(hr) + ").");
      break;
    }
    int64_t target = msToFrames(kNetBaseLatencyMs + std::clamp(extraDelayMs_.load(), 0.0, 500.0));
    int64_t slack = msToFrames(kSlackMs);
    int64_t n = 0;
    double queuedMs = 0;
    float pk = 0;
    {
      std::lock_guard<std::mutex> l(fifoMu_);
      bool streaming = lastPush_ > 0 && NowSec() - lastPush_ < 0.5;
      int64_t fifoFrames = int64_t(fifo_.size() / ch);
      int64_t total = fifoFrames + pad;
      if (total > target + slack) {
        int64_t drop = std::min(fifoFrames, total - target);
        fifo_.erase(fifo_.begin(), fifo_.begin() + size_t(drop) * ch);
        fifoFrames -= drop;
      } else if (streaming && total + slack < target) {
        int64_t add = target - total;  // underrun: re-buffer to the target
        fifo_.insert(fifo_.begin(), size_t(add) * ch, 0.0f);
        fifoFrames += add;
      }
      int64_t keep = std::min<int64_t>(target, renderFrames);
      n = std::min<int64_t>(fifoFrames, std::max<int64_t>(0, keep - pad));
      n = std::min<int64_t>(n, int64_t(renderFrames) - pad);
      BYTE* dst = nullptr;
      if (n > 0 && SUCCEEDED(rc->GetBuffer(UINT32(n), &dst))) {
        memcpy(dst, fifo_.data(), size_t(n) * ch * sizeof(float));
        rc->ReleaseBuffer(UINT32(n), 0);
        fifo_.erase(fifo_.begin(), fifo_.begin() + size_t(n) * ch);
      } else {
        n = 0;
      }
      queuedMs = double(fifo_.size() / ch + pad + n) * 1000.0 / rate;
      pk = pushPeak_;
      pushPeak_ = 0;
    }
    float v = muted_ ? 0.0f : std::clamp(volume_.load(), 0.0f, 1.0f);
    if (vol && v != lastVol) { vol->SetMasterVolume(v, nullptr); lastVol = v; }
    peak = std::max(peak * 0.9f, pk);
    std::lock_guard<std::mutex> l(mu_);
    status_.peak = peak;
    status_.latencyMs = queuedMs;
  }
  ren->Stop();
  std::lock_guard<std::mutex> l(mu_);
  status_.active = false;
  return keepGoing;
}
