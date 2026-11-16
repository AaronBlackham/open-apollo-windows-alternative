# Monitor features (docs\console-parity.md "Monitor"): ALT speakers, monitor and headphone sources, cue MIX/mono,
# cue sends and the talkback's cue sends, on each unit. Normal (non-admin) PowerShell, about a minute:
#   powershell -ExecutionPolicy Bypass -File C:\apollo-win\tools\phase1\step-mon.ps1
#   ... -Unit x8p | twinx     one unit only (default: both)
# NEEDS the [Monitor] apollo.sys running the unit in Mode 2. Installs nothing, restarts nothing, no power cycle.
#
# !! The speaker select (ALT) and the monitor source MOVE AUDIO between speaker sets and outputs: turn the
# !! speakers and headphones DOWN before you start. Each toggle is put back at once.
#
# What it checks, per unit: the driver takes each new setting the model has and refuses what it lacks (Twin X:
# no ALT 2, no HP 2, no CUE 3/4, no input 3); the speaker selection reads back (word 6 [29:28]); whether the
# readback also echoes the monitor source, CUE MIX/mono (word 2) and the HP sources (word 5) - INFO either way;
# the cue sends reach the DSP rings (the driver logs the mute re-send with status 0). ALT outputs and trims, and
# every send it set, end off / 0 dB / off (the unit does not report them, so there is no "before" to restore).
# PowerShell 5.1, ASCII only.
param([ValidateSet('both', 'x8p', 'twinx')][string]$Unit = 'both', [switch]$Yes)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$diag = Join-Path $root 'tools\apollo-diag\apollo-diag.exe'
$log = Join-Path $PSScriptRoot 'step-mon.log'
"step-mon $(Get-Date -Format o) Unit=$Unit" | Out-File -Encoding utf8 $log
$script:nPass = 0; $script:nFail = 0; $script:nInfo = 0
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }
function Pass([string]$s) { $script:nPass++; Note "PASS  $s" }
function Fail([string]$s) { $script:nFail++; Note "FAIL  $s" }
function Info([string]$s) { $script:nInfo++; Note "INFO  $s" }

# apollo-diag on one unit: @{ Ok; Text }
function Diag([string]$u, [string[]]$a) {
    $out = & $diag --unit $u @a 2>&1 | ForEach-Object { "$_" }
    return @{ Ok = ($LASTEXITCODE -eq 0); Text = ($out -join "`n") }
}
# The 40 readback words (after the service loop committed the last write)
function Words([string]$u) {
    Start-Sleep -Milliseconds 300
    $w = New-Object 'UInt32[]' 40
    foreach ($line in (Diag $u @('readback')).Text -split "`n") {
        if ($line -match '^\s+\[\s*(\d+)\]\s+([0-9A-F]{8})\s+([0-9A-F]{8})\s+([0-9A-F]{8})\s+([0-9A-F]{8})') {
            for ($k = 0; $k -lt 4; $k++) { $w[[int]$Matches[1] + $k] = [Convert]::ToUInt32($Matches[$k + 2], 16) }
        }
    }
    return ,$w
}
function Bits([UInt32]$v, [int]$lo, [int]$n) { return [int](($v -shr $lo) -band ((1 -shl $n) - 1)) }
# A setting the model has: the driver must take it
function Takes([string]$u, [string]$what, [string[]]$a) {
    $r = Diag $u $a
    if ($r.Ok) { Pass "$u $what accepted" } else { Fail "$u $what refused: $($r.Text)" }
    return $r.Ok
}
# A setting the model lacks: refused with error 50 (not supported) or 87 (invalid parameter)
function Refuses([string]$u, [string]$what, [string[]]$a, [string]$code) {
    $r = Diag $u $a
    if (-not $r.Ok -and $r.Text -match "error $code") { Pass "$u $what refused (error $code)" }
    elseif ($r.Ok) { Fail "$u $what was ACCEPTED (the model lacks it): put it back by hand if it changed anything" }
    else { Fail "$u $what refused, but not with error ${code}: $($r.Text)" }
}
# Toggle a field the readback may echo: INFO whether it followed, then put back
function Test-Echo([string]$u, [string]$what, [string[]]$set, [string[]]$back, [scriptblock]$read, [int]$want) {
    if (-not (Takes $u $what $set)) { return }
    $got = & $read (Words $u)
    if ($got -eq $want) { Info "$u ${what}: the readback echoes it ($got): Apollo Control could confirm it" }
    else { Info "$u ${what}: not echoed (reads $got, want $want): no readback for it, as assumed" }
    [void](Takes $u "$what put back" $back)
}

