<##
.SYNOPSIS
    Stages a componentized APO driver package and optionally signs its catalog.

.DESCRIPTION
    The package contains the APO software component and the controlled Realtek
    extension. Inf2Cat validates both INFs and produces one catalog covering
    their contents. The package is not installed by this script.

    A test certificate must be trusted and Windows test-signing must be enabled
    before a test-signed package can be installed. Production use requires a
    properly signed driver package.
##>

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$DllPath,

    [Parameter(Mandatory)]
    [string]$OutputDir,

    [string]$CertThumbprint,

    [switch]$SkipCatalog
)

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dll = (Resolve-Path -LiteralPath $DllPath).Path
$output = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $output) {
    throw "Output directory already exists: $output. Choose an empty new directory."
}

New-Item -ItemType Directory -Path $output | Out-Null
Copy-Item -LiteralPath $dll -Destination (Join-Path $output 'teedspapo.dll')
Copy-Item -LiteralPath (Join-Path $here 'TeeDspApoComponent.inf') -Destination $output
Copy-Item -LiteralPath (Join-Path $here 'TeeDspRealtekExtension.inf') -Destination $output

$signtool = $null
if ($CertThumbprint) {
    $kitsBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $signtool = Get-ChildItem -Path $kitsBin -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\' } |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $signtool) { throw 'signtool.exe not found. Install the Windows SDK.' }

    # The catalog hashes package contents, so the PE file must be signed
    # before Inf2Cat generates the catalog.
    $packageDll = Join-Path $output 'teedspapo.dll'
    & $signtool.FullName sign /fd SHA256 /sha1 $CertThumbprint /tr http://timestamp.digicert.com /td SHA256 $packageDll
    if ($LASTEXITCODE -ne 0) { throw "DLL signing failed ($LASTEXITCODE)." }
}

if (-not $SkipCatalog) {
    $kitsBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $inf2cat = Get-ChildItem -Path $kitsBin -Filter Inf2Cat.exe -Recurse -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $inf2cat) { throw 'Inf2Cat.exe not found. Install the Windows Driver Kit.' }

    & $inf2cat.FullName "/driver:$output" '/os:10_X64'
    if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed ($LASTEXITCODE)." }
}

if ($CertThumbprint) {
    $catalog = Join-Path $output 'TeeDspApo.cat'
    if (-not (Test-Path -LiteralPath $catalog)) {
        throw 'No catalog was generated. Omit -SkipCatalog before signing.'
    }

    & $signtool.FullName sign /fd SHA256 /sha1 $CertThumbprint /tr http://timestamp.digicert.com /td SHA256 $catalog
    if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)." }
}

Write-Host "Driver package staged: $output"
