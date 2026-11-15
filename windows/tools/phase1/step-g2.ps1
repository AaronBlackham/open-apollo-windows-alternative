# Twin X gate G2 "observe" (docs/spec-twinx.md section 7): bind ONLY the Twin X to apollo.sys in observe mode
# (Mode 0: identify and quiesce, nothing else), read its identity, keep the x8p exactly as it is. ADMIN.
#   powershell -ExecutionPolicy Bypass -File C:\Users\aaron\Desktop\Avium\apollo-win\tools\phase1\step-g2.ps1 [-Package <folder>] [-Yes]
# Default package: builds\2026-11-14-g2\package (the frozen G2 build).
#
# Both units share one loaded apollo.sys, so the x8p must already run this package:
#   run 1, if the x8p runs an older build: puts this package on the x8p ONLY (install.ps1 -Unit x8p; Windows
#          audio on the x8p stops for a few seconds), then asks you to power-cycle the x8p and run this again.
#          The Twin X is not touched in run 1.
#   run 2 (x8p on this package): install.ps1 -Unit twinx writes Parameters\Models\99\Mode=0, binds the Twin X
#          with its exact hardware ID, checks the x8p is unchanged, then this script saves
#          apollo-diag units, --unit twinx info and log, and the x8p's info and endpoints to step-g2.log.
# What you hear: the Twin X speakers go silent (its UA endpoints stop playing); the x8p and its HP 1 keep working.
# The way back: revert-unit.ps1 -Unit twinx (or the "Twin X back to UA" shortcut), then power-cycle the Twin X.
#Requires -RunAsAdministrator
param([string]$Package = '', [switch]$Yes)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Package) { $Package = Join-Path $root 'builds\2026-11-14-g2\package' }
$inf  = Join-Path $Package 'apollo.inf'
$log  = Join-Path $PSScriptRoot 'step-g2.log'
"step-g2 $(Get-Date -Format o) package $Package" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }
function Done([int]$rc) { Note "step-g2 exit $rc. Log: $log"; Read-Host 'Press Enter to close' | Out-Null; exit $rc }

$x8pHwid   = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00'
$twinxHwid = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00'
$ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

if (-not (Test-Path -LiteralPath $inf)) { Note "No apollo.inf in $Package (freeze the G2 build there, or pass -Package)."; Done 1 }
foreach ($h in $x8pHwid, $twinxHwid) {
    if (-not (Select-String -LiteralPath $inf -SimpleMatch -Pattern $h -Quiet)) { Note "$inf does not list $h`: not the G2 package."; Done 1 }
}
$pkgVer = ''
$m = Select-String -LiteralPath $inf -Pattern '^\s*DriverVer\s*=\s*[^,]*,\s*([0-9.]+)' | Select-Object -First 1
if ($m) { $pkgVer = $m.Matches[0].Groups[1].Value }
Note "G2 package DriverVer $pkgVer"

# apollo-diag only from the verified admin-only copy (safe-tools.ps1).
. (Join-Path $PSScriptRoot 'safe-tools.ps1')
$tools = Initialize-SafeTools $root @('tools\apollo-diag\apollo-diag.exe') ${function:Note}
if (-not $tools) { Note 'Stopped before changing anything.'; Done 1 }
$diag = $tools['tools\apollo-diag\apollo-diag.exe']
function Diag([string[]]$a) {
    $out = (& $diag @a 2>&1 | Out-String)
    Note ">>> apollo-diag $($a -join ' ')"
    Note $out.TrimEnd()
    return $out
}
function Unit([string]$hwid) {
    $d = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "$hwid*" } | Select-Object -First 1
    if (-not $d) { return $null }
    [pscustomobject]@{
        InstanceId = $d.InstanceId
        Status     = $d.Status
        Service    = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
        Inf        = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
        Version    = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
    }
}

