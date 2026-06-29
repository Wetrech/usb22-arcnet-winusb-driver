using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Management;
using System.Runtime.CompilerServices;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using System.Windows.Threading;

namespace WetrechArcnetManager;

public partial class MainWindow : Window
{
    private bool _testCalisiyor;
    private readonly List<CihazBaglanti> _acikCihazlar = new();

    public MainWindow()
    {
        InitializeComponent();
        Loaded  += (_, _) => CihazlariTara();
        Closing += (_, _) =>
        {
            foreach (var cb in _acikCihazlar) cb.Dispose();
            _acikCihazlar.Clear();
        };
    }

    // ── Araç çubuğu ──────────────────────────────────────────────

    private void YenileBtn_Click(object sender, RoutedEventArgs e) => CihazlariTara();

    // ── Cihaz sekme yönetimi (AŞAMA 1) ───────────────────────────

    private void TaraBtn_Click(object sender, RoutedEventArgs e)
    {
        // Remove all ARC device tabs (keep tab 0 = "WMI Cihazlar")
        while (AnaTablar.Items.Count > 1)
            AnaTablar.Items.RemoveAt(1);
        foreach (var cb in _acikCihazlar) cb.Dispose();
        _acikCihazlar.Clear();

        string[] paths;
        try { paths = ArcnetDevice.ListDevices(8); }
        catch (Exception ex)
        {
            AcLogPanel();
            LogEkle($"arc_list_devices hatası: {ex.Message}", BrKirmizi);
            return;
        }

        if (paths.Length == 0)
        {
            StatusBar.Text = "ARC cihazı bulunamadı — WinUSB sürücüsü kurulu mu?";
            return;
        }

        for (int i = 0; i < paths.Length; i++)
        {
            var cb = new CihazBaglanti(paths[i]) { NodeId = (byte)Math.Min(i + 1, 255) };
            _acikCihazlar.Add(cb);
            AnaTablar.Items.Add(CihazTabOlustur(cb));
        }

        AnaTablar.SelectedIndex = AnaTablar.Items.Count - 1;
        StatusBar.Text = $"{paths.Length} ARC cihazı bulundu — sekmeleri kullanarak bağlanın.";
    }

