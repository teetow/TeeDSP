#pragma once

#include "Compressor.h"
#include "Exciter.h"
#include "ParametricEQ.h"

#include <atomic>

namespace dsp {

// Ordered chain: EQ -> Exciter -> Compressor.
// Tone-shape first, then harmonic enhancement, and finally dynamic control.
class ProcessorChain
{
public:
    void prepare(double sampleRate, std::size_t channels);
    void reset();
    void process(float *interleaved, std::size_t frameCount);

    void setBypass(bool bypass) { m_bypass.store(bypass, std::memory_order_relaxed); }
    bool isBypassed() const { return m_bypass.load(std::memory_order_relaxed); }

    void setInputTrimDb(float db) { m_inputTrimDb.store(db, std::memory_order_relaxed); }
    void setOutputTrimDb(float db) { m_outputTrimDb.store(db, std::memory_order_relaxed); }
    float inputTrimDb() const { return m_inputTrimDb.load(std::memory_order_relaxed); }
    float outputTrimDb() const { return m_outputTrimDb.load(std::memory_order_relaxed); }

    ParametricEQ &eq() { return m_eq; }
    Compressor &compressor() { return m_compressor; }
    Exciter &exciter() { return m_exciter; }

private:
    ParametricEQ m_eq;
    Compressor m_compressor;
    Exciter m_exciter;
    std::atomic<bool> m_bypass{false};
    std::atomic<float> m_inputTrimDb{0.0f};
    std::atomic<float> m_outputTrimDb{0.0f};
    double m_sampleRate = 48000.0;
    std::size_t m_channels = 2;
};

} // namespace dsp
