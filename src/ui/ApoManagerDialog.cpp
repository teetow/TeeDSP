#include "ApoManagerDialog.h"

#include "AudioServiceRecovery.h"
#include "ApoLifecycleActions.h"
#include "../host/ApoBindingStatus.h"
#include "../host/ApoDiagnostics.h"
#include "../host/ApoSharedClient.h"
#include "../host/WasapiDevices.h"

#include <QAbstractItemView>
#include <QClipboard>
#include <QDate>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLayoutItem>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStyle>
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

void clearLayout(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
}

// Which known TeeDSP extension package (if any) targets a given render
// endpoint, and which FX slot it uses. Matched by driver/interface name
// rather than the endpoint's (user-renameable) friendly name. Listed here
// unconditionally -- unlike a plain endpoint enumeration, this is how a
// Bluetooth device like AirPods still gets a card while disconnected.
// A live endpoint matching nothing here (e.g. the Focusrite) just reads as
// "not available" — no explicit per-device exclusion list needed.
struct DeviceMapping {
    const char *matchInterface;
    const char *displayName;   // shown when the device isn't currently connected
    const char *extensionInfName;
    const char *slotLabel;
    const char *slotTooltip;
};
constexpr DeviceMapping kDeviceMappings[] = {
    {"AirPods", "AirPods Pro", "teedspairpodsextension.inf", "SFX",
     "Per-stream, pre-mix — Bluetooth owns the other slots on this device."},
    {"Realtek", "Realtek Speakers", "teedsprealtekextension.inf", "Composite MFX",
     "Post-mix, once for everything hitting this device."},
};

bool endpointMatches(const host::DeviceInfo &ep, const DeviceMapping &m)
{
    const QString needle = QString::fromLatin1(m.matchInterface);
    return ep.interfaceName.contains(needle, Qt::CaseInsensitive)
        || ep.name.contains(needle, Qt::CaseInsensitive);
}

// deploy-apo.ps1 encodes its UTC deployment minute in the final DriverVer
// component (HHmm). pnputil parses that component as an integer and drops a
// leading zero, so 09:16 comes back as ".916"; recover it numerically.
QDateTime parseDriverDeploymentTime(const QString &driverVersion)
{
    const QString datePart = driverVersion.section(QLatin1Char(' '), 0, 0);
    const QDate date = QDate::fromString(datePart, QStringLiteral("MM/dd/yyyy"));
    const QString versionPart = driverVersion.section(QLatin1Char(' '), 1, 1);
    const QStringList components = versionPart.split(QLatin1Char('.'));
    if (!date.isValid() || components.size() != 4
        || components[0] != QStringLiteral("0")
        || components[1] != QStringLiteral("1"))
        return {};

    bool ok = false;
    const int hhmm = components[3].toInt(&ok);
    const QTime time(hhmm / 100, hhmm % 100);
    if (!ok || !time.isValid())
        return {};
    return QDateTime(date, time, Qt::UTC);
}

QString displayDriverDeploymentTime(const QString &driverVersion)
{
    const QDateTime utc = parseDriverDeploymentTime(driverVersion);
    return utc.isValid()
        ? utc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))
        : driverVersion;
}

QString displayApoBuildStamp(const char *stamp)
{
    const QString raw = QString::fromLatin1(stamp);
    const QDateTime parsed =
        QLocale::c().toDateTime(raw, QStringLiteral("MMM d yyyy HH:mm:ss"));
    return parsed.isValid()
        ? parsed.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
        : raw;
}

QLabel *statusLabel(const QString &text, const char *role, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setProperty("role", role);
    return label;
}

