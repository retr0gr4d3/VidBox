#include "capture.h"
#include "common.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------ helpers

static void EnsureMf() {
  static std::once_flag once;
  std::call_once(once, [] {
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) LogF("capture: MFStartup failed 0x%08X", unsigned(hr));
  });
}

struct ComScope {
  bool uninit;
  ComScope() { uninit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
  ~ComScope() { if (uninit) CoUninitialize(); }
};

static std::string SubtypeName(const GUID& g) {
  if (g == MFVideoFormat_RGB32) return "RGB32";
  if (g == MFVideoFormat_ARGB32) return "ARGB32";
  if (g == MFVideoFormat_RGB24) return "RGB24";
  if (g == MFVideoFormat_RGB555) return "RGB555";
  if (g == MFVideoFormat_RGB565) return "RGB565";
  char s[5];
  for (int i = 0; i < 4; ++i) {
    char c = char((g.Data1 >> (8 * i)) & 0xFF);
    s[i] = (c >= 32 && c < 127) ? c : '?';
  }
  s[4] = 0;
  return s;
}

static bool ToRawFormat(const GUID& g, RawFormat* f) {
  if (g == MFVideoFormat_NV12) { *f = RawFormat::NV12; return true; }
  if (g == MFVideoFormat_YUY2) { *f = RawFormat::YUY2; return true; }
  if (g == MFVideoFormat_UYVY) { *f = RawFormat::UYVY; return true; }
  if (g == MFVideoFormat_I420 || g == MFVideoFormat_IYUV) { *f = RawFormat::I420; return true; }
  if (g == MFVideoFormat_RGB32 || g == MFVideoFormat_ARGB32) { *f = RawFormat::RGB32; return true; }
  if (g == MFVideoFormat_MJPG) { *f = RawFormat::MJPG; return true; }
  return false;
}

static bool SubtypeIsNative(const std::string& s) {
  return s == "NV12" || s == "YUY2" || s == "UYVY" || s == "I420" || s == "IYUV" || s == "RGB32" || s == "ARGB32" ||
         s == "MJPG";
}

bool CaptureFormat::Native() const { return SubtypeIsNative(subtype); }

std::string CaptureFormat::Label() const {
  char b[96];
  double f = Fps();
  if (std::fabs(f - std::round(f)) < 0.01) snprintf(b, sizeof(b), "%dx%d  %.0f fps  %s", width, height, f, subtype.c_str());
  else snprintf(b, sizeof(b), "%dx%d  %.2f fps  %s", width, height, f, subtype.c_str());
  std::string s = b;
  if (!Native()) s += " (converted by Windows)";
  return s;
}

static std::string HrText(HRESULT hr) {
  char b[32];
  snprintf(b, sizeof(b), "0x%08X", unsigned(hr));
  return b;
}

static HRESULT CreateSource(const std::string& link, IMFMediaSource** out) {
  ComPtr<IMFAttributes> a;
  HRESULT hr = MFCreateAttributes(&a, 2);
  if (FAILED(hr)) return hr;
  a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  std::wstring wl = Widen(link);
  a->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, wl.c_str());
  return MFCreateDeviceSource(a.Get(), out);
}

static std::string OpenErrorText(HRESULT hr) {
  if (hr == E_ACCESSDENIED)
    return "Windows blocked camera access. Turn on Settings > Privacy & security > Camera > "
           "\"Let desktop apps access your camera\".";
  if (hr == MF_E_HW_MFT_FAILED_START_STREAMING || hr == HRESULT(0xC00D3EA3L) ||
      hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION))
    return "The capture card is in use by another app (OBS, Camera, a browser). Close it and try again.";
  if (hr == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED || hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) ||
      hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
    return "The capture card was disconnected.";
  return "Could not open the capture card (" + HrText(hr) + ").";
}

