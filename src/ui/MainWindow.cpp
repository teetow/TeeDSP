#include "MainWindow.h"

#include "Theme.h"
#include "StartupRegistration.h"
#include "TrayController.h"
#include "widgets/EqCurve.h"
#include "widgets/Knob.h"
#include "widgets/LevelMeter.h"

#include "../dsp/DspController.h"
#include "../dsp/ProcessorChain.h"
#include "../host/AudioEngine.h"
#include "../host/SpectrumAnalyzer.h"
#include "../host/WasapiDevices.h"

#include <QApplication>

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QDateTime>
#include <QSettings>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>

#include <cmath>
#include <QVariantList>
#include <QVariantMap>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr const char *kCaptureDeviceKey = "io/captureDeviceId";
constexpr const char *kRenderDeviceKey  = "io/renderDeviceId";
constexpr const char *kFirstRunKey      = "ui/initialized";
constexpr const char *kGeometryKey      = "ui/geometry";
constexpr const char *kShowInputSpecKey  = "ui/showInputSpectrum";
constexpr const char *kShowOutputSpecKey = "ui/showOutputSpectrum";
constexpr const char *kShowHeatmapKey    = "ui/showHeatmap";

QGroupBox *createSection(const QString &title)
{
    auto *box = new QGroupBox(title);
    box->setFlat(false);
    return box;
}

QLabel *createCaption(const QString &text)
{
    auto *l = new QLabel(text);
    l->setProperty("role", "caption");
    return l;
}

ui::Knob *makeKnob(const QString &label,
                   double minVal, double maxVal, double defVal,
                   int decimals, const QString &unit = QString(),
                   ui::Knob::Scale scale = ui::Knob::Scale::Linear)
{
    auto *k = new ui::Knob();
    k->setRange(minVal, maxVal, scale);
    k->setDefaultValue(defVal);
    k->setValue(defVal);
    k->setLabel(label);
    k->setUnit(unit);
    k->setDecimals(decimals);
    return k;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_chain = new dsp::ProcessorChain();
    m_chain->prepare(48000.0, 2);

    m_dspController = new dsp::DspController(m_chain, this);
    m_dspController->loadFromSettings();

    m_engine = new host::AudioEngine(m_chain, this);

    // First-run defaults: register Start-with-Windows.
    {
        QSettings s;
        if (!s.value(QString::fromLatin1(kFirstRunKey), false).toBool()) {
            ui::startup::setEnabled(true);
            s.setValue(QString::fromLatin1(kFirstRunKey), true);
        }
    }

    setWindowTitle(QStringLiteral("TeeDSP"));

    {
        QSettings s;
        const QByteArray geo = s.value(QString::fromLatin1(kGeometryKey)).toByteArray();
        if (geo.isEmpty())
            resize(1100, 660);
        else
            restoreGeometry(geo);
    }

    buildUi();

    m_tray = new ui::TrayController(this, this);
    m_tray->setStartWithWindows(ui::startup::isEnabled());

    connectSignals();
    refreshDevices();
    restoreSelectedDevices();

    {
        QSettings s;
        m_showInputSpectrum->setChecked( s.value(QString::fromLatin1(kShowInputSpecKey),  true).toBool());
        m_showOutputSpectrum->setChecked(s.value(QString::fromLatin1(kShowOutputSpecKey), true).toBool());
        m_showHeatmap->setChecked(       s.value(QString::fromLatin1(kShowHeatmapKey),    false).toBool());
    }

    pullStateFromController();
    refreshEngineStatus();

    // Auto-start on launch if both endpoints are remembered. Deferred so the
    // window is up first; failure messages still surface clearly.
    QTimer::singleShot(0, this, [this]() {
        if (selectedCaptureDeviceId().isEmpty() || selectedRenderDeviceId().isEmpty())
            return;
        if (m_engine->isRunning()) return;

        const QString captureId = selectedCaptureDeviceId();
        const QString previousDefaultRender = host::WasapiDevices::defaultRenderId();

        // On launch, inject TeeDSP into the Windows output chain:
        // 1) TeeDSP renders to whatever Windows was previously sending to.
        // 2) Windows default output is switched to TeeDSP's loopback source.
        if (!previousDefaultRender.isEmpty() && previousDefaultRender != captureId) {
            const int idx = m_renderDevice->findData(previousDefaultRender);
            if (idx >= 0) {
                const bool wasSyncing = m_syncingUi;
                m_syncingUi = true;
                m_renderDevice->setCurrentIndex(idx);
                m_syncingUi = wasSyncing;
                saveSelectedDevices();
            }
        }

        const QString err = m_engine->start(captureId, selectedRenderDeviceId());
        if (!err.isEmpty()) m_statusLabel->setText(err);

        if (err.isEmpty() && !captureId.isEmpty() && previousDefaultRender != captureId) {
            if (!host::WasapiDevices::setDefaultRender(captureId)) {
                m_statusLabel->setText(QStringLiteral("Running (warning: failed to set Windows default output to capture device)."));
            }
        }

        refreshEngineStatus();
    });
}

