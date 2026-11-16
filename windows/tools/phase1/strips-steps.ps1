# Input-strip test (docs\STRIPS.md 10.6): set the strip registry values, then power-cycle the x8p. ADMIN.
#   powershell -ExecutionPolicy Bypass -File C:\apollo-win\tools\phase1\strips-steps.ps1 -Steps 9 [-Mask 1] [-Enable 1]
# -Steps N: stages 1..9 (9 = the whole chain). -Enable 0 switches the strips off (after a failed stage).
# Takes effect at the next cold start of the x8p (power cycle): a warm start never builds strips.
#Requires -RunAsAdministrator
param([ValidateRange(1, 9)][int]$Steps = 9, [ValidateRange(1, 255)][int]$Mask = 1, [ValidateRange(0, 1)][int]$Enable = 1)
$log = Join-Path $PSScriptRoot 'strips-steps.log'
$key = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters'
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
Set-ItemProperty -Path $key -Name StripsEnable -Value $Enable -Type DWord
Set-ItemProperty -Path $key -Name StripMask -Value $Mask -Type DWord
# MixerInitBatch is left as it is (release5 default and step3: 1, x8p-safe).
Set-ItemProperty -Path $key -Name StripSteps -Value $Steps -Type DWord
$p = Get-ItemProperty $key
$line = "{0} StripsEnable={1} StripMask=0x{2:X2} StripSteps={3} MixerInitBatch={4} EnabledEndpoints={5}" -f (Get-Date -Format o), $p.StripsEnable, $p.StripMask, $p.StripSteps, $p.MixerInitBatch, $p.EnabledEndpoints
$line | Out-File -Encoding utf8 $log
Write-Host $line
Write-Host 'Set. Now save your work, then power-cycle the x8p (off 10 s, on) and tell Claude.'
Read-Host 'Press Enter to close'
