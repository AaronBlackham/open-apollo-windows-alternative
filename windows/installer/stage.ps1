# Assembles the installer payload (build.cmd step 5). Copies, never builds; reads a driver build, app\bin and the
# docs, checks them, makes the driver package UNSIGNED, and writes payload-manifest.sha256.
#   powershell -NoProfile -ExecutionPolicy Bypass -File stage.ps1 -Build <dir> -Stage <dir> -Helper <apollo-helper.exe>
#       -Info <stage-info.cmd to write> [-Ewdk D:]
# -Build is either a frozen build (package\apollo.inf, bin\, src\include\) or a built source tree
# (driver\x64\Release\apollo\, tools\*\*.exe, asio\bin\, include\), as release.cmd makes in a neutral folder.
#
# Unsigned driver package: the payload's apollo.sys and apollo.cat carry no signature at all. Whatever signed them at
# build time (the WDK's test signing) is stripped: signtool remove /s on apollo.sys, and apollo.cat is made again,
# unsigned, by the EWDK's Inf2Cat. Setup signs both on the user's PC (sign-local.ps1). Then EVERY staged file is
# checked to carry no Authenticode signature (Get-AuthenticodeSignature: NotSigned or no signer). PowerShell 5.1, ASCII.
param(
    [Parameter(Mandatory = $true)][string]$Build,
    [Parameter(Mandatory = $true)][string]$Stage,
    [Parameter(Mandatory = $true)][string]$Helper,
    [Parameter(Mandatory = $true)][string]$Info,
    [string]$Ewdk = 'D:'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot          # the source tree this installer\ belongs to
$Build = (Resolve-Path $Build).Path

function Need([string]$p) { if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { throw "Missing: $p" }; $p }
# A native tool's output and exit code. stderr lines must not become terminating errors (PowerShell 5.1 turns them
# into ErrorRecords under 2>&1, and $ErrorActionPreference is Stop here).
function Invoke-Native([string]$exe, [string[]]$arguments) {
    $ErrorActionPreference = 'Continue'
    $text = (& $exe @arguments 2>&1 | ForEach-Object { "$_" }) -join "`r`n"
    return @{ Code = $LASTEXITCODE; Text = $text }
}

# ---- the EWDK's signtool (x64) and Inf2Cat (x86): from the EWDK environment, else the newest kit under <Ewdk>
function Find-KitTool([string]$arch, [string]$exe) {
    if ($env:WindowsSdkDir -and $env:Version_Number) {
        $p = Join-Path $env:WindowsSdkDir ("bin\{0}\{1}\{2}" -f $env:Version_Number, $arch, $exe)
        if (Test-Path -LiteralPath $p) { return $p }
    }
    $bin = Join-Path $Ewdk 'Program Files\Windows Kits\10\bin'
    $best = $null
    foreach ($d in @(Get-ChildItem -LiteralPath $bin -Directory -ErrorAction SilentlyContinue)) {
        $v = $null
        if (-not [version]::TryParse($d.Name, [ref]$v)) { continue }
        $p = Join-Path $d.FullName "$arch\$exe"
        if ((Test-Path -LiteralPath $p) -and (-not $best -or $v -gt $best.V)) { $best = @{ V = $v; P = $p } }
    }
    if (-not $best) { throw "No $exe ($arch) in the EWDK at $Ewdk (mount the EWDK ISO, or pass -Ewdk)" }
    return $best.P
}
$signtool = Find-KitTool 'x64' 'signtool.exe'
$inf2cat = Find-KitTool 'x86' 'Inf2Cat.exe'

# ---- the build's layout
$map = [ordered]@{}
if (Test-Path -LiteralPath (Join-Path $Build 'package\apollo.inf')) {
    $layout = 'frozen build'
    $pkg = Join-Path $Build 'package'
    $tool = @{
        'apollo-diag.exe'   = Join-Path $Build 'bin\apollo-diag.exe'
        'loopback-test.exe' = Join-Path $Build 'bin\loopback-test.exe'
        'wasapi-loop.exe'   = Join-Path $Build 'bin\wasapi-loop.exe'
        'asio-test.exe'     = Join-Path $Build 'bin\asio-test.exe'
    }
    $asioDll = Join-Path $Build 'bin\avium-asio.dll'
    $optionalTools = @('loopback-test.exe', 'wasapi-loop.exe')     # older frozen builds may not carry these
} elseif (Test-Path -LiteralPath (Join-Path $Build 'driver\x64\Release\apollo\apollo.inf')) {
    $layout = 'source tree'
    $pkg = Join-Path $Build 'driver\x64\Release\apollo'
    $tool = @{
        'apollo-diag.exe'   = Join-Path $Build 'tools\apollo-diag\apollo-diag.exe'
        'loopback-test.exe' = Join-Path $Build 'tools\loopback-test\loopback-test.exe'
        'wasapi-loop.exe'   = Join-Path $Build 'tools\wasapi-loop\wasapi-loop.exe'
        'asio-test.exe'     = Join-Path $Build 'asio\bin\asio-test.exe'
    }
    $asioDll = Join-Path $Build 'asio\bin\avium-asio.dll'
    $optionalTools = @()
} else {
    throw "No driver package under $Build (expected package\apollo.inf or driver\x64\Release\apollo\apollo.inf)"
}

# Source -> payload path. The layout the installer script and apollo-helper expect.
$map['apollo-helper.exe']   = Need $Helper
$map['sign-local.ps1']      = Need (Join-Path $PSScriptRoot 'sign-local.ps1')
$map['apollo-control.exe']  = Need (Join-Path $root 'app\bin\apollo-control.exe')
# [PlugSetup] UAD plug-ins on a release install: the parameter planner Apollo Control starts (next to the exe, where
# plugin_engine.h HelperStart looks) and the setup's code in plugin-setup\tools (setup-plugins.ps1, the vendored
# open-apollo mapper and our ports: GPL source). Only source files: no UA data, no maps, no downloads, no Python.
$map['plughelper.py']       = Need (Join-Path $root 'tools\plugin-host\plughelper.py')
$ptools = Join-Path $root 'plugins\tools'
$null = Need (Join-Path $ptools 'setup-plugins.ps1')
$srcExt = @('.py', '.ps1', '.json', '.md', '.txt', '.conf')
foreach ($f in @(Get-ChildItem -LiteralPath $ptools -Recurse -File)) {
    $rel = $f.FullName.Substring($ptools.Length + 1)
    if ($rel -match '(^|\\)__pycache__\\' -or $f.Extension -eq '.pyc') { continue }
    if ($srcExt -notcontains $f.Extension.ToLowerInvariant() -and $f.Name -ne 'LICENSE') {
        throw "plugins\tools\$rel is not a source file: only code and its text files go into plugin-setup\tools"
    }
    $map['plugin-setup\tools\' + $rel] = $f.FullName
}
$map['driver\apollo.inf']   = Need (Join-Path $pkg 'apollo.inf')
$map['driver\apollo.sys']   = Need (Join-Path $pkg 'apollo.sys')
# driver\apollo.cat is made below, unsigned, by Inf2Cat (never copied from the build)
$map['asio\avium-asio.dll'] = Need $asioDll
foreach ($t in 'apollo-diag.exe', 'loopback-test.exe', 'wasapi-loop.exe', 'asio-test.exe') {
    if ($optionalTools -contains $t -and -not (Test-Path -LiteralPath $tool[$t])) {
        Write-Host "note: $($tool[$t]) is not in this frozen build; tools\$t is left out"
        continue
    }
    $map["tools\$t"] = Need $tool[$t]
}
# User documents: no developer notes (they hold local paths and unit identifiers).
$map['docs\RELEASE-NOTES.md']       = Need (Join-Path $PSScriptRoot 'RELEASE-NOTES.md')
$map['docs\INSTALLER.md']           = Need (Join-Path $PSScriptRoot 'README.md')
$map['docs\ASIO.md']                = Need (Join-Path $root 'docs\ASIO.md')
$map['docs\ASIO-SDK-LICENSE.txt']   = Need (Join-Path $root 'third_party\asiosdk\LICENSE.txt')
# Licences: the GPL texts and which part is under which (LICENSES\README.txt)
$map['docs\LICENSES.txt']          = Need (Join-Path $root 'LICENSES\README.txt')
$map['docs\GPL-2.0.txt']           = Need (Join-Path $root 'LICENSES\GPL-2.0.txt')
$map['docs\GPL-3.0.txt']           = Need (Join-Path $root 'LICENSES\GPL-3.0.txt')
$ui = Join-Path $root 'app\bin\ui'
if (-not (Test-Path -LiteralPath (Join-Path $ui 'index.html'))) { throw "Missing: $ui\index.html" }
Get-ChildItem -LiteralPath $ui -Recurse -File | ForEach-Object {
    $map['ui\' + $_.FullName.Substring($ui.Length + 1)] = $_.FullName
}
# Registry defaults: a frozen build's own defaults.json wins over the installer's.
$defaults = Join-Path $Build 'defaults.json'
if ($layout -ne 'frozen build' -or -not (Test-Path -LiteralPath $defaults)) { $defaults = Join-Path $PSScriptRoot 'defaults.json' }
$map['defaults.json'] = Need $defaults
$null = Get-Content -Raw -LiteralPath $defaults | ConvertFrom-Json   # throws on invalid JSON (the helper's test parses it too)

# ---- the INF: DriverVer names the output; provider and hardware IDs
$infText = Get-Content -Raw -LiteralPath $map['driver\apollo.inf']
if ($infText -notmatch '(?m)^\s*DriverVer\s*=\s*([0-9/]+)\s*,\s*([0-9]+(\.[0-9]+){3})\s*$') { throw 'No DriverVer in apollo.inf (not a built package?)' }
$driverDate = $Matches[1]; $driverVer = $Matches[2]
if ($infText -notmatch '(?m)^\s*ProviderName\s*=\s*"Avium"') { throw 'apollo.inf provider is not "Avium" (apollo-helper unbind looks for it)' }
if ($infText -notmatch '(?m)^\s*CatalogFile\s*=\s*apollo\.cat\s*$') { throw 'apollo.inf does not name apollo.cat as its catalog' }
# Hardware IDs on the INF's model lines: exactly the x8p's and the Twin X Gen 1's. Any other UA ID, or one without
# its exact SUBSYS, is refused: Setup and apollo-helper switch only these two units, and every other Universal Audio
# device (Gen 2 units, other models) must stay on UA's driver.
$allowedIds = @('PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00', 'PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00')
$seen = @{}
foreach ($line in ($infText -split "\r?\n")) {
    $code = ($line -split ';', 2)[0]          # comments out; no hardware ID ever follows a ';'
    foreach ($m in [regex]::Matches($code, 'PCI\\VEN_1A00[^\s,;"]*', 'IgnoreCase')) {
        $id = $m.Value.ToUpperInvariant()
        if ($allowedIds -notcontains $id) {
            throw "apollo.inf lists $($m.Value): only the x8p and the Twin X Gen 1 exact IDs are allowed"
        }
        $seen[$id] = $true
    }
}
foreach ($id in $allowedIds) { if (-not $seen.ContainsKey($id)) { throw "apollo.inf does not list $id" } }

# ---- fresh staging folder
if (Test-Path -LiteralPath $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage | Out-Null
foreach ($rel in $map.Keys) {
    $dst = Join-Path $Stage $rel
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $dst))
    Copy-Item -LiteralPath $map[$rel] -Destination $dst
}

# ---- the driver package, unsigned
$drv = Join-Path $Stage 'driver'
$stSys = Join-Path $drv 'apollo.sys'
$sig = Get-AuthenticodeSignature -LiteralPath $stSys
if ($sig.SignerCertificate) {
    Write-Host "apollo.sys carries a build-time signature ($($sig.SignerCertificate.Subject)): stripping it"
    $r = Invoke-Native $signtool @('remove', '/s', $stSys)
    if ($r.Code -ne 0) { throw "signtool remove /s failed ($($r.Code)): $($r.Text)" }
} else {
    Write-Host 'apollo.sys carries no signature (built with signing off)'
}
$r = Invoke-Native $inf2cat @("/driver:$drv", '/os:10_x64,10_NI_X64', '/uselocaltime')
if ($r.Code -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $drv 'apollo.cat'))) {
    throw "Inf2Cat failed ($($r.Code)):`r`n$($r.Text)"
}
Write-Host "apollo.cat made again by $(Split-Path -Leaf $inf2cat) (unsigned)"
$map['driver\apollo.cat'] = '(Inf2Cat)'
$extra = @(Get-ChildItem -LiteralPath $drv -File | Where-Object { @('apollo.inf', 'apollo.sys', 'apollo.cat') -notcontains $_.Name })
if ($extra.Count -gt 0) { throw "Unexpected files in the staged driver folder: $(($extra | ForEach-Object Name) -join ', ')" }

