# Gate P1 "one UAD plug-in on the x8p's DSP" (docs/PLUGINS.md 4.1). ADMIN PowerShell.
#   powershell -ExecutionPolicy Bypass -File C:\apollo-win\tools\phase1\step-p1.ps1
#   ... -Step licence    only the licence read and CREATE CHECK (warm is fine)
#   ... -Step plain      + the unrouted CREATE on DSP 2, then DESTROY (needs a cold-started x8p)
#   ... -Step start      only START+CHAIN on DSP 1 input 1 (needs a cold-started x8p, strips live)
#   ... -Recipe NAME     the plug-in: a short name (lexicon-480l, ampeg-svtvr-classic: plugins\p1\<name>-mono-in1.recipe)
#                        or a recipe file. Default: ampeg-svtvr-classic (the smallest staged startup, 3 pages).
#   ... -Hold 20         seconds the chain runs while measuring (the second run, ended by closing the handle, holds 10)
#   ... -Unit twinx      (P7) the same gate on the Twin X Gen 1, input 1: its own licence answer, its DSP 1 (a Duo: the
#                        unrouted step runs on DSP 1 itself, the strip DSP, nothing routed), its 2 strips; recipes
#                        plugins\p1\<name>-twinx-mono-in1.recipe (p1_recipe.py --unit twinx), else the x8p's input-1 file
#                        (the same words). A Twin X power cycle drops only the Twin X; its monitor mix is the SPEAKERS.
#
# A recipe whose startup is over one 1016-word page (the Lexicon 480L: 64 pages; the Ampeg SVTVR Classic: 3) is
# STAGED, as open-apollo's daemon stages it: CREATE STAGE+CHAIN with the first page while input 1's plain strip keeps
# running, the other pages one SERVICE write each, the whole startup read back, and only then START routes it. Any
# failure before START takes the chain down without input 1 ever changing; closing this window does the same.
# The effect is measured as the recipe says (p1_recipe.py --effect): a reverb by the tail after short sounds (clap),
# an amp by input 1's level and spectrum against the same source on input 2, a latency by the record lag.
# The readback never compares the words the running DSP writes itself: the recipe's dsp_owned lines (bundle, meters,
# plugins\p1\dsp-owned.txt), and a word whose value moves between two reads (apollo-plug says to add it to that file).
# After each restore apollo-plug checks the driver's state, not the sound: strips Live with input 1's plain strip
# started, no instance left, DSP 1's ledger back to its baseline (derived from the pool registers when this is the
# first CREATE after a power cycle) and GET_STRIPS; input 1 itself only live/silent (exact zero) and, for a reverb,
# its tail gone. Input 1's level and lag against input 2 after the restore are no longer compared (Nov 15: two mic
# outputs differ acoustically, which says nothing about the teardown). A stereo recipe (a file made with p1_recipe.py
# --layout stereo) needs a driver with stereo inserts; its effect is still measured on its first input.
#   ... -Off             PluginHost back to 0 (plug-in hosting refused from the next call on; nothing else changes)
#   ... -KeepOn          leave PluginHost = 1 at the end (by default every exit puts back the value it found, so no
#                        other program can place or start plug-ins once this script is done)
#
# NEEDS the P0 apollo.sys running the x8p (apollo-plug caps answers). Installs no driver, restarts nothing.
# It writes Services\apollo\Parameters\PluginHost = 1 (the driver reads it at every call), copies the recipe's
# modules into the admin-only module store (System32\drivers\ApolloModules, from plugins\store\modules: bytes cut
# from your own UAD install, verified by SHA-256), and runs apollo-plug.exe (the verified copy) for each step.
# NOTHING HERE CONNECTS TO UNIVERSAL AUDIO OR STARTS A UA PROGRAM: the licence answer is the x8p's own board table,
# read through our driver; the table is never printed or logged (counts and the asked-for states only).
#
# The default run ("all"): licence read + CREATE CHECK; if the x8p was not cold-started by our driver it stops with
# power-cycle instructions (no DSP is ours to place on after a warm start). After the power cycle: licence + CHECK
# again (cold), the unrouted CREATE on the idle DSP 2 and its DESTROY, then START+CHAIN on DSP 1 input 1 (mono,
# defaults) held while measuring, restored by DESTROY; then once more, restored by closing the handle.
#
# SAFETY: an x8p power cycle also drops the Twin X (daisy chain): not during a Twin X test; the speakers go for a
# minute. A started chain is in the x8p monitor mix at once (HP1 carries it): HEADPHONES LOW. A bad recipe can hang
# DSP 1; recovery is an x8p power cycle (closing this window first lets the driver try to restore the strip).
# PowerShell 5.1, ASCII only.
#Requires -RunAsAdministrator
param([string]$Recipe, [ValidateSet('all', 'licence', 'plain', 'start')][string]$Step = 'all', [int]$Hold = 20,
      [ValidateSet('x8p', 'twinx')][string]$Unit = 'x8p', [switch]$Off, [switch]$KeepOn, [switch]$Yes)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$log  = Join-Path $PSScriptRoot 'step-p1.log'
