#include "kvmaudio.h"
#include "common.h"
#include "net.h"

#include <rtc/rtc.hpp>

#include <wincrypt.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "json.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

using nlohmann::json;

// ------------------------------------------------------------------ helpers

static std::string B64Encode(const std::string& s) {
  DWORD n = 0;
  DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
  if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(s.data()), DWORD(s.size()), flags, nullptr, &n)) return {};
  std::string out(n, '\0');
  if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(s.data()), DWORD(s.size()), flags, out.data(), &n)) return {};
  out.resize(n);
  while (!out.empty() && (out.back() == '\0' || out.back() == '\r' || out.back() == '\n')) out.pop_back();
  return out;
}

static std::string B64Decode(const std::string& s) {
  DWORD n = 0;
  if (!CryptStringToBinaryA(s.c_str(), DWORD(s.size()), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr)) return {};
  std::string out(n, '\0');
  if (!CryptStringToBinaryA(s.c_str(), DWORD(s.size()), CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(out.data()), &n,
                            nullptr, nullptr))
    return {};
  out.resize(n);
  return out;
}

static void InitRtcLogging() {
  static std::once_flag once;
  std::call_once(once, [] {
    rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel, std::string msg) { LogF("webrtc: %s", msg.c_str()); });
  });
}

// Opus -> interleaved float with FFmpeg's built-in decoder.
class OpusDecoder {
 public:
  ~OpusDecoder() {
    if (ctx_) avcodec_free_context(&ctx_);
    if (frame_) av_frame_free(&frame_);
    if (pkt_) av_packet_free(&pkt_);
  }
  bool Open() {
    const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!c) return false;
    ctx_ = avcodec_alloc_context3(c);
    ctx_->sample_rate = 48000;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&ctx_->ch_layout, &stereo);
    if (avcodec_open2(ctx_, c, nullptr) < 0) { avcodec_free_context(&ctx_); return false; }
    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    return true;
  }
  template <typename F>
  void Decode(const uint8_t* data, size_t len, F&& out) {
    pkt_->data = const_cast<uint8_t*>(data);
    pkt_->size = int(len);
    if (avcodec_send_packet(ctx_, pkt_) < 0) { pkt_->data = nullptr; pkt_->size = 0; return; }
    pkt_->data = nullptr;
    pkt_->size = 0;
    while (avcodec_receive_frame(ctx_, frame_) >= 0) {
      int ch = frame_->ch_layout.nb_channels, n = frame_->nb_samples;
      buf_.resize(size_t(n) * ch);
      if (frame_->format == AV_SAMPLE_FMT_FLTP) {
        for (int c = 0; c < ch; ++c) {
          const float* p = reinterpret_cast<const float*>(frame_->extended_data[c]);
          for (int i = 0; i < n; ++i) buf_[size_t(i) * ch + c] = p[i];
        }
      } else if (frame_->format == AV_SAMPLE_FMT_FLT) {
        memcpy(buf_.data(), frame_->data[0], buf_.size() * sizeof(float));
      } else if (frame_->format == AV_SAMPLE_FMT_S16) {
        const int16_t* p = reinterpret_cast<const int16_t*>(frame_->data[0]);
        for (size_t i = 0; i < buf_.size(); ++i) buf_[i] = p[i] / 32768.0f;
      } else {
        av_frame_unref(frame_);
        continue;
      }
      out(buf_.data(), n, frame_->sample_rate, ch);
      av_frame_unref(frame_);
    }
  }

 private:
  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  std::vector<float> buf_;
};

// ------------------------------------------------------------------ session