    private TabItem CihazTabOlustur(CihazBaglanti cb)
    {
        // ── Durum rozeti ─────────────────────────────────────────────
        var rozetMetin = new TextBlock
        {
            FontSize          = 10,
            FontWeight        = FontWeights.SemiBold,
            FontFamily        = new FontFamily("Segoe UI"),
            VerticalAlignment = VerticalAlignment.Center,
        };
        var rozet = new Border
        {
            CornerRadius = new CornerRadius(10),
            Padding      = new Thickness(10, 3, 10, 3),
            Margin       = new Thickness(0, 0, 14, 0),
            Child        = rozetMetin,
        };

        void GuncelleDurum()
        {
            // PropertyChanged always fires on the UI thread in this app — update directly.
            rozetMetin.Text       = cb.DurumMetin;
            rozet.Background      = cb.DurumArka;
            rozetMetin.Foreground = cb.DurumYazi;
        }

        // ── Node ID girişi ────────────────────────────────────────────
        var nodeLabel = new TextBlock
        {
            Text              = "Node ID:",
            FontFamily        = new FontFamily("Segoe UI"),
            FontSize          = 12,
            Foreground        = new SolidColorBrush(Color.FromRgb(0xCD, 0xD6, 0xF4)),
            VerticalAlignment = VerticalAlignment.Center,
            Margin            = new Thickness(0, 0, 6, 0),
        };
        var nodeBox = new TextBox
        {
            Text                     = cb.NodeId.ToString(),
            Width                    = 52,
            FontFamily               = new FontFamily("Segoe UI"),
            FontSize                 = 12,
            Foreground               = new SolidColorBrush(Color.FromRgb(0xCD, 0xD6, 0xF4)),
            Background               = new SolidColorBrush(Color.FromRgb(0x31, 0x32, 0x44)),
            BorderBrush              = new SolidColorBrush(Color.FromRgb(0x58, 0x5B, 0x70)),
            BorderThickness          = new Thickness(1),
            Padding                  = new Thickness(6, 3, 6, 3),
            VerticalContentAlignment = VerticalAlignment.Center,
            Margin                   = new Thickness(0, 0, 14, 0),
        };
        nodeBox.TextChanged += (_, _) =>
        {
            if (byte.TryParse(nodeBox.Text, out byte v)) cb.NodeId = v;
        };

        // ── Aç+Init / Kapat butonları ──────────────────────────────────
        var acBtn = new Button
        {
            Content  = "Aç+Init",
            Style    = (Style)FindResource("TestButton"),
            Margin   = new Thickness(0, 0, 8, 0),
            MinWidth = 90,
        };
        var kapatBtn = new Button
        {
            Content  = "Kapat",
            Style    = (Style)FindResource("RefreshButton"),
            MinWidth = 70,
        };
        acBtn.Click += async (_, _) => await AcVeInitIsle(cb, acBtn, nodeBox);
        // kapatBtn.Click is assigned after canliTimer/statusTimer/okumaDevam are declared (below)

        var kontrolSatiri = new WrapPanel { Orientation = Orientation.Horizontal };
        kontrolSatiri.Children.Add(rozet);
        kontrolSatiri.Children.Add(nodeLabel);
        kontrolSatiri.Children.Add(nodeBox);
        kontrolSatiri.Children.Add(acBtn);
        kontrolSatiri.Children.Add(kapatBtn);

        // ── Register paneli ───────────────────────────────────────────
        var (regRows, guncelleRegs) = RegPaneliOlustur();

        // ── Ağ durumu rozeti ─────────────────────────────────────────
        // Yorumlar gözleme dayalı (datasheet'ten doğrulanmadı):
        //   bit5 (0x20) set  → node ağa katılmış / token görüyor (gözlemledik)
        //   0x00             → ağ aktivitesi yok
        //   diğer non-zero  → geçiş / belirsiz durum
        var agMetin = new TextBlock
        {
            Text              = "— henüz okunmadı",
            FontFamily        = new FontFamily("Segoe UI"),
            FontSize          = 11,
            FontWeight        = FontWeights.SemiBold,
            VerticalAlignment = VerticalAlignment.Center,
            Foreground        = new SolidColorBrush(Color.FromRgb(0x6C, 0x70, 0x86)),
        };
        var agRozet = new Border
        {
            CornerRadius = new CornerRadius(10),
            Padding      = new Thickness(12, 4, 12, 4),
            Background   = new SolidColorBrush(Color.FromRgb(0x31, 0x32, 0x44)),
            Child        = agMetin,
            Margin       = new Thickness(0, 0, 10, 0),
        };
        var agNot = new TextBlock
        {
            Text              = "(reg0 yorumu — gözleme dayalı, kesin teşhis değil)",
            FontFamily        = new FontFamily("Segoe UI"),
            FontSize          = 10,
            Foreground        = new SolidColorBrush(Color.FromRgb(0x58, 0x5B, 0x70)),
            VerticalAlignment = VerticalAlignment.Center,
            FontStyle         = FontStyles.Italic,
        };
        var agPanel = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 8) };
        agPanel.Children.Add(agRozet);
        agPanel.Children.Add(agNot);

        void GuncelleAgDurumu(byte reg0)
        {
            string text; Color arka, yazi;
            if ((reg0 & 0x20) != 0)
            {
                text = "● Ağda aktif";
                arka = Color.FromRgb(0x26, 0x4A, 0x2E);
                yazi = Color.FromRgb(0xA6, 0xE3, 0xA1);
            }
            else if (reg0 == 0x00)
            {
                text = "● Ağ yok / token yok";
                arka = Color.FromRgb(0x4A, 0x1F, 0x1F);
                yazi = Color.FromRgb(0xF3, 0x8B, 0xA8);
            }
            else
            {
                text = $"● Geçiş / belirsiz  (0x{reg0:X2})";
                arka = Color.FromRgb(0x4A, 0x3B, 0x1A);
                yazi = Color.FromRgb(0xF9, 0xE2, 0xAF);
            }
            agMetin.Text       = text;
            agRozet.Background = new SolidColorBrush(arka);
            agMetin.Foreground = new SolidColorBrush(yazi);
        }

        // Unified update: register table + network badge + main status badge
        void GuncelleHepsi(byte[] vals)
        {
            guncelleRegs(vals);
            GuncelleAgDurumu(vals[0]);
            cb.RaporAgDurumu(vals[0]);
        }

        var okuBtn = new Button
        {
            Content  = "Oku",
            Style    = (Style)FindResource("RefreshButton"),
            MinWidth = 60,
            Margin   = new Thickness(0, 0, 8, 0),
        };
        var canliBtn = new Button
        {
            Content  = "▶  Canlı",
            Style    = (Style)FindResource("ScanButton"),
            MinWidth = 90,
        };

        okuBtn.Click += async (_, _) =>
        {
            okuBtn.IsEnabled = false;
            try
            {
                var vals = await Task.Run(() => cb.RegisterlariOkuSync());
                if (vals != null) GuncelleHepsi(vals);
            }
            finally { okuBtn.IsEnabled = true; }
        };

        var canliTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
        bool canliAcik  = false;
        bool okumaDevam = false;

        canliBtn.Click += (_, _) =>
        {
            canliAcik = !canliAcik;
            if (canliAcik) { canliTimer.Start(); canliBtn.Content = "■  Durdur"; }
            else           { canliTimer.Stop();  canliBtn.Content = "▶  Canlı";  }
        };

        canliTimer.Tick += async (_, _) =>
        {
            if (okumaDevam || !cb.AcikMi) return;
            okumaDevam = true;
            try
            {
                var vals = await Task.Run(() => cb.RegisterlariOkuSync());
                if (vals != null) GuncelleHepsi(vals);
            }
            finally { okumaDevam = false; }
        };

        // Status timer (1 s, always-on after init OK) — shares okumaDevam with canliTimer
        // so they never overlap on the USB bus.
        var statusTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        statusTimer.Tick += async (_, _) =>
        {
            if (okumaDevam || !cb.AcikMi) return;
            okumaDevam = true;
            try
            {
                var vals = await Task.Run(() => cb.RegisterlariOkuSync());
                if (vals != null) GuncelleHepsi(vals);
            }
            finally { okumaDevam = false; }
        };

        // Kapat: stop timers first, wait for any in-flight read, then Shutdown
        kapatBtn.Click += async (_, _) =>
        {
            canliTimer.Stop();
            statusTimer.Stop();
            canliAcik        = false;
            canliBtn.Content = "▶  Canlı";
            while (okumaDevam) await Task.Yield();
            await KapatIsleAsync(cb, acBtn, nodeBox);
        };

        // Header: title + buttons right-aligned
        var regBtnStrip = new StackPanel { Orientation = Orientation.Horizontal };
        regBtnStrip.Children.Add(okuBtn);
        regBtnStrip.Children.Add(canliBtn);

        var regTitle = new TextBlock
        {
            Text              = "Register Paneli  (COM20022)",
            FontFamily        = new FontFamily("Segoe UI"),
            FontSize          = 11,
            FontWeight        = FontWeights.SemiBold,
            Foreground        = new SolidColorBrush(Color.FromRgb(0x89, 0xB4, 0xFA)),
            VerticalAlignment = VerticalAlignment.Center,
        };
        var regHeader = new DockPanel { Margin = new Thickness(0, 0, 0, 8) };
        DockPanel.SetDock(regBtnStrip, Dock.Right);
        regHeader.Children.Add(regBtnStrip);
        regHeader.Children.Add(regTitle);

        var regInner = new StackPanel();
        regInner.Children.Add(regHeader);
        regInner.Children.Add(agPanel);
        regInner.Children.Add(regRows);

        var regBorder = new Border
        {
            Background      = new SolidColorBrush(Color.FromRgb(0x11, 0x11, 0x1B)),
            BorderBrush     = new SolidColorBrush(Color.FromRgb(0x31, 0x32, 0x44)),
            BorderThickness = new Thickness(1),
            CornerRadius    = new CornerRadius(6),
            Margin          = new Thickness(0, 12, 0, 0),
            Padding         = new Thickness(12, 10, 12, 10),
            Child           = regInner,
            IsEnabled       = false,   // enabled only after arc_open
        };

        // ── Transmit paneli ───────────────────────────────────────────
        var transmitBorder = TransmitPaneliOlustur(cb);

        // ── PropertyChanged: rozet + panels + live status timer ─────────
        cb.PropertyChanged += (_, ea) =>
        {
            GuncelleDurum();
            if (ea.PropertyName == nameof(CihazBaglanti.AcikMi))
            {
                Dispatcher.InvokeAsync(() =>
                {
                    bool acik = cb.AcikMi;
                    regBorder.IsEnabled      = acik;
                    transmitBorder.IsEnabled = acik;
                    if (!acik)
                    {
                        canliTimer.Stop();
                        statusTimer.Stop();
                        canliAcik        = false;
                        canliBtn.Content = "▶  Canlı";
                        agMetin.Text       = "—";
                        agRozet.Background = new SolidColorBrush(Color.FromRgb(0x31, 0x32, 0x44));
                        agMetin.Foreground = new SolidColorBrush(Color.FromRgb(0x6C, 0x70, 0x86));
                    }
                });
            }
            else if (ea.PropertyName == nameof(CihazBaglanti.InitTamamlandi))
            {
                Dispatcher.InvokeAsync(() =>
                {
                    if (cb.InitTamamlandi) statusTimer.Start();
                    else                   statusTimer.Stop();
                });
            }
        };
        GuncelleDurum();

        // ── Cihaz yolu etiketi ────────────────────────────────────────
        var pathLabel = new TextBlock
        {
            Text         = cb.DevicePath,
            FontFamily   = new FontFamily("Consolas"),
            FontSize     = 10,
            Foreground   = new SolidColorBrush(Color.FromRgb(0x6C, 0x70, 0x86)),
            TextWrapping = TextWrapping.Wrap,
            Margin       = new Thickness(0, 0, 0, 12),
        };

        // ── İçerik grid ──────────────────────────────────────────────
        var grid = new Grid { Margin = new Thickness(16) };
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        Grid.SetRow(pathLabel,      0);
        Grid.SetRow(kontrolSatiri,  1);
        Grid.SetRow(regBorder,      2);
        Grid.SetRow(transmitBorder, 3);
        grid.Children.Add(pathLabel);
        grid.Children.Add(kontrolSatiri);
        grid.Children.Add(regBorder);
        grid.Children.Add(transmitBorder);

        return new TabItem { Header = cb.KisaAd, Content = grid, Tag = cb };
    }

    // ── Register panel builder ────────────────────────────────────

    private static (UIElement rows, Action<byte[]> guncelle) RegPaneliOlustur()
    {
        // Register names for COM20022
        string[] ad = { "Reg0 STATUS", "Reg1", "Reg2", "Reg3", "Reg4", "Reg5", "Reg6", "Reg7" };

        var hexBlocks = new TextBlock[8];
        var binBlocks = new TextBlock[8];
        TextBlock? bitDetail = null;

        var stack = new StackPanel();

        for (int i = 0; i < 8; i++)
        {
            bool isReg0 = i == 0;
            var fg      = isReg0 ? Color.FromRgb(0x89, 0xB4, 0xFA) : Color.FromRgb(0xCD, 0xD6, 0xF4);
            var fgDim   = isReg0 ? Color.FromRgb(0x89, 0xB4, 0xFA) : Color.FromRgb(0x6C, 0x70, 0x86);

            var hexTb = new TextBlock
            {
                Text       = "----",
                Width      = 46,
                FontFamily = new FontFamily("Consolas"),
                FontSize   = 12,
                FontWeight = isReg0 ? FontWeights.Bold : FontWeights.Normal,
                Foreground = new SolidColorBrush(fg),
            };
            var binTb = new TextBlock
            {
                Text       = "---- ----",
                Width      = 90,
                FontFamily = new FontFamily("Consolas"),
                FontSize   = 12,
                Foreground = new SolidColorBrush(fgDim),
                Margin     = new Thickness(6, 0, 0, 0),
            };
            hexBlocks[i] = hexTb;
            binBlocks[i] = binTb;

            var nameTb = new TextBlock
            {
                Text       = ad[i] + ":",
                Width      = 110,
                FontFamily = new FontFamily("Segoe UI"),
                FontSize   = 12,
                FontWeight = isReg0 ? FontWeights.SemiBold : FontWeights.Normal,
                Foreground = new SolidColorBrush(Color.FromRgb(0xCD, 0xD6, 0xF4)),
            };
            var row = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Margin      = new Thickness(0, i == 0 ? 0 : 2, 0, 0),
            };
            row.Children.Add(nameTb);
            row.Children.Add(hexTb);
            row.Children.Add(binTb);
            stack.Children.Add(row);

            if (isReg0)
            {
                bitDetail = new TextBlock
                {
                    // Named bits: bit7=RST (confirmed), bit6=RI? (uncertain),
                    // bit2=RECON? (uncertain), bit1=TMA (confirmed), bit0=TA (confirmed)
                    Text         = "  → bit7(RST)=?  bit6(RI?)=?  bit2(RECON?)=?  bit1(TMA)=?  bit0(TA)=?",
                    FontFamily   = new FontFamily("Consolas"),
                    FontSize     = 10,
                    Foreground   = new SolidColorBrush(Color.FromRgb(0x6C, 0x70, 0x86)),
                    Margin       = new Thickness(0, 1, 0, 6),
                    TextWrapping = TextWrapping.Wrap,
                };
                stack.Children.Add(bitDetail);
            }
        }

        void Guncelle(byte[] vals)
        {
            for (int i = 0; i < 8; i++)
            {
                hexBlocks[i].Text = $"0x{vals[i]:X2}";
                binBlocks[i].Text = HexToBin(vals[i]);
            }
            if (bitDetail != null)
                bitDetail.Text = Reg0BitDetail(vals[0]);
        }

        return (stack, Guncelle);
    }

    private static string HexToBin(byte v) =>
        $"{(v>>7)&1}{(v>>6)&1}{(v>>5)&1}{(v>>4)&1} {(v>>3)&1}{(v>>2)&1}{(v>>1)&1}{v&1}";

    private static string Reg0BitDetail(byte v)
    {
        // Named bits in COM20022 STATUS register.
        // Confident: TA (bit0), TMA (bit1), RST (bit7).
        // Uncertain (marked ?): RI? (bit6), RECON? (bit2) — verify against your datasheet.
        var named = new (int bit, string name)[]
        {
            (7, "RST"), (6, "RI?"), (2, "RECON?"), (1, "TMA"), (0, "TA"),
        };
        var sb = new System.Text.StringBuilder("  → ");
        foreach (var (bit, name) in named)
        {
            if (sb.Length > 4) sb.Append("  ");
            sb.Append($"bit{bit}({name})={(v >> bit) & 1}");
        }
        return sb.ToString();
    }

    // ── Transmit panel builder ────────────────────────────────────

    private Border TransmitPaneliOlustur(CihazBaglanti cb)
    {
        // Shorthand for creating a SolidColorBrush (not frozen — per-tab, low count)
        SolidColorBrush C(byte r, byte g, byte b) => new SolidColorBrush(Color.FromRgb(r, g, b));
        var ff   = new FontFamily("Segoe UI");
        var mono = new FontFamily("Consolas");
        var wh   = C(0xCD, 0xD6, 0xF4); // text
        var dim  = C(0x6C, 0x70, 0x86); // dim text
        var surf = C(0x31, 0x32, 0x44); // surface0
        var brd  = C(0x58, 0x5B, 0x70); // border colour

        // ── Hedef node ────────────────────────────────────────────
        var hedefBox = new TextBox
        {
            Text = "2", Width = 52, FontFamily = ff, FontSize = 12,
            Foreground = wh, Background = surf, BorderBrush = brd, BorderThickness = new Thickness(1),
            Padding = new Thickness(6, 3, 6, 3), VerticalContentAlignment = VerticalAlignment.Center,
            Margin = new Thickness(6, 0, 14, 0),
        };

        // ── Mod: Metin / Hex ──────────────────────────────────────
        var metinRadio = new RadioButton
        {
            Content = "Metin", IsChecked = true,
            Foreground = wh, VerticalAlignment = VerticalAlignment.Center,
            FontFamily = ff, FontSize = 12, Margin = new Thickness(0, 0, 12, 0),
        };
        var hexRadio = new RadioButton
        {
            Content = "Hex",
            Foreground = wh, VerticalAlignment = VerticalAlignment.Center,
            FontFamily = ff, FontSize = 12, Margin = new Thickness(0, 0, 16, 0),
        };

        // ── ACK onay kutusu ───────────────────────────────────────
        var ackCheck = new CheckBox
        {
            Content = "ACK Bekle", IsChecked = true,
            Foreground = wh, VerticalAlignment = VerticalAlignment.Center,
            FontFamily = ff, FontSize = 12,
        };

        // ── Ayarlar satırı ────────────────────────────────────────
        var ayarSatiri = new WrapPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 6, 0, 0) };
        ayarSatiri.Children.Add(new TextBlock { Text = "Hedef:", Foreground = wh, FontFamily = ff, FontSize = 12, VerticalAlignment = VerticalAlignment.Center });
        ayarSatiri.Children.Add(hedefBox);
        ayarSatiri.Children.Add(metinRadio);
        ayarSatiri.Children.Add(hexRadio);
        ayarSatiri.Children.Add(ackCheck);

        // ── Veri girişi ───────────────────────────────────────────
        var veriBox = new TextBox
        {
            Text = "Merhaba ARCNET",
            FontFamily = mono, FontSize = 12,
            Foreground = wh, Background = surf, BorderBrush = brd, BorderThickness = new Thickness(1),
            Padding = new Thickness(6, 5, 6, 5), VerticalContentAlignment = VerticalAlignment.Center,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            Margin = new Thickness(0, 8, 0, 0),
        };
        var veriHint = new TextBlock
        {
            Text = "Metin modu: yazı yazın   |   Hex modu: boşlukla ayrılmış baytlar, örn: 48 65 6C 6C 6F",
            FontFamily = ff, FontSize = 9, Foreground = dim, Margin = new Thickness(0, 2, 0, 0),
        };

        // ── Sonuç rozeti ──────────────────────────────────────────
        var sonucMetin = new TextBlock
        {
            FontFamily = ff, FontSize = 11, FontWeight = FontWeights.SemiBold,
            VerticalAlignment = VerticalAlignment.Center,
        };
        var sonucRozet = new Border
        {
            CornerRadius = new CornerRadius(10), Padding = new Thickness(10, 3, 10, 3),
            Child = sonucMetin, Visibility = Visibility.Collapsed,
            Margin = new Thickness(12, 0, 0, 0),
        };

        void GosterSonuc(string txt, Color arka, Color yazi)
        {
            sonucMetin.Text       = txt;
            sonucRozet.Background = new SolidColorBrush(arka);
            sonucMetin.Foreground = new SolidColorBrush(yazi);
            sonucRozet.Visibility = Visibility.Visible;
        }

        // ── Gönder butonu ─────────────────────────────────────────
        var gonderBtn = new Button
        {
            Content = "⬆  Gönder", Style = (Style)FindResource("TestButton"), MinWidth = 100,
        };
        var gonderSatiri = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 8, 0, 0) };
        gonderSatiri.Children.Add(gonderBtn);
        gonderSatiri.Children.Add(sonucRozet);

        // ── Gönder click ──────────────────────────────────────────
        gonderBtn.Click += async (_, _) =>
        {
            // Validate destination (1-255; 0 = broadcast, unsupported here)
            if (!byte.TryParse(hedefBox.Text.Trim(), out byte dest) || dest == 0)
            {
                GosterSonuc("✘ Geçersiz hedef (1-255)",
                    Color.FromRgb(0x4A, 0x1F, 0x1F), Color.FromRgb(0xF3, 0x8B, 0xA8));
                return;
            }

            // Parse data according to selected mode
            byte[] data;
            if (metinRadio.IsChecked == true)
            {
                string txt = veriBox.Text;
                if (string.IsNullOrEmpty(txt))
                {
                    GosterSonuc("✘ Veri boş olamaz",
                        Color.FromRgb(0x4A, 0x1F, 0x1F), Color.FromRgb(0xF3, 0x8B, 0xA8));
                    return;
                }
                data = Encoding.ASCII.GetBytes(txt);
            }
            else
            {
                try   { data = ParseHex(veriBox.Text); }
                catch (Exception ex)
                {
                    GosterSonuc($"✘ Hex hatası: {ex.Message}",
                        Color.FromRgb(0x4A, 0x1F, 0x1F), Color.FromRgb(0xF3, 0x8B, 0xA8));
                    return;
                }
                if (data.Length == 0)
                {
                    GosterSonuc("✘ Veri boş olamaz",
                        Color.FromRgb(0x4A, 0x1F, 0x1F), Color.FromRgb(0xF3, 0x8B, 0xA8));
                    return;
                }
            }

            if (data.Length > 508)
            {
                GosterSonuc($"✘ Veri çok uzun ({data.Length} bayt, max 508)",
                    Color.FromRgb(0x4A, 0x1F, 0x1F), Color.FromRgb(0xF3, 0x8B, 0xA8));
                return;
            }

            // 254-256 dead zone: ARCNET packet sizes in this range are unreliable;
            // warn but allow the user to try anyway.
            bool deadZone = data.Length >= 254 && data.Length <= 256;
            if (deadZone)
                GosterSonuc($"⚠ {data.Length} bayt (254-256 ölü bölge, güvenilmez) — yine de deneniyor…",
                    Color.FromRgb(0x4A, 0x3B, 0x1A), Color.FromRgb(0xF9, 0xE2, 0xAF));
            else
                GosterSonuc("⏳ Gönderiliyor…",
                    Color.FromRgb(0x31, 0x32, 0x44), Color.FromRgb(0x6C, 0x70, 0x86));

            bool waitAck = ackCheck.IsChecked == true;

            // Log outgoing packet
            AcLogPanel();
            string ozet = data.Length <= 32
                ? "\"" + new string(data.Select(b => b >= 32 && b < 127 ? (char)b : '.').ToArray()) + "\""
                : "[" + data.Length + " bayt: " + string.Join(" ", data.Take(8).Select(b => $"{b:X2}")) + (data.Length > 8 ? "…" : "") + "]";
            LogEkle($"→ [{cb.KisaAd} → node {dest}]  {data.Length} bayt  waitAck={waitAck}  {ozet}", BrMavi);

            gonderBtn.IsEnabled = false;
            ArcResult r;
            try   { r = await cb.GonderAsync(dest, data, waitAck); }
            finally { gonderBtn.IsEnabled = true; }

            switch (r)
            {
                case ArcResult.Ok:
                    GosterSonuc("✔ Gönderildi (ACK alındı)",
                        Color.FromRgb(0x26, 0x4A, 0x2E), Color.FromRgb(0xA6, 0xE3, 0xA1));
                    LogEkle("  ✔ ARC_OK — ACK alındı.", BrYesil);
                    break;
                case ArcResult.NotAcked:
                    GosterSonuc("⚠ Gönderildi ama ACK yok (alıcı pasif?)",
                        Color.FromRgb(0x4A, 0x3B, 0x1A), Color.FromRgb(0xF9, 0xE2, 0xAF));
                    LogEkle("  ⚠ ARC_NOT_ACKED — alıcı yok veya init edilmemiş.", BrSari);
                    break;
                case ArcResult.ErrNetBusy:
                    GosterSonuc("⚠ Ağ meşgul, tekrar dene",
                        Color.FromRgb(0x4A, 0x3B, 0x1A), Color.FromRgb(0xF9, 0xE2, 0xAF));
                    LogEkle("  ⚠ ARC_ERR_NET_BUSY — ağ geçici meşgul.", BrSari);
                    break;
                case ArcResult.ErrDeviceGone:
                    GosterSonuc("✘ Cihaz çekildi",
                        Color.FromRgb(0x4A, 0x1F, 0x1F), Color.FromRgb(0xF3, 0x8B, 0xA8));
                    LogEkle("  ✘ ARC_ERR_DEVICE_GONE — cihaz bağlantısı kesildi.", BrKirmizi);
                    break;
                default:
                    string rs = ArcnetDevice.ResultString(r);
                    GosterSonuc($"✘ {rs}",
                        Color.FromRgb(0x4A, 0x1F, 0x1F), Color.FromRgb(0xF3, 0x8B, 0xA8));
                    LogEkle($"  ✘ {rs}", BrKirmizi);
                    break;
            }
        };

        // ── Panel ─────────────────────────────────────────────────
        var baslik = new TextBlock
        {
            Text = "Manuel Gönder",
            FontFamily = ff, FontSize = 11, FontWeight = FontWeights.SemiBold,
            Foreground = C(0x89, 0xB4, 0xFA),
        };
        var ic = new StackPanel();
        ic.Children.Add(baslik);
        ic.Children.Add(ayarSatiri);
        ic.Children.Add(veriBox);
        ic.Children.Add(veriHint);
        ic.Children.Add(gonderSatiri);

        return new Border
        {
            Background = C(0x11, 0x11, 0x1B), BorderBrush = C(0x31, 0x32, 0x44),
            BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(6),
            Margin = new Thickness(0, 8, 0, 0), Padding = new Thickness(12, 10, 12, 10),
            Child = ic, IsEnabled = false,
        };
    }

    private static byte[] ParseHex(string input)
    {
        var tokens = input.Trim().Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
        if (tokens.Length == 0) return Array.Empty<byte>();
        var result = new byte[tokens.Length];
        for (int i = 0; i < tokens.Length; i++)
        {
            string tok = tokens[i];
            if (tok.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) tok = tok[2..];
            result[i] = Convert.ToByte(tok, 16);
        }
        return result;
    }

    private async Task AcVeInitIsle(CihazBaglanti cb, Button acBtn, TextBox nodeBox)
    {
        if (cb.AcikMi) return;

        // Node ID çakışma kontrolü — sadece bu GUI oturumundaki açık cihazlar arasında
        var cakisan = _acikCihazlar.FirstOrDefault(o => o != cb && o.AcikMi && o.NodeId == cb.NodeId);
        if (cakisan != null)
        {
            AcLogPanel();
            LogEkle($"✘  Node ID {cb.NodeId} zaten [{cakisan.KisaAd}] tarafından kullanılıyor — farklı bir ID girin.", BrKirmizi);
            return;
        }

        acBtn.IsEnabled   = false;
        nodeBox.IsEnabled = false;
        AcLogPanel();
        LogEkle($"Bağlanıyor: {cb.KisaAd}  (node {cb.NodeId})", BrMavi);

        try
        {
            var r = await cb.AcVeInit();
            if (r == ArcResult.Ok)
                LogEkle($"✔  {cb.KisaAd} → Init OK (node={cb.NodeId})", BrYesil);
            else
            {
                LogEkle($"✘  {cb.KisaAd} → Init başarısız: {ArcnetDevice.ResultString(r)}", BrKirmizi);
                acBtn.IsEnabled   = true;
                nodeBox.IsEnabled = true;
            }
        }
        catch (ArcnetException ex)
        {
            LogEkle($"✘  {cb.KisaAd} → {ex.Message}", BrKirmizi);
            acBtn.IsEnabled   = true;
            nodeBox.IsEnabled = true;
        }
        catch (Exception ex)
        {
            LogEkle($"✘  {cb.KisaAd} → Beklenmeyen hata: {ex.Message}", BrKirmizi);
            acBtn.IsEnabled   = true;
            nodeBox.IsEnabled = true;
        }
    }

    private async Task KapatIsleAsync(CihazBaglanti cb, Button acBtn, TextBox nodeBox)
    {
        acBtn.IsEnabled = false;
        AcLogPanel();
        var r = await cb.KapatAsync(cLog: msg =>
            Dispatcher.InvokeAsync(() => LogEkle($"  [C] {msg.TrimEnd()}", BrGri)));
        acBtn.IsEnabled   = true;
        nodeBox.IsEnabled = true;
        if (r == ArcResult.Ok)
            LogEkle($"● {cb.KisaAd} kapatıldı.", BrGri);
        else
            LogEkle($"✘ {cb.KisaAd} kapatılırken arc_shutdown hatası: {ArcnetDevice.ResultString(r)}", BrKirmizi);
    }

    private async void KurBtn_Click(object sender, RoutedEventArgs e)
    {
        string? driverDir = DriverKlasorunuBul();

        AcLogPanel();
        LogKutu.Document.Blocks.Clear();

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

        var pnpCikti = new List<string>();
        ok = await KomutCalistirAsync(
            "pnputil.exe",
            $"/add-driver \"{infPath}\" /install",
            driverDir,
            pnpCikti,
            extraOkCodes: new[] { 259 });   // 259 = zaten güncel (ERROR_NO_MORE_ITEMS)

        if (!ok)
        {
            LogEkle("✘  pnputil başarısız — sürücü yüklenemedi.", BrKirmizi);
            return false;
        }

        bool zatenGuncel = pnpCikti.Any(s =>
            s.Contains("up-to-date",       StringComparison.OrdinalIgnoreCase) ||
            s.Contains("Already exists",   StringComparison.OrdinalIgnoreCase) ||
            s.Contains("added successfully", StringComparison.OrdinalIgnoreCase));

        LogEkle(zatenGuncel
            ? "✔  Sürücü zaten yüklü ve güncel."
            : "✔  Adım 2 tamamlandı.", BrYesil);
        return true;
    }

    // ── Test: seçili cihaz ───────────────────────────────────────

    private async void TestBtn_Click(object sender, RoutedEventArgs e)
    {
        AcLogPanel();
        LogKutu.Document.Blocks.Clear();

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
        AcLogPanel();
        LogKutu.Document.Blocks.Clear();

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
        bool openOk    = satirlar.Any(s => s.Contains("arc_open: ARC_OK"));
        bool initOk    = satirlar.Any(s => s.Contains("arc_init: ARC_OK"));
        bool hardHata  = satirlar.Any(s =>
            s.Contains("ARC_ERR_IO") || s.Contains("ARC_ERR_DEVICE_GONE") ||
            s.Contains("device gone") || s.Contains("hardware error"));

        LogEkle("", BrGri);
        LogEkle("── Sonuç ──────────────────────────────────────", BrGri);

        // Gerçek donanım hatası her zaman başarısız sayılır
        if (hardHata)
        {
            string satir = satirlar.FirstOrDefault(s =>
                s.Contains("ARC_ERR_IO") || s.Contains("ARC_ERR_DEVICE_GONE") ||
                s.Contains("device gone") || s.Contains("hardware error")) ?? "";
            LogEkle($"✘  Cihaza erişilemedi / donanım hatası: {satir.Trim()}", BrKirmizi);
            return false;
        }

        // Open veya init başarısız ise cihaza ulaşılamıyor
        if (!openOk || !initOk)
        {
            if (!openOk)
                LogEkle("✘  arc_open başarısız — cihaz bulunamadı veya sürücü yüklü değil.", BrKirmizi);
            else
                LogEkle("✘  arc_init başarısız — cihaz açıldı ama başlatılamadı.", BrKirmizi);
            return false;
        }

        // Open + init OK → cihaz çalışıyor. Transmit sonucu sadece bilgi amaçlı.
        bool transmitAck    = satirlar.Any(s => s.Contains("arc_transmit: ARC_OK"));
        bool transmitNotAck = satirlar.Any(s => s.Contains("ARC_NOT_ACKED"));
        bool netBusy        = satirlar.Any(s => s.Contains("ARC_ERR_NET_BUSY"));

        if (transmitAck)
        {
            LogEkle("✔  Cihaz çalışıyor (paket ACK'lendi)", BrYesil);
        }
        else if (transmitNotAck)
        {
            LogEkle("✔  Cihaz çalışıyor", BrYesil);
            LogEkle("   (ağda paketi onaylayan aktif bir node yok — tek cihaz testinde normaldir)", BrSari);
        }
        else if (netBusy)
        {
            LogEkle("✔  Cihaz çalışıyor", BrYesil);
            LogEkle("   (ağ geçici meşgul — RECON devam ediyor)", BrSari);
        }
        else
        {
            LogEkle("✔  Cihaz çalışıyor (transmit durumu bilinmiyor)", BrYesil);
        }
        return true;
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
        int timeoutMs = -1,
        int[]? extraOkCodes = null)
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

            bool exitOk = proc.ExitCode == 0 || (extraOkCodes?.Contains(proc.ExitCode) ?? false);
            if (!exitOk)
                LogEkle($"  (çıkış kodu: {proc.ExitCode})", BrKirmizi);
            return exitOk;
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

    private void AcLogPanel()
    {
        SplitterRowDef.Height  = new GridLength(5);
        if (LogRowDef.Height.Value == 0)
            LogRowDef.Height = new GridLength(220);
        LogSplitter.Visibility = Visibility.Visible;
        LogPanel.Visibility    = Visibility.Visible;
    }

    private void LogEkle(string metin, Brush renk)
    {
        var para = new Paragraph(new Run(metin))
        {
            Foreground = renk,
            Margin     = new Thickness(0),
            LineHeight = 16,
        };
        LogKutu.Document.Blocks.Add(para);
        Dispatcher.InvokeAsync(() => LogKutu.ScrollToEnd(), DispatcherPriority.Background);
    }

    private void LogKopyalaBtn_Click(object sender, RoutedEventArgs e)
    {
        var text = new TextRange(LogKutu.Document.ContentStart, LogKutu.Document.ContentEnd).Text;
        if (!string.IsNullOrWhiteSpace(text))
            Clipboard.SetText(text);
    }

    private void LogKaydetBtn_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.SaveFileDialog
        {
            Title    = "Log'u Kaydet",
            Filter   = "Metin dosyası|*.txt|Tüm dosyalar|*.*",
            FileName = $"arcnet-log-{DateTime.Now:yyyyMMdd-HHmmss}.txt",
        };
        if (dlg.ShowDialog() != true) return;

        var text = new TextRange(LogKutu.Document.ContentStart, LogKutu.Document.ContentEnd).Text;
        File.WriteAllText(dlg.FileName, text, System.Text.Encoding.UTF8);
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

