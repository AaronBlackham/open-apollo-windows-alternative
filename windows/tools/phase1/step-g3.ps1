# Twin X gate G3 "bring-up + transport" (docs/spec-twinx.md section 7 and 6.9.3). ADMIN PowerShell.
#   powershell -ExecutionPolicy Bypass -File C:\apollo-win\tools\phase1\step-g3.ps1 [-Package <folder>] [-Yes]
#   ... -WarmTest   after pass 2 and the tone test: restart the Twin X's driver (a WARM start) and read it again
#   ... -Rollback   the Twin X back to observe: its unit key Mode = 0, then power-cycle the Twin X
# Default package: builds\2026-11-14-g3\package (the frozen G3 build).
#
# Both units share one loaded apollo.sys, and the Twin X needs a COLD start of its own for G3.
#   Pass 1 (either unit not on this package yet). Apollo Control must be quit first.
#     - Twin X already on apollo.sys (observe, since G2): stages the package and updates BOTH units with
#       devcon (x8p, then Twin X). The new apollo.sys cannot load while either unit still holds the old one,
#       so both keep running the old build until they are off at the same time. devcon may say a restart is
#       needed: expected here, the power cycle below finishes it.
#     - Twin X still on UA's driver: install.ps1 -Unit x8p (the new image loads at once), then install.ps1
#       -Unit twinx (observe).
#     The Twin X stays at Mode 0 (observe) through all of this. Then the script writes
#     Parameters\Units\<Twin X key>\Mode = 1, read at the Twin X's next start (nothing is restarted now),
#     and asks you to power BOTH units off and on: the Twin X needs its own cold start (an x8p power cycle
#     alone brings it back warm, it stays powered), and the new image loads only when both are gone.
#   Pass 2 (both units on this package, the new image running): waits for the Twin X bring-up, saves units,
#     info, engine, readback, levels and log of the Twin X and info, engine, endpoints, strips and log of the
#     x8p to step-g3.log, and prints PASS / CHECK against docs/spec-twinx.md Appendix A.
# THIS SCRIPT NEVER PLAYS A TONE: the Twin X monitor outputs are the speakers. The tone test is by hand
# (pass 2 prints the commands), after the speakers are turned down.
# The way back: step-g3.ps1 -Rollback (Twin X observes again after its power cycle), or the whole unit back
# to UA: revert-unit.ps1 -Unit twinx (the "Twin X back to UA" shortcut), then step-g3.ps1 -Rollback too, so
# its unit key does not ask for Mode 1 the next time it is bound.
# PowerShell 5.1, ASCII only.
#Requires -RunAsAdministrator
param([string]$Package = '', [switch]$Yes, [switch]$WarmTest, [switch]$Rollback)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Package) { $Package = Join-Path $root 'builds\2026-11-14-g3\package' }
$inf  = Join-Path $Package 'apollo.inf'
$log  = Join-Path $PSScriptRoot 'step-g3.log'
$defaultsFile = Join-Path $PSScriptRoot 'step-g3.defaults.json'
"step-g3 $(Get-Date -Format o) package $Package WarmTest=$WarmTest Rollback=$Rollback" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }
function Done([int]$rc) { Note "step-g3 exit $rc. Log: $log"; Read-Host 'Press Enter to close' | Out-Null; exit $rc }

$x8pHwid   = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00'
$twinxHwid = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00'
$params    = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters'
$twinxKeyG2 = '000000002F00BEEF'      # the Twin X's HW_UID read at gate G2 (Units\<key>); checked against the unit
$ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
$script:nPass = 0
$script:nCheck = 0
function Pass([string]$s) { $script:nPass++; Note "PASS   $s" }
function Check([string]$s) { $script:nCheck++; Note "CHECK  $s" }

# --- tools: apollo-diag (and devcon for pass 1) only from the verified admin-only copies (safe-tools.ps1) ---
. (Join-Path $PSScriptRoot 'safe-tools.ps1')
$tools = Initialize-SafeTools $root @('tools\bin\devcon.exe', 'tools\apollo-diag\apollo-diag.exe') ${function:Note}
if (-not $tools) { Note 'Stopped before changing anything.'; Done 1 }
$devcon = $tools['tools\bin\devcon.exe']
$diag   = $tools['tools\apollo-diag\apollo-diag.exe']
$audio = $true
try { . (Join-Path $PSScriptRoot 'audio-helpers.ps1') } catch { $audio = $false; Note "note: audio helpers unavailable ($($_.Exception.Message)); defaults will not be restored" }

