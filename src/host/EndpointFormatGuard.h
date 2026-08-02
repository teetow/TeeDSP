#pragma once

#include "WasapiDevices.h"

#include <QString>

namespace host {

// ---------------------------------------------------------------------------
// Detects and repairs the Bluetooth shared-mode format wedge.
//
// The fault (reproduced and characterised 2026-08-02): an A2DP link comes up
// configured at one sample rate while the endpoint's cached
// PKEY_AudioEngine_DeviceFormat still holds another, and *nothing reconciles
// the two*. Windows will not reconfigure the link to match the cache, and will
// not update the cache to match the link, so every shared-mode
// IAudioClient::Initialize fails with AUDCLNT_E_UNSUPPORTED_FORMAT and every
// app -- browsers included -- silently refuses to play. Audio looks completely
// connected: the endpoint is ACTIVE, the device is the default, the APO is
// bound, and the headphones even play their own connect chime.
//
// It recurs because the negotiated rate genuinely varies per connection. Across
// this machine's AirPods endpoint history the cached rate was 48000 on 23
// endpoints and 44100 on 11, so a disagreement is close to a coin flip on every
// reconnect. Windows does re-derive the format correctly, but only when the
// audio engine starts -- never on reconnect while it is already running, which
// is exactly when the rate changes.
//
// Detection is assumption-free and two-stage. The cheap probe asks the endpoint
// whether its own engine mix format works in shared mode; healthy, that is true
// by construction. A refusal is necessary but not sufficient -- it reports the
// cached format disagreeing, which can occur while streams still open and audio
// still plays. So a refusal escalates to actually creating (never starting) a
// shared-mode client, which is what an app does and therefore what the user
// feels. Only when that also fails is the endpoint genuinely wedged. Repairing
// on the cheap probe alone glitches working audio, which is worse than the bug.
//
// Repair is IPolicyConfig::ResetDeviceFormat, which drops the cache and makes
// Windows re-derive from the link. Verified 2026-08-02 by poisoning the cached
// format to a rate the link rejects: Reset returned S_OK and Windows rewrote
// the cache to the link's actual rate, with no service restart.
//
// TeeDSP picks no sample rate here, and there is deliberately no
// SetDeviceFormat path anywhere in the codebase -- the link is authoritative,
// which is the only defensible direction for a remote sink.
// ---------------------------------------------------------------------------

struct FormatGuardResult {
    bool probed = false;      // a health probe actually ran on this tick
    bool wedged = false;      // both probes failed: streams genuinely cannot open
    bool repairTried = false; // ResetDeviceFormat was called
    bool repaired = false;    // ...and the endpoint probes healthy again
    long healthHr = 0;        // IsFormatSupported HRESULT (cheap probe)
    long openHr = 0;          // Initialize HRESULT, 0 if escalation never ran
    StreamFormat mixFormat;
};

// Call once per UI status tick (~400ms) with the default render endpoint id the
// caller has already resolved. Self-pacing: probes immediately when the
// endpoint changes, then every ~10s while healthy, and every tick while a wedge
// is suspected so recovery lands within about a second of a bad reconnect
// rather than at the end of a slow poll interval. Returns a mostly-empty result
// on the ticks where it decides not to probe.
FormatGuardResult tickFormatGuard(const QString &defaultRenderId);

} // namespace host
