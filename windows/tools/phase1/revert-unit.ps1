# Put ONE Apollo back on Universal Audio's driver and leave every other unit as it is. ADMIN PowerShell.
#   powershell -ExecutionPolicy Bypass -File C:\apollo-win\tools\phase1\revert-unit.ps1 -Unit twinx [-Yes]
#   ... -Unit x8p                 the x8p only; the Twin X keeps its driver
#   ... -InstanceId '<id>'        one unit by its PnP instance ID (PCI\VEN_1A00&DEV_0002&SUBSYS_...\...)
# The "Twin X back to UA" desktop shortcut (new-revert-shortcut.ps1) runs: -Unit twinx -Yes.
# revert.ps1 is the other way back: it removes our package from the PC (every unit on it goes back).
#
# docs/spec-twinx.md 6.10. What it does:
#   1. Finds the unit. Exactly one present device may carry its exact four-part hardware ID
#      (PCI\VEN_1A00&DEV_0002&SUBSYS_xxxxxxxx): devcon moves every device with that ID, so two units
#      with the same ID cannot be told apart and the script stops. DEV_0002 alone is never used.
#   2. Nothing to do if the unit is not on our driver (service apollo).
#   3. Checks that nothing is using that unit (audio sessions on its endpoints; for the x8p also
#      ACX streams and ASIO, as revert.ps1). -Yes: go ahead after a 60 s wait instead of asking.
#   4. Finds UA's package in the driver store: Original Name uad2pcie.inf, Provider Universal Audio,
#      signed by Microsoft, and its INF lists that exact hardware ID (the newest if several).
#   5. devcon update <that INF> <exact hardware ID>: Windows moves only devices with that ID.
#      If no verified devcon is available (safe-tools.ps1): pnputil /remove-device <this instance>,
#      then /scan-devices; Windows then picks the best-ranked driver, UA's signed one.
#   6. Does NOT delete our package: the other unit may still use it.
#   7. Checks: this unit is off our driver, every other Apollo kept the driver it had.
#   8. Puts your default audio devices back and asks for a power cycle of THIS unit only.
# PowerShell 5.1, ASCII only.
#Requires -RunAsAdministrator
param(
    [ValidateSet('twinx', 'x8p')][string]$Unit = '',
    [string]$InstanceId = '',
    [switch]$Yes
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$log = Join-Path $PSScriptRoot 'revert-unit.log'
"apollo revert-unit $(Get-Date -Format o) Unit='$Unit' InstanceId='$InstanceId'" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }
$pnputil = Join-Path (Join-Path $env:SystemRoot 'System32') 'pnputil.exe'

