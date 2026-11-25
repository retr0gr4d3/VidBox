#include "common.h"

#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

double NowSec() {
  static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return double(c.QuadPart) / double(freq.QuadPart);
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring AppDataDir() {
  static std::wstring dir = [] {
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) {
      std::wstring base = p;
      CoTaskMemFree(p);
      d = base + L"\\VidBox";
      CreateDirectoryW(d.c_str(), nullptr);
      // The app used to be called KVMBox: carry its settings (pinned certificate, saved password) over once.
      // Copied rather than moved, so it works even while an old KVMBox.exe is still running.
      std::wstring oldSettings = base + L"\\KVMBox\\settings.json";
      std::wstring newSettings = d + L"\\settings.json";
      if (GetFileAttributesW(newSettings.c_str()) == INVALID_FILE_ATTRIBUTES &&
          GetFileAttributesW(oldSettings.c_str()) != INVALID_FILE_ATTRIBUTES)
        CopyFileW(oldSettings.c_str(), newSettings.c_str(), TRUE);
    } else {
      d = L".\\VidBox";
    }
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
  }();
  return dir;
}

std::wstring ExeDir() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring p(buf, n);
  size_t slash = p.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

void LogF(const char* fmt, ...) {
  static std::mutex mu;
  static FILE* f = [] {
    FILE* fp = _wfopen((AppDataDir() + L"\\vidbox.log").c_str(), L"w");
    return fp;
  }();
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  char line[2200];
  snprintf(line, sizeof(line), "[%10.3f] %s\n", NowSec(), msg);
  std::lock_guard<std::mutex> lk(mu);
  OutputDebugStringA(line);
  if (f) { fputs(line, f); fflush(f); }
}

static std::string B64Encode(const BYTE* data, DWORD len) {
  DWORD n = 0;
  CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
  std::string s(n, '\0');
  CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s.data(), &n);
  s.resize(n);
  return s;
}

static std::vector<BYTE> B64Decode(const std::string& s) {
  DWORD n = 0;
  if (!CryptStringToBinaryA(s.c_str(), DWORD(s.size()), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr)) return {};
  std::vector<BYTE> v(n);
  CryptStringToBinaryA(s.c_str(), DWORD(s.size()), CRYPT_STRING_BASE64, v.data(), &n, nullptr, nullptr);
  v.resize(n);
  return v;
}

std::string ProtectString(const std::string& plain) {
  DATA_BLOB in{DWORD(plain.size()), (BYTE*)plain.data()}, out{};
  if (!CryptProtectData(&in, L"VidBox", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
  std::string r = B64Encode(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return r;
}

std::string UnprotectString(const std::string& b64) {
  if (b64.empty()) return {};
  std::vector<BYTE> raw = B64Decode(b64);
  if (raw.empty()) return {};
  DATA_BLOB in{DWORD(raw.size()), raw.data()}, out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
  std::string r((char*)out.pbData, out.cbData);
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return r;
}

std::string UrlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string r;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += char(c);
    else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
  }
  return r;
}

void RateCounter::Tick(double now) {
  if (windowStart_ == 0.0) windowStart_ = now;
  ++count_;
  Rate(now);
}

double RateCounter::Rate(double now) {
  if (windowStart_ == 0.0) return 0.0;
  double el = now - windowStart_;
  if (el >= 1.0) {
    rate_ = count_ / el;
    count_ = 0;
    windowStart_ = now;
  } else if (el > 2.0) {
    rate_ = 0.0;
  }
  return rate_;
}
