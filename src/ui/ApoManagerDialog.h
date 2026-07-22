#pragma once

#include <QDialog>
#include <QList>
#include <QStringList>

class QTableWidget;
class QPushButton;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QWidget;
class QVBoxLayout;

namespace ui {

// View + lifecycle control of TeeDSP's APO install/binding state, organized
// around the thing you actually care about -- your devices -- rather than
// raw driver-store plumbing. One card per render endpoint TeeDSP knows how to
// bind: whether it's bound, which FX slot, and an inline action to unbind
// just that device. A top summary line + "Restore Audio" button cover the
// "I already know it's broken" case; raw Driver Store packages and a full
// diagnostics dump both stay collapsed until needed. See ApoDiagnostics.h for
// the latter, ApoLifecycleActions.h for the elevated uninstall/retire path,
// and scripts\deploy-apo.ps1 / uninstall-apo.ps1 for what actually runs.
class ApoManagerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ApoManagerDialog(QWidget *parent = nullptr);

private:
    void refresh();
    void setActionsEnabled(bool enabled);   // false while an operation is in flight
    void appendLog(const QString &text);

    // Shared by every uninstall-shaped action (per-device, per-package-row,
    // remove-all, retire-superseded) -- they differ only in which package
    // names they target, whether they touch live FX bindings, and the
    // status text shown while running.
    void performRemoval(const QStringList &publishedNames, bool skipFxClear,
                         const QString &describedAs);

    void removeAllPackages();
    void redeployApo();
    void retireSuperseded();

    void toggleDiagnostics(bool expanded);
    void refreshDiagnostics();
    void runChurnCheck();

    QLabel *m_summaryLabel = nullptr;
    QPushButton *m_restoreAudioButton = nullptr;
    QLabel *m_loadedBuildLabel = nullptr;

    QWidget *m_deviceCardsContainer = nullptr;
    QVBoxLayout *m_deviceCardsLayout = nullptr;

    QTableWidget *m_packagesTable = nullptr;
    QLabel *m_packagesAnomalyLabel = nullptr;
    QPushButton *m_retireSupersededButton = nullptr;

    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_removeAllButton = nullptr;
    QPushButton *m_redeployButton = nullptr;
    QLabel *m_actionStatusLabel = nullptr;
    QPlainTextEdit *m_opLog = nullptr;

    // Per-device-card and per-package-row buttons, rebuilt every refresh() --
    // tracked so setActionsEnabled() can disable them all during an
    // operation without needing to walk the layout tree.
    QList<QPushButton *> m_dynamicActionButtons;

    QProcess *m_redeployProcess = nullptr;   // owned; only non-null while redeploying
    bool m_operationInFlight = false;

    QPushButton *m_diagToggle = nullptr;
    QWidget *m_diagContainer = nullptr;
    QPlainTextEdit *m_diagText = nullptr;
    QPushButton *m_diagRefreshButton = nullptr;
    QPushButton *m_diagCopyButton = nullptr;
    QPushButton *m_diagChurnButton = nullptr;
    bool m_diagLoadedOnce = false;
};

} // namespace ui
