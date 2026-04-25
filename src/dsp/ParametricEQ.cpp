#include "ParametricEQ.h"

#include <algorithm>
#include <cmath>

namespace dsp {

namespace {
constexpr float kMinDb = -120.0f;

Biquad::Type toBiquadType(ParametricEQ::BandType t)
{
    switch (t) {
    case ParametricEQ::BandType::LowShelf: return Biquad::Type::LowShelf;
    case ParametricEQ::BandType::HighShelf: return Biquad::Type::HighShelf;
    case ParametricEQ::BandType::Peaking:
    default: return Biquad::Type::Peaking;
    }
}

inline float linearToDb(float v)
{
    return v > 1e-9f ? 20.0f * std::log10(v) : kMinDb;
}

inline float onePoleCoeff(float timeMs, double sampleRate)
{
    const double t = timeMs * 0.001;
    if (t <= 0.0)
        return 0.0f;
    return static_cast<float>(std::exp(-1.0 / (t * sampleRate)));
}
} // namespace

ParametricEQ::ParametricEQ()
{
    // Sensible defaults across the audible spectrum.
    constexpr float defaults[kEqBandCount][2] = {
        {  80.0f,  0.0f },  // low shelf
        { 250.0f,  0.0f },  // low-mid
        {1000.0f,  0.0f },  // mid
        {4000.0f,  0.0f },  // high-mid
        {10000.0f, 0.0f },  // high shelf
    };
    constexpr ParametricEQ::BandType types[kEqBandCount] = {
        BandType::LowShelf,
        BandType::Peaking,
        BandType::Peaking,
        BandType::Peaking,
        BandType::HighShelf,
    };

    for (int i = 0; i < kEqBandCount; ++i) {
        m_bands[i].frequencyHz.store(defaults[i][0]);
        m_bands[i].gainDb.store(defaults[i][1]);
        m_bands[i].q.store(0.7f);
        m_bands[i].type.store(static_cast<int>(types[i]));
        m_bands[i].enabled.store(true);
        m_bands[i].dynThresholdDb.store(0.0f);
        m_bands[i].dynRatio.store(2.0f);
        m_bands[i].dynAttackMs.store(10.0f);
        m_bands[i].dynReleaseMs.store(120.0f);
        m_bands[i].dynRangeDb.store(12.0f);

        m_envelopeDb[i] = kMinDb;
        m_dynamicGainDb[i] = 0.0f;
        m_dynamicGrDb[i].store(0.0f, std::memory_order_relaxed);
        m_dirty[i].store(true);
    }
}

void ParametricEQ::prepare(double sampleRate, std::size_t channels)
{
    m_sampleRate = sampleRate;
    m_channels = channels;
    for (int i = 0; i < kEqBandCount; ++i) {
        m_filters[i].reset(channels);
        m_detectors[i].reset(channels);
        m_dirty[i].store(true);
        m_envelopeDb[i] = kMinDb;
        m_dynamicGainDb[i] = 0.0f;
        m_dynamicGrDb[i].store(0.0f, std::memory_order_relaxed);
    }
    recomputeIfNeeded();
}

void ParametricEQ::reset()
{
    for (int i = 0; i < kEqBandCount; ++i) {
        m_filters[i].reset(m_channels);
        m_detectors[i].reset(m_channels);
        m_envelopeDb[i] = kMinDb;
        m_dynamicGainDb[i] = 0.0f;
        m_dynamicGrDb[i].store(0.0f, std::memory_order_relaxed);
    }
}

void ParametricEQ::markDirty(int band)
{
    if (band < 0 || band >= kEqBandCount)
        return;
    m_dirty[band].store(true, std::memory_order_release);
}

void ParametricEQ::recomputeIfNeeded()
{
    for (int i = 0; i < kEqBandCount; ++i) {
        if (!m_dirty[i].exchange(false, std::memory_order_acquire))
            continue;
        const auto type = static_cast<BandType>(m_bands[i].type.load(std::memory_order_relaxed));
        const float freq = m_bands[i].frequencyHz.load(std::memory_order_relaxed);
        const float q = m_bands[i].q.load(std::memory_order_relaxed);
        const float gainDb = m_bands[i].gainDb.load(std::memory_order_relaxed) + m_dynamicGainDb[i];

        m_filters[i].setParams(toBiquadType(type),
                               m_sampleRate,
                               freq,
                               q,
                               gainDb);
        m_filters[i].setBypass(!m_bands[i].enabled.load(std::memory_order_relaxed));

        // Detector is centered on the band's frequency/Q but independent from EQ gain.
        m_detectors[i].setParams(Biquad::Type::BandPass,
                                 m_sampleRate,
                                 freq,
                                 q,
                                 0.0f);
        m_detectors[i].setBypass(!m_bands[i].enabled.load(std::memory_order_relaxed));
    }
}

void ParametricEQ::process(float *interleaved, std::size_t frameCount)
{
    if (m_bypass || interleaved == nullptr || frameCount == 0 || m_channels == 0)
        return;

    // Update the per-band dynamic gain based on the current block before
    // running the EQ filters on that block.
    for (int b = 0; b < kEqBandCount; ++b) {
        if (!m_bands[b].enabled.load(std::memory_order_relaxed)) {
            if (m_dynamicGainDb[b] != 0.0f) {
                m_dynamicGainDb[b] = 0.0f;
                markDirty(b);
            }
            m_dynamicGrDb[b].store(0.0f, std::memory_order_relaxed);
            continue;
        }

        const float thresholdDb = m_bands[b].dynThresholdDb.load(std::memory_order_relaxed);
        const float ratio = std::max(1.0f, m_bands[b].dynRatio.load(std::memory_order_relaxed));
        const float attackCoeff = onePoleCoeff(m_bands[b].dynAttackMs.load(std::memory_order_relaxed), m_sampleRate);
        const float releaseCoeff = onePoleCoeff(m_bands[b].dynReleaseMs.load(std::memory_order_relaxed), m_sampleRate);
        const float maxRangeDb = std::max(0.0f, m_bands[b].dynRangeDb.load(std::memory_order_relaxed));
        const float invRatio = 1.0f / ratio;

        float grDb = 0.0f;
        for (std::size_t f = 0; f < frameCount; ++f) {
            float peak = 0.0f;
            for (std::size_t c = 0; c < m_channels; ++c) {
                const float in = interleaved[f * m_channels + c];
                const float det = std::fabs(m_detectors[b].processSample(in, c));
                if (det > peak)
                    peak = det;
            }

            const float inDb = linearToDb(peak);
            const float coeff = (inDb > m_envelopeDb[b]) ? attackCoeff : releaseCoeff;
            m_envelopeDb[b] = inDb + (m_envelopeDb[b] - inDb) * coeff;

            const float over = m_envelopeDb[b] - thresholdDb;
            grDb = (over > 0.0f) ? ((1.0f - invRatio) * over) : 0.0f;
            if (grDb > maxRangeDb)
                grDb = maxRangeDb;
        }

        const float newDynGainDb = -grDb;
        if (std::fabs(newDynGainDb - m_dynamicGainDb[b]) > 0.01f) {
            m_dynamicGainDb[b] = newDynGainDb;
            markDirty(b);
        }
        m_dynamicGrDb[b].store(grDb, std::memory_order_relaxed);
    }

    recomputeIfNeeded();

    for (std::size_t f = 0; f < frameCount; ++f) {
        for (std::size_t c = 0; c < m_channels; ++c) {
            float &sample = interleaved[f * m_channels + c];
            float v = sample;
            for (int b = 0; b < kEqBandCount; ++b)
                v = m_filters[b].processSample(v, c);
            sample = v;
        }
    }
}

void ParametricEQ::setBandEnabled(int band, bool enabled)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_bands[band].enabled.store(enabled, std::memory_order_relaxed);
    markDirty(band);
}

