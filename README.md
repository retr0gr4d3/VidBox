# VidBox

Native Windows client for the GL.iNet GLKVM and for HDMI capture cards (formerly KVMBox). It replaces the browser and renders on the NVIDIA GPU, adding
NVIDIA Video Frame Generation (VFG) and Video Super Resolution (VSR) for console play.

## Getting the source

Dear ImGui (`third_party\imgui`) is a git submodule, so clone with:

```
git clone --recursive <repo-url>
```

In an existing clone, run `git submodule update --init`. `build.cmd` also does this automatically if the folder
is empty.

## Requirements

### Hardware

An NVIDIA RTX 40 (Ada) or RTX 50 (Blackwell) GPU with a current driver. These are the GPUs that Video Frame
Generation supports, and the only CUDA architectures the build targets (`sm_89`, `sm_120`).

### Tools to install

| Tool | Notes |
|---|---|
| Visual Studio 2022, any edition (Community, Professional, Enterprise or Build Tools) | Workload **Desktop development with C++**, plus the **vcpkg package manager** component. `build.cmd` finds the install with `vswhere`. VS also supplies CMake and Ninja. |
| CUDA Toolkit | 13.x is tested. 12.8 is the minimum (it is the first version with `sm_120`). The installer sets `CUDA_PATH`, which `build.cmd` uses. |

To use a standalone vcpkg instead of the one bundled with VS, set `VCPKG_ROOT` to its folder.

### Files you must download yourself

These are excluded by `.gitignore` (license terms or GitHub's file-size limit), so a fresh clone does not build
until you add them.

#### 1. NVIDIA Video Effects SDK → `sdk\vfx\VideoFX`

1. Download the **Windows x64** NVIDIA Video Effects (Maxine VFX) SDK **1.3.0** from NVIDIA
   ([docs and download](https://docs.nvidia.com/maxine/vfx/index.html)). It is under NVIDIA's license and cannot be
   redistributed in this repository.
2. Extract it so that the folder looks like this:

   ```
   sdk\vfx\VideoFX\
     bin\            NVVideoEffects.dll, NVCVImage.dll, ...
     features\       install_feature.ps1
     nvvfx\          include\, src\ (the proxy sources compiled into VidBox)
   ```

3. Install the two features VidBox uses. You need an NGC API key (free NVIDIA NGC account):

   ```powershell
   cd sdk\vfx\VideoFX\features
   $Env:NGC_CLI_API_KEY = "<your-ngc-api-key>"
   Set-ExecutionPolicy Bypass -Scope Process
   .\install_feature.ps1 -features nvvfxvideoframegeneration,nvvfxvideosuperres -version 1.3.0.0
   ```

   This adds `features\nvvfxvideoframegeneration`, `features\nvvfxvideosuperres` (their headers are needed to
   compile) and the TensorRT models in `bin\models` (needed at run time).

   The models are built for the GPU architecture the script detects. On a machine with a different GPU generation,
   run the script again there, or pass `-gpu 89` (RTX 40) or `-gpu 120` (RTX 50).

#### 2. FFmpeg → `third_party\ffmpeg`

Download an FFmpeg 8.1 **LGPL shared** Windows x64 build: `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1` from
[BtbN's FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases). Extract it so that `bin`, `include` and
`lib` sit directly in that folder:

```
third_party\ffmpeg\
  bin\       avcodec-62.dll, avutil-60.dll, ...
  include\   libavcodec\, libavutil\, ...
  lib\       avcodec.lib, avutil.lib, ...
```

The DLL major versions (`avcodec-62`, `avutil-60`) must match the 8.1 headers. A different FFmpeg major version
will not work as a drop-in replacement.

#### Created automatically (nothing to download)

- `vcpkg_installed\`: libdatachannel and OpenSSL, built by vcpkg from `vcpkg.json` on the first `build.cmd` run.
- `build\`: CMake/Ninja output. If it was copied from another folder or machine, `build.cmd` detects that and
  reconfigures from scratch.
- `%APPDATA%\VidBox\settings.json` and `*.log`: created by the app at run time.

## Build

From the repository root (in PowerShell, type `.\build.cmd`):

```
build.cmd
```

Output: `build\VidBox.exe` (FFmpeg DLLs are copied next to it). The first build also runs vcpkg to build
libdatachannel and OpenSSL (about 7 minutes).

This dev build loads the Video Effects runtime from `sdk\vfx\VideoFX` in your checkout, so it only runs on the
machine that built it.

### Distributable build

```
build.cmd --release
```

This builds into `build-release\` (the normal `build\` is left alone) and packages a standalone app as
`dist\VidBox-<version>-win64.zip` (about 1.3 GB, mostly NVIDIA's CUDA/TensorRT runtime). The version comes from
`vcpkg.json`. Users extract the zip and run `VidBox.exe`. Nothing else needs to be installed except an NVIDIA
driver on an RTX 40/50 GPU.

The zip contains:

| Path | Contents |
|---|---|
| `VidBox.exe` | Built without the SDK fallback path, so it loads the Video Effects runtime only from `vfx\` |
| `*.dll` | The FFmpeg DLLs the exe uses (found with `dumpbin`) and the Visual C++ runtime (app-local, so no VC++ Redistributable is needed) |
| `vfx\bin`, `vfx\features` | The Video Effects runtime and only the two features VidBox uses, with NVIDIA's license files |
| `licenses\` | FFmpeg (LGPL), Dear ImGui, nlohmann/json and the vcpkg packages that are statically linked (libdatachannel, OpenSSL, ...) |

Packaging is done by `scripts\package.ps1`. To check a package, extract it somewhere else and run
`VidBox.exe --selftest`. The log should show `vfx: SDK ... at <that folder>\vfx` and `0 failure(s)`.

Before publishing a zip, check that NVIDIA's license terms (`vfx\license`) allow you to redistribute the Video
Effects runtime with your app. FFmpeg is LGPL and is shipped as unmodified shared DLLs, which keeps it replaceable.

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
