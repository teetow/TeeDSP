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

    // Register as an APO so the audio engine will discover us.
    swprintf_s(key, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AudioEngine\\AudioProcessingObjects\\%s", kClsid);
    hr = writeRegString(HKEY_LOCAL_MACHINE, key, nullptr, L"TeeDSP Mode Effect APO");
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
