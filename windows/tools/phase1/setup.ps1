# Phase 1, step 1: one-time test-machine setup. Run in an ADMIN PowerShell, then reboot.
#   powershell -ExecutionPolicy Bypass -File C:\apollo-win\tools\phase1\setup.ps1
# Undo when development is over: bcdedit /set testsigning off ; verifier /reset
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$cer  = Join-Path $root 'driver\x64\Release\apollo.cer'
$log  = Join-Path $PSScriptRoot 'setup.log'

"apollo phase 1 setup $(Get-Date -Format o)" | Out-File -Encoding utf8 $log
function Run($exe, [string[]]$a) {
    $head = ">>> $exe $($a -join ' ')"
    $out = (& $exe @a 2>&1 | Out-String)
    $tail = "exit $LASTEXITCODE"
    Write-Host $head; Write-Host $out; Write-Host $tail
    Add-Content -Encoding utf8 $log @($head, $out, $tail)
}

if (-not (Test-Path $cer)) { Write-Host "missing $cer"; exit 1 }

Run 'bcdedit.exe'  @('/set', 'testsigning', 'on')
Run 'verifier.exe' @('/standard', '/driver', 'apollo.sys')
Run 'certutil.exe' @('-addstore', 'Root', $cer)
Run 'certutil.exe' @('-addstore', 'TrustedPublisher', $cer)
Run 'bcdedit.exe'  @('/enum', '{current}')
Run 'verifier.exe' @('/querysettings')

Add-Content -Encoding utf8 $log 'done'
Write-Host ''
Write-Host 'Setup finished. Save your work and reboot. "Test Mode" appears in the corner after the reboot.'
