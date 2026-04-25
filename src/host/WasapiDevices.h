#pragma once

#include <QList>
#include <QString>

namespace host {

struct DeviceInfo {
    QString id;       // IMMDevice GetId() — e.g. "{0.0.0.00000000}.{...guid...}"
    QString name;     // Friendly name — "Speakers (Realtek(R) Audio)"
    bool    isDefault = false;
};

struct StreamFormat {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;   // Effective container width
    bool isFloat = false;
};

class WasapiDevices
{
public:
    // Lists all active render (output) endpoints. Used both as render targets
    // AND as loopback capture sources.
    static QList<DeviceInfo> enumerateRender();

    // Returns the id of the default render endpoint (empty string on failure).
    static QString defaultRenderId();

    // Fetches the engine mix format for a render endpoint.
    // Used to confirm the capture and render sides can be wired up.
    static bool queryMixFormat(const QString &deviceId, StreamFormat &out);
};

} // namespace host
