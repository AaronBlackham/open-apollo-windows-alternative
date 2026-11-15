# Phase 2 step 1: install release3 with the two Windows endpoints ON (MON L/R playback + recording). ADMIN PowerShell.
#   powershell -ExecutionPolicy Bypass -File C:\Users\aaron\Desktop\Avium\apollo-win\tools\phase1\step1.ps1
# release3 = release1 + drift-free 1 ms engine timer + the saturating-add fix (ASIO and ACX render
# wrote 0x80000000 into every sample) + review fixes (capture packets, parked packet memory, no
# register reads while the engine is lost). Strips stay at uploads only (StripSteps=1).
# install.ps1 saves your default audio devices and puts them back (checks at about 4, 8 and 20 s).
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$pkg  = Join-Path $root 'builds\2026-11-14-release3\package'
$log  = Join-Path $PSScriptRoot 'step1.log'
"step1 $(Get-Date -Format o)" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }

$key = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters'
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
Set-ItemProperty -Path $key -Name EnabledEndpoints -Value 3 -Type DWord
Set-ItemProperty -Path $key -Name StripSteps -Value 1 -Type DWord
$p = Get-ItemProperty $key
Note ("EnabledEndpoints={0} StripsEnable={1} StripSteps={2}" -f $p.EnabledEndpoints, $p.StripsEnable, $p.StripSteps)

& (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
    -File (Join-Path $PSScriptRoot 'install.ps1') -Package $pkg
$rc = $LASTEXITCODE
Note "install.ps1 exit $rc"
if ($rc -ne 0) { Note 'Install did not finish: tell Claude (install.log has the details).'; exit 1 }
Note 'Done. Tell Claude. Open Sound settings (Win+R, mmsys.cpl) and check your speakers still have the green tick.'
