#pragma once

// The TeeDSP parameter contract — the single source of truth shared by the
// CLAP plugin (which exposes these via clap_plugin_params) and the host/UI
// (which pushes changes as param events and labels controls).
//
// This header is deliberately free of CLAP and DSP dependencies so it can be
// included from anywhere. CLAP param-flag composition and the actual DSP setter
// wiring live on the plugin side; the host only needs the IDs and descriptors.
//
// IMPORTANT: parameter IDs are persisted in plugin state and saved presets.
// Assign each ID once and NEVER renumber an existing one. Append new params
// with fresh IDs only.

#include <cstddef>
#include <cstdint>

namespace teedsp {

// ---- Per-band fields ------------------------------------------------------
// One entry per scalar field of an EQ band. Order is part of the ID contract.
enum BandField : uint32_t {
    BF_Enabled = 0,
    BF_Type,            // 0=Peaking 1=LowShelf 2=HighShelf
    BF_Freq,
    BF_Q,
    BF_Gain,
    BF_DynThreshold,
    BF_DynRatio,
    BF_DynAttack,
    BF_DynRelease,
    BF_DynRange,
    BF_Count,
};

inline constexpr int kBandCount = 5;

// Global / non-band parameter IDs. Band params live at >= 100.
enum ParamId : uint32_t {
    PID_Bypass = 0,
    PID_InputTrim,
    PID_OutputTrim,
    PID_StereoWidth,
    PID_LevelerEnabled,
    PID_OutputLevelerEnabled,
    PID_EqEnabled,
    PID_CompEnabled,
    PID_CompThreshold,
    PID_CompRatio,
    PID_CompKnee,
    PID_CompAttack,
    PID_CompRelease,
    PID_CompMakeup,
    PID_ExciterEnabled,
    PID_ExciterDrive,
    PID_ExciterMix,
    PID_ExciterTone,

    // Kept after every existing global ID. Its descriptor intentionally lives
    // after the legacy band descriptors so older raw plugin-state blobs retain
    // their original value ordering.
    PID_SpectralLevelerEnabled,

