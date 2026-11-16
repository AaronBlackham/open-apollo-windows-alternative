# chainfix-hw.ps1 - which part of the Sphere Mic Collection 180 + AKG BX 20 chain stopped x8p DSP 1 (Nov 15 2026).
# Run elevated (one UAC). PowerShell 5.1, ASCII only. Installs nothing, restarts nothing, writes no registry value.
#
# Steps, cheapest and least risky first; the run STOPS at the first quarantine (then only an x8p power cycle helps):
#   1  caps: DSP 1 admitted with its strips live, nothing quarantined, no instance (else stop, nothing sent)
#   2  AKG BX 20 alone, unrouted (create-plain on DSP 1): uploads incl. its kind-3 module to pool 3, zero-fills,
#      memspec; its code never runs. Proves the memory path alone leaves DSP 1 answering.
#   3  AKG BX 20 alone, routed (create-start, stereo on inputs 1/2, 10 s): the single-insert question.
#   4  a two-insert chain of plug-ins proven alone (Precision De-Esser + UA 1176LN Legacy), mono on input 1, 10 s
#   5  the incident's chain shape with the AKG swapped for a light plug-in: Sphere Mic Collection 180 + Precision
#      De-Esser, stereo on inputs 1/2, 3 startup pages staged, 10 s
#   -Upto N stops after step N; -From N skips the steps before N (2-5; step 1, the preconditions, always runs). Recipes: apollo-win\plugins\p1 (UA-derived, local only).
param([int]$Upto = 5, [int]$From = 2, [switch]$Yes)

$ErrorActionPreference = 'Continue'     # native tools write to stderr; never turn that into a throw
$root = 'C:\apollo-win'
$p1 = Join-Path $root 'plugins\p1'
$store = Join-Path $root 'plugins\store\modules'
$log = Join-Path $PSScriptRoot 'chainfix-hw.log'
"chainfix-hw $(Get-Date -Format s)" | Set-Content -LiteralPath $log -Encoding ascii
function Note([string]$s) { Write-Host $s; Add-Content -LiteralPath $log -Value $s -Encoding ascii }
function Done([int]$rc) { Note "chainfix-hw exit $rc"; if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }; exit $rc }

. (Join-Path $root 'tools\phase1\safe-tools.ps1')
$tools = Initialize-SafeTools $root @('tools\apollo-diag\apollo-diag.exe', 'tools\plugin-host\apollo-plug.exe') ${function:Note}
if (-not $tools) { Note 'Stopped before changing anything.'; Done 1 }
$diag = $tools['tools\apollo-diag\apollo-diag.exe']
$plug = $tools['tools\plugin-host\apollo-plug.exe']

function Plug([string[]]$a) {
    $a = @('--unit', 'x8p') + $a
    Note ">>> apollo-plug $($a -join ' ')"
    $out = (& $plug @a 2>&1 | Out-String)
    $rc = $LASTEXITCODE
    foreach ($l in ($out -split "`r?`n")) { if ($l.Length) { Note $l } }
    Note "<<< exit $rc"
    return $out
}
function Caps { return (& $plug --unit x8p caps 2>&1 | Out-String) }
function PluginLog {
    $l = (& $diag --unit x8p log 2>&1 | Out-String)
    Note '>>> apollo-diag --unit x8p log (plug-in and quarantine lines, the last 60)'
    $lines = @([regex]::Matches($l, '(?m)^.*\[x8p\] (plugins|strips: DSP \d+ quarantined|DSP \d+ (service|quarantined)).*$') | ForEach-Object { $_.Value.TrimEnd() })
    foreach ($x in ($lines | Select-Object -Last 60)) { Note $x }
}
function Quarantined([string]$caps) {
    $m = [regex]::Match($caps, 'quarantined 0x([0-9a-fA-F]+)')
    return (-not $m.Success) -or ([Convert]::ToInt32($m.Groups[1].Value, 16) -ne 0)
}
function Gate([string]$after) {
    $c = Caps
    foreach ($l in ($c -split "`r?`n")) { if ($l -match '^(DSPs|Instances|  DSP 1 )') { Note "  caps: $l" } }
    if (Quarantined $c) {
        PluginLog
        Note ''
        Note "NEEDS POWER CYCLE: DSP 1 quarantined after $after. Stopping here (nothing more is sent)."
        Done 3
    }
    if ($c -notmatch '(?m)^Instances\s+0\s*$') { Note "an instance is left after ${after}: stopping"; PluginLog; Done 2 }
}

