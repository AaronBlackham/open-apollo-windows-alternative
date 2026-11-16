# Checks the compiled Setup against the payload manifest (build.cmd step 7), without running Setup:
#  1. the staged files still hash to payload-manifest.sha256, and nothing else is staged;
#  2. Inno's own compile manifest (OutputManifestFile: every file compiled in, with its SHA-256)
#     lists exactly the staged files with the same hashes; the compile log agrees;
#  3. the Setup exe exists and carries no signature (nothing in a release is signed with anyone's key;
#     Setup signs the driver on the user's PC).
# Then prints the exe's size and SHA-256 and writes <exe>.sha256.
param(
    [Parameter(Mandatory = $true)][string]$Stage,
    [Parameter(Mandatory = $true)][string]$Files,
    [Parameter(Mandatory = $true)][string]$CompileLog,
    [Parameter(Mandatory = $true)][string]$Exe
)
$ErrorActionPreference = 'Stop'
$Stage = (Resolve-Path -LiteralPath $Stage).Path.TrimEnd('\')
$fail = @()

# 1. payload manifest vs staged files
$manifestPath = Join-Path $Stage 'payload-manifest.sha256'
$expected = @{}
foreach ($line in [IO.File]::ReadAllLines($manifestPath)) {
    if ($line -match '^([0-9a-f]{64})  (.+)$') { $expected[$Matches[2]] = $Matches[1] }
    elseif ($line.Trim()) { $fail += "bad manifest line: $line" }
}
$staged = @{}
Get-ChildItem -LiteralPath $Stage -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($Stage.Length + 1)
    $staged[$rel] = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
}
foreach ($rel in $expected.Keys) {
    if (-not $staged.ContainsKey($rel)) { $fail += "manifest lists $rel, not staged" }
    elseif ($staged[$rel] -ne $expected[$rel]) { $fail += "$rel changed after the manifest was written" }
}
foreach ($rel in $staged.Keys) {
    if ($rel -ne 'payload-manifest.sha256' -and -not $expected.ContainsKey($rel)) { $fail += "staged but not in the manifest: $rel" }
}

# 2. Inno's compile manifest (tab-separated, header row) and the compile log
$rows = Import-Csv -LiteralPath $Files -Delimiter "`t"
$compiled = @{}
foreach ($r in $rows) {
    $src = $r.SourceFilename
    if (-not $src.StartsWith($Stage + '\', [StringComparison]::OrdinalIgnoreCase)) { $fail += "compiled from outside the stage: $src"; continue }
    $rel = $src.Substring($Stage.Length + 1)
    if ($compiled.ContainsKey($rel)) { $fail += "compiled twice: $rel" }
    $compiled[$rel] = $r.SHA256Sum.ToLowerInvariant()
}
$want = @{} + $expected
$want['payload-manifest.sha256'] = $staged['payload-manifest.sha256']
foreach ($rel in $want.Keys) {
    if (-not $compiled.ContainsKey($rel)) { $fail += "not in Setup: $rel" }
    elseif ($compiled[$rel] -ne $want[$rel]) { $fail += "in Setup with a different SHA-256: $rel" }
}
foreach ($rel in $compiled.Keys) { if (-not $want.ContainsKey($rel)) { $fail += "in Setup but not in the payload: $rel" } }

$logged = @{}
foreach ($line in (Get-Content -LiteralPath $CompileLog)) {
    if ($line -match '^\s*Compressing: (.+?)\s*$' -and $Matches[1].StartsWith($Stage + '\', [StringComparison]::OrdinalIgnoreCase)) {
        $logged[$Matches[1].Substring($Stage.Length + 1)] = $true
    }
}
foreach ($rel in $want.Keys) { if (-not $logged.ContainsKey($rel)) { $fail += "compile log does not show $rel" } }
if ($logged.Count -ne $want.Count) { $fail += "compile log shows $($logged.Count) payload files, expected $($want.Count)" }

# 3. the exe
if (-not (Test-Path -LiteralPath $Exe)) { $fail += "no output: $Exe" }
if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FAIL: $_" }
    exit 1
}
$sig = Get-AuthenticodeSignature -LiteralPath $Exe
if ($sig.SignerCertificate) {
    Write-Host "FAIL: Setup carries a signature ($($sig.SignerCertificate.Subject), $($sig.Status)); a release is unsigned"
    exit 1
}
$signText = "unsigned ($($sig.Status)); the driver is signed on the user's PC by sign-local.ps1"
$item = Get-Item -LiteralPath $Exe
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Exe).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$Exe.sha256", "$hash  $($item.Name)`r`n", (New-Object Text.UTF8Encoding $false))

Write-Host "Verified: $($want.Count) files in Setup = payload manifest ($($expected.Count) files + the manifest), same SHA-256 each."
Write-Host "Output:   $($item.FullName)"
Write-Host ("Size:     {0:N0} bytes" -f $item.Length)
Write-Host "Signing:  $signText"
Write-Host "SHA-256:  $hash"
exit 0
