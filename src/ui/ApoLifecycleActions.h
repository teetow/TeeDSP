#pragma once

#include <QString>
#include <QStringList>

namespace ui::apolifecycle {

struct RemovePackagesResult {
    bool launched  = false;  // elevation was granted and the process started
    bool succeeded = false;  // process exited 0 (only meaningful if launched)
    QString log;             // scripts\uninstall-apo.ps1's own progress log
};

// Elevated (triggers a UAC prompt): runs scripts\uninstall-apo.ps1 to clear
// TeeDSP's FX CLSID on every render endpoint, delete the given pnputil-
// published driver packages, and restart Audiosrv. Pass skipFxClear=true to
// retire a superseded/duplicate package that isn't actually bound to
// anything, without touching any endpoint's live binding (scripts\
// uninstall-apo.ps1 -SkipFxClear).
//
// Unlike ui::recovery::restartAudioService(), this BLOCKS the calling thread
// until the elevated process exits (or the UAC prompt is declined) so the
// caller can report success/failure — always invoke it off the UI thread.
// launched=false means the prompt was declined or the process could not be
// started; check succeeded only when launched is true.
RemovePackagesResult removeApoPackages(const QString &scriptPath,
                                        const QStringList &publishedNames,
                                        bool skipFxClear = false);

} // namespace ui::apolifecycle
