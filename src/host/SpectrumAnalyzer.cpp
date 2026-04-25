#include "SpectrumAnalyzer.h"

#include "Fft.h"

#include <algorithm>
#include <cmath>

namespace host {

namespace {
constexpr int kTickIntervalMs = 33;          // ~30 Hz spectrum updates
constexpr float kSmoothingAlpha = 0.55f;     // EMA on the dB output

constexpr int kBins = SpectrumAnalyzer::kFftSize / 2 + 1;
} // namespace

SpectrumAnalyzer::SpectrumAnalyzer(QObject *parent)
    : QObject(parent)
{
    m_preRing.assign(kRingSize, 0.0f);
    m_postRing.assign(kRingSize, 0.0f);
    m_inDb.resize(kBins);
    m_outDb.resize(kBins);
    m_inDb.fill(-120.0f);
    m_outDb.fill(-120.0f);

    m_timer.setInterval(kTickIntervalMs);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &SpectrumAnalyzer::tick);
}

void SpectrumAnalyzer::start(double sampleRate, int /*channels*/)
{
    m_sampleRate.store(sampleRate);
    m_running.store(true);
    if (!m_timer.isActive()) m_timer.start();
}

void SpectrumAnalyzer::stop()
{
    m_running.store(false);
    m_timer.stop();
    m_inDb.fill(-120.0f);
    m_outDb.fill(-120.0f);
    m_preWriteIdx.store(0);
    m_postWriteIdx.store(0);
    // Empty vectors signal "no data" to the UI so it stops drawing the overlay.
    emit spectraUpdated(QVector<float>(), QVector<float>(),
                        m_sampleRate.load(), kFftSize);
}

void SpectrumAnalyzer::pushImpl(const float *src, int frames, int channels,
                                std::vector<float> &ring,
                                std::atomic<size_t> &writeIdx)
{
    if (!src || frames <= 0 || channels <= 0) return;
    if (!m_running.load()) return;

    // Down-mix to mono. For visualization this is plenty.
    size_t w = writeIdx.load(std::memory_order_relaxed);
    const size_t cap = ring.size();
    const float invCh = 1.0f / static_cast<float>(channels);
    for (int f = 0; f < frames; ++f) {
        float acc = 0.0f;
        const float *p = src + f * channels;
        for (int c = 0; c < channels; ++c) acc += p[c];
        ring[w % cap] = acc * invCh;
        ++w;
    }
    writeIdx.store(w, std::memory_order_release);
}

void SpectrumAnalyzer::pushPre(const float *src, int frames, int channels)
{
    std::lock_guard<std::mutex> g(m_preMutex);
    pushImpl(src, frames, channels, m_preRing, m_preWriteIdx);
}

void SpectrumAnalyzer::pushPost(const float *src, int frames, int channels)
{
    std::lock_guard<std::mutex> g(m_postMutex);
    pushImpl(src, frames, channels, m_postRing, m_postWriteIdx);
}

bool SpectrumAnalyzer::snapshot(std::vector<float> &dst,
                                const std::vector<float> &ring,
                                std::atomic<size_t> &writeIdx)
{
    const size_t w = writeIdx.load(std::memory_order_acquire);
    if (w < static_cast<size_t>(kFftSize)) return false;
    dst.resize(kFftSize);
    const size_t cap = ring.size();
    const size_t start = w - kFftSize;
    for (int i = 0; i < kFftSize; ++i)
        dst[i] = ring[(start + i) % cap];
    return true;
}

void SpectrumAnalyzer::tick()
{
    if (!m_running.load()) return;

    std::vector<float> preFrame;
    bool havePre = false;
    {
        std::lock_guard<std::mutex> g(m_preMutex);
        havePre = snapshot(preFrame, m_preRing, m_preWriteIdx);
    }
    std::vector<float> postFrame;
    bool havePost = false;
    {
        std::lock_guard<std::mutex> g(m_postMutex);
        havePost = snapshot(postFrame, m_postRing, m_postWriteIdx);
    }
    if (!havePre && !havePost) return;

    auto runFft = [](std::vector<float> &frame, QVector<float> &outDb) {
        Fft::hannWindow(frame.data(), static_cast<int>(frame.size()));
        std::vector<std::complex<float>> spec;
        Fft::realToComplex(frame.data(), static_cast<int>(frame.size()), spec);
        Fft::forward(spec);

        // Hann window coherent gain = 0.5 → multiply magnitudes by 2 to compensate.
        const float scale = 2.0f / static_cast<float>(SpectrumAnalyzer::kFftSize);
        for (int i = 0; i < kBins; ++i) {
            const float re = spec[i].real();
            const float im = spec[i].imag();
            const float mag = std::sqrt(re * re + im * im) * scale;
            const float db  = (mag > 1e-7f)
                ? 20.0f * std::log10(mag)
                : -120.0f;
            // Smooth in the dB domain so transients don't strobe.
            const float prev = outDb[i];
            outDb[i] = prev + kSmoothingAlpha * (db - prev);
        }
    };

    if (havePre)  runFft(preFrame,  m_inDb);
    if (havePost) runFft(postFrame, m_outDb);

    emit spectraUpdated(m_inDb, m_outDb, m_sampleRate.load(), kFftSize);
}

} // namespace host