// Native formats, de-duplicated; `indices` receives the native media type index of each.
static void EnumFormats(IMFSourceReader* r, std::vector<CaptureFormat>& out, std::vector<DWORD>* indices) {
  for (DWORD i = 0;; ++i) {
    ComPtr<IMFMediaType> t;
    if (FAILED(r->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &t))) break;
    GUID sub = {};
    if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub))) continue;
    CaptureFormat f;
    f.subtype = SubtypeName(sub);
    UINT32 w = 0, h = 0, n = 0, d = 1;
    MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &n, &d);
    if (!w || !h) continue;
    f.width = int(w);
    f.height = int(h);
    f.fpsNum = n;
    f.fpsDen = d ? d : 1;
    bool dup = false;
    for (auto& o : out) if (o.Same(f)) { dup = true; break; }
    if (dup) continue;
    out.push_back(f);
    if (indices) indices->push_back(i);
  }
}

// ------------------------------------------------------------------ async callback

class CaptureReaderCallback : public IMFSourceReaderCallback {
 public:
  explicit CaptureReaderCallback(CaptureClient* owner) : owner_(owner) {}
  void Detach() { std::lock_guard<std::mutex> l(mu_); owner_ = nullptr; }

  STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
      *ppv = static_cast<IMFSourceReaderCallback*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG r = --refs_;
    if (!r) delete this;
    return r;
  }
  STDMETHODIMP OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG ts, IMFSample* sample) override {
    std::lock_guard<std::mutex> l(mu_);
    if (owner_) owner_->OnSample(hr, flags, ts, sample);
    return S_OK;
  }
  STDMETHODIMP OnFlush(DWORD) override {
    std::lock_guard<std::mutex> l(mu_);
    if (owner_) owner_->OnFlushed();
    return S_OK;
  }
  STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

 private:
  std::atomic<ULONG> refs_{1};
  std::mutex mu_;
  CaptureClient* owner_;
};

// ------------------------------------------------------------------ enumeration

std::vector<CaptureDevice> CaptureClient::ListDevices() {
  EnsureMf();
  ComScope com;
  std::vector<CaptureDevice> out;
  ComPtr<IMFAttributes> a;
  if (FAILED(MFCreateAttributes(&a, 1))) return out;
  a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  IMFActivate** devs = nullptr;
  UINT32 count = 0;
  if (FAILED(MFEnumDeviceSources(a.Get(), &devs, &count))) return out;
  for (UINT32 i = 0; i < count; ++i) {
    WCHAR* name = nullptr;
    WCHAR* link = nullptr;
    UINT32 len = 0;
    devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
    devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len);
    if (link) out.push_back({name ? Narrow(name) : "Video device", Narrow(link)});
    CoTaskMemFree(name);
    CoTaskMemFree(link);
    devs[i]->Release();
  }
  CoTaskMemFree(devs);
  return out;
}

std::vector<CaptureFormat> CaptureClient::ListFormats(const std::string& link, std::string* err) {
  EnsureMf();
  ComScope com;
  std::vector<CaptureFormat> out;
  ComPtr<IMFMediaSource> src;
  HRESULT hr = CreateSource(link, &src);
  if (FAILED(hr)) { if (err) *err = OpenErrorText(hr); return out; }
  ComPtr<IMFSourceReader> r;
  hr = MFCreateSourceReaderFromMediaSource(src.Get(), nullptr, &r);
  if (SUCCEEDED(hr)) EnumFormats(r.Get(), out, nullptr);
  else if (err) *err = OpenErrorText(hr);
  r.Reset();
  src->Shutdown();
  return out;
}

int CaptureClient::PickDefault(const std::vector<CaptureFormat>& formats) {
  auto rank = [](const std::string& s) {
    if (s == "NV12") return 4;
    if (s == "YUY2" || s == "UYVY" || s == "I420" || s == "IYUV") return 3;
    if (s == "RGB32" || s == "ARGB32") return 2;
    if (s == "MJPG") return 1;  // CPU decode
    return 0;
  };
  int best = -1;
  auto better = [&](const CaptureFormat& a, const CaptureFormat& b) {
    bool a60 = a.Fps() >= 49, b60 = b.Fps() >= 49;
    if (a60 != b60) return a60;
    long long aa = std::min<long long>(1LL * a.width * a.height, 3840LL * 2160);
    long long ba = std::min<long long>(1LL * b.width * b.height, 3840LL * 2160);
    if (aa != ba) return aa > ba;
    if (std::fabs(a.Fps() - b.Fps()) > 0.5) return a.Fps() > b.Fps();
    return rank(a.subtype) > rank(b.subtype);
  };
  for (int i = 0; i < int(formats.size()); ++i)
    if (best < 0 || better(formats[i], formats[best])) best = i;
  return best;
}

