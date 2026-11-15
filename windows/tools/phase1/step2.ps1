# Phase 2 step 2: install release4 (endpoints named "MON L/R") and register the ASIO driver. ADMIN.
#   powershell -ExecutionPolicy Bypass -File C:\Users\aaron\Desktop\Avium\apollo-win\tools\phase1\step2.ps1
# 1. Driver: release4 with EnabledEndpoints=3, StripSteps=1, through install.ps1 (defaults saved and put back).
# 2. ASIO: the built avium-asio.dll, checked against tools\bin\tools-manifest.txt, is copied to
#    %ProgramFiles%\Avium\ASIO (Program Files permissions: everyone reads, only admins change it),
#    checked again, and registered there with regsvr32. Ableton and Avium Measure then list
#    "Apollo x8p (Avium)". A DAW that already has the old copy loaded must be closed first.
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$pkg  = Join-Path $root 'builds\2026-11-14-release4\package'
$log  = Join-Path $PSScriptRoot 'step2.log'
"step2 $(Get-Date -Format o)" | Out-File -Encoding utf8 $log
function Note([string]$s) { Write-Host $s; Add-Content -Encoding utf8 $log $s }
$sys32 = Join-Path $env:SystemRoot 'System32'

# --- 1. driver
$key = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters'
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
Set-ItemProperty -Path $key -Name EnabledEndpoints -Value 3 -Type DWord
Set-ItemProperty -Path $key -Name StripSteps -Value 1 -Type DWord
& (Join-Path $sys32 'WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
    -File (Join-Path $PSScriptRoot 'install.ps1') -Package $pkg
$rc = $LASTEXITCODE
Note "install.ps1 exit $rc"
if ($rc -ne 0) { Note 'Driver install did not finish: tell Claude (install.log has the details). ASIO not registered.'; Read-Host 'Press Enter to close'; exit 1 }

# --- 2. ASIO
. (Join-Path $PSScriptRoot 'safe-tools.ps1')
$rel = 'asio\bin\avium-asio.dll'
$manifest = Read-ToolsManifest $root
$want = $manifest[$rel.ToLowerInvariant()]
$src = Join-Path $root $rel
if (-not $want -or -not (Test-Path -LiteralPath $src)) { Note "No manifest entry or no $rel; ASIO not registered."; Read-Host 'Press Enter to close'; exit 1 }
$dir = Join-Path $env:ProgramFiles 'Avium\ASIO'
$dst = Join-Path $dir 'avium-asio.dll'
try {
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    Copy-Item -LiteralPath $src -Destination $dst -Force -ErrorAction Stop
} catch {
    Note "Could not copy the ASIO DLL to $dst ($($_.Exception.Message)). Close Ableton / Avium Measure and run this again."
    Read-Host 'Press Enter to close'; exit 1
}
$got = (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash.ToUpperInvariant()
if ($got -ne $want) {
    Remove-Item -LiteralPath $dst -Force -ErrorAction SilentlyContinue
    Note "$dst does not match tools-manifest.txt (SHA-256 $got): not registered. Tell Claude."
    Read-Host 'Press Enter to close'; exit 1
}
$p = Start-Process -FilePath (Join-Path $sys32 'regsvr32.exe') -ArgumentList '/s', "`"$dst`"" -Wait -PassThru
Note "regsvr32 exit $($p.ExitCode) for $dst"
$asio = Get-ChildItem 'HKLM:\SOFTWARE\ASIO' -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -like '*Avium*' }
if ($asio) {
    foreach ($a in $asio) { Note ("ASIO driver registered: '{0}' CLSID {1}" -f $a.PSChildName, (Get-ItemProperty $a.PSPath).CLSID) }
} else {
    Note 'No Avium entry under HKLM\SOFTWARE\ASIO after regsvr32: tell Claude.'
}
Note 'Done. Tell Claude.'
Read-Host 'Press Enter to close'
