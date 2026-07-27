#include "dsp/Leveler.h"
#include "dsp/ProcessorChain.h"
#include "dsp/SpectralLeveler.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr std::size_t kChannels = 2;
constexpr std::size_t kBlockFrames = 480;
constexpr double kPi = 3.14159265358979323846;

// Push `seconds` of a steady 1 kHz tone through an existing leveler, advancing
// `phase` so repeated calls form one continuous tone. Returns the applied gain.
float pushTone(dsp::Leveler &leveler, float amplitude, float seconds, double &phase)
{
    std::vector<float> block(kBlockFrames * kChannels);
    const double phaseStep = 2.0 * kPi * 1000.0 / kSampleRate;
    const int blocks = static_cast<int>(seconds * kSampleRate / kBlockFrames);
    for (int blockIndex = 0; blockIndex < blocks; ++blockIndex) {
        for (std::size_t frame = 0; frame < kBlockFrames; ++frame) {
            const float sample = amplitude * static_cast<float>(std::sin(phase));
            phase += phaseStep;
            if (phase >= 2.0 * kPi) phase -= 2.0 * kPi;
            block[frame * kChannels] = sample;
            block[frame * kChannels + 1] = sample;
        }
        leveler.process(block.data(), kBlockFrames);
    }
    return leveler.currentGainDb();
}

float runSteadyTone(float amplitude, float seconds)
{
    dsp::Leveler leveler;
    leveler.prepare(kSampleRate, kChannels);
    double phase = 0.0;
    return pushTone(leveler, amplitude, seconds, phase);
}

template<typename Processor>
void pushSilence(Processor &processor, float seconds)
{
    std::vector<float> block(kBlockFrames * kChannels, 0.0f);
    const int blocks = static_cast<int>(seconds * kSampleRate / kBlockFrames);
    for (int blockIndex = 0; blockIndex < blocks; ++blockIndex) {
        std::fill(block.begin(), block.end(), 0.0f);
        processor.process(block.data(), kBlockFrames);
    }
}

std::array<float, dsp::SpectralLeveler::kBandCount>
spectralGains(const dsp::SpectralLeveler &leveler)
{
    std::array<float, dsp::SpectralLeveler::kBandCount> gains{};
    for (int band = 0; band < dsp::SpectralLeveler::kBandCount; ++band)
        gains[band] = leveler.currentGainDb(band);
    return gains;
}

float maxDifference(
    const std::array<float, dsp::SpectralLeveler::kBandCount> &a,
    const std::array<float, dsp::SpectralLeveler::kBandCount> &b)
{
    float difference = 0.0f;
    for (int band = 0; band < dsp::SpectralLeveler::kBandCount; ++band)
        difference = std::max(difference, std::fabs(a[band] - b[band]));
    return difference;
}

bool expect(const char *name, bool condition, float value)
{
    if (condition) {
        std::printf("PASS: %s (%.3f dB)\n", name, value);
        return true;
    }
    std::fprintf(stderr, "FAIL: %s (%.3f dB)\n", name, value);
    return false;
}

} // namespace