// A real pill, not just colored text: background/foreground pair, rounded,
// with a small leading dot like the mockup's status LED. bg is "r, g, b"
// (Qt's rgba() takes 0-255 alpha, not CSS's 0-1). setFixedHeight is load-
// bearing here -- without it the label stretches to fill whatever row
// height the surrounding layout happens to have, ballooning into a much
// taller pill than its text needs.
QLabel *pillLabel(const QString &text, const QString &rgb, const QString &fg, QWidget *parent)
{
    auto *label = new QLabel(QStringLiteral("●  %1").arg(text), parent);
    label->setStyleSheet(QStringLiteral(
        "QLabel { background-color: rgba(%1, 40); color: %2; border-radius: 10px; "
        "padding: 0 10px; font-weight: 600; font-size: 8.5pt; }").arg(rgb, fg));
    label->setFixedHeight(20);
    return label;
}

// A small bordered tag for a technical code (SFX / Composite MFX) -- the
// mechanism is indicated, not narrated; the explanation lives in a tooltip.
QLabel *chipLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral(
        "QLabel { border: 1px solid #34363D; border-radius: 4px; padding: 0 7px; "
        "color: #9AA0AE; font-family: Consolas, monospace; font-size: 8pt; }"));
    label->setFixedHeight(18);
    return label;
}

} // namespace

