#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantList>

#include <array>
#include <vector>

#include "dsp/ChainParams.h"
#include "dsp/SpectralLeveler.h"       // dsp::SpectralLeveler::kBandCount
#include "editor/Transport.h"
#include "shared/TeeDspParams.h"   // teedsp::kBandCount


namespace dsp {

// Band count, mirrored from the shared param contract. Kept as a dsp:: name so
// existing call sites read unchanged; processing runs in a platform host.
inline constexpr int kEqBandCount = teedsp::kBandCount;
inline constexpr int kSpectralLevelerBandCount = SpectralLeveler::kBandCount;

// Lightweight POD view of a single EQ band, for high-frequency UI reads
// (paint loops). Intentionally NOT exposed via QVariant — direct field access
// avoids QVariantMap allocation and QString hashing on every meter tick.
struct EqBandView {
    bool  enabled;
    int   type;
    float freqHz;
    float q;
    float gainDb;
    float dynThresholdDb;
    float dynRatio;
    float dynAttackMs;
    float dynReleaseMs;
    float dynRangeDb;
    float dynGainReductionDb;
};

// Shared editable view of the processor. A transport owns persistence,
// delivery and telemetry for its platform.
class DspController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool bypass READ bypass WRITE setBypass NOTIFY bypassChanged FINAL)
    Q_PROPERTY(float inputTrimDb READ inputTrimDb WRITE setInputTrimDb NOTIFY bypassChanged FINAL)
    Q_PROPERTY(float outputTrimDb READ outputTrimDb WRITE setOutputTrimDb NOTIFY bypassChanged FINAL)
    Q_PROPERTY(float stereoWidth READ stereoWidth WRITE setStereoWidth NOTIFY bypassChanged FINAL)
    Q_PROPERTY(bool levelerEnabled READ levelerEnabled WRITE setLevelerEnabled NOTIFY levelerChanged FINAL)
    Q_PROPERTY(float levelerGainDb READ levelerGainDb NOTIFY meterChanged FINAL)
    Q_PROPERTY(bool spectralLevelerEnabled READ spectralLevelerEnabled WRITE setSpectralLevelerEnabled NOTIFY levelerChanged FINAL)
    Q_PROPERTY(bool outputLevelerEnabled READ outputLevelerEnabled WRITE setOutputLevelerEnabled NOTIFY levelerChanged FINAL)
    Q_PROPERTY(float outputLevelerGainDb READ outputLevelerGainDb NOTIFY meterChanged FINAL)

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
    explicit DspController(QObject *parent = nullptr);

    ChainParams buildSnapshot() const;
    editor::Transport *transport() const { return m_transport; }

    // Live meter snapshot in dBFS / LUFS. ch: 0=L, 1=R.
    float inPeakDbfs(int ch) const;
    float outPeakDbfs(int ch) const;
    float outRmsDbfs() const;
    float outLufs(int ch) const;   // per-channel momentary LUFS
    float outLufsM() const;        // combined momentary LUFS

    bool bypass() const;
    void setBypass(bool b);
    float inputTrimDb() const { return m_inputTrimDb; }
    void setInputTrimDb(float v);
    float outputTrimDb() const { return m_outputTrimDb; }
    void setOutputTrimDb(float v);
    float stereoWidth() const { return m_stereoWidth; }
    void setStereoWidth(float v);
    bool levelerEnabled() const { return m_levelerEnabled; }
    void setLevelerEnabled(bool b);
    float levelerGainDb() const;
    void spectralLevelerGainDb(std::array<float, kSpectralLevelerBandCount> &out) const;
    bool spectralLevelerEnabled() const { return m_spectralLevelerEnabled; }
    void setSpectralLevelerEnabled(bool b);
    bool outputLevelerEnabled() const { return m_outputLevelerEnabled; }
    void setOutputLevelerEnabled(bool b);
    float outputLevelerGainDb() const;

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

    // Typed snapshot — preferred for high-frequency UI reads.
    void eqBandViews(std::array<EqBandView, kEqBandCount> &out) const;
    EqBandView eqBandView(int band) const;

    Q_INVOKABLE void setEqBandEnabled(int band, bool enabled);
    Q_INVOKABLE void setEqBandType(int band, int type);
    Q_INVOKABLE void setEqBandFrequency(int band, float hz);
    Q_INVOKABLE void setEqBandQ(int band, float q);
    Q_INVOKABLE void setEqBandGainDb(int band, float gainDb);
    void setEqBandShape(int band, float hz, float gainDb);
    Q_INVOKABLE void setEqBandDynamicThresholdDb(int band, float thresholdDb);
    Q_INVOKABLE void setEqBandDynamicRatio(int band, float ratio);
    Q_INVOKABLE void setEqBandDynamicAttackMs(int band, float attackMs);
    Q_INVOKABLE void setEqBandDynamicReleaseMs(int band, float releaseMs);
    Q_INVOKABLE void setEqBandDynamicRangeDb(int band, float rangeDb);
    Q_INVOKABLE void resetBandToDefaults(int band);
    // Resets only the EQ-shape params (enabled / type / freq / Q / gain).
    // Leaves the per-band dynamics state untouched — the user often spends
    // real time dialing in dynamics and shouldn't lose it from a stray
    // double-click on the EQ node.
    Q_INVOKABLE void resetBandEqToDefaults(int band);
    Q_INVOKABLE void resetToDefaults();

    void start();
    void flush();

    // Pauses/resumes the meter tick. Used by the UI to silence the ~60 Hz
    // meter→widget repaint chain while the window is hidden or minimized.
    // Audio processing and parameter state are unaffected; meters resume from
    // current atomic state on the next tick after resume.
    void setMeterTimerActive(bool active);

    // Switches transport polling between visible and background cadences.
    void setEditorVisible(bool visible);

private slots:
    void submitChanges();

signals:
    void bypassChanged();
    void compressorChanged();
    void exciterChanged();
    void eqChanged();
    void levelerChanged();
    void meterChanged();

private:
    void applySnapshot(const ChainParams &params);
    // Editable EQ band cache; the transport delivers snapshots and telemetry.
    EqBandParams      m_eqBands[kEqBandCount];
    QTimer m_meterTimer;
    bool m_loadingSettings = false;
    editor::Transport *m_transport = nullptr;
    editor::Meters m_meterSnapshot;
    QTimer m_submitTimer;
    bool m_dirty = false;

    bool m_bypass = false;
    float m_inputTrimDb = 0.0f;
    float m_outputTrimDb = 0.0f;
    float m_stereoWidth = 1.0f;
    bool m_levelerEnabled = false;
    bool m_spectralLevelerEnabled = false;
    bool m_outputLevelerEnabled = false;

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
