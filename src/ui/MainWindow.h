#pragma once

#include "../host/WasapiDevices.h"

#include <QMainWindow>
#include <QVector>

class QLabel;
class QPushButton;
class QCheckBox;
class QComboBox;
class QProgressBar;

namespace dsp {
class DspController;
class ProcessorChain;
}

namespace host {
class AudioEngine;
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

private:
    struct EqBandWidgets {
        QCheckBox *enabled = nullptr;
    };

    void buildUi();
    QWidget *buildIoSection();
    QWidget *buildEqSection();
    QWidget *buildCompSection();
    QWidget *buildExciterSection();
    QWidget *buildInputPane();
    QWidget *buildOutputPane();
    QWidget *buildFooter();

    void connectSignals();
    void pullStateFromController();
    void refreshDevices();
    void refreshEngineStatus();
    void refreshEqCurve();
    // Lightweight refresh of just the dynamic knobs/labels for the currently
    // selected band — used on band-selection changes to avoid a full UI sync.
    void syncSelectedBandDyn();

    void onStartStopClicked();
    void onEngineError(const QString &message);

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

    QProgressBar *m_inputMeterBar = nullptr;
    ui::Knob *m_inputTrim = nullptr;

    QProgressBar *m_outputMeterBar = nullptr;
    QLabel *m_outputVuLabel = nullptr;
    QLabel *m_outputLufsLabel = nullptr;
    ui::Knob *m_outputTrim = nullptr;

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

    QCheckBox *m_exciterEnabled = nullptr;
    ui::Knob *m_exciterDrive = nullptr;
    ui::Knob *m_exciterMix = nullptr;
    ui::Knob *m_exciterTone = nullptr;

    QCheckBox *m_eqEnabled = nullptr;
    QCheckBox *m_showInputSpectrum = nullptr;
    QCheckBox *m_showOutputSpectrum = nullptr;
    QCheckBox *m_showHeatmap = nullptr;
    QLabel *m_eqSelectedBand = nullptr;
    ui::Knob *m_eqDynThreshold = nullptr;
    ui::Knob *m_eqDynRatio = nullptr;
    ui::Knob *m_eqDynAttack = nullptr;
    ui::Knob *m_eqDynRelease = nullptr;
    ui::Knob *m_eqDynRange = nullptr;
    QLabel *m_eqDynMeter = nullptr;
    ui::EqCurve *m_eqCurve = nullptr;
    QVector<EqBandWidgets> m_eqBands;
    int m_selectedEqBand = 0;


    dsp::ProcessorChain *m_chain = nullptr;
    dsp::DspController *m_dspController = nullptr;
    host::AudioEngine *m_engine = nullptr;
    ui::TrayController *m_tray = nullptr;

    QList<host::DeviceInfo> m_devices;
    bool m_syncingUi = false;
    bool m_quitting = false;

    // Smoothed meter state. Each tick: instant attack, exponential release
    // with kMeterReleaseTauMs time-constant. Avoids alternating -inf frames
    // when meter polling outpaces the WASAPI packet rate.
    float m_dispInPeakDbfs = -120.0f;
    float m_dispOutPeakDbfs = -120.0f;
    float m_dispOutRmsDbfs = -120.0f;
    float m_dispOutHotDbfs = -120.0f;
    qint64 m_lastMeterTickMs = 0;
};
