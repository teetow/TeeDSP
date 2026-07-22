#pragma once

#include <QDialog>
#include <QStringList>

class QTableWidget;
class QPushButton;
class QLabel;
class QPlainTextEdit;
class QProcess;

namespace ui {

// View + lifecycle control of TeeDSP's APO install/binding state: which
// driver packages pnputil currently has published in the Driver Store, which
// live output endpoints currently have the APO bound (and in which FX slot),
// and which build the currently-running APO instance was actually compiled
// from. Also drives the three lifecycle actions: uninstall a selected
// package, remove the APO entirely, and redeploy it from source (rebuild +
// repackage + reinstall — see scripts\deploy-apo.ps1 / uninstall-apo.ps1).
class ApoManagerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ApoManagerDialog(QWidget *parent = nullptr);

private:
    void refresh();
    void setActionsEnabled(bool enabled);   // false while an operation is in flight
    void appendLog(const QString &text);

    void uninstallSelected();
    void removeAllPackages();
    void redeployApo();

    QLabel *m_loadedBuildLabel = nullptr;
    QTableWidget *m_packagesTable = nullptr;
    QTableWidget *m_bindingsTable = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_uninstallSelectedButton = nullptr;
    QPushButton *m_removeAllButton = nullptr;
    QPushButton *m_redeployButton = nullptr;
    QPushButton *m_restartEngineButton = nullptr;
    QLabel *m_actionStatusLabel = nullptr;
    QPlainTextEdit *m_opLog = nullptr;

    QProcess *m_redeployProcess = nullptr;   // owned; only non-null while redeploying
    bool m_operationInFlight = false;
};

} // namespace ui
