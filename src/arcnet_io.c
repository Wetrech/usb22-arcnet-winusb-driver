/*
 * arcnet_io.c  —  Adim 4 (rev 2): Init + Register; UPDATE 4 duzeltmeleri.
 *
 * Protokol kaynak: reference/usb22-protocol-notes.md (UPDATE 1-4)
 *
 * UPDATE 4 degisiklikleri:
 *   1. read_response artik beklenen opcode'u parametre olarak aliyor;
 *      byte0 == expected_opcode olana kadar döngüde okuyor (0x20 event'leri
 *      ve baska beklenmeyen paketleri atlayarak).
 *   2. Cevap boyutu sabit 7 yerine fiili bytesTransferred kullaniliyor:
 *        opcode 0x00 Init   -> 6 byte cevap
 *        opcode 0x01 Reg    -> 7 byte cevap
 *        opcode 0x04 Cmd04  -> 4 byte cevap
 *   3. arc_init cevabini parse ediyor: byte[4] = status (0x22 = OK görüldü).
 *   4. arc_cmd04() eklendi: "04 00" gönderir, "04 00 00 00" bekler;
 *      orijinal sürücü Init'ten once bunu yapıyor.
 */

#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ---- Hedef cihaz ---- */
#define CC_VID              0x0D0Bu
#define CC_PID              0x1002u

/* ---- Endpoint adresleri ---- */
#define EP_CMD_OUT          0x01u   /* Komut kanali OUT                */
#define EP_EVT_IN           0x81u   /* Cevap + async event kanali IN   */
#define EP_TX_OUT           0x02u   /* ARCNET transmit veri kanali OUT */
#define EP_RX_IN            0x86u   /* ARCNET receive veri kanali IN   */

/* ---- Opcode'lar (protocol notes UPDATE 2 + 4) ---- */
#define OPCODE_INIT         0x00u   /* 12B komut, 6B cevap */
#define OPCODE_REGISTER     0x01u   /*  5B komut, 7B cevap */
#define OPCODE_CMD04        0x04u   /*  2B komut, 4B cevap; session baslangici */
#define OPCODE_EVENT        0x20u   /* 6B; cihazdan spontan RECON/status bildirim */

/* EP 0x81 MaxPacketSize (probe ciktisinda gözlemlendi: 64 byte).
 * Tüm ReadPipe çagrilari bu kadar tampon kullanir; short-packet kurali
 * sayesinde cihaz daha az byte gönderirse ReadPipe hemen döner. */
#define EP_EVT_MAXPACKET    64u

/* ReadPipe timeout (her okuma girisimi için, ms) */
#define READ_TIMEOUT_MS     1000u

/* read_response zaman butceleri (ms) */
#define BUDGET_SHORT_MS     1500u   /* cmd04, register: anlik cevap beklenir */
#define BUDGET_INIT_MS      5000u   /* init: cihaz ~2.5 sn dusunuyor         */

/* GUID_DEVINTERFACE_USB_DEVICE = {A5DCBF10-6530-11D2-901F-00C04FB951ED} */
static const GUID GUID_USB_DEVICE = {
    0xA5DCBF10u, 0x6530u, 0x11D2u,
    { 0x90u, 0x1Fu, 0x00u, 0xC0u, 0x4Fu, 0xB9u, 0x51u, 0xEDu }
};

/* Cihaz baglami */
typedef struct {
    HANDLE                  dev_handle;
    WINUSB_INTERFACE_HANDLE usb_handle;
} ARC_DEVICE;

/* =======================================================================
 * find_device_path  (winusb_probe.c ile ayni)
 * ===================================================================== */
