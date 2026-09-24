/*
 * mi_sideload_gui.c  --  Official Xiaomi OTA sideload flasher (native Win32).
 *
 * Flashes an OFFICIAL, Xiaomi-signed OTA .zip onto a phone that is in
 * MiAssistant / sideload mode. No bootloader unlock, no signature bypass:
 * the recovery still verifies the package. We only fetch the per-package
 * "Validate" token from Xiaomi's own OTA server and speak the ADB
 * sideload-host protocol directly.
 *
 * Dependencies: NONE beyond stock Windows system DLLs.
 *   USB    -> WinUSB   (winusb.dll / setupapi.dll)
 *   HTTP   -> WinHTTP  (winhttp.dll)
 *   AES/MD5-> CNG      (bcrypt.dll)
 *   base64 -> crypt32  (crypt32.dll)
 *   GUI    -> user32 / gdi32 / comctl32 / comdlg32
 *
 * Build (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -O2 -mwindows -o MiSideload.exe mi_sideload_gui.c \
 *       -lsetupapi -lwinusb -lwinhttp -lbcrypt -lcrypt32 -lcomctl32 -lcomdlg32 \
 *       -static -static-libgcc
 *
 * Build (MSVC):
 *   cl /O2 mi_sideload_gui.c /link setupapi.lib winusb.lib winhttp.lib \
 *       bcrypt.lib crypt32.lib comctl32.lib comdlg32.lib user32.lib gdi32.lib
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winusb.h>
#include <usbspec.h>
#include <setupapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ ADB proto */
#define A_CNXN 0x4E584E43u
#define A_OPEN 0x4E45504Fu
#define A_OKAY 0x59414B4Fu
#define A_WRTE 0x45545257u
#define A_CLSE 0x45534C43u

#define ADB_VERSION 0x01000001u
#define MAX_DATA    (1024u * 1024u)
#define CHUNK       (64u * 1024u)

static DWORD    g_usb_err = 0;       /* last USB failure code (diagnostics) */
static uint32_t g_dev_maxdata = 0;   /* maxdata the recovery reported in CNXN */

/* Android ADB WinUSB device-interface GUID (from Google's android_winusb.inf) */
DEFINE_GUID(GUID_DEVINTERFACE_ADB,
    0xF72FE0D4, 0xCBCB, 0x407D, 0x88, 0x14, 0x9E, 0xD6, 0x73, 0xD0, 0xDD, 0x6B);

/* generic "USB device" interface, present for every USB device (for diagnostics) */
DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE,
    0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);

/* ------------------------------------------------------ Xiaomi OTA validation */
static const unsigned char OTA_KEY[16] = "miuiotavalided11";
static const unsigned char OTA_IV[16]  = "0102030405060708";
#define OTA_HOST L"update.miui.com"
#define OTA_PATH L"/updates/miotaV3.php"

#define IDI_APP        1
#define APP_GITHUB_URL L"https://github.com/slfl/Random-Scripts/tree/main/MiSideload"

/* ------------------------------------------------------------- GUI messages */
#define WM_APP_LOG    (WM_APP + 1)
#define WM_APP_PROG   (WM_APP + 2)
#define WM_APP_DONE   (WM_APP + 3)
#define WM_APP_STATUS (WM_APP + 4)
#define WM_APP_RESULTS (WM_APP + 5)

/* child control ids */
#define IDC_LOG      1001
#define IDC_PROG     1009
#define IDC_FOOTER   1010
#define IDC_STATUS   1011

/* menu command ids */
#define IDM_OPEN      2001
#define IDM_INFO      2002
#define IDM_SERVER    2003
#define IDM_DOWNLOADS 2004
#define IDM_START     2005
#define IDM_ABOUT     2006
#define IDM_EXIT      2007
#define IDM_ADB_SET   2010
#define IDM_ADB_RESET 2011
#define IDM_ADB_CHECK 2012
#define IDM_ADB_KILL  2013
#define IDM_SET_WIPE  2020
#define IDM_SET_LOGS  2021
#define IDM_SET_GENTLE 2022
#define IDM_LANG_RU   2030
#define IDM_LANG_EN   2031
#define IDM_FW_MIUIER 2040
#define IDM_FW_EZBOX  2041
#define IDM_FW_SEARCH 2042
#define IDM_FW_PAUSE  2043
#define IDM_FW_STOP   2044
#define IDD_SEARCH       100
#define IDC_SEARCH_EDIT  101
#define IDD_RESULTS      110

/* ------------------------------------------------------------------- globals */
static HWND g_main, g_log, g_prog, g_status, g_footer;
static HMENU g_menu = NULL;
static char g_firmware[MAX_PATH] = {0};
static char g_adb_path[MAX_PATH] = {0};      /* custom adb.exe; empty => "adb.exe" */
static volatile LONG g_busy = 0;
static HFONT g_font = NULL, g_logfont = NULL;
static UINT  g_dpi = 96;

/* settings */
static int  g_lang = 0;                       /* 0 = RU, 1 = EN */
static int  g_wipe = 0;                        /* wipe userdata after flash */
static int  g_savelogs = 0;                    /* write session log to Logs/ */
static int  g_gentle   = 0;                    /* gentle transfer: 16 KiB blocks + delay */
static FILE *g_logfp = NULL;
static char g_launch_stamp[32] = {0};          /* DD.MM.YY-HH.MM at startup */

/* one open device */
typedef struct {
    HANDLE       file;
    WINUSB_INTERFACE_HANDLE winusb;
    UCHAR        ep_in;
    UCHAR        ep_out;
    unsigned     local_id;
} adb_dev;

/* ============================================================== localization */
enum {
    S_TITLE, S_MENU, S_ADB, S_SETTINGS,
    S_OPEN, S_INFO, S_SERVER, S_DOWNLOADS, S_START, S_ABOUT, S_EXIT,
    S_ADB_SET, S_ADB_RESET, S_ADB_CHECK, S_ADB_KILL,
    S_SET_WIPE, S_SET_LOGS, S_SET_GENTLE, S_LANG, S_LANG_RU, S_LANG_EN,
    S_FW_MENU, S_FW_MIUIER, S_FW_EZBOX, S_FW_SEARCH, S_FW_PAUSE, S_FW_STOP,
    S_FW_DLG_TITLE, S_FW_DLG_LABEL,
    S_FW_RESULTS_TITLE, S_FW_CHOOSE, S_FW_SOURCE, S_FW_REGION, S_FW_DL_BTN, S_FW_CANCEL,
    S_READY, S_NOFW, S_FW, S_ADBPATH_DEFAULT,
    S_ST_READY, S_ST_CONNECTING, S_ST_INFO, S_ST_TOKEN, S_ST_WIPE,
    S_ST_FLASHING, S_ST_DONE, S_ST_NOTCONN, S_ST_BUSY, S_ST_NODEV, S_ST_DEVREADY,
    S_L_PICKFIRST, S_L_WIPEWARN, S_L_TRANSFERDONE, S_L_ABOUT,
    S_COUNT
};

static const char *STR[S_COUNT][2] = {
/* {RU, EN} — RU literals are UTF-8 in source */
[S_TITLE]        = {"Mi OTA Sideload — только официальные прошивки", "Mi OTA Sideload — official firmware only"},
[S_MENU]         = {"Меню", "Menu"},
[S_ADB]          = {"ADB", "ADB"},
[S_SETTINGS]     = {"Настройки", "Settings"},
[S_OPEN]         = {"Открыть файл", "Open file"},
[S_INFO]         = {"Информация о телефоне", "Phone info"},
[S_SERVER]       = {"Проверить сервер", "Check server"},
[S_DOWNLOADS]    = {"Страница загрузок", "Downloads page"},
[S_START]        = {"Запустить прошивку", "Start flashing"},
[S_ABOUT]        = {"О программе", "About"},
[S_EXIT]         = {"Выход", "Exit"},
[S_ADB_SET]      = {"Указать путь до adb.exe…", "Set adb.exe path…"},
[S_ADB_RESET]    = {"Сбросить путь adb", "Reset adb path"},
[S_ADB_CHECK]    = {"Проверить adb", "Check adb"},
[S_ADB_KILL]     = {"Kill server", "Kill server"},
[S_SET_WIPE]     = {"Очищать данные после прошивки", "Wipe data after flash"},
[S_SET_LOGS]     = {"Сохранять логи", "Save logs"},
[S_SET_GENTLE]   = {"Щадящий режим передачи (16 КБ)", "Gentle transfer (16 KB)"},
[S_LANG]         = {"Язык", "Language"},
[S_LANG_RU]      = {"Русский", "Russian"},
[S_LANG_EN]      = {"English", "English"},
[S_FW_MENU]      = {"Прошивки", "Firmware"},
[S_FW_MIUIER]    = {"Открыть MIUI Roms", "Open MIUI Roms"},
[S_FW_EZBOX]     = {"Открыть MIUI Ezbox", "Open MIUI Ezbox"},
[S_FW_SEARCH]    = {"Поиск прошивки…", "Search firmware…"},
[S_FW_PAUSE]     = {"Пауза / продолжить загрузку", "Pause / resume download"},
[S_FW_STOP]      = {"Остановить загрузку", "Stop download"},
[S_FW_DLG_TITLE] = {"Поиск прошивки", "Search firmware"},
[S_FW_DLG_LABEL] = {"Модель, кодовое имя или версия (пусто = подключённый телефон):",
                    "Model, codename or version (empty = connected phone):"},
[S_FW_RESULTS_TITLE] = {"Выбор прошивки", "Choose firmware"},
[S_FW_CHOOSE]    = {"Выберите версию для загрузки:", "Choose a version to download:"},
[S_FW_SOURCE]    = {"Источник:", "Source:"},
[S_FW_REGION]    = {"Регион:", "Region:"},
[S_FW_DL_BTN]    = {"Скачать", "Download"},
[S_FW_CANCEL]    = {"Отмена", "Cancel"},
[S_READY]        = {"Mi OTA Sideload готов.", "Mi OTA Sideload ready."},
[S_NOFW]         = {"Прошивка: не выбрана", "Firmware: none"},
[S_FW]           = {"Прошивка:", "Firmware:"},
[S_ADBPATH_DEFAULT] = {"по умолчанию (adb.exe рядом с программой)", "default (adb.exe next to app)"},
[S_ST_READY]     = {"Готов.", "Ready."},
[S_ST_CONNECTING]= {"Подключение…", "Connecting…"},
[S_ST_INFO]      = {"Чтение информации…", "Reading device info…"},
[S_ST_TOKEN]     = {"Запрос токена у сервера…", "Requesting token…"},
[S_ST_WIPE]      = {"Очистка данных…", "Formatting userdata…"},
[S_ST_FLASHING]  = {"Передача прошивки…", "Flashing…"},
[S_ST_DONE]      = {"Готово — перезагрузка", "Done — rebooting"},
[S_ST_NOTCONN]   = {"Не подключено", "Not connected"},
[S_ST_BUSY]      = {"Занято (kill adb server)", "Busy (kill adb server)"},
[S_ST_NODEV]     = {"Нет устройства", "No device"},
[S_ST_DEVREADY]  = {"Устройство готово", "Device ready"},
[S_L_PICKFIRST]  = {"Сначала выберите файл прошивки (.zip).", "Select a firmware .zip first."},
[S_L_WIPEWARN]   = {"Полная очистка СОТРЁТ все данные пользователя. Продолжить?",
                    "Full wipe will ERASE all user data. Continue?"},
[S_L_TRANSFERDONE] = {"Передача завершена (100%). Телефон проверяет и устанавливает прошивку — НЕ отключай его. Установка и первая загрузка могут занять до ~10–15 минут.",
                      "Transfer complete (100%). The phone is verifying and installing — do NOT disconnect it. Install and first boot can take up to ~10–15 minutes."},
[S_L_ABOUT]      = {"Mi OTA Sideload\nОфициальная OTA-прошивка Xiaomi через режим MiAssistant.\nБез разблокировки загрузчика. Только официальные подписанные прошивки.",
                    "Mi OTA Sideload\nOfficial Xiaomi OTA flashing via MiAssistant mode.\nNo bootloader unlock. Official signed firmware only."},
};

static const char *L(int id) { return STR[id][g_lang]; }

/* UTF-8 -> wide (heap). Caller frees. */
static WCHAR *u8towide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) n = 1;
    WCHAR *w = (WCHAR *)malloc(n * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static void Lw(int id, WCHAR *buf, int cch) {
    MultiByteToWideChar(CP_UTF8, 0, L(id), -1, buf, cch);
}

/* ============================================================== logging (UI) */
static void write_logfile(const char *utf8);   /* fwd */

static void ui_log(const char *fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    write_logfile(buf);
    PostMessageA(g_main, WM_APP_LOG, 0, (LPARAM)_strdup(buf));
}
static void ui_status(const char *s) {
    PostMessageA(g_main, WM_APP_STATUS, 0, (LPARAM)_strdup(s));
}
static void ui_progress(int pct) {
    PostMessageA(g_main, WM_APP_PROG, (WPARAM)pct, 0);
}

/* ============================================================ WinUSB device */
static void dev_close(adb_dev *d) {
    if (d->winusb) { WinUsb_Free(d->winusb); d->winusb = NULL; }
    if (d->file && d->file != INVALID_HANDLE_VALUE) {
        CloseHandle(d->file); d->file = NULL;
    }
}

/* read a REG_SZ / first-string device property; returns "" on failure */
static void devprop_sz(HDEVINFO info, PSP_DEVINFO_DATA dd, DWORD prop,
                       char *out, DWORD outlen) {
    DWORD type = 0;
    out[0] = 0;
    SetupDiGetDeviceRegistryPropertyA(info, dd, prop, &type,
                                      (PBYTE)out, outlen - 1, NULL);
    out[outlen - 1] = 0;
}

/* Try one device-interface GUID: only WinUSB-backed interfaces are considered
 * (the WinUSB API cannot drive usbhub/usbccgp/libusbK nodes). Verifies the ADB
 * interface descriptor (class ff / sub 42 / proto 01) and grabs bulk endpoints.
 * Returns 1 on success. Sets *denied only for a genuinely busy WinUSB ADB node. */
static int g_verbose = 0;   /* when set, detection logs each step */

static int parse_guid(const char *s, GUID *g) {
    unsigned long d1; unsigned int d2, d3, b[8];
    if (sscanf(s, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3],
               &b[4], &b[5], &b[6], &b[7]) != 11 &&
        sscanf(s, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
               &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3],
               &b[4], &b[5], &b[6], &b[7]) != 11)
        return 0;
    g->Data1 = (DWORD)d1; g->Data2 = (WORD)d2; g->Data3 = (WORD)d3;
    for (int i = 0; i < 8; i++) g->Data4[i] = (BYTE)b[i];
    return 1;
}

