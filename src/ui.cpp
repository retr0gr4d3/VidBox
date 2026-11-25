// ImGui UI modelled on the GLKVM web interface: video fills the window, a floating icon toolbar at the top
// opens drop-down panels, and a status bar runs along the bottom.
#include "app.h"
#include "common.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"

// ------------------------------------------------------------------ theme

namespace col {
static const ImVec4 Bg = ImVec4(0.067f, 0.071f, 0.082f, 1.0f);        // #111215
static const ImVec4 Panel = ImVec4(0.118f, 0.122f, 0.137f, 0.97f);    // #1e1f23
static const ImVec4 Panel2 = ImVec4(0.161f, 0.165f, 0.184f, 1.0f);    // #292a2f
static const ImVec4 Border = ImVec4(1, 1, 1, 0.08f);
static const ImVec4 Text = ImVec4(0.93f, 0.93f, 0.95f, 1.0f);
static const ImVec4 TextDim = ImVec4(0.60f, 0.62f, 0.66f, 1.0f);
static const ImVec4 Accent = ImVec4(0.204f, 0.471f, 0.965f, 1.0f);   // #3478f6
static const ImVec4 AccentHover = ImVec4(0.290f, 0.545f, 1.0f, 1.0f);
static const ImVec4 Success = ImVec4(0.30f, 0.78f, 0.47f, 1.0f);
static const ImVec4 Warning = ImVec4(0.98f, 0.70f, 0.20f, 1.0f);
static const ImVec4 Error = ImVec4(0.95f, 0.33f, 0.33f, 1.0f);
}  // namespace col

static ImU32 U32(const ImVec4& c, float a = -1) {
  ImVec4 v = c;
  if (a >= 0) v.w = a;
  return ImGui::ColorConvertFloat4ToU32(v);
}

// Segoe Fluent Icons / Segoe MDL2 Assets code points.
static const char* Icon(unsigned cp) {
  static char buf[64][5];
  static int n = 0;
  char* b = buf[n++ & 63];
  b[0] = char(0xE0 | (cp >> 12));
  b[1] = char(0x80 | ((cp >> 6) & 0x3F));
  b[2] = char(0x80 | (cp & 0x3F));
  b[3] = 0;
  return b;
}
enum : unsigned {
  IC_VIDEO = 0xE714, IC_BOLT = 0xE945, IC_ZOOM = 0xE8A3, IC_KEYBOARD = 0xE765, IC_MOUSE = 0xE962,
  IC_POWER = 0xE7E8, IC_SETTINGS = 0xE713, IC_FULL = 0xE740, IC_UNFULL = 0xE73F, IC_UP = 0xE70E,
  IC_DOWN = 0xE70D, IC_STATS = 0xE9D9, IC_SIGNOUT = 0xF3B1, IC_WARN = 0xE7BA, IC_LOCK = 0xE72E,
  IC_CHECK = 0xE73E, IC_VOLUME = 0xE767, IC_MUTE = 0xE74F
};

static float g_dpi = 1.0f;
static ImFont* g_font = nullptr;

void SetupImGuiStyle(float dpi) {
  g_dpi = dpi;
  ImGuiIO& io = ImGui::GetIO();
  char winDir[MAX_PATH] = "C:\\Windows";
  GetWindowsDirectoryA(winDir, MAX_PATH);
  const std::string fontDir = std::string(winDir) + "\\Fonts\\";
  const std::string fontPath = fontDir + "segoeui.ttf";
  if (GetFileAttributesA(fontPath.c_str()) != INVALID_FILE_ATTRIBUTES) g_font = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f);
  else g_font = io.Fonts->AddFontDefault();
  const std::string iconFonts[] = {fontDir + "SegoeIcons.ttf", fontDir + "segmdl2.ttf"};
  for (const std::string& path : iconFonts) {
    const char* f = path.c_str();
    if (GetFileAttributesA(f) == INVALID_FILE_ATTRIBUTES) continue;
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.GlyphOffset = ImVec2(0, 2.0f);
    io.Fonts->AddFontFromFileTTF(f, 16.0f, &cfg);
    break;
  }

  ImGuiStyle& s = ImGui::GetStyle();
  ImGui::StyleColorsDark(&s);
  s.WindowRounding = 10;
  s.ChildRounding = 8;
  s.FrameRounding = 6;
  s.PopupRounding = 8;
  s.GrabRounding = 6;
  s.TabRounding = 6;
  s.ScrollbarRounding = 8;
  s.WindowBorderSize = 1;
  s.FrameBorderSize = 0;
  s.WindowPadding = ImVec2(14, 12);
  s.FramePadding = ImVec2(10, 6);
  s.ItemSpacing = ImVec2(8, 8);
  s.ItemInnerSpacing = ImVec2(6, 6);
  s.GrabMinSize = 12;
  s.ScrollbarSize = 12;
  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = col::Text;
  c[ImGuiCol_TextDisabled] = col::TextDim;
  c[ImGuiCol_WindowBg] = col::Panel;
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = col::Panel;
  c[ImGuiCol_Border] = col::Border;
  c[ImGuiCol_FrameBg] = col::Panel2;
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.205f, 0.23f, 1);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.23f, 0.235f, 0.26f, 1);
  c[ImGuiCol_TitleBg] = col::Panel;
  c[ImGuiCol_TitleBgActive] = col::Panel;
  c[ImGuiCol_Button] = col::Panel2;
  c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.225f, 0.25f, 1);
  c[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.265f, 0.29f, 1);
  c[ImGuiCol_Header] = ImVec4(col::Accent.x, col::Accent.y, col::Accent.z, 0.25f);
  c[ImGuiCol_HeaderHovered] = ImVec4(col::Accent.x, col::Accent.y, col::Accent.z, 0.35f);
  c[ImGuiCol_HeaderActive] = ImVec4(col::Accent.x, col::Accent.y, col::Accent.z, 0.45f);
  c[ImGuiCol_CheckMark] = col::Accent;
  c[ImGuiCol_SliderGrab] = col::Accent;
  c[ImGuiCol_SliderGrabActive] = col::AccentHover;
  c[ImGuiCol_Separator] = col::Border;
  c[ImGuiCol_Tab] = col::Panel2;
  c[ImGuiCol_TabHovered] = col::AccentHover;
  c[ImGuiCol_TabSelected] = col::Accent;
  s.ScaleAllSizes(dpi);
  s.FontSizeBase = 16.0f;
  s.FontScaleDpi = dpi;
}

// ------------------------------------------------------------------ widgets

