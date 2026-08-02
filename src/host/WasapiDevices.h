#pragma once

#include <QList>
#include <QString>

namespace host {

struct DeviceInfo {
    QString id;            // IMMDevice GetId() — e.g. "{0.0.0.00000000}.{...guid...}"
    QString name;          // Friendly name — "Speakers (Realtek(R) Audio)"
    QString interfaceName; // PKEY_DeviceInterface_FriendlyName — vendor/driver string
    bool    isDefault = false;
    bool    isActive = true;
    // Heuristic: appears to be a virtual loopback cable (VB-Audio, VoiceMeeter,
    // Hi-Fi Cable, etc.) and should be excluded from auto-route fallback.
    bool    isVirtual = false;
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
    // Lists all active render (output) endpoints. Used as render targets and,
    // for physical devices, as optional loopback capture sources.
    static QList<DeviceInfo> enumerateRender();

    // Lists all active capture/input endpoints. Virtual cables expose their
    // readable side here (e.g. "CABLE Output"), while Windows renders into
    // their paired render endpoint (e.g. "CABLE Input").
    static QList<DeviceInfo> enumerateCapture();

    // If inputDeviceId is a capture endpoint backed by a virtual cable, return
    // the paired render endpoint Windows should be switched to. If it is
    // already a render endpoint, return it unchanged for classic loopback.
    static QString routeRenderForInput(const QString &inputDeviceId);

    // Migration helper: if a saved input is an old virtual render-loopback id,
    // return the paired capture endpoint that actually carries the cable audio.
    static QString pairedCaptureForRender(const QString &renderDeviceId);

    // Returns the id of the default render endpoint (empty string on failure).
    static QString defaultRenderId();

    // Fetches the engine mix format for a render or capture endpoint.
    // Used to confirm the capture and render sides can be wired up.
    static bool queryMixFormat(const QString &deviceId, StreamFormat &out);

    // Instantaneous output peak (0..1) on a render endpoint, read straight from
    // the endpoint meter — independent of the TeeDSP APO. Lets the UI tell
    // "engine dead" (audio is playing but the APO isn't processing) apart from
    // "idle" (nothing playing). Returns -1.0 if the meter is unavailable.
    static float endpointPeak(const QString &deviceId);

    // Sets the Windows default render endpoint for all roles (Console,
    // Multimedia, Communications). Uses the private IPolicyConfig COM interface
    // that every Windows audio utility relies on for this purpose.
    // Returns true on success; on failure the system default is unchanged.
    static bool setDefaultRender(const QString &deviceId);

    // Asks the endpoint whether its own engine mix format is usable in shared
    // mode. In a healthy state the answer is tautologically yes, which makes a
    // "no" a precise, assumption-free detector for the A2DP format wedge
    // (see EndpointFormatGuard.h). Returns the IsFormatSupported HRESULT, or
    // the HRESULT of whichever earlier step failed. mixOut, when non-null,
    // receives the mix format that was tested.
    static long sharedFormatHealth(const QString &deviceId, StreamFormat *mixOut);

    // Confirming probe for the wedge: actually create (but never start) a
    // shared-mode client, which is what an app does and therefore what the user
    // feels. sharedFormatHealth() alone is not sufficient -- it reports the
    // cached format disagreeing, which can happen while streams still open fine,
    // so acting on it alone repairs endpoints that were never broken. Returns
    // the Initialize HRESULT. Costs an engine stream, so call it only after the
    // cheap probe has already failed, never on a routine tick.
    static long sharedInitializeProbe(const QString &deviceId);

    // Drops the endpoint's cached PKEY_AudioEngine_DeviceFormat so Windows
    // re-derives it from what the device actually accepts right now. Chooses no
    // format of its own -- the point is to adopt the device's, which for
    // Bluetooth is the only sane direction since you cannot dictate a rate to a
    // remote sink. Deliberately paired with no SetDeviceFormat counterpart:
    // TeeDSP has no preferred sample rate and must never impose one.
    static bool resetDeviceFormat(const QString &deviceId);
};

} // namespace host