# --- preconditions ---
$ac = @(Get-Process -Name 'apollo-control' -ErrorAction SilentlyContinue)
if ($ac.Count) {
    Note 'Apollo Control is running: asking it to close (its inserts come off with its handle).'
    foreach ($p in $ac) { [void]$p.CloseMainWindow() }
    Start-Sleep -Seconds 8
    if (@(Get-Process -Name 'apollo-control' -ErrorAction SilentlyContinue).Count) { Note 'Apollo Control did not close: stopping, nothing sent.'; Done 1 }
}
$c0 = Caps
foreach ($l in ($c0 -split "`r?`n")) { if ($l.Length) { Note "  $l" } }
if ((Quarantined $c0) -or $c0 -notmatch '(?m)^\s+DSP 1 .*admitted strips' -or $c0 -notmatch '(?m)^Instances\s+0\s*$' -or $c0 -notmatch 'started 0x[0-9a-f]*[37bf]\b') {
    Note 'Preconditions not met (DSP 1 admitted with strips, nothing quarantined, no instance, inputs 1-2 started): nothing sent.'
    Done 1
}
$akg = Join-Path $p1 'akg-bx-20-stereo-in1.recipe'
$c1m = Join-Path $p1 'chain-deesser+1176-mono-in1.recipe'
$c1s = Join-Path $p1 'chain-sphere180+deesser-stereo-in1.recipe'
foreach ($r in $akg, $c1m, $c1s) {
    if (-not (Test-Path -LiteralPath $r)) { Note "missing recipe $r"; Done 1 }
    $v = Plug @('modules', 'verify', $r)
    if ($v -notmatch '(?m)^PASS ' -or $v -match '(?m)^CHECK ') { [void](Plug @('modules', 'install', $r, '--from', $store)) }
}

# --- 2: the AKG alone, unrouted ---
if ($From -le 2) {
Note ''; Note '=== step 2: AKG BX 20 alone, unrouted on DSP 1 (its code never runs)'
[void](Plug @('create-plain', $akg, '--dsp', '1'))
Gate 'step 2 (AKG unrouted)'
}
if ($Upto -le 2) { PluginLog; Done 0 }

# --- 3: the AKG alone, routed ---
if ($From -le 3) {
Note ''; Note '=== step 3: AKG BX 20 alone, routed stereo on inputs 1/2 for 10 s'
[void](Plug @('create-start', $akg, '--dsp', '1', '--hold', '10', '--mon'))
Gate 'step 3 (AKG alone, routed)'
}
if ($Upto -le 3) { PluginLog; Done 0 }

# --- 4: two proven inserts, mono ---
Note ''; Note '=== step 4: Precision De-Esser + UA 1176LN Legacy chain, mono on input 1 for 10 s'
[void](Plug @('create-start', $c1m, '--dsp', '1', '--hold', '10', '--mon'))
Gate 'step 4 (two-insert chain, mono)'
if ($Upto -le 4) { PluginLog; Done 0 }

# --- 5: two proven inserts, stereo ---
Note ''; Note '=== step 5: Sphere Mic Collection 180 + Precision De-Esser chain, stereo on inputs 1/2 for 10 s (staged)'
[void](Plug @('create-start', $c1s, '--dsp', '1', '--hold', '10', '--mon'))
Gate 'step 5 (Sphere 180 + De-Esser chain, stereo)'
PluginLog
Note 'All steps ran with DSP 1 answering.'
Done 0
