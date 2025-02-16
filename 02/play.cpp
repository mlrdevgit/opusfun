#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <memory>

#include <xaudio2.h>

#include <wrl\client.h>

namespace mwrl = Microsoft::WRL;

#define IFC(x) do { hr = x; if (FAILED(hr)) { goto Cleanup; } } while (false)

#include "WAVFileReader.h"

HRESULT PlayWave(IXAudio2 * pXaudio2, LPCWSTR szFilename)
{
  HRESULT hr = S_OK;
  BOOL isRunning = TRUE;
  std::unique_ptr<uint8_t[]> waveFile;
  DirectX::WAVData waveData;
  XAUDIO2_BUFFER buffer = {};
  IXAudio2SourceVoice* pSourceVoice = nullptr;

  IFC(DirectX::LoadWAVAudioFromFileEx(szFilename, waveFile, waveData));
  IFC(pXaudio2->CreateSourceVoice(&pSourceVoice, waveData.wfx));

  // Submit the wave sample data using an XAUDIO2_BUFFER structure
  buffer.pAudioData = waveData.startAudio;
  buffer.Flags = XAUDIO2_END_OF_STREAM;  // tell the source voice not to expect any data after this buffer
  buffer.AudioBytes = waveData.audioBytes;
  IFC(pSourceVoice->SubmitSourceBuffer(&buffer));
  IFC(pSourceVoice->Start(0));

  // Let the sound play
  while (SUCCEEDED(hr) && isRunning)
  {
    XAUDIO2_VOICE_STATE state;
    pSourceVoice->GetState(&state);
    isRunning = (state.BuffersQueued > 0) != 0;

    // Wait till the escape key is pressed
    if (GetAsyncKeyState(VK_ESCAPE))
        break;

    Sleep(10);
  }

  // Wait till the escape key is released
  while (GetAsyncKeyState(VK_ESCAPE))
    Sleep(10);

  pSourceVoice->DestroyVoice();
Cleanup:
  return hr;
}

HRESULT RunFilePlayback()
{
  HRESULT hr = S_OK;
  mwrl::ComPtr<IXAudio2> xaudio;
  IXAudio2MasteringVoice* masteringVoice;

  const wchar_t WaveFileName[] = LR"(directx-sdk-samples\Media\Wavs\MusicMono.wav)";

  IFC(XAudio2Create(xaudio.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR));
  IFC(xaudio->CreateMasteringVoice(&masteringVoice));
  IFC(PlayWave(xaudio.Get(), WaveFileName));

  masteringVoice->DestroyVoice();
  xaudio.Reset();

Cleanup:
  return hr;
}

int main()
{
  HRESULT hr = S_OK;

  IFC(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  IFC(RunFilePlayback());
  CoUninitialize();

Cleanup:
  return FAILED(hr) ? 1 : 0;
}