/* Open one device-interface path, verify it's the ADB WinUSB interface
 * (class ff/42/01), grab bulk endpoints. Returns 1 on success. */
static int finish_open(const char *path, adb_dev *d, int *denied) {
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED) *denied = 1;
        if (g_verbose) ui_log(g_lang ? "    open failed (err %lu): %s" : "    не удалось открыть (код %lu): %s", e, path);
        return 0;
    }
    WINUSB_INTERFACE_HANDLE wu;
    if (!WinUsb_Initialize(h, &wu)) {
        if (g_verbose) ui_log(g_lang ? "    WinUsb_Initialize failed (err %lu)" : "    WinUsb_Initialize не удался (код %lu)", GetLastError());
        CloseHandle(h); return 0;
    }
    USB_INTERFACE_DESCRIPTOR id;
    if (!WinUsb_QueryInterfaceSettings(wu, 0, &id)) {
        if (g_verbose) ui_log("%s", g_lang ? "    QueryInterfaceSettings failed" : "    QueryInterfaceSettings не удался");
        WinUsb_Free(wu); CloseHandle(h); return 0;
    }
    if (id.bInterfaceClass != 0xFF || id.bInterfaceSubClass != 0x42 ||
        id.bInterfaceProtocol != 0x01) {
        if (g_verbose) ui_log(g_lang ? "    not ADB iface (cls=%u/sub=%u/proto=%u)" : "    не ADB-интерфейс (cls=%u/sub=%u/proto=%u)",
                              id.bInterfaceClass, id.bInterfaceSubClass,
                              id.bInterfaceProtocol);
        WinUsb_Free(wu); CloseHandle(h); return 0;
    }
    UCHAR in = 0, out = 0;
    for (UCHAR p = 0; p < id.bNumEndpoints; p++) {
        WINUSB_PIPE_INFORMATION pi;
        if (!WinUsb_QueryPipe(wu, 0, p, &pi)) continue;
        if (pi.PipeType != UsbdPipeTypeBulk) continue;
        if (pi.PipeId & 0x80) in = pi.PipeId; else out = pi.PipeId;
    }
    if (!in || !out) { WinUsb_Free(wu); CloseHandle(h); return 0; }
    ULONG timeout = 60000;
    WinUsb_SetPipePolicy(wu, in,  PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
    WinUsb_SetPipePolicy(wu, out, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
    UCHAR yes = 1;
    WinUsb_SetPipePolicy(wu, in,  ALLOW_PARTIAL_READS, sizeof(yes), &yes);
    WinUsb_SetPipePolicy(wu, in,  AUTO_CLEAR_STALL,    sizeof(yes), &yes);
    WinUsb_SetPipePolicy(wu, out, AUTO_CLEAR_STALL,    sizeof(yes), &yes);
    d->file = h; d->winusb = wu; d->ep_in = in; d->ep_out = out; d->local_id = 1;
    if (g_verbose) ui_log(g_lang ? "    OK: ADB interface opened (ep_in=0x%02x ep_out=0x%02x)" : "    OK: ADB-интерфейс открыт (ep_in=0x%02x ep_out=0x%02x)", in, out);
    return 1;
}

/* For a given USB device node, open its WinUSB ADB interface. First tries the
 * device-interface path itself, then every GUID registered in the device's
 * Device Parameters\DeviceInterfaceGUIDs (this is how libusb finds it). */
static int open_for_devnode(HDEVINFO info, SP_DEVINFO_DATA *dd,
                            const char *usbdev_path, adb_dev *d, int *denied) {
    if (usbdev_path && finish_open(usbdev_path, d, denied)) return 1;

    HKEY k = SetupDiOpenDevRegKey(info, dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (k == INVALID_HANDLE_VALUE) return 0;
    char guids[1024]; DWORD type = 0, len = sizeof(guids) - 2;
    memset(guids, 0, sizeof(guids));
    LONG rr = RegQueryValueExA(k, "DeviceInterfaceGUIDs", NULL, &type,
                               (LPBYTE)guids, &len);
    if (rr != ERROR_SUCCESS) {
        len = sizeof(guids) - 2;
        rr = RegQueryValueExA(k, "DeviceInterfaceGUID", NULL, &type,
                              (LPBYTE)guids, &len);
    }
    RegCloseKey(k);
    if (rr != ERROR_SUCCESS) { if (g_verbose) ui_log("%s", g_lang ? "    no DeviceInterfaceGUIDs value" : "    нет DeviceInterfaceGUIDs"); return 0; }

    for (char *gp = guids; *gp; gp += strlen(gp) + 1) {
        GUID g;
        if (!parse_guid(gp, &g)) continue;
        if (g_verbose) ui_log(g_lang ? "    trying registered GUID %s" : "    пробую зарегистрированный GUID %s", gp);
        HDEVINFO i2 = SetupDiGetClassDevs(&g, NULL, NULL,
                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (i2 == INVALID_HANDLE_VALUE) continue;
        SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof(ifd);
        int done = 0;
        for (DWORD j = 0; !done && SetupDiEnumDeviceInterfaces(i2, NULL, &g, j, &ifd); j++) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailA(i2, &ifd, NULL, 0, &need, NULL);
            if (!need) continue;
            PSP_DEVICE_INTERFACE_DETAIL_DATA_A det =
                (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(need);
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            SP_DEVINFO_DATA d2; d2.cbSize = sizeof(d2);
            if (SetupDiGetDeviceInterfaceDetailA(i2, &ifd, det, need, NULL, &d2) &&
                d2.DevInst == dd->DevInst &&
                finish_open(det->DevicePath, d, denied)) {
                done = 1;
            }
            free(det);
        }
        SetupDiDestroyDeviceInfoList(i2);
        if (done) return 1;
    }
    return 0;
}

/* Log every Xiaomi/Google USB device present and which driver it uses, so the
 * user can see what Windows actually bound the phone to. */
static void diagnose_usb(void) {
    HDEVINFO info = SetupDiGetClassDevs(&GUID_DEVINTERFACE_USB_DEVICE, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof(ifd);
    int any = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(info, NULL,
            &GUID_DEVINTERFACE_USB_DEVICE, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(info, &ifd, NULL, 0, &need, NULL);
        if (!need) continue;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A det =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(need);
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        SP_DEVINFO_DATA dd; dd.cbSize = sizeof(dd);
        if (!SetupDiGetDeviceInterfaceDetailA(info, &ifd, det, need, NULL, &dd)) {
            free(det); continue;
        }
        free(det);
        char hw[256], svc[64];
        devprop_sz(info, &dd, SPDRP_HARDWAREID, hw, sizeof(hw));
        devprop_sz(info, &dd, SPDRP_SERVICE, svc, sizeof(svc));
        char lo[256]; strncpy(lo, hw, sizeof(lo) - 1); lo[sizeof(lo) - 1] = 0;
        for (char *p = lo; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (strstr(lo, "vid_18d1") || strstr(lo, "vid_2717") ||
            strstr(lo, "vid_05c6") /* Qualcomm/EDL */) {
            ui_log(g_lang ? "  USB %s  driver=%s" : "  USB %s  драйвер=%s", hw, svc[0] ? svc : "(none)");
            any = 1;
        }
    }
    SetupDiDestroyDeviceInfoList(info);
    if (!any)
        ui_log("%s", g_lang ? "  No Xiaomi/Google USB device visible. Check cable / MiAssistant mode / driver."
                            : "  USB-устройств Xiaomi/Google не видно. Проверьте кабель / режим MiAssistant / драйвер.");
}

/* returns 0 ok; 1 not found; 2 found but access denied (adb server?) */
/* localized guidance when the phone can't be opened */
static void log_conn_help(void) {
    if (g_lang) {
        ui_log("%s", "Hint:");
        ui_log("%s", "  1) On the phone enter Recovery and choose \"Connect with MiAssistant\".");
        ui_log("%s", "  2) Plug the phone into the PC (rear USB 2.0 port, no hub).");
        ui_log("%s", "  3) Press ADB > Kill server.");
        ui_log("%s", "  4) Menu > Phone info.");
        ui_log("%s", "  Driver/busy issues: close adb.exe and Mi tools; if driver != WinUSB, install WinUSB with Zadig on the VID 18D1/2717 interface.");
    } else {
        ui_log("%s", "Подсказка:");
        ui_log("%s", "  1) На телефоне войдите в Recovery и выберите «Connect with MiAssistant» (连接小米助手).");
        ui_log("%s", "  2) Подключите телефон к ПК кабелем (лучше задний порт USB 2.0, без хаба).");
        ui_log("%s", "  3) Нажмите ADB → Kill server.");
        ui_log("%s", "  4) Меню → Информация о телефоне.");
        ui_log("%s", "  Если про драйвер/занятость: закройте adb.exe и Mi-утилиты; если драйвер ≠ WinUSB — поставьте WinUSB через Zadig на интерфейс с VID 18D1/2717.");
    }
}

/* Enumerate all present USB devices; for each WinUSB-backed one, resolve and
 * open its ADB interface. This mirrors how libusb finds the phone. */
static int dev_open(adb_dev *d, char *errbuf, int errlen) {
    memset(d, 0, sizeof(*d));
    d->local_id = 1;
    int denied = 0, saw_winusb = 0;

    HDEVINFO info = SetupDiGetClassDevs(&GUID_DEVINTERFACE_USB_DEVICE, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof(ifd);
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(info, NULL,
                &GUID_DEVINTERFACE_USB_DEVICE, i, &ifd); i++) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailA(info, &ifd, NULL, 0, &need, NULL);
            if (!need) continue;
            PSP_DEVICE_INTERFACE_DETAIL_DATA_A det =
                (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(need);
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            SP_DEVINFO_DATA dd; dd.cbSize = sizeof(dd);
            if (!SetupDiGetDeviceInterfaceDetailA(info, &ifd, det, need, NULL, &dd)) {
                free(det); continue;
            }
            char svc[64];
            devprop_sz(info, &dd, SPDRP_SERVICE, svc, sizeof(svc));
            if (_stricmp(svc, "winusb") != 0) { free(det); continue; }
            saw_winusb = 1;
            if (g_verbose) {
                char hw[256]; devprop_sz(info, &dd, SPDRP_HARDWAREID, hw, sizeof(hw));
                ui_log(g_lang ? "  WinUSB device: %s" : "  WinUSB-устройство: %s", hw);
            }
            int got = open_for_devnode(info, &dd, det->DevicePath, d, &denied);
            free(det);
            if (got) { SetupDiDestroyDeviceInfoList(info); return 0; }
        }
        SetupDiDestroyDeviceInfoList(info);
    }

    if (denied) {
        _snprintf(errbuf, errlen, "%s", g_lang
            ? "ADB interface is present but busy (access denied). Press \"Kill ADB "
              "server\", close Mi PC Suite / MiFlash / Android Studio / python.exe, replug, and retry."
            : "ADB-интерфейс найден, но занят (доступ запрещён). Нажмите \"Kill ADB server\", "
              "закройте Mi PC Suite / MiFlash / Android Studio / python.exe, переподключите кабель и повторите.");
        return 2;
    }
    if (saw_winusb)
        _snprintf(errbuf, errlen, "%s", g_lang
            ? "A WinUSB device is present but its ADB interface could not be opened (see lines above)."
            : "WinUSB-устройство есть, но открыть его ADB-интерфейс не удалось (см. строки выше).");
    else
        _snprintf(errbuf, errlen, "%s", g_lang
            ? "No WinUSB ADB interface found. Phone must be in MiAssistant mode with a WinUSB driver "
              "on its ADB interface (see the device list below)."
            : "ADB-интерфейс WinUSB не найден. Телефон должен быть в режиме MiAssistant, а на его "
              "ADB-интерфейсе — драйвер WinUSB (см. список устройств ниже).");
    return 1;
}

/* Lightweight, non-opening presence check for the status monitor. Returns
 * 1 if a Xiaomi/Google USB device is present; fills driver service name. */
static int probe_present(char *drv, int drvlen) {
    int found = 0;
    if (drv && drvlen) drv[0] = 0;
    HDEVINFO info = SetupDiGetClassDevs(&GUID_DEVINTERFACE_USB_DEVICE, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return 0;
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(info, NULL,
            &GUID_DEVINTERFACE_USB_DEVICE, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(info, &ifd, NULL, 0, &need, NULL);
        if (!need) continue;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A det =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(need);
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        SP_DEVINFO_DATA dd; dd.cbSize = sizeof(dd);
        if (!SetupDiGetDeviceInterfaceDetailA(info, &ifd, det, need, NULL, &dd)) {
            free(det); continue;
        }
        free(det);
        char hw[256]; devprop_sz(info, &dd, SPDRP_HARDWAREID, hw, sizeof(hw));
        for (char *p = hw; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (strstr(hw, "vid_18d1") || strstr(hw, "vid_2717")) {
            if (drv && drvlen) devprop_sz(info, &dd, SPDRP_SERVICE, drv, drvlen);
            found = 1;
        }
    }
    SetupDiDestroyDeviceInfoList(info);
    return found;
}


static int usb_write(adb_dev *d, const void *data, ULONG len) {
    OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ULONG done = 0;
    BOOL ok = WinUsb_WritePipe(d->winusb, d->ep_out, (PUCHAR)data, len, &done, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = WinUsb_GetOverlappedResult(d->winusb, &ov, &done, TRUE);
    if (!ok) g_usb_err = GetLastError();
    CloseHandle(ov.hEvent);
    return ok ? (int)done : -1;
}
static int usb_read_exact(adb_dev *d, void *data, ULONG len) {
    ULONG got = 0;
    unsigned char *p = (unsigned char *)data;
    while (got < len) {
        OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        ULONG n = 0;
        BOOL ok = WinUsb_ReadPipe(d->winusb, d->ep_in, p + got, len - got, &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = WinUsb_GetOverlappedResult(d->winusb, &ov, &n, TRUE);
        if (!ok) g_usb_err = GetLastError();
        CloseHandle(ov.hEvent);
        if (!ok || n == 0) return -1;
        got += n;
    }
    return (int)got;
}

/* one ADB message = 24-byte header (+ payload) */
static int adb_send(adb_dev *d, uint32_t cmd, uint32_t a0, uint32_t a1,
                    const void *data, uint32_t len) {
    uint32_t hdr[6];
    uint32_t sum = 0;
    const unsigned char *b = (const unsigned char *)data;
    for (uint32_t i = 0; i < len; i++) sum += b[i];   /* classic ADB data checksum */
    hdr[0] = cmd; hdr[1] = a0; hdr[2] = a1; hdr[3] = len;
    hdr[4] = sum; hdr[5] = cmd ^ 0xFFFFFFFFu;
    if (usb_write(d, hdr, sizeof(hdr)) < 0) return -1;
    if (len && usb_write(d, data, len) < 0) return -1;
    return 0;
}
static int adb_recv(adb_dev *d, uint32_t *cmd, uint32_t *a0, uint32_t *a1,
                    unsigned char *data, uint32_t cap, uint32_t *dlen) {
    uint32_t hdr[6];
    if (usb_read_exact(d, hdr, sizeof(hdr)) < 0) return -1;
    *cmd = hdr[0]; *a0 = hdr[1]; *a1 = hdr[2];
    uint32_t len = hdr[3];
    if (len > cap) return -1;
    if (len && usb_read_exact(d, data, len) < 0) return -1;
    *dlen = len;
    return 0;
}

static int adb_connect(adb_dev *d, char *banner, int blen) {
    Sleep(200);   /* settle after claiming the interface — reduces a race on Windows */
    if (adb_send(d, A_CNXN, ADB_VERSION, MAX_DATA, "host::\x00", 7)) return -1;
    unsigned char buf[1024];
    for (int t = 0; t < 10; t++) {
        uint32_t cmd, a0, a1, n;
        if (adb_recv(d, &cmd, &a0, &a1, buf, sizeof(buf), &n)) return -1;
        if (cmd == A_CNXN) {
            g_dev_maxdata = a1;
            uint32_t k = n < (uint32_t)blen - 1 ? n : (uint32_t)blen - 1;
            memcpy(banner, buf, k); banner[k] = 0;
            if (strncmp(banner, "sideload::", 10) != 0) return -2;
            return 0;
        }
    }
    return -1;
}

/* one-shot recovery service ("getsn:", "reboot:", ...) -> text response */
static int adb_service(adb_dev *d, const char *svc, char *out, int outlen) {
    char cmd[128];
    int n = _snprintf(cmd, sizeof(cmd) - 1, "%s", svc);
    if (adb_send(d, A_OPEN, d->local_id, 0, cmd, n + 1)) return -1;
    unsigned char buf[1024];
    int total = 0;
    if (out && outlen) out[0] = 0;
    for (;;) {
        uint32_t c, a0, a1, len;
        if (adb_recv(d, &c, &a0, &a1, buf, sizeof(buf), &len)) return -1;
        if (c == A_WRTE) {
            if (out) {
                int cp = (int)len;
                if (total + cp > outlen - 1) cp = outlen - 1 - total;
                if (cp > 0) { memcpy(out + total, buf, cp); total += cp; out[total] = 0; }
            }
        } else if (c == A_CLSE) {
            adb_send(d, A_CLSE, a1, a0, NULL, 0);
            break;
        }
    }
    /* trim trailing newline/space */
    while (total > 0 && (out[total-1] == '\n' || out[total-1] == '\r' || out[total-1] == ' '))
        out[--total] = 0;
    return 0;
}

/* ==================================================== crypto / http helpers */
static int b64_encode(const unsigned char *in, DWORD inlen, char **out) {
    DWORD n = 0;
    if (!CryptBinaryToStringA(in, inlen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &n))
        return -1;
    *out = (char *)malloc(n + 1);
    if (!CryptBinaryToStringA(in, inlen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, *out, &n)) {
        free(*out); return -1;
    }
    (*out)[n] = 0;
    return 0;
}
static int b64_decode(const char *in, unsigned char **out, DWORD *outlen) {
    DWORD n = 0;
    if (!CryptStringToBinaryA(in, 0, CRYPT_STRING_BASE64, NULL, &n, NULL, NULL)) return -1;
    *out = (unsigned char *)malloc(n ? n : 1);
    if (!CryptStringToBinaryA(in, 0, CRYPT_STRING_BASE64, *out, &n, NULL, NULL)) {
        free(*out); return -1;
    }
    *outlen = n;
    return 0;
}

/* AES-128-CBC one shot (no padding here; caller handles PKCS7) */
static int aes_cbc(int encrypt, const unsigned char *in, DWORD inlen,
                   unsigned char *out) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    int rc = -1;
    unsigned char iv[16];
    memcpy(iv, OTA_IV, 16);
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0)) return -1;
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                          (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0)) goto done;
    if (BCryptGenerateSymmetricKey(alg, &key, NULL, 0,
                                   (PUCHAR)OTA_KEY, 16, 0)) goto done;
    ULONG res = 0;
    NTSTATUS st = encrypt
        ? BCryptEncrypt(key, (PUCHAR)in, inlen, NULL, iv, 16, out, inlen, &res, 0)
        : BCryptDecrypt(key, (PUCHAR)in, inlen, NULL, iv, 16, out, inlen, &res, 0);
    if (st == 0) rc = 0;
done:
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return rc;
}

static int md5_file(const char *path, char hexout[33]) {
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE h = NULL;
    int rc = -1;
    unsigned char digest[16];
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, NULL, 0)) goto done;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0)) goto done;
    {
        unsigned char *buf = (unsigned char *)malloc(1 << 20);
        size_t r;
        while ((r = fread(buf, 1, 1 << 20, fp)) > 0)
            BCryptHashData(h, buf, (ULONG)r, 0);
        free(buf);
    }
    if (BCryptFinishHash(h, digest, 16, 0)) goto done;
    for (int i = 0; i < 16; i++) sprintf(hexout + i * 2, "%02x", digest[i]);
    hexout[32] = 0;
    rc = 0;
done:
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    fclose(fp);
    return rc;
}