# Units known by name: exact four-part hardware IDs (docs/spec-twinx.md 0.1).
$known = @{
    'x8p'   = @{ Hwid = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00'; Name = 'Apollo x8p'; Endpoints = '(Apollo x8p' }
    'twinx' = @{ Hwid = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00'; Name = 'Apollo Twin X'; Endpoints = '(Apollo Twin X' }
}

# Every present Apollo (vendor 0x1A00, device 0x0002) with its exact hardware ID and current driver.
function Get-ApolloUnits {
    foreach ($d in @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\VEN_1A00&DEV_0002&*' })) {
        $id = $d.InstanceId
        $hw = @((Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_HardwareIds -ErrorAction SilentlyContinue).Data)
        [pscustomobject]@{
            InstanceId = $id
            Hwid       = ($hw | Where-Object { $_ -match '^PCI\\VEN_1A00&DEV_0002&SUBSYS_[0-9A-Fa-f]{8}$' } | Select-Object -First 1)
            Service    = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
            Inf        = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
            Status     = $d.Status
            Problem    = $d.Problem
        }
    }
}

# UA's uad2pcie package whose INF lists this exact hardware ID; the newest if several. $null if none.
function Find-UaInf([string]$hwid) {
    $best = $null
    $bestVer = $null
    foreach ($block in ((& $pnputil /enum-drivers | Out-String) -split "\r?\n\r?\n")) {
        if (-not ($block -match '(?m)^\s*Original Name:\s+uad2pcie\.inf\s*$')) { continue }
        if (-not ($block -match '(?m)^\s*Provider Name:\s+Universal Audio')) { continue }
        if (-not ($block -match '(?m)^\s*Signer Name:\s+Microsoft ')) { continue }
        if (-not ($block -match '(?m)^\s*Published Name:\s+(oem\d+\.inf)\s*$')) { continue }
        $path = Join-Path $env:SystemRoot ('INF\' + $Matches[1])
        if (-not (Test-Path -LiteralPath $path)) { continue }
        if (-not (Select-String -LiteralPath $path -SimpleMatch -Pattern $hwid -Quiet)) { continue }
        $ver = [version]'0.0'
        if ($block -match '(?m)^\s*Driver Version:\s+\S+\s+(\d+(\.\d+){1,3})\s*$') {
            try { $ver = [version]$Matches[1] } catch { }
        }
        if (-not $best -or $ver -gt $bestVer) { $best = $path; $bestVer = $ver }
    }
    return $best
}

# --- 1. which unit ---
if (($Unit -and $InstanceId) -or (-not $Unit -and -not $InstanceId)) {
    Note 'Say which unit: -Unit twinx, -Unit x8p, or -InstanceId <PnP instance ID>. Nothing changed.'
    exit 1
}
$all = @(Get-ApolloUnits)
$hwid = $null
$name = $null
$epFilter = ''
if ($Unit) {
    $hwid = $known[$Unit].Hwid
    $name = $known[$Unit].Name
    $epFilter = $known[$Unit].Endpoints
} else {
    $pick = @($all | Where-Object { $_.InstanceId -eq $InstanceId })
    if ($pick.Count -eq 1) {
        $hwid = $pick[0].Hwid
        $name = "Apollo ($hwid)"
        foreach ($k in $known.Values) { if ($k.Hwid -eq $hwid) { $name = $k.Name; $epFilter = $k.Endpoints } }
    }
}
if (-not $hwid) {
    Note "No present Apollo matches $(if ($Unit) { "-Unit $Unit" } else { "-InstanceId $InstanceId" }) with an exact hardware ID. Present Apollos:"
    foreach ($o in $all) { Note "  $($o.InstanceId) driver $($o.Inf) service $($o.Service)" }
    Note 'Is the unit connected and powered on? Nothing changed.'
    exit 1
}
$same = @($all | Where-Object { $_.Hwid -eq $hwid })
if ($same.Count -eq 0) {
    Note "$name ($hwid) is not present: is it connected and powered on? Nothing changed."
    exit 1
}
if ($same.Count -gt 1) {
    Note "$($same.Count) present devices carry $hwid; devcon would move all of them. Nothing changed:"
    foreach ($o in $same) { Note "  $($o.InstanceId) driver $($o.Inf)" }
    exit 1
}
$dev = $same[0]
Note "unit:  $name $($dev.InstanceId) driver $($dev.Inf) service $($dev.Service) status $($dev.Status)"
$others = @($all | Where-Object { $_.InstanceId -ne $dev.InstanceId })
foreach ($o in $others) { Note "other: $($o.InstanceId) driver $($o.Inf) service $($o.Service) (left as it is)" }

# --- 2. on our driver? ---
if ($dev.Service -ne 'apollo') {
    Note "$name is not on the open driver (service $($dev.Service)): nothing to do."
    exit 0
}

# --- tools: verified admin-only copies only (safe-tools.ps1); the revert goes on without them ---
. (Join-Path $PSScriptRoot 'safe-tools.ps1')
$devcon = $null
$diag = $null
$isX8p = ($hwid -eq $known['x8p'].Hwid)
$t = Initialize-SafeTools $root @('tools\bin\devcon.exe') ${function:Note}
if ($t) { $devcon = $t['tools\bin\devcon.exe'] }
else { Note 'No verified devcon: the unit will be moved with pnputil /remove-device and /scan-devices instead.' }
if ($isX8p) {
    $t = Initialize-SafeTools $root @('tools\apollo-diag\apollo-diag.exe') ${function:Note}
    if ($t) { $diag = $t['tools\apollo-diag\apollo-diag.exe'] }
}
$audio = $false
if (Test-SafeTemp) {
    $audio = $true
    try { . (Join-Path $PSScriptRoot 'audio-helpers.ps1') } catch { $audio = $false; Note "note: audio helpers unavailable ($($_.Exception.Message))" }
} else {
    Note 'note: no admin-only temp folder, so the audio helpers are not compiled; defaults will not be restored.'
}

# --- 3. nothing may be using this unit ---
if ($audio -and $isX8p) {
    if (-not (Wait-ApolloIdle $diag -Yes:$Yes -Note ${function:Note})) { Note 'Stopped before changing anything.'; exit 1 }
} elseif ($audio -and $epFilter) {
    $deadline = (Get-Date).AddSeconds(60)
    $told = $false
    while ($true) {
        $busy = @()
        try { $busy = @(Get-ApolloAudioSessions -Filter $epFilter) } catch { }
        if ($busy.Count -eq 0) { break }
        if (-not $told) {
            Note "$name is in use:"
            foreach ($s in $busy) { Note "  - $($s.Process) (PID $($s.Pid)) is using $($s.Endpoint) for $($s.Flow)" }
            Note 'Close those programs or stop their playback/recording. Waiting up to 60 s...'
            $told = $true
        }
        if ((Get-Date) -gt $deadline) {
            if ($Yes) { Note '-Yes given: going ahead. If Windows cannot stop the driver it asks for a reboot.'; break }
            $a = Read-Host '[Enter] go ahead anyway (Windows may ask for a reboot), [Q] quit'
            if ($a -match '^[Qq]') { Note 'Stopped before changing anything.'; exit 1 }
            break
        }
        Start-Sleep -Seconds 2
    }
} elseif (-not $audio) {
    Note "Close any program using the $name before going on."
}
$saved = @()
if ($audio) { try { $saved = @(Get-AudioDefaults) } catch { } }

# --- 4. UA's package ---
$uaInf = Find-UaInf $hwid
if (-not $uaInf) {
    Note "UA's driver package (uad2pcie.inf, signed by Microsoft) with $hwid is not in the driver store."
    Note 'Reinstall UA Connect / the UAD software, then run this again. Nothing changed.'
    exit 1
}
Note "UA's package: $uaInf"

# --- 5. move this unit, and only this unit ---
if ($devcon) {
    $out = (& $devcon update $uaInf $hwid 2>&1 | Out-String)
    $rc = $LASTEXITCODE
    Note ">>> devcon update $uaInf $hwid"; Note $out; Note "exit $rc"
    if ($rc -eq 1) { Note "devcon: installed, but the old driver could not be stopped yet: power-cycle the $name to finish." }
} else {
    $out = (& $pnputil /remove-device $dev.InstanceId 2>&1 | Out-String)
    Note ">>> pnputil /remove-device $($dev.InstanceId)"; Note $out
    $out = (& $pnputil /scan-devices 2>&1 | Out-String)
    Note '>>> pnputil /scan-devices'; Note $out
}

# --- 7. check ---
Start-Sleep -Seconds 3
$after = @(Get-ApolloUnits)
$failed = $false
$now = $after | Where-Object { $_.Hwid -eq $hwid } | Select-Object -First 1
if (-not $now) {
    Note "$name is not present right now (re-enumerating, or switched off). Power-cycle it and check Device Manager."
} elseif ($now.Service -eq 'apollo') {
    $failed = $true
    Note "$name is STILL on the open driver ($($now.Inf)). Power-cycle it (or reboot) and run this again."
} else {
    Note "$name now: driver $($now.Inf) service $($now.Service) status $($now.Status)"
}
if ($now -and @(14, 21, 38, 47) -contains [int]$now.Problem) {
    Note "Windows could not stop the old driver (something still had the $name open): power-cycle it or reboot to finish."
}
foreach ($o in $others) {
    $p = $after | Where-Object { $_.InstanceId -eq $o.InstanceId } | Select-Object -First 1
    if (-not $p) { Note "WARNING: $($o.InstanceId) is no longer present (switched off or unplugged?)" }
    elseif ($p.Service -ne $o.Service -or $p.Inf -ne $o.Inf) {
        $failed = $true
        Note "WARNING: $($o.InstanceId) CHANGED: $($o.Inf)/$($o.Service) -> $($p.Inf)/$($p.Service). Tell Claude."
    } else { Note "unchanged: $($o.InstanceId) on $($p.Inf) ($($p.Service))" }
}

# --- 8. defaults, power cycle ---
if ($audio -and $saved.Count -gt 0) {
    Start-Sleep -Seconds 3
    try { foreach ($l in @(Restore-AudioDefaults $saved)) { Note $l } } catch { }
}
if ($isX8p) {
    Note 'Now power-cycle the x8p (off 10 s, on) so UA''s software starts it fresh. The Twin X behind it drops too and comes back on its own driver.'
} else {
    Note "Now power-cycle the $name (off 10 s, on) so UA's software starts it fresh. The x8p stays up."
}
Note "Log: $log"
if ($failed) { exit 1 }
exit 0
