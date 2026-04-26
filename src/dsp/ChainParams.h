#pragma once

#include <cstdint>

namespace dsp {

// Flat POD snapshot of every DSP parameter.
// Must remain trivially copyable — lives inside the shared-memory block
// and is transferred via seqlock memcpy.
struct EqBandParams {
    bool    enabled;
    int32_t type;       // 0=Peaking 1=LowShelf 2=HighShelf
    float   freqHz;
    float   q;
    float   gainDb;

    float   dynThresholdDb;
    float   dynRatio;
    float   dynAttackMs;
    float   dynReleaseMs;
    float   dynRangeDb;
};

struct ChainParams {
    uint32_t    version = 3;
    bool        bypassed = false;

    float       inputTrimDb = 0.0f;
    float       outputTrimDb = 0.0f;
    bool        levelerEnabled = false;

    bool        eqEnabled = true;
    EqBandParams eqBands[5] = {
        { true, 1, 80.0f,   0.7f, 0.0f, 0.0f, 2.0f, 10.0f, 120.0f, 12.0f },
        { true, 0, 250.0f,  0.7f, 0.0f, 0.0f, 2.0f, 10.0f, 120.0f, 12.0f },
        { true, 0, 1000.0f, 0.7f, 0.0f, 0.0f, 2.0f, 10.0f, 120.0f, 12.0f },
        { true, 0, 4000.0f, 0.7f, 0.0f, 0.0f, 2.0f, 10.0f, 120.0f, 12.0f },
        { true, 2, 10000.f, 0.7f, 0.0f, 0.0f, 2.0f, 10.0f, 120.0f, 12.0f },
    };

    bool  compEnabled    = true;
    float compThreshDb   = -18.0f;
    float compRatio      = 4.0f;
    float compKneeDb     = 6.0f;
    float compAttackMs   = 10.0f;
    float compReleaseMs  = 120.0f;
    float compMakeupDb   = 0.0f;
    float stereoWidth    = 1.0f;

    bool  exciterEnabled = true;
    float exciterDrive   = 2.0f;
    float exciterMix     = 0.25f;
    float exciterToneHz  = 3500.0f;
};

} // namespace dsp
