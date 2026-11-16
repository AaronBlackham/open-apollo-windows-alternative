# run-sandbox-check.ps1 - the release installer on a clean PC: Windows Sandbox (networking off), Setup in /SANDBOXTEST=1
# mode (per-PC signing, driver staged, no unit switched), every install check, the uninstaller, every removal check.
#   powershell -ExecutionPolicy Bypass -File installer\sandbox\run-sandbox-check.ps1 [-Installer <exe>] [-Work <folder>]
# Needs the Windows Sandbox feature. Default installer: the newest installer\out\release\ApolloSetup-*.exe.
# Default work folder: %USERPROFILE%\Sandbox\apollo-test (in = installer + script, read-only; out = report and logs).
# The sandbox shuts itself down when the check ends. Exit 0 = every check passed.
param([string]$Installer = '', [string]$Work = (Join-Path $env:USERPROFILE 'Sandbox\apollo-test'), [int]$Minutes = 10)
$ErrorActionPreference = 'Stop'
if (-not $Installer) {
    $Installer = Get-ChildItem (Join-Path $PSScriptRoot '..\out\release') -Filter 'ApolloSetup-*.exe' |
                 Sort-Object LastWriteTime | Select-Object -Last 1 | ForEach-Object FullName
}
if (-not $Installer -or -not (Test-Path -LiteralPath $Installer)) { throw 'No ApolloSetup-*.exe: build one with installer\release.cmd, or pass -Installer.' }
if (Get-Process WindowsSandbox* -ErrorAction SilentlyContinue) { throw 'Windows Sandbox is already running: close it first (one sandbox at a time).' }

$in = Join-Path $Work 'in'; $out = Join-Path $Work 'out'
New-Item -ItemType Directory -Force $in, $out | Out-Null
Get-ChildItem $in -Filter 'ApolloSetup-*.exe' | Remove-Item -Force
Get-ChildItem $out -File | Remove-Item -Force
Copy-Item -LiteralPath $Installer $in
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'sandbox-check.ps1') $in -Force

$wsb = Join-Path $Work 'apollo-check.wsb'
@"
<Configuration>
  <vGPU>Disable</vGPU>
  <Networking>Disable</Networking>
  <ClipboardRedirection>Disable</ClipboardRedirection>
  <AudioInput>Disable</AudioInput>
  <VideoInput>Disable</VideoInput>
  <MemoryInMB>6144</MemoryInMB>
  <MappedFolders>
    <MappedFolder><HostFolder>$in</HostFolder><SandboxFolder>C:\Users\WDAGUtilityAccount\Desktop\apollo-in</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$out</HostFolder><SandboxFolder>C:\Users\WDAGUtilityAccount\Desktop\apollo-out</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
  </MappedFolders>
  <LogonCommand><Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\Users\WDAGUtilityAccount\Desktop\apollo-in\sandbox-check.ps1</Command></LogonCommand>
</Configuration>
"@ | Set-Content -LiteralPath $wsb -Encoding ascii

Write-Host "Sandbox check of $(Split-Path -Leaf $Installer) (up to $Minutes min)..."
Start-Process -FilePath $wsb
$done = Join-Path $out 'done.txt'
$deadline = (Get-Date).AddMinutes($Minutes)
while (-not (Test-Path $done) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 10 }
Start-Sleep -Seconds 2
$report = Join-Path $out 'report.txt'
if (-not (Test-Path $done) -or -not (Test-Path $report)) { Write-Host "No report after $Minutes min (logs, if any: $out)."; exit 2 }
$text = Get-Content -LiteralPath $report
$text | Write-Host
if (($text | Select-String -SimpleMatch 'sandbox check: 0 failure(s)')) { exit 0 } else { exit 1 }
