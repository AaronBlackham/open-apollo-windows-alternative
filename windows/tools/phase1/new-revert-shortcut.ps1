# Make the desktop shortcut "Twin X back to UA": the panic button for the speakers (docs/spec-twinx.md 6.10).
#   powershell -ExecutionPolicy Bypass -File C:\Users\aaron\Desktop\Avium\apollo-win\tools\phase1\new-revert-shortcut.ps1 [-Unit twinx|x8p]
# Run it once as yourself (no admin needed). Double-clicking the shortcut asks Windows for administrator
# rights (it is marked "Run as administrator"), then runs
#   revert-unit.ps1 -Unit twinx -Yes
# from this folder in a window that stays open, so you can read what it did. Only the Twin X moves back to
# Universal Audio's driver; the x8p is left as it is. Afterwards: power-cycle the Twin X.
# -Unit x8p makes "x8p back to UA" instead. The shortcut runs the script where it is now: move the folder
# and run this again.
# PowerShell 5.1, ASCII only.
param([ValidateSet('twinx', 'x8p')][string]$Unit = 'twinx')
$ErrorActionPreference = 'Stop'

$script = Join-Path $PSScriptRoot 'revert-unit.ps1'
if (-not (Test-Path -LiteralPath $script)) { Write-Host "revert-unit.ps1 is not next to this script ($PSScriptRoot)."; exit 1 }
$title = @{ twinx = 'Twin X back to UA'; x8p = 'x8p back to UA' }[$Unit]
$what = @{ twinx = 'Apollo Twin X'; x8p = 'Apollo x8p' }[$Unit]
$other = @{ twinx = 'the x8p'; x8p = 'the Twin X' }[$Unit]

$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$desktop = [Environment]::GetFolderPath('Desktop')
$lnk = Join-Path $desktop "$title.lnk"

$shell = New-Object -ComObject WScript.Shell
$s = $shell.CreateShortcut($lnk)
$s.TargetPath = $powershell
$s.Arguments = "-NoProfile -NoExit -ExecutionPolicy Bypass -File `"$script`" -Unit $Unit -Yes"
$s.WorkingDirectory = $PSScriptRoot
$s.Description = "Put the $what back on Universal Audio's driver ($other stays as it is)"
$s.IconLocation = (Join-Path $env:SystemRoot 'System32\SndVol.exe') + ',0'
$s.Save()
[System.Runtime.InteropServices.Marshal]::ReleaseComObject($shell) | Out-Null

# "Run as administrator": shell link header LinkFlags bit 0x2000 (SLDF_RUNAS_USER) = byte 0x15, bit 0x20.
$bytes = [System.IO.File]::ReadAllBytes($lnk)
if ($bytes.Length -lt 0x4C -or $bytes[0] -ne 0x4C) { Write-Host "$lnk is not a shell link; not changed further."; exit 1 }
$bytes[0x15] = $bytes[0x15] -bor 0x20
[System.IO.File]::WriteAllBytes($lnk, $bytes)

Write-Host "Made: $lnk"
Write-Host "  runs: $powershell $($s.Arguments)"
Write-Host '  as administrator (Windows asks first).'
