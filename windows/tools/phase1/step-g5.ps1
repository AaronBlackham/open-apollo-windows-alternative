# Twin X gate G5 "Twin X inputs" (docs/spec-twinx.md section 7 and 6.9.5). ADMIN PowerShell.
#   powershell -ExecutionPolicy Bypass -File C:\Users\aaron\Desktop\Avium\apollo-win\tools\phase1\step-g5.ps1 [-Yes]
#   ... -Batch 2    the A/B of spec-twinx 4.3: MixerInitBatch 2 (legacy) instead of 1. Only if a mic that is plugged
#                   in still reads exact zero with batch 1. Read at the next Twin X power cycle.
#   ... -Rollback   the Twin X's strips off (StripsEnable 0) and its endpoints back to MON L/R only (EnabledEndpoints 3);
#                   Mode 2 stays (speakers on our driver). Read at the next Twin X power cycle.
#
# G5 = the Twin X's input strips (Mic Input Strips on DSP 1 for inputs 1-2; input 1 is also the Hi-Z jack), its preamp
# settings (gain, 48V, pad, low cut, phase, mic/line, link, Hi-Z impedance) and its Windows endpoints
# "MIC/LINE 1" and "MIC/LINE 2 (Apollo Twin X ...)".
#
# NEEDS the G5 apollo.sys already running both units (Twin X in Mode 2; `apollo-diag --unit twinx info` shows
# NumPreamps 2). This script installs nothing and restarts nothing: it writes the Twin X's own values
# (Parameters\Units\<its key>), which the driver reads at the Twin X's next start, and reads the result.
# Nothing here connects to Universal Audio or starts any UA program: the strip modules are the files apollo.sys
# already reads for the x8p (System32\drivers\ApolloStrips).
#
# Three runs, each after a power cycle of the TWIN X ONLY (the x8p stays up; strips need the Twin X's own cold start):
#   Run 1 (its StripsEnable is neither 2 nor 1): the dry run values: StripsEnable 2, StripSteps 0, StripMask 3,
#     StripDsp 1, MixerInitBatch 1, EnabledEndpoints 0xF (MON L/R + MIC/LINE 1-2). Saves the Windows default devices.
#   Run 2 (StripsEnable 2): checks the dry run (every gate passed, the plan logged per input, nothing sent), the
#     endpoints and the x8p; on PASS writes StripsEnable 1 (the full chain). Puts the default devices back.
#   Run 3 (StripsEnable 1): checks the strips (live, inputs 1-2 taken, 48V resync), a preamp setting round trip
#     (low cut on input 2 on and off, seen in readback), the readback; then asks you to plug a mic into input 1,
#     then input 2, then an instrument into the Hi-Z jack, and reads the record slots while you make sound.
# THIS SCRIPT NEVER PLAYS SOUND. A live strip feeds its input into the Twin X monitor mix at once, and the Twin X
# monitor outputs are the speakers: SPEAKERS OFF (or the Twin X monitor knob all the way down), headphones low,
# before every Twin X power cycle of this gate (docs/spec-twinx.md 5.2, feedback hazard).
# An empty jack reads EXACT ZERO on the Apollos (the unit mutes it): zero is only a finding with something plugged in.
# PowerShell 5.1, ASCII only.
#Requires -RunAsAdministrator
param([switch]$Yes, [ValidateSet('1', '2')][string]$Batch, [switch]$Rollback)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$log  = Join-Path $PSScriptRoot 'step-g5.log'
$defaultsFile = Join-Path $PSScriptRoot 'step-g5.defaults.json'
"step-g5 $(Get-Date -Format o) Batch=$Batch Rollback=$Rollback" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }
function Done([int]$rc) { Note "step-g5 exit $rc. Log: $log"; if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }; exit $rc }