function Test-Unit([string]$u) {
    $i = Diag $u @('info')
    if (-not $i.Ok -or $i.Text -notmatch 'Mode\s+2') { Note "--- $u : not present or not in Mode 2, skipped"; return }
    $twin = $u -eq 'twinx'
    Note "--- $u"
    $w0 = Words $u
    Note ("baseline: word 2 0x{0:X8}, word 5 0x{1:X8}, word 6 0x{2:X8} (speakers {3})" -f $w0[2], $w0[5], $w0[6], (Bits $w0[6] 28 2))

    # 1. ALT speakers. The selection reads back in word 6 [29:28].
    $alts = if ($twin) { 1 } else { 2 }
    for ($n = 1; $n -le $alts; $n++) {
        [void](Takes $u "ALT $n output on" @('mon', "alt$n", '1'))
        [void](Takes $u "ALT $n trim -6 dB" @('mon', "alt${n}trim", '-6'))
        if (Takes $u "speakers ALT $n (audio moves to the ALT pair)" @('mon', 'alt', "alt$n")) {
            $sel = Bits (Words $u)[6] 28 2
            if ($sel -eq $n) { Pass "$u speakers read back ALT $n" } else { Fail "$u speakers read back $sel, not ALT $n" }
        }
        if (Takes $u 'speakers MAIN again' @('mon', 'alt', 'main')) {
            $sel = Bits (Words $u)[6] 28 2
            if ($sel -eq 0) { Pass "$u speakers read back MAIN" } else { Fail "$u speakers read back $sel, not MAIN" }
        }
        [void](Takes $u "ALT $n trim 0 dB" @('mon', "alt${n}trim", '0'))
        [void](Takes $u "ALT $n output off" @('mon', "alt$n", '0'))
    }

    # 2. Sources (the monitor source moves audio: the monitor plays that cue)
    $src = (Bits $w0[2] 18 2) + 4 * (Bits $w0[2] 29 1)
    $srcName = if ($src -le 4) { @('mix', 'cue1', 'cue2', 'cue3', 'cue4')[$src] } else { 'mix' }
    Test-Echo $u 'monitor source CUE 1' @('mon', 'source', 'cue1') @('mon', 'source', 'mix') { param($w) Bits $w[2] 18 2 } 1
    Test-Echo $u 'monitor source CUE 2' @('mon', 'source', 'cue2') @('mon', 'source', 'mix') { param($w) Bits $w[2] 18 2 } 2
    if (-not $twin) {
        Test-Echo $u 'monitor source CUE 3' @('mon', 'source', 'cue3') @('mon', 'source', 'mix') { param($w) Bits $w[2] 18 2 } 3
        Test-Echo $u 'monitor source CUE 4' @('mon', 'source', 'cue4') @('mon', 'source', 'mix') { param($w) Bits $w[2] 29 1 } 1
    }
    if ($srcName -ne 'mix') { [void](Takes $u "monitor source back to $srcName (from the readback)" @('mon', 'source', $srcName)) }
    $hp1 = @('cue1', 'cue2')[[Math]::Min(1, (Bits $w0[5] 24 3))]
    $other = if ($hp1 -eq 'cue1') { 'cue2' } else { 'cue1' }
    Test-Echo $u "HP 1 source $other" @('mon', 'hp1src', $other) @('mon', 'hp1src', $hp1) { param($w) Bits $w[5] 24 3 } ([int]($other -eq 'cue2'))
    if (-not $twin) {
        $hp2 = @('cue1', 'cue2')[[Math]::Min(1, (Bits $w0[5] 27 3))]
        $o2 = if ($hp2 -eq 'cue1') { 'cue2' } else { 'cue1' }
        Test-Echo $u "HP 2 source $o2" @('mon', 'hp2src', $o2) @('mon', 'hp2src', $hp2) { param($w) Bits $w[5] 27 3 } ([int]($o2 -eq 'cue2'))
    }

    # 3. Cue outputs: MIX source and mono (CUE 1-2 may echo in word 2; CUE 3/4 live in setting 8)
    Test-Echo $u 'CUE 1 mono' @('mon', 'cue1mono', '1') @('mon', 'cue1mono', '0') { param($w) Bits $w[2] 21 1 } 1
    Test-Echo $u 'CUE 2 follows the monitor mix' @('mon', 'cue2mix', 'mon') @('mon', 'cue2mix', 'own') { param($w) Bits $w[2] 27 1 } 1
    if (-not $twin) {
        [void](Takes $u 'CUE 4 mono' @('mon', 'cue4mono', '1')); [void](Takes $u 'CUE 4 mono off' @('mon', 'cue4mono', '0'))
    }

    # 4. Cue sends (DSP bus coefficients, no readback): input 1 and the talkback mic into the last cue
    $c = if ($twin) { 2 } else { 4 }
    [void](Takes $u "cue $c send of input 1 at -10 dB, pan -50" @('cue', "$c", 'send', '1', '-10', '-50'))
    [void](Takes $u "cue $c send of the talkback at -6 dB" @('cue', "$c", 'send', 'tb', '-6'))
    if (Takes $u "cue $c mute" @('cue', "$c", 'mute', '1')) {
        $l = (Diag $u @('log')).Text -split "`n" | Where-Object { $_ -match "cues: cue $c muted: (\d+) send\(s\) re-sent \(0x([0-9a-f]{8})\)" } | Select-Object -Last 1
        if ($l -and $l -match "muted: (\d+) send.*\(0x00000000\)" -and [int]$Matches[1] -ge 2) { Pass "$u cue $c mute re-sent its sends to the DSP rings: $($l.Trim())" }
        else { Fail "$u no clean 'cues: cue $c muted' log line (want 2 sends, status 0): '$l'" }
    }
    [void](Takes $u "cue $c unmute" @('cue', "$c", 'mute', '0'))
    [void](Takes $u "cue $c sends back off" @('cue', "$c", 'send', '1', 'off', '0'))
    [void](Takes $u "cue $c talkback send back off" @('cue', "$c", 'send', 'tb', 'off'))

    # 5. What the model lacks
    if ($twin) {
        Refuses $u 'speakers ALT 2' @('mon', 'alt', 'alt2') '50'
        Refuses $u 'ALT 2 output' @('mon', 'alt2', '0') '50'
        Refuses $u 'HP 2 source' @('mon', 'hp2src', 'cue1') '50'
        Refuses $u 'monitor source CUE 3' @('mon', 'source', 'cue3') '50'
        Refuses $u 'CUE 3 mono' @('mon', 'cue3mono', '0') '50'
        Refuses $u 'cue 3 send' @('cue', '3', 'send', '1', 'off') '50'
        Refuses $u 'cue 1 send of input 3' @('cue', '1', 'send', '3', 'off') '87'
    } else {
        Refuses $u 'HP 1 source CUE 3 (no known encoding)' @('mon', 'hp1src', 'cue3') '87'
    }
    $w1 = Words $u
    Note ("after: word 2 0x{0:X8}, word 5 0x{1:X8}, word 6 0x{2:X8}" -f $w1[2], $w1[5], $w1[6])
}

if (-not (Test-Path -LiteralPath $diag)) { Note "apollo-diag.exe not found at ${diag}: run tools\build-all.cmd"; exit 1 }
Note 'Speakers and headphones DOWN: the ALT and source toggles move audio between outputs (each is put back).'
if (-not $Yes) { Read-Host 'Press Enter to start (Ctrl+C to stop)' | Out-Null }
foreach ($u in @('x8p', 'twinx')) { if ($Unit -eq 'both' -or $Unit -eq $u) { Test-Unit $u } }
Note "step-mon: $($script:nPass) passed, $($script:nFail) failed, $($script:nInfo) info. Log: $log"
if ($script:nInfo) { Note 'INFO lines say which fields the readback echoes: send them back with the log.' }
if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }
exit ([int]($script:nFail -ne 0))
