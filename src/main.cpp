// VidBox - native GLKVM and capture card client with NVIDIA frame generation and super resolution.
#include "app.h"
#include "common.h"
#include "input.h"

#include <windowsx.h>
#include <timeapi.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static App* g_app = nullptr;

// ------------------------------------------------------------------ App actions

void App::Toast(const std::string& msg, double seconds) {
  toast = msg;
  toastUntil = NowSec() + seconds;
}

void App::StartLogin(bool useSavedPassword) {
  Settings& s = GetSettings();
  std::string h = host, u = user, p = pass;
  if (useSavedPassword) p = UnprotectString(s.passwordProtected);
  if (h.empty()) { loginError = "Enter the KVM address"; return; }
  loginError.clear();
  loginCertMismatch = false;
  screen = Screen::Connecting;
  kvm.SetPinnedCert(s.certPin);
  loginTask = std::async(std::launch::async, [this, h, u, p]() {
    LoginResult r;
    r.ok = kvm.Login(h, u, p, &r.error, &r.certMismatch);
    return r;
  });
  // Only remembered once the login succeeds (OnLoggedIn).
  pendingPassword = useSavedPassword ? s.passwordProtected : (remember ? ProtectString(p) : std::string());
  s.host = h;
  s.user = u;
  SecureZeroMemory(pass, sizeof(pass));
}

void App::OnLoggedIn() {
  Settings& s = GetSettings();
  s.rememberPassword = remember;
  s.passwordProtected = remember ? pendingPassword : std::string();
  pendingPassword.clear();
  if (s.certPin.empty()) {
    s.certPin = kvm.SeenCert();
    LogF("Pinned KVM certificate %s", s.certPin.c_str());
  }
  s.Save();
  pipeline.Reset();
  kvm.Start(
      [this](const uint8_t* d, size_t n, bool key, double arrival) {
        if (pipelineReady) pipeline.OnPacket(d, n, key, arrival);
      },
      [this]() { pipeline.Reset(); });
  kvm.ApplyStreamQuality(s.quality, s.zeroDelay, s.vencSmart);
  StartAudio();
  screen = Screen::Main;
}

void App::RefreshCaptureDevices() {
  captureDevices = CaptureClient::ListDevices();
  devicesScanned = true;
  Settings& s = GetSettings();
  bool found = false;
  for (auto& d : captureDevices) if (d.link == s.captureLink) { found = true; s.captureName = d.name; }
  if (!found && !captureDevices.empty() && !capture.Running()) {
    // Prefer something that looks like a capture card over a webcam.
    int pick = 0;
    for (int i = 0; i < int(captureDevices.size()); ++i) {
      std::string n = captureDevices[i].name;
      for (auto& c : n) c = char(tolower(c));
      if (n.find("capture") != std::string::npos || n.find("hdmi") != std::string::npos ||
          n.find("elgato") != std::string::npos || n.find("avermedia") != std::string::npos ||
          n.find("live gamer") != std::string::npos || n.find("usb video") != std::string::npos) { pick = i; break; }
    }
    s.captureLink = captureDevices[pick].link;
    s.captureName = captureDevices[pick].name;
  }
  LoadCaptureFormats();
}

void App::LoadCaptureFormats() {
  std::string link = GetSettings().captureLink;
  if (link.empty() || capture.Running()) return;
  if (formatTask.valid()) {
    if (formatTaskFor == link) return;
    formatTask.wait();  // a different device was picked; the old enumeration finishes quickly
    formatTask.get();
  }
  captureFormats.clear();
  captureFormatsError.clear();
  captureFormatsFor.clear();
  formatTaskFor = link;
  formatTask = std::async(std::launch::async, [link]() {
    std::string err;
    auto f = CaptureClient::ListFormats(link, &err);
    return std::make_pair(std::move(f), err);
  });
}

void App::StartCapture() {
  Settings& s = GetSettings();
  if (s.captureLink.empty()) { loginError = "Pick a capture card"; return; }
  loginError.clear();
  if (formatTask.valid()) formatTask.wait();  // the device can't be opened twice at once
  CaptureFormat want;
  want.subtype = s.captureSubtype;
  want.width = s.captureW;
  want.height = s.captureH;
  want.fpsNum = s.captureFpsNum;
  want.fpsDen = s.captureFpsDen;
  s.Save();
  pipeline.SetCaptureColorRange(s.captureRange);
  pipeline.Reset();
  renderer.ClearVideo();
  shownA = shownB = 0;
  capture.Start({s.captureName, s.captureLink}, want,
                [this](const RawFrame& f, double arrival) {
                  if (pipelineReady) pipeline.OnRawFrame(f, arrival);
                },
                [this]() { pipeline.Reset(); });
  if (!audio.Running() || audioNote.size()) StartAudio();  // format changes keep the audio running
  screen = Screen::Main;
}

