#include "Leveler.h"

#include <algorithm>
#include <cmath>

namespace dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;

// One-pole smoothing coefficient. The smoothed value reaches ~63% of a
// step input after tauMs. y_n = c * y_{n-1} + (1 - c) * target.
inline float onePoleCoef(float tauMs, double sampleRate)
{
    if (tauMs <= 0.0f || sampleRate <= 0.0) return 0.0f;
    return static_cast<float>(std::exp(-1000.0
        / (static_cast<double>(tauMs) * sampleRate)));
}

inline float linFromDb(float db) { return std::pow(10.0f, db / 20.0f); }

inline float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}
} // namespace

void Leveler::prepare(double sampleRate, std::size_t channels)
{
    m_sampleRate    = sampleRate;
    m_channels      = channels;
    m_numCh         = std::min<int>(static_cast<int>(channels), kMaxCh);
    m_windowSamples = std::max(1, static_cast<int>(sampleRate * kWindowSec));

    // K-weighting (BS.1770-3) — same biquad design as host::LufsMonitor.
    // Stage 1: high-shelf pre-filter.
    const double f1 = 1681.974450955533, G = 3.999843853973347, Q1 = 0.7071752369554196;
    const double K1 = std::tan(kPi * f1 / sampleRate);
    const double Vh = std::pow(10.0, G / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);
    const double d1 = 1.0 + K1 / Q1 + K1 * K1;
    const double pre_b0 = (Vh + Vb * K1 / Q1 + K1 * K1) / d1;
    const double pre_b1 = 2.0 * (K1 * K1 - Vh) / d1;
    const double pre_b2 = (Vh - Vb * K1 / Q1 + K1 * K1) / d1;
    const double pre_a1 = 2.0 * (K1 * K1 - 1.0) / d1;
    const double pre_a2 = (1.0 - K1 / Q1 + K1 * K1) / d1;
    // Stage 2: RLB high-pass.
    const double f2 = 38.13547087602444, Q2 = 0.5003270373238773;
    const double K2 = std::tan(kPi * f2 / sampleRate);
    const double d2 = 1.0 + K2 / Q2 + K2 * K2;
    const double rlb_b0 =  1.0 / d2,  rlb_b1 = -2.0 / d2,  rlb_b2 = 1.0 / d2;
    const double rlb_a1 = 2.0 * (K2 * K2 - 1.0) / d2;
    const double rlb_a2 = (1.0 - K2 / Q2 + K2 * K2) / d2;

    for (int c = 0; c < m_numCh; ++c) {
        auto &ch = m_ch[c];
        ch.pre.b0 = pre_b0; ch.pre.b1 = pre_b1; ch.pre.b2 = pre_b2;
        ch.pre.a1 = pre_a1; ch.pre.a2 = pre_a2; ch.pre.reset();
        ch.rlb.b0 = rlb_b0; ch.rlb.b1 = rlb_b1; ch.rlb.b2 = rlb_b2;
        ch.rlb.a1 = rlb_a1; ch.rlb.a2 = rlb_a2; ch.rlb.reset();
        ch.ring.assign(static_cast<size_t>(m_windowSamples), 0.0f);
        ch.sumSq = 0.0;
    }
    m_writePos    = 0;
    m_accumulated = 0;

    m_attackCoef     = onePoleCoef(kAttackMs,    sampleRate);
    m_releaseCoef    = onePoleCoef(kReleaseMs,   sampleRate);
    m_enableMixCoef  = onePoleCoef(kEnableMixMs, sampleRate);

    m_smoothedGainDb = 0.0f;
    m_enableMix      = m_bypass ? 0.0f : 1.0f;
    m_currentGainDb.store(0.0f, std::memory_order_relaxed);
}

void Leveler::reset()
{
    for (int c = 0; c < m_numCh; ++c) {
        m_ch[c].pre.reset();
        m_ch[c].rlb.reset();
        std::fill(m_ch[c].ring.begin(), m_ch[c].ring.end(), 0.0f);
        m_ch[c].sumSq = 0.0;
    }
    m_writePos       = 0;
    m_accumulated    = 0;
    m_smoothedGainDb = 0.0f;
    m_enableMix      = m_bypass ? 0.0f : 1.0f;
    m_currentGainDb.store(0.0f, std::memory_order_relaxed);
}

void Leveler::process(float *interleaved, std::size_t frameCount)
{
    if (interleaved == nullptr || frameCount == 0
        || m_numCh <= 0 || m_windowSamples <= 0)
        return;

    const int   nCh           = std::min(static_cast<int>(m_channels), m_numCh);
    const float silenceLin    = linFromDb(kSilenceDbfs);
    const int   warmupSamples = m_windowSamples / 2;
    const float enableTarget  = m_bypass ? 0.0f : 1.0f;

    for (std::size_t f = 0; f < frameCount; ++f) {
        // Detector runs continuously regardless of bypass state, so the
        // toggle only fades the *application* of gain — when the user flicks
        // the rider back on it engages immediately at the right level rather
        // than warming up from scratch.
        float framePeak = 0.0f;
        for (int c = 0; c < nCh; ++c) {
            const float x  = interleaved[f * m_channels + c];
            const float y  = m_ch[c].rlb.process(m_ch[c].pre.process(x));
            const float sq = y * y;
            m_ch[c].sumSq -= static_cast<double>(m_ch[c].ring[m_writePos]);
            m_ch[c].ring[m_writePos] = sq;
            m_ch[c].sumSq += static_cast<double>(sq);

            const float ax = std::fabs(x);
            if (ax > framePeak) framePeak = ax;
        }
        m_writePos = (m_writePos + 1) % m_windowSamples;
        if (m_accumulated < m_windowSamples) ++m_accumulated;

        // Channel-weight 1.0 across the board — this app is stereo, so the
        // surround weights from BS.1770 §2 don't apply.
        const bool silent  = (framePeak < silenceLin);
        const bool warming = (m_accumulated < warmupSamples);

        if (!silent && !warming) {
            double power = 0.0;
            for (int c = 0; c < nCh; ++c)
                power += m_ch[c].sumSq / static_cast<double>(m_accumulated);
            if (power > 1e-10) {
                const float lufs = static_cast<float>(-0.691 + 10.0 * std::log10(power));
                const float targetGainDb =
                    clampf(kTargetLufs - lufs, -kMaxCutDb, kMaxBoostDb);
                // Asymmetric one-pole: faster down-ride than up-ride.
                const float coef = (targetGainDb < m_smoothedGainDb)
                                       ? m_attackCoef : m_releaseCoef;
                m_smoothedGainDb = coef * m_smoothedGainDb
                                 + (1.0f - coef) * targetGainDb;
            }
        }
        // While silent or warming, freeze m_smoothedGainDb (no update).

        // Crossfade between unity and the rider's tracked gain. Short tau
        // (~40 ms) is fast enough to feel responsive on toggle, slow enough
        // to avoid clicks.
        m_enableMix = m_enableMixCoef * m_enableMix
                    + (1.0f - m_enableMixCoef) * enableTarget;

        const float appliedDb = m_smoothedGainDb * m_enableMix;
        const float gainLin   = linFromDb(appliedDb);
        for (int c = 0; c < nCh; ++c)
            interleaved[f * m_channels + c] *= gainLin;
    }

    m_currentGainDb.store(m_smoothedGainDb * m_enableMix,
                          std::memory_order_relaxed);
}

} // namespace dsp