"step-p1 $(Get-Date -Format o) Unit=$Unit Step=$Step Hold=$Hold Off=$Off KeepOn=$KeepOn" | Out-File -Encoding utf8 $log
# [P7] the unit: its name in messages, its PCI id, its log tag, the DSP of the unrouted step (the x8p's idle DSP 2; a
# Twin X Duo has none idle, so its strip DSP 1), where its monitor mix goes
$twx     = $Unit -eq 'twinx'
$uName   = if ($twx) { 'Twin X' } else { 'x8p' }
$uTag    = if ($twx) { 'twx' } else { 'x8p' }
$plainDsp = if ($twx) { '1' } else { '2' }
$monWhere = if ($twx) { 'the Twin X monitor mix: your SPEAKERS (turn them down or off; headphones low)' } else { 'the x8p monitor mix (HP1)' }
$params  = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters'
$script:phSet = $false          # this run wrote PluginHost = 1
$script:phPrev = $null          # the value it found ($null = absent, which the driver reads as 0)
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }
# PluginHost back to what this run found (review P0 finding 2): instances end with apollo-plug's handle anyway;
# this closes the door for every other program. Idempotent; -KeepOn skips it.
function Restore-PluginHost {
    if (-not $script:phSet) { return }
    $script:phSet = $false
    if ($KeepOn) { Note 'PluginHost stays 1 (-KeepOn): any program may place plug-ins until step-p1.ps1 -Off.'; return }
    try {
        if ($null -eq $script:phPrev) { Remove-ItemProperty -Path $params -Name 'PluginHost' -ErrorAction Stop }
        else { Set-ItemProperty -Path $params -Name 'PluginHost' -Value $script:phPrev -Type DWord -ErrorAction Stop }
        Note "PluginHost put back to $(if ($null -eq $script:phPrev) { 'absent (0)' } else { $script:phPrev }): plug-in calls are refused again."
    } catch { Note "WARNING: PluginHost could not be put back ($($_.Exception.Message)): run step-p1.ps1 -Off" }
}
function Done([int]$rc) {
    Restore-PluginHost
    Note "step-p1 exit $rc. Log: $log"
    if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }
    exit $rc
}
$script:nPass = 0
$script:nCheck = 0
function Pass([string]$s) { $script:nPass++; Note "PASS   $s" }
function Check([string]$s) { $script:nCheck++; Note "CHECK  $s" }

# apollo-win\plugins (store, bundles, the recipe): in the tree, or two levels up from a builds\<work> copy
$plugins = Join-Path $root 'plugins'
if (-not (Test-Path -LiteralPath (Join-Path $plugins 'tools\oa_offline.py'))) {
    $plugins = Join-Path (Split-Path -Parent (Split-Path -Parent $root)) 'plugins'
}
# -Recipe: a recipe file, or a short name: plugins\p1\<name>.recipe or <name>-mono-in1.recipe
$known = @{ 'lexicon-480l' = 'UAD Lexicon 480L'; 'ampeg-svtvr-classic' = 'UAD Ampeg SVTVR Classic';
            'oxford-supresser-ds' = 'UAD Oxford SuprEsser DS' }
