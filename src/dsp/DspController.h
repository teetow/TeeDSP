#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantList>

#include "ChainParams.h"
#include "ProcessorChain.h"

namespace dsp {

// QObject wrapper that owns the ProcessorChain and exposes its parameters
// to QML. The audio thread reads parameters via std::atomic loads inside
// each Processor; this controller only writes them, so cross-thread access
// is safe without additional locking.
class DspController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool bypass READ bypass WRITE setBypass NOTIFY bypassChanged FINAL)
    Q_PROPERTY(float inputTrimDb READ inputTrimDb WRITE setInputTrimDb NOTIFY bypassChanged FINAL)
    Q_PROPERTY(float outputTrimDb READ outputTrimDb WRITE setOutputTrimDb NOTIFY bypassChanged FINAL)

    Q_PROPERTY(bool compressorEnabled READ compressorEnabled WRITE setCompressorEnabled NOTIFY compressorChanged FINAL)
    Q_PROPERTY(float compThresholdDb READ compThresholdDb WRITE setCompThresholdDb NOTIFY compressorChanged FINAL)
    Q_PROPERTY(float compRatio READ compRatio WRITE setCompRatio NOTIFY compressorChanged FINAL)
    Q_PROPERTY(float compKneeDb READ compKneeDb WRITE setCompKneeDb NOTIFY compressorChanged FINAL)
    Q_PROPERTY(float compAttackMs READ compAttackMs WRITE setCompAttackMs NOTIFY compressorChanged FINAL)
    Q_PROPERTY(float compReleaseMs READ compReleaseMs WRITE setCompReleaseMs NOTIFY compressorChanged FINAL)
    Q_PROPERTY(float compMakeupDb READ compMakeupDb WRITE setCompMakeupDb NOTIFY compressorChanged FINAL)
    Q_PROPERTY(float compGainReductionDb READ compGainReductionDb NOTIFY meterChanged FINAL)

    Q_PROPERTY(bool exciterEnabled READ exciterEnabled WRITE setExciterEnabled NOTIFY exciterChanged FINAL)
    Q_PROPERTY(float exciterDrive READ exciterDrive WRITE setExciterDrive NOTIFY exciterChanged FINAL)
    Q_PROPERTY(float exciterMix READ exciterMix WRITE setExciterMix NOTIFY exciterChanged FINAL)
    Q_PROPERTY(float exciterToneHz READ exciterToneHz WRITE setExciterToneHz NOTIFY exciterChanged FINAL)

    Q_PROPERTY(bool eqEnabled READ eqEnabled WRITE setEqEnabled NOTIFY eqChanged FINAL)
    Q_PROPERTY(int eqBandCount READ eqBandCount CONSTANT FINAL)
    Q_PROPERTY(QVariantList eqBands READ eqBands NOTIFY eqChanged FINAL)

public:
    explicit DspController(ProcessorChain *chain, QObject *parent = nullptr);

    ChainParams buildSnapshot() const;

    bool bypass() const;
    void setBypass(bool b);
    float inputTrimDb() const { return m_inputTrimDb; }
    void setInputTrimDb(float v);
    float outputTrimDb() const { return m_outputTrimDb; }
    void setOutputTrimDb(float v);

    bool compressorEnabled() const;
    void setCompressorEnabled(bool b);
    float compThresholdDb() const { return m_compThresholdDb; }
    void setCompThresholdDb(float v);
    float compRatio() const { return m_compRatio; }
    void setCompRatio(float v);
    float compKneeDb() const { return m_compKneeDb; }
    void setCompKneeDb(float v);
    float compAttackMs() const { return m_compAttackMs; }
    void setCompAttackMs(float v);
    float compReleaseMs() const { return m_compReleaseMs; }
    void setCompReleaseMs(float v);
    float compMakeupDb() const { return m_compMakeupDb; }
    void setCompMakeupDb(float v);
    float compGainReductionDb() const;

    bool exciterEnabled() const;
    void setExciterEnabled(bool b);
    float exciterDrive() const { return m_exciterDrive; }
    void setExciterDrive(float v);
    float exciterMix() const { return m_exciterMix; }
    void setExciterMix(float v);
    float exciterToneHz() const { return m_exciterToneHz; }
    void setExciterToneHz(float v);

    bool eqEnabled() const;
    void setEqEnabled(bool b);
    int eqBandCount() const { return kEqBandCount; }
    QVariantList eqBands() const;

    Q_INVOKABLE void setEqBandEnabled(int band, bool enabled);
    Q_INVOKABLE void setEqBandType(int band, int type);
    Q_INVOKABLE void setEqBandFrequency(int band, float hz);
    Q_INVOKABLE void setEqBandQ(int band, float q);
    Q_INVOKABLE void setEqBandGainDb(int band, float gainDb);
    Q_INVOKABLE void setEqBandDynamicThresholdDb(int band, float thresholdDb);
    Q_INVOKABLE void setEqBandDynamicRatio(int band, float ratio);
    Q_INVOKABLE void setEqBandDynamicAttackMs(int band, float attackMs);
    Q_INVOKABLE void setEqBandDynamicReleaseMs(int band, float releaseMs);
    Q_INVOKABLE void setEqBandDynamicRangeDb(int band, float rangeDb);
    Q_INVOKABLE void resetBandToDefaults(int band);
    Q_INVOKABLE void resetToDefaults();

    void loadFromSettings();
    void saveToSettings() const;

signals:
    void bypassChanged();
    void compressorChanged();
    void exciterChanged();
    void eqChanged();
    void meterChanged();

private:
    void pushCompressorParams();
    void pushExciterParams();
    void applySnapshot(const ChainParams &params);

    ProcessorChain   *m_chain;
    QTimer m_meterTimer;

    bool m_bypass = false;
    float m_inputTrimDb = 0.0f;
    float m_outputTrimDb = 0.0f;

    bool m_compressorEnabled = true;
    float m_compThresholdDb = -18.0f;
    float m_compRatio = 4.0f;
    float m_compKneeDb = 6.0f;
    float m_compAttackMs = 10.0f;
    float m_compReleaseMs = 120.0f;
    float m_compMakeupDb = 0.0f;

    bool m_exciterEnabled = true;
    float m_exciterDrive = 2.0f;
    float m_exciterMix = 0.25f;
    float m_exciterToneHz = 3500.0f;

    bool m_eqEnabled = true;
};

} // namespace dsp
