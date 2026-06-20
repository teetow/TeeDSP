#pragma once

#include "../host/WasapiDevices.h"

#include <QMainWindow>
#include <QTimer>
#include <QVector>

#include <vector>

class QLabel;
class QPushButton;
class QCheckBox;
class QComboBox;
class QProgressBar;

namespace dsp {
class DspController;
}

namespace host {
class SpectrumAnalyzer;
}

namespace ui {
class Knob;
class LevelMeter;
class EqCurve;
class TrayController;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    struct EqBandWidgets {};

    void buildUi();
    QWidget *buildIoSection();
    QWidget *buildEqSection();
    QWidget *buildCompSection();
    QWidget *buildChannelMixerSection();
    QWidget *buildExciterSection();
    QWidget *buildInputPane();
    QWidget *buildOutputPane();

    void connectSignals();
    void pullStateFromController();
    void refreshDevices();
    void syncDevicePickerToDefaultOutput(const QString &deviceId);
    void refreshEngineStatus();
    void refreshEqCurve();
    // Lightweight refresh of just the dynamic knobs/labels for the currently
    // selected band — used on band-selection changes to avoid a full UI sync.
    void syncSelectedBandDyn();

    // Pauses/resumes the analyzer + meter timers based on whether the window
    // is actually being looked at — no point burning FFTs/repaints when hidden.
    void updateUiTimerGate();

    QString selectedCaptureDeviceId() const;
    QString selectedRenderDeviceId() const;
    void saveSelectedDevices() const;
    void restoreSelectedDevices();

    QWidget *m_central = nullptr;

    QComboBox *m_captureDevice = nullptr;
    QComboBox *m_renderDevice = nullptr;
    QPushButton *m_refreshDevicesButton = nullptr;
    QPushButton *m_startStopButton = nullptr;
    QLabel *m_statusLabel = nullptr;

    QProgressBar *m_inputMeterBarL = nullptr;
    QProgressBar *m_inputMeterBarR = nullptr;
    ui::Knob *m_inputTrim = nullptr;
    QCheckBox *m_levelerEnabled = nullptr;
    QLabel *m_levelerGainLabel = nullptr;

    QProgressBar *m_outputMeterBarL = nullptr;
    QProgressBar *m_outputMeterBarR = nullptr;
    QProgressBar *m_outputLufsBarL  = nullptr;
    QProgressBar *m_outputLufsBarR  = nullptr;
    QLabel *m_outputVuLabel = nullptr;
    QLabel *m_outputLufsLabel = nullptr;
    ui::Knob *m_outputTrim = nullptr;
    QCheckBox *m_outputLevelerEnabled = nullptr;
    QLabel *m_outputLevelerGainLabel = nullptr;

    QCheckBox *m_globalBypass = nullptr;

    QCheckBox *m_compEnabled = nullptr;
    ui::Knob *m_compThreshold = nullptr;
    ui::Knob *m_compRatio = nullptr;
    ui::Knob *m_compKnee = nullptr;
    ui::Knob *m_compAttack = nullptr;
    ui::Knob *m_compRelease = nullptr;
    ui::Knob *m_compMakeup = nullptr;
    ui::LevelMeter *m_compMeter = nullptr;
    QLabel *m_compMeterValue = nullptr;
    QLabel *m_outputHotIndicator = nullptr;
    ui::Knob *m_stereoWidth = nullptr;

    QCheckBox *m_exciterEnabled = nullptr;
    ui::Knob *m_exciterDrive = nullptr;
    ui::Knob *m_exciterMix = nullptr;
    ui::Knob *m_exciterTone = nullptr;

    QCheckBox *m_eqEnabled = nullptr;
    QCheckBox *m_showInputSpectrum = nullptr;
    QCheckBox *m_showOutputSpectrum = nullptr;
    QCheckBox *m_showHeatmap = nullptr;
    QCheckBox *m_eqBandEnabled = nullptr;
    QVector<QPushButton *> m_eqBandTabs;
    ui::Knob *m_eqDynThreshold = nullptr;
    ui::Knob *m_eqDynRatio = nullptr;
    ui::Knob *m_eqDynAttack = nullptr;
    ui::Knob *m_eqDynRelease = nullptr;
    ui::Knob *m_eqDynRange = nullptr;
    QLabel *m_eqDynMeter = nullptr;
    ui::LevelMeter *m_eqDynInputMeter = nullptr;
    ui::LevelMeter *m_eqDynOutputMeter = nullptr;
    ui::LevelMeter *m_eqDynGrMeter = nullptr;
    ui::EqCurve *m_eqCurve = nullptr;
    QVector<EqBandWidgets> m_eqBands;
    int m_selectedEqBand = 0;


    dsp::DspController *m_dspController = nullptr;
    ui::TrayController *m_tray = nullptr;

    QList<host::DeviceInfo> m_inputDevices;
    QList<host::DeviceInfo> m_outputDevices;
    bool m_syncingUi = false;
    bool m_quitting = false;

    // Coalesces resize/move events into a single QSettings write a moment
    // after the user stops dragging. Without this, geometry would only persist
    // on a clean close — a crash or force-quit would lose layout changes.
    QTimer m_geometrySaveTimer;

    // Smoothed meter state. Each tick: instant attack, exponential release
    // with kMeterReleaseTauMs time-constant. Avoids alternating -inf frames
    // when meter polling outpaces the WASAPI packet rate.
    float m_dispInPeakDbfs = -120.0f;
    float m_dispInPeakDbfsR = -120.0f;
    float m_dispOutPeakDbfs = -120.0f;
    float m_dispOutPeakDbfsR = -120.0f;
    float m_dispOutRmsDbfs = -120.0f;
    float m_dispOutHotDbfs = -120.0f;
    float m_dispOutLufsPctL = 0.0f;
    float m_dispOutLufsPctR = 0.0f;
    qint64 m_lastMeterTickMs = 0;

    // Polls the APO's shared telemetry to drive the status line (and tray text),
    // independent of the engine — the engine is retired from the APO path.
    QTimer m_apoStatusTimer;
    unsigned long long m_lastApoProcessCalls = 0;

    // Spectrum: drain the APO's pre/post sample ring and feed the analyzer
    // (whose spectraUpdated drives the EqCurve overlay + heatmap).
    void onSpectrumTick();
    host::SpectrumAnalyzer *m_analyzer = nullptr;
    QTimer m_spectrumTimer;
    bool   m_analyzerStarted = false;
    double m_analyzerSr = 0.0;
    std::vector<float> m_specPre;
    std::vector<float> m_specPost;
};
