#pragma once

#include <QObject>
#include <QPointer>
#include <QSystemTrayIcon>
#include <QString>

class QAction;
class QMenu;

class TrayController : public QObject
{
    Q_OBJECT
public:
    TrayController(QObject *windowObject,
                   QObject *parent = nullptr);
    ~TrayController() override;

    void setConfigWindow(QObject *configWindow);

private slots:
    void handleStartupToggled(bool enabled);
    void rebuildMenu();

private:
    void updateStartupActionChecked();
    bool isStartupSupported() const;
    bool isStartupEnabled() const;
    void setStartupEnabled(bool enabled);
    void showConfigWindow();

    QPointer<QObject> m_windowObject;
    QPointer<QObject> m_configWindow;
    QSystemTrayIcon m_trayIcon;
    QMenu *m_menu = nullptr;
    QAction *m_startupAction = nullptr;
    QAction *m_configureAction = nullptr;
    QString m_iconPath;
};
