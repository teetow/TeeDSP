#pragma once

#include "WasapiLoopbackCapture.h"
#include "WasapiRender.h"

#include <QObject>
#include <QString>

namespace dsp { class ProcessorChain; }

namespace host {

class SpectrumAnalyzer;

// Orchestrates capture -> DSP -> render.
// Owns a loopback capture on the chosen source render endpoint, feeds every
// incoming packet through the given ProcessorChain, and writes the result to
// the chosen render endpoint. Also taps the audio pre- and post-DSP into a
// SpectrumAnalyzer so the UI can show input/output magnitude spectra.
class AudioEngine : public QObject
{
    Q_OBJECT

public:
    explicit AudioEngine(dsp::ProcessorChain *chain, QObject *parent = nullptr);
    ~AudioEngine() override;

    QString start(const QString &captureDeviceId, const QString &renderDeviceId);
    void stop();

    bool isRunning() const;
    QString lastError() const { return m_lastError; }

    int captureSampleRate() const { return m_captureSampleRate; }
    int captureChannels() const { return m_captureChannels; }

    SpectrumAnalyzer *analyzer() const { return m_analyzer; }

signals:
    void runningChanged();
    void errorOccurred(QString message);

private:
    void onCapturePacket(const float *interleaved, int numFrames, int numChannels, int sampleRate);

    dsp::ProcessorChain *m_chain;
    WasapiLoopbackCapture m_capture;
    WasapiRender m_render;
    SpectrumAnalyzer *m_analyzer;

    int m_captureSampleRate = 0;
    int m_captureChannels = 0;
    bool m_chainPrepared = false;
    QString m_lastError;
};

} // namespace host
