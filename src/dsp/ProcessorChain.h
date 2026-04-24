#pragma once

#include "Compressor.h"
#include "Exciter.h"
#include "ParametricEQ.h"

#include <atomic>

namespace dsp {

// Ordered chain: EQ -> Compressor -> Exciter.
// Tone-shape first so the compressor sees the post-EQ signal, then add the
// harmonic sparkle on top of the controlled dynamics.
class ProcessorChain
{
public:
    void prepare(double sampleRate, std::size_t channels);
    void reset();
    void process(float *interleaved, std::size_t frameCount);

    void setBypass(bool bypass) { m_bypass.store(bypass, std::memory_order_relaxed); }
    bool isBypassed() const { return m_bypass.load(std::memory_order_relaxed); }

    ParametricEQ &eq() { return m_eq; }
    Compressor &compressor() { return m_compressor; }
    Exciter &exciter() { return m_exciter; }

private:
    ParametricEQ m_eq;
    Compressor m_compressor;
    Exciter m_exciter;
    std::atomic<bool> m_bypass{false};
    double m_sampleRate = 48000.0;
    std::size_t m_channels = 2;
};

} // namespace dsp