if (-not $Recipe) { $Recipe = 'ampeg-svtvr-classic' }
$slug = ''
if (-not (Test-Path -LiteralPath $Recipe -PathType Leaf) -and $Recipe -notmatch '[\\/:]|\.recipe$') {
    $slug = (($Recipe.ToLower() -replace '^uad[ -]+', '') -replace '[^a-z0-9]+', '-').Trim('-')
    $slug = $slug -replace '-mono-in1$', ''
    $Recipe = Join-Path $plugins "p1\$slug-mono-in1.recipe"
    if (-not (Test-Path -LiteralPath $Recipe) -and (Test-Path -LiteralPath (Join-Path $plugins "p1\$slug.recipe"))) {
        $Recipe = Join-Path $plugins "p1\$slug.recipe"
    }
    # [P7] the Twin X's own file first (p1_recipe.py --unit twinx); the x8p's input-1 recipe has the same words
    if ($twx -and (Test-Path -LiteralPath (Join-Path $plugins "p1\$slug-twinx-mono-in1.recipe"))) {
        $Recipe = Join-Path $plugins "p1\$slug-twinx-mono-in1.recipe"
    }
}
$names   = Join-Path $plugins 'p1\renderids.txt'
$authOut = Join-Path $plugins 'p1\authorized.txt'
$store   = Join-Path $plugins 'store\modules'
$x8pHwid = if ($twx) { 'PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00' } else { 'PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00' }   # [P7] the unit's
# What the recipe says about itself: its plug-in, how its effect is measured, its startup pages
function RecipeField([string]$re) {
    $m = Select-String -LiteralPath $Recipe -Pattern $re -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($m) { return $m.Matches[0].Groups[1].Value } else { return '' }
}

if ($Off) {
    Set-ItemProperty -Path $params -Name 'PluginHost' -Value 0 -Type DWord
    Note "PluginHost = $((Get-ItemProperty $params).PluginHost): plug-in hosting refused from the next call on."
    Note 'Running instances are not touched: close the program that holds them (closing restores the plain strip).'
    Done 0
}

