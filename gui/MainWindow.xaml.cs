using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Management;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;

namespace WetrechArcnetManager;

public partial class MainWindow : Window
{
    private readonly ObservableCollection<LogGirdi> _loglar = new();
    private bool _testCalisiyor;

    public MainWindow()
    {
        InitializeComponent();
        LogKutu.ItemsSource = _loglar;
        Loaded += (_, _) => CihazlariTara();
    }

    // ── Araç çubuğu ──────────────────────────────────────────────

    private void YenileBtn_Click(object sender, RoutedEventArgs e) => CihazlariTara();

    private async void KurBtn_Click(object sender, RoutedEventArgs e)
    {
        string? driverDir = DriverKlasorunuBul();

        LogPanel.Visibility = Visibility.Visible;
        _loglar.Clear();

        if (driverDir == null)
        {
            LogEkle("HATA: driver/ klasörü bulunamadı.", BrKirmizi);
            LogEkle($"  Exe dizini: {AppContext.BaseDirectory}", BrSari);
            return;
        }

        LogEkle($"driver/ bulundu: {driverDir}", BrGri);

        KurBtn.IsEnabled    = false;
        YenileBtn.IsEnabled = false;
        KurBtn.Content      = "⏳  Kuruluyor…";

        bool basarili;
        try
        {
            basarili = await SurucuKurAsync(driverDir);
        }
        finally
        {
            KurBtn.IsEnabled    = true;
            YenileBtn.IsEnabled = true;
            KurBtn.Content      = "⬇  WinUSB Sürücüsünü Kur";
        }

        if (basarili)
        {
            LogEkle("", BrGri);
            switch (KurulumSonrasiTara())
            {
                case KurulumSonucu.WinUsbHazir:
                    LogEkle("✔  Sürücü kuruldu ve cihaz hazır (WinUSB).", BrYesil);
                    break;
                case KurulumSonucu.CihazYok:
                    LogEkle("✔  Sürücü paketi sisteme eklendi. Cihazı taktığınızda otomatik tanınacaktır.", BrMavi);
                    break;
                case KurulumSonucu.CihazBootloader:
                    LogEkle("⚠  Sürücü eklendi. Cihaz hazırlanıyor, birkaç saniye sonra 'Yenile'ye basın.", BrSari);
                    break;
            }
            CihazlariTara();
        }
    }

    // ── Kurulum adımları ─────────────────────────────────────────

    private async Task<bool> SurucuKurAsync(string driverDir)
    {
        string signPs1 = Path.Combine(driverDir, "sign.ps1");
        LogEkle("── Adım 1/2: Sertifika imzalama", BrMavi);

        bool ok = await KomutCalistirAsync(
            "powershell.exe",
            $"-ExecutionPolicy Bypass -NonInteractive -File \"{signPs1}\"",
            driverDir);

        if (!ok) { LogEkle("✘  sign.ps1 başarısız — kurulum durdu.", BrKirmizi); return false; }
        LogEkle("✔  Adım 1 tamamlandı.", BrYesil);
        LogEkle("", BrGri);

        string infPath = Path.Combine(driverDir, "usb22_winusb.inf");
        LogEkle("── Adım 2/2: Sürücü yükleme (pnputil)", BrMavi);

        ok = await KomutCalistirAsync(
            "pnputil.exe",
            $"/add-driver \"{infPath}\" /install",
            driverDir);

        if (!ok) { LogEkle("✘  pnputil başarısız — sürücü yüklenemedi.", BrKirmizi); return false; }
        LogEkle("✔  Adım 2 tamamlandı.", BrYesil);
        return true;
    }

    // ── Test: seçili cihaz ───────────────────────────────────────

    private async void TestBtn_Click(object sender, RoutedEventArgs e)
    {
        LogPanel.Visibility = Visibility.Visible;
        _loglar.Clear();

        var secili = CihazListesi.SelectedItem as CihazSatiri;
        if (secili == null)
        {
            LogEkle("Lütfen listeden bir cihaz seçin.", BrSari);
            return;
        }

        string? testExe = TestExeBul();
        if (testExe == null)
        {
            LogEkle("HATA: test_arcnet.exe bulunamadı — önce 'build.bat' ile derleyin.", BrKirmizi);
            return;
        }

        SetTestUI(calisiyor: true);
        TestBtn.Content = "⏳  Test çalışıyor…";
        try
        {
            await TestEtVeGuncelle(secili, testExe, logBaslik: true);
        }
        finally
        {
            SetTestUI(calisiyor: false);
            TestBtn.Content = "▶  Seçiliyi Test Et";
        }
    }

