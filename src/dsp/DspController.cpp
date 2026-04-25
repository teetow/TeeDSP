#include "DspController.h"

#include <QSettings>
#include <QVariantMap>

namespace dsp {

namespace {
// ~125 Hz polling — drives both the meter readouts and the EQ-curve repaint.
// With the EqCurve on QOpenGLWidget, update() requests are coalesced to
// vsync, so this rate doesn't burn extra GPU above the display refresh.
constexpr int kMeterIntervalMs = 8;
constexpr const char *kSettingsGroup = "dsp";

ChainParams defaultParams()
{
    return {};
}
}

DspController::DspController(ProcessorChain *chain, QObject *parent)
    : QObject(parent)
    , m_chain(chain)
{
    Q_ASSERT(m_chain != nullptr);

    pushCompressorParams();
    pushExciterParams();
    m_chain->compressor().setBypass(!m_compressorEnabled);
    m_chain->exciter().setBypass(!m_exciterEnabled);
    m_chain->eq().setBypass(!m_eqEnabled);
    m_chain->setInputTrimDb(m_inputTrimDb);
    m_chain->setOutputTrimDb(m_outputTrimDb);
    m_chain->setStereoWidth(m_stereoWidth);
    m_chain->setBypass(m_bypass);

    m_meterTimer.setInterval(kMeterIntervalMs);
    m_meterTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_meterTimer, &QTimer::timeout, this, &DspController::meterChanged);
    m_meterTimer.start();
}

bool DspController::bypass() const { return m_bypass; }

void DspController::setBypass(bool b)
{
    if (m_bypass == b) return;
    m_bypass = b;
    m_chain->setBypass(b);
    emit bypassChanged();
}

void DspController::setInputTrimDb(float v)
{
    if (m_inputTrimDb == v) return;
    m_inputTrimDb = v;
    m_chain->setInputTrimDb(v);
    emit bypassChanged();
}

void DspController::setOutputTrimDb(float v)
{
    if (m_outputTrimDb == v) return;
    m_outputTrimDb = v;
    m_chain->setOutputTrimDb(v);
    emit bypassChanged();
}

void DspController::setStereoWidth(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (m_stereoWidth == v) return;
    m_stereoWidth = v;
    m_chain->setStereoWidth(v);
    emit bypassChanged();
}

bool DspController::compressorEnabled() const { return m_compressorEnabled; }

void DspController::setCompressorEnabled(bool b)
{
    if (m_compressorEnabled == b) return;
    m_compressorEnabled = b;
    m_chain->compressor().setBypass(!b);
    emit compressorChanged();
}

void DspController::setCompThresholdDb(float v)
{
    if (m_compThresholdDb == v) return;
    m_compThresholdDb = v;
    m_chain->compressor().setThresholdDb(v);
    emit compressorChanged();
}

void DspController::setCompRatio(float v)
{
    if (v < 1.0f) v = 1.0f;
    if (m_compRatio == v) return;
    m_compRatio = v;
    m_chain->compressor().setRatio(v);
    emit compressorChanged();
}

void DspController::setCompKneeDb(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (m_compKneeDb == v) return;
    m_compKneeDb = v;
    m_chain->compressor().setKneeDb(v);
    emit compressorChanged();
}

void DspController::setCompAttackMs(float v)
{
    if (v < 0.1f) v = 0.1f;
    if (m_compAttackMs == v) return;
    m_compAttackMs = v;
    m_chain->compressor().setAttackMs(v);
    emit compressorChanged();
}

void DspController::setCompReleaseMs(float v)
{
    if (v < 1.0f) v = 1.0f;
    if (m_compReleaseMs == v) return;
    m_compReleaseMs = v;
    m_chain->compressor().setReleaseMs(v);
    emit compressorChanged();
}

void DspController::setCompMakeupDb(float v)
{
    if (m_compMakeupDb == v) return;
    m_compMakeupDb = v;
    m_chain->compressor().setMakeupDb(v);
    emit compressorChanged();
}

float DspController::compGainReductionDb() const
{
    return m_chain->compressor().currentGainReductionDb();
}

