#include "TrayController.h"

#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QSignalBlocker>
#include <QDebug>
#include <QSettings>
#include <QCoreApplication>
#include <QDir>

TrayController::TrayController(QObject *windowObject,
                               QObject *parent)
    : QObject(parent)
    , m_windowObject(windowObject)
    , m_iconPath(QString())
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qWarning() << "TrayController: system tray not available";
        return;
    }

    m_menu = new QMenu();

    rebuildMenu();

    QIcon icon(m_iconPath);
    if (icon.isNull())
        qWarning() << "TrayController: tray icon not set";

    m_trayIcon.setIcon(icon);
    m_trayIcon.setToolTip(tr("TeeDSP"));
    m_trayIcon.setContextMenu(m_menu);
    m_trayIcon.show();

    connect(&m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::DoubleClick)
                    showConfigWindow();
            });
}

TrayController::~TrayController()
{
    if (m_trayIcon.isVisible())
        m_trayIcon.hide();
    delete m_menu;
    m_menu = nullptr;
}

void TrayController::rebuildMenu()
{
    if (!m_menu)
        return;

    m_menu->clear();

    m_configureAction = m_menu->addAction(tr("Configure DSP"));
    connect(m_configureAction, &QAction::triggered, this, &TrayController::showConfigWindow);

    m_startupAction = m_menu->addAction(tr("Start with Windows"));
    m_startupAction->setCheckable(true);
    connect(m_startupAction, &QAction::toggled,
            this, &TrayController::handleStartupToggled);

    updateStartupActionChecked();

    m_menu->addSeparator();
    QAction *quitAction = m_menu->addAction(tr("Quit"));
    connect(quitAction, &QAction::triggered, []() {
        QCoreApplication::quit();
    });
}

void TrayController::handleStartupToggled(bool enabled)
{
    if (!isStartupSupported()) {
        updateStartupActionChecked();
        return;
    }

    setStartupEnabled(enabled);
    updateStartupActionChecked();
}

void TrayController::updateStartupActionChecked()
{
    if (!m_startupAction)
        return;

    const bool supported = isStartupSupported();
    m_startupAction->setEnabled(supported);

    const bool enabled = supported && isStartupEnabled();
    QSignalBlocker blocker(m_startupAction);
    m_startupAction->setChecked(enabled);
}

bool TrayController::isStartupSupported() const
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool TrayController::isStartupEnabled() const
{
#ifdef Q_OS_WIN
    static const QString runKeyPath = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    QSettings runKey(runKeyPath, QSettings::NativeFormat);
    runKey.sync();
    const auto status = runKey.status();
    if (status != QSettings::NoError) {
        qWarning() << "TrayController: failed to read run key" << runKeyPath << "status" << status;
        return false;
    }
    const QString value = runKey.value(QStringLiteral("TeeDSP")).toString().trimmed();
    const bool enabled = !value.isEmpty();
    qInfo() << "TrayController: startup" << (enabled ? "enabled" : "disabled") << "value" << value;
    return enabled;
#else
    return false;
#endif
}

void TrayController::setStartupEnabled(bool enabled)
{
#ifdef Q_OS_WIN
    static const QString runKeyPath = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    QSettings runKey(runKeyPath, QSettings::NativeFormat);
    if (enabled) {
        const QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        const QString command = QStringLiteral("\"%1\"").arg(exePath);
        runKey.setValue(QStringLiteral("TeeDSP"), command);
    } else {
        runKey.remove(QStringLiteral("TeeDSP"));
    }
    runKey.sync();
    const auto status = runKey.status();
    if (status != QSettings::NoError) {
        qWarning() << "TrayController: failed to" << (enabled ? "write" : "remove")
                   << "run key entry" << runKeyPath << "status" << status;
    } else {
        qInfo() << "TrayController: startup entry" << (enabled ? "set" : "cleared")
                << "->" << runKey.value(QStringLiteral("TeeDSP")).toString();
    }
#else
    Q_UNUSED(enabled);
#endif
}

void TrayController::setConfigWindow(QObject *configWindow)
{
    m_configWindow = configWindow;
}

void TrayController::showConfigWindow()
{
    if (!m_configWindow)
        return;
    m_configWindow->setProperty("visible", true);
    QMetaObject::invokeMethod(m_configWindow, "raise");
    QMetaObject::invokeMethod(m_configWindow, "requestActivate");
}
