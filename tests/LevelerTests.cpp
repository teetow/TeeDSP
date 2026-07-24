#include "dsp/Leveler.h"
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
        for (int blockIndex = 0; blockIndex < 500; ++blockIndex) {
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
        ok &= expect("spectral leveler learns a correction",
                     std::fabs(learnedCorrection) > 0.1f, learnedCorrection);

        pushSilence(leveler, 10.0f);
        const auto afterSilence = spectralGains(leveler);
        ok &= expect("spectral calibration survives valid-buffer silence",
                     maxDifference(settled, afterSilence) < 0.01f,
                     maxDifference(settled, afterSilence));

        leveler.reset();
        const auto afterReset = spectralGains(leveler);
        ok &= expect("spectral calibration survives reset",
                     maxDifference(settled, afterReset) < 0.01f,
                     maxDifference(settled, afterReset));
    }

    return ok ? 0 : 1;
}
