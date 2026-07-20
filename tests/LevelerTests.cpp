#include "dsp/Leveler.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr std::size_t kChannels = 2;
constexpr std::size_t kBlockFrames = 480;
constexpr double kPi = 3.14159265358979323846;

float runSteadyTone(float amplitude, float seconds)
{
    dsp::Leveler leveler;
    leveler.prepare(kSampleRate, kChannels);

    std::vector<float> block(kBlockFrames * kChannels);
    double phase = 0.0;
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

    return ok ? 0 : 1;
}
