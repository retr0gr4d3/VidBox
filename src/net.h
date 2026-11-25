#pragma once
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

struct HttpResponse {
  int status = 0;
  std::string body;
  std::string setCookie;  // raw Set-Cookie header(s)
  std::string error;     // transport error (empty on success)
  bool certMismatch = false;
};

// HTTPS client for a single KVM host. The KVM uses a self-signed certificate, so we
// accept it on first use and pin its SHA-256 fingerprint afterwards (like SSH).
class HttpClient {
 public:
  HttpClient();
  ~HttpClient();
  bool SetHost(const std::string& hostAndPort);
  void SetPinnedCert(const std::string& sha256hex);
  std::string SeenCert();

  HttpResponse Request(const wchar_t* method, const std::string& pathAndQuery,
                       const std::string& body = {}, const std::string& contentType = {},
                       const std::string& token = {});

  // Returns a WinHTTP websocket handle (or nullptr).
  HINTERNET OpenWebSocket(const std::string& pathAndQuery, std::string* err, bool* certMismatch);

 private:
  HINTERNET OpenRequest(const wchar_t* method, const std::string& path);
  bool VerifyCert(HINTERNET req, std::string* err);

  HINTERNET session_ = nullptr;
  HINTERNET connect_ = nullptr;
  std::mutex mu_;
  std::string pin_;
  std::string seen_;
};

class WebSocket {
 public:
  enum class Msg { Binary, Text, Closed, Error };
  explicit WebSocket(HINTERNET h) : ws_(h) {}
  ~WebSocket();
  bool SendBinary(const void* data, size_t len);
  bool SendText(const std::string& s);
  // Blocks until a complete message is received.
  Msg Receive(std::vector<uint8_t>& out);
  void Close();
  bool IsOpen() const { return ws_ != nullptr && !closed_; }

 private:
  HINTERNET ws_ = nullptr;
  std::mutex sendMu_;
  std::atomic<bool> closed_{false};
};