function Diag([string[]]$a) {
    $out = (& $diag @a 2>&1 | Out-String)
    Note ">>> apollo-diag $($a -join ' ')"
    Note $out.TrimEnd()
    return $out
}
function DiagQuiet([string[]]$a) { return (& $diag @a 2>&1 | Out-String) }
function Field([string]$text, [string]$name) {
    $m = [regex]::Match($text, "(?m)^$([regex]::Escape($name))\s+(.*?)\s*$")
    if ($m.Success) { return $m.Groups[1].Value } else { return '' }
}
function FirstInt([string]$s) {
    $m = [regex]::Match($s, '^\s*(0x[0-9A-Fa-f]+|\d+)')
    if (-not $m.Success) { return -1 }
    if ($m.Groups[1].Value -like '0x*') { return [Convert]::ToInt64($m.Groups[1].Value.Substring(2), 16) }
    return [int64]$m.Groups[1].Value
}
function Unit([string]$hwid) {
    $d = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "$hwid*" } | Select-Object -First 1
    if (-not $d) { return $null }
    [pscustomobject]@{
        InstanceId = $d.InstanceId
        Status     = $d.Status
        Problem    = $d.Problem
        Service    = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
        Inf        = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
        Version    = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
    }
}
function Format-Unit($u) { "$($u.InstanceId): driver $($u.Inf) $($u.Version) service $($u.Service) status $($u.Status) problem $($u.Problem)" }
function Set-UnitMode([string]$key, [int]$mode) {
    $k = Join-Path $params "Units\$key"
    if (-not (Test-Path $k)) { New-Item -Path $k -Force | Out-Null }
    Set-ItemProperty -Path $k -Name Mode -Value $mode -Type DWord
    return (Get-ItemProperty $k).Mode
}
function Get-UnitMode([string]$key) {
    $k = Join-Path $params "Units\$key"
    if (-not (Test-Path $k)) { return $null }
    return (Get-ItemProperty $k -ErrorAction SilentlyContinue).Mode
}
# The Twin X's unit key as the driver names it (GET_INFO UnitKey), else the one G2 read.
function TwinxKey {
    $i = DiagQuiet @('--unit', 'twinx', 'info')
    $k = Field $i 'UnitKey'
    if ($k -match '^([0-9A-Fa-f]{16}|SN[0-9A-Za-z]+)$') { return $k }
    return $twinxKeyG2
}
function Quit-Blockers {
    foreach ($p in 'apollo-control', 'apollo-diag', 'asio-test', 'loopback-test', 'wasapi-loop') {
        if (Get-Process $p -ErrorAction SilentlyContinue) {
            Note "Quit $p first (it holds the Apollo devices open, and an open handle keeps the old driver loaded), then run this again."
            return $false
        }
    }
    return $true
}

# Defaults: saved in pass 1 (a file, so pass 2 can put them back after the power cycle).
function Save-Defaults {
    if (-not $audio) { return }
    try {
        $d = @(Get-AudioDefaults)
        Note 'Default audio devices now:'
        Format-AudioDefaults $d | ForEach-Object { Note $_ }
        ($d | ConvertTo-Json -Depth 3) | Out-File -Encoding ascii $defaultsFile
        Note "(saved to $defaultsFile for pass 2)"
    } catch { Note "note: could not read the default audio devices ($($_.Exception.Message))" }
}
function Restore-SavedDefaults([int[]]$waits) {
    if (-not $audio -or -not (Test-Path -LiteralPath $defaultsFile)) { return }
    $saved = @()
    # PS 5.1 ConvertFrom-Json returns a JSON array as ONE object: unroll it into its entries.
    try { $saved = @((Get-Content -Raw -LiteralPath $defaultsFile | ConvertFrom-Json) | ForEach-Object { $_ }) } catch { Note "note: $defaultsFile unreadable"; return }
    $moved = 0
    foreach ($w in $waits) {
        Start-Sleep -Seconds $w
        try {
            $lines = @(Restore-AudioDefaults $saved)
            foreach ($l in $lines) { Note $l }
            $moved += $lines.Count
        } catch { Note "note: could not check the default audio devices ($($_.Exception.Message))" }
    }
    if ($moved -eq 0) { Note 'Default audio devices: as saved in pass 1.' }
}

