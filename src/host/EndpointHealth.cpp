#include "EndpointHealth.h"

#include <windows.h>
#include <audioclient.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>

namespace host {

namespace {

// Format-probe cadence in ~400ms status ticks while healthy. The wedge can only
// appear when the link renegotiates, which always comes with an endpoint change
// that probes immediately, so this slow cadence is only a backstop.
constexpr int kProbeEveryTicks = 25;

// Consecutive failing probes before repairing. Immediately after an A2DP connect
// the endpoint can briefly report an inconsistent format chain, and
// ResetDeviceFormat is not something to fire on a transient. At the
// suspected-wedge cadence of one probe per tick this costs ~400ms.
constexpr int kConfirmProbes = 2;

// Give up after this many resets for one endpoint; the counter clears when the
// endpoint changes. If Reset is not clearing the wedge, retrying forever would
// churn the format cache and spam the log.
constexpr int kMaxRepairAttempts = 3;

// Preserved verbatim from the original dead-engine detection: audio must be
// present at the endpoint for ~2s with the APO bound and not processing before
// we accuse the engine. Rides out the gap at stream start and format changes.
constexpr int   kStallTicks = 5;
constexpr float kSilenceFloor = 0.0003f;

QString describe(const StreamFormat &f)
{
    return QStringLiteral("%1Hz %2ch %3bit%4")
        .arg(f.sampleRate).arg(f.channels).arg(f.bitsPerSample)
        .arg(f.isFloat ? QStringLiteral(" float") : QString());
}

const char *verdictName(EndpointVerdict v)
{
    switch (v) {
    case EndpointVerdict::Unknown:     return "Unknown";
    case EndpointVerdict::Idle:        return "Idle";
    case EndpointVerdict::Active:      return "Active";
    case EndpointVerdict::Bypassed:    return "Bypassed";
    case EndpointVerdict::NotOnOutput: return "NotOnOutput";
    case EndpointVerdict::CannotOpen:  return "CannotOpen";
    case EndpointVerdict::NotFlowing:  return "NotFlowing";
    case EndpointVerdict::WrongDevice: return "WrongDevice";
    }
    return "?";
}

// Sticky for the rest of the process once a forced fault has been injected, so
// every subsequent line is tagged. Marking is automatic rather than per-call:
// a deliberately induced fault must never be mistakable for a real incident in
// the audit trail, and that guarantee should not depend on remembering a prefix
// at each log site.
bool g_forcedRun = false;

// Always on, not env-gated: these events are rare, user-visible as silence, and
// the whole point is that the next incident is read off a log rather than
// reconstructed by registry archaeology after the state has already moved --
// which is what made the 2026-08-02 investigation expensive, since the wedge
// cleared halfway through it.
void logLine(const QString &text)
{
    QFile f(QDir::temp().filePath(QStringLiteral("teedsp_endpoint_health.log")));
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    QTextStream(&f) << QDateTime::currentDateTime().toString(Qt::ISODate)
                    << QLatin1Char(' ')
                    << (g_forcedRun ? QLatin1String("[FORCED TEST] ") : QLatin1String(""))
                    << text << QLatin1Char('\n');
}

bool isFault(EndpointVerdict v)
{
    return v == EndpointVerdict::CannotOpen
        || v == EndpointVerdict::NotFlowing
        || v == EndpointVerdict::WrongDevice;
}

// Full tuple, so an incident report is one grep rather than a live
// investigation. Carries the persisted format alongside the engine mix format:
// the two normally track, and a divergence is itself a fault signature.
void logState(const QString &event, const HealthInputs &in,
              const EndpointHealthResult &r)
{
    logLine(QStringLiteral("%1  endpoint=\"%2\" %3  apoBound=%4 bypassed=%5 "
                           "processing=%6 cached=%7Hz mix=%8 peak=%9 "
                           "IsFormatSupported=0x%10 Initialize=0x%11%12")
                .arg(event, in.defaultRenderName, in.defaultRenderId)
                .arg(in.apoBound).arg(in.bypassed).arg(in.apoProcessing)
                .arg(WasapiDevices::cachedDeviceFormatRate(in.defaultRenderId))
                .arg(describe(r.mixFormat))
                .arg(r.peak, 0, 'f', 5)
                .arg(static_cast<quint32>(r.formatHr), 8, 16, QLatin1Char('0'))
                .arg(static_cast<quint32>(r.openHr), 8, 16, QLatin1Char('0'))
                .arg(r.hotOtherEndpoint.isEmpty()
                         ? QString()
                         : QStringLiteral(" hotOther=\"%1\"").arg(r.hotOtherEndpoint)));
}

// What is worth a line, learned the hard way on 2026-08-02.
//
// Logging every verdict transition sounded thorough and was useless in both
// directions at once. It missed two AirPods connections entirely -- an endpoint
// change that does not alter the verdict produced no record, and with TeeDSP
// bypassed the verdict never moved -- while filling the file with routine
// Active/Idle churn every 45-90s as Chrome released its stream and Windows tore
// down the idle A2DP stream. Blind to the events we cared about, noisy with
// events we did not.
//
// So: log endpoint changes (rare, and the thing under study), anything entering
// or leaving a fault, and repairs. Never healthy-to-healthy transitions.
bool worthLogging(EndpointVerdict from, EndpointVerdict to, bool endpointChanged)
{
    if (endpointChanged) return true;
    return isFault(from) || isFault(to);
}

// Is some other active render endpoint carrying signal while the default is
// silent? Only consulted when the default is silent and the APO is bound, so the
// enumeration cost stays off the common path.
QString findHotOtherEndpoint(const QString &defaultId)
{
    for (const DeviceInfo &d : WasapiDevices::enumerateRender()) {
        if (!d.isActive || d.id == defaultId) continue;
        if (WasapiDevices::endpointPeak(d.id) > kSilenceFloor)
            return d.name.isEmpty() ? d.id : d.name;
    }
    return {};
}

EndpointVerdict classifyHealthy(const HealthInputs &in)
{
    return !in.apoBound     ? EndpointVerdict::NotOnOutput
         : in.bypassed      ? EndpointVerdict::Bypassed
         : in.apoProcessing ? EndpointVerdict::Active
                            : EndpointVerdict::Idle;
}

// Test hook. TEEDSP_FORCE_ENDPOINT_FAULT=<n> makes the next <n> format probes
// report the wedge, so the repair ladder can be exercised deliberately.
//
// It exists because the real fault cannot be manufactured: SetDeviceFormat
// validates against the live link and refuses an unsupported rate, and poking
// the registry directly self-corrects as soon as anything opens a stream. That
// left the repair path verifiable only by waiting for a real incident, which is
// how untested recovery code ends up shipping.
//
// Deliberately bounded rather than a boolean: each forced cycle performs a real
// ResetDeviceFormat, and an unbounded switch would reset the format every few
// seconds forever. The budget drains, then the endpoint recovers on its own, so
// the whole ladder plus the recovery transition runs in one pass.
bool consumeForcedFault()
{
    static int remaining = qEnvironmentVariableIntValue("TEEDSP_FORCE_ENDPOINT_FAULT");
    if (remaining <= 0) return false;
    --remaining;
    return true;
}

// Applies the test hook on top of a real probe. Also used for the post-repair
// re-check, so a forced run genuinely escalates instead of "recovering"
// immediately and hiding the upper rungs of the ladder. thisProbeForced reports
// whether *this* call was injected (the caller must then also fake the confirming
// open), while g_forcedRun latches for logging.
long probeFormat(const QString &id, StreamFormat *mix, bool *thisProbeForced)
{
    const long hr = WasapiDevices::sharedFormatHealth(id, mix);
    if (thisProbeForced) *thisProbeForced = false;
    if (consumeForcedFault()) {
        if (thisProbeForced) *thisProbeForced = true;
        g_forcedRun = true;
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    return hr;
}

} // namespace

EndpointHealthResult tickEndpointHealth(const HealthInputs &in)
{
    static QString s_lastId;
    static EndpointVerdict s_lastVerdict = EndpointVerdict::Unknown;
    static int  s_ticksSinceProbe = 0;
    static int  s_consecutiveUnsupported = 0;
    static int  s_repairAttempts = 0;
    static int  s_stallTicks = 0;
    static long s_lastFormatHr = S_OK;
    static StreamFormat s_lastMix;
    static bool s_loggedBenignMismatch = false;
    static bool s_loggedRung3 = false;
    // Whether the most recent probe was injected by the test hook, so the
    // confirming open is faked to match instead of contradicting it.
    bool thisProbeForced = false;

    EndpointHealthResult r;

    if (in.defaultRenderId.isEmpty()) {
        s_lastId.clear();
        s_consecutiveUnsupported = s_repairAttempts = s_stallTicks = 0;
        s_lastVerdict = EndpointVerdict::Unknown;
        return r;
    }

    const bool endpointChanged = (in.defaultRenderId != s_lastId);
    if (endpointChanged) {
        s_lastId = in.defaultRenderId;
        s_ticksSinceProbe = 0;
        s_consecutiveUnsupported = s_repairAttempts = s_stallTicks = 0;
        s_lastFormatHr = S_OK;
        s_loggedBenignMismatch = false;
        s_loggedRung3 = false;
    }

    // A recovery is in flight: the engine is expected to be inconsistent, so ask
    // no fault questions and repair nothing until it settles.
    if (in.suppressFaults) {
        s_consecutiveUnsupported = s_stallTicks = 0;
        r.verdict = classifyHealthy(in);
        if (endpointChanged)
            logState(QStringLiteral("endpoint (recovery in flight)"), in, r);
        s_lastVerdict = r.verdict;
        return r;
    }

    // ---- question 1: can a shared-mode stream open? ------------------------
    const int cadence = (s_consecutiveUnsupported > 0) ? 1 : kProbeEveryTicks;
    if (endpointChanged || ++s_ticksSinceProbe >= cadence) {
        s_ticksSinceProbe = 0;
        r.probed = true;
        r.formatHr = probeFormat(in.defaultRenderId, &r.mixFormat, &thisProbeForced);
        s_lastFormatHr = r.formatHr;
        s_lastMix = r.mixFormat;

        if (r.formatHr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
            // The cheap probe only proves the cached format disagrees, which also
            // happens while streams still open. Confirm with what an app actually
            // does before touching anything.
            r.openHr = thisProbeForced ? AUDCLNT_E_UNSUPPORTED_FORMAT
                                       : WasapiDevices::sharedInitializeProbe(in.defaultRenderId);
            if (r.openHr != S_OK) {
                ++s_consecutiveUnsupported;
            } else {
                s_consecutiveUnsupported = 0;
                // Worth a line even though it is not a fault and produces no
                // verdict transition: the cache disagreeing while streams still
                // open is the precursor to the wedge, and silence here would hide
                // the most informative state in the whole sequence.
                if (!s_loggedBenignMismatch) {
                    s_loggedBenignMismatch = true;
                    logLine(QStringLiteral("note: %1 cached format disagrees "
                                           "(IsFormatSupported=0x%2) but a shared stream "
                                           "still opens — not a fault, not repairing")
                                .arg(in.defaultRenderName)
                                .arg(static_cast<quint32>(r.formatHr), 8, 16, QLatin1Char('0')));
                }
                // Streams open, so the endpoint is usable again: clear the repair
                // budget too, not just the confirmation counter.
                s_repairAttempts = 0;
                s_loggedRung3 = false;
            }
        } else {
            // Healthy probe. Clearing the repair budget here matters: without it a
            // fault that resolves on its own (a reconnect landing on a matching
            // rate, say) would leave the budget exhausted, and the next genuine
            // wedge in the same session would get no repair at all.
            s_consecutiveUnsupported = 0;
            s_repairAttempts = 0;
            s_loggedRung3 = false;
        }
    } else {
        r.formatHr = s_lastFormatHr;
        r.mixFormat = s_lastMix;
    }

    if (s_consecutiveUnsupported >= kConfirmProbes) {
        r.verdict = EndpointVerdict::CannotOpen;
        // Logged before the repair runs, so the log reads chronologically: fault
        // first, then what was done about it.
        if (worthLogging(s_lastVerdict, r.verdict, endpointChanged)
            && r.verdict != s_lastVerdict) {
            logState(QStringLiteral("FAULT CannotOpen"), in, r);
        }
        s_lastVerdict = r.verdict;
        if (s_repairAttempts < kMaxRepairAttempts) {
            ++s_repairAttempts;
            r.resetFormatTried = true;
            r.resetFormatOk = WasapiDevices::resetDeviceFormat(in.defaultRenderId);

            StreamFormat after;
            const long afterHr = probeFormat(in.defaultRenderId, &after, nullptr);
            logLine(QStringLiteral("rung1 ResetDeviceFormat %1 (attempt %2/%3) -> %4  "
                                   "mix now %5, IsFormatSupported=0x%6")
                        .arg(r.resetFormatOk ? QStringLiteral("ok") : QStringLiteral("FAILED"))
                        .arg(s_repairAttempts).arg(kMaxRepairAttempts)
                        .arg(afterHr == S_OK ? QStringLiteral("REPAIRED")
                                             : QStringLiteral("still wedged"),
                             describe(after))
                        .arg(static_cast<quint32>(afterHr), 8, 16, QLatin1Char('0')));
            if (afterHr == S_OK) {
                s_consecutiveUnsupported = 0;
                s_repairAttempts = 0;
                s_loggedRung3 = false;
                r.mixFormat = after;
                r.formatHr = afterHr;
                s_lastFormatHr = afterHr;
                s_lastMix = after;
            }
        }
        // Reset exhausted and still wedged: hand rung 3 to the user.
        if (s_consecutiveUnsupported >= kConfirmProbes
            && s_repairAttempts >= kMaxRepairAttempts) {
            if (!s_loggedRung3) {
                s_loggedRung3 = true;
                logLine(QStringLiteral("rung3: %1 resets did not clear it — offering the "
                                       "audio engine restart to the user")
                            .arg(kMaxRepairAttempts));
            }
            r.needsServiceRestart = true;
        }
        return r;
    }

    // ---- question 2: audio present at the endpoint but not processed? ------
    if (in.apoBound && !in.bypassed && !in.apoProcessing) {
        r.peak = WasapiDevices::endpointPeak(in.defaultRenderId);
        if (r.peak > kSilenceFloor) {
            if (++s_stallTicks >= kStallTicks) {
                r.verdict = EndpointVerdict::NotFlowing;
                r.needsServiceRestart = true;
                if (r.verdict != s_lastVerdict)
                    logState(QStringLiteral("FAULT NotFlowing"), in, r);
                s_lastVerdict = r.verdict;
                return r;
            }
        } else {
            s_stallTicks = 0;
        }
    } else {
        s_stallTicks = 0;
    }

    // ---- question 3: is audio landing somewhere else? ----------------------
    // Only when our endpoint is silent and TeeDSP should be shaping it. Advisory
    // only: this is indistinguishable from legitimate per-app device selection,
    // so it is never auto-repaired.
    //
    // Gated on r.probed, i.e. the ~10s cadence rather than every tick: this is
    // the one check that enumerates endpoints and reads a meter per candidate,
    // and "idle with the APO bound" is the common resting state, so running it
    // per tick would burn COM round trips continuously for a hint.
    if (r.probed && in.apoBound && !in.bypassed && !in.apoProcessing
        && r.peak >= 0.0f && r.peak <= kSilenceFloor) {
        r.hotOtherEndpoint = findHotOtherEndpoint(in.defaultRenderId);
        if (!r.hotOtherEndpoint.isEmpty()) {
            r.verdict = EndpointVerdict::WrongDevice;
            if (r.verdict != s_lastVerdict)
                logState(QStringLiteral("FAULT WrongDevice"), in, r);
            s_lastVerdict = r.verdict;
            return r;
        }
    }

    // ---- healthy ----------------------------------------------------------
    r.verdict = classifyHealthy(in);

    // Endpoint changes and fault recoveries get a line; routine Active/Idle
    // churn does not. See worthLogging().
    if (endpointChanged) {
        logState(QStringLiteral("endpoint"), in, r);
    } else if (isFault(s_lastVerdict)) {
        logState(QStringLiteral("recovered"), in, r);
    }
    s_lastVerdict = r.verdict;
    return r;
}

} // namespace host