/* percent-encode everything that isn't RFC3986 unreserved */
static char *url_encode(const char *s) {
    static const char *hex = "0123456789ABCDEF";
    size_t n = strlen(s);
    char *o = (char *)malloc(n * 3 + 1), *p = o;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            *p++ = c;
        else { *p++ = '%'; *p++ = hex[c >> 4]; *p++ = hex[c & 0xF]; }
    }
    *p = 0;
    return o;
}
/* decode %XX only (leave '+' as-is, like curl_easy_unescape) */
static void url_decode_inplace(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            int hi = (r[1] <= '9') ? r[1]-'0' : (r[1]|0x20)-'a'+10;
            int lo = (r[2] <= '9') ? r[2]-'0' : (r[2]|0x20)-'a'+10;
            *w++ = (char)((hi << 4) | lo); r += 3;
        } else *w++ = *r++;
    }
    *w = 0;
}

/* POST the encrypted request, return raw response body (malloc'd) or NULL */
static char *http_post(const char *body, char *errbuf, int errlen) {
    char *resp = NULL;
    HINTERNET s = WinHttpOpen(L"MiTunes_UserAgent_v3.0",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { _snprintf(errbuf, errlen, "WinHttpOpen failed"); return NULL; }
    HINTERNET c = WinHttpConnect(s, OTA_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!c) { _snprintf(errbuf, errlen, "WinHttpConnect failed"); WinHttpCloseHandle(s); return NULL; }
    HINTERNET r = WinHttpOpenRequest(c, L"POST", OTA_PATH, NULL,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
    if (!r) { _snprintf(errbuf, errlen, "WinHttpOpenRequest failed"); goto out2; }

    const WCHAR *hdrs =
        L"clientId: MITUNES\r\n"
        L"Content-Type: application/x-www-form-urlencoded\r\n"
        L"Accept-Encoding: identity\r\n";
    if (!WinHttpSendRequest(r, hdrs, -1L, (LPVOID)body, (DWORD)strlen(body),
                            (DWORD)strlen(body), 0)) {
        _snprintf(errbuf, errlen, "WinHttpSendRequest failed (err %lu)", GetLastError());
        goto out1;
    }
    if (!WinHttpReceiveResponse(r, NULL)) {
        _snprintf(errbuf, errlen, "WinHttpReceiveResponse failed (err %lu)", GetLastError());
        goto out1;
    }
    DWORD status = 0, slen = sizeof(status);
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        NULL, &status, &slen, NULL);
    if (status != 200) { _snprintf(errbuf, errlen, "server HTTP %lu", status); goto out1; }

    DWORD cap = 4096, len = 0;
    resp = (char *)malloc(cap);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
        if (len + avail + 1 > cap) { cap = len + avail + 1; resp = (char *)realloc(resp, cap); }
        DWORD got = 0;
        if (!WinHttpReadData(r, resp + len, avail, &got) || got == 0) break;
        len += got;
    }
    resp[len] = 0;
    ui_log("  server: HTTP %lu, %lu bytes", status, len);
out1:
    WinHttpCloseHandle(r);
out2:
    WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return resp;
}

/* extract JSON string value of PkgRom.Validate (unescaping \/ \" \\), malloc'd */
static char *json_find_validate(const char *json) {
    const char *scope = strstr(json, "\"PkgRom\"");
    if (!scope) scope = json;
    const char *k = strstr(scope, "\"Validate\"");
    if (!k) return NULL;
    const char *p = strchr(k + 10, ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    char *out = (char *)malloc(strlen(p) + 1), *w = out;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case '/': *w++ = '/'; break;
                case '\\': *w++ = '\\'; break;
                case '"': *w++ = '"'; break;
                case 'n': *w++ = '\n'; break;
                default: *w++ = *p; break;
            }
            p++;
        } else *w++ = *p++;
    }
    *w = 0;
    return out;
}

