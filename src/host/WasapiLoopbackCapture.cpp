#include "WasapiLoopbackCapture.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#include <wrl/client.h>

#include <vector>

using Microsoft::WRL::ComPtr;

namespace host {

namespace {

constexpr REFERENCE_TIME kBufferDuration = 10'000'000; // 1 second in 100ns ticks — worst-case pool

bool isFloatFormat(const WAVEFORMATEX *wf)
{
    if (!wf) return false;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && wf->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(wf);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

bool isIntPcmFormat(const WAVEFORMATEX *wf)
{
    if (!wf) return false;
    if (wf->wFormatTag == WAVE_FORMAT_PCM) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && wf->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(wf);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
    }
    return false;
}

// Converts a raw capture packet into interleaved float32. Supports:
//   - IEEE_FLOAT 32-bit
//   - PCM 16/24/32-bit little-endian signed
void convertToFloat(const BYTE *src, UINT32 frames, const WAVEFORMATEX *wf,
                    std::vector<float> &dst)
{
    const int ch = wf->nChannels;
    const int container = wf->wBitsPerSample / 8;
    dst.resize(static_cast<size_t>(frames) * ch);

    if (isFloatFormat(wf) && container == 4) {
        std::memcpy(dst.data(), src, static_cast<size_t>(frames) * ch * 4);
        return;
    }
    if (isIntPcmFormat(wf)) {
        if (container == 2) {
            const auto *s = reinterpret_cast<const int16_t *>(src);
            const float inv = 1.0f / 32768.0f;
            for (size_t i = 0, n = static_cast<size_t>(frames) * ch; i < n; ++i)
                dst[i] = static_cast<float>(s[i]) * inv;
            return;
        }
        if (container == 3) {
            const float inv = 1.0f / 8388608.0f;
            for (size_t i = 0, n = static_cast<size_t>(frames) * ch; i < n; ++i) {
                const int b0 = src[i * 3 + 0];
                const int b1 = src[i * 3 + 1];
                const int b2 = static_cast<int8_t>(src[i * 3 + 2]);
                const int v = b0 | (b1 << 8) | (b2 << 16);
                dst[i] = static_cast<float>(v) * inv;
            }
            return;
        }
        if (container == 4) {
            const auto *s = reinterpret_cast<const int32_t *>(src);
            const float inv = 1.0f / 2147483648.0f;
            for (size_t i = 0, n = static_cast<size_t>(frames) * ch; i < n; ++i)
                dst[i] = static_cast<float>(s[i]) * inv;
            return;
        }
    }
    std::fill(dst.begin(), dst.end(), 0.0f);
}

} // namespace

WasapiLoopbackCapture::WasapiLoopbackCapture() = default;

WasapiLoopbackCapture::~WasapiLoopbackCapture()
{
    stop();
}

QString WasapiLoopbackCapture::start(const QString &renderDeviceId, Callback cb)
{
    stop();
    if (!cb) return QStringLiteral("no callback provided");
    if (renderDeviceId.isEmpty()) return QStringLiteral("no capture device selected");

    m_stopRequested.store(false);
    m_running.store(true);
    m_lastError.clear();
    m_thread = std::thread(&WasapiLoopbackCapture::threadMain, this, renderDeviceId, std::move(cb));
    return {};
}

void WasapiLoopbackCapture::stop()
{
    if (m_thread.joinable()) {
        m_stopRequested.store(true);
        m_thread.join();
    }
    m_running.store(false);
}

void WasapiLoopbackCapture::threadMain(QString deviceId, Callback cb)
{
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD mmcssTaskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);

    // Scoped work — on any failure we just exit the thread; UI polls isRunning().
    do {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                    CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) break;

        ComPtr<IMMDevice> dev;
        if (FAILED(enumerator->GetDevice(
                reinterpret_cast<LPCWSTR>(deviceId.utf16()), &dev))) break;

        ComPtr<IAudioClient> client;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) break;

        WAVEFORMATEX *mix = nullptr;
        if (FAILED(client->GetMixFormat(&mix)) || !mix) break;

        HRESULT hr = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            kBufferDuration, 0, mix, nullptr);
        if (FAILED(hr)) { CoTaskMemFree(mix); break; }

        UINT32 bufferFrames = 0;
        client->GetBufferSize(&bufferFrames);

        ComPtr<IAudioCaptureClient> capture;
        if (FAILED(client->GetService(IID_PPV_ARGS(&capture)))) { CoTaskMemFree(mix); break; }

        if (FAILED(client->Start())) { CoTaskMemFree(mix); break; }

        std::vector<float> scratch;
        scratch.reserve(bufferFrames * mix->nChannels);

        // Loopback capture has no event handle on the capture client when
        // initialized in shared mode without EVENTCALLBACK — we poll with a
        // short sleep, which is the Microsoft-documented pattern.
        while (!m_stopRequested.load()) {
            UINT32 packetFrames = 0;
            if (FAILED(capture->GetNextPacketSize(&packetFrames))) break;

            if (packetFrames == 0) {
                Sleep(2);
                continue;
            }

            BYTE *data = nullptr;
            DWORD flags = 0;
            UINT32 framesAvailable = 0;
            hr = capture->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                scratch.assign(static_cast<size_t>(framesAvailable) * mix->nChannels, 0.0f);
            } else {
                convertToFloat(data, framesAvailable, mix, scratch);
            }

            cb(scratch.data(),
               static_cast<int>(framesAvailable),
               mix->nChannels,
               static_cast<int>(mix->nSamplesPerSec));

            capture->ReleaseBuffer(framesAvailable);
        }

        client->Stop();
        CoTaskMemFree(mix);
    } while (false);

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    m_running.store(false);
}

} // namespace host
