# Put the x8p back on UA's driver: remove every staged apollo.inf package, then rescan. ADMIN PowerShell.
#   powershell -ExecutionPolicy Bypass -File C:\Users\aaron\Desktop\Avium\apollo-win\tools\phase1\revert.ps1 [-Yes] [-AllUnits]
# This MOVES THE x8p (and removes our package from the PC). Power-cycle the x8p afterwards so UA's software
# starts it fresh.
# Removing the package also moves every OTHER unit on it (a Twin X on our driver, later gates). So if any
# Apollo other than the x8p is on our driver, the script stops before changing anything unless -AllUnits
# is given. To move one unit only, use revert-unit.ps1 -Unit twinx|x8p (it keeps the package).
# First checks that nothing is using the x8p (Windows refuses to remove a driver under a running audio
# stream and then asks for a reboot). -Yes: do not ask, go ahead after a 60 s wait.
# apollo-diag runs only from a verified admin-only copy (safe-tools.ps1). If that check fails, the revert
# still runs (it is the way back), just without apollo-diag's in-use check.
#Requires -RunAsAdministrator
param([switch]$Yes, [switch]$AllUnits)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$log = Join-Path $PSScriptRoot 'revert.log'
"apollo revert $(Get-Date -Format o)" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }
$pnputil = Join-Path (Join-Path $env:SystemRoot 'System32') 'pnputil.exe'

# Any Apollo other than the x8p on our driver would go back to UA with the package: not without -AllUnits.
# [release7 review] Present or not: a switched-off Twin X that was on our driver keeps it, and removing the
# package moves it too, so look at every device PnP knows (no -PresentOnly), by its driver service.
$x8pHwid = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00'
$othersOnOurs = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
        $_.InstanceId -like 'PCI\VEN_1A00&DEV_0002&*' -and $_.InstanceId -notlike "$x8pHwid*" -and
        (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data -eq 'apollo' })
if ($othersOnOurs.Count -gt 0 -and -not $AllUnits) {
    Note 'Another Apollo is on the open driver too (connected or not); removing the package would move it back to UA as well:'
    foreach ($o in $othersOnOurs) { Note "  $($o.InstanceId) ($(if ($o.Present) { 'connected' } else { 'not connected now' }))" }
    Note 'To move only the x8p: revert-unit.ps1 -Unit x8p. To move every unit: run this again with -AllUnits.'
    Note 'Nothing changed.'
    exit 1
}
foreach ($o in $othersOnOurs) { Note "-AllUnits: $($o.InstanceId) goes back to UA too" }

. (Join-Path $PSScriptRoot 'safe-tools.ps1')
$diag = $null
$tools = Initialize-SafeTools $root @('tools\apollo-diag\apollo-diag.exe') ${function:Note}
if ($tools) { $diag = $tools['tools\apollo-diag\apollo-diag.exe'] }
else { Note 'Going on without apollo-diag (the driver checks are skipped; processes and audio sessions are still checked).' }

$audio = $false
if (Test-SafeTemp) {
    $audio = $true
    try { . (Join-Path $PSScriptRoot 'audio-helpers.ps1') } catch { $audio = $false; Note "note: audio helpers unavailable ($($_.Exception.Message))" }
} else {
    Note 'note: no admin-only temp folder, so the audio helpers are not compiled; defaults will not be restored.'
}
if ($audio) {
    if (-not (Wait-ApolloIdle $diag -Yes:$Yes -Note ${function:Note})) { Note 'Stopped before changing anything.'; exit 1 }
} else {
    Note 'Close any program using the x8p (Ableton, asio-test, loopback-test, wasapi-loop) before going on.'
}
$saved = @()
if ($audio) { try { $saved = @(Get-AudioDefaults) } catch { } }

$enum = (& $pnputil /enum-drivers | Out-String) -split "\r?\n\r?\n"
$ours = foreach ($block in $enum) {
    # Ours = original name apollo.inf AND provider Avium (another vendor's apollo.inf is left alone).
    if ($block -match 'Original Name:\s+apollo\.inf' -and $block -match '(?m)^\s*Provider Name:\s+Avium\s*$' -and
        $block -match 'Published Name:\s+(oem\d+\.inf)') { $Matches[1] }
}
if (-not $ours) { Note 'No apollo.inf package from Avium is installed.' }
foreach ($p in $ours) {
    $out = (& $pnputil /delete-driver $p /uninstall /force 2>&1 | Out-String)
    Write-Host $out; Add-Content -Encoding utf8 $log $out
}
$out = (& $pnputil /scan-devices 2>&1 | Out-String)
Write-Host $out; Add-Content -Encoding utf8 $log $out

Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_1A00&DEV_0002*' | ForEach-Object {
    $inf = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
    $line = "$($_.InstanceId) -> $inf ($($_.Status))"
    Note $line
    if (@(14, 21, 38, 47) -contains [int]$_.Problem) {
        Note 'Windows could not stop the old driver (something still had the x8p open): reboot to finish.'
    }
}

# If an Apollo endpoint was a default, Windows picked another one; put back the rest of your defaults.
if ($audio -and $saved.Count -gt 0) {
    Start-Sleep -Seconds 3
    try { foreach ($l in @(Restore-AudioDefaults $saved)) { Note $l } } catch { }
}
