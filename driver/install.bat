@echo off
setlocal

echo =================================================
echo  USB22-485 ARCNET WinUSB - Surucu Kurulumu
echo  Wetrech - Takim ici kullanim
echo =================================================
echo.

:: ---- Yonetici yetkisi kontrolu ----------------------------------------
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [HATA] Yonetici yetkisi gerekli.
    echo.
    echo  Lutfen bu dosyaya sag tiklayin ve
    echo  "Yonetici olarak calistir" secenegini secin.
    echo.
    pause
    exit /b 1
)

echo [1/2] INF surucusu yukleniyor...
echo       Dosya : %~dp0usb22_winusb.inf
echo.

pnputil /add-driver "%~dp0usb22_winusb.inf" /install

if %errorlevel% equ 0 (
    echo.
    echo [OK] Surucu basariyla yuklendi.
    echo.
    echo  Sonraki adim:
    echo    - Cihaz zaten takili ise: cikarip tekrar takin.
    echo    - Aygit Yoneticisi'nde "USB22-485 ARCNET WinUSB" gorunmelidir.
    echo    - Cihaz once bootloader olarak gorunur, ~2 saniyede otomatik
    echo      PID 0x1002'ye gecer; bu normal davranistir.
) else (
    echo.
    echo [HATA] pnputil basarisiz oldu ^(cikis kodu: %errorlevel%^).
    echo.
    echo  Olasiliklar ve cozumler:
    echo.
    echo   1. Imza zorlamasi acik
    echo      Cozum: INSTALL.md "Imza Zorlamasini Gecici Kapatma" bolumune bakin.
    echo.
    echo   2. Yonetici yetkisi eksik
    echo      Cozum: Sag tik -> "Yonetici olarak calistir"
    echo.
    echo   3. INF veya klasor yolu bozuk
    echo      Cozum: Bu .bat dosyasinin usb22_winusb.inf ile ayni klasorde
    echo             oldugunu dogrulayin.
    echo.
    echo  Yukleme hatasi ile devam etmek isterseniz Zadig araci (https://zadig.akeo.ie)
    echo  ile manuel olarak WinUSB bind yapabilirsiniz.
)

echo.
pause
endlocal
