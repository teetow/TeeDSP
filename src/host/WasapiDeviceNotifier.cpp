#include "WasapiDeviceNotifier.h"

#include <windows.h>
#include <mmdeviceapi.h>

#include <QMetaObject>

namespace host {

class WasapiDeviceNotifier::CallbackImpl : public IMMNotificationClient
{
public:
    explicit CallbackImpl(WasapiDeviceNotifier *owner) : m_owner(owner) {}

    void detach() { m_owner = nullptr; }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(&m_refCount);
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG c = InterlockedDecrement(&m_refCount);
        if (c == 0) delete this;
        return c;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override        { notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override                       { notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override                     { notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR newId) override
    {
        notify();
        // Only the eConsole role for render endpoints — Windows fires this
        // three times for a single user-visible change (one per role); we
        // forward just one to subscribers.
        if (flow == eRender && role == eConsole && m_owner) {
            const QString id = newId ? QString::fromWCharArray(newId) : QString();
            QMetaObject::invokeMethod(m_owner, "defaultRenderChanged",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, id));
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    void notify()
    {
        if (m_owner) {
            QMetaObject::invokeMethod(m_owner, "devicesChanged", Qt::QueuedConnection);
        }
    }

    LONG m_refCount = 1;
    WasapiDeviceNotifier *m_owner;
};

WasapiDeviceNotifier::WasapiDeviceNotifier(QObject *parent)
    : QObject(parent)
{
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) return;

    m_callback = new CallbackImpl(this);
    hr = enumerator->RegisterEndpointNotificationCallback(m_callback);
    if (SUCCEEDED(hr)) {
        m_active = true;
    } else {
        m_callback->Release();
        m_callback = nullptr;
    }
    enumerator->Release();
}

WasapiDeviceNotifier::~WasapiDeviceNotifier()
{
    if (!m_callback) return;

    // Detach the back-pointer so any in-flight callback queued after this
    // point becomes a no-op rather than dereferencing a dead QObject.
    m_callback->detach();

    IMMDeviceEnumerator *enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))
        && enumerator) {
        enumerator->UnregisterEndpointNotificationCallback(m_callback);
        enumerator->Release();
    }

    m_callback->Release();
    m_callback = nullptr;
    m_active = false;
}

} // namespace host