MainWindow::~MainWindow()
{
    if (m_engine) m_engine->stop();
    if (m_dspController) m_dspController->saveToSettings();
    saveSelectedDevices();
    delete m_chain;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Save state on every close — whether we're hiding to tray or fully
    // quitting — so the user never loses settings on either path.
    if (m_dspController) m_dspController->saveToSettings();
    saveSelectedDevices();
    {
        QSettings s;
        s.setValue(QString::fromLatin1(kGeometryKey), saveGeometry());
        s.setValue(QString::fromLatin1(kShowInputSpecKey),  m_showInputSpectrum->isChecked());
        s.setValue(QString::fromLatin1(kShowOutputSpecKey), m_showOutputSpectrum->isChecked());
        s.setValue(QString::fromLatin1(kShowHeatmapKey),    m_showHeatmap->isChecked());
    }

    if (m_quitting || !m_tray) {
        // Heal the gap: restore Windows default output to TeeDSP's render
        // target so audio flows directly there once we stop processing.
        if (m_engine && m_engine->isRunning()) {
            const QString renderId = m_engine->currentRender();
            if (!renderId.isEmpty())
                host::WasapiDevices::setDefaultRender(renderId);
        }
        if (m_engine) m_engine->stop();
        QMainWindow::closeEvent(event);
        return;
    }

    // Hide to tray. Engine keeps running so audio flows uninterrupted.
    event->ignore();
    hide();
}

void MainWindow::buildUi()
{
    m_central = new QWidget(this);
    auto *root = new QVBoxLayout(m_central);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    root->addWidget(buildIoSection());

    auto *mainRow = new QHBoxLayout();
    mainRow->setSpacing(10);
    mainRow->addWidget(buildInputPane(), 0);

    auto *centerRow = new QHBoxLayout();
    centerRow->setSpacing(10);
    centerRow->addWidget(buildEqSection(), 5);

    auto *fxCol = new QVBoxLayout();
    fxCol->setSpacing(10);
    fxCol->addWidget(buildExciterSection(), 1);
    fxCol->addWidget(buildCompSection(), 1);
    centerRow->addLayout(fxCol, 2);

    mainRow->addLayout(centerRow, 1);
    mainRow->addWidget(buildOutputPane(), 0);
    root->addLayout(mainRow);

    root->addWidget(buildFooter());

    setCentralWidget(m_central);
}

QWidget *MainWindow::buildIoSection()
{
    auto *section = createSection(QStringLiteral("Audio I/O"));
    auto *grid = new QGridLayout(section);
    grid->setContentsMargins(12, 18, 12, 12);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);

    grid->addWidget(createCaption(QStringLiteral("Input")), 0, 0);
    m_captureDevice = new QComboBox();
    m_captureDevice->setMinimumWidth(220);
    grid->addWidget(m_captureDevice, 0, 1);

    grid->addWidget(createCaption(QStringLiteral("Output")), 0, 2);
    m_renderDevice = new QComboBox();
    m_renderDevice->setMinimumWidth(220);
    grid->addWidget(m_renderDevice, 0, 3);

    m_refreshDevicesButton = new QPushButton(QStringLiteral("Refresh"));
    grid->addWidget(m_refreshDevicesButton, 0, 4);

    m_globalBypass = new QCheckBox(QStringLiteral("Bypass"));
    grid->addWidget(m_globalBypass, 0, 5);

    m_startStopButton = new QPushButton(QStringLiteral("Start"));
    m_startStopButton->setProperty("role", "primary");
    m_startStopButton->setMinimumWidth(110);
    grid->addWidget(m_startStopButton, 0, 6);

    grid->setColumnStretch(1, 2);
    grid->setColumnStretch(3, 2);

    m_statusLabel = new QLabel(QStringLiteral("Idle."));
    m_statusLabel->setProperty("role", "status");
    statusBar()->addWidget(m_statusLabel, 1);

    return section;
}