int main()
{
    bool ok = true;

    // A quiet source used to remain at exactly 0 dB forever because its first
    // loudness window was below the relative gate seeded from the target.
    const float quietGain = runSteadyTone(0.01f, 8.0f);
    ok &= expect("quiet source is boosted after bootstrap", quietGain > 0.5f, quietGain);

    const float hotGain = runSteadyTone(0.5f, 5.0f);
    ok &= expect("hot source is attenuated", hotGain < -0.5f, hotGain);

    // A transport pause (COM Reset() / same-format relock) must NOT throw away
    // the learned gain: reset() flushes filter memory but preserves the rider's
    // state, so resuming the same source holds its level instead of collapsing
    // to unity and re-converging. Pre-fix this settled near 0 dB after reset.
    {
        dsp::Leveler leveler;
        leveler.prepare(kSampleRate, kChannels);
        double phase = 0.0;
        const float settled = pushTone(leveler, 0.5f, 5.0f, phase);
        pushSilence(leveler, 10.0f);                       // valid zero-filled buffers
        const float afterSilence = leveler.currentGainDb();
        ok &= expect("gain survives valid-buffer silence",
                     std::fabs(afterSilence - settled) < 0.01f,
                     afterSilence - settled);

        dsp::Leveler replacement;
        replacement.prepare(kSampleRate, kChannels);
        const bool restored = replacement.restoreCalibration(leveler.calibrationState());
        ok &= expect("gain survives replacement stream instance",
                     restored
                         && std::fabs(replacement.currentGainDb() - settled) < 0.01f,
                     replacement.currentGainDb() - settled);

        leveler.reset();                                   // simulate stream flush
        const float afterReset = pushTone(leveler, 0.5f, 0.5f, phase);
        ok &= expect("gain survives reset (stays attenuating)",
                     afterReset < -0.5f, afterReset);
        ok &= expect("gain survives reset (near pre-pause level)",
                     std::fabs(afterReset - settled) < 1.0f, afterReset - settled);
    }

    // The spectral leveler used to keep decaying its detector state through
    // valid zero-filled pause buffers, then reset every learned correction to
    // 0 dB when the stream restarted.
    {
        dsp::SpectralLeveler leveler;
        leveler.prepare(kSampleRate, kChannels);
        std::vector<float> block(kBlockFrames * kChannels);
        double phase = 0.0;
        const double phaseStep = 2.0 * kPi * 1000.0 / kSampleRate;
        // Three seconds is the practical acquisition window for a new speaker.
        for (int blockIndex = 0; blockIndex < 300; ++blockIndex) {
            for (std::size_t frame = 0; frame < kBlockFrames; ++frame) {
                const float sample = 0.2f * static_cast<float>(std::sin(phase));
                phase += phaseStep;
                if (phase >= 2.0 * kPi) phase -= 2.0 * kPi;
                block[frame * kChannels] = sample;
                block[frame * kChannels + 1] = sample;
            }
            leveler.process(block.data(), kBlockFrames);
        }

        const auto settled = spectralGains(leveler);
        const float learnedCorrection = *std::max_element(
            settled.begin(), settled.end(),
            [](float a, float b) { return std::fabs(a) < std::fabs(b); });
        // A 1 kHz tone is the extreme case: only the 1.1 kHz detector sees
        // anything, so that band runs for its limit. Since the response curve
        // was tilted, that limit is 4 dB rather than 6, which caps how far
        // three seconds of one-pole movement can get.
        ok &= expect("spectral leveler responds moderately within three seconds",
                     std::fabs(learnedCorrection) > 1.5f
                         && std::fabs(learnedCorrection) < 3.5f,
                     learnedCorrection);

        // No band may exceed what its own entry allows, and the frozen low
        // bands must sit at exactly 0 dB rather than merely close to it.
        float worstOvershoot = 0.0f;
        for (int band = 0; band < dsp::SpectralLeveler::kBandCount; ++band) {
            const float gain = settled[band];
            worstOvershoot = std::max(worstOvershoot,
                std::max(gain - dsp::kSpectralBands[band].maxBoostDb,
                         -gain - dsp::kSpectralBands[band].maxCutDb));
        }
        ok &= expect("spectral leveler honours its per-band range",
                     worstOvershoot <= 0.001f, worstOvershoot);

        pushSilence(leveler, 10.0f);
        const auto afterSilence = spectralGains(leveler);
        ok &= expect("spectral calibration survives valid-buffer silence",
                     maxDifference(settled, afterSilence) < 0.01f,
                     maxDifference(settled, afterSilence));

        dsp::SpectralLeveler replacement;
        replacement.prepare(kSampleRate, kChannels);
        const bool restored = replacement.restoreCalibration(leveler.calibrationState());
        const auto afterReplacement = spectralGains(replacement);
        ok &= expect("spectral calibration survives replacement stream instance",
                     restored && maxDifference(settled, afterReplacement) < 0.01f,
                     maxDifference(settled, afterReplacement));

        leveler.reset();
        const auto afterReset = spectralGains(leveler);
        ok &= expect("spectral calibration survives reset",
                     maxDifference(settled, afterReset) < 0.01f,
                     maxDifference(settled, afterReset));
    }

    // Calibration describes programme loudness/shape, not transport format.
    // Device handoffs must preserve it even when the new endpoint negotiates
    // a different sample rate or channel count.
    {
        dsp::LevelerChainCalibration calibration;
        calibration.sampleRate = 48000;
        calibration.channels = 2;
        calibration.input = {-21.0f, 3.0f, 1u};
        calibration.output = {-9.0f, -3.0f, 1u};
        calibration.spectral.valid = 1u;
        calibration.spectral.widePower = 0.01f;
        for (int band = 0; band < dsp::kSpectralCalibrationBandCount; ++band) {
            calibration.spectral.bandPower[band] = 0.001f;
            calibration.spectral.gainDb[band] = (band % 2 == 0) ? 1.0f : -1.0f;
        }

        dsp::ProcessorChain replacement;
        replacement.prepare(44100.0, 1);
        const bool restored = replacement.restoreCalibration(calibration);
        // Band 8 (4.8 kHz) carries the full correction range, so its restored
        // gain survives verbatim; the low bands are clamped flat by design.
        ok &= expect("chain calibration crosses sample-rate/channel handoff",
                     restored
                         && std::fabs(replacement.leveler().currentGainDb() - 3.0f) < 0.01f
                         && std::fabs(replacement.outputLeveler().currentGainDb() + 3.0f) < 0.01f
                         && std::fabs(replacement.spectralLeveler().currentGainDb(8) - 1.0f) < 0.01f,
                     replacement.leveler().currentGainDb());
    }

    return ok ? 0 : 1;
}
