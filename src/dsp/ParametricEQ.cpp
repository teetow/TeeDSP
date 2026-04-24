#include "ParametricEQ.h"

namespace dsp {

namespace {
Biquad::Type toBiquadType(ParametricEQ::BandType t)
{
    switch (t) {
    case ParametricEQ::BandType::LowShelf: return Biquad::Type::LowShelf;
    case ParametricEQ::BandType::HighShelf: return Biquad::Type::HighShelf;
    case ParametricEQ::BandType::Peaking:
    default: return Biquad::Type::Peaking;
    }
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
        m_dirty[i].store(true);
    }
}

void ParametricEQ::prepare(double sampleRate, std::size_t channels)
{
    m_sampleRate = sampleRate;
    m_channels = channels;
    for (int i = 0; i < kEqBandCount; ++i) {
        m_filters[i].reset(channels);
        m_dirty[i].store(true);
    }
    recomputeIfNeeded();
}

void ParametricEQ::reset()
{
    for (int i = 0; i < kEqBandCount; ++i)
        m_filters[i].reset(m_channels);
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
        m_filters[i].setParams(toBiquadType(type),
                               m_sampleRate,
                               m_bands[i].frequencyHz.load(std::memory_order_relaxed),
                               m_bands[i].q.load(std::memory_order_relaxed),
                               m_bands[i].gainDb.load(std::memory_order_relaxed));
        m_filters[i].setBypass(!m_bands[i].enabled.load(std::memory_order_relaxed));
    }
}

void ParametricEQ::process(float *interleaved, std::size_t frameCount)
{
    if (m_bypass || interleaved == nullptr || frameCount == 0 || m_channels == 0)
        return;

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

} // namespace dsp