QWidget *MainWindow::buildEqSection()
{
    auto *section = createSection(QStringLiteral("Dynamic EQ"));
    auto *col = new QVBoxLayout(section);
    col->setContentsMargins(12, 18, 12, 12);
    col->setSpacing(8);

    auto *headerRow = new QHBoxLayout();
    m_eqEnabled = new QCheckBox(QStringLiteral("Enable EQ"));
    headerRow->addWidget(m_eqEnabled);
    headerRow->addStretch();

    m_showInputSpectrum = new QCheckBox(QStringLiteral("Input"));
    m_showInputSpectrum->setChecked(true);
    m_showInputSpectrum->setStyleSheet(
        QStringLiteral("QCheckBox::indicator:checked { background-color: #4FC1E9; border-color: #4FC1E9; }"));
    headerRow->addWidget(m_showInputSpectrum);

    m_showOutputSpectrum = new QCheckBox(QStringLiteral("Output"));
    m_showOutputSpectrum->setChecked(true);
    m_showOutputSpectrum->setStyleSheet(
        QStringLiteral("QCheckBox::indicator:checked { background-color: #E67E22; border-color: #E67E22; }"));
    headerRow->addWidget(m_showOutputSpectrum);

    m_showHeatmap = new QCheckBox(QStringLiteral("Heatmap"));
    headerRow->addWidget(m_showHeatmap);

    col->addLayout(headerRow);

    m_eqCurve = new ui::EqCurve();
    m_eqCurve->setSampleRate(48000.0);
    m_eqCurve->setMinimumHeight(220);
    col->addWidget(m_eqCurve, 1);

    auto *bandsRow = new QGridLayout();
    bandsRow->setHorizontalSpacing(10);
    bandsRow->setVerticalSpacing(4);

    m_eqBands.reserve(5);
    for (int i = 0; i < 5; ++i) {
        EqBandWidgets w;

        auto *header = new QLabel(QStringLiteral("Band %1").arg(i + 1));
        header->setAlignment(Qt::AlignCenter);
        header->setProperty("role", "caption");
        bandsRow->addWidget(header, 0, i);

        w.enabled = new QCheckBox();
        auto *enableWrap = new QHBoxLayout();
        enableWrap->addStretch();
        enableWrap->addWidget(w.enabled);
        enableWrap->addStretch();
        bandsRow->addLayout(enableWrap, 1, i);

        m_eqBands.push_back(w);
    }

    col->addLayout(bandsRow);

    auto *dynBox = new QGroupBox(QStringLiteral("Selected Band Dynamics"));
    auto *dynCol = new QVBoxLayout(dynBox);
    dynCol->setContentsMargins(8, 10, 8, 8);
    dynCol->setSpacing(6);

    auto *metaRow = new QHBoxLayout();
    m_eqSelectedBand = new QLabel(QStringLiteral("Band 1"));
    m_eqSelectedBand->setProperty("role", "caption");
    metaRow->addWidget(m_eqSelectedBand);
    metaRow->addStretch();
    m_eqDynMeter = new QLabel(QStringLiteral("GR 0.0 dB"));
    m_eqDynMeter->setProperty("role", "status");
    metaRow->addWidget(m_eqDynMeter);
    dynCol->addLayout(metaRow);

    auto *dynRow = new QHBoxLayout();
    dynRow->setSpacing(2);
    m_eqDynThreshold = makeKnob(QStringLiteral("Thresh"), -60.0, 0.0, -18.0, 1, QStringLiteral("dB"));
    m_eqDynRatio = makeKnob(QStringLiteral("Ratio"), 1.0, 20.0, 2.0, 2);
    m_eqDynAttack = makeKnob(QStringLiteral("Attack"), 0.1, 200.0, 10.0, 1, QStringLiteral("ms"), ui::Knob::Scale::Log);
    m_eqDynRelease = makeKnob(QStringLiteral("Release"), 1.0, 1000.0, 120.0, 0, QStringLiteral("ms"), ui::Knob::Scale::Log);
    m_eqDynRange = makeKnob(QStringLiteral("Range"), 0.0, 24.0, 12.0, 1, QStringLiteral("dB"));
    dynRow->addWidget(m_eqDynThreshold);
    dynRow->addWidget(m_eqDynRatio);
    dynRow->addWidget(m_eqDynAttack);
    dynRow->addWidget(m_eqDynRelease);
    dynRow->addWidget(m_eqDynRange);
    dynCol->addLayout(dynRow);

    col->addWidget(dynBox);

    return section;
}

QWidget *MainWindow::buildCompSection()
{
    auto *section = createSection(QStringLiteral("Compressor"));
    auto *col = new QVBoxLayout(section);
    col->setContentsMargins(12, 18, 12, 12);
    col->setSpacing(8);

    m_compEnabled = new QCheckBox(QStringLiteral("Enable"));
    col->addWidget(m_compEnabled);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(2);
    grid->setVerticalSpacing(4);

    m_compThreshold = makeKnob(QStringLiteral("Thresh"),  -60.0,    0.0, -18.0, 1, QStringLiteral("dB"));
    m_compRatio     = makeKnob(QStringLiteral("Ratio"),     1.0,   20.0,   4.0, 2);
    m_compKnee      = makeKnob(QStringLiteral("Knee"),      0.0,   24.0,   6.0, 1, QStringLiteral("dB"));
    m_compAttack    = makeKnob(QStringLiteral("Attack"),    0.1,  200.0,  10.0, 1, QStringLiteral("ms"), ui::Knob::Scale::Log);
    m_compRelease   = makeKnob(QStringLiteral("Release"),   1.0, 1000.0, 120.0, 0, QStringLiteral("ms"), ui::Knob::Scale::Log);
    m_compMakeup    = makeKnob(QStringLiteral("Makeup"),  -12.0,   24.0,   0.0, 1, QStringLiteral("dB"));
    m_compMakeup->setPolarity(ui::Knob::Polarity::Bipolar);
    m_compMakeup->setBipolarOrigin(0.0);

    grid->addWidget(m_compThreshold, 0, 0);
    grid->addWidget(m_compRatio,     0, 1);
    grid->addWidget(m_compKnee,      0, 2);
    grid->addWidget(m_compAttack,    1, 0);
    grid->addWidget(m_compRelease,   1, 1);
    grid->addWidget(m_compMakeup,    1, 2);
    col->addLayout(grid);

    auto *meterCaption = createCaption(QStringLiteral("Gain Reduction"));
    col->addWidget(meterCaption);

    auto *meterRow = new QHBoxLayout();
    m_compMeter = new ui::LevelMeter();
    m_compMeterValue = new QLabel(QStringLiteral("0.0 dB"));
    m_compMeterValue->setMinimumWidth(48);
    m_compMeterValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_compMeterValue->setProperty("role", "status");
    meterRow->addWidget(m_compMeter, 1);
    meterRow->addWidget(m_compMeterValue, 0);
    col->addLayout(meterRow);

    m_outputHotIndicator = new QLabel(QStringLiteral("Output headroom: OK"));
    m_outputHotIndicator->setProperty("role", "status");
    col->addWidget(m_outputHotIndicator);

    col->addStretch();
    return section;
}

