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

void Leveler::configure(float targetLufs, float maxBoostDb, float maxCutDb,
                         float longTermTauSec, float relativeGateLu, float deadbandLu,
                         float glideDownTauSec, float glideUpTauSec)
{
    m_targetLufs = targetLufs;
    m_maxBoostDb = std::max(0.0f, maxBoostDb);
    m_maxCutDb   = std::max(0.0f, maxCutDb);

    m_longTermTauSec  = std::max(0.001f, longTermTauSec);
    m_relativeGateLu  = std::max(0.0f, relativeGateLu);
    m_deadbandLu      = std::max(0.0f, deadbandLu);
    m_glideDownTauSec = std::max(0.001f, glideDownTauSec);
    m_glideUpTauSec   = std::max(0.001f, glideUpTauSec);
}

void Leveler::prepare(double sampleRate, std::size_t channels)
{
    // A same-format re-prepare is a transport restart (pause/resume, stream
    // relock), not a genuine reconfiguration. We still reallocate/flush the
    // measurement window and filter memory below, but the *learned* loudness
    // and applied gain are preserved (see the tail of this function) so the
    // rider resumes where it left off instead of crawling back from unity.
    const bool formatChanged = (sampleRate != m_sampleRate)
                            || (channels   != m_channels);
    const bool coldStart     = formatChanged || !m_hasLoudnessEstimate;

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

    m_longTermCoef  = onePoleCoef(m_longTermTauSec * 1000.0f, sampleRate);
    m_enableMixCoef = onePoleCoef(kEnableMixMs, sampleRate);
    m_glideDownCoef = onePoleCoef(m_glideDownTauSec * 1000.0f, sampleRate);
    m_glideUpCoef   = onePoleCoef(m_glideUpTauSec * 1000.0f, sampleRate);

    // Cold start (first run or a real format change): forget everything and
    // re-learn from scratch. Warm restart: keep the estimate and gain so the
    // silence-freeze below holds the *correct* level until audio returns.
    if (coldStart) {
        m_longTermLufs        = m_targetLufs;
        m_hasLoudnessEstimate = false;
        m_smoothedGainDb      = 0.0f;
    }
    m_enableMix = m_bypass ? 0.0f : 1.0f;
    m_currentGainDb.store(m_smoothedGainDb * m_enableMix, std::memory_order_relaxed);
}

