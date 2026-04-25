#pragma once

#include "Resampler.h"
#include "WasapiLoopbackCapture.h"
#include "WasapiRender.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <vector>

namespace dsp { class ProcessorChain; }

namespace host {

class SpectrumAnalyzer;
class WasapiDeviceNotifier;

// Orchestrates capture -> DSP -> render.
//
// Capture is loopback from a chosen render endpoint (typically a virtual
// cable, e.g. VB-CABLE). Render is to a chosen physical endpoint. When
// auto-route is enabled, the render side dynamically follows device events:
// the preferred render endpoint is used when active, with the first available
// non-virtual endpoint as fallback.
class AudioEngine : public QObject
{
    Q_OBJECT

public:
    explicit AudioEngine(dsp::ProcessorChain *chain, QObject *parent = nullptr);
    ~AudioEngine() override;

    // captureDeviceId is the loopback source — typically VB-CABLE.
    // preferredRenderId is the user's preferred physical output. When
    // autoRoute is true, routing falls back to the best available non-virtual
    // endpoint if the preferred is unavailable.
    QString start(const QString &captureDeviceId,
                  const QString &preferredRenderId);
    void stop();

    bool isRunning() const;
    QString lastError() const { return m_lastError; }

    int captureSampleRate() const { return m_captureSampleRate; }
    int captureChannels() const { return m_captureChannels; }

    SpectrumAnalyzer *analyzer() const { return m_analyzer; }

    void setAutoRoute(bool enabled);
    bool autoRoute() const { return m_autoRoute; }

    void setPreferredRender(const QString &id);
    QString preferredRender() const { return m_preferredRenderId; }
    QString currentRender() const { return m_currentRenderId; }

    // Returns max observed post-DSP peak level in dBFS since last call,
    // then resets the accumulator. Values near 0 dBFS indicate likely
    // hard limiting or imminent clipping in downstream paths.
    float consumeOutputHotDbfs() { return m_recentHotDbfs.exchange(-120.0f, std::memory_order_relaxed); }
    float consumeInputPeakDbfs() { return m_recentInputPeakDbfs.exchange(-120.0f, std::memory_order_relaxed); }
    float consumeOutputPeakDbfs() { return m_recentOutputPeakDbfs.exchange(-120.0f, std::memory_order_relaxed); }
    float consumeOutputRmsDbfs() { return m_recentOutputRmsDbfs.exchange(-120.0f, std::memory_order_relaxed); }

signals:
    void runningChanged();
    void errorOccurred(QString message);
    void currentRenderChanged(QString deviceId);
    void captureFormatChanged(int sampleRate, int channels);
    // Re-emitted from WasapiDeviceNotifier; subscribers should re-enumerate.
    void devicesChanged();

private slots:
    void onDevicesChanged();

private:
    void onCapturePacket(const float *interleaved, int numFrames, int numChannels, int sampleRate);
    QString pickRenderId() const;
    bool switchRenderTo(const QString &deviceId);

    dsp::ProcessorChain *m_chain;
    WasapiLoopbackCapture m_capture;
    WasapiRender m_render;
    SpectrumAnalyzer *m_analyzer;
    WasapiDeviceNotifier *m_notifier;

    QString m_captureDeviceId;
    QString m_preferredRenderId;
    QString m_currentRenderId;
    bool m_autoRoute = true;

    int m_captureSampleRate = 0;
    int m_captureChannels = 0;
    bool m_chainPrepared = false;
    QString m_lastError;

    Resampler m_resampler;
    int m_resamplerSrcSr = 0;
    int m_resamplerDstSr = 0;
    int m_resamplerChannels = 0;
    std::vector<float> m_resampleScratch;

    std::atomic<float> m_recentHotDbfs{-120.0f};
    std::atomic<float> m_recentInputPeakDbfs{-120.0f};
    std::atomic<float> m_recentOutputPeakDbfs{-120.0f};
    std::atomic<float> m_recentOutputRmsDbfs{-120.0f};
};

} // namespace host
