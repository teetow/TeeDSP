#include "DspController.h"
#include "shared/TeeDspParams.h"
#include <QVariantMap>

namespace dsp {
namespace { constexpr int kMeterIntervalMs=16; ChainParams defaultParams(){return {}; } }

DspController::DspController(QObject *parent): QObject(parent) {
    const ChainParams def;
    for(int i=0;i<kEqBandCount;++i) m_eqBands[i]=def.eqBands[i];
    m_transport=editor::createTransport(this);
    connect(m_transport,&editor::Transport::parametersReceived,this,[this](const ChainParams &p) {
        // The processor owns its accepted settings, including on first load.
        m_loadingSettings=true;
        applySnapshot(p);
        m_loadingSettings=false;
        m_dirty=false;
    });
    const auto dirty=[this] {if(!m_loadingSettings) m_dirty=true;};
    connect(this,&DspController::bypassChanged,this,dirty);
    connect(this,&DspController::compressorChanged,this,dirty);
    connect(this,&DspController::exciterChanged,this,dirty);
    connect(this,&DspController::eqChanged,this,dirty);
    connect(this,&DspController::levelerChanged,this,dirty);
    m_submitTimer.setInterval(50);
    connect(&m_submitTimer,&QTimer::timeout,this,&DspController::submitChanges);
    m_submitTimer.start();
    m_meterTimer.setInterval(kMeterIntervalMs);
    m_meterTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_meterTimer,&QTimer::timeout,this,[this] {
        m_meterSnapshot=m_transport->meters();
        emit meterChanged();
    });
    m_meterTimer.start();
}
void DspController::start() { m_transport->start(); }
void DspController::submitChanges() {
    if(m_dirty && !m_loadingSettings && m_transport->ready()) {
        m_transport->submit(buildSnapshot());
        m_dirty=false;
    }
}
void DspController::flush() {
    submitChanges();
    m_transport->flush();
}
void DspController::setMeterTimerActive(bool active) {
    if(active) {if(!m_meterTimer.isActive())m_meterTimer.start();}
    else m_meterTimer.stop();
}
void DspController::setEditorVisible(bool visible) {
    setMeterTimerActive(visible);
    m_transport->setEditorVisible(visible);
}

bool DspController::bypass() const { return m_bypass; }

void DspController::setBypass(bool b)
{
    if (m_bypass == b) return;
    m_bypass = b;
    emit bypassChanged();
    submitChanges();   // discrete toggle: push now, don't wait for the batch timer
}

void DspController::setInputTrimDb(float v)
{
    if (m_inputTrimDb == v) return;
    m_inputTrimDb = v;
    emit bypassChanged();
}

void DspController::setOutputTrimDb(float v)
{
    if (m_outputTrimDb == v) return;
    m_outputTrimDb = v;
    emit bypassChanged();
}

void DspController::setStereoWidth(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (m_stereoWidth == v) return;
    m_stereoWidth = v;
    emit bypassChanged();
}

void DspController::setLevelerEnabled(bool b)
{
    if (m_levelerEnabled == b) return;
    m_levelerEnabled = b;
    emit levelerChanged();
    submitChanges();   // discrete toggle: push now, don't wait for the batch timer
}

void DspController::relearnLeveler(bool output)
{
    submitChanges();
    if (m_transport->ready()) m_transport->relearnLeveler(output);
}

float DspController::levelerGainDb() const
{
    return m_meterSnapshot.levelerGainDb;
}

void DspController::spectralLevelerGainDb(
    std::array<float, kSpectralLevelerBandCount> &out) const
{
    static_assert(editor::Meters::kSpectralBandCount
                  == kSpectralLevelerBandCount,
                  "Meter spectral band count must match SpectralLeveler");
    for (int b = 0; b < kSpectralLevelerBandCount; ++b)
        out[b] = m_meterSnapshot.spectralGainDb[b];
}

void DspController::setSpectralLevelerEnabled(bool b)
{
    if (m_spectralLevelerEnabled == b) return;
    m_spectralLevelerEnabled = b;
    emit levelerChanged();
    submitChanges();   // discrete toggle: push now, don't wait for the batch timer
}

