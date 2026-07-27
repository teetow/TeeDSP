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
//
// The available range is not flat across the spectrum: see kSpectralBands.
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

struct SpectralBandDesign {
    float detectorHz; // band-pass centre this region is measured through
    float detectorQ;
    Biquad::Type correctionType;
    float correctionHz;
    float correctionQ;
    float maxCutDb;   // how far this band may pull its region down...
    float maxBoostDb; // ...and up. 0/0 freezes the band flat.
};

// The response curve is deliberately tilted, because "how much correction is
// welcome here" is not constant with frequency:
//
//   below 200 Hz   frozen. Bass content varies enormously between programme
//                  material and almost none of that variation is a timbre
//                  fault worth chasing; moving it was the single most audible
//                  source of restlessness.
//   200-800 Hz     trim only. This is where over-correction turns into
//                  boominess, so the band may take energy away but has very
//                  little licence to add it.
//   800 Hz-3 kHz   the useful region, but a full 12 dB swing here is far more
//                  authority than voice timbre needs. Roughly half of it.
//   3-8 kHz        full range. Muddiness versus harshness up here really does
//                  span 12 dB across the input material we see.
//
// The table is public because it is also this stage's contract with the meter:
// the UI reads centres and limits from here rather than duplicating them.
inline constexpr SpectralBandDesign kSpectralBands[SpectralLeveler::kBandCount] = {
    //   detector    Q  correction type              Hz     Q     cut  boost
    {   90.0f, 1.1f, Biquad::Type::LowShelf,     120.0f, 0.7f, 0.0f, 0.0f },
    {  150.0f, 1.1f, Biquad::Type::Peaking,      150.0f, 1.1f, 0.0f, 0.0f },
    {  240.0f, 1.1f, Biquad::Type::Peaking,      240.0f, 1.1f, 2.0f, 1.0f },
    {  400.0f, 1.1f, Biquad::Type::Peaking,      400.0f, 1.1f, 2.5f, 1.0f },
    {  660.0f, 1.1f, Biquad::Type::Peaking,      660.0f, 1.1f, 3.0f, 1.5f },
    { 1100.0f, 1.1f, Biquad::Type::Peaking,     1100.0f, 1.1f, 4.0f, 3.0f },
    { 1800.0f, 1.1f, Biquad::Type::Peaking,     1800.0f, 1.1f, 4.0f, 3.0f },
    { 2900.0f, 1.1f, Biquad::Type::Peaking,     2900.0f, 1.1f, 4.5f, 3.5f },
    { 4800.0f, 1.1f, Biquad::Type::Peaking,     4800.0f, 1.1f, 6.0f, 6.0f },
    { 8000.0f, 1.1f, Biquad::Type::HighShelf,   7700.0f, 0.7f, 6.0f, 6.0f },
};

// The widest excursion any band can reach — the meter's full-scale.
inline constexpr float kSpectralMaxGainDb = 6.0f;

inline constexpr bool spectralBandMovable(int band)
{
    return kSpectralBands[band].maxCutDb > 0.0f
        || kSpectralBands[band].maxBoostDb > 0.0f;
}

} // namespace dsp
