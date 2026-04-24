<#
.SYNOPSIS
    Removes the TeeDSP APO MFX registration from an audio endpoint.
#>
param(
    [string]$EndpointId,
    [string]$DllPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$logPath = Join-Path $PSScriptRoot 'uninstall_apo.last.log'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    try {
        $argList = @('-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
        if ($EndpointId) { $argList += @('-EndpointId', $EndpointId) }
        if ($DllPath) { $argList += @('-DllPath', $DllPath) }
        $proc = Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs -Wait -PassThru
        exit $proc.ExitCode
    } catch {
        Set-Content -Path $logPath -Value 'Administrator approval is required to remove system-wide DSP.' -Force
        Write-Error 'Administrator approval is required to remove system-wide DSP.'
    }
}

Start-Transcript -Path $logPath -Force | Out-Null

# Helper: safely read a named value from a registry key without tripping StrictMode.
# Returns $null if the key or value does not exist.
function Get-RegValueSafe {
    param([string]$Path, [string]$Name)
    if (-not (Test-Path $Path)) { return $null }
    $props = Get-ItemProperty -Path $Path -ErrorAction SilentlyContinue
    if ($null -eq $props) { return $null }
    # Avoid StrictMode property-not-found; inspect the PSObject's own properties.
    $member = $props.PSObject.Properties[$Name]
    if ($null -eq $member) { return $null }
    return $member.Value
}

# Whatever happens below, we MUST restart the audio services before exiting,
# otherwise the user's computer is left with no audio.
$script:uninstallFailed = $false
try {

function Resolve-ApoDllPath {
    param([string]$ProvidedPath)

    if ($ProvidedPath) {
        if (Test-Path $ProvidedPath) {
            return (Resolve-Path $ProvidedPath).Path
        }
        return $null
    }

    $repoRoot = Split-Path $PSScriptRoot -Parent
    $candidates = @(
        (Join-Path $repoRoot 'dist\TeeDspApo.dll'),
        (Join-Path $repoRoot 'out\build\vs2022\Debug\TeeDspApo.dll'),
        (Join-Path $repoRoot 'out\build\vs2022\Release\TeeDspApo.dll')
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    $globMatches = Get-ChildItem -Path (Join-Path $repoRoot 'out\build') -Filter 'TeeDspApo.dll' -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending
    if ($globMatches -and $globMatches.Count -gt 0) {
        return $globMatches[0].FullName
    }

    return $null
}

function Invoke-SystemCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CommandLine
    )

    $taskName = 'TeeDSP-' + [Guid]::NewGuid().ToString('N')
    $cmdPath = Join-Path $env:TEMP ($taskName + '.cmd')
    "@echo off`r`n$CommandLine`r`nexit /b %errorlevel%" | Set-Content -Path $cmdPath -Encoding Ascii -Force

    try {
        $action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"$cmdPath`""
        $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -RunLevel Highest
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -StartWhenAvailable
        Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings -Force | Out-Null
        Start-ScheduledTask -TaskName $taskName

        $deadline = (Get-Date).AddSeconds(30)
        do {
            Start-Sleep -Milliseconds 500
            $task = Get-ScheduledTask -TaskName $taskName
            $info = Get-ScheduledTaskInfo -TaskName $taskName
        } while ($task.State -eq 'Running' -and (Get-Date) -lt $deadline)

        if ($task.State -eq 'Running') {
            throw 'SYSTEM task timed out.'
        }
        if ($info.LastTaskResult -ne 0) {
            throw "SYSTEM task failed with result $($info.LastTaskResult)."
        }
    }
    finally {
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue | Out-Null
        Remove-Item -Path $cmdPath -Force -ErrorAction SilentlyContinue
    }
}

$devRoot  = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
$clsid    = '{B3A4C5D6-E7F8-4A1B-9C2D-3E4F5A6B7C8D}'
$targetValueNames = @(
    '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6'
    '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14'
)

Write-Host "Stopping audio services to release the protected FxProperties key..."
Stop-Service -Name 'Audiosrv' -Force -ErrorAction SilentlyContinue
Stop-Service -Name 'AudioEndpointBuilder' -Force -ErrorAction SilentlyContinue
Write-Host "Audio services stopped."

# Read saved endpoint if not passed
if (-not $EndpointId) {
    $configPath = Join-Path $PSScriptRoot 'installed_endpoint.txt'
    if (Test-Path $configPath) {
        $EndpointId = (Get-Content $configPath).Trim()
        Write-Host "Using saved endpoint: $EndpointId"
    } else {
        Write-Error "No endpoint specified and installed_endpoint.txt not found. Pass -EndpointId explicitly."
    }
}

$fxKey = "$devRoot\$EndpointId\FxProperties"

if (Test-Path $fxKey) {
    foreach ($slotValueName in $targetValueNames) {
        $current = Get-RegValueSafe -Path $fxKey -Name $slotValueName
        $existingValues = @()
        if ($null -ne $current) {
            if ($current -is [array]) {
                $existingValues = @($current | ForEach-Object { [string]$_ })
            } else {
                $existingValues = @([string]$current)
            }
        }

        if ($existingValues -contains $clsid) {
            $remainingValues = @($existingValues | Where-Object { $_ -ne $clsid })
            $regKeyPath = "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$EndpointId\FxProperties"
            if ($remainingValues.Count -gt 0) {
                $multiSzData = [string]::Join('\0', $remainingValues)
                $regArgs = @('add', $regKeyPath, '/v', $slotValueName, '/t', 'REG_MULTI_SZ', '/d', $multiSzData, '/f')
                $proc = Start-Process -FilePath 'reg.exe' -ArgumentList $regArgs -Wait -PassThru -NoNewWindow
                if ($proc.ExitCode -ne 0) {
                    Write-Warning "Direct registry write failed; retrying as SYSTEM..."
                    $regCommand = 'reg add "' + $regKeyPath + '" /v "' + $slotValueName + '" /t REG_MULTI_SZ /d "' + $multiSzData + '" /f'
                    Invoke-SystemCommand -CommandLine $regCommand
                }
                Write-Host ("Removed TeeDSP APO from FX slot " + $slotValueName + ".")
            } else {
                $proc = Start-Process -FilePath 'reg.exe' -ArgumentList @('delete', $regKeyPath, '/v', $slotValueName, '/f') -Wait -PassThru -NoNewWindow
                if ($proc.ExitCode -ne 0) {
                    Write-Warning "Direct registry delete failed; retrying as SYSTEM..."
                    $regCommand = 'reg delete "' + $regKeyPath + '" /v "' + $slotValueName + '" /f'
                    Invoke-SystemCommand -CommandLine $regCommand
                }
                Write-Host ("Removed the empty FX slot " + $slotValueName + ".")
            }
        }
    }
}

# Also remove the MFX and any stale SFX processing-mode keys we may have written.
$modeFmtid = '{d3993a3f-99c2-4402-b5ec-a92a0367664b}'
$regKeyPath = "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$EndpointId\FxProperties"
$poshKeyPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$EndpointId\FxProperties"
foreach ($modePid in @(5, 6)) {
    $propName = "$modeFmtid,$modePid"
    $existing = Get-RegValueSafe -Path $poshKeyPath -Name $propName
    if ($null -ne $existing) {
        $proc = Start-Process -FilePath 'reg.exe' -ArgumentList @('delete', $regKeyPath, '/v', $propName, '/f') -Wait -PassThru -NoNewWindow
        if ($proc.ExitCode -ne 0) {
            Invoke-SystemCommand -CommandLine ('reg delete "' + $regKeyPath + '" /v "' + $propName + '" /f')
        }
        Write-Host "Removed processing-mode key $propName"
    }
}
$dllPath = Resolve-ApoDllPath -ProvidedPath $DllPath
# Unregister COM server
if (Test-Path $dllPath) {
    Start-Process -FilePath 'regsvr32.exe' `
        -ArgumentList "/s /u `"$dllPath`"" `
        -Wait | Out-Null
    Write-Host "COM server unregistered."
}

Start-Service -Name 'AudioEndpointBuilder' -ErrorAction SilentlyContinue
Start-Service -Name 'Audiosrv'
Write-Host "Done. TeeDSP APO removed."
}
catch {
    Write-Error $_
    $script:uninstallFailed = $true
}
finally {
    # ALWAYS restart audio services — even if the uninstall failed — so the user
    # is never left with a silenced computer.
    try {
        Write-Host "Ensuring audio services are running..."
        Start-Service -Name 'AudioEndpointBuilder' -ErrorAction SilentlyContinue
        Start-Service -Name 'Audiosrv' -ErrorAction SilentlyContinue
    } catch {
        Write-Warning ("Failed to restart audio services: " + $_.Exception.Message)
    }
    Stop-Transcript | Out-Null
    if ($script:uninstallFailed) { exit 1 }
}