void DspController::setOutputLevelerEnabled(bool b)
{
    if (m_outputLevelerEnabled == b) return;
    m_outputLevelerEnabled = b;
    emit levelerChanged();
    submitChanges();   // discrete toggle: push now, don't wait for the batch timer
}

float DspController::outputLevelerGainDb() const
{
    return m_meterSnapshot.outLevelerGainDb;
}

bool DspController::compressorEnabled() const { return m_compressorEnabled; }

void DspController::setCompressorEnabled(bool b)
{
    if (m_compressorEnabled == b) return;
    m_compressorEnabled = b;
    emit compressorChanged();
    submitChanges();   // discrete toggle: push now, don't wait for the batch timer
}

void DspController::setCompThresholdDb(float v)
{
    if (m_compThresholdDb == v) return;
    m_compThresholdDb = v;
    emit compressorChanged();
}

void DspController::setCompRatio(float v)
{
    if (v < 1.0f) v = 1.0f;
    if (m_compRatio == v) return;
    m_compRatio = v;
    emit compressorChanged();
}

void DspController::setCompKneeDb(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (m_compKneeDb == v) return;
    m_compKneeDb = v;
    emit compressorChanged();
}

void DspController::setCompAttackMs(float v)
{
    if (v < 0.1f) v = 0.1f;
    if (m_compAttackMs == v) return;
    m_compAttackMs = v;
    emit compressorChanged();
}

void DspController::setCompReleaseMs(float v)
{
    if (v < 1.0f) v = 1.0f;
    if (m_compReleaseMs == v) return;
    m_compReleaseMs = v;
    emit compressorChanged();
}

void DspController::setCompMakeupDb(float v)
{
    if (m_compMakeupDb == v) return;
    m_compMakeupDb = v;
    emit compressorChanged();
}

float DspController::compGainReductionDb() const
{
    return m_meterSnapshot.compGrDb;
}

float DspController::inPeakDbfs(int ch) const
{
    return m_meterSnapshot.inPeakDbfs[(ch == 1) ? 1 : 0];
}

float DspController::outPeakDbfs(int ch) const
{
    return m_meterSnapshot.outPeakDbfs[(ch == 1) ? 1 : 0];
}

float DspController::outRmsDbfs() const
{
    return m_meterSnapshot.outRmsDbfs;
}

float DspController::outLufs(int ch) const
{
    return m_meterSnapshot.outLufsCh[(ch == 1) ? 1 : 0];
}

float DspController::outLufsM() const
{
    return m_meterSnapshot.outLufsM;
}

bool DspController::exciterEnabled() const { return m_exciterEnabled; }

void DspController::setExciterEnabled(bool b)
{
    if (m_exciterEnabled == b) return;
    m_exciterEnabled = b;
    emit exciterChanged();
    submitChanges();   // discrete toggle: push now, don't wait for the batch timer
}

void DspController::setExciterDrive(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 20.0f) v = 20.0f;
    if (m_exciterDrive == v) return;
    m_exciterDrive = v;
    emit exciterChanged();
}

void DspController::setExciterMix(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (m_exciterMix == v) return;
    m_exciterMix = v;
    emit exciterChanged();
}

void DspController::setExciterToneHz(float v)
{
    if (v < 200.0f) v = 200.0f;
    if (m_exciterToneHz == v) return;
    m_exciterToneHz = v;
    emit exciterChanged();
}

bool DspController::eqEnabled() const { return m_eqEnabled; }

void DspController::setEqEnabled(bool b)
{
    if (m_eqEnabled == b) return;
    m_eqEnabled = b;
    emit eqChanged();
    submitChanges();   // discrete toggle: push now, don't wait for the batch timer
}

// --- EQ band reads (cache for params, telemetry for GR) --------------------

