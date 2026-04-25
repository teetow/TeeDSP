#include "TrayController.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMainWindow>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QSystemTrayIcon>

namespace ui {

namespace {
QIcon trayIconForState(bool running)
{
    // Generate a tiny circle so we don't need a packaged asset.
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(running ? QColor(0x2E, 0xCC, 0x71) : QColor(0x9A, 0xA0, 0xAE));
        p.drawEllipse(QRectF(4, 4, 24, 24));
        QFont f = p.font();
        f.setPointSize(14);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(0x16, 0x17, 0x1B));
        p.drawText(QRectF(4, 4, 24, 24), Qt::AlignCenter, QStringLiteral("T"));
    }
    return QIcon(pm);
}
} // namespace

TrayController::TrayController(QMainWindow *window, QObject *parent)
    : QObject(parent)
    , m_window(window)
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        // No tray — degrade gracefully; the rest of the app still works.
        return;
    }

    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(trayIconForState(false));
    m_tray->setToolTip(QStringLiteral("TeeDSP"));

    m_menu = new QMenu();

    m_showAction = m_menu->addAction(QStringLiteral("Show TeeDSP"));
    connect(m_showAction, &QAction::triggered, this, &TrayController::onShowToggle);

    m_menu->addSeparator();

    m_bypassAction = m_menu->addAction(QStringLiteral("Bypass DSP"));
    m_bypassAction->setCheckable(true);
    connect(m_bypassAction, &QAction::toggled, this, &TrayController::onBypass);

    m_autoRouteAction = m_menu->addAction(QStringLiteral("Auto-route to physical output"));
    m_autoRouteAction->setCheckable(true);
    connect(m_autoRouteAction, &QAction::toggled, this, &TrayController::onAutoRoute);

    m_startWithWindowsAction = m_menu->addAction(QStringLiteral("Start with Windows"));
    m_startWithWindowsAction->setCheckable(true);
    connect(m_startWithWindowsAction, &QAction::toggled, this, &TrayController::onStartWithWindows);

    m_menu->addSeparator();

    m_quitAction = m_menu->addAction(QStringLiteral("Quit TeeDSP"));
    connect(m_quitAction, &QAction::triggered, this, &TrayController::onQuit);

    m_tray->setContextMenu(m_menu);
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason r) { onActivated(static_cast<int>(r)); });

    m_tray->show();
}

void TrayController::setStatusText(const QString &text)
{
    if (m_tray) m_tray->setToolTip(text);
}

void TrayController::setRunning(bool running)
{
    if (m_tray) m_tray->setIcon(trayIconForState(running));
}

void TrayController::setBypass(bool bypass)
{
    if (m_bypassAction) {
        QSignalBlocker block(m_bypassAction);
        m_bypassAction->setChecked(bypass);
    }
}

void TrayController::setAutoRoute(bool on)
{
    if (m_autoRouteAction) {
        QSignalBlocker block(m_autoRouteAction);
        m_autoRouteAction->setChecked(on);
    }
}

void TrayController::setStartWithWindows(bool on)
{
    if (m_startWithWindowsAction) {
        QSignalBlocker block(m_startWithWindowsAction);
        m_startWithWindowsAction->setChecked(on);
    }
}

void TrayController::onActivated(int reason)
{
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        onShowToggle();
    }
}

void TrayController::onShowToggle()
{
    if (!m_window) return;
    if (m_window->isVisible() && !m_window->isMinimized()) {
        m_window->hide();
    } else {
        m_window->showNormal();
        m_window->raise();
        m_window->activateWindow();
    }
}

void TrayController::onBypass(bool b)               { emit bypassToggled(b); }
void TrayController::onAutoRoute(bool b)            { emit autoRouteToggled(b); }
void TrayController::onStartWithWindows(bool b)     { emit startWithWindowsToggled(b); }
void TrayController::onQuit()                       { emit quitRequested(); }

} // namespace ui
