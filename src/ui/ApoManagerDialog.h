#pragma once

#include <QDialog>

class QTableWidget;
class QPushButton;
class QLabel;

namespace ui {

// Read-only view of TeeDSP's APO install/binding state: which driver
// packages pnputil currently has published in the Driver Store, and which
// live output endpoints currently have the APO bound (and in which FX slot).
// Install/uninstall stays a scripted operation (scripts\deploy-apo.ps1, see
// apo/driver/README.md) — this dialog only reports on that state.
class ApoManagerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ApoManagerDialog(QWidget *parent = nullptr);

private:
    void refresh();

    QTableWidget *m_packagesTable = nullptr;
    QTableWidget *m_bindingsTable = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_restartEngineButton = nullptr;
    QLabel *m_actionStatusLabel = nullptr;
};

} // namespace ui
