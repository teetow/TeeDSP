#include "AudioEngine.h"

#include "../dsp/ProcessorChain.h"
#include "SpectrumAnalyzer.h"
#include "WasapiDeviceNotifier.h"
#include "WasapiDevices.h"

namespace host {

AudioEngine::AudioEngine(dsp::ProcessorChain *chain, QObject *parent)
    : QObject(parent)
    , m_chain(chain)
    , m_analyzer(new SpectrumAnalyzer(this))
    , m_notifier(new WasapiDeviceNotifier(this))
{
    connect(m_notifier, &WasapiDeviceNotifier::devicesChanged,
            this, &AudioEngine::onDevicesChanged);
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
    }

    if (m_analyzer) m_analyzer->pushPre(interleaved, numFrames, numChannels);

    m_chain->process(const_cast<float *>(interleaved),
                     static_cast<std::size_t>(numFrames));

    if (m_analyzer) m_analyzer->pushPost(interleaved, numFrames, numChannels);

    m_render.write(interleaved, numFrames, numChannels);
}

} // namespace host
