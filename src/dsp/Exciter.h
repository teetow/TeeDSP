#pragma once

#include "Biquad.h"
#include "Processor.h"

#include <atomic>

namespace dsp {

// Harmonic exciter: high-pass the input, push it through a soft-clipping
// non-linearity, low-pass the harmonics back below Nyquist, and mix in.
// Drive controls the saturation amount; tone shifts the focus frequency;
// mix controls how much of the harmonic content is added on top of the dry signal.
class Exciter : public Processor
{
public:
    void prepare(double sampleRate, std::size_t channels) override;
    void reset() override;
    void process(float *interleaved, std::size_t frameCount) override;

    void setDrive(float v) { m_drive.store(v < 0.0f ? 0.0f : v, std::memory_order_relaxed); }
    void setMix(float v)
    {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        m_mix.store(v, std::memory_order_relaxed);
    }
    void setToneHz(float v) { m_toneHz.store(v < 200.0f ? 200.0f : v, std::memory_order_relaxed); m_dirty = true; }

private:
    void updateFilters();

    std::atomic<float> m_drive{2.0f};
    std::atomic<float> m_mix{0.25f};
    std::atomic<float> m_toneHz{3500.0f};
    bool m_dirty = true;

    Biquad m_highpass;
    Biquad m_lowpass;
    float m_lastToneHz = 0.0f;
};

} // namespace dsp
