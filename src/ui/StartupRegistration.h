#pragma once

#include <QString>

namespace ui {

// Manages the HKCU\Software\Microsoft\Windows\CurrentVersion\Run entry that
// makes TeeDsp launch with the user's session.
//
// We use the Run registry key rather than a Startup folder shortcut because
// it's atomic, leaves no .lnk artifact behind on uninstall, and survives
// publish-time wipes of %LOCALAPPDATA%\Programs\TeeDsp.
namespace startup {

bool isEnabled();

// Registers the given exe path under the "TeeDsp" Run value. If exePath is
// empty, the current process's executable path is used.
bool setEnabled(bool enabled, const QString &exePath = QString());

} // namespace startup
} // namespace ui
