# [Mix] The monitor mix on the Twin X (docs\console-parity.md "Mixer"): input 1 muted, unmuted, then at -12 dB in the
# monitor mix, read on the monitor-mix loopback. Normal (non-admin) PowerShell, no power cycle, about 40 s:
#   powershell -ExecutionPolicy Bypass -File C:\Users\aaron\Desktop\Avium\apollo-win\tools\phase1\step-mix.ps1
#   ... -In 2                 test input 2 instead (the other inputs are muted while it runs)
#   ... -Meter "Monitor mix (Apollo Twin X"   another recording endpoint (wasapi-loop --list shows them; the default
#                             is the combined "Monitor mix", the loopback of the unit Windows plays to: the Twin X.
#                             -Unit x8p needs -Meter naming the x8p's own monitor-mix endpoint)
# NEEDS the [Mix] apollo.sys running the Twin X in Mode 2, its input strips live with a mic on the input under test
# picking up the room (a fan or steady room tone is ideal; please do not talk during the reads), and NOTHING PLAYING
# to the Twin X (Windows or a DAW), so the loopback carries the mic alone. Installs nothing, restarts nothing.
#
# Speakers: the loopback is the monitor mix BEFORE the monitor level, so the Twin X monitor knob can stay all the way
# down. The test only lowers the mix (mute, -12 dB) and puts it back.
#
# What it checks, each read a 3 s wasapi-loop --meter (the median of its 250 ms peaks, the louder channel):
#   1. baseline: there is signal in the monitor mix (else it stops: strips, mic, 48V?);
#   2. the other inputs muted, so the input under test is alone (INFO: how much they carried);
#   3. the input set explicitly to 0 dB centre: the level does not move (PASS: the unit's own default IS 0 dB
#      centre, what Apollo Control shows for an untouched input; INFO with the difference otherwise);
#   4. M on: the level drops by 10 dB or more (digital zero expected), and the driver logged the mute with status 0;
#   5. M off: back within 3 dB;
#   6. fader -12 dB: the level drops by about 12 dB (9-15 dB passes);
#   7. put back (fader 0 dB, the other inputs unmuted) and read once more (INFO against the baseline).
# Puts back only what it changed: the input under test at 0 dB centre and unmuted, the inputs it muted unmuted. The
# unit does not report the mix: if Apollo Control's Monitor mix card had other values for the Twin X, set them again.
# PowerShell 5.1, ASCII only.
param([ValidateSet('twinx', 'x8p')][string]$Unit = 'twinx', [ValidateRange(1, 8)][int]$In = 1,
      [string]$Meter = 'Monitor mix (Apollo', [switch]$Yes)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$diag = Join-Path $root 'tools\apollo-diag\apollo-diag.exe'
$loop = Join-Path $root 'tools\wasapi-loop\wasapi-loop.exe'
$log = Join-Path $PSScriptRoot 'step-mix.log'
"step-mix $(Get-Date -Format o) Unit=$Unit In=$In Meter=$Meter" | Out-File -Encoding utf8 $log
$script:nPass = 0; $script:nFail = 0; $script:nInfo = 0
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }
function Pass([string]$s) { $script:nPass++; Note "PASS  $s" }
function Fail([string]$s) { $script:nFail++; Note "FAIL  $s" }
function Info([string]$s) { $script:nInfo++; Note "INFO  $s" }

