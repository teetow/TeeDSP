#include "DspController.h"

#include "host/ClapHost.h"
#include "shared/TeeDspParams.h"
#include "shared/TeeDspTelemetry.h"

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
} // namespace

DspController::DspController(host::ClapHost *host, QObject *parent)
    : QObject(parent)
    , m_host(host)
{
    Q_ASSERT(m_host != nullptr);

    // Seed the EQ band cache from the compiled-in defaults.
    const ChainParams def = defaultParams();
    for (int i = 0; i < kEqBandCount; ++i)
        m_eqBands[i] = def.eqBands[i];

    // Push the full default state across the CLAP boundary so the plugin and
    // controller start in agreement.
    pushAllToHost();

    m_meterTimer.setInterval(kMeterIntervalMs);
    m_meterTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_meterTimer, &QTimer::timeout, this, &DspController::meterChanged);
    m_meterTimer.start();

    // Save-on-change: every parameter mutation emits a *Changed signal, which
    // kicks the debounce timer. The timer's timeout writes settings, so a
    // crash, force-quit, or OS shutdown can never lose a change the user made
    // more than ~500 ms ago. Backstop saves in MainWindow's destructor and
    // closeEvent remain.
    m_saveDebounceTimer.setInterval(500);
    m_saveDebounceTimer.setSingleShot(true);
    connect(&m_saveDebounceTimer, &QTimer::timeout, this, &DspController::saveToSettings);
    connect(this, &DspController::bypassChanged,     this, &DspController::scheduleSave);
    connect(this, &DspController::compressorChanged, this, &DspController::scheduleSave);
    connect(this, &DspController::exciterChanged,    this, &DspController::scheduleSave);
    connect(this, &DspController::eqChanged,         this, &DspController::scheduleSave);
    connect(this, &DspController::levelerChanged,    this, &DspController::scheduleSave);

    // Mirror every committed change to the system-wide APO inside audiodg, and
    // run a 20 Hz heartbeat so the APO processes while we're alive and bypasses
    // when we're gone. The section opens lazily once the APO has created it.
    connect(this, &DspController::bypassChanged,     this, [this]{ m_apoDirty = true; });
    connect(this, &DspController::compressorChanged, this, [this]{ m_apoDirty = true; });
    connect(this, &DspController::exciterChanged,    this, [this]{ m_apoDirty = true; });
    connect(this, &DspController::eqChanged,         this, [this]{ m_apoDirty = true; });
    connect(this, &DspController::levelerChanged,    this, [this]{ m_apoDirty = true; });
    m_apoTimer.setInterval(50);
    connect(&m_apoTimer, &QTimer::timeout, this, &DspController::syncApo);
    m_apoTimer.start();
}

host::ApoSharedClient::ApoStatus DspController::apoStatus()
{
    host::ApoSharedClient::ApoStatus s;
    m_apo.readStatus(s);
    return s;
}

void DspController::syncApo()
{
    if (!m_apo.isOpen()) {
        if (!m_apo.tryOpen())
            return;            // APO hasn't created the shared section yet
        m_apoDirty = true;     // freshly opened — push the current state once
    }
    m_apo.heartbeat();
    if (m_apoDirty) {
        m_apo.writeParams(buildSnapshot());
        m_apoDirty = false;
    }
}

void DspController::setMeterTimerActive(bool active)
{
    if (active) {
        if (!m_meterTimer.isActive()) m_meterTimer.start();
    } else {
        m_meterTimer.stop();
    }
}

void DspController::scheduleSave()
{
    if (m_loadingSettings) return;
    m_saveDebounceTimer.start();
}

// --- param push helpers ----------------------------------------------------

