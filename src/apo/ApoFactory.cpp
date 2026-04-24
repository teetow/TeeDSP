#include "TeeDspApo.h"

#include <new>
#include <strsafe.h>

void ApoTrace(const wchar_t *message)
{
    // Write to two locations: dist dir (if writable) AND a hardcoded system temp path
    // so audiodg (LOCAL SERVICE) traces are never lost.
    const wchar_t *kFallback = L"C:\\Windows\\Temp\\TeeDspApo_load.log";

    wchar_t distPath[MAX_PATH]{};
    bool hasDistPath = false;
    if (g_hModule && GetModuleFileNameW(g_hModule, distPath, MAX_PATH) > 0) {
        wchar_t *lastSlash = wcsrchr(distPath, L'\\');
        if (lastSlash) {
            *(lastSlash + 1) = L'\0';
            StringCchCatW(distPath, MAX_PATH, L"TeeDspApo_load.log");
            hasDistPath = true;
        }
    }

    wchar_t line[512]{};
    StringCchPrintfW(line, 512, L"pid=%lu %s\r\n", GetCurrentProcessId(), message ? message : L"(null)");
    DWORD bytes = static_cast<DWORD>(wcslen(line) * sizeof(wchar_t));

    auto writeLog = [&](const wchar_t *filePath) {
        HANDLE h = CreateFileW(filePath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, line, bytes, &written, nullptr);
            CloseHandle(h);
        }
    };

    if (hasDistPath) writeLog(distPath);
    writeLog(kFallback);
}

long   g_lockCount = 0;
HMODULE g_hModule  = nullptr;

BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        ApoTrace(L"DllMain: process attach");
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Class factory
// ---------------------------------------------------------------------------
class TeeDspApoFactory final : public IClassFactory
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
            *ppv = static_cast<IClassFactory *>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override  { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP CreateInstance(IUnknown *pOuter, REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;

        wchar_t riidStr[40]{};
        StringFromGUID2(riid, riidStr, 40);
        wchar_t msg[160]{};
        StringCchPrintfW(msg, 160, L"ClassFactory::CreateInstance pOuter=%p riid=%s", (void*)pOuter, riidStr);
        ApoTrace(msg);

        // COM aggregation support.
        //
        // Windows 11's audio engine wraps MFX APOs in an outer proxy and
        // creates them with aggregation: pOuter != nullptr, riid = IID_IUnknown.
        // If we refuse aggregation, the proxy silently drops us from the
        // graph — no Initialize(), no LockForProcess(), no APOProcess().
        //
        // The canonical aggregation contract (see MSDN "Aggregation") is:
        //   * When pOuter != null, the caller MUST ask for IID_IUnknown only.
        //   * We must return a "non-delegating" IUnknown whose AddRef/Release
        //     manage OUR refcount independently of the outer.
        //   * Our other interfaces' AddRef/Release would normally forward to
        //     the outer — but we don't implement interface-level delegation,
        //     because the audio proxy always Releases us through the same
        //     inner IUnknown it got from this call, so our simple refcount
        //     is sufficient in practice.  (This matches what several shipping
        //     APOs including Realtek's own effect modules do.)
        if (pOuter != nullptr) {
            if (!IsEqualIID(riid, IID_IUnknown)) {
                ApoTrace(L"ClassFactory::CreateInstance: aggregation but riid != IID_IUnknown -> E_INVALIDARG");
                return CLASS_E_NOAGGREGATION;
            }
            ApoTrace(L"ClassFactory::CreateInstance: accepting aggregation, returning inner IUnknown");
            // Fall through to normal construction; TeeDspApo::CreateInstance
            // will QI for IID_IUnknown which returns *our* IUnknown (the one
            // the outer will use to tear us down at the end).
        }

        return TeeDspApo::CreateInstance(pOuter, riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL lock) override
    {
        if (lock) InterlockedIncrement(&g_lockCount);
        else      InterlockedDecrement(&g_lockCount);
        return S_OK;
    }
};

static TeeDspApoFactory g_factory;

// ---------------------------------------------------------------------------
// COM DLL exports
// ---------------------------------------------------------------------------
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;

    wchar_t clsidStr[40]{}, riidStr[40]{};
    StringFromGUID2(rclsid, clsidStr, 40);
    StringFromGUID2(riid, riidStr, 40);
    wchar_t msg[128]{};
    StringCchPrintfW(msg, 128, L"DllGetClassObject clsid=%s riid=%s", clsidStr, riidStr);
    ApoTrace(msg);

    if (!IsEqualCLSID(rclsid, CLSID_TeeDspApo)) {
        ApoTrace(L"DllGetClassObject: wrong CLSID -> CLASS_E_CLASSNOTAVAILABLE");
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    HRESULT hr = g_factory.QueryInterface(riid, ppv);
    if (FAILED(hr)) {
        wchar_t hrMsg[64]{};
        StringCchPrintfW(hrMsg, 64, L"DllGetClassObject: factory QI failed hr=0x%08X", (unsigned)hr);
        ApoTrace(hrMsg);
    }
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return (g_lockCount == 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    ApoTrace(L"DllRegisterServer called");
    if (!g_hModule) return E_UNEXPECTED;

    wchar_t dllPath[MAX_PATH];
    GetModuleFileNameW(g_hModule, dllPath, MAX_PATH);

    // Register the CLSID in HKLM so audiodg.exe (running as SYSTEM) can find it.
    wchar_t keyPath[256];
    wsprintfW(keyPath,
              L"SOFTWARE\\Classes\\CLSID\\{B3A4C5D6-E7F8-4A1B-9C2D-3E4F5A6B7C8D}\\InprocServer32");

    HKEY hKey = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, nullptr,
                                  REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                  &hKey, nullptr);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);

    RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE *>(dllPath),
                   static_cast<DWORD>((wcslen(dllPath) + 1) * sizeof(wchar_t)));
    RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ,
                   reinterpret_cast<const BYTE *>(L"Both"), 10);
    RegCloseKey(hKey);

    // Friendly name key
    wchar_t nameKey[256];
    wsprintfW(nameKey,
              L"SOFTWARE\\Classes\\CLSID\\{B3A4C5D6-E7F8-4A1B-9C2D-3E4F5A6B7C8D}");
    HKEY hNameKey = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, nameKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                        &hNameKey, nullptr) == ERROR_SUCCESS) {
        const wchar_t kName[] = L"TeeDSP APO";
        RegSetValueExW(hNameKey, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(kName),
                       static_cast<DWORD>(sizeof(kName)));
        RegCloseKey(hNameKey);
    }

    // Register the APO with the audio engine so endpoint FxProperties can resolve it.
    IAudioProcessingObject *apo = nullptr;
    APO_REG_PROPERTIES *props = nullptr;
    HRESULT hr = TeeDspApo::CreateInstance(nullptr, __uuidof(IAudioProcessingObject),
                                              reinterpret_cast<void **>(&apo));
    if (FAILED(hr))
        return hr;

    hr = apo->GetRegistrationProperties(&props);
    apo->Release();
    if (FAILED(hr))
        return hr;

    hr = RegisterAPO(props);
    CoTaskMemFree(props);
    return hr;
}

STDAPI DllUnregisterServer()
{
    UnregisterAPO(CLSID_TeeDspApo);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Classes\\CLSID\\{B3A4C5D6-E7F8-4A1B-9C2D-3E4F5A6B7C8D}");
    return S_OK;
}
