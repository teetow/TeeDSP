#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioenginebaseapo.h>

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

const CLSID kTeeDspApoMfx
    = { 0xB7E1A0C0, 0x7E5D, 0x4D8B, { 0x9E, 0x2A, 0x1C, 0x4F, 0x8D, 0x3A, 0x2B, 0x11 } };

bool isFloat32(const WAVEFORMATEX *format)
{
    if (!format || format->wBitsPerSample != 32) return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE
        || format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return false;
    }
    const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
    return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

int fail(const wchar_t *stage, HRESULT hr)
{
    std::fwprintf(stderr, L"%ls failed: 0x%08X\n", stage, static_cast<unsigned>(hr));
    return 1;
}

// The probe only needs GetAudioFormat(), but the APO takes an IAudioMediaType.
// Keep a tiny local implementation here so the diagnostic remains independent
// of the optional ATL-based audiomediatype CRT library.
class TestAudioMediaType final : public IAudioMediaType {
public:
    TestAudioMediaType(const WAVEFORMATEX *format, UINT32 formatSize)
        : m_bytes(formatSize)
    {
        std::memcpy(m_bytes.data(), format, formatSize);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid != IID_IUnknown && riid != __uuidof(IAudioMediaType))
            return E_NOINTERFACE;
        *ppv = static_cast<IAudioMediaType *>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (refs == 0) delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE IsCompressedFormat(BOOL *compressed) override
    {
        if (!compressed) return E_POINTER;
        *compressed = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE IsEqual(IAudioMediaType *, DWORD *flags) override
    {
        if (!flags) return E_POINTER;
        *flags = 0;
        return S_FALSE;
    }

    const WAVEFORMATEX *STDMETHODCALLTYPE GetAudioFormat() override
    {
        return reinterpret_cast<const WAVEFORMATEX *>(m_bytes.data());
    }

    HRESULT STDMETHODCALLTYPE GetUncompressedAudioFormat(UNCOMPRESSEDAUDIOFORMAT *) override
    {
        return E_NOTIMPL;
    }

private:
    std::atomic<ULONG> m_refs{1};
    std::vector<BYTE>  m_bytes;
};

void printFormat(const WAVEFORMATEX *format)
{
    if (!format) return;
    std::wprintf(L"Mix format: tag=0x%04X, %u Hz, %u-bit, %u ch, align=%u, cbSize=%u\n",
                 format->wFormatTag, format->nSamplesPerSec, format->wBitsPerSample,
                 format->nChannels, format->nBlockAlign, format->cbSize);

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        wchar_t subformat[64] = {};
        StringFromGUID2(ext->SubFormat, subformat, static_cast<int>(std::size(subformat)));
        std::wprintf(L"  valid bits=%u, channel mask=0x%08X, subformat=%ls\n",
                     ext->Samples.wValidBitsPerSample, ext->dwChannelMask, subformat);
    }
}

