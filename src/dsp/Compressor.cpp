#include "Compressor.h"

#include <algorithm>
#include <cmath>

namespace dsp {

namespace {
constexpr float kMinDb = -120.0f;

inline float linearToDb(float v)
{
    return v > 1e-9f ? 20.0f * std::log10(v) : kMinDb;
}

inline float dbToLinear(float db)
{
    return std::pow(10.0f, db / 20.0f);
}

inline float onePoleCoeff(float timeMs, double sampleRate)
{
    const double t = timeMs * 0.001;
    if (t <= 0.0)
        return 0.0f;
    return static_cast<float>(std::exp(-1.0 / (t * sampleRate)));
}
} // namespace

void Compressor::prepare(double sampleRate, std::size_t channels)
{
    m_sampleRate = sampleRate;
    m_channels = channels;
    reset();
}

void Compressor::reset()
{
    m_envelopeDb = kMinDb;
    m_peakState.assign(m_channels, 0.0f);
}

void Compressor::process(float *interleaved, std::size_t frameCount)
{
    if (m_bypass || interleaved == nullptr || frameCount == 0 || m_channels == 0)
        return;

    const float thresholdDb = m_thresholdDb.load(std::memory_order_relaxed);
    const float ratio = m_ratio.load(std::memory_order_relaxed);
    const float kneeDb = m_kneeDb.load(std::memory_order_relaxed);
    const float attackMs = m_attackMs.load(std::memory_order_relaxed);
    const float releaseMs = m_releaseMs.load(std::memory_order_relaxed);
    const float makeupLin = dbToLinear(m_makeupDb.load(std::memory_order_relaxed));

    const float attackCoeff = onePoleCoeff(attackMs, m_sampleRate);
    const float releaseCoeff = onePoleCoeff(releaseMs, m_sampleRate);
    const float halfKnee = kneeDb * 0.5f;
    const float invRatio = 1.0f / ratio;

    float maxGrDbObserved = 0.0f;

    for (std::size_t f = 0; f < frameCount; ++f) {
        // Stereo-linked detection: take the per-frame max absolute value across channels.
        float peak = 0.0f;
        for (std::size_t c = 0; c < m_channels; ++c) {
            const float s = std::fabs(interleaved[f * m_channels + c]);
            if (s > peak)
                peak = s;
        }

        const float inDb = linearToDb(peak);

        // Asymmetric one-pole on the envelope (in dB domain for musical taper).
        const float coeff = (inDb > m_envelopeDb) ? attackCoeff : releaseCoeff;
        m_envelopeDb = inDb + (m_envelopeDb - inDb) * coeff;

        // Soft-knee gain computer.
        const float over = m_envelopeDb - thresholdDb;
        float grDb = 0.0f;
        if (kneeDb > 0.0f && over > -halfKnee && over < halfKnee) {
            const float x = over + halfKnee;
            grDb = (1.0f - invRatio) * (x * x) / (2.0f * kneeDb);
        } else if (over >= halfKnee) {
            grDb = (1.0f - invRatio) * over;
        }

        if (grDb > maxGrDbObserved)
            maxGrDbObserved = grDb;

        const float gainLin = dbToLinear(-grDb) * makeupLin;
        for (std::size_t c = 0; c < m_channels; ++c) {
            interleaved[f * m_channels + c] *= gainLin;
        }
    }

    m_currentGrDb.store(maxGrDbObserved, std::memory_order_relaxed);
}

} // namespace dsp
