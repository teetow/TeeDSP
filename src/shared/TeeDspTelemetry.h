#pragma once

// A private CLAP extension that lets the TeeDSP host pull live metering data
// out of the plugin for its UI (gain-reduction meters, leveler gain readouts).
//
// CLAP core has no standard meter-readback channel, but the plugin runs
// in-process in the host, so this is just a function that copies the
// processors' existing lock-free atomic getters into a small struct. It is
// safe to call from the UI thread at meter-tick rate (~125 Hz).

#include <clap/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TEEDSP_EXT_TELEMETRY "teedsp.telemetry/2"

typedef struct teedsp_telemetry_data {
    float bandGrDb[5];          // per-EQ-band dynamic gain reduction (dB, >=0)
    float compGrDb;             // compressor gain reduction (dB, >=0)
    float levelerGainDb;        // input leveler rider gain (dB, signed)
    float spectralGainDb[4];    // spectral leveler corrections (dB, signed)
    float outputLevelerGainDb;  // output leveler rider gain (dB, signed)
} teedsp_telemetry_data;

typedef struct teedsp_telemetry {
    // Copies the current metering snapshot into *out. Never blocks.
    void (*read)(const clap_plugin_t *plugin, teedsp_telemetry_data *out);
} teedsp_telemetry;

#ifdef __cplusplus
}
#endif
