#pragma once

#include "Resampler.h"
#include "WasapiLoopbackCapture.h"
#include "WasapiRender.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <vector>

namespace dsp { class ProcessorChain; }

namespace host {

struct LufsMonitor;  // defined in AudioEngine.cpp
class SpectrumAnalyzer;
class WasapiDeviceNotifier;

// Orchestrates capture -> DSP -> render.
//
// Capture is loopback from a chosen render endpoint (typically a virtual
// cable, e.g. VB-CABLE). Render is to a chosen physical endpoint. The render
// side dynamically follows device events: the preferred render endpoint is
// used when it's active and non-virtual, with the first available non-virtual
// endpoint as fallback.
class AudioEngine : public QObject
{
    Q_OBJECT

public:
    explicit AudioEngine(dsp::ProcessorChain *chain, QObject *parent = nullptr);
    ~AudioEngine() override;

    // captureDeviceId is the loopback source — typically VB-CABLE.
    // preferredRenderId is the user's preferred physical output. Routing
    // falls back to the best available non-virtual endpoint if the preferred
    // one is unavailable.
    QString start(const QString &captureDeviceId,
                  const QString &preferredRenderId);
    void stop();

    bool isRunning() const;
    QString lastError() const { return m_lastError; }

    int captureSampleRate() const { return m_captureSampleRate; }
    int captureChannels() const { return m_captureChannels; }

    SpectrumAnalyzer *analyzer() const { return m_analyzer; }

    void setPreferredRender(const QString &id);
    QString preferredRender() const { return m_preferredRenderId; }
    QString currentRender() const { return m_currentRenderId; }

    // Hot-swap the capture endpoint. Unlike render switching, this is a
    // full stop/restart — capture format (rate/channels) drives the chain
    // and resampler, so there's no useful in-place handover. Expect a
    // brief silence while WASAPI tears down and rebuilds.
    void setPreferredCapture(const QString &id);
    QString preferredCapture() const { return m_captureDeviceId; }

    // Latest-packet meter telemetry in dBFS. Each capture packet stores its
    // Latest-packet meter telemetry in dBFS.
    // ch=0 → L, ch=1 → R; default ch=0 for backwards-compatible call sites.
    float currentOutputHotDbfs()            const { return m_recentHotDbfs.load(std::memory_order_relaxed); }
    float currentInputPeakDbfs(int ch = 0)  const;
    float currentOutputPeakDbfs(int ch = 0) const;
    float currentOutputRmsDbfs()            const { return m_recentOutputRmsDbfs.load(std::memory_order_relaxed); }
    float currentOutputLufsM()              const { return m_recentLufsM.load(std::memory_order_relaxed); }
    float currentOutputLufsM(int ch)        const;

signals:
    void runningChanged();
    void errorOccurred(QString message);
    void currentRenderChanged(QString deviceId);
    void captureFormatChanged(int sampleRate, int channels);
    // Re-emitted from WasapiDeviceNotifier; subscribers should re-enumerate.
    void devicesChanged();

private slots:
    void onDevicesChanged();
    // Posted from the capture thread when it observes the render thread has
    // exited unexpectedly (audio client invalidated, device went away mid-
    // stream, etc.). Re-picks and re-starts on the engine's own thread.
    void onRenderEndedUnexpectedly();

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
    std::atomic<float> m_recentInputPeakDbfs{-120.0f};   // mono-mixed, kept for compat
    std::atomic<float> m_recentOutputPeakDbfs{-120.0f};  // mono-mixed, kept for compat
    std::atomic<float> m_recentOutputRmsDbfs{-120.0f};
    std::atomic<float> m_recentInputPeakChL{-120.0f};
    std::atomic<float> m_recentInputPeakChR{-120.0f};
    std::atomic<float> m_recentOutputPeakChL{-120.0f};
    std::atomic<float> m_recentOutputPeakChR{-120.0f};
    std::atomic<float> m_recentLufsM{-70.0f};
    std::atomic<float> m_recentLufsChL{-70.0f};
    std::atomic<float> m_recentLufsChR{-70.0f};
    std::unique_ptr<LufsMonitor> m_lufsMonitor;
};

} // namespace host
