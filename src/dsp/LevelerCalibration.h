#pragma once

#include <cstdint>
#include <type_traits>

namespace dsp {

inline constexpr uint32_t kLevelerCalibrationVersion = 2u;
inline constexpr int kSpectralCalibrationBandCount = 10;

// Small, allocation-free snapshot of the learned parts of the leveler chain.
// Filter delay lines and measurement windows are deliberately excluded: those
// are transient stream state and are rebuilt when a replacement APO locks.
struct LoudnessLevelerCalibration {
    float longTermLufs = 0.0f;
    float smoothedGainDb = 0.0f;
    // 0: absent; 1: learned estimate; 2: hold gain and acquire a fresh estimate.
    // The acquiring state lets a replacement stream finish a manual relearn.
    uint32_t valid = 0;
};

struct SpectralLevelerCalibration {
    float widePower = 0.0f;
    float bandPower[kSpectralCalibrationBandCount] = {};
    float gainDb[kSpectralCalibrationBandCount] = {};
    uint32_t valid = 0;
};

struct LevelerChainCalibration {
    uint32_t version = kLevelerCalibrationVersion;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    LoudnessLevelerCalibration input;
    SpectralLevelerCalibration spectral;
    LoudnessLevelerCalibration output;
};

static_assert(std::is_trivially_copyable_v<LevelerChainCalibration>);

} // namespace dsp