    // ── Test: hepsini sırayla ────────────────────────────────────

    private async void HepsiniTestBtn_Click(object sender, RoutedEventArgs e)
    {
        LogPanel.Visibility = Visibility.Visible;
        _loglar.Clear();

        var cihazlar = (CihazListesi.ItemsSource as IEnumerable<CihazSatiri>)?.ToList();
        if (cihazlar == null || cihazlar.Count == 0)
        {
            LogEkle("Listede cihaz yok — önce 'Yenile'ye basın.", BrSari);
            return;
        }

        string? testExe = TestExeBul();
        if (testExe == null)
        {
            LogEkle("HATA: test_arcnet.exe bulunamadı — önce 'build.bat' ile derleyin.", BrKirmizi);
            return;
        }

        SetTestUI(calisiyor: true);
        HepsiniTestBtn.Content = "⏳  Test ediliyor…";
        int basarili = 0;
        try
        {
            for (int idx = 0; idx < cihazlar.Count; idx++)
            {
                var c = cihazlar[idx];
                StatusBar.Text = $"Cihaz {idx + 1}/{cihazlar.Count} test ediliyor…";
                LogEkle($"── Cihaz {idx + 1}/{cihazlar.Count}: {KisaPath(c.InstancePath)}", BrMavi);

                bool ok = await TestEtVeGuncelle(c, testExe, logBaslik: false);
                if (ok) basarili++;

                LogEkle("", BrGri);
            }

            LogEkle($"─── Tamamlandı: {basarili}/{cihazlar.Count} başarılı ───",
                basarili == cihazlar.Count ? BrYesil : BrSari);
        }
        finally
        {
            SetTestUI(calisiyor: false);
            HepsiniTestBtn.Content = "▶▶  Hepsini Test Et";
        }
    }

    // ── Tek cihaz test çekirdeği (her ikisi de buraya gelir) ─────

    private async Task<bool> TestEtVeGuncelle(CihazSatiri cihaz, string testExe, bool logBaslik)
    {
        if (logBaslik)
        {
            LogEkle($"exe  : {testExe}", BrGri);
            LogEkle($"args : --no-receive --device-path \"{cihaz.InstancePath}\"", BrGri);
            LogEkle("", BrGri);
        }

        cihaz.TestSonucu = TestSonucu.Calisiyor;

        var satirlar = new List<string>();
        bool exitOk = await KomutCalistirAsync(
            testExe,
            $"--no-receive --device-path \"{cihaz.InstancePath}\"",
            Path.GetDirectoryName(testExe)!,
            satirlar,
            timeoutMs: 8_000);

        bool basarili = TestSonucunuDegerlendir(satirlar, exitOk);
        cihaz.TestSonucu = basarili ? TestSonucu.Basarili : TestSonucu.Basarisiz;
        return basarili;
    }

    private bool TestSonucunuDegerlendir(IReadOnlyList<string> satirlar, bool exitOk)
    {
        bool initOk     = satirlar.Any(s => s.Contains("arc_init: ARC_OK"));
        bool transmitOk = satirlar.Any(s => s.Contains("arc_transmit: ARC_OK"));
        bool hataVar    = satirlar.Any(s =>
            s.Contains("ARC_ERR") || s.Contains("device not found") || s.Contains("hardware error"));

        LogEkle("", BrGri);
        LogEkle("── Sonuç ──────────────────────────────────────", BrGri);

        if (initOk && transmitOk)
        {
            LogEkle("✔  Cihaz çalışıyor (init + transmit OK)", BrYesil);
            return true;
        }
        else if (hataVar)
        {
            string ilkHata = satirlar.FirstOrDefault(s =>
                s.Contains("ARC_ERR") || s.Contains("device not found")) ?? "";
            LogEkle($"✘  Test başarısız: {ilkHata.Trim()}", BrKirmizi);
        }
        else
        {
            LogEkle($"✘  Test başarısız: {(exitOk ? "init/transmit onaylanamadı" : "süreç hata ile çıktı")}", BrKirmizi);
        }
        return false;
    }

