# VidBox

Native Windows client for the GL.iNet GLKVM and for HDMI capture cards (formerly KVMBox). It replaces the browser and renders on the NVIDIA GPU, adding
NVIDIA Video Frame Generation (VFG) and Video Super Resolution (VSR) for console play.

## Build

```
build.cmd
```

Output: `build\VidBox.exe` (FFmpeg DLLs are copied next to it). The first build also runs vcpkg to build
libdatachannel and OpenSSL (`vcpkg.json`, about 7 minutes).

Requirements: VS 2022 Build Tools (C++, including its bundled vcpkg), CMake, CUDA Toolkit 13.x, and the **Windows x64** NVIDIA Video Effects
SDK in `sdk\vfx\VideoFX` with the `nvvfxvideoframegeneration` and `nvvfxvideosuperres` features installed.

Not in the repository (see `.gitignore`); download them yourself:

- `sdk\vfx\VideoFX`: the NVIDIA Video Effects SDK, from NVIDIA under their license.
- `third_party\ffmpeg`: an FFmpeg 8.1 **LGPL shared** Windows x64 build (`ffmpeg-n8.1-latest-win64-lgpl-shared-8.1`
  from BtbN's FFmpeg-Builds), extracted so that `bin`, `include` and `lib` sit directly in that folder.

## Check the GPU features without the KVM

```
build\VidBox.exe --selftest
```

Results are written to `%APPDATA%\VidBox\vidbox.log` (frame generation and super resolution timings at 1080p).

## Two modes

Pick the mode at the top of the start screen:

- **KVMBox**: the GL.iNet GLKVM over the network (video, audio, keyboard, mouse, power).
- **CCBox**: a local HDMI capture card (UVC / Media Foundation device), with its audio played on the default
  Windows output (Audio panel: input, volume, mute, sync with frame generation). There is no keyboard, mouse or
  power control. Frame generation, super resolution, repeated-frame detection, layouts and the stats overlay work the same as in
  KVMBox mode. NV12, YUY2, UYVY, I420 and RGB32 are converted on the GPU. MJPEG is decoded on the CPU. Windows
  converts any other format to NV12. Choose the format and colour range in the Video panel.

Check the capture card without the UI (results go to the log):

```
build\VidBox.exe --capture-test
```

## How it works

| Stage | Where |
|---|---|
| Login, stream settings, ATX, paste: GLKVM REST API (`/api/...`) | `src/kvm.cpp` |
| Video: "Direct H.264" websocket `/api/media/ws` | `src/kvm.cpp` |
| Keyboard/mouse: binary HID messages on `/api/ws` | `src/kvm.cpp`, `src/input.cpp` |
| Capture card (CCBox): Media Foundation async source reader | `src/capture.cpp` |
| Capture card audio: WASAPI input -> default output, delayed to match frame generation | `src/audio.cpp` |
| GLKVM audio: audio-only WebRTC session on `/pion/ws` (libdatachannel), Opus decoded by FFmpeg | `src/kvmaudio.cpp`, `src/kvm.cpp` |
| H.264 decode on NVDEC, NV12 to RGBA in CUDA | `src/decoder.cpp`, `src/kernels.cu` |
| Repeated-frame detection, cadence timestamps, VSR | `src/pipeline.cpp` |
| VFG timestep interpolation paced to the monitor's refresh | `src/main.cpp` (`UpdateVideo`) |
| D3D11 on the RTX GPU, CUDA/D3D interop | `src/renderer.cpp` |
| UI (Dear ImGui, laid out like the GLKVM web UI) | `src/ui.cpp` |

A 30 fps game reaches the KVM as a 60 Hz signal with every frame shown twice. VidBox compares each decoded
frame with the previous distinct one on the GPU and drops the repeats. Each real frame gets a timestamp locked
to the capture cadence. At every refresh, the renderer shows content from one game frame (plus a small jitter
buffer) in the past and asks VFG for the exact in-between moment. The added latency is about one game frame
(~33 ms at 30 fps) plus the buffer. Turn frame generation off for the lowest latency.

The KVM's self-signed TLS certificate is pinned on first login. The password is sent only after the certificate
matches. If you choose to remember the password, it is stored encrypted with Windows DPAPI under
`%APPDATA%\VidBox\settings.json`.

## Not done yet

- Nothing major. GLKVM audio comes from a separate WebRTC session that runs alongside the Direct H.264 video (the
  web UI itself hides the speaker in Direct mode). Check it with `build\VidBox.exe --kvm-audio-test`, which needs
  a remembered password.
