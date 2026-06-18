/*
 * winusb_probe.c  —  Adım 3: WinUSB ile USB22-485'i aç, endpoint'leri doğrula.
 *
 * Ön koşul: VID=0x0D0B / PID=0x1002 cihazına Zadig ile WinUSB bind edilmiş olmalı.
 *
 * Bu dosyanın yaptığı:
 *   1. SetupAPI + GUID_DEVINTERFACE_USB_DEVICE ile cihaz yolunu bul
 *   2. CreateFile + WinUsb_Initialize ile aç
 *   3. Device descriptor + interface descriptor + TÜM pipe bilgilerini yazdır
 *   4. Beklenen 4 endpoint'in (0x01/0x81/0x02/0x86) varlığını ve bulk tipini doğrula
 *
 * Veri gönderilmiyor / alınmıyor; protokol fonksiyonları yok.
 * Amaç: "WinUSB bind çalışıyor + endpoint'ler doğru" kanıtı.
 *
 * Endpoint'ler (reference/usb22-protocol-notes.md'den):
 *   0x01 OUT Bulk  — komut kanalı (Init opcode 0x00, Register opcode 0x01, …)
 *   0x81 IN  Bulk  — cevap + async event (opcode 0x20 status mesajları)
 *   0x02 OUT Bulk  — ARCNET transmit paketi (dst + 256-L + data)
 *   0x86 IN  Bulk  — ARCNET receive paketi  (src + dst + 256-L + data)
 */

#include <windows.h>
#include <setupapi.h>
#include <winusb.h>     /* WinUsb_*, WINUSB_PIPE_INFORMATION, USB_*_DESCRIPTOR */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ---- Hedef cihaz ---- */
#define CC_VID     0x0D0Bu
#define CC_PID     0x1002u

/* ---- Beklenen endpoint adresleri ---- */
#define EP_CMD_OUT 0x01u
#define EP_EVT_IN  0x81u
#define EP_TX_OUT  0x02u
#define EP_RX_IN   0x86u

/*
 * GUID_DEVINTERFACE_USB_DEVICE = {A5DCBF10-6530-11D2-901F-00C04FB951ED}
 *
 * Zadig'in oluşturduğu WinUSB INF, bu stanrdart USB-device arayüz GUID'ini
 * DeviceInterfaceGUIDs değeri olarak yazar.
 *
 * Cihaz bulunamazsa, Zadig'in gerçekte hangi GUID'i yazdığını kontrol et:
 *   regedit →
 *   HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_0D0B&PID_1002\<instance>
 *     \Device Parameters\DeviceInterfaceGUIDs
 */
static const GUID GUID_USB_DEVICE = {
    0xA5DCBF10u, 0x6530u, 0x11D2u,
    { 0x90u, 0x1Fu, 0x00u, 0xC0u, 0x4Fu, 0xB9u, 0x51u, 0xEDu }
};

/* -----------------------------------------------------------------------
 * find_device_path
 *   GUID_USB_DEVICE arayüz kümesini tarar; cihaz yolunda "VID_XXXX&PID_YYYY"
 *   geçen ilk cihazı bulur, yolunu path_buf'a kopyalar.
 *   Başarıda 1, bulunamadığında veya hata durumunda 0 döner.
 * --------------------------------------------------------------------- */
static int find_device_path(unsigned int vid, unsigned int pid,
                             char *path_buf, DWORD buf_size)
{
    HDEVINFO                           devs;
    SP_DEVICE_INTERFACE_DATA           iface_data;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail;
    DWORD                              idx, needed;
    char                               token[32];  /* "VID_0D0B&PID_1002" */
    char                               upper[512]; /* büyük harf kopyası  */
    char                              *p;
    int                                found = 0;

    /* Büyük harfli arama tokeni — Windows yolları büyük harf kullanır */
    snprintf(token, sizeof(token), "VID_%04X&PID_%04X", vid, pid);

    devs = SetupDiGetClassDevsA(&GUID_USB_DEVICE, NULL, NULL,
                                 DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[HATA] SetupDiGetClassDevs: %lu\n", GetLastError());
        return 0;
    }

    iface_data.cbSize = sizeof(iface_data);

    for (idx = 0;
         SetupDiEnumDeviceInterfaces(devs, NULL, &GUID_USB_DEVICE,
                                     idx, &iface_data);
         idx++)
    {
        /* 1) Kaç byte gerekeceğini öğren (beklenen hata ERROR_INSUFFICIENT_BUFFER) */
        SetupDiGetDeviceInterfaceDetailA(devs, &iface_data,
                                          NULL, 0, &needed, NULL);

        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(needed);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A); /* sabit kısım */

        /* 2) Gerçek yolu al */
        if (!SetupDiGetDeviceInterfaceDetailA(devs, &iface_data, detail,
                                               needed, NULL, NULL)) {
            free(detail);
            continue;
        }

        /* 3) Yolun büyük harfli kopyasında token'ı ara */
        strncpy(upper, detail->DevicePath, sizeof(upper) - 1);
        upper[sizeof(upper) - 1] = '\0';
        for (p = upper; *p; p++)
            *p = (char)toupper((unsigned char)*p);

        if (strstr(upper, token)) {
            strncpy(path_buf, detail->DevicePath, buf_size - 1);
            path_buf[buf_size - 1] = '\0';
            found = 1;
            free(detail);
            break;
        }

        free(detail);
    }

    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