// ── CihazBaglanti — tek ARC cihaz bağlantısı (AŞAMA 1) ───────────

public class CihazBaglanti : INotifyPropertyChanged, IDisposable
{
    private ArcnetDevice? _dev;

    public string DevicePath { get; }
    public string KisaAd     { get; }

    private byte _nodeId = 1;
    public byte NodeId
    {
        get => _nodeId;
        set { _nodeId = value; OnPropertyChanged(); }
    }

    private string _durumMetin = "● Kapalı";
    public string DurumMetin
    {
        get => _durumMetin;
        private set { _durumMetin = value; OnPropertyChanged(); }
    }

    private Brush _durumArka = BrKapali;
    public Brush DurumArka
    {
        get => _durumArka;
        private set { _durumArka = value; OnPropertyChanged(); }
    }

    private Brush _durumYazi = BrYaziKapali;
    public Brush DurumYazi
    {
        get => _durumYazi;
        private set { _durumYazi = value; OnPropertyChanged(); }
    }

    public bool AcikMi => _dev != null;

    private bool _initTamamlandi;
    public bool InitTamamlandi
    {
        get => _initTamamlandi;
        private set { if (_initTamamlandi == value) return; _initTamamlandi = value; OnPropertyChanged(); }
    }

    public CihazBaglanti(string devicePath)
    {
        DevicePath = devicePath;
        KisaAd     = CikarKisaAd(devicePath);
    }

