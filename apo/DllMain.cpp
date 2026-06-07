#include "TeeDspApo.h"
#include "Guids.h"

#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <new>
#include <atomic>

// Single TU defining the GUID.
extern "C" const GUID CLSID_TeeDspApoMfx
    = { 0xB7E1A0C0, 0x7E5D, 0x4D8B, { 0x9E, 0x2A, 0x1C, 0x4F, 0x8D, 0x3A, 0x2B, 0x11 } };

namespace {

std::atomic<LONG> g_objectCount{0};
std::atomic<LONG> g_lockCount{0};
HMODULE g_hModule = nullptr;

class ClassFactory final : public IClassFactory
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return m_ref.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG n = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *pUnkOuter, REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;

        auto *obj = new (std::nothrow) teedsp::apo::TeeDspApo();
        if (!obj) return E_OUTOFMEMORY;

        g_objectCount.fetch_add(1, std::memory_order_relaxed);
        const HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        if (FAILED(hr)) g_objectCount.fetch_sub(1, std::memory_order_relaxed);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override
    {
        if (fLock) g_lockCount.fetch_add(1, std::memory_order_relaxed);
        else       g_lockCount.fetch_sub(1, std::memory_order_relaxed);
        return S_OK;
    }

private:
    std::atomic<LONG> m_ref{1};
};

HRESULT writeRegString(HKEY parent, const wchar_t *subkey, const wchar_t *name, const wchar_t *value)
{
    HKEY h = nullptr;
    LONG rc = RegCreateKeyExW(parent, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    const DWORD cb = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
    rc = RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value), cb);
    RegCloseKey(h);
    return rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(rc);
}

HRESULT writeRegDword(HKEY parent, const wchar_t *subkey, const wchar_t *name, DWORD value)
{
    HKEY h = nullptr;
    LONG rc = RegCreateKeyExW(parent, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    rc = RegSetValueExW(h, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
    RegCloseKey(h);
    return rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(rc);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_TeeDspApoMfx) return CLASS_E_CLASSNOTAVAILABLE;

    auto *cf = new (std::nothrow) ClassFactory();
    if (!cf) return E_OUTOFMEMORY;
    const HRESULT hr = cf->QueryInterface(riid, ppv);
    cf->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllCanUnloadNow()
{
    return (g_objectCount.load(std::memory_order_acquire) == 0
            && g_lockCount.load(std::memory_order_acquire) == 0) ? S_OK : S_FALSE;
}

extern "C" HRESULT __stdcall DllRegisterServer()
{
    wchar_t modulePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_hModule, modulePath, MAX_PATH))
        return HRESULT_FROM_WIN32(GetLastError());

    constexpr wchar_t kClsid[] = L"{B7E1A0C0-7E5D-4D8B-9E2A-1C4F8D3A2B11}";
    wchar_t key[256];

    swprintf_s(key, L"CLSID\\%s", kClsid);
    HRESULT hr = writeRegString(HKEY_CLASSES_ROOT, key, nullptr, L"TeeDSP Mode Effect APO");
    if (FAILED(hr)) return hr;

    swprintf_s(key, L"CLSID\\%s\\InprocServer32", kClsid);
    hr = writeRegString(HKEY_CLASSES_ROOT, key, nullptr, modulePath);
    if (FAILED(hr)) return hr;

    hr = writeRegString(HKEY_CLASSES_ROOT, key, L"ThreadingModel", L"Both");
    if (FAILED(hr)) return hr;

    // Register the APO metadata so the audio engine will discover and accept
    // us. The engine reads this whole property set; a bare friendly name is not
    // enough. Mirrors the APO_REG_PROPERTIES returned by GetRegistrationProperties.
    // The APOInterface list advertises IAudioSystemEffects — the interface the
    // engine QIs for to treat the CLSID as a system-effects APO.
    swprintf_s(key, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AudioEngine\\AudioProcessingObjects\\%s", kClsid);
    hr = writeRegString(HKEY_LOCAL_MACHINE, key, nullptr, L"TeeDSP Mode Effect APO");
    if (FAILED(hr)) return hr;
    hr = writeRegString(HKEY_LOCAL_MACHINE, key, L"FriendlyName", L"TeeDSP Mode Effect");
    if (FAILED(hr)) return hr;
    hr = writeRegString(HKEY_LOCAL_MACHINE, key, L"Copyright", L"TeeDSP");
    if (FAILED(hr)) return hr;

    const DWORD apoFlags = static_cast<DWORD>(APO_FLAG_SAMPLESPERFRAME_MUST_MATCH
                                              | APO_FLAG_FRAMESPERSECOND_MUST_MATCH
                                              | APO_FLAG_BITSPERSAMPLE_MUST_MATCH);
    struct { const wchar_t *name; DWORD value; } dwords[] = {
        { L"MajorVersion",          1 },
        { L"MinorVersion",          0 },
        { L"Flags",                 apoFlags },
        { L"MinInputConnections",   1 },
        { L"MaxInputConnections",   1 },
        { L"MinOutputConnections",  1 },
        { L"MaxOutputConnections",  1 },
        { L"MaxInstances",          0xFFFFFFFFul },
        { L"NumAPOInterfaces",      1 },
    };
    for (const auto &d : dwords) {
        hr = writeRegDword(HKEY_LOCAL_MACHINE, key, d.name, d.value);
        if (FAILED(hr)) return hr;
    }

    // APOInterface0 = IID_IAudioSystemEffects {FD7F2B29-24D0-4B5C-B177-592C39F9CA10}
    hr = writeRegString(HKEY_LOCAL_MACHINE, key, L"APOInterface0",
                        L"{FD7F2B29-24D0-4B5C-B177-592C39F9CA10}");
    return hr;
}

extern "C" HRESULT __stdcall DllUnregisterServer()
{
    constexpr wchar_t kClsid[] = L"{B7E1A0C0-7E5D-4D8B-9E2A-1C4F8D3A2B11}";
    wchar_t key[256];

    swprintf_s(key, L"CLSID\\%s\\InprocServer32", kClsid);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, key);

    swprintf_s(key, L"CLSID\\%s", kClsid);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, key);

    swprintf_s(key, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AudioEngine\\AudioProcessingObjects\\%s", kClsid);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);

    return S_OK;
}