bool DspController::exciterEnabled() const { return m_exciterEnabled; }

void DspController::setExciterEnabled(bool b)
{
    if (m_exciterEnabled == b) return;
    m_exciterEnabled = b;
    m_chain->exciter().setBypass(!b);
    emit exciterChanged();
}

void DspController::setExciterDrive(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (m_exciterDrive == v) return;
    m_exciterDrive = v;
    m_chain->exciter().setDrive(v);
    emit exciterChanged();
}

void DspController::setExciterMix(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (m_exciterMix == v) return;
    m_exciterMix = v;
    m_chain->exciter().setMix(v);
    emit exciterChanged();
}

void DspController::setExciterToneHz(float v)
{
    if (v < 200.0f) v = 200.0f;
    if (m_exciterToneHz == v) return;
    m_exciterToneHz = v;
    m_chain->exciter().setToneHz(v);
    emit exciterChanged();
}

bool DspController::eqEnabled() const { return m_eqEnabled; }

void DspController::setEqEnabled(bool b)
{
    if (m_eqEnabled == b) return;
    m_eqEnabled = b;
    m_chain->eq().setBypass(!b);
    emit eqChanged();
}

QVariantList DspController::eqBands() const
{
    QVariantList list;
    auto &eq = m_chain->eq();
    for (int i = 0; i < kEqBandCount; ++i) {
        QVariantMap map;
        map.insert(QStringLiteral("enabled"), eq.bandEnabled(i));
        map.insert(QStringLiteral("type"), static_cast<int>(eq.bandType(i)));
        map.insert(QStringLiteral("frequencyHz"), eq.bandFrequency(i));
        map.insert(QStringLiteral("q"), eq.bandQ(i));
        map.insert(QStringLiteral("gainDb"), eq.bandGainDb(i));
        map.insert(QStringLiteral("dynThresholdDb"), eq.bandDynamicThresholdDb(i));
        map.insert(QStringLiteral("dynRatio"), eq.bandDynamicRatio(i));
        map.insert(QStringLiteral("dynAttackMs"), eq.bandDynamicAttackMs(i));
        map.insert(QStringLiteral("dynReleaseMs"), eq.bandDynamicReleaseMs(i));
        map.insert(QStringLiteral("dynRangeDb"), eq.bandDynamicRangeDb(i));
        map.insert(QStringLiteral("dynGainReductionDb"), eq.bandDynamicGainReductionDb(i));
        list.append(map);
    }
    return list;
}

void DspController::eqBandViews(std::array<EqBandView, kEqBandCount> &out) const
{
    auto &eq = m_chain->eq();
    for (int i = 0; i < kEqBandCount; ++i) {
        EqBandView &v = out[i];
        v.enabled            = eq.bandEnabled(i);
        v.type               = static_cast<int>(eq.bandType(i));
        v.freqHz             = eq.bandFrequency(i);
        v.q                  = eq.bandQ(i);
        v.gainDb             = eq.bandGainDb(i);
        v.dynThresholdDb     = eq.bandDynamicThresholdDb(i);
        v.dynRatio           = eq.bandDynamicRatio(i);
        v.dynAttackMs        = eq.bandDynamicAttackMs(i);
        v.dynReleaseMs       = eq.bandDynamicReleaseMs(i);
        v.dynRangeDb         = eq.bandDynamicRangeDb(i);
        v.dynGainReductionDb = eq.bandDynamicGainReductionDb(i);
    }
}

EqBandView DspController::eqBandView(int band) const
{
    EqBandView v{};
    if (band < 0 || band >= kEqBandCount) return v;
    auto &eq = m_chain->eq();
    v.enabled            = eq.bandEnabled(band);
    v.type               = static_cast<int>(eq.bandType(band));
    v.freqHz             = eq.bandFrequency(band);
    v.q                  = eq.bandQ(band);
    v.gainDb             = eq.bandGainDb(band);
    v.dynThresholdDb     = eq.bandDynamicThresholdDb(band);
    v.dynRatio           = eq.bandDynamicRatio(band);
    v.dynAttackMs        = eq.bandDynamicAttackMs(band);
    v.dynReleaseMs       = eq.bandDynamicReleaseMs(band);
    v.dynRangeDb         = eq.bandDynamicRangeDb(band);
    v.dynGainReductionDb = eq.bandDynamicGainReductionDb(band);
    return v;
}

