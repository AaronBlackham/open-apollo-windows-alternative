# Runs INSIDE Windows Sandbox (LogonCommand). Installs ApolloSetup silently in /SANDBOXTEST=1 mode (per-PC signing,
# driver staged, no unit switched), checks the install, uninstalls, checks the removal. Report: Desktop\apollo-out.
$ErrorActionPreference = 'Continue'
$in  = 'C:\Users\WDAGUtilityAccount\Desktop\apollo-in'
$out = 'C:\Users\WDAGUtilityAccount\Desktop\apollo-out'
$rep = Join-Path $out 'report.txt'
$script:lines = New-Object System.Collections.Generic.List[string]; $script:lines.Add("sandbox check $(Get-Date -Format o)")
$script:fails = 0
function AddLine([string]$s) { $script:lines.Add($s) }
function Ok([bool]$c, [string]$s) { if ($c) { AddLine "PASS  $s" } else { $script:fails++; AddLine "FAIL  $s" } }
$app = Join-Path $env:ProgramFiles 'Avium\Apollo'
$setup = Get-ChildItem $in -Filter 'ApolloSetup-*.exe' | Select-Object -First 1
AddLine "installer: $($setup.Name)"

function Ours { (& pnputil.exe /enum-drivers | Out-String) -split "\r?\n\r?\n" | Where-Object { $_ -match 'Original Name:\s+apollo\.inf' -and $_ -match 'Provider Name:\s+Avium' } }

# ---- install
$p = Start-Process $setup.FullName -ArgumentList '/SANDBOXTEST=1', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$out\setup.log`"" -Wait -PassThru
AddLine "setup exit $($p.ExitCode)"
Ok ($p.ExitCode -eq 0) 'Setup finished with exit 0'

# ---- files against the payload manifest
$man = Get-ChildItem $app -Recurse -Filter 'payload-manifest.sha256' -EA SilentlyContinue | Select-Object -First 1
if ($man) {
    $n = 0; $bad = @()
    foreach ($line in Get-Content $man.FullName) {
        if ($line -match '^([0-9A-Fa-f]{64})\s+\*?(.+)$') {
            $n++; $rel = $Matches[2].Trim(); $f = Join-Path $man.DirectoryName $rel
            if (-not (Test-Path $f)) { $bad += "missing $rel" }
            elseif ($rel -notmatch 'apollo\.(sys|cat)$' -and (Get-FileHash $f -Algorithm SHA256).Hash -ne $Matches[1].ToUpper()) { $bad += "hash $rel" }
        }
    }
    Ok ($bad.Count -eq 0) "payload: $n files present and matching the manifest (apollo.sys/.cat re-signed) $($bad -join ', ')"
} else { Ok $false 'payload-manifest.sha256 installed' }

# ---- per-PC signing
$lc = Get-ChildItem $app -Recurse -Filter 'local-cert.txt' -EA SilentlyContinue | Select-Object -First 1
$thumbs = @(); if ($lc) { $thumbs = @([regex]::Matches((Get-Content $lc.FullName -Raw), '\b[0-9A-Fa-f]{40}\b') | ForEach-Object { $_.Value.ToUpper() } | Select-Object -Unique) }
Ok ($thumbs.Count -ge 1) "state\local-cert.txt names the certificate ($($thumbs.Count) thumbprint)"
foreach ($t in $thumbs) {
    foreach ($s in 'Root', 'TrustedPublisher') { Ok ([bool](Get-ChildItem "Cert:\LocalMachine\$s" | Where-Object Thumbprint -eq $t)) "certificate in LocalMachine\$s" }
    $mine = @(Get-ChildItem Cert:\LocalMachine\My, Cert:\CurrentUser\My -EA SilentlyContinue | Where-Object Thumbprint -eq $t)
    Ok ($mine.Count -eq 0) 'no copy with a private key in a Personal store (private key deleted)'
    $c = Get-ChildItem Cert:\LocalMachine\Root | Where-Object Thumbprint -eq $t | Select-Object -First 1
    if ($c) { Ok (-not $c.HasPrivateKey) "Root copy has no private key ($($c.Subject))" }
}
foreach ($f in 'apollo.sys', 'apollo.cat') {
    $x = Get-ChildItem $app -Recurse -Filter $f -EA SilentlyContinue | Select-Object -First 1
    if ($x) { $sig = Get-AuthenticodeSignature $x.FullName; Ok ($sig.Status -eq 'Valid' -and $thumbs -contains $sig.SignerCertificate.Thumbprint) "$f signed by the per-PC certificate ($($sig.Status))" } else { Ok $false "$f installed" }
}

# ---- driver package, registry, ASIO, shortcuts
Ok ([bool](Ours)) 'driver store holds an Avium apollo.inf'
Ok (Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\apollo\Parameters') 'service Parameters key written'
$asio = @(Get-ChildItem 'HKLM:\SOFTWARE\ASIO' -EA SilentlyContinue | Where-Object { $_.PSChildName -match 'Avium|Apollo' } | ForEach-Object PSChildName)
Ok ($asio.Count -ge 1) "ASIO drivers registered: $($asio -join ', ')"
$sm = @(Get-ChildItem "$env:ProgramData\Microsoft\Windows\Start Menu\Programs" -Recurse -Filter *.lnk -EA SilentlyContinue | Where-Object { $_.FullName -match 'Apollo|Avium' } | ForEach-Object BaseName)
Ok ($sm.Count -ge 3) "Start-menu shortcuts: $($sm -join ', ')"
$ac = Get-ChildItem $app -Recurse -Filter 'apollo-control.exe' -EA SilentlyContinue | Select-Object -First 1
Ok ([bool]$ac) 'Apollo Control installed'

# ---- uninstall
$un = Get-ChildItem $app -Filter 'unins*.exe' -EA SilentlyContinue | Select-Object -First 1
if ($un) {
    $p = Start-Process $un.FullName -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$out\uninstall.log`"" -Wait -PassThru
    Start-Sleep -Seconds 5
    AddLine "uninstall exit $($p.ExitCode)"
    Ok ($p.ExitCode -eq 0) 'uninstaller finished with exit 0'
    Ok (-not (Test-Path (Join-Path $app 'apollo-helper.exe'))) 'program files removed'
    foreach ($t in $thumbs) { foreach ($s in 'Root', 'TrustedPublisher') { Ok (-not (Get-ChildItem "Cert:\LocalMachine\$s" | Where-Object Thumbprint -eq $t)) "certificate removed from LocalMachine\$s" } }
    Ok (-not (Ours)) 'no Avium apollo.inf left in the driver store'
    Ok (@(Get-ChildItem 'HKLM:\SOFTWARE\ASIO' -EA SilentlyContinue | Where-Object { $_.PSChildName -match 'Avium|Apollo' }).Count -eq 0) 'ASIO registrations removed'
    Ok (@(Get-ChildItem "$env:ProgramData\Microsoft\Windows\Start Menu\Programs" -Recurse -Filter *.lnk -EA SilentlyContinue | Where-Object { $_.FullName -match 'Apollo|Avium' }).Count -eq 0) 'Start-menu shortcuts removed'
} else { Ok $false 'uninstaller present' }

AddLine ''
AddLine "sandbox check: $script:fails failure(s)"
$script:lines | Out-File -Encoding ascii $rep
$script:lines | Out-File -Encoding ascii 'C:\Users\WDAGUtilityAccount\Desktop\report-local.txt'
'DONE' | Out-File -Encoding ascii (Join-Path $out 'done.txt')
Start-Sleep -Seconds 2
shutdown.exe /s /t 0
