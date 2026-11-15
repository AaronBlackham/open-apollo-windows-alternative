# Strips test T3 (uploads only), one command. ADMIN PowerShell. Then power-cycle the x8p.
#   powershell -ExecutionPolicy Bypass -File C:\Users\aaron\Desktop\Avium\apollo-win\tools\phase1\t3.ps1
# Uses the reviewed, frozen build in builds\2026-11-14-t3-strip-uploads (driver + its matching apollo-diag).
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$snap = Join-Path $root 'builds\2026-11-14-t3-strip-uploads'
$diag = Join-Path $snap 'src\apollo-diag\apollo-diag.exe'
$log  = Join-Path $PSScriptRoot 't3.log'
"T3 setup $(Get-Date -Format o)" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }

foreach ($p in 'loopback-test', 'asio-test', 'apollo-control') {
    if (Get-Process $p -ErrorAction SilentlyContinue) {
        Note "Close $p first (it holds the device open), then run this again."
        exit 1
    }
}

Note '== 1. Install the reviewed T3 driver =='
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'install.ps1') -Package (Join-Path $snap 'package')
if ($LASTEXITCODE -ne 0) { Note 'install.ps1 failed: see install.log. Stopping.'; exit 1 }

Note '== 2. Copy the five strip modules from your UAD install =='
$out = (& $diag strips install 2>&1 | Out-String)
Note $out
if ($out -notmatch 'Installed') {
    Note 'Trying the backup copies in ua-files...'
    $out = (& $diag strips install (Join-Path $root 'ua-files') 2>&1 | Out-String)
    Note $out
    if ($out -notmatch 'Installed') { Note 'Module install failed. Stopping.'; exit 1 }
}

Note '== 3. Turn on uploads only =='
$key = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters'
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
Set-ItemProperty -Path $key -Name StripsEnable -Value 1 -Type DWord
Set-ItemProperty -Path $key -Name StripSteps -Value 1 -Type DWord
Note ("StripsEnable={0} StripSteps={1}" -f (Get-ItemProperty $key).StripsEnable, (Get-ItemProperty $key).StripSteps)

Note ''
Note 'Done. Now POWER-CYCLE THE x8p: off 10 seconds, on. Wait about 15 seconds, then tell Claude.'
Note 'To turn strips off again: Set-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters StripsEnable 0'
