# Finds the driver's test certificate, private key included, in the current user's stores (the WDK
# keeps "WDKTestCert <user>" in My or PrivateCertStore). Exit code: 0 = in My, 2 = in
# PrivateCertStore, 1 = not found (build.cmd then leaves Setup unsigned).
param([Parameter(Mandatory = $true)][string]$Thumbprint)
$codes = [ordered]@{ 'My' = 0; 'PrivateCertStore' = 2 }
foreach ($name in $codes.Keys) {
    try {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($name, 'CurrentUser')
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]'ReadOnly, OpenExistingOnly')
        $found = $store.Certificates.Find('FindByThumbprint', $Thumbprint, $false)
        $ok = $found.Count -gt 0 -and $found[0].HasPrivateKey
        $store.Close()
        if ($ok) { exit $codes[$name] }
    } catch { }
}
exit 1