# The ASIO DLL registered in Program Files must be the current build (step-g2.ps1 explains why).
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
            Move-Item -LiteralPath $dst -Destination ("$dst.old-" + (Get-Date -Format 'yyyyMMdd-HHmmss')) -Force -ErrorAction Stop
        }
        Move-Item -LiteralPath $tmp -Destination $dst -Force -ErrorAction Stop
        $p = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\regsvr32.exe') -ArgumentList '/s', "`"$dst`"" -Wait -PassThru
        Get-ChildItem -LiteralPath (Split-Path $dst) -Filter 'avium-asio.dll.old-*' -ErrorAction SilentlyContinue |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
        Note "ASIO DLL updated and registered (regsvr32 exit $($p.ExitCode)): $dst"
        return ($p.ExitCode -eq 0)
    } catch {
        Note "ASIO DLL: could not update ($($_.Exception.Message)). Close Ableton / Avium Measure / ASIO Link Pro and run step-g3 again."
        return $false
    }
}

# Waits until the Twin X's bring-up has an outcome (Connected, FAILED, GONE). DSP ready can take 30 s, ACEFACE 20 s.
function Wait-TwinxOutcome([int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    do {
        $i = DiagQuiet @('--unit', 'twinx', 'info')
        $stage = FirstInt (Field $i 'Stage')
        Write-Host "  Twin X stage: $(Field $i 'Stage')"
        if ($stage -eq 5 -or $stage -eq 256 -or $stage -eq 512) { return $i }
        Start-Sleep -Seconds 3
    } while ((Get-Date) -lt $deadline)
    return $i
}

$monitorWarning = @(
    'BEFORE the Twin X starts on the G3 build: turn its MONITOR knob (your speakers) all the way DOWN.',
    'Nothing plays during the bring-up (the rings are silent), but the unit comes up at whatever level its',
    'firmware restores, and nothing of ours sets the monitor level (docs/spec-twinx.md 4.2).'
)

# ======================================================================================================
# -Rollback: the Twin X observes again
# ======================================================================================================
if ($Rollback) {
    $key = TwinxKey
    $was = Get-UnitMode $key
    $now = Set-UnitMode $key 0
    Note "Twin X unit key Units\$key`: Mode $(if ($null -eq $was) { '(not set)' } else { $was }) -> $now (observe)."
    Note 'It takes effect at the Twin X''s next start: power-cycle the TWIN X only (off 10 s, on); the x8p stays up.'
    Note 'Then apollo-diag --unit twinx info shows Mode 0 observe.'
    Note "All the way back to UA's driver: $(Join-Path $PSScriptRoot 'revert-unit.ps1') -Unit twinx (or the"
    Note "'Twin X back to UA' desktop shortcut), then power-cycle the Twin X."
    Done 0
}

# --- the package ---
if (-not (Test-Path -LiteralPath $inf)) { Note "No apollo.inf in $Package (freeze the G3 build there, or pass -Package)."; Done 1 }
foreach ($h in $x8pHwid, $twinxHwid) {
    if (-not (Select-String -LiteralPath $inf -SimpleMatch -Pattern $h -Quiet)) { Note "$inf does not list $h`: not a G3 package."; Done 1 }
}
$unsafe = Join-Path (Split-Path -Parent $Package) 'UNSAFE.txt'
if (Test-Path -LiteralPath $unsafe) { Note "Refusing $Package`: $(Get-Content -Raw $unsafe)"; Done 1 }
$pkgVer = ''
$m = Select-String -LiteralPath $inf -Pattern '^\s*DriverVer\s*=\s*[^,]*,\s*([0-9.]+)' | Select-Object -First 1
if ($m) { $pkgVer = $m.Matches[0].Groups[1].Value }
Note "G3 package DriverVer $pkgVer"

# --- both units present ---
$x8p = Unit $x8pHwid
$twx = Unit $twinxHwid
if (-not $x8p) { Note 'The x8p is not connected: G3 needs it up (the Twin X is daisy-chained behind it).'; Done 1 }
if (-not $twx) { Note 'The Twin X is not connected or not powered on.'; Done 1 }
Note "x8p:    $(Format-Unit $x8p)"
Note "Twin X: $(Format-Unit $twx)"
if ($x8p.Service -ne 'apollo' -or $x8p.Status -ne 'OK') { Note 'The x8p is not running on apollo.sys: install a release on it first (step-g2.ps1 run 1).'; Done 1 }

# ======================================================================================================
# -WarmTest: the warm start (docs/spec-twinx.md 7, G3 row: "disable/enable the Twin X (warm)")
# ======================================================================================================
if ($WarmTest) {
    if (-not (Quit-Blockers)) { Done 1 }
    $i = DiagQuiet @('--unit', 'twinx', 'info')
    if ((FirstInt (Field $i 'Mode')) -ne 1 -or (FirstInt (Field $i 'Stage')) -ne 5) {
        Note 'The Twin X is not Connected in Mode 1: run pass 2 first (and pass it). Nothing changed.'
        Done 1
    }
    Note ''
    Note 'WARM TEST: the Twin X driver restarts now (pnputil /restart-device). The unit stays powered, so its DSP'
    Note 'keeps running: a warm start, with the full ACEFACE while the transport is stopped. No tone is played.'
    if (-not $Yes) {
        $a = Read-Host '[Enter] restart the Twin X driver now, [Q] quit'
        if ($a -match '^[Qq]') { Note 'Nothing changed.'; Done 1 }
    }
    # A new bring-up writes a new "... start (Apollo Twin X (Gen 1), AudioExtension path" line: wait for it,
    # so the readings below are the restarted driver's, not the old state.
    $startPat = 'start \(Apollo Twin X \(Gen 1\), AudioExtension path'
    $before = ([regex]::Matches((DiagQuiet @('--unit', 'twinx', 'log')), $startPat)).Count
    $out = (& $pnputil /restart-device $twx.InstanceId 2>&1 | Out-String)
    Note ">>> pnputil /restart-device $($twx.InstanceId)"
    Note $out.TrimEnd()
    $deadline = (Get-Date).AddSeconds(30)
    do {
        Start-Sleep -Seconds 2
        $after = ([regex]::Matches((DiagQuiet @('--unit', 'twinx', 'log')), $startPat)).Count
    } while ($after -le $before -and (Get-Date) -lt $deadline)
    if ($after -le $before) { Note 'No new bring-up seen in the log within 30 s (did the restart happen? see pnputil above).' }
    $i = Wait-TwinxOutcome 90
    Diag @('--unit', 'twinx', 'info') | Out-Null
    Diag @('--unit', 'twinx', 'log') | Out-Null
    $flags = Field $i 'Flags'
    $stage = FirstInt (Field $i 'Stage')
    Note ''
    Note '=== G3 warm start ==='
    if ($flags -match 'WARM_START') { Pass 'warm start (the DSP kept running)' } else { Check "not a warm start (Flags: $flags)" }
    if ($stage -eq 5 -and $flags -match 'TRANSPORT') {
        Pass 'warm ACEFACE with the transport stopped works on Gen 1; ring connect consumed; transport running'
    } else {
        $fs = Field $i 'BringupFailStep'
        Check "warm start did not reach Connected + transport (Stage $(Field $i 'Stage'); BringupFailStep $fs)"
        if ($fs -match '^11\b') {
            Note 'The warm ACEFACE timed out with the transport stopped: the documented fallback is the boot transport on'
            Note 'warm starts too. To try it:'
            Note "  reg add HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters\Units\$(TwinxKey) /v WarmBootTransport /t REG_DWORD /d 1 /f"
            Note '  then step-g3.ps1 -WarmTest again. Or simply power-cycle the Twin X (cold start).'
        }
    }
    Diag @('--unit', 'x8p', 'info') | Out-Null
    $xi = DiagQuiet @('--unit', 'x8p', 'info')
    if ((FirstInt (Field $xi 'Stage')) -eq 5) { Pass 'x8p unchanged (Connected)' } else { Check "x8p stage: $(Field $xi 'Stage')" }
    Note ''
    Note "Summary: $script:nPass PASS, $script:nCheck CHECK. Send Claude step-g3.log."
    Done 0
}

# ======================================================================================================
# Which pass
# ======================================================================================================
$onPkg = ($x8p.Version -eq $pkgVer) -and ($twx.Service -eq 'apollo') -and ($twx.Version -eq $pkgVer)
if (-not $onPkg) {
    # ---------------------------------------------------------------- PASS 1 ----------------------------
    Note ''
    Note 'PASS 1: put the G3 package on both units (the Twin X stays in observe), set the Twin X to Mode 1, then a'
    Note 'power cycle of both units. Windows audio on the x8p stops for a few seconds; close Ableton and anything'
    Note 'using it. Your headphones on the x8p keep working after the x8p comes back.'
    if (-not (Quit-Blockers)) { Done 1 }
    if ($audio) {
        if (-not (Wait-ApolloIdle $diag -Yes:$Yes -Note ${function:Note})) { Note 'Stopped before changing anything.'; Done 1 }
    }
    if (-not $Yes) {
        $a = Read-Host '[Enter] go on, [Q] quit'
        if ($a -match '^[Qq]') { Note 'Nothing changed.'; Done 1 }
    }
    Save-Defaults

    # The Twin X observes until its power cycle: Models\99 and its own key at 0 while drivers change.
    try {
        $mkey = Join-Path $params 'Models\99'
        if (-not (Test-Path $mkey)) { New-Item -Path $mkey -Force | Out-Null }
        Set-ItemProperty -Path $mkey -Name Mode -Value 0 -Type DWord
        $key = if ($twx.Service -eq 'apollo') { TwinxKey } else { $twinxKeyG2 }
        if ($null -ne (Get-UnitMode $key)) { [void](Set-UnitMode $key 0) }
        Note "Models\99\Mode = 0, Units\$key\Mode = $(Get-UnitMode $key): the Twin X observes while the drivers change."
    } catch { Note "Could not write the Twin X's Mode ($($_.Exception.Message)). Nothing else changed."; Done 1 }

    if ($twx.Service -eq 'apollo') {
        # Both on apollo.sys: one staged package, devcon for each exact hardware ID. One shared image: the
        # new one loads when both units are gone (the power cycle).
        # install.ps1's guards, since this branch does not go through it: test signing, one device per ID.
        $opts = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control').SystemStartOptions
        if ($opts -notmatch 'TESTSIGNING') { Note "Test signing is not active (boot options: $opts). Nothing changed."; Done 1 }
        foreach ($h in @($x8pHwid, $twinxHwid)) {
            $same = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "$h*" })
            if ($same.Count -ne 1) {
                Note "$($same.Count) present devices carry $h (expected 1); devcon would move all of them. Nothing changed."
                Done 1
            }
        }
        $pinf = Join-Path $Package 'apollo.inf'
        $o = (& $pnputil /add-driver $pinf 2>&1 | Out-String); Note ">>> pnputil /add-driver $pinf"; Note $o.TrimEnd()
        $rcs = @{}
        foreach ($pair in @(@('x8p', $x8pHwid), @('Twin X', $twinxHwid))) {
            $o = (& $devcon update $pinf $pair[1] 2>&1 | Out-String)
            $rcs[$pair[0]] = $LASTEXITCODE
            Note ">>> devcon update $pinf $($pair[1])"; Note $o.TrimEnd(); Note "exit $($rcs[$pair[0]])"
        }
        foreach ($n in $rcs.Keys) {
            if ($rcs[$n] -eq 1) {
                Note "NOTE ($n): devcon says a restart is needed. Expected here: the old apollo.sys stays loaded while the other"
                Note 'unit holds it. The power cycle of BOTH units below loads the new one (no PC reboot needed).'
            } elseif ($rcs[$n] -ne 0) {
                Note "devcon failed on the $n (exit $($rcs[$n])): see above. Tell Claude. To undo nothing is needed yet:"
                Note 'the units keep running the build they had.'
                Done 1
            }
        }
        Start-Sleep -Seconds 3
        $x8p = Unit $x8pHwid
        $twx = Unit $twinxHwid
        Note "x8p now:    $(if ($x8p) { Format-Unit $x8p } else { 'not present' })"
        Note "Twin X now: $(if ($twx) { Format-Unit $twx } else { 'not present' })"
        if (-not $x8p -or -not $twx -or $x8p.Version -ne $pkgVer -or $twx.Version -ne $pkgVer -or $x8p.Service -ne 'apollo' -or $twx.Service -ne 'apollo') {
            Note 'Not both units on the G3 package now: see above. Tell Claude (step-g3.log).'
            Done 1
        }
        # A problem code is fine only as "restart pending" (devcon exit 1), which the power cycle clears.
        foreach ($pair in @(@('x8p', $x8p), @('Twin X', $twx))) {
            $u = $pair[1]
            if ($u.Status -ne 'OK' -and $rcs[$pair[0]] -ne 1) {
                Note "The $($pair[0]) reports status $($u.Status), problem $($u.Problem) after the update, and devcon did not ask"
                Note 'for a restart: see above. Tell Claude (step-g3.log). Mode 1 was NOT written.'
                Done 1
            }
        }
    } else {
        # Twin X on UA's driver: the x8p alone holds apollo.sys, so its install loads the new image at once.
        $args1 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'install.ps1'), '-Unit', 'x8p', '-Package', $Package)
        if ($Yes) { $args1 += '-Yes' }
        & $ps @args1
        Note "install.ps1 -Unit x8p exit $LASTEXITCODE (install.log has every step)"
        if ($LASTEXITCODE -ne 0) { Note 'The x8p install did not finish: tell Claude. The Twin X was not touched.'; Done 1 }
        $args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'install.ps1'), '-Unit', 'twinx', '-Package', $Package)
        if ($Yes) { $args2 += '-Yes' }
        & $ps @args2
        Note "install.ps1 -Unit twinx exit $LASTEXITCODE (Twin X in observe; install.log has every step)"
        if ($LASTEXITCODE -ne 0) { Note 'The Twin X bind did not finish cleanly: see above, tell Claude. Undo: revert-unit.ps1 -Unit twinx.'; Done 1 }
    }
    if (-not (Sync-AsioDll)) { Note 'The ASIO DLL is not current (see above): fix that before using ASIO; G3 itself goes on.' }

    # Mode 1 for THIS Twin X, read at its next start (the power cycle). Nothing restarts now.
    $key = TwinxKey
    if ($key -ne $twinxKeyG2) { Note "note: the Twin X's unit key is $key (G2 read $twinxKeyG2): using $key." }
    $mode = Set-UnitMode $key 1
    Note "Units\$key\Mode = $mode (bring-up + transport at the Twin X's next start; nothing restarted now)."
    Diag @('units') | Out-Null
    Restore-SavedDefaults @(4, 4, 12)

    Note ''
    Note 'Pass 1 done. Now:'
    foreach ($l in $monitorWarning) { Note "  $l" }
    Note '  1. Power the Twin X OFF, then the x8p OFF. Wait 10 s.'
    Note '  2. Power the x8p ON, then the Twin X ON. Wait about 60 s (the Twin X DSP-ready wait alone can take 30 s).'
    Note '  3. Run step-g3.ps1 again (pass 2). It plays nothing.'
    Done 0
}