ApoManagerDialog::ApoManagerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Manage APO"));
    resize(820, 700);

    auto *layout = new QVBoxLayout(this);

    m_summaryLabel = statusLabel(QStringLiteral("Checking…"), "status", this);
    QFont summaryFont = m_summaryLabel->font();
    summaryFont.setPointSizeF(summaryFont.pointSizeF() + 1.5);
    summaryFont.setBold(true);
    m_summaryLabel->setFont(summaryFont);
    layout->addWidget(m_summaryLabel);

    m_loadedBuildLabel = new QLabel(QStringLiteral("Loaded build: —"), this);
    m_loadedBuildLabel->setProperty("role", "status");
    layout->addWidget(m_loadedBuildLabel);

    m_deviceCardsContainer = new QWidget(this);
    m_deviceCardsLayout = new QVBoxLayout(m_deviceCardsContainer);
    m_deviceCardsLayout->setContentsMargins(0, 8, 0, 0);
    m_deviceCardsLayout->setSpacing(8);
    layout->addWidget(m_deviceCardsContainer);

    // If the dialog is ever resized taller than its content, the extra space
    // collects here (below the cards, above the footer) instead of Qt
    // spreading it evenly between every widget -- the default when nothing
    // claims stretch, and not what a form-shaped dialog should do.
    layout->addStretch();

    // ---- Driver Store packages: always visible, part of the normal
    // top-down flow (unlike Diagnostics, this one isn't a rare/advanced
    // lookup -- duplicate/superseded packages are a real anomaly this
    // dialog detects, so it needs to be on-screen without an extra click) ----
    layout->addWidget(new QLabel(QStringLiteral("<b>Driver Store packages</b>"), this));

    m_packagesAnomalyLabel = statusLabel(QString(), "statusError", this);
    m_packagesAnomalyLabel->setWordWrap(true);
    m_packagesAnomalyLabel->hide();
    layout->addWidget(m_packagesAnomalyLabel);

    m_retireSupersededButton = new QPushButton(QStringLiteral("Retire Superseded"), this);
    m_retireSupersededButton->setToolTip(
        QStringLiteral("Removes the older duplicate component package(s) from the Driver "
                       "Store without touching any endpoint's binding — safe cleanup after "
                       "an interrupted redeploy. Requires elevation (a UAC prompt appears)."));
    m_retireSupersededButton->hide();
    connect(m_retireSupersededButton, &QPushButton::clicked, this, &ApoManagerDialog::retireSuperseded);
    layout->addWidget(m_retireSupersededButton);

    m_packagesTable = new QTableWidget(0, 5, this);
    m_packagesTable->setHorizontalHeaderLabels(
        {QStringLiteral("Package"), QStringLiteral("Purpose"),
         QStringLiteral("Published name"), QStringLiteral("Deployed (local)"), QString()});
    m_packagesTable->horizontalHeader()->setStretchLastSection(false);
    m_packagesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_packagesTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_packagesTable->verticalHeader()->setVisible(false);
    m_packagesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_packagesTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_packagesTable->setColumnWidth(4, 90);
    layout->addWidget(m_packagesTable);

    // ---- Diagnostics: raw, complete dump for unknown-unknowns debugging --
    // collapsible (it's a rare, deliberate lookup), but a normal item in the
    // top-down flow, not squeezed against the action buttons at the bottom ----
    m_diagToggle = new QPushButton(QStringLiteral("▸ Diagnostics (stats for nerds)"), this);
    m_diagToggle->setCheckable(true);
    m_diagToggle->setProperty("role", "bandTab");
    connect(m_diagToggle, &QPushButton::toggled, this, &ApoManagerDialog::toggleDiagnostics);
    layout->addWidget(m_diagToggle);

    m_diagContainer = new QWidget(this);
    auto *diagLayout = new QVBoxLayout(m_diagContainer);
    diagLayout->setContentsMargins(0, 0, 0, 0);

    m_diagText = new QPlainTextEdit(m_diagContainer);
    m_diagText->setReadOnly(true);
    m_diagText->setFont(QFont(QStringLiteral("Consolas"), 9));
    m_diagText->setFixedHeight(220);
    diagLayout->addWidget(m_diagText);

    auto *diagButtons = new QHBoxLayout();
    m_diagRefreshButton = new QPushButton(QStringLiteral("Refresh"), m_diagContainer);
    connect(m_diagRefreshButton, &QPushButton::clicked, this, &ApoManagerDialog::refreshDiagnostics);
    diagButtons->addWidget(m_diagRefreshButton);

    m_diagCopyButton = new QPushButton(QStringLiteral("Copy All"), m_diagContainer);
    connect(m_diagCopyButton, &QPushButton::clicked, this, [this]() {
        QGuiApplication::clipboard()->setText(m_diagText->toPlainText());
    });
    diagButtons->addWidget(m_diagCopyButton);

    m_diagChurnButton = new QPushButton(QStringLiteral("Run AirPods Churn Check"), m_diagContainer);
    m_diagChurnButton->setToolTip(
        QStringLiteral("Shells out to scripts\\Get-AirPodsEndpointHistory.ps1 -- scans the "
                       "Windows Event Log for AirPods endpoint mints/reconnects. Read-only, "
                       "no elevation, but can take a few seconds on a large log."));
    connect(m_diagChurnButton, &QPushButton::clicked, this, &ApoManagerDialog::runChurnCheck);
    if (repoScriptPath("scripts/Get-AirPodsEndpointHistory.ps1").isEmpty())
        m_diagChurnButton->setEnabled(false);
    diagButtons->addWidget(m_diagChurnButton);
    diagButtons->addStretch();
    diagLayout->addLayout(diagButtons);

    m_diagContainer->setVisible(false);
    layout->addWidget(m_diagContainer);

    // ---- action feedback: what the buttons below actually did ----
    m_actionStatusLabel = new QLabel(this);
    m_actionStatusLabel->setProperty("role", "status");
    m_actionStatusLabel->hide();
    layout->addWidget(m_actionStatusLabel);

    m_opLog = new QPlainTextEdit(this);
    m_opLog->setReadOnly(true);
    m_opLog->setFont(QFont(QStringLiteral("Consolas"), 9));
    m_opLog->setFixedHeight(110);
    m_opLog->hide();
    layout->addWidget(m_opLog);

    // ---- whole-APO danger zone: a labeled row, kept visually apart from
    // both the per-device card actions and the plain Refresh/Close row ----
    auto *dangerRow = new QHBoxLayout();
    auto *dangerLabel = new QLabel(QStringLiteral("WHOLE-APO"), this);
    dangerLabel->setProperty("role", "caption");
    dangerRow->addWidget(dangerLabel);

    m_removeAllButton = new QPushButton(QStringLiteral("Remove APO Entirely"), this);
    m_removeAllButton->setToolTip(
        QStringLiteral("Removes every teedsp*.inf package from the Driver Store and "
                       "clears the FX binding on every render endpoint, then restarts "
                       "Windows Audio. TeeDSP stops processing everywhere until "
                       "redeployed. Requires elevation (a UAC prompt appears)."));
    connect(m_removeAllButton, &QPushButton::clicked, this, &ApoManagerDialog::removeAllPackages);
    dangerRow->addWidget(m_removeAllButton);

    m_redeployButton = new QPushButton(QStringLiteral("Redeploy APO"), this);
    m_redeployButton->setToolTip(
        QStringLiteral("Rebuilds the DSP/APO, repackages and re-signs it, installs it "
                       "into the Driver Store, retires superseded packages, and "
                       "restarts Windows Audio (scripts\\deploy-apo.ps1 -SkipUiPublish). "
                       "Does not rebuild the TeeDSP UI itself. Requires elevation "
                       "partway through (a UAC prompt appears) and can take a minute."));
    connect(m_redeployButton, &QPushButton::clicked, this, &ApoManagerDialog::redeployApo);
    dangerRow->addWidget(m_redeployButton);
    dangerRow->addStretch();

    // scripts\deploy-apo.ps1 / uninstall-apo.ps1 only exist in the dev
    // checkout — hide the actions that depend on it rather than fail at
    // click time if that checkout can't be located (e.g. a build produced
    // outside this repo).
    if (repoScriptPath("scripts/deploy-apo.ps1").isEmpty()) {
        dangerLabel->hide();
        m_removeAllButton->hide();
        m_redeployButton->hide();
        layout->addWidget(new QLabel(QStringLiteral(
            "Uninstall/redeploy actions are unavailable: no dev checkout found "
            "(TEEDSP_SOURCE_DIR)."), this));
    }
    layout->addLayout(dangerRow);

    // ---- bottom row: the panic button on the left, Refresh/Close on the right ----
    auto *bottomRow = new QHBoxLayout();

    m_restoreAudioButton = new QPushButton(QStringLiteral("Restore Audio"), this);
    m_restoreAudioButton->setProperty("role", "recover");
    m_restoreAudioButton->setToolTip(
        QStringLiteral("Restarts the Windows Audio service (Audiosrv), which forces "
                       "audiodg.exe to respawn and reload the APO. The fix for the "
                       "common failure modes: audio silently stopped, playing on the "
                       "wrong device, or TeeDSP shows \"not active\" for no clear "
                       "reason. Requires elevation (a UAC prompt appears)."));
    connect(m_restoreAudioButton, &QPushButton::clicked, this, [this]() {
        m_restoreAudioButton->setEnabled(false);
        const bool launched = ui::recovery::restartAudioService();
        m_actionStatusLabel->setText(launched
            ? QStringLiteral("Restoring audio… approve the UAC prompt if one appears.")
            : QStringLiteral("Restore was declined or could not be launched."));
        m_actionStatusLabel->show();
        m_restoreAudioButton->setEnabled(true);
    });
    bottomRow->addWidget(m_restoreAudioButton);
    bottomRow->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);

    m_refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
    buttons->addButton(m_refreshButton, QDialogButtonBox::ActionRole);
    connect(m_refreshButton, &QPushButton::clicked, this, &ApoManagerDialog::refresh);

    connect(buttons, &QDialogButtonBox::rejected, this, &ApoManagerDialog::reject);
    bottomRow->addWidget(buttons);
    layout->addLayout(bottomRow);

    refresh();
}

