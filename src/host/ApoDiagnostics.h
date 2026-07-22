#pragma once

#include <QString>

namespace host {

// One self-contained, copy-pasteable plain-text dump of every moving part of
// the APO subsystem this app knows how to inspect: live shared-memory
// telemetry, raw FX registry bindings (all three slots, not just the
// interpreted one), Driver Store packages, the Audiosrv service, and whether
// TeeDspApo.dll is actually mapped into audiodg.exe right now. For debugging
// failure modes nobody's hit yet -- deliberately raw and complete rather than
// curated, unlike the rest of the dialog.
//
// Synchronous: a handful of WinAPI calls plus one process enumeration, all
// sub-100ms in practice. Called directly from the UI thread on user request
// (expanding the Diagnostics section / clicking Refresh), matching how
// queryInstalledApoPackages() already blocks briefly on pnputil.
QString buildDiagnosticsReport();

// Opt-in and slower (an Event Log scan over the full log, typically 1-3s):
// shells out to scripts\Get-AirPodsEndpointHistory.ps1 and returns its
// output verbatim. Left in PowerShell deliberately, not folded into
// buildDiagnosticsReport() -- it's mature Event Log + setupapi.dev.log
// parsing that would be a substantial, riskier reimplementation in C++ for
// no real benefit. scriptPath must be the resolved path to that script.
QString runAirPodsChurnCheck(const QString &scriptPath);

} // namespace host
