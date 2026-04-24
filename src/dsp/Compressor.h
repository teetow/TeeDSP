#pragma once

#include "Processor.h"

#include <atomic>
#include <vector>

namespace dsp {

// Feed-forward peak compressor with smoothed gain reduction.
// Threshold/ratio/knee in dB; attack/release in milliseconds.
class Compressor : public Processor
{
public:
    void prepare(double sampleRate, std::size_t channels) override;
    void reset() override;
    void process(float *interleaved, std::size_t frameCount) override;

    void setThresholdDb(float v) { m_thresholdDb.store(v, std::memory_order_relaxed); }
    void setRatio(float v) { m_ratio.store(v < 1.0f ? 1.0f : v, std::memory_order_relaxed); }
    void setKneeDb(float v) { m_kneeDb.store(v < 0.0f ? 0.0f : v, std::memory_order_relaxed); }
    void setAttackMs(float v) { m_attackMs.store(v < 0.1f ? 0.1f : v, std::memory_order_relaxed); }
    void setReleaseMs(float v) { m_releaseMs.store(v < 1.0f ? 1.0f : v, std::memory_order_relaxed); }
    void setMakeupDb(float v) { m_makeupDb.store(v, std::memory_order_relaxed); }

    // Most recent gain reduction in dB (positive = how much it pulled down).
    float currentGainReductionDb() const { return m_currentGrDb.load(std::memory_order_relaxed); }

private:
    std::atomic<float> m_thresholdDb{-18.0f};
    std::atomic<float> m_ratio{4.0f};
    std::atomic<float> m_kneeDb{6.0f};
    std::atomic<float> m_attackMs{10.0f};
    std::atomic<float> m_releaseMs{120.0f};
    std::atomic<float> m_makeupDb{0.0f};

    std::atomic<float> m_currentGrDb{0.0f};

    float m_envelopeDb = -120.0f;
    std::vector<float> m_peakState;
};

} // namespace dsp
