#include "MainWindow.h"

#include "Theme.h"
#include "widgets/EqCurve.h"
#include "widgets/Knob.h"
#include "widgets/LevelMeter.h"

#include "../dsp/DspController.h"
#include "../dsp/ProcessorChain.h"
#include "../host/AudioEngine.h"
#include "../host/WasapiDevices.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QStyle>
#include <QVariantList>
#include <QVariantMap>
#include <QVBoxLayout>

namespace {

constexpr const char *kCaptureDeviceKey = "io/captureDeviceId";
constexpr const char *kRenderDeviceKey  = "io/renderDeviceId";

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

    setWindowTitle(QStringLiteral("TeeDSP"));
    resize(1100, 640);

    buildUi();
    connectSignals();
    refreshDevices();
    restoreSelectedDevices();
    pullStateFromController();
    refreshEngineStatus();
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
    if (m_engine) m_engine->stop();
    if (m_dspController) m_dspController->saveToSettings();
    saveSelectedDevices();
    QMainWindow::closeEvent(event);
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
    mainRow->addWidget(buildEqSection(), 5);
    mainRow->addWidget(buildCompSection(), 2);
    mainRow->addWidget(buildExciterSection(), 2);
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

    grid->addWidget(createCaption(QStringLiteral("Capture (loopback source)")), 0, 0);
    m_captureDevice = new QComboBox();
    m_captureDevice->setMinimumWidth(220);
    grid->addWidget(m_captureDevice, 0, 1);

    grid->addWidget(createCaption(QStringLiteral("Render (output)")), 0, 2);
    m_renderDevice = new QComboBox();
    m_renderDevice->setMinimumWidth(220);
    grid->addWidget(m_renderDevice, 0, 3);

    m_refreshDevicesButton = new QPushButton(QStringLiteral("Refresh"));
    grid->addWidget(m_refreshDevicesButton, 0, 4);

    m_startStopButton = new QPushButton(QStringLiteral("Start"));
    m_startStopButton->setProperty("role", "primary");
    m_startStopButton->setMinimumWidth(110);
    grid->addWidget(m_startStopButton, 0, 5);

    grid->setColumnStretch(1, 2);
    grid->setColumnStretch(3, 2);

    m_statusLabel = new QLabel(QStringLiteral("Idle."));
    m_statusLabel->setProperty("role", "status");
    m_statusLabel->setWordWrap(true);
    grid->addWidget(m_statusLabel, 1, 0, 1, 6);

    return section;
}

QWidget *MainWindow::buildEqSection()
{
    auto *section = createSection(QStringLiteral("Parametric EQ"));
    auto *col = new QVBoxLayout(section);
    col->setContentsMargins(12, 18, 12, 12);
    col->setSpacing(8);

    auto *headerRow = new QHBoxLayout();
    m_eqEnabled = new QCheckBox(QStringLiteral("Enable EQ"));
    headerRow->addWidget(m_eqEnabled);
    headerRow->addStretch();
    auto *hint = new QLabel(QStringLiteral("Drag points · double-click knob to reset"));
    hint->setProperty("role", "caption");
    headerRow->addWidget(hint);
    col->addLayout(headerRow);

    m_eqCurve = new ui::EqCurve();
    m_eqCurve->setSampleRate(48000.0);
    m_eqCurve->setMinimumHeight(220);
    col->addWidget(m_eqCurve, 1);

    // Per-band controls — 5 columns × {enable, type, freq, Q, gain}
    auto *bandsRow = new QGridLayout();
    bandsRow->setHorizontalSpacing(8);
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

        w.type = new QComboBox();
        w.type->addItems({QStringLiteral("Peaking"),
                          QStringLiteral("Low Shelf"),
                          QStringLiteral("High Shelf")});
        bandsRow->addWidget(w.type, 2, i);

        w.frequency = makeKnob(QStringLiteral("Freq"),
                               20.0, 20000.0, 1000.0, 0,
                               QStringLiteral("Hz"), ui::Knob::Scale::Log);
        w.q   = makeKnob(QStringLiteral("Q"),    0.1, 20.0, 0.707, 2);
        w.gain = makeKnob(QStringLiteral("Gain"), -24.0, 24.0, 0.0, 1, QStringLiteral("dB"));

        auto *knobsRow = new QHBoxLayout();
        knobsRow->setSpacing(2);
        knobsRow->addWidget(w.frequency);
        knobsRow->addWidget(w.q);
        knobsRow->addWidget(w.gain);
        bandsRow->addLayout(knobsRow, 3, i);

        m_eqBands.push_back(w);
    }

    col->addLayout(bandsRow);

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

QWidget *MainWindow::buildFooter()
{
    auto *footer = new QFrame();
    auto *row = new QHBoxLayout(footer);
    row->setContentsMargins(0, 0, 0, 0);

    m_globalBypass = new QCheckBox(QStringLiteral("Bypass entire chain"));
    row->addWidget(m_globalBypass);
    row->addStretch();

    m_resetButton = new QPushButton(QStringLiteral("Reset to Defaults"));
    row->addWidget(m_resetButton);
    return footer;
}

