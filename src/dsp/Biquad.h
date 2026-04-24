#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace dsp {

// Direct-form-I biquad with per-channel state.
// Coefficients follow the RBJ Audio EQ Cookbook (b normalised by a0).
class Biquad
{
public:
    enum class Type {
        Peaking,
        LowShelf,
        HighShelf,
        LowPass,
        HighPass,
        Notch,
    };

    void reset(std::size_t channels)
    {
        m_state.assign(channels, State{});
    }

    void setBypass(bool bypass) { m_bypass = bypass; }
    bool isBypassed() const { return m_bypass; }

    // Compute coefficients. gainDb is only used for Peaking/LowShelf/HighShelf.
    void setParams(Type type, double sampleRate, double freqHz, double q, double gainDb)
    {
        if (sampleRate <= 0.0 || freqHz <= 0.0)
            return;

        // Clamp frequency to a sane band — avoid Nyquist instability and DC degeneracies.
        const double maxFreq = sampleRate * 0.49;
        if (freqHz > maxFreq)
            freqHz = maxFreq;
        if (freqHz < 10.0)
            freqHz = 10.0;
        if (q < 0.05)
            q = 0.05;

        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * 3.14159265358979323846 * freqHz / sampleRate;
        const double cosw0 = std::cos(w0);
        const double sinw0 = std::sin(w0);
        const double alpha = sinw0 / (2.0 * q);

        double b0 = 1.0, b1 = 0.0, b2 = 0.0;
        double a0 = 1.0, a1 = 0.0, a2 = 0.0;

        switch (type) {
        case Type::Peaking: {
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cosw0;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cosw0;
            a2 = 1.0 - alpha / A;
            break;
        }
        case Type::LowShelf: {
            const double sqrtA2alpha = 2.0 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0) - (A - 1.0) * cosw0 + sqrtA2alpha);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0);
            b2 = A * ((A + 1.0) - (A - 1.0) * cosw0 - sqrtA2alpha);
            a0 = (A + 1.0) + (A - 1.0) * cosw0 + sqrtA2alpha;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw0);
            a2 = (A + 1.0) + (A - 1.0) * cosw0 - sqrtA2alpha;
            break;
        }
        case Type::HighShelf: {
            const double sqrtA2alpha = 2.0 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0) + (A - 1.0) * cosw0 + sqrtA2alpha);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0);
            b2 = A * ((A + 1.0) + (A - 1.0) * cosw0 - sqrtA2alpha);
            a0 = (A + 1.0) - (A - 1.0) * cosw0 + sqrtA2alpha;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw0);
            a2 = (A + 1.0) - (A - 1.0) * cosw0 - sqrtA2alpha;
            break;
        }
        case Type::LowPass: {
            b0 = (1.0 - cosw0) * 0.5;
            b1 = 1.0 - cosw0;
            b2 = (1.0 - cosw0) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosw0;
            a2 = 1.0 - alpha;
            break;
        }
        case Type::HighPass: {
            b0 = (1.0 + cosw0) * 0.5;
            b1 = -(1.0 + cosw0);
            b2 = (1.0 + cosw0) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosw0;
            a2 = 1.0 - alpha;
            break;
        }
        case Type::Notch: {
            b0 = 1.0;
            b1 = -2.0 * cosw0;
            b2 = 1.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosw0;
            a2 = 1.0 - alpha;
            break;
        }
        }

        m_b0 = static_cast<float>(b0 / a0);
        m_b1 = static_cast<float>(b1 / a0);
        m_b2 = static_cast<float>(b2 / a0);
        m_a1 = static_cast<float>(a1 / a0);
        m_a2 = static_cast<float>(a2 / a0);
    }

    // Process one interleaved sample for a given channel index.
    inline float processSample(float x, std::size_t channel)
    {
        if (m_bypass || channel >= m_state.size())
            return x;
        State &s = m_state[channel];
        const float y = m_b0 * x + m_b1 * s.x1 + m_b2 * s.x2 - m_a1 * s.y1 - m_a2 * s.y2;
        s.x2 = s.x1;
        s.x1 = x;
        s.y2 = s.y1;
        s.y1 = y;
        return y;
    }

private:
    struct State {
        float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    };

    float m_b0 = 1.0f, m_b1 = 0.0f, m_b2 = 0.0f;
    float m_a1 = 0.0f, m_a2 = 0.0f;
    bool m_bypass = false;
    std::vector<State> m_state;
};

} // namespace dsp
