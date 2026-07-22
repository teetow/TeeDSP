#include "ApoManagerDialog.h"

#include "AudioServiceRecovery.h"
#include "ApoLifecycleActions.h"
#include "../host/ApoBindingStatus.h"
#include "../host/ApoSharedClient.h"
#include "../host/WasapiDevices.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <thread>

namespace ui {

namespace {
QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

// scripts\deploy-apo.ps1 and scripts\uninstall-apo.ps1 only exist in the dev
// checkout (TEEDSP_SOURCE_DIR, see CMakeLists.txt) — this app has no other
// notion of "where is my source tree". Returns an empty string, not a path
// that doesn't exist, if the checkout can't be located.
QString repoScriptPath(const char *relativePath)
{
#ifdef TEEDSP_SOURCE_DIR
    const QString path = QDir(QString::fromUtf8(TEEDSP_SOURCE_DIR))
                              .filePath(QString::fromUtf8(relativePath));
    if (QFileInfo::exists(path))
        return path;
#else
    Q_UNUSED(relativePath);
#endif
    return QString();
}
} // namespace

ApoManagerDialog::ApoManagerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Manage APO"));
    resize(820, 560);

    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(QStringLiteral(
        "Every teedsp*.inf package pnputil currently has published in the "
        "Driver Store. Only the APO component contains the DSP DLL; extension "
        "packages merely bind that same component to devices. Their DriverVer "
        "dates are independent package versions, not DLL compile dates. The "
        "tables also show which live output endpoints currently have the APO "
        "bound. Uninstall/redeploy below act on this state directly; each "
        "needs one elevation (UAC prompt)."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_loadedBuildLabel = new QLabel(QStringLiteral("Loaded build: —"), this);
    m_loadedBuildLabel->setProperty("role", "status");
    layout->addWidget(m_loadedBuildLabel);

    layout->addWidget(new QLabel(QStringLiteral("<b>Installed driver packages</b>"), this));
    m_packagesTable = new QTableWidget(0, 4, this);
    m_packagesTable->setHorizontalHeaderLabels(
        {QStringLiteral("Package"), QStringLiteral("Purpose"),
         QStringLiteral("Published name"),
         QStringLiteral("Driver version")});
    m_packagesTable->horizontalHeader()->setStretchLastSection(true);
    m_packagesTable->verticalHeader()->setVisible(false);
    m_packagesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_packagesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_packagesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(m_packagesTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        m_uninstallSelectedButton->setEnabled(
            !m_operationInFlight && m_packagesTable->currentRow() >= 0);
    });
    layout->addWidget(m_packagesTable);

    layout->addWidget(new QLabel(QStringLiteral("<b>Live endpoint bindings</b>"), this));
    m_bindingsTable = new QTableWidget(0, 3, this);
    m_bindingsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Output endpoint"), QStringLiteral("APO bound"), QStringLiteral("Slot")});
    m_bindingsTable->horizontalHeader()->setStretchLastSection(true);
    m_bindingsTable->verticalHeader()->setVisible(false);
    m_bindingsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bindingsTable->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(m_bindingsTable);

    m_actionStatusLabel = new QLabel(this);
    m_actionStatusLabel->setProperty("role", "status");
    m_actionStatusLabel->hide();
    layout->addWidget(m_actionStatusLabel);

    m_opLog = new QPlainTextEdit(this);
    m_opLog->setReadOnly(true);
    m_opLog->setFont(QFont(QStringLiteral("Consolas"), 9));
    m_opLog->setFixedHeight(120);
    m_opLog->hide();
    layout->addWidget(m_opLog);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);

    m_refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
    buttons->addButton(m_refreshButton, QDialogButtonBox::ActionRole);
    connect(m_refreshButton, &QPushButton::clicked, this, &ApoManagerDialog::refresh);

    m_uninstallSelectedButton = new QPushButton(QStringLiteral("Uninstall Selected"), this);
    m_uninstallSelectedButton->setEnabled(false);
    m_uninstallSelectedButton->setToolTip(
        QStringLiteral("Removes the selected package from the Driver Store (pnputil "
                       "/delete-driver) and clears any endpoint FX binding pointing at "
                       "it, then restarts Windows Audio. Requires elevation (a UAC "
                       "prompt appears). If this is the DSP component package, TeeDSP "
                       "stops processing on every endpoint until reinstalled."));
    buttons->addButton(m_uninstallSelectedButton, QDialogButtonBox::ActionRole);
    connect(m_uninstallSelectedButton, &QPushButton::clicked, this, &ApoManagerDialog::uninstallSelected);

    m_removeAllButton = new QPushButton(QStringLiteral("Remove APO Entirely"), this);
    m_removeAllButton->setToolTip(
        QStringLiteral("Removes every teedsp*.inf package from the Driver Store and "
                       "clears the FX binding on every render endpoint, then restarts "
                       "Windows Audio. TeeDSP stops processing everywhere until "
                       "redeployed. Requires elevation (a UAC prompt appears)."));
    buttons->addButton(m_removeAllButton, QDialogButtonBox::ActionRole);
    connect(m_removeAllButton, &QPushButton::clicked, this, &ApoManagerDialog::removeAllPackages);

