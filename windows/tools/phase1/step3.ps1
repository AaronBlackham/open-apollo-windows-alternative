# Phase 2 step 3: install release5 = the multi-strip fix + Windows MIC/LINE 1-8 endpoints. ADMIN.
#   powershell -ExecutionPolicy Bypass -File C:\Users\aaron\Desktop\Avium\apollo-win\tools\phase1\step3.ps1
# Sets: EnabledEndpoints=0x3FF (MON L/R out+in, MIC/LINE 1-8), StripsEnable=1, StripMask=0xFF (all 8
# inputs), StripSteps=9 (whole chain), MixerInitBatch=1 (x8p-safe: the unit keeps its own gains/polarity/48V).
# Then install.ps1 (saves and restores your default devices). Strips build only on a COLD start:
# power-cycle the x8p afterwards.
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$pkg  = Join-Path $root 'builds\2026-11-14-release5\package'
$log  = Join-Path $PSScriptRoot 'step3.log'
"step3 $(Get-Date -Format o)" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }

$key = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters'
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
Set-ItemProperty -Path $key -Name EnabledEndpoints -Value 0x3FF -Type DWord
Set-ItemProperty -Path $key -Name StripsEnable -Value 1 -Type DWord
Set-ItemProperty -Path $key -Name StripMask -Value 0xFF -Type DWord
Set-ItemProperty -Path $key -Name StripSteps -Value 9 -Type DWord
Set-ItemProperty -Path $key -Name MixerInitBatch -Value 1 -Type DWord
$p = Get-ItemProperty $key
Note ("EnabledEndpoints=0x{0:X} StripsEnable={1} StripMask=0x{2:X2} StripSteps={3} MixerInitBatch={4}" -f $p.EnabledEndpoints, $p.StripsEnable, $p.StripMask, $p.StripSteps, $p.MixerInitBatch)

& (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
    -File (Join-Path $PSScriptRoot 'install.ps1') -Package $pkg
$rc = $LASTEXITCODE
Note "install.ps1 exit $rc"
if ($rc -ne 0) { Note 'Install did not finish: tell Claude (install.log has the details).' }
else { Note 'Done. Now save your work and power-cycle the x8p (off 10 s, on), then tell Claude.' }
Read-Host 'Press Enter to close'
