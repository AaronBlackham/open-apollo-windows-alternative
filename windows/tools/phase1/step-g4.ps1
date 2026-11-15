# Twin X gate G4 "speakers on our driver" (docs/spec-twinx.md section 7 and 6.9.4). ADMIN PowerShell.
#   powershell -ExecutionPolicy Bypass -File C:\Users\aaron\Desktop\Avium\apollo-win\tools\phase1\step-g4.ps1 [-Package <folder>] [-Yes]
#   ... -Restart    pass 1 restarts both units' drivers itself (warm, no power cycle) and goes on to pass 2
#   ... -Rollback   the Twin X back to Mode 1 (G3: bring-up + transport, no Windows endpoints)
# Default package: builds\2026-11-14-g4\package (the frozen G4 build).
#
# G4 = the Twin X in Mode 2: its own Windows endpoints "MON L/R (Apollo Twin X ...)" out (its monitor
# outputs: the speakers) and in (the monitor-mix loopback), and the monitor settings that apply to it.
# No ASIO (still the x8p's), no Twin X strips (G5). The x8p runs exactly as before.
#   Pass 1 (either unit not on this package, or the Twin X not in Mode 2 yet). Quit Apollo Control first.
#     - Waits until the x8p is idle, saves the Windows default audio devices to step-g4.defaults.json.
#     - Both units on apollo.sys (since G2/G3): stages the package and updates BOTH units with devcon for
#       their exact four-part hardware IDs (x8p, then Twin X), with the install guards (test signing, one
#       device per ID). Twin X still on UA's driver: install.ps1 -Unit x8p, then install.ps1 -Unit twinx.
#     - Writes Parameters\Units\<Twin X key>\Mode = 2 and EnabledEndpoints = 3, read at the Twin X's next
#       start, puts the saved defaults back, then asks you to power BOTH units off and on: the x8p's strips
#       need its cold start, and the new apollo.sys loads only when both units have let go of the old one.
#       Order: Twin X off, then x8p off; wait 10 s; x8p on, then Twin X on.
#     - With -Restart: instead of the power cycle, disables both devices (Twin X, then x8p) and enables them
#       again (x8p, then Twin X): warm starts on the new image at once. The x8p's input strips stay off
#       until its next power cycle (Windows audio and its MON outputs work). Then pass 2 runs.
#   Pass 2 (both units on this package, the Twin X running Mode 2): waits for the Twin X bring-up, checks
#     both units (Twin X Mode 2 Connected, its two endpoints present with their Windows names; the x8p
#     unchanged with its 10 endpoints and its strips), puts the saved defaults back (Windows makes a new
#     endpoint the default when it first appears), prints PASS / CHECK, and then PRINTS (does not run) the
#     30-minute WASAPI loop command for the Twin X.
# THIS SCRIPT NEVER PLAYS SOUND. The loop test it prints plays a tone into the speakers.
# The way back: step-g4.ps1 -Rollback (Mode 1 at the Twin X's next start), or the whole unit back to UA:
# revert-unit.ps1 -Unit twinx (the "Twin X back to UA" shortcut).
# PowerShell 5.1, ASCII only.
#Requires -RunAsAdministrator
param([string]$Package = '', [switch]$Yes, [switch]$Restart, [switch]$Rollback)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Package) { $Package = Join-Path $root 'builds\2026-11-14-g4\package' }
$inf  = Join-Path $Package 'apollo.inf'
$log  = Join-Path $PSScriptRoot 'step-g4.log'
$defaultsFile = Join-Path $PSScriptRoot 'step-g4.defaults.json'
"step-g4 $(Get-Date -Format o) package $Package Restart=$Restart Rollback=$Rollback" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }
function Done([int]$rc) { Note "step-g4 exit $rc. Log: $log"; if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }; exit $rc }

$x8pHwid   = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00'
$twinxHwid = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00'
$params    = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters'
$twinxKeyG2 = '000000002F00BEEF'      # the Twin X's HW_UID read at gate G2 (Units\<key>); checked against the unit
$twinxEndpointsDefault = 3            # driver\bringup_plan.h APOLLO_TWX_EP_DEFAULT: MON L/R out + in
$ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
$script:nPass = 0
$script:nCheck = 0
function Pass([string]$s) { $script:nPass++; Note "PASS   $s" }
function Check([string]$s) { $script:nCheck++; Note "CHECK  $s" }