QVariantList DspController::eqBands() const
{
    QVariantList list;
    for (int i = 0; i < kEqBandCount; ++i) {
        const EqBandParams &b = m_eqBands[i];
        QVariantMap map;
        map.insert(QStringLiteral("enabled"), b.enabled);
        map.insert(QStringLiteral("type"), b.type);
        map.insert(QStringLiteral("frequencyHz"), b.freqHz);
        map.insert(QStringLiteral("q"), b.q);
        map.insert(QStringLiteral("gainDb"), b.gainDb);
        map.insert(QStringLiteral("dynThresholdDb"), b.dynThresholdDb);
        map.insert(QStringLiteral("dynRatio"), b.dynRatio);
        map.insert(QStringLiteral("dynAttackMs"), b.dynAttackMs);
        map.insert(QStringLiteral("dynReleaseMs"), b.dynReleaseMs);
        map.insert(QStringLiteral("dynRangeDb"), b.dynRangeDb);
        map.insert(QStringLiteral("dynGainReductionDb"), m_meterSnapshot.bandGrDb[i]);
        list.append(map);
    }
    return list;
}

void DspController::eqBandViews(std::array<EqBandView, kEqBandCount> &out) const
{
    for (int i = 0; i < kEqBandCount; ++i) {
        const EqBandParams &b = m_eqBands[i];
        EqBandView &v = out[i];
        v.enabled            = b.enabled;
        v.type               = b.type;
        v.freqHz             = b.freqHz;
        v.q                  = b.q;
        v.gainDb             = b.gainDb;
        v.dynThresholdDb     = b.dynThresholdDb;
        v.dynRatio           = b.dynRatio;
        v.dynAttackMs        = b.dynAttackMs;
        v.dynReleaseMs       = b.dynReleaseMs;
        v.dynRangeDb         = b.dynRangeDb;
        v.dynGainReductionDb = m_meterSnapshot.bandGrDb[i];
    }
}

EqBandView DspController::eqBandView(int band) const
{
    EqBandView v{};
    if (band < 0 || band >= kEqBandCount) return v;
    const EqBandParams &b = m_eqBands[band];
    v.enabled            = b.enabled;
    v.type               = b.type;
    v.freqHz             = b.freqHz;
    v.q                  = b.q;
    v.gainDb             = b.gainDb;
    v.dynThresholdDb     = b.dynThresholdDb;
    v.dynRatio           = b.dynRatio;
    v.dynAttackMs        = b.dynAttackMs;
    v.dynReleaseMs       = b.dynReleaseMs;
    v.dynRangeDb         = b.dynRangeDb;
    v.dynGainReductionDb = m_meterSnapshot.bandGrDb[band];
    return v;
}

// --- EQ band setters -------------------------------------------------------

void DspController::setEqBandEnabled(int band, bool enabled)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_eqBands[band].enabled = enabled;
    emit eqChanged();
    submitChanges();   // discrete toggle: push now, don't wait for the batch timer
}

void DspController::setEqBandType(int band, int type)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_eqBands[band].type = type;
    emit eqChanged();
}

void DspController::setEqBandFrequency(int band, float hz)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (hz < 10.0f) hz = 10.0f;
    if (m_eqBands[band].freqHz == hz) return;
    m_eqBands[band].freqHz = hz;
    emit eqChanged();
}

void DspController::setEqBandQ(int band, float q)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (q < 0.05f) q = 0.05f;
    if (m_eqBands[band].q == q) return;
    m_eqBands[band].q = q;
    emit eqChanged();
}

void DspController::setEqBandGainDb(int band, float gainDb)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (m_eqBands[band].gainDb == gainDb) return;
    m_eqBands[band].gainDb = gainDb;
    emit eqChanged();
}

void DspController::setEqBandShape(int band, float hz, float gainDb)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (hz < 10.0f) hz = 10.0f;
    EqBandParams &b = m_eqBands[band];
    if (b.freqHz == hz && b.gainDb == gainDb) return;
    b.freqHz = hz;
    b.gainDb = gainDb;
    emit eqChanged();
}

void DspController::setEqBandDynamicThresholdDb(int band, float thresholdDb)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_eqBands[band].dynThresholdDb = thresholdDb;
    emit eqChanged();
}

void DspController::setEqBandDynamicRatio(int band, float ratio)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (ratio < 1.0f) ratio = 1.0f;
    m_eqBands[band].dynRatio = ratio;
    emit eqChanged();
}

void DspController::setEqBandDynamicAttackMs(int band, float attackMs)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (attackMs < 0.1f) attackMs = 0.1f;
    m_eqBands[band].dynAttackMs = attackMs;
    emit eqChanged();
}

