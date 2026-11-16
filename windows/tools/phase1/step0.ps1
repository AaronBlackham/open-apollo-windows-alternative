# Phase 2 step 0: install release1 with every Windows audio endpoint OFF (ACX init only). ADMIN PowerShell.
#   powershell -ExecutionPolicy Bypass -File C:\apollo-win\tools\phase1\step0.ps1
# Proves the private interface (apollo-diag, Apollo Control) still works on an ACX device.
# Strips stay at uploads only (StripSteps=1). Endpoints are turned on later (step 2).
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$pkg  = Join-Path $root 'builds\2026-11-14-release1\package'
$log  = Join-Path $PSScriptRoot 'step0.log'
"step0 $(Get-Date -Format o)" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }

$key = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters'
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
Set-ItemProperty -Path $key -Name EnabledEndpoints -Value 0 -Type DWord
Set-ItemProperty -Path $key -Name StripSteps -Value 1 -Type DWord
$p = Get-ItemProperty $key
Note ("EnabledEndpoints={0} StripsEnable={1} StripSteps={2}" -f $p.EnabledEndpoints, $p.StripsEnable, $p.StripSteps)

& (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
    -File (Join-Path $PSScriptRoot 'install.ps1') -Package $pkg
$rc = $LASTEXITCODE
Note "install.ps1 exit $rc"
if ($rc -ne 0) { Note 'Install did not finish: tell Claude (install.log has the details).'; exit 1 }

# Admin-only command, run from the verified admin-only copy: proves the admin path still works under ACX.
$diag = Join-Path $env:ProgramFiles 'Avium\ApolloTools\apollo-diag.exe'
if (Test-Path $diag) {
    $out = (& $diag read 0x2218 2>&1 | Out-String)
    Note "admin read 0x2218 (FPGA_REV): $($out.Trim())"
} else {
    Note "no verified apollo-diag at $diag"
}
Note 'Done. Tell Claude; then open Apollo Control from apollo-win\app\bin\apollo-control.exe.'
