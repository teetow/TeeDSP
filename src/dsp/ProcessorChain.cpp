#include "ProcessorChain.h"

#include <cmath>

namespace dsp {

namespace {
inline float dbToLinear(float db)
{
    return std::pow(10.0f, db / 20.0f);
}
}

void ProcessorChain::prepare(double sampleRate, std::size_t channels)
{
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_leveler.prepare(sampleRate, channels);
    // Output stage rides toward -12 LUFS with a symmetric ±12 dB window.
    // Tighter target than the input rider since by here the chain has
    // already roughly normalized; the output stage just trims residual
    // loudness drift caused by internal gain changes (EQ, makeup, etc.).
    m_outputLeveler.configure(-12.0f, 12.0f, 12.0f);
    m_outputLeveler.prepare(sampleRate, channels);
    m_eq.prepare(sampleRate, channels);
    m_compressor.prepare(sampleRate, channels);
    m_exciter.prepare(sampleRate, channels);
}

void ProcessorChain::reset()
{
    m_leveler.reset();
    m_outputLeveler.reset();
    m_eq.reset();
    m_compressor.reset();
    m_exciter.reset();
}

void ProcessorChain::process(float *interleaved, std::size_t frameCount)
{
    if (m_bypass.load(std::memory_order_relaxed) || interleaved == nullptr || frameCount == 0)
        return;

    // Leveler runs ahead of input trim so the trim knob still rides on top
    // of the auto-leveled signal — flick the rider off and the trim's effect
    // is unchanged.
    m_leveler.process(interleaved, frameCount);

    const float inTrimLin = dbToLinear(m_inputTrimDb.load(std::memory_order_relaxed));
    const float outTrimLin = dbToLinear(m_outputTrimDb.load(std::memory_order_relaxed));

    if (inTrimLin != 1.0f) {
        const std::size_t sampleCount = frameCount * m_channels;
        for (std::size_t i = 0; i < sampleCount; ++i)
            interleaved[i] *= inTrimLin;
    }

    m_eq.process(interleaved, frameCount);
    m_exciter.process(interleaved, frameCount);
    m_compressor.process(interleaved, frameCount);

    // Width control on stereo content: 0.0 = mono sum, 1.0 = unchanged stereo.
    if (m_channels >= 2) {
        float width = m_stereoWidth.load(std::memory_order_relaxed);
        if (width < 0.0f) width = 0.0f;
        if (width > 1.0f) width = 1.0f;
        if (width < 1.0f) {
            const float sideGain = width;
            for (std::size_t f = 0; f < frameCount; ++f) {
                const std::size_t base = f * m_channels;
                const float l = interleaved[base + 0];
                const float r = interleaved[base + 1];
                const float mid = 0.5f * (l + r);
                const float side = 0.5f * (l - r) * sideGain;
                interleaved[base + 0] = mid + side;
                interleaved[base + 1] = mid - side;
            }
        }
    }

    // Output leveler runs ahead of out trim — symmetric with the input
    // side (leveler → trim) and keeps Out Trim audibly functional even
    // when the leveler is engaged: trim rides on top of the leveled
    // signal rather than being absorbed by it.
    m_outputLeveler.process(interleaved, frameCount);

    if (outTrimLin != 1.0f) {
        const std::size_t sampleCount = frameCount * m_channels;
        for (std::size_t i = 0; i < sampleCount; ++i)
            interleaved[i] *= outTrimLin;
    }
}

} // namespace dsp