# The registered ASIO DLL (Program Files\Avium\ASIO) must be the build that picks the x8p by model: the old
# one opens the first Apollo interface, which can be the Twin X once it joins our driver. Checked on EVERY
# run; updated safely (temp copy, verify, move the old, possibly loaded, DLL aside, then register).
# TRUE when the registered DLL matches tools-manifest.txt.
function Sync-AsioDll {
    $rel  = 'asio\bin\avium-asio.dll'
    $want = (Read-ToolsManifest $root)[$rel.ToLowerInvariant()]
    $src  = Join-Path $root $rel
    $dst  = Join-Path $env:ProgramFiles 'Avium\ASIO\avium-asio.dll'
    if (-not $want -or -not (Test-Path -LiteralPath $src)) { Note "ASIO DLL: no manifest entry or no $rel (rebuild with tools\build-all.cmd)."; return $false }
    if ((Test-Path -LiteralPath $dst) -and (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash.ToUpperInvariant() -eq $want) {
        Note "ASIO DLL: the registered copy is current ($dst)."
        return $true
    }
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
        $tmp = "$dst.new"
        Copy-Item -LiteralPath $src -Destination $tmp -Force -ErrorAction Stop
        if ((Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash.ToUpperInvariant() -ne $want) {
            Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
            Note 'ASIO DLL: the copy does not match tools-manifest.txt; nothing changed. Tell Claude.'
            return $false
        }
        if (Test-Path -LiteralPath $dst) {
            # A loaded DLL can be renamed (not overwritten): programs holding it keep the old file until they close.
            Move-Item -LiteralPath $dst -Destination ("$dst.old-" + (Get-Date -Format 'yyyyMMdd-HHmmss')) -Force -ErrorAction Stop
        }
        Move-Item -LiteralPath $tmp -Destination $dst -Force -ErrorAction Stop
        $p = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\regsvr32.exe') -ArgumentList '/s', "`"$dst`"" -Wait -PassThru
        Get-ChildItem -LiteralPath (Split-Path $dst) -Filter 'avium-asio.dll.old-*' -ErrorAction SilentlyContinue |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }   # best effort (in use: stays)
        Note "ASIO DLL updated and registered (regsvr32 exit $($p.ExitCode)): $dst"
        return ($p.ExitCode -eq 0)
    } catch {
        Note "ASIO DLL: could not update ($($_.Exception.Message)). Close Ableton / Avium Measure / ASIO Link Pro and run step-g2 again."
        return $false
    }
}

# --- 1. both units present; the x8p on apollo.sys ---
$x8p = Unit $x8pHwid
$twx = Unit $twinxHwid
if (-not $x8p) { Note 'The x8p is not connected: G2 needs it up (the Twin X is daisy-chained behind it).'; Done 1 }
if (-not $twx) { Note 'The Twin X is not connected or not powered on.'; Done 1 }
Note "x8p:    $($x8p.InstanceId) driver $($x8p.Inf) $($x8p.Version) service $($x8p.Service) status $($x8p.Status)"
Note "Twin X: $($twx.InstanceId) driver $($twx.Inf) $($twx.Version) service $($twx.Service) status $($twx.Status)"
if ($x8p.Service -ne 'apollo' -or $x8p.Status -ne 'OK') { Note 'The x8p is not running on apollo.sys: install a release on it first (step5.ps1).'; Done 1 }

# --- 2. run 1: the x8p must run this very package (one loaded image for both units) ---
$x8pInfo = Diag @('--unit', 'x8p', 'info')
$x8pV6 = $x8pInfo -match '(?m)^Version\s+6\b'
if ($x8p.Version -ne $pkgVer -or -not $x8pV6) {
    Note ''
    Note "RUN 1: the x8p runs apollo.sys $($x8p.Version) (interface $(if ($x8pV6) { 6 } else { 'older than 6' })); the Twin X would join that"
    Note 'image, not this build. So first this package goes on the x8p ONLY (the Twin X is not touched now):'
    Note '  Windows audio on the x8p stops for a few seconds while its driver restarts; close Ableton and anything using it.'
    if (-not $Yes) {
        $a = Read-Host '[Enter] put the G2 build on the x8p now, [Q] quit'
        if ($a -match '^[Qq]') { Note 'Nothing changed.'; Done 1 }
    }
    $args1 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'install.ps1'), '-Unit', 'x8p', '-Package', $Package)
    if ($Yes) { $args1 += '-Yes' }
    & $ps @args1
    $rc = $LASTEXITCODE
    Note "install.ps1 -Unit x8p exit $rc"
    if ($rc -ne 0) { Note 'The x8p install did not finish: tell Claude (install.log has the details). The Twin X was not touched.'; Done 1 }
    [void](Sync-AsioDll)    # checked again, and required, before the Twin X bind
    Note ''
    Note 'Run 1 done. Now power-cycle the x8p (off 10 s, on). The Twin X behind it drops too and comes back on UA''s driver.'
    Note 'Wait about 30 s, check your headphones on the x8p HP 1 work, then run step-g2.ps1 again for the Twin X.'
    Done 0
}
Note "The x8p runs this package ($pkgVer, interface 6): going on with the Twin X."
if (-not (Sync-AsioDll)) {
    Note 'Stopped before the Twin X bind: the registered ASIO DLL must be the current build first (see above).'
    Done 1
}

