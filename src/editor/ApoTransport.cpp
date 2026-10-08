#include "ApoTransport.h"
#include "shared/TeeDspParams.h"
#include "ui/widgets/WidgetMetrics.h"
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>

namespace editor {
namespace { constexpr const char *kSettingsGroup = "dsp"; }

ApoTransport::ApoTransport(QObject *parent): Transport(parent), m_analyzer(this) {
    static_assert(Meters::kSpectralBandCount ==
                  sizeof(teedsp::ApoShared::spectralGainDb)/sizeof(float));
    m_heartbeat.setInterval(50);
    connect(&m_heartbeat, &QTimer::timeout, this, &ApoTransport::tick);
    m_spectrum.setTimerType(Qt::PreciseTimer);
    m_spectrum.setInterval(17);
    connect(&m_spectrum, &QTimer::timeout, this, &ApoTransport::spectrumTick);
    connect(&m_analyzer, &host::SpectrumAnalyzer::spectraUpdated,
            this, &Transport::spectraUpdated);
    m_saveDebounce.setSingleShot(true);
    m_saveDebounce.setInterval(500);
    connect(&m_saveDebounce, &QTimer::timeout, this, &ApoTransport::flush);
}
void ApoTransport::start() {
    if (m_started) return;
    dsp::ChainParams params;

    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    params.bypassed = settings.value(QStringLiteral("bypass"), params.bypassed).toBool();
    params.inputTrimDb = settings.value(QStringLiteral("inputTrimDb"), params.inputTrimDb).toFloat();
    params.outputTrimDb = settings.value(QStringLiteral("outputTrimDb"), params.outputTrimDb).toFloat();
    params.stereoWidth = settings.value(QStringLiteral("channelMixer/width"), params.stereoWidth).toFloat();
    params.levelerEnabled = settings.value(QStringLiteral("leveler/enabled"), params.levelerEnabled).toBool();
    params.spectralLevelerEnabled = settings.value(QStringLiteral("spectralLeveler/enabled"), params.spectralLevelerEnabled).toBool();
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
    for (int i = 0; i < teedsp::kBandCount; ++i) {
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

    m_params=params;
    m_started=true;
    emit parametersReceived(params);
    m_heartbeat.start();
    tick();
}
void ApoTransport::submit(const dsp::ChainParams &params) {
    m_params=params;m_dirty=true;
    m_saveDebounce.start();
    tick();
}
void ApoTransport::relearnLeveler(bool output) {
    tick();
    m_apo.relearnLeveler(output);
}
void ApoTransport::tick() {
    const bool opened=!m_apo.isOpen() && m_apo.tryOpen();
    if(opened) m_dirty=true;
    if(m_dirty) {
        writeParamsFile(m_params);
        if(m_apo.isOpen()) m_apo.writeParams(m_params);
        m_dirty=false;
    }
    if(m_apo.isOpen()) m_apo.heartbeat();
}
void ApoTransport::writeParamsFile(const dsp::ChainParams &p) const {
    const QString path=QString::fromWCharArray(teedsp::kApoParamsPath);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if(!f.open(QIODevice::WriteOnly)) return;
    const quint32 magic=teedsp::kApoParamsMagic;
    f.write(reinterpret_cast<const char*>(&magic),sizeof(magic));
    f.write(reinterpret_cast<const char*>(&p),sizeof(p));
    f.commit();
}
void ApoTransport::flush() {
    if(!m_started) return;
    const auto &p=m_params;
    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    settings.setValue(QStringLiteral("bypass"), p.bypassed);
    settings.setValue(QStringLiteral("inputTrimDb"), p.inputTrimDb);
    settings.setValue(QStringLiteral("outputTrimDb"), p.outputTrimDb);
    settings.setValue(QStringLiteral("channelMixer/width"), p.stereoWidth);
    settings.setValue(QStringLiteral("leveler/enabled"), p.levelerEnabled);
    settings.setValue(QStringLiteral("spectralLeveler/enabled"), p.spectralLevelerEnabled);
    settings.setValue(QStringLiteral("leveler/outputEnabled"), p.outputLevelerEnabled);
    settings.setValue(QStringLiteral("comp/enabled"), p.compEnabled);
    settings.setValue(QStringLiteral("comp/threshold"), p.compThreshDb);
    settings.setValue(QStringLiteral("comp/ratio"), p.compRatio);
    settings.setValue(QStringLiteral("comp/knee"), p.compKneeDb);
    settings.setValue(QStringLiteral("comp/attack"), p.compAttackMs);
    settings.setValue(QStringLiteral("comp/release"), p.compReleaseMs);
    settings.setValue(QStringLiteral("comp/makeup"), p.compMakeupDb);

    settings.setValue(QStringLiteral("exciter/enabled"), p.exciterEnabled);
    settings.setValue(QStringLiteral("exciter/drive"), p.exciterDrive);
    settings.setValue(QStringLiteral("exciter/mix"), p.exciterMix);
    settings.setValue(QStringLiteral("exciter/tone"), p.exciterToneHz);

    settings.setValue(QStringLiteral("eq/enabled"), p.eqEnabled);

    settings.beginWriteArray(QStringLiteral("eq/bands"), teedsp::kBandCount);
    for (int i = 0; i < teedsp::kBandCount; ++i) {
        const dsp::EqBandParams &b = p.eqBands[i];
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
Meters ApoTransport::meters() const {
    Meters out;
    m_apo.readMeters(out);
    return out;
}
host::ApoSharedClient::ApoStatus ApoTransport::status() const {
    host::ApoSharedClient::ApoStatus out;
    m_apo.readStatus(out);
    return out;
}
void ApoTransport::setEditorVisible(bool visible) {
    m_visible=visible;
    m_heartbeat.setInterval(visible ? 50 : 200);
    setSpectrumVisible(m_showSpectrum);
}
void ApoTransport::setSpectrumVisible(bool visible) {
    m_showSpectrum=visible;
    const bool active=m_visible && visible;
    m_analyzer.setUiActive(active);
    if(active) m_spectrum.start(); else m_spectrum.stop();
}
void ApoTransport::spectrumTick() {
    m_apo.drainAudio(m_pre,m_post);
    if(m_pre.empty()) return;
    const auto st=status();
    const double sampleRate=st.sampleRate ? st.sampleRate : 48000.0;
    if(sampleRate!=m_analyzerSampleRate) {
        m_analyzer.start(sampleRate,1);
        m_analyzerSampleRate=sampleRate;
    }
    m_analyzer.pushPre(m_pre.data(),int(m_pre.size()),1);
    m_analyzer.pushPost(m_post.data(),int(m_post.size()),1);
    m_analyzer.processPending();
}
Transport *createTransport(QObject *parent) { return new ApoTransport(parent); }
} // namespace editor
