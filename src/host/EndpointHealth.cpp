#include "EndpointHealth.h"

#include <windows.h>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>

namespace host {

namespace {

// Preserved verbatim from the original dead-engine detection: audio must be
// present at the endpoint for ~2s with the APO bound and not processing before
// we accuse the engine. Rides out the gap at stream start and format changes.
constexpr int   kStallTicks = 5;
constexpr float kSilenceFloor = 0.0003f;

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
                    << text << QLatin1Char('\n');
}

bool isFault(EndpointVerdict v)
{
    return v == EndpointVerdict::EffectsDisabled
        || v == EndpointVerdict::NotFlowing;
}

void logState(const QString &event, const HealthInputs &in,
              const EndpointHealthResult &r)
{
    logLine(QStringLiteral("%1  endpoint=\"%2\" %3  apoBound=%4 effectsEnabled=%5 "
                           "bypassed=%6 processing=%7 peak=%8")
                .arg(event, in.defaultRenderName, in.defaultRenderId)
                .arg(in.apoBound).arg(in.effectsEnabled).arg(in.bypassed).arg(in.apoProcessing)
                .arg(r.peak, 0, 'f', 5));
}

EndpointVerdict classifyHealthy(const HealthInputs &in)
{
    return !in.apoBound     ? EndpointVerdict::NotOnOutput
         : !in.effectsEnabled ? EndpointVerdict::EffectsDisabled
         : in.bypassed      ? EndpointVerdict::Bypassed
         : in.apoProcessing ? EndpointVerdict::Active
                            : EndpointVerdict::Idle;
}

} // namespace

EndpointHealthResult tickEndpointHealth(const HealthInputs &in)
{
    static QString s_lastId;
    static EndpointVerdict s_lastVerdict = EndpointVerdict::Unknown;
    static int  s_stallTicks = 0;

    EndpointHealthResult r;

    if (in.defaultRenderId.isEmpty()) {
        s_lastId.clear();
        s_stallTicks = 0;
        s_lastVerdict = EndpointVerdict::Unknown;
        return r;
    }

    const bool endpointChanged = (in.defaultRenderId != s_lastId);
    if (endpointChanged) {
        s_lastId = in.defaultRenderId;
        s_stallTicks = 0;
    }

    // A recovery is in flight: the engine is expected to be inconsistent, so ask
    // no fault questions and repair nothing until it settles.
    if (in.suppressFaults) {
        s_stallTicks = 0;
        HealthInputs recovering = in;
        recovering.effectsEnabled = true;
        r.verdict = classifyHealthy(recovering);
        if (endpointChanged)
            logState(QStringLiteral("endpoint (recovery in flight)"), in, r);
        s_lastVerdict = r.verdict;
        return r;
    }

    if (in.apoBound && !in.effectsEnabled) {
        s_stallTicks = 0;
        r.verdict = EndpointVerdict::EffectsDisabled;
        r.needsEffectsEnable = true;
        if (endpointChanged || r.verdict != s_lastVerdict)
            logState(QStringLiteral("FAULT EffectsDisabled"), in, r);
        s_lastVerdict = r.verdict;
        return r;
    }

    // Is audio present at the endpoint but not processed by TeeDSP?
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

    // ---- healthy ----------------------------------------------------------
    r.verdict = classifyHealthy(in);

    // Endpoint changes and fault recoveries get a line; routine Active/Idle
    // churn does not.
    if (endpointChanged) {
        logState(QStringLiteral("endpoint"), in, r);
    } else if (isFault(s_lastVerdict)) {
        logState(QStringLiteral("recovered"), in, r);
    }
    s_lastVerdict = r.verdict;
    return r;
}

} // namespace host