void App::StartAudio() {
  Settings& s = GetSettings();
  if (!CaptureMode()) {
    // GLKVM: WebRTC audio session alongside the Direct H.264 video.
    kvm.StopAudio();
    kvmPlayer.Stop();
    if (!s.audioEnabled || !kvm.Running()) return;
    kvmPlayer.Start(48000, 2);
    kvm.StartAudio([this](const float* pcm, int frames, int rate, int ch) { kvmPlayer.Push(pcm, frames, rate, ch); });
    return;
  }
  audio.Stop();
  audioNote.clear();
  audioInputs = AudioPassthrough::ListInputs();
  int pick = -1;
  if (!s.captureAudioId.empty())
    for (int i = 0; i < int(audioInputs.size()); ++i) if (audioInputs[i].id == s.captureAudioId) pick = i;
  if (pick < 0) pick = AudioPassthrough::MatchInput(audioInputs, s.captureName);
  audioInputName = pick >= 0 ? audioInputs[pick].name : std::string();
  if (!s.audioEnabled) return;
  if (pick < 0) {
    audioNote = s.captureAudioId.empty() ? "No audio input found for this capture card. Pick one below."
                                         : "The selected audio input isn't connected.";
    LogF("audio: %s", audioNote.c_str());
    return;
  }
  audio.Start(audioInputs[pick]);
}

void App::Logout() {
  ReleaseMouseCapture();
  if (capture.Running()) {
    audio.Stop();
    capture.Stop();
    devicesScanned = false;  // rescan when the start screen shows
  } else {
    kvm.Logout();
    kvmPlayer.Stop();
  }
  pipeline.Reset();
  renderer.ClearVideo();
  shownA = shownB = 0;
  screen = Screen::Login;
  panel = Panel::None;
}