void ApoManagerDialog::refresh()
{
    const auto packages = host::queryInstalledApoPackages();
    const auto endpoints = host::WasapiDevices::enumerateRender();

    // ---- device cards ----
    clearLayout(m_deviceCardsLayout);
    m_dynamicActionButtons.clear();

    int notBoundAnomalyCount = 0;

    // displayName/endpointId/connected describe the device; mapping is null
    // for a live endpoint that doesn't match any known extension (e.g. the
    // Focusrite) -- it still gets a card, just an inert "Not available" one.
    // isDefault only means anything when connected.
    auto buildCard = [&](const QString &displayName, const QString &endpointId,
                          bool connected, bool isDefault, const DeviceMapping *mapping) {
        auto *card = new QWidget(m_deviceCardsContainer);
        card->setObjectName(QStringLiteral("apoDeviceCard"));
        // An ID selector plus an explicit child reset -- a bare `QWidget {…}`
        // rule here would otherwise cascade the panel background/border onto
        // every QLabel inside the card too, boxing each word individually.
        card->setStyleSheet(QStringLiteral(
            "#apoDeviceCard { background-color: #1C1E24; border: 1px solid #2A2C33; border-radius: 6px; }"
            "#apoDeviceCard > QLabel { background: transparent; border: none; }"));
        // Fixed, not minimum: whether a card ends up with a pill+chip+button
        // or just a plain "Not available" label, every row must be the same
        // height or the list reads as jagged instead of a uniform table.
        card->setFixedHeight(64);
        auto *outer = new QHBoxLayout(card);
        outer->setContentsMargins(15, 10, 13, 10);
        outer->setSpacing(16);

        // Two lines, like the mockup: name (+ Default tag) on top, the
        // pill + slot chip on a quieter line underneath -- not all crammed
        // into one row.
        auto *mainCol = new QVBoxLayout();
        mainCol->setContentsMargins(0, 0, 0, 0);
        mainCol->setSpacing(4);

        auto *nameRow = new QHBoxLayout();
        nameRow->setSpacing(8);
        auto *nameLabel = new QLabel(displayName, card);
        QFont nameFont = nameLabel->font();
        nameFont.setBold(true);
        nameFont.setPointSizeF(nameFont.pointSizeF() + 0.5);
        nameLabel->setFont(nameFont);
        nameRow->addWidget(nameLabel);
        if (connected && isDefault) {
            auto *defaultTag = new QLabel(QStringLiteral("Default"), card);
            defaultTag->setStyleSheet(QStringLiteral(
                "QLabel { border: 1px solid #2F5A6B; border-radius: 4px; padding: 1px 7px; "
                "color: #4FC1E9; font-size: 8pt; }"));
            defaultTag->setFixedHeight(18);
            defaultTag->setToolTip(
                QStringLiteral("This is the current Windows default output -- audio plays here "
                               "unless an app picks a different device."));
            nameRow->addWidget(defaultTag);
        }
        nameRow->addStretch();
        mainCol->addLayout(nameRow);

        QString publishedName;   // of mapping->extensionInfName, if installed
        bool extensionInstalled = false;
        if (mapping) {
            for (const auto &pkg : packages) {
                if (pkg.originalInfName.compare(QString::fromLatin1(mapping->extensionInfName),
                                                Qt::CaseInsensitive) == 0) {
                    extensionInstalled = true;
                    publishedName = pkg.publishedName;
                    break;
                }
            }
        }

        auto *statusRow = new QHBoxLayout();
        statusRow->setSpacing(8);
        if (!mapping || !extensionInstalled) {
            // Absence of a state, not a state -- plain text, no pill.
            statusRow->addWidget(statusLabel(QStringLiteral("Not available"), "status", card));
        } else if (!connected) {
            auto *notConnected = pillLabel(QStringLiteral("Not connected"),
                                           QStringLiteral("154, 160, 174"), QStringLiteral("#9AA0AE"), card);
            notConnected->setToolTip(
                QStringLiteral("The extension is installed — it'll bind automatically the next "
                               "time this device connects."));
            statusRow->addWidget(notConnected);
            auto *slotChip = chipLabel(QString::fromLatin1(mapping->slotLabel), card);
            slotChip->setToolTip(QString::fromLatin1(mapping->slotTooltip));
            statusRow->addWidget(slotChip);
        } else {
            const bool bound = host::queryApoBinding(endpointId).bound;
            if (bound) {
                statusRow->addWidget(pillLabel(QStringLiteral("Bound"),
                    QStringLiteral("46, 204, 113"), QStringLiteral("#2ECC71"), card));
            } else {
                ++notBoundAnomalyCount;
                auto *notBound = pillLabel(QStringLiteral("Not bound"),
                    QStringLiteral("230, 126, 34"), QStringLiteral("#E67E22"), card);
                notBound->setToolTip(
                    QStringLiteral("The extension is installed but this device isn't bound — "
                                   "that shouldn't happen. Try Restore Audio first; Redeploy "
                                   "APO if that doesn't help."));
                statusRow->addWidget(notBound);
            }

            auto *slotChip = chipLabel(QString::fromLatin1(mapping->slotLabel), card);
            slotChip->setToolTip(QString::fromLatin1(mapping->slotTooltip));
            statusRow->addWidget(slotChip);
        }
        statusRow->addStretch();
        mainCol->addLayout(statusRow);

        outer->addLayout(mainCol);
        outer->addStretch();

        if (mapping && extensionInstalled) {
            auto *uninstallButton = new QPushButton(QStringLiteral("Uninstall"), card);
            uninstallButton->setToolTip(
                QStringLiteral("Removes the extension package binding this device and clears "
                               "its FX binding. Requires elevation (a UAC prompt appears)."));
            connect(uninstallButton, &QPushButton::clicked, this, [this, publishedName, displayName]() {
                performRemoval({publishedName}, /*skipFxClear=*/false,
                              QStringLiteral("Uninstalling %1's binding").arg(displayName));
            });
            m_dynamicActionButtons.append(uninstallButton);
            outer->addWidget(uninstallButton);
        }

        m_deviceCardsLayout->addWidget(card);
    };

    QList<bool> endpointMatched(endpoints.size(), false);
    for (const auto &mapping : kDeviceMappings) {
        int matchIdx = -1;
        for (int i = 0; i < endpoints.size(); ++i) {
            if (!endpointMatched[i] && endpointMatches(endpoints[i], mapping)) { matchIdx = i; break; }
        }
        if (matchIdx >= 0) {
            endpointMatched[matchIdx] = true;
            const auto &ep = endpoints[matchIdx];
            buildCard(ep.name, ep.id, /*connected=*/true, ep.isDefault, &mapping);
        } else {
            buildCard(QString::fromLatin1(mapping.displayName), QString(), /*connected=*/false,
                      /*isDefault=*/false, &mapping);
        }
    }
    for (int i = 0; i < endpoints.size(); ++i) {
        if (!endpointMatched[i])
            buildCard(endpoints[i].name, endpoints[i].id, /*connected=*/true, endpoints[i].isDefault, nullptr);
    }

    // ---- Driver Store packages table + duplicate-component anomaly ----
    // Shrink to 0 first: cell widgets (the per-row Uninstall buttons) are
    // only torn down on row removal, not on same-index replacement, and a
    // leftover button would still be clickable with a stale captured name.
    m_packagesTable->setRowCount(0);
    m_packagesTable->setRowCount(packages.size());
    QList<int> componentRows;
    for (int row = 0; row < packages.size(); ++row) {
        const auto &pkg = packages[row];
        m_packagesTable->setItem(row, 0, readOnlyItem(pkg.label));
        m_packagesTable->setItem(row, 1, readOnlyItem(pkg.purpose));
        m_packagesTable->setItem(row, 2, readOnlyItem(pkg.publishedName));
        auto *deployedItem = readOnlyItem(displayDriverDeploymentTime(pkg.driverVersion));
        deployedItem->setToolTip(QStringLiteral("Raw DriverVer: %1").arg(pkg.driverVersion));
        m_packagesTable->setItem(row, 3, deployedItem);

        auto *rowUninstall = new QPushButton(QStringLiteral("Uninstall"), m_packagesTable);
        const QString publishedName = pkg.publishedName;
        const QString label = pkg.label;
        connect(rowUninstall, &QPushButton::clicked, this, [this, publishedName, label]() {
            performRemoval({publishedName}, /*skipFxClear=*/false,
                          QStringLiteral("Uninstalling %1").arg(label));
        });
        m_dynamicActionButtons.append(rowUninstall);
        m_packagesTable->setCellWidget(row, 4, rowUninstall);

        if (pkg.originalInfName.compare(QStringLiteral("teedspapocomponent.inf"),
                                        Qt::CaseInsensitive) == 0)
            componentRows.append(row);
    }

    // QTableWidget defaults to an Expanding vertical size policy, so once it
    // sits directly in the dialog's layout (no wrapping container to absorb
    // slack) it grabs any leftover height instead of sizing to its rows --
    // that's what produced the near-empty table with cell widgets rendering
    // stacked in one corner. Pin it to exactly what its rows need, capped so
    // a future long package list scrolls instead of pushing the dialog tall.
    m_packagesTable->resizeRowsToContents();
    int packagesTableHeight = m_packagesTable->horizontalHeader()->height() + 4;
    for (int row = 0; row < m_packagesTable->rowCount(); ++row)
        packagesTableHeight += m_packagesTable->rowHeight(row);
    m_packagesTable->setFixedHeight(qMin(packagesTableHeight, 220));

    if (componentRows.size() > 1) {
        int latestRow = -1;
        QDateTime latestTime;
        bool allParsed = true;
        for (int row : componentRows) {
            const QDateTime time = parseDriverDeploymentTime(packages[row].driverVersion);
            if (!time.isValid()) { allParsed = false; continue; }
            if (latestRow < 0 || time > latestTime) { latestTime = time; latestRow = row; }
        }
        m_packagesAnomalyLabel->setText(
            QStringLiteral("%1 copies of the APO component are installed at once — likely a "
                           "leftover from an interrupted redeploy.").arg(componentRows.size()));
        m_packagesAnomalyLabel->show();
        if (allParsed && latestRow >= 0) {
            m_retireSupersededButton->setEnabled(!m_operationInFlight);
            m_retireSupersededButton->show();
        } else {
            // Couldn't tell which one is newest -- don't guess.
            m_retireSupersededButton->hide();
        }
    } else {
        m_packagesAnomalyLabel->hide();
        m_retireSupersededButton->hide();
    }

    m_removeAllButton->setEnabled(!m_operationInFlight && !packages.isEmpty());

    // ---- live telemetry + summary ----
    host::ApoSharedClient client;
    host::ApoSharedClient::ApoStatus status;
    const bool haveStatus = client.tryOpen() && client.readStatus(status) && status.dspBuildStamp[0] != '\0';
    if (haveStatus) {
        m_loadedBuildLabel->setText(
            QStringLiteral("Loaded build: %1 (%2)")
                .arg(displayApoBuildStamp(status.dspBuildStamp),
                     status.locked ? QStringLiteral("processing now") : QStringLiteral("idle")));
    } else {
        m_loadedBuildLabel->setText(
            QStringLiteral("Loaded build: — (no audio has hit the APO since boot)"));
    }

    const int issues = notBoundAnomalyCount + (componentRows.size() > 1 ? 1 : 0);
    if (issues == 0) {
        m_summaryLabel->setText(QStringLiteral("Nominal"));
        m_summaryLabel->setProperty("role", "statusRunning");
    } else {
        m_summaryLabel->setText(QStringLiteral("%1 issue%2 detected")
                                    .arg(issues).arg(issues == 1 ? QString() : QStringLiteral("s")));
        m_summaryLabel->setProperty("role", "statusError");
    }
    m_summaryLabel->style()->unpolish(m_summaryLabel);
    m_summaryLabel->style()->polish(m_summaryLabel);
}

