#pragma once

#include "Processor.h"

#include <atomic>
#include <vector>

namespace dsp {

// Vocal-rider style auto-leveler. A slow K-weighted (BS.1770) loudness
// follower drives a static, asymmetrically-smoothed gain so the signal
// hits a target loudness on average. Faster down-ride than up-ride keeps
// it from pumping while still protecting the downstream limiter from hot
// phrases; silence-freeze prevents room tone from being ridden up to
// target between phrases. Sits before EQ/comp so they see a level-stable
// signal regardless of source level.
class Leveler : public Processor
{
public:
    void prepare(double sampleRate, std::size_t channels) override;
    void reset() override;
    void process(float *interleaved, std::size_t frameCount) override;

    // Live readout of the rider's currently-applied gain in dB. Positive
    // means the rider is boosting; negative means it is attenuating.
    float currentGainDb() const { return m_currentGainDb.load(std::memory_order_relaxed); }

    // Retune target loudness and clamp range. Defaults suit an input-stage
    // rider correcting for highly varying source loudness (music vs. voice
    // calls); the output stage instance uses a tighter, hotter target.
    void configure(float targetLufs, float maxBoostDb, float maxCutDb);

private:
    static constexpr int   kMaxCh        = 8;
    static constexpr float kSilenceDbfs  = -50.0f;
    static constexpr float kAttackMs     =  400.0f;  // gain decreases (down-ride)
    static constexpr float kReleaseMs    = 3500.0f;  // gain increases (up-ride)
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

    ChannelState m_ch[kMaxCh];
    int   m_numCh         = 0;
    int   m_writePos      = 0;
    int   m_windowSamples = 0;
    int   m_accumulated   = 0;   // valid samples in ring since last enable

    float m_smoothedGainDb = 0.0f;   // rider's continuous tracking (always live)
    float m_enableMix      = 0.0f;   // 0..1 crossfade — what fraction of rider gain to apply
    float m_attackCoef     = 0.0f;
    float m_releaseCoef    = 0.0f;
    float m_enableMixCoef  = 0.0f;

    std::atomic<float> m_currentGainDb{0.0f};
};

} // namespace dsp