void App::ToggleFullscreen() {
  if (!fullscreen) {
    savedStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
    GetWindowPlacement(hwnd, &savedPlacement);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLongPtrW(hwnd, GWL_STYLE, (savedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    fullscreen = true;
  } else {
    SetWindowLongPtrW(hwnd, GWL_STYLE, savedStyle);
    SetWindowPlacement(hwnd, &savedPlacement);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    fullscreen = false;
  }
}

static void ReleaseButtons(App* app) {
  static const char* names[] = {"left", "right", "middle", "up", "down"};
  for (int i = 0; i < 5; ++i)
    if (app->buttonsDown & (1u << i)) app->kvm.SendMouseButton(names[i], false);
  app->buttonsDown = 0;
  ReleaseCapture();
}

void App::ReleaseMouseCapture() {
  if (!relativeCaptured) return;
  RAWINPUTDEVICE rid = {0x01, 0x02, RIDEV_REMOVE, nullptr};
  RegisterRawInputDevices(&rid, 1, sizeof(rid));
  ClipCursor(nullptr);
  relativeCaptured = false;
  cursorHidden = false;
  ReleaseButtons(this);
}

static void CaptureRelativeMouse(App* app) {
  RAWINPUTDEVICE rid = {0x01, 0x02, 0, app->hwnd};
  if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) return;
  RECT rc;
  GetClientRect(app->hwnd, &rc);
  MapWindowPoints(app->hwnd, nullptr, reinterpret_cast<POINT*>(&rc), 2);
  ClipCursor(&rc);
  app->relativeCaptured = true;
  app->cursorHidden = true;
  SetCursor(nullptr);
  app->Toast("Mouse captured. Press Ctrl+Alt to release.", 4.0);
}

// ------------------------------------------------------------------ Frame generation presentation

static void EnsureFrameGen(App& app, int w, int h) {
  Settings& s = GetSettings();
  if (app.fgPending.valid() && app.fgPending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto fg = app.fgPending.get();
    if (fg && fg->Ready()) {
      app.fg = std::move(fg);
      app.pstats.fgState = "Active";
    } else {
      memcpy(app.fgFailKey, app.fgPendingKey, sizeof(app.fgFailKey));
      app.pstats.fgState = "Could not start NVIDIA frame generation (see %APPDATA%\\VidBox\\vidbox.log)";
    }
  }
  int q = std::clamp(s.fgQuality, 0, 2);
  int key[3] = {w, h, q};
  if (app.fg && app.fg->Width() == w && app.fg->Height() == h && app.fg->Quality() == q) {
    app.pstats.fgState = "Active";
    return;
  }
  if (memcmp(key, app.fgFailKey, sizeof(key)) == 0) return;
  if (app.fgPending.valid()) return;
  app.fg.reset();
  memcpy(app.fgPendingKey, key, sizeof(key));
  app.pstats.fgState = "Preparing frame generation (the first run can take a minute)...";
  int dev = app.renderer.CudaDevice();
  cudaStream_t stream = app.renderStream;
  app.fgPending = std::async(std::launch::async, [=]() {
    cudaSetDevice(dev);
    auto fg = std::make_unique<FrameGen>();
    std::string err;
    if (!fg->Init(w, h, q, stream, &err)) {
      LogF("frame generation init failed: %s", err.c_str());
      return std::unique_ptr<FrameGen>();
    }
    return fg;
  });
}

static void UpdateVideo(App& app) {
  if (app.screen != Screen::Main || !app.pipelineReady) return;
  Settings& s = GetSettings();
  PresentStats& ps = app.pstats;
  double now = NowSec();
  ps.refreshHz = app.renderer.RefreshRate();
  ps.outputFps = app.outRate.Rate(now);
  ps.fgMs = app.fgMs.value;

  if (!s.fgEnabled) {
    // Lowest latency: always show the newest decoded frame.
    ps.addedLatencyMs = 0;
    ps.multiplier = 1;
    app.vclock = 0;
    Pipeline::Frame f;
    if (app.pipeline.PickLatest(&f)) {
      if (f.id != app.shownA || app.shownStep != -1) {
        if (app.renderer.UploadVideo(app.pipeline.SlotImage(f.slot), app.renderStream)) {
          cudaStreamSynchronize(app.renderStream);
          app.shownA = f.id;
          app.shownB = 0;
          app.shownStep = -1;
          app.outRate.Tick(now);
        }
      }
      app.pipeline.Release(f);
    }
    return;
  }

  // A steady clock advancing one refresh per loop iteration (the loop is vsync-locked).
  double period = 1.0 / ps.refreshHz;
  if (app.vclock == 0 || std::fabs(now - app.vclock) > 2 * period) app.vclock = now;
  else {
    app.vclock += period;
    app.vclock += 0.1 * (now - app.vclock);
  }

  double interval = std::clamp(app.pipeline.UniqueInterval(), 1.0 / 240, 0.1);
  int M = 0;  // 0 = new frame every refresh
  switch (s.fgTarget) {
    case FgTarget::MatchDisplay: M = 0; break;
    case FgTarget::X2: M = 2; break;
    case FgTarget::X3: M = 3; break;
    case FgTarget::X4: M = 4; break;
    case FgTarget::Custom: M = std::clamp(int(std::lround(s.fgCustomFps * interval)), 1, 8); break;
  }
  ps.multiplier = M ? M : std::max(1, int(std::lround(interval * ps.refreshHz)));

  if (s.fgAdaptiveBuffer) {
    if (app.extraBufferMs > 0 && now - app.lastStarveTime > 5.0) {
      app.extraBufferMs = std::max(0.0, app.extraBufferMs - 1.0);
      app.lastStarveTime = now;
    }
  } else {
    app.extraBufferMs = 0;
  }
  ps.extraBufferMs = app.extraBufferMs;
  // Show content from one game frame (+ jitter buffer) in the past, so the next real frame is always
  // available to interpolate towards.
  double delay = interval + (s.fgBufferMs + app.extraBufferMs) / 1000.0;
  ps.addedLatencyMs = delay * 1000.0;
  Pipeline::Pick p = app.pipeline.PickAt(app.vclock - delay, s.fgMaxGapMs / 1000.0);
  if (!p.hasA) {
    app.pipeline.Release(p);
    if (ps.fgState.empty()) ps.fgState = "Waiting for video";
    return;
  }
  if (p.starved && p.a.id != app.lastStarveId) {
    app.lastStarveId = p.a.id;
    ps.starvations++;
    if (s.fgAdaptiveBuffer) app.extraBufferMs = std::min(30.0, app.extraBufferMs + 2.0);
    app.lastStarveTime = now;
  }
  EnsureFrameGen(app, p.a.width, p.a.height);
  bool fgReady = app.fg && app.fg->Ready() && app.fg->Width() == p.a.width && app.fg->Height() == p.a.height;

  // Decide what to show: a real frame, or a generated one at timestep tq.
  uint64_t keyA, keyB = 0;
  int keyStep = -1;
  const GpuImage* real = nullptr;
  bool generate = false;
  float tq = 0;
  auto showReal = [&](const Pipeline::Frame& f) {
    keyA = f.id;
    real = &app.pipeline.SlotImage(f.slot);
  };
  if (!p.hasB || !p.interpolate) {
    showReal(p.a);
  } else {
    int step;
    if (M > 0) { step = int(std::floor(p.t * M + 1e-4)); tq = float(step) / M; }
    else { step = int(p.t * 1000); tq = p.t; }
    if (tq < 0.01f) showReal(p.a);
    else if (tq > 0.99f) showReal(p.b);
    else if (fgReady) { generate = true; keyA = p.a.id; keyB = p.b.id; keyStep = step; }
    else showReal(tq < 0.5f ? p.a : p.b);
  }

  if (keyA == app.shownA && keyB == app.shownB && keyStep == app.shownStep) {
    app.pipeline.Release(p);
    return;  // nothing new to show this refresh
  }

  bool ok = false;
  if (generate) {
    double g0 = NowSec();
    const GpuImage* out = app.fg->Run(app.pipeline.SlotImage(p.a.slot), app.pipeline.SlotImage(p.b.slot), tq, false);
    if (out) {
      ok = app.renderer.UploadVideo(*out, app.renderStream);
      cudaStreamSynchronize(app.renderStream);
      app.fgMs.Add((NowSec() - g0) * 1000, 0.05);
    }
    if (!ok) {  // fall back to the nearest real frame
      showReal(tq < 0.5f ? p.a : p.b);
      keyB = 0;
      keyStep = -1;
    }
  }
  if (!ok && real) {
    ok = app.renderer.UploadVideo(*real, app.renderStream);
    cudaStreamSynchronize(app.renderStream);
  }
  if (ok) {
    app.shownA = keyA;
    app.shownB = keyB;
    app.shownStep = keyStep;
    app.outRate.Tick(now);
  }
  app.pipeline.Release(p);
}

static void PollTasks(App& app) {
  if (app.loginTask.valid() && app.loginTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    LoginResult r = app.loginTask.get();
    if (r.ok) {
      app.OnLoggedIn();
    } else {
      app.screen = Screen::Login;
      app.loginError = r.error;
      app.loginCertMismatch = r.certMismatch;
    }
  }
  if (app.formatTask.valid() && app.formatTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto r = app.formatTask.get();
    app.captureFormats = std::move(r.first);
    app.captureFormatsError = r.second;
    app.captureFormatsFor = app.formatTaskFor;
  }
  if (app.toastUntil && NowSec() > app.toastUntil) { app.toast.clear(); app.toastUntil = 0; }
}

static void SendPendingMouse(App& app) {
  double now = NowSec();
  if (app.absPending && now - app.lastAbsSend >= 0.010) {
    app.kvm.SendMouseMove(app.absX, app.absY);
    app.absPending = false;
    app.lastAbsSend = now;
  }
  app.kvm.FlushRelative();
}

// ------------------------------------------------------------------ Input

static bool InVideo(App* a, int x, int y) {
  return a->renderer.HasVideo() && x >= a->videoX0 && x < a->videoX1 && y >= a->videoY0 && y < a->videoY1;
}

static void MapAbsolute(App* a, int x, int y) {
  double nx = std::clamp((x - a->videoX0) / std::max(1.0f, a->videoX1 - a->videoX0), 0.0f, 1.0f);
  double ny = std::clamp((y - a->videoY0) / std::max(1.0f, a->videoY1 - a->videoY0), 0.0f, 1.0f);
  a->absX = int(std::lround(nx * 65535.0 - 32768.0));
  a->absY = int(std::lround(ny * 65535.0 - 32768.0));
  a->absPending = true;
}

static bool MouseActive(App* a) {
  // A capture card has no way back to the console, so input is only forwarded in KVM mode.
  return a->screen == Screen::Main && !a->CaptureMode() && GetSettings().mouseEnabled;
}

static void OnMouseButton(App* a, int index, bool down, LPARAM lp) {
  static const char* names[] = {"left", "right", "middle", "up", "down"};
  if (!MouseActive(a)) return;
  unsigned bit = 1u << index;
  int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
  if (down) {
    bool overUi = ImGui::GetIO().WantCaptureMouse;
    if (!a->relativeCaptured && (overUi || !InVideo(a, x, y))) return;
    if (!a->status.mouseAbsolute && !a->relativeCaptured) {
      CaptureRelativeMouse(a);  // first click only captures the mouse
      return;
    }
    if (a->status.mouseAbsolute) {
      MapAbsolute(a, x, y);
      a->kvm.SendMouseMove(a->absX, a->absY);
      a->absPending = false;
    }
    a->kvm.SendMouseButton(names[index], true);
    a->buttonsDown |= bit;
    SetCapture(a->hwnd);
  } else if (a->buttonsDown & bit) {
    a->kvm.SendMouseButton(names[index], false);
    a->buttonsDown &= ~bit;
    if (!a->buttonsDown && !a->relativeCaptured) ReleaseCapture();
  }
}

static bool OnKey(App* a, UINT msg, WPARAM wp, LPARAM lp) {
  if (a->screen != Screen::Main) return false;
  bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
  if (wp == VK_F11) {
    if (down && !(lp & (1 << 30))) a->ToggleFullscreen();
    return true;
  }
  if (a->relativeCaptured && down && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_MENU) & 0x8000)) {
    a->kvm.ReleaseAllKeys();
    a->ReleaseMouseCapture();
    a->Toast("Mouse released");
    return true;
  }
  if (ImGui::GetIO().WantTextInput) return false;
  if (a->CaptureMode() || !GetSettings().keyboardEnabled) return false;
  const char* code = KeyCodeFromMessage(wp, lp);
  if (!code) return false;
  a->kvm.SendKey(code, down);
  return true;
}

