#pragma once

#include <QObject>
#include <QTimer>
#include <QVector>

#include <atomic>
#include <complex>
#include <mutex>
#include <vector>

namespace host {

// Captures pre-DSP and post-DSP audio packets, runs a windowed FFT on a Qt
// timer, and emits magnitude (in dB) for both signals so the UI can draw an
// input-vs-output spectrum overlay.
//
// Producer side (capture thread): pushPre / pushPost — non-blocking, mono-mixes
// and drops into a small staging buffer.
// Consumer side (UI thread): a QTimer pulls a frame, windows it, FFTs it,
// converts to dB, and emits spectraUpdated.
class SpectrumAnalyzer : public QObject
{
    Q_OBJECT

public:
    static constexpr int kFftSize = 2048;

    explicit SpectrumAnalyzer(QObject *parent = nullptr);

    void start(double sampleRate, int channels);
    void stop();
    bool isRunning() const { return m_running.load(); }

    // Pauses/resumes the FFT tick timer. Used by the UI to silence the 60 Hz
    // analyzer→EqCurve repaint chain while the window is hidden or minimized:
    // there is no point burning CPU on FFTs whose output nothing draws, and
    // a continuously refreshing QOpenGLWidget across a display-sleep / GPU
    // power transition is the most likely path to a runaway repaint loop.
    // Must be called from the timer's owning thread (the main thread).
    void setUiActive(bool active);

    // Called from the capture thread.
    void pushPre(const float *interleaved, int frames, int channels);
    void pushPost(const float *interleaved, int frames, int channels);

signals:
    void spectraUpdated(QVector<float> inMagDb,
                        QVector<float> outMagDb,
                        double sampleRate,
                        int fftSize);

private slots:
    void tick();

private:
    void pushImpl(const float *interleaved, int frames, int channels,
                  std::vector<float> &ring, std::atomic<size_t> &writeIdx);
    bool snapshot(std::vector<float> &dst, const std::vector<float> &ring,
                  std::atomic<size_t> &writeIdx);

    std::atomic<bool> m_running{false};
    std::atomic<double> m_sampleRate{48000.0};

    // Two staging rings, sized to give us a comfortable margin past kFftSize.
    static constexpr int kRingSize = kFftSize * 4;

    std::mutex m_preMutex;
    std::vector<float> m_preRing;
    std::atomic<size_t> m_preWriteIdx{0};

    std::mutex m_postMutex;
    std::vector<float> m_postRing;
    std::atomic<size_t> m_postWriteIdx{0};

    QVector<float> m_inDb;
    QVector<float> m_outDb;

    QTimer m_timer;
};

} // namespace host
