#pragma once

#include <QObject>
#include <QList>
#include <QString>

class QAction;
class QMainWindow;
class QSystemTrayIcon;
class QMenu;

namespace ui {

// Owns the system tray icon and its right-click menu. Hides/shows the main
// window on activation; offers Bypass and Start-with-Windows toggles plus a
// hard Quit.
class TrayController : public QObject
{
    Q_OBJECT

public:
    struct DeviceChoice {
        QString id;
        QString name;
    };

    explicit TrayController(QMainWindow *window, QObject *parent = nullptr);

    void setStatusText(const QString &text);
    void setRunning(bool running);
    void setBypass(bool bypass);
    void setStartWithWindows(bool on);
    void setKeepInjected(bool on);
    void setRoutingOptions(const QList<DeviceChoice> &inputs,
                           const QString &selectedInputId,
                           const QList<DeviceChoice> &outputs,
                           const QString &selectedOutputId);

signals:
    void startStopRequested();
    void bypassToggled(bool on);
    void startWithWindowsToggled(bool on);
    void keepInjectedToggled(bool on);
    void inputDeviceSelected(const QString &id);
    void outputDeviceSelected(const QString &id);
    void quitRequested();

private slots:
    void onActivated(int reason);
    void onShowToggle();
    void onBypass(bool);
    void onStartWithWindows(bool);
    void onKeepInjected(bool);
    void onQuit();

private:
    QMainWindow *m_window;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_menu = nullptr;
    QAction *m_showAction = nullptr;
    QAction *m_startStopAction = nullptr;
    QMenu *m_inputMenu = nullptr;
    QMenu *m_outputMenu = nullptr;
    QAction *m_bypassAction = nullptr;
    QAction *m_startWithWindowsAction = nullptr;
    QAction *m_keepInjectedAction = nullptr;
    QAction *m_quitAction = nullptr;
};

} // namespace ui
