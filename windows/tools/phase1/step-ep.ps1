# step-ep.ps1 - [Endpoints] install the build with the full Windows endpoint tables on BOTH Apollos, clean
# up the endpoints earlier builds left, put the Windows defaults back, list what Windows shows, play one
# 5-second tone on a LINE output. ADMIN PowerShell:
#   powershell -ExecutionPolicy Bypass -File tools\phase1\step-ep.ps1 [-Package <dir>] [-NoTone] [-Yes]
#
# What it does, in order (it stops at the first thing that is not right, and says why):
#   1. Refuses if anything plays or records through an Apollo (a Windows app on any "(Apollo" endpoint,
#      an ACX stream on either unit, an ASIO host, our test tools): a driver update under a running
#      stream turns into a removal that needs a reboot (Nov 15: Spotify on the combined Monitor).
#   2. Quits Apollo Control (it holds a handle on both units).
#   3. Saves the Windows default devices (step-ep.defaults.json).
#   4. pnputil /add-driver, then devcon update on the x8p's exact ID, then on the Twin X's. devcon exit 1
#      (restart needed) = stop here and report: never remove a unit.
#   5. Both units share one loaded apollo.sys image, so after the two updates the old image still runs: if
#      the driver log does not show the new build, both units are disabled (Twin X, then x8p: the image
#      unloads) and enabled again (x8p, then Twin X) - only while nothing holds them; a vetoed disable is
#      undone at once. This is a warm start: the x8p's input strips (mic/line inputs) come back at its next
#      power cycle.
#   6. clean-endpoints.ps1: removes the not-present endpoints the earlier builds left under both units.
#   7. Puts the defaults back; a default that pointed at an endpoint of the old build goes to the new
#      endpoint of the same name ("Monitor (Apollo x8p (open driver, test build))" -> "Monitor (Apollo)").
#   8. Lists the Apollo endpoints Windows shows (present devnodes and their MMDevices names).
#   9. One 5-second tone (-12 dBFS sine) on LINE 1-2 of the x8p through Windows (wasapi-loop --tone), and the
#      driver's counters for that endpoint. Not on MON: nothing reaches the speakers.
# PowerShell 5.1, ASCII only. Log: tools\phase1\step-ep.log.
param(
    [string]$Package = '',
    [switch]$NoTone,
    [switch]$Yes
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Package) { $Package = Join-Path $root 'driver\x64\Release\apollo' }
$inf = Join-Path $Package 'apollo.inf'
$log = Join-Path $PSScriptRoot 'step-ep.log'
$defaultsFile = Join-Path $PSScriptRoot 'step-ep.defaults.json'
"step-ep $(Get-Date -Format o) Package=$Package" | Out-File -Encoding ascii $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding ascii $log $s }
function Done([int]$rc) { Note "step-ep exit $rc. Log: $log"; exit $rc }

$x8pHwid   = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00'
$twinxHwid = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00'
$pnputil   = Join-Path $env:SystemRoot 'System32\pnputil.exe'

. (Join-Path $PSScriptRoot 'safe-tools.ps1')
$tools = Initialize-SafeTools $root @('tools\bin\devcon.exe', 'tools\apollo-diag\apollo-diag.exe', 'tools\wasapi-loop\wasapi-loop.exe') ${function:Note}
if (-not $tools) { Note 'Stopped before changing anything.'; Done 1 }
$devcon = $tools['tools\bin\devcon.exe']
$diag = $tools['tools\apollo-diag\apollo-diag.exe']
$loop = $tools['tools\wasapi-loop\wasapi-loop.exe']
. (Join-Path $PSScriptRoot 'audio-helpers.ps1')
if (-not (Test-Path -LiteralPath $inf)) { Note "No package at $Package (build with tools\build-all.cmd)."; Done 1 }