/* find an integer JSON field ("Erase":1 or "Erase":"1"); returns 1 if found */
static int json_find_int(const char *json, const char *key, int *out) {
    char pat[64]; _snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(json, pat);
    if (!k) return 0;
    const char *p = strchr(k + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    int neg = (*p == '-'); if (neg) p++;
    if (*p < '0' || *p > '9') return 0;
    int v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    *out = neg ? -v : v;
    return 1;
}

/* find a string JSON field ("message":"..."); returns 1 if found and non-empty */
static int json_find_str(const char *json, const char *key, char *out, int n) {
    char pat[64]; _snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(json, pat);
    if (!k) { out[0] = 0; return 0; }
    const char *p = strchr(k + strlen(pat), ':');
    if (!p) { out[0] = 0; return 0; }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') { out[0] = 0; return 0; }
    p++; int i = 0;
    while (*p && *p != '"' && i < n - 1) {
        if (*p == '\\' && p[1]) { p++; out[i++] = *p; } else out[i++] = *p;
        p++;
    }
    out[i] = 0;
    return out[0] != 0;
}

/* trim to the outermost { ... } (strip any pre/post garbage after decrypt) */
static void json_trim_braces(char *s) {
    char *a = strchr(s, '{');
    char *b = strrchr(s, '}');
    if (a && b && b > a) { *(b + 1) = 0; memmove(s, a, strlen(a) + 1); }
}
static DWORD pkcs7_pad(const unsigned char *in, DWORD len, unsigned char **out) {
    DWORD pad = 16 - (len % 16);
    DWORD tot = len + pad;
    *out = (unsigned char *)malloc(tot);
    memcpy(*out, in, len);
    memset(*out + len, (int)pad, pad);
    return tot;
}

/* device info collected from recovery */
typedef struct {
    char device[64], version[64], sn[128], codebase[64],
         branch[64], lang[64], region[64], romzone[64];
} dev_info;

static int read_info(adb_dev *d, dev_info *fi) {
    if (adb_service(d, "getdevice:",   fi->device,   sizeof(fi->device)))   return -1;
    if (adb_service(d, "getversion:",  fi->version,  sizeof(fi->version)))  return -1;
    if (adb_service(d, "getsn:",       fi->sn,       sizeof(fi->sn)))       return -1;
    if (adb_service(d, "getcodebase:", fi->codebase, sizeof(fi->codebase))) return -1;
    if (adb_service(d, "getbranch:",   fi->branch,   sizeof(fi->branch)))   return -1;
    if (adb_service(d, "getlanguage:", fi->lang,     sizeof(fi->lang)))     return -1;
    if (adb_service(d, "getregion:",   fi->region,   sizeof(fi->region)))   return -1;
    if (adb_service(d, "getromzone:",  fi->romzone,  sizeof(fi->romzone)))  return -1;
    return 0;
}

/* structured device info, one field per line */
static void log_device_info(const dev_info *fi) {
    ui_log("%s %s", g_lang ? "Model:"    : "Модель телефона:", fi->device);
    ui_log("%s %s", g_lang ? "Firmware:" : "Версия прошивки:", fi->version);
    ui_log("%s %s", g_lang ? "Region:"   : "Регион:",          fi->region);
    ui_log("%s %s", g_lang ? "Codebase:" : "Кодовая база:",    fi->codebase);
    ui_log("%s %s", g_lang ? "Branch:"   : "Ветка:",           fi->branch);
    ui_log("%s %s", g_lang ? "Zone:"     : "Зона:",            fi->romzone);
    ui_log("%s %s", g_lang ? "Language:" : "Язык:",            fi->lang);
    ui_log("%s %s", g_lang ? "Serial:"   : "Серийный номер:",  fi->sn);
}

/* obtain the Validate token from Xiaomi's server; token malloc'd into *out.
 * *erase_out is set to 1 if the server requires a data wipe (cross-region). */
static int get_validate(const dev_info *fi, const char *fw,
                        char **out, int *erase_out, char *errbuf, int errlen) {
    char md5[33];
    if (md5_file(fw, md5)) { _snprintf(errbuf, errlen, "cannot MD5 firmware"); return -1; }
    ui_log("  pkg md5  : %s", md5);
    if (erase_out) *erase_out = 0;

    /* romzone injected verbatim (unquoted, may be non-numeric); default "1" */
    const char *zone = fi->romzone[0] ? fi->romzone : "1";

    /* request shape matches the proven Mi Assistant client exactly:
     * no "r"/"id", language pinned to en-US, zone verbatim */
    char json[1024];
    int jlen = _snprintf(json, sizeof(json) - 1,
        "{\"d\":\"%s\",\"v\":\"%s\",\"c\":\"%s\",\"b\":\"%s\",\"sn\":\"%s\","
        "\"l\":\"en-US\",\"f\":\"1\",\"options\":{\"zone\":%s},\"pkg\":\"%s\"}",
        fi->device, fi->version, fi->codebase, fi->branch, fi->sn, zone, md5);
    if (jlen < 0) { _snprintf(errbuf, errlen, "request too large"); return -1; }

    unsigned char *padded; DWORD plen = pkcs7_pad((unsigned char *)json, jlen, &padded);
    unsigned char *enc = (unsigned char *)malloc(plen);
    if (aes_cbc(1, padded, plen, enc)) { free(padded); free(enc);
        _snprintf(errbuf, errlen, "AES encrypt failed"); return -1; }
    free(padded);

    char *b64; if (b64_encode(enc, plen, &b64)) { free(enc);
        _snprintf(errbuf, errlen, "base64 failed"); return -1; }
    free(enc);

    char *q = url_encode(b64); free(b64);
    char *body = (char *)malloc(strlen(q) + 16);
    sprintf(body, "q=%s&t=&s=1", q); free(q);
    ui_log("  request  : %s", json);
    ui_log("  POST https://%ls%ls (encrypted %lu bytes)", OTA_HOST, OTA_PATH, plen);

    char *resp = http_post(body, errbuf, errlen); free(body);
    if (!resp) return -1;

    url_decode_inplace(resp);
    unsigned char *cipher; DWORD clen;
    if (b64_decode(resp, &cipher, &clen) || clen == 0 || clen % 16) {
        free(resp); _snprintf(errbuf, errlen, "bad server response"); return -1;
    }
    free(resp);
    unsigned char *plain = (unsigned char *)malloc(clen + 1);
    if (aes_cbc(0, cipher, clen, plain)) { free(cipher); free(plain);
        _snprintf(errbuf, errlen, "AES decrypt failed"); return -1; }
    free(cipher);
    DWORD pad = plain[clen - 1];
    if (pad > 16) pad = 0;
    plain[clen - pad] = 0;

    json_trim_braces((char *)plain);

    char *val = json_find_validate((char *)plain);
    if (!val) {
        char msg[256];
        if (json_find_str((char *)plain, "message", msg, sizeof(msg)))
            ui_log(g_lang ? "  server message: %s" : "  сообщение сервера: %s", msg);
        ui_log(g_lang ? "  server reply: %.480s" : "  ответ сервера: %.480s", (char *)plain);
        _snprintf(errbuf, errlen, "%s", g_lang
            ? "server issued no Validate token — likely a downgrade/blocked target, a "
              "wrong region/variant, or a corrupt zip (md5 not recognised)."
            : "сервер не выдал токен Validate — вероятно даунгрейд/запрещённая цель, "
              "не тот регион/вариант, или битый zip (md5 не распознан).");
        free(plain); return -1;
    }
    int erase = 0;
    if (json_find_int((char *)plain, "Erase", &erase) && erase_out) *erase_out = erase;
    ui_log("  decrypted: %lu bytes, Erase=%d, token len=%d",
           (unsigned long)strlen((char *)plain), erase, (int)strlen(val));
    free(plain);
    *out = val;
    return 0;
}

/* stream the OTA to the device using sideload-host + validate token.
 * wipe: 0/1 -> final field of sideload-host (data-wipe flag). */
static int do_sideload(adb_dev *d, const char *fw, const char *validate,
                       int wipe, char *errbuf, int errlen) {
    FILE *fp = fopen(fw, "rb");
    if (!fp) { _snprintf(errbuf, errlen, "cannot open firmware"); return -1; }
    _fseeki64(fp, 0, SEEK_END);
    long long size = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);
    if (size <= 0) { _snprintf(errbuf, errlen, "firmware is empty"); fclose(fp); return -1; }

    uint32_t chunk = g_gentle ? 16384u : CHUNK;   /* gentle mode = smaller blocks */
    int block_delay = g_gentle ? 1 : 0;           /* + tiny pause between blocks */
    long long total_blocks = (size + chunk - 1) / chunk;
    unsigned char *served = (unsigned char *)calloc((size_t)total_blocks, 1);
    long long served_count = 0, max_end = 0;

    char open_str[1024];
    _snprintf(open_str, sizeof(open_str) - 1,
              "sideload-host:%lld:%u:%s:%d", size, chunk, validate, wipe ? 1 : 0);
    ui_log("%s %lld %s, %lld %s x %u KiB, wipe=%d, recovery maxdata=%u",
           g_lang ? "sideload:" : "sideload:", size, g_lang ? "bytes" : "байт",
           total_blocks, g_lang ? "blocks" : "блоков", chunk / 1024, wipe ? 1 : 0,
           g_dev_maxdata);
    if (g_dev_maxdata && g_dev_maxdata < chunk)
        ui_log("%s", g_lang
            ? "WARNING: recovery maxdata < block size — transport may reject blocks!"
            : "ВНИМАНИЕ: maxdata рекавери меньше размера блока — передача может отклоняться!");
    if (adb_send(d, A_OPEN, d->local_id, 0, open_str, (uint32_t)strlen(open_str) + 1)) {
        _snprintf(errbuf, errlen, "sideload open failed");
        free(served); fclose(fp); return -1;
    }
    ui_log("%s", g_lang ? "  OPEN sent, waiting for recovery…"
                        : "  OPEN отправлен, ждём рекавери…");

    unsigned char *work = (unsigned char *)malloc(CHUNK);
    unsigned char msg[256];
    int last_pct = -1, last_decile = -1, rc = -1, done = 0, failed = 0, have_wrte = 0;
    char status[256] = {0};
    uint32_t p_cmd = 0, p_a0 = 0, p_a1 = 0, p_len = 0;

    /* --- open phase: wait for the first WRTE; record remote id from OKAY,
     *     but do NOT acknowledge that OKAY (matches the proven client) --- */
    for (int i = 0; i < 32 && !have_wrte; i++) {
        uint32_t cmd, a0, a1, len;
        if (adb_recv(d, &cmd, &a0, &a1, msg, sizeof(msg), &len)) {
            _snprintf(errbuf, errlen, "device closed while opening sideload");
            free(work); free(served); fclose(fp); return -1;
        }
        if (cmd == A_OKAY) { ui_log("  recovery OKAY (remote-id=%u)", a0); continue; }
        if (cmd == A_CLSE) { _snprintf(errbuf, errlen, "recovery refused sideload (CLSE)");
                             free(work); free(served); fclose(fp); return -1; }
        if (cmd == A_WRTE) { p_cmd = cmd; p_a0 = a0; p_a1 = a1; p_len = len; have_wrte = 1;
                             msg[len < sizeof(msg) ? len : sizeof(msg) - 1] = 0;
                             ui_log("  first block request: \"%s\"", (char *)msg); }
    }
    if (!have_wrte) { _snprintf(errbuf, errlen, "recovery did not start sideload");
                      free(work); free(served); fclose(fp); return -1; }

    /* --- transfer phase --- */
    while (!done) {
        uint32_t cmd, a0, a1, len;
        if (p_len || p_cmd) { cmd = p_cmd; a0 = p_a0; a1 = p_a1; len = p_len; p_cmd = p_len = 0; }
        else if (adb_recv(d, &cmd, &a0, &a1, msg, sizeof(msg), &len)) {
            /* link dropped: success if we already delivered everything or got a status */
            if (status[0] || max_end >= size) rc = 0;
            else _snprintf(errbuf, errlen,
                    "USB read failed (err %lu) after %lld blocks / %lld MiB",
                    g_usb_err, served_count, max_end / (1024 * 1024));
            break;
        }

        if (cmd == A_OKAY) { adb_send(d, A_OKAY, a1, a0, NULL, 0); continue; }
        if (cmd == A_CLSE) { adb_send(d, A_CLSE, a1, a0, NULL, 0); rc = 0; break; }
        if (cmd != A_WRTE) continue;

        msg[len < sizeof(msg) ? len : sizeof(msg) - 1] = 0;

        if (len == 8 && memcmp(msg, "DONEDONE", 8) == 0) {
            adb_send(d, A_OKAY, a1, a0, NULL, 0); rc = 0; done = 1; break;
        }
        if (len == 8 && memcmp(msg, "FAILFAIL", 8) == 0) {
            adb_send(d, A_OKAY, a1, a0, NULL, 0);
            _snprintf(errbuf, errlen, "recovery reported FAILFAIL (package rejected)");
            failed = 1; done = 1; break;
        }

        int numeric = (len > 0);
        for (uint32_t i = 0; i < len; i++)
            if (msg[i] < '0' || msg[i] > '9') { numeric = 0; break; }

        if (!numeric) {
            strncpy(status, (char *)msg, sizeof(status) - 1);
            status[sizeof(status) - 1] = 0;
            ui_log("[device] %s", status);
            adb_send(d, A_OKAY, a1, a0, NULL, 0);
            rc = 0; done = 1; break;
        }

        long long block = strtoll((char *)msg, NULL, 10);
        long long offset = block * chunk;
        if (offset < 0 || offset >= size) { adb_send(d, A_OKAY, a1, a0, NULL, 0); continue; }

        uint32_t to_write = chunk;
        if (offset + chunk > size) to_write = (uint32_t)(size - offset);
        _fseeki64(fp, offset, SEEK_SET);
        fread(work, 1, to_write, fp);
        adb_send(d, A_WRTE, a1, a0, work, to_write);
        adb_send(d, A_OKAY, a1, a0, NULL, 0);
        if (block_delay) Sleep(block_delay);

        if (offset + to_write > max_end) max_end = offset + to_write;
        if (block < total_blocks && !served[block]) { served[block] = 1; served_count++; }
        if (served_count <= 3 || served_count % 200 == 0)
            ui_log("  block %lld/%lld  (offset %lld MiB)",
                   block, total_blocks, offset / (1024 * 1024));
        int pct = (int)((served_count * 100) / total_blocks);
        if (pct != last_pct) { ui_progress(pct); last_pct = pct; }
        if (pct / 10 != last_decile) {         /* log every 10% */
            last_decile = pct / 10;
            ui_log("%s %d%%  (%lld / %lld MiB)",
                   g_lang ? "Transfer:" : "Передача:", pct,
                   max_end / (1024 * 1024), size / (1024 * 1024));
        }
    }

    adb_send(d, A_CLSE, d->local_id, 0, NULL, 0);   /* close from our side */
    Sleep(100);
    free(work);
    free(served);
    fclose(fp);

    if (failed) return -1;
    if (rc != 0) return -1;

    if (status[0]) {
        char low[256]; int i = 0;
        for (; status[i] && i < 255; i++) low[i] = (char)tolower((unsigned char)status[i]);
        low[i] = 0;
        if (strstr(low, "abort") || strstr(low, "fail") || strstr(low, "error")) {
            _snprintf(errbuf, errlen, "recovery reported failure: %s", status);
            return -1;
        }
    }
    if (max_end < size && served_count < total_blocks) {
        _snprintf(errbuf, errlen,
                  "transfer ended early (%lld of %lld bytes)", max_end, size);
        return -1;
    }
    ui_progress(100);
    return 0;
}

/* ============================================================ log to file */
static CRITICAL_SECTION g_logcs;

static void write_logfile(const char *utf8) {
    EnterCriticalSection(&g_logcs);
    if (g_logfp) {
        fputs(utf8, g_logfp);
        fputc('\r', g_logfp); fputc('\n', g_logfp);
        fflush(g_logfp);
    }
    LeaveCriticalSection(&g_logcs);
}
static void app_path(char *out, int n, const char *leaf) {
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof(dir));
    char *p = strrchr(dir, '\\');
    if (p) *(p + 1) = 0; else strcpy(dir, ".\\");
    _snprintf(out, n, "%s%s", dir, leaf);
}
static void open_logfile(void) {
    EnterCriticalSection(&g_logcs);
    if (!g_logfp) {
        char base[MAX_PATH];
        app_path(base, sizeof(base), "Logs");
        CreateDirectoryA(base, NULL);
        char path[MAX_PATH];
        _snprintf(path, sizeof(path), "%s\\MiSideload-%s.log", base, g_launch_stamp);
        g_logfp = fopen(path, "ab");
        if (g_logfp) {
            fseek(g_logfp, 0, SEEK_END);
            if (ftell(g_logfp) == 0) {   /* UTF-8 BOM so Notepad shows Cyrillic */
                fputc(0xEF, g_logfp); fputc(0xBB, g_logfp); fputc(0xBF, g_logfp);
            }
        }
    }
    LeaveCriticalSection(&g_logcs);
}
static void close_logfile(void) {
    EnterCriticalSection(&g_logcs);
    if (g_logfp) { fclose(g_logfp); g_logfp = NULL; }
    LeaveCriticalSection(&g_logcs);
}

/* ============================================================ adb helpers */
static void adb_cmdline(char *out, int n, const char *args) {
    const char *e = g_adb_path[0] ? g_adb_path : "adb.exe";
    _snprintf(out, n, "\"%s\" %s", e, args);
}

