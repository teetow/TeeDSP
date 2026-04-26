#include "AudioEngine.h"

#include "../dsp/ProcessorChain.h"
#include "SpectrumAnalyzer.h"
#include "WasapiDeviceNotifier.h"
#include "WasapiDevices.h"

#include <QMetaObject>

#include <cmath>

namespace host {

// ---- BS.1770-3 K-weighted LUFS monitor ---------------------------------- //
namespace {

struct KWeightBiquad {
    double b0{1}, b1{0}, b2{0}, a1{0}, a2{0};
    double z1{0}, z2{0};
    float process(float x) noexcept {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return static_cast<float>(y);
    }
    void reset() noexcept { z1 = z2 = 0.0; }
};

} // anonymous namespace

struct LufsMonitor {
    static constexpr int kMaxCh = 8;

    struct ChannelState {
        KWeightBiquad s1, s2;
        std::vector<float> ring;
        double sumSq = 0.0;
    } ch[kMaxCh];

    int numCh         = 0;
    int writePos      = 0;
    int windowSamples = 0;
    std::atomic<float> lufsM{-70.0f};
    std::atomic<float> lufsChL{-70.0f};
    std::atomic<float> lufsChR{-70.0f};

    static void computeCoeffs(double fs, KWeightBiquad &s1, KWeightBiquad &s2) {
        static constexpr double kPi = 3.14159265358979323846;
        // Stage 1: ITU-R BS.1770 high-shelf pre-filter
        const double f1 = 1681.974450955533, G = 3.999843853973347, Q1 = 0.7071752369554196;
        const double K1 = std::tan(kPi * f1 / fs);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double d1 = 1.0 + K1 / Q1 + K1 * K1;
        s1.b0 = (Vh + Vb * K1 / Q1 + K1 * K1) / d1;
        s1.b1 = 2.0 * (K1 * K1 - Vh) / d1;
        s1.b2 = (Vh - Vb * K1 / Q1 + K1 * K1) / d1;
        s1.a1 = 2.0 * (K1 * K1 - 1.0) / d1;
        s1.a2 = (1.0 - K1 / Q1 + K1 * K1) / d1;
        // Stage 2: RLB high-pass
        const double f2 = 38.13547087602444, Q2 = 0.5003270373238773;
        const double K2 = std::tan(kPi * f2 / fs);
        const double d2 = 1.0 + K2 / Q2 + K2 * K2;
        s2.b0 =  1.0 / d2;  s2.b1 = -2.0 / d2;  s2.b2 = 1.0 / d2;
        s2.a1 = 2.0 * (K2 * K2 - 1.0) / d2;
        s2.a2 = (1.0 - K2 / Q2 + K2 * K2) / d2;
    }

    void prepare(double fs, int channels) {
        numCh = std::min(channels, kMaxCh);
        windowSamples = std::max(1, static_cast<int>(fs * 0.4)); // 400 ms
        KWeightBiquad s1, s2;
        computeCoeffs(fs, s1, s2);
        for (int c = 0; c < numCh; ++c) {
            ch[c].s1 = s1;  ch[c].s1.reset();
            ch[c].s2 = s2;  ch[c].s2.reset();
            ch[c].ring.assign(static_cast<size_t>(windowSamples), 0.0f);
            ch[c].sumSq = 0.0;
        }
        writePos = 0;
        lufsM.store(-70.0f, std::memory_order_relaxed);
        lufsChL.store(-70.0f, std::memory_order_relaxed);
        lufsChR.store(-70.0f, std::memory_order_relaxed);
    }

    void resetBuffers() noexcept {
        for (int c = 0; c < numCh; ++c) {
            ch[c].s1.reset(); ch[c].s2.reset();
            std::fill(ch[c].ring.begin(), ch[c].ring.end(), 0.0f);
            ch[c].sumSq = 0.0;
        }
        writePos = 0;
        lufsM.store(-70.0f, std::memory_order_relaxed);
        lufsChL.store(-70.0f, std::memory_order_relaxed);
        lufsChR.store(-70.0f, std::memory_order_relaxed);
    }

