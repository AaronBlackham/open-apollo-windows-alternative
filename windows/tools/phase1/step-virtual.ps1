# step-virtual.ps1 - [Virtual] the first hardware run of the Virtual Input Strips (docs/STRIPS.md 13), VIRTUAL 1 only,
# on the x8p. One UAC (ADMIN PowerShell), about 3 minutes:
#   powershell -ExecutionPolicy Bypass -File tools\phase1\step-virtual.ps1 [-Package <dir>] [-SkipInstall] [-Yes]
#
# Steps; it stops at the first thing that is not right and says why, and at a quarantine it prints NEEDS POWER CYCLE:
#   1. apollo-diag --unit x8p params: StripsEnable must be 1 (the Mic strips run); StripMask/StripSteps are shown.
#   2. apollo-diag --unit x8p param StripVirtualMask 1 (Units\<x8p key>; read at the unit's next start).
#   3. step-ep.ps1 -NoTone [-Package]: installs the build on both units; their restart is a WARM start.
#   4. waits for the x8p's strips (up to 60 s). The warm start rebuilds them when the record proves them this driver's
#      (StripWarmRebuild, x8p default 1: the record of the build before has no VIRTUAL 1, so the plan differs). If it
#      cannot (no anchored record), it asks for ONE x8p power cycle and waits again (up to 120 s). VIRTUAL 1 taken
#      with its SYNCH sent = go on. Quarantined or FAILED = NEEDS POWER CYCLE, stop.
#   5. the driver log lines of the pass (VIRTUAL 1 prepare / start / bus).
#   6. tone: apollo-diag tone 8 s, 997 Hz at -30 dBFS on playback slots 20/21 = VIRTUAL 1/2 in the x8p's descriptor (the
#      VIRTUAL 1-2 endpoint in driver\endpoint_plan.h is a RECORDING endpoint, slots 18/19: Windows has no render
#      endpoint for the Virtual channels, so wasapi-loop cannot play into them), while apollo-diag levels reads record
#      slots 18/19 (VIRTUAL 1/2 as the DSP returns them: VIRTUAL 1 has its Input Strip, VIRTUAL 2 none, so 19 should
#      stay exact zero), 26/27 (MON, the monitor mix) and 28/29 (AUX 1).
#   7. the mixer on bus 0x18 (all DSP bus coefficients, put back after): mix v1 mute 1 then 0 (MON follows);
#      aux 1 send v1 0 dB (AUX 1 record slots 28/29 then carry the tone), then off; cue 1 send v1 -20 dB, then off
#      (CUE 1 = HP 1: no record slot carries a cue on the x8p, the driver's acceptance is what is checked; listen on HP 1).
#   8. strips again: still live, nothing quarantined.
# SPEAKERS: VIRTUAL 1 joins the monitor mix at the DSP's own level the moment its strip runs, so the -30 dBFS tone is on
# the monitor outputs: turn the x8p's monitor knob down before step 6.
# Leaves StripVirtualMask=1 in Units\<x8p key> (the build's default is 0); 'apollo-diag --unit x8p param StripVirtualMask
# --remove' (elevated) and the unit's next start take it away. PowerShell 5.1, ASCII only. Log: step-virtual.log.
param([string]$Package = '', [switch]$SkipInstall, [switch]$Yes)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$log = Join-Path $PSScriptRoot 'step-virtual.log'
"step-virtual $(Get-Date -Format o) Package=$Package SkipInstall=$SkipInstall" | Out-File -Encoding ascii $log
$script:nPass = 0; $script:nFail = 0
function Note([string]$s) { Write-Host $s; Add-Content -Encoding ascii $log $s }
function Pass([string]$s) { $script:nPass++; Note "PASS  $s" }
function Fail([string]$s) { $script:nFail++; Note "FAIL  $s" }
function Done([int]$rc) {
    Note ("step-virtual exit {0}: {1} pass, {2} fail. Log: {3}" -f $rc, $script:nPass, $script:nFail, $log)
    if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }
    exit $rc
}

. (Join-Path $PSScriptRoot 'safe-tools.ps1')
$tools = Initialize-SafeTools $root @('tools\apollo-diag\apollo-diag.exe') ${function:Note}
if (-not $tools) { Note 'Stopped before changing anything.'; Done 1 }
$diag = $tools['tools\apollo-diag\apollo-diag.exe']

