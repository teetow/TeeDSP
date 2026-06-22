#pragma once

namespace ui::recovery {

// Restarts the Windows Audio service (Audiosrv) via a one-shot elevated
// PowerShell, triggering a UAC prompt. This forces audiodg to respawn and
// re-read DisableProtectedAudioDG, which reloads the (dev-signed) TeeDSP APO
// after a protected-mode relaunch silently unloaded it.
//
// Fire-and-forget: returns true once the elevated process has been launched,
// false if the launch failed or the user declined the UAC prompt. Actual
// recovery is observed asynchronously when the APO telemetry resumes — the
// caller should not block on this.
bool restartAudioService();

} // namespace ui::recovery
