#pragma once

#include "Biquad.h"
#include "LevelerCalibration.h"
#include "Processor.h"

#include <array>
#include <atomic>

namespace dsp {

// A gentle speech-oriented multiband AGC. It measures ten log-spaced spectral
// regions (~90 Hz-8 kHz) relative to the incoming broadband level, then drives
// correction filters toward a reference speech contour. Keeping the mean gain
// at unity makes this a timbre leveller rather than a second loudness rider;
// Leveler remains responsible for absolute loudness.
//
// The detector and gain movement are deliberately slow, range-limited, and
// frozen below the speech gate. This makes it useful ahead of dynamic EQ for
// voice chat (different mics, voices, and codec colour) without tracking
// individual phonemes or pulling room noise up between phrases.
class SpectralLeveler : public Processor
{
public:
    static constexpr int kBandCount = 10;

    void prepare(double sampleRate, std::size_t channels) override;
    void reset() override;
    void process(float *interleaved, std::size_t frameCount) override;

    float currentGainDb(int band) const;

    SpectralLevelerCalibration calibrationState() const noexcept;
    bool restoreCalibration(const SpectralLevelerCalibration &state);

private:
    void updateCorrectionFilters();
    void updateGains();

    std::array<Biquad, kBandCount> m_detectors;
    std::array<Biquad, kBandCount> m_corrections;
    std::array<float, kBandCount> m_bandPower{};
    std::array<float, kBandCount> m_gainDb{};
    std::array<float, kBandCount> m_filterGainDb{};
    std::array<std::atomic<float>, kBandCount> m_currentGainDb;

    float m_widePower = 0.0f;
    float m_detectorAttackCoef = 0.0f;
    float m_detectorReleaseCoef = 0.0f;
    float m_gainAttackCoef = 0.0f;
    float m_gainReleaseCoef = 0.0f;
    float m_enableMixCoef = 0.0f;
    float m_enableMix = 0.0f;
    int m_controlCountdown = 0;
    bool m_prepared = false;
    bool m_hasCalibration = false;
};

static_assert(SpectralLeveler::kBandCount == kSpectralCalibrationBandCount);

} // namespace dsp
