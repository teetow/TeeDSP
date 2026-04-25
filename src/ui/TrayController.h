#pragma once

#include <QObject>

class QAction;
class QMainWindow;
class QSystemTrayIcon;
class QMenu;

namespace ui {

// Owns the system tray icon and its right-click menu. Hides/shows the main
// window on activation; offers Auto-route, Bypass, Start-with-Windows
// toggles plus a hard Quit.
class TrayController : public QObject
{
    Q_OBJECT

public:
    explicit TrayController(QMainWindow *window, QObject *parent = nullptr);

    void setStatusText(const QString &text);
    void setRunning(bool running);
    void setBypass(bool bypass);
    void setAutoRoute(bool on);
    void setStartWithWindows(bool on);

signals:
    void bypassToggled(bool on);
    void autoRouteToggled(bool on);
    void startWithWindowsToggled(bool on);
    void quitRequested();

private slots:
    void onActivated(int reason);
    void onShowToggle();
    void onBypass(bool);
    void onAutoRoute(bool);
    void onStartWithWindows(bool);
    void onQuit();

private:
    QMainWindow *m_window;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_menu = nullptr;
    QAction *m_showAction = nullptr;
    QAction *m_bypassAction = nullptr;
    QAction *m_autoRouteAction = nullptr;
    QAction *m_startWithWindowsAction = nullptr;
    QAction *m_quitAction = nullptr;
};

} // namespace ui
