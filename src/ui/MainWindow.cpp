#include "MainWindow.h"

#include "Theme.h"
#include "widgets/EqCurve.h"
#include "widgets/BipolarGainMeter.h"
#include "widgets/Knob.h"
#include "widgets/LevelMeter.h"
#include "widgets/SectionBox.h"
#include "widgets/SpectralGainMeter.h"
#include "widgets/WidgetMetrics.h"

#include "../editor/DspController.h"

#include <QApplication>
#include <QMessageBox>

#include <QCheckBox>
#include <QCloseEvent>
#include <QEvent>
#include <QHideEvent>
#include <QShowEvent>
#include <QComboBox>
#include <QMoveEvent>
#include <QResizeEvent>
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
#include <utility>

namespace {

constexpr const char *kGeometryKey      = "ui/geometry";
constexpr const char *kShowInputSpecKey  = "ui/showInputSpectrum";
constexpr const char *kShowOutputSpecKey = "ui/showOutputSpectrum";
constexpr const char *kShowHeatmapKey    = "ui/showHeatmap";

namespace UiMetrics {
constexpr int kRootMarginTop = 14;
constexpr int kRootMarginSide = 14;
constexpr int kRootMarginBottom = 4;
constexpr int kRootSpacing = 10;
constexpr int kPanelPadLr = 12;
constexpr int kPanelPadTop = 18;
constexpr int kPanelPadBottom = 12;
constexpr int kPanePadLr = 10;
constexpr int kPanePadTop = 18;
constexpr int kPanePadBottom = 10;
constexpr int kDeviceMinWidth = 220;
constexpr int kMeterTallHeight = 260;
constexpr int kMeterHeight = 220;
constexpr int kCompactSpacing = 2;
} // namespace UiMetrics

void repolish(QWidget *w)
{
    if (!w) return;
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

ui::SectionBox *createSection(const QString &title)
{
    auto *box = new ui::SectionBox(title);
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
    m_dspController = new dsp::DspController(this);
    m_dspController->start();

    setWindowTitle(QStringLiteral("TeeDSP"));

    {
        QSettings s;
        const QByteArray geo = s.value(QString::fromLatin1(kGeometryKey)).toByteArray();
        if (geo.isEmpty())
            resize(1100, 660);
        else
            restoreGeometry(geo);
    }

    m_geometrySaveTimer.setInterval(500);
    m_geometrySaveTimer.setSingleShot(true);
    connect(&m_geometrySaveTimer, &QTimer::timeout, this, [this]() {
        QSettings().setValue(QString::fromLatin1(kGeometryKey), saveGeometry());
    });

    buildUi();
    platformSetup();
    connectSignals();
    initializePlatform();

    {
        QSettings s;
        m_showInputSpectrum->setChecked( s.value(QString::fromLatin1(kShowInputSpecKey),  true).toBool());
        m_showOutputSpectrum->setChecked(s.value(QString::fromLatin1(kShowOutputSpecKey), true).toBool());
        m_showHeatmap->setChecked(       s.value(QString::fromLatin1(kShowHeatmapKey),    false).toBool());
    }

    pullStateFromController();
    refreshEngineStatus();

    m_statusTimer.setInterval(400);
    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshEngineStatus);

    updateUiTimerGate();
}

MainWindow::~MainWindow()
{
    if(m_dspController) m_dspController->flush();
    savePlatformState();
    cleanupPlatform();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if(m_dspController) m_dspController->flush();
    savePlatformState();
    QSettings s;
    s.setValue(QString::fromLatin1(kGeometryKey), saveGeometry());
    s.setValue(QString::fromLatin1(kShowInputSpecKey), m_showInputSpectrum->isChecked());
    s.setValue(QString::fromLatin1(kShowOutputSpecKey), m_showOutputSpectrum->isChecked());
    s.setValue(QString::fromLatin1(kShowHeatmapKey), m_showHeatmap->isChecked());
    if(!handleClose(event)) QMainWindow::closeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    m_geometrySaveTimer.start();
}

void MainWindow::moveEvent(QMoveEvent *event)
{
    QMainWindow::moveEvent(event);
    m_geometrySaveTimer.start();
}

void MainWindow::hideEvent(QHideEvent *event)
{
    QMainWindow::hideEvent(event);
    updateUiTimerGate();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    updateUiTimerGate();
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) updateUiTimerGate();
}

void MainWindow::updateUiTimerGate()
{
    const bool active = isVisible() && !isMinimized();
    const bool spectraEnabled = m_showInputSpectrum && m_showOutputSpectrum
        && (m_showInputSpectrum->isChecked() || m_showOutputSpectrum->isChecked());
    const bool analyze = active && spectraEnabled;
    if (m_dspController) m_dspController->setEditorVisible(active);
    if(m_dspController) m_dspController->transport()->setSpectrumVisible(analyze);

    // Keep status polling in the background for transport health and tray state.
    if (!m_statusTimer.isActive()) {
        refreshEngineStatus();
        m_statusTimer.start();
    }
}

