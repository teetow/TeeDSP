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

} // namespace host
