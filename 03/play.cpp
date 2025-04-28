#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cassert>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include <WinSock2.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <xaudio2.h>

#include <opus.h>
#pragma comment(lib, "opus.lib")

#include "hiredis.h"

#include "speex_resampler.h"

#define REFTIMES_PER_SEC 10000000
#define REFTIMES_PER_MILLISEC 10000

const wchar_t PlayFileName[] =
    LR"(directx-sdk-samples\Media\Wavs\MusicMono.wav)";
const char CompressedFileName[] = "scratch.opus";
const wchar_t DecompressedFileName[] = L"scratch.opus.wav";

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

// Error codes from C libraries (0n150) - 0x8096xxxx
#define FACILITY_ERRNO (0x96)
#define HRESULT_FROM_ERRNO(x) MAKE_HRESULT(1, FACILITY_ERRNO, (x))

// Error handling.
#define IFC(x)                                                                 \
  do {                                                                         \
    hr = x;                                                                    \
    if (FAILED(hr)) {                                                          \
      printf("Failed with 0x%08x at %d\n", hr, __LINE__); \
      goto Cleanup;                                                            \
    }                                                                          \
  } while (false)
#define IFC_OPUS(x)                                                            \
  do {                                                                         \
    int __opus_err = (x);                                                      \
    if (__opus_err < 0) {                                                      \
      hr = E_FAIL;                                                             \
      goto Cleanup;                                                            \
    }                                                                          \
  } while (false)
#define IFC_RESAMPLER(x)                                                       \
  do {                                                                         \
    int __resampler_err = (x);                                                 \
    if (__resampler_err != RESAMPLER_ERR_SUCCESS) {                            \
      hr = E_FAIL;                                                             \
      goto Cleanup;                                                            \
    }                                                                          \
  } while (false)

////////////////////////////////////////////////////////////////////////////
// Shared declarations.

struct SenderPacketHeader {
  uint32_t dataLength;
  uint32_t samplesPerSecond;
  uint16_t frameIndex;
  uint8_t channels;
  uint8_t padding;
};

const char *g_rhost;
const char *g_rpwd;
const int g_rport = 6379;
const char *g_broadcastTopic = "convo";

bool IsFormatOrSubFormat(const WAVEFORMATEX *wfx, WORD format,
                         const GUID &subFormat) {
  return wfx->wFormatTag == format ||
         (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
          IsEqualGUID(((const WAVEFORMATEXTENSIBLE *)wfx)->SubFormat,
                      subFormat));
}

