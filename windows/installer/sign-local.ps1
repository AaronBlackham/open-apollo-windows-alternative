# sign-local.ps1 - sign the Apollo driver package on THIS PC with a certificate made for this PC alone.
#
# The release ships apollo.sys and apollo.cat unsigned (nothing in it is signed with anyone else's key). Setup runs this,
# elevated, from the install folder (admin-only), before it stages the driver:
#   1. makes a new code-signing certificate, "Apollo driver (signed on <PC name>)", key non-exportable, in LocalMachine\My,
#      and writes its thumbprint to <state>\local-cert.txt at once (Setup's uninstaller removes exactly these
#      certificates, so even a run that fails half-way leaves nothing it does not know about);
#   2. signs apollo.sys and apollo.cat with it (SHA-256; no time-stamp server is contacted);
#   3. trusts its PUBLIC certificate for this PC only: LocalMachine Trusted Root and Trusted Publishers (Windows installs a
#      test-signed driver package from a publisher in those stores without asking, once test mode is on);
#   4. deletes the private key, so nothing can sign anything with this certificate again; an update or repair makes a
#      new one. This step runs whatever happened before it (finally).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File sign-local.ps1 -Package <driver folder> -State <state folder>
# Exit 0 = signed and trusted; anything else = nothing usable was signed (the message says what), and Setup then removes
# what local-cert.txt lists (apollo-helper cert remove-local). PowerShell 5.1, ASCII.
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [Parameter(Mandatory = $true)][string]$State
)
$ErrorActionPreference = 'Stop'
function Fail([string]$why, [int]$rc) { Write-Output "sign-local: $why"; exit $rc }

# This runs elevated: load PowerShell's own certificate modules from System32 only, never a module of the same name
# from a folder the user can write (Documents\WindowsPowerShell\Modules comes first on the default module path).
$env:PSModulePath = Join-Path $PSHOME 'Modules'
try {
    Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1')
    Import-Module (Join-Path $PSHOME 'Modules\PKI\pki.psd1')
} catch { Fail "could not load the certificate modules from $PSHOME\Modules: $($_.Exception.Message)" 6 }

$sys = Join-Path $Package 'apollo.sys'
$cat = Join-Path $Package 'apollo.cat'
foreach ($f in @($sys, $cat)) { if (-not (Test-Path -LiteralPath $f)) { Fail "missing $f" 2 } }
if (-not (Test-Path -LiteralPath $State)) { New-Item -ItemType Directory -Force -Path $State | Out-Null }

$name = "Apollo driver (signed on $env:COMPUTERNAME)"
try {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=$name" -FriendlyName $name `
        -CertStoreLocation 'Cert:\LocalMachine\My' -KeyExportPolicy NonExportable -KeyLength 3072 -HashAlgorithm SHA256 `
        -NotAfter (Get-Date).AddYears(20)
} catch { Fail "could not make the certificate: $($_.Exception.Message)" 3 }
$thumb = $cert.Thumbprint
try { Add-Content -LiteralPath (Join-Path $State 'local-cert.txt') -Value $thumb -Encoding ascii }
catch { Write-Output "sign-local: could not record $thumb in local-cert.txt: $($_.Exception.Message)" }

$ok = $true
try {
    foreach ($f in @($sys, $cat)) {
        try {
            $r = Set-AuthenticodeSignature -LiteralPath $f -Certificate $cert -HashAlgorithm SHA256
        } catch {
            $r = $null
            Write-Output "sign-local: signing $f failed: $($_.Exception.Message)"
        }
        if (-not $r -or -not $r.SignerCertificate -or $r.SignerCertificate.Thumbprint -ne $thumb) {
            if ($r) { Write-Output "sign-local: $f not signed ($($r.Status) $($r.StatusMessage))" }
            $ok = $false
        }
    }

    # the public certificate only, into this PC's trust
    if ($ok) {
        $cer = Join-Path $State 'local-cert.cer'
        [IO.File]::WriteAllBytes($cer, $cert.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
        foreach ($store in @('Root', 'TrustedPublisher')) {
            $s = New-Object Security.Cryptography.X509Certificates.X509Store($store, 'LocalMachine')
            $s.Open('ReadWrite')
            try { $s.Add((New-Object Security.Cryptography.X509Certificates.X509Certificate2(, [IO.File]::ReadAllBytes($cer)))) }
            finally { $s.Close() }
        }
    }
} catch {
    Write-Output "sign-local: trusting the certificate failed: $($_.Exception.Message)"
    $ok = $false
} finally {
    # the private key goes: delete the certificate (and its key) from LocalMachine\My
    try { Remove-Item -LiteralPath "Cert:\LocalMachine\My\$thumb" -DeleteKey -ErrorAction Stop }
    catch { Write-Output "sign-local: could not delete the private key: $($_.Exception.Message)"; $ok = $false }
}

if (-not $ok) { Fail "signing did not complete (certificate $thumb)" 4 }
foreach ($f in @($sys, $cat)) {
    $v = Get-AuthenticodeSignature -LiteralPath $f
    if ($v.Status -ne 'Valid') { Fail "$f signature is $($v.Status) after trusting the certificate" 5 }
}
Write-Output "sign-local: signed apollo.sys and apollo.cat with $name ($thumb); private key deleted"
exit 0