# Everything below runs inside try/finally: Ctrl+C or an error still puts PluginHost back.
try {
# --- tools: the verified admin-only copies (safe-tools.ps1) ---
. (Join-Path $PSScriptRoot 'safe-tools.ps1')
$tools = Initialize-SafeTools $root @('tools\apollo-diag\apollo-diag.exe', 'tools\plugin-host\apollo-plug.exe') ${function:Note}
if (-not $tools) { Note 'Stopped before changing anything.'; Done 1 }
$diag = $tools['tools\apollo-diag\apollo-diag.exe']
$plug = $tools['tools\plugin-host\apollo-plug.exe']

# Runs apollo-plug, logs its output, counts its PASS/CHECK lines. Returns the exit code.
function Plug([string[]]$a) {
    $a = @('--unit', $Unit) + $a                        # [P7] every call on the unit asked for
    Note ">>> apollo-plug $($a -join ' ')"
    $out = (& $plug @a 2>&1 | Out-String)
    $rc = $LASTEXITCODE
    foreach ($l in ($out -split "`r?`n")) {
        if ($l -match '^PASS ') { $script:nPass++ }
        if ($l -match '^CHECK ') { $script:nCheck++ }
        if ($l.Length) { Note $l }
    }
    return $rc
}
function DiagQuiet([string[]]$a) { return (& $diag @a 2>&1 | Out-String) }
function Field([string]$text, [string]$name) {
    $m = [regex]::Match($text, "(?m)^$([regex]::Escape($name))\s+(.*?)\s*$")
    if ($m.Success) { return $m.Groups[1].Value } else { return '' }
}
function PluginLog {
    $l = DiagQuiet @('--unit', $Unit, 'log')
    $lines = @([regex]::Matches($l, "(?m)^.*\[$uTag\] (plugins|strips: DSP \d+ quarantined).*$") | ForEach-Object { $_.Value.TrimEnd() })
    Note ">>> apollo-diag --unit $Unit log (plug-in lines, the last 40)"
    foreach ($x in ($lines | Select-Object -Last 40)) { Note $x }
}

# ======================================================================================================
# Preconditions
# ======================================================================================================
$d = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "$x8pHwid*" } | Select-Object -First 1
if (-not $d) { Note "The $uName is not connected."; Done 1 }
$svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
$ver = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
Note "${uName}: $($d.InstanceId) service $svc $ver $($d.Status)"
if ($svc -ne 'apollo') { Note "The $uName is not on apollo.sys."; Done 1 }
if (-not (Test-Path -LiteralPath $Recipe)) {
    $pname = if ($known.ContainsKey($slug)) { $known[$slug] } else { '<the plug-in''s name, as in plugins\p1\authorized.txt>' }
    Note ''
    Note "No recipe at $Recipe. Make it from a NORMAL (not elevated) PowerShell, offline, then run this again:"
    Note "  & `"$plugins\.venv\Scripts\python.exe`" -I -X utf8 `"$root\tools\plugin-host\p1_recipe.py`" `"$pname`" --authorized `"$authOut`" --renderids `"$names`"$(if ($twx) { ' --unit twinx' })"
    Note 'Nothing changed.'
    Done 1
}
$rName   = RecipeField '^name (.+)$'
$effect  = RecipeField '^effect (\w+)'
if (-not $effect) { $effect = 'delay' }
# where the chain goes (titles only): mono on input 1, or a stereo pair from p1_recipe.py --layout stereo
$layout  = RecipeField '^layout (\w+)'
if (-not $layout) { $layout = 'mono' }
$synch   = RecipeField '^synch (\d+) '
$in1     = if ($synch) { [int]$synch + 1 } else { 1 }
$where   = if ($layout -eq 'stereo') { "inputs $in1/$($in1 + 1)" } else { "input $in1" }
$nPages  = @(Select-String -LiteralPath $Recipe -Pattern '^chunk ' -ErrorAction SilentlyContinue).Count
$staged  = $nPages -gt 1
Note "Recipe: $rName ($Recipe), $layout on $where; effect measured as: $effect; startup $(if ($staged) { "$nPages pages: STAGED" } else { 'one page' })"
$rc = Plug @('caps')
if ($rc -ne 0) {
    Note ''
    Note 'apollo-plug caps got no answer: the running apollo.sys is not the P0 build. Install the P0 package first'
    Note '(install.ps1 -Package <the P0 package folder>), then run this again. Nothing changed.'
    Done 1
}
if ($staged -and -not ((& $plug --unit $Unit caps 2>&1 | Out-String) -match '(?m)^Features .*\bstage\b')) {
    Note ''
    Note "This recipe's startup is $nPages pages, and the running apollo.sys cannot stage one (no 'stage' in Features)."
    Note 'Install the build with staged startup first (install.ps1 -Package <that package>), then run this again. Nothing changed.'
    Done 1
}
if ($twx -and -not ((& $plug --unit $Unit caps 2>&1 | Out-String) -match '(?m)^Features .*\bunits\b')) {
    Note ''
    Note "The running apollo.sys hosts plug-ins on the x8p only (no 'units' in Features): install the P7 build on the Twin X"
    Note '(install.ps1 -Package <that package>), power-cycle the Twin X, then run this again. Nothing changed.'
    Done 1
}
$rc = Plug @('check', $Recipe, '--store', $store)
if ($rc -ne 0) { Note 'The recipe does not pass the driver''s own offline checks (CHECK lines above). Nothing changed.'; Done 1 }

# PluginHost = 1: read by the driver at every plug-in call (no restart); every exit puts back what was there
$script:phPrev = (Get-ItemProperty -Path $params -Name 'PluginHost' -ErrorAction SilentlyContinue).PluginHost
$script:phSet = $true
Set-ItemProperty -Path $params -Name 'PluginHost' -Value 1 -Type DWord
if ((Get-ItemProperty $params).PluginHost -eq 1) {
    Pass "PluginHost = 1 for this run (was $(if ($null -eq $script:phPrev) { 'absent' } else { $script:phPrev }); put back at the end$(if ($KeepOn) { ' unless -KeepOn: kept on' }))"
} else { Check 'PluginHost could not be written'; Done 1 }

$rc = Plug @('modules', 'install', $Recipe, '--from', $store)
if ($rc -ne 0) { Note "The module store is incomplete (see above). Nothing was sent to the $uName."; Done 1 }