void DspController::pushAllToHost()
{
    using namespace teedsp;
    m_host->setParam(PID_Bypass, m_bypass ? 1.0 : 0.0);
    m_host->setParam(PID_InputTrim, m_inputTrimDb);
    m_host->setParam(PID_OutputTrim, m_outputTrimDb);
    m_host->setParam(PID_StereoWidth, m_stereoWidth);
    m_host->setParam(PID_LevelerEnabled, m_levelerEnabled ? 1.0 : 0.0);
    m_host->setParam(PID_OutputLevelerEnabled, m_outputLevelerEnabled ? 1.0 : 0.0);
    m_host->setParam(PID_EqEnabled, m_eqEnabled ? 1.0 : 0.0);

    m_host->setParam(PID_CompEnabled, m_compressorEnabled ? 1.0 : 0.0);
    m_host->setParam(PID_CompThreshold, m_compThresholdDb);
    m_host->setParam(PID_CompRatio, m_compRatio);
    m_host->setParam(PID_CompKnee, m_compKneeDb);
    m_host->setParam(PID_CompAttack, m_compAttackMs);
    m_host->setParam(PID_CompRelease, m_compReleaseMs);
    m_host->setParam(PID_CompMakeup, m_compMakeupDb);

    m_host->setParam(PID_ExciterEnabled, m_exciterEnabled ? 1.0 : 0.0);
    m_host->setParam(PID_ExciterDrive, m_exciterDrive);
    m_host->setParam(PID_ExciterMix, m_exciterMix);
    m_host->setParam(PID_ExciterTone, m_exciterToneHz);

    for (int b = 0; b < kEqBandCount; ++b) {
        const EqBandParams &p = m_eqBands[b];
        m_host->setParam(bandParamId(b, BF_Enabled),      p.enabled ? 1.0 : 0.0);
        m_host->setParam(bandParamId(b, BF_Type),         p.type);
        m_host->setParam(bandParamId(b, BF_Freq),         p.freqHz);
        m_host->setParam(bandParamId(b, BF_Q),            p.q);
        m_host->setParam(bandParamId(b, BF_Gain),         p.gainDb);
        m_host->setParam(bandParamId(b, BF_DynThreshold), p.dynThresholdDb);
        m_host->setParam(bandParamId(b, BF_DynRatio),     p.dynRatio);
        m_host->setParam(bandParamId(b, BF_DynAttack),    p.dynAttackMs);
        m_host->setParam(bandParamId(b, BF_DynRelease),   p.dynReleaseMs);
        m_host->setParam(bandParamId(b, BF_DynRange),     p.dynRangeDb);
    }
}

// --- global / dynamics setters ---------------------------------------------

bool DspController::bypass() const { return m_bypass; }

void DspController::setBypass(bool b)
{
    if (m_bypass == b) return;
    m_bypass = b;
    m_host->setParam(teedsp::PID_Bypass, b ? 1.0 : 0.0);
    emit bypassChanged();
}

void DspController::setInputTrimDb(float v)
{
    if (m_inputTrimDb == v) return;
    m_inputTrimDb = v;
    m_host->setParam(teedsp::PID_InputTrim, v);
    emit bypassChanged();
}

void DspController::setOutputTrimDb(float v)
{
    if (m_outputTrimDb == v) return;
    m_outputTrimDb = v;
    m_host->setParam(teedsp::PID_OutputTrim, v);
    emit bypassChanged();
}

void DspController::setStereoWidth(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (m_stereoWidth == v) return;
    m_stereoWidth = v;
    m_host->setParam(teedsp::PID_StereoWidth, v);
    emit bypassChanged();
}

void DspController::setLevelerEnabled(bool b)
{
    if (m_levelerEnabled == b) return;
    m_levelerEnabled = b;
    m_host->setParam(teedsp::PID_LevelerEnabled, b ? 1.0 : 0.0);
    emit levelerChanged();
}

float DspController::levelerGainDb() const
{
    host::ApoSharedClient::ApoMeters m;
    m_apo.readMeters(m);
    return m.levelerGainDb;
}

void DspController::setOutputLevelerEnabled(bool b)
{
    if (m_outputLevelerEnabled == b) return;
    m_outputLevelerEnabled = b;
    m_host->setParam(teedsp::PID_OutputLevelerEnabled, b ? 1.0 : 0.0);
    emit levelerChanged();
}

float DspController::outputLevelerGainDb() const
{
    host::ApoSharedClient::ApoMeters m;
    m_apo.readMeters(m);
    return m.outLevelerGainDb;
}

bool DspController::compressorEnabled() const { return m_compressorEnabled; }

void DspController::setCompressorEnabled(bool b)
{
    if (m_compressorEnabled == b) return;
    m_compressorEnabled = b;
    m_host->setParam(teedsp::PID_CompEnabled, b ? 1.0 : 0.0);
    emit compressorChanged();
}

void DspController::setCompThresholdDb(float v)
{
    if (m_compThresholdDb == v) return;
    m_compThresholdDb = v;
    m_host->setParam(teedsp::PID_CompThreshold, v);
    emit compressorChanged();
}

void DspController::setCompRatio(float v)
{
    if (v < 1.0f) v = 1.0f;
    if (m_compRatio == v) return;
    m_compRatio = v;
    m_host->setParam(teedsp::PID_CompRatio, v);
    emit compressorChanged();
}

void DspController::setCompKneeDb(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (m_compKneeDb == v) return;
    m_compKneeDb = v;
    m_host->setParam(teedsp::PID_CompKnee, v);
    emit compressorChanged();
}

void DspController::setCompAttackMs(float v)
{
    if (v < 0.1f) v = 0.1f;
    if (m_compAttackMs == v) return;
    m_compAttackMs = v;
    m_host->setParam(teedsp::PID_CompAttack, v);
    emit compressorChanged();
}

