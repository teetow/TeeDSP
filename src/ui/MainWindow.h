#pragma once

#include "../host/WasapiDevices.h"

#include <QMainWindow>
#include <QVector>

class QLabel;
class QPushButton;
class QCheckBox;
class QComboBox;

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
        QComboBox *type = nullptr;
        ui::Knob *frequency = nullptr;
        ui::Knob *q = nullptr;
        ui::Knob *gain = nullptr;
    };

    void buildUi();
    QWidget *buildIoSection();
    QWidget *buildEqSection();
    QWidget *buildCompSection();
    QWidget *buildExciterSection();
    QWidget *buildFooter();

    void connectSignals();
    void pullStateFromController();
    void refreshDevices();
    void refreshEngineStatus();
    void refreshEqCurve();

    void onStartStopClicked();
    void onResetDefaultsClicked();
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

    QCheckBox *m_exciterEnabled = nullptr;
    ui::Knob *m_exciterDrive = nullptr;
    ui::Knob *m_exciterMix = nullptr;
    ui::Knob *m_exciterTone = nullptr;

    QCheckBox *m_eqEnabled = nullptr;
    QCheckBox *m_showInputSpectrum = nullptr;
    QCheckBox *m_showOutputSpectrum = nullptr;
    ui::EqCurve *m_eqCurve = nullptr;
    QVector<EqBandWidgets> m_eqBands;

    QPushButton *m_resetButton = nullptr;

    dsp::ProcessorChain *m_chain = nullptr;
    dsp::DspController *m_dspController = nullptr;
    host::AudioEngine *m_engine = nullptr;

    QList<host::DeviceInfo> m_devices;
    bool m_syncingUi = false;
};
