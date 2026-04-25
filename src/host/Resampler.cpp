#include "Resampler.h"

#include <cmath>

namespace host {

void Resampler::prepare(double srcSr, double dstSr, int channels)
{
    if (srcSr <= 0.0 || dstSr <= 0.0 || channels <= 0) {
        m_step = 1.0;
        m_phase = 0.0;
        m_channels = 0;
        m_lastFrame.clear();
        return;
    }
    m_step = srcSr / dstSr;
    m_phase = 0.0;
    m_channels = channels;
    m_lastFrame.assign(channels, 0.0f);
}

bool Resampler::isPassthrough() const
{
    return std::abs(m_step - 1.0) < 1e-9;
}

void Resampler::process(const float *src, int srcFrames, std::vector<float> &dst)
{
    if (m_channels <= 0 || srcFrames <= 0 || !src) return;

    // m_phase ∈ [-1, srcFrames - 1) is the valid output range. -1 indexes
    // m_lastFrame (carried from the previous call); srcFrames - 1 is the
    // last source sample for which we have a known successor (src[i+1]).
    while (m_phase < static_cast<double>(srcFrames - 1)) {
        const int i0   = static_cast<int>(std::floor(m_phase));
        const double t = m_phase - i0;
        for (int c = 0; c < m_channels; ++c) {
            const float s0 = (i0 < 0)
                ? m_lastFrame[c]
                : src[i0 * m_channels + c];
            const float s1 = src[(i0 + 1) * m_channels + c];
            dst.push_back(static_cast<float>(s0 + t * (s1 - s0)));
        }
        m_phase += m_step;
    }

    // Carry remaining phase into the next block's coordinate system.
    m_phase -= static_cast<double>(srcFrames);
    if (m_phase < -1.0) m_phase = -1.0;  // clamp pathological underflow

    // Save the tail so the next call can interpolate across the boundary.
    for (int c = 0; c < m_channels; ++c)
        m_lastFrame[c] = src[(srcFrames - 1) * m_channels + c];
}

} // namespace host