void DspController::setCompReleaseMs(float v)
{
    if (v < 1.0f) v = 1.0f;
    if (m_compReleaseMs == v) return;
    m_compReleaseMs = v;
    m_host->setParam(teedsp::PID_CompRelease, v);
    emit compressorChanged();
}

void DspController::setCompMakeupDb(float v)
{
    if (m_compMakeupDb == v) return;
    m_compMakeupDb = v;
    m_host->setParam(teedsp::PID_CompMakeup, v);
    emit compressorChanged();
}

float DspController::compGainReductionDb() const
{
    host::ApoSharedClient::ApoMeters m;
    m_apo.readMeters(m);
    return m.compGrDb;
}

float DspController::apoInPeakDbfs(int ch) const
{
    host::ApoSharedClient::ApoMeters m;
    m_apo.readMeters(m);
    return m.inPeakDbfs[(ch == 1) ? 1 : 0];
}

float DspController::apoOutPeakDbfs(int ch) const
{
    host::ApoSharedClient::ApoMeters m;
    m_apo.readMeters(m);
    return m.outPeakDbfs[(ch == 1) ? 1 : 0];
}

float DspController::apoOutRmsDbfs() const
{
    host::ApoSharedClient::ApoMeters m;
    m_apo.readMeters(m);
    return m.outRmsDbfs;
}

float DspController::apoOutLufs(int ch) const
{
    host::ApoSharedClient::ApoMeters m;
    m_apo.readMeters(m);
    return m.outLufsCh[(ch == 1) ? 1 : 0];
}

float DspController::apoOutLufsM() const
{
    host::ApoSharedClient::ApoMeters m;
    m_apo.readMeters(m);
    return m.outLufsM;
}

void DspController::drainApoAudio(std::vector<float> &pre, std::vector<float> &post)
{
    m_apo.drainAudio(pre, post);
}

bool DspController::exciterEnabled() const { return m_exciterEnabled; }

void DspController::setExciterEnabled(bool b)
{
    if (m_exciterEnabled == b) return;
    m_exciterEnabled = b;
    m_host->setParam(teedsp::PID_ExciterEnabled, b ? 1.0 : 0.0);
    emit exciterChanged();
}

void DspController::setExciterDrive(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (m_exciterDrive == v) return;
    m_exciterDrive = v;
    m_host->setParam(teedsp::PID_ExciterDrive, v);
    emit exciterChanged();
}

void DspController::setExciterMix(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (m_exciterMix == v) return;
    m_exciterMix = v;
    m_host->setParam(teedsp::PID_ExciterMix, v);
    emit exciterChanged();
}

void DspController::setExciterToneHz(float v)
{
    if (v < 200.0f) v = 200.0f;
    if (m_exciterToneHz == v) return;
    m_exciterToneHz = v;
    m_host->setParam(teedsp::PID_ExciterTone, v);
    emit exciterChanged();
}

bool DspController::eqEnabled() const { return m_eqEnabled; }

void DspController::setEqEnabled(bool b)
{
    if (m_eqEnabled == b) return;
    m_eqEnabled = b;
    m_host->setParam(teedsp::PID_EqEnabled, b ? 1.0 : 0.0);
    emit eqChanged();
}

// --- EQ band reads (cache for params, telemetry for GR) --------------------

QVariantList DspController::eqBands() const
{
    host::ApoSharedClient::ApoMeters t;
    m_apo.readMeters(t);
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
        map.insert(QStringLiteral("dynGainReductionDb"), t.bandGrDb[i]);
        list.append(map);
    }
    return list;
}

void DspController::eqBandViews(std::array<EqBandView, kEqBandCount> &out) const
{
    host::ApoSharedClient::ApoMeters t;
    m_apo.readMeters(t);
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
        v.dynGainReductionDb = t.bandGrDb[i];
    }
}

EqBandView DspController::eqBandView(int band) const
{
    EqBandView v{};
    if (band < 0 || band >= kEqBandCount) return v;
    host::ApoSharedClient::ApoMeters t;
    m_apo.readMeters(t);
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
    v.dynGainReductionDb = t.bandGrDb[band];
    return v;
}

// --- EQ band setters -------------------------------------------------------

void DspController::setEqBandEnabled(int band, bool enabled)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_eqBands[band].enabled = enabled;
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Enabled), enabled ? 1.0 : 0.0);
    emit eqChanged();
}

void DspController::setEqBandType(int band, int type)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_eqBands[band].type = type;
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Type), type);
    emit eqChanged();
}

void DspController::setEqBandFrequency(int band, float hz)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (hz < 10.0f) hz = 10.0f;
    m_eqBands[band].freqHz = hz;
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Freq), hz);
    emit eqChanged();
}

