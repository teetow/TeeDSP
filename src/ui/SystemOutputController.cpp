#include "SystemOutputController.h"

#include "dsp/ApoSharedClient.h"

#include <QCoreApplication>
#include <QDir>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

SystemOutputController::SystemOutputController(dsp::ApoSharedClient *apoClient,
                                               QObject *parent)
    : QObject(parent)
    , m_apoClient(apoClient)
{
    if (m_apoClient) {
        connect(m_apoClient, &dsp::ApoSharedClient::connectedChanged,
                this, &SystemOutputController::statusChanged);
    }
}

bool SystemOutputController::active() const
{
    return m_apoClient && m_apoClient->isConnected();
}

QString SystemOutputController::statusText() const
{
    return active()
        ? QStringLiteral("DSP active on Windows output")
        : QStringLiteral("System DSP inactive");
}

QString SystemOutputController::detailText() const
{
    if (active()) {
        return QStringLiteral("TeeDSP is applying the APO DSP chain to the default Windows output path.");
    }

    if (!m_actionError.isEmpty()) {
        return QStringLiteral("Activation did not complete. Click the button again and approve the Windows admin prompt.");
    }

    const QDir appDir(QCoreApplication::applicationDirPath());
    QString endpointMarker = appDir.absoluteFilePath(QStringLiteral("../scripts/installed_endpoint.txt"));
    if (!QFileInfo::exists(endpointMarker))
        endpointMarker = appDir.absoluteFilePath(QStringLiteral("scripts/installed_endpoint.txt"));

    if (QFileInfo::exists(endpointMarker)) {
        return QStringLiteral("TeeDSP registered the APO, but Windows still is not loading it on this output. This machine likely needs a driver-packaged APO install.");
    }

    return QStringLiteral("Click Activate system DSP below, then approve the one-time Windows admin prompt.");
}

QString SystemOutputController::errorText() const
{
    if (!m_actionError.isEmpty())
        return m_actionError;
    return {};
}

bool SystemOutputController::installOrRepair()
{
#ifdef Q_OS_WIN
    m_actionError.clear();

    const QDir appDir(QCoreApplication::applicationDirPath());
    QString scriptPath = appDir.absoluteFilePath(QStringLiteral("../scripts/install_apo.ps1"));
    if (!QFileInfo::exists(scriptPath))
        scriptPath = appDir.absoluteFilePath(QStringLiteral("scripts/install_apo.ps1"));

    if (!QFileInfo::exists(scriptPath)) {
        m_actionError = QStringLiteral("Could not find the APO installer script.");
        emit statusChanged();
        return false;
    }

    const QString params = QStringLiteral("-ExecutionPolicy Bypass -File \"%1\"").arg(QDir::toNativeSeparators(scriptPath));
    const auto result = reinterpret_cast<quintptr>(
        ShellExecuteW(nullptr, L"runas", L"powershell.exe",
                      reinterpret_cast<LPCWSTR>(params.utf16()), nullptr, SW_SHOWNORMAL));

    if (result <= 32) {
        m_actionError = QStringLiteral("Windows blocked or cancelled the admin approval prompt.");
        emit statusChanged();
        return false;
    }

    emit statusChanged();
    return true;
#else
    m_actionError = QStringLiteral("System DSP install is only supported on Windows.");
    emit statusChanged();
    return false;
#endif
}