static void run_one(const char *cmdline) {
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    char buf[512]; strncpy(buf, cmdline, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    if (CreateProcessA(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 8000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
}

/* run a child and capture its stdout+stderr (ASCII) */
static int run_capture(const char *cmdline, char *out, int outlen) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi;
    char buf[512]; strncpy(buf, cmdline, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    if (!CreateProcessA(NULL, buf, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr); return -1;
    }
    CloseHandle(wr);
    int total = 0; DWORD got;
    while (total < outlen - 1 &&
           ReadFile(rd, out + total, outlen - 1 - total, &got, NULL) && got > 0)
        total += got;
    out[total] = 0;
    WaitForSingleObject(pi.hProcess, 8000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    /* collapse lone newlines for single-line-ish logging */
    for (char *p = out; *p; p++) if (*p == '\r' || *p == '\n') *p = ' ';
    return 0;
}

/* forward */
static void set_busy(int busy);

static void run_kill_adb_server(void) {
    char c[MAX_PATH + 32];
    adb_cmdline(c, sizeof(c), "kill-server");
    run_one(c);
    run_one("taskkill /F /IM adb.exe /T");
    ui_log("%s", g_lang ? "Killed adb server + any stray adb.exe."
                        : "adb-сервер остановлен, посторонние adb.exe сняты.");
}

static DWORD WINAPI adb_check_thread(LPVOID p) {
    (void)p;
    char cmd[MAX_PATH + 16], out[1024];
    adb_cmdline(cmd, sizeof(cmd), "version");
    if (run_capture(cmd, out, sizeof(out)) == 0 && out[0])
        ui_log("%s %s", g_lang ? "adb:" : "adb:", out);
    else
        ui_log("%s", g_lang ? "adb.exe not found — set its path in the ADB menu."
                            : "adb.exe не найден — укажите путь в меню ADB.");
    PostMessage(g_main, WM_APP_DONE, 0, 0);
    return 0;
}

static DWORD WINAPI server_check_thread(LPVOID p) {
    (void)p;
    int okc = 0; DWORD status = 0;
    HINTERNET s = WinHttpOpen(L"MiTunes_UserAgent_v3.0",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) {
        HINTERNET c = WinHttpConnect(s, OTA_HOST, 80, 0);
        if (c) {
            HINTERNET r = WinHttpOpenRequest(c, L"GET", L"/", NULL,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            if (r) {
                if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       NULL, 0, 0, 0) &&
                    WinHttpReceiveResponse(r, NULL)) {
                    DWORD sl = sizeof(status);
                    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE |
                                        WINHTTP_QUERY_FLAG_NUMBER, NULL,
                                        &status, &sl, NULL);
                    okc = 1;
                }
                WinHttpCloseHandle(r);
            }
            WinHttpCloseHandle(c);
        }
        WinHttpCloseHandle(s);
    }
    if (okc)
        ui_log(g_lang ? "Server update.miui.com reachable (HTTP %lu)."
                      : "Сервер update.miui.com доступен (HTTP %lu).", status);
    else
        ui_log(g_lang ? "Server update.miui.com is NOT reachable."
                      : "Сервер update.miui.com НЕдоступен.");
    PostMessage(g_main, WM_APP_DONE, 0, 0);
    return 0;
}

/* ============================================================ firmware fetch */
/* forward decls (defined later in the file) */
static void update_footer(void);
static void app_path(char *out, int n, const char *leaf);

#define DL_IDLE  0
#define DL_RUN   1
#define DL_PAUSE 2
#define DL_STOP  3
static volatile LONG g_dl_state = DL_IDLE;
static char g_dl_url[1024] = {0};

#define MAXRES 500
static char g_res_ver[MAXRES][48];    /* version, e.g. OS1.0.19.0.UKWEUXM */
static char g_res_file[MAXRES][160];  /* recovery zip filename */
static char g_res_reg[MAXRES][24];    /* region label, e.g. eea/orange */
static int  g_res_count = 0, g_res_current = -1, g_res_sel = 0, g_mirror_sel = 0;
static char g_dev_curver[64] = {0};
static char g_dev_region[24] = {0};
static char g_res_codename[64] = {0};
static char g_regions[64][24]; static int g_region_count = 0, g_region_sel = 0;
static int  g_filt[MAXRES], g_filt_count = 0;
static const char *MIRRORS[] = {
    "bkt-sgp-miui-ota-update-alisgp.oss-ap-southeast-1.aliyuncs.com",
    "bigota.d.miui.com", "hugeota.d.miui.com", "cdnorg.d.miui.com", "bn.d.miui.com"
};
static const char *MIRROR_NAMES[] = { "OSS (aliyun)", "bigota", "hugeota", "cdnorg", "bn" };

typedef struct { WCHAR host[256]; WCHAR path[1200]; INTERNET_PORT port; int secure; } url_t;

static int url_parse(const char *url, url_t *u) {
    const char *p = url; int sec;
    if (!_strnicmp(p, "https://", 8)) { sec = 1; p += 8; }
    else if (!_strnicmp(p, "http://", 7)) { sec = 0; p += 7; }
    else return -1;
    char host[256]; int i = 0;
    while (*p && *p != '/' && *p != ':' && i < 255) host[i++] = *p++;
    host[i] = 0;
    int port = sec ? 443 : 80;
    if (*p == ':') { p++; port = atoi(p); while (*p && *p != '/') p++; }
    const char *path = (*p) ? p : "/";
    MultiByteToWideChar(CP_UTF8, 0, host, -1, u->host, 256);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, u->path, 1200);
    u->port = (INTERNET_PORT)port; u->secure = sec;
    return 0;
}

/* GET a text page (HTML). Returns malloc'd body or NULL. */
static char *fetch_url_text(const char *url) {
    url_t u; if (url_parse(url, &u)) return NULL;
    char *resp = NULL;
    HINTERNET s = WinHttpOpen(L"Mozilla/5.0 MiSideload",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return NULL;
    HINTERNET c = WinHttpConnect(s, u.host, u.port, 0);
    if (c) {
        HINTERNET r = WinHttpOpenRequest(c, L"GET", u.path, NULL, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0);
        if (r) {
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) &&
                WinHttpReceiveResponse(r, NULL)) {
                DWORD cap = 1 << 16, len = 0; resp = (char *)malloc(cap);
                for (;;) {
                    DWORD avail = 0; if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
                    if (len + avail + 1 > cap) { cap = len + avail + 1; resp = (char *)realloc(resp, cap); }
                    DWORD got = 0; if (!WinHttpReadData(r, resp + len, avail, &got) || !got) break;
                    len += got;
                }
                resp[len] = 0;
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return resp;
}

/* version = first path segment after ".com/" */
static void url_version(const char *url, char *out, int n) {
    const char *p = strstr(url, ".com/");
    if (!p) { out[0] = 0; return; }
    p += 5;
    const char *e = strchr(p, '/');
    int L = e ? (int)(e - p) : (int)strlen(p);
    if (L <= 0 || L >= n) L = n - 1;
    memcpy(out, p, L); out[L] = 0;
}
static void url_basename(const char *url, char *out, int n) {
    const char *s = strrchr(url, '/');
    s = s ? s + 1 : url;
    strncpy(out, s, n - 1); out[n - 1] = 0;
}

/* pull recovery zip URLs (bigota .../miui_*.zip) out of the HTML, newest first, deduped */
static int extract_recovery_urls(const char *html, char urls[][512], int maxn) {
    int n = 0;
    const char *p = html;
    while (n < maxn && (p = strstr(p, "https://bigota.d.miui.com/")) != NULL) {
        const char *e = p;
        while (*e && *e != '"' && *e != '\'' && *e != ' ' && *e != '<' && *e != ')') e++;
        size_t L = (size_t)(e - p);
        if (L > 20 && L < 500) {
            char tmp[512]; memcpy(tmp, p, L); tmp[L] = 0;
            if (strstr(tmp, "/miui_") && L >= 4 && !strcmp(tmp + L - 4, ".zip")) {
                int dup = 0; for (int i = 0; i < n; i++) if (!strcmp(urls[i], tmp)) { dup = 1; break; }
                if (!dup) { strncpy(urls[n], tmp, 511); urls[n][511] = 0; n++; }
            }
        }
        p = e;
    }
    return n;
}

/* one download attempt with resume; returns 0 done, 1 neterr, 2 stopped, 3 http, 4 fatal */
static int download_once(const char *url, const char *path, long long *existing) {
    url_t u; if (url_parse(url, &u)) return 4;
    HINTERNET s = WinHttpOpen(L"Mozilla/5.0 MiSideload",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 1;
    HINTERNET c = WinHttpConnect(s, u.host, u.port, 0);
    if (!c) { WinHttpCloseHandle(s); return 1; }
    HINTERNET r = WinHttpOpenRequest(c, L"GET", u.path, NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!r) { WinHttpCloseHandle(c); WinHttpCloseHandle(s); return 1; }
    if (*existing > 0) {
        WCHAR rng[64]; wsprintfW(rng, L"Range: bytes=%I64d-", *existing);
        WinHttpAddRequestHeaders(r, rng, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }
    int res = 1;
    if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) &&
        WinHttpReceiveResponse(r, NULL)) {
        DWORD status = 0, sl = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            NULL, &status, &sl, NULL);
        int append = (status == 206);
        if (*existing > 0 && status == 200) { *existing = 0; append = 0; }
        if (status != 200 && status != 206) {
            ui_log("HTTP %lu", status);
            res = 3;
        } else {
            long long clen = 0;
            { WCHAR cl[32]; DWORD cll = sizeof(cl);
              if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, NULL, cl, &cll, NULL))
                  clen = _wtoi64(cl); }
            long long total = *existing + clen;
            FILE *fp = fopen(path, append ? "ab" : "wb");
            if (!fp) { res = 4; }
            else {
                char *buf = (char *)malloc(1 << 16);
                long long got = *existing; int last_pct = -1, last_dec = -1;
                ULONGLONG sp_last = GetTickCount64(); long long sp_bytes = got;
                char spbuf[32] = "";
                res = 0;
                for (;;) {
                    if (g_dl_state == DL_STOP) { res = 2; break; }
                    while (g_dl_state == DL_PAUSE) {
                        ui_status(g_lang ? "Download paused" : "Загрузка на паузе");
                        Sleep(200);
                    }
                    if (g_dl_state == DL_STOP) { res = 2; break; }
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(r, &avail)) { res = 1; break; }
                    if (!avail) break;          /* done */
                    if (avail > (1 << 16)) avail = 1 << 16;
                    DWORD rd = 0;
                    if (!WinHttpReadData(r, buf, avail, &rd) || !rd) { res = 1; break; }
                    fwrite(buf, 1, rd, fp); got += rd; *existing = got;
                    ULONGLONG now = GetTickCount64(); int tick = 0;
                    if (now - sp_last >= 700) {
                        double dt = (now - sp_last) / 1000.0;
                        double bps = dt > 0 ? (double)(got - sp_bytes) / dt : 0;
                        sp_last = now; sp_bytes = got;
                        if (bps >= 1048576.0) _snprintf(spbuf, sizeof(spbuf), "%.1f MB/s", bps / 1048576.0);
                        else                  _snprintf(spbuf, sizeof(spbuf), "%.0f KB/s", bps / 1024.0);
                        tick = 1;
                    }
                    if (total > 0) {
                        int pct = (int)(got * 100 / total);
                        if (pct != last_pct) { ui_progress(pct); last_pct = pct; tick = 1; }
                        if (tick) {
                            char st[160];
                            _snprintf(st, sizeof(st),
                                g_lang ? "Downloading: %d%%  %s  (%lld / %lld MiB)"
                                       : "Загрузка: %d%%  %s  (%lld / %lld МиБ)",
                                pct, spbuf, got / (1024 * 1024), total / (1024 * 1024));
                            ui_status(st);
                        }
                        if (pct / 2 != last_dec) {
                            last_dec = pct / 2;
                            ui_log(g_lang ? "Download: %d%%  %s  (%lld / %lld MiB)"
                                          : "Загрузка: %d%%  %s  (%lld / %lld МиБ)",
                                   pct, spbuf, got / (1024 * 1024), total / (1024 * 1024));
                        }
                    } else if (tick) {
                        char st[128];
                        _snprintf(st, sizeof(st),
                            g_lang ? "Downloading: %s  (%lld MiB)" : "Загрузка: %s  (%lld МиБ)",
                            spbuf, got / (1024 * 1024));
                        ui_status(st);
                    }
                }
                free(buf); fclose(fp);
            }
        }
    }
    WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return res;
}

/* full download with resume + network retry; runs in a worker thread */
static void do_download(const char *url) {
    char base[260]; url_basename(url, base, sizeof(base));
    char path[MAX_PATH]; app_path(path, sizeof(path), base);
    long long existing = 0;
    { HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
      if (h != INVALID_HANDLE_VALUE) { LARGE_INTEGER sz; if (GetFileSizeEx(h, &sz)) existing = sz.QuadPart; CloseHandle(h); } }

    ui_log(g_lang ? "Download: %s" : "Загрузка: %s", base);
    if (existing > 0) ui_log(g_lang ? "  resuming from %lld MiB" : "  докачка с %lld МиБ",
                             existing / (1024 * 1024));
    ui_status(g_lang ? "Downloading…" : "Загрузка…");
    g_dl_state = DL_RUN;

    int retries = 0;
    for (;;) {
        int res = download_once(url, path, &existing);
        if (res == 0) {
            ui_log(g_lang ? "Download complete: %s" : "Загрузка завершена: %s", path);
            ui_status(g_lang ? "Download complete" : "Загрузка завершена");
            strncpy(g_firmware, path, sizeof(g_firmware) - 1); g_firmware[sizeof(g_firmware) - 1] = 0;
            update_footer();
            ui_log("%s", g_lang ? "Set as firmware. Menu > Start flashing."
                                : "Выбрано как прошивка. Меню > Запустить прошивку.");
            break;
        }
        if (res == 2) { ui_log("%s", g_lang ? "Download stopped (partial kept for resume)."
                                            : "Загрузка остановлена (частично сохранено для докачки).");
                        ui_status(g_lang ? "Stopped" : "Остановлено"); break; }
        if (res == 3 || res == 4) { ui_log("%s", g_lang ? "Download failed." : "Загрузка не удалась."); break; }
        if (g_dl_state == DL_STOP) { ui_log("%s", g_lang ? "Download stopped." : "Загрузка остановлена."); break; }
        if (++retries > 15) { ui_log("%s", g_lang ? "Network error — paused. Search again to resume."
                                                  : "Ошибка сети — пауза. Повторите поиск для докачки."); break; }
        ui_log(g_lang ? "Network error — retry %d…" : "Ошибка сети — повтор %d…", retries);
        Sleep(3000);
    }
    g_dl_state = DL_IDLE;
}

