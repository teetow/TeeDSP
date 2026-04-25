#pragma once

#include <vector>

namespace host {

// Streaming linear-interpolation resampler. State is preserved across calls
// so per-block phase doesn't drift; the very last source frame from each call
// is kept for cross-block interpolation.
//
// Linear is intentionally minimum-viable: it nails the pitch (which is the
// audible problem when capture and render disagree on rate) at the cost of
// some imaging/aliasing far above the audible band. A polyphase FIR is the
// natural upgrade if quality ever matters.
class Resampler
{
public:
    void prepare(double srcSampleRate, double dstSampleRate, int channels);
    bool isPassthrough() const;

    // Appends interpolated frames to `dst` (interleaved). Caller manages dst.
    void process(const float *src, int srcFrames, std::vector<float> &dst);

private:
    double m_step = 1.0;     // src frames consumed per output frame
    double m_phase = 0.0;    // position in current src block, in src-frame units
    int    m_channels = 0;
    std::vector<float> m_lastFrame;  // one virtual sample at index -1 of next block
};

} // namespace host