    void process(const float *interleaved, int frames, int inCh) noexcept {
        if (numCh <= 0 || windowSamples <= 0) return;
        const int nCh = std::min(inCh, numCh);
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < nCh; ++c) {
                const float y  = ch[c].s2.process(ch[c].s1.process(interleaved[i * inCh + c]));
                const float sq = y * y;
                ch[c].sumSq -= static_cast<double>(ch[c].ring[writePos]);
                ch[c].ring[writePos] = sq;
                ch[c].sumSq += static_cast<double>(sq);
            }
            writePos = (writePos + 1) % windowSamples;
        }
        // Momentary LUFS: G_i=1.0 for L/R/C, 1.41 for Ls/Rs (BS.1770 §2)
        double power = 0.0;
        for (int c = 0; c < nCh; ++c)
            power += ((c >= 3) ? 1.41 : 1.0) * (ch[c].sumSq / windowSamples);
        lufsM.store(power > 1e-10
            ? static_cast<float>(-0.691 + 10.0 * std::log10(power))
            : -70.0f,
            std::memory_order_relaxed);

        const auto chToLufs = [this](int c) {
            if (c >= numCh || windowSamples <= 0) return -70.0f;
            const double p = ch[c].sumSq / static_cast<double>(windowSamples);
            return static_cast<float>(p > 1e-10 ? (-0.691 + 10.0 * std::log10(p)) : -70.0);
        };
        lufsChL.store(chToLufs(0), std::memory_order_relaxed);
        lufsChR.store(chToLufs(1), std::memory_order_relaxed);
    }
};
// ---- end LufsMonitor ---------------------------------------------------- //

AudioEngine::AudioEngine(dsp::ProcessorChain *chain, QObject *parent)
    : QObject(parent)
    , m_chain(chain)
    , m_analyzer(new SpectrumAnalyzer(this))
    , m_notifier(new WasapiDeviceNotifier(this))
{
    connect(m_notifier, &WasapiDeviceNotifier::devicesChanged,
            this, &AudioEngine::onDevicesChanged);
    connect(m_notifier, &WasapiDeviceNotifier::devicesChanged,
            this, &AudioEngine::devicesChanged);
    connect(m_notifier, &WasapiDeviceNotifier::defaultRenderChanged,
            this, &AudioEngine::defaultRenderChanged);
    m_lufsMonitor = std::make_unique<LufsMonitor>();
}

AudioEngine::~AudioEngine()
{
    stop();
}

QString AudioEngine::start(const QString &captureDeviceId,
                           const QString &preferredRenderId)
{
    stop();

    m_captureDeviceId   = captureDeviceId;
    m_preferredRenderId = preferredRenderId;

    if (m_captureDeviceId.isEmpty()) {
        m_lastError = QStringLiteral("No capture device selected.");
        emit errorOccurred(m_lastError);
        return m_lastError;
    }

    const QString renderId = pickRenderId();
    if (renderId.isEmpty()) {
        m_lastError = QStringLiteral("No render endpoint available.");
        emit errorOccurred(m_lastError);
        return m_lastError;
    }
    if (renderId == m_captureDeviceId) {
        m_lastError = QStringLiteral(
            "Capture and render devices must differ — looping back a device into itself feeds back.");
        emit errorOccurred(m_lastError);
        return m_lastError;
    }

    const QString renderErr = m_render.start(renderId);
    if (!renderErr.isEmpty()) {
        m_lastError = QStringLiteral("Render: ") + renderErr;
        emit errorOccurred(m_lastError);
        return m_lastError;
    }
    m_currentRenderId = renderId;

    const QString captureErr = m_capture.start(m_captureDeviceId,
        [this](const float *d, int nf, int nc, int sr) {
            onCapturePacket(d, nf, nc, sr);
        });
    if (!captureErr.isEmpty()) {
        m_render.stop();
        m_lastError = QStringLiteral("Capture: ") + captureErr;
        emit errorOccurred(m_lastError);
        return m_lastError;
    }

    // Pre-fill the capture format from the mix format so that the status
    // bar shows real values (Hz / ch) as soon as the engine signals running,
    // rather than waiting for the first audio packet to arrive.
    {
        StreamFormat fmt{};
        if (WasapiDevices::queryMixFormat(m_captureDeviceId, fmt) && fmt.sampleRate > 0) {
            m_captureSampleRate = fmt.sampleRate;
            m_captureChannels   = fmt.channels;
        }
    }

    m_lastError.clear();
    emit currentRenderChanged(m_currentRenderId);
    emit runningChanged();
    return {};
}

