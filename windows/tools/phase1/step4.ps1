# Phase 2 step 4: install release6 = release5 + the strip probe (docs\STRIPS.md 10.9). ADMIN.
#   powershell -ExecutionPolicy Bypass -File C:\apollo-win\tools\phase1\step4.ps1
# Sets EnabledEndpoints=0x3FF, StripsEnable=1, StripMask=0xFF, StripSteps=9, MixerInitBatch=1 and removes
# StripProbe (absent = 2: after a cold start the driver measures every strip input and tries UA's
# stop+start and span-on-every-DSP repairs on dead ones, logging which one worked).
# Close Apollo Control first if it is an old build (the Start-menu one is fixed). Then power-cycle the x8p.
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$pkg  = Join-Path $root 'builds\2026-11-14-release6\package'
$log  = Join-Path $PSScriptRoot 'step4.log'
"step4 $(Get-Date -Format o)" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }

$key = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters'
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
Set-ItemProperty -Path $key -Name EnabledEndpoints -Value 0x3FF -Type DWord
Set-ItemProperty -Path $key -Name StripsEnable -Value 1 -Type DWord
Set-ItemProperty -Path $key -Name StripMask -Value 0xFF -Type DWord
Set-ItemProperty -Path $key -Name StripSteps -Value 9 -Type DWord
Set-ItemProperty -Path $key -Name MixerInitBatch -Value 1 -Type DWord
Remove-ItemProperty -Path $key -Name StripProbe -ErrorAction SilentlyContinue
$p = Get-ItemProperty $key
Note ("EnabledEndpoints=0x{0:X} StripsEnable={1} StripMask=0x{2:X2} StripSteps={3} MixerInitBatch={4} StripProbe={5}" -f $p.EnabledEndpoints, $p.StripsEnable, $p.StripMask, $p.StripSteps, $p.MixerInitBatch, $(if ($null -eq $p.StripProbe) { 'absent (2)' } else { $p.StripProbe }))

& (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
    -File (Join-Path $PSScriptRoot 'install.ps1') -Package $pkg
$rc = $LASTEXITCODE
Note "install.ps1 exit $rc"
if ($rc -ne 0) { Note 'Install did not finish: tell Claude (install.log has the details).' }
else { Note 'Done. Now save your work and power-cycle the x8p (off 10 s, on); wait about 30 s, then tell Claude.' }
Read-Host 'Press Enter to close'
