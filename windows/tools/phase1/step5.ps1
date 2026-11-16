# Phase 2 step 5 (Twin X gate G1): install release7 on the x8p = release6 + Twin X groundwork (model identification, per-unit settings, serial fix), strip probe off. ADMIN.
#   powershell -ExecutionPolicy Bypass -File C:\apollo-win\tools\phase1\step5.ps1
# Sets EnabledEndpoints=0x3FF, StripsEnable=1, StripMask=0xFF, StripSteps=9, MixerInitBatch=1, StripProbe=0
# (the probe is diagnostics only: the "dead" inputs were empty jacks, which the x8p mutes by design).
# Only the x8p is touched; the Twin X stays on UA's driver (install.ps1 refuses any other unit).
# Close Apollo Control first if it is an old build (the Start-menu one is fixed). Then power-cycle the x8p.
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$pkg  = Join-Path $root 'builds\2026-11-14-release7\package'
$log  = Join-Path $PSScriptRoot 'step5.log'
"step5 $(Get-Date -Format o)" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }

$key = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters'
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
Set-ItemProperty -Path $key -Name EnabledEndpoints -Value 0x3FF -Type DWord
Set-ItemProperty -Path $key -Name StripsEnable -Value 1 -Type DWord
Set-ItemProperty -Path $key -Name StripMask -Value 0xFF -Type DWord
Set-ItemProperty -Path $key -Name StripSteps -Value 9 -Type DWord
Set-ItemProperty -Path $key -Name MixerInitBatch -Value 1 -Type DWord
Set-ItemProperty -Path $key -Name StripProbe -Value 0 -Type DWord
$p = Get-ItemProperty $key
Note ("EnabledEndpoints=0x{0:X} StripsEnable={1} StripMask=0x{2:X2} StripSteps={3} MixerInitBatch={4} StripProbe={5}" -f $p.EnabledEndpoints, $p.StripsEnable, $p.StripMask, $p.StripSteps, $p.MixerInitBatch, $(if ($null -eq $p.StripProbe) { 'absent (0)' } else { $p.StripProbe }))

& (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
    -File (Join-Path $PSScriptRoot 'install.ps1') -Package $pkg
$rc = $LASTEXITCODE
Note "install.ps1 exit $rc"
if ($rc -ne 0) { Note 'Install did not finish: tell Claude (install.log has the details).' }
else { Note 'Done. Now save your work and power-cycle the x8p (off 10 s, on) so its input strips rebuild; the Twin X behind it drops out and comes back on UA''s driver (speakers briefly silent). Wait about 30 s, then tell Claude.' }
Read-Host 'Press Enter to close'
