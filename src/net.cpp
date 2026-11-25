#include "net.h"
#include "common.h"

#include <wincrypt.h>

#pragma comment(lib, "winhttp.lib")

static std::string LastErr(const char* what) {
  DWORD e = GetLastError();
  char buf[128];
  snprintf(buf, sizeof(buf), "%s failed (WinHTTP error %lu)", what, e);
  return buf;
}

HttpClient::HttpClient() {
  session_ = WinHttpOpen(L"VidBox/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                         WINHTTP_NO_PROXY_BYPASS, 0);
  if (session_) {
    WinHttpSetTimeouts(session_, 5000, 5000, 10000, 15000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
  }
}

HttpClient::~HttpClient() {
  if (connect_) WinHttpCloseHandle(connect_);
  if (session_) WinHttpCloseHandle(session_);
}

bool HttpClient::SetHost(const std::string& hostAndPort) {
  std::lock_guard<std::mutex> lk(mu_);
  if (connect_) { WinHttpCloseHandle(connect_); connect_ = nullptr; }
  std::string host = hostAndPort;
  INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
  // Strip scheme and path if the user pasted a URL.
  if (auto p = host.find("://"); p != std::string::npos) host = host.substr(p + 3);
  if (auto p = host.find('/'); p != std::string::npos) host = host.substr(0, p);
  if (auto p = host.rfind(':'); p != std::string::npos && host.find(']') == std::string::npos) {
    port = INTERNET_PORT(atoi(host.c_str() + p + 1));
    host = host.substr(0, p);
  }
  connect_ = WinHttpConnect(session_, Widen(host).c_str(), port, 0);
  return connect_ != nullptr;
}

void HttpClient::SetPinnedCert(const std::string& sha256hex) {
  std::lock_guard<std::mutex> lk(mu_);
  pin_ = sha256hex;
}

std::string HttpClient::SeenCert() {
  std::lock_guard<std::mutex> lk(mu_);
  return seen_;
}

HINTERNET HttpClient::OpenRequest(const wchar_t* method, const std::string& path) {
  HINTERNET req = WinHttpOpenRequest(connect_, method, Widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!req) return nullptr;
  // Self-signed device certificate: the chain can't be validated, so we pin instead (VerifyCert).
  DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
  WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
  return req;
}

bool HttpClient::VerifyCert(HINTERNET req, std::string* err) {
  PCCERT_CONTEXT cert = nullptr;
  DWORD sz = sizeof(cert);
  if (!WinHttpQueryOption(req, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &sz) || !cert) {
    if (err) *err = "Could not read the KVM's TLS certificate";
    return false;
  }
  BYTE hash[32];
  DWORD hs = sizeof(hash);
  BOOL ok = CertGetCertificateContextProperty(cert, CERT_SHA256_HASH_PROP_ID, hash, &hs);
  CertFreeCertificateContext(cert);
  if (!ok) {
    if (err) *err = "Could not hash the KVM's TLS certificate";
    return false;
  }
  char hex[65];
  for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", hash[i]);
  std::lock_guard<std::mutex> lk(mu_);
  seen_ = hex;
  if (!pin_.empty() && pin_ != seen_) {
    if (err) *err = "The KVM's TLS certificate changed. If you reset or updated the KVM, clear the saved certificate in Settings; otherwise someone may be intercepting the connection.";
    return false;
  }
  return true;
}

HttpResponse HttpClient::Request(const wchar_t* method, const std::string& path, const std::string& body,
                                 const std::string& contentType, const std::string& token) {
  HttpResponse r;
  HINTERNET req = OpenRequest(method, path);
  if (!req) { r.error = LastErr("WinHttpOpenRequest"); return r; }

  std::wstring headers;
  if (!contentType.empty()) headers += L"Content-Type: " + Widen(contentType) + L"\r\n";
  if (!token.empty()) {
    headers += L"token: " + Widen(token) + L"\r\n";
    headers += L"Cookie: auth_token=" + Widen(token) + L"\r\n";
  }
  // Send headers first; only write the body (which may contain the password) after the cert check.
  if (!WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), DWORD(-1),
                          WINHTTP_NO_REQUEST_DATA, 0, DWORD(body.size()), 0)) {
    r.error = LastErr("Connect");
    WinHttpCloseHandle(req);
    return r;
  }
  if (!VerifyCert(req, &r.error)) {
    r.certMismatch = true;
    WinHttpCloseHandle(req);
    return r;
  }
  if (!body.empty()) {
    DWORD written = 0;
    if (!WinHttpWriteData(req, body.data(), DWORD(body.size()), &written)) {
      r.error = LastErr("WinHttpWriteData");
      WinHttpCloseHandle(req);
      return r;
    }
  }
  if (!WinHttpReceiveResponse(req, nullptr)) {
    r.error = LastErr("WinHttpReceiveResponse");
    WinHttpCloseHandle(req);
    return r;
  }
  DWORD status = 0, ssz = sizeof(status);
  WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &ssz, WINHTTP_NO_HEADER_INDEX);
  r.status = int(status);
  {
    DWORD csz = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &csz,
                        WINHTTP_NO_HEADER_INDEX);
    if (csz > 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
      std::wstring ck(csz / sizeof(wchar_t), L'\0');
      if (WinHttpQueryHeaders(req, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX, ck.data(), &csz,
                              WINHTTP_NO_HEADER_INDEX)) {
        ck.resize(csz / sizeof(wchar_t));
        r.setCookie = Narrow(ck);
      }
    }
  }
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
    size_t old = r.body.size();
    r.body.resize(old + avail);
    DWORD rd = 0;
    if (!WinHttpReadData(req, r.body.data() + old, avail, &rd)) break;
    r.body.resize(old + rd);
  }
  WinHttpCloseHandle(req);
  return r;
}

