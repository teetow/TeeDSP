#include "SpectralLeveler.h"

#include <algorithm>
#include <cmath>

namespace dsp {

namespace {

constexpr int kControlIntervalSamples = 64;
constexpr float kSilencePower = 1.0e-5f; // -50 dBFS RMS
constexpr float kMaxBoostDb = 6.0f;
constexpr float kMaxCutDb = 6.0f;

// These targets are the detector levels, relative to broadband RMS, of a
// reasonably natural close-mic voice. The filter bandwidths matter here, hence
// the intentionally non-flat values. They form a gentle speech contour: enough
// body and presence to stay intelligible, without forcing every voice bright.
// Ten log-spaced points (~0.72 octaves apart) refine the same four-region
// shape the previous body/low-mid/presence/brilliance contour used, fit
// through the same anchors (170→-12, 600→-11, 1800→-13, 5000→-20 dB).
constexpr float kTargetRelativeDb[SpectralLeveler::kBandCount] = {
    -14.0f, //  90 Hz  sub
    -12.5f, // 150 Hz  body
    -11.5f, // 240 Hz  body-mid
    -11.0f, // 400 Hz  low-mid
    -11.0f, // 660 Hz  low-mid
    -12.0f, //  1.1 kHz mid
    -13.0f, //  1.8 kHz presence
    -16.5f, //  2.9 kHz presence-hi
    -20.0f, //  4.8 kHz brilliance
    -26.0f, //  8.0 kHz air
};

struct BandDesign {
    float detectorHz;
    float detectorQ;
    Biquad::Type correctionType;
    float correctionHz;
    float correctionQ;
};

constexpr BandDesign kBands[SpectralLeveler::kBandCount] = {
    {   90.0f, 1.1f, Biquad::Type::LowShelf,    120.0f, 0.7f },
    {  150.0f, 1.1f, Biquad::Type::Peaking,     150.0f, 1.1f },
    {  240.0f, 1.1f, Biquad::Type::Peaking,     240.0f, 1.1f },
    {  400.0f, 1.1f, Biquad::Type::Peaking,     400.0f, 1.1f },
    {  660.0f, 1.1f, Biquad::Type::Peaking,     660.0f, 1.1f },
    { 1100.0f, 1.1f, Biquad::Type::Peaking,    1100.0f, 1.1f },
    { 1800.0f, 1.1f, Biquad::Type::Peaking,    1800.0f, 1.1f },
    { 2900.0f, 1.1f, Biquad::Type::Peaking,    2900.0f, 1.1f },
    { 4800.0f, 1.1f, Biquad::Type::Peaking,    4800.0f, 1.1f },
    { 8000.0f, 1.1f, Biquad::Type::HighShelf,  7700.0f, 0.7f },
};

inline float onePoleCoef(float timeMs, double sampleRate)
{
    if (timeMs <= 0.0f || sampleRate <= 0.0)
        return 0.0f;
    return static_cast<float>(std::exp(-1000.0
        / (static_cast<double>(timeMs) * sampleRate)));
}

inline float powerToDb(float power)
{
    return power > 1.0e-12f ? 10.0f * std::log10(power) : -120.0f;
}

inline float clampf(float value, float lo, float hi)
{
    return std::clamp(value, lo, hi);
}

} // namespace

void SpectralLeveler::prepare(double sampleRate, std::size_t channels)
{
    m_sampleRate = sampleRate;
    m_channels = channels;

    for (int b = 0; b < kBandCount; ++b) {
        m_detectors[b].reset(channels);
        m_detectors[b].setParams(Biquad::Type::BandPass, sampleRate,
                                 kBands[b].detectorHz, kBands[b].detectorQ, 0.0f);
        m_detectors[b].setBypass(false);

        m_corrections[b].reset(channels);
        m_corrections[b].setParams(kBands[b].correctionType, sampleRate,
                                   kBands[b].correctionHz, kBands[b].correctionQ, 0.0f);
        m_corrections[b].setBypass(false);
    }

    // Detector smoothing removes phoneme-scale movement. Gain smoothing is a
    // little faster when taking excess energy away than when adding energy.
    m_detectorAttackCoef = onePoleCoef(180.0f, sampleRate);
    m_detectorReleaseCoef = onePoleCoef(850.0f, sampleRate);
    m_gainAttackCoef = onePoleCoef(280.0f, sampleRate);
    m_gainReleaseCoef = onePoleCoef(650.0f, sampleRate);
    m_enableMixCoef = onePoleCoef(40.0f, sampleRate);

    m_bandPower.fill(0.0f);
    m_gainDb.fill(0.0f);
    m_filterGainDb.fill(0.0f);
    for (auto &gain : m_currentGainDb)
        gain.store(0.0f, std::memory_order_relaxed);
    m_widePower = 0.0f;
    m_controlCountdown = 0;
    m_enableMix = m_bypass ? 0.0f : 1.0f;
}

void SpectralLeveler::reset()
{
    for (int b = 0; b < kBandCount; ++b) {
        m_detectors[b].reset(m_channels);
        m_corrections[b].reset(m_channels);
        m_corrections[b].setParams(kBands[b].correctionType, m_sampleRate,
                                   kBands[b].correctionHz, kBands[b].correctionQ, 0.0f);
    }
    m_bandPower.fill(0.0f);
    m_gainDb.fill(0.0f);
    m_filterGainDb.fill(0.0f);
    for (auto &gain : m_currentGainDb)
        gain.store(0.0f, std::memory_order_relaxed);
    m_widePower = 0.0f;
    m_controlCountdown = 0;
    m_enableMix = m_bypass ? 0.0f : 1.0f;
    updateCorrectionFilters();
}

float SpectralLeveler::currentGainDb(int band) const
{
    return (band >= 0 && band < kBandCount)
        ? m_currentGainDb[band].load(std::memory_order_relaxed) : 0.0f;
}

void SpectralLeveler::updateCorrectionFilters()
{
    for (int b = 0; b < kBandCount; ++b) {
        if (std::fabs(m_gainDb[b] - m_filterGainDb[b]) < 0.005f)
            continue;
        m_corrections[b].setParams(kBands[b].correctionType, m_sampleRate,
                                   kBands[b].correctionHz, kBands[b].correctionQ,
                                   m_gainDb[b]);
        m_filterGainDb[b] = m_gainDb[b];
    }
}

void SpectralLeveler::updateGains()
{
    if (m_widePower <= kSilencePower)
        return; // Do not infer a voice contour from silence / room tone.

    const float wideDb = powerToDb(m_widePower);
    std::array<float, kBandCount> desired{};
    float meanDesired = 0.0f;
    for (int b = 0; b < kBandCount; ++b) {
        const float relativeDb = powerToDb(m_bandPower[b]) - wideDb;
        desired[b] = kTargetRelativeDb[b] - relativeDb;
        meanDesired += desired[b];
    }

    // Remove the common component: broadband Leveler owns absolute loudness;
    // this stage should only correct the spectral shape.
    meanDesired /= static_cast<float>(kBandCount);
    for (int b = 0; b < kBandCount; ++b) {
        const float target = clampf(desired[b] - meanDesired, -kMaxCutDb, kMaxBoostDb);
        const float coef = (target < m_gainDb[b]) ? m_gainAttackCoef : m_gainReleaseCoef;
        m_gainDb[b] = coef * m_gainDb[b] + (1.0f - coef) * target;
    }
    updateCorrectionFilters();
}

void SpectralLeveler::process(float *interleaved, std::size_t frameCount)
{
    if (!interleaved || frameCount == 0 || m_channels == 0)
        return;

    const float enableTarget = m_bypass ? 0.0f : 1.0f;
    for (std::size_t f = 0; f < frameCount; ++f) {
        float wideEnergy = 0.0f;
        std::array<float, kBandCount> bandEnergy{};

        for (std::size_t c = 0; c < m_channels; ++c) {
            const float x = interleaved[f * m_channels + c];
            wideEnergy += x * x;
            for (int b = 0; b < kBandCount; ++b) {
                const float d = m_detectors[b].processSample(x, c);
                bandEnergy[b] += d * d;
            }
        }

        wideEnergy /= static_cast<float>(m_channels);
        const float wideCoef = (wideEnergy > m_widePower)
            ? m_detectorAttackCoef : m_detectorReleaseCoef;
        m_widePower = wideCoef * m_widePower + (1.0f - wideCoef) * wideEnergy;
        for (int b = 0; b < kBandCount; ++b) {
            bandEnergy[b] /= static_cast<float>(m_channels);
            const float coef = (bandEnergy[b] > m_bandPower[b])
                ? m_detectorAttackCoef : m_detectorReleaseCoef;
            m_bandPower[b] = coef * m_bandPower[b] + (1.0f - coef) * bandEnergy[b];
        }

        if (++m_controlCountdown >= kControlIntervalSamples) {
            m_controlCountdown = 0;
            updateGains();
        }

        // Always run the wet filters, even while bypassed, so re-enabling is a
        // short crossfade into an already-settled state rather than a pop.
        m_enableMix = m_enableMixCoef * m_enableMix
            + (1.0f - m_enableMixCoef) * enableTarget;
        for (std::size_t c = 0; c < m_channels; ++c) {
            float &sample = interleaved[f * m_channels + c];
            const float dry = sample;
            float wet = dry;
            for (int b = 0; b < kBandCount; ++b)
                wet = m_corrections[b].processSample(wet, c);
            sample = dry + (wet - dry) * m_enableMix;
        }
    }

    for (int b = 0; b < kBandCount; ++b)
        m_currentGainDb[b].store(m_gainDb[b] * m_enableMix, std::memory_order_relaxed);
}

} // namespace dsp