    private void SetTestUI(bool calisiyor)
    {
        _testCalisiyor           = calisiyor;
        TestBtn.IsEnabled        = !calisiyor;
        HepsiniTestBtn.IsEnabled = !calisiyor;
        KurBtn.IsEnabled         = !calisiyor;
        YenileBtn.IsEnabled      = !calisiyor;
    }

    private static string KisaPath(string path)
    {
        // "USB\VID_0D0B&PID_1002\5&1234&0&1" → "5&1234&0&1"
        int i = path.LastIndexOf('\\');
        return i >= 0 ? path[(i + 1)..] : path;
    }

    private static string? TestExeBul()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 7; i++)
        {
            if (dir == null) break;
            string c = Path.Combine(dir.FullName, "test_arcnet.exe");
            if (File.Exists(c)) return c;
            c = Path.Combine(dir.FullName, "build", "test_arcnet.exe");
            if (File.Exists(c)) return c;
            dir = dir.Parent;
        }
        return null;
    }

    // ── Ortak process çalıştırıcı ────────────────────────────────

    private async Task<bool> KomutCalistirAsync(
        string exe, string args, string calismaDir,
        List<string>? ciktiTopla = null,
        int timeoutMs = -1)
    {
        var psi = new ProcessStartInfo
        {
            FileName               = exe,
            Arguments              = args,
            UseShellExecute        = false,
            RedirectStandardOutput = true,
            RedirectStandardError  = true,
            CreateNoWindow         = true,
            WorkingDirectory       = calismaDir,
        };

        Process? proc = null;
        try
        {
            proc = new Process { StartInfo = psi };

            proc.OutputDataReceived += (_, ev) =>
            {
                if (ev.Data is not null)
                {
                    ciktiTopla?.Add(ev.Data);
                    Dispatcher.InvokeAsync(() => LogEkle(ev.Data, BrBeyaz));
                }
            };
            proc.ErrorDataReceived += (_, ev) =>
            {
                if (ev.Data is not null)
                {
                    ciktiTopla?.Add(ev.Data);
                    Dispatcher.InvokeAsync(() => LogEkle(ev.Data, BrSari));
                }
            };

            if (!proc.Start())
            {
                LogEkle($"  Süreç başlatılamadı: {Path.GetFileName(exe)}", BrKirmizi);
                return false;
            }

            proc.BeginOutputReadLine();
            proc.BeginErrorReadLine();

            if (timeoutMs > 0)
            {
                using var cts = new System.Threading.CancellationTokenSource(timeoutMs);
                try
                {
                    await proc.WaitForExitAsync(cts.Token);
                }
                catch (OperationCanceledException)
                {
                    try { proc.Kill(entireProcessTree: true); } catch { }
                    LogEkle($"  ⏱ Güvenlik zaman aşımı ({timeoutMs / 1000} sn) — process sonlandırıldı.", BrKirmizi);
                    return false;
                }
            }
            else
            {
                await proc.WaitForExitAsync();
            }

            if (proc.ExitCode != 0)
                LogEkle($"  (çıkış kodu: {proc.ExitCode})", BrKirmizi);

            return proc.ExitCode == 0;
        }
        catch (Exception ex)
        {
            LogEkle($"  Süreç başlatılamadı ({Path.GetFileName(exe)}): {ex.Message}", BrKirmizi);
            return false;
        }
        finally
        {
            proc?.Dispose();
        }
    }

    // ── Log yardımcıları ─────────────────────────────────────────

    private void LogEkle(string metin, Brush renk)
    {
        _loglar.Add(new LogGirdi(metin, renk));
        Dispatcher.InvokeAsync(() => LogScroll.ScrollToBottom(), DispatcherPriority.Background);
    }

    private static readonly Brush BrBeyaz   = Dondur(0xCD, 0xD6, 0xF4);
    private static readonly Brush BrYesil   = Dondur(0xA6, 0xE3, 0xA1);
    private static readonly Brush BrKirmizi = Dondur(0xF3, 0x8B, 0xA8);
    private static readonly Brush BrSari    = Dondur(0xF9, 0xE2, 0xAF);
    private static readonly Brush BrMavi    = Dondur(0x89, 0xB4, 0xFA);
    private static readonly Brush BrGri     = Dondur(0x6C, 0x70, 0x86);

    private static SolidColorBrush Dondur(byte r, byte g, byte b)
    {
        var b2 = new SolidColorBrush(Color.FromRgb(r, g, b));
        b2.Freeze();
        return b2;
    }

    // ── Kurulum sonrası cihaz durumu ─────────────────────────────

    private enum KurulumSonucu { WinUsbHazir, CihazYok, CihazBootloader }

    private KurulumSonucu KurulumSonrasiTara()
    {
        try
        {
            using var searcher = new ManagementObjectSearcher(
                "SELECT * FROM Win32_PnPEntity WHERE DeviceID LIKE '%VID_0D0B%'");

            bool herhangiCihaz = false;
            foreach (ManagementObject obj in searcher.Get())
            {
                herhangiCihaz = true;
                string deviceId = obj["DeviceID"]?.ToString() ?? "";
                string service  = obj["Service"]?.ToString()  ?? "";
                string pid      = CikartPid(deviceId);

                if (pid is "0x1002" or "0x1003" &&
                    service.Equals("WinUSB", StringComparison.OrdinalIgnoreCase))
                    return KurulumSonucu.WinUsbHazir;
            }
            return herhangiCihaz ? KurulumSonucu.CihazBootloader : KurulumSonucu.CihazYok;
        }
        catch { return KurulumSonucu.CihazYok; }
    }

    // ── Driver klasörü bulma ──────────────────────────────────────

    private static string? DriverKlasorunuBul()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 7; i++)
        {
            if (dir == null) break;
            string candidate = Path.Combine(dir.FullName, "driver");
            if (Directory.Exists(candidate)
                && File.Exists(Path.Combine(candidate, "usb22_winusb.inf"))
                && File.Exists(Path.Combine(candidate, "sign.ps1")))
                return candidate;
            dir = dir.Parent;
        }
        return null;
    }

    // ── Cihaz tarama ─────────────────────────────────────────────

    private void CihazlariTara()
    {
        if (_testCalisiyor) return;

        StatusBar.Text      = "Taranıyor…";
        YenileBtn.IsEnabled = false;

        var liste = new List<CihazSatiri>();
        try
        {
            using var searcher = new ManagementObjectSearcher(
                "SELECT * FROM Win32_PnPEntity WHERE DeviceID LIKE '%VID_0D0B%'");

            foreach (ManagementObject obj in searcher.Get())
            {
                string deviceId = obj["DeviceID"]?.ToString() ?? "";
                string name     = obj["Name"]?.ToString()     ?? "";
                string service  = obj["Service"]?.ToString()  ?? "";
                liste.Add(new CihazSatiri(CikartPid(deviceId), service, name, deviceId));
            }
        }
        catch (Exception ex)
        {
            StatusBar.Text      = $"WMI hatası: {ex.Message}";
            YenileBtn.IsEnabled = true;
            return;
        }

        if (liste.Count == 0)
        {
            CihazListesi.Visibility = Visibility.Collapsed;
            BosMesaj.Visibility     = Visibility.Visible;
            StatusBar.Text          = "Bağlı USB22 cihazı bulunamadı.";
        }
        else
        {
            CihazListesi.Visibility  = Visibility.Visible;
            BosMesaj.Visibility      = Visibility.Collapsed;
            CihazListesi.ItemsSource = liste;
            StatusBar.Text           = $"{liste.Count} cihaz bulundu — son tarama: {DateTime.Now:HH:mm:ss}";
        }

        YenileBtn.IsEnabled = true;
    }

    private static string CikartPid(string deviceId)
    {
        var pidIdx = deviceId.IndexOf("PID_", StringComparison.OrdinalIgnoreCase);
        if (pidIdx < 0) return "?";
        var raw = deviceId.Substring(pidIdx + 4, 4);
        return $"0x{raw.ToUpperInvariant()}";
    }
}