QWidget *MainWindow::buildExciterSection()
{
    auto *section = createSection(QStringLiteral("Exciter"));
    auto *col = new QVBoxLayout(section);
    col->setContentsMargins(12, 18, 12, 12);
    col->setSpacing(8);

    m_exciterEnabled = new QCheckBox(QStringLiteral("Enable"));
    col->addWidget(m_exciterEnabled);

    auto *row = new QHBoxLayout();
    row->setSpacing(2);

    m_exciterDrive = makeKnob(QStringLiteral("Drive"),    0.0,    20.0,    2.0, 1);
    m_exciterMix   = makeKnob(QStringLiteral("Mix"),      0.0,     1.0,    0.25, 2);
    m_exciterTone  = makeKnob(QStringLiteral("Tone"),   200.0, 12000.0, 3500.0, 0,
                              QStringLiteral("Hz"), ui::Knob::Scale::Log);

    row->addWidget(m_exciterDrive);
    row->addWidget(m_exciterMix);
    row->addWidget(m_exciterTone);
    col->addLayout(row);

    col->addStretch();
    return section;
}

QWidget *MainWindow::buildInputPane()
{
    auto *section = createSection(QStringLiteral("Input"));
    section->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    section->setMinimumWidth(96);

    auto *col = new QVBoxLayout(section);
    col->setContentsMargins(10, 18, 10, 10);
    col->setSpacing(8);

    m_inputMeterBar = new QProgressBar();
    m_inputMeterBar->setOrientation(Qt::Vertical);
    m_inputMeterBar->setRange(0, 100);
    m_inputMeterBar->setValue(0);
    m_inputMeterBar->setTextVisible(false);
    m_inputMeterBar->setMinimumHeight(260);
    m_inputMeterBar->setStyleSheet(QStringLiteral(
        "QProgressBar { border: 1px solid #2A2C33; background: #121418; width: 18px; }"
        "QProgressBar::chunk { background: #4FC1E9; }"));

    auto *meterWrap = new QHBoxLayout();
    meterWrap->addStretch();
    meterWrap->addWidget(m_inputMeterBar);
    meterWrap->addStretch();
    col->addLayout(meterWrap, 1);

    m_inputTrim = makeKnob(QStringLiteral("In Trim"), -18.0, 18.0, 0.0, 1, QStringLiteral("dB"));
    m_inputTrim->setPolarity(ui::Knob::Polarity::Bipolar);
    m_inputTrim->setBipolarOrigin(0.0);
    col->addWidget(m_inputTrim, 0, Qt::AlignHCenter);

    return section;
}

QWidget *MainWindow::buildOutputPane()
{
    auto *section = createSection(QStringLiteral("Output"));
    section->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    section->setMinimumWidth(120);

    auto *col = new QVBoxLayout(section);
    col->setContentsMargins(10, 18, 10, 10);
    col->setSpacing(8);

    m_outputMeterBar = new QProgressBar();
    m_outputMeterBar->setOrientation(Qt::Vertical);
    m_outputMeterBar->setRange(0, 100);
    m_outputMeterBar->setValue(0);
    m_outputMeterBar->setTextVisible(false);
    m_outputMeterBar->setMinimumHeight(220);
    m_outputMeterBar->setStyleSheet(QStringLiteral(
        "QProgressBar { border: 1px solid #2A2C33; background: #121418; width: 18px; }"
        "QProgressBar::chunk { background: #2ECC71; }"));

    auto *meterWrap = new QHBoxLayout();
    meterWrap->addStretch();
    meterWrap->addWidget(m_outputMeterBar);
    meterWrap->addStretch();
    col->addLayout(meterWrap, 1);

    m_outputVuLabel = new QLabel(QStringLiteral("VU: -inf"));
    m_outputVuLabel->setProperty("role", "status");
    col->addWidget(m_outputVuLabel, 0, Qt::AlignHCenter);

    m_outputLufsLabel = new QLabel(QStringLiteral("LUFS: -inf"));
    m_outputLufsLabel->setProperty("role", "status");
    col->addWidget(m_outputLufsLabel, 0, Qt::AlignHCenter);

    m_outputTrim = makeKnob(QStringLiteral("Out Trim"), -18.0, 18.0, 0.0, 1, QStringLiteral("dB"));
    m_outputTrim->setPolarity(ui::Knob::Polarity::Bipolar);
    m_outputTrim->setBipolarOrigin(0.0);
    col->addWidget(m_outputTrim, 0, Qt::AlignHCenter);

    return section;
}

QWidget *MainWindow::buildFooter()
{
    auto *footer = new QFrame();
    auto *row = new QHBoxLayout(footer);
    row->setContentsMargins(0, 0, 0, 0);
    row->addStretch();
    return footer;
}