static bool Toggle(const char* label, bool* v) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w->SkipItems) return false;
  float h = ImGui::GetFrameHeight() * 0.8f, wd = h * 1.8f;
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::PushID(label);
  bool clicked = ImGui::InvisibleButton("##t", ImVec2(wd, h));
  if (clicked) *v = !*v;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 bg = *v ? U32(ImGui::IsItemHovered() ? col::AccentHover : col::Accent)
                : U32(ImGui::IsItemHovered() ? ImVec4(0.32f, 0.33f, 0.36f, 1) : ImVec4(0.27f, 0.28f, 0.31f, 1));
  dl->AddRectFilled(p, ImVec2(p.x + wd, p.y + h), bg, h * 0.5f);
  float r = h * 0.5f - 2.5f;
  float cx = *v ? p.x + wd - r - 2.5f : p.x + r + 2.5f;
  dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f), r, IM_COL32_WHITE);
  ImGui::SameLine();
  ImGui::TextUnformatted(label);
  ImGui::PopID();
  return clicked;
}

static void SectionTitle(const char* t) {
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, col::TextDim);
  ImGui::TextUnformatted(t);
  ImGui::PopStyleColor();
}

static void Hint(const char* t) {
  ImGui::PushStyleColor(ImGuiCol_Text, col::TextDim);
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(t);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

// Segmented control (like the web UI's option buttons). Returns true when changed.
static bool Segmented(const char* id, int* value, const char* const* labels, const int* values, int count) {
  bool changed = false;
  ImGui::PushID(id);
  float avail = ImGui::GetContentRegionAvail().x;
  float spacing = 4 * g_dpi;
  float w = (avail - spacing * (count - 1)) / count;
  for (int i = 0; i < count; ++i) {
    if (i) ImGui::SameLine(0, spacing);
    bool sel = *value == values[i];
    ImGui::PushStyleColor(ImGuiCol_Button, sel ? col::Accent : col::Panel2);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel ? col::AccentHover : ImVec4(0.22f, 0.225f, 0.25f, 1));
    if (ImGui::Button(labels[i], ImVec2(w, 0)) && !sel) { *value = values[i]; changed = true; }
    ImGui::PopStyleColor(2);
  }
  ImGui::PopID();
  return changed;
}

static bool IconButton(const char* icon, const char* tip, bool active, float size) {
  ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(col::Accent.x, col::Accent.y, col::Accent.z, 0.9f)
                                                : ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? col::AccentHover : ImVec4(1, 1, 1, 0.10f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.16f));
  ImGui::PushFont(nullptr, 19.0f);
  bool r = ImGui::Button(icon, ImVec2(size, size));
  ImGui::PopFont();
  ImGui::PopStyleColor(3);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tip);
  return r;
}

static void StatusDot(ImVec4 c) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = ImGui::GetTextLineHeight();
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + h * 0.3f, p.y + h * 0.55f), h * 0.25f, U32(c));
  ImGui::Dummy(ImVec2(h * 0.7f, h));
  ImGui::SameLine(0, 4 * g_dpi);
}

// ------------------------------------------------------------------ login

static int FindFormat(const std::vector<CaptureFormat>& fmts) {
  const Settings& s = GetSettings();
  if (s.captureSubtype.empty()) return -1;
  for (int i = 0; i < int(fmts.size()); ++i) {
    const CaptureFormat& f = fmts[i];
    if (f.subtype == s.captureSubtype && f.width == s.captureW && f.height == s.captureH &&
        f.fpsNum == s.captureFpsNum && f.fpsDen == s.captureFpsDen)
      return i;
  }
  return -1;
}

// Format picker. "Automatic" = highest frame rate, then resolution. Returns true when the choice changed.
static bool CaptureFormatCombo(const std::vector<CaptureFormat>& fmts, const char* autoNote) {
  Settings& s = GetSettings();
  int cur = FindFormat(fmts);
  std::string autoLabel = "Automatic";
  if (autoNote && *autoNote) autoLabel += std::string("  (") + autoNote + ")";
  bool changed = false;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##fmt", cur < 0 ? autoLabel.c_str() : fmts[cur].Label().c_str(), ImGuiComboFlags_HeightLarge)) {
    if (ImGui::Selectable(autoLabel.c_str(), cur < 0) && cur >= 0) {
      s.captureSubtype.clear();
      s.captureW = s.captureH = 0;
      s.captureFpsNum = 0;
      s.captureFpsDen = 1;
      changed = true;
    }
    for (int i = 0; i < int(fmts.size()); ++i) {
      ImGui::PushID(i);
      if (ImGui::Selectable(fmts[i].Label().c_str(), i == cur) && i != cur) {
        s.captureSubtype = fmts[i].subtype;
        s.captureW = fmts[i].width;
        s.captureH = fmts[i].height;
        s.captureFpsNum = fmts[i].fpsNum;
        s.captureFpsDen = fmts[i].fpsDen;
        changed = true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  if (changed) s.Save();
  return changed;
}

static void DrawKvmLoginForm(App& app) {
  bool busy = app.screen == Screen::Connecting;
  ImGui::BeginDisabled(busy);
  ImGui::SetNextItemWidth(-1);
  ImGui::TextUnformatted("Device address");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##host", "192.168.8.138", app.host, sizeof(app.host));
  ImGui::TextUnformatted("Username");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##user", app.user, sizeof(app.user));
  ImGui::TextUnformatted("Password");
  ImGui::SetNextItemWidth(-1);
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  bool enter = ImGui::InputText("##pass", app.pass, sizeof(app.pass),
                                ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::Checkbox("Remember password on this PC", &app.remember);
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Button, col::Accent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::AccentHover);
  bool go = ImGui::Button(busy ? "Connecting..." : "Log in", ImVec2(-1, 38 * g_dpi));
  ImGui::PopStyleColor(2);
  ImGui::EndDisabled();
  if ((go || enter) && !busy) app.StartLogin(false);

  if (!app.loginError.empty()) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, col::Error);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(app.loginError.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    if (app.loginCertMismatch) {
      if (ImGui::Button("I reset the KVM - trust its new certificate")) {
        GetSettings().certPin.clear();
        GetSettings().Save();
        app.loginError = "Saved certificate cleared. Log in again to trust the new one.";
        app.loginCertMismatch = false;
      }
    }
  }
}

static void DrawCaptureForm(App& app) {
  Settings& s = GetSettings();
  if (!app.devicesScanned) app.RefreshCaptureDevices();

  ImGui::TextUnformatted("Capture card");
  int cur = -1;
  for (int i = 0; i < int(app.captureDevices.size()); ++i) if (app.captureDevices[i].link == s.captureLink) cur = i;
  float refreshW = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - refreshW - ImGui::GetStyle().ItemSpacing.x);
  const char* preview = cur >= 0 ? app.captureDevices[cur].name.c_str()
                                 : (app.captureDevices.empty() ? "No video capture devices found" : "Select...");
  if (ImGui::BeginCombo("##dev", preview)) {
    for (int i = 0; i < int(app.captureDevices.size()); ++i) {
      ImGui::PushID(i);
      if (ImGui::Selectable(app.captureDevices[i].name.c_str(), i == cur) && i != cur) {
        s.captureLink = app.captureDevices[i].link;
        s.captureName = app.captureDevices[i].name;
        s.captureSubtype.clear();  // formats differ between devices
        s.captureW = s.captureH = 0;
        s.Save();
        app.LoadCaptureFormats();
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button("Refresh", ImVec2(refreshW, 0))) app.RefreshCaptureDevices();

  ImGui::TextUnformatted("Format");
  bool loaded = app.captureFormatsFor == s.captureLink && !s.captureLink.empty();
  if (!loaded && !app.formatTask.valid()) app.LoadCaptureFormats();
  if (!loaded) {
    ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##fmtwait", s.captureLink.empty() ? "-" : "Reading formats...")) ImGui::EndCombo();
    ImGui::EndDisabled();
  } else {
    int def = CaptureClient::PickDefault(app.captureFormats);
    std::string note = def >= 0 ? app.captureFormats[def].Label() : std::string();
    CaptureFormatCombo(app.captureFormats, note.c_str());
    if (!app.captureFormatsError.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, col::Warning);
      ImGui::TextWrapped("%s", app.captureFormatsError.c_str());
      ImGui::PopStyleColor();
    }
  }
  if (ImGui::Checkbox("Start automatically on launch", &s.captureAutoStart)) s.Save();
  Hint("Keyboard, mouse and power control need the KVM; a capture card only brings in picture and sound. "
       "Frame generation, super resolution and repeated-frame detection work the same.");
  ImGui::Spacing();
  ImGui::BeginDisabled(s.captureLink.empty());
  ImGui::PushStyleColor(ImGuiCol_Button, col::Accent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::AccentHover);
  if (ImGui::Button("Start", ImVec2(-1, 38 * g_dpi))) app.StartCapture();
  ImGui::PopStyleColor(2);
  ImGui::EndDisabled();
  if (!app.loginError.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::Error);
    ImGui::TextWrapped("%s", app.loginError.c_str());
    ImGui::PopStyleColor();
  }
}

