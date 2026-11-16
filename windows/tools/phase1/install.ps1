# Put apollo.sys on ONE Apollo and wait for it to come up. ADMIN PowerShell, after the setup reboot.
#   powershell -ExecutionPolicy Bypass -File C:\apollo-win\tools\phase1\install.ps1 [-Unit x8p|twinx] [-Package <folder with apollo.inf>] [-Yes]
# -Unit x8p (default, as every release so far): the x8p (SUBSYS 0x0014), full driver.
# -Unit twinx (gate G2, docs/spec-twinx.md 6.10 and 7): the Twin X Gen 1 (SUBSYS 0x0019) in OBSERVE mode:
#   Parameters\Models\99\Mode=0 is written BEFORE binding, so the driver only identifies and quiesces it
#   (no audio, no Windows endpoints, no mixer writes). Its speakers go silent until it goes back to UA:
#   revert-unit.ps1 -Unit twinx, then power-cycle the Twin X (the x8p stays up).
# Default package: the latest build (driver\x64\Release\apollo). Frozen, reviewed builds live in builds\<name>\package.
# Only the named unit is touched: devcon names its exact four-part hardware ID, and the script records every
# other Apollo's driver and state before and checks them after, and never deletes a package another unit uses.
# Both units share one loaded apollo.sys image, so -Unit twinx refuses a package other than the one the x8p
# runs (install it on the x8p first). One unit back to UA: revert-unit.ps1 -Unit twinx|x8p.
# Before PnP touches the driver it checks that nothing is using that unit (Windows refuses to stop a driver
# under a running audio stream). -Yes: do not ask, go ahead after a 60 s wait.
# UA's apps (UA Connect, Console) are closed only when the unit leaves UA's driver AND no other Apollo is still
# on UA's driver (UAD2Pcie): then nothing else needs them. Otherwise they keep running.
# Windows may make a new Apollo endpoint the default output; the script saves your defaults and puts them back.
# devcon.exe and apollo-diag.exe run only from verified admin-only copies (safe-tools.ps1): run
# tools\build-all.cmd after changing them, it writes the hashes this script checks.
#Requires -RunAsAdministrator
param([ValidateSet('x8p', 'twinx')][string]$Unit = 'x8p', [string]$Package = '', [switch]$Yes)
$ErrorActionPreference = 'Continue'
$root   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Package) { $Package = Join-Path $root 'driver\x64\Release\apollo' }
$inf    = Join-Path $Package 'apollo.inf'
if (-not (Test-Path $inf)) { Write-Host "No apollo.inf in $Package"; exit 1 }
# Frozen builds from before the saturating-add fix carry UNSAFE.txt (full-scale DC on playback): never install them.
$unsafe = Join-Path (Split-Path -Parent $Package) 'UNSAFE.txt'
if (Test-Path $unsafe) { Write-Host "Refusing $Package`: $(Get-Content -Raw $unsafe)"; exit 1 }
$log    = Join-Path $PSScriptRoot 'install.log'

# Units by name: exact four-part hardware IDs (docs/spec-twinx.md 0.1). devcon moves every present device
# with that ID and nothing else. Never shorten one to DEV_0002.
$known = @{
    'x8p'   = @{ Hwid = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00'; Name = 'Apollo x8p';    Type = '22' }
    'twinx' = @{ Hwid = 'PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00'; Name = 'Apollo Twin X'; Type = '99' }
}
$hwid   = $known[$Unit].Hwid
$name   = $known[$Unit].Name

"apollo phase 1 install $(Get-Date -Format o) -Unit $Unit" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }

# [release7 review] A real check of the ID about to be handed to devcon: one of the two known exact IDs,
# the one of the unit asked for, and listed by this package's INF (else devcon would fail half-way).
$exact = '^PCI\\VEN_1A00&DEV_0002&SUBSYS_(00141A00|00191A00)$'
if ($hwid -notmatch $exact -or ($Unit -eq 'x8p' -and $hwid -notmatch '00141A00$') -or ($Unit -eq 'twinx' -and $hwid -notmatch '00191A00$')) {
    Note "Refusing: hardware ID '$hwid' is not the exact ID of the $name."; exit 1
}
if (-not (Select-String -LiteralPath $inf -SimpleMatch -Pattern $hwid -Quiet)) {
    Note "Refusing: $inf does not list $hwid (this package has no install section for the $name)."; exit 1
}