void MainWindow::connectSignals()
{
    connect(m_refreshDevicesButton, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(m_startStopButton, &QPushButton::clicked, this, &MainWindow::onStartStopClicked);

    connect(m_engine, &host::AudioEngine::runningChanged, this, &MainWindow::refreshEngineStatus);
    connect(m_engine, &host::AudioEngine::errorOccurred, this, &MainWindow::onEngineError);
    connect(m_engine, &host::AudioEngine::devicesChanged, this, &MainWindow::refreshDevices);

    connect(m_globalBypass, &QCheckBox::toggled, this, [this](bool c) {
        if (!m_syncingUi) m_dspController->setBypass(c);
    });

    connect(m_inputTrim, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setInputTrimDb(static_cast<float>(v));
    });
    connect(m_outputTrim, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setOutputTrimDb(static_cast<float>(v));
    });

    connect(m_compEnabled, &QCheckBox::toggled, this, [this](bool c) {
        if (!m_syncingUi) m_dspController->setCompressorEnabled(c);
    });
    connect(m_compThreshold, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setCompThresholdDb(static_cast<float>(v));
    });
    connect(m_compRatio, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setCompRatio(static_cast<float>(v));
    });
    connect(m_compKnee, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setCompKneeDb(static_cast<float>(v));
    });
    connect(m_compAttack, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setCompAttackMs(static_cast<float>(v));
    });
    connect(m_compRelease, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setCompReleaseMs(static_cast<float>(v));
    });
    connect(m_compMakeup, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setCompMakeupDb(static_cast<float>(v));
    });

    connect(m_exciterEnabled, &QCheckBox::toggled, this, [this](bool c) {
        if (!m_syncingUi) m_dspController->setExciterEnabled(c);
    });
    connect(m_exciterDrive, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setExciterDrive(static_cast<float>(v));
    });
    connect(m_exciterMix, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setExciterMix(static_cast<float>(v));
    });
    connect(m_exciterTone, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setExciterToneHz(static_cast<float>(v));
    });

    connect(m_eqEnabled, &QCheckBox::toggled, this, [this](bool c) {
        if (!m_syncingUi) m_dspController->setEqEnabled(c);
    });

    for (int i = 0; i < m_eqBands.size(); ++i) {
        const int band = i;
        auto &w = m_eqBands[i];

        connect(w.enabled, &QCheckBox::toggled, this, [this, band](bool c) {
            if (!m_syncingUi) m_dspController->setEqBandEnabled(band, c);
        });
    }

    connect(m_eqCurve, &ui::EqCurve::bandDragged, this,
            [this](int band, float freqHz, float gainDb) {
        if (band < 0 || band >= m_eqBands.size()) return;
        m_dspController->setEqBandFrequency(band, freqHz);
        m_dspController->setEqBandGainDb(band, gainDb);
    });

    connect(m_eqCurve, &ui::EqCurve::bandReset, this, [this](int band) {
        if (band < 0 || band >= m_eqBands.size()) return;
        m_dspController->resetBandToDefaults(band);
    });

    connect(m_eqCurve, &ui::EqCurve::bandSelected, this, [this](int band) {
        if (band < 0 || band >= m_eqBands.size()) return;
        m_selectedEqBand = band;
        syncSelectedBandDyn();
    });

    connect(m_eqCurve, &ui::EqCurve::bandQAdjusted, this, [this](int band, float q) {
        if (band < 0 || band >= m_eqBands.size()) return;
        m_dspController->setEqBandQ(band, q);
    });

    connect(m_eqCurve, &ui::EqCurve::bandTypeChanged, this, [this](int band, int type) {
        if (band < 0 || band >= m_eqBands.size()) return;
        m_dspController->setEqBandType(band, type);
    });

    connect(m_eqDynThreshold, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setEqBandDynamicThresholdDb(m_selectedEqBand, static_cast<float>(v));
    });
    connect(m_eqDynRatio, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setEqBandDynamicRatio(m_selectedEqBand, static_cast<float>(v));
    });
    connect(m_eqDynAttack, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setEqBandDynamicAttackMs(m_selectedEqBand, static_cast<float>(v));
    });
    connect(m_eqDynRelease, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setEqBandDynamicReleaseMs(m_selectedEqBand, static_cast<float>(v));
    });
    connect(m_eqDynRange, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setEqBandDynamicRangeDb(m_selectedEqBand, static_cast<float>(v));
    });

    connect(m_showInputSpectrum, &QCheckBox::toggled, this, [this](bool on) {
        m_eqCurve->setShowInputSpectrum(on);
    });
    connect(m_showOutputSpectrum, &QCheckBox::toggled, this, [this](bool on) {
        m_eqCurve->setShowOutputSpectrum(on);
    });

    connect(m_showHeatmap, &QCheckBox::toggled, this, [this](bool on) {
        m_eqCurve->setShowHeatmap(on);
    });

    if (auto *analyzer = m_engine->analyzer()) {
        connect(analyzer, &host::SpectrumAnalyzer::spectraUpdated,
                this, [this](QVector<float> inDb, QVector<float> outDb,
                             double sr, int fftSize) {
            m_eqCurve->setInputSpectrum(inDb, sr, fftSize);
            m_eqCurve->setOutputSpectrum(outDb, sr, fftSize);
        });
    }

    connect(m_dspController, &dsp::DspController::bypassChanged,    this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::compressorChanged, this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::exciterChanged,    this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::eqChanged,         this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::meterChanged, this, [this]() {
        const double db = m_dspController->compGainReductionDb();
        m_compMeter->setReductionDb(std::abs(db));
        m_compMeterValue->setText(QString::number(db, 'f', 1) + QStringLiteral(" dB"));

        const auto dbToMeterPct = [](float dbfs) {
            if (dbfs <= -60.0f) return 0;
            if (dbfs >= 0.0f) return 100;
            return static_cast<int>((dbfs + 60.0f) * (100.0f / 60.0f));
        };

        // Tick delta drives the smoothing alpha. Skew a missing first sample
        // toward the nominal 8 ms timer interval so initial transitions don't
        // snap.
        constexpr float kMeterReleaseTauMs = 10.0f;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        float dtMs = (m_lastMeterTickMs > 0)
            ? static_cast<float>(nowMs - m_lastMeterTickMs)
            : 8.0f;
        if (dtMs <= 0.0f) dtMs = 1.0f;
        m_lastMeterTickMs = nowMs;
        const float alpha = 1.0f - std::exp(-dtMs / kMeterReleaseTauMs);

        const auto smooth = [alpha](float &disp, float fresh) {
            if (fresh > disp) disp = fresh;                  // attack: snap up
            else              disp += alpha * (fresh - disp); // release: smooth
        };

        const float inPeakRaw  = m_engine ? m_engine->currentInputPeakDbfs()  : -120.0f;
        const float outPeakRaw = m_engine ? m_engine->currentOutputPeakDbfs() : -120.0f;
        const float outRmsRaw  = m_engine ? m_engine->currentOutputRmsDbfs()  : -120.0f;
        const float outHotRaw  = m_engine ? m_engine->currentOutputHotDbfs()  : -120.0f;

        smooth(m_dispInPeakDbfs,  inPeakRaw);
        smooth(m_dispOutPeakDbfs, outPeakRaw);
        smooth(m_dispOutRmsDbfs,  outRmsRaw);
        smooth(m_dispOutHotDbfs,  outHotRaw);

        m_inputMeterBar->setValue(dbToMeterPct(m_dispInPeakDbfs));
        m_outputMeterBar->setValue(dbToMeterPct(m_dispOutPeakDbfs));

        // Treat anything below -100 dBFS as silence — exponential decay would
        // otherwise asymptote toward -120 forever and never display "-inf".
        if (m_dispOutRmsDbfs > -100.0f) {
            const float vu = m_dispOutRmsDbfs + 18.0f;       // 0 VU ~= -18 dBFS reference
            const float lufsApprox = m_dispOutRmsDbfs - 0.7f; // rough unweighted estimate
            m_outputVuLabel->setText(QStringLiteral("VU: %1").arg(vu, 0, 'f', 1));
            m_outputLufsLabel->setText(QStringLiteral("LUFS: %1").arg(lufsApprox, 0, 'f', 1));
        } else {
            m_outputVuLabel->setText(QStringLiteral("VU: -inf"));
            m_outputLufsLabel->setText(QStringLiteral("LUFS: -inf"));
        }

        const float hotDbfs = m_dispOutHotDbfs;
        if (hotDbfs > -0.2f) {
            m_outputHotIndicator->setText(QStringLiteral("Output HOT: %1 dBFS").arg(hotDbfs, 0, 'f', 2));
            m_outputHotIndicator->setStyleSheet(QStringLiteral("color:#ff6b6b;"));
        } else if (hotDbfs > -1.0f) {
            m_outputHotIndicator->setText(QStringLiteral("Output near limit: %1 dBFS").arg(hotDbfs, 0, 'f', 2));
            m_outputHotIndicator->setStyleSheet(QStringLiteral("color:#f7c948;"));
        } else {
            m_outputHotIndicator->setText(QStringLiteral("Output headroom: OK"));
            m_outputHotIndicator->setStyleSheet(QString());
        }

        if (m_selectedEqBand >= 0 && m_selectedEqBand < dsp::kEqBandCount) {
            const dsp::EqBandView v = m_dspController->eqBandView(m_selectedEqBand);
            m_eqDynMeter->setText(QStringLiteral("GR %1 dB").arg(v.dynGainReductionDb, 0, 'f', 1));
        }

        // Dynamic gain reduction changes continuously even when controls are
        // static, so refresh the EQ response from meter ticks. update() on the
        // (QOpenGLWidget) curve is coalesced to vsync, so this is cheap.
        refreshEqCurve();
    });

    connect(m_captureDevice, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int){ if (!m_syncingUi) saveSelectedDevices(); });
    connect(m_renderDevice, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int){
        if (m_syncingUi) return;
        saveSelectedDevices();
        // Push the new preference into the engine; if running, this triggers
        // a hot render-side switch without dropping the capture stream.
        m_engine->setPreferredRender(selectedRenderDeviceId());
    });

    connect(m_engine, &host::AudioEngine::currentRenderChanged,
            this, [this](const QString &) { refreshEngineStatus(); });
    connect(m_engine, &host::AudioEngine::captureFormatChanged,
            this, [this](int, int) { refreshEngineStatus(); });

    if (m_tray) {
        connect(m_tray, &ui::TrayController::bypassToggled, this, [this](bool b) {
            m_dspController->setBypass(b);
        });
        connect(m_tray, &ui::TrayController::startWithWindowsToggled,
                this, [](bool on) { ui::startup::setEnabled(on); });
        connect(m_tray, &ui::TrayController::quitRequested, this, [this]() {
            m_quitting = true;
            close();
            QApplication::quit();
        });
        connect(m_dspController, &dsp::DspController::bypassChanged,
                this, [this]() { m_tray->setBypass(m_dspController->bypass()); });
    }
}