/* -----------------------------------------------------------------------
 * pipe_type_str  —  USBD_PIPE_TYPE enum'unu yazdırılabilir stringe çevirir
 * --------------------------------------------------------------------- */
static const char *pipe_type_str(USBD_PIPE_TYPE t)
{
    switch (t) {
    case UsbdPipeTypeControl:     return "Control";
    case UsbdPipeTypeIsochronous: return "Isochronous";
    case UsbdPipeTypeBulk:        return "Bulk";
    case UsbdPipeTypeInterrupt:   return "Interrupt";
    default:                      return "?";
    }
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void)
{
    char                     dev_path[512];
    HANDLE                   dev_handle = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE  usb_handle = NULL;
    USB_DEVICE_DESCRIPTOR    dev_desc;
    USB_INTERFACE_DESCRIPTOR iface_desc;
    WINUSB_PIPE_INFORMATION  pipe;
    ULONG                    xferred;
    BYTE                     pi;          /* pipe index */
    int                      ep_found[4]; /* 0x01, 0x81, 0x02, 0x86 */
    int                      i, all_ok;
    int                      ret = 1;

    static const BYTE        exp_ep[4]   = { EP_CMD_OUT, EP_EVT_IN,
                                              EP_TX_OUT,  EP_RX_IN };
    static const char *const exp_name[4] = { "0x01 CMD-OUT", "0x81 EVT-IN",
                                              "0x02 TX-OUT",  "0x86 RX-IN" };

    memset(ep_found, 0, sizeof(ep_found));

    printf("==============================================\n");
    printf("  USB22-485 WinUSB Probe\n");
    printf("  VID=0x%04X  PID=0x%04X\n", CC_VID, CC_PID);
    printf("==============================================\n\n");

    /* ------------------------------------------------------------------ */
    /* [1] Cihaz yolunu bul                                               */
    /* ------------------------------------------------------------------ */
    printf("[1] Cihaz yolu aranıyor...\n");

    if (!find_device_path(CC_VID, CC_PID, dev_path, sizeof(dev_path))) {
        fprintf(stderr,
            "\n[HATA] Cihaz bulunamadi (VID=0x%04X / PID=0x%04X).\n"
            "\n"
            "  Olasi nedenler ve cozumler:\n"
            "  1) Cihaz takili degil      -> takip tekrar calistir.\n"
            "  2) WinUSB bind yapilmamis  -> once Zadig'i calistir.\n"
            "  3) Zadig farkli GUID kullanmis -> su registry yoluna bak:\n"
            "     HKLM\\SYSTEM\\CurrentControlSet\\Enum\\USB\\\n"
            "     VID_%04X&PID_%04X\\<instance>\\Device Parameters\\\n"
            "     DeviceInterfaceGUIDs\n\n",
            CC_VID, CC_PID, CC_VID, CC_PID);
        return 1;
    }

    printf("    Yol: %s\n\n", dev_path);

    /* ------------------------------------------------------------------ */
    /* [2] CreateFile ile cihazı aç                                       */
    /* ------------------------------------------------------------------ */
    printf("[2] CreateFile...\n");
    dev_handle = CreateFileA(
        dev_path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,   /* WinUSB: overlapped I/O zorunlu */
        NULL);

    if (dev_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[HATA] CreateFile: %lu\n"
                "  (Yonetici yetkisiyle calistirmak gerekebilir)\n",
                GetLastError());
        goto cleanup;
    }
    printf("    OK\n\n");

    /* ------------------------------------------------------------------ */
    /* [3] WinUsb_Initialize                                               */
    /* ------------------------------------------------------------------ */
    printf("[3] WinUsb_Initialize...\n");
    if (!WinUsb_Initialize(dev_handle, &usb_handle)) {
        fprintf(stderr, "[HATA] WinUsb_Initialize: %lu\n", GetLastError());
        goto cleanup;
    }
    printf("    OK\n\n");

    /* ------------------------------------------------------------------ */
    /* [4] Device Descriptor                                               */
    /* ------------------------------------------------------------------ */
    printf("[4] Device Descriptor:\n");
    xferred = 0;
    if (!WinUsb_GetDescriptor(usb_handle,
                               USB_DEVICE_DESCRIPTOR_TYPE,
                               0, 0,    /* index=0, languageID=0 */
                               (PUCHAR)&dev_desc, sizeof(dev_desc),
                               &xferred)) {
        fprintf(stderr, "    [UYARI] GetDescriptor: %lu\n", GetLastError());
    } else {
        printf("    bcdUSB         = %04X  (USB sürümü)\n", dev_desc.bcdUSB);
        printf("    idVendor       = %04X\n",                dev_desc.idVendor);
        printf("    idProduct      = %04X\n",                dev_desc.idProduct);
        printf("    bcdDevice      = %04X  (firmware sürümü)\n", dev_desc.bcdDevice);
        printf("    bNumConfigs    = %u\n",                  dev_desc.bNumConfigurations);
    }
    printf("\n");

    /* ------------------------------------------------------------------ */
    /* [5] Interface Descriptor  (AlternateSetting 0)                     */
    /* ------------------------------------------------------------------ */
    printf("[5] Interface Descriptor (alt-setting 0):\n");
    if (!WinUsb_QueryInterfaceSettings(usb_handle, 0, &iface_desc)) {
        fprintf(stderr, "[HATA] QueryInterfaceSettings: %lu\n", GetLastError());
        goto cleanup;
    }
    printf("    bInterfaceNumber  = %u\n",     iface_desc.bInterfaceNumber);
    printf("    bAlternateSetting = %u\n",     iface_desc.bAlternateSetting);
    printf("    bNumEndpoints     = %u\n",     iface_desc.bNumEndpoints);
    printf("    bInterfaceClass   = 0x%02X\n", iface_desc.bInterfaceClass);
    printf("\n");

    /* ------------------------------------------------------------------ */
    /* [6] Pipe (endpoint) listesi                                         */
    /* ------------------------------------------------------------------ */
    printf("[6] Pipe listesi:\n");
    printf("    %-5s  %-8s  %-14s  %s\n",
           "Idx", "EP Adr", "Tip", "MaxPacket");
    printf("    -----  --------  --------------  ---------\n");

    for (pi = 0; pi < iface_desc.bNumEndpoints; pi++) {
        if (!WinUsb_QueryPipe(usb_handle, 0, pi, &pipe)) {
            fprintf(stderr, "    [UYARI] QueryPipe(%u): %lu\n",
                    pi, GetLastError());
            continue;
        }
        printf("    [%u]    0x%02X      %-14s  %u\n",
               pi,
               pipe.PipeId,
               pipe_type_str(pipe.PipeType),
               pipe.MaximumPacketSize);

        /* Beklenen EP mi? İşaretle; tip kontrolü de yap. */
        for (i = 0; i < 4; i++) {
            if (pipe.PipeId == exp_ep[i]) {
                ep_found[i] = 1;
                if (pipe.PipeType != UsbdPipeTypeBulk)
                    printf("      [UYARI] %s BULK bekleniyor, %s bulundu!\n",
                           exp_name[i], pipe_type_str(pipe.PipeType));
                break;
            }
        }
    }
    printf("\n");

    /* ------------------------------------------------------------------ */
    /* [7] Doğrulama raporu                                               */
    /* ------------------------------------------------------------------ */
    printf("[7] Endpoint dogrulama:\n");
    all_ok = 1;
    for (i = 0; i < 4; i++) {
        printf("    %-14s  %s\n",
               exp_name[i],
               ep_found[i] ? "OK" : "*** EKSIK ***");
        if (!ep_found[i]) all_ok = 0;
    }
    printf("\n");

    if (all_ok) {
        printf("  >> Tum endpoint'ler mevcut. WinUSB bind tamam.\n");
        printf("  >> Sonraki adim: Init + Register komutlarini gonder.\n");
        ret = 0;
    } else {
        printf("  >> Eksik endpoint var — bind durumunu kontrol edin.\n");
    }

cleanup:
    if (usb_handle)                         WinUsb_Free(usb_handle);
    if (dev_handle != INVALID_HANDLE_VALUE) CloseHandle(dev_handle);
    printf("\n");
    return ret;
}
