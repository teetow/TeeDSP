#include "AudioEngine.h"

#include "../dsp/ProcessorChain.h"
#include "SpectrumAnalyzer.h"
#include "WasapiDeviceNotifier.h"
#include "WasapiDevices.h"

#include <cmath>

namespace host {

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
    emit runningChanged();
}

bool AudioEngine::isRunning() const
{
    return m_capture.isRunning() && m_render.isRunning();
}

void AudioEngine::setAutoRoute(bool enabled)
{
    if (m_autoRoute == enabled) return;
    m_autoRoute = enabled;
    if (isRunning()) onDevicesChanged();
}

void AudioEngine::setPreferredRender(const QString &id)
{
    if (m_preferredRenderId == id) return;
    m_preferredRenderId = id;
    if (isRunning()) onDevicesChanged();
}

// Returns the device id we should be rendering to right now, given the
// current capture target, the user's preference, and auto-route mode.
//
//  - In manual mode, this is just the preferred render id (or empty if it
//    isn't in the device list at all).
//  - In auto-route mode, the preferred wins when active+non-virtual; if it's
//    inactive we fall back to the first non-virtual active endpoint, ignoring
//    the capture endpoint to prevent a feedback loop.
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

    if (!m_autoRoute) {
        const auto *p = byId(m_preferredRenderId);
        if (p && p->isActive && p->id != m_captureDeviceId)
            return p->id;
        return m_preferredRenderId;
    }

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
    if (!isRunning()) return;
    const QString target = pickRenderId();
    if (target.isEmpty() || target == m_currentRenderId) return;
    switchRenderTo(target);
}

void AudioEngine::onCapturePacket(const float *interleaved,
                                  int numFrames, int numChannels, int sampleRate)
{
    if (!m_chain || numFrames <= 0 || numChannels <= 0) return;

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
        // The capture thread hits this on the very first packet — let the UI
        // know the real format so the status line stops showing 0 Hz / 0 ch.
        emit captureFormatChanged(sampleRate, numChannels);
    }

    if (m_analyzer) m_analyzer->pushPre(interleaved, numFrames, numChannels);

    m_chain->process(const_cast<float *>(interleaved),
                     static_cast<std::size_t>(numFrames));

    // Track near-full-scale output as a practical indicator that a hidden
    // limiter/safety clamp may be pumping somewhere downstream.
    float peak = 0.0f;
    const int sampleCount = numFrames * numChannels;
    for (int i = 0; i < sampleCount; ++i) {
        const float a = std::fabs(interleaved[i]);
        if (a > peak)
            peak = a;
    }
    if (peak > 0.0f) {
        const float dbfs = 20.0f * std::log10(peak);
        float cur = m_recentHotDbfs.load(std::memory_order_relaxed);
        while (dbfs > cur
               && !m_recentHotDbfs.compare_exchange_weak(cur, dbfs, std::memory_order_relaxed)) {}
    }

    if (m_analyzer) m_analyzer->pushPost(interleaved, numFrames, numChannels);

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