# ======================================================================================================
# 1. Licence read (warm is fine) and CREATE CHECK
# ======================================================================================================
Note ''
Note "=== 1. The $uName's own licence answer (board query on its DSP 0; it also sets the device date, as UA's driver does) ==="
$rc = Plug @('licence', '--recipe', $Recipe, '--names', $names, '--authorized-out', $authOut)
if ($rc -ne 0) {
    PluginLog
    Note ''
    Note "Summary: $script:nPass PASS, $script:nCheck CHECK. The licence read did not pass: nothing was placed."
    Note "If the reply stayed at its prefill on a warm start, power-cycle the $uName and run this again (DSP 0 answers cold)."
    Note 'If the plug-in is not authorized, pick an authorized candidate from the INFO lines and make its recipe (normal'
    Note 'prompt): p1_recipe.py "<name>" --authorized plugins\p1\authorized.txt, then run with -Recipe <that file>.'
    Done 1
}
Note ''
Note '=== 2. CREATE with CHECK: the device''s answer for this plug-in, nothing placed ==='
$rc = Plug @('create-check', $Recipe)
if ($rc -ne 0) { PluginLog; Note "Summary: $script:nPass PASS, $script:nCheck CHECK. Stopped: not admitted."; Done 1 }

$info = DiagQuiet @('--unit', $Unit, 'info')
$strips = DiagQuiet @('--unit', $Unit, 'strips')
$warm = (Field $info 'Flags') -match '\bWARM_START\b'
$owned = (Field $strips 'Flags') -match '\bOWNED\b'
$live = (Field $strips 'State') -match '\bLive\b'
if ($Step -eq 'licence') {
    Note ''
    Note "Summary: $script:nPass PASS, $script:nCheck CHECK (Step licence)."
    Done 0
}
if ($warm -or -not $owned) {
    Note ''
    Note "The $uName was not cold-started by our driver (warm start, or the strip DSP not owned): no DSP is ours to place on."
    Note "NEXT, one $uName power cycle:"
    if ($twx) {
        Note '  - Only the Twin X goes (the x8p keeps running). Its strips need StripsEnable 1 (step-g5.ps1 left it so).'
        Note '  - SPEAKERS DOWN OR OFF, HEADPHONES LOW: the plug-in on input 1 will be in the Twin X monitor mix (the speakers).'
        Note '  1. Power the Twin X off, wait 10 s, power it on. Wait about 60 s (the strips start 0.5 s after the bring-up).'
        Note "  2. Run step-p1.ps1 -Unit twinx again: licence (cold), CHECK, the unrouted DSP 1 test, then START on DSP 1 input 1."
    } else {
        Note '  - It also drops the Twin X (daisy chain): no Twin X test may be running; your speakers go for about a minute.'
        Note '  - HEADPHONES LOW: the plug-in on input 1 will be in the x8p monitor mix (HP1).'
        Note '  1. Power the x8p off, wait 10 s, power it on. Wait about 60 s (the strips start 0.5 s after the bring-up).'
        Note '  2. Run step-p1.ps1 again: licence (cold), CHECK, the unrouted DSP 2 test, then START on DSP 1 input 1.'
    }
    Note "Summary: $script:nPass PASS, $script:nCheck CHECK."
    Done 0
}
Pass "$uName cold-started by our driver: DSPs owned"
if ($Step -ne 'plain' -and -not $live) {
    Check "$uName strips are not Live ($(Field $strips 'State') - $(Field $strips 'Why')): START needs input 1's plain strip"
}