bool KvmAudioReceiver::Run(std::shared_ptr<WebSocket> ws, PcmCallback onPcm, StateCallback onState, std::string* err) {
  InitRtcLogging();
  auto decoder = std::make_shared<OpusDecoder>();
  if (!decoder->Open()) { if (err) *err = "FFmpeg has no Opus decoder"; return false; }

  rtc::Configuration config;
  config.disableAutoNegotiation = true;
  auto pc = std::make_shared<rtc::PeerConnection>(config);

  // Callbacks run on libdatachannel threads and may outlive this call, so they only touch shared state.
  struct Shared {
    std::atomic<bool> gotAudio{false};
    std::atomic<long long> packets{0};
  };
  auto st = std::make_shared<Shared>();

  pc->onLocalDescription([ws](rtc::Description d) {
    json desc = {{"type", d.typeString()}, {"sdp", std::string(d)}};
    ws->SendText(json({{"type", d.typeString()}, {"sdp", B64Encode(desc.dump())}}).dump());
    LogF("kvm audio: sent %s", d.typeString().c_str());
  });
  pc->onLocalCandidate([ws](rtc::Candidate c) {
    std::string cand = c.candidate();
    if (cand.rfind("a=", 0) == 0) cand = cand.substr(2);
    ws->SendText(json({{"type", "candidate"}, {"candidate", cand}, {"sdpMid", c.mid()}, {"sdpMLineIndex", 0}}).dump());
  });
  pc->onGatheringStateChange([ws](rtc::PeerConnection::GatheringState s) {
    if (s == rtc::PeerConnection::GatheringState::Complete) ws->SendText(R"({"type":"client-ice-complete"})");
  });
  pc->onStateChange([ws, onState](rtc::PeerConnection::State s) {
    const char* names[] = {"new", "connecting", "connected", "disconnected", "failed", "closed"};
    int i = int(s);
    LogF("kvm audio: peer %s", i >= 0 && i < 6 ? names[i] : "?");
    if (onState) onState(i >= 0 && i < 6 ? names[i] : "?");
    if (s == rtc::PeerConnection::State::Failed || s == rtc::PeerConnection::State::Disconnected ||
        s == rtc::PeerConnection::State::Closed) {
      ws->Close();  // ends Run(); the caller reconnects
    }
  });

  rtc::Description::Audio media("0", rtc::Description::Direction::RecvOnly);
  media.addOpusCodec(111);
  auto track = pc->addTrack(media);
  track->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());  // receiver reports back to the KVM
  track->onMessage(
      [decoder, onPcm, st](rtc::binary msg) {
        if (msg.size() < 12) return;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(msg.data());
        if ((b[0] >> 6) != 2) return;
        if (b[1] >= 200 && b[1] <= 206) return;  // RTCP
        size_t cc = b[0] & 0x0F;
        size_t off = 12 + cc * 4;
        if (b[0] & 0x10) {  // header extension
          if (msg.size() < off + 4) return;
          size_t words = (size_t(b[off + 2]) << 8) | b[off + 3];
          off += 4 + words * 4;
        }
        size_t end = msg.size();
        if (b[0] & 0x20) {  // padding
          uint8_t padLen = b[end - 1];
          if (padLen > end - off) return;
          end -= padLen;
        }
        if (end <= off) return;
        st->packets++;
        decoder->Decode(b + off, end - off, [&](const float* s, int frames, int rate, int ch) {
          if (!st->gotAudio.exchange(true)) LogF("kvm audio: first audio, %d Hz %d ch", rate, ch);
          onPcm(s, frames, rate, ch);
        });
      },
      nullptr);

  ws->SendText(R"({"type":"config","video":false,"speaker":true,"mic":false,"hid":false,"offerer":true})");
  pc->setLocalDescription(rtc::Description::Type::Offer);

  std::vector<uint8_t> buf;
  bool answered = false;
  std::vector<rtc::Candidate> pending;
  for (;;) {
    auto m = ws->Receive(buf);
    if (m != WebSocket::Msg::Text) {
      if (m == WebSocket::Msg::Binary) continue;
      break;
    }
    json j = json::parse(buf.begin(), buf.end(), nullptr, false);
    if (!j.is_object()) continue;
    std::string type = j.value("type", "");
    try {
      if (type == "answer" || type == "offer") {
        json d = json::parse(B64Decode(j.value("sdp", "")), nullptr, false);
        if (!d.is_object()) { LogF("kvm audio: bad %s", type.c_str()); continue; }
        if (type == "offer") {
          // Not expected (we are the offerer), but follow the web UI: answer it.
          pc->setRemoteDescription(rtc::Description(d.value("sdp", ""), "offer"));
          pc->setLocalDescription(rtc::Description::Type::Answer);
        } else {
          pc->setRemoteDescription(rtc::Description(d.value("sdp", ""), "answer"));
        }
        answered = true;
        LogF("kvm audio: got %s", type.c_str());
        for (auto& c : pending) pc->addRemoteCandidate(c);
        pending.clear();
      } else if (type == "candidate") {
        std::string c = j.value("candidate", "");
        if (c.empty()) continue;
        std::string mid = j.contains("sdpMid") && j["sdpMid"].is_string() ? j["sdpMid"].get<std::string>() : "0";
        rtc::Candidate cand(c, mid);
        if (answered) pc->addRemoteCandidate(cand);
        else pending.push_back(cand);
      } else if (type == "error") {
        LogF("kvm audio: server error %s", j.dump().c_str());
        if (err) *err = j.value("message", j.value("error", std::string("The KVM refused the audio session")));
        break;
      } else if (type != "server-ice-complete") {
        LogF("kvm audio: message %s", j.dump().substr(0, 200).c_str());
      }
    } catch (const std::exception& e) {
      LogF("kvm audio: %s handling %s", e.what(), type.c_str());
      if (err) *err = std::string("WebRTC error: ") + e.what();
      break;
    }
  }
  LogF("kvm audio: session ended (%lld packets)", st->packets.load());
  track->onMessage(nullptr, nullptr);
  pc->onStateChange(nullptr);
  pc->close();
  bool gotAudio = st->gotAudio;
  if (!gotAudio && err && err->empty())
    *err = answered ? "Connected, but the KVM sent no audio" : "The KVM did not answer the audio request";
  return gotAudio;
}