    private static string CikarKisaAd(string path)
    {
        // \\?\usb#vid_0d0b&pid_1002#6&18b4745&0&4#{guid}
        // segs[2] = "6&18b4745&0&4"  → last two '&' fields = port/instance suffix
        var segs = path.Split('#');
        if (segs.Length >= 3)
        {
            var parts = segs[2].Split('&');
            if (parts.Length >= 2)
                return $"USB22 &{parts[^2]}&{parts[^1]}";
        }
        return path.Length > 20 ? path[^20..] : path;
    }

    public async Task<ArcResult> AcVeInit()
    {
        if (_dev != null) return ArcResult.Ok;

        SetDurum("⌛ Açılıyor…", BrAciyor, BrYaziAciyor);
        try
        {
            // arc_open is fast; arc_init blocks ~2.5 s → offload
            _dev = new ArcnetDevice(DevicePath, verbose: false);
            OnPropertyChanged(nameof(AcikMi));

            SetDurum("⌛ Init…", BrAciyor, BrYaziAciyor);
            byte nodeId = _nodeId;
            var r = await Task.Run(() => _dev.Init(nodeId));

            if (r == ArcResult.Ok)
            {
                InitTamamlandi = true;
                SetDurum($"✔ Ağda aktif (node={nodeId})", BrInitOk, BrYaziInitOk);
            }
            else
            {
                _dev.Dispose(); _dev = null;
                OnPropertyChanged(nameof(AcikMi));
                SetDurum($"✘ Init başarısız: {ArcnetDevice.ResultString(r)}", BrHata, BrYaziHata);
            }

            return r;
        }
        catch
        {
            _dev?.Dispose(); _dev = null;
            OnPropertyChanged(nameof(AcikMi));
            SetDurum("✘ Açılamadı", BrHata, BrYaziHata);
            throw;
        }
    }

