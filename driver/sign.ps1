# sign.ps1 — USB22-485 WinUSB surucusunu self-signed sertifika ile imzalar.
#
# Gereksinim : PowerShell 5.1+, Yonetici yetkisi
# Ekstra arac: YOK — tum islemler Windows yerlesik cmdlet'leri ile yapilir
# Secure Boot: ACIK olsa bile calisir
#
# Calistirma:
#   Yonetici PowerShell -> cd C:\ArcnetDriver\driver -> .\sign.ps1

#Requires -RunAsAdministrator

$ErrorActionPreference = "Stop"

$DriverDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$InfFile    = Join-Path $DriverDir "usb22_winusb.inf"
$CatFile    = Join-Path $DriverDir "usb22_winusb.cat"
$CertSubj   = "CN=Wetrech Driver Signing"

Write-Host ""
Write-Host "======================================================="
Write-Host "  USB22-485 WinUSB - Surucu Imzalama"
Write-Host "======================================================="
Write-Host ""

# ----------------------------------------------------------------
# 1. Self-signed kod imzalama sertifikasi olustur
# ----------------------------------------------------------------
Write-Host "[1/5] Sertifika olusturuluyor: $CertSubj"

# Ayni isimde eski sertifika varsa kaldir
Get-ChildItem "Cert:\LocalMachine\My" |
    Where-Object { $_.Subject -eq $CertSubj } |
    Remove-Item -Force -ErrorAction SilentlyContinue

$cert = New-SelfSignedCertificate `
    -Type          CodeSigningCert `
    -Subject       $CertSubj `
    -CertStoreLocation "Cert:\LocalMachine\My" `
    -HashAlgorithm SHA256 `
    -KeyAlgorithm  RSA `
    -KeyLength     2048 `
    -NotAfter      (Get-Date).AddYears(10)

Write-Host "    Thumbprint: $($cert.Thumbprint)"
Write-Host "    Gecerlilik: $($cert.NotBefore.ToString('yyyy-MM-dd')) - $($cert.NotAfter.ToString('yyyy-MM-dd'))"

# ----------------------------------------------------------------
# 2. Trusted Root CA deposuna ekle
# ----------------------------------------------------------------
Write-Host "[2/5] Trusted Root CA deposuna ekleniyor..."
$rootStore = New-Object `
    System.Security.Cryptography.X509Certificates.X509Store("Root", "LocalMachine")
$rootStore.Open("ReadWrite")
$rootStore.Add($cert)
$rootStore.Close()
Write-Host "    OK"

# ----------------------------------------------------------------
# 3. Trusted Publisher deposuna ekle
# ----------------------------------------------------------------
Write-Host "[3/5] Trusted Publisher deposuna ekleniyor..."
$pubStore = New-Object `
    System.Security.Cryptography.X509Certificates.X509Store("TrustedPublisher", "LocalMachine")
$pubStore.Open("ReadWrite")
$pubStore.Add($cert)
$pubStore.Close()
Write-Host "    OK"

# ----------------------------------------------------------------
# 4. Katalog dosyasi olustur (sadece INF'i kapsar)
# ----------------------------------------------------------------
Write-Host "[4/5] Katalog olusturuluyor: usb22_winusb.cat"

if (Test-Path $CatFile) { Remove-Item $CatFile -Force }

# CatalogVersion 2 = SHA-256 destekli, Windows 8.1+ uyumlu
New-FileCatalog -Path $InfFile -CatalogFilePath $CatFile -CatalogVersion 2 | Out-Null

if (-not (Test-Path $CatFile)) {
    Write-Error "Katalog dosyasi olusturulamadi: $CatFile"
    exit 1
}
Write-Host "    OK"

# ----------------------------------------------------------------
# 5. Katalog dosyasini imzala
# ----------------------------------------------------------------
Write-Host "[5/5] Katalog imzalaniyor..."
$sig = Set-AuthenticodeSignature -FilePath $CatFile -Certificate $cert -HashAlgorithm SHA256

if ($sig.Status -eq "Valid") {
    Write-Host "    OK — imza gecerli"
} else {
    Write-Warning "    Imza durumu: $($sig.Status) — kurulum basarisiz olabilir"
}

# ----------------------------------------------------------------
# Sonuc
# ----------------------------------------------------------------
Write-Host ""
Write-Host "======================================================="
Write-Host "  Imzalama tamamlandi."
Write-Host "  Simdi install.bat'i Yonetici olarak calistirin."
Write-Host "======================================================="
Write-Host ""
