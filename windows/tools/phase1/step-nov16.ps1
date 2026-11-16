# step-nov16.ps1 - one UAC for Nov 16 2026: install builds\2026-11-16-outputs (output settings, identify, driver
# Parameters IOCTLs) with step-ep.ps1 on both Apollos, then the chain-hang test (chainfix-hw.ps1, steps 1-5; it stops
# at the first quarantine: then only an x8p power cycle helps). ADMIN PowerShell:
#   powershell -ExecutionPolicy Bypass -File tools\phase1\step-nov16.ps1
# PowerShell 5.1, ASCII only. Logs: step-ep.log, chainfix-hw.log, step-nov16.log (all next to this script).
param([switch]$NoChain)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$log = Join-Path $PSScriptRoot 'step-nov16.log'
"step-nov16 $(Get-Date -Format o)" | Out-File -Encoding ascii $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding ascii $log $s }

$pkg = Join-Path $root 'builds\2026-11-16-outputs\package'
Note "1/2 install $pkg (step-ep.ps1)"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'step-ep.ps1') -Package $pkg -Yes
$rc = $LASTEXITCODE
Note "step-ep exit $rc"
if ($rc -ne 0) { Note 'Install did not finish: the chain test is skipped.'; Read-Host 'Press Enter to close' | Out-Null; exit $rc }
if ($NoChain) { Note 'Chain test skipped (-NoChain).'; exit 0 }

# the units' bring-up and the x8p's warm strip adoption settle within a few seconds of the install
Start-Sleep -Seconds 8
Note '2/2 chain-hang test (chainfix-hw.ps1, steps 1-5)'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'chainfix-hw.ps1') -Upto 5 -Yes
Note "chainfix-hw exit $LASTEXITCODE"
Read-Host 'Done. Press Enter to close' | Out-Null