HRESULT CheckFloatOrInt16(const WAVEFORMATEX *wfx, bool *isFloat) {
  if (IsFormatOrSubFormat(wfx, WAVE_FORMAT_IEEE_FLOAT,
                          KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
    *isFloat = true;
  } else if (IsFormatOrSubFormat(wfx, WAVE_FORMAT_PCM,
                                 KSDATAFORMAT_SUBTYPE_PCM)) {
    *isFloat = false;
  } else {
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
  }
  return S_OK;
}

//! Create a connection to a redis host.
static redisContext *connectToHost(const char *rhost, const char *rpwd) {
  redisContext *rctx; // redis context object
  redisReply *reply;  // redis reply object

  printf("Connecting to redis server %s...\n", rhost);
  rctx = redisConnect(rhost, g_rport);
  if (!rctx || rctx->err) {
    if (rctx) {
      printf("Failed to connect: %s\n", rctx->errstr);
    } else {
      printf("Failed to create redis context\n");
    }
    return 0;
  }

  printf("Authenticating with redis server...\n");
  reply = (redisReply *)redisCommand(rctx, "AUTH %s", rpwd);
  if (!reply || rctx->err) {
    printf("Failed redis authorization\n");
    freeReplyObject(reply); // ok to call if null
    redisFree(rctx);
    return 0;
  }
  freeReplyObject(reply);
  printf("Connected to redis server\n");
  return rctx;
}

static HRESULT CheckWaveFormat(WAVEFORMATEX *pwfx, bool *isFloat) {
  HRESULT hr = S_OK;
  // Should probably support WAVE_FORMAT_PCM as well.
  if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    *isFloat = true;
  } else {
    WAVEFORMATEXTENSIBLE
        *extensible; // pointer into extensible area of audio format description
    if (pwfx->wFormatTag != WAVE_FORMAT_EXTENSIBLE) {
      wprintf(L"Only supporting WAVE_FORMAT_IEEE_FLOAT(%u) or "
              L"WAVE_FORMAT_EXTENSIBLE(%u) format\n",
              (unsigned)WAVE_FORMAT_PCM, (unsigned)WAVE_FORMAT_EXTENSIBLE);
      IFC(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    }
    extensible = (WAVEFORMATEXTENSIBLE *)pwfx;
    if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
      *isFloat = true;
    } else if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
      if (pwfx->wBitsPerSample != 16 ||
          extensible->Samples.wValidBitsPerSample != 16) {
        wprintf(L"Only 16-bit int PCM samples are supported\n");
        IFC(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
      }
      *isFloat = false;
    } else {
      wprintf(L"Only supporting KSDATAFORMAT_SUBTYPE_IEEE_FLOAT for "
              L"WAVE_FORMAT_EXTENSIBLE(%u) format\n",
              (unsigned)WAVE_FORMAT_EXTENSIBLE);
      IFC(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    }
  }
Cleanup:
  return hr;
}

static void PrintWaveFormat(WAVEFORMATEX *pwfx) {
  wprintf(
      L"Data format tag=%u channels=%u samples-per-sec=%u avg-bytes-per-sec=%u "
      L"block-align=%u bits-per-mono-sample=%u extra-size=%u\n",
      (unsigned)pwfx->wFormatTag, (unsigned)pwfx->nChannels,
      (unsigned)pwfx->nSamplesPerSec, (unsigned)pwfx->nAvgBytesPerSec,
      (unsigned)pwfx->nBlockAlign, (unsigned)pwfx->wBitsPerSample,
      (unsigned)pwfx->cbSize);
}

static HRESULT ResampleWaveData(int channels, int inRate, int outRate,
                                bool isFloat, const void *in, int inCount,
                                unsigned int *inProcessed, void *out,
                                int outSize, unsigned int *outCount) {
  const int quality = SPEEX_RESAMPLER_QUALITY_DEFAULT;
  int err = 0;
  HRESULT hr = S_OK;
  SpeexResamplerState *resampler = nullptr;
  if (channels > 1) {
    // Not yet implemented - need different function for interleaved
    IFC(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
  }
  resampler = speex_resampler_init(channels, inRate, outRate, quality, &err);
  IFC_RESAMPLER(err);
  if (isFloat) {
    *inProcessed = inCount;
    *outCount = outSize;
    int channelIdx = 0;
    IFC_RESAMPLER(speex_resampler_process_float(resampler, channelIdx,
                                                (const float *)in, inProcessed,
                                                (float *)out, outCount));
  } else {
    *inProcessed = inCount;
    *outCount = outSize;
    int channelIdx = 0;
    IFC_RESAMPLER(speex_resampler_process_int(resampler, channelIdx,
                                              (const short *)in, inProcessed,
                                              (short *)out, outCount));
  }
Cleanup:
  if (resampler) {
    speex_resampler_destroy(resampler);
  }
  return hr;
}

HRESULT WriteToFile(const void *ptr, size_t len, FILE *fp) {
  size_t written = fwrite(ptr, 1, len, fp);
  if (written != len) {
    return HRESULT_FROM_ERRNO(errno);
  }
  return S_OK;
}

template <typename T> HRESULT WriteValueToFile(const T &value, FILE *fp) {
  return WriteToFile(&value, sizeof(T), fp);
}

template <typename T, typename TOther>
void AppendBufferByMemcpy(std::vector<T> &value, const TOther *ptr,
                          size_t elementCount) {
  static_assert(sizeof(TOther) % sizeof(T) == 0);
  size_t s = value.size();
  size_t destSize = elementCount * sizeof(TOther) / sizeof(T);
  value.resize(value.size() + destSize);
  memcpy(value.data() + s, ptr, elementCount * sizeof(TOther));
}

////////////////////////////////////////////////////////////////////////////
// Sender.

//! Use this class to manage audio frame data from the microphone.
class MicrophoneAudioFrameDataController {
public:
  void Setup(size_t frameDataForTenMsInBytes) {
    m_frameDataForTenMsInBytes = frameDataForTenMsInBytes;
  }

  void HandleAudioData(const uint8_t *pData, size_t dataSizeInBytes) {
    assert(m_pData == nullptr);
    assert(m_numBytesAvailableInData == 0);
    m_pData = pData;
    m_numBytesAvailableInData = dataSizeInBytes;
  }

  bool AcquireFrameData(const uint8_t **pOutData, uint32_t *outDataSize) {
    *pOutData = nullptr;
    *outDataSize = 0;
    // If we have enough data in the managed buffer, return it.
    if (m_frameDataSize >= m_frameDataForTenMsInBytes) {
      *pOutData = m_frameData.data();
      *outDataSize = m_frameDataForTenMsInBytes;
      return true;
    }

    // If we have a bit left over, see if we can move some data into it and
    // return a continguous buffer.
    if (m_frameDataSize > 0) {
      if (m_frameDataSize + m_numBytesAvailableInData >=
          m_frameDataForTenMsInBytes) {
        AppendDataIntoManagedBuffer(m_frameDataForTenMsInBytes -
                                    m_frameDataSize);
        *pOutData = m_frameData.data();
        *outDataSize = m_frameDataForTenMsInBytes;
        return true;
      } else {
        return false;
      }
    }

    // If the managed buffer is empty, see if we can return some data from the
    // audio buffer.
    assert(m_frameDataSize == 0);
    if (m_frameDataForTenMsInBytes <= m_numBytesAvailableInData) {
      *pOutData = m_pData;
      *outDataSize = m_frameDataForTenMsInBytes;
      return true;
    }

    return false;
  }

  void ReleaseFrameData(const uint8_t *ptr, uint32_t dataSize) {
    if (ptr == m_frameData.data()) {
      if (dataSize == m_frameDataSize) {
        m_frameDataSize = 0;
      } else {
        m_frameDataSize -= dataSize;
        memcpy(m_frameData.data(), m_frameData.data() + dataSize,
               m_frameDataSize);
      }
    } else {
      assert(ptr == m_pData);
      assert(dataSize <= m_numBytesAvailableInData);
      m_pData += dataSize;
      m_numBytesAvailableInData -= dataSize;
    }
  }

  void PrepareToRelease() {
    // Move the remainder of available data into managed buffer.
    if (m_numBytesAvailableInData > 0) {
      AppendDataIntoManagedBuffer(m_numBytesAvailableInData);
      assert(m_numBytesAvailableInData == 0);
    }
    m_pData = nullptr;
  }

private:
  void AppendDataIntoManagedBuffer(uint32_t dataInBytes) {
    assert(dataInBytes <= m_numBytesAvailableInData);
    if (m_frameDataSize + dataInBytes > m_frameData.size()) {
      m_frameData.resize(m_frameDataSize + dataInBytes);
    }
    uint8_t *end = m_frameData.data() + m_frameDataSize;
    memcpy(end, m_pData, dataInBytes);
    m_frameDataSize += dataInBytes;
    m_numBytesAvailableInData -= dataInBytes;
    m_pData += dataInBytes;
  }

  const uint8_t *m_pData{nullptr};
  size_t m_numBytesAvailableInData{0};
  size_t m_frameDataForTenMsInBytes{0};
  size_t m_frameDataSize{0};
  std::vector<uint8_t> m_frameData;
};

HRESULT RunSender() {
  HRESULT hr = S_OK;

  // Time control.
  ULONGLONG senderTimeMs = GetTickCount64();
  ULONGLONG exitTimeMs = senderTimeMs + 10 * 1000;

  // Audio client (microphone) and buffers.
  REFERENCE_TIME hnsRequestedDuration =
      REFTIMES_PER_MILLISEC * 10;   // initial desired duration
  REFERENCE_TIME hnsActualDuration; // actual duration in allocated buffer
  UINT32 bufferFrameCount; // maximum number of frames in audio client buffer
  UINT32
      numFramesAvailable; // available number of frames in audio client buffer
  IMMDeviceEnumerator *pEnumerator = NULL; // used to find microphone
  IMMDevice *pDevice = NULL;               // used to find microphone
  IPropertyStore *pDeviceProperties = nullptr;
  IAudioClient *pAudioClient =
      NULL; // used to create and initialize an audio stream between e app and
            // the audio engine
  IAudioCaptureClient *pCaptureClient =
      NULL; // used to read input data from a capture endpoint buffer
  WAVEFORMATEX *pwfx = NULL; // audio format for audio client
  WAVEFORMATEXTENSIBLE *extensible =
      NULL; // pointer into extensible area of audio format description
  UINT32 packetLength = 0; // number of frames in the next data packet in the
                           // capture endpoint buffer
  BOOL bDone = FALSE;
  BYTE *pData; // pointer into the capture client buffer for the next data
               // packet to be read
  DWORD flags; // flags about bufffer
  MicrophoneAudioFrameDataController
      audioFrameData; // controller for audio frame data

  // Resampler and buffers.
  const int quality = SPEEX_RESAMPLER_QUALITY_DEFAULT;
  int err = 0;
  SpeexResamplerState *resampler = nullptr;
  const int application = OPUS_APPLICATION_VOIP;
  int error;
  int maxFrameSizeInSamples = 0;
  bool isFloat;
  bool isOpusSampleRate;
  void *resampledData = nullptr;
  size_t audioSampleCount = 0; // count of audio samples in buffer
  size_t audioSampleSize;      // size of a single audio sample, in bytes
  const int resampleTargetRate = 24000;
  int audioSamplesPerSec;
  unsigned numFramesIn10Ms;

  // Encoder and buffers.
  OpusEncoder *enc = nullptr;
  std::vector<uint8_t> packetBuffer;
  SenderPacketHeader *packetHeader = nullptr;
  unsigned char *encodedData = nullptr;
  size_t encodedDataCapacity = 0;

  // Connection.
  redisContext *senderContext = nullptr;
  redisReply *senderReply = nullptr;

  // Setup microphone, resampler, encoder, connection.
  IFC(CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                       IID_IMMDeviceEnumerator, (void **)&pEnumerator));
  hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &pDevice);
  if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
  {
    printf("You do not seem to have a microphone connected.\n");
  }
  IFC(hr);
  IFC(pDevice->OpenPropertyStore(STGM_READ, &pDeviceProperties));
  {
    DWORD c;
    IFC(pDeviceProperties->GetCount(&c));
    for (DWORD i = 0; i < c; ++c) {
      PROPERTYKEY key;
      PROPVARIANT pv;
      PropVariantInit(&pv);
      IFC(pDeviceProperties->GetAt(i, &key));
      IFC(pDeviceProperties->GetValue(key, &pv));
      if (pv.vt == VT_BSTR) {
        wprintf(L"%s\n", pv.bstrVal);
      } else {
        printf("Unknown property vt=%u\n", pv.vt);
      }
      IFC(PropVariantClear(&pv));
    }
  }
  IFC(pDevice->Activate(IID_IAudioClient, CLSCTX_ALL, NULL,
                        (void **)&pAudioClient));
  IFC(pAudioClient->GetMixFormat(&pwfx));
  IFC(pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                               hnsRequestedDuration, 0, pwfx, NULL));
  IFC(pAudioClient->GetBufferSize(&bufferFrameCount));
  IFC(pAudioClient->GetService(IID_IAudioCaptureClient,
                               (void **)&pCaptureClient));
  PrintWaveFormat(pwfx);
  IFC(CheckWaveFormat(pwfx, &isFloat));
  // Calculate the actual duration of the allocated buffer.
  hnsActualDuration =
      (double)REFTIMES_PER_SEC * bufferFrameCount / pwfx->nSamplesPerSec;
  printf("Actual buffer duration in ms: %u bufferFrameCount=%u\n",
         (unsigned)(hnsActualDuration / REFTIMES_PER_MILLISEC),
         bufferFrameCount);
  audioSamplesPerSec = pwfx->nSamplesPerSec;

  // Example values:
  // pwfx->wFormatTag = WAVE_FORMAT_EXTENSIBLE
  // pwfx->nChannels = 2
  // pwfx->nSamplesPerSec = 48000
  // pwfx->nAvgBytesPerSec = 384000 (ie, 48000 * 8 or nSamplesPerSec *
  // nBlockAlign for PCM) pwfx->wBitsPerSample = 32 (ie, 4 bytes per sample)
  // extensible->Samples.wValidBitsPerSample = 32 (ie, all bits have
  // information, this isn't a 20-bits-in-32 case) extensible->dwChannelMask = 3
  // (front left and right speakers) extensible->SubFormat =
  // WAVE_FORMAT_IEEE_FLOAT

  // Setup resampler.
  isOpusSampleRate = audioSamplesPerSec == 8000 ||
                     audioSamplesPerSec == 12000 ||
                     audioSamplesPerSec == 16000 ||
                     audioSamplesPerSec == 24000 || audioSamplesPerSec == 48000;

  if (pwfx->nChannels > 2) {
    // Supporting only mono or stereo.
    IFC(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
  }
  audioSamplesPerSec = pwfx->nSamplesPerSec;
  audioSampleSize = isFloat ? sizeof(float) : sizeof(uint16_t);
  maxFrameSizeInSamples = audioSamplesPerSec / 100; // (10 ms intervals)
  if (!isOpusSampleRate) {
    resampler = speex_resampler_init(pwfx->nChannels, audioSamplesPerSec,
                                     resampleTargetRate, quality, &err);
    IFC_RESAMPLER(err);
  }

  // Setup encoder.
  enc = opus_encoder_create(audioSamplesPerSec, pwfx->nChannels, application,
                            &error);
  IFC_OPUS(error);
  numFramesIn10Ms = (audioSamplesPerSec / 100) * pwfx->nChannels;
  encodedDataCapacity =
      (audioSamplesPerSec / 100) * 4 *
      pwfx->nChannels; // 4 bytes per sample for each 10ms, per channel
  packetBuffer.resize(sizeof(SenderPacketHeader) + encodedDataCapacity);
  packetHeader = reinterpret_cast<SenderPacketHeader *>(packetBuffer.data());
  encodedData = reinterpret_cast<unsigned char *>(packetHeader + 1);
  packetHeader->channels = pwfx->nChannels;
  packetHeader->frameIndex = 0;
  packetHeader->padding = 0;
  packetHeader->samplesPerSecond = audioSamplesPerSec;
  audioFrameData.Setup(pwfx->nAvgBytesPerSec / 100);

  // Setup connection.
  senderContext = connectToHost(g_rhost, g_rpwd);
  if (!senderContext) {
    printf("Failed to create sender context\n");
    return 1;
  }

  // Loop: read microphone, resample, encode, transmit.
  IFC(pAudioClient->Start());
  for (;;) {
    if (GetTickCount64() >= exitTimeMs) {
      break;
    }

    // Sleep for half the buffer duration.
    Sleep(hnsActualDuration / REFTIMES_PER_MILLISEC / 2);

    for (;;) {
      if (GetTickCount64() >= exitTimeMs) {
        break;
      }

      // Packets may be smaller than 10ms, in which case we should copy the data
      // to concatenate with data in future packets.
      // TODO: copy and append into buffer, extract (or try and be clever and
      // minimize copies)
      //
      // Get the available data in the shared buffer.
      IFC(pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL,
                                    NULL));
      if (hr == AUDCLNT_S_BUFFER_EMPTY) {
        break;
      }
      audioFrameData.HandleAudioData(
          pData, numFramesAvailable * pwfx->nBlockAlign * pwfx->nChannels);
      opus_int32 lenOrErr;
      if (!isOpusSampleRate) {
        // TODO: resample and encode
      } else {
        const uint8_t *encodingFrameData;
        uint32_t encodingFrameDataSizeInBytes;
        while (audioFrameData.AcquireFrameData(&encodingFrameData,
                                               &encodingFrameDataSizeInBytes)) {
          unsigned encodingFrameDataSizeInFrames =
              encodingFrameDataSizeInBytes /
              (pwfx->nBlockAlign * pwfx->nChannels);
          lenOrErr =
              isFloat ? opus_encode_float(enc, (const float *)encodingFrameData,
                                          encodingFrameDataSizeInFrames,
                                          encodedData, encodedDataCapacity)
                      : opus_encode(enc, (const int16_t *)encodingFrameData,
                                    encodingFrameDataSizeInFrames, encodedData,
                                    encodedDataCapacity);
          if (lenOrErr < 0) {
            // The last frame might not be an acceptable frame size, drop the
            // last few milliseconds.
            printf("Failed with numFramesAvailable=%u numFramesIn10Ms=%u\n",
                   (unsigned)encodingFrameDataSizeInFrames, numFramesIn10Ms);
            break;
          }
          audioFrameData.ReleaseFrameData(encodingFrameData,
                                          encodingFrameDataSizeInBytes);

          // Now, packetize and send it out.
          packetHeader->frameIndex++;
          packetHeader->dataLength = lenOrErr;
          size_t packetLength = sizeof(*packetHeader) + lenOrErr;
          void *senderReply =
              redisCommand(senderContext, "PUBLISH %s %b", g_broadcastTopic,
                           packetHeader, packetLength);
          freeReplyObject(senderReply);
          printf("Sent packet %u len %u\n", packetHeader->frameIndex,
                 (unsigned)packetLength);
        }
      }

      audioFrameData.PrepareToRelease();
      IFC(pCaptureClient->ReleaseBuffer(numFramesAvailable));
    }
  }

