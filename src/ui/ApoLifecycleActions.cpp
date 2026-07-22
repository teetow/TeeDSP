#include "ApoLifecycleActions.h"

#include <windows.h>
#include <shellapi.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>

namespace ui::apolifecycle {

namespace {

// Wrap in "…", escaping embedded quotes — same convention PowerShell itself
// expects on a Win32 command line.
QString quoteArg(const QString &arg)
{
    QString escaped = arg;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

} // namespace

RemovePackagesResult removeApoPackages(const QString &scriptPath,
                                        const QStringList &publishedNames)
{
    RemovePackagesResult result;
    if (publishedNames.isEmpty())
        return result;

    const QString logPath = QDir::temp().filePath(
        QStringLiteral("teedsp-uninstall-%1.log")
            .arg(QDateTime::currentMSecsSinceEpoch()));
    QFile::remove(logPath);   // stale leftover from a previous run, if any

    const QString params =
        QStringLiteral("-NoProfile -ExecutionPolicy Bypass -File %1 -PublishedNames %2 -Log %3")
            .arg(quoteArg(scriptPath),
                 quoteArg(publishedNames.join(QLatin1Char(','))),
                 quoteArg(logPath));

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb       = L"runas";   // request elevation -> UAC prompt
    sei.lpFile       = L"powershell.exe";
    const std::wstring paramsW = params.toStdWString();
    sei.lpParameters = paramsW.c_str();
    sei.nShow        = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        // GetLastError() == ERROR_CANCELLED (1223) when the user dismisses UAC.
        return result;
    }
    result.launched = true;

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        result.succeeded = (exitCode == 0);
        CloseHandle(sei.hProcess);
    }

    QFile logFile(logPath);
    if (logFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.log = QString::fromUtf8(logFile.readAll());
        logFile.close();
        QFile::remove(logPath);
    }

    return result;
}

} // namespace ui::apolifecycle
