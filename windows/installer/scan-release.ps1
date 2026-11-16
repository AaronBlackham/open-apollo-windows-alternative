# scan-release.ps1 - nothing local in a release: scans every byte of the given files (folders: every file below them)
# for text that identifies the build PC, as ASCII and as UTF-16LE, case-insensitive. Built binaries embed their PDB
# path, a compiler may embed a source path, a document may hold a log line: none of that may name a user folder.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scan-release.ps1 -Paths <file or folder>[,...]
# Exit 0 = clean, 1 = something found (each hit is listed), 2 = bad arguments.
#
# What counts as a leak (the personal values are read on this PC at run time, never written in this file):
#   C:\Users\                          any user folder (the build tree must live outside it: release.cmd)
#   %USERPROFILE%                      this user's profile path (also if it is not under C:\Users)
#   \<user name>\ and /<user name>/    the user name as a path part
#   <computer name>                    this PC's name (5 characters or more)
#   WDKTestCert                        the WDK's per-developer test certificate (a signature or certificate)
#   Parameters\Units\<key>             the unit keys of Apollo units this PC has run (HW_UID or serial based)
#   PCI\VEN_1A00&DEV_0002&...\<id>     the PnP instance IDs of Universal Audio devices this PC has seen
# PowerShell 5.1, ASCII.
param([Parameter(Mandatory = $true)][string[]]$Paths)
$ErrorActionPreference = 'Stop'

$needles = New-Object System.Collections.Generic.List[object]
function Add-Needle([string]$text, [string]$why, [int]$min = 4) {
    if (-not $text -or $text.Length -lt $min) { return }
    $low = $text.ToLowerInvariant()
    foreach ($n in $needles) { if ($n.Text -eq $low) { return } }
    # Wide: the UTF-16LE form, the ASCII one with a 0 after each char (made once here, not once per file)
    $needles.Add([pscustomobject]@{ Text = $low; Wide = -join ($low.ToCharArray() | ForEach-Object { "$_$([char]0)" }); Why = $why })
}

Add-Needle 'C:\Users\' 'a user folder path'
Add-Needle 'WDKTestCert' 'the WDK developer test certificate'
if ($env:USERPROFILE) { Add-Needle $env:USERPROFILE 'this user''s profile path' }
if ($env:USERNAME -and $env:USERNAME.Length -ge 3) {
    Add-Needle ('\' + $env:USERNAME + '\') 'this user name as a path part'
    Add-Needle ('/' + $env:USERNAME + '/') 'this user name as a path part'
}
if ($env:COMPUTERNAME) { Add-Needle $env:COMPUTERNAME 'this PC''s name' 5 }

# Unit keys this PC's driver has used (read-only registry reads).
$units = 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters\Units'
foreach ($k in @(Get-ChildItem -LiteralPath $units -ErrorAction SilentlyContinue)) {
    $name = $k.PSChildName
    Add-Needle $name 'an Apollo unit key on this PC' 6
    $trim = $name.TrimStart('0')
    if ($trim.Length -ge 6) { Add-Needle $trim 'an Apollo unit key on this PC' 6 }
}
# Serial numbers of the units connected now (the x8p's unit key is its HW_UID, so its serial is nowhere in the
# registry): apollo-diag units, a read-only query; nothing when no driver or tool is there.
foreach ($tool in @((Join-Path $PSScriptRoot '..\tools\apollo-diag\apollo-diag.exe'),
                    (Join-Path $env:ProgramFiles 'Avium\ApolloTools\apollo-diag.exe'))) {
    if (-not (Test-Path -LiteralPath $tool)) { continue }
    foreach ($m in [regex]::Matches((& $tool units 2>$null | Out-String), '(?m)^\d+\s+Apollo.*?\s{2,}([0-9A-Za-z]{8,16})\s{2,}')) {
        Add-Needle $m.Groups[1].Value 'an Apollo unit serial number on this PC' 8
    }
    break
}
# Instance IDs of Universal Audio PCI devices this PC has seen: the part after the hardware ID, and its unique hex part.
$enum = 'HKLM:\SYSTEM\CurrentControlSet\Enum\PCI'
foreach ($hw in @(Get-ChildItem -LiteralPath $enum -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -like 'VEN_1A00&*' })) {
    foreach ($inst in @(Get-ChildItem -LiteralPath $hw.PSPath -ErrorAction SilentlyContinue)) {
        $id = $inst.PSChildName
        Add-Needle $id 'a Universal Audio device instance ID on this PC' 6
        foreach ($part in ($id -split '&')) {
            if ($part -match '^[0-9A-Fa-f]{6,}$' -and $part -notmatch '^0+$') { Add-Needle $part 'a Universal Audio device instance ID on this PC' 6 }
        }
    }
}

# Files to scan.
$files = New-Object System.Collections.Generic.List[string]
foreach ($p in $Paths) {
    foreach ($one in ($p -split ',')) {
        if (-not $one) { continue }
        if (Test-Path -LiteralPath $one -PathType Container) {
            Get-ChildItem -LiteralPath $one -Recurse -File | ForEach-Object { $files.Add($_.FullName) }
        } elseif (Test-Path -LiteralPath $one -PathType Leaf) {
            $files.Add((Resolve-Path -LiteralPath $one).Path)
        } else {
            Write-Host "scan-release: no such file or folder: $one"
            exit 2
        }
    }
}

# Each file as Latin-1 (one char per byte), lower case, searched for each needle's ASCII and UTF-16LE forms.
$latin1 = [Text.Encoding]::GetEncoding(28591)
$hits = @()
$bytesTotal = 0
foreach ($f in $files) {
    $bytes = [IO.File]::ReadAllBytes($f)
    $bytesTotal += $bytes.Length
    $hay = $latin1.GetString($bytes).ToLowerInvariant()
    foreach ($n in $needles) {
        foreach ($form in @(@{ T = $n.Text; K = 'ASCII' }, @{ T = $n.Wide; K = 'UTF-16' })) {
            $at = $hay.IndexOf($form.T, [StringComparison]::Ordinal)
            if ($at -ge 0) {
                $start = [Math]::Max(0, $at - 24)
                $len = [Math]::Min($hay.Length - $start, $form.T.Length + 64)
                $ctx = ($latin1.GetString($bytes, $start, $len) -replace '[^\x20-\x7E]', '.')
                $hits += "$f : $($n.Why) ($($form.K), offset $at): ...$ctx..."
            }
        }
    }
}

Write-Host ("scan-release: {0} file(s), {1:N0} bytes, {2} pattern(s) (ASCII and UTF-16)" -f $files.Count, $bytesTotal, $needles.Count)
if ($hits.Count -gt 0) {
    Write-Host "scan-release: FOUND $($hits.Count) local identifier(s):"
    $hits | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host 'scan-release: clean (no user folder, user name, PC name, test certificate, unit key or device instance ID)'
exit 0
