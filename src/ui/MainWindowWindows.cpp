#include "MainWindow.h"
#include "StartupRegistration.h"
#include "AudioServiceRecovery.h"
#include "ApoManagerDialog.h"
#include "TrayController.h"
#include "widgets/EqCurve.h"
#include "../editor/DspController.h"
#include "../host/ApoBindingStatus.h"
#include "../host/EndpointHealth.h"
#include "../host/WasapiDevices.h"
#include "editor/ApoTransport.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QStyle>

namespace {
constexpr const char *kCaptureDeviceKey = "io/captureDeviceId";
constexpr const char *kFirstRunKey = "ui/initialized";
QLabel *createCaption(const QString &text) {
    auto *label=new QLabel(text);
    label->setProperty("role","caption");
    return label;
}
}
struct MainWindow::PlatformState {
    ui::TrayController *tray=nullptr;
    QList<host::DeviceInfo> outputDevices;
    bool quitting=false;
    bool effectsEnableNeeded=false;
    QPushButton *recoveryButton=nullptr;
    QPushButton *manageApoButton=nullptr;
    unsigned long long lastApoProcessCalls=0;
    qint64 recoverySuppressUntilMs=0;
};

void MainWindow::platformSetup() {
    QSettings s;
    if(!s.value(QString::fromLatin1(kFirstRunKey),false).toBool()) {
        ui::startup::setEnabled(true);
        s.setValue(QString::fromLatin1(kFirstRunKey),true);
    }
    m_platform->tray=new ui::TrayController(this,this);
    m_platform->tray->setStartWithWindows(ui::startup::isEnabled());
}
void MainWindow::initializePlatform() {refreshDevices();restoreSelectedDevices();}
void MainWindow::connectPlatformSignals() {
    connect(m_captureDevice, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int){
        if (m_syncingUi) return;
        // Device picker = which endpoint's TeeDSP we're editing. Just remember
        // the choice; the APO is already inline on whichever device has it.
        // (Per-device param routing arrives with multi-device support.)
        saveSelectedDevices();
        refreshEngineStatus();
    });

    if (m_platform->tray) {
        connect(m_platform->tray, &ui::TrayController::bypassToggled, this, [this](bool b) {
            m_dspController->setBypass(b);
        });
        connect(m_platform->tray, &ui::TrayController::startWithWindowsToggled,
                this, [](bool on) { ui::startup::setEnabled(on); });
        connect(m_platform->tray, &ui::TrayController::quitRequested, this, [this]() {
            m_platform->quitting = true;
            close();
            QApplication::quit();
        });
        connect(m_dspController, &dsp::DspController::bypassChanged,
                this, [this]() { m_platform->tray->setBypass(m_dspController->bypass()); });
    }
}
void MainWindow::savePlatformState() const {saveSelectedDevices();}
void MainWindow::cleanupPlatform() {delete m_platform;m_platform=nullptr;}
bool MainWindow::handleClose(QCloseEvent *event) {
    if(m_platform->quitting || !m_platform->tray) return false;
    event->ignore();hide();return true;
}

QWidget *MainWindow::buildIoSection()
{
    m_platform=new PlatformState;
    auto *section = new QWidget();
    auto *grid = new QGridLayout(section);
    grid->setContentsMargins(0, 4, 0, 4);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);

    // The APO follows Windows' current output. Mirror that output in the device
    // picker so the editor never displays a stale endpoint after an automatic
    // Bluetooth/Realtek switch.
    grid->addWidget(createCaption(QStringLiteral("Device")), 0, 0);
    m_captureDevice = new QComboBox();
    m_captureDevice->setMinimumWidth(220);
    grid->addWidget(m_captureDevice, 0, 1);

    m_platform->manageApoButton = new QPushButton(QStringLiteral("Manage APO..."));
    connect(m_platform->manageApoButton, &QPushButton::clicked,
            this, &MainWindow::onManageApoRequested);
    grid->addWidget(m_platform->manageApoButton, 0, 2);

    m_globalBypass = new QCheckBox(QStringLiteral("Bypass"));
    grid->addWidget(m_globalBypass, 0, 3);

    grid->setColumnStretch(1, 2);

    m_statusLabel = new QLabel(QStringLiteral("Idle."));
    m_statusLabel->setProperty("role", "status");
    statusBar()->addWidget(m_statusLabel, 1);

    m_platform->recoveryButton = new QPushButton(QStringLiteral("Restart audio engine"));
    m_platform->recoveryButton->setProperty("role", "recover");
    m_platform->recoveryButton->setToolTip(
        QStringLiteral("The audio engine stopped processing. Restart Windows Audio "
                       "(requires elevation) to reload TeeDSP."));
    m_platform->recoveryButton->hide();
    connect(m_platform->recoveryButton, &QPushButton::clicked,
            this, &MainWindow::onRecoveryRequested);
    statusBar()->addPermanentWidget(m_platform->recoveryButton);

    m_dspBuildLabel = new QLabel(QStringLiteral("DSP build: \u2014"));
    m_dspBuildLabel->setProperty("role", "status");
    m_dspBuildLabel->setToolTip(
        QStringLiteral("Compile timestamp reported live by the APO instance "
                       "currently loaded in audiodg.exe \u2014 proves which DSP "
                       "code is actually processing your audio right now, as "
                       "opposed to a stale copy the audio engine hasn't "
                       "reloaded yet."));
    statusBar()->addPermanentWidget(m_dspBuildLabel);

    return section;
}

