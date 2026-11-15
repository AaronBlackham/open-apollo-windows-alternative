# Assembles the installer payload (build.cmd step 2). Copies, never builds; reads the frozen build,
# app\bin, docs and the driver's test certificate, checks them, and writes payload-manifest.sha256.
#   powershell -NoProfile -ExecutionPolicy Bypass -File stage.ps1 -Build <frozen build> -Stage <dir>
#       -Helper <apollo-helper.exe> -Thumbprint <expected cert SHA-1> -Info <stage-info.cmd to write>
param(
    [Parameter(Mandatory = $true)][string]$Build,
    [Parameter(Mandatory = $true)][string]$Stage,
    [Parameter(Mandatory = $true)][string]$Helper,
    [Parameter(Mandatory = $true)][string]$Thumbprint,
    [Parameter(Mandatory = $true)][string]$Info,
    [string]$Cert = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot          # apollo-win
if (-not $Cert) { $Cert = Join-Path $root 'driver\x64\Release\apollo.cer' }
$Build = (Resolve-Path $Build).Path

function Need([string]$p) { if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { throw "Missing: $p" }; $p }

# Source -> payload path. The layout the installer script and apollo-helper expect.
$map = [ordered]@{}
$map['apollo-helper.exe']        = Need $Helper
$map['apollo-control.exe']       = Need (Join-Path $root 'app\bin\apollo-control.exe')
$map['driver\apollo.inf']        = Need (Join-Path $Build 'package\apollo.inf')
$map['driver\apollo.sys']        = Need (Join-Path $Build 'package\apollo.sys')
$map['driver\apollo.cat']        = Need (Join-Path $Build 'package\apollo.cat')
$map['driver\apollo.cer']        = Need $Cert
$map['asio\avium-asio.dll']      = Need (Join-Path $Build 'bin\avium-asio.dll')
foreach ($t in 'apollo-diag.exe', 'loopback-test.exe', 'wasapi-loop.exe', 'asio-test.exe') {
    $map["tools\$t"] = Need (Join-Path $Build "bin\$t")
}
foreach ($d in 'PHASE1.md', 'PHASE2.md', 'STRIPS.md', 'ASIO.md') {
    $map["docs\$d"] = Need (Join-Path $root "docs\$d")
}
$ui = Join-Path $root 'app\bin\ui'
if (-not (Test-Path -LiteralPath (Join-Path $ui 'index.html'))) { throw "Missing: $ui\index.html" }
Get-ChildItem -LiteralPath $ui -Recurse -File | ForEach-Object {
    $map['ui\' + $_.FullName.Substring($ui.Length + 1)] = $_.FullName
}
# Registry defaults: the frozen build's own defaults.json wins over the installer's.
$defaults = Join-Path $Build 'defaults.json'
if (-not (Test-Path -LiteralPath $defaults)) { $defaults = Join-Path $PSScriptRoot 'defaults.json' }
$map['defaults.json'] = Need $defaults
$null = Get-Content -Raw -LiteralPath $defaults | ConvertFrom-Json   # throws on invalid JSON

# The certificate must be the expected one, and must be what signed the catalog and the driver.
$x509 = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $Cert
if ($x509.Thumbprint -ne $Thumbprint) { throw "apollo.cer is $($x509.Thumbprint), expected $Thumbprint" }
foreach ($f in 'driver\apollo.cat', 'driver\apollo.sys') {
    $sig = Get-AuthenticodeSignature -LiteralPath $map[$f]
    if (-not $sig.SignerCertificate -or $sig.SignerCertificate.Thumbprint -ne $Thumbprint) {
        throw "$f is not signed with the test certificate $Thumbprint (signer: $($sig.SignerCertificate.Thumbprint))"
    }
}

# DriverVer from the INF names the output.
$infText = Get-Content -Raw -LiteralPath $map['driver\apollo.inf']
if ($infText -notmatch '(?m)^\s*DriverVer\s*=\s*([0-9/]+)\s*,\s*([0-9]+(\.[0-9]+){3})\s*$') { throw 'No DriverVer in apollo.inf' }
$driverDate = $Matches[1]; $driverVer = $Matches[2]
if ($infText -notmatch '(?m)^\s*ProviderName\s*=\s*"Avium"') { throw 'apollo.inf provider is not "Avium" (apollo-helper unbind looks for it)' }
if ($infText -notmatch 'PCI\\VEN_1A00&DEV_0002&SUBSYS_00141A00') { throw 'apollo.inf does not match the x8p hardware ID' }
# Hardware IDs on the INF's model lines: the x8p (the only unit Setup binds) and, from gate G2 on, the
# Twin X Gen 1 (test only, never bound by Setup; apollo-helper hands it back to UA at uninstall). Any
# other UA ID, or one without its exact SUBSYS, is refused: apollo-helper knows only these two units.
$allowedIds = @('PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00', 'PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00')
foreach ($line in ($infText -split "\r?\n")) {
    $code = ($line -split ';', 2)[0]          # comments out; no hardware ID ever follows a ';'
    foreach ($m in [regex]::Matches($code, 'PCI\\VEN_1A00[^\s,;"]*', 'IgnoreCase')) {
        if ($allowedIds -notcontains $m.Value.ToUpperInvariant()) {
            throw "apollo.inf lists $($m.Value): only the x8p and the Twin X Gen 1 exact IDs are allowed"
        }
    }
}

# Fresh staging folder.
if (Test-Path -LiteralPath $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage | Out-Null
foreach ($rel in $map.Keys) {
    $dst = Join-Path $Stage $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $map[$rel] -Destination $dst
}

# SHA-256 of every payload file (sha256sum format, Windows relative paths).
$lines = foreach ($rel in ($map.Keys | Sort-Object { $_ } -CaseSensitive)) {
    '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Stage $rel)).Hash.ToLowerInvariant(), $rel
}
$manifest = Join-Path $Stage 'payload-manifest.sha256'
[IO.File]::WriteAllLines($manifest, [string[]]$lines, (New-Object Text.UTF8Encoding $false))

@(
    '@rem written by stage.ps1',
    "set DRIVERVER=$driverVer",
    "set DRIVERDATE=$driverDate",
    "set CERTTHUMB=$($x509.Thumbprint)",
    "set PAYLOADFILES=$($map.Count)"
) | Set-Content -Encoding ASCII -LiteralPath $Info

Write-Host "Staged $($map.Count) files (+ payload-manifest.sha256) from $Build"
Write-Host "Driver $driverVer ($driverDate), certificate $($x509.Thumbprint), defaults from $defaults"