// ------------------------------------------------------------------ streaming

CaptureClient::~CaptureClient() {
  Stop();
  if (flushEvent_) CloseHandle(flushEvent_);
}

void CaptureClient::Start(const CaptureDevice& dev, const CaptureFormat& want, FrameCallback onFrame,
                          ResetCallback onReset) {
  Stop();
  EnsureMf();
  if (!flushEvent_) flushEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  onFrame_ = std::move(onFrame);
  onReset_ = std::move(onReset);
  {
    std::lock_guard<std::mutex> l(mu_);
    stopping_ = false;
    tsInit_ = false;
    lastTs_ = -1;
  }
  {
    std::lock_guard<std::mutex> l(statMu_);
    status_ = CaptureStatus();
    status_.running = true;
    status_.deviceName = dev.name;
    rate_ = RateCounter();
    lastFrameTime_ = 0;
  }
  running_ = true;
  openThread_ = std::thread(&CaptureClient::OpenThread, this, dev, want);
}

void CaptureClient::Fail(const std::string& msg) {
  LogF("capture: %s", msg.c_str());
  std::lock_guard<std::mutex> l(statMu_);
  status_.error = msg;
  status_.opened = false;
}

void CaptureClient::OpenThread(CaptureDevice dev, CaptureFormat want) {
  ComScope com;
  LogF("capture: opening \"%s\"", dev.name.c_str());
  ComPtr<IMFMediaSource> src;
  HRESULT hr = CreateSource(dev.link, &src);
  if (FAILED(hr)) { Fail(OpenErrorText(hr)); return; }

  auto* cb = new CaptureReaderCallback(this);
  ComPtr<IMFAttributes> a;
  MFCreateAttributes(&a, 4);
  a->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, cb);
  a->SetUINT32(MF_LOW_LATENCY, TRUE);
  a->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);  // for formats we can't convert ourselves
  a->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);
  ComPtr<IMFSourceReader> r;
  hr = MFCreateSourceReaderFromMediaSource(src.Get(), a.Get(), &r);
  if (FAILED(hr)) { cb->Release(); src->Shutdown(); Fail(OpenErrorText(hr)); return; }

  std::vector<CaptureFormat> formats;
  std::vector<DWORD> indices;
  EnumFormats(r.Get(), formats, &indices);
  if (formats.empty()) { cb->Release(); src->Shutdown(); Fail("The capture card reports no video formats."); return; }
  int pick = -1;
  for (int i = 0; i < int(formats.size()); ++i) if (formats[i].Same(want)) { pick = i; break; }
  if (pick < 0) {
    pick = PickDefault(formats);
    if (want.width) LogF("capture: saved format %s not offered, using %s", want.Label().c_str(), formats[pick].Label().c_str());
  }
  const DWORD stream = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  r->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  r->SetStreamSelection(stream, TRUE);
  ComPtr<IMFMediaType> native;
  hr = r->GetNativeMediaType(stream, indices[pick], &native);
  if (SUCCEEDED(hr)) hr = r->SetCurrentMediaType(stream, nullptr, native.Get());
  if (SUCCEEDED(hr) && !formats[pick].Native()) {
    // Let Windows decode/convert (H.264, HEVC, RGB24, P010, ...) to NV12.
    ComPtr<IMFMediaType> nv12;
    MFCreateMediaType(&nv12);
    nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    hr = r->SetCurrentMediaType(stream, nullptr, nv12.Get());
    if (FAILED(hr)) {
      cb->Release();
      src->Shutdown();
      Fail("Windows can't convert " + formats[pick].subtype + " from this capture card. Pick another format.");
      return;
    }
  }
  if (FAILED(hr)) { cb->Release(); src->Shutdown(); Fail(OpenErrorText(hr)); return; }

  std::lock_guard<std::mutex> l(mu_);
  reader_ = r.Detach();
  source_ = src.Detach();
  callback_ = cb;
  if (!ReadCurrentType()) {
    Fail("Unsupported output format from the capture card.");
    return;  // Stop() releases everything
  }
  {
    std::lock_guard<std::mutex> sl(statMu_);
    status_.formats = formats;
    status_.format = formats[pick];
    status_.opened = true;
  }
  LogF("capture: streaming %s", formats[pick].Label().c_str());
  RequestNext();
}