# --- tools: apollo-diag and devcon only from the verified admin-only copies (safe-tools.ps1) ---
. (Join-Path $PSScriptRoot 'safe-tools.ps1')
$tools = Initialize-SafeTools $root @('tools\bin\devcon.exe', 'tools\apollo-diag\apollo-diag.exe') ${function:Note}
if (-not $tools) { Note 'Stopped before changing anything.'; Done 1 }
$devcon = $tools['tools\bin\devcon.exe']
$diag   = $tools['tools\apollo-diag\apollo-diag.exe']
$loop   = Join-Path $root 'tools\wasapi-loop\wasapi-loop.exe'
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
function Set-UnitValue([string]$key, [string]$name, [int]$value) {
    $k = Join-Path $params "Units\$key"
    if (-not (Test-Path $k)) { New-Item -Path $k -Force | Out-Null }
    Set-ItemProperty -Path $k -Name $name -Value $value -Type DWord
    return (Get-ItemProperty $k).$name
}
function Get-UnitValue([string]$key, [string]$name) {
    $k = Join-Path $params "Units\$key"
    if (-not (Test-Path $k)) { return $null }
    return (Get-ItemProperty $k -ErrorAction SilentlyContinue).$name
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
        Note "ASIO DLL: could not update ($($_.Exception.Message)). Close Ableton / Avium Measure / ASIO Link Pro and run step-g4 again."
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

# -Restart: both devices off at the same time (so the shared apollo.sys image is released), then on again.
function Restart-BothUnits($x8p, $twx) {
    foreach ($u in @($twx, $x8p)) {
        $o = (& $pnputil /disable-device $u.InstanceId 2>&1 | Out-String)
        Note ">>> pnputil /disable-device $($u.InstanceId)"; Note $o.TrimEnd()
    }
    Start-Sleep -Seconds 3
    foreach ($u in @($x8p, $twx)) {
        $o = (& $pnputil /enable-device $u.InstanceId 2>&1 | Out-String)
        Note ">>> pnputil /enable-device $($u.InstanceId)"; Note $o.TrimEnd()
        Start-Sleep -Seconds 2
    }
}

$monitorNote = @(
    'Nothing plays while the Twin X starts (its rings are silent), and the driver never writes the monitor level at',
    'connect (docs/spec-twinx.md 4.2): the speakers come up at whatever level the Twin X knob is at. From pass 2 on,',
    'Windows audio sent to "MON L/R (Apollo Twin X ...)" plays on the speakers.'
)

# ======================================================================================================
# -Rollback: the Twin X back to Mode 1 (G3)
# ======================================================================================================
if ($Rollback) {
    $key = TwinxKey
    $was = Get-UnitValue $key 'Mode'
    $now = Set-UnitValue $key 'Mode' 1
    Note "Twin X unit key Units\$key`: Mode $(if ($null -eq $was) { '(not set)' } else { $was }) -> $now (bring-up + transport, no endpoints)."
    Note 'It takes effect at the Twin X''s next start: power-cycle the TWIN X only (off 10 s, on), or restart its driver:'
    $twx = Unit $twinxHwid
    if ($twx) { Note "  pnputil /restart-device `"$($twx.InstanceId)`"" }
    Note 'Its two Windows endpoints go away then; Windows picks another default if one of them was the default.'
    Note "All the way back to UA's driver: $(Join-Path $PSScriptRoot 'revert-unit.ps1') -Unit twinx (or the"
    Note "'Twin X back to UA' desktop shortcut), then power-cycle the Twin X."
    Done 0
}

# --- the package ---
if (-not (Test-Path -LiteralPath $inf)) { Note "No apollo.inf in $Package (freeze the G4 build there, or pass -Package)."; Done 1 }
foreach ($h in $x8pHwid, $twinxHwid) {
    if (-not (Select-String -LiteralPath $inf -SimpleMatch -Pattern $h -Quiet)) { Note "$inf does not list $h`: not a G4 package."; Done 1 }
}
if (-not (Select-String -LiteralPath $inf -SimpleMatch -Pattern 'ApolloTwxMon' -Quiet)) {
    Note "$inf has no Twin X endpoint interfaces (ApolloTwxMon): a G3 package, not G4."; Done 1
}
$unsafe = Join-Path (Split-Path -Parent $Package) 'UNSAFE.txt'
if (Test-Path -LiteralPath $unsafe) { Note "Refusing $Package`: $(Get-Content -Raw $unsafe)"; Done 1 }
$pkgVer = ''
$m = Select-String -LiteralPath $inf -Pattern '^\s*DriverVer\s*=\s*[^,]*,\s*([0-9.]+)' | Select-Object -First 1
if ($m) { $pkgVer = $m.Matches[0].Groups[1].Value }
Note "G4 package DriverVer $pkgVer"

# --- both units present ---
$x8p = Unit $x8pHwid
$twx = Unit $twinxHwid
if (-not $x8p) { Note 'The x8p is not connected: G4 needs it up (the Twin X is daisy-chained behind it).'; Done 1 }
if (-not $twx) { Note 'The Twin X is not connected or not powered on.'; Done 1 }
Note "x8p:    $(Format-Unit $x8p)"
Note "Twin X: $(Format-Unit $twx)"
if ($x8p.Service -ne 'apollo' -or $x8p.Status -ne 'OK') { Note 'The x8p is not running on apollo.sys: install a release on it first (step-g2.ps1 run 1).'; Done 1 }

# ======================================================================================================
# Which pass: pass 2 needs both units on the package AND the new image running the Twin X in Mode 2
# (the G3 image runs a Mode 2 key as Mode 1, so "Mode 2" in GET_INFO proves the G4 apollo.sys is loaded).
# ======================================================================================================
$onPkg = ($x8p.Version -eq $pkgVer) -and ($twx.Service -eq 'apollo') -and ($twx.Version -eq $pkgVer)
$ti0 = DiagQuiet @('--unit', 'twinx', 'info')
$runsMode2 = ((FirstInt (Field $ti0 'Mode')) -eq 2)
if (-not ($onPkg -and $runsMode2)) {
    # ---------------------------------------------------------------- PASS 1 ----------------------------
    # Not on the package: install it. On it, but the Twin X not in Mode 2 yet (the G3 image is still
    # loaded, or the Mode was never written): only the Mode, then the restart. Running it twice is harmless.
    Note ''
    if (-not $onPkg) {
        Note 'PASS 1: put the G4 package on both units, set the Twin X to Mode 2 (its own Windows endpoints), then a'
        Note 'power cycle of both units (or -Restart). Windows audio on the x8p stops for a few seconds; close'
        Note 'Ableton and anything using it.'
    } else {
        Note 'PASS 1: both units carry the G4 package, but the Twin X does not run Mode 2 yet: Mode 2 is written and'
        Note 'both drivers must restart (power cycle, or -Restart) so the new apollo.sys loads.'
    }
    if (-not (Quit-Blockers)) { Done 1 }
    if ($audio) {
        if (-not (Wait-ApolloIdle $diag -Yes:$Yes -Note ${function:Note})) { Note 'Stopped before changing anything.'; Done 1 }
    }
    if (-not $Yes) {
        $a = Read-Host '[Enter] go on, [Q] quit'
        if ($a -match '^[Qq]') { Note 'Nothing changed.'; Done 1 }
    }
    Save-Defaults

    if (-not $onPkg) {
        if ($twx.Service -eq 'apollo') {
            # Both on apollo.sys: one staged package, devcon for each exact hardware ID. One shared image: the
            # new one loads when both units are gone (the power cycle, or -Restart).
            $opts = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control').SystemStartOptions
            if ($opts -notmatch 'TESTSIGNING') { Note "Test signing is not active (boot options: $opts). Nothing changed."; Done 1 }
            foreach ($h in @($x8pHwid, $twinxHwid)) {
                $same = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "$h*" })
                if ($same.Count -ne 1) {
                    Note "$($same.Count) present devices carry $h (expected 1); devcon would move all of them. Nothing changed."
                    Done 1
                }
            }
            $o = (& $pnputil /add-driver $inf 2>&1 | Out-String); Note ">>> pnputil /add-driver $inf"; Note $o.TrimEnd()
            $rcs = @{}
            foreach ($pair in @(@('x8p', $x8pHwid), @('Twin X', $twinxHwid))) {
                $o = (& $devcon update $inf $pair[1] 2>&1 | Out-String)
                $rcs[$pair[0]] = $LASTEXITCODE
                Note ">>> devcon update $inf $($pair[1])"; Note $o.TrimEnd(); Note "exit $($rcs[$pair[0]])"
            }
            foreach ($n in $rcs.Keys) {
                if ($rcs[$n] -eq 1) {
                    Note "NOTE ($n): devcon says a restart is needed. Expected here: the old apollo.sys stays loaded while the other"
                    Note 'unit holds it. The power cycle of BOTH units (or -Restart) loads the new one (no PC reboot needed).'
                } elseif ($rcs[$n] -ne 0) {
                    Note "devcon failed on the $n (exit $($rcs[$n])): see above. Tell Claude. Nothing else changed:"
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
                Note 'Not both units on the G4 package now: see above. Tell Claude (step-g4.log). Mode 2 was NOT written.'
                Done 1
            }
            foreach ($pair in @(@('x8p', $x8p), @('Twin X', $twx))) {
                $u = $pair[1]
                if ($u.Status -ne 'OK' -and $rcs[$pair[0]] -ne 1) {
                    Note "The $($pair[0]) reports status $($u.Status), problem $($u.Problem) after the update, and devcon did not ask"
                    Note 'for a restart: see above. Tell Claude (step-g4.log). Mode 2 was NOT written.'
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
        if (-not (Sync-AsioDll)) { Note 'The ASIO DLL is not current (see above): fix that before using ASIO; G4 itself goes on.' }
    }

    # Mode 2 + its endpoint set for THIS Twin X, read at its next start. Nothing restarts here.
    $key = TwinxKey
    if ($key -ne $twinxKeyG2) { Note "note: the Twin X's unit key is $key (G2 read $twinxKeyG2): using $key." }
    try {
        $mode = Set-UnitValue $key 'Mode' 2
        $eps = Set-UnitValue $key 'EnabledEndpoints' $twinxEndpointsDefault
    } catch { Note "Could not write Units\$key ($($_.Exception.Message)). Tell Claude."; Done 1 }
    Note "Units\$key\Mode = $mode, EnabledEndpoints = $eps (MON L/R out + in), read at the Twin X's next start."
    Diag @('units') | Out-Null

    if (-not $Restart) {
        Restore-SavedDefaults @(4, 4, 12)
        Note ''
        Note 'Pass 1 done. Now:'
        foreach ($l in $monitorNote) { Note "  $l" }
        Note '  1. Power the Twin X OFF, then the x8p OFF. Wait 10 s.'
        Note '  2. Power the x8p ON, then the Twin X ON. Wait about 60 s (the Twin X DSP-ready wait alone can take 30 s).'
        Note '  3. Run step-g4.ps1 again (pass 2). It plays nothing.'
        Note '  (Or, without the power cycle: step-g4.ps1 -Restart does a warm restart of both drivers; the x8p''s'
        Note '   input strips then stay off until its next power cycle.)'
        Done 0
    }
    Note ''
    Note 'Restarting both drivers now (warm; the x8p''s input strips stay off until its next power cycle).'
    Restart-BothUnits $x8p $twx
    Start-Sleep -Seconds 5
    $x8p = Unit $x8pHwid
    $twx = Unit $twinxHwid
}

# ---------------------------------------------------------------- PASS 2 ------------------------------
$ti = DiagQuiet @('--unit', 'twinx', 'info')
if ((FirstInt (Field $ti 'Mode')) -ne 2) {
    Note ''
    Note "The Twin X runs Mode '$(Field $ti 'Mode')', not 2 (its unit key: Mode $(Get-UnitValue (TwinxKey) 'Mode'))."
    Note 'Either the old apollo.sys (G3: it runs a Mode 2 key as Mode 1) is still loaded because the units were not'
    Note 'both off at the same time, or the Mode was not written. Power the Twin X OFF, then the x8p OFF; wait 10 s;'
    Note 'x8p ON, then Twin X ON; wait 60 s; run this again (or step-g4.ps1 -Restart). Quit Apollo Control and any'
    Note 'test tool first: they keep the old driver loaded.'
    Done 1
}
Note ''
Note 'PASS 2: the Twin X runs the G4 build in Mode 2. Waiting for its bring-up (up to 120 s)...'
$ti = Wait-TwinxOutcome 120
Start-Sleep -Seconds 3
Restore-SavedDefaults @(1, 3)
# One use only: a later pass 2 (a re-check after a power cycle) must not undo a default the user chose since.
if (Test-Path -LiteralPath $defaultsFile) {
    Move-Item -LiteralPath $defaultsFile -Destination "$defaultsFile.used" -Force
    Note "(saved defaults used once; kept as $defaultsFile.used)"
}

Note ''
Note '=== G4 readings: the Twin X ==='
Diag @('units') | Out-Null
$ti  = Diag @('--unit', 'twinx', 'info')
$te  = Diag @('--unit', 'twinx', 'engine', '5')
$tep = Diag @('--unit', 'twinx', 'endpoints')
Diag @('--unit', 'twinx', 'readback') | Out-Null
$tl  = Diag @('--unit', 'twinx', 'log')
Note '=== the x8p ==='
$xi  = Diag @('--unit', 'x8p', 'info')
$xe  = Diag @('--unit', 'x8p', 'engine', '5')
$xep = Diag @('--unit', 'x8p', 'endpoints')
$xs  = Diag @('--unit', 'x8p', 'strips')
Diag @('--unit', 'x8p', 'log') | Out-Null
$winEps = @()
if ($audio) {
    try { $winEps = @(Get-AudioEndpoints) } catch { Note "note: could not list the Windows endpoints ($($_.Exception.Message))" }
    Note '=== Windows audio endpoints of the Apollos ==='
    foreach ($e in $winEps) { if ($e.Name -like '*(Apollo*') { Note ("  {0} {1}" -f $(if ($e.Flow -eq 0) { 'out' } else { 'in ' }), $e.Name) } }
    Note 'Default audio devices now:'
    Format-AudioDefaults @(Get-AudioDefaults) | ForEach-Object { Note $_ }
}

# ---------------------------------------------------------------- PASS / CHECK -------------------------
Note ''
Note '=== G4 results (PASS = as expected; CHECK = look at it, then tell Claude) ==='
$stage = FirstInt (Field $ti 'Stage')
$flags = Field $ti 'Flags'
if ((Field $ti 'DeviceType') -match '^0x99\b' -and (FirstInt (Field $ti 'NumDsps')) -eq 2) { Pass 'Twin X identity: type 0x99, 2 DSPs' }
else { Check "Twin X identity: type $(Field $ti 'DeviceType'), DSPs $(Field $ti 'NumDsps')" }
if ((Field $ti 'Mode') -match '^2\b') { Pass 'Twin X Mode 2 (full: endpoints + monitor settings)' } else { Check "Twin X Mode $(Field $ti 'Mode')" }
if ($stage -eq 5) { Pass 'Twin X Stage 5 Connected' } else {
    Check "Twin X Stage $(Field $ti 'Stage'), FailedStage $(Field $ti 'FailedStage'), LastStatus $(Field $ti 'LastStatus'), BringupFailStep $(Field $ti 'BringupFailStep')"
    foreach ($f in [regex]::Matches($tl, '(?m)^.*G3 bring-up FAILED.*$')) { Note "       log: $($f.Value.Trim())" }
}
foreach ($f in 'ACEFACE_DONE', 'DSPS_CONNECTED', 'CONNECTED', 'TRANSPORT', 'SERVICE_RUNNING', 'READBACK_VALID') {
    if ($flags -match "\b$f\b") { Pass "Twin X flag $f" } else { Check "Twin X flag $f missing (Flags: $flags)" }
}
$rate = Field $ti 'NotifRateInfo'
if ($rate -match '^0x00000201\b') { Pass 'Twin X rate info 0x201: 48 kHz locked' } else { Check "Twin X rate info $rate (expected 0x00000201)" }
if ((FirstInt (Field $ti 'PlayChannels')) -eq 46 -and (FirstInt (Field $ti 'RecChannels')) -eq 34) { Pass 'Twin X descriptor counts play 46 / record 34' }
else { Check "Twin X descriptor counts play $(Field $ti 'PlayChannels') / record $(Field $ti 'RecChannels') (G3: 46 / 34)" }
# Steps: ok / n/a (not on this start's path) / -- (not passed)
$steps = [regex]::Matches($ti, '(?m)^\s+step\s+(\d+)\s+(ok|--|n/a)\s+(.*)$')
$na = 0
foreach ($s in $steps) {
    $n = [int]$s.Groups[1].Value
    switch ($s.Groups[2].Value) {
        'ok'  { Pass "step $n $($s.Groups[3].Value.Trim())" }
        'n/a' { $na++ }
        default { Check "step $n $($s.Groups[3].Value.Trim()) not passed" }
    }
}
if ($steps.Count -eq 0) { Check 'no step list in Twin X info (is the G4 build running?)' }
elseif ($na -gt 0) { Note "INFO   $na step(s) n/a on this start's path ($(if ($flags -match 'WARM_START') { 'warm start: no E1-E3, no boot transport' } else { 'cold start' }))" }
foreach ($l in [regex]::Matches($tl, '(?m)^.*\] rate(:| ack).*$')) { Note "INFO   $($l.Value.Trim())" }
$mEng = [regex]::Match($te, 'Over\s+[\d.]+\s+s:\s+([\d.]+)\s+ticks/s,\s+([-\d.]+)\s+frames/s')
if ($mEng.Success) {
    $fps = [double]$mEng.Groups[2].Value
    if ($fps -gt 47900 -and $fps -lt 48100) { Pass "Twin X SAMPLE_POS at $fps frames/s" } else { Check "Twin X SAMPLE_POS at $fps frames/s (expected about 48000)" }
    if ([double]$mEng.Groups[1].Value -gt 950) { Pass "Twin X copy engine $($mEng.Groups[1].Value) ticks/s" } else { Check "Twin X engine $($mEng.Groups[1].Value) ticks/s" }
} else { Check 'Twin X engine rates not read' }
# Its two circuits, added (driver side)
if ($tep -match 'Endpoints\s+2 in the driver''s table, EnabledEndpoints 0x3\b') { Pass 'Twin X driver table: 2 endpoints, EnabledEndpoints 0x3' }
else { Check "Twin X endpoint table: $((Field $tep 'Endpoints'))" }
foreach ($c in 'ApolloTwxMon ', 'ApolloTwxMonIn') {
    $row = [regex]::Match($tep, "(?m)^\s+\d+\s+(out|in)\s+$([regex]::Escape($c.Trim()))\s+.*$")
    if ($row.Success -and $row.Value -notmatch 'not added|disabled') { Pass "Twin X circuit $($c.Trim()) added" } else { Check "Twin X circuit $($c.Trim()): $($row.Value.Trim())" }
}
# Their Windows names
$twRender = @($winEps | Where-Object { $_.Flow -eq 0 -and $_.Name -like '*(Apollo Twin X*' })
$twCap    = @($winEps | Where-Object { $_.Flow -eq 1 -and $_.Name -like '*(Apollo Twin X*' })
$renderMatch = 'MON L/R (Apollo Twin X'
$captureMatch = 'MON L/R (Apollo Twin X'
if ($twRender.Count -eq 1 -and $twRender[0].Name -like 'MON L/R (Apollo Twin X*') { Pass "Windows playback endpoint: $($twRender[0].Name)" }
elseif ($twRender.Count -ge 1) {
    Check "Windows playback endpoint(s) of the Twin X: $(($twRender | ForEach-Object { $_.Name }) -join '; ') (expected 'MON L/R (Apollo Twin X ...)')"
    $renderMatch = '(Apollo Twin X'
} else { Check 'no Windows playback endpoint of the Twin X' }
if ($twCap.Count -eq 1 -and $twCap[0].Name -like 'MON L/R (Apollo Twin X*') { Pass "Windows recording endpoint: $($twCap[0].Name)" }
elseif ($twCap.Count -ge 1) { Check "Windows recording endpoint(s) of the Twin X: $(($twCap | ForEach-Object { $_.Name }) -join '; ')" }
else { Check 'no Windows recording endpoint of the Twin X (the monitor loopback)' }

# The x8p: exactly as before
if ((FirstInt (Field $xi 'Stage')) -eq 5 -and (Field $xi 'Mode') -match '^2\b') { Pass 'x8p: Connected, Mode 2' } else { Check "x8p: Stage $(Field $xi 'Stage'), Mode $(Field $xi 'Mode')" }
if ((Field $xi 'Flags') -match '\bTRANSPORT\b') { Pass 'x8p: transport running' } else { Check "x8p flags: $(Field $xi 'Flags')" }
if ((Field $xi 'DmaCtrl') -match '^0x0000007F\b') { Pass 'x8p: DMA_CTRL 0x7F' } else { Check "x8p DMA_CTRL $(Field $xi 'DmaCtrl')" }
if ((FirstInt (Field $xi 'PlayChannels')) -eq 34 -and (FirstInt (Field $xi 'RecChannels')) -eq 32) { Pass 'x8p: legacy layout 34 / 32' } else { Check 'x8p: layout changed' }
if ((Field $xi 'BringupSteps') -eq '') { Pass 'x8p: no step record (its own path ran)' } else { Check 'x8p shows bring-up steps: tell Claude' }
$xm = [regex]::Match($xe, 'Over\s+[\d.]+\s+s:\s+([\d.]+)\s+ticks/s,\s+([-\d.]+)\s+frames/s')
if ($xm.Success -and [double]$xm.Groups[2].Value -gt 47900 -and [double]$xm.Groups[2].Value -lt 48100) { Pass "x8p: $($xm.Groups[2].Value) frames/s" } else { Check 'x8p engine rate (see above)' }
if ($xep -match 'Endpoints\s+10 in the driver''s table, EnabledEndpoints 0x3FF\b') { Pass 'x8p: 10 endpoints, EnabledEndpoints 0x3FF' } else { Check "x8p endpoints: $(Field $xep 'Endpoints')" }
$notAdded = @([regex]::Matches($xep, '(?m)^\s+\d+\s+(out|in)\s+Apollo\S+\s+.*(not added|disabled).*$')).Count
if ($notAdded -eq 0) { Pass 'x8p: every circuit added' } else { Check "x8p: $notAdded circuit(s) not added" }
$x8pWin = @($winEps | Where-Object { $_.Name -like '*(Apollo x8p*' })
if ($x8pWin.Count -eq 10) { Pass 'x8p: 10 Windows endpoints (1 out, 9 in)' } elseif ($audio) { Check "x8p: $($x8pWin.Count) Windows endpoints (expected 10)" }
if ((Field $xs 'State') -match '\bLive\b') { Pass 'x8p strips: Live' }
else { Check "x8p strips: $(Field $xs 'State') (a warm restart of the x8p leaves them off until its next power cycle)" }

Note ''
Note "Summary: $script:nPass PASS, $script:nCheck CHECK. Send Claude step-g4.log."
Note ''
if ($stage -ne 5 -or $flags -notmatch '\bTRANSPORT\b' -or $twRender.Count -eq 0) {
    Note 'The Twin X is not playing Windows audio yet (see the CHECK lines), so no loop test. Send Claude step-g4.log.'
    Note 'Meanwhile: step-g4.ps1 -Rollback (Mode 1, no endpoints, after a Twin X restart), or revert-unit.ps1 -Unit twinx (UA).'
    Done 0
}
Note '30-MINUTE GLITCH TEST (by hand; this script plays nothing).'
Note '  WARNING: it plays a continuous 997 Hz tone at -12 dBFS into the Twin X MON outputs, i.e. THE SPEAKERS, for'
Note '  30 minutes. Turn the Twin X monitor knob down (or mute the speakers) first; the test reads the monitor-mix'
Note '  loopback, so it does not depend on the knob.'
Note "  `"$loop`" 1800 --render `"$renderMatch`" --capture `"$captureMatch`""
Note '  Exit 0 = clean; 1 = glitches; 2 = setup error; 3 = no signal came back. Normal user rights are enough.'
Note 'To make the Twin X your Windows output: Sound settings -> Output -> "MON L/R (Apollo Twin X ...)".'
Note 'Way back: step-g4.ps1 -Rollback (Mode 1), or revert-unit.ps1 -Unit twinx (UA).'
Done 0