# Every present Apollo with its exact hardware ID, driver and state.
function Get-ApolloUnits {
    foreach ($d in @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\VEN_1A00&DEV_0002&*' })) {
        $id = $d.InstanceId
        $hw = @((Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_HardwareIds -ErrorAction SilentlyContinue).Data)
        [pscustomobject]@{
            InstanceId = $id
            Hwid       = ($hw | Where-Object { $_ -match '^PCI\\VEN_1A00&DEV_0002&SUBSYS_[0-9A-Fa-f]{8}$' } | Select-Object -First 1)
            Service    = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
            Inf        = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
            Version    = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
            Status     = $d.Status
            Problem    = $d.Problem
        }
    }
}
function Format-Unit($u) { "$($u.InstanceId): driver $($u.Inf) $($u.Version) service $($u.Service) status $($u.Status) problem $($u.Problem)" }
function Run($exe, [string[]]$a) {
    $head = ">>> $(Split-Path -Leaf $exe) $($a -join ' ')"
    $out = (& $exe @a 2>&1 | Out-String)
    $tail = "exit $LASTEXITCODE"
    Write-Host $head; Write-Host $out; Write-Host $tail
    Add-Content -Encoding utf8 $log @($head, $out, $tail)
    return $out
}

$sys32 = Join-Path $env:SystemRoot 'System32'
$pnputil = Join-Path $sys32 'pnputil.exe'

# Tools run elevated only from verified, admin-only copies (safe-tools.ps1); Add-Type compiles there too.
. (Join-Path $PSScriptRoot 'safe-tools.ps1')
$tools = Initialize-SafeTools $root @('tools\bin\devcon.exe', 'tools\apollo-diag\apollo-diag.exe') ${function:Note}
if (-not $tools) { Note 'Stopped before changing anything.'; exit 1 }
$devcon = $tools['tools\bin\devcon.exe']
$diag   = $tools['tools\apollo-diag\apollo-diag.exe']

$audio = $true
try { . (Join-Path $PSScriptRoot 'audio-helpers.ps1') } catch { $audio = $false; Note "note: audio helpers unavailable ($($_.Exception.Message)); defaults will not be restored" }

# Preconditions
$opts = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control').SystemStartOptions
if ($opts -notmatch 'TESTSIGNING') { Note "Test signing is not active (boot options: $opts). Run setup.ps1 and reboot first."; exit 1 }
$all = @(Get-ApolloUnits)
$same = @($all | Where-Object { $_.Hwid -eq $hwid })
if ($same.Count -eq 0) { Note "$name not found ($hwid): is it connected and powered on?"; exit 1 }
if ($same.Count -gt 1) {
    Note "$($same.Count) present devices carry $hwid; devcon would move all of them. Nothing changed:"
    foreach ($o in $same) { Note "  $(Format-Unit $o)" }
    exit 1
}
$target = $same[0]
Note "$name`: $(Format-Unit $target)"
$othersBefore = @($all | Where-Object { $_.InstanceId -ne $target.InstanceId })
foreach ($o in $othersBefore) { Note "other Apollo (left as it is): $(Format-Unit $o)" }
# A package whose INF also lists another Apollo is still only bound to the named unit here (devcon names $hwid).
$infOthers = @(Select-String -LiteralPath $inf -Pattern 'PCI\\VEN_1A00&DEV_0002&SUBSYS_[0-9A-Fa-f]{8}' -AllMatches |
    ForEach-Object { $_.Matches } | ForEach-Object { $_.Value.ToUpperInvariant() } | Where-Object { $_ -ne $hwid } | Sort-Object -Unique)