void DspController::setEqBandDynamicReleaseMs(int band, float releaseMs)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (releaseMs < 1.0f) releaseMs = 1.0f;
    m_eqBands[band].dynReleaseMs = releaseMs;
    emit eqChanged();
}

void DspController::setEqBandDynamicRangeDb(int band, float rangeDb)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (rangeDb < 0.0f) rangeDb = 0.0f;
    m_eqBands[band].dynRangeDb = rangeDb;
    emit eqChanged();
}

// --- snapshots / presets / persistence -------------------------------------

ChainParams DspController::buildSnapshot() const
{
    ChainParams p;
    p.bypassed        = m_bypass;
    p.inputTrimDb     = m_inputTrimDb;
    p.outputTrimDb    = m_outputTrimDb;
    p.stereoWidth     = m_stereoWidth;
    p.levelerEnabled  = m_levelerEnabled;
    p.spectralLevelerEnabled = m_spectralLevelerEnabled;
    p.outputLevelerEnabled = m_outputLevelerEnabled;
    p.eqEnabled       = m_eqEnabled;
    p.compEnabled     = m_compressorEnabled;
    p.compThreshDb    = m_compThresholdDb;
    p.compRatio       = m_compRatio;
    p.compKneeDb      = m_compKneeDb;
    p.compAttackMs    = m_compAttackMs;
    p.compReleaseMs   = m_compReleaseMs;
    p.compMakeupDb    = m_compMakeupDb;
    p.exciterEnabled  = m_exciterEnabled;
    p.exciterDrive    = m_exciterDrive;
    p.exciterMix      = m_exciterMix;
    p.exciterToneHz   = m_exciterToneHz;

    for (int i = 0; i < kEqBandCount; ++i)
        p.eqBands[i] = m_eqBands[i];
    return p;
}

void DspController::applySnapshot(const ChainParams &params)
{
    m_bypass = params.bypassed;
    m_inputTrimDb = params.inputTrimDb;
    m_outputTrimDb = params.outputTrimDb;
    m_stereoWidth = params.stereoWidth;
    m_levelerEnabled = params.levelerEnabled;
    m_spectralLevelerEnabled = params.spectralLevelerEnabled;
    m_outputLevelerEnabled = params.outputLevelerEnabled;
    m_compressorEnabled = params.compEnabled;
    m_compThresholdDb = params.compThreshDb;
    m_compRatio = params.compRatio;
    m_compKneeDb = params.compKneeDb;
    m_compAttackMs = params.compAttackMs;
    m_compReleaseMs = params.compReleaseMs;
    m_compMakeupDb = params.compMakeupDb;

    m_exciterEnabled = params.exciterEnabled;
    m_exciterDrive = params.exciterDrive;
    m_exciterMix = params.exciterMix;
    m_exciterToneHz = params.exciterToneHz;

    m_eqEnabled = params.eqEnabled;

    for (int i = 0; i < kEqBandCount; ++i)
        m_eqBands[i] = params.eqBands[i];


    emit bypassChanged();
    emit compressorChanged();
    emit exciterChanged();
    emit eqChanged();
    emit levelerChanged();
}

void DspController::resetBandToDefaults(int band)
{
    if (band < 0 || band >= kEqBandCount) return;
    const ChainParams defaults = defaultParams();
    m_eqBands[band] = defaults.eqBands[band];
    const EqBandParams &b = m_eqBands[band];
    emit eqChanged();
}

void DspController::resetBandEqToDefaults(int band)
{
    if (band < 0 || band >= kEqBandCount) return;
    const ChainParams defaults = defaultParams();
    const EqBandParams &d = defaults.eqBands[band];
    // EQ-shape only — leave the per-band dynamics state untouched.
    m_eqBands[band].enabled = d.enabled;
    m_eqBands[band].type = d.type;
    m_eqBands[band].freqHz = d.freqHz;
    m_eqBands[band].q = d.q;
    m_eqBands[band].gainDb = d.gainDb;
    emit eqChanged();
}

void DspController::resetToDefaults()
{
    applySnapshot(defaultParams());
}

} // namespace dsp
