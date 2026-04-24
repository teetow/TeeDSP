#include "MainWindow.h"

#include "SystemOutputController.h"
#include "../dsp/ApoSharedClient.h"
#include "../dsp/DspController.h"
#include "../dsp/ProcessorChain.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVariantList>
#include <QVariantMap>

namespace {
QGroupBox *createSection(const QString &title)
{
    auto *box = new QGroupBox(title);
    box->setFlat(false);
    return box;
}

QDoubleSpinBox *createSpin(double min, double max, double step, int decimals, const QString &suffix = QString())
{
    auto *spin = new QDoubleSpinBox();
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setSuffix(suffix);
    spin->setKeyboardTracking(false);
    return spin;
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    auto *chain = new dsp::ProcessorChain();
    chain->prepare(48000.0, 2);

    m_apoClient = new dsp::ApoSharedClient(this);
    m_dspController = new dsp::DspController(chain, this);
    m_systemOutput = new SystemOutputController(m_apoClient, this);

    m_dspController->setApoClient(m_apoClient);
    m_dspController->loadFromSettings();

    setWindowTitle(QStringLiteral("TeeDSP APO Configuration"));
    resize(1040, 760);

    buildUi();
    connectSignals();
    pullStateFromController();
    refreshSystemStatus();
}

MainWindow::~MainWindow()
{
    if (m_dspController)
        m_dspController->saveToSettings();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_dspController)
        m_dspController->saveToSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::buildUi()
{
    m_central = new QWidget(this);
    auto *root = new QVBoxLayout(m_central);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto *systemSection = createSection(QStringLiteral("System APO Status"));
    auto *systemLayout = new QVBoxLayout(systemSection);
    m_statusLabel = new QLabel();
    m_detailLabel = new QLabel();
    m_errorLabel = new QLabel();
    m_detailLabel->setWordWrap(true);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(QStringLiteral("color: #A73737;"));

    m_installButton = new QPushButton(QStringLiteral("Activate / Repair System DSP"));

    systemLayout->addWidget(m_statusLabel);
    systemLayout->addWidget(m_detailLabel);
    systemLayout->addWidget(m_errorLabel);
    systemLayout->addWidget(m_installButton, 0, Qt::AlignLeft);
    root->addWidget(systemSection);

    m_globalBypass = new QCheckBox(QStringLiteral("Bypass Entire DSP Chain"));
    root->addWidget(m_globalBypass);

    auto *row = new QHBoxLayout();
    row->setSpacing(12);

    auto *eqSection = createSection(QStringLiteral("Parametric EQ"));
    auto *eqLayout = new QVBoxLayout(eqSection);
    m_eqEnabled = new QCheckBox(QStringLiteral("Enable EQ"));
    eqLayout->addWidget(m_eqEnabled);

    auto *eqHeader = new QHBoxLayout();
    eqHeader->addWidget(new QLabel(QStringLiteral("On")), 0);
    eqHeader->addWidget(new QLabel(QStringLiteral("Type")), 1);
    eqHeader->addWidget(new QLabel(QStringLiteral("Freq")), 1);
    eqHeader->addWidget(new QLabel(QStringLiteral("Q")), 1);
    eqHeader->addWidget(new QLabel(QStringLiteral("Gain")), 1);
    eqLayout->addLayout(eqHeader);

    m_eqBands.reserve(5);
    for (int i = 0; i < 5; ++i) {
        EqBandWidgets widgets;
        auto *line = new QHBoxLayout();

        widgets.enabled = new QCheckBox(QStringLiteral("Band %1").arg(i + 1));
        widgets.type = new QComboBox();
        widgets.type->addItems({QStringLiteral("Peaking"), QStringLiteral("Low Shelf"), QStringLiteral("High Shelf")});
        widgets.frequency = createSpin(20.0, 20000.0, 1.0, 1, QStringLiteral(" Hz"));
        widgets.q = createSpin(0.1, 20.0, 0.05, 2);
        widgets.gain = createSpin(-24.0, 24.0, 0.1, 1, QStringLiteral(" dB"));

        line->addWidget(widgets.enabled, 0);
        line->addWidget(widgets.type, 1);
        line->addWidget(widgets.frequency, 1);
        line->addWidget(widgets.q, 1);
        line->addWidget(widgets.gain, 1);

        eqLayout->addLayout(line);
        m_eqBands.push_back(widgets);
    }

    auto *compSection = createSection(QStringLiteral("Compressor"));
    auto *compLayout = new QFormLayout(compSection);
    m_compEnabled = new QCheckBox(QStringLiteral("Enable Compressor"));
    m_compThreshold = createSpin(-60.0, 0.0, 0.5, 1, QStringLiteral(" dB"));
    m_compRatio = createSpin(1.0, 20.0, 0.1, 2, QString());
    m_compKnee = createSpin(0.0, 24.0, 0.5, 1, QStringLiteral(" dB"));
    m_compAttack = createSpin(0.1, 200.0, 0.1, 1, QStringLiteral(" ms"));
    m_compRelease = createSpin(1.0, 1000.0, 1.0, 1, QStringLiteral(" ms"));
    m_compMakeup = createSpin(-12.0, 24.0, 0.1, 1, QStringLiteral(" dB"));
    m_compGainReductionLabel = new QLabel(QStringLiteral("0.0 dB"));

    compLayout->addRow(m_compEnabled);
    compLayout->addRow(QStringLiteral("Threshold"), m_compThreshold);
    compLayout->addRow(QStringLiteral("Ratio"), m_compRatio);
    compLayout->addRow(QStringLiteral("Knee"), m_compKnee);
    compLayout->addRow(QStringLiteral("Attack"), m_compAttack);
    compLayout->addRow(QStringLiteral("Release"), m_compRelease);
    compLayout->addRow(QStringLiteral("Makeup"), m_compMakeup);
    compLayout->addRow(QStringLiteral("Gain Reduction"), m_compGainReductionLabel);

    auto *exciterSection = createSection(QStringLiteral("Exciter"));
    auto *exciterLayout = new QFormLayout(exciterSection);
    m_exciterEnabled = new QCheckBox(QStringLiteral("Enable Exciter"));
    m_exciterDrive = createSpin(0.0, 20.0, 0.1, 2);
    m_exciterMix = createSpin(0.0, 1.0, 0.01, 2);
    m_exciterTone = createSpin(200.0, 12000.0, 10.0, 1, QStringLiteral(" Hz"));

    exciterLayout->addRow(m_exciterEnabled);
    exciterLayout->addRow(QStringLiteral("Drive"), m_exciterDrive);
    exciterLayout->addRow(QStringLiteral("Mix"), m_exciterMix);
    exciterLayout->addRow(QStringLiteral("Tone"), m_exciterTone);

    auto *rightCol = new QVBoxLayout();
    rightCol->addWidget(compSection);
    rightCol->addWidget(exciterSection);
    rightCol->addStretch();

    row->addWidget(eqSection, 2);
    auto *rightWrap = new QWidget();
    rightWrap->setLayout(rightCol);
    row->addWidget(rightWrap, 1);

    root->addLayout(row);

    auto *footer = new QHBoxLayout();
    footer->addStretch();
    m_resetButton = new QPushButton(QStringLiteral("Reset To Defaults"));
    footer->addWidget(m_resetButton);
    root->addLayout(footer);

    setCentralWidget(m_central);
}

void MainWindow::connectSignals()
{
    connect(m_installButton, &QPushButton::clicked, this, &MainWindow::onInstallClicked);
    connect(m_resetButton, &QPushButton::clicked, this, &MainWindow::onResetDefaultsClicked);

    connect(m_systemOutput, &SystemOutputController::statusChanged,
            this, &MainWindow::refreshSystemStatus);

    connect(m_globalBypass, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_syncingUi) m_dspController->setBypass(checked);
    });

    connect(m_compEnabled, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_syncingUi) m_dspController->setCompressorEnabled(checked);
    });
    connect(m_compThreshold, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!m_syncingUi) m_dspController->setCompThresholdDb(static_cast<float>(value));
    });
    connect(m_compRatio, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!m_syncingUi) m_dspController->setCompRatio(static_cast<float>(value));
    });
    connect(m_compKnee, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!m_syncingUi) m_dspController->setCompKneeDb(static_cast<float>(value));
    });
    connect(m_compAttack, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!m_syncingUi) m_dspController->setCompAttackMs(static_cast<float>(value));
    });
    connect(m_compRelease, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!m_syncingUi) m_dspController->setCompReleaseMs(static_cast<float>(value));
    });
    connect(m_compMakeup, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!m_syncingUi) m_dspController->setCompMakeupDb(static_cast<float>(value));
    });

    connect(m_exciterEnabled, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_syncingUi) m_dspController->setExciterEnabled(checked);
    });
    connect(m_exciterDrive, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!m_syncingUi) m_dspController->setExciterDrive(static_cast<float>(value));
    });
    connect(m_exciterMix, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!m_syncingUi) m_dspController->setExciterMix(static_cast<float>(value));
    });
    connect(m_exciterTone, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!m_syncingUi) m_dspController->setExciterToneHz(static_cast<float>(value));
    });

    connect(m_eqEnabled, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_syncingUi) m_dspController->setEqEnabled(checked);
    });

    for (int i = 0; i < m_eqBands.size(); ++i) {
        const int band = i;
        auto &widgets = m_eqBands[i];

        connect(widgets.enabled, &QCheckBox::toggled, this, [this, band](bool checked) {
            if (!m_syncingUi) m_dspController->setEqBandEnabled(band, checked);
        });
        connect(widgets.type, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, band](int type) {
            if (!m_syncingUi) m_dspController->setEqBandType(band, type);
        });
        connect(widgets.frequency, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, band](double value) {
            if (!m_syncingUi) m_dspController->setEqBandFrequency(band, static_cast<float>(value));
        });
        connect(widgets.q, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, band](double value) {
            if (!m_syncingUi) m_dspController->setEqBandQ(band, static_cast<float>(value));
        });
        connect(widgets.gain, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, band](double value) {
            if (!m_syncingUi) m_dspController->setEqBandGainDb(band, static_cast<float>(value));
        });
    }

    connect(m_dspController, &dsp::DspController::bypassChanged, this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::compressorChanged, this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::exciterChanged, this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::eqChanged, this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::meterChanged, this, [this]() {
        m_compGainReductionLabel->setText(QString::number(m_dspController->compGainReductionDb(), 'f', 1) + QStringLiteral(" dB"));
    });
}