void MainWindow::pullStateFromController()
{
    if (!m_dspController) return;

    m_syncingUi = true;

    m_globalBypass->setChecked(m_dspController->bypass());
    m_inputTrim->setValue(m_dspController->inputTrimDb());
    m_outputTrim->setValue(m_dspController->outputTrimDb());

    m_compEnabled->setChecked(m_dspController->compressorEnabled());
    m_compThreshold->setValue(m_dspController->compThresholdDb());
    m_compRatio->setValue(m_dspController->compRatio());
    m_compKnee->setValue(m_dspController->compKneeDb());
    m_compAttack->setValue(m_dspController->compAttackMs());
    m_compRelease->setValue(m_dspController->compReleaseMs());
    m_compMakeup->setValue(m_dspController->compMakeupDb());

    m_exciterEnabled->setChecked(m_dspController->exciterEnabled());
    m_exciterDrive->setValue(m_dspController->exciterDrive());
    m_exciterMix->setValue(m_dspController->exciterMix());
    m_exciterTone->setValue(m_dspController->exciterToneHz());

    m_eqEnabled->setChecked(m_dspController->eqEnabled());

    std::array<dsp::EqBandView, dsp::kEqBandCount> views{};
    m_dspController->eqBandViews(views);
    for (int i = 0; i < m_eqBands.size() && i < static_cast<int>(views.size()); ++i) {
        m_eqBands[i].enabled->setChecked(views[i].enabled);
    }

    m_selectedEqBand = std::clamp(m_selectedEqBand, 0, dsp::kEqBandCount - 1);
    const dsp::EqBandView &selected = views[m_selectedEqBand];
    m_eqSelectedBand->setText(QStringLiteral("Band %1").arg(m_selectedEqBand + 1));
    m_eqDynThreshold->setValue(selected.dynThresholdDb);
    m_eqDynRatio->setValue(selected.dynRatio);
    m_eqDynAttack->setValue(selected.dynAttackMs);
    m_eqDynRelease->setValue(selected.dynReleaseMs);
    m_eqDynRange->setValue(selected.dynRangeDb);
    m_eqDynMeter->setText(QStringLiteral("GR %1 dB").arg(selected.dynGainReductionDb, 0, 'f', 1));

    m_syncingUi = false;
    refreshEqCurve();
}