# ---------------------------------------------------------------- PASS 2 ------------------------------
$xi = DiagQuiet @('--unit', 'x8p', 'info')
$ver = FirstInt (Field $xi 'Version')
if ($ver -lt 7) {
    Note ''
    Note "Both units carry the G3 package, but the old apollo.sys (interface $ver) is still loaded: they were not both"
    Note 'off at the same time yet.'
    foreach ($l in $monitorWarning) { Note "  $l" }
    Note '  Power the Twin X OFF, then the x8p OFF; wait 10 s; the x8p ON, then the Twin X ON; wait 60 s; run this again.'
    Note '  (If you did exactly that: quit Apollo Control and any test tool, they can keep the old driver loaded.)'
    Done 0
}
$ti = DiagQuiet @('--unit', 'twinx', 'info')
if ((FirstInt (Field $ti 'Mode')) -ne 1) {
    Note "The Twin X runs Mode '$(Field $ti 'Mode')', not 1. Its unit key Units\$(TwinxKey)\Mode is $(Get-UnitMode (TwinxKey))."
    Note 'Run pass 1 again (it sets Mode 1), or set it by hand, then power-cycle the Twin X.'
    Done 1
}
Note ''
Note 'PASS 2: the Twin X runs the G3 build in Mode 1. Waiting for its bring-up (up to 120 s)...'
$ti = Wait-TwinxOutcome 120
Start-Sleep -Seconds 2
Restore-SavedDefaults @(1)