void DspController::setEqBandQ(int band, float q)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (q < 0.05f) q = 0.05f;
    m_eqBands[band].q = q;
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Q), q);
    emit eqChanged();
}

void DspController::setEqBandGainDb(int band, float gainDb)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_eqBands[band].gainDb = gainDb;
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Gain), gainDb);
    emit eqChanged();
}

void DspController::setEqBandDynamicThresholdDb(int band, float thresholdDb)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_eqBands[band].dynThresholdDb = thresholdDb;
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_DynThreshold), thresholdDb);
    emit eqChanged();
}

void DspController::setEqBandDynamicRatio(int band, float ratio)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (ratio < 1.0f) ratio = 1.0f;
    m_eqBands[band].dynRatio = ratio;
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_DynRatio), ratio);
    emit eqChanged();
}

void DspController::setEqBandDynamicAttackMs(int band, float attackMs)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (attackMs < 0.1f) attackMs = 0.1f;
    m_eqBands[band].dynAttackMs = attackMs;
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_DynAttack), attackMs);
    emit eqChanged();
}

void DspController::setEqBandDynamicReleaseMs(int band, float releaseMs)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (releaseMs < 1.0f) releaseMs = 1.0f;
    m_eqBands[band].dynReleaseMs = releaseMs;
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_DynRelease), releaseMs);
    emit eqChanged();
}

void DspController::setEqBandDynamicRangeDb(int band, float rangeDb)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (rangeDb < 0.0f) rangeDb = 0.0f;
    m_eqBands[band].dynRangeDb = rangeDb;
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_DynRange), rangeDb);
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

    pushAllToHost();

    emit bypassChanged();
    emit compressorChanged();
    emit exciterChanged();
    emit eqChanged();
    emit levelerChanged();
}

void DspController::loadFromSettings()
{
    m_loadingSettings = true;
    ChainParams params = defaultParams();

    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    params.bypassed = settings.value(QStringLiteral("bypass"), params.bypassed).toBool();
    params.inputTrimDb = settings.value(QStringLiteral("inputTrimDb"), params.inputTrimDb).toFloat();
    params.outputTrimDb = settings.value(QStringLiteral("outputTrimDb"), params.outputTrimDb).toFloat();
    params.stereoWidth = settings.value(QStringLiteral("channelMixer/width"), params.stereoWidth).toFloat();
    params.levelerEnabled = settings.value(QStringLiteral("leveler/enabled"), params.levelerEnabled).toBool();
    params.outputLevelerEnabled = settings.value(QStringLiteral("leveler/outputEnabled"), params.outputLevelerEnabled).toBool();
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
    m_loadingSettings = false;
}

void DspController::resetBandToDefaults(int band)
{
    if (band < 0 || band >= kEqBandCount) return;
    const ChainParams defaults = defaultParams();
    m_eqBands[band] = defaults.eqBands[band];
    const EqBandParams &b = m_eqBands[band];
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Enabled),      b.enabled ? 1.0 : 0.0);
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Type),         b.type);
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Freq),         b.freqHz);
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Q),            b.q);
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Gain),         b.gainDb);
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_DynThreshold), b.dynThresholdDb);
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_DynRatio),     b.dynRatio);
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_DynAttack),    b.dynAttackMs);
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_DynRelease),   b.dynReleaseMs);
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_DynRange),     b.dynRangeDb);
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
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Enabled), d.enabled ? 1.0 : 0.0);
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Type),    d.type);
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Freq),    d.freqHz);
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Q),       d.q);
    m_host->setParam(teedsp::bandParamId(band, teedsp::BF_Gain),    d.gainDb);
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
    settings.setValue(QStringLiteral("leveler/enabled"), m_levelerEnabled);
    settings.setValue(QStringLiteral("leveler/outputEnabled"), m_outputLevelerEnabled);
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
    for (int i = 0; i < kEqBandCount; ++i) {
        const EqBandParams &b = m_eqBands[i];
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("enabled"), b.enabled);
        settings.setValue(QStringLiteral("type"), b.type);
        settings.setValue(QStringLiteral("frequencyHz"), b.freqHz);
        settings.setValue(QStringLiteral("q"), b.q);
        settings.setValue(QStringLiteral("gainDb"), b.gainDb);
        settings.setValue(QStringLiteral("dynThresholdDb"), b.dynThresholdDb);
        settings.setValue(QStringLiteral("dynRatio"), b.dynRatio);
        settings.setValue(QStringLiteral("dynAttackMs"), b.dynAttackMs);
        settings.setValue(QStringLiteral("dynReleaseMs"), b.dynReleaseMs);
        settings.setValue(QStringLiteral("dynRangeDb"), b.dynRangeDb);
    }
    settings.endArray();
    settings.endGroup();
    settings.sync();
}

} // namespace dsp