void MainWindow::refreshEqCurve()
{
    if (!m_eqCurve) return;
    // Hot path: avoid QVariant. Pull a typed snapshot directly from the
    // controller and copy into the curve's input buffer.
    std::array<dsp::EqBandView, dsp::kEqBandCount> views{};
    m_dspController->eqBandViews(views);
    QVector<ui::EqBandData> data;
    data.reserve(static_cast<int>(views.size()));
    for (const auto &v : views) {
        ui::EqBandData d;
        d.enabled            = v.enabled;
        d.type               = v.type;
        d.freqHz             = v.freqHz;
        d.q                  = v.q;
        d.gainDb             = v.gainDb;
        d.dynThresholdDb     = v.dynThresholdDb;
        d.dynGainReductionDb = v.dynGainReductionDb;
        data.push_back(d);
    }
    m_eqCurve->setBands(data);
    m_eqCurve->setEqEnabled(m_dspController->eqEnabled());
}

void MainWindow::syncSelectedBandDyn()
{
    if (!m_dspController) return;
    if (m_selectedEqBand < 0 || m_selectedEqBand >= dsp::kEqBandCount) return;
    const dsp::EqBandView v = m_dspController->eqBandView(m_selectedEqBand);

    const bool was = m_syncingUi;
    m_syncingUi = true;
    m_eqSelectedBand->setText(QStringLiteral("Band %1").arg(m_selectedEqBand + 1));
    m_eqDynThreshold->setValue(v.dynThresholdDb);
    m_eqDynRatio->setValue(v.dynRatio);
    m_eqDynAttack->setValue(v.dynAttackMs);
    m_eqDynRelease->setValue(v.dynReleaseMs);
    m_eqDynRange->setValue(v.dynRangeDb);
    m_eqDynMeter->setText(QStringLiteral("GR %1 dB").arg(v.dynGainReductionDb, 0, 'f', 1));
    m_syncingUi = was;
}

void MainWindow::refreshDevices()
{
    const QString prevCapture = selectedCaptureDeviceId();
    const QString prevRender = selectedRenderDeviceId();

    m_devices = host::WasapiDevices::enumerateRender();

    // Hold m_syncingUi across the entire populate + select sequence — every
    // setCurrentIndex emits currentIndexChanged, and we don't want any of
    // those to clobber persisted device IDs.
    const bool wasSyncing = m_syncingUi;
    m_syncingUi = true;
    m_captureDevice->clear();
    m_renderDevice->clear();
    for (const auto &d : m_devices) {
        const QString label = d.isDefault
            ? QStringLiteral("%1  ★").arg(d.name)
            : d.name;
        m_captureDevice->addItem(label, d.id);
        m_renderDevice->addItem(label, d.id);
    }

    auto selectById = [](QComboBox *cb, const QString &id) {
        if (id.isEmpty()) return;
        const int idx = cb->findData(id);
        if (idx >= 0) cb->setCurrentIndex(idx);
    };
    selectById(m_captureDevice, prevCapture);
    selectById(m_renderDevice, prevRender);

    if (m_captureDevice->currentIndex() < 0 && m_captureDevice->count() > 0) {
        for (int i = 0; i < m_devices.size(); ++i) {
            if (m_devices[i].isDefault) {
                m_captureDevice->setCurrentIndex(i);
                break;
            }
        }
    }
    if (m_renderDevice->currentIndex() < 0 && m_renderDevice->count() > 0) {
        for (int i = 0; i < m_devices.size(); ++i) {
            if (!m_devices[i].isDefault) {
                m_renderDevice->setCurrentIndex(i);
                break;
            }
        }
        if (m_renderDevice->currentIndex() < 0) m_renderDevice->setCurrentIndex(0);
    }
    m_syncingUi = wasSyncing;
}