/* small model -> codename map (best-effort; codename input is always reliable) */
static const char *model_to_codename(const char *lo) {
    static const char *map[][2] = {
        {"xiaomi 11t", "agate"}, {"11t", "agate"},
        {"xiaomi 11t pro", "vili"}, {"11t pro", "vili"},
        {"xiaomi 12", "cupid"}, {"xiaomi 12 pro", "zeus"},
        {"xiaomi 13", "fuxi"}, {"xiaomi 13 pro", "nuwa"},
        {"redmi note 7", "lavender"}, {"redmi note 8", "ginkgo"},
        {"redmi note 8 pro", "begonia"}, {"redmi note 9", "merlin"},
        {"redmi note 9 pro", "joyeuse"}, {"redmi note 10 pro", "sweet"},
        {"redmi note 11 pro", "pissarro"}, {"redmi note 12 pro", "ruby"},
        {"redmi 9t", "lime"}, {"redmi 9", "lancelot"},
        {"poco f3", "alioth"}, {"poco x3 pro", "vayu"}, {"poco f4", "munch"},
        {"redmi note 10", "mojito"}, {"redmi note 12", "tapas"},
    };
    for (int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++)
        if (strstr(lo, map[i][0])) return map[i][1];
    return NULL;
}

/* firmware search thread: resolve codename/region, list ezbox recovery ROMs, download pick */
/* build a URL on the chosen mirror host (stored URLs are bigota.d.miui.com/...) */
static void build_dl_url(int idx, int mi, char *out, int n) {
    int m = (mi >= 0 && mi < (int)(sizeof(MIRRORS) / sizeof(MIRRORS[0]))) ? mi : 0;
    _snprintf(out, n, "https://%s/%s/%s", MIRRORS[m], g_res_ver[idx], g_res_file[idx]);
}

/* read a JSON string starting at the opening quote; unescapes \/ \" ; returns ptr after */
static const char *read_jstr(const char *p, char *out, int n) {
    if (*p != '"') { out[0] = 0; return p; }
    p++; int i = 0;
    while (*p && *p != '"' && i < n - 1) {
        if (*p == '\\' && p[1]) { p++; out[i++] = (*p == 'n') ? ' ' : *p; }
        else out[i++] = *p;
        p++;
    }
    out[i] = 0; if (*p == '"') p++;
    return p;
}

/* download worker: pulls URL from g_dl_url */
static DWORD WINAPI dl_thread(LPVOID p) {
    (void)p;
    do_download(g_dl_url);
    PostMessage(g_main, WM_APP_DONE, 0, 0);
    return 0;
}

/* parse hub.miuier.com V3 device JSON -> results (region/carrier + version + recovery file) */
static int parse_miuier_device(const char *json) {
    g_res_count = 0;
    const char *p = json;
    char region[24] = "", carrier[24] = "", pend[48] = "";
    for (;;) {
        const char *pr = strstr(p, "\"region\":");
        const char *pc = strstr(p, "\"carrier\":");
        const char *pm = strstr(p, "\"miui\":");
        const char *pv = strstr(p, "\"recovery\":");
        const char *nx = NULL; int which = 0;
        if (pr && (!nx || pr < nx)) { nx = pr; which = 1; }
        if (pc && (!nx || pc < nx)) { nx = pc; which = 2; }
        if (pm && (!nx || pm < nx)) { nx = pm; which = 3; }
        if (pv && (!nx || pv < nx)) { nx = pv; which = 4; }
        if (!nx) break;
        const char *q = strchr(nx, ':'); if (!q) break; q++;
        while (*q==' '||*q=='\n'||*q=='\r'||*q=='\t') q++;
        if (which == 1) { p = read_jstr(q, region, sizeof(region)); }
        else if (which == 2) { while (*q=='['||*q==' '||*q=='\n'||*q=='\r'||*q=='\t') q++;
                               if (*q == '"') p = read_jstr(q, carrier, sizeof(carrier));
                               else { carrier[0] = 0; p = q; } }
        else if (which == 3) { p = read_jstr(q, pend, sizeof(pend)); }
        else { char file[160]; p = read_jstr(q, file, sizeof(file));
               if (pend[0] && g_res_count < MAXRES) {
                   strncpy(g_res_ver[g_res_count], pend, 47); g_res_ver[g_res_count][47] = 0;
                   strncpy(g_res_file[g_res_count], file, 159); g_res_file[g_res_count][159] = 0;
                   if (carrier[0]) _snprintf(g_res_reg[g_res_count], 24, "%s/%s", region, carrier);
                   else { strncpy(g_res_reg[g_res_count], region, 23); g_res_reg[g_res_count][23] = 0; }
                   g_res_count++;
               }
               pend[0] = 0; }
    }
    return g_res_count;
}

/* resolve a model name (or codename) to a codename via hub index.json */
static int resolve_model(const char *qlo, char *out, int n) {
    char *idx = fetch_url_text("https://api.miuier.com/api/v3/index.json");
    if (!idx) return 0;
    int found = 0; char best[64] = "";
    const char *p = idx;
    while ((p = strstr(p, "\"device\":")) != NULL) {
        const char *q = strchr(p, ':'); q++; while (*q==' '||*q=='\n'||*q=='\r'||*q=='\t') q++;
        char cn[64]; read_jstr(q, cn, sizeof(cn));
        const char *nd = strstr(p + 1, "\"device\":");
        const char *en = strstr(p, "\"en\":");
        if (en && (!nd || en < nd)) {
            const char *q2 = strchr(en, ':'); q2++; while (*q2==' '||*q2=='\n'||*q2=='\r'||*q2=='\t') q2++;
            char model[96]; read_jstr(q2, model, sizeof(model));
            char ml[96]; strncpy(ml, model, 95); ml[95] = 0; for (char *x = ml; *x; x++) *x = (char)tolower((unsigned char)*x);
            char cl[64]; strncpy(cl, cn, 63); cl[63] = 0; for (char *x = cl; *x; x++) *x = (char)tolower((unsigned char)*x);
            if (!strcmp(qlo, cl)) { strncpy(out, cn, n - 1); out[n - 1] = 0; found = 1; break; }
            if (!best[0] && (strstr(ml, qlo) || (strlen(qlo) >= 3 && strstr(qlo, ml)))) { strncpy(best, cn, 63); best[63] = 0; }
        }
        p += 1;
    }
    if (!found && best[0]) { strncpy(out, best, n - 1); out[n - 1] = 0; found = 1; }
    free(idx);
    return found;
}

/* build unique region-label list from results */
static void build_regions(void) {
    g_region_count = 0;
    for (int i = 0; i < g_res_count; i++) {
        int dup = 0;
        for (int j = 0; j < g_region_count; j++) if (!strcmp(g_regions[j], g_res_reg[i])) { dup = 1; break; }
        if (!dup && g_region_count < 64) { strncpy(g_regions[g_region_count], g_res_reg[i], 23);
                                           g_regions[g_region_count][23] = 0; g_region_count++; }
    }
}

/* fetch all ROMs for a codename: hub.miuier.com (all regions/carriers), ezbox fallback */
static int fw_fetch(const char *codename) {
    g_res_count = 0;
    char url[256];
    _snprintf(url, sizeof(url), "https://api.miuier.com/api/v3/devices/%s.json", codename);
    char *j = fetch_url_text(url);
    if (j) { parse_miuier_device(j); free(j); }
    if (g_res_count > 0) {
        ui_log(g_lang ? "hub.miuier.com: %d recovery ROMs (all regions/carriers)"
                      : "hub.miuier.com: %d recovery-прошивок (все регионы/операторы)", g_res_count);
        return g_res_count;
    }
    ui_log("%s", g_lang ? "hub empty — trying ezbox…" : "hub пуст — пробую ezbox…");
    const char *regs[] = { "global", "eea", "ru", "in", "tw", "id" };
    for (int i = 0; i < 6; i++) {
        char u[256];
        _snprintf(u, sizeof(u), "https://mirom.ezbox.idv.tw/en/phone/%s/roms-%s-stable/", codename, regs[i]);
        char *h = fetch_url_text(u); if (!h) continue;
        static char urls[40][512]; int mn = extract_recovery_urls(h, urls, 40); free(h);
        for (int k = 0; k < mn && g_res_count < MAXRES; k++) {
            url_version(urls[k], g_res_ver[g_res_count], 48);
            url_basename(urls[k], g_res_file[g_res_count], 160);
            strncpy(g_res_reg[g_res_count], regs[i], 23); g_res_reg[g_res_count][23] = 0;
            g_res_count++;
        }
    }
    if (g_res_count > 0) ui_log(g_lang ? "ezbox: %d recovery ROMs" : "ezbox: %d recovery-прошивок", g_res_count);
    return g_res_count;
}

/* firmware search: resolve codename/region, list ezbox recovery ROMs into the
 * results table, then hand off to the UI to let the user choose. */
static DWORD WINAPI fw_search_thread(LPVOID param) {
    char q[160] = {0};
    if (param) { strncpy(q, (char *)param, 159); free(param); }

    char codename[64] = {0}, target_ver[64] = {0}; int want_exact = 0;
    g_dev_curver[0] = 0; g_dev_region[0] = 0;

    /* device gives codename + region + current version */
    { adb_dev d; char err[128], ban[512];
      if (dev_open(&d, err, sizeof(err)) == 0 && adb_connect(&d, ban, sizeof(ban)) == 0) {
          char dev[64] = {0}; adb_service(&d, "getdevice:", dev, sizeof(dev));
          adb_service(&d, "getversion:", g_dev_curver, sizeof(g_dev_curver));
          dev_close(&d);
          int i = 0; for (; dev[i] && dev[i] != '_' && i < 63; i++) codename[i] = dev[i]; codename[i] = 0;
          char lo[64]; strncpy(lo, dev, 63); lo[63] = 0;
          for (char *p = lo; *p; p++) *p = (char)tolower((unsigned char)*p);
          if (strstr(lo, "eea")) strcpy(g_dev_region, "eea");
          else if (strstr(lo, "russia") || strstr(lo, "_ru")) strcpy(g_dev_region, "ru");
          else if (strstr(lo, "india")  || strstr(lo, "_in")) strcpy(g_dev_region, "in");
          else if (strstr(lo, "taiwan") || strstr(lo, "_tw")) strcpy(g_dev_region, "tw");
          else if (strstr(lo, "global")) strcpy(g_dev_region, "global");
      }
    }

    /* query can override codename / exact version */
    if (q[0]) {
        char lo[160]; strncpy(lo, q, 159); lo[159] = 0;
        for (char *p = lo; *p; p++) *p = (char)tolower((unsigned char)*p);
        int isver = ((q[0] == 'V' || q[0] == 'v' || !_strnicmp(q, "OS", 2)) && strchr(q, '.'));
        if (isver) { strncpy(target_ver, q, 63); target_ver[63] = 0; want_exact = 1;
                     for (char *p = target_ver; *p; p++) *p = (char)toupper((unsigned char)*p); }
        else {
            char cn[64] = "";
            if (resolve_model(lo, cn, sizeof(cn)) || (model_to_codename(lo) && strcpy(cn, model_to_codename(lo)))) {
                strncpy(codename, cn, 63); codename[63] = 0;
            } else {
                int i = 0; for (; lo[i] && lo[i] != ' ' && i < 63; i++) codename[i] = lo[i]; codename[i] = 0;
            }
        }
    }

    if (!codename[0]) {
        ui_log("%s", g_lang ? "No device/codename. Connect the phone or type a model/codename (e.g. Xiaomi 11T / agate)."
                            : "Нет устройства/кодового имени. Подключите телефон или введите модель/кодовое имя (напр. Xiaomi 11T / agate).");
        g_res_count = 0; goto post;
    }
    strncpy(g_res_codename, codename, 63); g_res_codename[63] = 0;
    ui_log(g_lang ? "Searching firmware: codename=%s" : "Поиск прошивки: кодовое имя=%s", codename);

    if (fw_fetch(codename) == 0) {
        ui_log("%s", g_lang ? "Nothing found. Check the codename (e.g. agate) or open the site."
                            : "Ничего не найдено. Проверьте кодовое имя (напр. agate) или откройте сайт.");
        goto post;
    }

    /* current / requested version match */
    g_res_current = -1;
    if (want_exact) {
        for (int i = 0; i < g_res_count; i++) if (!_stricmp(g_res_ver[i], target_ver)) { g_res_current = i; break; }
    }
    if (g_res_current < 0)
        for (int i = 0; i < g_res_count; i++) if (g_dev_curver[0] && !_stricmp(g_res_ver[i], g_dev_curver)) { g_res_current = i; break; }

    build_regions();
    /* default region: current's region, else device region prefix, else first */
    g_region_sel = 0;
    if (g_res_current >= 0) {
        for (int j = 0; j < g_region_count; j++) if (!strcmp(g_regions[j], g_res_reg[g_res_current])) { g_region_sel = j; break; }
    } else if (g_dev_region[0]) {
        for (int j = 0; j < g_region_count; j++)
            if (!_strnicmp(g_regions[j], g_dev_region, strlen(g_dev_region))) { g_region_sel = j; break; }
    }
    if (g_res_current >= 0)
        ui_log(g_lang ? "Current/requested %s found (region %s) — preselected."
                      : "Текущая/запрошенная %s найдена (регион %s) — выбрана по умолчанию.",
               g_res_ver[g_res_current], g_res_reg[g_res_current]);
    else if (g_dev_curver[0])
        ui_log(g_lang ? "Current build %s not in data — latest preselected."
                      : "Текущей сборки %s нет в данных — выбрана последняя.", g_dev_curver);

post:
    PostMessage(g_main, WM_APP_RESULTS, 0, 0);
    return 0;
}

/* ============================================================ worker threads */
#define FLASH_MAX_TRIES 3

