# Gate G7 "combined Windows endpoints" (docs/spec-twinx.md 6.7 and 6.9.7). ADMIN PowerShell.
#   powershell -ExecutionPolicy Bypass -File C:\apollo-win\tools\phase1\step-g7.ps1 [-Yes]
#   ... -Restart    pass 1 restarts both drivers itself (warm, no power cycle) and goes on to pass 2
#   ... -NoAudio    pass 2 checks the endpoints only, plays nothing
#   ... -LossTest   pass 2 also plays Windows audio for 90 s while you power the Twin X off and on
#   ... -Rollback   back to each unit's own (standalone) endpoints: EndpointSet 0, then a restart
# It INSTALLS NOTHING: the G7 build must already be on both units (the merged build, after the reboot).
#
# G7 = one set of Windows endpoints for both units, on the x8p (the host: the Twin X is daisy-chained behind
# it), following the combined device's configuration exactly like the combined ASIO driver "Apollo (Avium)":
#   "Monitor"           out: Windows audio to the configuration's "windows" routes (Studio: the Twin X MON
#                       outputs, the speakers; no route: the clock master's MON), summed with the DAW there
#   "Monitor mix"       in:  the monitor-mix loopback of that unit (never a default device)
#   "x8p MIC/LINE 1-8"  in:  the x8p's preamps (through the resampler when the Twin X is the clock master)
#   "TwinX MIC/LINE 1-2" in: only with Parameters\Aggregate\EndpointMask bits 10-11 (G5 not passed yet)
# The x8p's and the Twin X's own endpoints ("MON L/R (Apollo x8p ...)", "MIC/LINE n (Apollo x8p ...)",
# "MON L/R (Apollo Twin X ...)") go away while the combined set is on; -Rollback brings them back.
#
#   Pass 1 (the combined endpoints are not there yet). Quit Apollo Control first.
#     - Checks: both units on apollo.sys with a G7 package (its INF has ApolloAggMon), the Twin X in Mode 2.
#     - Saves the Windows default audio devices to step-g7.defaults.json.
#     - Writes Parameters\Aggregate\EndpointSet = 1 (read when a unit's driver starts).
#     - Then: power the Twin X off, then the x8p; x8p on, then the Twin X (cold: the x8p's strips start, so its
#       MIC/LINE rows carry signal), and run this again. -Restart: disables and enables both drivers instead
#       (warm, at once; the x8p's strips then stay off until its next power cycle) and goes on to pass 2.
#   Pass 2 (the combined endpoints exist).
#     - Waits for both bring-ups, puts the saved defaults back (Windows makes new endpoints the default when
#       they first appear). A saved default that was one of the units' own endpoints, now gone, moves to its
#       combined counterpart (MON L/R -> Monitor, MIC/LINE n of the x8p -> x8p MIC/LINE n).
#     - Checks the driver side (12-row combined table on the x8p, none on the Twin X, the aggregate running
#       with the Twin X as timeline) and the Windows side (names, one Monitor out, no standalone left).
#     - Audio (not with -NoAudio), about 50 s in all, PLAYS INTO THE SPEAKERS:
#         a0) 3 s meter of "Monitor mix" with nothing playing: other signal there (live input
#            strips, the talkback mic) makes a) read small steady errors, which are then no glitch;
#         a) 20 s wasapi-loop through "Monitor" -> "Monitor mix": Windows audio through the combined endpoints,
#            sample-checked for glitches;
#         b) the routing set to "none" for the test (DAW pairs unrouted, Windows on the master's MON; your
#            configuration is saved first and put back after), then a 30 s 997 Hz Windows tone into "Monitor"
#            while asio-test --agg 20 plays 1 kHz into both units' MON through "Apollo (Avium)": the Twin X
#            monitor-mix loopback must carry BOTH tones (Windows and the DAW sum), the x8p's only the DAW's.
#     - Prints PASS / CHECK, puts the defaults back once more.
# PowerShell 5.1, ASCII only.
#Requires -RunAsAdministrator
param([switch]$Yes, [switch]$Restart, [switch]$NoAudio, [switch]$LossTest, [switch]$Rollback)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$log  = Join-Path $PSScriptRoot 'step-g7.log'
$defaultsFile = Join-Path $PSScriptRoot 'step-g7.defaults.json'
"step-g7 $(Get-Date -Format o) Restart=$Restart NoAudio=$NoAudio LossTest=$LossTest Rollback=$Rollback" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }
function Done([int]$rc) { Note "step-g7 exit $rc. Log: $log"; if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }; exit $rc }

$x8pHwid   = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00'
$twinxHwid = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00'
$aggKey    = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters\Aggregate'
$pnputil   = Join-Path $env:SystemRoot 'System32\pnputil.exe'
$script:nPass = 0
$script:nCheck = 0
function Pass([string]$s) { $script:nPass++; Note "PASS   $s" }
function Check([string]$s) { $script:nCheck++; Note "CHECK  $s" }

# --- tools: only the verified admin-only copies (safe-tools.ps1); asio-test loads avium-asio.dll beside it ---
. (Join-Path $PSScriptRoot 'safe-tools.ps1')
$need = @('tools\bin\devcon.exe', 'tools\apollo-diag\apollo-diag.exe', 'tools\agg-test\agg-test.exe',
          'tools\wasapi-loop\wasapi-loop.exe', 'asio\bin\asio-test.exe', 'asio\bin\avium-asio.dll')