static void DrawLogin(App& app) {
  ImGuiIO& io = ImGui::GetIO();
  Settings& s = GetSettings();
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, U32(col::Bg));
  float w = 440 * g_dpi;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(w, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * g_dpi, 26 * g_dpi));
  ImGui::Begin("##login", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings);
  bool capture = s.mode == SourceMode::Capture;
  ImGui::PushFont(nullptr, 30.0f);
  ImGui::TextUnformatted(capture ? "Capture card" : "GLKVM");
  ImGui::PopFont();
  ImGui::PushStyleColor(ImGuiCol_Text, col::TextDim);
  ImGui::TextUnformatted("VidBox");
  ImGui::PopStyleColor();
  ImGui::Spacing();

  static const char* mLabels[] = {"KVMBox", "CCBox"};
  static const int mValues[] = {0, 1};
  int mode = int(s.mode);
  ImGui::BeginDisabled(app.screen == Screen::Connecting);
  if (Segmented("mode", &mode, mLabels, mValues, 2)) {
    s.mode = SourceMode(mode);
    app.loginError.clear();
    s.Save();
  }
  ImGui::EndDisabled();
  ImGui::PushStyleColor(ImGuiCol_Text, col::TextDim);
  ImGui::TextUnformatted(capture ? "Local HDMI capture card" : "GL.iNet GLKVM over the network");
  ImGui::PopStyleColor();
  ImGui::Spacing();

  if (capture) DrawCaptureForm(app);
  else DrawKvmLoginForm(app);

  if (!app.fatalError.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::Warning);
    ImGui::TextWrapped("%s", app.fatalError.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

// ------------------------------------------------------------------ video

static void DrawVideo(App& app, float bottomBar) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  bg->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 255));
  float aw = io.DisplaySize.x, ah = io.DisplaySize.y - bottomBar;
  app.videoX0 = app.videoY0 = app.videoX1 = app.videoY1 = 0;
  if (!app.renderer.HasVideo() || ah <= 0) return;
  float vw = float(app.renderer.VideoWidth()), vh = float(app.renderer.VideoHeight());
  // Super resolution changes the texture size but not the picture's shape; layout uses the texture aspect.
  float x0, y0, x1, y1;
  switch (GetSettings().layout) {
    case VideoLayout::Stretch: x0 = 0; y0 = 0; x1 = aw; y1 = ah; break;
    case VideoLayout::Original: {
      float sw = float(app.stats.decodedW ? app.stats.decodedW : vw), sh = float(app.stats.decodedH ? app.stats.decodedH : vh);
      float k = std::min(1.0f, std::min(aw / sw, ah / sh));
      float w = sw * k, h = sh * k;
      x0 = std::floor((aw - w) * 0.5f); y0 = std::floor((ah - h) * 0.5f); x1 = x0 + w; y1 = y0 + h;
      break;
    }
    default: {
      float k = std::min(aw / vw, ah / vh);
      float w = vw * k, h = vh * k;
      x0 = std::floor((aw - w) * 0.5f); y0 = std::floor((ah - h) * 0.5f); x1 = x0 + w; y1 = y0 + h;
      break;
    }
  }
  bg->AddImage(ImTextureRef((ImTextureID)(intptr_t)app.renderer.VideoSrv()), ImVec2(x0, y0), ImVec2(x1, y1));
  app.videoX0 = x0; app.videoY0 = y0; app.videoX1 = x1; app.videoY1 = y1;
}

static void DrawCenterMessage(App& app, float bottomBar) {
  const KvmStatus& st = app.status;
  const char* title = nullptr;
  const char* lines[4] = {};
  int n = 0;
  bool retry = false;
  if (app.CaptureMode()) {
    const CaptureStatus& cs = app.cstatus;
    if (!cs.error.empty()) {
      title = "Capture card problem";
      lines[n++] = cs.error.c_str();
      retry = true;
    } else if (!cs.opened) {
      title = "Opening the capture card...";
    } else if (!cs.signal) {
      title = "No signal";
      lines[n++] = "Make sure the Xbox is powered on and not in rest mode.";
      lines[n++] = "Check the HDMI cable between the Xbox and the capture card.";
      lines[n++] = "If the card can't capture the console's mode, set the Xbox to 1080p 60Hz.";
    } else if (!app.renderer.HasVideo()) {
      title = "Waiting for the first frame...";
    }
  } else if (!st.eventsConnected) {
    title = "Connecting to the KVM...";
  } else if (!st.sourceOnline) {
    title = "No HDMI signal";
    lines[n++] = "Make sure the Xbox is powered on and not in rest mode.";
    lines[n++] = "Check the HDMI cable between the Xbox and the KVM.";
    lines[n++] = "If it persists, set the Xbox to 1080p 60Hz in TV & display options.";
  } else if (!st.mediaConnected) {
    title = "Starting video stream...";
    if (!st.mediaError.empty()) lines[n++] = st.mediaError.c_str();
  } else if (!app.renderer.HasVideo()) {
    title = "Waiting for the first frame...";
  }
  if (!title) return;
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, (io.DisplaySize.y - bottomBar) * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.85f);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
  if (!retry) flags |= ImGuiWindowFlags_NoInputs;
  ImGui::Begin("##center", nullptr, flags);
  ImGui::PushFont(nullptr, 22.0f);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  for (int i = 0; i < n; ++i) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::TextDim);
    ImGui::Text("•  %s", lines[i]);
    ImGui::PopStyleColor();
  }
  if (retry) {
    ImGui::Spacing();
    if (ImGui::Button("Try again")) app.StartCapture();
    ImGui::SameLine();
    if (ImGui::Button("Choose another device")) app.Logout();
  }
  ImGui::End();
}