    // Transmits data synchronously (call from Task.Run — waitAck can block ~100 ms).
    // Returns the ArcResult; never throws.
    public async Task<ArcResult> GonderAsync(byte destNode, byte[] data, bool waitAck)
    {
        var dev = _dev;
        if (dev == null) return ArcResult.ErrOpen;
        try   { return await Task.Run(() => dev.Transmit(destNode, data, waitAck: waitAck)); }
        catch (ObjectDisposedException) { return ArcResult.ErrDeviceGone; }
        catch                           { return ArcResult.ErrIo; }
    }

    // Reads registers 0-7 synchronously (call from Task.Run to avoid UI freeze).
    // Returns 8-byte array on success, null on any error.
    public byte[]? RegisterlariOkuSync()
    {
        var dev = _dev;
        if (dev == null) return null;
        var regs = new byte[8];
        try
        {
            for (int i = 0; i < 8; i++)
            {
                var r = dev.ReadRegister((byte)i, out regs[i]);
                if (r != ArcResult.Ok) return null;
            }
            return regs;
        }
        catch (ObjectDisposedException) { return null; }
        catch { return null; }
    }

    public void Kapat()
    {
        var dev = _dev;
        _dev = null;
        InitTamamlandi = false;
        OnPropertyChanged(nameof(AcikMi));
        SetDurum("● Kapalı", BrKapali, BrYaziKapali);
        if (dev != null) try { dev.Shutdown(); } catch { }
    }

