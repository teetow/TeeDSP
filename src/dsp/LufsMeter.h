#pragma once

// BS.1770 K-weighted momentary (400 ms) loudness meter, up to 2 channels.
// Same K-weighting design as dsp::Leveler. process() is RT-safe (running
// sliding-sum over a ring, no allocation); prepare() allocates the rings.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace dsp {

class LufsMeter {
public:
    void prepare(double sampleRate, std::size_t channels)
    {
        constexpr double kPi = 3.14159265358979323846;
        m_numCh = static_cast<int>(std::min<std::size_t>(channels, 2));
        m_win   = std::max(1, static_cast<int>(sampleRate * 0.4)); // 400 ms

        // Stage 1: high-shelf pre-filter.
        const double f1 = 1681.974450955533, G = 3.999843853973347, Q1 = 0.7071752369554196;
        const double K1 = std::tan(kPi * f1 / sampleRate);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double d1 = 1.0 + K1 / Q1 + K1 * K1;
        m_pre = { (Vh + Vb * K1 / Q1 + K1 * K1) / d1, 2.0 * (K1 * K1 - Vh) / d1,
                  (Vh - Vb * K1 / Q1 + K1 * K1) / d1, 2.0 * (K1 * K1 - 1.0) / d1,
                  (1.0 - K1 / Q1 + K1 * K1) / d1 };
        // Stage 2: RLB high-pass.
        const double f2 = 38.13547087602444, Q2 = 0.5003270373238773;
        const double K2 = std::tan(kPi * f2 / sampleRate);
        const double d2 = 1.0 + K2 / Q2 + K2 * K2;
        m_rlb = { 1.0 / d2, -2.0 / d2, 1.0 / d2, 2.0 * (K2 * K2 - 1.0) / d2,
                  (1.0 - K2 / Q2 + K2 * K2) / d2 };

        for (int c = 0; c < 2; ++c) {
            m_st[c] = State{};
            m_st[c].ring.assign(static_cast<size_t>(m_win), 0.0f);
        }
        m_writePos = 0;
        m_accum = 0;
    }

    void reset()
    {
        for (int c = 0; c < 2; ++c) {
            m_st[c].z1 = m_st[c].z2 = m_st[c].r1 = m_st[c].r2 = 0.0;
            m_st[c].sumSq = 0.0;
            std::fill(m_st[c].ring.begin(), m_st[c].ring.end(), 0.0f);
        }
        m_writePos = 0;
        m_accum = 0;
    }

    // interleaved may be null (treated as silence). stride = channels in buffer.
    void process(const float *interleaved, std::size_t frames, std::size_t stride)
    {
        if (m_numCh <= 0 || m_win <= 0) return;
        for (std::size_t f = 0; f < frames; ++f) {
            for (int c = 0; c < m_numCh; ++c) {
                const double x = interleaved ? interleaved[f * stride + c] : 0.0;
                State &s = m_st[c];
                const double y1 = m_pre.b0 * x + s.z1;
                s.z1 = m_pre.b1 * x - m_pre.a1 * y1 + s.z2;
                s.z2 = m_pre.b2 * x - m_pre.a2 * y1;
                const double y2 = m_rlb.b0 * y1 + s.r1;
                s.r1 = m_rlb.b1 * y1 - m_rlb.a1 * y2 + s.r2;
                s.r2 = m_rlb.b2 * y1 - m_rlb.a2 * y2;
                const float sq = static_cast<float>(y2 * y2);
                s.sumSq -= static_cast<double>(s.ring[m_writePos]);
                s.ring[m_writePos] = sq;
                s.sumSq += static_cast<double>(sq);
            }
            m_writePos = (m_writePos + 1) % m_win;
            if (m_accum < m_win) ++m_accum;
        }
    }

    float channelLufs(int c) const
    {
        if (c < 0 || c >= m_numCh || m_accum <= 0) return -120.0f;
        const double ms = m_st[c].sumSq / static_cast<double>(m_accum);
        return ms > 1e-10 ? static_cast<float>(-0.691 + 10.0 * std::log10(ms)) : -120.0f;
    }

    float momentaryLufs() const
    {
        if (m_accum <= 0) return -120.0f;
        double p = 0.0;
        for (int c = 0; c < m_numCh; ++c)
            p += m_st[c].sumSq / static_cast<double>(m_accum);
        return p > 1e-10 ? static_cast<float>(-0.691 + 10.0 * std::log10(p)) : -120.0f;
    }

private:
    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    struct State { double z1 = 0, z2 = 0, r1 = 0, r2 = 0, sumSq = 0; std::vector<float> ring; };

    int    m_numCh = 0;
    int    m_win = 0;
    int    m_writePos = 0;
    int    m_accum = 0;
    Biquad m_pre, m_rlb;
    State  m_st[2];
};

} // namespace dsp
