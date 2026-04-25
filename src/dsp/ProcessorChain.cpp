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
    m_eq.prepare(sampleRate, channels);
    m_compressor.prepare(sampleRate, channels);
    m_exciter.prepare(sampleRate, channels);
}

void ProcessorChain::reset()
{
    m_eq.reset();
    m_compressor.reset();
    m_exciter.reset();
}

void ProcessorChain::process(float *interleaved, std::size_t frameCount)
{
    if (m_bypass.load(std::memory_order_relaxed) || interleaved == nullptr || frameCount == 0)
        return;

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

    if (outTrimLin != 1.0f) {
        const std::size_t sampleCount = frameCount * m_channels;
        for (std::size_t i = 0; i < sampleCount; ++i)
            interleaved[i] *= outTrimLin;
    }
}

} // namespace dsp
