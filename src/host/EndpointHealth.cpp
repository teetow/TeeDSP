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
                    << QLatin1Char(' ') << text << QLatin1Char('\n');
}

// Full tuple, logged on every verdict transition so an incident report is one
// grep rather than a live investigation.
void logTransition(EndpointVerdict from, const HealthInputs &in,
                   const EndpointHealthResult &r)
{
    logLine(QStringLiteral("%1 -> %2  endpoint=\"%3\" %4  apoBound=%5 bypassed=%6 "
                           "processing=%7 mix=%8 peak=%9 IsFormatSupported=0x%10 "
                           "Initialize=0x%11%12")
                .arg(QLatin1String(verdictName(from)), QLatin1String(verdictName(r.verdict)),
                     in.defaultRenderName, in.defaultRenderId)
                .arg(in.apoBound).arg(in.bypassed).arg(in.apoProcessing)
                .arg(describe(r.mixFormat))
                .arg(r.peak, 0, 'f', 5)
                .arg(static_cast<quint32>(r.formatHr), 8, 16, QLatin1Char('0'))
                .arg(static_cast<quint32>(r.openHr), 8, 16, QLatin1Char('0'))
                .arg(r.hotOtherEndpoint.isEmpty()
                         ? QString()
                         : QStringLiteral(" hotOther=\"%1\"").arg(r.hotOtherEndpoint)));
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
    }

    // A recovery is in flight: the engine is expected to be inconsistent, so ask
    // no fault questions and repair nothing until it settles.
    if (in.suppressFaults) {
        s_consecutiveUnsupported = s_stallTicks = 0;
        r.verdict = classifyHealthy(in);
        if (r.verdict != s_lastVerdict) { logTransition(s_lastVerdict, in, r); s_lastVerdict = r.verdict; }
        return r;
    }

    // ---- question 1: can a shared-mode stream open? ------------------------
    const int cadence = (s_consecutiveUnsupported > 0) ? 1 : kProbeEveryTicks;
    if (endpointChanged || ++s_ticksSinceProbe >= cadence) {
        s_ticksSinceProbe = 0;
        r.probed = true;
        r.formatHr = WasapiDevices::sharedFormatHealth(in.defaultRenderId, &r.mixFormat);
        s_lastFormatHr = r.formatHr;
        s_lastMix = r.mixFormat;

        if (r.formatHr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
            // The cheap probe only proves the cached format disagrees, which also
            // happens while streams still open. Confirm with what an app actually
            // does before touching anything.
            r.openHr = WasapiDevices::sharedInitializeProbe(in.defaultRenderId);
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
            }
        } else {
            s_consecutiveUnsupported = 0;
        }
    } else {
        r.formatHr = s_lastFormatHr;
        r.mixFormat = s_lastMix;
    }

    if (s_consecutiveUnsupported >= kConfirmProbes) {
        r.verdict = EndpointVerdict::CannotOpen;
        if (s_repairAttempts < kMaxRepairAttempts) {
            ++s_repairAttempts;
            r.resetFormatTried = true;
            r.resetFormatOk = WasapiDevices::resetDeviceFormat(in.defaultRenderId);

            StreamFormat after;
            const long afterHr = WasapiDevices::sharedFormatHealth(in.defaultRenderId, &after);
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
                r.mixFormat = after;
                r.formatHr = afterHr;
                s_lastFormatHr = afterHr;
                s_lastMix = after;
            }
        }
        // Reset exhausted and still wedged: hand rung 3 to the user.
        if (s_consecutiveUnsupported >= kConfirmProbes
            && s_repairAttempts >= kMaxRepairAttempts)
            r.needsServiceRestart = true;

        if (r.verdict != s_lastVerdict) { logTransition(s_lastVerdict, in, r); s_lastVerdict = r.verdict; }
        return r;
    }

    // ---- question 2: audio present at the endpoint but not processed? ------
    if (in.apoBound && !in.bypassed && !in.apoProcessing) {
        r.peak = WasapiDevices::endpointPeak(in.defaultRenderId);
        if (r.peak > kSilenceFloor) {
            if (++s_stallTicks >= kStallTicks) {
                r.verdict = EndpointVerdict::NotFlowing;
                r.needsServiceRestart = true;
                if (r.verdict != s_lastVerdict) { logTransition(s_lastVerdict, in, r); s_lastVerdict = r.verdict; }
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
            if (r.verdict != s_lastVerdict) { logTransition(s_lastVerdict, in, r); s_lastVerdict = r.verdict; }
            return r;
        }
    }

    // ---- healthy ----------------------------------------------------------
    r.verdict = classifyHealthy(in);

    if (r.verdict != s_lastVerdict) { logTransition(s_lastVerdict, in, r); s_lastVerdict = r.verdict; }
    return r;
}

} // namespace host
