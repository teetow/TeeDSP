#pragma once

#include <QList>
#include <QString>

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

// One row per teedsp*.inf package pnputil currently has published in the
// Driver Store, independent of whether the target device is plugged in right
// now. Discovered dynamically (not matched against a fixed expected set) so a
// stale or unexpected package — e.g. a leftover extension for a device that's
// since been dropped from apo/driver/ — still shows up instead of silently
// going unreported.
struct InstalledApoPackage {
    QString label;           // friendly name for known packages, else the raw inf name
    QString originalInfName; // e.g. "teedsprealtekextension.inf"
    QString publishedName;   // e.g. "oem123.inf"
    QString driverVersion;   // e.g. "07/08/2026 0.1.189.1234"
};

QList<InstalledApoPackage> queryInstalledApoPackages();

} // namespace host
