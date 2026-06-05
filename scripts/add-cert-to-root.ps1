$cert = Get-ChildItem -Path Cert:\CurrentUser\My -CodeSigningCert | Where-Object { $_.Subject -eq 'CN=PspTestSigner' }
if (-not $cert) {
    Write-Host "ERROR: PspTestSigner not found in My store"
    exit 1
}

$store = New-Object System.Security.Cryptography.X509Certificates.X509Store('Root', 'CurrentUser')
$store.Open('ReadWrite')
$store.Add($cert)
$store.Close()

Write-Host "OK: PspTestSigner added to Trusted Root store"