// ------------------------------------------------------------------ panels

static void ComboKeys(App& app, std::initializer_list<const char*> keys) {
  for (const char* k : keys) app.kvm.SendKey(k, true);
  Sleep(30);
  std::vector<const char*> rev(keys);
  std::reverse(rev.begin(), rev.end());
  for (const char* k : rev) app.kvm.SendKey(k, false);
}

static void LayoutPicker() {
  Settings& s = GetSettings();
  SectionTitle("Layout");
  static const char* lLabels[] = {"Fit", "Stretch", "Original"};
  static const int lValues[] = {0, 1, 2};
  int lay = int(s.layout);
  if (Segmented("layout", &lay, lLabels, lValues, 3)) { s.layout = VideoLayout(lay); s.Save(); }
}

static void PanelVideoCapture(App& app) {
  Settings& s = GetSettings();
  const CaptureStatus& cs = app.cstatus;
  SectionTitle("Device");
  ImGui::TextUnformatted(s.captureName.empty() ? "Capture card" : s.captureName.c_str());
  SectionTitle("Capture format");
  std::string note = cs.format.width ? cs.format.Label() : std::string();
  ImGui::BeginDisabled(cs.formats.empty());
  if (CaptureFormatCombo(cs.formats, note.c_str())) app.StartCapture();
  ImGui::EndDisabled();
  if (cs.opened) {
    ImGui::TextDisabled("Receiving %.0f fps", cs.fps);
    if (!cs.format.Native())
      ImGui::TextDisabled("Windows converts %s to %s", cs.format.subtype.c_str(), cs.outputSubtype.c_str());
    if (cs.format.subtype == "MJPG")
      Hint("MJPEG is compressed by the card and decoded on the CPU. Use YUY2/NV12 if your card offers it at the "
           "same resolution and frame rate.");
  }
  SectionTitle("Colour range");
  static const char* rLabels[] = {"Auto", "Limited", "Full"};
  static const int rValues[] = {0, 1, 2};
  if (Segmented("range", &s.captureRange, rLabels, rValues, 3)) {
    app.pipeline.SetCaptureColorRange(s.captureRange);
    s.Save();
  }
  Hint("If blacks look grey or the picture looks washed out, try Full. If shadows are crushed, try Limited.");
  LayoutPicker();
  ImGui::Spacing();
  if (ImGui::Button("Restart capture", ImVec2(-1, 0))) app.StartCapture();
}

static void LevelMeter(float peak) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x, h = 6 * g_dpi;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(col::Panel2), h * 0.5f);
  // dB-ish scale so quiet game audio still moves the bar.
  float db = peak > 0 ? 20.0f * std::log10(peak) : -60.0f;
  float k = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
  if (k > 0) dl->AddRectFilled(p, ImVec2(p.x + w * k, p.y + h), U32(db > -3 ? col::Warning : col::Success), h * 0.5f);
  ImGui::Dummy(ImVec2(w, h));
}

static void PanelAudio(App& app) {
  Settings& s = GetSettings();
  const AudioStatus& as = app.astatus;
  bool capture = app.CaptureMode();
  if (Toggle(capture ? "Play the capture card's audio" : "Play the console's audio from the KVM", &s.audioEnabled)) {
    s.Save();
    app.StartAudio();
  }
  ImGui::BeginDisabled(!s.audioEnabled);
  SectionTitle("Volume");
  float vol = s.audioVolume * 100.0f;
  float bw = ImGui::GetFrameHeight() * 1.4f;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x);
  if (ImGui::SliderFloat("##vol", &vol, 0, 100, "%.0f%%")) { s.audioVolume = vol / 100.0f; s.Save(); }
  ImGui::SameLine();
  if (IconButton(Icon(s.audioMuted ? IC_MUTE : IC_VOLUME), s.audioMuted ? "Unmute" : "Mute",
                 s.audioMuted, bw)) {
    s.audioMuted = !s.audioMuted;
    s.Save();
  }
  LevelMeter(as.active ? as.peak : 0.0f);

  if (capture) {
  SectionTitle("Input");
  std::string autoLabel = "Automatic";
  if (s.captureAudioId.empty() && !app.audioInputName.empty()) autoLabel += "  (" + app.audioInputName + ")";
  const char* preview = autoLabel.c_str();
  for (auto& d : app.audioInputs) if (!s.captureAudioId.empty() && d.id == s.captureAudioId) preview = d.name.c_str();
  float refreshW = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - refreshW - ImGui::GetStyle().ItemSpacing.x);
  if (ImGui::BeginCombo("##ain", preview)) {
    if (ImGui::Selectable(autoLabel.c_str(), s.captureAudioId.empty()) && !s.captureAudioId.empty()) {
      s.captureAudioId.clear();
      s.Save();
      app.StartAudio();
    }
    for (int i = 0; i < int(app.audioInputs.size()); ++i) {
      ImGui::PushID(i);
      bool sel = app.audioInputs[i].id == s.captureAudioId;
      if (ImGui::Selectable(app.audioInputs[i].name.c_str(), sel) && !sel) {
        s.captureAudioId = app.audioInputs[i].id;
        s.Save();
        app.StartAudio();
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button("Refresh", ImVec2(refreshW, 0))) app.StartAudio();
  }

  SectionTitle("Sync");
  if (ImGui::Checkbox("Delay audio to match frame generation", &s.audioSync)) s.Save();
  ImGui::SetNextItemWidth(-1);
  if (ImGui::SliderInt("##aoff", &s.audioOffsetMs, 0, 300, "Extra audio delay %d ms")) s.Save();
  Hint("If the sound comes before the picture, raise the extra delay.");
  ImGui::EndDisabled();

  ImGui::Separator();
  if (!s.audioEnabled) {
    ImGui::TextDisabled("Audio is off");
  } else if (capture && !app.audioNote.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::Warning);
    ImGui::TextWrapped("%s", app.audioNote.c_str());
    ImGui::PopStyleColor();
  } else if (!capture && !as.error.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::Warning);
    ImGui::TextWrapped("%s", as.error.c_str());
    ImGui::PopStyleColor();
  } else if (!capture && !app.status.audioConnected) {
    const KvmStatus& st = app.status;
    if (!st.audioError.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, col::Warning);
      ImGui::TextWrapped("%s", st.audioError.c_str());
      ImGui::PopStyleColor();
      ImGui::TextDisabled("Retrying automatically.");
    } else {
      ImGui::TextDisabled("Connecting to the KVM's audio (%s)...", st.audioState.empty() ? "starting" : st.audioState.c_str());
    }
  } else if (!capture && as.active) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::Success);
    ImGui::TextWrapped("Playing on %s", as.output.c_str());
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Opus over WebRTC, %d Hz, %.0f ms buffered", as.sampleRate, as.latencyMs);
  } else if (!as.error.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::Warning);
    ImGui::TextWrapped("%s", as.error.c_str());
    ImGui::PopStyleColor();
  } else if (as.active) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::Success);
    ImGui::TextWrapped("Playing on %s", as.output.c_str());
    ImGui::PopStyleColor();
    ImGui::TextDisabled("%d Hz, %d ch, %.0f ms buffered", as.sampleRate, as.channels, as.latencyMs);
    if (as.silentSec > 3.0) {
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, col::Warning);
      ImGui::TextWrapped("%s The capture card is sending complete silence.", Icon(IC_WARN));
      ImGui::PopStyleColor();
      if (!app.cstatus.signal)
        Hint("There's no HDMI signal either. Check the console and the HDMI cable.");
      else
        Hint("If a TV or monitor is plugged into the card's HDMI out (passthrough), unplug it: with a screen "
             "attached, some cards (the Razer Ripsaw HD among them) pass the sound to the screen but send silence "
             "to the PC.");
        Hint("Otherwise check, in order: 1) Xbox: Settings > General > Volume & audio output > HDMI audio: \"Stereo "
             "uncompressed\", and \"Allow bitstream passthrough\" off. 2) A headset on the controller or a "
             "wireless headset isn't taking all the game audio. 3) Unplug and replug the HDMI cable from the "
             "console to the card, so the console renegotiates its audio format.");
    }
  } else {
    ImGui::TextDisabled("Starting audio...");
  }
}