function Quiet([string]$exe, [string[]]$a) { return (& $exe @a 2>&1 | Out-String) }
function Unit([string]$hwid) {
    $d = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "$hwid*" } | Select-Object -First 1
    if (-not $d) { return $null }
    [pscustomobject]@{
        InstanceId = $d.InstanceId
        Status     = $d.Status
        Name       = $d.FriendlyName
        Service    = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
        Inf        = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
        Version    = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
    }
}
function Format-Unit($u) { "$($u.InstanceId) [$($u.Name)]: $($u.Inf) $($u.Version) service $($u.Service) status $($u.Status)" }
# Everything that holds either unit (step-g7 Test-AllIdle): apps on any Apollo endpoint, ACX streams, ASIO, tools.
function Test-AllIdle {
    $why = @(Test-ApolloInUse $diag | ForEach-Object { $_ })   # it returns ,$array: flatten
    $ep = Quiet $diag @('--unit', 'twinx', 'endpoints')
    if ($ep -match 'ACX streams: prepared (\d+), running (\d+)' -and [int]$Matches[1] -gt 0) {
        $why += "$($Matches[1]) Windows stream(s) open on the Twin X, $($Matches[2]) running"
    }
    return ,@($why | Select-Object -Unique)
}
function Enable-Unit($u) {
    $o = (& $pnputil /enable-device $u.InstanceId 2>&1 | Out-String)
    Note ">>> pnputil /enable-device $($u.InstanceId)"; Note $o.TrimEnd()
}
function Restart-BothUnits($x8p, $twx) {
    $disabled = @()
    foreach ($u in @($twx, $x8p)) {
        if ($null -eq $u) { continue }
        $o = (& $pnputil /disable-device $u.InstanceId 2>&1 | Out-String)
        Note ">>> pnputil /disable-device $($u.InstanceId)"; Note $o.TrimEnd()
        if ($LASTEXITCODE -ne 0 -or $o -match '(?i)reboot|restart') {
            Note 'Windows could not stop that unit now (something still holds it). Enabling again what this script disabled.'
            foreach ($d in @($u) + $disabled) { Enable-Unit $d; Start-Sleep -Seconds 2 }
            return $false
        }
        $disabled += $u
    }
    Start-Sleep -Seconds 3
    foreach ($u in @($x8p, $twx)) {
        if ($null -eq $u) { continue }
        Enable-Unit $u
        Start-Sleep -Seconds 2
    }
    return $true
}
# The new build runs when the x8p's GET_ENDPOINTS is version 2 (this build's tables).
function Test-NewBuild {
    $ep = Quiet $diag @('--unit', 'x8p', 'endpoints')
    return ($ep -notmatch 'warning: endpoints version' -and $ep -match 'Endpoints\s+(\d+) in the driver''s table' -and
            ([int]$Matches[1] -eq 22 -or [int]$Matches[1] -eq 31))
}
# Saved defaults whose endpoint is gone go to the active endpoint of the same pin name (the part before " (").
function Restore-Mapped($saved) {
    $active = @(Get-AudioEndpoints)
    $byId = @{}
    foreach ($e in $active) { $byId[$e.Id] = $e }
    foreach ($d in $saved) {
        if (-not $d.Id) { continue }
        $target = $null
        if ($byId.ContainsKey($d.Id)) {
            $target = $byId[$d.Id]
        } elseif ($d.Name -match '^(.*) \((Apollo.*)\)$') {
            $pin = $Matches[1]
            $target = $active | Where-Object { $_.Flow -eq $d.Flow -and $_.Name -match ('^' + [regex]::Escape($pin) + ' \(Apollo') } | Select-Object -First 1
        }
        if (-not $target) {
            Note ("  {0} {1}: was '{2}', which is gone. Pick one in Sound settings." -f $script:FlowNames[$d.Flow], $script:RoleNames[$d.Role], $d.Name)
            continue
        }
        $now = [AviumAudio.Audio]::DefaultId($d.Flow, $d.Role)
        if ($now -eq $target.Id) { Note ("  {0} {1}: '{2}' (unchanged)" -f $script:FlowNames[$d.Flow], $script:RoleNames[$d.Role], $target.Name); continue }
        $hr = [AviumAudio.Audio]::SetDefault($target.Id, $d.Role)
        Note ("  {0} {1}: '{2}'{3}" -f $script:FlowNames[$d.Flow], $script:RoleNames[$d.Role], $target.Name,
              $(if ($hr -eq 0) { ' (put back)' } else { " FAILED 0x{0:X8}" -f $hr }))
    }
}
function List-Apollo([string]$title) {
    Note "--- $title"
    $units = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '^PCI\\VEN_1A00&DEV_0002&SUBSYS_00(14|19)1A00&' })
    $ids = @{}
    foreach ($u in $units) { $ids[$u.InstanceId.ToUpperInvariant()] = $u.FriendlyName }
    $rows = @()
    foreach ($d in @(Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue)) {
        $p = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue).Data
        if (-not $p -or -not $ids.ContainsKey(([string]$p).ToUpperInvariant())) { continue }
        $flow = if ($d.InstanceId -match '\{0\.0\.0\.') { 'out' } else { 'in' }
        $rows += [pscustomobject]@{ Status = $d.Status; Flow = $flow; Name = $d.FriendlyName }
    }
    foreach ($r in ($rows | Sort-Object Status, Flow, Name)) { Note ("  {0,-8} {1,-4} {2}" -f $r.Status, $r.Flow, $r.Name) }
    Note ("  ({0} Apollo endpoint devnodes: {1} OK, {2} not present)" -f $rows.Count, @($rows | Where-Object Status -eq 'OK').Count,
          @($rows | Where-Object Status -ne 'OK').Count)
}

