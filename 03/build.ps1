# If your powershell is old, try updating with:
# winget install --id Microsoft.PowerShell --source winget
# Run `pwsh` to access PowerShell 7.x

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function CloneAsNeeded {
  param (
    [string]$CloneRelativeDir,
    [string]$CloneUrl
  )
  $Result = Join-Path $PSScriptRoot $CloneRelativeDir
  if (Test-Path $Result) {
    Write-Host "Repo clone found at $Result"
  } else {
    Write-Host "Cloning repo at $Result"
    git clone $CloneUrl $Result
  }
  return $Result
}

$ProjectPath = $PSScriptRoot

# Clone repos
$SdkSamplePath = CloneAsNeeded "directx-sdk-samples" "https://github.com/walbourn/directx-sdk-samples"
$OpusPath = CloneAsNeeded "opus" "https://github.com/xiph/opus"
$OpusToolsPath = CloneAsNeeded "opus-tools" "https://github.com/xiph/opus-tools"

# Build opus
pushd .
cd $OpusPath
if (-not (test-path build)) {
  mkdir build
}
cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build .
popd

# Copy resampler files
$ResamplerHdr = Join-Path $OpusToolsPath "src" "speex_resampler.h"
$ResamplerCpp = Join-Path $OpusToolsPath "src" "resample.c"
$ArchHdr = Join-Path $OpusToolsPath "src" "arch.h"
Copy-Item $ResamplerHdr (Join-Path $ProjectPath "speex_resampler.h")
Copy-Item $ResamplerCpp (Join-Path $ProjectPath "resample.c")
Copy-Item $ArchHdr (Join-Path $ProjectPath "arch.h")

# Build sample
cl.exe /Iopus\include /EHsc /MDd /D "RANDOM_PREFIX=opustools" /D "OUTSIDE_SPEEX" /D "RESAMPLE_FULL_SINC_TABLE" play.cpp WAVFileReader.cpp resample.c /Fe: play.exe /link ole32.lib user32.lib /LIBPATH:opus\build\Debug

# Run sample
.\play.exe