static void PanelVideo(App& app) {
  if (app.CaptureMode()) { PanelVideoCapture(app); return; }
  Settings& s = GetSettings();
  SectionTitle("Stream quality");
  static const char* qLabels[] = {"Auto", "Low", "Medium", "High", "Ultra", "Lossless"};
  static const int qValues[] = {5, 0, 1, 2, 3, 4};
  if (Segmented("quality", &s.quality, qLabels, qValues, 6)) {
    app.kvm.ApplyStreamQuality(s.quality, s.zeroDelay, s.vencSmart);
    s.Save();
  }
  SectionTitle("Response mode");
  static const char* rLabels[] = {"Fast response", "Smooth display"};
  static const int rValues[] = {1, 0};
  int zd = s.zeroDelay ? 1 : 0;
  if (Segmented("resp", &zd, rLabels, rValues, 2)) {
    s.zeroDelay = zd == 1;
    app.kvm.SetStreamerParam(std::string("zero_delay=") + (s.zeroDelay ? "true" : "false"));
    s.Save();
  }
  SectionTitle("Encoder mode");
  static const char* eLabels[] = {"Smart", "Normal"};
  static const int eValues[] = {1, 0};
  int vs = s.vencSmart ? 1 : 0;
  if (Segmented("venc", &vs, eLabels, eValues, 2)) {
    s.vencSmart = vs == 1;
    app.kvm.SetStreamerParam(std::string("venc_mode=") + (s.vencSmart ? "smart" : "normal"));
    s.Save();
  }
  if (ImGui::CollapsingHeader("Custom bitrate")) {
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##br", &app.customBitrate, 500, 20000, "H.264 bitrate %d kbps");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##gop", &app.customGop, 15, 120, "H.264 GOP %d");
    if (ImGui::Button("Apply", ImVec2(-1, 0))) {
      app.kvm.SetStreamerParam("h264_bitrate=" + std::to_string(app.customBitrate) +
                               "&h264_gop=" + std::to_string(app.customGop));
    }
  }
  if (app.status.h264Bitrate || app.status.h264Gop)
    ImGui::TextDisabled("Current: %d kbps, GOP %d", app.status.h264Bitrate, app.status.h264Gop);
  LayoutPicker();
  ImGui::Spacing();
  if (ImGui::Button("Reconnect video stream", ImVec2(-1, 0))) app.kvm.RequestKeyframe();
}