void MainWindow::buildUi()
{
    m_central = new QWidget(this);
    auto *root = new QVBoxLayout(m_central);
    root->setContentsMargins(UiMetrics::kRootMarginSide, UiMetrics::kRootMarginTop,
                             UiMetrics::kRootMarginSide, UiMetrics::kRootMarginBottom);
    root->setSpacing(UiMetrics::kRootSpacing);

    root->addWidget(buildIoSection());

    auto *mainRow = new QHBoxLayout();
    mainRow->setSpacing(UiMetrics::kRootSpacing);
    mainRow->addWidget(buildInputPane(), 0);

    auto *centerRow = new QHBoxLayout();
    centerRow->setSpacing(UiMetrics::kRootSpacing);
    centerRow->addWidget(buildEqSection(), 5);

    auto *fxCol = new QVBoxLayout();
    fxCol->setSpacing(UiMetrics::kRootSpacing);
    fxCol->addWidget(buildSpectralSection(), 0);
    fxCol->addWidget(buildExciterSection(), 0);
    fxCol->addWidget(buildCompSection(), 0);
    fxCol->addWidget(buildChannelMixerSection(), 0);
    fxCol->addStretch(1);
    centerRow->addLayout(fxCol, 0);

    mainRow->addLayout(centerRow, 1);
    mainRow->addWidget(buildOutputPane(), 0);
    root->addLayout(mainRow);

    setCentralWidget(m_central);
}

QWidget *MainWindow::buildEqSection()
{
    auto *section = createSection(QStringLiteral("Dynamic EQ"));
    auto *col = new QVBoxLayout(section);
    col->setContentsMargins(UiMetrics::kPanelPadLr, UiMetrics::kPanelPadTop,
                            UiMetrics::kPanelPadLr, UiMetrics::kPanelPadBottom);
    col->setSpacing(6);

    auto *headerRow = new QHBoxLayout();
    m_eqEnabled = new QCheckBox(QStringLiteral("Enable EQ"));
    headerRow->addWidget(m_eqEnabled);
    headerRow->addStretch();

    m_showInputSpectrum = new QCheckBox(QStringLiteral("Input"));
    m_showInputSpectrum->setChecked(true);
    m_showInputSpectrum->setProperty("spectrumRole", "input");
    headerRow->addWidget(m_showInputSpectrum);

    m_showOutputSpectrum = new QCheckBox(QStringLiteral("Output"));
    m_showOutputSpectrum->setChecked(true);
    m_showOutputSpectrum->setProperty("spectrumRole", "output");
    headerRow->addWidget(m_showOutputSpectrum);

    m_showHeatmap = new QCheckBox(QStringLiteral("Heatmap"));
    headerRow->addWidget(m_showHeatmap);

    col->addLayout(headerRow);

    m_eqCurve = new ui::EqCurve();
    m_eqCurve->setSampleRate(48000.0);
    m_eqCurve->setMinimumHeight(UiMetrics::kMeterHeight);
    col->addWidget(m_eqCurve, 1);

    auto *tabBar = new QHBoxLayout();
    tabBar->setSpacing(UiMetrics::kCompactSpacing);

    m_eqBands.reserve(5);
    m_eqBandTabs.reserve(5);
    for (int i = 0; i < 5; ++i) {
        auto *btn = new QPushButton(QStringLiteral("Band %1").arg(i + 1));
        btn->setProperty("role", "bandTab");
        btn->setCheckable(true);
        btn->setChecked(i == 0);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_eqBandTabs.push_back(btn);
        tabBar->addWidget(btn);
        m_eqBands.push_back(EqBandWidgets{});
    }

    col->addLayout(tabBar);

    auto *dynBox = new QWidget();
    auto *dynCol = new QVBoxLayout(dynBox);
    dynCol->setContentsMargins(0, 0, 0, 0);
    dynCol->setSpacing(4);

    auto *metaRow = new QHBoxLayout();
    m_eqBandEnabled = new QCheckBox(QStringLiteral("Enable band"));
    metaRow->addWidget(m_eqBandEnabled);
    metaRow->addStretch();
    m_eqDynMeter = new QLabel(QStringLiteral("GR 0.0 dB"));
    m_eqDynMeter->setProperty("role", "status");
    metaRow->addWidget(m_eqDynMeter);
    dynCol->addLayout(metaRow);

    auto *dynRow = new QHBoxLayout();
    dynRow->setSpacing(UiMetrics::kCompactSpacing);
    m_eqDynThreshold = makeKnob(QStringLiteral("Thresh"), -60.0, 0.0, -18.0, 1, QStringLiteral("dB"));
    m_eqDynRatio = makeKnob(QStringLiteral("Ratio"), 1.0, 20.0, 2.0, 2);
    m_eqDynAttack = makeKnob(QStringLiteral("Attack"), 0.1, 200.0, 10.0, 1, QStringLiteral("ms"), ui::Knob::Scale::Log);
    m_eqDynRelease = makeKnob(QStringLiteral("Release"), 1.0, 3000.0, 120.0, 0, QStringLiteral("ms"), ui::Knob::Scale::Log);
    dynRow->addWidget(m_eqDynThreshold);
    dynRow->addWidget(m_eqDynRatio);
    dynRow->addWidget(m_eqDynAttack);
    dynRow->addWidget(m_eqDynRelease);
    dynCol->addLayout(dynRow);

    // Mini signal / GR meters
    auto *meterGrid = new QGridLayout();
    meterGrid->setHorizontalSpacing(4);
    meterGrid->setVerticalSpacing(2);

    auto *inCaption = createCaption(QStringLiteral("In"));
    auto *outCaption = createCaption(QStringLiteral("Out"));
    auto *grCaption  = createCaption(QStringLiteral("GR"));
    inCaption->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    outCaption->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    grCaption->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_eqDynInputMeter  = new ui::LevelMeter();
    m_eqDynOutputMeter = new ui::LevelMeter();
    m_eqDynGrMeter     = new ui::LevelMeter();
    m_eqDynInputMeter->setBarColor(ui::LevelMeter::BarColor::Input);
    m_eqDynOutputMeter->setBarColor(ui::LevelMeter::BarColor::Output);
    // GainReduction is the default (yellow)

    meterGrid->addWidget(inCaption,           0, 0);
    meterGrid->addWidget(m_eqDynInputMeter,   0, 1);
    meterGrid->addWidget(outCaption,          1, 0);
    meterGrid->addWidget(m_eqDynOutputMeter,  1, 1);
    meterGrid->addWidget(grCaption,           2, 0);
    meterGrid->addWidget(m_eqDynGrMeter,      2, 1);
    meterGrid->setColumnStretch(1, 1);
    dynCol->addLayout(meterGrid);

    col->addWidget(dynBox);

    return section;
}

