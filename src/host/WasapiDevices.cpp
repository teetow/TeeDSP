#include "WasapiDevices.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <wrl/client.h>

#include <QString>

using Microsoft::WRL::ComPtr;

namespace host {

namespace {

// ---------------------------------------------------------------------------
// IPolicyConfig — private COM interface stable across Windows Vista–11.
// Used by many audio utilities (EarTrumpet, SoundSwitch, NirCmd, …) for
// changing the default audio endpoint. Not in any public SDK header; the
// vtable layout below matches the documented community reverse-engineering.
// ---------------------------------------------------------------------------
struct DeviceShareMode { int a; };   // opaque placeholder — unused here

MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, BOOL, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX *, WAVEFORMATEX *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, BOOL, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, DeviceShareMode *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, DeviceShareMode *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, BOOL) = 0;
};

static const CLSID CLSID_PolicyConfigClient =
    {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
// ---------------------------------------------------------------------------

struct CoInitScope {
    HRESULT hr;
    CoInitScope() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~CoInitScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

QString wideToQString(const wchar_t *w)
{
    return QString::fromWCharArray(w);
}

bool looksVirtual(const QString &friendlyName, const QString &interfaceName)
{
    // Match against known virtual-cable vendors. These are the products people
    // route system audio through; none are physical outputs you'd want in an
    // auto-route fallback chain.
    static const char *kPatterns[] = {
        "VB-Audio", "VB-Cable", "CABLE Input", "CABLE Output",
        "VoiceMeeter", "Voicemeeter",
        "Hi-Fi Cable", "HiFi Cable",
        "Virtual Cable", "Virtual Audio Cable",
        nullptr,
    };
    for (const char **p = kPatterns; *p; ++p) {
        if (friendlyName.contains(QString::fromUtf8(*p), Qt::CaseInsensitive))  return true;
        if (interfaceName.contains(QString::fromUtf8(*p), Qt::CaseInsensitive)) return true;
    }
    return false;
}

bool waveFormatToStreamFormat(const WAVEFORMATEX *wf, StreamFormat &out)
{
    if (!wf) return false;
    out.sampleRate = static_cast<int>(wf->nSamplesPerSec);
    out.channels = wf->nChannels;
    out.bitsPerSample = wf->wBitsPerSample;

    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        out.isFloat = true;
        return true;
    }
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && wf->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(wf);
        out.isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        return true;
    }
    if (wf->wFormatTag == WAVE_FORMAT_PCM) {
        out.isFloat = false;
        return true;
    }
    return false;
}

} // namespace

QList<DeviceInfo> WasapiDevices::enumerateRender()
{
    QList<DeviceInfo> out;
    CoInitScope co;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return out;

    QString defaultId;
    {
        ComPtr<IMMDevice> defDev;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defDev))) {
            LPWSTR pid = nullptr;
            if (SUCCEEDED(defDev->GetId(&pid))) {
                defaultId = wideToQString(pid);
                CoTaskMemFree(pid);
            }
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)))
        return out;

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(collection->Item(i, &dev))) continue;

        DeviceInfo info;
        LPWSTR pid = nullptr;
        if (SUCCEEDED(dev->GetId(&pid))) {
            info.id = wideToQString(pid);
            CoTaskMemFree(pid);
        }

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                info.name = wideToQString(pv.pwszVal);
            }
            PropVariantClear(&pv);

            PROPVARIANT pv2;
            PropVariantInit(&pv2);
            if (SUCCEEDED(props->GetValue(PKEY_DeviceInterface_FriendlyName, &pv2)) && pv2.vt == VT_LPWSTR) {
                info.interfaceName = wideToQString(pv2.pwszVal);
            }
            PropVariantClear(&pv2);
        }

        DWORD state = 0;
        if (SUCCEEDED(dev->GetState(&state))) {
            info.isActive = (state & DEVICE_STATE_ACTIVE) != 0;
        }

        info.isVirtual = looksVirtual(info.name, info.interfaceName);
        info.isDefault = (info.id == defaultId);
        out.append(std::move(info));
    }

    return out;
}

QString WasapiDevices::defaultRenderId()
{
    CoInitScope co;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return {};
    ComPtr<IMMDevice> dev;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev)))
        return {};
    LPWSTR pid = nullptr;
    if (FAILED(dev->GetId(&pid))) return {};
    QString id = wideToQString(pid);
    CoTaskMemFree(pid);
    return id;
}

bool WasapiDevices::queryMixFormat(const QString &deviceId, StreamFormat &out)
{
    CoInitScope co;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return false;

    ComPtr<IMMDevice> dev;
    if (FAILED(enumerator->GetDevice(reinterpret_cast<LPCWSTR>(deviceId.utf16()), &dev)))
        return false;

    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
        return false;

    WAVEFORMATEX *mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || !mix)
        return false;

    const bool ok = waveFormatToStreamFormat(mix, out);
    CoTaskMemFree(mix);
    return ok;
}

bool WasapiDevices::setDefaultRender(const QString &deviceId)
{
    if (deviceId.isEmpty()) return false;
    CoInitScope co;

    ComPtr<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&policy))) || !policy)
        return false;

    const std::wstring wId = deviceId.toStdWString();
    const HRESULT hr = policy->SetDefaultEndpoint(wId.c_str(), eConsole);
    policy->SetDefaultEndpoint(wId.c_str(), eMultimedia);
    policy->SetDefaultEndpoint(wId.c_str(), eCommunications);
    return SUCCEEDED(hr);
}

} // namespace host
