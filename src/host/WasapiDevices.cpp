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

} // namespace host
