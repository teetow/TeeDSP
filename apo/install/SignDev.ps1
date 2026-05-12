#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Generate a self-signed code-signing cert and sign TeeDspApo.dll for
    use with `bcdedit /set testsigning on`.

.DESCRIPTION
    Audiodg.exe runs as a Protected Process Light on modern Windows and
    will refuse to load unsigned APOs in many configurations. This script
    creates a personal cert, plants it in the Trusted Root and Trusted
    Publishers stores, and signs the built DLL. To use the resulting
    signed DLL, test signing mode must be enabled:

        bcdedit /set testsigning on
        # reboot

    To revert:
        bcdedit /set testsigning off
        # reboot

    The self-signed cert remains in your stores until you remove it from
    certmgr.msc.

.PARAMETER DllPath
    Path to TeeDspApo.dll. Defaults to first found in the build dirs.

.PARAMETER CertSubject
    Subject for the cert. Default: 'CN=TeeDSP Dev Self-Sign'
#>

[CmdletBinding()]
param(
    [string]$DllPath,
    [string]$CertSubject = 'CN=TeeDSP Dev Self-Sign'
)

$ErrorActionPreference = 'Stop'

if (-not $DllPath) {
    $here = Split-Path -Parent $MyInvocation.MyCommand.Path
    $candidates = @(
        Join-Path $here '..\..\out\build\vs2022-local\apo\Release\TeeDspApo.dll'
        Join-Path $here '..\..\out\build\vs2022\apo\Release\TeeDspApo.dll'
        Join-Path $here '..\..\build\apo\Release\TeeDspApo.dll'
        Join-Path $here '..\..\build\apo\Debug\TeeDspApo.dll'
    )
    $DllPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $DllPath) { throw "No TeeDspApo.dll found. Build first." }
}
$DllPath = (Resolve-Path $DllPath).Path

# 1. Get-or-create cert.
$cert = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $CertSubject } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1

if (-not $cert) {
    Write-Host "Creating self-signed cert: $CertSubject"
    $cert = New-SelfSignedCertificate `
        -Subject $CertSubject `
        -Type CodeSigningCert `
        -KeyExportPolicy Exportable `
        -KeyUsage DigitalSignature `
        -KeyLength 2048 `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -NotAfter (Get-Date).AddYears(5)
} else {
    Write-Host "Reusing existing cert: $($cert.Thumbprint)"
}

# 2. Plant cert into Trusted Root + Trusted Publishers (machine).
$bytes = $cert.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert)
$tmp   = [IO.Path]::GetTempFileName() + '.cer'
[IO.File]::WriteAllBytes($tmp, $bytes)
try {
    & certutil.exe -addstore -f Root              $tmp | Out-Null
    & certutil.exe -addstore -f TrustedPublisher  $tmp | Out-Null
} finally {
    Remove-Item $tmp -Force
}

# 3. Locate signtool.exe.
$signtool = Get-ChildItem -Path "${env:ProgramFiles(x86)}\Windows Kits\10\bin" `
    -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { throw "signtool.exe not found. Install Windows SDK." }

# 4. Sign.
Write-Host "Signing $DllPath"
& $signtool.FullName sign /fd SHA256 /sha1 $cert.Thumbprint `
    /t http://timestamp.digicert.com $DllPath
if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }

Write-Host ""
Write-Host "Signed. If test-signing mode isn't on yet:"
Write-Host "    bcdedit /set testsigning on"
Write-Host "    # reboot, then run Install.ps1"