static void PanelFrameGen(App& app) {
  Settings& s = GetSettings();
  bool sdk = VfxInitLibrary(nullptr);
  if (!sdk) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::Warning);
    ImGui::TextWrapped("%s NVIDIA Video Effects SDK not found. Frame generation needs the SDK with the "
                       "nvvfxvideoframegeneration feature installed.", Icon(IC_WARN));
    ImGui::PopStyleColor();
  }
  ImGui::BeginDisabled(!sdk);
  if (Toggle("NVIDIA Video Frame Generation", &s.fgEnabled)) {
    app.pstats.starvations = 0;
    app.shownStep = -2;
    s.Save();
  }
  ImGui::EndDisabled();
  Hint("Generates new frames between the console's real ones on your RTX GPU. A 30 fps game "
       "is shown at your monitor's refresh rate.");

  SectionTitle("Output frame rate");
  double hz = app.pstats.refreshHz;
  char matchLbl[64];
  snprintf(matchLbl, sizeof(matchLbl), "Match display (%.0f Hz)", hz);
  const char* tLabels[] = {matchLbl, "2x game fps", "3x game fps", "4x game fps", "Custom"};
  int tgt = int(s.fgTarget);
  ImGui::SetNextItemWidth(-1);
  if (ImGui::Combo("##target", &tgt, tLabels, 5)) { s.fgTarget = FgTarget(tgt); s.Save(); }
  if (s.fgTarget == FgTarget::Custom) {
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##cfps", &s.fgCustomFps, 30, 360, "%d fps")) s.Save();
  }

  SectionTitle("Quality");
  static const char* qLabels[] = {"Performance", "Balanced", "Quality"};
  static const int qValues[] = {0, 1, 2};
  if (Segmented("fgq", &s.fgQuality, qLabels, qValues, 3)) s.Save();

  ImGui::Spacing();
  ImGui::Separator();
  const PipelineStats& st = app.stats;
  const PresentStats& ps = app.pstats;
  if (s.fgEnabled) {
    ImVec4 c = ps.fgState == "Active" ? col::Success : (ps.fgState.rfind("Could not", 0) == 0 ? col::Error : col::Warning);
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrapped("%s", ps.fgState.empty() ? "Waiting for video" : ps.fgState.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::Text("Game: %.0f fps    Output: %.0f fps", st.uniqueFps, ps.outputFps);
  if (s.fgEnabled) {
    ImGui::Text("Generation: %.1f ms    Added latency: ~%.0f ms", ps.fgMs, ps.addedLatencyMs);
    if (ps.starvations) ImGui::TextDisabled("Late frames: %d (buffer +%.0f ms)", ps.starvations, ps.extraBufferMs);
    if (ps.fgMs > 0.8 * 1000.0 / std::max(1.0, ps.refreshHz)) {
      ImGui::PushStyleColor(ImGuiCol_Text, col::Warning);
      ImGui::TextWrapped("%s Generation takes longer than one refresh at %.0f Hz. Use Balanced, or a 2x/3x target.",
                         Icon(IC_WARN), ps.refreshHz);
      ImGui::PopStyleColor();
    }
  }

  if (ImGui::CollapsingHeader("Advanced")) {
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##buf", &s.fgBufferMs, 0, 30, "Jitter buffer %d ms")) s.Save();
    if (ImGui::Checkbox("Grow the buffer automatically on late frames", &s.fgAdaptiveBuffer)) s.Save();
    if (ImGui::Checkbox("Detect repeated frames (30 fps games on a 60 Hz signal)", &s.dupDetection)) s.Save();
    ImGui::BeginDisabled(!s.dupDetection);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##dup", &s.dupThreshold, 0.1f, 4.0f, "Repeat threshold %.2f")) s.Save();
    ImGui::TextDisabled("Last frame difference: %.2f", st.lastDiff);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##gap", &s.fgMaxGapMs, 40, 250, "Don't interpolate gaps over %d ms")) s.Save();
    Hint("If menus or static scenes shimmer, raise the repeat threshold. If you see occasional stutter, "
         "raise the jitter buffer.");
  }
}

static void PanelSuperRes(App& app) {
  Settings& s = GetSettings();
  bool sdk = VfxInitLibrary(nullptr);
  ImGui::BeginDisabled(!sdk);
  if (Toggle("NVIDIA Video Super Resolution", &s.srEnabled)) s.Save();
  ImGui::EndDisabled();
  Hint(app.CaptureMode() ? "AI upscaling for the capture card's picture (RTX Video Super Resolution)."
                         : "AI upscaling and compression clean-up for the KVM's H.264 stream (RTX Video Super Resolution).");
  SectionTitle("Output resolution");
  static const char* oLabels[] = {"1440p", "4K"};
  static const int oValues[] = {0, 1};
  int tgt = s.srTarget == SrTarget::P1440 ? 0 : 1;
  if (Segmented("srt", &tgt, oLabels, oValues, 2)) { s.srTarget = tgt == 0 ? SrTarget::P1440 : SrTarget::P2160; s.Save(); }
  SectionTitle("Mode");
  int m = std::clamp(s.srMode, 0, kVsrModeCount - 1);
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##srmode", kVsrModes[m].name)) {
    for (int i = 0; i < kVsrModeCount; ++i)
      if (ImGui::Selectable(kVsrModes[i].name, i == m)) { s.srMode = i; s.Save(); }
    ImGui::EndCombo();
  }
  Hint(app.CaptureMode()
           ? "Uncompressed capture is clean, so the plain modes fit best; \"Streaming artifacts\" modes help with "
             "MJPEG. Upscaled frames also feed frame generation, which costs more at 4K."
           : "\"Streaming artifacts\" modes are tuned for compressed video like the KVM's. "
             "Upscaled frames also feed frame generation, which costs more at 4K.");
  ImGui::Separator();
  const PipelineStats& st = app.stats;
  if (s.srEnabled) {
    ImGui::PushStyleColor(ImGuiCol_Text, st.srState == "Active" ? col::Success : col::Warning);
    ImGui::TextUnformatted(st.srState.empty() ? "Waiting for video" : st.srState.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::Text("%dx%d  ->  %dx%d", st.decodedW, st.decodedH, st.outW, st.outH);
  if (s.srEnabled) ImGui::Text("Upscale time: %.1f ms", st.srMs);
}

static void PanelKeyboard(App& app) {
  Settings& s = GetSettings();
  if (Toggle("Send keyboard to the console", &s.keyboardEnabled)) s.Save();
  SectionTitle("Shortcuts");
  float w = (ImGui::GetContentRegionAvail().x - 8 * g_dpi) / 2;
  if (ImGui::Button("Xbox / Win", ImVec2(w, 0))) ComboKeys(app, {"MetaLeft"});
  ImGui::SameLine();
  if (ImGui::Button("Esc", ImVec2(w, 0))) ComboKeys(app, {"Escape"});
  if (ImGui::Button("Ctrl+Alt+Del", ImVec2(w, 0))) ComboKeys(app, {"ControlLeft", "AltLeft", "Delete"});
  ImGui::SameLine();
  if (ImGui::Button("Alt+Tab", ImVec2(w, 0))) ComboKeys(app, {"AltLeft", "Tab"});
  if (ImGui::Button("Alt+F4", ImVec2(w, 0))) ComboKeys(app, {"AltLeft", "F4"});
  ImGui::SameLine();
  if (ImGui::Button("Print Screen", ImVec2(w, 0))) ComboKeys(app, {"PrintScreen"});
  SectionTitle("Paste text");
  ImGui::InputTextMultiline("##paste", app.pasteBuf, sizeof(app.pasteBuf), ImVec2(-1, 80 * g_dpi));
  if (ImGui::Button("Type it on the console", ImVec2(-1, 0)) && app.pasteBuf[0]) {
    app.kvm.PasteText(app.pasteBuf);
    app.Toast("Typing text...");
  }
  if (app.status.capsLock) ImGui::TextDisabled("Caps Lock is on");
}

static void PanelMouse(App& app) {
  Settings& s = GetSettings();
  if (Toggle("Send mouse to the console", &s.mouseEnabled)) s.Save();
  bool hasRel = std::find(app.status.mouseOutputs.begin(), app.status.mouseOutputs.end(), "usb_rel") !=
                app.status.mouseOutputs.end();
  SectionTitle("Mouse mode");
  static const char* mLabels[] = {"Absolute", "Relative"};
  static const int mValues[] = {0, 1};
  int mode = app.status.mouseAbsolute ? 0 : 1;
  ImGui::BeginDisabled(!hasRel);
  if (Segmented("mm", &mode, mLabels, mValues, 2)) {
    app.kvm.SetMouseOutput(mode == 0 ? "usb" : "usb_rel");
    if (mode == 0) app.ReleaseMouseCapture();
  }
  ImGui::EndDisabled();
  Hint(app.status.mouseAbsolute ? "The pointer follows your cursor over the video."
                                : "Click the video to capture the mouse. Press Ctrl+Alt to release it.");
  if (ImGui::Checkbox("Show local cursor over the video", &s.showCursor)) s.Save();
  ImGui::SetNextItemWidth(-1);
  if (ImGui::SliderInt("##sense", &s.relativeSense, 1, 40, "Relative sensitivity %d")) s.Save();
  ImGui::SetNextItemWidth(-1);
  if (ImGui::SliderInt("##scroll", &s.scrollRate, 1, 20, "Scroll rate %d")) s.Save();
  if (ImGui::Checkbox("Reverse scrolling", &s.reverseScroll)) s.Save();
}

static void PanelPower(App& app) {
  const KvmStatus& st = app.status;
  StatusDot(st.atxPowerLed ? col::Success : col::TextDim);
  ImGui::Text("Power %s", st.atxPowerLed ? "on" : "off");
  ImGui::SameLine(0, 20 * g_dpi);
  StatusDot(st.atxHddLed ? col::Warning : col::TextDim);
  ImGui::TextUnformatted("Activity");
  static const char* pending = nullptr;
  if (ImGui::Button("Power", ImVec2(-1, 0))) pending = "power";
  if (ImGui::Button("Force off (long press)", ImVec2(-1, 0))) pending = "power_long";
  if (ImGui::Button("Reset", ImVec2(-1, 0))) pending = "reset";
  if (pending) ImGui::OpenPopup("Confirm power action");
  if (ImGui::BeginPopupModal("Confirm power action", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Send \"%s\" to the console?", pending ? pending : "");
    if (ImGui::Button("Confirm", ImVec2(120 * g_dpi, 0))) {
      if (pending) app.kvm.AtxClick(pending);
      pending = nullptr;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120 * g_dpi, 0))) { pending = nullptr; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
  }
}

static void PanelSettings(App& app) {
  Settings& s = GetSettings();
  SectionTitle("Display");
  if (ImGui::Checkbox("Show status bar", &s.showStatusBar)) s.Save();
  if (ImGui::Checkbox("Show performance overlay", &s.showStatsOverlay)) s.Save();
  if (ImGui::Checkbox("VSync (when frame generation is off)", &s.vsync)) s.Save();
  if (!s.vsync && !app.renderer.TearingSupported()) ImGui::TextDisabled("Tearing isn't supported on this display path");
  if (ImGui::Button(app.fullscreen ? "Exit full screen (F11)" : "Full screen (F11)", ImVec2(-1, 0))) app.ToggleFullscreen();
  SectionTitle(app.CaptureMode() ? "Capture card" : "Connection");
  if (app.CaptureMode()) {
    ImGui::TextUnformatted(s.captureName.c_str());
    if (ImGui::Checkbox("Start automatically on launch", &s.captureAutoStart)) s.Save();
    if (ImGui::Button("Stop capture (change device or mode)", ImVec2(-1, 0))) app.Logout();
  } else {
  ImGui::Text("%s  as %s", s.host.c_str(), s.user.c_str());
  if (ImGui::Checkbox("Connect automatically on launch", &s.autoConnect)) s.Save();
  if (s.rememberPassword && ImGui::Button("Forget saved password", ImVec2(-1, 0))) {
    s.rememberPassword = false;
    s.passwordProtected.clear();
    s.Save();
    app.Toast("Saved password removed");
  }
  if (!s.certPin.empty()) ImGui::TextDisabled("Certificate pinned: %.16s...", s.certPin.c_str());
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(col::Error.x, col::Error.y, col::Error.z, 0.25f));
  if (ImGui::Button("Log out", ImVec2(-1, 0))) app.Logout();
  ImGui::PopStyleColor();
  }
  SectionTitle("About");
  ImGui::TextDisabled("GPU: %s", Narrow(app.renderer.AdapterName()).c_str());
  ImGui::TextDisabled("Decoder: %s", app.stats.decoderName.empty() ? "-" : app.stats.decoderName.c_str());
  ImGui::TextDisabled("NVIDIA VFX SDK: %s", VfxSdkDir().empty() ? "not found" : VfxSdkDir().c_str());
}

