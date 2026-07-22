#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Remove one or more installed teedsp*.inf driver packages and clear any
    endpoint FX bindings pointing at them.

.DESCRIPTION
    Companion to deploy-apo.ps1, for the reverse direction. Driven from
    ApoManagerDialog's "Uninstall Selected" / "Remove APO Entirely" buttons
    (see src/ui/ApoLifecycleActions.cpp), but usable standalone too.

    Clears the TeeDSP FX CLSID (all three known slots: SFX/AirPods, MFX,
    composite MFX/Realtek) on every render endpoint unconditionally, rather
    than trying to determine which endpoint belongs to which package —
    clearing an unset value is a no-op, so this is safe, and it means removal
    never leaves a dangling FX CLSID behind regardless of which package(s)
    are being removed.

.PARAMETER PublishedNames
    Comma-separated pnputil-published package names to remove, e.g.
    'oem123.inf' or 'oem123.inf,oem124.inf'. From
    host::queryInstalledApoPackages() / pnputil /enum-drivers "Published
    Name:" field — NOT the original teedsp*.inf filename. A single comma-
    joined string (not a native [string[]]) so the caller doesn't have to
    rely on how powershell.exe -File splits array arguments passed on a
    command line.

.PARAMETER Log
    Optional path to append progress to, in addition to stdout. The caller
    (ApoLifecycleActions::removeApoPackages) reads this back after the
    elevated process exits, since a UAC-elevated process's stdout isn't
    otherwise visible to the launching app.

.PARAMETER SkipFxClear
    Skip the FX-binding-clear step and only remove the package(s). For
    retiring a superseded/duplicate Driver Store package that isn't the one
    actually bound to anything -- e.g. a leftover from an interrupted
    deploy-apo.ps1 run. Removing a package nothing points at doesn't need
    (and shouldn't risk) touching a live, working binding on some other
    endpoint.

.EXAMPLE
    .\uninstall-apo.ps1 -PublishedNames oem123.inf
    .\uninstall-apo.ps1 -PublishedNames oem123.inf,oem124.inf,oem125.inf -Log C:\temp\uninstall.log
    .\uninstall-apo.ps1 -PublishedNames oem120.inf -SkipFxClear   # retire a stale duplicate only
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PublishedNames,
    [string]$Log,
    [switch]$SkipFxClear
)

$ErrorActionPreference = 'Stop'
$names = $PublishedNames -split ',' | Where-Object { $_ }
if (-not $names) { throw "No package names given in -PublishedNames." }

function Out-Line([string]$msg) {
    if ($Log) { $msg | Out-File -FilePath $Log -Append -Encoding utf8 }
    Write-Host $msg
}

. "$PSScriptRoot\..\apo\install\_Interop.ps1"

if ($SkipFxClear) {
    Out-Line "== Skipping FX-binding clear (-SkipFxClear) =="
} else {
    Out-Line "== Clearing TeeDSP FX bindings on all render endpoints =="
    foreach ($pair in [TeeDsp.Apo.Helpers]::ListRender()) {
        $epId = $pair[0]
        $epName = $pair[1]
        foreach ($pid in 5, 6, 14) {
            try {
                [TeeDsp.Apo.Helpers]::ClearClsidProp($epId, $pid)
            } catch {
                Out-Line "  warn: clearing pid $pid on '$epName': $($_.Exception.Message)"
            }
        }
        Out-Line "  cleared: $epName"
    }
}

Out-Line "== Removing driver package(s) =="
$failed = @()
foreach ($name in $names) {
    Out-Line "Removing $name"
    $output = pnputil /delete-driver $name /uninstall /force 2>&1
    $output | ForEach-Object { Out-Line "  $_" }
    if ($LASTEXITCODE -ne 0) { $failed += $name }
}

Out-Line "== Restarting Windows Audio =="
Restart-Service Audiosrv -Force

if ($failed.Count -gt 0) {
    Out-Line "Done, with failures: $($failed -join ', ')"
    exit 1
}
Out-Line "Done."
