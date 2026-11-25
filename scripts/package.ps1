# Stages a self-contained copy of VidBox and zips it. Called by "build.cmd --release" after the
# Visual Studio environment is set up (needs dumpbin and VCToolsRedistDir from vcvars64).
#
# Layout of the package:
#   VidBox\VidBox.exe
#   VidBox\*.dll              FFmpeg DLLs the exe actually uses + the Visual C++ runtime (app-local)
#   VidBox\vfx\bin\           NVIDIA Video Effects runtime (loaded from exe\vfx, see src/vfx.cpp)
#   VidBox\vfx\features\      only the two features VidBox uses (frame generation, super resolution)
#   VidBox\licenses\          third-party licenses
param(
  [Parameter(Mandatory)] [string]$BuildDir,
  [Parameter(Mandatory)] [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$root    = Split-Path -Parent $PSScriptRoot
$vfx     = Join-Path $root 'sdk\vfx\VideoFX'
$ffmpeg  = Join-Path $root 'third_party\ffmpeg'
$vcpkg   = Join-Path $root 'vcpkg_installed\x64-windows-static-md'
$features = 'nvvfxvideoframegeneration', 'nvvfxvideosuperres'

$version = (Get-Content (Join-Path $root 'vcpkg.json') -Raw | ConvertFrom-Json).'version-string'
$name    = "VidBox-$version-win64"
$stage   = Join-Path $OutDir 'VidBox'
$zip     = Join-Path $OutDir "$name.zip"

function Copy-Into([string]$src, [string]$dstDir) {
  New-Item -ItemType Directory -Force $dstDir | Out-Null
  Copy-Item $src $dstDir -Recurse -Force
}

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
if (Test-Path $zip)   { Remove-Item $zip -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

# --- exe ---
Copy-Item (Join-Path $BuildDir 'VidBox.exe') $stage

# --- FFmpeg: only the DLLs the exe needs, following their own dependencies ---
$ffDlls = @{}
Get-ChildItem (Join-Path $ffmpeg 'bin') -Filter *.dll | ForEach-Object { $ffDlls[$_.Name.ToLower()] = $_.FullName }
$queue = [System.Collections.Generic.Queue[string]]::new()
$queue.Enqueue((Join-Path $stage 'VidBox.exe'))
$seen = @{}
while ($queue.Count) {
  $file = $queue.Dequeue()
  foreach ($line in (& dumpbin /nologo /dependents $file)) {
    $dll = $line.Trim().ToLower()
    if ($ffDlls.ContainsKey($dll) -and -not $seen.ContainsKey($dll)) {
      $seen[$dll] = $true
      Copy-Item $ffDlls[$dll] $stage
      $queue.Enqueue($ffDlls[$dll])
    }
  }
}
if (-not $seen.Count) { throw 'No FFmpeg DLLs found among the exe dependencies (is dumpbin on PATH?)' }
Write-Host "FFmpeg DLLs: $($seen.Keys -join ', ')"

# --- Visual C++ runtime, app-local, so the target PC needs no VC++ Redistributable install ---
if (-not $env:VCToolsRedistDir) { throw 'VCToolsRedistDir is not set (run from build.cmd)' }
$crt = Get-ChildItem (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
if (-not $crt) { throw "Visual C++ runtime not found under $env:VCToolsRedistDir" }
Copy-Item (Join-Path $crt.FullName '*.dll') $stage

# --- NVIDIA Video Effects runtime ---
$vfxOut = Join-Path $stage 'vfx'
Copy-Into (Join-Path $vfx 'bin\*.dll') (Join-Path $vfxOut 'bin')
Copy-Item (Join-Path $vfx 'bin\ThirdPartyLicenses.txt') (Join-Path $vfxOut 'bin')
# bin\models only holds TensorRT models for features VidBox does not use; skip it.
foreach ($f in $features) {
  $src = Join-Path $vfx "features\$f"
  if (-not (Test-Path (Join-Path $src 'bin'))) { throw "Feature $f is not installed in the SDK (see README)" }
  Copy-Into (Join-Path $src 'bin\*') (Join-Path $vfxOut "features\$f\bin")
  Copy-Into (Join-Path $src 'license\*') (Join-Path $vfxOut "features\$f\license")
}
Copy-Into (Join-Path $vfx 'license\*') (Join-Path $vfxOut 'license')

# --- Licenses for everything else that ships ---
$lic = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force $lic | Out-Null
Copy-Item (Join-Path $ffmpeg 'LICENSE.txt') (Join-Path $lic 'FFmpeg-LICENSE.txt')
foreach ($p in 'imgui', 'json') {
  $l = Get-ChildItem (Join-Path $root "third_party\$p") -File | Where-Object Name -match '^(LICENSE|COPYING)' | Select-Object -First 1
  if ($l) { Copy-Item $l.FullName (Join-Path $lic "$p-$($l.Name)") }
}
# libdatachannel, OpenSSL, libsrtp, ... (statically linked, built by vcpkg)
Get-ChildItem (Join-Path $vcpkg 'share') -Directory | ForEach-Object {
  $c = Join-Path $_.FullName 'copyright'
  if (Test-Path $c) { Copy-Item $c (Join-Path $lic "$($_.Name)-copyright.txt") }
}

# --- Zip (bsdtar ships with Windows 10+, much faster than Compress-Archive for ~1 GB) ---
& tar.exe -a -c -f $zip -C $OutDir VidBox
if ($LASTEXITCODE) { throw "tar failed ($LASTEXITCODE)" }

$size = (Get-Item $zip).Length / 1MB
Write-Host ("Package: {0} ({1:N0} MB)" -f $zip, $size)
