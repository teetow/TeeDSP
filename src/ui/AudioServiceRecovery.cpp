#include "AudioServiceRecovery.h"

#include <windows.h>
#include <shellapi.h>

#include <QByteArray>
#include <QString>

namespace ui::recovery {

namespace {

bool launchElevatedPowerShell(const QString &script)
{
    const QByteArray utf16(reinterpret_cast<const char *>(script.utf16()),
                           script.size() * static_cast<int>(sizeof(char16_t)));
    const QString params = QStringLiteral(
        "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand %1")
                               .arg(QString::fromLatin1(utf16.toBase64()));

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb       = L"runas";
    sei.lpFile       = L"powershell.exe";
    sei.lpParameters = reinterpret_cast<LPCWSTR>(params.utf16());
    sei.nShow        = SW_HIDE;

    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}

} // namespace

bool restartAudioService()
{
    // A Medium-integrity UI can't restart Audiosrv directly (OpenService fails
    // with access-denied), so elevate a one-shot PowerShell that does it.
    // -Force pulls any dependent services through the restart; the hidden
    // window keeps it silent. We don't wait for completion — MainWindow's
    // status poll detects recovery when the APO telemetry starts advancing.
    return launchElevatedPowerShell(QStringLiteral("Restart-Service Audiosrv -Force"));
}

} // namespace ui::recovery