// ------------------------------------------------------------------ toolbar, status bar

struct PanelDef { Panel id; unsigned icon; const char* tip; };

static void DrawToolbar(App& app) {
  Settings& s = GetSettings();
  ImGuiIO& io = ImGui::GetIO();
  float btn = 38 * g_dpi;
  if (s.toolbarCollapsed) {
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 0), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * g_dpi, 2 * g_dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8 * g_dpi);
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##tbc", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    if (IconButton(Icon(IC_DOWN), "Show toolbar", false, 26 * g_dpi)) { s.toolbarCollapsed = false; s.Save(); }
    ImGui::End();
    ImGui::PopStyleVar(2);
    return;
  }

  std::vector<PanelDef> defs = {
    {Panel::Video, IC_VIDEO, "Video"},
    {Panel::Audio, GetSettings().audioMuted ? IC_MUTE : IC_VOLUME, "Audio"},
    {Panel::FrameGen, IC_BOLT, "Frame generation"},
    {Panel::SuperRes, IC_ZOOM, "Super resolution"},
  };
  if (!app.CaptureMode()) {
    defs.push_back({Panel::Keyboard, IC_KEYBOARD, "Keyboard"});
    defs.push_back({Panel::Mouse, IC_MOUSE, "Mouse"});
    if (app.status.atxEnabled) defs.push_back({Panel::Power, IC_POWER, "Power"});
  }
  defs.push_back({Panel::Settings, IC_SETTINGS, "Settings"});

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 10 * g_dpi), ImGuiCond_Always, ImVec2(0.5f, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * g_dpi, 6 * g_dpi));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * g_dpi, 4 * g_dpi));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * g_dpi);
  ImGui::SetNextWindowBgAlpha(0.88f);
  ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
  ImVec2 tbPos = ImGui::GetWindowPos();
  for (auto& d : defs) {
    bool active = app.panel == d.id;
    // Frame gen button lights up while it's running, like an "on" indicator.
    bool on = (d.id == Panel::FrameGen && s.fgEnabled) || (d.id == Panel::SuperRes && s.srEnabled) ||
              (d.id == Panel::Audio && app.astatus.active && !s.audioMuted &&
               (app.CaptureMode() || app.status.audioConnected));
    if (on && !active) ImGui::PushStyleColor(ImGuiCol_Text, col::Success);
    if (IconButton(Icon(d.icon), d.tip, active, btn)) app.panel = active ? Panel::None : d.id;
    if (on && !active) ImGui::PopStyleColor();
    ImGui::SameLine();
  }
  ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
  ImGui::SameLine();
  if (IconButton(Icon(app.fullscreen ? IC_UNFULL : IC_FULL), app.fullscreen ? "Exit full screen (F11)" : "Full screen (F11)",
                 false, btn))
    app.ToggleFullscreen();
  ImGui::SameLine();
  if (IconButton(Icon(IC_UP), "Hide toolbar", false, btn)) { s.toolbarCollapsed = true; app.panel = Panel::None; s.Save(); }
  float tbH = ImGui::GetWindowHeight();
  ImGui::End();
  ImGui::PopStyleVar(3);

  if (app.panel == Panel::None) return;
  const char* title = "";
  for (auto& d : defs) if (d.id == app.panel) title = d.tip;
  if (app.panel == Panel::Power && !app.status.atxEnabled) { app.panel = Panel::None; return; }
  if (app.CaptureMode() && (app.panel == Panel::Keyboard || app.panel == Panel::Mouse || app.panel == Panel::Power)) {
    app.panel = Panel::None;
    return;
  }
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, tbPos.y + tbH + 8 * g_dpi), ImGuiCond_Always, ImVec2(0.5f, 0));
  ImGui::SetNextWindowSize(ImVec2(380 * g_dpi, 0));
  ImGui::SetNextWindowSizeConstraints(ImVec2(380 * g_dpi, 0), ImVec2(380 * g_dpi, io.DisplaySize.y - tbH - 60 * g_dpi));
  ImGui::Begin("##panel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PushFont(nullptr, 18.0f);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::GetFrameHeight());
  if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) app.panel = Panel::None;
  ImGui::Separator();
  switch (app.panel) {
    case Panel::Video: PanelVideo(app); break;
    case Panel::Audio: PanelAudio(app); break;
    case Panel::FrameGen: PanelFrameGen(app); break;
    case Panel::SuperRes: PanelSuperRes(app); break;
    case Panel::Keyboard: PanelKeyboard(app); break;
    case Panel::Mouse: PanelMouse(app); break;
    case Panel::Power: PanelPower(app); break;
    case Panel::Settings: PanelSettings(app); break;
    default: break;
  }
  ImGui::End();
}

