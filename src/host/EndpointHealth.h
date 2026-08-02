#pragma once

#include "WasapiDevices.h"

#include <QString>

namespace host {

// ---------------------------------------------------------------------------
// One health model for "is the user hearing what they should", replacing a
// growing pile of per-signature detectors.
//
// Four separate-looking Bluetooth failures turned out to be one defect: Windows
// MMDevices persists, per endpoint, state that for A2DP is a transient
// negotiation outcome rather than a device property -- and only revalidates it
// when the audio engine starts. A wired card really does have a stable mix
// format and FX chain, so caching them is correct; a Bluetooth link
// renegotiates rate, codec and capabilities on every connect. Windows writes
// the outcome to the registry as though durable, reloads it on reconnect, and
// has no invalidation path when the new negotiation disagrees.
//
// Everything downstream is that same defect in a different cached property:
// a stale MFX slot-6 binding, a stale device format (the 2026-08-02 wedge),
// and the chronic endpoint churn -- which is not a third bug but Windows'
// non-solution to the first two, minting a fresh endpoint when it cannot
// reconcile identity. Hence the prediction worth checking against the churn
// history: a mint inherits clean state and works, a reuse inherits stale state
// and is a coin flip.
//
// So this deliberately does NOT detect signatures. It asks the three questions
// that together cover what the user actually experiences, whichever cached
// property went stale:
//
//   1. Can a shared-mode stream open at all?   -> CannotOpen  (format wedge)
//   2. Is audio reaching the endpoint but not being processed?
//                                              -> NotFlowing  (stalled engine,
//                                                 e.g. audiodg relaunched
//                                                 protected and dropped the APO)
//   3. Is audio landing on a different endpoint than the default?
//                                              -> WrongDevice
//
// Repairs form one ordered ladder instead of unrelated mechanisms, and only the
// unambiguous rung is automatic:
//
//   rung 1  ResetDeviceFormat      -- automatic. CannotOpen only, where nothing
//                                     can play anyway so a rebuild costs nothing.
//   rung 2  re-assert the default  -- not automatic; see WrongDevice below.
//   rung 3  restart Audiosrv       -- needs elevation, so it is surfaced to the
//                                     UI as needsServiceRestart for the user to
//                                     confirm, never fired silently.
//
// WrongDevice is advisory only and never auto-repaired: "the default endpoint is
// silent while another is hot" is also what per-app device selection looks like,
// so acting on it would fight the user. Reporting a repair on a working system
// is worse than the bug -- the first cut of the format guard shipped exactly
// that mistake by treating a cached-format mismatch as proof of breakage when
// streams were still opening fine.
// ---------------------------------------------------------------------------

enum class EndpointVerdict {
    Unknown,      // nothing probed yet, or no default endpoint
    Idle,         // healthy, nothing playing -- not a fault
    Active,       // healthy, audio flowing and being processed
    Bypassed,     // healthy, TeeDSP deliberately out of the path
    NotOnOutput,  // healthy, APO simply not bound to this endpoint
    CannotOpen,   // no shared-mode stream can be created -- the format wedge
    NotFlowing,   // audio present at the endpoint but the APO is not processing
    WrongDevice,  // default endpoint silent while another active one is hot
};

// What the host already knows and should not be recomputed here.
struct HealthInputs {
    QString defaultRenderId;
    QString defaultRenderName;
    bool    apoBound = false;      // TeeDSP APO bound to the default endpoint
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
    bool  probed = false;          // a format probe ran on this tick

    long  formatHr = 0;            // IsFormatSupported (cheap probe)
    long  openHr = 0;              // Initialize, 0 if escalation never ran
    float peak = -1.0f;            // endpoint meter, independent of the APO
    StreamFormat mixFormat;

    bool  resetFormatTried = false;
    bool  resetFormatOk = false;
    bool  needsServiceRestart = false;  // rung 3, for the UI to offer
    QString hotOtherEndpoint;           // set when verdict == WrongDevice
};

// Call once per UI status tick (~400ms). Self-pacing: the format probe runs
// immediately when the endpoint changes, then every ~10s, and every tick while a
// wedge is suspected so recovery lands about a second after a bad reconnect.
// Cheap on the ticks it decides not to probe.
EndpointHealthResult tickEndpointHealth(const HealthInputs &in);

} // namespace host