void ParametricEQ::setBandType(int band, BandType type)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_bands[band].type.store(static_cast<int>(type), std::memory_order_relaxed);
    markDirty(band);
}

void ParametricEQ::setBandFrequency(int band, float hz)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (hz < 10.0f) hz = 10.0f;
    m_bands[band].frequencyHz.store(hz, std::memory_order_relaxed);
    markDirty(band);
}

void ParametricEQ::setBandQ(int band, float q)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (q < 0.05f) q = 0.05f;
    m_bands[band].q.store(q, std::memory_order_relaxed);
    markDirty(band);
}

void ParametricEQ::setBandGainDb(int band, float gainDb)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_bands[band].gainDb.store(gainDb, std::memory_order_relaxed);
    markDirty(band);
}

bool ParametricEQ::bandEnabled(int band) const
{
    if (band < 0 || band >= kEqBandCount) return false;
    return m_bands[band].enabled.load(std::memory_order_relaxed);
}

ParametricEQ::BandType ParametricEQ::bandType(int band) const
{
    if (band < 0 || band >= kEqBandCount) return BandType::Peaking;
    return static_cast<BandType>(m_bands[band].type.load(std::memory_order_relaxed));
}

float ParametricEQ::bandFrequency(int band) const
{
    if (band < 0 || band >= kEqBandCount) return 0.0f;
    return m_bands[band].frequencyHz.load(std::memory_order_relaxed);
}