Note ''
Note '=== G3 readings: the Twin X (docs/spec-twinx.md Appendix A) ==='
Diag @('units') | Out-Null
$ti  = Diag @('--unit', 'twinx', 'info')
$te  = Diag @('--unit', 'twinx', 'engine', '5')
Diag @('--unit', 'twinx', 'readback') | Out-Null
Diag @('--unit', 'twinx', 'levels', '2') | Out-Null
$tl  = Diag @('--unit', 'twinx', 'log')
Note '=== the x8p ==='
$xi  = Diag @('--unit', 'x8p', 'info')
$xe  = Diag @('--unit', 'x8p', 'engine', '5')
[void](Diag @('--unit', 'x8p', 'endpoints'))   # logged only
Diag @('--unit', 'x8p', 'strips') | Out-Null
Diag @('--unit', 'x8p', 'log') | Out-Null

# ---------------------------------------------------------------- PASS / CHECK -------------------------
Note ''
Note '=== G3 results (PASS = as expected; CHECK = look at it, then tell Claude) ==='
$stage = FirstInt (Field $ti 'Stage')
$flags = Field $ti 'Flags'
if ((Field $ti 'DeviceType') -match '^0x99\b' -and (Field $ti 'PublicType') -match '^0x19\b' -and (FirstInt (Field $ti 'NumDsps')) -eq 2) {
    Pass 'identity: type 0x99, public 0x19, 2 DSPs (Duo)'
} else { Check "identity: type $(Field $ti 'DeviceType'), public $(Field $ti 'PublicType'), DSPs $(Field $ti 'NumDsps')" }
if ((Field $ti 'ExtCaps') -match 'family 19') { Pass 'ext_caps family 19' } else { Check "ext_caps: $(Field $ti 'ExtCaps')" }
if ($stage -eq 5) { Pass 'Stage 5 Connected' } else {
    Check "Stage $(Field $ti 'Stage'), FailedStage $(Field $ti 'FailedStage'), LastStatus $(Field $ti 'LastStatus'), BringupFailStep $(Field $ti 'BringupFailStep')"
    $fl = [regex]::Matches($tl, '(?m)^.*G3 bring-up FAILED.*$')
    foreach ($f in $fl) { Note "       log: $($f.Value.Trim())" }
}
if ($flags -notmatch 'WARM_START') { Pass 'cold start (WarmStart 0 after the power cycle)' } else { Check 'WARM start: the Twin X was not powered off (power BOTH off, then run this again)' }
foreach ($f in 'ACEFACE_DONE', 'DSPS_CONNECTED', 'CONNECTED', 'TRANSPORT', 'SERVICE_RUNNING', 'READBACK_VALID') {
    if ($flags -match "\b$f\b") { Pass "flag $f" } else { Check "flag $f missing (Flags: $flags)" }
}
$dma = Field $ti 'DmaCtrl'
if ($dma -match '^0x00000007\b') { Pass 'DMA_CTRL 0x7 (global + 2 DSP enables, no strobe)' } else { Check "DMA_CTRL $dma (expected 0x00000007)" }
$rate = Field $ti 'NotifRateInfo'
if ($rate -match '^0x00000201\b') { Pass 'rate info 0x201: 48 kHz locked' } else { Check "rate info $rate (expected 0x00000201: 48 kHz, lock 1)" }
$play = FirstInt (Field $ti 'PlayChannels')
$rec  = FirstInt (Field $ti 'RecChannels')
if ($play -eq 46 -and $rec -eq 34) { Pass 'descriptor counts play 46 / record 34 (as UA left them at G2)' }
else { Check "descriptor counts play $play / record $rec (G2 stale: 46 / 34; the live ones decide)" }
$clk = Field $ti 'ClockSrcsLive'
if ($clk) {
    Note "INFO   clock sources, live read: $clk"
    if ($clk -match 'Peer\(0x0A\)') { Pass 'Peer clock 0x0A offered on a live read (G6 plan: Thunderbolt peer clock)' }
    else { Check 'Peer clock 0x0A NOT offered on the live read (G6 falls back to the ASRC plan)' }
} else { Check 'no live clock-source read (ClockSrcsLive missing)' }
$steps = [regex]::Matches($ti, '(?m)^\s+step\s+(\d+)\s+(ok|--)\s+(.*)$')
$e8ok = $false
foreach ($s in $steps) { if ($s.Groups[1].Value -eq '6' -and $s.Groups[2].Value -eq 'ok') { $e8ok = $true } }
foreach ($s in $steps) {
    $n = [int]$s.Groups[1].Value
    if ($s.Groups[2].Value -eq 'ok') { Pass "step $n $($s.Groups[3].Value.Trim())" }
    elseif ($n -eq 8 -and $e8ok) { Note "INFO   step 8 $($s.Groups[3].Value.Trim()): not needed (E8 answered)" }
    else { Check "step $n $($s.Groups[3].Value.Trim()) not passed" }
}
if ($steps.Count -eq 0) { Check 'no G3 step list in info (is the G3 build running?)' }
$mEng = [regex]::Match($te, 'Over\s+[\d.]+\s+s:\s+([\d.]+)\s+ticks/s,\s+([-\d.]+)\s+frames/s')
if ($mEng.Success) {
    $tps = [double]$mEng.Groups[1].Value
    $fps = [double]$mEng.Groups[2].Value
    if ($fps -gt 47900 -and $fps -lt 48100) { Pass "SAMPLE_POS at $fps frames/s (48 kHz)" } else { Check "SAMPLE_POS at $fps frames/s (expected about 48000)" }
    if ($tps -gt 950) { Pass "copy engine $tps ticks/s" } else { Check "copy engine $tps ticks/s (expected about 1000)" }
} else { Check 'engine rates not read (apollo-diag --unit twinx engine 5)' }
foreach ($l in [regex]::Matches($tl, '(?m)^.*G5: DSP \d+ (took|did NOT take).*$')) { Note "INFO   $($l.Value.Trim())" }
$mon = [regex]::Match($tl, 'MON L/R: playback slots (-?\d+)/(-?\d+), record \(monitor loopback\) slots (-?\d+)/(-?\d+)')
$monPlay = '0,1'
$monRec = '26 and 27'
if ($mon.Success) {
    $monPlay = "$($mon.Groups[1].Value),$($mon.Groups[2].Value)"
    $monRec = "$($mon.Groups[3].Value) and $($mon.Groups[4].Value)"
    if ($monPlay -eq '0,1' -and $monRec -eq '26 and 27') { Pass 'MON L/R: playback 0/1, loopback record 26/27 (as at G2)' }
    else { Check "MON L/R: playback $monPlay, loopback record $monRec (G2 stale: 0,1 / 26 and 27)" }
} else { Check 'no MON L/R line in the log' }