int checkApoFormat(IAudioProcessingObject *apo, const WAVEFORMATEX *format)
{
    const UINT32 formatSize = sizeof(WAVEFORMATEX) + format->cbSize;
    ComPtr<TestAudioMediaType> mediaType;
    mediaType.Attach(new (std::nothrow) TestAudioMediaType(format, formatSize));
    if (!mediaType) return fail(L"TestAudioMediaType", E_OUTOFMEMORY);

    ComPtr<IAudioMediaType> supported;
    HRESULT hr = apo->IsInputFormatSupported(mediaType.Get(), mediaType.Get(), supported.GetAddressOf());
    std::wprintf(L"APO IsInputFormatSupported:  0x%08X\n", static_cast<unsigned>(hr));
    if (FAILED(hr)) return 1;

    supported.Reset();
    hr = apo->IsOutputFormatSupported(mediaType.Get(), mediaType.Get(), supported.GetAddressOf());
    std::wprintf(L"APO IsOutputFormatSupported: 0x%08X\n", static_cast<unsigned>(hr));
    if (FAILED(hr)) return 1;

    ComPtr<IAudioProcessingObjectConfiguration> configuration;
    hr = apo->QueryInterface(IID_PPV_ARGS(configuration.GetAddressOf()));
    if (FAILED(hr)) return fail(L"QueryInterface(IAudioProcessingObjectConfiguration)", hr);

    APO_CONNECTION_DESCRIPTOR input{};
    input.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL;
    input.u32MaxFrameCount = 480;
    input.pFormat = mediaType.Get();
    input.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;
    APO_CONNECTION_DESCRIPTOR output = input;
    APO_CONNECTION_DESCRIPTOR *inputs[] = { &input };
    APO_CONNECTION_DESCRIPTOR *outputs[] = { &output };

    hr = configuration->LockForProcess(1, inputs, 1, outputs);
    std::wprintf(L"APO LockForProcess:          0x%08X\n", static_cast<unsigned>(hr));
    if (FAILED(hr)) return 1;
    configuration->UnlockForProcess();
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2) {
        std::fwprintf(stderr, L"Usage: TeeDspApoProbe <render-endpoint-id>\n");
        return 2;
    }

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr)) return fail(L"CoInitializeEx", coHr);

    int result = 1;
    WAVEFORMATEX *format = nullptr;
    do {
        // Confirm that the installed COM class reports the registration
        // contract we compiled, independently of audiodg's protected loader.
        ComPtr<IAudioProcessingObject> apo;
        HRESULT hr = CoCreateInstance(kTeeDspApoMfx, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&apo));
        if (FAILED(hr)) { result = fail(L"CoCreateInstance(TeeDSP APO)", hr); break; }

        APO_REG_PROPERTIES *properties = nullptr;
        hr = apo->GetRegistrationProperties(&properties);
        if (FAILED(hr)) { result = fail(L"GetRegistrationProperties", hr); break; }
        wchar_t primaryIid[64] = {};
        StringFromGUID2(properties->iidAPOInterfaceList[0], primaryIid,
                        static_cast<int>(std::size(primaryIid)));
        std::wprintf(L"APO primary interface: %ls\n", primaryIid);
        CoTaskMemFree(properties);

        ComPtr<IMMDeviceEnumerator> enumerator;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) { result = fail(L"CoCreateInstance", hr); break; }

        ComPtr<IMMDevice> device;
        hr = enumerator->GetDevice(argv[1], &device);
        if (FAILED(hr)) { result = fail(L"GetDevice", hr); break; }

        ComPtr<IAudioClient> client;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(client.GetAddressOf()));
        if (FAILED(hr)) { result = fail(L"Activate", hr); break; }

        hr = client->GetMixFormat(&format);
        if (FAILED(hr)) { result = fail(L"GetMixFormat", hr); break; }
        printFormat(format);

        hr = apo->Initialize(0, nullptr);
        if (FAILED(hr)) { result = fail(L"APO Initialize", hr); break; }
        if (checkApoFormat(apo.Get(), format) != 0) {
            result = 1;
            break;
        }

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200'000, 0, format, nullptr);
        if (FAILED(hr)) { result = fail(L"Initialize", hr); break; }

        UINT32 bufferFrames = 0;
        hr = client->GetBufferSize(&bufferFrames);
        if (FAILED(hr)) { result = fail(L"GetBufferSize", hr); break; }

        ComPtr<IAudioRenderClient> render;
        hr = client->GetService(IID_PPV_ARGS(&render));
        if (FAILED(hr)) { result = fail(L"GetService", hr); break; }

        const bool float32 = isFloat32(format);
        const bool pcm16 = format->wFormatTag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16;
        if (!float32 && !pcm16) {
            std::fwprintf(stderr, L"Unsupported mix format: tag %u, %u bits\n",
                          format->wFormatTag, format->wBitsPerSample);
            result = 1;
            break;
        }

        hr = client->Start();
        if (FAILED(hr)) { result = fail(L"Start", hr); break; }

        const auto end = GetTickCount64() + 2000;
        double phase = 0.0;
        const double phaseStep = 2.0 * std::acos(-1.0) * 440.0 / format->nSamplesPerSec;
        while (GetTickCount64() < end) {
            UINT32 padding = 0;
            hr = client->GetCurrentPadding(&padding);
            if (FAILED(hr)) { result = fail(L"GetCurrentPadding", hr); break; }

            const UINT32 frames = bufferFrames - padding;
            if (frames != 0) {
                BYTE *data = nullptr;
                hr = render->GetBuffer(frames, &data);
                if (FAILED(hr)) { result = fail(L"GetBuffer", hr); break; }

                for (UINT32 frame = 0; frame < frames; ++frame) {
                    const float sample = 0.05f * static_cast<float>(std::sin(phase));
                    phase += phaseStep;
                    if (phase >= 2.0 * std::acos(-1.0)) phase -= 2.0 * std::acos(-1.0);
                    for (WORD channel = 0; channel < format->nChannels; ++channel) {
                        const size_t index = static_cast<size_t>(frame) * format->nChannels + channel;
                        if (float32) reinterpret_cast<float *>(data)[index] = sample;
                        else reinterpret_cast<int16_t *>(data)[index] = static_cast<int16_t>(sample * 32767.0f);
                    }
                }
                render->ReleaseBuffer(frames, 0);
            }
            Sleep(5);
        }

        client->Stop();
        if (SUCCEEDED(hr)) {
            std::wprintf(L"Rendered a 2-second probe tone.\n");
            result = 0;
        }
    } while (false);

    if (format) CoTaskMemFree(format);
    CoUninitialize();
    return result;
}