# ---- no file in the payload carries any signature (nothing of anyone's key, no certificate files)
$signed = @()
Get-ChildItem -LiteralPath $Stage -Recurse -File | ForEach-Object {
    if (@('.cer', '.crt', '.pfx', '.p12', '.p7b', '.pvk', '.spc') -contains $_.Extension.ToLowerInvariant()) {
        $signed += "$($_.FullName): a certificate file"
        return
    }
    $s = Get-AuthenticodeSignature -LiteralPath $_.FullName
    if ($s.SignerCertificate) { $signed += "$($_.FullName): signed by $($s.SignerCertificate.Subject) ($($s.Status))" }
}
if ($signed.Count -gt 0) { throw ("Signed files in the payload:`r`n  " + ($signed -join "`r`n  ")) }
foreach ($f in @($stSys, (Join-Path $drv 'apollo.cat'))) {
    $st = (Get-AuthenticodeSignature -LiteralPath $f).Status
    if ($st -ne 'NotSigned') { throw "$f is '$st', expected NotSigned" }
}
Write-Host 'Unsigned payload: apollo.sys and apollo.cat are NotSigned; no staged file carries a signature.'

# ---- SHA-256 of every payload file (sha256sum format, Windows relative paths)
$lines = foreach ($rel in ($map.Keys | Sort-Object { $_ } -CaseSensitive)) {
    '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Stage $rel)).Hash.ToLowerInvariant(), $rel
}
$manifest = Join-Path $Stage 'payload-manifest.sha256'
[IO.File]::WriteAllLines($manifest, [string[]]$lines, (New-Object Text.UTF8Encoding $false))

@(
    '@rem written by stage.ps1',
    "set DRIVERVER=$driverVer",
    "set DRIVERDATE=$driverDate",
    "set PAYLOADFILES=$($map.Count)"
) | Set-Content -Encoding ASCII -LiteralPath $Info

Write-Host "Staged $($map.Count) files (+ payload-manifest.sha256) from the $layout $Build"
Write-Host "Driver $driverVer ($driverDate), defaults from $defaults"
Write-Host "Tools: $signtool, $inf2cat"