# --- 3. run 2: bind the Twin X in observe mode (install.ps1 writes Models\99\Mode=0 first) ---
$lnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Twin X back to UA.lnk'
if (Test-Path -LiteralPath $lnk) { Note "Way back ready: the desktop shortcut 'Twin X back to UA' (revert-unit.ps1 -Unit twinx -Yes)." }
else { Note "note: no 'Twin X back to UA' shortcut on the desktop; make it with new-revert-shortcut.ps1 (no admin), or use revert-unit.ps1 -Unit twinx." }
if ($twx.Service -eq 'apollo') {
    Note 'The Twin X is already on apollo.sys: reading it again (nothing is reinstalled).'
} else {
    Note ''
    Note 'Binding the Twin X to apollo.sys in OBSERVE mode. Its speakers go silent until revert-unit.ps1 -Unit twinx.'
    Note 'UA Console and UA Connect lose the Twin X (and are closed if nothing else uses UA''s driver).'
    if (-not $Yes) {
        $a = Read-Host '[Enter] bind the Twin X now, [Q] quit'
        if ($a -match '^[Qq]') { Note 'Nothing changed.'; Done 1 }
    }
    $args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'install.ps1'), '-Unit', 'twinx', '-Package', $Package)
    if ($Yes) { $args2 += '-Yes' }
    & $ps @args2
    $rc = $LASTEXITCODE
    Note "install.ps1 -Unit twinx exit $rc (install.log has every step)"
    if ($rc -ne 0) {
        Note 'The Twin X bind did not finish cleanly: read the lines above. If devcon asked for a restart, power-cycle the'
        Note 'TWIN X only (the x8p stays up) and run this script again. To undo: revert-unit.ps1 -Unit twinx.'
    }
}

# --- 4. what G2 is for: the Twin X as UA left it, and the x8p unchanged ---
Start-Sleep -Seconds 2
Note ''
Note '=== G2 readings (docs/spec-twinx.md Appendix A: compare family 19, DSPs, FPGA, firmware, serial digits, ==='
Note '=== public 0x19 -> type 0x99, ready cap, HW_UID, BAR0 size, 0x01A4/0x01AC, descriptors and clock mask (STALE)) ==='
Diag @('units') | Out-Null
Diag @('--unit', 'twinx', 'info') | Out-Null
Diag @('--unit', 'twinx', 'log') | Out-Null
Note '=== the x8p after ==='
Diag @('--unit', 'x8p', 'info') | Out-Null
Diag @('--unit', 'x8p', 'endpoints') | Out-Null
$x8pNow = Unit $x8pHwid
$twxNow = Unit $twinxHwid
if ($x8pNow) { Note "x8p now:    driver $($x8pNow.Inf) $($x8pNow.Version) service $($x8pNow.Service) status $($x8pNow.Status)" }
else { Note 'WARNING: the x8p is not present now. Tell Claude.' }
if ($twxNow) { Note "Twin X now: driver $($twxNow.Inf) $($twxNow.Version) service $($twxNow.Service) status $($twxNow.Status)" }
Note ''
Note 'Done. Send Claude step-g2.log. The Twin X stays on apollo.sys (observe) until you run:'
Note "  powershell -ExecutionPolicy Bypass -File $(Join-Path $PSScriptRoot 'revert-unit.ps1') -Unit twinx"
Note 'then power-cycle the Twin X (the x8p stays up).'
Done 0
