#pragma once

#include "Biquad.h"
#include "Processor.h"

#include <array>
#include <atomic>

namespace dsp {

inline constexpr int kEqBandCount = 5;

// 5-band parametric EQ. Bands 0 and 4 default to shelves; 1-3 are peaking by default.
class ParametricEQ : public Processor
{
public:
    enum class BandType {
        Peaking = 0,
        LowShelf = 1,
        HighShelf = 2,
    };

    struct BandParams {
        std::atomic<bool> enabled{true};
        std::atomic<int> type{static_cast<int>(BandType::Peaking)};
        std::atomic<float> frequencyHz{1000.0f};
        std::atomic<float> q{1.0f};
        std::atomic<float> gainDb{0.0f};

        std::atomic<float> dynThresholdDb{0.0f};
        std::atomic<float> dynRatio{2.0f};
        std::atomic<float> dynAttackMs{10.0f};
        std::atomic<float> dynReleaseMs{120.0f};
        std::atomic<float> dynRangeDb{12.0f};
    };

    ParametricEQ();

    void prepare(double sampleRate, std::size_t channels) override;
    void reset() override;
    void process(float *interleaved, std::size_t frameCount) override;

    void setBandEnabled(int band, bool enabled);
    void setBandType(int band, BandType type);
    void setBandFrequency(int band, float hz);
    void setBandQ(int band, float q);
    void setBandGainDb(int band, float gainDb);

    void setBandDynamicThresholdDb(int band, float thresholdDb);
    void setBandDynamicRatio(int band, float ratio);
    void setBandDynamicAttackMs(int band, float attackMs);
    void setBandDynamicReleaseMs(int band, float releaseMs);
    void setBandDynamicRangeDb(int band, float rangeDb);

    bool bandEnabled(int band) const;
    BandType bandType(int band) const;
    float bandFrequency(int band) const;
    float bandQ(int band) const;
    float bandGainDb(int band) const;

    float bandDynamicThresholdDb(int band) const;
    float bandDynamicRatio(int band) const;
    float bandDynamicAttackMs(int band) const;
    float bandDynamicReleaseMs(int band) const;
    float bandDynamicRangeDb(int band) const;
    float bandDynamicGainReductionDb(int band) const;

private:
    void markDirty(int band);
    void recomputeIfNeeded();

    std::array<BandParams, kEqBandCount> m_bands;
    std::array<Biquad, kEqBandCount> m_filters;
    std::array<Biquad, kEqBandCount> m_detectors;
    std::array<std::atomic<bool>, kEqBandCount> m_dirty;

    std::array<float, kEqBandCount> m_envelopeDb{};
    std::array<std::atomic<float>, kEqBandCount> m_dynamicGrDb;
    std::array<float, kEqBandCount> m_dynamicGainDb{};
};

} // namespace dsp