void MainWindow::refreshDevices()
{
    // Use QSettings as the authoritative preference source — not the combo's
    // current selection, which can drift between refreshes.
    const QSettings s;
    const QString prefCapture = s.value(QString::fromLatin1(kCaptureDeviceKey)).toString();

    m_platform->outputDevices = host::WasapiDevices::enumerateRender();

    // Hold m_syncingUi across the entire populate + select sequence — every
    // setCurrentIndex emits currentIndexChanged, and we don't want any of
    // those to clobber persisted device IDs.
    const bool wasSyncing = m_syncingUi;
    m_syncingUi = true;
    m_captureDevice->clear();
    // Device picker lists output endpoints — the things a TeeDSP APO sits on.
    for (const auto &d : m_platform->outputDevices) {
        m_captureDevice->addItem(d.name, d.id);
    }

    auto selectById = [](QComboBox *cb, const QString &id) -> bool {
        if (id.isEmpty()) return false;
        const int idx = cb->findData(id);
        if (idx >= 0) { cb->setCurrentIndex(idx); return true; }
        return false;
    };

    bool migratedCapture = false;
    if (!selectById(m_captureDevice, prefCapture)) {
        const QString pairedCapture = host::WasapiDevices::pairedCaptureForRender(prefCapture);
        migratedCapture = selectById(m_captureDevice, pairedCapture);
    }

    // First-run / no-pref fallback: pick something reasonable.
    if (m_captureDevice->currentIndex() < 0 && m_captureDevice->count() > 0) {
        int defIdx = -1;
        for (int i = 0; i < m_platform->outputDevices.size(); ++i)
            if (m_platform->outputDevices[i].isDefault) { defIdx = i; break; }
        m_captureDevice->setCurrentIndex(defIdx >= 0 ? defIdx : 0);
    }
    m_syncingUi = wasSyncing;

    if (migratedCapture)
        saveSelectedDevices();
}

void MainWindow::syncDevicePickerToDefaultOutput(const QString &deviceId)
{
    if (deviceId.isEmpty() || !m_captureDevice) return;

    // A Bluetooth endpoint may have appeared since the last manual refresh.
    // Re-enumerate once in that case, then align the picker to the Windows
    // default endpoint.
    int captureIndex = m_captureDevice->findData(deviceId);
    if (captureIndex < 0) {
        refreshDevices();
        captureIndex = m_captureDevice->findData(deviceId);
    }
    if (captureIndex < 0) return;
    if (m_captureDevice->currentIndex() == captureIndex) return;

    const bool wasSyncing = m_syncingUi;
    m_syncingUi = true;
    m_captureDevice->setCurrentIndex(captureIndex);
    m_syncingUi = wasSyncing;
    saveSelectedDevices();
}

QString MainWindow::selectedCaptureDeviceId() const
{
    return m_captureDevice ? m_captureDevice->currentData().toString() : QString();
}

void MainWindow::saveSelectedDevices() const
{
    QSettings s;
    s.setValue(QString::fromLatin1(kCaptureDeviceKey), selectedCaptureDeviceId());
}

void MainWindow::restoreSelectedDevices()
{
    QSettings s;
    const QString cap = s.value(QString::fromLatin1(kCaptureDeviceKey)).toString();

    const bool wasSyncing = m_syncingUi;
    m_syncingUi = true;
    if (!cap.isEmpty()) {
        const int idx = m_captureDevice->findData(cap);
        if (idx >= 0) m_captureDevice->setCurrentIndex(idx);
    }
    m_syncingUi = wasSyncing;
}

