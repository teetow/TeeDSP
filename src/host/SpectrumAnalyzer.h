#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QVector>

#include <atomic>
#include <complex>
#include <mutex>
#include <vector>

namespace host {

// Collects pre-DSP and post-DSP audio packets and emits windowed FFT magnitudes
// for the input-vs-output spectrum overlay.
//
// Producer side (capture thread): pushPre / pushPost — non-blocking, mono-mixes
// and drops into a small staging buffer.
// Consumer side (UI thread): processPending() pulls the newest frame, windows
// it, FFTs it, converts to dB, and emits spectraUpdated.
class SpectrumAnalyzer : public QObject
{
    Q_OBJECT

public:
    static constexpr int kFftSize = 2048;

    explicit SpectrumAnalyzer(QObject *parent = nullptr);

    void start(double sampleRate, int channels);
    void stop();
    bool isRunning() const { return m_running.load(); }

    // Pauses/resumes analyzer output while the UI is hidden or both spectra are
    // disabled. Disabling also clears the last frame from consumers.
    void setUiActive(bool active);

    // Called from the capture thread.
    void pushPre(const float *interleaved, int frames, int channels);
    void pushPost(const float *interleaved, int frames, int channels);

    // Runs at most one analysis pass for the samples pushed since the previous
    // UI drain. Keeping this on the drain cadence avoids an independent timer
    // repeatedly transforming the same stale frame.
    void processPending();

signals:
    void spectraUpdated(QVector<float> inMagDb,
                        QVector<float> outMagDb,
                        double sampleRate,
                        int fftSize);

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
    std::vector<float> m_preFrame;
    std::vector<float> m_postFrame;
    std::vector<float> m_hannWindow;
    std::vector<std::complex<float>> m_preSpec;
    std::vector<std::complex<float>> m_postSpec;
    QElapsedTimer m_processClock;
    bool m_uiActive = true;
};

} // namespace host