if ($infOthers.Count -gt 0) { Note "note: this package also lists $($infOthers -join ', '); install.ps1 binds only the $name ($hwid)" }
Run (Join-Path $sys32 'verifier.exe') @('/querysettings') | Out-Null

# The package's DriverVer, and the driver version every other unit on apollo.sys runs.
$pkgVer = ''
$m = Select-String -LiteralPath $inf -Pattern '^\s*DriverVer\s*=\s*[^,]*,\s*([0-9.]+)' | Select-Object -First 1
if ($m) { $pkgVer = $m.Matches[0].Groups[1].Value }
Note "package: $inf DriverVer $pkgVer"
# One service, one loaded image: while another unit keeps apollo.sys loaded, a unit (re)bound now joins that
# image, whatever package its INF says. So every other unit on apollo.sys must run this very package.
foreach ($o in @($othersBefore | Where-Object { $_.Service -eq 'apollo' })) {
    if (-not $pkgVer -or $o.Version -ne $pkgVer) {
        Note "Refusing: $($o.InstanceId) runs apollo.sys $($o.Version), this package is $pkgVer. Both units share one loaded"
        if ($Unit -eq 'twinx') {
            Note "driver image, so put this package on the x8p first (install.ps1 -Unit x8p -Package '$Package', then"
            Note "power-cycle the x8p), then run this again. step-g2.ps1 does that for you. Nothing changed."
        } else {
            Note "driver image, so first move that unit back to UA (revert-unit.ps1 -InstanceId '$($o.InstanceId)'), then"
            Note "run this again. Nothing changed."
        }
        exit 1
    }
}
if ($Unit -eq 'twinx') {
    $x8p = $othersBefore | Where-Object { $_.Hwid -eq $known['x8p'].Hwid } | Select-Object -First 1
    if ($x8p) {
        Note 'x8p before (apollo-diag --unit x8p info):'
        $x8pInfoBefore = Run $diag @('--unit', 'x8p', 'info')
    }
}

# Nothing may be streaming on the unit while PnP stops its old driver.
if ($Unit -eq 'x8p') {
    if ($audio) {
        if (-not (Wait-ApolloIdle $diag -Yes:$Yes -Note ${function:Note})) { Note 'Stopped before changing anything.'; exit 1 }
    } else {
        Note 'Close any program using the x8p (Ableton, asio-test, loopback-test, wasapi-loop) before going on.'
    }
} elseif ($audio) {
    # Twin X: its Windows audio today is UA's WDM device ("Universal Audio Thunderbolt WDM"), or ours ("Apollo Twin X").
    $deadline = (Get-Date).AddSeconds(60)
    $told = $false
    while ($true) {
        $busy = @()
        try { $busy = @(Get-ApolloAudioSessions -Filter '(Universal Audio Thunderbolt WDM)') + @(Get-ApolloAudioSessions -Filter '(Apollo Twin X') } catch { }
        if ($busy.Count -eq 0) { break }
        if (-not $told) {
            Note 'The Twin X is in use:'
            foreach ($s in $busy) { Note "  - $($s.Process) (PID $($s.Pid)) is using $($s.Endpoint) for $($s.Flow)" }
            Note 'Stop that playback/recording (the Twin X speakers go silent in G2 anyway). Waiting up to 60 s...'
            $told = $true
        }
        if ((Get-Date) -gt $deadline) {
            if ($Yes) { Note '-Yes given: going ahead. If Windows cannot stop UA''s driver it asks for a Twin X power cycle.'; break }
            $a = Read-Host '[Enter] go ahead anyway, [Q] quit'
            if ($a -match '^[Qq]') { Note 'Stopped before changing anything.'; exit 1 }
            break
        }
        Start-Sleep -Seconds 2
    }
} else {
    Note 'Stop any playback on the Twin X before going on.'
}

# Your default playback/recording devices, to put back if Windows moves them.
$saved = @()
if ($audio) {
    try { $saved = @(Get-AudioDefaults); Note 'Default audio devices before the install:'; Format-AudioDefaults $saved | ForEach-Object { Note $_ } }
    catch { Note "note: could not read the default audio devices ($($_.Exception.Message))" }
}

