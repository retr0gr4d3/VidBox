#pragma once
#include <string>
#include <cstdint>
#include <atomic>

// Monotonic time in seconds (QueryPerformanceCounter).
double NowSec();

// Log to %APPDATA%\VidBox\vidbox.log and the debugger.
void LogF(const char* fmt, ...);

std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);

// %APPDATA%\VidBox (created on demand).
std::wstring AppDataDir();
// Directory containing VidBox.exe.
std::wstring ExeDir();

// DPAPI helpers (current-user scope) for remembering the password.
std::string ProtectString(const std::string& plain);    // returns base64
std::string UnprotectString(const std::string& b64);

std::string UrlEncode(const std::string& s);

// Exponential moving average helper.
struct Ema {
  double value = 0.0;
  bool init = false;
  void Add(double v, double alpha) {
    if (!init) { value = v; init = true; }
    else value += alpha * (v - value);
  }
};

// Counts events per second over a sliding one-second window.
class RateCounter {
 public:
  void Tick(double now);
  double Rate(double now);
 private:
  double windowStart_ = 0.0;
  int count_ = 0;
  double rate_ = 0.0;
};