Cleanup:
  // Cleanup microphone, resampler, encoder, connection.
  if (pAudioClient) {
    pAudioClient->Stop();
    pAudioClient->Release();
  }
  CoTaskMemFree(pwfx);
  if (pEnumerator)
    pEnumerator->Release();
  if (pDeviceProperties)
    pDeviceProperties->Release();
  if (pDevice)
    pDevice->Release();
  if (pCaptureClient)
    pCaptureClient->Release();
  if (resampler) {
    speex_resampler_destroy(resampler);
  }
  opus_encoder_destroy(enc);
  redisFree(senderContext);
  return hr;
}

////////////////////////////////////////////////////////////////////////////
// Receiver.

static redisContext *g_receiverContext;

void RunReceiverNetwork() {
  HRESULT hr = S_OK;
  FILE *fp = nullptr;
  redisContext *rc = connectToHost(g_rhost, g_rpwd);
  redisReply *reply;
  g_receiverContext = rc;
  fp = fopen("scratch_received.bin", "wb");
  reply = (redisReply *)redisCommand(rc, "SUBSCRIBE %s", g_broadcastTopic);
  if (reply) {
    freeReplyObject(reply);
    reply = NULL;
  }

  // struct timeval tv = { 0, 1000 };
  // redisSetTimeout(rc, tv);
  for (;;) {
    if (redisGetReply(rc, (void **)&reply) != REDIS_OK) {
      printf("Failed to read reply\n");
      break;
    }
    if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
      printf("Broadcast listener message: %d\n", (int)reply->element[2]->len);
      IFC(WriteToFile(reply->element[2]->str, reply->element[2]->len, fp));
    } else {
      printf("did not understand reply\n");
    }
    freeReplyObject(reply);
    reply = NULL;
  }
