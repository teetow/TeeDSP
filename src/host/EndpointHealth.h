#pragma once

#include "WasapiDevices.h"

#include <QString>

namespace host {

// TeeDSP owns only the health of its APO processing path. Bluetooth endpoint
// lifecycle, persisted A2DP formats, and transport recovery belong to BluePod.
// This model therefore asks one product-specific question: is audio present on
// the current output while the bound, non-bypassed TeeDSP APO is not processing?
// If sustained, the UI may offer an Audiosrv restart to reload the APO.
//
// The cached-format wedge -- endpoint ACTIVE, APO bound, and every shared-mode
// open failing AUDCLNT_E_UNSUPPORTED_FORMAT because the endpoint's cached rate
// disagrees with the A2DP link -- is therefore NOT handled here. It lives in
// TeeToys/src/TeeToys.BluePod/BluePodEndpointFormatService.cs. Said explicitly
// because that repair briefly lived on this side, and the notes pointing readers
// here outlived the code by some margin.

enum class EndpointVerdict {
    Unknown,      // nothing probed yet, or no default endpoint
    Idle,         // healthy, nothing playing -- not a fault
    Active,       // healthy, audio flowing and being processed
    Bypassed,     // healthy, TeeDSP deliberately out of the path
    NotOnOutput,  // healthy, APO simply not bound to this endpoint
    EffectsDisabled, // bound, but Windows is skipping every endpoint APO
    NotFlowing,   // audio present at the endpoint but the APO is not processing
};

// What the host already knows and should not be recomputed here.
struct HealthInputs {
    QString defaultRenderId;
    QString defaultRenderName;
    bool    apoBound = false;      // TeeDSP APO bound to the default endpoint
    bool    effectsEnabled = true; // Windows system-effects chain enabled
    bool    bypassed = false;
    bool    apoProcessing = false; // shared-block processCalls advancing
    // A recovery was just kicked off (service restarting, APO reloading). While
    // set, report only healthy verdicts and repair nothing: the engine is
    // legitimately in flux for a few seconds and accusing it would both lie to
    // the user and stack a second repair on top of one already in progress.
    bool    suppressFaults = false;
};

struct EndpointHealthResult {
    EndpointVerdict verdict = EndpointVerdict::Unknown;
    float peak = -1.0f;            // endpoint meter, independent of the APO
    bool  needsServiceRestart = false;
    bool  needsEffectsEnable = false;
};

// Call once per UI status tick (~400ms).
EndpointHealthResult tickEndpointHealth(const HealthInputs &in);

} // namespace host