# apollo-diag on the unit: @{ Ok; Text } (the log file keeps the first 300 characters of its answer)
function Diag([string[]]$a) {
    $out = & $diag --unit $Unit @a 2>&1 | ForEach-Object { "$_" }
    $r = @{ Ok = ($LASTEXITCODE -eq 0); Text = ($out -join "`n") }
    $t = $r.Text.Trim()
    if ($t.Length -gt 300) { $t = $t.Substring(0, 300) + ' ...' }
    Add-Content -Encoding utf8 $log ("  apollo-diag {0}: {1}" -f ($a -join ' '), $t)
    return $r
}
# One monitor-mix write; a refusal is a FAIL with the driver's answer
function Mix([string]$what, [string[]]$a) {
    $r = Diag (@('mix') + $a)
    if (-not $r.Ok) { Fail "$what refused: $($r.Text.Trim())" }
    return $r.Ok
}
# A 3 s read of the monitor-mix loopback: the median of the 250 ms peaks (louder channel), dBFS; -200 = digital zero.
# $null when wasapi-loop could not read the endpoint.
function Read-Mix([string]$label) {
    $out = & $loop --meter $Meter 3 2>&1 | ForEach-Object { "$_" }
    $rc = $LASTEXITCODE
    $vals = @()
    foreach ($line in $out) {
        if ($line -notmatch '^\s*\d+\.\d+ s') { continue }
        $best = -200.0
        foreach ($m in [regex]::Matches($line, '(-?\d+\.\d|-inf) dBFS')) {
            $v = if ($m.Groups[1].Value -eq '-inf') { -200.0 } else { [double]$m.Groups[1].Value }
            if ($v -gt $best) { $best = $v }
        }
        $vals += $best
    }
    if ($rc -eq 2 -or $vals.Count -lt 4) {
        Fail "${label}: wasapi-loop could not meter '$Meter' (exit $rc): $(($out | Select-Object -Last 3) -join ' / ')"
        return $null
    }
    $sorted = $vals | Sort-Object
    $med = [double]$sorted[[int][Math]::Floor($sorted.Count / 2)]
    $shown = if ($med -le -199) { 'digital zero' } else { '{0:0.0} dBFS' -f $med }
    Note ("  read {0}: {1} (median of {2} peaks; spread {3:0.0} .. {4:0.0})" -f $label, $shown, $vals.Count, $sorted[0], $sorted[-1])
    return $med
}
function Db([double]$v) { if ($v -le -199) { return 'digital zero' } return ('{0:0.0} dBFS' -f $v) }

if (-not (Test-Path -LiteralPath $diag)) { Note "apollo-diag.exe not found at ${diag}: run tools\build-all.cmd"; exit 1 }
if (-not (Test-Path -LiteralPath $loop)) { Note "wasapi-loop.exe not found at ${loop}: run tools\build-all.cmd"; exit 1 }
$i = Diag @('info')
if (-not $i.Ok -or $i.Text -notmatch 'Mode\s+2') { Note "The $Unit is not present or not in Mode 2: nothing to test."; exit 1 }
$inputs = if ($Unit -eq 'twinx') { 2 } else { 8 }
if ($In -gt $inputs) { Note "The $Unit has inputs 1-$inputs."; exit 1 }
$others = @(1..$inputs | Where-Object { $_ -ne $In })

Note "Monitor mix test on the $Unit, input $In, read on '$Meter'."
Note 'Nothing may play to the unit. The monitor knob can stay down (the loopback is before it). Keep the room steady.'
if (-not $Yes) { Read-Host 'Press Enter to start (Ctrl+C to stop)' | Out-Null }

