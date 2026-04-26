#pragma once

#include <QObject>

namespace host {

// Listens for IMMDeviceEnumerator endpoint notifications (added/removed,
// active/disabled, default-changed) and re-emits a single Qt signal on the
// owning thread. The COM callback runs on a Windows audio service thread,
// so we marshal everything via a queued connection.
class WasapiDeviceNotifier : public QObject
{
    Q_OBJECT

public:
    explicit WasapiDeviceNotifier(QObject *parent = nullptr);
    ~WasapiDeviceNotifier() override;

    bool isActive() const { return m_active; }

signals:
    // Anything device-shaped happened. Subscribers should re-enumerate.
    void devicesChanged();

    // Fires only when the system default render endpoint for the eConsole
    // role changes — narrower than devicesChanged. Used by the auto-reinject
    // path so it doesn't trigger on every property edit. Filtered to eConsole
    // because Windows fires this event three times (Console/Multimedia/
    // Communications) on a single user-visible default change.
    void defaultRenderChanged(QString deviceId);

private:
    // PIMPL — the COM callback class needs to live separately so its lifetime
    // is governed by COM's AddRef/Release rather than Qt's parent ownership.
    class CallbackImpl;
    CallbackImpl *m_callback = nullptr;
    bool m_active = false;
};

} // namespace host
