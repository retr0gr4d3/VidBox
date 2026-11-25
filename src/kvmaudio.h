#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>

class WebSocket;

// Audio-only WebRTC session with the GLKVM's pion server (/pion/ws), the same one its web UI opens for the
// speaker: {"type":"config","speaker":true,...}, then a client offer with one recvonly Opus track. Signaling
// messages carry base64(JSON(RTCSessionDescription)) and trickled ICE candidates.
class KvmAudioReceiver {
 public:
  // Decoded audio: interleaved float, called on a libdatachannel thread.
  using PcmCallback = std::function<void(const float* samples, int frames, int rate, int channels)>;
  using StateCallback = std::function<void(const std::string& state)>;

  // Runs one session on an open websocket; blocks until the websocket closes or the peer connection fails.
  // Returns false (with `err`) if the session never got audio flowing.
  static bool Run(std::shared_ptr<WebSocket> ws, PcmCallback onPcm, StateCallback onState, std::string* err);
};