void DspController::setEqBandEnabled(int band, bool enabled)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_chain->eq().setBandEnabled(band, enabled);
    emit eqChanged();
}

void DspController::setEqBandType(int band, int type)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_chain->eq().setBandType(band, static_cast<ParametricEQ::BandType>(type));
    emit eqChanged();
}

void DspController::setEqBandFrequency(int band, float hz)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_chain->eq().setBandFrequency(band, hz);
    emit eqChanged();
}

void DspController::setEqBandQ(int band, float q)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_chain->eq().setBandQ(band, q);
    emit eqChanged();
}

void DspController::setEqBandGainDb(int band, float gainDb)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_chain->eq().setBandGainDb(band, gainDb);
    emit eqChanged();
}

void DspController::setEqBandDynamicThresholdDb(int band, float thresholdDb)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_chain->eq().setBandDynamicThresholdDb(band, thresholdDb);
    emit eqChanged();
}

void DspController::setEqBandDynamicRatio(int band, float ratio)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_chain->eq().setBandDynamicRatio(band, ratio);
    emit eqChanged();
}

void DspController::setEqBandDynamicAttackMs(int band, float attackMs)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_chain->eq().setBandDynamicAttackMs(band, attackMs);
    emit eqChanged();
}

void DspController::setEqBandDynamicReleaseMs(int band, float releaseMs)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_chain->eq().setBandDynamicReleaseMs(band, releaseMs);
    emit eqChanged();
}

void DspController::setEqBandDynamicRangeDb(int band, float rangeDb)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_chain->eq().setBandDynamicRangeDb(band, rangeDb);
    emit eqChanged();
}

void DspController::pushCompressorParams()
{
    auto &c = m_chain->compressor();
    c.setThresholdDb(m_compThresholdDb);
    c.setRatio(m_compRatio);
    c.setKneeDb(m_compKneeDb);
    c.setAttackMs(m_compAttackMs);
    c.setReleaseMs(m_compReleaseMs);
    c.setMakeupDb(m_compMakeupDb);
}

void DspController::pushExciterParams()
{
    auto &e = m_chain->exciter();
    e.setDrive(m_exciterDrive);
    e.setMix(m_exciterMix);
    e.setToneHz(m_exciterToneHz);
}

ChainParams DspController::buildSnapshot() const
{
    ChainParams p;
    p.bypassed        = m_bypass;
    p.inputTrimDb     = m_inputTrimDb;
    p.outputTrimDb    = m_outputTrimDb;
    p.stereoWidth     = m_stereoWidth;
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

    auto &eq = m_chain->eq();
    for (int i = 0; i < kEqBandCount; ++i) {
        auto &b      = p.eqBands[i];
        b.enabled    = eq.bandEnabled(i);
        b.type       = static_cast<int32_t>(eq.bandType(i));
        b.freqHz     = eq.bandFrequency(i);
        b.q          = eq.bandQ(i);
        b.gainDb     = eq.bandGainDb(i);
        b.dynThresholdDb = eq.bandDynamicThresholdDb(i);
        b.dynRatio = eq.bandDynamicRatio(i);
        b.dynAttackMs = eq.bandDynamicAttackMs(i);
        b.dynReleaseMs = eq.bandDynamicReleaseMs(i);
        b.dynRangeDb = eq.bandDynamicRangeDb(i);
    }
    return p;
}