    m_redeployButton = new QPushButton(QStringLiteral("Redeploy APO"), this);
    m_redeployButton->setToolTip(
        QStringLiteral("Rebuilds the DSP/APO, repackages and re-signs it, installs it "
                       "into the Driver Store, retires superseded packages, and "
                       "restarts Windows Audio (scripts\\deploy-apo.ps1 -SkipUiPublish). "
                       "Does not rebuild the TeeDSP UI itself. Requires elevation "
                       "partway through (a UAC prompt appears) and can take a minute."));
    buttons->addButton(m_redeployButton, QDialogButtonBox::ActionRole);
    connect(m_redeployButton, &QPushButton::clicked, this, &ApoManagerDialog::redeployApo);

    m_restartEngineButton = new QPushButton(QStringLiteral("Restart Audio Engine"), this);
    m_restartEngineButton->setToolTip(
        QStringLiteral("Restarts the Windows Audio service (Audiosrv), which forces "
                       "audiodg.exe to respawn and reload the APO. Requires elevation "
                       "(a UAC prompt appears). Use this if audio is playing on the "
                       "wrong device, sounds unprocessed, or TeeDSP shows \"not active\" "
                       "on the current output without a clear reason — the same fix as "
                       "scripts\\Restart-AudioEngine.ps1, without leaving the app."));
    buttons->addButton(m_restartEngineButton, QDialogButtonBox::ActionRole);
    connect(m_restartEngineButton, &QPushButton::clicked, this, [this]() {
        m_restartEngineButton->setEnabled(false);
        const bool launched = ui::recovery::restartAudioService();
        m_actionStatusLabel->setText(launched
            ? QStringLiteral("Restarting Windows Audio… approve the UAC prompt if one appears.")
            : QStringLiteral("Restart was declined or could not be launched."));
        m_actionStatusLabel->show();
        m_restartEngineButton->setEnabled(true);
    });

    // scripts\deploy-apo.ps1 / uninstall-apo.ps1 only exist in the dev
    // checkout — hide the actions that depend on it rather than fail at
    // click time if that checkout can't be located (e.g. a build produced
    // outside this repo).
    if (repoScriptPath("scripts/deploy-apo.ps1").isEmpty()) {
        m_uninstallSelectedButton->hide();
        m_removeAllButton->hide();
        m_redeployButton->hide();
        layout->addWidget(new QLabel(QStringLiteral(
            "Uninstall/redeploy actions are unavailable: no dev checkout found "
            "(TEEDSP_SOURCE_DIR)."), this));
    }

    connect(buttons, &QDialogButtonBox::rejected, this, &ApoManagerDialog::reject);
    layout->addWidget(buttons);

    refresh();
}

void ApoManagerDialog::refresh()
{
    const auto packages = host::queryInstalledApoPackages();
    m_packagesTable->setRowCount(packages.size());
    for (int row = 0; row < packages.size(); ++row) {
        const auto &pkg = packages[row];
        m_packagesTable->setItem(row, 0, readOnlyItem(pkg.label));
        m_packagesTable->setItem(row, 1, readOnlyItem(pkg.purpose));
        m_packagesTable->setItem(row, 2, readOnlyItem(pkg.publishedName));
        m_packagesTable->setItem(row, 3, readOnlyItem(pkg.driverVersion));
    }
    m_uninstallSelectedButton->setEnabled(
        !m_operationInFlight && m_packagesTable->currentRow() >= 0);
    m_removeAllButton->setEnabled(!m_operationInFlight && !packages.isEmpty());

    const auto endpoints = host::WasapiDevices::enumerateRender();
    m_bindingsTable->setRowCount(endpoints.size());
    for (int row = 0; row < endpoints.size(); ++row) {
        const auto &ep = endpoints[row];
        const host::ApoBindingInfo binding = host::queryApoBinding(ep.id);
        m_bindingsTable->setItem(row, 0, readOnlyItem(ep.name));
        m_bindingsTable->setItem(row, 1,
            readOnlyItem(binding.bound ? QStringLiteral("Yes") : QStringLiteral("No")));
        m_bindingsTable->setItem(row, 2, readOnlyItem(binding.slot));
    }

    // What's actually loaded into audiodg right now, independent of what's
    // registered in the Driver Store — the two can disagree (e.g. redeploy
    // needed, or the APO isn't loaded on any endpoint at all).
    host::ApoSharedClient client;
    host::ApoSharedClient::ApoStatus status;
    if (client.tryOpen() && client.readStatus(status) && status.dspBuildStamp[0] != '\0') {
        m_loadedBuildLabel->setText(
            QStringLiteral("Loaded build: %1").arg(QString::fromLatin1(status.dspBuildStamp)));
    } else {
        m_loadedBuildLabel->setText(
            QStringLiteral("Loaded build: — (no audio has hit the APO since boot)"));
    }
}