static float DrawStatusBar(App& app) {
  if (!GetSettings().showStatusBar || app.fullscreen) return 0;
  ImGuiIO& io = ImGui::GetIO();
  float h = 30 * g_dpi;
  ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - h));
  ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * g_dpi, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col::Panel);
  ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::SetCursorPosY((h - ImGui::GetTextLineHeight()) * 0.5f);
  const KvmStatus& st = app.status;
  const PipelineStats& ps = app.stats;
  const Settings& s = GetSettings();
  if (app.CaptureMode()) {
    const CaptureStatus& cs = app.cstatus;
    bool err = !cs.error.empty();
    StatusDot(err ? col::Error : (cs.opened && cs.signal) ? col::Success : col::Warning);
    ImGui::Text("%s", err ? "Error" : !cs.opened ? "Opening" : cs.signal ? "Capturing" : "No signal");
    ImGui::SameLine(0, 18 * g_dpi);
    ImGui::TextDisabled("%s", s.captureName.c_str());
    if (cs.format.width) {
      ImGui::SameLine(0, 18 * g_dpi);
      ImGui::Text("%dx%d %s", cs.format.width, cs.format.height, cs.format.subtype.c_str());
    }
    ImGui::SameLine(0, 18 * g_dpi);
    ImGui::Text("Capture %.0f fps", ps.kvmFps);
  } else {
    bool ok = st.eventsConnected && st.mediaConnected;
    StatusDot(ok ? (st.sourceOnline ? col::Success : col::Warning) : col::Error);
    ImGui::Text("%s", ok ? (st.sourceOnline ? "Connected" : "No signal") : "Connecting");
    ImGui::SameLine(0, 18 * g_dpi);
    ImGui::TextDisabled("%s", s.host.c_str());
    if (st.width) {
      ImGui::SameLine(0, 18 * g_dpi);
      ImGui::Text("%dx%d", st.width, st.height);
    }
    ImGui::SameLine(0, 18 * g_dpi);
    ImGui::Text("KVM %.0f fps", ps.kvmFps);
  }
  ImGui::SameLine(0, 12 * g_dpi);
  ImGui::Text("Game %.0f fps", ps.uniqueFps);

  char right[256];
  int n = snprintf(right, sizeof(right), "Output %.0f fps", app.pstats.outputFps);
  if (s.fgEnabled) n += snprintf(right + n, sizeof(right) - n, "   %s FG +%.0f ms", Icon(IC_BOLT), app.pstats.addedLatencyMs);
  if (s.srEnabled && ps.srState == "Active") n += snprintf(right + n, sizeof(right) - n, "   VSR %dp", ps.outH);
  snprintf(right + n, sizeof(right) - n, "   %s   %s", ps.decoderName.empty() ? "-" : ps.decoderName.c_str(),
           Narrow(app.renderer.AdapterName()).c_str());
  float rw = ImGui::CalcTextSize(right).x;
  ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20 * g_dpi, io.DisplaySize.x - rw - 12 * g_dpi));
  ImGui::TextDisabled("%s", right);
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
  return h;
}

static void DrawStatsOverlay(App& app) {
  if (!GetSettings().showStatsOverlay) return;
  const PipelineStats& st = app.stats;
  const PresentStats& ps = app.pstats;
  ImGui::SetNextWindowPos(ImVec2(10 * g_dpi, 10 * g_dpi));
  ImGui::SetNextWindowBgAlpha(0.7f);
  ImGui::Begin("##stats", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::Text("%s Performance", Icon(IC_STATS));
  ImGui::Separator();
  ImGui::Text("%s %5.1f fps (%.1f ms)", app.CaptureMode() ? "Capture     " : "KVM stream  ", st.kvmFps,
              st.kvmInterval * 1000);
  ImGui::Text("Game frames  %5.1f fps (%.1f ms)", st.uniqueFps, st.uniqueInterval * 1000);
  ImGui::Text("Output       %5.1f fps @ %.0f Hz", ps.outputFps, ps.refreshHz);
  ImGui::Text("Decode       %5.2f ms  %s", st.decodeMs, st.decoderName.c_str());
  if (GetSettings().srEnabled) ImGui::Text("Upscale      %5.2f ms  %dx%d", st.srMs, st.outW, st.outH);
  if (GetSettings().fgEnabled) {
    ImGui::Text("Frame gen    %5.2f ms  x%d", ps.fgMs, ps.multiplier);
    ImGui::Text("Added delay  %5.1f ms", ps.addedLatencyMs);
    ImGui::Text("Late frames  %d", ps.starvations);
  }
  ImGui::Text("Frame diff   %5.2f", st.lastDiff);
  ImGui::End();
}

static void DrawToast(App& app, float bottom) {
  if (app.toast.empty()) return;
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - bottom - 20 * g_dpi), ImGuiCond_Always,
                          ImVec2(0.5f, 1.0f));
  ImGui::SetNextWindowBgAlpha(0.9f);
  ImGui::Begin("##toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::TextUnformatted(app.toast.c_str());
  ImGui::End();
}

void DrawUi(App& app) {
  if (app.screen != Screen::Main) {
    DrawLogin(app);
    return;
  }
  // The status bar must be laid out first so the video knows how much room it has.
  float bar = (GetSettings().showStatusBar && !app.fullscreen) ? 30 * g_dpi : 0;
  DrawVideo(app, bar);
  DrawCenterMessage(app, bar);
  DrawStatsOverlay(app);
  DrawToolbar(app);
  DrawStatusBar(app);
  DrawToast(app, bar);
}