$tools = Initialize-SafeTools $root $need ${function:Note}
if (-not $tools) { Note 'Stopped before changing anything (rebuild with tools\build-all.cmd if a tool is missing).'; Done 1 }
$diag = $tools['tools\apollo-diag\apollo-diag.exe']
$aggt = $tools['tools\agg-test\agg-test.exe']
$loop = $tools['tools\wasapi-loop\wasapi-loop.exe']
$asio = $tools['asio\bin\asio-test.exe']
$audio = $true
try { . (Join-Path $PSScriptRoot 'audio-helpers.ps1') } catch { $audio = $false; Note "note: audio helpers unavailable ($($_.Exception.Message)); defaults will not be restored" }

function Run([string]$exe, [string[]]$a, [string]$label) {
    $out = (& $exe @a 2>&1 | Out-String)
    $script:lastRc = $LASTEXITCODE
    Note ">>> $label $($a -join ' ')  (exit $script:lastRc)"
    Note $out.TrimEnd()
    return $out
}
function Quiet([string]$exe, [string[]]$a) { return (& $exe @a 2>&1 | Out-String) }
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
        Service    = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
        Inf        = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
        Version    = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
    }
}
function Format-Unit($u) { "$($u.InstanceId): driver $($u.Inf) $($u.Version) service $($u.Service) status $($u.Status)" }
function Quit-Blockers {
    foreach ($p in 'apollo-control', 'apollo-diag', 'asio-test', 'loopback-test', 'wasapi-loop', 'Ableton Live*') {
        if (Get-Process $p -ErrorAction SilentlyContinue) {
            Note "Quit $p first (it holds the Apollo devices or their endpoints open), then run this again."
            return $false
        }
    }
    return $true
}
function Get-EndpointSet {
    if (-not (Test-Path $aggKey)) { return 0 }
    $v = (Get-ItemProperty $aggKey -ErrorAction SilentlyContinue).EndpointSet
    if ($null -eq $v) { return 0 } else { return [int]$v }
}
function Set-EndpointSet([int]$v) {
    if (-not (Test-Path $aggKey)) { New-Item -Path $aggKey -Force | Out-Null }
    if ($v -eq 0) { Remove-ItemProperty -Path $aggKey -Name 'EndpointSet' -ErrorAction SilentlyContinue }
    else { Set-ItemProperty -Path $aggKey -Name 'EndpointSet' -Value $v -Type DWord }
    return (Get-EndpointSet)
}

# --- Windows default devices: saved in pass 1, put back in pass 2 (and after every restart) ---
function Save-Defaults {
    if (-not $audio) { return }
    try {
        $d = @(Get-AudioDefaults)
        Note 'Default audio devices now:'
        Format-AudioDefaults $d | ForEach-Object { Note $_ }
        ($d | ConvertTo-Json -Depth 3) | Out-File -Encoding ascii $defaultsFile
        Note "(saved to $defaultsFile)"
    } catch { Note "note: could not read the default audio devices ($($_.Exception.Message))" }
}
# The combined counterpart of one of the units' own endpoints (or the reverse for -Rollback), by name.
function Counterpart([string]$name, [int]$flow, $active, [switch]$Back) {
    $want = $null
    if (-not $Back) {
        if ($flow -eq 0 -and $name -match '^MON L/R \(Apollo (Twin X|x8p)') { $want = { param($n) $n -like 'Monitor (*' -and $n -like '*(Apollo*' } }
        elseif ($flow -eq 1 -and $name -match '^MIC/LINE (\d) \(Apollo x8p') { $k = $Matches[1]; $want = { param($n) $n -like "x8p MIC/LINE $k (*" } }
        elseif ($flow -eq 1 -and $name -match '^MIC/LINE (\d) \(Apollo Twin X') { $k = $Matches[1]; $want = { param($n) $n -like "TwinX MIC/LINE $k (*" } }
    } else {
        if ($flow -eq 0 -and $name -like 'Monitor (*' -and $name -like '*(Apollo*') { $want = { param($n) $n -like 'MON L/R (Apollo Twin X*' } }
        elseif ($flow -eq 1 -and $name -match '^x8p MIC/LINE (\d) \(') { $k = $Matches[1]; $want = { param($n) $n -like "MIC/LINE $k (Apollo x8p*" } }
        elseif ($flow -eq 1 -and $name -match '^TwinX MIC/LINE (\d) \(') { $k = $Matches[1]; $want = { param($n) $n -like "MIC/LINE $k (Apollo Twin X*" } }
    }
    if (-not $want) { return $null }
    return @($active | Where-Object { $_.Flow -eq $flow -and (& $want $_.Name) }) | Select-Object -First 1
}
function Restore-SavedDefaults([int[]]$waits, [switch]$Back) {
    if (-not $audio -or -not (Test-Path -LiteralPath $defaultsFile)) { return }
    $saved = @()
    # PS 5.1 ConvertFrom-Json returns a JSON array as ONE object: unroll it into its entries.
    try { $saved = @((Get-Content -Raw -LiteralPath $defaultsFile | ConvertFrom-Json) | ForEach-Object { $_ }) } catch { Note "note: $defaultsFile unreadable"; return }
    foreach ($w in $waits) {
        Start-Sleep -Seconds $w
        try {
            foreach ($l in @(Restore-AudioDefaults $saved)) { Note $l }
            # a saved default that was one of the units' own endpoints and is gone now: its combined counterpart
            $active = @(Get-AudioEndpoints)
            foreach ($d in $saved) {
                if (-not $d.Id -or ($active | Where-Object { $_.Id -eq $d.Id })) { continue }
                $c = Counterpart $d.Name $d.Flow $active -Back:$Back
                if (-not $c) { continue }
                $now = [AviumAudio.Audio]::DefaultId($d.Flow, $d.Role)
                if ($now -eq $c.Id) { continue }
                $hr = [AviumAudio.Audio]::SetDefault($c.Id, $d.Role)
                Note ("  {0} {1}: '{2}' is gone with the endpoint set; '{3}' (the same outputs/input) made the default instead (0x{4:X8})" -f
                      $(if ($d.Flow -eq 0) { 'playback' } else { 'recording' }), @('Console', 'Multimedia', 'Communications')[$d.Role],
                      $d.Name, $c.Name, $hr)
            }
        } catch { Note "note: could not check the default audio devices ($($_.Exception.Message))" }
    }
}