void Leveler::reset()
{
    // Flush *transient* state only: filter memory and the measurement window
    // must be cleared at start-of-stream / format change so stale pre-gap
    // samples don't leak into the new window and so the biquads don't click.
    // The *learned* state (loudness estimate + applied gain) is deliberately
    // preserved — a transport pause or stream relock shouldn't force the rider
    // to re-converge from unity. prepare() wipes it on a genuine cold start.
    for (int c = 0; c < m_numCh; ++c) {
        m_ch[c].pre.reset();
        m_ch[c].rlb.reset();
        std::fill(m_ch[c].ring.begin(), m_ch[c].ring.end(), 0.0f);
        m_ch[c].sumSq = 0.0;
    }
    m_writePos    = 0;
    m_accumulated = 0;
    m_enableMix   = m_bypass ? 0.0f : 1.0f;
    m_currentGainDb.store(m_smoothedGainDb * m_enableMix, std::memory_order_relaxed);
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

    // Some clients (including browsers) keep the render stream alive and send
    // ordinary BUFFER_VALID blocks filled with zeroes while paused. Gate the
    // whole block instead of individual samples: zero crossings and natural
    // gaps remain part of a real programme window, while an actually silent
    // block cannot displace learned samples or advance either calibration
    // smoother.
    float blockPeak = 0.0f;
    for (std::size_t f = 0; f < frameCount; ++f) {
        for (int c = 0; c < nCh; ++c)
            blockPeak = std::max(blockPeak,
                std::fabs(interleaved[f * m_channels + c]));
    }
    const bool calibrationFrozen = blockPeak < silenceLin;

    for (std::size_t f = 0; f < frameCount; ++f) {
        // Detector runs continuously regardless of bypass state, so the
        // toggle only fades the *application* of gain — when the user flicks
        // the rider back on it engages immediately at the right level rather
        // than warming up from scratch.
        for (int c = 0; c < nCh; ++c) {
            const float x  = interleaved[f * m_channels + c];
            const float y  = m_ch[c].rlb.process(m_ch[c].pre.process(x));
            if (!calibrationFrozen) {
                const float sq = y * y;
                m_ch[c].sumSq -= static_cast<double>(m_ch[c].ring[m_writePos]);
                m_ch[c].ring[m_writePos] = sq;
                m_ch[c].sumSq += static_cast<double>(sq);
            }
        }
        if (!calibrationFrozen) {
            m_writePos = (m_writePos + 1) % m_windowSamples;
            if (m_accumulated < m_windowSamples) ++m_accumulated;
        }

        // Channel-weight 1.0 across the board — this app is stereo, so the
        // surround weights from BS.1770 §2 don't apply.
        // Warmup only guards the *cold* bootstrap: the first estimate must be
        // taken over a reasonable window, not one noisy sample. On a warm
        // restart we already hold an estimate, so skip warmup — individual
        // early samples are negligible under the multi-second tau and the ring
        // refills long before they'd matter.
        const bool warming = !m_hasLoudnessEstimate
                          && (m_accumulated < warmupSamples);

        if (!calibrationFrozen && !warming) {
            double power = 0.0;
            for (int c = 0; c < nCh; ++c)
                power += m_ch[c].sumSq / static_cast<double>(m_accumulated);
            if (power > 1e-10) {
                const float lufs = static_cast<float>(-0.691 + 10.0 * std::log10(power));

                // Bootstrap from the first valid window before applying the
                // relative gate. Starting at the target and gating immediately
                // deadlocks quiet sources: their first reading is rejected,
                // the estimate remains at target, and the rider stays at 0 dB.
                if (!m_hasLoudnessEstimate) {
                    m_longTermLufs = lufs;
                    m_hasLoudnessEstimate = true;
                // Relative gate (EBU R128 style): a short-term dip more than
                // m_relativeGateLu below the running long-term estimate is
                // treated as musical dynamics (a quiet verse), not a genuine
                // source-level change, so it's excluded from the estimate.
                // This is what stops the rider from chasing a song's own
                // arrangement instead of just leveling between sources.
                } else if (lufs >= m_longTermLufs - m_relativeGateLu) {
                    m_longTermLufs = m_longTermCoef * m_longTermLufs
                                   + (1.0f - m_longTermCoef) * lufs;
                }

                // Deadband: ignore small errors outright so the gain sits
                // dead still on ordinary program material instead of
                // jittering around target.
                float err = m_targetLufs - m_longTermLufs;
                if (err > m_deadbandLu) err -= m_deadbandLu;
                else if (err < -m_deadbandLu) err += m_deadbandLu;
                else err = 0.0f;

                const float desiredGainDb = clampf(err, -m_maxCutDb, m_maxBoostDb);

                // Error-proportional glide: ease the applied gain toward the
                // target with a fixed time constant instead of a fixed dB/sec
                // slew. Velocity is proportional to the remaining error, so a
                // large correction moves fast when it first appears and eases
                // in as it lands (roughly complete in ~1 tau regardless of
                // size) — the opposite of a slew's "big jump crawls forever"
                // feel. Anti-pumping is already handled upstream by the
                // gated/deadbanded estimate, so a livelier actuator here can't
                // reintroduce chasing of musical dynamics. Faster down-glide
                // than up-glide keeps hot phrases off the downstream limiter.
                const float glideCoef = (desiredGainDb < m_smoothedGainDb)
                    ? m_glideDownCoef : m_glideUpCoef;
                m_smoothedGainDb = glideCoef * m_smoothedGainDb
                                 + (1.0f - glideCoef) * desiredGainDb;
            }
        }
        // While silent or warming, freeze the learned window, long-term
        // estimate, and m_smoothedGainDb (no calibration update).

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
