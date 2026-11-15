# Writes tools\bin\tools-manifest.txt: SHA-256 of the binaries the elevated scripts may run
# (tools\phase1\safe-tools.ps1 checks its admin-only copies against it). Called by tools\build-all.cmd.
# PowerShell 5.1, ASCII only.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# A work copy under builds\ has no tools\bin (no devcon, nothing the elevated scripts run): no manifest there.
if (-not (Test-Path -LiteralPath (Join-Path $root 'tools\bin'))) {
    Write-Host "No tools\bin under $root (a work copy): no manifest written."
    exit 0
}
$files = @(
    'tools\bin\devcon.exe',
    'tools\apollo-diag\apollo-diag.exe',
    'tools\plugin-host\apollo-plug.exe',
    'tools\loopback-test\loopback-test.exe',
    'tools\wasapi-loop\wasapi-loop.exe',
    'tools\agg-test\agg-test.exe',          # [G7] step-g7.ps1: endpoint set state, config save/restore
    'asio\bin\avium-asio.dll',
    'asio\bin\asio-test.exe',
    'app\bin\apollo-control.exe'
)
$lines = @("# SHA-256 of the built tools, written by tools\write-manifest.ps1 on $(Get-Date -Format o)")
foreach ($rel in $files) {
    $p = Join-Path $root $rel
    if (Test-Path -LiteralPath $p) {
        $lines += ('{0}  {1}' -f (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash, $rel)
    }
}
$out = Join-Path $root 'tools\bin\tools-manifest.txt'
[System.IO.File]::WriteAllLines($out, [string[]]$lines, (New-Object System.Text.ASCIIEncoding))
Write-Host "Wrote $out ($($lines.Count - 1) files)"