$muted = @()
$touched = $false                       # the input under test was written: put it back at the end
try {
    # 1. Baseline
    $b0 = Read-Mix 'baseline'
    if ($null -eq $b0) { throw 'no meter' }
    if ($b0 -lt -80) {
        Fail ("baseline $(Db $b0): no signal in the monitor mix. Is the mic on input $In live (input strips running, 48V if " +
              'it needs it)? Stopped before changing anything.')
        throw 'no signal'
    }
    Pass "baseline: the monitor mix carries $(Db $b0) with nothing playing"

    # 2. The other inputs muted: the input under test alone
    foreach ($o in $others) {
        if (-not (Mix "mute input $o" @("$o", 'mute', '1'))) { throw 'refused' }
        $muted += $o
    }
    $l1 = Read-Mix "input $In alone"
    if ($null -eq $l1) { throw 'no meter' }
    Info ("input $In alone: {0} ({1:0.0} dB below the baseline: what input(s) {2} carried)" -f (Db $l1), ($b0 - $l1), ($others -join ', '))
    if ($l1 -lt -80) { Fail "input $In alone carries nothing ($(Db $l1)): the signal was on another input. Put the mic on input $In."; throw 'no signal' }

    # 3. Set explicitly to 0 dB centre: the zero state the driver and Apollo Control assume for an untouched input
    $touched = $true
    if (-not (Mix "input $In pan centre" @("$In", 'pan', '0'))) { throw 'refused' }
    if (-not (Mix "input $In level 0 dB" @("$In", 'level', '0'))) { throw 'refused' }
    $l0 = Read-Mix "input $In set to 0 dB"
    if ($null -eq $l0) { throw 'no meter' }
    $d0 = $l0 - $l1
    if ([Math]::Abs($d0) -le 3) { Pass ("0 dB centre written: {0} (moved {1:+0.0;-0.0} dB): the unit's own default is 0 dB centre, as the app shows" -f (Db $l0), $d0) }
    else { Info ("0 dB centre written: {0}, {1:+0.0;-0.0} dB from the unit's own default: an untouched input is NOT at 0 dB centre (send this log)" -f (Db $l0), $d0) }

    # 4. Mute
    if (-not (Mix "mute input $In" @("$In", 'mute', '1'))) { throw 'refused' }
    $lm = Read-Mix "input $In muted"
    if ($null -eq $lm) { throw 'no meter' }
    $drop = $l0 - $lm
    if ($lm -le -199) { Pass "M on input ${In}: the monitor mix is digital zero (from $(Db $l0))" }
    elseif ($drop -ge 10) { Pass ("M on input ${In}: {0}, {1:0.0} dB down" -f (Db $lm), $drop) }
    else { Fail ("M on input ${In}: {0}, only {1:0.0} dB down: the mute did not reach the DSP that mixes the input (send this log)" -f (Db $lm), $drop) }
    $bus = $In - 1
    $line = (Diag @('log')).Text -split "`n" | Where-Object { $_ -match "mix: bus $bus muted: " } | Select-Object -Last 1
    if ($line -and $line -match '\(0x00000000\)') { Pass "the driver logged it: $($line.Trim())" }
    else { Fail "no clean 'mix: bus $bus muted' log line (status 0 wanted): '$line'" }

    # 5. Unmute
    if (-not (Mix "unmute input $In" @("$In", 'mute', '0'))) { throw 'refused' }
    $lu = Read-Mix "input $In unmuted"
    if ($null -eq $lu) { throw 'no meter' }
    if ([Math]::Abs($lu - $l0) -le 3) { Pass ("M off: back to {0} ({1:+0.0;-0.0} dB)" -f (Db $lu), ($lu - $l0)) }
    else { Fail ("M off: {0}, {1:+0.0;-0.0} dB from {2}" -f (Db $lu), ($lu - $l0), (Db $l0)) }

    # 6. Fader -12 dB
    if (-not (Mix "input $In level -12 dB" @("$In", 'level', '-12'))) { throw 'refused' }
    $lf = Read-Mix "input $In at -12 dB"
    if ($null -eq $lf) { throw 'no meter' }
    $fd = $lu - $lf
    if ($fd -ge 9 -and $fd -le 15) { Pass ("fader -12 dB: {0}, {1:0.0} dB down (about 12)" -f (Db $lf), $fd) }
    else { Fail ("fader -12 dB: {0}, {1:0.0} dB down, not about 12 (a louder or quieter room during a read moves this; run it again once)" -f (Db $lf), $fd) }
} catch {
    if ("$_" -notmatch '^(no meter|no signal|refused)$') { Fail "stopped: $_" }
} finally {
    # 7. Put back what this run changed: the input under test at 0 dB and unmuted, the inputs it muted unmuted
    if ($touched) {
        [void](Mix "input $In back to 0 dB" @("$In", 'level', '0'))
        [void](Mix "input $In unmuted" @("$In", 'mute', '0'))
    }
    foreach ($o in $muted) { [void](Mix "input $o unmuted" @("$o", 'mute', '0')) }
}
if ($touched -and $script:nFail -eq 0) {
    $le = Read-Mix 'put back'
    if ($null -ne $le) { Info ("put back: {0} ({1:+0.0;-0.0} dB from the baseline)" -f (Db $le), ($le - $b0)) }
}
Note "step-mix: $($script:nPass) passed, $($script:nFail) failed, $($script:nInfo) info. Log: $log"
if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }
exit ([int]($script:nFail -ne 0))
