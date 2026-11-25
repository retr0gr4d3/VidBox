#include "settings.h"
#include "common.h"

#include <windows.h>
#include <fstream>
#include <sstream>
#include "json.hpp"

using nlohmann::json;

Settings& GetSettings() {
  static Settings s;
  return s;
}

template <typename T>
static void Get(const json& j, const char* key, T& out) {
  auto it = j.find(key);
  if (it == j.end()) return;
  try { out = it->get<T>(); } catch (...) {}
}

template <typename E>
static void GetEnum(const json& j, const char* key, E& out) {
  int v = int(out);
  Get(j, key, v);
  out = E(v);
}

bool Settings::Load() {
  std::ifstream f(AppDataDir() + L"\\settings.json");
  if (!f) return false;
  json j;
  try { f >> j; } catch (...) { return false; }
  GetEnum(j, "mode", mode);
  Get(j, "captureLink", captureLink);
  Get(j, "captureName", captureName);
  Get(j, "captureSubtype", captureSubtype);
  Get(j, "captureW", captureW);
  Get(j, "captureH", captureH);
  Get(j, "captureFpsNum", captureFpsNum);
  Get(j, "captureFpsDen", captureFpsDen);
  Get(j, "captureRange", captureRange);
  Get(j, "captureAutoStart", captureAutoStart);
  Get(j, "audioEnabled", audioEnabled);
  Get(j, "captureAudioId", captureAudioId);
  Get(j, "audioVolume", audioVolume);
  Get(j, "audioMuted", audioMuted);
  Get(j, "audioSync", audioSync);
  Get(j, "audioOffsetMs", audioOffsetMs);
  Get(j, "host", host);
  Get(j, "user", user);
  Get(j, "rememberPassword", rememberPassword);
  Get(j, "password", passwordProtected);
  Get(j, "certPin", certPin);
  Get(j, "autoConnect", autoConnect);
  Get(j, "quality", quality);
  Get(j, "zeroDelay", zeroDelay);
  Get(j, "vencSmart", vencSmart);
  GetEnum(j, "layout", layout);
  Get(j, "vsync", vsync);
  Get(j, "showStatusBar", showStatusBar);
  Get(j, "showStatsOverlay", showStatsOverlay);
  Get(j, "toolbarCollapsed", toolbarCollapsed);
  Get(j, "fgEnabled", fgEnabled);
  GetEnum(j, "fgTarget", fgTarget);
  Get(j, "fgCustomFps", fgCustomFps);
  Get(j, "fgQuality", fgQuality);
  Get(j, "fgBufferMs", fgBufferMs);
  Get(j, "fgAdaptiveBuffer", fgAdaptiveBuffer);
  Get(j, "dupThreshold", dupThreshold);
  Get(j, "dupDetection", dupDetection);
  Get(j, "fgMaxGapMs", fgMaxGapMs);
  Get(j, "srEnabled", srEnabled);
  GetEnum(j, "srTarget", srTarget);
  Get(j, "srMode", srMode);
  Get(j, "keyboardEnabled", keyboardEnabled);
  Get(j, "mouseEnabled", mouseEnabled);
  Get(j, "scrollRate", scrollRate);
  Get(j, "reverseScroll", reverseScroll);
  Get(j, "relativeSense", relativeSense);
  Get(j, "showCursor", showCursor);
  return true;
}

bool Settings::Save() const {
  json j = {
    {"mode", int(mode)},
    {"captureLink", captureLink}, {"captureName", captureName}, {"captureSubtype", captureSubtype},
    {"captureW", captureW}, {"captureH", captureH}, {"captureFpsNum", captureFpsNum},
    {"captureFpsDen", captureFpsDen}, {"captureRange", captureRange}, {"captureAutoStart", captureAutoStart},
    {"audioEnabled", audioEnabled}, {"captureAudioId", captureAudioId},
    {"audioVolume", audioVolume}, {"audioMuted", audioMuted},
    {"audioSync", audioSync}, {"audioOffsetMs", audioOffsetMs},
    {"host", host}, {"user", user}, {"rememberPassword", rememberPassword},
    {"password", rememberPassword ? passwordProtected : std::string()},
    {"certPin", certPin}, {"autoConnect", autoConnect},
    {"quality", quality}, {"zeroDelay", zeroDelay}, {"vencSmart", vencSmart},
    {"layout", int(layout)}, {"vsync", vsync}, {"showStatusBar", showStatusBar},
    {"showStatsOverlay", showStatsOverlay}, {"toolbarCollapsed", toolbarCollapsed},
    {"fgEnabled", fgEnabled}, {"fgTarget", int(fgTarget)}, {"fgCustomFps", fgCustomFps},
    {"fgQuality", fgQuality}, {"fgBufferMs", fgBufferMs}, {"fgAdaptiveBuffer", fgAdaptiveBuffer},
    {"dupThreshold", dupThreshold}, {"dupDetection", dupDetection}, {"fgMaxGapMs", fgMaxGapMs},
    {"srEnabled", srEnabled}, {"srTarget", int(srTarget)}, {"srMode", srMode},
    {"keyboardEnabled", keyboardEnabled}, {"mouseEnabled", mouseEnabled},
    {"scrollRate", scrollRate}, {"reverseScroll", reverseScroll},
    {"relativeSense", relativeSense}, {"showCursor", showCursor},
  };
  std::wstring path = AppDataDir() + L"\\settings.json";
  std::wstring tmp = path + L".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << j.dump(2);
  }
  return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