# ---- 1. nothing may hold either unit
$x8p = Unit $x8pHwid
$twx = Unit $twinxHwid
if (-not $x8p) { Note 'The x8p is not present: nothing changed.'; Done 1 }
Note "x8p:    $(Format-Unit $x8p)"
if ($twx) { Note "Twin X: $(Format-Unit $twx)" } else { Note 'Twin X: not present (only the x8p is updated).' }
$why = Test-AllIdle
if ($why.Count -gt 0) {
    Note 'NOT INSTALLED: something plays or records through an Apollo now:'
    foreach ($w in $why) { Note "  - $w" }
    Note 'Pause or close it (or move it to another output), then run this again. Nothing was changed.'
    Done 3
}
Note 'Nothing holds either Apollo.'

# ---- 2. Apollo Control holds both units: quit it
foreach ($p in @(Get-Process -Name 'apollo-control' -ErrorAction SilentlyContinue)) {
    Note "Quitting Apollo Control (PID $($p.Id))."
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 2

# ---- 3. the Windows defaults
$saved = @(Get-AudioDefaults)
$saved | ConvertTo-Json | Out-File -Encoding ascii $defaultsFile
Note 'Windows defaults saved:'
Format-AudioDefaults $saved | ForEach-Object { Note $_ }

# ---- 4. the package on both units
$o = (& $pnputil /add-driver $inf 2>&1 | Out-String)
Note ">>> pnputil /add-driver $inf (exit $LASTEXITCODE)"; Note $o.TrimEnd()
if ($LASTEXITCODE -ne 0 -and $o -notmatch '(?i)already exists|Added driver packages:\s*1|Published Name') { Note 'pnputil could not stage the package.'; Done 1 }
foreach ($u in @(@{ Hwid = $x8pHwid; Name = 'x8p'; Dev = $x8p }, @{ Hwid = $twinxHwid; Name = 'Twin X'; Dev = $twx })) {
    if ($null -eq $u.Dev) { continue }
    $o = (& $devcon update $inf $u.Hwid 2>&1 | Out-String)
    $rc = $LASTEXITCODE
    Note ">>> devcon update $inf $($u.Hwid) (exit $rc)"; Note $o.TrimEnd()
    if ($rc -eq 1 -or $o -match '(?i)reboot|restart') {
        Note "STOPPED: devcon says the $($u.Name) needs a restart to take the new driver (something still held it). No unit was removed."
        Note 'Report this; a reboot (or a power cycle of both units) loads the new build.'
        Restore-Mapped $saved
        Done 4
    }
    if ($rc -ne 0) { Note "devcon failed on the $($u.Name)."; Restore-Mapped $saved; Done 5 }
}
Start-Sleep -Seconds 5

# ---- 5. the new image: both units must stop together once
if (Test-NewBuild) {
    Note 'The new build runs already.'
} else {
    Note 'The old apollo.sys image still runs (both units share it): restarting both units once.'
    $x8p = Unit $x8pHwid; $twx = Unit $twinxHwid
    $why = Test-AllIdle
    if ($why.Count -gt 0) {
        Note 'NOT RESTARTED: something opened an Apollo meanwhile:'
        foreach ($w in $why) { Note "  - $w" }
        Note 'The new driver is installed and loads at the next power cycle of both units or a reboot.'
        Restore-Mapped $saved
        Done 6
    }
    if (-not (Restart-BothUnits $x8p $twx)) {
        Note 'NOT RESTARTED: the new driver is installed and loads at the next power cycle of both units or a reboot.'
        Restore-Mapped $saved
        Done 6
    }
    $deadline = (Get-Date).AddSeconds(45)
    while ((Get-Date) -lt $deadline -and -not (Test-NewBuild)) { Start-Sleep -Seconds 2 }
    if (-not (Test-NewBuild)) { Note 'CHECK: the x8p does not answer with the new endpoint tables yet (see apollo-diag endpoints / log).' }
}
Start-Sleep -Seconds 5
$x8p = Unit $x8pHwid; $twx = Unit $twinxHwid
Note "x8p:    $(if ($x8p) { Format-Unit $x8p } else { 'NOT PRESENT' })"
Note "Twin X: $(if ($twx) { Format-Unit $twx } else { 'not present' })"
foreach ($l in @((Quiet $diag @('--unit', 'x8p', 'log')).Split("`n") | Where-Object { $_ -match 'friendly name|endpoint set, mask|EndpointSet 1' })) {
    Note $l.TrimEnd()
}
foreach ($un in 'x8p', 'twinx') {
    Note ">>> apollo-diag --unit $un endpoints"
    Note (Quiet $diag @('--unit', $un, 'endpoints')).TrimEnd()
}

# ---- 6. the leftovers of earlier builds
Note '>>> clean-endpoints.ps1'
$o = (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'clean-endpoints.ps1') 2>&1 | Out-String)
Note $o.TrimEnd()