# UA's apps talk to every unit on UA's driver. Close them only when this unit is leaving UA's driver and no
# other present Apollo is still on it (then nothing else needs them); otherwise leave them running.
$onUa = @($othersBefore | Where-Object { $_.Service -eq 'UAD2Pcie' })
if (-not $target.Service) { Note "note: could not read the $name's current driver service; UA's apps are left running" }
elseif ($target.Service -eq 'apollo') { Note "The $name is already on apollo.sys: UA's apps are left alone." }
elseif ($onUa.Count -gt 0) {
    Note "UA's apps are left running: $($onUa[0].InstanceId) still uses UA's driver. If devcon says a restart is needed, power-cycle the $name."
} else {
    Get-Process | Where-Object { $_.Path -match 'Universal Audio|UA Connect' } | ForEach-Object {
        Note "closing $($_.ProcessName) ($($_.Path)): no other Apollo needs it"; Stop-Process -Id $_.Id -Force
    }
}

# Twin X: observe mode BEFORE the driver ever starts on it (docs/spec-twinx.md 6.9). Models\99 applies to every
# Twin X Gen 1; a Units\<key> value would win, so list any that exist.
if ($Unit -eq 'twinx') {
    $params = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters'
    $mkey = Join-Path $params 'Models\99'
    try {
        if (-not (Test-Path $mkey)) { New-Item -Path $mkey -Force | Out-Null }
        Set-ItemProperty -Path $mkey -Name Mode -Value 0 -Type DWord
        Note "Models\99\Mode = $((Get-ItemProperty $mkey).Mode) (observe: identify and quiesce only)"
    } catch {
        Note "Could not write $mkey\Mode ($($_.Exception.Message)). Stopped before binding the Twin X."; exit 1
    }
    # [G3] Since gate G3 a Twin X whose own unit key says Mode 1 IS brought up (bring-up + transport), so a
    # key left at Mode 1 (step-g3.ps1) would not observe. The x8p's own key (if any) is left alone.
    $x8pKey = ''
    if ($x8pInfoBefore -and $x8pInfoBefore -match '(?m)^UnitKey\s+([0-9A-Fa-f]{16}|SN[0-9A-Za-z]+)\s*$') { $x8pKey = $Matches[1] }
    $ukeys = Join-Path $params 'Units'
    $active = @()
    if (Test-Path $ukeys) {
        foreach ($k in @(Get-ChildItem $ukeys -ErrorAction SilentlyContinue)) {
            $mv = (Get-ItemProperty $k.PSPath -ErrorAction SilentlyContinue).Mode
            Note "note: unit key $($k.PSChildName) has Mode $(if ($null -eq $mv) { '(not set)' } else { $mv })$(if ($k.PSChildName -eq $x8pKey) { ' (the x8p)' } else { '' })"
            if ($null -ne $mv -and [int]$mv -ne 0 -and $k.PSChildName -ne $x8pKey) { $active += $k.PSChildName }
        }
    }
    if ($active.Count -gt 0) {
        Note "WARNING: unit key(s) $($active -join ', ') ask for Mode 1 or 2. If one is this Twin X, the driver brings it up"
        Note '(gate G3) instead of only observing. For observe: step-g3.ps1 -Rollback (sets its Mode to 0) first.'
        if ($Yes) {
            # Unattended binds are observe binds: never bring a unit up without someone confirming it.
            Note 'Stopped before binding the Twin X (-Yes never binds a unit whose key asks for Mode 1 or 2).'
            exit 1
        }
        $a = Read-Host '[Enter] bind anyway (that unit runs its Mode), [Q] quit'
        if ($a -match '^[Qq]') { Note 'Stopped before binding the Twin X.'; exit 1 }
    }
}