void MainWindow::connectSignals()
{
    connect(m_refreshDevicesButton, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(m_startStopButton, &QPushButton::clicked, this, &MainWindow::onStartStopClicked);
    connect(m_resetButton, &QPushButton::clicked, this, &MainWindow::onResetDefaultsClicked);

    connect(m_engine, &host::AudioEngine::runningChanged, this, &MainWindow::refreshEngineStatus);
    connect(m_engine, &host::AudioEngine::errorOccurred, this, &MainWindow::onEngineError);

    connect(m_globalBypass, &QCheckBox::toggled, this, [this](bool c) {
        if (!m_syncingUi) m_dspController->setBypass(c);
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
        connect(w.type, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this, band](int t) {
            if (!m_syncingUi) m_dspController->setEqBandType(band, t);
        });
        connect(w.frequency, &ui::Knob::valueChanged, this, [this, band](double v) {
            if (!m_syncingUi) m_dspController->setEqBandFrequency(band, static_cast<float>(v));
        });
        connect(w.q, &ui::Knob::valueChanged, this, [this, band](double v) {
            if (!m_syncingUi) m_dspController->setEqBandQ(band, static_cast<float>(v));
        });
        connect(w.gain, &ui::Knob::valueChanged, this, [this, band](double v) {
            if (!m_syncingUi) m_dspController->setEqBandGainDb(band, static_cast<float>(v));
        });
    }

    connect(m_eqCurve, &ui::EqCurve::bandDragged, this,
            [this](int band, float freqHz, float gainDb) {
        if (band < 0 || band >= m_eqBands.size()) return;
        // Updating the controller will fire eqChanged → pullStateFromController,
        // which we want, but it also flicks the curve via setBands. Avoid
        // re-entrancy by setting m_syncingUi here only for the knob updates.
        m_dspController->setEqBandFrequency(band, freqHz);
        m_dspController->setEqBandGainDb(band, gainDb);
    });

    connect(m_dspController, &dsp::DspController::bypassChanged,    this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::compressorChanged, this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::exciterChanged,    this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::eqChanged,         this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::meterChanged, this, [this]() {
        const double db = m_dspController->compGainReductionDb();
        m_compMeter->setReductionDb(std::abs(db));
        m_compMeterValue->setText(QString::number(db, 'f', 1) + QStringLiteral(" dB"));
    });

    connect(m_captureDevice, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int){ saveSelectedDevices(); });
    connect(m_renderDevice, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int){ saveSelectedDevices(); });
}

void MainWindow::pullStateFromController()
{
    if (!m_dspController) return;

    m_syncingUi = true;

    m_globalBypass->setChecked(m_dspController->bypass());

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

    const QVariantList eq = m_dspController->eqBands();
    for (int i = 0; i < m_eqBands.size() && i < eq.size(); ++i) {
        const QVariantMap b = eq[i].toMap();
        auto &w = m_eqBands[i];
        w.enabled->setChecked(b.value(QStringLiteral("enabled")).toBool());
        w.type->setCurrentIndex(b.value(QStringLiteral("type")).toInt());
        w.frequency->setValue(b.value(QStringLiteral("frequencyHz")).toDouble());
        w.q->setValue(b.value(QStringLiteral("q")).toDouble());
        w.gain->setValue(b.value(QStringLiteral("gainDb")).toDouble());
    }

    m_syncingUi = false;
    refreshEqCurve();
}

void MainWindow::refreshEqCurve()
{
    if (!m_eqCurve) return;
    QVector<ui::EqBandData> data;
    data.reserve(m_eqBands.size());
    const QVariantList eq = m_dspController->eqBands();
    for (int i = 0; i < eq.size(); ++i) {
        const QVariantMap b = eq[i].toMap();
        ui::EqBandData d;
        d.enabled = b.value(QStringLiteral("enabled")).toBool();
        d.type    = b.value(QStringLiteral("type")).toInt();
        d.freqHz  = static_cast<float>(b.value(QStringLiteral("frequencyHz")).toDouble());
        d.q       = static_cast<float>(b.value(QStringLiteral("q")).toDouble());
        d.gainDb  = static_cast<float>(b.value(QStringLiteral("gainDb")).toDouble());
        data.push_back(d);
    }
    m_eqCurve->setBands(data);
    m_eqCurve->setEqEnabled(m_dspController->eqEnabled());
}

void MainWindow::refreshDevices()
{
    const QString prevCapture = selectedCaptureDeviceId();
    const QString prevRender = selectedRenderDeviceId();

    m_devices = host::WasapiDevices::enumerateRender();

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
    m_syncingUi = false;

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
    if (!cap.isEmpty()) {
        const int idx = m_captureDevice->findData(cap);
        if (idx >= 0) m_captureDevice->setCurrentIndex(idx);
    }
    if (!ren.isEmpty()) {
        const int idx = m_renderDevice->findData(ren);
        if (idx >= 0) m_renderDevice->setCurrentIndex(idx);
    }
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

void MainWindow::onResetDefaultsClicked()
{
    m_dspController->resetToDefaults();
    pullStateFromController();
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

    if (running) {
        m_statusLabel->setText(QStringLiteral("Running · %1 Hz · %2 ch")
            .arg(m_engine->captureSampleRate())
            .arg(m_engine->captureChannels()));
        m_statusLabel->setProperty("role", "statusRunning");
        if (m_engine->captureSampleRate() > 0)
            m_eqCurve->setSampleRate(m_engine->captureSampleRate());
    } else {
        m_statusLabel->setText(QStringLiteral("Idle."));
        m_statusLabel->setProperty("role", "status");
    }
    m_statusLabel->style()->unpolish(m_statusLabel);
    m_statusLabel->style()->polish(m_statusLabel);
}