# The x8p: exactly as before
if ((FirstInt (Field $xi 'Stage')) -eq 5 -and (Field $xi 'Mode') -match '^2\b') { Pass 'x8p: Connected, Mode 2' } else { Check "x8p: Stage $(Field $xi 'Stage'), Mode $(Field $xi 'Mode')" }
if ((Field $xi 'Flags') -match '\bTRANSPORT\b') { Pass 'x8p: transport running' } else { Check "x8p flags: $(Field $xi 'Flags')" }
if ((Field $xi 'DmaCtrl') -match '^0x0000007F\b') { Pass 'x8p: DMA_CTRL 0x7F' } else { Check "x8p DMA_CTRL $(Field $xi 'DmaCtrl')" }
if ((FirstInt (Field $xi 'PlayChannels')) -eq 34 -and (FirstInt (Field $xi 'RecChannels')) -eq 32) { Pass 'x8p: legacy layout 34 / 32' } else { Check 'x8p: layout changed' }
if ((Field $xi 'BringupSteps') -eq '') { Pass 'x8p: no G3 step record (its own path ran)' } else { Check 'x8p shows G3 steps: tell Claude' }
$xm = [regex]::Match($xe, 'Over\s+[\d.]+\s+s:\s+([\d.]+)\s+ticks/s,\s+([-\d.]+)\s+frames/s')
if ($xm.Success -and [double]$xm.Groups[2].Value -gt 47900 -and [double]$xm.Groups[2].Value -lt 48100) { Pass "x8p: $($xm.Groups[2].Value) frames/s" } else { Check 'x8p engine rate (see above)' }