HINTERNET HttpClient::OpenWebSocket(const std::string& path, std::string* err, bool* certMismatch) {
  if (certMismatch) *certMismatch = false;
  HINTERNET req = OpenRequest(L"GET", path);
  if (!req) { if (err) *err = LastErr("WinHttpOpenRequest"); return nullptr; }
  if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
    if (err) *err = LastErr("WebSocket upgrade option");
    WinHttpCloseHandle(req);
    return nullptr;
  }
  if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
    if (err) *err = LastErr("Connect");
    WinHttpCloseHandle(req);
    return nullptr;
  }
  if (!VerifyCert(req, err)) {
    if (certMismatch) *certMismatch = true;
    WinHttpCloseHandle(req);
    return nullptr;
  }
  if (!WinHttpReceiveResponse(req, nullptr)) {
    if (err) *err = LastErr("WebSocket handshake");
    WinHttpCloseHandle(req);
    return nullptr;
  }
  DWORD status = 0, ssz = sizeof(status);
  WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &ssz, WINHTTP_NO_HEADER_INDEX);
  if (status != 101) {
    if (err) { char b[64]; snprintf(b, sizeof(b), "WebSocket rejected (HTTP %lu)", status); *err = b; }
    WinHttpCloseHandle(req);
    return nullptr;
  }
  HINTERNET ws = WinHttpWebSocketCompleteUpgrade(req, 0);
  WinHttpCloseHandle(req);
  if (!ws && err) *err = LastErr("WinHttpWebSocketCompleteUpgrade");
  return ws;
}

WebSocket::~WebSocket() { Close(); }

void WebSocket::Close() {
  bool expected = false;
  if (!closed_.compare_exchange_strong(expected, true)) return;
  if (ws_) {
    std::lock_guard<std::mutex> lk(sendMu_);
    // Closing the handle aborts a blocked Receive() on another thread.
    WinHttpWebSocketShutdown(ws_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    WinHttpCloseHandle(ws_);
  }
}

bool WebSocket::SendBinary(const void* data, size_t len) {
  std::lock_guard<std::mutex> lk(sendMu_);
  if (closed_) return false;
  return WinHttpWebSocketSend(ws_, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, (PVOID)data, DWORD(len)) ==
         NO_ERROR;
}

bool WebSocket::SendText(const std::string& s) {
  std::lock_guard<std::mutex> lk(sendMu_);
  if (closed_) return false;
  return WinHttpWebSocketSend(ws_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)s.data(), DWORD(s.size())) ==
         NO_ERROR;
}

WebSocket::Msg WebSocket::Receive(std::vector<uint8_t>& out) {
  out.clear();
  if (closed_) return Msg::Closed;
  for (;;) {
    size_t old = out.size();
    out.resize(old + 65536);
    DWORD rd = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
    DWORD e = WinHttpWebSocketReceive(ws_, out.data() + old, 65536, &rd, &type);
    if (e != NO_ERROR) { out.clear(); return closed_ ? Msg::Closed : Msg::Error; }
    out.resize(old + rd);
    switch (type) {
      case WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE: return Msg::Binary;
      case WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE: return Msg::Text;
      case WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE:
      case WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE: continue;
      case WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE: default: return Msg::Closed;
    }
  }
}