function DiagQuiet([string[]]$a) { return (& $diag --unit x8p @a 2>&1 | Out-String) }
function Diag([string[]]$a) {
    $o = DiagQuiet $a
    Note ">>> apollo-diag --unit x8p $($a -join ' ') (exit $LASTEXITCODE)"
    Note $o.TrimEnd()
    return $o
}
# One mixer write; a refusal is a FAIL with the driver's answer.
function Mixer([string]$what, [string[]]$a) {
    $o = DiagQuiet $a
    $ok = ($LASTEXITCODE -eq 0)
    if ($ok) { Note "  $what`: sent" } else { Fail "$what refused: $($o.Trim())" }
    return $ok
}
# Peak of one record slot over an 'apollo-diag levels' run: dBFS, or $null when every reading was exact zero.
function SlotPeak([string]$levelsText, [int]$slot) {
    $best = $null
    foreach ($m in [regex]::Matches($levelsText, "(?:\s|\|)$slot`:(zero|-?[\d.]+)")) {
        if ($m.Groups[1].Value -eq 'zero') { continue }
        $v = [double]$m.Groups[1].Value
        if ($null -eq $best -or $v -gt $best) { $best = $v }
    }
    return $best
}
function Db($v) { if ($null -eq $v) { return 'exact zero' } return ('{0:0.0} dBFS' -f $v) }
function Field([string]$text, [string]$name) {
    $m = [regex]::Match($text, "(?m)^$([regex]::Escape($name))\s+(.*?)\s*$")
    if ($m.Success) { return $m.Groups[1].Value } else { return '' }
}
function Quarantined([string]$s) { return ((Field $s 'Flags') -match 'QUARANTINED') }
function V1Row([string]$s) { return [regex]::Match($s, '(?m)^V1\s+(\S+)\s+(\S+)\s+0x18\s+(0x[0-9a-fA-F]+)\s.*$') }
function StopPowerCycle([string]$why, [string]$s) {
    Note $s.TrimEnd()
    Note ''
    Note "NEEDS POWER CYCLE: $why. Stopping here (nothing more is sent)."
    Done 3
}
# Tone on playback slots 20/21 in the background, levels read meanwhile; returns the levels text.
function ToneLevels([string]$label) {
    $out = Join-Path $env:TEMP 'step-virtual-tone.txt'
    $tone = Start-Process -FilePath $diag -ArgumentList @('--unit', 'x8p', 'tone', '8', '--slots', '20,21', '--freq', '997', '--level', '-30') `
                          -PassThru -NoNewWindow -RedirectStandardOutput $out
    Start-Sleep -Seconds 2
    $lv = DiagQuiet @('levels', '4')
    if (-not $tone.WaitForExit(20000)) { Stop-Process -Id $tone.Id -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $out) { Add-Content -Encoding ascii $log ("  tone ($label): " + ((Get-Content -Raw -LiteralPath $out) -as [string]).Trim()) }
    Add-Content -Encoding ascii $log "  levels ($label):"
    Add-Content -Encoding ascii $log $lv.TrimEnd()
    $r = @{}
    foreach ($s in 18, 19, 26, 27, 28, 29) { $r[$s] = SlotPeak $lv $s }
    Note ("  {0}: slot 18 (VIRTUAL 1) {1}, 19 (VIRTUAL 2) {2}, 26/27 (MON) {3} / {4}, 28/29 (AUX 1) {5} / {6}" -f $label,
          (Db $r[18]), (Db $r[19]), (Db $r[26]), (Db $r[27]), (Db $r[28]), (Db $r[29]))
    return $r
}

# ---- 1. the x8p, its values
$info = DiagQuiet @('info')
if ($LASTEXITCODE -ne 0 -or $info -notmatch 'Mode\s+2') { Note 'The x8p is not present on apollo.sys in Mode 2: nothing changed.'; Note $info.TrimEnd(); Done 1 }
$params = Diag @('params')
$en = [regex]::Match($params, '(?m)^\s+StripsEnable\s+(\d+)')
if (-not $en.Success -or $en.Groups[1].Value -ne '1') {
    Note 'StripsEnable is not 1 on the x8p: the Virtual strips are built with the Mic strips, so there would be nothing. Nothing changed.'
    Done 1
}

# ---- 2. StripVirtualMask 1
$o = Diag @('param', 'StripVirtualMask', '1')
if ($LASTEXITCODE -ne 0) { Note 'Could not store StripVirtualMask (elevated? a unit key?). Nothing else changed.'; Done 1 }

# ---- 3. the build
if ($SkipInstall) {
    Note 'Install skipped (-SkipInstall): the value applies at the x8p''s next start (restart or power cycle it now if it has not).'
} else {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'step-ep.ps1'), '-NoTone', '-Yes')
    if ($Package) { $a += @('-Package', $Package) }
    Note ">>> step-ep.ps1 -NoTone $(if ($Package) { "-Package $Package" })"
    & powershell @a
    $rc = $LASTEXITCODE
    Note "step-ep exit $rc (its log: step-ep.log)"
    if ($rc -ne 0) { Note 'The install did not finish: stopping (see step-ep.log). StripVirtualMask=1 stays stored for the next start.'; Done 1 }
}

# ---- 4. the strips: VIRTUAL 1 taken and started
function WaitV1([int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $s = ''
    do {
        Start-Sleep -Seconds 3
        $s = DiagQuiet @('strips')
        if (Quarantined $s) { return $s }
        $st = Field $s 'State'
        $row = V1Row $s
        if ($row.Success -and $row.Groups[1].Value -eq 'taken' -and $row.Groups[2].Value -eq 'sent') { return $s }
        if ($st -match '^(0|5)\b' -and (Get-Date) -gt $deadline.AddSeconds(-$seconds + 20)) { return $s }   # off or failed: settled
    } while ((Get-Date) -lt $deadline)
    return $s
}
Note 'Waiting for the x8p''s strips (up to 60 s)...'
$s = WaitV1 60
if (Quarantined $s) { StopPowerCycle 'the strip DSP is quarantined' $s }
$row = V1Row $s
if (-not ($row.Success -and $row.Groups[1].Value -eq 'taken' -and $row.Groups[2].Value -eq 'sent')) {
    Note $s.TrimEnd()
    Note ''
    Note ("VIRTUAL 1 is not live after this start (State {0}; Why: {1})." -f (Field $s 'State'), (Field $s 'Why'))
    if ((Field $s 'State') -match '^5\b') { StopPowerCycle 'the strips FAILED' $s }
    Note 'POWER-CYCLE THE x8p NOW: switch it off, wait 5 seconds, switch it on (a cold start builds every strip).'
    if (-not $Yes) { Read-Host 'Press Enter once the x8p is back on' | Out-Null } else { Start-Sleep -Seconds 30 }
    Note 'Waiting for the x8p''s cold start and its strips (up to 120 s)...'
    $s = WaitV1 120
    if (Quarantined $s) { StopPowerCycle 'the strip DSP is quarantined' $s }
    $row = V1Row $s
    if (-not ($row.Success -and $row.Groups[1].Value -eq 'taken' -and $row.Groups[2].Value -eq 'sent')) {
        StopPowerCycle 'VIRTUAL 1 is not live after a cold start either (the strips output above says why)' $s
    }
}
Note $s.TrimEnd()
Pass ("VIRTUAL 1's Input Strip: taken, SYNCH sent, header {0} (the plan: 0x9cf10)" -f $row.Groups[3].Value)
if ($row.Groups[3].Value -ne '0x9cf10') { Fail "VIRTUAL 1's header is $($row.Groups[3].Value), not the planned 0x9cf10" }
if ((Field $s 'Virtual') -notmatch '0x01 built') { Fail "the Virtual line does not say 0x01 built: $(Field $s 'Virtual')" }
$mic = [regex]::Matches($s, '(?m)^[1-8]\s+taken\s+sent\s').Count
Note "  Mic strips taken and started: $mic (StripMask in force: $(Field $s 'Registry'))"

# ---- 5. the pass in the driver's log
$l = DiagQuiet @('log')
Note '>>> apollo-diag --unit x8p log (VIRTUAL and warm-start lines, the last 30)'
foreach ($x in @([regex]::Matches($l, '(?m)^.*\[x8p\] strips: (VIRTUAL|.*Virtual|warm start|this pass|adopted|record).*$') |
                 ForEach-Object { $_.Value.TrimEnd() } | Select-Object -Last 30)) { Note $x }

# ---- 6. tone into VIRTUAL 1/2
Note ''
Note 'Turn the x8p monitor knob DOWN now: a -30 dBFS tone goes into VIRTUAL 1-2, and VIRTUAL 1 is in the monitor mix.'
if (-not $Yes) { Read-Host 'Press Enter to play 8 s of tone' | Out-Null }
$quiet = DiagQuiet @('levels', '2')
Add-Content -Encoding ascii $log "  levels (before the tone):"
Add-Content -Encoding ascii $log $quiet.TrimEnd()
$t0 = ToneLevels 'tone, as built'
if ($null -ne $t0[18] -and $t0[18] -gt -40) { Pass "VIRTUAL 1 records what plays into it: slot 18 at $(Db $t0[18])" }
else { Fail "VIRTUAL 1 (slot 18) reads $(Db $t0[18]) with the tone on playback slot 20: the Input Strip passes nothing (docs/STRIPS.md 13.8)" }
if ($null -eq $t0[19]) { Pass 'VIRTUAL 2 (no strip) stays exact zero: the strip is what makes VIRTUAL 1 carry it' }
else { Note "INFO  VIRTUAL 2 (no strip) reads $(Db $t0[19]) (expected exact zero: is a Virtual strip left from another host?)" }
if ($null -ne $t0[26] -or $null -ne $t0[27]) { Pass "the monitor mix carries VIRTUAL 1: MON $(Db $t0[26]) / $(Db $t0[27])" }
else { Fail 'the monitor mix (slots 26/27) stays exact zero with VIRTUAL 1 live' }
$s = DiagQuiet @('strips')
if (Quarantined $s) { StopPowerCycle 'the strip DSP is quarantined after the tone' $s }

# ---- 7. the mixer on bus 0x18
Note ''
Note 'Mixer on VIRTUAL 1 (bus 0x18): mute in the monitor mix, then unmute'
$muted = $false; $auxSet = $false; $cueSet = $false
try {
    if (Mixer 'mix v1 mute 1' @('mix', 'v1', 'mute', '1')) {
        $muted = $true
        $t1 = ToneLevels 'VIRTUAL 1 muted in the monitor mix'
        if ($null -ne $t0[26] -and ($null -eq $t1[26] -or $t1[26] -lt $t0[26] - 10)) { Pass "mix mute of VIRTUAL 1: MON from $(Db $t0[26]) to $(Db $t1[26])" }
        else { Fail "mix mute of VIRTUAL 1 did not lower MON (from $(Db $t0[26]) to $(Db $t1[26]))" }
        if ($null -ne $t1[18]) { Pass "... VIRTUAL 1 still records ($(Db $t1[18])): the mute is the monitor mix's only" }
        if (Mixer 'mix v1 mute 0' @('mix', 'v1', 'mute', '0')) { $muted = $false }
    }
    Note 'AUX 1 send of VIRTUAL 1 at 0 dB (pre-fader): AUX 1 records on slots 28/29'
    if (Mixer 'aux 1 send v1 0' @('aux', '1', 'send', 'v1', '0')) {
        $auxSet = $true
        $t2 = ToneLevels 'VIRTUAL 1 sent to AUX 1'
        if ($null -ne $t2[28] -or $null -ne $t2[29]) {
            Pass "AUX 1 carries VIRTUAL 1: slots 28/29 $(Db $t2[28]) / $(Db $t2[29]) (before the send $(Db $t0[28]) / $(Db $t0[29]))"
        } else {
            Fail 'AUX 1 (slots 28/29) stays exact zero with VIRTUAL 1 sent to it at 0 dB'
        }
        if (Mixer 'aux 1 send v1 off' @('aux', '1', 'send', 'v1', 'off')) { $auxSet = $false }
    }
    Note 'CUE 1 send of VIRTUAL 1 at -20 dB: no record slot carries a cue on the x8p (CUE 1 = HP 1: listen there)'
    if (Mixer 'cue 1 send v1 -20' @('cue', '1', 'send', 'v1', '-20')) {
        $cueSet = $true
        Pass 'the driver takes a cue send of bus 0x18 on this unit (StripVirtualMask 1)'
        $null = ToneLevels 'VIRTUAL 1 in CUE 1 (HP 1)'
        if (Mixer 'cue 1 send v1 off' @('cue', '1', 'send', 'v1', 'off')) { $cueSet = $false }
    }
    $o = DiagQuiet @('mix', 'v2', 'level', '0')
    if ($LASTEXITCODE -ne 0) { Pass 'VIRTUAL 2 (no strip) is refused by the mixer, as it should be' }
    else { Fail 'the mixer took VIRTUAL 2 without its strip' }
} finally {
    if ($muted) { $null = DiagQuiet @('mix', 'v1', 'mute', '0') }
    if ($auxSet) { $null = DiagQuiet @('aux', '1', 'send', 'v1', 'off') }
    if ($cueSet) { $null = DiagQuiet @('cue', '1', 'send', 'v1', 'off') }
}

# ---- 8. strips again
$s = Diag @('strips')
if (Quarantined $s) { StopPowerCycle 'the strip DSP is quarantined at the end' $s }
$row = V1Row $s
if ($row.Success -and $row.Groups[1].Value -eq 'taken') { Pass 'VIRTUAL 1 still live, nothing quarantined' } else { Fail 'VIRTUAL 1 is no longer taken' }
Done ($(if ($script:nFail -eq 0) { 0 } else { 2 }))
