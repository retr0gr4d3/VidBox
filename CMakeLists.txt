cmake_minimum_required(VERSION 3.24)
project(VidBox LANGUAGES C CXX CUDA)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
# Ada (RTX 40) and Blackwell (RTX 50) - the GPUs NVIDIA VFG supports.
set(CMAKE_CUDA_ARCHITECTURES 89 120)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

find_package(CUDAToolkit REQUIRED)

# WebRTC for the GLKVM's audio (libdatachannel + OpenSSL, installed by vcpkg from vcpkg.json; see build.cmd).
list(APPEND CMAKE_PREFIX_PATH ${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows-static-md)
find_package(LibDataChannel CONFIG REQUIRED)

set(TP  ${CMAKE_SOURCE_DIR}/third_party)
set(VFX ${CMAKE_SOURCE_DIR}/sdk/vfx/VideoFX)
set(FFMPEG ${TP}/ffmpeg)

set(IMGUI_SRC
  ${TP}/imgui/imgui.cpp
  ${TP}/imgui/imgui_draw.cpp
  ${TP}/imgui/imgui_tables.cpp
  ${TP}/imgui/imgui_widgets.cpp
  ${TP}/imgui/backends/imgui_impl_win32.cpp
  ${TP}/imgui/backends/imgui_impl_dx11.cpp)

add_executable(VidBox WIN32
  src/main.cpp
  src/common.cpp
  src/settings.cpp
  src/net.cpp
  src/kvm.cpp
  src/capture.cpp
  src/audio.cpp
  src/kvmaudio.cpp
  src/decoder.cpp
  src/kernels.cu
  src/vfx.cpp
  src/pipeline.cpp
  src/renderer.cpp
  src/input.cpp
  src/ui.cpp
  ${VFX}/nvvfx/src/nvVideoEffectsProxy.cpp
  ${VFX}/nvvfx/src/nvCVImageProxy.cpp
  ${IMGUI_SRC})

# Feature headers (installed by install_feature.ps1) live under features/<name>/include.
file(GLOB VFX_FEATURE_INCLUDES LIST_DIRECTORIES true ${VFX}/features/*/include)

target_include_directories(VidBox PRIVATE
  src
  ${TP}/imgui ${TP}/imgui/backends
  ${TP}/json
  ${FFMPEG}/include
  ${VFX}/nvvfx/include
  ${VFX_FEATURE_INCLUDES})

target_compile_definitions(VidBox PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
  VIDBOX_VFX_SDK_DIR="${VFX}")

if(MSVC)
  target_compile_options(VidBox PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3 /utf-8 /MP>)
endif()

target_link_libraries(VidBox PRIVATE
  d3d11 dxgi d3dcompiler dwmapi winhttp crypt32 imm32 winmm
  mfplat mf mfreadwrite mfuuid ole32
  CUDA::cudart_static
  LibDataChannel::LibDataChannel
  ${FFMPEG}/lib/avcodec.lib
  ${FFMPEG}/lib/avutil.lib)

# Ship FFmpeg runtime DLLs next to the exe.
file(GLOB FFMPEG_DLLS ${FFMPEG}/bin/*.dll)
add_custom_command(TARGET VidBox POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:VidBox>)
