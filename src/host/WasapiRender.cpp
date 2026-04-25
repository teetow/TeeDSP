#include "WasapiRender.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#include <wrl/client.h>

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace host {

namespace {

constexpr REFERENCE_TIME kBufferDuration = 200'000; // 20 ms — shared-mode nominal

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

} // namespace

WasapiRender::WasapiRender() = default;

WasapiRender::~WasapiRender() { stop(); }

QString WasapiRender::start(const QString &renderDeviceId)
{
    stop();
    if (renderDeviceId.isEmpty()) return QStringLiteral("no render device selected");

    m_stopRequested.store(false);
    m_running.store(true);

    {
        std::lock_guard<std::mutex> g(m_mutex);
        // 2 seconds of stereo float headroom — dropped if underrun recovery falls behind.
        m_ring.assign(96000 * 2 * 2, 0.0f);
        m_writePos = m_readPos = m_available = 0;
    }

    m_thread = std::thread(&WasapiRender::threadMain, this, renderDeviceId);

    // Briefly wait for the thread to publish channels/sampleRate or fail.
    for (int i = 0; i < 500 && m_channels.load() == 0 && m_running.load(); ++i)
        Sleep(1);

    if (!m_running.load()) return QStringLiteral("failed to initialise render device");
    if (m_channels.load() == 0) return QStringLiteral("render device did not negotiate a format");
    return {};
}

void WasapiRender::stop()
{
    if (m_thread.joinable()) {
        m_stopRequested.store(true);
        m_thread.join();
    }
    m_running.store(false);
    m_channels.store(0);
    m_sampleRate.store(0);
}

void WasapiRender::write(const float *interleaved, int numFrames, int numChannels)
{
    if (!interleaved || numFrames <= 0 || numChannels <= 0) return;

    const int deviceCh = m_channels.load();
    if (deviceCh <= 0) return;

    std::lock_guard<std::mutex> g(m_mutex);
    const size_t cap = m_ring.size();
    if (cap == 0) return;

    // Upmix/downmix: if channel counts match, memcpy. Otherwise average (downmix)
    // or duplicate (upmix) per frame — MVP-grade, enough to not explode when a
    // stereo loopback feeds a 5.1 render endpoint.
    for (int f = 0; f < numFrames; ++f) {
        if (m_available + static_cast<size_t>(deviceCh) > cap) {
            // Drop this frame — produce faster than consumer drains.
            return;
        }
        if (numChannels == deviceCh) {
            for (int c = 0; c < deviceCh; ++c) {
                m_ring[m_writePos] = interleaved[f * numChannels + c];
                m_writePos = (m_writePos + 1) % cap;
            }
        } else if (numChannels < deviceCh) {
            // Upmix: replicate last available source channel into extra device channels.
            for (int c = 0; c < deviceCh; ++c) {
                const int src = std::min(c, numChannels - 1);
                m_ring[m_writePos] = interleaved[f * numChannels + src];
                m_writePos = (m_writePos + 1) % cap;
            }
        } else {
            // Downmix: average down to deviceCh channels.
            // Simple grouping: take numChannels/deviceCh source channels per dest.
            for (int c = 0; c < deviceCh; ++c) {
                const int chunk = numChannels / deviceCh;
                const int start = c * chunk;
                float acc = 0.0f;
                for (int k = 0; k < chunk; ++k)
                    acc += interleaved[f * numChannels + start + k];
                m_ring[m_writePos] = acc / static_cast<float>(chunk ? chunk : 1);
                m_writePos = (m_writePos + 1) % cap;
            }
        }
        m_available += static_cast<size_t>(deviceCh);
    }
}

void WasapiRender::threadMain(QString deviceId)
{
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD mmcssTaskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);

    HANDLE renderEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);

    do {
        if (!renderEvent) break;

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
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            kBufferDuration, 0, mix, nullptr);
        if (FAILED(hr)) { CoTaskMemFree(mix); break; }

        if (FAILED(client->SetEventHandle(renderEvent))) { CoTaskMemFree(mix); break; }

        UINT32 bufferFrames = 0;
        client->GetBufferSize(&bufferFrames);

        ComPtr<IAudioRenderClient> renderClient;
        if (FAILED(client->GetService(IID_PPV_ARGS(&renderClient)))) { CoTaskMemFree(mix); break; }

        const int channels = mix->nChannels;
        const int bytesPerSample = mix->wBitsPerSample / 8;
        const bool floatFormat = isFloatFormat(mix);
        m_channels.store(channels);
        m_sampleRate.store(static_cast<int>(mix->nSamplesPerSec));
        m_isFloat.store(floatFormat);

        // Pre-fill the buffer with silence so the device can start cleanly.
        BYTE *data = nullptr;
        if (SUCCEEDED(renderClient->GetBuffer(bufferFrames, &data))) {
            std::memset(data, 0, static_cast<size_t>(bufferFrames) * mix->nBlockAlign);
            renderClient->ReleaseBuffer(bufferFrames, 0);
        }

        if (FAILED(client->Start())) { CoTaskMemFree(mix); break; }

        while (!m_stopRequested.load()) {
            const DWORD wr = WaitForSingleObject(renderEvent, 200);
            if (wr == WAIT_TIMEOUT) continue;
            if (wr != WAIT_OBJECT_0) break;

            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;
            const UINT32 framesToWrite = bufferFrames - padding;
            if (framesToWrite == 0) continue;

            BYTE *buf = nullptr;
            if (FAILED(renderClient->GetBuffer(framesToWrite, &buf))) break;

            const size_t samplesWanted = static_cast<size_t>(framesToWrite) * channels;
            size_t samplesGiven = 0;
            {
                std::lock_guard<std::mutex> g(m_mutex);
                const size_t cap = m_ring.size();
                const size_t take = std::min(samplesWanted, m_available);
                if (floatFormat && bytesPerSample == 4) {
                    auto *out = reinterpret_cast<float *>(buf);
                    for (size_t i = 0; i < take; ++i) {
                        out[i] = m_ring[m_readPos];
                        m_readPos = (m_readPos + 1) % cap;
                    }
                } else if (!floatFormat && bytesPerSample == 2) {
                    auto *out = reinterpret_cast<int16_t *>(buf);
                    for (size_t i = 0; i < take; ++i) {
                        float v = m_ring[m_readPos];
                        v = std::max(-1.0f, std::min(1.0f, v));
                        out[i] = static_cast<int16_t>(v * 32767.0f);
                        m_readPos = (m_readPos + 1) % cap;
                    }
                } else if (!floatFormat && bytesPerSample == 4) {
                    auto *out = reinterpret_cast<int32_t *>(buf);
                    for (size_t i = 0; i < take; ++i) {
                        float v = m_ring[m_readPos];
                        v = std::max(-1.0f, std::min(1.0f, v));
                        out[i] = static_cast<int32_t>(v * 2147483647.0f);
                        m_readPos = (m_readPos + 1) % cap;
                    }
                } else {
                    // Unsupported container — emit silence and bail fast.
                    std::memset(buf, 0, static_cast<size_t>(framesToWrite) * mix->nBlockAlign);
                }
                m_available -= take;
                samplesGiven = take;
            }

            if (samplesGiven < samplesWanted) {
                // Underrun: fill the remainder with silence in the native format.
                const size_t remain = samplesWanted - samplesGiven;
                BYTE *tail = buf + samplesGiven * bytesPerSample;
                std::memset(tail, 0, remain * bytesPerSample);
            }
            renderClient->ReleaseBuffer(framesToWrite, 0);
        }

        client->Stop();
        CoTaskMemFree(mix);
    } while (false);

    if (renderEvent) CloseHandle(renderEvent);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    m_running.store(false);
}

} // namespace host