# Stage our package, then force it onto the named unit (UA's WHQL package ranks higher).
Run $pnputil @('/add-driver', $inf) | Out-Null
Run $devcon @('update', $inf, $hwid) | Out-Null
$devconRc = $LASTEXITCODE
if ($devconRc -eq 1) {
    # devcon: 1 = installed, but the running driver could not be stopped (something held the device).
    if ($Unit -eq 'twinx') {
        Note 'NOTE: the open driver is installed on the Twin X but NOT running yet (devcon: restart needed). Power-cycle the TWIN X (off 10 s, on); the x8p stays up. Then run apollo-diag --unit twinx info.'
    } else {
        Note 'NOTE: the new driver is installed but NOT running yet (devcon: restart needed). Power-cycle the x8p (or reboot) to load it; until then the old build keeps running.'
    }
}

Start-Sleep -Seconds 2
$after = @(Get-ApolloUnits)
$now = $after | Where-Object { $_.InstanceId -eq $target.InstanceId } | Select-Object -First 1
$probStatus = $null
if ($now) { $probStatus = (Get-PnpDeviceProperty -InstanceId $now.InstanceId -KeyName DEVPKEY_Device_ProblemStatus -ErrorAction SilentlyContinue).Data }
Note "$name now: $(if ($now) { Format-Unit $now } else { 'not present' })"

# Every other Apollo must be exactly as before: same driver, version, service and state. devcon named only $hwid.
$othersChanged = $false
foreach ($o in $othersBefore) {
    $p = $after | Where-Object { $_.InstanceId -eq $o.InstanceId } | Select-Object -First 1
    if (-not $p) { $othersChanged = $true; Note "WARNING: $($o.InstanceId) is no longer present. Tell Claude." }
    elseif ($p.Service -ne $o.Service -or $p.Inf -ne $o.Inf -or $p.Version -ne $o.Version -or $p.Status -ne $o.Status -or $p.Problem -ne $o.Problem) {
        $othersChanged = $true
        Note "WARNING: $($o.InstanceId) CHANGED: $(Format-Unit $o) -> $(Format-Unit $p). Tell Claude; to put it back: revert-unit.ps1 -InstanceId '$($o.InstanceId)'"
    } else { Note "unchanged: $(Format-Unit $p)" }
}

# Older staged copies of our package: remove the ones no Apollo is using (this unit or any other).
$inUse = @($after | ForEach-Object { $_.Inf })
$blocks = (& $pnputil /enum-drivers | Out-String) -split "\r?\n\r?\n"
foreach ($block in $blocks) {
    if ($block -match 'Original Name:\s+apollo\.inf' -and $block -match '(?m)^\s*Provider Name:\s+Avium\s*$' -and
        $block -match 'Published Name:\s+(oem\d+\.inf)' -and $inUse -notcontains $Matches[1]) {
        Run $pnputil @('/delete-driver', $Matches[1]) | Out-Null
    }
}

if (-not $now -or $now.Status -ne 'OK') {
    if ($now) { Note ("The driver did not start on the {0}: problem code {1}, status 0x{2:X8}. Tell Claude; this log has what it needs." -f $name, $now.Problem, [uint32]$probStatus) }
    if ($now -and @(14, 21, 38, 47) -contains [int]$now.Problem) {
        Note "The old driver could not be stopped (something still had the $name open): power-cycle the $name to finish."
    }
    exit 1
}
if ($now.Service -ne 'apollo') {
    Note "The $name is still on $($now.Service): devcon did not move it (see above). Power-cycle the $name, then check apollo-diag units."
    exit 1
}

