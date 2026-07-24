#include "ProcessorChain.h"

#include <cmath>

namespace dsp {

namespace {
inline float dbToLinear(float db)
{
    return std::pow(10.0f, db / 20.0f);
}
}

void ProcessorChain::prepare(double sampleRate, std::size_t channels)
{
    m_sampleRate = sampleRate;
    m_channels = channels;
    // Input stage: normalizes wildly different source loudness (music vs.
    // voice calls) before EQ/dynamics see it, so it reacts a bit more
    // readily than the output stage — still gated/deadbanded/glide-eased
    // (see Leveler::configure defaults), just not as glacially gentle.
    m_leveler.configure(-18.0f, 18.0f, 9.0f);
    m_leveler.prepare(sampleRate, channels);
    m_spectralLeveler.prepare(sampleRate, channels);
    // Output stage rides toward -12 LUFS with a symmetric ±12 dB window.
    // Tighter target than the input rider since by here the chain has
    // already roughly normalized; the output stage just trims residual
    // loudness drift caused by internal gain changes (EQ, makeup, etc.), so
    // its ballistics are slower and its deadband wider than the input
    // stage's — it should be the last thing the listener can ever perceive
    // moving.
    m_outputLeveler.configure(-12.0f, 12.0f, 12.0f,
                               /*longTermTauSec=*/20.0f, /*relativeGateLu=*/10.0f,
                               /*deadbandLu=*/2.5f,
                               /*glideDownTauSec=*/1.2f, /*glideUpTauSec=*/3.0f);
    m_outputLeveler.prepare(sampleRate, channels);
    m_eq.prepare(sampleRate, channels);
    m_compressor.prepare(sampleRate, channels);
    m_exciter.prepare(sampleRate, channels);
}

void ProcessorChain::reset()
{
    m_leveler.reset();
    m_spectralLeveler.reset();
    m_outputLeveler.reset();
    m_eq.reset();
    m_compressor.reset();
    m_exciter.reset();
}

LevelerChainCalibration ProcessorChain::calibrationState() const noexcept
{
    LevelerChainCalibration state;
    state.sampleRate = static_cast<uint32_t>(m_sampleRate);
    state.channels = static_cast<uint32_t>(m_channels);
    state.input = m_leveler.calibrationState();
    state.spectral = m_spectralLeveler.calibrationState();
    state.output = m_outputLeveler.calibrationState();
    return state;
}

bool ProcessorChain::restoreCalibration(const LevelerChainCalibration &state)
{
    // Learned loudness, relative spectral energy, and gain corrections are
    // dimensionless programme state. The filters themselves were rebuilt by
    // prepare() for this stream's format, so a sample-rate/channel/device
    // change is a transport handoff, not a reason to discard calibration.
    if (state.version != kLevelerCalibrationVersion)
        return false;

    const bool inputRestored = m_leveler.restoreCalibration(state.input);
    const bool spectralRestored = m_spectralLeveler.restoreCalibration(state.spectral);
    const bool outputRestored = m_outputLeveler.restoreCalibration(state.output);
    return inputRestored || spectralRestored || outputRestored;
}

void ProcessorChain::process(float *interleaved, std::size_t frameCount)
{
    if (m_bypass.load(std::memory_order_relaxed) || interleaved == nullptr || frameCount == 0)
        return;

    // Leveler runs ahead of the spectral stage and input trim so the trim knob still rides on top
    // of the auto-leveled signal — flick the rider off and the trim's effect
    // is unchanged.
    m_leveler.process(interleaved, frameCount);
    m_spectralLeveler.process(interleaved, frameCount);

    const float inTrimLin = dbToLinear(m_inputTrimDb.load(std::memory_order_relaxed));
    const float outTrimLin = dbToLinear(m_outputTrimDb.load(std::memory_order_relaxed));

    if (inTrimLin != 1.0f) {
        const std::size_t sampleCount = frameCount * m_channels;
        for (std::size_t i = 0; i < sampleCount; ++i)
            interleaved[i] *= inTrimLin;
    }

    m_eq.process(interleaved, frameCount);
    m_exciter.process(interleaved, frameCount);
    m_compressor.process(interleaved, frameCount);

    // Width control on stereo content: 0.0 = mono sum, 1.0 = unchanged stereo.
    if (m_channels >= 2) {
        float width = m_stereoWidth.load(std::memory_order_relaxed);
        if (width < 0.0f) width = 0.0f;
        if (width > 1.0f) width = 1.0f;
        if (width < 1.0f) {
            const float sideGain = width;
            for (std::size_t f = 0; f < frameCount; ++f) {
                const std::size_t base = f * m_channels;
                const float l = interleaved[base + 0];
                const float r = interleaved[base + 1];
                const float mid = 0.5f * (l + r);
                const float side = 0.5f * (l - r) * sideGain;
                interleaved[base + 0] = mid + side;
                interleaved[base + 1] = mid - side;
            }
        }
    }

    // Output leveler runs ahead of out trim — symmetric with the input
    // side (leveler → trim) and keeps Out Trim audibly functional even
    // when the leveler is engaged: trim rides on top of the leveled
    // signal rather than being absorbed by it.
    m_outputLeveler.process(interleaved, frameCount);

    if (outTrimLin != 1.0f) {
        const std::size_t sampleCount = frameCount * m_channels;
        for (std::size_t i = 0; i < sampleCount; ++i)
            interleaved[i] *= outTrimLin;
    }
}

} // namespace dsp