void MainWindow::pullStateFromController()
{
    if (!m_dspController)
        return;

    m_syncingUi = true;

    m_globalBypass->setChecked(m_dspController->bypass());

    m_compEnabled->setChecked(m_dspController->compressorEnabled());
    m_compThreshold->setValue(m_dspController->compThresholdDb());
    m_compRatio->setValue(m_dspController->compRatio());
    m_compKnee->setValue(m_dspController->compKneeDb());
    m_compAttack->setValue(m_dspController->compAttackMs());
    m_compRelease->setValue(m_dspController->compReleaseMs());
    m_compMakeup->setValue(m_dspController->compMakeupDb());
    m_compGainReductionLabel->setText(QString::number(m_dspController->compGainReductionDb(), 'f', 1) + QStringLiteral(" dB"));

    m_exciterEnabled->setChecked(m_dspController->exciterEnabled());
    m_exciterDrive->setValue(m_dspController->exciterDrive());
    m_exciterMix->setValue(m_dspController->exciterMix());
    m_exciterTone->setValue(m_dspController->exciterToneHz());

    m_eqEnabled->setChecked(m_dspController->eqEnabled());

    const QVariantList eq = m_dspController->eqBands();
    for (int i = 0; i < m_eqBands.size() && i < eq.size(); ++i) {
        const QVariantMap band = eq[i].toMap();
        auto &widgets = m_eqBands[i];
        widgets.enabled->setChecked(band.value(QStringLiteral("enabled")).toBool());
        widgets.type->setCurrentIndex(band.value(QStringLiteral("type")).toInt());
        widgets.frequency->setValue(band.value(QStringLiteral("frequencyHz")).toDouble());
        widgets.q->setValue(band.value(QStringLiteral("q")).toDouble());
        widgets.gain->setValue(band.value(QStringLiteral("gainDb")).toDouble());
    }

    m_syncingUi = false;
}

void MainWindow::refreshSystemStatus()
{
    m_statusLabel->setText(m_systemOutput->statusText());
    m_detailLabel->setText(m_systemOutput->detailText());
    m_errorLabel->setText(m_systemOutput->errorText());
}

void MainWindow::onInstallClicked()
{
    m_systemOutput->installOrRepair();
    refreshSystemStatus();
}

void MainWindow::onResetDefaultsClicked()
{
    m_dspController->resetToDefaults();
    pullStateFromController();
}
