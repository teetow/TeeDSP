#pragma once

#include "LevelerCalibration.h"
#include "Processor.h"

#include <atomic>
#include <vector>

namespace dsp {

// Vocal-rider style auto-leveler, tuned to track *source* loudness rather
// than *musical* dynamics. A fast K-weighted (BS.1770) detector feeds a
// relative-gated (EBU R128 style) long-term loudness estimate — quiet
// passages more than kRelativeGateLu below the running estimate don't pull
// it down, so a verse-to-chorus swing isn't mistaken for a level change.
// The estimate then drives gain through a deadband (small errors are
// ignored outright) and an error-proportional glide: gain eases toward the
// target with a fixed time constant, so it moves fastest right when the
// error is largest and settles gently, and a correction of any size mostly
// completes in ~1 tau instead of crawling at a fixed dB/sec (a big jump no
// longer takes proportionally forever). Anti-pumping lives in the estimate
// (tau/gate/deadband), not the actuator, so a livelier glide can't chase a
// song's own arrangement. Silence-freeze prevents room tone from being
// ridden up to target between phrases. The learned estimate and applied
// gain survive a transport pause / stream restart (only filter memory is
// flushed) so resuming playback doesn't re-converge from unity. Sits before
// EQ/comp so they see a level-stable signal regardless of source level.
class Leveler : public Processor
{
public:
    void prepare(double sampleRate, std::size_t channels) override;
    void reset() override;
    void process(float *interleaved, std::size_t frameCount) override;

    // Live readout of the rider's currently-applied gain in dB. Positive
    // means the rider is boosting; negative means it is attenuating.
    float currentGainDb() const { return m_currentGainDb.load(std::memory_order_relaxed); }

    LoudnessLevelerCalibration calibrationState() const noexcept;
    bool restoreCalibration(const LoudnessLevelerCalibration &state) noexcept;

    // Retune target loudness, clamp range, and ballistics. Defaults suit an
    // input-stage rider normalizing highly varying source loudness (music
    // vs. voice calls) ahead of fixed-threshold downstream stages, so it
    // reacts a little more readily than the output stage, which should be
    // as close to inaudible as possible and just mop up internal chain
    // drift (EQ/comp makeup gain etc).
    void configure(float targetLufs, float maxBoostDb, float maxCutDb,
                   float longTermTauSec   = 12.0f,
                   float relativeGateLu   = 10.0f,
                   float deadbandLu       =  1.5f,
                   float glideDownTauSec  =  0.8f,
                   float glideUpTauSec    =  2.0f);

private:
    static constexpr int   kMaxCh        = 8;
    static constexpr float kSilenceDbfs  = -50.0f;
    static constexpr float kWindowSec    =    3.0f;  // short-term LUFS window
    static constexpr float kEnableMixMs  =   40.0f;  // crossfade tau for enable toggle

    struct KWeightBiquad {
        double b0{1}, b1{0}, b2{0}, a1{0}, a2{0};
        double z1{0}, z2{0};
        float process(float x) noexcept {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return static_cast<float>(y);
        }
        void reset() noexcept { z1 = z2 = 0.0; }
    };

    struct ChannelState {
        KWeightBiquad pre, rlb;
        std::vector<float> ring;
        double sumSq = 0.0;
    };

    float m_targetLufs   = -18.0f;
    float m_maxBoostDb   =  18.0f;
    float m_maxCutDb     =   9.0f;

    // Ballistics — see configure().
    float m_longTermTauSec   = 12.0f;
    float m_relativeGateLu   = 10.0f;
    float m_deadbandLu       =  1.5f;
    float m_glideDownTauSec  =  0.8f;   // gain eases down (attenuating) faster…
    float m_glideUpTauSec    =  2.0f;   // …than it eases up (boosting)

    ChannelState m_ch[kMaxCh];
    int   m_numCh         = 0;
    int   m_writePos      = 0;
    int   m_windowSamples = 0;
    int   m_accumulated   = 0;   // valid samples in ring since last enable

    float m_longTermLufs   = -18.0f;  // gated, slowly-tracked loudness estimate
    bool  m_hasLoudnessEstimate = false; // first valid window bootstraps the gate
    float m_smoothedGainDb = 0.0f;    // rider's continuous tracking (always live)
    float m_enableMix      = 0.0f;    // 0..1 crossfade — what fraction of rider gain to apply
    float m_longTermCoef   = 0.0f;
    float m_glideDownCoef  = 0.0f;   // per-sample one-pole coef (attenuating)
    float m_glideUpCoef    = 0.0f;   // per-sample one-pole coef (boosting)
    float m_enableMixCoef  = 0.0f;

    std::atomic<float> m_currentGainDb{0.0f};
};

} // namespace dsp