// ── Veri modelleri ────────────────────────────────────────────────

public enum TestSonucu { Bilinmiyor, Calisiyor, Basarili, Basarisiz }

public record LogGirdi(string Metin, Brush Renk);

public class CihazSatiri : INotifyPropertyChanged
{
    // Sabit sürücü bilgileri
    public string PID           { get; }
    public string SurucuServisi { get; }
    public string CihazTipi     { get; }
    public string Aciklama      { get; }
    public string InstancePath  { get; }
    public string Durum         { get; }
    public Brush  RozetArka     { get; }
    public Brush  RozetYazi     { get; }

    // Mutable: test sonucu (INotifyPropertyChanged ile DataGrid'i günceller)
    private TestSonucu _testSonucu = TestSonucu.Bilinmiyor;
    public TestSonucu TestSonucu
    {
        get => _testSonucu;
        set
        {
            _testSonucu = value;
            OnPropertyChanged(nameof(TestGorunum));
            OnPropertyChanged(nameof(TestMetin));
            OnPropertyChanged(nameof(TestArka));
            OnPropertyChanged(nameof(TestYazi));
        }
    }

    public Visibility TestGorunum =>
        _testSonucu != TestSonucu.Bilinmiyor ? Visibility.Visible : Visibility.Collapsed;