bool CaptureClient::ReadCurrentType() {
  ComPtr<IMFMediaType> t;
  if (FAILED(reader_->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &t))) return false;
  GUID sub = {};
  t->GetGUID(MF_MT_SUBTYPE, &sub);
  if (!ToRawFormat(sub, &rawFormat_)) {
    LogF("capture: output subtype %s not handled", SubtypeName(sub).c_str());
    return false;
  }
  UINT32 w = 0, h = 0;
  MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
  width_ = int(w);
  height_ = int(h);
  UINT32 st = 0;
  if (SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &st))) stride_ = int(INT32(st));
  else {
    LONG s = 0;
    stride_ = SUCCEEDED(MFGetStrideForBitmapInfoHeader(sub.Data1, w, &s)) ? int(s) : 0;
  }
  UINT32 range = 0, matrix = 0;
  fullRange_ = SUCCEEDED(t->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range))
                   ? (range == MFNominalRange_0_255 ? 1 : range == MFNominalRange_16_235 ? 0 : -1)
                   : -1;
  bt709_ = SUCCEEDED(t->GetUINT32(MF_MT_YUV_MATRIX, &matrix))
               ? (matrix == MFVideoTransferMatrix_BT709 ? 1 : matrix == MFVideoTransferMatrix_BT601 ? 0 : -1)
               : -1;
  {
    std::lock_guard<std::mutex> sl(statMu_);
    status_.outputSubtype = SubtypeName(sub);
  }
  LogF("capture: output %s %dx%d stride %d range %d matrix %d", SubtypeName(sub).c_str(), width_, height_, stride_,
       fullRange_, bt709_);
  return width_ > 0 && height_ > 0;
}

void CaptureClient::RequestNext() {
  HRESULT hr = reader_->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr, nullptr, nullptr);
  if (FAILED(hr)) Fail("Capture stopped (" + HrText(hr) + ").");
}

void CaptureClient::OnFlushed() {
  if (flushEvent_) SetEvent(flushEvent_);
}

