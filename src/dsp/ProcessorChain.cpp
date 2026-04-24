#include "ProcessorChain.h"

namespace dsp {

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
    m_eq.process(interleaved, frameCount);
    m_compressor.process(interleaved, frameCount);
    m_exciter.process(interleaved, frameCount);
}

} // namespace dsp