if ($Unit -eq 'x8p') {
    # Bring-up runs in a work item (ACEFACE can take ~20 s, DSP ready 30 s+).
    $deadline = (Get-Date).AddSeconds(120)
    do {
        Start-Sleep -Seconds 3
        $info = (& $diag --unit x8p info 2>&1 | Out-String)
        $stage = ($info -split "`n" | Where-Object { $_ -match '^Stage ' }) -join ''
        Write-Host "  $stage"
    } while ((Get-Date) -lt $deadline -and $stage -notmatch 'Connected \(|FAILED|GONE')
    Run $diag @('--unit', 'x8p', 'info') | Out-Null
    Run $diag @('--unit', 'x8p', 'log') | Out-Null
} else {
    # Observe: identification and the quiesce happen in PrepareHardware, so the unit answers at once.
    $deadline = (Get-Date).AddSeconds(30)
    $ok = $false
    do {
        Start-Sleep -Seconds 2
        $info = (& $diag --unit twinx info 2>&1 | Out-String)
        $ok = ($info -match '(?m)^Stage\s+(1|256|512)\s')
    } while ((Get-Date) -lt $deadline -and -not $ok)
    Run $diag @('units') | Out-Null
    Run $diag @('--unit', 'twinx', 'info') | Out-Null
    Run $diag @('--unit', 'twinx', 'log') | Out-Null
    if (-not $ok) { Note 'The Twin X did not answer apollo-diag --unit twinx within 30 s: see apollo-diag units and the log above.' }
    # No Windows endpoint may hang off the Twin X in G2.
    $eps = @(Get-PnpDevice -Class AudioEndpoint -PresentOnly -ErrorAction SilentlyContinue | Where-Object {
            (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Parent -ErrorAction SilentlyContinue).Data -eq $now.InstanceId })
    if ($eps.Count -eq 0) { Note 'Twin X Windows endpoints on apollo.sys: none (as G2 requires).' }
    else { Note "WARNING: $($eps.Count) Windows endpoint(s) under the Twin X: $(@($eps | ForEach-Object { $_.FriendlyName }) -join ', '). Tell Claude." }
    # The x8p after: still connected, same endpoints.
    if ($x8p) {
        Note 'x8p after (apollo-diag --unit x8p info, endpoints):'
        $x8pInfoAfter = Run $diag @('--unit', 'x8p', 'info')
        Run $diag @('--unit', 'x8p', 'endpoints') | Out-Null
        $sb = ($x8pInfoBefore -split "`n" | Where-Object { $_ -match '^Stage ' }) -join ''
        $sa = ($x8pInfoAfter -split "`n" | Where-Object { $_ -match '^Stage ' }) -join ''
        if ($sb.Trim() -ne $sa.Trim()) { $othersChanged = $true; Note "WARNING: the x8p's stage changed: '$($sb.Trim())' -> '$($sa.Trim())'. Tell Claude." }
        else { Note "x8p stage unchanged: $($sa.Trim())" }
    }
}

# Windows audio endpoints and the defaults (Windows may switch shortly after endpoints appear or vanish).
if ($audio -and $saved.Count -gt 0) {
    if ($Unit -eq 'x8p') { Run $diag @('--unit', 'x8p', 'endpoints') | Out-Null }
    $moved = 0
    # Checks at about 4, 8 and 20 s: Windows can move a default a while after an endpoint appears.
    foreach ($wait in 4, 4, 12) {
        Start-Sleep -Seconds $wait
        try {
            $lines = @(Restore-AudioDefaults $saved)
            foreach ($l in $lines) { Note $l }
            $moved += $lines.Count
        } catch { Note "note: could not check the default audio devices ($($_.Exception.Message))" }
    }
    if ($moved -eq 0) { Note 'Default audio devices unchanged.' }
    try {
        $ours = @(Get-AudioEndpoints | Where-Object { $_.Name -like '*(Apollo x8p*' -or $_.Name -like '*(Apollo Twin X*' })
        if ($ours.Count -gt 0) {
            Note 'Apollo endpoints in Windows:'
            foreach ($e in $ours) { Note ("  {0,-9} {1}" -f $(if ($e.Flow -eq 0) { 'playback' } else { 'recording' }), $e.Name) }
        }
    } catch { }
}
if ($othersChanged) { Note 'Another Apollo changed during this install: see the WARNING lines above.' }
Note "Finished. Log: $log"
Note "Control app (no admin needed): $(Join-Path $root 'app\bin\apollo-control.exe')"
if ($othersChanged) { exit 1 }
exit 0