# ---- 7. the defaults
Note 'Windows defaults:'
Restore-Mapped $saved
Format-AudioDefaults (Get-AudioDefaults) | ForEach-Object { Note $_ }

# ---- 8. what Windows shows
List-Apollo 'Apollo endpoints Windows has now'
Note '--- active endpoint names (MMDevices)'
foreach ($e in @(Get-AudioEndpoints | Where-Object { $_.Name -match '\(Apollo' } | Sort-Object Flow, Name)) {
    Note ("  {0,-9} {1}" -f $script:FlowNames[$e.Flow], $e.Name)
}

# ---- 9. one tone on a LINE output
if (-not $NoTone) {
    $target = @(Get-AudioEndpoints | Where-Object { $_.Flow -eq 0 -and $_.Name -match '^(x8p )?LINE 1-2 \(Apollo' }) | Select-Object -First 1
    if (-not $target) {
        Note 'CHECK: no "LINE 1-2" playback endpoint of the x8p: no tone.'
    } else {
        $match = $target.Name
        Note ">>> wasapi-loop --tone 5 --render `"$match`""
        Note (Quiet $loop @('--tone', '5', '--render', $match)).TrimEnd()
        Note ">>> apollo-diag --unit x8p endpoints (after the tone)"
        Note (Quiet $diag @('--unit', 'x8p', 'endpoints')).TrimEnd()
    }
}
Done 0
