#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace host {

// Is the TeeDSP APO bound to this render endpoint's FX chain, and in which
// slot? Windows exposes the binding as one of three PKEY_*FX*Clsid values
// under the endpoint's FxProperties key (see apo/driver/README.md): composite
// MFX (,14, Realtek's pattern), plain MFX (,6), or third-party SFX (,5, the
// AirPods A2DP pattern).
struct ApoBindingInfo {
    bool bound = false;
    QString slot; // "MFX (composite)", "MFX", or "SFX" — empty when not bound
};

ApoBindingInfo queryApoBinding(const QString &renderDeviceId);

// Uncollapsed view of the same FxProperties key queryApoBinding() reads:
// the raw CLSID string (or empty) at each of the three known slots, plus the
// raw MFX-modes-supported multi-string. For diagnostics — protects against
// the interpretation in queryApoBinding() itself having a blind spot (it's
// happened before: an earlier script only ever touched the ,6 slot).
struct ApoBindingRaw {
    QString sfxClsid;             // ,5  (PKEY_FX_StreamEffectClsid)
    QString mfxClsid;              // ,6  (PKEY_FX_ModeEffectClsid)
    QString compositeMfxClsid;     // ,14 (PKEY_CompositeFX_ModeEffectClsid)
    QStringList modesSupported;    // MFX_ProcessingModes_Supported_For_Streaming
};

ApoBindingRaw queryApoBindingRaw(const QString &renderDeviceId);

// One row per teedsp*.inf package pnputil currently has published in the
// Driver Store, independent of whether the target device is plugged in right
// now. Discovered dynamically (not matched against a fixed expected set) so a
// stale or unexpected package — e.g. a leftover extension for a device that's
// since been dropped from apo/driver/ — still shows up instead of silently
// going unreported.
struct InstalledApoPackage {
    QString label;           // friendly name for known packages, else the raw inf name
    QString purpose;         // DSP component or device-binding extension
    QString originalInfName; // e.g. "teedsprealtekextension.inf"
    QString publishedName;   // e.g. "oem123.inf"
    QString driverVersion;   // e.g. "07/08/2026 0.1.189.1234"
};

QList<InstalledApoPackage> queryInstalledApoPackages();

} // namespace host