static DWORD WINAPI flash_thread(LPVOID param) {
    (void)param;
    char err[256], banner[512];
    char *validate = NULL;       /* cached across retries (token is stable) */
    int erase = 0, allow_wipe = 0, ok = 0;

    for (int attempt = 1; attempt <= FLASH_MAX_TRIES && !ok; attempt++) {
        adb_dev d;
        if (attempt > 1)
            ui_log(g_lang ? "--- retry %d/%d ---" : "--- попытка %d/%d ---",
                   attempt, FLASH_MAX_TRIES);

        ui_status(L(S_ST_CONNECTING));
        int r = dev_open(&d, err, sizeof(err));
        if (r) { ui_log("ERROR: %s", err);
                 if (attempt == 1) { ui_log("%s", g_lang ? "Devices seen:" : "Найденные устройства:"); diagnose_usb(); log_conn_help(); }
                 ui_status(L(S_ST_NOTCONN)); Sleep(3000); continue; }

        r = adb_connect(&d, banner, sizeof(banner));
        if (r) { ui_log("%s", g_lang ? "ERROR: handshake failed (not in MiAssistant?)"
                                      : "ОШИБКА: рукопожатие не удалось (не MiAssistant?)");
                 dev_close(&d); Sleep(3000); continue; }
        ui_log("Connected: %s", banner);

        dev_info fi;
        if (read_info(&d, &fi)) { ui_log("%s", g_lang ? "ERROR: failed to read device info"
                                                      : "ОШИБКА: не удалось прочитать информацию");
                                  dev_close(&d); Sleep(3000); continue; }
        if (attempt == 1) log_device_info(&fi);

        if (!validate) {   /* fetch token once; MD5 of a multi-GB ROM is slow */
            ui_status(L(S_ST_TOKEN));
            ui_log("%s", g_lang ? "Requesting Validate token..." : "Запрос токена Validate…");
            if (get_validate(&fi, g_firmware, &validate, &erase, err, sizeof(err))) {
                ui_log("ERROR: %s", err); dev_close(&d); break;  /* token error won't fix on retry */
            }
            ui_log("  validate : %.48s...", validate);
            allow_wipe = g_wipe || (erase == 1);
            if (erase == 1 && !g_wipe)
                ui_log("%s", g_lang
                    ? "Server requires a DATA WIPE for this ROM (cross-region) — wiping."
                    : "Сервер требует ОЧИСТКУ ДАННЫХ (кросс-регион) — данные будут стёрты.");
        }

        ui_status(L(S_ST_FLASHING));
        ui_log("%s %s%s", g_lang ? "Sideloading" : "Передача", g_firmware,
               allow_wipe ? "  [wipe]" : "");
        if (do_sideload(&d, g_firmware, validate, allow_wipe, err, sizeof(err))) {
            ui_log("ERROR: %s", err);
            ui_log("%s", g_lang ? "Link dropped mid-transfer — will reconnect and retry."
                                : "Обрыв связи в процессе — переподключаюсь и повторю.");
            dev_close(&d);
            Sleep(3500);       /* let the device re-enumerate */
            continue;
        }

        ui_log("%s", L(S_L_TRANSFERDONE));
        ui_status(L(S_ST_DONE));
        { char tmp[64]; adb_service(&d, "reboot:", tmp, sizeof(tmp)); }
        dev_close(&d);
        ok = 1;
    }

    if (validate) free(validate);
    if (!ok) {
        ui_log("%s", g_lang
            ? "Flashing failed after retries. Try another USB cable / a rear USB 2.0 "
              "port / no hub, and keep the phone screen on."
            : "Не удалось прошить после повторов. Попробуй другой USB-кабель / задний "
              "порт USB 2.0 / без хаба, и не давай экрану телефона гаснуть.");
        ui_status(L(S_ST_NOTCONN));
    }
    PostMessage(g_main, WM_APP_DONE, 0, 0);
    return 0;
}

/* phone info: quick connect, dump info, disconnect */
static DWORD WINAPI check_thread(LPVOID param) {
    (void)param;
    adb_dev d; char err[256], banner[512];
    g_verbose = 1;
    int r = dev_open(&d, err, sizeof(err));
    g_verbose = 0;
    if (r) { ui_log("%s", err); ui_log("%s", g_lang ? "Devices seen:" : "Найденные устройства:"); diagnose_usb(); log_conn_help();
             ui_status(r == 2 ? L(S_ST_BUSY) : L(S_ST_NODEV)); goto done; }
    r = adb_connect(&d, banner, sizeof(banner));
    if (r) { ui_log("%s", g_lang ? "Device found but not in sideload mode."
                                  : "Устройство найдено, но не в режиме sideload.");
             ui_status(L(S_ST_NODEV)); dev_close(&d); goto done; }
    dev_info fi;
    if (read_info(&d, &fi)) { ui_log("%s", g_lang ? "Connected but info read failed."
                                                  : "Подключено, но чтение информации не удалось.");
                              dev_close(&d); goto done; }
    ui_log("--- %s ---", g_lang ? "device ready" : "устройство готово");
    log_device_info(&fi);
    ui_status(L(S_ST_DEVREADY));
    dev_close(&d);
done:
    PostMessage(g_main, WM_APP_DONE, 0, 0);
    return 0;
}

/* ============================================================ presence monitor */
static int g_present = -1;
static void presence_tick(void) {
    char drv[64] = "";
    int now = probe_present(drv, sizeof(drv));
    if (now == g_present) return;
    g_present = now;
    if (now)
        ui_log(g_lang ? "[+] Device connected (driver=%s)"
                      : "[+] Устройство подключено (драйвер=%s)", drv[0] ? drv : "?");
    else
        ui_log(g_lang ? "[-] Device disconnected" : "[-] Устройство отключено");
}

/* ============================================================ DPI + fonts + layout */
#define S(v) MulDiv((v), (int)g_dpi, 96)
static HFONT g_smallfont = NULL;

static UINT query_dpi(HWND hwnd) {
    HMODULE u = GetModuleHandleW(L"user32");
    if (u) {
        typedef UINT (WINAPI *pfn)(HWND);
        pfn f = (pfn)GetProcAddress(u, "GetDpiForWindow");
        if (f) { UINT d = f(hwnd); if (d) return d; }
    }
    HDC dc = GetDC(hwnd);
    UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(hwnd, dc);
    return d ? d : 96;
}

static void rebuild_fonts(void) {
    if (g_font)      DeleteObject(g_font);
    if (g_logfont)   DeleteObject(g_logfont);
    if (g_smallfont) DeleteObject(g_smallfont);
    g_font = CreateFontW(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_logfont = CreateFontW(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    g_smallfont = CreateFontW(-MulDiv(8, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessage(g_status, WM_SETFONT, (WPARAM)g_font,      TRUE);
    SendMessage(g_prog,   WM_SETFONT, (WPARAM)g_font,      TRUE);
    SendMessage(g_log,    WM_SETFONT, (WPARAM)g_logfont,   TRUE);
    SendMessage(g_footer, WM_SETFONT, (WPARAM)g_smallfont, TRUE);
}

static void layout(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int m = S(12), gap = S(8);
    int fh = S(16);                       /* footer height */
    int y = m;

    MoveWindow(g_prog,   m, y, W - 2 * m, S(18), TRUE); y += S(18) + gap;
    MoveWindow(g_status, m, y, W - 2 * m, S(20), TRUE); y += S(20) + gap;

    int logh = H - y - m - fh - gap;
    if (logh < S(80)) logh = S(80);
    MoveWindow(g_log, m, y, W - 2 * m, logh, TRUE);

    MoveWindow(g_footer, m, H - m - fh, W - 2 * m, fh, TRUE);  /* bottom-left */
}

/* ============================================================ menu + language */
static void build_menu(void) {
    if (g_menu) DestroyMenu(g_menu);
    g_menu = CreateMenu();
    WCHAR w[160];

    HMENU m1 = CreatePopupMenu();
    Lw(S_OPEN, w, 160);      AppendMenuW(m1, MF_STRING, IDM_OPEN, w);
    Lw(S_INFO, w, 160);      AppendMenuW(m1, MF_STRING, IDM_INFO, w);
    Lw(S_SERVER, w, 160);    AppendMenuW(m1, MF_STRING, IDM_SERVER, w);
    Lw(S_START, w, 160);     AppendMenuW(m1, MF_STRING, IDM_START, w);
    AppendMenuW(m1, MF_SEPARATOR, 0, NULL);
    Lw(S_ABOUT, w, 160);     AppendMenuW(m1, MF_STRING, IDM_ABOUT, w);
    Lw(S_EXIT, w, 160);      AppendMenuW(m1, MF_STRING, IDM_EXIT, w);

    HMENU m2 = CreatePopupMenu();
    Lw(S_ADB_SET, w, 160);   AppendMenuW(m2, MF_STRING, IDM_ADB_SET, w);
    Lw(S_ADB_RESET, w, 160); AppendMenuW(m2, MF_STRING, IDM_ADB_RESET, w);
    Lw(S_ADB_CHECK, w, 160); AppendMenuW(m2, MF_STRING, IDM_ADB_CHECK, w);
    Lw(S_ADB_KILL, w, 160);  AppendMenuW(m2, MF_STRING, IDM_ADB_KILL, w);

    HMENU m3 = CreatePopupMenu();
    Lw(S_SET_WIPE, w, 160);
    AppendMenuW(m3, MF_STRING | (g_wipe ? MF_CHECKED : 0), IDM_SET_WIPE, w);
    Lw(S_SET_LOGS, w, 160);
    AppendMenuW(m3, MF_STRING | (g_savelogs ? MF_CHECKED : 0), IDM_SET_LOGS, w);
    Lw(S_SET_GENTLE, w, 160);
    AppendMenuW(m3, MF_STRING | (g_gentle ? MF_CHECKED : 0), IDM_SET_GENTLE, w);
    AppendMenuW(m3, MF_SEPARATOR, 0, NULL);
    HMENU ml = CreatePopupMenu();
    Lw(S_LANG_RU, w, 160);
    AppendMenuW(ml, MF_STRING | (g_lang == 0 ? MF_CHECKED : 0), IDM_LANG_RU, w);
    Lw(S_LANG_EN, w, 160);
    AppendMenuW(ml, MF_STRING | (g_lang == 1 ? MF_CHECKED : 0), IDM_LANG_EN, w);
    Lw(S_LANG, w, 160);      AppendMenuW(m3, MF_POPUP, (UINT_PTR)ml, w);

    Lw(S_MENU, w, 160);      AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)m1, w);
    Lw(S_ADB, w, 160);       AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)m2, w);
    HMENU mfw = CreatePopupMenu();
    Lw(S_FW_MIUIER, w, 160); AppendMenuW(mfw, MF_STRING, IDM_FW_MIUIER, w);
    Lw(S_FW_EZBOX, w, 160);  AppendMenuW(mfw, MF_STRING, IDM_FW_EZBOX, w);
    Lw(S_FW_SEARCH, w, 160); AppendMenuW(mfw, MF_STRING, IDM_FW_SEARCH, w);
    AppendMenuW(mfw, MF_SEPARATOR, 0, NULL);
    Lw(S_FW_PAUSE, w, 160);  AppendMenuW(mfw, MF_STRING, IDM_FW_PAUSE, w);
    Lw(S_FW_STOP, w, 160);   AppendMenuW(mfw, MF_STRING, IDM_FW_STOP, w);
    Lw(S_FW_MENU, w, 160);   AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)mfw, w);
    Lw(S_SETTINGS, w, 160);  AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)m3, w);

    SetMenu(g_main, g_menu);
    DrawMenuBar(g_main);
}

static void update_footer(void) {
    char buf[MAX_PATH + 64];
    if (g_firmware[0]) _snprintf(buf, sizeof(buf), "%s %s", L(S_FW), g_firmware);
    else               _snprintf(buf, sizeof(buf), "%s", L(S_NOFW));
    WCHAR *w = u8towide(buf); SetWindowTextW(g_footer, w); free(w);
}

static void apply_language(void) {
    build_menu();
    WCHAR t[160]; Lw(S_TITLE, t, 160); SetWindowTextW(g_main, t);
    update_footer();
}

static HRESULT CALLBACK about_tdcb(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR ref) {
    (void)wp; (void)ref;
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(hwnd, L"open", (LPCWSTR)lp, NULL, NULL, SW_SHOWNORMAL);
    return S_OK;
}
static void about_show(HWND owner) {
    static const WCHAR *ru =
        L"Официальная OTA-прошивка Xiaomi через режим MiAssistant.\n"
        L"Без разблокировки загрузчика. Только официальные подписанные прошивки.\n\n"
        L"© 2026 (Mansi)  <a href=\"" APP_GITHUB_URL L"\">Github</a>";
    static const WCHAR *en =
        L"Official Xiaomi OTA flashing via MiAssistant mode.\n"
        L"No bootloader unlock. Official signed firmware only.\n\n"
        L"© 2026 (Mansi)  <a href=\"" APP_GITHUB_URL L"\">Github</a>";
    TASKDIALOGCONFIG c; memset(&c, 0, sizeof(c));
    c.cbSize = sizeof(c);
    c.hwndParent = owner;
    c.hInstance = GetModuleHandle(NULL);
    c.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_HICON_MAIN;
    c.hMainIcon = (HICON)LoadImageW(GetModuleHandle(NULL), MAKEINTRESOURCEW(IDI_APP),
                                    IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    c.pszWindowTitle = L"Mi OTA Sideload";
    c.pszMainInstruction = L"Mi OTA Sideload";
    c.pszContent = g_lang ? en : ru;
    c.dwCommonButtons = TDCBF_OK_BUTTON;
    c.pfCallback = about_tdcb;
    TaskDialogIndirect(&c, NULL, NULL, NULL);
}

/* ============================================================ actions */
static void set_busy(int busy) {
    g_busy = busy;
    UINT f = busy ? MF_GRAYED : MF_ENABLED;
    int ids[] = { IDM_OPEN, IDM_INFO, IDM_SERVER, IDM_START,
                  IDM_ADB_SET, IDM_ADB_RESET, IDM_ADB_CHECK, IDM_ADB_KILL };
    for (int i = 0; i < (int)(sizeof(ids) / sizeof(ids[0])); i++)
        EnableMenuItem(g_menu, ids[i], MF_BYCOMMAND | f);
    DrawMenuBar(g_main);
}

static void log_append_u8(const char *utf8) {
    WCHAR *w = u8towide(utf8);
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)w);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    free(w);
}

static void pick_firmware(void) {
    OPENFILENAMEA ofn; char file[MAX_PATH] = {0};
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = "OTA firmware (*.zip)\0*.zip\0All files\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) {
        strncpy(g_firmware, file, sizeof(g_firmware) - 1);
        g_firmware[sizeof(g_firmware) - 1] = 0;
        update_footer();
        ui_log("%s %s", g_lang ? "Selected:" : "Выбрано:", g_firmware);
    }
}