# ======================================================================================================
# 3. Unrouted CREATE on the idle DSP 2, then DESTROY (upstream's first native CREATE, oa:HARDWARE-SESSIONS.md:314-318)
# ======================================================================================================
if ($Step -eq 'all' -or $Step -eq 'plain') {
    Note ''
    if ($twx) {
        Note '=== 3. CREATE without START on DSP 1 (the Duo has no idle DSP: beside the live strips, never routed): uploads acknowledged, rows zero-filled and linked; DESTROY ==='
    } else {
        Note '=== 3. CREATE without START on DSP 2 (idle, never routed): uploads acknowledged, rows zero-filled and linked; DESTROY ==='
    }
    $rc = Plug @('create-plain', $Recipe, '--dsp', $plainDsp)
    if ($rc -ne 0) {
        PluginLog
        Note "Summary: $script:nPass PASS, $script:nCheck CHECK. Stopped after the unrouted test (send Claude step-p1.log)."
        if ($twx) { Note 'If DSP 1 is quarantined now, power-cycle the Twin X (it rebuilds the strips) before anything else.' }
        else { Note 'If DSP 2 is quarantined, START on DSP 1 is still possible: step-p1.ps1 -Step start (or power-cycle first).' }
        Done 1
    }
    if ($Step -eq 'plain') { PluginLog; Note "Summary: $script:nPass PASS, $script:nCheck CHECK (Step plain)."; Done 0 }
}

# ======================================================================================================
# 4. START+CHAIN on DSP 1, input 1, mono, defaults: held while measuring, restored by DESTROY; again, by closing
# ======================================================================================================
if (-not $live) { Note "Stopped: the strips are not Live, so there is no plain strip on input 1 to replace."; PluginLog; Done 1 }
Note ''
if ($staged) {
    Note "=== 4. $rName on DSP 1, $where ($layout, its defaults): STAGED, $nPages pages ==="
    Note 'CREATE STAGE with page 1 (input 1''s plain strip keeps running), the other pages, the whole startup read back,'
    Note 'then START routes it. Anything wrong before START takes it down without input 1 ever changing.'
} else {
    Note "=== 4. START+CHAIN of $rName on DSP 1, $where ($layout, its defaults) ==="
}
Note "HEADPHONES LOW now: once it is routed the plug-in is in $monWhere."
$ref = @()
if (-not $Yes) {
    $a = Read-Host 'Is a second output of the same source (the Sphere''s second output, or a second mic beside the first) plugged into input 2? [y/N]'
    if ($a -match '^[Yy]') { $ref = @('--ref', '1') }
} else {
    $ref = @('--ref', '1')
}
if ($ref.Count -eq 0) {
    if ($effect -eq 'tail') { Note 'No reference on input 2: the tail is measured on input 1 alone (against its own peaks).' }
    else { Note 'No reference on input 2: input 1 alone (the lag and level checks need the same source on inputs 1 and 2).' }
}
$both = if ($ref.Count) { ' (and so into input 2)' } else { '' }
$what = switch ($effect) {
    'tail'  { "Clap, click or tap the mic sharply into input 1$both about once a second, with silence between, for the whole run" }
    'level' { "Make continuous sound into input 1$both for the whole run (speak, sing, play)" }
    default { "Make continuous sound into input 1$both for the whole run (speak, sing, play)" }
}
Note "$what (about $($Hold + 5) s$(if ($staged) { ', plus up to about 20 s first for the staged startup and its readback' }))."
if (-not $Yes) { $null = Read-Host '[Enter] starts' }
$rc = Plug (@('create-start', $Recipe, '--dsp', '1', '--hold', "$Hold", '--end', 'destroy', '--mon') + $ref)
if ($rc -ne 0) {
    PluginLog
    $null = DiagQuiet @('--unit', $Unit, 'strips')
    Note "Summary: $script:nPass PASS, $script:nCheck CHECK. See the CHECK lines; send Claude step-p1.log."
    Note "If DSP 1 is quarantined or input 1 is silent now: power-cycle the $uName (it rebuilds the strips)."
    Done 1
}
Note ''
Note '=== 5. The same again, restored by closing the handle (the driver destroys it at close) ==='
if (-not $Yes) { $null = Read-Host "Keep it going into input 1 ($(if ($effect -eq 'tail') { 'short sounds, silence between' } else { 'continuous sound' })). [Enter] starts (10 s)" }
$rc = Plug (@('create-start', $Recipe, '--dsp', '1', '--hold', '10', '--end', 'close') + $ref)
PluginLog
$null = Plug @('caps')
Note ''
Note "Summary: $script:nPass PASS, $script:nCheck CHECK. Send Claude step-p1.log."
Note 'Nothing runs now: every instance was destroyed.'
Done $(if ($rc -eq 0) { 0 } else { 1 })
} finally {
    Restore-PluginHost
}
