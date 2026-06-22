#include "AudioServiceRecovery.h"

#include <windows.h>
#include <shellapi.h>

namespace ui::recovery {

bool restartAudioService()
{
    // A Medium-integrity UI can't restart Audiosrv directly (OpenService fails
    // with access-denied), so elevate a one-shot PowerShell that does it.
    // -Force pulls any dependent services through the restart; the hidden
    // window keeps it silent. We don't wait for completion — MainWindow's
    // status poll detects recovery when the APO telemetry starts advancing.
    const wchar_t *params =
        L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden "
        L"-Command \"Restart-Service Audiosrv -Force\"";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb       = L"runas";          // request elevation -> UAC prompt
    sei.lpFile       = L"powershell.exe";
    sei.lpParameters = params;
    sei.nShow        = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        // GetLastError() == ERROR_CANCELLED (1223) when the user dismisses UAC.
        return false;
    }
    if (sei.hProcess)
        CloseHandle(sei.hProcess);
    return true;
}

} // namespace ui::recovery