namespace {
struct DefaultOutInfo {
    bool hasApo = false;
    bool effectsEnabled = true;
    QString name;
    QString id;
};

// Is the TeeDSP APO bound to the *current* default render endpoint, and what's
// its name? Realtek uses the composite MFX slot (pid 14), while the inbox A2DP
// stack keeps its own MFX and hosts TeeDSP in the third-party SFX slot (pid 5).
DefaultOutInfo queryDefaultOut()
{
    // defaultRenderId() alone is a COM round trip (cheap); the two
    // FxProperties/Properties registry reads below are not, and the default
    // device changes only on rare user action — so skip them on most 400ms
    // status-poll ticks where the device hasn't changed. Bounded to ~10s
    // (not cached indefinitely) so re-registering the FX binding mid-session
    // — e.g. deploy-apo.ps1 installing a new device-extension package while
    // developing — still shows up without an app restart.
    constexpr int kRecheckEveryTicks = 25;
    static QString s_lastId;
    static DefaultOutInfo s_lastInfo;
    static bool s_hasCache = false;
    static int s_ticksSinceRecheck = 0;

    DefaultOutInfo info;
    const QString def = host::WasapiDevices::defaultRenderId();   // {0.0.0...}.{guid}
    if (def.isEmpty()) { s_hasCache = false; return info; }
    if (s_hasCache && def == s_lastId && ++s_ticksSinceRecheck < kRecheckEveryTicks)
        return s_lastInfo;
    s_ticksSinceRecheck = 0;

    info.id = def;
    const host::ApoBindingInfo binding = host::queryApoBinding(def);
    info.hasApo = binding.bound;
    info.effectsEnabled = !binding.effectsDisabled;

    const int dot = def.lastIndexOf(QLatin1Char('.'));
    const QString guid = (dot >= 0) ? def.mid(dot + 1) : def;
    const QString base =
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
                       "\\MMDevices\\Audio\\Render\\") + guid;

    QSettings pr(base + QStringLiteral("\\Properties"), QSettings::NativeFormat);
    info.name = pr.value(QStringLiteral("{a45c254e-df1c-4efd-8020-67d146a850e0},2")).toString();
    if (info.name.isEmpty()) info.name = QStringLiteral("current output");

    s_lastId = def;
    s_lastInfo = info;
    s_hasCache = true;
    return info;
}
} // namespace

void MainWindow::refreshEngineStatus()
{
    // The tray lights only when TeeDSP is shaping the *current* output device —
    // i.e. the APO is bound to the default render endpoint and not bypassed.
    // The shared-block telemetry tells us whether audio is flowing. processCalls
    // is cumulative across all concurrent APO instances (one SFX per stream).
    if (!m_dspController) return;
    const auto st = static_cast<editor::ApoTransport*>(m_dspController->transport())->status();
    const bool bypassed = m_dspController->bypass();
    const DefaultOutInfo out = queryDefaultOut();
    syncDevicePickerToDefaultOutput(out.id);

    const bool advancing = st.open && (st.processCalls != m_platform->lastApoProcessCalls);
    m_platform->lastApoProcessCalls = st.processCalls;
    // Key off the cumulative counter advancing, not the per-instance `locked`
    // flag: with several streams at once (e.g. a Zoom call + media) a co-stream
    // ending would clear `locked` spuriously and fake a stall. processCalls keeps
    // climbing while ANY instance processes, and genuinely stops if the APO is
    // unloaded — so this still catches a real dead engine.
    const bool processing = st.open && advancing && !bypassed;

    const bool activeOnOutput = out.hasApo && out.effectsEnabled && !bypassed;

    // TeeDSP's health model is deliberately limited to its own APO path. Device
    // and transport health belong to the device manager (BluePod).
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    host::HealthInputs hin;
    hin.defaultRenderId = out.id;
    hin.defaultRenderName = out.name;
    hin.apoBound = out.hasApo;
    hin.effectsEnabled = out.effectsEnabled;
    hin.bypassed = bypassed;
    hin.apoProcessing = processing;
    hin.suppressFaults = (nowMs < m_platform->recoverySuppressUntilMs);
    const host::EndpointHealthResult health = host::tickEndpointHealth(hin);

    const bool engineStalled = health.needsServiceRestart;
    m_platform->effectsEnableNeeded = health.needsEffectsEnable;

    QString text;
    const char *role = "status";
    switch (health.verdict) {
    case host::EndpointVerdict::NotFlowing:
        text = QStringLiteral("TeeDSP stopped processing on %1 — audio engine may need a restart")
                   .arg(out.name);
        role = "statusError";
        break;
    case host::EndpointVerdict::NotOnOutput:
        text = QStringLiteral("TeeDSP not active on current output (%1)").arg(out.name);
        break;
    case host::EndpointVerdict::EffectsDisabled:
        text = QStringLiteral("Windows audio enhancements are off on %1 — TeeDSP cannot load")
                   .arg(out.name);
        role = "statusError";
        break;
    case host::EndpointVerdict::Bypassed:
        text = QStringLiteral("TeeDSP — bypassed (%1)").arg(out.name);
        break;
    case host::EndpointVerdict::Active:
        text = QStringLiteral("TeeDSP active on %1 · %2 Hz · %3 ch")
                   .arg(out.name).arg(st.sampleRate).arg(st.channels);
        role = "statusRunning";
        if (st.sampleRate > 0) m_eqCurve->setSampleRate(static_cast<double>(st.sampleRate));
        break;
    case host::EndpointVerdict::Idle:
    case host::EndpointVerdict::Unknown:
        text = QStringLiteral("TeeDSP ready on %1 — no audio").arg(out.name);
        break;
    }
    if (m_platform->recoveryButton) {
        const bool recoveryNeeded = engineStalled || m_platform->effectsEnableNeeded;
        m_platform->recoveryButton->setVisible(recoveryNeeded);
        m_platform->recoveryButton->setText(m_platform->effectsEnableNeeded
            ? QStringLiteral("Enable TeeDSP")
            : QStringLiteral("Restart audio engine"));
        m_platform->recoveryButton->setToolTip(m_platform->effectsEnableNeeded
            ? QStringLiteral("Windows is skipping endpoint effects. Re-enable Device Default "
                             "Effects so the bound TeeDSP APO can load.")
            : QStringLiteral("The audio engine stopped processing. Restart Windows Audio "
                             "(requires elevation) to reload TeeDSP."));
    }
    if (m_statusLabel->text() != text)
        m_statusLabel->setText(text);
    const QString roleName = QString::fromLatin1(role);
    if (m_statusLabel->property("role").toString() != roleName) {
        m_statusLabel->setProperty("role", roleName);
        m_statusLabel->style()->unpolish(m_statusLabel);
        m_statusLabel->style()->polish(m_statusLabel);
    }

    if (m_dspBuildLabel) {
        QString buildText;
        if (st.open && st.dspBuildStamp[0] != '\0')
            buildText = QStringLiteral("DSP build: %1").arg(QString::fromLatin1(st.dspBuildStamp));
        else
            buildText = QStringLiteral("DSP build: \u2014");
        if (m_dspBuildLabel->text() != buildText)
            m_dspBuildLabel->setText(buildText);
    }

    if (m_platform->tray) {
        m_platform->tray->setRunning(activeOnOutput && !engineStalled);
        m_platform->tray->setStatusText(
            engineStalled ? QStringLiteral("TeeDSP — engine stopped (needs restart)")
            : m_platform->effectsEnableNeeded ? QStringLiteral("TeeDSP — Windows audio enhancements are off")
            : !out.hasApo ? QStringLiteral("TeeDSP — not on %1").arg(out.name)
            : bypassed    ? QStringLiteral("TeeDSP — bypassed")
                          : QStringLiteral("TeeDSP — active on %1").arg(out.name));
    }
}