static int find_device_path(unsigned int vid, unsigned int pid,
                             char *path_buf, DWORD buf_size)
{
    HDEVINFO                           devs;
    SP_DEVICE_INTERFACE_DATA           iface_data;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail;
    DWORD                              idx, needed;
    char                               token[32];
    char                               upper[512];
    char                              *p;
    int                                found = 0;

    snprintf(token, sizeof(token), "VID_%04X&PID_%04X", vid, pid);
    devs = SetupDiGetClassDevsA(&GUID_USB_DEVICE, NULL, NULL,
                                 DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[HATA] SetupDiGetClassDevs: %lu\n", GetLastError());
        return 0;
    }
    iface_data.cbSize = sizeof(iface_data);
    for (idx = 0;
         SetupDiEnumDeviceInterfaces(devs, NULL, &GUID_USB_DEVICE, idx, &iface_data);
         idx++) {
        SetupDiGetDeviceInterfaceDetailA(devs, &iface_data, NULL, 0, &needed, NULL);
        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(needed);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(devs, &iface_data, detail,
                                               needed, NULL, NULL)) {
            free(detail); continue;
        }
        strncpy(upper, detail->DevicePath, sizeof(upper) - 1);
        upper[sizeof(upper) - 1] = '\0';
        for (p = upper; *p; p++) *p = (char)toupper((unsigned char)*p);
        if (strstr(upper, token)) {
            strncpy(path_buf, detail->DevicePath, buf_size - 1);
            path_buf[buf_size - 1] = '\0';
            found = 1;
            free(detail); break;
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

/* =======================================================================
 * arc_open
 * ===================================================================== */
static int arc_open(ARC_DEVICE *dev)
{
    char  path[512];
    ULONG timeout_ms = READ_TIMEOUT_MS;

    dev->dev_handle = INVALID_HANDLE_VALUE;
    dev->usb_handle = NULL;

    if (!find_device_path(CC_VID, CC_PID, path, sizeof(path))) {
        fprintf(stderr, "[HATA] Cihaz bulunamadi. Zadig ile WinUSB bind edildi mi?\n");
        return 0;
    }
    printf("[arc_open] %s\n", path);

    dev->dev_handle = CreateFileA(path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (dev->dev_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[HATA] CreateFile: %lu\n", GetLastError());
        return 0;
    }
    if (!WinUsb_Initialize(dev->dev_handle, &dev->usb_handle)) {
        fprintf(stderr, "[HATA] WinUsb_Initialize: %lu\n", GetLastError());
        CloseHandle(dev->dev_handle);
        dev->dev_handle = INVALID_HANDLE_VALUE;
        return 0;
    }
    /* EP 0x81 icin zaman asimi politikasi */
    if (!WinUsb_SetPipePolicy(dev->usb_handle, EP_EVT_IN,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(timeout_ms), &timeout_ms))
        fprintf(stderr, "[UYARI] SetPipePolicy: %lu\n", GetLastError());

    printf("[arc_open] OK\n\n");
    return 1;
}

/* =======================================================================
 * arc_close
 * ===================================================================== */
static void arc_close(ARC_DEVICE *dev)
{
    if (dev->usb_handle)                         WinUsb_Free(dev->usb_handle);
    if (dev->dev_handle != INVALID_HANDLE_VALUE) CloseHandle(dev->dev_handle);
    dev->usb_handle = NULL;
    dev->dev_handle = INVALID_HANDLE_VALUE;
    printf("[arc_close] Cihaz kapatildi.\n");
}

/* =======================================================================
 * write_cmd  —  EP 0x01'e komut gönder; ortak yardimci.
 *   Basarada gönderilen byte sayisini döner, 0 = hata.
 * ===================================================================== */
static ULONG write_cmd(ARC_DEVICE *dev, const BYTE *cmd, ULONG len)
{
    ULONG xferred = 0;
    if (!WinUsb_WritePipe(dev->usb_handle, EP_CMD_OUT,
                           (PUCHAR)cmd, len, &xferred, NULL)) {
        fprintf(stderr, "[HATA] WritePipe: %lu\n", GetLastError());
        return 0;
    }
    if (xferred != len) {
        fprintf(stderr, "[HATA] WritePipe: %lu/%lu byte gönderildi\n", xferred, len);
        return 0;
    }
    return xferred;
}

/* =======================================================================
 * read_response  —  EP 0x81'den beklenen opcode'a sahip paketi oku.
 *
 *   expected_opcode : byte0'in eslemesi gereken deger (örn. OPCODE_REGISTER)
 *   buf             : en az EP_EVT_MAXPACKET byte'lik tampon
 *   buf_size        : tampon boyutu
 *
 *   Okuma döngüsü:
 *     - byte0 == expected_opcode  -> döner (basari)
 *     - byte0 == OPCODE_EVENT (0x20) -> async event; atla, tekrar oku
 *     - byte0 == baska bir deger  -> beklenmeyen; atla, uyar, tekrar oku
 *     - MAX_SKIP asiminda veya hata/timeout'ta 0 döner
 *
 *   Not: EP_EVT_MAXPACKET (64) byte'lik tampon + short-packet kurali
 *   sayesinde cihaz kac byte gönderirse o kadar aliniyor; gerçek boyut
 *   dönüs degerinde.
 * ===================================================================== */
static ULONG read_response(ARC_DEVICE *dev, BYTE expected_opcode,
                            BYTE *buf, ULONG buf_size, DWORD budget_ms)
{
    ULONGLONG start = GetTickCount64();
    ULONG     xferred;
    DWORD     err;
    double    elapsed;

    while ((GetTickCount64() - start) < (ULONGLONG)budget_ms) {

        xferred = 0;
        if (!WinUsb_ReadPipe(dev->usb_handle, EP_EVT_IN,
                              buf, buf_size, &xferred, NULL)) {
            err = GetLastError();
            if (err == ERROR_SEM_TIMEOUT) {
                /*
                 * PIPE_TRANSFER_TIMEOUT (1 sn) doldu ama butce kalmis olabilir.
                 * Init ~2.5 sn bekletiyor; bu dala birden fazla girilebilir.
                 */
                continue;
            }
            fprintf(stderr, "[HATA] ReadPipe: %lu\n", err);
            return 0;
        }

        if (xferred == 0)
            continue;   /* ZLP: cihaz "henuz hazir degil"; butce kaldiysa devam */

        if (buf[0] == expected_opcode)
            return xferred;

        /* Farkli opcode: 0x20 event veya beklenmeyen; atla */
        elapsed = (double)(GetTickCount64() - start) / 1000.0;
        if (buf[0] == OPCODE_EVENT)
            printf("[read_response] 0x20 event atlanıyor (%.1fs)\n", elapsed);
        else
            printf("[read_response] Beklenmeyen opcode 0x%02X atlanıyor (%.1fs)\n",
                   buf[0], elapsed);
    }

    fprintf(stderr,
        "[HATA] read_response: %lu ms butce doldu, opcode 0x%02X bulunamadi.\n",
        budget_ms, expected_opcode);
    return 0;
}

/* =======================================================================
 * arc_handshake
 *   Init'ten once zorunlu veri-kanali el sikismasi (UPDATE 5).
 *   Orijinal surucu siralama:  cmd04 -> HANDSHAKE -> init
 *
 *   EP 0x02 OUT'a 10 bayt sifir yaz ("poll / are-you-there").
 *   EP 0x86 IN'den cevap oku (bos kanalda 10x 0x00 döner; timeout da kabul).
 *   Bu adim atlanirsa init cevap vermiyor.
 * ===================================================================== */
static int arc_handshake(ARC_DEVICE *dev)
{
    BYTE  out_buf[10];
    BYTE  in_buf[EP_EVT_MAXPACKET];
    ULONG xferred;
    ULONG timeout_ms = 500u;  /* kisa timeout; bos kanal yoklamasi */
    DWORD err;
    DWORD i;

    /* EP 0x86 icin ayri timeout politikasi (EP 0x81'den bagimsiz) */
    if (!WinUsb_SetPipePolicy(dev->usb_handle, EP_RX_IN,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(timeout_ms), &timeout_ms))
        fprintf(stderr, "[UYARI] arc_handshake SetPipePolicy EP0x86: %lu\n",
                GetLastError());

    /* 10 bayt sifir -> EP 0x02 OUT */
    memset(out_buf, 0x00, sizeof(out_buf));
    xferred = 0;
    if (!WinUsb_WritePipe(dev->usb_handle, EP_TX_OUT,
                           out_buf, sizeof(out_buf), &xferred, NULL)) {
        fprintf(stderr, "[HATA] arc_handshake WritePipe EP0x02: %lu\n",
                GetLastError());
        return 0;
    }
    printf("[arc_handshake] EP0x02 OUT: %lu byte sifir gönderildi.\n", xferred);

    /* EP 0x86 IN'den yanit oku */
    memset(in_buf, 0, sizeof(in_buf));
    xferred = 0;
    if (!WinUsb_ReadPipe(dev->usb_handle, EP_RX_IN,
                          in_buf, sizeof(in_buf), &xferred, NULL)) {
        err = GetLastError();
        if (err == ERROR_SEM_TIMEOUT) {
            /* Bos kanalda timeout normal kabul edilir */
            printf("[arc_handshake] EP0x86 IN: timeout (bos kanal, devam)\n");
            return 1;
        }
        fprintf(stderr, "[HATA] arc_handshake ReadPipe EP0x86: %lu\n", err);
        return 0;
    }

    printf("[arc_handshake] EP0x86 IN: %lu byte alindi:", xferred);
    for (i = 0; i < xferred; i++) printf(" %02X", in_buf[i]);
    printf("\n");

    return 1;
}

/* =======================================================================
 * arc_cmd04
 *   Orijinal CC surucusunun session basinda gonderdigi "bozuk-sifirla /
 *   oturumu baslat" komutu. Protokol notu UPDATE 4'ten:
 *     OUT (EP 0x01): 04 00          (2 byte)
 *     IN  (EP 0x81): 04 00 00 00   (4 byte)
 *   Basarada 1, hata durumunda 0 döner.
 * ===================================================================== */
static int arc_cmd04(ARC_DEVICE *dev)
{
    BYTE  cmd[2]               = { OPCODE_CMD04, 0x00 };
    BYTE  resp[EP_EVT_MAXPACKET];
    ULONG xferred;
    DWORD i;

    printf("[arc_cmd04] Gönderilen: 04 00\n");

    if (!write_cmd(dev, cmd, sizeof(cmd))) return 0;

    memset(resp, 0, sizeof(resp));
    xferred = read_response(dev, OPCODE_CMD04, resp, sizeof(resp), BUDGET_SHORT_MS);
    if (xferred == 0) return 0;

    printf("[arc_cmd04] Cevap (%lu byte):", xferred);
    for (i = 0; i < xferred; i++) printf(" %02X", resp[i]);
    printf("\n");

    /* Beklenen: 04 00 00 00 */
    if (xferred < 4 || resp[1] != 0x00 || resp[2] != 0x00 || resp[3] != 0x00) {
        fprintf(stderr, "[UYARI] arc_cmd04 cevap beklentiden farkli"
                " (devam ediyoruz)\n");
    }
    return 1;
}

/* =======================================================================
 * arc_init
 *   COM20020 chipi yapilandir.
 *   OUT (EP 0x01): 12 byte = opcode(00 00) + COM20020_CONFIG(10B)
 *   IN  (EP 0x81):  6 byte = 00 00 00 00 [status] 00
 *     status=0x22 goruldu; sifir-olmayan status'u hata saymiyoruz
 *     (UPDATE 4: "treat non-error as success").
 *   Basarada 1, hata durumunda 0 döner.
 * ===================================================================== */
static int arc_init(ARC_DEVICE *dev,
                    BYTE nodeID, BYTE timeout,
                    BYTE clockPrescaler, BOOL recvBroadcasts)
{
    BYTE  cmd[12];
    BYTE  resp[EP_EVT_MAXPACKET];
    ULONG xferred;
    DWORD i;

    /*
     * COM20020_CONFIG elle dolduruldu (pack(1) yerine; derleyiciden bagimsiz):
     * [0] opcode 0x00  [1] 0x00  [2-3] BaseIO=0x0000 LE
     * [4] IRQ=0  [5] Timeout  [6] NodeID  [7] 128NAKs=1
     * [8] RcvAll=0  [9] ClockPrescaler  [10] SlowArb  [11] RcvBcast
     */
    cmd[0]  = OPCODE_INIT;
    cmd[1]  = 0x00;
    cmd[2]  = 0x00;  cmd[3]  = 0x00;   /* BaseIOAddress LE */
    cmd[4]  = 0x00;                     /* InterruptLevel  */
    cmd[5]  = timeout;
    cmd[6]  = nodeID;
    cmd[7]  = 0x01;                     /* 128NAKs = TRUE  */
    cmd[8]  = 0x00;                     /* ReceiveAll = FALSE */
    cmd[9]  = clockPrescaler;
    cmd[10] = (clockPrescaler > 5u) ? 0x01u : 0x00u;  /* SlowArbitration */
    cmd[11] = recvBroadcasts ? 0x01u : 0x00u;

    printf("[arc_init] Gönderilen (%lu byte):", (ULONG)sizeof(cmd));
    for (i = 0; i < sizeof(cmd); i++) printf(" %02X", cmd[i]);
    printf("\n");
    printf("[arc_init] Talk kaydi:              00 00 00 00 00 18 01 01 00 00 00 01\n");

    if (!write_cmd(dev, cmd, sizeof(cmd))) return 0;

    memset(resp, 0, sizeof(resp));
    xferred = read_response(dev, OPCODE_INIT, resp, sizeof(resp), BUDGET_INIT_MS);
    if (xferred == 0) {
        /* UPDATE 4 oncesi davranis: Init tek yönlü görünüyordu.
         * Artik cevap bekleniyor; yoksa gercek bir sorun var. */
        fprintf(stderr, "[HATA] arc_init: cevap alinamadi.\n");
        return 0;
    }

    printf("[arc_init] Cevap (%lu byte):", xferred);
    for (i = 0; i < xferred; i++) printf(" %02X", resp[i]);
    printf("\n");

    /*
     * Cevap formati (UPDATE 4): 00 00 00 00 [status] 00
     * resp[4] = init status bayt.
     * 0x22 gözlemlendi -> OK. Sifir-olmayan degerleri hata saymiyoruz.
     */
    if (xferred >= 6)
        printf("[arc_init] Status bayt = 0x%02X%s\n",
               resp[4], resp[4] == 0x00 ? " (sifir)" : " (sifir-olmayan, OK)");

    return 1;
}

/* =======================================================================
 * arc_register
 *   COM20020 chip registerini oku (bWrite=FALSE) veya yaz (bWrite=TRUE).
 *   OUT (EP 0x01): 01 00 [bWrite] [reg] [val_or_00]   (5 byte)
 *   IN  (EP 0x81): 01 00 00 00 [bWrite] [reg] [val]   (7 byte)
 *   Basarada 1, hata durumunda 0 döner.
 * ===================================================================== */
static int arc_register(ARC_DEVICE *dev, BOOL bWrite, BYTE reg, BYTE *value)
{
    BYTE  cmd[5];
    BYTE  resp[EP_EVT_MAXPACKET];
    ULONG xferred;

    cmd[0] = OPCODE_REGISTER;
    cmd[1] = 0x00;
    cmd[2] = bWrite ? 0x01u : 0x00u;
    cmd[3] = reg;
    cmd[4] = bWrite ? *value : 0x00u;

    if (!write_cmd(dev, cmd, sizeof(cmd))) return 0;

    memset(resp, 0, sizeof(resp));
    xferred = read_response(dev, OPCODE_REGISTER, resp, sizeof(resp), BUDGET_SHORT_MS);
    if (xferred == 0) return 0;

    /* read_response opcode eşlemesini garantiliyor; sadece boyut ve echo kontrol et */
    if (xferred < 7) {
        fprintf(stderr, "[HATA] arc_register cevap cok kisa: %lu byte\n", xferred);
        return 0;
    }
    /* byte[4]=bWrite echo, byte[5]=reg echo */
    if (resp[4] != cmd[2] || resp[5] != reg) {
        fprintf(stderr,
            "[HATA] arc_register echo uyumsuz:"
            " bWrite_echo=0x%02X reg_echo=0x%02X (gönderilen 0x%02X 0x%02X)\n",
            resp[4], resp[5], cmd[2], reg);
        return 0;
    }

    if (!bWrite)
        *value = resp[6];   /* byte[6] = okunan deger */

    return 1;
}

/* =======================================================================
 * arc_transmit
 *   ARCNET paketi gönder (EP 0x02 OUT, bulk).
 *   Protokol notu UPDATE 2+3 formati:
 *     byte0   = hedef node ID
 *     byte1   = ARCNET count = (256 - L) & 0xFF
 *     byte2.. = veri (L bayt)
 *
 *   len: 1..252 (su an icin; COM20020 max 508 bayt ama 252'yi asmayin).
 *   Basarada 1, hata durumunda 0 döner.
 * ===================================================================== */
static int arc_transmit(ARC_DEVICE *dev,
                         BYTE destNode, const BYTE *data, int len)
{
    BYTE  buf[254];   /* 2 header + 252 max veri */
    ULONG xferred;
    int   i;

    if (len < 1 || len > 252) {
        fprintf(stderr, "[HATA] arc_transmit: len=%d gecersiz (1..252 olmali)\n",
                len);
        return 0;
    }

    /* Cerceve kur */
    buf[0] = destNode;
    buf[1] = (BYTE)((256 - len) & 0xFF);  /* ARCNET count bayt */
    memcpy(buf + 2, data, (size_t)len);

    /* Dogrulama icin ham baytlari yazdir */
    printf("[arc_transmit] EP0x02 OUT (%d byte):", len + 2);
    for (i = 0; i < len + 2; i++) printf(" %02X", buf[i]);
    printf("\n");
    printf("[arc_transmit] Hedef=0x%02X  count=0x%02X  veri=\"%.*s\"\n",
           buf[0], buf[1], len, (const char *)data);

    xferred = 0;
    if (!WinUsb_WritePipe(dev->usb_handle, EP_TX_OUT,
                           buf, (ULONG)(len + 2), &xferred, NULL)) {
        fprintf(stderr, "[HATA] arc_transmit WritePipe: %lu\n", GetLastError());
        return 0;
    }
    if (xferred != (ULONG)(len + 2)) {
        fprintf(stderr, "[HATA] arc_transmit: %lu/%d byte gönderildi\n",
                xferred, len + 2);
        return 0;
    }

    printf("[arc_transmit] OK — %lu byte EP0x02'ye yazildi.\n", xferred);
    return 1;
}

/* =======================================================================
 * arc_receive
 *   ARCNET paketi al (poll mekanizmasi, UPDATE 3 + UPDATE 5).
 *
 *   Protokol:
 *     1) EP 0x02 OUT'a 10 bayt sifir yaz  ("bende paket var mi?" sorgusu).
 *     2) EP 0x86 IN'den yanit oku.
 *     3) Parse: byte0=src, byte1=dst, byte2=count, byte3..=veri
 *        L = (256 - count) & 0xFF
 *
 *   Donus degeri:
 *      1  : Paket alindi; out_* alanlari dolduruldu.
 *      0  : Paket yok (bos/sifir yanit veya timeout).
 *     -1  : Donanim/IO hatasi.
 *
 *   Parametreler:
 *     out_src  : kaynak node ID
 *     out_dst  : hedef node ID
 *     out_data : veri tamponu (en az 256 bayt)
 *     out_len  : alinan veri uzunlugu
 * ===================================================================== */
static int arc_receive(ARC_DEVICE *dev,
                       BYTE *out_src, BYTE *out_dst,
                       BYTE *out_data, int *out_len)
{
    BYTE  poll[10];
    BYTE  buf[512];   /* EP 0x86 MaxPacketSize=512 (probe ciktisi) */
    ULONG xferred;
    DWORD err;
    int   L;

    /* ---- Poll: 10 sifir bayt -> EP 0x02 OUT ---- */
    memset(poll, 0x00, sizeof(poll));
    xferred = 0;
    if (!WinUsb_WritePipe(dev->usb_handle, EP_TX_OUT,
                           poll, sizeof(poll), &xferred, NULL)) {
        fprintf(stderr, "[HATA] arc_receive WritePipe EP0x02: %lu\n",
                GetLastError());
        return -1;
    }

    /* ---- EP 0x86 IN'den yanit oku ---- */
    memset(buf, 0, sizeof(buf));
    xferred = 0;
    if (!WinUsb_ReadPipe(dev->usb_handle, EP_RX_IN,
                          buf, sizeof(buf), &xferred, NULL)) {
        err = GetLastError();
        if (err == ERROR_SEM_TIMEOUT)
            return 0;   /* kanal bos, timeout normal */
        fprintf(stderr, "[HATA] arc_receive ReadPipe EP0x86: %lu\n", err);
        return -1;
    }

    /*
     * Bos yanit kontrolu:
     *   - xferred < 3     : header bile yok
     *   - buf[0]==0 && buf[1]==0 : src ve dst sifir -> bos poll yankisi
     */
    if (xferred < 3 || (buf[0] == 0 && buf[1] == 0))
        return 0;

    /* ---- Parse ---- */
    *out_src = buf[0];
    *out_dst = buf[1];
    L = (256 - (int)buf[2]) & 0xFF;

    /* Anlamsiz L degeri ya da tampon tasma kontrolu */
    if (L == 0 || (int)xferred < L + 3)
        return 0;

    *out_len = L;
    memcpy(out_data, buf + 3, (size_t)L);
    return 1;
}

/* =======================================================================
 * main — RECEIVE modu: 30 sn dinle, gelen paketi yazdir.
 * ===================================================================== */
int main(void)
{
    ARC_DEVICE  dev;
    int         ok;
    ULONGLONG   loop_start;
    int         result;
    BYTE        src, dst;
    BYTE        data[256];
    int         data_len;
    int         pkt_count = 0;
    ULONG       rx_timeout = 200u;   /* EP 0x86 per-okuma zaman asimi (ms) */
    int         i;

    printf("==============================================\n");
    printf("  USB22-485 ARCNET IO Testi — RECEIVE modu\n");
    printf("  VID=0x%04X  PID=0x%04X\n", CC_VID, CC_PID);
    printf("==============================================\n\n");

    if (!arc_open(&dev)) return 1;

    /* ------------------------------------------------------------------
     * Adim 1-3: Baslangic zinciri (degismedi)
     * ------------------------------------------------------------------ */
    printf("--- arc_cmd04 ---\n");
    ok = arc_cmd04(&dev);
    printf("[arc_cmd04] %s\n\n", ok ? "OK" : "HATA");
    if (!ok) goto done;

    printf("--- arc_handshake ---\n");
    ok = arc_handshake(&dev);
    printf("[arc_handshake] %s\n\n", ok ? "OK" : "HATA");
    if (!ok) goto done;

    printf("--- arc_init ---\n");
    ok = arc_init(&dev, /*nodeID=*/1, /*timeout=*/0x18,
                        /*clockPrescaler=*/0x00, /*recvBroadcasts=*/TRUE);
    printf("[arc_init] %s\n\n", ok ? "OK" : "HATA");
    if (!ok) goto done;

    /* ------------------------------------------------------------------
     * EP 0x86 icin okuma baskina zaman asimini ayarla.
     * arc_handshake 500ms koymustu; receive dongusu icin 200ms daha uygun.
     * ------------------------------------------------------------------ */
    if (!WinUsb_SetPipePolicy(dev.usb_handle, EP_RX_IN,
                               PIPE_TRANSFER_TIMEOUT,
                               sizeof(rx_timeout), &rx_timeout))
        fprintf(stderr, "[UYARI] SetPipePolicy EP0x86: %lu\n", GetLastError());

    /* ------------------------------------------------------------------
     * Adim 4: Receive dongusu — 30 saniye boyunca dinle.
     * Her iterasyonda arc_receive bir poll + okuma yapar (~200ms bekler).
     * ------------------------------------------------------------------ */
    printf("--- Receive dongusu (30 sn) ---\n");
    printf("Diger node'dan paket bekleniyor... (Ctrl+C ile iptal)\n\n");

    loop_start = GetTickCount64();

    while ((GetTickCount64() - loop_start) < 30000ULL) {

        memset(data, 0, sizeof(data));
        data_len = 0;
        result = arc_receive(&dev, &src, &dst, data, &data_len);

        if (result == 1) {
            /* ---- Paket alindi ---- */
            pkt_count++;
            printf("=== Paket #%d alindi ===\n", pkt_count);
            printf("  Kaynak node : %u (0x%02X)\n", src, src);
            printf("  Hedef node  : %u (0x%02X)\n", dst, dst);
            printf("  Veri uzunl. : %d bayt\n",      data_len);

            /* Hex dump */
            printf("  Hex         :");
            for (i = 0; i < data_len; i++) printf(" %02X", data[i]);
            printf("\n");

            /* ASCII (yazdirilabilir karakterler; digerleri '.') */
            printf("  ASCII       : \"");
            for (i = 0; i < data_len; i++)
                putchar((data[i] >= 0x20 && data[i] < 0x7F) ? data[i] : '.');
            printf("\"\n\n");

            /* Ilk paketten sonra cik */
            break;

        } else if (result == -1) {
            /* Donanim hatasi — dongu kirilsin */
            fprintf(stderr, "[HATA] arc_receive hatasi, duruluyor.\n");
            ok = 0;
            goto done;
        }
        /* result == 0: paket yok, sessizce devam */
    }

    if (pkt_count == 0)
        printf("30 saniye doldu, paket alinamadi.\n");

done:
    printf("\n");
    arc_close(&dev);
    return (ok && pkt_count > 0) ? 0 : 1;
}