QWidget *MainWindow::buildCompSection()
{
    auto *section = createSection(QStringLiteral("Compressor"));
    auto *col = new QVBoxLayout(section);
    col->setContentsMargins(UiMetrics::kPanelPadLr, UiMetrics::kPanelPadTop,
                            UiMetrics::kPanelPadLr, UiMetrics::kPanelPadBottom);
    col->setSpacing(8);

    m_compEnabled = new QCheckBox();
    m_compEnabled->setToolTip(QStringLiteral("Enable the compressor."));
    section->setCornerWidget(m_compEnabled);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(2);
    grid->setVerticalSpacing(4);

    m_compThreshold = makeKnob(QStringLiteral("Thresh"),  -60.0,    0.0, -18.0, 1, QStringLiteral("dB"));
    m_compRatio     = makeKnob(QStringLiteral("Ratio"),     1.0,   16.0,   4.0, 2, QString(), ui::Knob::Scale::Log);
    m_compKnee      = makeKnob(QStringLiteral("Knee"),      0.0,   24.0,   6.0, 1, QStringLiteral("dB"));
    m_compAttack    = makeKnob(QStringLiteral("Attack"),    0.1,  200.0,  10.0, 1, QStringLiteral("ms"), ui::Knob::Scale::Log);
    m_compRelease   = makeKnob(QStringLiteral("Release"),   1.0, 3000.0, 120.0, 0, QStringLiteral("ms"), ui::Knob::Scale::Log);
    m_compMakeup    = makeKnob(QStringLiteral("Makeup"),  -12.0,   12.0,   0.0, 1, QStringLiteral("dB"));
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
    m_outputHotIndicator->setProperty("role", "hotIndicator");
    m_outputHotIndicator->setProperty("hotState", "ok");
    col->addWidget(m_outputHotIndicator);

    col->addStretch();
    return section;
}

