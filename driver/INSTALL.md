# USB22-485 ARCNET WinUSB — Sürücü Kurulum Kılavuzu

**Cihaz:** Contemporary Controls USB22-485 ARCNET Adaptörü  
**Paket:** `usb22_winusb.inf` + `install.bat` + `sign.ps1`  
**Hazırlayan:** Wetrech — Ekip içi kullanım

---

> **UYARI — BitLocker kullanan makineler:** Shift+Restart → "Sorun giderme → Gelişmiş Seçenekler → Başlangıç Ayarları → Yeniden Başlat → **7**" yöntemini **KULLANMAYIN** — BitLocker kurtarma anahtarı ister ve makineye erişimi keser. Bu makinelerde doğrudan **[Yöntem A](#yöntem-a-signps1-ile-imzala-ve-kur-önerilen--secure-boot-uyumlu)** (sign.ps1) kullanın.

---

## Bu Paket Ne Yapar?

Windows'un kendi yerleşik sürücüsü olan `winusb.sys`'i USB22-485 cihazına bağlar.
Harici `.dll` veya kernel sürücüsü içermez; sadece bir `.inf` dosyası ve `sign.ps1` tarafından kurulum anında üretilen bir `.cat` dosyasından oluşur (`.cat` repoda hazır gelmez — her makinede ayrıca üretilmesi gerekir).

---

## Ön Koşul: Makineye Göre Kurulum Yöntemi

| Durum | Yöntem |
|---|---|
| Secure Boot **KAPALI**, BitLocker yok | [Yöntem C](#yöntem-c-bcdedit-test-signing-ile-imzasız) |
| Secure Boot **AÇIK** (çoğu Win11 laptopı) | [Yöntem A veya B — imzalı paket](#yöntem-a-sign.ps1-ile-imzala-ve-kur-önerilen) |

Secure Boot açık makinelerde `bcdedit /set testsigning on` çalışmaz
("The value is protected by Secure Boot policy" hatası). `sign.ps1` bu sorunu çözer —
Secure Boot açıkken bile çalışır, yeniden başlatma gerektirmez.

---

## Yöntem A: sign.ps1 ile İmzala ve Kur (Önerilen — Secure Boot uyumlu)

Tüm adımlar PowerShell ile yapılır, ekstra araç gerekmez.

### Adım 1 — sign.ps1'i çalıştır

Yönetici PowerShell aç:

```powershell
cd C:\ArcnetDriver\driver
.\sign.ps1
```

> **Not:** `"Bu sistemde betik çalıştırma devre dışı bırakıldı"` (*running scripts is disabled*) hatası alırsanız önce şunu çalıştırın:
> ```powershell
> Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
> ```
> Ardından `.\sign.ps1`'i tekrar çalıştırın.

Bu komut:
- Makineye özgü bir self-signed kod imzalama sertifikası oluşturur
- Sertifikayı **LocalMachine\Trusted Root CA** ve **LocalMachine\Trusted Publisher** depolarına ekler
- `usb22_winusb.cat` dosyasını oluşturur ve imzalar

### Adım 2 — install.bat'i çalıştır

```
driver\install.bat   →  sağ tık  →  Yönetici olarak çalıştır
```

veya Yönetici komut isteminde:

```cmd
pnputil /add-driver "C:\ArcnetDriver\driver\usb22_winusb.inf" /install
```

### Adım 3 — Cihazı tak

Cihazı USB portuna takın. Aygıt Yöneticisi'nde birkaç saniye içinde
**"USB22-485 ARCNET WinUSB (Backplane, PID 1002)"** görünmelidir.

---

## Yöntem B: Aygıt Yöneticisi ile Manuel Kurulum

sign.ps1'i çalıştırdıktan sonra (adım 1 yukarıda):

1. USB22-485'i USB portuna takın.
2. **Aygıt Yöneticisi**'ni açın (`devmgmt.msc`).
3. Cihaz **"Diğer aygıtlar"** altında sarı ünlem işaretiyle **"Bilinmeyen Aygıt /
   Unknown Device"** veya USB22 benzeri bir adla görünebilir. Cihaza **sağ tıklayın →
   "Sürücüyü güncelleştir"**.
4. **"Sürücü yazılımı için bilgisayarıma gözat"** seçeneğini seçin.
5. `driver/` klasörünü gösterin → **İleri**.

---

## Yöntem C: bcdedit Test Signing ile (Yalnızca Secure Boot KAPALI makineler)

```cmd
:: Yönetici komut istemi
bcdedit /set testsigning on
```

Yeniden başlat. Masaüstünde "Test Mode" yazısı belirir. Sonra:

```cmd
pnputil /add-driver "C:\ArcnetDriver\driver\usb22_winusb.inf" /install
```

Kurulum sonrası test signing'i kapatmak için:

```cmd
bcdedit /set testsigning off
```

---

## Cihaz Davranışı — Bootloader Notu

USB22-485 takıldığında Windows **iki farklı PID** görür:

| Aşama | PID | Süre |
|---|---|---|
| Bootloader | `0xB001` veya `0xB002` | ~0–2 saniye |
| Operasyonel (firmware yüklü) | `0x1002` veya `0x1003` | Kalıcı |

Bu geçiş otomatiktir; cihazın EEPROM'undaki firmware kendini yükler.
**Bu paket yalnızca `0x1002` ve `0x1003`'ü bağlar.**
Sürücü kurulumu **tek seferdir** — her açılışta tekrar gerekmez.

---

## Sürücüyü Kaldırma

```cmd
:: Yönetici komut isteminde — oem##.inf numarasını bulmak için:
pnputil /enum-drivers | findstr "usb22"

:: Ardından kaldır:
pnputil /delete-driver oem##.inf /uninstall
```

Sertifikayı kaldırmak için: `certmgr.msc` → Güvenilen Yayımcılar →
"Wetrech Driver Signing" → Sil. Ardından Güvenilen Kök Sertifika Yetkilileri'nden de sil.

---

## İleride: EV Sertifika ile Geniş Dağıtım

Self-signed sertifika yalnızca `sign.ps1`'in çalıştırıldığı makinelerde geçerlidir.
Ekip dışına dağıtım için yol haritası:

1. Ticari bir **EV kod imzalama sertifikası** satın al (DigiCert, Sectigo vb.).
2. WDK'dan `inf2cat /driver:driver\ /os:10_X64` ile katalog oluştur.
3. `signtool sign /sha1 <EV_THUMBPRINT> /tr http://timestamp.digicert.com /td sha256 usb22_winusb.cat`
4. İmzalı paketi `pnputil` ile dağıt — hedef makinede hiçbir hazırlık gerekmez.
