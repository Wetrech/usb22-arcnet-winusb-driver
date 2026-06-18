/*
 * arcnet_scan - Adım 1: Tüm USB cihazlarını listele, VID=0x0D0B olanları vurgula.
 *
 * Derleme zincirini ve cihaz görünürlüğünü doğrulamak için yazılmıştır.
 * WinUSB bağlantısı YOK; cihaz açılmıyor.
 *
 * Kullanılan API'ler:
 *   SetupDiGetClassDevsA  - cihaz kümesi handle'ı aç
 *   SetupDiEnumDeviceInfo - kümedeki cihazları sırayla dolaş
 *   SetupDiGetDeviceRegistryPropertyA - donanım kimliği / açıklama oku
 *   SetupDiDestroyDeviceInfoList      - handle kapat
 */

#include <windows.h>
#include <setupapi.h>   /* SetupDi* fonksiyonları          */
#include <cfgmgr32.h>   /* CM_* sabitleri (ileride lazım)  */
#include <stdio.h>
#include <string.h>

/* Contemporary Controls Systems, Inc. Vendor ID */
#define CC_VID  0x0D0B

/* -----------------------------------------------------------------------
 * parse_vid_pid
 *   Donanım ID dizisinden (örn. "USB\VID_0D0B&PID_B002\5&...") VID ve
 *   PID değerlerini ayrıştırır.
 *   Başarıda 1, başarısızlıkta 0 döner.
 * --------------------------------------------------------------------- */
static int parse_vid_pid(const char *hw_id,
                         unsigned int *out_vid,
                         unsigned int *out_pid)
{
    const char *p;

    p = strstr(hw_id, "VID_");
    if (!p) return 0;
    if (sscanf(p + 4, "%4X", out_vid) != 1) return 0;

    p = strstr(hw_id, "PID_");
    if (!p) return 0;
    if (sscanf(p + 4, "%4X", out_pid) != 1) return 0;

    return 1;
}

/* -----------------------------------------------------------------------
 * cc_pid_description
 *   VID=0x0D0B olan cihazlar için PID'e göre kullanıcı dostu isim döner.
 *   .inf dosyasındaki [Strings] bölümünden alınmıştır.
 *
 *   Bootloader aşaması : PID B001-B010
 *   Firmware aşaması   : PID 1002, 1003
 * --------------------------------------------------------------------- */
static const char *cc_pid_description(unsigned int pid)
{
    switch (pid) {
    /* ----- Bootloader (firmware henüz yüklenmemiş) ----- */
    case 0xB001: return "Bootloader - Backplane DC EIA-485";
    case 0xB002: return "Bootloader - DC EIA-485 (USB22-485)";
    case 0xB003: return "Bootloader - AC EIA-485";
    case 0xB004: return "Bootloader - Coaxial Bus";
    case 0xB005: return "Bootloader - Coaxial Bus";
    case 0xB006: return "Bootloader - SMA Fiber";
    case 0xB007: return "Bootloader - ST Fiber";
    case 0xB008: return "Bootloader - Twisted Pair";
    case 0xB009: return "Bootloader - Backplane 5Mbps DC EIA-485";
    case 0xB00A: return "Bootloader - 5Mbps DC EIA-485";
    case 0xB00B: return "Bootloader - 5Mbps AC EIA-485";
    case 0xB00C: return "Bootloader - 5Mbps ST Fiber";
    case 0xB00D: return "Bootloader - 5Mbps SMA Fiber";
    case 0xB00E: return "Bootloader - Backplane AC EIA-485";
    case 0xB00F: return "Bootloader - Raymarine HSB2";
    case 0xB010: return "Bootloader - Control Techniques CTNet";
    /* ----- Firmware yüklü (operasyonel) ----- */
    case 0x1002: return "*** FIRMWARE YUKLU - Backplane ARCNET Adapter ***";
    case 0x1003: return "*** FIRMWARE YUKLU - USB2.0 ARCNET Adapter (USB22-485) ***";
    default:     return "(bilinmeyen PID)";
    }
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void)
{
    HDEVINFO         dev_info;
    SP_DEVINFO_DATA  dev_data;
    DWORD            idx;
    char             hw_id[512];   /* REG_MULTI_SZ: birden fazla ID satırı */
    char             desc[256];
    unsigned int     vid, pid;
    int              total        = 0;
    int              cc_count     = 0;

    printf("============================================================\n");
    printf("  ARCNET USB Tarayici - Adim 1: Cihaz Listesi\n");
    printf("============================================================\n\n");

    /*
     * "USB" enumerator'ü altındaki TÜM cihazları iste.
     * DIGCF_ALLCLASSES: sınıf filtresi yok, hepsi gelsin.
     * DIGCF_PRESENT   : sadece şu an takılı olanlar.
     */
    dev_info = SetupDiGetClassDevsA(NULL, "USB", NULL,
                                    DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (dev_info == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[HATA] SetupDiGetClassDevs basarisiz: %lu\n",
                GetLastError());
        return 1;
    }

    dev_data.cbSize = sizeof(dev_data);

    for (idx = 0; SetupDiEnumDeviceInfo(dev_info, idx, &dev_data); idx++) {

        /* Donanım kimliği al (REG_MULTI_SZ - ilk null'a kadar oku) */
        if (!SetupDiGetDeviceRegistryPropertyA(
                dev_info, &dev_data, SPDRP_HARDWAREID,
                NULL, (PBYTE)hw_id, sizeof(hw_id) - 1, NULL)) {
            continue; /* Sürücüsüz cihazlarda bazen boş olabilir */
        }
        hw_id[sizeof(hw_id) - 1] = '\0';

        /* VID/PID ayrıştır */
        if (!parse_vid_pid(hw_id, &vid, &pid))
            continue;

        /* Cihaz açıklaması (SPDRP_DEVICEDESC) */
        if (!SetupDiGetDeviceRegistryPropertyA(
                dev_info, &dev_data, SPDRP_DEVICEDESC,
                NULL, (PBYTE)desc, sizeof(desc) - 1, NULL)) {
            strcpy(desc, "(aciklama yok)");
        }
        desc[sizeof(desc) - 1] = '\0';

        total++;

        if (vid == CC_VID) {
            /* ---- Contemporary Controls cihazı bulundu ---- */
            printf(">>> CC ARCNET cihazi bulundu: VID=0x%04X  PID=0x%04X\n",
                   vid, pid);
            printf("    Windows aciklamasi : %s\n", desc);
            printf("    Model bilgisi      : %s\n",
                   cc_pid_description(pid));
            printf("    Ham HW ID          : %.80s\n", hw_id);
            printf("\n");
            cc_count++;
        } else {
            printf("  VID=0x%04X  PID=0x%04X  %s\n", vid, pid, desc);
        }
    }

    /* ERROR_NO_MORE_ITEMS (0x103) dışındaki hatalar gerçek sorun */
    if (GetLastError() != ERROR_NO_MORE_ITEMS) {
        fprintf(stderr, "[UYARI] Enumeration erken bitti: %lu\n",
                GetLastError());
    }

    SetupDiDestroyDeviceInfoList(dev_info);

    printf("\n============================================================\n");
    printf("  Toplam USB cihaz    : %d\n", total);
    printf("  CC (VID=0x0D0B)     : %d\n", cc_count);
    if (cc_count == 0)
        printf("  [!] CC cihazi BULUNAMADI - cihazin takili oldugunu kontrol edin.\n");
    printf("============================================================\n");

    return 0;
}
