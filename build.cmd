@echo off
rem Builds VidBox (Release) into build\VidBox.exe
rem Works from any checkout location: Visual Studio, vcpkg and CUDA are located at run time.
setlocal
cd /d "%~dp0"

rem --- Visual Studio (any edition: Community/Professional/Enterprise/BuildTools) ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe not found. Install Visual Studio 2022 or the VS 2022 Build Tools with the C++ workload.
  exit /b 1
)
set "VSROOT="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo ERROR: No Visual Studio install with the C++ x64 tools was found.
  exit /b 1
)
rem vcvars/vcpkg call vswhere by bare name, so put it on PATH.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

rem --- CUDA: prefer the machine-wide value (a stale per-shell value can point at an old toolkit) ---
for /f "delims=" %%i in ('powershell -NoProfile -Command "[Environment]::GetEnvironmentVariable('CUDA_PATH','Machine')"') do if not "%%i"=="" set "CUDA_PATH=%%i"
if not exist "%CUDA_PATH%\bin\nvcc.exe" (
  echo ERROR: CUDA Toolkit not found. Install it, or set CUDA_PATH to its folder.
  exit /b 1
)
set "PATH=%CUDA_PATH%\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%ProgramFiles%\CMake\bin;%PATH%"

rem --- vcpkg: VCPKG_ROOT if set (vcvars sets it for the VS-bundled copy), else the VS-bundled one ---
set "VCPKG_EXE="
if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\vcpkg.exe" set "VCPKG_EXE=%VCPKG_ROOT%\vcpkg.exe"
if not defined VCPKG_EXE if exist "%VSROOT%\VC\vcpkg\vcpkg.exe" set "VCPKG_EXE=%VSROOT%\VC\vcpkg\vcpkg.exe"
if not defined VCPKG_EXE (
  echo ERROR: vcpkg not found. Add the "vcpkg package manager" component in the VS Installer, or set VCPKG_ROOT.
  exit /b 1
)

rem libdatachannel (WebRTC audio from the GLKVM). The first run builds OpenSSL and takes a while.
if not exist vcpkg_installed\x64-windows-static-md\share\libdatachannel (
  "%VCPKG_EXE%" install --triplet x64-windows-static-md --x-install-root="%~dp0vcpkg_installed" || exit /b 1
)

rem A build folder copied from another checkout/machine keeps that location's absolute paths
rem in CMakeCache.txt. Throw it away if it was configured for a different source folder.
if exist build\CMakeCache.txt (
  powershell -NoProfile -Command "$m = Select-String -Path 'build\CMakeCache.txt' -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$'; if (-not $m) { exit 1 }; $a = [IO.Path]::GetFullPath($m.Matches[0].Groups[1].Value).TrimEnd('\'); $b = [IO.Path]::GetFullPath('.').TrimEnd('\'); if ($a -ine $b) { exit 1 }"
  if errorlevel 1 (
    echo Build folder was configured for a different location; reconfiguring from scratch.
    rmdir /s /q build
  )
)

if not exist build\build.ninja (
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_COMPILER="%CUDA_PATH%\bin\nvcc.exe" || exit /b 1
)
cmake --build build || exit /b 1
echo.
echo Built: %~dp0build\VidBox.exe
