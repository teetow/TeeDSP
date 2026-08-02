#include "EndpointFormatGuard.h"

#include <windows.h>
#include <audioclient.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>

namespace host {

namespace {

// Healthy re-probe cadence, in ~400ms status ticks. Matches the existing
// default-device recheck interval in MainWindow (~10s): the wedge can only
// appear when the link renegotiates, which always comes with an endpoint state
// change, so a slow background cadence is only a backstop.
constexpr int kProbeEveryTicks = 25;

// Consecutive wedged probes required before repairing. A single probe is not
// enough: immediately after an A2DP connect the endpoint can briefly report an
// inconsistent format chain, and ResetDeviceFormat is not something to fire on a
// transient. At the suspected-wedge cadence of one probe per tick this costs
// about 400ms of extra latency.
constexpr int kConfirmProbes = 2;

// Give up after this many repairs for one endpoint. If Reset is not clearing the
// wedge, retrying forever would churn the format cache and spam the log; the
// counter clears when the endpoint changes.
constexpr int kMaxRepairAttempts = 3;

QString describe(const StreamFormat &f)
{
    return QStringLiteral("%1Hz %2ch %3bit%4")
        .arg(f.sampleRate).arg(f.channels).arg(f.bitsPerSample)
        .arg(f.isFloat ? QStringLiteral(" float") : QString());
}

// Always logged, not gated behind an env var: a wedge is rare, user-visible as
// total silence, and the whole point of this guard is that the next occurrence
// leaves evidence instead of requiring another live investigation.
void logLine(const QString &text)
{
    QFile f(QDir::temp().filePath(QStringLiteral("teedsp_format_guard.log")));
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    QTextStream(&f) << QDateTime::currentDateTime().toString(Qt::ISODate)
                    << QLatin1Char(' ') << text << QLatin1Char('\n');
}

} // namespace

FormatGuardResult tickFormatGuard(const QString &defaultRenderId)
{
    static QString s_lastId;
    static int s_ticksSinceProbe = 0;
    static int s_consecutiveWedged = 0;
    static int s_repairAttempts = 0;
    static bool s_loggedBenignMismatch = false;

    FormatGuardResult result;

    if (defaultRenderId.isEmpty()) {
        s_lastId.clear();
        s_consecutiveWedged = 0;
        s_repairAttempts = 0;
        return result;
    }

    const bool endpointChanged = (defaultRenderId != s_lastId);
    if (endpointChanged) {
        s_lastId = defaultRenderId;
        s_ticksSinceProbe = 0;
        s_consecutiveWedged = 0;
        s_repairAttempts = 0;
        s_loggedBenignMismatch = false;
    } else {
        // Probe every tick once a wedge is suspected, so confirmation and repair
        // complete in ~1s instead of waiting out two full 10s intervals.
        const int cadence = (s_consecutiveWedged > 0) ? 1 : kProbeEveryTicks;
        if (++s_ticksSinceProbe < cadence) return result;
        s_ticksSinceProbe = 0;
    }

    result.probed = true;
    result.healthHr = WasapiDevices::sharedFormatHealth(defaultRenderId, &result.mixFormat);

    if (result.healthHr == S_OK) {
        if (s_consecutiveWedged > 0)
            logLine(QStringLiteral("recovered: %1 healthy at %2")
                        .arg(defaultRenderId, describe(result.mixFormat)));
        s_consecutiveWedged = 0;
        s_repairAttempts = 0;
        return result;
    }

    // Only AUDCLNT_E_UNSUPPORTED_FORMAT is the wedge. Anything else -- the
    // device vanishing mid-probe, an activation failure during connect churn --
    // is not something ResetDeviceFormat can fix, so leave it alone.
    if (result.healthHr != AUDCLNT_E_UNSUPPORTED_FORMAT) {
        s_consecutiveWedged = 0;
        return result;
    }

    // The cheap probe says the cached format disagrees -- but that alone is not
    // the fault the user experiences, and repairing on it glitches audio that
    // was working. Observed 2026-08-02: with a poisoned cache,
    // IsFormatSupported returned 0x88890008 while Initialize still returned
    // S_OK and playback continued normally. Only a shared-mode client that
    // genuinely cannot be created is the wedge, so confirm before touching
    // anything. This escalation runs only on the already-failing path, keeping
    // the routine tick free of stream creation.
    result.openHr = WasapiDevices::sharedInitializeProbe(defaultRenderId);
    if (result.openHr == S_OK) {
        if (!s_loggedBenignMismatch) {
            s_loggedBenignMismatch = true;
            logLine(QStringLiteral("benign mismatch: %1 mix=%2 "
                                   "IsFormatSupported=0x%3 but a shared stream still "
                                   "opens -- not repairing")
                        .arg(defaultRenderId, describe(result.mixFormat))
                        .arg(static_cast<quint32>(result.healthHr), 8, 16, QLatin1Char('0')));
        }
        s_consecutiveWedged = 0;
        return result;
    }

    result.wedged = true;
    if (++s_consecutiveWedged < kConfirmProbes) return result;
    if (s_repairAttempts >= kMaxRepairAttempts) return result;

    ++s_repairAttempts;
    result.repairTried = true;
    logLine(QStringLiteral("wedged: %1 mix=%2 IsFormatSupported=0x%3 Initialize=0x%4 -- "
                           "no shared stream can open; resetting (attempt %5/%6)")
                .arg(defaultRenderId, describe(result.mixFormat))
                .arg(static_cast<quint32>(result.healthHr), 8, 16, QLatin1Char('0'))
                .arg(static_cast<quint32>(result.openHr), 8, 16, QLatin1Char('0'))
                .arg(s_repairAttempts).arg(kMaxRepairAttempts));

    const bool resetOk = WasapiDevices::resetDeviceFormat(defaultRenderId);

    StreamFormat after;
    const long afterHr = WasapiDevices::sharedFormatHealth(defaultRenderId, &after);
    result.repaired = (afterHr == S_OK);
    if (result.repaired) {
        s_consecutiveWedged = 0;
        s_repairAttempts = 0;
        result.mixFormat = after;
    }

    logLine(QStringLiteral("reset %1 -> %2 (mix now %3, IsFormatSupported=0x%4)")
                .arg(resetOk ? QStringLiteral("ok") : QStringLiteral("FAILED"),
                     result.repaired ? QStringLiteral("REPAIRED") : QStringLiteral("still wedged"),
                     describe(after))
                .arg(static_cast<quint32>(afterHr), 8, 16, QLatin1Char('0')));

    return result;
}

} // namespace host