void AudioEngine::stop()
{
    m_capture.stop();
    m_render.stop();
    if (m_analyzer) m_analyzer->stop();
    m_chainPrepared = false;
    m_captureSampleRate = 0;
    m_captureChannels = 0;
    m_currentRenderId.clear();
    // Park all meters at silence.
    for (auto *a : {&m_recentHotDbfs, &m_recentInputPeakDbfs, &m_recentOutputPeakDbfs,
                    &m_recentOutputRmsDbfs, &m_recentInputPeakChL, &m_recentInputPeakChR,
                    &m_recentOutputPeakChL, &m_recentOutputPeakChR})
        a->store(-120.0f, std::memory_order_relaxed);
    m_recentLufsM.store(-70.0f, std::memory_order_relaxed);
    m_recentLufsChL.store(-70.0f, std::memory_order_relaxed);
    m_recentLufsChR.store(-70.0f, std::memory_order_relaxed);
    if (m_lufsMonitor) m_lufsMonitor->resetBuffers();
    emit runningChanged();
}

bool AudioEngine::isRunning() const
{
    return m_capture.isRunning() && m_render.isRunning();
}

void AudioEngine::setPreferredRender(const QString &id)
{
    if (m_preferredRenderId == id) return;
    m_preferredRenderId = id;
    // Gate on capture (not full isRunning) so a manual device pick can
    // resurrect a render thread that's already dead — that's the common
    // recovery path when the previous render endpoint went away mid-stream
    // and we haven't auto-rerouted yet.
    if (m_capture.isRunning()) onDevicesChanged();
}

// Returns the device id we should be rendering to right now. The preferred
// endpoint wins when it's active and non-virtual; otherwise we fall back to
// the first non-virtual active endpoint, skipping the capture endpoint to
// prevent a feedback loop.
QString AudioEngine::pickRenderId() const
{
    const auto devices = WasapiDevices::enumerateRender();

    auto byId = [&](const QString &id) -> const DeviceInfo * {
        for (const auto &d : devices)
            if (d.id == id) return &d;
        return nullptr;
    };

    auto eligible = [&](const DeviceInfo &d) {
        return d.isActive && !d.isVirtual && d.id != m_captureDeviceId;
    };

    if (!m_preferredRenderId.isEmpty()) {
        const auto *p = byId(m_preferredRenderId);
        if (p && eligible(*p)) return p->id;
    }
    for (const auto &d : devices) {
        if (eligible(d)) return d.id;
    }
    return {};
}

bool AudioEngine::switchRenderTo(const QString &deviceId)
{
    if (deviceId.isEmpty()) return false;
    if (deviceId == m_currentRenderId && m_render.isRunning()) return false;

    m_render.stop();
    // Force the resampler to re-prepare against the new render rate on the
    // next packet. Without this we'd happily linear-interp 48 → 44.1 into a
    // device that's now running at 96k.
    m_resamplerSrcSr = 0;
    m_resamplerDstSr = 0;
    m_resamplerChannels = 0;
    const QString err = m_render.start(deviceId);
    if (!err.isEmpty()) {
        m_lastError = QStringLiteral("Render switch failed: ") + err;
        emit errorOccurred(m_lastError);
        m_currentRenderId.clear();
        return false;
    }
    m_currentRenderId = deviceId;
    emit currentRenderChanged(deviceId);
    return true;
}

