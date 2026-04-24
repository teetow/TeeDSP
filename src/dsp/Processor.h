#pragma once

#include <cstddef>
#include <cstdint>

namespace dsp {

// Common interface for an in-place processor on interleaved float audio.
class Processor
{
public:
    virtual ~Processor() = default;

    virtual void prepare(double sampleRate, std::size_t channels) = 0;
    virtual void reset() = 0;
    virtual void process(float *interleaved, std::size_t frameCount) = 0;

    void setBypass(bool bypass) { m_bypass = bypass; }
    bool isBypassed() const { return m_bypass; }

protected:
    bool m_bypass = false;
    double m_sampleRate = 48000.0;
    std::size_t m_channels = 2;
};

} // namespace dsp
