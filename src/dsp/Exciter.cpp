#include "Exciter.h"

#include <cmath>

namespace dsp {

void Exciter::prepare(double sampleRate, std::size_t channels)
{
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_highpass.reset(channels);
    m_lowpass.reset(channels);
    m_dirty = true;
    updateFilters();
}

void Exciter::reset()
{
    m_highpass.reset(m_channels);
    m_lowpass.reset(m_channels);
}

void Exciter::updateFilters()
{
    const float toneHz = m_toneHz.load(std::memory_order_relaxed);
    if (!m_dirty && std::fabs(toneHz - m_lastToneHz) < 0.5f)
        return;

    // High-pass keeps the saturator off the low end (where it would mud up the mix).
    // Low-pass tames the harmonics so we don't create aliasing fizz above ~14 kHz.
    m_highpass.setParams(Biquad::Type::HighPass, m_sampleRate, toneHz, 0.707, 0.0);
    const double lp = std::min(m_sampleRate * 0.45, 14000.0);
    m_lowpass.setParams(Biquad::Type::LowPass, m_sampleRate, lp, 0.707, 0.0);
    m_lastToneHz = toneHz;
    m_dirty = false;
}

void Exciter::process(float *interleaved, std::size_t frameCount)
{
    if (m_bypass || interleaved == nullptr || frameCount == 0 || m_channels == 0)
        return;

    if (m_dirty)
        updateFilters();

    const float drive = m_drive.load(std::memory_order_relaxed);
    const float mix = m_mix.load(std::memory_order_relaxed);
    if (mix <= 0.0001f)
        return;

    // tanh saturation gain compensation so increasing drive doesn't blow up output.
    const float invComp = 1.0f / std::tanh(drive > 0.0f ? drive : 1e-3f);

    for (std::size_t f = 0; f < frameCount; ++f) {
        for (std::size_t c = 0; c < m_channels; ++c) {
            float &sample = interleaved[f * m_channels + c];
            const float dry = sample;
            float side = m_highpass.processSample(dry, c);
            side = std::tanh(side * drive) * invComp;
            side = m_lowpass.processSample(side, c);
            sample = dry + side * mix;
        }
    }
}

} // namespace dsp