void AudioEngine::onDevicesChanged()
{
    // Recovery must be possible even when the render side is currently dead
    // (Bluetooth removal storm killed the previous client). Capture alive is
    // the meaningful precondition.
    if (!m_capture.isRunning()) return;
    const QString target = pickRenderId();
    if (target.isEmpty()) return;
    if (target == m_currentRenderId && m_render.isRunning()) return;
    switchRenderTo(target);
}

void AudioEngine::onRenderEndedUnexpectedly()
{
    if (!m_capture.isRunning()) return;
    const QString target = pickRenderId();
    if (target.isEmpty()) {
        m_lastError = QStringLiteral("Render endpoint disappeared and no fallback is available.");
        emit errorOccurred(m_lastError);
        m_currentRenderId.clear();
        emit currentRenderChanged(m_currentRenderId);
        emit runningChanged();
        return;
    }
    switchRenderTo(target);
}

void AudioEngine::onCapturePacket(const float *interleaved,
                                  int numFrames, int numChannels, int sampleRate)
{
    if (!m_chain || numFrames <= 0 || numChannels <= 0) return;

    // The render thread can die silently if the audio client gets
    // invalidated (Bluetooth tear-down is the common one). Surface that to
    // the engine's own thread for a clean re-pick — checking from here
    // because this is the one place that runs every audio packet.
    if (m_render.consumeEndedFlag()) {
        QMetaObject::invokeMethod(this, "onRenderEndedUnexpectedly",
                                  Qt::QueuedConnection);
    }

    // Post-input-trim peak — per-channel and mono-mixed.
    // Measured before the rest of DSP so it reflects what hits the EQ/compressor.
    {
        float prePeakL = 0.0f, prePeakR = 0.0f;
        const int sampleCount = numFrames * numChannels;
        for (int i = 0; i < numFrames; ++i) {
            const float aL = std::fabs(interleaved[i * numChannels + 0]);
            const float aR = std::fabs(interleaved[i * numChannels + std::min(1, numChannels - 1)]);
            if (aL > prePeakL) prePeakL = aL;
            if (aR > prePeakR) prePeakR = aR;
        }
        const auto toDbfs = [](float a) {
            return (a > 1e-6f) ? 20.0f * std::log10(a) : -120.0f;
        };
        const float trimDb = m_chain->inputTrimDb();
        const float dbL = toDbfs(prePeakL) + trimDb, dbR = toDbfs(prePeakR) + trimDb;
        m_recentInputPeakChL.store(dbL, std::memory_order_relaxed);
        m_recentInputPeakChR.store(dbR, std::memory_order_relaxed);
        m_recentInputPeakDbfs.store(std::max(dbL, dbR), std::memory_order_relaxed);
    }

    if (!m_chainPrepared
        || sampleRate != m_captureSampleRate
        || numChannels != m_captureChannels) {
        m_chain->prepare(static_cast<double>(sampleRate),
                         static_cast<std::size_t>(numChannels));
        m_chain->reset();
        m_captureSampleRate = sampleRate;
        m_captureChannels = numChannels;
        m_chainPrepared = true;
        if (m_analyzer) m_analyzer->start(sampleRate, numChannels);
        if (m_lufsMonitor) m_lufsMonitor->prepare(static_cast<double>(sampleRate), numChannels);
        // The capture thread hits this on the very first packet — let the UI
        // know the real format so the status line stops showing 0 Hz / 0 ch.
        emit captureFormatChanged(sampleRate, numChannels);
    }

    if (m_analyzer) m_analyzer->pushPre(interleaved, numFrames, numChannels);

    m_chain->process(const_cast<float *>(interleaved),
                     static_cast<std::size_t>(numFrames));

    // Track near-full-scale output as a practical indicator that a hidden
    // limiter/safety clamp may be pumping somewhere downstream.
    float peakL = 0.0f, peakR = 0.0f;
    double sumSq = 0.0;
    const int sampleCount = numFrames * numChannels;
    for (int i = 0; i < numFrames; ++i) {
        const float sL = interleaved[i * numChannels + 0];
        const float sR = interleaved[i * numChannels + std::min(1, numChannels - 1)];
        if (std::fabs(sL) > peakL) peakL = std::fabs(sL);
        if (std::fabs(sR) > peakR) peakR = std::fabs(sR);
        for (int c = 0; c < numChannels; ++c) {
            const float s = interleaved[i * numChannels + c];
            sumSq += static_cast<double>(s) * static_cast<double>(s);
        }
    }
    {
        const auto toDbfs = [](float a) {
            return (a > 1e-6f) ? 20.0f * std::log10(a) : -120.0f;
        };
        const float dbL = toDbfs(peakL), dbR = toDbfs(peakR);
        m_recentOutputPeakChL.store(dbL, std::memory_order_relaxed);
        m_recentOutputPeakChR.store(dbR, std::memory_order_relaxed);
        const float dbPeak = std::max(dbL, dbR);
        m_recentHotDbfs.store(dbPeak, std::memory_order_relaxed);
        m_recentOutputPeakDbfs.store(dbPeak, std::memory_order_relaxed);
    }
    {
        float rmsDbfs = -120.0f;
        if (sampleCount > 0 && sumSq > 0.0) {
            const double rms = std::sqrt(sumSq / static_cast<double>(sampleCount));
            if (rms > 1e-6) rmsDbfs = static_cast<float>(20.0 * std::log10(rms));
        }
        m_recentOutputRmsDbfs.store(rmsDbfs, std::memory_order_relaxed);
    }

    if (m_analyzer) m_analyzer->pushPost(interleaved, numFrames, numChannels);

    if (m_lufsMonitor) m_lufsMonitor->process(interleaved, numFrames, numChannels);
    m_recentLufsM.store(m_lufsMonitor ? m_lufsMonitor->lufsM.load(std::memory_order_relaxed) : -70.0f,
                        std::memory_order_relaxed);
    m_recentLufsChL.store(m_lufsMonitor ? m_lufsMonitor->lufsChL.load(std::memory_order_relaxed) : -70.0f,
                          std::memory_order_relaxed);
    m_recentLufsChR.store(m_lufsMonitor ? m_lufsMonitor->lufsChR.load(std::memory_order_relaxed) : -70.0f,
                          std::memory_order_relaxed);

    // Resample if capture and render disagree on sample rate (e.g., capturing
    // VB-CABLE @ 48 kHz while rendering to a Focusrite negotiated at 44.1 kHz).
    // Without this step the render side plays the buffer at its own clock,
    // which sounds pitch-shifted. The render side may not know its rate yet
    // immediately after start(), so guard on a positive value.
    const int renderSr = m_render.sampleRate();
    if (renderSr > 0 && renderSr != sampleRate) {
        if (m_resamplerSrcSr != sampleRate
            || m_resamplerDstSr != renderSr
            || m_resamplerChannels != numChannels) {
            m_resampler.prepare(static_cast<double>(sampleRate),
                                static_cast<double>(renderSr),
                                numChannels);
            m_resamplerSrcSr = sampleRate;
            m_resamplerDstSr = renderSr;
            m_resamplerChannels = numChannels;
        }
        m_resampleScratch.clear();
        m_resampler.process(interleaved, numFrames, m_resampleScratch);
        const int outFrames = static_cast<int>(m_resampleScratch.size() / numChannels);
        if (outFrames > 0) {
            m_render.write(m_resampleScratch.data(), outFrames, numChannels);
        }
    } else {
        m_render.write(interleaved, numFrames, numChannels);
    }
}

} // namespace host

float host::AudioEngine::currentInputPeakDbfs(int ch) const
{
    return (ch == 0 ? m_recentInputPeakChL : m_recentInputPeakChR)
        .load(std::memory_order_relaxed);
}

float host::AudioEngine::currentOutputPeakDbfs(int ch) const
{
    return (ch == 0 ? m_recentOutputPeakChL : m_recentOutputPeakChR)
        .load(std::memory_order_relaxed);
}

float host::AudioEngine::currentOutputLufsM(int ch) const
{
    return (ch == 0 ? m_recentLufsChL : m_recentLufsChR)
        .load(std::memory_order_relaxed);
}