# Both units' bring-ups have an outcome (Connected 5, Failed 256, Gone 512); DSP ready can take 30 s.
function Wait-Outcome([string]$unit, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    do {
        $i = Quiet $diag @('--unit', $unit, 'info')
        $stage = FirstInt (Field $i 'Stage')
        Write-Host "  $unit stage: $(Field $i 'Stage')"
        if ($stage -eq 5 -or $stage -eq 256 -or $stage -eq 512) { return $i }
        Start-Sleep -Seconds 3
    } while ((Get-Date) -lt $deadline)
    return $i
}
function Enable-Unit($u) {
    $o = (& $pnputil /enable-device $u.InstanceId 2>&1 | Out-String)
    Note ">>> pnputil /enable-device $($u.InstanceId)"; Note $o.TrimEnd()
}
# Disable the Twin X, then the x8p; enable the x8p, then the Twin X. On a refused (vetoed) disable, every unit
# this function disabled, and the one Windows queued for a reboot, is enabled again before it returns, so no
# unit is left off or waiting for a reboot ([G7 review]).
function Restart-BothUnits($x8p, $twx) {
    $disabled = @()
    foreach ($u in @($twx, $x8p)) {
        $o = (& $pnputil /disable-device $u.InstanceId 2>&1 | Out-String)
        Note ">>> pnputil /disable-device $($u.InstanceId)"; Note $o.TrimEnd()
        if ($LASTEXITCODE -ne 0 -or $o -match '(?i)reboot|restart') {
            Note 'Windows could not stop that driver now (something still holds it). Enabling again what this script disabled:'
            foreach ($d in @($u) + $disabled) { Enable-Unit $d; Start-Sleep -Seconds 2 }
            Note 'Nothing is left disabled. If Windows still lists a pending reboot for the x8p, devcon enable "@<its id>" clears'
            Note 'the queued disable. Close what holds the Apollos (Apollo Control, Ableton, browser tabs playing audio) and use'
            Note 'the power-cycle way instead.'
            return $false
        }
        $disabled += $u
    }
    Start-Sleep -Seconds 3
    foreach ($u in @($x8p, $twx)) {
        Enable-Unit $u
        Start-Sleep -Seconds 2
    }
    return $true
}
# Everything that holds either unit: processes, ASIO, Windows sessions on any Apollo endpoint (Test-ApolloInUse
# lists those), and the ACX streams of BOTH units ([G7 review]: with EndpointSet 1 the combined endpoints are the
# x8p's circuits).
function Test-AllIdle {
    $why = @()
    if ($audio) { $why += @(Test-ApolloInUse $diag | ForEach-Object { $_ }) }   # it returns ,$array: flatten
    foreach ($un in 'x8p', 'twinx') {
        $ep = Quiet $diag @('--unit', $un, 'endpoints')
        if ($ep -match 'ACX streams: prepared (\d+), running (\d+)' -and [int]$Matches[1] -gt 0) {
            $what = if ($un -eq 'x8p' -and (Get-EndpointSet) -eq 1) { 'the x8p (the combined endpoints)' } else { "the $un" }
            $why += "$($Matches[1]) Windows stream(s) open on $what, $($Matches[2]) running"
        }
    }
    return ,@($why | Select-Object -Unique)
}
# Before a warm restart: wait up to 60 s for both units to be free. $false = do not restart.
function Wait-AllIdle {
    $why = Test-AllIdle
    if ($why.Count -eq 0) { return $true }
    Note 'The units are in use:'; foreach ($w in $why) { Note "  - $w" }
    Note 'Close those programs or stop their playback/recording. Waiting up to 60 s...'
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        $why = Test-AllIdle
        if ($why.Count -eq 0) { Note 'Both units are free.'; return $true }
    }
    Note 'Still in use:'; foreach ($w in $why) { Note "  - $w" }
    if ($Yes) { Note '-Yes: not restarting into a likely veto; power-cycle the units instead.'; return $false }
    $a = Read-Host '[Enter] restart anyway (a refused disable is undone at once), [Q] quit'
    return -not ($a -match '^[Qq]')
}

# The combined configuration around the sum test. Kept next to the log (TEMP is emptied by the next run), with a
# marker while the test routing is in force: a run stopped there (Ctrl+C is handled by finally; a closed window
# is not) is put right by the next run of this script before anything else ([G7 review]).
$aggSaved = Join-Path $PSScriptRoot 'step-g7.aggconfig.bin'
$aggPending = Join-Path $PSScriptRoot 'step-g7.restore-pending'
function Restore-AggConfig {
    if (-not (Test-Path -LiteralPath $aggSaved)) { return $false }
    Run $aggt @('set', '--load', $aggSaved) 'agg-test' | Out-Null
    if ($script:lastRc -ne 0) { return $false }
    Remove-Item -LiteralPath $aggPending -Force -ErrorAction SilentlyContinue
    return $true
}