QWidget *MainWindow::buildExciterSection()
{
    auto *section = createSection(QStringLiteral("Exciter"));
    auto *col = new QVBoxLayout(section);
    col->setContentsMargins(UiMetrics::kPanelPadLr, UiMetrics::kPanelPadTop,
                            UiMetrics::kPanelPadLr, UiMetrics::kPanelPadBottom);
    col->setSpacing(8);

    m_exciterEnabled = new QCheckBox();
    m_exciterEnabled->setToolTip(QStringLiteral("Enable the harmonic exciter."));
    section->setCornerWidget(m_exciterEnabled);

    auto *row = new QHBoxLayout();
    row->setSpacing(UiMetrics::kCompactSpacing);

    m_exciterDrive = makeKnob(QStringLiteral("Drive"),    0.0,    20.0,    2.0, 1,
                              QString(), ui::Knob::Scale::Log);
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

QWidget *MainWindow::buildSpectralSection()
{
    auto *section = createSection(QStringLiteral("Spectral"));
    section->setMinimumWidth(128);
    auto *col = new QVBoxLayout(section);
    col->setContentsMargins(UiMetrics::kPanelPadLr, UiMetrics::kPanelPadTop,
                            UiMetrics::kPanelPadLr, UiMetrics::kPanelPadBottom);
    col->setSpacing(UiMetrics::kCompactSpacing);

    m_spectralLevelerEnabled = new QCheckBox();
    m_spectralLevelerEnabled->setToolTip(
        QStringLiteral("Speech spectral leveler — a gentle ten-band AGC that "
                       "evens out voice timbre before the dynamic EQ. It helps "
                       "muffled or unusually bright voices land in a consistent "
                       "working range."));
    section->setCornerWidget(m_spectralLevelerEnabled);

    m_spectralGainMeter = new ui::SpectralGainMeter();
    m_spectralGainMeter->setToolTip(
        QStringLiteral("Live spectral correction in dB, low band to high, left "
                       "to right. Teal adds energy; orange reduces it. Shaded "
                       "areas are outside a band's allowed range: the low bands "
                       "are held flat and the low mids may only trim, while the "
                       "presence and brilliance bands get the full range. Hover "
                       "a band for its centre frequency and exact gain."));
    col->addWidget(m_spectralGainMeter);
    col->addStretch(1);

    return section;
}

QWidget *MainWindow::buildChannelMixerSection()
{
    auto *section = createSection(QStringLiteral("Channel Mixer"));
    auto *col = new QVBoxLayout(section);
    col->setContentsMargins(UiMetrics::kPanelPadLr, UiMetrics::kPanelPadTop,
                            UiMetrics::kPanelPadLr, UiMetrics::kPanelPadBottom);
    col->setSpacing(8);

    auto *row = new QHBoxLayout();
    row->setSpacing(UiMetrics::kCompactSpacing);

    m_stereoWidth = makeKnob(QStringLiteral("Width"), 0.0, 100.0, 100.0, 0, QStringLiteral("%"));
    row->addWidget(m_stereoWidth, 0, Qt::AlignHCenter);
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
    col->setContentsMargins(UiMetrics::kPanePadLr, UiMetrics::kPanePadTop,
                            UiMetrics::kPanePadLr, UiMetrics::kPanePadBottom);
    col->setSpacing(8);

    const auto makeVBar = [](const char *kind, const char *widthRole) {
        auto *b = new QProgressBar();
        b->setOrientation(Qt::Vertical);
        b->setRange(0, 100);
        b->setValue(0);
        b->setTextVisible(false);
        b->setProperty("role", "vMeter");
        b->setProperty("meterKind", kind);
        b->setProperty("meterWidthRole", widthRole);
        b->setProperty("meterFramed", true);
        return b;
    };

    m_inputMeterBarL = makeVBar("input", "inputWide");
    m_inputMeterBarR = makeVBar("input", "inputWide");
    m_inputMeterBarL->setMinimumHeight(UiMetrics::kMeterTallHeight);
    m_inputMeterBarR->setMinimumHeight(UiMetrics::kMeterTallHeight);

    m_inputGainMeter = new ui::BipolarGainMeter();
    m_inputGainMeter->setRangeDb(18.0);
    m_inputGainMeter->setMinimumHeight(UiMetrics::kMeterTallHeight);
    m_inputGainMeter->setFixedWidth(10);
    m_inputGainMeter->setToolTip(
        QStringLiteral("Auto-Level's live gain: gray = neutral, green = "
                       "boosting, red = cutting."));

    auto *meterWrap = new QHBoxLayout();
    meterWrap->setSpacing(3);
    meterWrap->addStretch();
    meterWrap->addWidget(m_inputGainMeter);
    meterWrap->addSpacing(3);
    meterWrap->addWidget(m_inputMeterBarL);
    meterWrap->addWidget(m_inputMeterBarR);
    meterWrap->addStretch();
    col->addLayout(meterWrap, 1);

    m_inputTrim = makeKnob(QStringLiteral("In Trim"), -18.0, 18.0, 0.0, 1, QStringLiteral("dB"));
    m_inputTrim->setPolarity(ui::Knob::Polarity::Bipolar);
    m_inputTrim->setBipolarOrigin(0.0);
    col->addWidget(m_inputTrim, 0, Qt::AlignHCenter);

    m_levelerEnabled = new QCheckBox(QStringLiteral("Auto"));
    m_levelerEnabled->setToolTip(
        QStringLiteral("Auto-leveler — slow loudness rider that nudges the input "
                       "toward -18 LUFS (up to +18 / -9 dB) without pumping. "
                       "Sits before the input trim, so the trim knob still "
                       "rides on top."));
    col->addWidget(m_levelerEnabled, 0, Qt::AlignHCenter);

    m_levelerGainLabel = new QLabel(QStringLiteral("0.0 dB"));
    m_levelerGainLabel->setProperty("role", "status");
    m_levelerGainLabel->setAlignment(Qt::AlignHCenter);
    col->addWidget(m_levelerGainLabel, 0, Qt::AlignHCenter);

    return section;
}

QWidget *MainWindow::buildOutputPane()
{
    auto *section = createSection(QStringLiteral("Output"));
    section->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    section->setMinimumWidth(120);

    auto *col = new QVBoxLayout(section);
    col->setContentsMargins(UiMetrics::kPanePadLr, UiMetrics::kPanePadTop,
                            UiMetrics::kPanePadLr, UiMetrics::kPanePadBottom);
    col->setSpacing(8);

    const auto makeVBarOut = [](const char *kind, const char *widthRole, bool framed = true) {
        auto *b = new QProgressBar();
        b->setOrientation(Qt::Vertical);
        b->setRange(0, 100);
        b->setValue(0);
        b->setTextVisible(false);
        b->setProperty("role", "vMeter");
        b->setProperty("meterKind", kind);
        b->setProperty("meterWidthRole", widthRole);
        b->setProperty("meterFramed", framed);
        return b;
    };

    m_outputLufsBarL  = makeVBarOut("lufs", "thin");         // LUFS-L (thin)
    m_outputMeterBarL = makeVBarOut("output", "outputWide", false); // VU-L (center frame)
    m_outputMeterBarR = makeVBarOut("output", "outputWide", false); // VU-R (center frame)
    m_outputLufsBarR  = makeVBarOut("lufs", "thin");         // LUFS-R (thin)
    m_outputLufsBarL->setMinimumHeight(UiMetrics::kMeterHeight);
    m_outputMeterBarL->setMinimumHeight(UiMetrics::kMeterHeight);
    m_outputMeterBarR->setMinimumHeight(UiMetrics::kMeterHeight);
    m_outputLufsBarR->setMinimumHeight(UiMetrics::kMeterHeight);

    auto *stereoFrame = new QFrame();
    stereoFrame->setProperty("role", "stereoMeterFrame");
    auto *stereoRow = new QHBoxLayout(stereoFrame);
    stereoRow->setContentsMargins(3, 3, 3, 3);
    stereoRow->setSpacing(UiMetrics::kCompactSpacing);
    stereoRow->addWidget(m_outputMeterBarL);
    stereoRow->addWidget(m_outputMeterBarR);

    m_outputGainMeter = new ui::BipolarGainMeter();
    m_outputGainMeter->setRangeDb(12.0);
    m_outputGainMeter->setMinimumHeight(UiMetrics::kMeterHeight);
    m_outputGainMeter->setFixedWidth(10);
    m_outputGainMeter->setToolTip(
        QStringLiteral("Auto-Level's live gain: gray = neutral, green = "
                       "boosting, red = cutting."));

    auto *meterWrap = new QHBoxLayout();
    meterWrap->setSpacing(UiMetrics::kCompactSpacing);
    meterWrap->addStretch();
    meterWrap->addWidget(m_outputGainMeter);
    meterWrap->addSpacing(3);
    meterWrap->addWidget(m_outputLufsBarL);
    meterWrap->addSpacing(3);
    meterWrap->addWidget(stereoFrame);
    meterWrap->addSpacing(3);
    meterWrap->addWidget(m_outputLufsBarR);
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

    m_outputLevelerEnabled = new QCheckBox(QStringLiteral("Auto"));
    m_outputLevelerEnabled->setToolTip(
        QStringLiteral("Output auto-leveler — slow loudness rider that anchors "
                       "the chain output near -12 LUFS regardless of internal "
                       "gain choices. Sits before Out Trim, so the trim still "
                       "rides on top."));
    col->addWidget(m_outputLevelerEnabled, 0, Qt::AlignHCenter);

    m_outputLevelerGainLabel = new QLabel(QStringLiteral("0.0 dB"));
    m_outputLevelerGainLabel->setProperty("role", "status");
    m_outputLevelerGainLabel->setAlignment(Qt::AlignHCenter);
    col->addWidget(m_outputLevelerGainLabel, 0, Qt::AlignHCenter);

    return section;
}

void MainWindow::connectSignals()
{
    // No Start/Stop: the APO is always inline in audiodg. The bridge engine is
    // retired from the audio path, so we don't wire its run/error signals.

    connect(m_globalBypass, &QCheckBox::toggled, this, [this](bool c) {
        if (!m_syncingUi) m_dspController->setBypass(c);
    });

    connect(m_inputTrim, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setInputTrimDb(static_cast<float>(v));
    });
    connect(m_levelerEnabled, &QCheckBox::toggled, this, [this](bool c) {
        if (!m_syncingUi) m_dspController->setLevelerEnabled(c);
    });
    connect(m_spectralLevelerEnabled, &QCheckBox::toggled, this, [this](bool c) {
        if (!m_syncingUi) m_dspController->setSpectralLevelerEnabled(c);
    });
    connect(m_outputTrim, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setOutputTrimDb(static_cast<float>(v));
    });
    connect(m_outputLevelerEnabled, &QCheckBox::toggled, this, [this](bool c) {
        if (!m_syncingUi) m_dspController->setOutputLevelerEnabled(c);
    });
    connect(m_stereoWidth, &ui::Knob::valueChanged, this, [this](double v) {
        if (!m_syncingUi) m_dspController->setStereoWidth(static_cast<float>(v / 100.0));
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

    connect(m_eqBandEnabled, &QCheckBox::toggled, this, [this](bool c) {
        if (!m_syncingUi) m_dspController->setEqBandEnabled(m_selectedEqBand, c);
    });

    for (int i = 0; i < m_eqBandTabs.size(); ++i) {
        const int band = i;
        connect(m_eqBandTabs[i], &QPushButton::clicked, this, [this, band]() {
            m_selectedEqBand = band;
            for (int j = 0; j < m_eqBandTabs.size(); ++j)
                m_eqBandTabs[j]->setChecked(j == band);
            syncSelectedBandDyn();
            refreshEqCurve();
        });
    }

    connect(m_eqCurve, &ui::EqCurve::bandDragged, this,
            [this](int band, float freqHz, float gainDb) {
        if (band < 0 || band >= m_eqBands.size()) return;
        m_dspController->setEqBandShape(band, freqHz, gainDb);
    });

    connect(m_eqCurve, &ui::EqCurve::bandReset, this, [this](int band) {
        if (band < 0 || band >= m_eqBands.size()) return;
        m_dspController->resetBandToDefaults(band);
    });

    connect(m_eqCurve, &ui::EqCurve::bandEqReset, this, [this](int band) {
        if (band < 0 || band >= m_eqBands.size()) return;
        m_dspController->resetBandEqToDefaults(band);
    });

    connect(m_eqCurve, &ui::EqCurve::bandSelected, this, [this](int band) {
        if (band < 0 || band >= m_eqBands.size()) return;
        m_selectedEqBand = band;
        for (int j = 0; j < m_eqBandTabs.size(); ++j)
            m_eqBandTabs[j]->setChecked(j == band);
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

    connect(m_showInputSpectrum, &QCheckBox::toggled, this, [this](bool on) {
        m_eqCurve->setShowInputSpectrum(on);
        updateUiTimerGate();
        QSettings().setValue(QString::fromLatin1(kShowInputSpecKey), on);
    });
    connect(m_showOutputSpectrum, &QCheckBox::toggled, this, [this](bool on) {
        m_eqCurve->setShowOutputSpectrum(on);
        updateUiTimerGate();
        QSettings().setValue(QString::fromLatin1(kShowOutputSpecKey), on);
    });

    connect(m_showHeatmap, &QCheckBox::toggled, this, [this](bool on) {
        m_eqCurve->setShowHeatmap(on);
        QSettings().setValue(QString::fromLatin1(kShowHeatmapKey), on);
    });

    connect(m_dspController->transport(), &editor::Transport::spectraUpdated,
            this, [this](QVector<float> input, QVector<float> output,
                         double sampleRate, int fftSize) {
        m_eqCurve->setSpectra(input, output, sampleRate, fftSize);
    });

    connect(m_dspController, &dsp::DspController::bypassChanged,    this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::compressorChanged, this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::exciterChanged,    this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::eqChanged,         this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::levelerChanged, this, &MainWindow::pullStateFromController);
    connect(m_dspController, &dsp::DspController::meterChanged, this, [this]() {
        const auto setLabelText = [](QLabel *label, QString text) {
            if (label && label->text() != text)
                label->setText(std::move(text));
        };

        const double db = m_dspController->compGainReductionDb();
        m_compMeter->setReductionDb(std::abs(db));
        setLabelText(m_compMeterValue,
                     QString::number(db, 'f', 1) + QStringLiteral(" dB"));

        const auto dbToMeterPct = [](float dbfs) {
            using namespace ui::widget_metrics::meter_runtime;
            if (dbfs <= kDbMeterMin) return 0;
            if (dbfs >= kDbMeterMax) return 100;
            return static_cast<int>((dbfs - kDbMeterMin) * (100.0f / (kDbMeterMax - kDbMeterMin)));
        };
        const auto lufsToMeterPct = [](float lufs) {
            using namespace ui::widget_metrics::meter_runtime;
            if (lufs <= kLufsMeterMin) return 0;
            if (lufs >= kLufsMeterMax) return 100;
            return static_cast<int>((lufs - kLufsMeterMin) * (100.0f / (kLufsMeterMax - kLufsMeterMin)));
        };

        // Tick delta drives the smoothing alpha. Skew a missing first sample
        // toward the nominal 16 ms timer interval so initial transitions don't
        // snap.
        constexpr float kMeterReleaseTauMs = ui::widget_metrics::meter_runtime::kReleaseTauMs;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        float dtMs = (m_lastMeterTickMs > 0)
            ? static_cast<float>(nowMs - m_lastMeterTickMs)
            : ui::widget_metrics::meter_runtime::kInitialDtMs;
        if (dtMs <= 0.0f) dtMs = 1.0f;
        m_lastMeterTickMs = nowMs;
        const float alpha = 1.0f - std::exp(-dtMs / kMeterReleaseTauMs);

        const auto smooth = [alpha](float &disp, float fresh) {
            if (fresh > disp) disp = fresh;                  // attack: snap up
            else              disp += alpha * (fresh - disp); // release: smooth
        };

        // Both transports expose the same live meter snapshot.
        const float inPeakRaw   = m_dspController->inPeakDbfs(0);
        const float inPeakRawR  = m_dspController->inPeakDbfs(1);
        const float outPeakRaw  = m_dspController->outPeakDbfs(0);
        const float outPeakRawR = m_dspController->outPeakDbfs(1);
        const float outRmsRaw   = m_dspController->outRmsDbfs();
        const float outHotRaw   = std::max(outPeakRaw, outPeakRawR);
        const float outLufsRawL = m_dspController->outLufs(0);
        const float outLufsRawR = m_dspController->outLufs(1);

        smooth(m_dispInPeakDbfs,  inPeakRaw);
        smooth(m_dispInPeakDbfsR, inPeakRawR);
        smooth(m_dispOutPeakDbfs, outPeakRaw);
        smooth(m_dispOutPeakDbfsR, outPeakRawR);
        smooth(m_dispOutRmsDbfs,  outRmsRaw);
        smooth(m_dispOutHotDbfs,  outHotRaw);
        {
            const float lufsPctL = static_cast<float>(lufsToMeterPct(outLufsRawL));
            const float lufsPctR = static_cast<float>(lufsToMeterPct(outLufsRawR));
            if (lufsPctL > m_dispOutLufsPctL) m_dispOutLufsPctL = lufsPctL;
            else m_dispOutLufsPctL += alpha * (lufsPctL - m_dispOutLufsPctL);
            if (lufsPctR > m_dispOutLufsPctR) m_dispOutLufsPctR = lufsPctR;
            else m_dispOutLufsPctR += alpha * (lufsPctR - m_dispOutLufsPctR);
        }

        m_inputMeterBarL->setValue(dbToMeterPct(m_dispInPeakDbfs));
        m_inputMeterBarR->setValue(dbToMeterPct(m_dispInPeakDbfsR));
        m_outputMeterBarL->setValue(dbToMeterPct(m_dispOutPeakDbfs));
        m_outputMeterBarR->setValue(dbToMeterPct(m_dispOutPeakDbfsR));
        m_outputLufsBarL->setValue(static_cast<int>(m_dispOutLufsPctL));
        m_outputLufsBarR->setValue(static_cast<int>(m_dispOutLufsPctR));

        // Dedicated bipolar meters for each leveler's live gain — a standalone
        // bar (not an overlay) so there's never any ambiguity about whether
        // it's showing something: gray/centered = neutral, green = boosting,
        // red = cutting. Forced to neutral while the leveler is off.
        if (m_inputGainMeter) {
            m_inputGainMeter->setGainDb(
                m_dspController->levelerEnabled() ? m_dspController->levelerGainDb() : 0.0);
        }
        if (m_outputGainMeter) {
            m_outputGainMeter->setGainDb(
                m_dspController->outputLevelerEnabled() ? m_dspController->outputLevelerGainDb() : 0.0);
        }

        // Treat anything below -100 dBFS as silence — exponential decay would
        // otherwise asymptote toward -120 forever and never display "-inf".
        if (m_dispOutRmsDbfs > ui::widget_metrics::meter_runtime::kSilenceDisplayDbfs) {
            const float vu = m_dispOutRmsDbfs + 18.0f;       // 0 VU ~= -18 dBFS reference
            setLabelText(m_outputVuLabel, QStringLiteral("VU: %1").arg(vu, 0, 'f', 1));
        } else {
            setLabelText(m_outputVuLabel, QStringLiteral("VU: -inf"));
        }
        const float lufsM = m_dspController->outLufsM();
        if (lufsM > ui::widget_metrics::meter_runtime::kLufsDisplayFloor)
            setLabelText(m_outputLufsLabel, QStringLiteral("LUFS-M: %1").arg(lufsM, 0, 'f', 1));
        else
            setLabelText(m_outputLufsLabel, QStringLiteral("LUFS-M: -inf"));

        if (m_levelerGainLabel) {
            const float g = m_dspController->levelerGainDb();
            const QChar sign = g >= 0.0f ? QLatin1Char('+') : QLatin1Char('-');
            setLabelText(m_levelerGainLabel, QStringLiteral("%1%2 dB")
                .arg(sign).arg(std::fabs(g), 0, 'f', 1));
        }

        if (m_spectralGainMeter) {
            std::array<float, dsp::kSpectralLevelerBandCount> gains{};
            m_dspController->spectralLevelerGainDb(gains);
            m_spectralGainMeter->setGainsDb(gains, m_dspController->spectralLevelerEnabled());
        }

        if (m_outputLevelerGainLabel) {
            const float g = m_dspController->outputLevelerGainDb();
            const QChar sign = g >= 0.0f ? QLatin1Char('+') : QLatin1Char('-');
            setLabelText(m_outputLevelerGainLabel, QStringLiteral("%1%2 dB")
                .arg(sign).arg(std::fabs(g), 0, 'f', 1));
        }

        const float hotDbfs = m_dispOutHotDbfs;
        int hotState = 0;
        QString hotText;
        if (hotDbfs > -0.2f) {
            hotState = 2;
            hotText = QStringLiteral("Output HOT: %1 dBFS").arg(hotDbfs, 0, 'f', 2);
        } else if (hotDbfs > -1.0f) {
            hotState = 1;
            hotText = QStringLiteral("Output near limit: %1 dBFS").arg(hotDbfs, 0, 'f', 2);
        } else {
            hotText = QStringLiteral("Output headroom: OK");
        }
        setLabelText(m_outputHotIndicator, std::move(hotText));
        if (hotState != m_outputHotState) {
            static constexpr const char *kHotStateNames[] = {"ok", "warn", "hot"};
            m_outputHotState = hotState;
            m_outputHotIndicator->setProperty("hotState", kHotStateNames[hotState]);
            repolish(m_outputHotIndicator);
        }

        if (m_selectedEqBand >= 0 && m_selectedEqBand < dsp::kEqBandCount) {
            const dsp::EqBandView v = m_dspController->eqBandView(m_selectedEqBand);
            setLabelText(m_eqDynMeter,
                         QStringLiteral("GR %1 dB").arg(v.dynGainReductionDb, 0, 'f', 1));
            m_eqDynInputMeter->setLevelDbfs(m_dispInPeakDbfs);
            m_eqDynOutputMeter->setLevelDbfs(m_dispOutPeakDbfs);
            m_eqDynGrMeter->setReductionDb(std::abs(v.dynGainReductionDb));
        }

        // Dynamic gain reduction changes continuously even when controls are
        // static. EqCurve ignores unchanged snapshots and caches response
        // geometry, so spectrum-only paints do not repeat the filter math.
        refreshEqCurve();
    });

    connectPlatformSignals();
}

void MainWindow::pullStateFromController()
{
    if (!m_dspController) return;

    m_syncingUi = true;

    m_globalBypass->setChecked(m_dspController->bypass());
    m_inputTrim->setValue(m_dspController->inputTrimDb());
    m_outputTrim->setValue(m_dspController->outputTrimDb());
    m_stereoWidth->setValue(m_dspController->stereoWidth() * 100.0f);
    m_levelerEnabled->setChecked(m_dspController->levelerEnabled());
    if (m_spectralLevelerEnabled)
        m_spectralLevelerEnabled->setChecked(m_dspController->spectralLevelerEnabled());
    if (m_outputLevelerEnabled)
        m_outputLevelerEnabled->setChecked(m_dspController->outputLevelerEnabled());

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
        // keep per-band tab visual in sync
    }

    m_selectedEqBand = std::clamp(m_selectedEqBand, 0, dsp::kEqBandCount - 1);
    const dsp::EqBandView &selected = views[m_selectedEqBand];
    m_eqBandEnabled->setChecked(selected.enabled);
    m_eqDynThreshold->setValue(selected.dynThresholdDb);
    m_eqDynRatio->setValue(selected.dynRatio);
    m_eqDynAttack->setValue(selected.dynAttackMs);
    m_eqDynRelease->setValue(selected.dynReleaseMs);
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
    m_eqBandEnabled->setChecked(v.enabled);
    m_eqDynThreshold->setValue(v.dynThresholdDb);
    m_eqDynRatio->setValue(v.dynRatio);
    m_eqDynAttack->setValue(v.dynAttackMs);
    m_eqDynRelease->setValue(v.dynReleaseMs);
    m_eqDynMeter->setText(QStringLiteral("GR %1 dB").arg(v.dynGainReductionDb, 0, 'f', 1));
    m_syncingUi = was;
}
