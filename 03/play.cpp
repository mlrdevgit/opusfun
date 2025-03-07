#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <memory>
#include <vector>

#include <xaudio2.h>

#include <wrl\client.h>

#include <opus.h>
#pragma comment(lib, "opus.lib")

#include "speex_resampler.h"

#include "WAVFileReader.h"

const wchar_t PlayFileName[] = LR"(directx-sdk-samples\Media\Wavs\MusicMono.wav)";
const char CompressedFileName[] = "scratch.opus";
const wchar_t DecompressedFileName[] = L"scratch.opus.wav";

namespace mwrl = Microsoft::WRL;

// Error codes from C libraries (0n150) - 0x8096xxxx
#define FACILITY_ERRNO (0x96)
#define HRESULT_FROM_ERRNO(x) MAKE_HRESULT(1, FACILITY_ERRNO, (x))

// Error handling.
#define IFC(x) do { \
  hr = x; if (FAILED(hr)) { goto Cleanup; } \
  } while (false)
#define IFC_OPUS(x) do { \
  int __opus_err = (x); if (__opus_err < 0) { hr = E_FAIL; goto Cleanup; } \
  } while (false)
#define IFC_RESAMPLER(x) do { \
  int __resampler_err = (x); if (__resampler_err != RESAMPLER_ERR_SUCCESS) { \
    hr = E_FAIL; goto Cleanup; \
  } } while (false)


bool IsFormatOrSubFormat(const WAVEFORMATEX* wfx, WORD format, const GUID& subFormat)
{
    return wfx->wFormatTag == format ||
        (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            IsEqualGUID(((const WAVEFORMATEXTENSIBLE*)wfx)->SubFormat, subFormat));
}

