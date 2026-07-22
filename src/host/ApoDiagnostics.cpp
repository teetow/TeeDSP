#include "ApoDiagnostics.h"

#include "ApoBindingStatus.h"
#include "ApoSharedClient.h"
#include "WasapiDevices.h"

#include <QDateTime>
#include <QProcess>
#include <QSettings>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace host {

namespace {

void line(QString &out, const QString &label, const QString &value)
{
    out += label.leftJustified(28) + value + QStringLiteral("\n");
}

void header(QString &out, const QString &title)
{
    if (!out.isEmpty())
        out += QStringLiteral("\n");
    out += QStringLiteral("=== %1 ===\n").arg(title);
}

QString bufferFlagsLabel(unsigned long long flags)
{
    switch (flags) {
    case 0: return QStringLiteral("0 (BUFFER_INVALID)");
    case 1: return QStringLiteral("1 (BUFFER_SILENT)");
    case 2: return QStringLiteral("2 (BUFFER_VALID)");
    default: return QString::number(flags);
    }
}

// Finds audiodg.exe's PID (there's normally exactly one). 0 if not running.
DWORD findAudiodgPid()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"audiodg.exe") == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

// "loaded", "not loaded", or "unknown (access denied)" -- audiodg normally
// runs as a protected process, so failing to open it is the expected case
// unless DisableProtectedAudioDG is set, not itself a sign of anything wrong.
QString isTeeDspApoLoadedIn(DWORD pid)
{
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc)
        return QStringLiteral("unknown (access denied opening audiodg -- "
                              "normal unless DisableProtectedAudioDG is set)");

    HMODULE modules[1024];
    DWORD needed = 0;
    bool found = false;
    if (EnumProcessModules(proc, modules, sizeof(modules), &needed)) {
        const DWORD count = needed / sizeof(HMODULE);
        wchar_t path[MAX_PATH];
        for (DWORD i = 0; i < count && !found; ++i) {
            if (GetModuleFileNameExW(proc, modules[i], path, MAX_PATH)) {
                const QString modPath = QString::fromWCharArray(path);
                found = modPath.endsWith(QStringLiteral("TeeDspApo.dll"), Qt::CaseInsensitive);
            }
        }
    } else {
        CloseHandle(proc);
        return QStringLiteral("unknown (EnumProcessModules failed)");
    }
    CloseHandle(proc);
    return found ? QStringLiteral("yes") : QStringLiteral("no");
}

QString serviceStateLabel(DWORD state)
{
    switch (state) {
    case SERVICE_STOPPED:          return QStringLiteral("STOPPED");
    case SERVICE_START_PENDING:    return QStringLiteral("START_PENDING");
    case SERVICE_STOP_PENDING:     return QStringLiteral("STOP_PENDING");
    case SERVICE_RUNNING:          return QStringLiteral("RUNNING");
    case SERVICE_CONTINUE_PENDING: return QStringLiteral("CONTINUE_PENDING");
    case SERVICE_PAUSE_PENDING:    return QStringLiteral("PAUSE_PENDING");
    case SERVICE_PAUSED:           return QStringLiteral("PAUSED");
    default:                       return QStringLiteral("unknown (%1)").arg(state);
    }
}

} // namespace

