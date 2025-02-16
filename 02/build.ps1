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