    public async Task<ArcResult> KapatAsync(Action<string>? cLog = null)
    {
        var dev = _dev;
        _dev = null;
        InitTamamlandi = false;
        OnPropertyChanged(nameof(AcikMi));
        SetDurum("● Kapalı", BrKapali, BrYaziKapali);
        if (dev == null) return ArcResult.Ok;
        return await Task.Run(() => {
            try
            {
                if (cLog != null)
                {
                    dev.SetLogLevel(ArcLogLevel.Info);
                    dev.SetLogCallback((_, msg) => cLog(msg));
                }
                return dev.Shutdown();
            }
            catch { return ArcResult.ErrIo; }
        });
    }

    public void RaporAgDurumu(byte reg0)
    {
        if (_dev == null) return;
        if ((reg0 & 0x20) != 0)
            SetDurum($"✔ Ağda aktif (node={_nodeId})", BrInitOk, BrYaziInitOk);
        else if (reg0 == 0x00)
            SetDurum("⚠ Ağda DEĞİL", BrHata, BrYaziHata);
        else
            SetDurum($"● Geçiş 0x{reg0:X2}", BrAciyor, BrYaziAciyor);
    }

    public void Dispose() => Kapat();

    private void SetDurum(string metin, Brush arka, Brush yazi)
    {
        DurumMetin = metin;
        DurumArka  = arka;
        DurumYazi  = yazi;
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void OnPropertyChanged([CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    private static readonly Brush BrKapali     = Mk(0x31, 0x32, 0x44);
    private static readonly Brush BrAciyor     = Mk(0x4A, 0x3B, 0x1A);
    private static readonly Brush BrInitOk     = Mk(0x26, 0x4A, 0x2E);
    private static readonly Brush BrHata       = Mk(0x4A, 0x1F, 0x1F);
    private static readonly Brush BrYaziKapali = Mk(0x6C, 0x70, 0x86);
    private static readonly Brush BrYaziAciyor = Mk(0xF9, 0xE2, 0xAF);
    private static readonly Brush BrYaziInitOk = Mk(0xA6, 0xE3, 0xA1);
    private static readonly Brush BrYaziHata   = Mk(0xF3, 0x8B, 0xA8);

    private static SolidColorBrush Mk(byte r, byte g, byte b)
    {
        var br = new SolidColorBrush(Color.FromRgb(r, g, b)); br.Freeze(); return br;
    }
}

// ── Veri modelleri ────────────────────────────────────────────────

public enum TestSonucu { Bilinmiyor, Calisiyor, Basarili, Basarisiz }

public class CihazSatiri : INotifyPropertyChanged
{
    public string PID           { get; }
    public string SurucuServisi { get; }
    public string CihazTipi     { get; }
    public string Aciklama      { get; }
    public string InstancePath  { get; }
    public string Durum         { get; }
    public Brush  RozetArka     { get; }
    public Brush  RozetYazi     { get; }

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