void ApoManagerDialog::setActionsEnabled(bool enabled)
{
    m_operationInFlight = !enabled;
    m_refreshButton->setEnabled(enabled);
    m_uninstallSelectedButton->setEnabled(enabled && m_packagesTable->currentRow() >= 0);
    m_removeAllButton->setEnabled(enabled && m_packagesTable->rowCount() > 0);
    m_redeployButton->setEnabled(enabled);
    m_restartEngineButton->setEnabled(enabled);
}

void ApoManagerDialog::appendLog(const QString &text)
{
    m_opLog->show();
    m_opLog->appendPlainText(text);
}

void ApoManagerDialog::uninstallSelected()
{
    const int row = m_packagesTable->currentRow();
    if (row < 0)
        return;
    const QString publishedName = m_packagesTable->item(row, 2)->text();
    const QString label = m_packagesTable->item(row, 0)->text();
    const QString scriptPath = repoScriptPath("scripts/uninstall-apo.ps1");
    if (scriptPath.isEmpty()) {
        m_actionStatusLabel->setText(QStringLiteral("uninstall-apo.ps1 not found."));
        m_actionStatusLabel->show();
        return;
    }

    setActionsEnabled(false);
    m_actionStatusLabel->setText(QStringLiteral("Uninstalling \"%1\"… approve the UAC prompt if one appears.").arg(label));
    m_actionStatusLabel->show();
    appendLog(QStringLiteral("== Uninstalling %1 (%2) ==").arg(label, publishedName));

    const QStringList names{publishedName};
    std::thread([this, scriptPath, names]() {
        const auto result = apolifecycle::removeApoPackages(scriptPath, names);
        QMetaObject::invokeMethod(this, [this, result]() {
            appendLog(result.log);
            m_actionStatusLabel->setText(!result.launched
                ? QStringLiteral("Uninstall was declined or could not be launched.")
                : (result.succeeded
                       ? QStringLiteral("Uninstall finished.")
                       : QStringLiteral("Uninstall finished with errors — see log below.")));
            setActionsEnabled(true);
            refresh();
        }, Qt::QueuedConnection);
    }).detach();
}

void ApoManagerDialog::removeAllPackages()
{
    QStringList names;
    for (int row = 0; row < m_packagesTable->rowCount(); ++row)
        names << m_packagesTable->item(row, 2)->text();
    if (names.isEmpty())
        return;
    const QString scriptPath = repoScriptPath("scripts/uninstall-apo.ps1");
    if (scriptPath.isEmpty()) {
        m_actionStatusLabel->setText(QStringLiteral("uninstall-apo.ps1 not found."));
        m_actionStatusLabel->show();
        return;
    }

    setActionsEnabled(false);
    m_actionStatusLabel->setText(QStringLiteral("Removing the APO entirely… approve the UAC prompt if one appears."));
    m_actionStatusLabel->show();
    appendLog(QStringLiteral("== Removing all TeeDSP packages (%1) ==").arg(names.join(QStringLiteral(", "))));

    std::thread([this, scriptPath, names]() {
        const auto result = apolifecycle::removeApoPackages(scriptPath, names);
        QMetaObject::invokeMethod(this, [this, result]() {
            appendLog(result.log);
            m_actionStatusLabel->setText(!result.launched
                ? QStringLiteral("Removal was declined or could not be launched.")
                : (result.succeeded
                       ? QStringLiteral("APO removed.")
                       : QStringLiteral("Removal finished with errors — see log below.")));
            setActionsEnabled(true);
            refresh();
        }, Qt::QueuedConnection);
    }).detach();
}

void ApoManagerDialog::redeployApo()
{
    const QString scriptPath = repoScriptPath("scripts/deploy-apo.ps1");
    if (scriptPath.isEmpty()) {
        m_actionStatusLabel->setText(QStringLiteral("deploy-apo.ps1 not found."));
        m_actionStatusLabel->show();
        return;
    }
    if (m_redeployProcess)
        return;   // already running

    setActionsEnabled(false);
    m_actionStatusLabel->setText(QStringLiteral("Redeploying APO… this rebuilds the DSP and can take a minute."));
    m_actionStatusLabel->show();
    appendLog(QStringLiteral("== Redeploying APO =="));

    m_redeployProcess = new QProcess(this);
    m_redeployProcess->setWorkingDirectory(QString::fromUtf8(TEEDSP_SOURCE_DIR));
    m_redeployProcess->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_redeployProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        appendLog(QString::fromLocal8Bit(m_redeployProcess->readAllStandardOutput()).trimmed());
    });
    connect(m_redeployProcess, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus) {
        m_actionStatusLabel->setText(exitCode == 0
            ? QStringLiteral("Redeploy finished.")
            : QStringLiteral("Redeploy failed (exit code %1) — see log below.").arg(exitCode));
        m_redeployProcess->deleteLater();
        m_redeployProcess = nullptr;
        setActionsEnabled(true);
        refresh();
    });

    m_redeployProcess->start(QStringLiteral("powershell.exe"),
        {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
         QStringLiteral("-File"), scriptPath, QStringLiteral("-SkipUiPublish")});
}

} // namespace ui