void DspController::applySnapshot(const ChainParams &params)
{
    m_bypass = params.bypassed;
    m_inputTrimDb = params.inputTrimDb;
    m_outputTrimDb = params.outputTrimDb;
    m_stereoWidth = params.stereoWidth;
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

    auto &eq = m_chain->eq();
    for (int i = 0; i < kEqBandCount; ++i) {
        const auto &band = params.eqBands[i];
        eq.setBandEnabled(i, band.enabled);
        eq.setBandType(i, static_cast<ParametricEQ::BandType>(band.type));
        eq.setBandFrequency(i, band.freqHz);
        eq.setBandQ(i, band.q);
        eq.setBandGainDb(i, band.gainDb);
        eq.setBandDynamicThresholdDb(i, band.dynThresholdDb);
        eq.setBandDynamicRatio(i, band.dynRatio);
        eq.setBandDynamicAttackMs(i, band.dynAttackMs);
        eq.setBandDynamicReleaseMs(i, band.dynReleaseMs);
        eq.setBandDynamicRangeDb(i, band.dynRangeDb);
    }

    pushCompressorParams();
    pushExciterParams();
    m_chain->compressor().setBypass(!m_compressorEnabled);
    m_chain->exciter().setBypass(!m_exciterEnabled);
    m_chain->eq().setBypass(!m_eqEnabled);
    m_chain->setInputTrimDb(m_inputTrimDb);
    m_chain->setOutputTrimDb(m_outputTrimDb);
    m_chain->setStereoWidth(m_stereoWidth);
    m_chain->setBypass(m_bypass);

    emit bypassChanged();
    emit compressorChanged();
    emit exciterChanged();
    emit eqChanged();
}

void DspController::loadFromSettings()
{
    ChainParams params = defaultParams();

    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    params.bypassed = settings.value(QStringLiteral("bypass"), params.bypassed).toBool();
    params.inputTrimDb = settings.value(QStringLiteral("inputTrimDb"), params.inputTrimDb).toFloat();
    params.outputTrimDb = settings.value(QStringLiteral("outputTrimDb"), params.outputTrimDb).toFloat();
    params.stereoWidth = settings.value(QStringLiteral("channelMixer/width"), params.stereoWidth).toFloat();
    params.compEnabled = settings.value(QStringLiteral("comp/enabled"), params.compEnabled).toBool();
    params.compThreshDb = settings.value(QStringLiteral("comp/threshold"), params.compThreshDb).toFloat();
    params.compRatio = settings.value(QStringLiteral("comp/ratio"), params.compRatio).toFloat();
    params.compKneeDb = settings.value(QStringLiteral("comp/knee"), params.compKneeDb).toFloat();
    params.compAttackMs = settings.value(QStringLiteral("comp/attack"), params.compAttackMs).toFloat();
    params.compReleaseMs = settings.value(QStringLiteral("comp/release"), params.compReleaseMs).toFloat();
    params.compMakeupDb = settings.value(QStringLiteral("comp/makeup"), params.compMakeupDb).toFloat();

    params.exciterEnabled = settings.value(QStringLiteral("exciter/enabled"), params.exciterEnabled).toBool();
    params.exciterDrive = settings.value(QStringLiteral("exciter/drive"), params.exciterDrive).toFloat();
    params.exciterMix = settings.value(QStringLiteral("exciter/mix"), params.exciterMix).toFloat();
    params.exciterToneHz = settings.value(QStringLiteral("exciter/tone"), params.exciterToneHz).toFloat();

    params.eqEnabled = settings.value(QStringLiteral("eq/enabled"), params.eqEnabled).toBool();

    settings.beginReadArray(QStringLiteral("eq/bands"));
    for (int i = 0; i < kEqBandCount; ++i) {
        settings.setArrayIndex(i);
        if (settings.contains(QStringLiteral("frequencyHz"))) {
            auto &band = params.eqBands[i];
            band.enabled = settings.value(QStringLiteral("enabled"), band.enabled).toBool();
            band.type = settings.value(QStringLiteral("type"), band.type).toInt();
            band.freqHz = settings.value(QStringLiteral("frequencyHz"), band.freqHz).toFloat();
            band.q = settings.value(QStringLiteral("q"), band.q).toFloat();
            band.gainDb = settings.value(QStringLiteral("gainDb"), band.gainDb).toFloat();
            band.dynThresholdDb = settings.value(QStringLiteral("dynThresholdDb"), band.dynThresholdDb).toFloat();
            band.dynRatio = settings.value(QStringLiteral("dynRatio"), band.dynRatio).toFloat();
            band.dynAttackMs = settings.value(QStringLiteral("dynAttackMs"), band.dynAttackMs).toFloat();
            band.dynReleaseMs = settings.value(QStringLiteral("dynReleaseMs"), band.dynReleaseMs).toFloat();
            band.dynRangeDb = settings.value(QStringLiteral("dynRangeDb"), band.dynRangeDb).toFloat();
        }
    }
    settings.endArray();
    settings.endGroup();

    applySnapshot(params);
}