static void OnWheel(App* a, int delta, bool horizontal, int sx, int sy) {
  if (!MouseActive(a)) return;
  POINT pt = {sx, sy};
  ScreenToClient(a->hwnd, &pt);
  if (!a->relativeCaptured && (ImGui::GetIO().WantCaptureMouse || !InVideo(a, pt.x, pt.y))) return;
  const Settings& s = GetSettings();
  a->wheelAccum += delta;
  while (std::abs(a->wheelAccum) >= WHEEL_DELTA) {
    int sign = a->wheelAccum > 0 ? 1 : -1;
    int v = sign * s.scrollRate * (s.reverseScroll ? -1 : 1);
    if (horizontal) a->kvm.SendWheel(-v, 0);
    else a->kvm.SendWheel(0, v);
    a->wheelAccum -= sign * WHEEL_DELTA;
  }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  App* a = g_app;
  if (msg == WM_SETCURSOR && a && LOWORD(lp) == HTCLIENT && ImGui::GetCurrentContext()) {
    // Must run before ImGui's handler, which would otherwise restore the arrow cursor.
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    bool hide = a->cursorHidden ||
                (a->screen == Screen::Main && !a->CaptureMode() && !GetSettings().showCursor && a->status.mouseAbsolute &&
                 InVideo(a, pt.x, pt.y) && !ImGui::GetIO().WantCaptureMouse);
    if (hide) { SetCursor(nullptr); return TRUE; }
  }
  if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
  switch (msg) {
    case WM_SIZE:
      if (a && a->renderer.Device() && wp != SIZE_MINIMIZED) a->renderer.Resize(LOWORD(lp), HIWORD(lp));
      return 0;
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMinTrackSize = {640, 420};
      return 0;
    }
    case WM_DPICHANGED: {
      RECT* r = reinterpret_cast<RECT*>(lp);
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      if (ImGui::GetCurrentContext()) ImGui::GetStyle().FontScaleDpi = HIWORD(wp) / 96.0f;
      return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
      if (a && OnKey(a, msg, wp, lp)) return 0;
      break;
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // Alt is forwarded to the console, don't open the menu
      break;
    case WM_MOUSEMOVE:
      if (a && MouseActive(a) && a->status.mouseAbsolute && !a->relativeCaptured) {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (a->buttonsDown || (InVideo(a, x, y) && !ImGui::GetIO().WantCaptureMouse)) MapAbsolute(a, x, y);
      }
      break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: if (a) OnMouseButton(a, 0, true, lp); break;
    case WM_LBUTTONUP: if (a) OnMouseButton(a, 0, false, lp); break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: if (a) OnMouseButton(a, 1, true, lp); break;
    case WM_RBUTTONUP: if (a) OnMouseButton(a, 1, false, lp); break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: if (a) OnMouseButton(a, 2, true, lp); break;
    case WM_MBUTTONUP: if (a) OnMouseButton(a, 2, false, lp); break;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
      if (a) OnMouseButton(a, GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 3 : 4, true, lp);
      return TRUE;
    case WM_XBUTTONUP:
      if (a) OnMouseButton(a, GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 3 : 4, false, lp);
      return TRUE;
    case WM_MOUSEWHEEL:
      if (a) OnWheel(a, GET_WHEEL_DELTA_WPARAM(wp), false, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      break;
    case WM_MOUSEHWHEEL:
      if (a) OnWheel(a, GET_WHEEL_DELTA_WPARAM(wp), true, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      break;
    case WM_INPUT:
      if (a && a->relativeCaptured) {
        RAWINPUT ri;
        UINT sz = sizeof(ri);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
          double k = GetSettings().relativeSense / 10.0;
          int dx = int(std::lround(ri.data.mouse.lLastX * k)), dy = int(std::lround(ri.data.mouse.lLastY * k));
          if (dx || dy) a->kvm.SendMouseRelative(dx, dy);
        }
      }
      break;
    case WM_ACTIVATEAPP:
      if (a && !wp) {
        a->kvm.ReleaseAllKeys();
        ReleaseButtons(a);
        a->ReleaseMouseCapture();
      }
      break;
    case WM_KILLFOCUS:
      if (a) a->kvm.ReleaseAllKeys();
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ Self test (no KVM needed)

static int RunSelfTest(App& app) {
  LogF("=== self test ===");
  int dev = app.renderer.CudaDevice();
  cudaSetDevice(dev);
  cudaStream_t st = app.renderStream;
  const int W = 1920, H = 1080;
  GpuImage a, b;
  a.Alloc(W, H);
  b.Alloc(W, H);
  cudaMemset2D(a.ptr, a.pitch, 40, size_t(W) * 4, H);
  cudaMemset2D(b.ptr, b.pitch, 200, size_t(W) * 4, H);
  int failures = 0;
  if (!VfxInitLibrary(nullptr)) { LogF("selftest: VFX SDK missing"); return 2; }

  for (int q = 0; q <= 2; ++q) {
    FrameGen fg;
    std::string err;
    double t0 = NowSec();
    if (!fg.Init(W, H, q, st, &err)) { LogF("selftest: FrameGen q%d init FAILED: %s", q, err.c_str()); ++failures; continue; }
    LogF("selftest: FrameGen q%d init %.0f ms", q, (NowSec() - t0) * 1000);
    for (int i = 0; i < 5; ++i) { fg.Run(a, b, 0.5f, false); cudaStreamSynchronize(st); }  // warm up
    const int N = 60;
    t0 = NowSec();
    const GpuImage* out = nullptr;
    for (int i = 0; i < N; ++i) out = fg.Run(a, b, float(i % 9 + 1) / 10.0f, false);
    cudaStreamSynchronize(st);
    double ms = (NowSec() - t0) * 1000 / N;
    unsigned char px[4] = {};
    if (out) cudaMemcpy(px, out->ptr + (H / 2) * out->pitch + (W / 2) * 4, 4, cudaMemcpyDeviceToHost);
    LogF("selftest: FrameGen q%d 1080p: %.2f ms/frame (max %.0f fps), mid pixel %u,%u,%u", q, ms, 1000 / ms, px[0], px[1], px[2]);
    if (!out) ++failures;
  }

  int modes[] = {3, 21, 23};
  for (int m : modes) {
    SuperRes sr;
    std::string err;
    if (!sr.Init(W, H, 2560, 1440, m, st, &err)) { LogF("selftest: VSR mode %d init FAILED: %s", m, err.c_str()); ++failures; continue; }
    GpuImage o;
    o.Alloc(2560, 1440);
    for (int i = 0; i < 3; ++i) sr.Run(a, o);
    cudaStreamSynchronize(st);
    double t0 = NowSec();
    for (int i = 0; i < 30; ++i) sr.Run(a, o);
    cudaStreamSynchronize(st);
    LogF("selftest: VSR mode %d 1080p->1440p: %.2f ms/frame", m, (NowSec() - t0) * 1000 / 30);
  }
  LogF("=== self test done, %d failure(s) ===", failures);
  return failures ? 1 : 0;
}

// Lists capture devices and formats, then streams the saved (or first) device through the pipeline for 5 s.
static int RunCaptureTest(App& app) {
  LogF("=== capture test ===");
  auto devs = CaptureClient::ListDevices();
  LogF("capturetest: %d device(s)", int(devs.size()));
  for (auto& d : devs) {
    std::string err;
    auto fmts = CaptureClient::ListFormats(d.link, &err);
    LogF("capturetest: \"%s\" %d format(s)%s%s", d.name.c_str(), int(fmts.size()), err.empty() ? "" : " - ", err.c_str());
    for (auto& f : fmts) LogF("capturetest:    %s", f.Label().c_str());
    int def = CaptureClient::PickDefault(fmts);
    if (def >= 0) LogF("capturetest:    default -> %s", fmts[def].Label().c_str());
  }
  if (devs.empty()) return 2;
  const Settings& s = GetSettings();
  CaptureDevice dev = devs[0];
  for (auto& d : devs) if (d.link == s.captureLink) dev = d;
  std::atomic<int> frames{0};
  app.capture.Start(dev, CaptureFormat(), [&](const RawFrame& f, double arrival) {
    frames++;
    app.pipeline.OnRawFrame(f, arrival);
  }, [&]() { app.pipeline.Reset(); });
  auto ains = AudioPassthrough::ListInputs();
  for (auto& a : ains) LogF("capturetest: audio input \"%s\"", a.name.c_str());
  int ai = AudioPassthrough::MatchInput(ains, dev.name);
  LogF("capturetest: audio match -> %s", ai >= 0 ? ains[ai].name.c_str() : "(none)");
  if (ai >= 0) app.audio.Start(ains[ai]);
  for (int i = 0; i < 10; ++i) {
    Sleep(500);
    AudioStatus as = app.audio.Status();
    LogF("capturetest: audio active=%d err=\"%s\" peak %.3f queued %.0f ms -> %s", as.active, as.error.c_str(), as.peak,
         as.latencyMs, as.output.c_str());
    CaptureStatus cs = app.capture.Status();
    PipelineStats st = app.pipeline.Stats();
    LogF("capturetest: t=%.1fs opened=%d signal=%d err=\"%s\" cap %.1f fps, unique %.1f fps, %dx%d via %s, convert %.2f ms, diff %.2f",
         (i + 1) * 0.5, cs.opened, cs.signal, cs.error.c_str(), st.kvmFps, st.uniqueFps, st.decodedW, st.decodedH,
         st.decoderName.c_str(), st.decodeMs, st.lastDiff);
  }
  app.audio.Stop();
  app.capture.Stop();
  LogF("=== capture test done: %d frames ===", frames.load());
  return frames > 0 ? 0 : 1;
}

// Logs in with the remembered password and runs the GLKVM WebRTC audio session for 10 s.
static int RunKvmAudioTest(App& app) {
  LogF("=== kvm audio test ===");
  Settings& s = GetSettings();
  if (!s.rememberPassword || s.passwordProtected.empty()) {
    LogF("kvmaudiotest: no remembered password; log in once with \"Remember password\" ticked");
    return 2;
  }
  std::string err;
  bool mismatch = false;
  app.kvm.SetPinnedCert(s.certPin);
  if (!app.kvm.Login(s.host, s.user, UnprotectString(s.passwordProtected), &err, &mismatch)) {
    LogF("kvmaudiotest: login failed: %s", err.c_str());
    return 3;
  }
  app.kvm.Start([](const uint8_t*, size_t, bool, double) {}, []() {});
  std::atomic<long long> frames{0};
  app.kvmPlayer.Start(48000, 2);
  app.kvm.StartAudio([&](const float* pcm, int n, int rate, int ch) {
    frames += n;
    app.kvmPlayer.Push(pcm, n, rate, ch);
  });
  for (int i = 0; i < 20; ++i) {
    Sleep(500);
    KvmStatus st = app.kvm.Status();
    AudioStatus as = app.kvmPlayer.Status();
    LogF("kvmaudiotest: t=%.1fs state=%s connected=%d err=\"%s\" samples=%lld peak %.3f queued %.0f ms -> %s",
         (i + 1) * 0.5, st.audioState.c_str(), st.audioConnected, st.audioError.c_str(), frames.load(), as.peak,
         as.latencyMs, as.output.c_str());
  }
  app.kvm.Stop();  // keeps the session token valid for the running app
  app.kvmPlayer.Stop();
  LogF("=== kvm audio test done: %lld samples ===", frames.load());
  return frames > 0 ? 0 : 1;
}

// ------------------------------------------------------------------ Entry point

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
  ImGui_ImplWin32_EnableDpiAwareness();
  timeBeginPeriod(1);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Settings& s = GetSettings();
  s.Load();

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wc.lpszClassName = L"VidBoxWindow";
  RegisterClassExW(&wc);

  float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
  int ww = int(1440 * dpi), wh = int(880 * dpi);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"VidBox", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, ww,
                              wh, nullptr, nullptr, hInst, nullptr);

  auto app = std::make_unique<App>();
  app->hwnd = hwnd;
  std::string err;
  if (!app->renderer.Init(hwnd, &err)) {
    MessageBoxW(hwnd, Widen(err).c_str(), L"VidBox", MB_ICONERROR);
    return 1;
  }
  g_app = app.get();
  cudaSetDevice(app->renderer.CudaDevice());
  cudaStreamCreate(&app->renderStream);
  if (app->pipeline.Init(app->renderer.CudaDevice(), &err)) app->pipelineReady = true;
  else app->fatalError = "Video decoder failed to start: " + err;
  if (!VfxInitLibrary(nullptr)) LogF("NVIDIA VFX SDK unavailable: frame generation and super resolution disabled");
  const wchar_t* cmdline = GetCommandLineW();
  if (wcsstr(cmdline, L"--selftest") || wcsstr(cmdline, L"--capture-test") || wcsstr(cmdline, L"--kvm-audio-test")) {
    int rc = wcsstr(cmdline, L"--selftest")       ? RunSelfTest(*app)
             : wcsstr(cmdline, L"--capture-test") ? RunCaptureTest(*app)
                                                  : RunKvmAudioTest(*app);
    app->capture.Stop();
    app->pipeline.Shutdown();
    app->renderer.Shutdown();
    return rc;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
  SetupImGuiStyle(dpi);
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(app->renderer.Device(), app->renderer.Context());

  strncpy(app->host, s.host.c_str(), sizeof(app->host) - 1);
  strncpy(app->user, s.user.c_str(), sizeof(app->user) - 1);
  app->remember = s.rememberPassword;
  if (s.mode == SourceMode::Capture) {
    if (s.captureAutoStart && !s.captureLink.empty()) app->StartCapture();
  } else if (s.autoConnect && s.rememberPassword && !s.passwordProtected.empty()) {
    app->StartLogin(true);
  }

  ShowWindow(hwnd, nCmdShow);
  UpdateWindow(hwnd);

  bool done = false;
  while (!done) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (done) break;
    if (IsIconic(hwnd)) { Sleep(16); continue; }

    app->renderer.WaitForNextFrame();
    PollTasks(*app);
    app->status = app->kvm.Status();
    app->cstatus = app->capture.Status();
    {
      double sync = (s.audioSync && s.fgEnabled) ? app->pstats.addedLatencyMs : 0.0;
      if (app->CaptureMode()) {
        app->astatus = app->audio.Status();
        app->audio.SetVolume(s.audioVolume);
        app->audio.SetMuted(s.audioMuted);
        app->audio.SetExtraDelayMs(sync + s.audioOffsetMs);
      } else {
        app->astatus = app->kvmPlayer.Status();
        app->kvmPlayer.SetVolume(s.audioVolume);
        app->kvmPlayer.SetMuted(s.audioMuted);
        app->kvmPlayer.SetExtraDelayMs(sync + s.audioOffsetMs);
      }
    }
    app->stats = app->pipeline.Stats();
    UpdateVideo(*app);
    SendPendingMouse(*app);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawUi(*app);
    ImGui::Render();
    app->renderer.BeginFrame(0.0f, 0.0f, 0.0f);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    bool vsync = s.fgEnabled || s.vsync;
    app->renderer.Present(vsync);
  }

  app->ReleaseMouseCapture();
  app->kvm.Stop();
  app->kvmPlayer.Stop();
  app->audio.Stop();
  app->capture.Stop();
  if (app->formatTask.valid()) app->formatTask.wait();
  if (app->loginTask.valid()) app->loginTask.wait();
  if (app->fgPending.valid()) app->fgPending.wait();
  app->fg.reset();
  app->pipeline.Shutdown();
  s.Save();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  g_app = nullptr;
  app->renderer.Shutdown();
  CoUninitialize();
  timeEndPeriod(1);
  return 0;
}
