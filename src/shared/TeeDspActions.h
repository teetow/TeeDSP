#pragma once

#include <clap/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TEEDSP_EXT_ACTIONS "teedsp.actions/1"

typedef struct teedsp_actions {
    // Thread-safe, allocation-free request. Consumed at the next audio block.
    // output=false selects the input rider, output=true the output rider.
    void (*relearn_leveler)(const clap_plugin_t *plugin, bool output);
} teedsp_actions;

#ifdef __cplusplus
}
#endif