HRESULT CheckFloatOrInt16(const WAVEFORMATEX* wfx, bool* isFloat)
{
    if (IsFormatOrSubFormat(wfx, WAVE_FORMAT_IEEE_FLOAT, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
    {
        *isFloat = true;
    }
    else if (IsFormatOrSubFormat(wfx, WAVE_FORMAT_PCM, KSDATAFORMAT_SUBTYPE_PCM))
    {
        *isFloat = false;
    }
    else
    {
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    return S_OK;
}

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

  IFC(XAudio2Create(xaudio.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR));
  IFC(xaudio->CreateMasteringVoice(&masteringVoice));
  IFC(PlayWave(xaudio.Get(), PlayFileName));

  masteringVoice->DestroyVoice();
  xaudio.Reset();

Cleanup:
  return hr;
}

HRESULT ResampleWaveData(int channels, int inRate, int outRate, bool isFloat, const void *in, int inCount, unsigned int *inProcessed, void *out, int outSize, unsigned int *outCount)
{
    const int quality = SPEEX_RESAMPLER_QUALITY_DEFAULT;
    int err = 0;
    HRESULT hr = S_OK;
    SpeexResamplerState *resampler = nullptr;
    if (channels > 1)
    {
        // Not yet implemented - need different function for interleaved
        IFC(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    }
    resampler = speex_resampler_init(channels, inRate, outRate, quality, &err);
    IFC_RESAMPLER(err);
    if (isFloat)
    {
        *inProcessed = inCount;
        *outCount = outSize;
        int channelIdx = 0;
        IFC_RESAMPLER(speex_resampler_process_float(resampler, channelIdx, (const float *)in, inProcessed, (float *)out, outCount));
    }
    else
    {
        *inProcessed = inCount;
        *outCount = outSize;
        int channelIdx = 0;
        IFC_RESAMPLER(speex_resampler_process_int(resampler, channelIdx, (const short*)in, inProcessed, (short*)out, outCount));
    }
Cleanup:
    if (resampler)
    {
        speex_resampler_destroy(resampler);
    }
    return hr;
}

HRESULT WriteToFile(const void* ptr, size_t len, FILE* fp)
{
    size_t written = fwrite(ptr, 1, len, fp);
    if (written != len)
    {
        return HRESULT_FROM_ERRNO(errno);
    }
    return S_OK;
}

template <typename T>
HRESULT WriteValueToFile(const T& value, FILE* fp)
{
    return WriteToFile(&value, sizeof(T), fp);
}

HRESULT ReadFromFile(void *ptr, size_t len, FILE* fp)
{
    size_t read = fread(ptr, 1, len, fp);
    if (read != len)
    {
        return HRESULT_FROM_ERRNO(errno);
    }
    return S_OK;
}

template <typename T>
HRESULT ReadValueFromFile(T& value, FILE* fp)
{
    return ReadFromFile(&value, sizeof(T), fp);
}

template <typename T, typename TOther>
void AppendBufferByMemcpy(std::vector<T>& value, const TOther* ptr, size_t elementCount)
{
    static_assert(sizeof(TOther) % sizeof(T) == 0);
    size_t s = value.size();
    size_t destSize = elementCount * sizeof(TOther) / sizeof(T);
    value.resize(value.size() + destSize);
    memcpy(value.data() + s, ptr, elementCount * sizeof(TOther));
}

HRESULT RunFileCompress()
{
    HRESULT hr = S_OK;
    BOOL isRunning = TRUE;
    std::unique_ptr<uint8_t[]> waveFile;
    DirectX::WAVData waveData;
    XAUDIO2_BUFFER buffer = {};
    IXAudio2SourceVoice* pSourceVoice = nullptr;
    unsigned char encodedData[1024 * 4];
    size_t encodedDataLength = 0;
    // Could be OPUS_APPLICATION_VOIP instead
    const int application = OPUS_APPLICATION_AUDIO;
    int error;
    OpusEncoder* enc = nullptr;
    const opus_int16* audioSamples = nullptr;
    const opus_int16* audioSamplesEnd = nullptr;
    int maxFrameSizeInSamples = 0;
    bool isFloat;
    bool isOpusSampleRate;
    void* resampledData = nullptr;
    size_t audioSampleCount = 0; // count of audio samples in buffer
    size_t audioSampleSize; // size of a single audio sample, in bytes
    const int resampleTargetRate = 24000;
    int audioSamplesPerSec;
    FILE* encodedFile = nullptr;

    // Write out the encoded data to a file.
    encodedFile = fopen(CompressedFileName, "wb");
    if (encodedFile == nullptr)
    {
        IFC(HRESULT_FROM_ERRNO(errno));
    }

    IFC(DirectX::LoadWAVAudioFromFileEx(PlayFileName, waveFile, waveData));
    IFC(CheckFloatOrInt16(waveData.wfx, &isFloat));
    audioSamplesPerSec = waveData.wfx->nSamplesPerSec;
    audioSampleSize = isFloat ? sizeof(float) : sizeof(uint16_t);
    maxFrameSizeInSamples = audioSamplesPerSec / 100; // (10 ms intervals)
    audioSamples = (opus_int16 *)waveFile.get();
    audioSamplesEnd = audioSamples + waveData.audioBytes / 2;
    audioSampleCount = waveData.audioBytes / audioSampleSize;

    // should resample if not one of 8000, 12000, 16000, 24000, or 48000
    isOpusSampleRate =
        audioSamplesPerSec == 8000 ||
        audioSamplesPerSec == 12000 ||
        audioSamplesPerSec == 16000 ||
        audioSamplesPerSec == 24000 ||
        audioSamplesPerSec == 48000;
    if (!isOpusSampleRate)
    {
        const int* inData = (const int *)audioSamples;
        float audioDurationSeconds = (float)audioSampleCount / audioSamplesPerSec;
        int inCount = audioSampleCount;
        unsigned int inProcessed = 0;
        int outSize = audioDurationSeconds * resampleTargetRate;
        unsigned int outCount = 0;
        resampledData = malloc(outSize * audioSampleSize);
        IFC(ResampleWaveData(waveData.wfx->nChannels, audioSamplesPerSec, 24000, isFloat, inData, inCount, &inProcessed, resampledData, outSize, &outCount));
        audioSamplesPerSec = resampleTargetRate;
        audioSamples = (const opus_int16*)resampledData;
        audioSamplesEnd = audioSamples + outCount * (audioSampleSize / sizeof(opus_int16));
        maxFrameSizeInSamples = audioSamplesPerSec / 100; // (10 ms intervals)
        audioSampleCount = outCount;
    }

    IFC(WriteValueToFile(audioSamplesPerSec, encodedFile));

    enc = opus_encoder_create(audioSamplesPerSec, waveData.wfx->nChannels, application, &error);
    IFC_OPUS(error);

    // Now, process all audio samples in 10ms increments, and simply add them to a large buffer.
    while (audioSamples < audioSamplesEnd)
    {
        // likely wrong, need channels
        int frameSize = std::min(maxFrameSizeInSamples, (int)(audioSamplesEnd - audioSamples));
        opus_int32 max_data_bytes = sizeof(encodedData);
        opus_int32 lenOrErr = isFloat ?
            opus_encode_float(enc, (const float *)audioSamples, frameSize, encodedData, max_data_bytes) : 
            opus_encode(enc, audioSamples, frameSize, encodedData, max_data_bytes);
        if (lenOrErr < 0 && frameSize < maxFrameSizeInSamples)
        {
            // The last frame might not be an acceptable frame size, drop the last few milliseconds.
            break;
        }
        IFC_OPUS(lenOrErr);
        IFC(WriteValueToFile(lenOrErr, encodedFile));
        IFC(WriteToFile(encodedData, lenOrErr, encodedFile));
        audioSamples += frameSize;
        encodedDataLength += lenOrErr;
    }

Cleanup:
    if (encodedFile)
    {
        fclose(encodedFile);
    }
    free(resampledData);
    opus_encoder_destroy(enc);
    return hr;
}

HRESULT RunFileDecompress()
{
    HRESULT hr = S_OK;
    OpusDecoder* dec = nullptr;
    const int sampleRate = 24000;
    const int channels = 1;
    int error;
    FILE* encodedFile = nullptr;
    long encodedFileLength;
    long encodedFileRead;
    int audioSamplesPerSec;
    std::unique_ptr<uint8_t[]> encodedBuffer;
    std::vector<uint8_t> outData;
    encodedFile = fopen(CompressedFileName, "rb");
    if (!encodedFile)
    {
        IFC(HRESULT_FROM_ERRNO(errno));
    }
    ReadValueFromFile(audioSamplesPerSec, encodedFile);

    dec = opus_decoder_create(sampleRate, channels, &error);
    IFC_OPUS(error);

    for (;;)
    {
        uint8_t encodedBuffer[1024 * 8];
        opus_int16 pcmBuffer[1024 * 8];
        opus_int32 packetLen;
        if (FAILED(ReadValueFromFile(packetLen, encodedFile)))
        {
            hr = S_OK;
            break;
        }
        IFC(ReadFromFile(encodedBuffer, packetLen, encodedFile));

        // Lost packets can be replaced with loss concealment by calling
        // the decoder with a null pointer and zero length for the missing packet.
        int sampleCount = opus_decode(dec, encodedBuffer, packetLen, pcmBuffer, sizeof(pcmBuffer) / sizeof(pcmBuffer[0]), 0);
        IFC_OPUS(sampleCount);
        AppendBufferByMemcpy(outData, pcmBuffer, sampleCount);
    }

    {
        WAVEFORMATEX localFormat;
        localFormat.wFormatTag = WAVE_FORMAT_PCM;
        localFormat.nChannels = 1;
        localFormat.nSamplesPerSec = audioSamplesPerSec;
        localFormat.nAvgBytesPerSec = localFormat.nSamplesPerSec * 2;
        localFormat.nBlockAlign = 2;
        localFormat.wBitsPerSample = 16;
        localFormat.cbSize = 0;

        DirectX::WAVData wavData = {};
        wavData.audioBytes = outData.size();
        wavData.wfx = &localFormat;
        wavData.startAudio = outData.data();
        DirectX::WriteWAVDataToFile(DecompressedFileName, wavData);
    }

Cleanup:
    if (encodedFile)
    {
        fclose(encodedFile);
    }
    opus_decoder_destroy(dec);
    return hr;
}

int main()
{
  HRESULT hr = S_OK;

  IFC(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  IFC(RunFileCompress());
  IFC(RunFileDecompress());
  IFC(RunFilePlayback());
  CoUninitialize();

Cleanup:
  return FAILED(hr) ? 1 : 0;
}
