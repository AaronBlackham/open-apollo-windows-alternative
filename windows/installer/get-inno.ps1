# Fetches the pinned Inno Setup 6 compiler into installer\tools\inno-<version>\ (per-user, portable,
# silent: no admin, no registry, no Start-menu entries, no uninstaller). build.cmd calls this when the
# compiler is missing. Run it by hand with:
#   powershell -NoProfile -ExecutionPolicy Bypass -File get-inno.ps1
#
# Source: https://jrsoftware.org/isdl.php links each release to jrsoftware's own immutable GitHub
# release (github.com/jrsoftware/issrc). The SHA-256 below is the one in jrsoftware's signed .issig
# file (https://files.jrsoftware.org/is/6/innosetup-6.7.4.exe.issig); the Authenticode signer must
# be "Pyrsys B.V." (jrsoftware's publisher name since 6.4.2). Either check failing stops the build.
# To move to a newer 6.x: change the three values below after checking the new .issig and signature.
param([switch]$Force)
$ErrorActionPreference = 'Stop'

$Version = '6.7.4'
$Url     = 'https://github.com/jrsoftware/issrc/releases/download/is-6_7_4/innosetup-6.7.4.exe'
$Sha256  = 'a549929319f9120716677c685535ca4671e4b937b5fdc87e2585f32d8eb30e8c'
$Signer  = 'CN=Pyrsys B.V., O=Pyrsys B.V., S=Noord-Holland, C=NL'

$tools = Join-Path $PSScriptRoot 'tools'
$dest  = Join-Path $tools "inno-$Version"
$iscc  = Join-Path $dest 'ISCC.exe'
if ((Test-Path $iscc) -and -not $Force) { Write-Host "Inno Setup $Version already in $dest"; exit 0 }

$dl = Join-Path $tools 'download'
New-Item -ItemType Directory -Force -Path $dl | Out-Null
$exe = Join-Path $dl "innosetup-$Version.exe"

if (-not (Test-Path $exe)) {
    Write-Host "Downloading $Url"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $tmp = "$exe.part"
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $tmp
    Move-Item -Force $tmp $exe
}

$hash = (Get-FileHash -Algorithm SHA256 $exe).Hash.ToLowerInvariant()
if ($hash -ne $Sha256) {
    Remove-Item -Force $exe
    throw "SHA-256 mismatch for innosetup-$Version.exe: got $hash, pinned $Sha256 (file deleted)"
}
$sig = Get-AuthenticodeSignature $exe
if ($sig.Status -ne 'Valid') { throw "Authenticode signature is $($sig.Status): $($sig.StatusMessage)" }
if ($sig.SignerCertificate.Subject -ne $Signer) { throw "Unexpected signer: $($sig.SignerCertificate.Subject)" }
Write-Host "Verified: SHA-256 $hash, Authenticode Valid, signer $($sig.SignerCertificate.Subject)"

# /PORTABLE=1: Inno Setup's own script then skips the uninstaller, icons and file association.
# /CURRENTUSER: no elevation. Output goes only to $dest.
$log = Join-Path $dl "innosetup-$Version-install.log"
$isArgs = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/CURRENTUSER', '/PORTABLE=1',
            '/NOICONS', "/DIR=`"$dest`"", "/LOG=`"$log`"")
$p = Start-Process -FilePath $exe -ArgumentList $isArgs -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "Inno Setup's installer exited with $($p.ExitCode); see $log" }
if (-not (Test-Path $iscc)) { throw "ISCC.exe not found in $dest after the install; see $log" }
Write-Host "Inno Setup $Version installed (portable, per-user) in $dest"
exit 0