    public string TestMetin => _testSonucu switch
    {
        TestSonucu.Calisiyor => "⏳ Test…",
        TestSonucu.Basarili  => "✔ Çalışıyor",
        TestSonucu.Basarisiz => "✗ Hata",
        _                    => "",
    };

    public Brush TestArka => _testSonucu switch
    {
        TestSonucu.Calisiyor => TArkaC,
        TestSonucu.Basarili  => TArkaB,
        TestSonucu.Basarisiz => TArkaH,
        _                    => TArkaX,
    };

    public Brush TestYazi => _testSonucu switch
    {
        TestSonucu.Calisiyor => TYaziC,
        TestSonucu.Basarili  => TYaziB,
        TestSonucu.Basarisiz => TYaziH,
        _                    => TYaziX,
    };

    // Statik frozen brush'lar — her property erişiminde yeni nesne yaratmaz
    private static readonly Brush TArkaX = Fırça(0x1E, 0x1E, 0x2E);
    private static readonly Brush TArkaC = Fırça(0x4A, 0x3B, 0x1A);
    private static readonly Brush TArkaB = Fırça(0x26, 0x4A, 0x2E);
    private static readonly Brush TArkaH = Fırça(0x4A, 0x1F, 0x1F);
    private static readonly Brush TYaziX = Fırça(0x6C, 0x70, 0x86);
    private static readonly Brush TYaziC = Fırça(0xF9, 0xE2, 0xAF);
    private static readonly Brush TYaziB = Fırça(0xA6, 0xE3, 0xA1);
    private static readonly Brush TYaziH = Fırça(0xF3, 0x8B, 0xA8);

    public event PropertyChangedEventHandler? PropertyChanged;
    private void OnPropertyChanged([CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    public CihazSatiri(string pid, string service, string name, string instancePath)
    {
        PID           = pid;
        SurucuServisi = string.IsNullOrEmpty(service) ? "(yok)" : service;
        Aciklama      = name;
        InstancePath  = instancePath;

        CihazTipi = pid switch
        {
            "0x1002" => "Operasyonel (DC-485 Backplane)",
            "0x1003" => "Operasyonel (DC-485 Normal)",
            "0xB001" or "0xB002" => "Bootloader",
            _        => "Bilinmiyor",
        };

        bool winUsb      = service.Equals("WinUSB", StringComparison.OrdinalIgnoreCase);
        bool operasyonel = pid is "0x1002" or "0x1003";
        bool bootloader  = pid is "0xB001" or "0xB002";

        if (operasyonel && winUsb)
        {
            Durum     = "WinUSB Hazır";
            RozetArka = Fırça(0x26, 0x4A, 0x2E);
            RozetYazi = Fırça(0xA6, 0xE3, 0xA1);
        }
        else if (operasyonel && !string.IsNullOrEmpty(service))
        {
            Durum     = "Eski Sürücüde";
            RozetArka = Fırça(0x4A, 0x3B, 0x1A);
            RozetYazi = Fırça(0xF9, 0xE2, 0xAF);
        }
        else if (bootloader)
        {
            Durum     = "Bootloader";
            RozetArka = Fırça(0x35, 0x35, 0x4A);
            RozetYazi = Fırça(0xBA, 0xC2, 0xDE);
        }
        else
        {
            Durum     = "Sürücüsüz";
            RozetArka = Fırça(0x4A, 0x1F, 0x1F);
            RozetYazi = Fırça(0xF3, 0x8B, 0xA8);
        }
    }

    private static SolidColorBrush Fırça(byte r, byte g, byte b)
    {
        var b2 = new SolidColorBrush(Color.FromRgb(r, g, b));
        b2.Freeze();
        return b2;
    }
}