void DspController::resetBandToDefaults(int band)
{
    if (band < 0 || band >= kEqBandCount) return;
    const ChainParams defaults = defaultParams();
    const auto &b = defaults.eqBands[band];
    auto &eq = m_chain->eq();
    eq.setBandEnabled(band, b.enabled);
    eq.setBandType(band, static_cast<ParametricEQ::BandType>(b.type));
    eq.setBandFrequency(band, b.freqHz);
    eq.setBandQ(band, b.q);
    eq.setBandGainDb(band, b.gainDb);
    eq.setBandDynamicThresholdDb(band, b.dynThresholdDb);
    eq.setBandDynamicRatio(band, b.dynRatio);
    eq.setBandDynamicAttackMs(band, b.dynAttackMs);
    eq.setBandDynamicReleaseMs(band, b.dynReleaseMs);
    eq.setBandDynamicRangeDb(band, b.dynRangeDb);
    emit eqChanged();
}

void DspController::resetToDefaults()
{
    applySnapshot(defaultParams());
}

void DspController::saveToSettings() const
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    settings.setValue(QStringLiteral("bypass"), m_bypass);
    settings.setValue(QStringLiteral("inputTrimDb"), m_inputTrimDb);
    settings.setValue(QStringLiteral("outputTrimDb"), m_outputTrimDb);
    settings.setValue(QStringLiteral("channelMixer/width"), m_stereoWidth);
    settings.setValue(QStringLiteral("comp/enabled"), m_compressorEnabled);
    settings.setValue(QStringLiteral("comp/threshold"), m_compThresholdDb);
    settings.setValue(QStringLiteral("comp/ratio"), m_compRatio);
    settings.setValue(QStringLiteral("comp/knee"), m_compKneeDb);
    settings.setValue(QStringLiteral("comp/attack"), m_compAttackMs);
    settings.setValue(QStringLiteral("comp/release"), m_compReleaseMs);
    settings.setValue(QStringLiteral("comp/makeup"), m_compMakeupDb);

    settings.setValue(QStringLiteral("exciter/enabled"), m_exciterEnabled);
    settings.setValue(QStringLiteral("exciter/drive"), m_exciterDrive);
    settings.setValue(QStringLiteral("exciter/mix"), m_exciterMix);
    settings.setValue(QStringLiteral("exciter/tone"), m_exciterToneHz);

    settings.setValue(QStringLiteral("eq/enabled"), m_eqEnabled);

    settings.beginWriteArray(QStringLiteral("eq/bands"), kEqBandCount);
    auto &eq = m_chain->eq();
    for (int i = 0; i < kEqBandCount; ++i) {
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("enabled"), eq.bandEnabled(i));
        settings.setValue(QStringLiteral("type"), static_cast<int>(eq.bandType(i)));
        settings.setValue(QStringLiteral("frequencyHz"), eq.bandFrequency(i));
        settings.setValue(QStringLiteral("q"), eq.bandQ(i));
        settings.setValue(QStringLiteral("gainDb"), eq.bandGainDb(i));
        settings.setValue(QStringLiteral("dynThresholdDb"), eq.bandDynamicThresholdDb(i));
        settings.setValue(QStringLiteral("dynRatio"), eq.bandDynamicRatio(i));
        settings.setValue(QStringLiteral("dynAttackMs"), eq.bandDynamicAttackMs(i));
        settings.setValue(QStringLiteral("dynReleaseMs"), eq.bandDynamicReleaseMs(i));
        settings.setValue(QStringLiteral("dynRangeDb"), eq.bandDynamicRangeDb(i));
    }
    settings.endArray();
    settings.endGroup();
    settings.sync();
}

} // namespace dsp