static void pick_adb(void) {
    OPENFILENAMEA ofn; char file[MAX_PATH] = {0};
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = "adb.exe\0adb.exe\0Executable (*.exe)\0*.exe\0";
    ofn.lpstrFile = file; ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) {
        strncpy(g_adb_path, file, sizeof(g_adb_path) - 1);
        g_adb_path[sizeof(g_adb_path) - 1] = 0;
        ui_log("%s %s", g_lang ? "adb path set:" : "Путь adb задан:", g_adb_path);
    }
}

static void start_flash(void) {
    if (!g_firmware[0]) {
        WCHAR *w = u8towide(L(S_L_PICKFIRST));
        MessageBoxW(g_main, w, L"Mi OTA Sideload", MB_ICONINFORMATION);
        free(w); return;
    }
    if (g_wipe) {
        WCHAR *w = u8towide(L(S_L_WIPEWARN));
        int r = MessageBoxW(g_main, w, L"Mi OTA Sideload", MB_ICONWARNING | MB_YESNO);
        free(w);
        if (r != IDYES) return;
    }
    set_busy(1);
    SendMessage(g_prog, PBM_SETPOS, 0, 0);
    CloseHandle(CreateThread(NULL, 0, flash_thread, NULL, 0, NULL));
}

static void start_thread(LPTHREAD_START_ROUTINE fn) {
    if (g_busy) return;
    set_busy(1);
    CloseHandle(CreateThread(NULL, 0, fn, NULL, 0, NULL));
}

/* ============================================================ window proc */
static void fill_results_listbox(HWND h) {
    SendDlgItemMessageW(h, 201, LB_RESETCONTENT, 0, 0);
    int cursel = 0;
    for (int i = 0; i < g_filt_count; i++) {
        int gi = g_filt[i];
        const char *tag = "";
        if (gi == g_res_current && i == 0) tag = g_lang ? "  (current, latest)" : "  (текущая, последняя)";
        else if (gi == g_res_current)      { tag = g_lang ? "  (current)" : "  (текущая)"; cursel = i; }
        else if (i == 0)                   tag = g_lang ? "  (latest)"  : "  (последняя)";
        if (gi == g_res_current) cursel = i;
        char line[200]; _snprintf(line, sizeof(line), "%s%s", g_res_ver[gi], tag);
        WCHAR *wl = u8towide(line); SendDlgItemMessageW(h, 201, LB_ADDSTRING, 0, (LPARAM)wl); free(wl);
    }
    SendDlgItemMessageW(h, 201, LB_SETCURSEL, (WPARAM)cursel, 0);
}

static void apply_region_filter(int regidx) {
    g_filt_count = 0;
    if (regidx < 0 || regidx >= g_region_count) return;
    for (int i = 0; i < g_res_count && g_filt_count < MAXRES; i++)
        if (!strcmp(g_res_reg[i], g_regions[regidx])) g_filt[g_filt_count++] = i;
}

static INT_PTR CALLBACK ResultsDlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)l;
    switch (m) {
    case WM_INITDIALOG: {
        WCHAR t[192];
        Lw(S_FW_RESULTS_TITLE, t, 192); SetWindowTextW(h, t);
        Lw(S_FW_CHOOSE, t, 192); SetDlgItemTextW(h, 203, t);
        Lw(S_FW_REGION, t, 192); SetDlgItemTextW(h, 206, t);
        Lw(S_FW_SOURCE, t, 192); SetDlgItemTextW(h, 204, t);
        Lw(S_FW_DL_BTN, t, 192); SetDlgItemTextW(h, IDOK, t);
        Lw(S_FW_CANCEL, t, 192); SetDlgItemTextW(h, IDCANCEL, t);
        for (int i = 0; i < g_region_count; i++) {
            WCHAR *wr = u8towide(g_regions[i]); SendDlgItemMessageW(h, 205, CB_ADDSTRING, 0, (LPARAM)wr); free(wr);
        }
        SendDlgItemMessageW(h, 205, CB_SETCURSEL, (WPARAM)g_region_sel, 0);
        for (int i = 0; i < (int)(sizeof(MIRROR_NAMES) / sizeof(MIRROR_NAMES[0])); i++) {
            WCHAR *wm = u8towide(MIRROR_NAMES[i]); SendDlgItemMessageW(h, 202, CB_ADDSTRING, 0, (LPARAM)wm); free(wm);
        }
        SendDlgItemMessageW(h, 202, CB_SETCURSEL, 0, 0);
        apply_region_filter(g_region_sel);
        fill_results_listbox(h);
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD(w) == CBN_SELCHANGE && LOWORD(w) == 205) {
            g_region_sel = (int)SendDlgItemMessageW(h, 205, CB_GETCURSEL, 0, 0); if (g_region_sel < 0) g_region_sel = 0;
            apply_region_filter(g_region_sel);
            fill_results_listbox(h);
            return TRUE;
        }
        if (LOWORD(w) == IDOK) {
            if (g_filt_count <= 0) { EndDialog(h, 0); return TRUE; }
            int se = (int)SendDlgItemMessageW(h, 201, LB_GETCURSEL, 0, 0); if (se < 0) se = 0;
            g_res_sel = g_filt[se];
            int mm = (int)SendDlgItemMessageW(h, 202, CB_GETCURSEL, 0, 0); if (mm < 0) mm = 0; g_mirror_sel = mm;
            EndDialog(h, 1); return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(h, 0); return TRUE; }
        break;
    }
    return FALSE;
}

static char g_search_out[128];
static INT_PTR CALLBACK SearchDlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)l;
    switch (m) {
    case WM_INITDIALOG: {
        WCHAR t[192]; Lw(S_FW_DLG_TITLE, t, 192); SetWindowTextW(h, t);
        Lw(S_FW_DLG_LABEL, t, 192); SetDlgItemTextW(h, 102, t);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) { GetDlgItemTextA(h, IDC_SEARCH_EDIT, g_search_out, sizeof(g_search_out)); EndDialog(h, 1); return TRUE; }
        if (LOWORD(w) == IDCANCEL) { EndDialog(h, 0); return TRUE; }
        break;
    }
    return FALSE;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_main = hwnd;                 /* needed now: build_menu/ui_log use g_main */
        g_prog   = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
                                   0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_PROG,
                                   GetModuleHandle(NULL), NULL);
        SendMessage(g_prog, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUS,
                                   GetModuleHandle(NULL), NULL);
        g_log    = CreateWindowExW(0, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | ES_MULTILINE |
                                   ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_BORDER,
                                   0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_LOG,
                                   GetModuleHandle(NULL), NULL);
        g_footer = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_FOOTER,
                                   GetModuleHandle(NULL), NULL);
        g_dpi = query_dpi(hwnd);
        rebuild_fonts();
        { WCHAR *w = u8towide(L(S_ST_READY)); SetWindowTextW(g_status, w); free(w); }
        build_menu();
        update_footer();
        layout(hwnd);
        SetTimer(hwnd, 1, 1500, NULL);
        presence_tick();
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN:  pick_firmware(); return 0;
        case IDM_INFO:
            if (!g_busy) { ui_log("%s", g_lang ? "Reading phone info..." : "Чтение информации о телефоне…");
                           start_thread(check_thread); }
            return 0;
        case IDM_SERVER:
            if (!g_busy) { ui_log("%s", g_lang ? "Checking server..." : "Проверка сервера…");
                           start_thread(server_check_thread); }
            return 0;
        case IDM_FW_MIUIER:
            ShellExecuteW(hwnd, L"open", L"https://roms.miuier.com/en-us/", NULL, NULL, SW_SHOWNORMAL);
            return 0;
        case IDM_FW_EZBOX:
            ShellExecuteW(hwnd, L"open", L"https://mirom.ezbox.idv.tw/en/phone/", NULL, NULL, SW_SHOWNORMAL);
            return 0;
        case IDM_FW_SEARCH:
            if (g_busy) return 0;
            if (DialogBoxParamW(GetModuleHandle(NULL), MAKEINTRESOURCEW(IDD_SEARCH), hwnd, SearchDlgProc, 0) == 1) {
                set_busy(1);
                CloseHandle(CreateThread(NULL, 0, fw_search_thread, _strdup(g_search_out), 0, NULL));
            }
            return 0;
        case IDM_FW_PAUSE:
            if (g_dl_state == DL_RUN) g_dl_state = DL_PAUSE;
            else if (g_dl_state == DL_PAUSE) g_dl_state = DL_RUN;
            return 0;
        case IDM_FW_STOP:
            if (g_dl_state == DL_RUN || g_dl_state == DL_PAUSE) g_dl_state = DL_STOP;
            return 0;
        case IDM_START:  start_flash(); return 0;
        case IDM_ABOUT:
            about_show(hwnd);
            return 0;
        case IDM_EXIT:   DestroyWindow(hwnd); return 0;

        case IDM_ADB_SET:   pick_adb(); return 0;
        case IDM_ADB_RESET:
            g_adb_path[0] = 0;
            ui_log("%s %s", g_lang ? "adb path reset to" : "Путь adb сброшен на",
                   L(S_ADBPATH_DEFAULT));
            return 0;
        case IDM_ADB_CHECK: if (!g_busy) start_thread(adb_check_thread); return 0;
        case IDM_ADB_KILL:  run_kill_adb_server(); return 0;

        case IDM_SET_WIPE:
            g_wipe = !g_wipe;
            CheckMenuItem(g_menu, IDM_SET_WIPE, MF_BYCOMMAND | (g_wipe ? MF_CHECKED : MF_UNCHECKED));
            return 0;
        case IDM_SET_LOGS:
            g_savelogs = !g_savelogs;
            CheckMenuItem(g_menu, IDM_SET_LOGS, MF_BYCOMMAND | (g_savelogs ? MF_CHECKED : MF_UNCHECKED));
            if (g_savelogs) { open_logfile();
                ui_log("%s Logs\\MiSideload-%s.log",
                       g_lang ? "Logging to" : "Логи пишутся в", g_launch_stamp); }
            else            { ui_log("%s", g_lang ? "Logging stopped." : "Запись логов остановлена.");
                              close_logfile(); }
            return 0;
        case IDM_SET_GENTLE:
            g_gentle = !g_gentle;
            CheckMenuItem(g_menu, IDM_SET_GENTLE, MF_BYCOMMAND | (g_gentle ? MF_CHECKED : MF_UNCHECKED));
            return 0;
        case IDM_LANG_RU: if (g_lang != 0) { g_lang = 0; apply_language(); } return 0;
        case IDM_LANG_EN: if (g_lang != 1) { g_lang = 1; apply_language(); } return 0;
        }
        return 0;

    case WM_TIMER:        if (wp == 1 && !g_busy) presence_tick(); return 0;
    case WM_DEVICECHANGE: if (!g_busy) presence_tick(); return 0;
    case WM_SIZE:         layout(hwnd); return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = S(520);
        mmi->ptMinTrackSize.y = S(380);
        return 0;
    }

    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        RECT *pr = (RECT *)lp;
        SetWindowPos(hwnd, NULL, pr->left, pr->top,
                     pr->right - pr->left, pr->bottom - pr->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        rebuild_fonts();
        layout(hwnd);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HWND ctl = (HWND)lp;
        if (ctl == g_log) break;      /* read-only EDIT: let it paint opaque (no bold buildup) */
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_APP_LOG:
        log_append_u8((char *)lp); free((void *)lp); return 0;
    case WM_APP_STATUS: {
        WCHAR *w = u8towide((char *)lp);
        SetWindowTextW(g_status, w); free(w); free((void *)lp); return 0;
    }
    case WM_APP_PROG:
        SendMessage(g_prog, PBM_SETPOS, (WPARAM)(int)wp, 0); return 0;
    case WM_APP_RESULTS:
        set_busy(0);
        if (g_res_count > 0 &&
            DialogBoxParamW(GetModuleHandle(NULL), MAKEINTRESOURCEW(IDD_RESULTS), hwnd, ResultsDlgProc, 0) == 1) {
            char url[700]; build_dl_url(g_res_sel, g_mirror_sel, url, sizeof(url));
            strncpy(g_dl_url, url, sizeof(g_dl_url) - 1); g_dl_url[sizeof(g_dl_url) - 1] = 0;
            ui_log(g_lang ? "Selected %s [%s] via %s" : "Выбрано %s [%s] через %s",
                   g_res_ver[g_res_sel], g_res_reg[g_res_sel], MIRROR_NAMES[g_mirror_sel]);
            set_busy(1);
            CloseHandle(CreateThread(NULL, 0, dl_thread, NULL, 0, NULL));
        }
        return 0;
    case WM_APP_DONE:
        set_busy(0); return 0;

    case WM_CLOSE:
        if (g_busy) {
            WCHAR *w = u8towide(g_lang ? "Operation in progress. Quit anyway?"
                                       : "Идёт операция. Всё равно выйти?");
            int r = MessageBoxW(hwnd, w, L"Mi OTA Sideload", MB_ICONWARNING | MB_YESNO);
            free(w);
            if (r != IDYES) return 0;
        }
        DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        close_logfile();
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev; (void)cmd;
    InitializeCriticalSection(&g_logcs);

    SYSTEMTIME st; GetLocalTime(&st);
    _snprintf(g_launch_stamp, sizeof(g_launch_stamp), "%02d.%02d.%02d-%02d.%02d",
              st.wDay, st.wMonth, st.wYear % 100, st.wHour, st.wMinute);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"MiSideloadWnd";
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   16, 16, LR_SHARED);
    RegisterClassExW(&wc);

    WCHAR title[160]; Lw(S_TITLE, title, 160);
    g_main = CreateWindowExW(0, L"MiSideloadWnd", title, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 680, 560,
                             NULL, NULL, hInst, NULL);
    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    ui_log("%s", L(S_READY));
    ui_log("%s", g_lang ? "MiAssistant mode -> ADB > Kill server -> Menu > Phone info -> Open file -> Start flashing"
                        : "Режим MiAssistant -> ADB > Kill server -> Меню > Инфо о телефоне -> Открыть файл -> Запустить прошивку");

    MSG m;
    while (GetMessage(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessage(g_main, &m)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
    }
    DeleteCriticalSection(&g_logcs);
    return 0;
}