$x8pHwid   = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00'
$twinxHwid = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00'
$params    = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters'
$twinxKeyG2 = '000000002F00BEEF'      # the Twin X's HW_UID read at gate G2 (Units\<key>); checked against the unit
# The test values (driver\apollo_unit.h Twin X defaults, docs/spec-twinx.md 6.9.5)
$stripSteps = 0                       # 0 = every stage: uploads ... span/route, routes to DSP 0, 48V resync
$stripMask  = 3                       # inputs 1-2 (strip_plan.h: the Twin X has no others)
$stripDsp   = 1                       # DSP 1: capture and the monitor mix (0 = capture only, an A/B)
$endpointsG5 = 0xF                    # MON L/R out + in, MIC/LINE 1 + 2 in (bringup_plan.h APOLLO_TWX_EP_ALL)
$endpointsG4 = 3                      # MON L/R only (the default)
$script:nPass = 0
$script:nCheck = 0
function Pass([string]$s) { $script:nPass++; Note "PASS   $s" }
function Check([string]$s) { $script:nCheck++; Note "CHECK  $s" }

# --- tools: apollo-diag only, from the verified admin-only copy (safe-tools.ps1); then the audio helpers ---
. (Join-Path $PSScriptRoot 'safe-tools.ps1')
$tools = Initialize-SafeTools $root @('tools\apollo-diag\apollo-diag.exe') ${function:Note}
if (-not $tools) { Note 'Stopped before changing anything.'; Done 1 }
$diag = $tools['tools\apollo-diag\apollo-diag.exe']
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
        Service    = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
        Version    = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
    }
}
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
function TwinxKey {
    $i = DiagQuiet @('--unit', 'twinx', 'info')
    $k = Field $i 'UnitKey'
    if ($k -match '^([0-9A-Fa-f]{16}|SN[0-9A-Za-z]+)$') { return $k }
    return $twinxKeyG2
}
function Save-Defaults {
    if (-not $audio) { return }
    try {
        $d = @(Get-AudioDefaults)
        Note 'Default audio devices now:'
        Format-AudioDefaults $d | ForEach-Object { Note $_ }
        ($d | ConvertTo-Json -Depth 3) | Out-File -Encoding ascii $defaultsFile
        Note "(saved to $defaultsFile for the next runs)"
    } catch { Note "note: could not read the default audio devices ($($_.Exception.Message))" }
}
# Windows makes a new endpoint the default when it first appears (the MIC/LINE rows): put the saved ones back.
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
    if ($moved -eq 0) { Note 'Default audio devices: as saved in run 1.' }
}
# The Twin X's bring-up outcome (Connected, FAILED, GONE). DSP ready can take 30 s, ACEFACE 20 s.
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
# The strip pass starts 500 ms after the bring-up, once the DSPs run at 48 kHz: wait until it is not Armed/Building.
# Idle with "cold start: DSP 1 owned" is the moment before the bring-up arms it: not an outcome yet.
function Wait-StripsOutcome([int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    Start-Sleep -Seconds 2
    do {
        $s = DiagQuiet @('--unit', 'twinx', 'strips')
        $st = FirstInt (Field $s 'State')
        Write-Host "  Twin X strips: $(Field $s 'State') - $(Field $s 'Why')"
        $preArm = ($st -eq 1 -and (Field $s 'Why') -match '^cold start')
        if ($st -ne 2 -and $st -ne 3 -and $st -ge 0 -and -not $preArm) { return $s }
        Start-Sleep -Seconds 2
    } while ((Get-Date) -lt $deadline)
    return $s
}
# Polls the Twin X readback (refreshed by the driver every 10 ms) until Pattern matches, up to Ms.
function Wait-Readback([string]$pattern, [int]$ms) {
    $deadline = (Get-Date).AddMilliseconds($ms)
    do {
        $r = DiagQuiet @('--unit', 'twinx', 'readback')
        if ([regex]::Match($r, $pattern).Success) { return $true }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    return $false
}
function RegistryValue([string]$stripsText, [string]$name) {
    $m = [regex]::Match((Field $stripsText 'Registry'), "\b$([regex]::Escape($name))=(0x[0-9A-Fa-f]+|\d+)")
    if (-not $m.Success) { return -1 }
    return (FirstInt $m.Groups[1].Value)
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
function Write-Values([string]$key, [int]$enable) {
    $w = @{}
    $w['StripsEnable'] = Set-UnitValue $key 'StripsEnable' $enable
    $w['StripSteps'] = Set-UnitValue $key 'StripSteps' $stripSteps
    $w['StripMask'] = Set-UnitValue $key 'StripMask' $stripMask
    $w['StripDsp'] = Set-UnitValue $key 'StripDsp' $stripDsp
    $w['MixerInitBatch'] = Set-UnitValue $key 'MixerInitBatch' $(if ($Batch -eq '2') { 2 } else { 1 })
    $w['EnabledEndpoints'] = Set-UnitValue $key 'EnabledEndpoints' $endpointsG5
    Note ("Units\$key`: StripsEnable={0} StripSteps={1} StripMask={2} StripDsp={3} MixerInitBatch={4} EnabledEndpoints=0x{5:X}" -f
          $w['StripsEnable'], $w['StripSteps'], $w['StripMask'], $w['StripDsp'], $w['MixerInitBatch'], $w['EnabledEndpoints'])
}
$safetyLines = @(
    'BEFORE THE POWER CYCLE: speakers OFF (or the Twin X monitor knob all the way down), headphones low. A live strip',
    'puts its input into the Twin X monitor mix the moment it starts, and the monitor outputs are your speakers (a mic',
    'in the same room can feed back). Nothing needs to be plugged into the Twin X inputs yet.'
)
function Show-PowerCycle([string]$next) {
    Note ''
    foreach ($l in $safetyLines) { Note "  $l" }
    Note '  1. Power the TWIN X off (its power switch or plug), wait 10 s, power it on. Leave the x8p on.'
    Note '     (A driver restart is not enough: strips need the Twin X''s own cold start.)'
    Note '  2. Wait about 60 s (its DSP-ready wait alone can take 30 s).'
    Note "  3. Run step-g5.ps1 again ($next). It plays nothing."
}

# ======================================================================================================
# Preconditions: both units on apollo.sys, the Twin X on the G5 driver in Mode 2
# ======================================================================================================
$x8p = Unit $x8pHwid
$twx = Unit $twinxHwid
if (-not $x8p) { Note 'The x8p is not connected: G5 needs it up (the Twin X is daisy-chained behind it).'; Done 1 }
if (-not $twx) { Note 'The Twin X is not connected or not powered on. Power it on, wait 60 s, run this again.'; Done 1 }
Note "x8p:    $($x8p.InstanceId) service $($x8p.Service) $($x8p.Version) $($x8p.Status)"
Note "Twin X: $($twx.InstanceId) service $($twx.Service) $($twx.Version) $($twx.Status)"
if ($twx.Service -ne 'apollo') { Note 'The Twin X is not on apollo.sys. G5 starts from G4 (the Twin X in Mode 2 on our driver).'; Done 1 }
$key = TwinxKey
if ($key -ne $twinxKeyG2) { Note "note: the Twin X's unit key is $key (G2 read $twinxKeyG2): using $key." }

# ======================================================================================================
# -Rollback
# ======================================================================================================
if ($Rollback) {
    $a = Set-UnitValue $key 'StripsEnable' 0
    $b = Set-UnitValue $key 'EnabledEndpoints' $endpointsG4
    Note "Units\$key`: StripsEnable = $a (strips off), EnabledEndpoints = $b (MON L/R only). Mode stays $(Get-UnitValue $key 'Mode')."
    Note 'It takes effect at the Twin X''s next start: power the TWIN X off, 10 s, on. Its MIC/LINE endpoints go away;'
    Note 'Windows picks another default microphone if one of them was the default. The speakers keep working (Mode 2).'
    Done 0
}
if ($Batch) {
    $v = Set-UnitValue $key 'MixerInitBatch' ([int]$Batch)
    Note "Units\$key`: MixerInitBatch = $v ($(if ($v -eq 2) { 'legacy: every setting 1-37 but S2 at 0x20/0xFF, the A/B of spec-twinx 4.3' } else { 'Twin X-safe skip list' }))."
    Note 'Read at the Twin X''s next start. Power-cycle the TWIN X (speakers off first), then run step-g5.ps1 again.'
    Done 0
}

$ti0 = DiagQuiet @('--unit', 'twinx', 'info')
$preamps = FirstInt (Field $ti0 'NumPreamps')
$mode = FirstInt (Field $ti0 'Mode')
if ($preamps -ne 2) {
    Note ''
    Note "The running apollo.sys is not the G5 build: the Twin X reports NumPreamps '$(Field $ti0 'NumPreamps')' (G5: 2)."
    Note 'Install the G5 package on both units first (as for G4: step-g4.ps1 -Package <the G5 package folder>, then both'
    Note 'units off and on), then run this again. Nothing changed.'
    Done 1
}
if ($mode -ne 2) {
    Note "The Twin X runs Mode '$(Field $ti0 'Mode')', not 2: G5 needs Mode 2 (its endpoints, settings and strips). Run"
    Note 'step-g4.ps1 first (it writes Mode 2), power-cycle the Twin X, then this again. Nothing changed.'
    Done 1
}

$askedEnable = Get-UnitValue $key 'StripsEnable'
if ($null -eq $askedEnable) { $askedEnable = 0 }

# ======================================================================================================
# RUN 1: the dry run values
# ======================================================================================================
if ($askedEnable -ne 2 -and $askedEnable -ne 1) {
    Note ''
    Note 'RUN 1: write the Twin X''s dry-run values (StripsEnable 2: every gate is checked and the whole pass is LOGGED,'
    Note 'nothing is sent to its DSPs) and its MIC/LINE 1-2 endpoints, then a Twin X power cycle.'
    Note 'Windows audio on the Twin X (the speakers) stops while it is off; the x8p is not touched.'
    if (-not $Yes) {
        $a = Read-Host '[Enter] go on, [Q] quit'
        if ($a -match '^[Qq]') { Note 'Nothing changed.'; Done 1 }
    }
    Save-Defaults
    try { Write-Values $key 2 } catch { Note "Could not write Units\$key ($($_.Exception.Message)). Nothing else changed."; Done 1 }
    Show-PowerCycle 'run 2: checks the dry run'
    Done 0
}

# Both later runs: the Twin X must have restarted on these values (the driver shows what it read).
Note ''
Note "RUN $(if ($askedEnable -eq 2) { 2 } else { 3 }): waiting for the Twin X bring-up (up to 120 s)..."
$ti = Wait-TwinxOutcome 120
$ts = Wait-StripsOutcome 60
Restore-SavedDefaults @(1, 3)
$readEnable = RegistryValue $ts 'StripsEnable'
if ($readEnable -ne $askedEnable) {
    Note ''
    Note "The Twin X driver runs StripsEnable=$readEnable, its key asks for ${askedEnable}: the Twin X has not restarted since"
    Note 'the values were written. Power the TWIN X off, 10 s, on (speakers off first), wait 60 s, run this again.'
    Done 1
}

Note ''
Note '=== G5 readings: the Twin X ==='
$ti  = Diag @('--unit', 'twinx', 'info')
$ts  = Diag @('--unit', 'twinx', 'strips')
$tep = Diag @('--unit', 'twinx', 'endpoints')
$null = Diag @('--unit', 'twinx', 'readback')
$tl  = DiagQuiet @('--unit', 'twinx', 'log')
$stripLines = @([regex]::Matches($tl, '(?m)^.*\[twx\] (strips|unit values|unit Mode 2|mixer init batch).*$') | ForEach-Object { $_.Value.TrimEnd() })
Note '>>> apollo-diag --unit twinx log (strip lines of this start)'
# the last start only: from the last 'strips: StripsEnable=' line on
$from = 0
for ($k = 0; $k -lt $stripLines.Count; $k++) { if ($stripLines[$k] -match 'strips: StripsEnable=') { $from = [Math]::Max(0, $k - 3) } }
for ($k = $from; $k -lt $stripLines.Count; $k++) { Note $stripLines[$k] }
Note '=== the x8p ==='
$xi = Diag @('--unit', 'x8p', 'info')
$xs = DiagQuiet @('--unit', 'x8p', 'strips')
$winEps = @()
if ($audio) {
    try { $winEps = @(Get-AudioEndpoints) } catch { Note "note: could not list the Windows endpoints ($($_.Exception.Message))" }
    Note '=== Windows audio endpoints of the Twin X ==='
    foreach ($e in $winEps) { if ($e.Name -like '*(Apollo Twin X*') { Note ("  {0} {1}" -f $(if ($e.Flow -eq 0) { 'out' } else { 'in ' }), $e.Name) } }
}

# ---------------------------------------------------------------- common checks ------------------------
Note ''
Note '=== G5 results (PASS = as expected; CHECK = look at it, then tell Claude) ==='
$flags = Field $ti 'Flags'
if ((FirstInt (Field $ti 'Stage')) -eq 5) { Pass 'Twin X Stage 5 Connected, Mode 2' } else { Check "Twin X Stage $(Field $ti 'Stage'), BringupFailStep $(Field $ti 'BringupFailStep')" }
if ($flags -notmatch '\bWARM_START\b') { Pass 'Twin X cold start (strips need one)' } else { Check 'Twin X WARM start: it was not powered off; strips stay off (power-cycle the Twin X itself)' }
if ((Field $ti 'NotifRateInfo') -match '^0x00000201\b') { Pass 'Twin X 48 kHz locked (0x201)' } else { Check "Twin X rate info $(Field $ti 'NotifRateInfo') (the strips wait for 48 kHz)" }
$sflags = Field $ts 'Flags'
foreach ($f in 'OWNED', 'MODULES_OK', 'BOOT_READ', 'LAYOUT_READ') {
    if ($sflags -match "\b$f\b") { Pass "strips flag $f" } else { Check "strips flag $f missing (Flags: $sflags)" }
}
foreach ($f in 'QUARANTINED', 'TAINTED') { if ($sflags -match "\b$f\b") { Check "strips flag ${f}: power-cycle the Twin X; send Claude the log" } }
$bw = [regex]::Match((Field $ts 'Boot words'), '^([0-9a-f]{8})\s+([0-9a-f]{8})')
if ($bw.Success) { Note "INFO   boot words DSP 0 0x$($bw.Groups[1].Value), DSP 1 0x$($bw.Groups[2].Value) (G3 cold: 00002703 00000003)" }
if ((Field $ts 'Layout') -match '\(code 0\)') { Pass 'DSP 1 pools match the recipe (code 0xE0000, tops 0x9D1B8 / 0xC6FFE / 0x08FFE000)' }
else { Check "DSP 1 pools: $(Field $ts 'Layout') (nothing was sent; send Claude the log)" }
if ((RegistryValue $ts 'StripMask') -eq 3 -and (RegistryValue $ts 'StripDsp') -eq 1) { Pass 'strips on inputs 1-2 (StripMask 0x03), DSP 1' }
else { Check "strips registry as the driver uses it: $(Field $ts 'Registry')" }
if ($tep -match "Endpoints\s+4 in the driver's table, EnabledEndpoints 0xF\b") { Pass 'Twin X driver table: 4 endpoints, EnabledEndpoints 0xF' }
else { Check "Twin X endpoint table: $(Field $tep 'Endpoints')" }
foreach ($c in 'ApolloTwxMon', 'ApolloTwxMonIn', 'ApolloTwxIn1', 'ApolloTwxIn2') {
    $row = [regex]::Match($tep, "(?m)^\s+\d+\s+(out|in)\s+$([regex]::Escape($c))\s+.*$")
    if ($row.Success -and $row.Value -notmatch 'not added|disabled') { Pass "Twin X circuit $c added" } else { Check "Twin X circuit $c`: $($row.Value.Trim())" }
}
$winIn = @{}
foreach ($n in 1, 2) {
    $e = @($winEps | Where-Object { $_.Flow -eq 1 -and $_.Name -like "MIC/LINE $n (Apollo Twin X*" })
    $winIn[$n] = $e
    if ($e.Count -eq 1) { Pass "Windows recording endpoint: $($e[0].Name)" } elseif ($audio) { Check "Windows endpoint 'MIC/LINE $n (Apollo Twin X ...)': $($e.Count) found" }
}
if ((FirstInt (Field $xi 'Stage')) -eq 5 -and (Field $xi 'Flags') -match '\bTRANSPORT\b') { Pass 'x8p: Connected, transport running (untouched)' } else { Check "x8p: Stage $(Field $xi 'Stage'), Flags $(Field $xi 'Flags')" }
if ((Field $xs 'State') -match '\bLive\b') { Pass 'x8p strips: Live (untouched)' } else { Note "INFO   x8p strips: $(Field $xs 'State') (a warm restart of the x8p leaves them off until its next power cycle)" }

# ======================================================================================================
# RUN 2: the dry run
# ======================================================================================================
if ($askedEnable -eq 2) {
    $dryOk = $true
    $state = Field $ts 'State'
    $why = Field $ts 'Why'
    if ((FirstInt $state) -eq 1 -and $why -match 'dry run complete') { Pass "dry run: every gate passed, nothing sent ($why)" }
    else { Check "dry run: strips $state, '$why'"; $dryOk = $false }
    foreach ($n in 1, 2) {
        $l = $stripLines | Where-Object { $_ -match "dry run plan: input $n = DSP input $($n - 1) -> record slot (-?\d+)" } | Select-Object -Last 1
        if ($l -and $l -match "record slot $($n - 1) ") { Pass "dry run plan: input $n -> DSP input $($n - 1), record slot $($n - 1)" }
        else { Check "dry run plan line for input $n`: $l"; $dryOk = $false }
    }
    $r = $stripLines | Where-Object { $_ -match 'route 0x1ffff to DSP mask 0x01' } | Select-Object -First 1
    if ($r) { Pass 'dry run plan: route words to DSP 0 only (a Duo)' } else { Check 'dry run plan: no "route ... DSP mask 0x01" line'; $dryOk = $false }
    foreach ($b in 'MODULES_OK', 'OWNED') { if ($sflags -notmatch "\b$b\b") { $dryOk = $false } }
    if ((Field $ts 'Layout') -notmatch '\(code 0\)') { $dryOk = $false }
    Note ''
    Note "Summary: $script:nPass PASS, $script:nCheck CHECK."
    if (-not $dryOk) {
        Note 'The dry run did not pass every gate (see the CHECK lines): the full chain is NOT armed. Send Claude step-g5.log.'
        if ($sflags -notmatch '\bMODULES_OK\b') {
            Note 'Strip modules missing or not the pinned bytes: the x8p uses the same files (System32\drivers\ApolloStrips).'
            Note "Copy them again (reads your local UAD files offline, connects to nothing): `"$diag`" strips install"
        }
        if ($flags -match '\bWARM_START\b' -or $sflags -notmatch '\bOWNED\b') { Note 'Not owned: the Twin X needs a real power cycle (off 10 s, on), then run 2 again.' }
        Done 1
    }
    Note ''
    Note 'Dry run passed. Next: the full chain (StripsEnable 1): the five modules, both strips, the routes, the 48V resync.'
    if (-not $Yes) {
        $a = Read-Host '[Enter] arm the full chain for the next Twin X start, [Q] quit (nothing changes)'
        if ($a -match '^[Qq]') { Note 'Nothing changed: StripsEnable stays 2 (dry run).'; Done 0 }
    }
    try { Write-Values $key 1 } catch { Note "Could not write Units\$key ($($_.Exception.Message))."; Done 1 }
    Show-PowerCycle 'run 3: checks the strips, then asks you to plug in a mic'
    Done 0
}

# ======================================================================================================
# RUN 3: the full chain
# ======================================================================================================
$state = Field $ts 'State'
if ((FirstInt $state) -eq 4) { Pass "strips live: $(Field $ts 'Why')" } else { Check "strips: $state, '$(Field $ts 'Why')'" }
if ((Field $ts 'Stages') -match 'done 9\b') { Pass 'strip stages: done 9 (everything, 48V resync included)' } else { Check "strip stages: $(Field $ts 'Stages')" }
$cnt = Field $ts 'Counters'
if ($cnt -match 'replies ok 5, bad 0\), timeouts 0') { Pass "modules: 5 uploads accepted by DSP 1 ($cnt)" } else { Check "modules: $cnt" }
foreach ($n in 1, 2) {
    $row = [regex]::Match($ts, "(?m)^$n\s+taken\s+sent\s+0x([0-9a-f]+).*$")
    if ($row.Success) { Pass "input $n`: Mic Input Strip taken and started (header 0x$($row.Groups[1].Value))" } else { Check "input $n strip row missing or not taken (see strips)" }
}
if ((Field $ts 'Routes') -match '^\s*2\b') { Pass 'route words: 2 (inputs 1-2 to DSP 0)' } else { Check "routes: $(Field $ts 'Routes')" }
Note "INFO   48V resync: $(Field $ts '48V resync')"

# A preamp setting round trip on the (empty) input 2: low cut on, seen in readback, then off. Silent on an empty jack.
$lc = DiagQuiet @('--unit', 'twinx', 'lowcut', '2', '1')
$on = Wait-Readback '(?m)^\s+preamp 2: .*LowCut 1\b' 3000
$null = DiagQuiet @('--unit', 'twinx', 'lowcut', '2', '0')
$off = Wait-Readback '(?m)^\s+preamp 2: .*LowCut 0\b' 3000
if ($lc -match 'queued' -and $on -and $off) { Pass 'preamp settings: low cut on input 2 on, then off, both seen in the readback' }
else { Check "preamp settings round trip: set '$($lc.Trim())', readback on $on, off $off" }
$hz = DiagQuiet @('--unit', 'twinx', 'hiz', '1', '2.2M')
if ($hz -match 'queued') { Pass 'Hi-Z impedance of input 1: 2.2M (code 0) taken (no readback field exists for it)' } else { Check "hiz 1 2.2M: $($hz.Trim())" }
$no = DiagQuiet @('--unit', 'twinx', 'gain', '3', '40')
if ($no -notmatch 'queued') { Pass 'preamp 3 refused (the Twin X has 2)' } else { Check 'gain on preamp 3 was accepted: tell Claude' }

Note ''
Note "Summary so far: $script:nPass PASS, $script:nCheck CHECK."
if ((FirstInt $state) -ne 4) {
    Note 'The strips are not live (see above), so no input test. Send Claude step-g5.log.'
    Note "Back to G4 (strips off, MON endpoints only): step-g5.ps1 -Rollback, then power-cycle the Twin X."
    Done 1
}
if ($Yes) {
    Note '-Yes: the input tests need you at the Twin X; run step-g5.ps1 again without -Yes for them.'
    Done 0
}

# ---------------------------------------------------------------- the inputs, one question at a time -----
function Test-Input([int]$n, [string]$what, [int]$slot) {
    Note ''
    $a = Read-Host "Plug $what into Twin X input $n now (headphones low, speakers still off). [Enter] when it is in, [S] skip input $n"
    if ($a -match '^[Ss]') { Note "INFO   input $n skipped"; return }
    if ($n -le 2 -and $what -like 'a microphone*') {
        $p = Read-Host 'Does this microphone need 48V phantom power (a condenser)? Never for a ribbon mic. [y/N]'
        if ($p -match '^[Yy]') {
            $o = DiagQuiet @('--unit', 'twinx', '48v', "$n", '1')
            Note "48V on input $n`: $($o.Trim())"
            if (Wait-Readback "(?m)^\s+preamp $n`: .*48V 1\b" 3000) { Pass "48V on input $n (readback)" } else { Check "48V on input $n not seen in the readback" }
            Note 'Waiting 5 s: the 48V switch mutes the input for a moment.'
            Start-Sleep -Seconds 5
        }
    }
    $null = Read-Host "Now make sound into input $n ($(if ($what -like 'an instrument*') { 'play it' } else { 'speak or sing' })) and keep going; [Enter] starts a 5 s reading"
    $lv = DiagQuiet @('--unit', 'twinx', 'levels', '5')
    Note ">>> apollo-diag --unit twinx levels 5 (record slot $slot = input $n)"
    $lines = @($lv -split "`r?`n" | Where-Object { $_ -match '^pos ' })
    foreach ($l in $lines) { $m = [regex]::Match($l, "(?:\s)$slot`:(zero|-?[\d.]+)"); if ($m.Success) { Note "  slot $slot`: $($m.Groups[1].Value)" } }
    $pk = SlotPeak $lv $slot
    if ($null -ne $pk) {
        Pass ("input $n reaches record slot $slot`: peak {0:N1} dBFS" -f $pk)
        if ($pk -lt -60) { Note "       (quiet: raise the gain, e.g. `"$diag`" --unit twinx gain $n 45)" }
    } else {
        Check "input $n read EXACT ZERO on record slot $slot with $what plugged in and sound going in"
        Note '       Check: is it plugged into the right jack? A condenser needs 48V (above). Then the A/B of spec-twinx 4.3:'
        Note '       step-g5.ps1 -Batch 2, power-cycle the Twin X (speakers off), run step-g5.ps1 again.'
    }
}

Note ''
Note '=== The inputs (each question asks one thing; nothing plays) ==='
Note 'An empty jack reads exact zero ("zero"): that is the Apollo muting it, not a fault.'
Test-Input 1 'a microphone (the front MIC/LINE 1 combo jack)' 0
Test-Input 2 'a microphone (MIC/LINE 2)' 1
$a = Read-Host 'Hi-Z next: unplug the mic from input 1, plug an instrument cable (guitar, bass) into the front Hi-Z jack. [Enter] when it is in, [S] skip'
if ($a -notmatch '^[Ss]') {
    if (Wait-Readback '(?m)^\s+preamp 1: .*Hi-Z jack: cable in' 3000) { Pass 'Hi-Z jack: the cable is seen (readback word 0 bit 24)' }
    else { Check 'Hi-Z jack: no cable seen in the readback (word 0 bit 24)' }
    $null = Read-Host 'Play the instrument and keep going; [Enter] starts a 5 s reading'
    $lv = DiagQuiet @('--unit', 'twinx', 'levels', '5')
    $pk = SlotPeak $lv 0
    if ($null -ne $pk) { Pass ("Hi-Z input reaches record slot 0: peak {0:N1} dBFS (same strip as input 1)" -f $pk) }
    else { Check 'Hi-Z input read exact zero on record slot 0 while playing' }
}
Diag @('--unit', 'twinx', 'readback') | Out-Null
Restore-SavedDefaults @(1)

Note ''
Note "Summary: $script:nPass PASS, $script:nCheck CHECK. Send Claude step-g5.log."
Note 'Windows: "MIC/LINE 1" and "MIC/LINE 2 (Apollo Twin X ...)" are recording devices now (Sound settings ->'
Note 'Input shows their meters). ASIO reaches the Twin X through the combined device (gate G6), not this gate.'
Note 'Optional by hand: the talkback mic: "apollo-diag --unit twinx mon talkback 1" then 0 (readback word 6 bit 8; it has no'
Note 'strip yet, so TALKBACK 1-2 record silence).'
Note 'Strips stay on at every Twin X cold start from now on. Back to G4 (strips off, MON endpoints only):'
Note '  step-g5.ps1 -Rollback, then power-cycle the Twin X.'
Done 0