Note ''
Note "Summary: $script:nPass PASS, $script:nCheck CHECK. Send Claude step-g3.log."
Note ''
if ($stage -ne 5 -or $flags -notmatch '\bTRANSPORT\b') {
    Note 'The Twin X is not running its transport, so there is no tone test. It is idle on our driver (the log line'
    Note '"G3 bring-up FAILED at step ..." says where it stopped). Send Claude step-g3.log. Meanwhile:'
    Note '  step-g3.ps1 -Rollback, then power-cycle the Twin X: observe again (silent, as since G2), or'
    Note '  revert-unit.ps1 -Unit twinx (the desktop shortcut), then power-cycle it: back on UA, speakers work.'
    Done 0
}
Note 'TONE TEST (by hand; this script plays nothing). First turn the Twin X monitor knob DOWN, then up a little.'
Note "  Window 1, elevated:  `"$diag`" --unit twinx tone 5"
Note "                       (1 kHz, -20 dBFS, on MON L/R = playback slots $monPlay from the live descriptor; at most 10 s)"
Note "  Window 2, normal:    `"$diag`" --unit twinx levels 5"
Note "                       expect about -20.0 dBFS on record slots $monRec while the tone plays (the monitor loopback)"
Note 'Then the warm start: step-g3.ps1 -WarmTest (restarts the Twin X driver, plays nothing).'
Note 'Way back: step-g3.ps1 -Rollback (observe after a Twin X power cycle), or revert-unit.ps1 -Unit twinx (UA).'
Done 0
