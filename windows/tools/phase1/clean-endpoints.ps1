# clean-endpoints.ps1 - [Endpoints] one-time cleanup: removes the Windows audio endpoints earlier apollo.sys
# builds left behind ("Unknown" in Device Manager, leftovers in the Sound control panel): the SWD\MMDEVAPI
# endpoint devnodes that are NOT present and whose parent is one of the Apollo units (the x8p's or the Twin
# X's PCI device on apollo.sys). Present endpoints, and every endpoint of any other device, are never touched.
# It lists what it found before removing anything.
#
#   powershell -ExecutionPolicy Bypass -File tools\phase1\clean-endpoints.ps1            (elevated: list, then remove)
#   powershell -ExecutionPolicy Bypass -File tools\phase1\clean-endpoints.ps1 -ListOnly  (any user: list only)
#
# Why they exist: Windows keeps an endpoint devnode (and the name it cached for it) for every circuit a driver
# ever registered. The Nov 15 endpoint tables (driver\endpoint_plan.h) use new reference strings, so every
# endpoint of an earlier build ("MON L/R", "MIC/LINE n", "Speakers", "Monitor", ...) stays behind as not
# present. pnputil /remove-device deletes such a devnode; if the driver ever registers that circuit again,
# Windows simply creates it anew.
# PowerShell 5.1, ASCII only. Writes tools\phase1\clean-endpoints.log.
param(
    [switch]$ListOnly
)

$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot 'clean-endpoints.log'
function Note([string]$s) { Write-Host $s; Add-Content -LiteralPath $log -Value $s -Encoding ASCII }

Note ("==== clean-endpoints {0}{1}" -f (Get-Date -Format o), $(if ($ListOnly) { ' (list only)' } else { '' }))

# The Apollo units: the two exact hardware IDs apollo.inf matches (present or not).
$units = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
    $_.InstanceId -match '^PCI\\VEN_1A00&DEV_0002&SUBSYS_00(14|19)1A00&'
})
if ($units.Count -eq 0) { Note 'No Apollo x8p or Twin X device is known to Windows: nothing to do.'; exit 0 }
$unitIds = @{}
foreach ($u in $units) {
    $unitIds[$u.InstanceId.ToUpperInvariant()] = $u
    Note ("unit   {0,-8} {1}  [{2}]" -f $u.Status, $u.InstanceId, $u.FriendlyName)
}

$present = @{}
foreach ($d in @(Get-PnpDevice -Class AudioEndpoint -PresentOnly -ErrorAction SilentlyContinue)) {
    $present[$d.InstanceId.ToUpperInvariant()] = $true
}

$stale = @()
$kept = 0
foreach ($d in @(Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue)) {
    if ($d.InstanceId -notmatch '^SWD\\MMDEVAPI\\') { continue }
    $parent = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue).Data
    if (-not $parent -or -not $unitIds.ContainsKey(([string]$parent).ToUpperInvariant())) { continue }
    if ($present.ContainsKey($d.InstanceId.ToUpperInvariant())) { $kept++; continue }
    $stale += [pscustomobject]@{ Name = $d.FriendlyName; Id = $d.InstanceId; Parent = $parent }
}

Note ("Apollo endpoints present (kept): {0}" -f $kept)
if ($stale.Count -eq 0) { Note 'No stale Apollo endpoint: nothing to remove.'; exit 0 }
Note ("Stale Apollo endpoints (not present, parent = an Apollo unit): {0}" -f $stale.Count)
foreach ($s in $stale) {
    $who = if ($s.Parent -match 'SUBSYS_0014') { 'x8p' } else { 'Twin X' }
    Note ("  [{0}] {1}  {2}" -f $who, $s.Name, $s.Id)
}
if ($ListOnly) { Note 'List only: nothing removed.'; exit 0 }

$admin = (New-Object System.Security.Principal.WindowsPrincipal([System.Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { Note 'Removing needs an elevated prompt (Run as administrator); nothing removed.'; exit 1 }

$removed = 0; $failed = 0
foreach ($s in $stale) {
    # Re-check right before removing: still not present, still an Apollo child.
    $now = Get-PnpDevice -InstanceId $s.Id -ErrorAction SilentlyContinue
    if ($null -eq $now) { continue }
    if (@(Get-PnpDevice -InstanceId $s.Id -PresentOnly -ErrorAction SilentlyContinue).Count -gt 0) {
        Note "  skipped (present again): $($s.Name)"
        continue
    }
    $out = (& pnputil.exe /remove-device $s.Id 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -eq 0) { $removed++; Note "  removed: $($s.Name)" }
    else { $failed++; Note ("  FAILED ({0}): {1}: {2}" -f $LASTEXITCODE, $s.Name, ($out -replace '\s+', ' ')) }
}
Note ("Removed {0}, failed {1}." -f $removed, $failed)
exit $(if ($failed -gt 0) { 2 } else { 0 })