float ParametricEQ::bandQ(int band) const
{
    if (band < 0 || band >= kEqBandCount) return 0.0f;
    return m_bands[band].q.load(std::memory_order_relaxed);
}

float ParametricEQ::bandGainDb(int band) const
{
    if (band < 0 || band >= kEqBandCount) return 0.0f;
    return m_bands[band].gainDb.load(std::memory_order_relaxed);
}

void ParametricEQ::setBandDynamicThresholdDb(int band, float thresholdDb)
{
    if (band < 0 || band >= kEqBandCount) return;
    m_bands[band].dynThresholdDb.store(thresholdDb, std::memory_order_relaxed);
}

void ParametricEQ::setBandDynamicRatio(int band, float ratio)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (ratio < 1.0f) ratio = 1.0f;
    m_bands[band].dynRatio.store(ratio, std::memory_order_relaxed);
}

void ParametricEQ::setBandDynamicAttackMs(int band, float attackMs)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (attackMs < 0.1f) attackMs = 0.1f;
    m_bands[band].dynAttackMs.store(attackMs, std::memory_order_relaxed);
}

void ParametricEQ::setBandDynamicReleaseMs(int band, float releaseMs)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (releaseMs < 1.0f) releaseMs = 1.0f;
    m_bands[band].dynReleaseMs.store(releaseMs, std::memory_order_relaxed);
}

void ParametricEQ::setBandDynamicRangeDb(int band, float rangeDb)
{
    if (band < 0 || band >= kEqBandCount) return;
    if (rangeDb < 0.0f) rangeDb = 0.0f;
    m_bands[band].dynRangeDb.store(rangeDb, std::memory_order_relaxed);
}

float ParametricEQ::bandDynamicThresholdDb(int band) const
{
    if (band < 0 || band >= kEqBandCount) return 0.0f;
    return m_bands[band].dynThresholdDb.load(std::memory_order_relaxed);
}

float ParametricEQ::bandDynamicRatio(int band) const
{
    if (band < 0 || band >= kEqBandCount) return 1.0f;
    return m_bands[band].dynRatio.load(std::memory_order_relaxed);
}

float ParametricEQ::bandDynamicAttackMs(int band) const
{
    if (band < 0 || band >= kEqBandCount) return 10.0f;
    return m_bands[band].dynAttackMs.load(std::memory_order_relaxed);
}

float ParametricEQ::bandDynamicReleaseMs(int band) const
{
    if (band < 0 || band >= kEqBandCount) return 120.0f;
    return m_bands[band].dynReleaseMs.load(std::memory_order_relaxed);
}

float ParametricEQ::bandDynamicRangeDb(int band) const
{
    if (band < 0 || band >= kEqBandCount) return 12.0f;
    return m_bands[band].dynRangeDb.load(std::memory_order_relaxed);
}

float ParametricEQ::bandDynamicGainReductionDb(int band) const
{
    if (band < 0 || band >= kEqBandCount) return 0.0f;
    return m_dynamicGrDb[band].load(std::memory_order_relaxed);
}

} // namespace dsp