void MainWindow::onRecoveryRequested()
{
    if (m_platform->effectsEnableNeeded) {
        const QString endpointId = host::WasapiDevices::defaultRenderId();
        if (endpointId.isEmpty() || !host::WasapiDevices::setSystemEffectsEnabled(endpointId)) {
            if (m_statusLabel)
                m_statusLabel->setText(QStringLiteral(
                    "Could not enable Windows audio enhancements — use Sound settings"));
            return;
        }

        // PolicyConfig rebuilds the endpoint graph itself. Keep the old cached
        // registry verdict suppressed until queryDefaultOut's bounded cache has
        // refreshed and the new APO instance has begun processing.
        m_platform->recoverySuppressUntilMs = QDateTime::currentMSecsSinceEpoch() + 12'000;
        m_platform->effectsEnableNeeded = false;
        if (m_platform->recoveryButton)
            m_platform->recoveryButton->hide();
        if (m_statusLabel)
            m_statusLabel->setText(QStringLiteral("Enabling TeeDSP on the current output…"));
        return;
    }

    // audiodg can relaunch in protected mode and silently refuse the dev-signed
    // APO. Restarting Windows Audio forces it to respawn and reload TeeDSP.
    if (!ui::recovery::restartAudioService()) {
        // Launch failed or the user dismissed UAC — leave the banner up so they
        // can try again. No modal nag.
        return;
    }
    // Hide the prompt and stop accusing the engine while the service cycles and
    // the APO reloads (~a few seconds). The health model clears its own fault
    // counters while suppressFaults is set.
    m_platform->recoverySuppressUntilMs = QDateTime::currentMSecsSinceEpoch() + 10'000;
    if (m_platform->recoveryButton)
        m_platform->recoveryButton->hide();
    if (m_statusLabel)
        m_statusLabel->setText(QStringLiteral("Restarting audio engine…"));
}

void MainWindow::onManageApoRequested()
{
    ui::ApoManagerDialog dlg(this);
    dlg.exec();
}