    PID_GlobalCount,    // = 19; not a real param
};

// Band params are encoded as a stable offset block. Band b, field f maps to
// id = kBandParamBase + b*BF_Count + f.
inline constexpr uint32_t kBandParamBase = 100;

inline constexpr uint32_t bandParamId(int band, BandField field)
{
    return kBandParamBase + static_cast<uint32_t>(band) * BF_Count + field;
}
inline constexpr bool isBandParam(uint32_t id) { return id >= kBandParamBase; }
inline constexpr int bandOf(uint32_t id)
{
    return static_cast<int>((id - kBandParamBase) / BF_Count);
}
inline constexpr BandField fieldOf(uint32_t id)
{
    return static_cast<BandField>((id - kBandParamBase) % BF_Count);
}

inline constexpr int kLegacyGlobalParamCount = PID_SpectralLevelerEnabled; // 18
inline constexpr int kParamCount = PID_GlobalCount + kBandCount * BF_Count; // 19 + 50 = 69

// ---- Descriptor table -----------------------------------------------------
// Ordered by CLAP param index (0..kParamCount-1). `id` is the stable ParamId;
// it need not equal the index. `stepped` marks booleans/enums (the plugin ORs
// CLAP_PARAM_IS_STEPPED for these).
struct ParamDescriptor {
    uint32_t    id;
    const char *module;   // grouping label, e.g. "Compressor", "EQ Band 1"
    const char *name;     // control label, e.g. "Threshold"
    double      minVal;
    double      maxVal;
    double      defVal;
    bool        stepped;
};

// Shared per-band defaults (band-specific type/freq are supplied per band).
#define TEEDSP_BAND(N, BASE, DEFTYPE, DEFFREQ)                                            \
    { (BASE) + BF_Enabled,      "EQ Band " #N, "Enabled",       0.0,    1.0,    1.0, true  }, \
    { (BASE) + BF_Type,         "EQ Band " #N, "Type",          0.0,    2.0,  DEFTYPE, true  }, \
    { (BASE) + BF_Freq,         "EQ Band " #N, "Frequency",    10.0, 20000.0, DEFFREQ, false }, \
    { (BASE) + BF_Q,            "EQ Band " #N, "Q",             0.05,   10.0,   0.7, false }, \
    { (BASE) + BF_Gain,         "EQ Band " #N, "Gain",        -24.0,   24.0,   0.0, false }, \
    { (BASE) + BF_DynThreshold, "EQ Band " #N, "Dyn Threshold",-60.0,   0.0,   0.0, false }, \
    { (BASE) + BF_DynRatio,     "EQ Band " #N, "Dyn Ratio",     1.0,   20.0,   2.0, false }, \
    { (BASE) + BF_DynAttack,    "EQ Band " #N, "Dyn Attack",    0.1,  200.0,  10.0, false }, \
    { (BASE) + BF_DynRelease,   "EQ Band " #N, "Dyn Release",   1.0, 3000.0, 120.0, false }, \
    { (BASE) + BF_DynRange,     "EQ Band " #N, "Dyn Range",     0.0,   24.0,  12.0, false }

inline constexpr ParamDescriptor kParams[] = {
    { PID_Bypass,               "Global",     "Bypass",          0.0,    1.0,   0.0, true  },
    { PID_InputTrim,            "Global",     "Input Trim",    -24.0,   24.0,   0.0, false },
    { PID_OutputTrim,           "Global",     "Output Trim",   -24.0,   24.0,   0.0, false },
    { PID_StereoWidth,          "Global",     "Stereo Width",    0.0,    1.0,   1.0, false },
    { PID_LevelerEnabled,       "Leveler",    "Input Leveler",   0.0,    1.0,   0.0, true  },
    { PID_OutputLevelerEnabled, "Leveler",    "Output Leveler",  0.0,    1.0,   0.0, true  },
    { PID_EqEnabled,            "EQ",         "EQ Enabled",      0.0,    1.0,   1.0, true  },
    { PID_CompEnabled,          "Compressor", "Enabled",         0.0,    1.0,   1.0, true  },
    { PID_CompThreshold,        "Compressor", "Threshold",     -60.0,    0.0, -18.0, false },
    { PID_CompRatio,            "Compressor", "Ratio",           1.0,   20.0,   4.0, false },
    { PID_CompKnee,             "Compressor", "Knee",            0.0,   24.0,   6.0, false },
    { PID_CompAttack,           "Compressor", "Attack",          0.1,  200.0,  10.0, false },
    { PID_CompRelease,          "Compressor", "Release",         1.0, 3000.0, 120.0, false },
    { PID_CompMakeup,           "Compressor", "Makeup",          0.0,   12.0,   0.0, false },
    { PID_ExciterEnabled,       "Exciter",    "Enabled",         0.0,    1.0,   1.0, true  },
    { PID_ExciterDrive,         "Exciter",    "Drive",           0.0,   20.0,   2.0, false },
    { PID_ExciterMix,           "Exciter",    "Mix",             0.0,    1.0,  0.25, false },
    { PID_ExciterTone,          "Exciter",    "Tone",          200.0, 16000.0, 3500.0, false },

    TEEDSP_BAND(1, kBandParamBase + 0 * BF_Count, 1.0,    80.0),  // low shelf
    TEEDSP_BAND(2, kBandParamBase + 1 * BF_Count, 0.0,   250.0),  // peaking
    TEEDSP_BAND(3, kBandParamBase + 2 * BF_Count, 0.0,  1000.0),  // peaking
    TEEDSP_BAND(4, kBandParamBase + 3 * BF_Count, 0.0,  4000.0),  // peaking
    TEEDSP_BAND(5, kBandParamBase + 4 * BF_Count, 2.0, 10000.0),  // high shelf

    { PID_SpectralLevelerEnabled, "Spectral Leveler", "Enabled", 0.0, 1.0, 0.0, true },
};

#undef TEEDSP_BAND

static_assert(sizeof(kParams) / sizeof(kParams[0]) == kParamCount,
              "kParams table size must match kParamCount");

// Linear lookup by ID (table is tiny; only used off the audio path or once per
// event). Returns nullptr if not found.
inline const ParamDescriptor *findParam(uint32_t id)
{
    for (const auto &d : kParams)
        if (d.id == id)
            return &d;
    return nullptr;
}

} // namespace teedsp
