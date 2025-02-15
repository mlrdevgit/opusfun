# If your powershell is old, try updating with:
# winget install --id Microsoft.PowerShell --source winget
# Run `pwsh` to access PowerShell 7.x

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Clone walbourn/directx-sdk-samples
$ProjectPath = $PSScriptRoot
$SdkSamplePath = Join-Path $ProjectPath "directx-sdk-samples"
Write-Host $ProjectPath
Write-Host $SdkSamplePath
if (Test-Path $SdkSamplePath) {
  Write-Host "DX SDK samples found at $SdkSamplePath"
} else {
  Write-Host "Cloning DX SDK samples found at $SdkSamplePath"
  git clone https://github.com/walbourn/directx-sdk-samples $SdkSamplePath
}

# Copy file reader files.
$WAVFileReaderHdr = Join-Path $SdkSamplePath "XAudio2" "Common" "WAVFileReader.h"
$WAVFileReaderCpp = Join-Path $SdkSamplePath "XAudio2" "Common" "WAVFileReader.cpp"
Copy-Item $WAVFileReaderHdr (Join-Path $ProjectPath "WAVFileReader.h")
Copy-Item $WAVFileReaderCpp (Join-Path $ProjectPath "WAVFileReader.cpp")

# Make sure cl.exe is on path
$ClPath = ""
try {
  $ClPath = (Get-Command cl.exe).Path
} catch {
  # %comspec% /k "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  Write-Host "Cannot find cl.exe - are you missing running from VS Native Tools Command Prompt?"
  Return
}

# Build
cl.exe play.cpp WAVFileReader.cpp /Fe: play.exe /link ole32.lib user32.lib

# Run
.\play.exe