void CaptureClient::OnSample(long hrIn, unsigned long flags, long long ts, IMFSample* sample) {
  HRESULT hr = hrIn;
  std::lock_guard<std::mutex> l(mu_);
  if (stopping_ || !reader_) return;
  if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
    Fail(FAILED(hr) ? OpenErrorText(hr) : "The capture card stopped streaming.");
    return;
  }
  if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { Fail("The capture card ended the stream."); return; }
  if (flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) {
    if (!ReadCurrentType()) { Fail("The capture card switched to an unsupported format."); return; }
    tsInit_ = false;
    if (onReset_) onReset_();
  }
  if (sample) {
    double now = NowSec();
    // Map the device timestamp onto our clock: it has far less jitter than the callback time.
    double arrival = now;
    if (ts > 0 && ts > lastTs_) {
      double t = ts * 1e-7;
      double cand = now - t;
      if (!tsInit_ || std::fabs(cand - tsOffset_) > 0.1) { tsOffset_ = cand; tsInit_ = true; }
      else if (cand < tsOffset_) tsOffset_ = cand;      // follow the fastest delivery
      else tsOffset_ += 0.002 * (cand - tsOffset_);      // and slow drift
      arrival = t + tsOffset_;
    }
    lastTs_ = ts;

    ComPtr<IMFMediaBuffer> buf;
    if (SUCCEEDED(sample->GetBufferByIndex(0, &buf)) || SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
      RawFrame f;
      f.format = rawFormat_;
      f.width = width_;
      f.height = height_;
      f.fullRange = fullRange_;
      f.bt709 = bt709_;
      bool planar = rawFormat_ == RawFormat::NV12 || rawFormat_ == RawFormat::I420;
      ComPtr<IMF2DBuffer> b2;
      bool locked2d = false;
      BYTE* data = nullptr;
      DWORD len = 0;
      if (!planar && rawFormat_ != RawFormat::MJPG && SUCCEEDED(buf.As(&b2))) {
        // Single plane: Lock2D gives the real pitch and orientation.
        LONG pitch = 0;
        if (SUCCEEDED(b2->Lock2D(&data, &pitch))) {
          locked2d = true;
          f.pitch = int(pitch);
          f.data = pitch < 0 ? data + LONG_PTR(pitch) * (height_ - 1) : data;
          f.size = size_t(pitch < 0 ? -pitch : pitch) * height_;
        }
      }
      if (!locked2d) {
        DWORD maxLen = 0;
        if (SUCCEEDED(buf->Lock(&data, &maxLen, &len))) {
          f.data = data;
          f.size = len;
          if (rawFormat_ == RawFormat::MJPG) f.pitch = 0;
          else if (b2 || SUCCEEDED(buf.As(&b2))) {
            // A 2D buffer locked with Lock() is in contiguous format: minimum stride, top-down.
            int bpp = rawFormat_ == RawFormat::RGB32 ? 4 : (rawFormat_ == RawFormat::NV12 || rawFormat_ == RawFormat::I420) ? 1 : 2;
            f.pitch = width_ * bpp;
          } else {
            int bpp = rawFormat_ == RawFormat::RGB32 ? 4 : planar ? 1 : 2;
            int s = stride_ ? stride_ : width_ * bpp;
            if (rawFormat_ != RawFormat::RGB32 && s < 0) s = -s;
            f.pitch = s;
          }
        } else {
          data = nullptr;
        }
      }
      if (f.data && onFrame_) onFrame_(f, arrival);
      if (locked2d) b2->Unlock2D();
      else if (data) buf->Unlock();
      {
        std::lock_guard<std::mutex> sl(statMu_);
        rate_.Tick(now);
        lastFrameTime_ = now;
      }
    }
  }
  RequestNext();
}

void CaptureClient::Stop() {
  if (openThread_.joinable()) openThread_.join();
  running_ = false;
  IMFSourceReader* r;
  IMFMediaSource* s;
  CaptureReaderCallback* cb;
  {
    std::lock_guard<std::mutex> l(mu_);
    stopping_ = true;
    r = reader_;
    s = source_;
    cb = callback_;
    reader_ = nullptr;
    source_ = nullptr;
    callback_ = nullptr;
  }
  if (r && flushEvent_) {
    ResetEvent(flushEvent_);
    if (SUCCEEDED(r->Flush(DWORD(MF_SOURCE_READER_ALL_STREAMS)))) WaitForSingleObject(flushEvent_, 2000);
  }
  if (cb) cb->Detach();
  if (r) r->Release();
  if (s) { s->Shutdown(); s->Release(); }
  if (cb) cb->Release();
  std::lock_guard<std::mutex> sl(statMu_);
  if (status_.running) LogF("capture: stopped");
  status_.running = false;
  status_.opened = false;
  status_.signal = false;
}

CaptureStatus CaptureClient::Status() {
  double now = NowSec();
  std::lock_guard<std::mutex> l(statMu_);
  CaptureStatus st = status_;
  st.fps = rate_.Rate(now);
  st.signal = st.opened && lastFrameTime_ > 0 && now - lastFrameTime_ < 1.0;
  return st;
}
