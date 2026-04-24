#pragma once

#include <QMainWindow>
#include <QVector>

class QLabel;
class QPushButton;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;

namespace dsp {
class DspController;
class ApoSharedClient;
}

class SystemOutputController;

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
        QDoubleSpinBox *frequency = nullptr;
        QDoubleSpinBox *q = nullptr;
        QDoubleSpinBox *gain = nullptr;
    };

    void buildUi();
    void connectSignals();
    void pullStateFromController();
    void refreshSystemStatus();
    void onInstallClicked();
    void onResetDefaultsClicked();

    QWidget *m_central = nullptr;

    QLabel *m_statusLabel = nullptr;
    QLabel *m_detailLabel = nullptr;
    QLabel *m_errorLabel = nullptr;
    QPushButton *m_installButton = nullptr;

    QCheckBox *m_globalBypass = nullptr;

    QCheckBox *m_compEnabled = nullptr;
    QDoubleSpinBox *m_compThreshold = nullptr;
    QDoubleSpinBox *m_compRatio = nullptr;
    QDoubleSpinBox *m_compKnee = nullptr;
    QDoubleSpinBox *m_compAttack = nullptr;
    QDoubleSpinBox *m_compRelease = nullptr;
    QDoubleSpinBox *m_compMakeup = nullptr;
    QLabel *m_compGainReductionLabel = nullptr;

    QCheckBox *m_exciterEnabled = nullptr;
    QDoubleSpinBox *m_exciterDrive = nullptr;
    QDoubleSpinBox *m_exciterMix = nullptr;
    QDoubleSpinBox *m_exciterTone = nullptr;

    QCheckBox *m_eqEnabled = nullptr;
    QVector<EqBandWidgets> m_eqBands;

    QPushButton *m_resetButton = nullptr;

    dsp::ApoSharedClient *m_apoClient = nullptr;
    dsp::DspController *m_dspController = nullptr;
    SystemOutputController *m_systemOutput = nullptr;

    bool m_syncingUi = false;
};