QString buildDiagnosticsReport()
{
    QString out;
    out += QStringLiteral("TeeDSP diagnostics -- %1\n")
               .arg(QDateTime::currentDateTime().toString(Qt::ISODate));

    header(out, QStringLiteral("Live telemetry (Global\\TeeDspApoSharedV8)"));
    {
        host::ApoSharedClient client;
        host::ApoSharedClient::ApoStatus s;
        if (client.tryOpen() && client.readStatus(s)) {
            line(out, QStringLiteral("Section open:"), QStringLiteral("yes"));
            line(out, QStringLiteral("Active streams:"), s.locked ? QStringLiteral("yes") : QStringLiteral("no"));
            line(out, QStringLiteral("Channels / sample rate:"),
                 QStringLiteral("%1 / %2 Hz").arg(s.channels).arg(s.sampleRate));
            line(out, QStringLiteral("Bytes per frame:"), QString::number(s.bytesPerFrame));
            line(out, QStringLiteral("UI heartbeat alive:"), s.uiAlive ? QStringLiteral("yes") : QStringLiteral("no"));
            line(out, QStringLiteral("UI heartbeat (raw):"), QString::number(s.uiHeartbeat));
            line(out, QStringLiteral("Process calls (cum.):"), QString::number(s.processCalls));
            line(out, QStringLiteral("Frames processed (cum.):"), QString::number(s.framesProcessed));
            line(out, QStringLiteral("Last buffer flags:"), bufferFlagsLabel(s.lastBufferFlags));
            line(out, QStringLiteral("Param generation:"),
                 QStringLiteral("%1 (applied: %2)").arg(s.paramGen).arg(s.appliedGen));
            line(out, QStringLiteral("Meter-owner instance id:"), QString::number(s.meterOwner));
            line(out, QStringLiteral("DSP build stamp:"),
                 s.dspBuildStamp[0] ? QString::fromLatin1(s.dspBuildStamp) : QStringLiteral("(empty)"));
        } else {
            line(out, QStringLiteral("Section open:"),
                 QStringLiteral("no (no audio has hit the APO since boot)"));
        }
    }

    header(out, QStringLiteral("FX bindings (raw registry values)"));
    {
        const auto endpoints = host::WasapiDevices::enumerateRender();
        for (const auto &ep : endpoints) {
            out += QStringLiteral("%1  (%2)\n").arg(ep.name, ep.id);
            const auto raw = host::queryApoBindingRaw(ep.id);
            line(out, QStringLiteral("  ,5  SFX:"), raw.sfxClsid.isEmpty() ? QStringLiteral("(not set)") : raw.sfxClsid);
            line(out, QStringLiteral("  ,6  MFX:"), raw.mfxClsid.isEmpty() ? QStringLiteral("(not set)") : raw.mfxClsid);
            line(out, QStringLiteral("  ,14 Composite MFX:"),
                 raw.compositeMfxClsid.isEmpty() ? QStringLiteral("(not set)") : raw.compositeMfxClsid);
            line(out, QStringLiteral("  Modes supported:"),
                 raw.modesSupported.isEmpty() ? QStringLiteral("(not set)") : raw.modesSupported.join(QStringLiteral(", ")));
        }
    }

    header(out, QStringLiteral("Driver Store packages"));
    {
        const auto packages = host::queryInstalledApoPackages();
        if (packages.isEmpty()) {
            out += QStringLiteral("(none published)\n");
        }
        for (const auto &pkg : packages) {
            out += QStringLiteral("%1  [%2]  %3  %4\n")
                       .arg(pkg.label, pkg.publishedName, pkg.driverVersion, pkg.originalInfName);
        }
    }

    header(out, QStringLiteral("Audiosrv service"));
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scm) {
            SC_HANDLE svc = OpenServiceW(scm, L"Audiosrv", SERVICE_QUERY_STATUS);
            if (svc) {
                SERVICE_STATUS_PROCESS status{};
                DWORD needed = 0;
                if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                         reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed)) {
                    line(out, QStringLiteral("Status:"), serviceStateLabel(status.dwCurrentState));
                    line(out, QStringLiteral("Host process PID:"), QString::number(status.dwProcessId));
                } else {
                    line(out, QStringLiteral("Status:"), QStringLiteral("unknown (QueryServiceStatusEx failed)"));
                }
                CloseServiceHandle(svc);
            } else {
                line(out, QStringLiteral("Status:"), QStringLiteral("unknown (OpenService failed)"));
            }
            CloseServiceHandle(scm);
        } else {
            line(out, QStringLiteral("Status:"), QStringLiteral("unknown (OpenSCManager failed)"));
        }
    }

    header(out, QStringLiteral("audiodg.exe"));
    {
        const DWORD pid = findAudiodgPid();
        if (pid) {
            line(out, QStringLiteral("PID:"), QString::number(pid));
            line(out, QStringLiteral("TeeDspApo.dll loaded:"), isTeeDspApoLoadedIn(pid));
        } else {
            line(out, QStringLiteral("PID:"), QStringLiteral("not running"));
        }
    }

    header(out, QStringLiteral("DisableProtectedAudioDG"));
    {
        QSettings audio(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio"),
                        QSettings::NativeFormat);
        const QVariant v = audio.value(QStringLiteral("DisableProtectedAudioDG"));
        line(out, QStringLiteral("Set:"), v.isValid() ? QStringLiteral("yes (%1)").arg(v.toString())
                                                       : QStringLiteral("no"));
    }

    return out;
}

QString runAirPodsChurnCheck(const QString &scriptPath)
{
    if (scriptPath.isEmpty())
        return QStringLiteral("Get-AirPodsEndpointHistory.ps1 not found.\n");

    QProcess proc;
    proc.start(QStringLiteral("powershell.exe"),
              {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
               QStringLiteral("-File"), scriptPath});
    if (!proc.waitForFinished(15000))
        return QStringLiteral("Get-AirPodsEndpointHistory.ps1 timed out after 15s.\n");
    return QString::fromLocal8Bit(proc.readAllStandardOutput())
         + QString::fromLocal8Bit(proc.readAllStandardError());
}

} // namespace host