QString MainWindow::selectedCaptureDeviceId() const
{
    return m_captureDevice ? m_captureDevice->currentData().toString() : QString();
}

QString MainWindow::selectedRenderDeviceId() const
{
    return m_renderDevice ? m_renderDevice->currentData().toString() : QString();
}

void MainWindow::saveSelectedDevices() const
{
    QSettings s;
    s.setValue(QString::fromLatin1(kCaptureDeviceKey), selectedCaptureDeviceId());
    s.setValue(QString::fromLatin1(kRenderDeviceKey), selectedRenderDeviceId());
}

void MainWindow::restoreSelectedDevices()
{
    QSettings s;
    const QString cap = s.value(QString::fromLatin1(kCaptureDeviceKey)).toString();
    const QString ren = s.value(QString::fromLatin1(kRenderDeviceKey)).toString();

    const bool wasSyncing = m_syncingUi;
    m_syncingUi = true;
    if (!cap.isEmpty()) {
        const int idx = m_captureDevice->findData(cap);
        if (idx >= 0) m_captureDevice->setCurrentIndex(idx);
    }
    if (!ren.isEmpty()) {
        const int idx = m_renderDevice->findData(ren);
        if (idx >= 0) m_renderDevice->setCurrentIndex(idx);
    }
    m_syncingUi = wasSyncing;
}

void MainWindow::onStartStopClicked()
{
    if (!m_engine) return;
    if (m_engine->isRunning()) {
        m_engine->stop();
        refreshEngineStatus();
        return;
    }
    const QString err = m_engine->start(selectedCaptureDeviceId(), selectedRenderDeviceId());
    if (!err.isEmpty()) m_statusLabel->setText(err);
    refreshEngineStatus();
}

void MainWindow::onEngineError(const QString &message)
{
    m_statusLabel->setProperty("role", "statusError");
    m_statusLabel->setStyleSheet(QString());  // re-evaluate role property
    m_statusLabel->setText(message);
    m_statusLabel->style()->unpolish(m_statusLabel);
    m_statusLabel->style()->polish(m_statusLabel);
}

void MainWindow::refreshEngineStatus()
{
    const bool running = m_engine && m_engine->isRunning();

    m_startStopButton->setText(running ? QStringLiteral("Stop") : QStringLiteral("Start"));
    m_startStopButton->setProperty("running", running);
    m_startStopButton->style()->unpolish(m_startStopButton);
    m_startStopButton->style()->polish(m_startStopButton);

    QString currentName;
    if (running) {
        const QString currentId = m_engine->currentRender();
        for (const auto &d : m_devices) {
            if (d.id == currentId) { currentName = d.name; break; }
        }
        const QString src = QStringLiteral("%1 Hz · %2 ch")
            .arg(m_engine->captureSampleRate())
            .arg(m_engine->captureChannels());
        if (currentName.isEmpty()) {
            m_statusLabel->setText(QStringLiteral("Running · %1").arg(src));
        } else {
            m_statusLabel->setText(QStringLiteral("Running · %1 → %2").arg(src, currentName));
        }
        m_statusLabel->setProperty("role", "statusRunning");
        if (m_engine->captureSampleRate() > 0)
            m_eqCurve->setSampleRate(m_engine->captureSampleRate());
    } else {
        m_statusLabel->setText(QStringLiteral("Idle."));
        m_statusLabel->setProperty("role", "status");
        m_eqCurve->clearSpectra();
        m_inputMeterBar->setValue(0);
        m_outputMeterBar->setValue(0);
        m_outputVuLabel->setText(QStringLiteral("VU: -inf"));
        m_outputLufsLabel->setText(QStringLiteral("LUFS: -inf"));
        // Reset smoothing state too — otherwise the next meter tick after
        // stop would smoothly decay from the last observed level back to 0.
        m_dispInPeakDbfs  = -120.0f;
        m_dispOutPeakDbfs = -120.0f;
        m_dispOutRmsDbfs  = -120.0f;
        m_dispOutHotDbfs  = -120.0f;
    }
    m_statusLabel->style()->unpolish(m_statusLabel);
    m_statusLabel->style()->polish(m_statusLabel);

    if (m_tray) {
        m_tray->setRunning(running);
        m_tray->setStatusText(running
            ? (currentName.isEmpty()
                 ? QStringLiteral("TeeDSP — running")
                 : QStringLiteral("TeeDSP — %1").arg(currentName))
            : QStringLiteral("TeeDSP — idle"));
    }
}
