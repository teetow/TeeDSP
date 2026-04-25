#include "AudioEngine.h"

#include "../dsp/ProcessorChain.h"

namespace host {

AudioEngine::AudioEngine(dsp::ProcessorChain *chain, QObject *parent)
    : QObject(parent)
    , m_chain(chain)
{
}

AudioEngine::~AudioEngine()
{
    stop();
}

QString AudioEngine::start(const QString &captureDeviceId, const QString &renderDeviceId)
{
    stop();

    if (captureDeviceId == renderDeviceId) {
        m_lastError = QStringLiteral(
            "Capture and render devices must differ — looping back a device into itself feeds back.");
        emit errorOccurred(m_lastError);
        return m_lastError;
    }

    const QString renderErr = m_render.start(renderDeviceId);
    if (!renderErr.isEmpty()) {
        m_lastError = QStringLiteral("Render: ") + renderErr;
        emit errorOccurred(m_lastError);
        return m_lastError;
    }

    const QString captureErr = m_capture.start(captureDeviceId,
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
    emit runningChanged();
    return {};
}

void AudioEngine::stop()
{
    m_capture.stop();
    m_render.stop();
    m_chainPrepared = false;
    m_captureSampleRate = 0;
    m_captureChannels = 0;
    emit runningChanged();
}

bool AudioEngine::isRunning() const
{
    return m_capture.isRunning() && m_render.isRunning();
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
    }

    // Process in place. The capture-side scratch buffer is mutable; the UI
    // thread never reads it.
    m_chain->process(const_cast<float *>(interleaved),
                     static_cast<std::size_t>(numFrames));

    m_render.write(interleaved, numFrames, numChannels);
}

} // namespace host