void ApoManagerDialog::setActionsEnabled(bool enabled)
{
    m_operationInFlight = !enabled;
    m_refreshButton->setEnabled(enabled);
    m_removeAllButton->setEnabled(enabled && m_packagesTable->rowCount() > 0);
    m_redeployButton->setEnabled(enabled);
    m_restoreAudioButton->setEnabled(enabled);
    m_retireSupersededButton->setEnabled(enabled);
    for (QPushButton *btn : m_dynamicActionButtons)
        btn->setEnabled(enabled);
}

void ApoManagerDialog::appendLog(const QString &text)
{
    m_opLog->show();
    m_opLog->appendPlainText(text);
}

void ApoManagerDialog::performRemoval(const QStringList &publishedNames, bool skipFxClear,
                                      const QString &describedAs)
{
    const QString scriptPath = repoScriptPath("scripts/uninstall-apo.ps1");
    if (scriptPath.isEmpty()) {
        m_actionStatusLabel->setText(QStringLiteral("uninstall-apo.ps1 not found."));
        m_actionStatusLabel->show();
        return;
    }

    setActionsEnabled(false);
    m_actionStatusLabel->setText(
        QStringLiteral("%1… approve the UAC prompt if one appears.").arg(describedAs));
    m_actionStatusLabel->show();
    appendLog(QStringLiteral("== %1 (%2) ==").arg(describedAs, publishedNames.join(QStringLiteral(", "))));

    std::thread([this, scriptPath, publishedNames, skipFxClear, describedAs]() {
        const auto result = apolifecycle::removeApoPackages(scriptPath, publishedNames, skipFxClear);
        QMetaObject::invokeMethod(this, [this, result, describedAs]() {
            appendLog(result.log);
            m_actionStatusLabel->setText(!result.launched
                ? QStringLiteral("%1 was declined or could not be launched.").arg(describedAs)
                : (result.succeeded
                       ? QStringLiteral("%1: done.").arg(describedAs)
                       : QStringLiteral("%1: finished with errors — see log below.").arg(describedAs)));
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
    performRemoval(names, /*skipFxClear=*/false, QStringLiteral("Removing the APO entirely"));
}

void ApoManagerDialog::retireSuperseded()
{
    const auto packages = host::queryInstalledApoPackages();
    QList<int> componentIdx;
    for (int i = 0; i < packages.size(); ++i) {
        if (packages[i].originalInfName.compare(QStringLiteral("teedspapocomponent.inf"),
                                                 Qt::CaseInsensitive) == 0)
            componentIdx.append(i);
    }
    if (componentIdx.size() <= 1)
        return;

    int latestIdx = -1;
    QDateTime latestTime;
    for (int i : componentIdx) {
        const QDateTime time = parseDriverDeploymentTime(packages[i].driverVersion);
        if (!time.isValid()) return;   // shouldn't happen -- refresh() already checked this
        if (latestIdx < 0 || time > latestTime) { latestTime = time; latestIdx = i; }
    }

    QStringList stale;
    for (int i : componentIdx) {
        if (i != latestIdx)
            stale << packages[i].publishedName;
    }
    if (stale.isEmpty())
        return;
    performRemoval(stale, /*skipFxClear=*/true, QStringLiteral("Retiring superseded package(s)"));
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

void ApoManagerDialog::toggleDiagnostics(bool expanded)
{
    m_diagToggle->setText(expanded
        ? QStringLiteral("▾ Diagnostics (stats for nerds)")
        : QStringLiteral("▸ Diagnostics (stats for nerds)"));
    m_diagContainer->setVisible(expanded);
    if (expanded && !m_diagLoadedOnce) {
        m_diagLoadedOnce = true;
        refreshDiagnostics();
    }
}

void ApoManagerDialog::refreshDiagnostics()
{
    m_diagText->setPlainText(host::buildDiagnosticsReport());
}

void ApoManagerDialog::runChurnCheck()
{
    const QString scriptPath = repoScriptPath("scripts/Get-AirPodsEndpointHistory.ps1");
    if (scriptPath.isEmpty())
        return;

    m_diagChurnButton->setEnabled(false);
    m_diagText->appendPlainText(QStringLiteral("\n== Running Get-AirPodsEndpointHistory.ps1 (may take a few seconds) ==\n"));

    std::thread([this, scriptPath]() {
        const QString result = host::runAirPodsChurnCheck(scriptPath);
        QMetaObject::invokeMethod(this, [this, result]() {
            m_diagText->appendPlainText(result);
            m_diagChurnButton->setEnabled(true);
        }, Qt::QueuedConnection);
    }).detach();
}

} // namespace ui