# ======================================================================================================
# Both units, the package, Mode
# ======================================================================================================
$x8p = Unit $x8pHwid
$twx = Unit $twinxHwid
if (-not $x8p) { Note 'The x8p is not connected: G7 needs it (it hosts the combined endpoints; the Twin X sits behind it).'; Done 1 }
if (-not $twx) { Note 'The Twin X is not connected or not powered on.'; Done 1 }
Note "x8p:    $(Format-Unit $x8p)"
Note "Twin X: $(Format-Unit $twx)"
if ($x8p.Service -ne 'apollo' -or $twx.Service -ne 'apollo') { Note 'Both units must run apollo.sys (install the merged G7 build on both first).'; Done 1 }
$infPath = Join-Path $env:SystemRoot "INF\$($x8p.Inf)"
if (-not (Test-Path -LiteralPath $infPath) -or -not (Select-String -LiteralPath $infPath -SimpleMatch -Pattern 'ApolloAggMon' -Quiet)) {
    Note "The x8p's driver package ($($x8p.Inf)) has no combined endpoints (ApolloAggMon): not a G7 build. Install the merged build first."
    Done 1
}
if ($twx.Version -ne $x8p.Version) { Note "note: the units carry different driver versions (x8p $($x8p.Version), Twin X $($twx.Version)); one apollo.sys image serves both, the older one may still be loaded." }
$ti0 = Quiet $diag @('--unit', 'twinx', 'info')
if ((Field $ti0 'Mode') -notmatch '^2\b') { Note "The Twin X runs Mode '$(Field $ti0 'Mode')', not 2: G7 needs it running in full (step-g4.ps1)."; Done 1 }
$st0 = Quiet $aggt @('state')
$gen = [regex]::Match($st0, 'config generation (\d+)')
if (-not $gen.Success -or [int64]$gen.Groups[1].Value -eq 0) {
    Note 'agg-test state shows no configuration generation: the apollo.sys running is older than G7 (the new one loads only'
    Note 'when both units have let go of the old image: power the Twin X off, then the x8p; x8p on, then the Twin X).'
    Note $st0.TrimEnd()
    Done 1
}
if (Test-Path -LiteralPath $aggPending) {
    Note 'A previous step-g7 run stopped while the sum test''s routing was in force: putting your combined configuration back.'
    if (Restore-AggConfig) { Note 'Done: your combined configuration is back.' }
    else { Note "Could not: run  `"$aggt`" set --load `"$aggSaved`"  or Apply in Apollo Control, then run this again."; Done 1 }
}

# ======================================================================================================
# -Rollback: each unit's own endpoints again
# ======================================================================================================
if ($Rollback) {
    $stillCombined = (Quiet $diag @('--unit', 'x8p', 'endpoints')) -match '(?m)^\s+\d+\s+out\s+ApolloAggMon\s'
    if ((Get-EndpointSet) -eq 0 -and -not $stillCombined) {
        # rollback pass 2: the units run their own endpoints again; put the defaults back
        Note 'ROLLBACK PASS 2: both units have their own endpoints again.'
        Restore-SavedDefaults @(1, 3) -Back
        if (Test-Path -LiteralPath $defaultsFile) { Move-Item -LiteralPath $defaultsFile -Destination "$defaultsFile.rollback-used" -Force }
        Run $diag @('--unit', 'x8p', 'endpoints') 'apollo-diag' | Out-Null
        Run $diag @('--unit', 'twinx', 'endpoints') 'apollo-diag' | Out-Null
        if ($audio) { Format-AudioDefaults @(Get-AudioDefaults) | ForEach-Object { Note $_ } }
        Done 0
    }
    if (-not (Quit-Blockers)) { Done 1 }
    if (Test-Path -LiteralPath "$defaultsFile.used") {
        # [G7 review] the defaults from before the combined set (pass 2 kept them as .used), not the combined ones
        Copy-Item -LiteralPath "$defaultsFile.used" -Destination $defaultsFile -Force
        Note "Default devices to put back after the restart: the ones saved before the combined set ($defaultsFile.used):"
        try { Format-AudioDefaults @((Get-Content -Raw -LiteralPath $defaultsFile | ConvertFrom-Json) | ForEach-Object { $_ }) | ForEach-Object { Note $_ } } catch { }
    } else {
        Save-Defaults                       # none kept: today's, mapped back (Monitor -> MON L/R ...)
    }
    $v = Set-EndpointSet 0
    Note "Parameters\Aggregate\EndpointSet = $v (standalone: each unit its own endpoints, as before G7)."
    if (-not $Restart) {
        Note 'Now power the Twin X OFF, then the x8p OFF; wait 10 s; x8p ON, then the Twin X ON; wait about 60 s.'
        Note 'Then run step-g7.ps1 -Rollback again: it puts your default devices back (Monitor -> MON L/R (Apollo Twin X ...)).'
        Note '(Or step-g7.ps1 -Rollback -Restart: a warm restart of both drivers now; the x8p''s strips then stay off until its'
        Note ' next power cycle.)'
        Done 0
    }
    if (-not (Wait-AllIdle)) { Note 'EndpointSet is 0 now; power-cycle the units instead, then run step-g7.ps1 -Rollback again.'; Done 1 }
    if (-not (Restart-BothUnits $x8p $twx)) { Note 'EndpointSet is 0 now; power-cycle the units, then run step-g7.ps1 -Rollback again.'; Done 1 }
    Note 'Waiting for both bring-ups...'
    Wait-Outcome 'x8p' 90 | Out-Null
    Wait-Outcome 'twinx' 120 | Out-Null
    Restore-SavedDefaults @(2, 4, 8) -Back
    if (Test-Path -LiteralPath $defaultsFile) { Move-Item -LiteralPath $defaultsFile -Destination "$defaultsFile.rollback-used" -Force }
    Run $diag @('--unit', 'x8p', 'endpoints') 'apollo-diag' | Out-Null
    Run $diag @('--unit', 'twinx', 'endpoints') 'apollo-diag' | Out-Null
    Done 0
}

# ======================================================================================================
# Which pass: pass 2 when the x8p's driver table is the combined one
# ======================================================================================================
$xep0 = Quiet $diag @('--unit', 'x8p', 'endpoints')
$combinedUp = $xep0 -match '(?m)^\s+\d+\s+out\s+ApolloAggMon\s'
if (-not $combinedUp) {
    # ---------------------------------------------------------------- PASS 1 ----------------------------
    Note ''
    Note 'PASS 1: switch both units to the combined endpoint set (Parameters\Aggregate\EndpointSet = 1), then restart'
    Note 'them (power cycle, or -Restart). Windows audio on the Apollos stops for a few seconds; close Ableton and'
    Note 'anything playing through them.'
    if (-not (Quit-Blockers)) { Done 1 }
    if ($Restart -and -not (Wait-AllIdle)) { Note 'Nothing changed.'; Done 1 }
    if (-not $Yes) {
        $a = Read-Host '[Enter] go on, [Q] quit'
        if ($a -match '^[Qq]') { Note 'Nothing changed.'; Done 1 }
    }
    Save-Defaults
    $v = Set-EndpointSet 1
    if ($v -ne 1) { Note "Could not write $aggKey\EndpointSet (reads $v). Nothing else changed."; Done 1 }
    $mask = (Get-ItemProperty $aggKey -ErrorAction SilentlyContinue).EndpointMask
    Note "Parameters\Aggregate\EndpointSet = 1, EndpointMask $(if ($null -eq $mask) { 'absent (default 0x3FF: Monitor, Monitor mix, x8p MIC/LINE 1-8)' } else { '0x{0:X}' -f $mask })."
    Note '(read when each unit''s driver starts; nothing restarted yet)'
    if (-not $Restart) {
        Note ''
        Note 'Pass 1 done. Now:'
        Note '  1. Power the Twin X OFF, then the x8p OFF. Wait 10 s.'
        Note '  2. Power the x8p ON, then the Twin X ON. Wait about 60 s (the Twin X DSP-ready wait alone can take 30 s).'
        Note '  3. Run step-g7.ps1 again (pass 2). It plays test tones into the speakers unless you add -NoAudio.'
        Note '  (Or step-g7.ps1 -Restart: a warm restart of both drivers now; the x8p''s strips then stay off until its'
        Note '   next power cycle, so its MIC/LINE endpoints are silent until then.)'
        Note "Way back: step-g7.ps1 -Rollback (EndpointSet 0)."
        Done 0
    }
    Note ''
    Note 'Restarting both drivers now (warm).'
    if (-not (Restart-BothUnits $x8p $twx)) { Note 'EndpointSet is 1 now: power-cycle both units instead, then run step-g7.ps1 (pass 2).'; Done 1 }
    Start-Sleep -Seconds 5
}

# ---------------------------------------------------------------- PASS 2 ------------------------------
Note ''
Note 'PASS 2: waiting for both bring-ups (up to 120 s)...'
$xi = Wait-Outcome 'x8p' 90
$ti = Wait-Outcome 'twinx' 120
Start-Sleep -Seconds 3
Restore-SavedDefaults @(1, 3, 6)

Note ''
Note '=== G7 readings ==='
Run $diag @('units') 'apollo-diag' | Out-Null
$xi  = Run $diag @('--unit', 'x8p', 'info') 'apollo-diag'
$ti  = Run $diag @('--unit', 'twinx', 'info') 'apollo-diag'
$xep = Run $diag @('--unit', 'x8p', 'endpoints') 'apollo-diag'
$tep = Run $diag @('--unit', 'twinx', 'endpoints') 'apollo-diag'
$st  = Run $aggt @('state') 'agg-test'
Run $aggt @('config') 'agg-test' | Out-Null
$winEps = @()
if ($audio) {
    try { $winEps = @(Get-AudioEndpoints) } catch { Note "note: could not list the Windows endpoints ($($_.Exception.Message))" }
    Note '=== Windows audio endpoints of the Apollos ==='
    foreach ($e in $winEps) { if ($e.Name -like '*(Apollo*') { Note ("  {0} {1}" -f $(if ($e.Flow -eq 0) { 'out' } else { 'in ' }), $e.Name) } }
    Note 'Default audio devices now:'
    Format-AudioDefaults @(Get-AudioDefaults) | ForEach-Object { Note $_ }
}

Note ''
Note '=== G7 results (PASS = as expected; CHECK = look at it, then tell Claude) ==='
foreach ($pair in @(@('x8p', $xi), @('Twin X', $ti))) {
    $i = $pair[1]
    if ((FirstInt (Field $i 'Stage')) -eq 5 -and (Field $i 'Mode') -match '^2\b' -and (Field $i 'Flags') -match '\bTRANSPORT\b') { Pass "$($pair[0]): Connected, Mode 2, transport running" }
    else { Check "$($pair[0]): Stage $(Field $i 'Stage'), Mode $(Field $i 'Mode'), Flags $(Field $i 'Flags')" }
}
# driver side: the combined table on the x8p, nothing on the Twin X
$m = [regex]::Match($xep, 'Endpoints\s+(\d+) in the driver''s table, EnabledEndpoints 0x([0-9A-Fa-f]+)')
$xmask = 0
if ($m.Success -and [int]$m.Groups[1].Value -eq 12) { $xmask = [Convert]::ToInt32($m.Groups[2].Value, 16); Pass "x8p: the combined table (12 rows), EndpointMask 0x$($m.Groups[2].Value)" }
else { Check "x8p endpoint table: $(Field $xep 'Endpoints') (expected 12 rows: the combined set)" }
$rows = @('ApolloAggMon', 'ApolloAggMonIn', 'ApolloAggX8pIn1', 'ApolloAggX8pIn2', 'ApolloAggX8pIn3', 'ApolloAggX8pIn4',
          'ApolloAggX8pIn5', 'ApolloAggX8pIn6', 'ApolloAggX8pIn7', 'ApolloAggX8pIn8', 'ApolloAggTwxIn1', 'ApolloAggTwxIn2')
for ($r = 0; $r -lt $rows.Count; $r++) {
    $row = [regex]::Match($xep, "(?m)^\s+\d+\s+(out|in)\s+$($rows[$r])\s+.*$")
    $want = (($xmask -shr $r) -band 1) -eq 1
    if ($want -and $row.Success -and $row.Value -notmatch 'not added|disabled') { Pass "x8p circuit $($rows[$r]) added" }
    elseif (-not $want -and (-not $row.Success -or $row.Value -match 'disabled')) { Note "INFO   $($rows[$r]) not in EndpointMask (opt-in)" }
    else { Check "x8p circuit $($rows[$r]): $($row.Value.Trim())" }
}
if ($tep -match 'Endpoints\s+0 in the driver''s table') { Pass 'Twin X: no circuits of its own (the x8p hosts the set)' }
else { Check "Twin X endpoint table: $(Field $tep 'Endpoints') (expected 0 rows with EndpointSet 1)" }
# the aggregate
if ($st -match 'endpoint set: COMBINED') { Pass 'aggregate: endpoint set COMBINED' } else { Check 'aggregate: endpoint set not COMBINED (agg-test state above)' }
if ($st -match 'combined device: (enabled|OFF), active') { Pass 'aggregate: a timeline runs' } else { Check 'aggregate not active (agg-test state above)' }
if ($st -match '(?m)^\s+\[\d\]\s+TwinX\s+\S+\s+present master timeline running') { Pass 'aggregate: the Twin X is master and timeline (default)' }
else { Check 'the Twin X is not the running master/timeline (agg-test state above; fine if you chose the x8p as master)' }
if ($st -match '(?m)^\s+\[\d\]\s+x8p\s+\S+\s+present\b[^\n]*clock LOCKED') { Pass 'aggregate: the x8p follower LOCKED' }
else { Note 'INFO   the x8p follower is not LOCKED yet (it locks within about a second of both units running)' }
# Windows side
$apo = @($winEps | Where-Object { $_.Name -like '*(Apollo*' })
$monOut = @($apo | Where-Object { $_.Flow -eq 0 -and $_.Name -like 'Monitor (*' })
$monIn  = @($apo | Where-Object { $_.Flow -eq 1 -and $_.Name -like 'Monitor mix (*' })
$mics   = @($apo | Where-Object { $_.Flow -eq 1 -and $_.Name -match '^(x8p|TwinX) MIC/LINE \d \(' })
$stand  = @($apo | Where-Object { $_.Name -match '^(MON L/R|MIC/LINE \d) \(Apollo (x8p|Twin X)' })
if ($audio) {
    if ($monOut.Count -eq 1) { Pass "Windows playback endpoint: $($monOut[0].Name)" } else { Check "Windows playback endpoints of the Apollos: $(($apo | Where-Object { $_.Flow -eq 0 } | ForEach-Object { $_.Name }) -join '; ') (expected one 'Monitor (...)')" }
    if (@($apo | Where-Object { $_.Flow -eq 0 }).Count -eq 1) { Pass 'one Apollo playback endpoint in all (no Speakers, no standalone MON L/R)' } else { Check 'more than one Apollo playback endpoint' }
    if ($monIn.Count -eq 1) { Pass "Windows recording endpoint: $($monIn[0].Name)" } else { Check "no single 'Monitor mix (...)' recording endpoint" }
    $wantMics = 0; for ($r = 2; $r -lt 12; $r++) { if ((($xmask -shr $r) -band 1) -eq 1) { $wantMics++ } }
    if ($mics.Count -eq $wantMics) { Pass "$($mics.Count) MIC/LINE recording endpoints, named by unit ($(($mics | Select-Object -First 1).Name) ...)" }
    else { Check "$($mics.Count) MIC/LINE endpoints named 'x8p/TwinX MIC/LINE n (...)' (expected $wantMics)" }
    if ($stand.Count -eq 0) { Pass "the units' own endpoints are gone (no duplicates)" } else { Check "standalone endpoints still active: $(($stand | ForEach-Object { $_.Name }) -join '; ')" }
    if ($monOut.Count -eq 1) {
        if ($monOut[0].Name -like '*(Apollo)') { Note 'INFO   Windows names the combined endpoints after the INF FriendlyName: "(Apollo)".' }
        else { Note "INFO   Windows names them after the x8p's device: '$($monOut[0].Name)' (the INF FriendlyName 'Apollo' shows in Sound settings' device line)." }
    }
}

# ---------------------------------------------------------------- audio ---------------------------------
$renderMatch = 'Monitor (Apollo'
$captureMatch = 'Monitor mix (Apollo'
if ($NoAudio -or -not $audio -or $monOut.Count -ne 1 -or $monIn.Count -ne 1) {
    if (-not $NoAudio) { Note 'Audio tests skipped: the combined Monitor endpoints are not both there (see the CHECK lines).' }
} else {
    Note ''
    Note 'AUDIO TESTS (about 50 s): 997 Hz at -12 dBFS from Windows and 1 kHz at -20 dBFS from the DAW test go to the'
    Note 'Twin X MON outputs, i.e. THE SPEAKERS. Turn the Twin X monitor knob down now; the tests read the monitor-mix'
    Note 'loopback, not the speakers.'
    if (-not $Yes) {
        $a = Read-Host '[Enter] play the tests, [S] skip them'
        if ($a -match '^[Ss]') { $NoAudio = $true }
    } else {
        for ($c = 5; $c -gt 0; $c--) { Write-Host "  starting in $c s..."; Start-Sleep -Seconds 1 }
    }
}
if (-not $NoAudio -and $audio -and $monOut.Count -eq 1 -and $monIn.Count -eq 1) {
    # a0) The monitor mix with nothing playing. The loop below is only sample-exact when the mix
    #     carries the playback alone: a live input strip (a mic on the Twin X inputs), the talkback mic or another
    #     app on "Monitor" add their own signal, which the sine check reads as small steady errors (the first G7
    #     run: 1.9e-3 at -11.98 dBFS, 0 dropouts). The master's channels are an integer copy (aggep-test 8).
    $floor = Run $loop @('--meter', $captureMatch, '3') 'wasapi-loop'
    $floorRc = $script:lastRc
    $floorPeak = ''
    $fm = [regex]::Match($floor, 'Loudest peak:([^\r\n]*)')
    if ($fm.Success) { $floorPeak = $fm.Groups[1].Value.Trim() }
    switch ($floorRc) {
        3 { Pass 'the monitor mix is digital zero with nothing playing: the loop below must be sample-exact' }
        0 { Note "INFO   the monitor mix carries other signal with nothing playing (loudest: $floorPeak): live inputs, the talkback"
            Note '       mic or another app. The loop below reads that signal as small steady errors; only jumps, Windows'
            Note '       discontinuities, position gaps or driver counters are dropouts.' }
        default { Note "INFO   could not meter the monitor mix first (exit $floorRc)" }
    }
    # a) Windows alone through the combined endpoints, every sample checked
    $o = Run $loop @('20', '--render', $renderMatch, '--capture', $captureMatch) 'wasapi-loop'
    $loopRc = $script:lastRc
    # With other signal in the mix (a0), steady errors far below a dropout's (a lost or repeated packet of a
    # -12 dBFS tone errs by 0.1 or more) and no discontinuity, gap, late feed or driver counter are no glitch.
    $worst = 0.0
    foreach ($wm in [regex]::Matches($o, 'Worst error L ([0-9.]+e[+-]?[0-9]+), R ([0-9.]+e[+-]?[0-9]+)')) {
        foreach ($g in 1, 2) { $v = [double]$wm.Groups[$g].Value; if ($v -gt $worst) { $worst = $v } }
    }
    $counted = [regex]::Matches($o, '(?m)^\s*driver:.*$') | Where-Object { $_.Value -match '\+[1-9]' }
    $clean = ($o -match 'Windows discontinuities 0, position gaps 0\)') -and ($o -match 'Playback fed late: 0') -and (-not $counted)
    if ($loopRc -eq 1 -and $floorRc -eq 0 -and $clean -and $worst -gt 0 -and $worst -lt 0.02) {
        $why = 'Windows through "Monitor" -> "Monitor mix": 20 s, no dropout (worst error {0:0.0e+0} = the monitor mix''s own' -f $worst
        Pass "$why signal, see INFO above; unplug or mute the Twin X inputs for a sample-exact run)"
    } else {
        switch ($loopRc) {
            0 { Pass 'Windows through "Monitor" -> "Monitor mix": 20 s glitch-free' }
            1 { Check 'wasapi-loop found glitches through the combined endpoints (see above)' }
            3 { Check 'wasapi-loop: no signal came back on "Monitor mix" (routing or loopback; see above)' }
            default { Check "wasapi-loop exit $loopRc (setup; see above)" }
        }
    }
    # b) Windows and the DAW at the same time: they must sum on the Twin X MON
    $o = Run $aggt @('config', '--save', $aggSaved) 'agg-test'
    if ($o -match 'DEFAULT, nothing stored') {
        Note 'INFO   no combined configuration was stored (the driver default ran): after the test the same values are put back'
        Note '       as a stored configuration (units fixed to the ones seen now). Apollo Control shows and changes it as usual.'
    }
    if ($script:lastRc -ne 0) { Check 'could not save the combined configuration: the summing test is skipped (nothing changed)' }
    else {
        # [G7 review] from here until the configuration is back: the marker (a closed window) and finally (Ctrl+C)
        Set-Content -LiteralPath $aggPending -Value "step-g7 sum test $(Get-Date -Format o)" -Encoding ascii
        $tone = $null
        $ao = ''
        $arc = -1
        $routed = $false
        try {
            $o = Run $aggt @('set', '--routes', 'none') 'agg-test'
            if ($script:lastRc -ne 0) { Check 'could not set the test routing (see above): summing test skipped' }
            else {
                $routed = $true
                $toneOut = Join-Path $env:TEMP 'step-g7.tone.txt'
                $tone = Start-Process -FilePath $loop -ArgumentList @('--tone', '30', '--render', "`"$renderMatch`"") -PassThru -NoNewWindow -RedirectStandardOutput $toneOut
                Start-Sleep -Seconds 2
                $ao = Run $asio @('--agg', '20', '128', '--also', '997') 'asio-test'
                $arc = $script:lastRc
                if (-not $tone.WaitForExit(15000)) { Stop-Process -Id $tone.Id -Force -ErrorAction SilentlyContinue }
                if (Test-Path -LiteralPath $toneOut) { Note '>>> wasapi-loop --tone (background) said:'; Note ((Get-Content -Raw -LiteralPath $toneOut) -as [string]).TrimEnd() }
            }
        } finally {
            if ($tone -and -not $tone.HasExited) { Stop-Process -Id $tone.Id -Force -ErrorAction SilentlyContinue }
            if (Restore-AggConfig) { Pass 'your combined configuration is back (agg-test set --load)' }
            else { Check "could not put the configuration back: run  `"$aggt`" set --load `"$aggSaved`"  or Apply in Apollo Control (the next run of this script tries again)" }
        }
        if ($routed) {
            if ($arc -eq 0 -and $ao -match 'combined loopback: PASS') { Pass 'DAW through "Apollo (Avium)": 1 kHz on both units'' MON loopbacks, phase constant' }
            else { Check "asio-test --agg exit $arc (see above)" }
            $mo = [regex]::Match($ao, 'other tone 997 Hz: TwinX MON L (-?[\d.]+) dBFS, x8p MON L (-?[\d.]+) dBFS')
            if ($mo.Success) {
                $twxL = [double]$mo.Groups[1].Value; $x8pL = [double]$mo.Groups[2].Value
                if ($twxL -gt -60) { Pass "Windows audio sums with the DAW on the Twin X MON: 997 Hz at $twxL dBFS beside the 1 kHz" }
                else { Check "the Windows tone did not reach the Twin X MON loopback (997 Hz at $twxL dBFS)" }
                if ($x8pL -lt -80 -or $x8pL -lt $twxL - 30) { Pass "and only there: the x8p MON loopback has it at $x8pL dBFS" }
                else { Check "the Windows tone is on the x8p MON loopback too ($x8pL dBFS): routing?" }
            } else { Check 'asio-test printed no second-tone line (an older asio-test?)' }
            if ($ao -match 'driver \(combined\): (\d+) switches, (\d+) underruns, (\d+) overruns, (\d+) resyncs') {
                if ([int]$Matches[2] -eq 0 -and [int]$Matches[3] -eq 0) { Pass "combined ASIO with Windows playing: $($Matches[1]) switches, 0 underruns, 0 overruns" }
                else { Check "combined ASIO with Windows playing: $($Matches[2]) underruns, $($Matches[3]) overruns" }
            }
        }
    }
}

# ---------------------------------------------------------------- Twin X loss (optional) ----------------
if ($LossTest -and -not $NoAudio -and $monOut.Count -eq 1) {
    Note ''
    Note 'TWIN X LOSS TEST: Windows plays 997 Hz into "Monitor" (the speakers) for 90 s. When asked, power the TWIN X'
    Note 'OFF, wait 10 s, power it ON. Expected: silence while it is off, no crash, the stream keeps running on the x8p''s'
    Note 'clock, and once the Twin X is back (30-60 s) the tone plays on the speakers again with the Twin X as timeline.'
    $toneOut = Join-Path $env:TEMP 'step-g7.loss.txt'
    $tone = Start-Process -FilePath $loop -ArgumentList @('--tone', '90', '--render', "`"$renderMatch`"") -PassThru -NoNewWindow -RedirectStandardOutput $toneOut
    Start-Sleep -Seconds 3
    Read-Host 'Now power the Twin X OFF, wait 10 s, power it ON, then press Enter here' | Out-Null
    $sawAbsent = $false; $back = $false
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline) {
        $s = Quiet $aggt @('state')
        if ($s -match '(?m)^\s+\[\d\]\s+TwinX\s+\S+\s+ABSENT') { $sawAbsent = $true }
        if ($s -match '(?m)^\s+\[\d\]\s+TwinX\s+\S+\s+present master timeline running') { $back = $true; break }
        Start-Sleep -Seconds 2
    }
    $s = Run $aggt @('state') 'agg-test'
    $running = -not $tone.HasExited
    if (-not $tone.WaitForExit(90000)) { Stop-Process -Id $tone.Id -Force -ErrorAction SilentlyContinue }
    if ($back) { Pass 'Twin X back: master and timeline again' } else { Check 'the Twin X did not come back as timeline within 120 s (agg-test state above)' }
    if ($sawAbsent) { Note 'INFO   the aggregate saw the Twin X absent while it was off' }
    if ($running) { Pass 'the Windows stream on "Monitor" kept running through the loss' } else { Check 'wasapi-loop --tone ended early (see step-g7.loss.txt)' }
    $xl = Quiet $diag @('--unit', 'x8p', 'info')
    if ((FirstInt (Field $xl 'Stage')) -eq 5) { Pass 'x8p still Connected (no crash)' } else { Check "x8p Stage $(Field $xl 'Stage')" }
    if (Test-Path -LiteralPath $toneOut) { Note ((Get-Content -Raw -LiteralPath $toneOut) -as [string]).TrimEnd() }
}

Restore-SavedDefaults @(1)
if (Test-Path -LiteralPath $defaultsFile) {
    Move-Item -LiteralPath $defaultsFile -Destination "$defaultsFile.used" -Force
    Note "(saved defaults used; kept as $defaultsFile.used)"
}
Run $diag @('--unit', 'x8p', 'endpoints') 'apollo-diag' | Out-Null
Note ''
Note "Summary: $script:nPass PASS, $script:nCheck CHECK. Send Claude step-g7.log."
Note 'To make the combined output your Windows default: Sound settings -> Output -> "Monitor (...)".'
Note 'Way back to each unit''s own endpoints: step-g7.ps1 -Rollback (then a power cycle, or add -Restart).'
Done 0