Cleanup:
  if (fp) {
    fclose(fp);
  }
  g_receiverContext = nullptr;
  redisFree(rc);
}

HRESULT RunReceiver() {
  std::thread receiverNetwork(RunReceiverNetwork);
  Sleep(30 * 1000);
  printf("Shutting down receiver...");
  closesocket(g_receiverContext->fd);
  receiverNetwork.join();
Cleanup:
  return S_OK;
}

////////////////////////////////////////////////////////////////////////////
// Main function, decide to act as sender or receiver.

int main(int argc, char *argv[]) {
  HRESULT hr = S_OK;
  bool isSender = true;
  bool isReceiver = false;

  IFC(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

  g_rhost = getenv("REDIS_HOST");
  g_rpwd = getenv("REDIS_PWD");
  g_rhost = "127.0.0.1";
  g_rpwd = "pwd";
  isSender = true;
  if (g_rhost == nullptr || g_rpwd == nullptr) {
    printf("Specify the REDIS_HOST and REDIS_PWD env variables\n");
    IFC(E_FAIL);
  }

  for (int i = 0; i < argc; i++) {
    if (strcmp("--send", argv[i]) == 0) {
      isSender = true;
      isReceiver = false;
    } else if (strcmp("--receive", argv[i]) == 0) {
      isReceiver = true;
      isSender = false;
    }
  }

  if (isSender == isReceiver) {
    printf("Use --send or --receive to specify one role\n");
    IFC(E_FAIL);
  }

  if (isSender) {
    IFC(RunSender());
  } else if (isReceiver) {
    IFC(RunReceiver());
  }

Cleanup:
  CoUninitialize();
  if (FAILED(hr)) {
    printf("Failed with error 0x%08x\n", hr);
  }
  return FAILED(hr) ? 1 : 0;
}
