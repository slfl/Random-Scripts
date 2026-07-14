/*
 * GoodFon Wallpaper Changer — C edition (Win32)
 * ------------------------------------------------------------------
 * Резидентное приложение с иконкой в трее:
 *   - таймер смены обоев (интервал настраивается из меню);
 *   - плавная смена обоев через IActiveDesktop (как в Python-версии);
 *   - логин на goodfon (JSON-ответ) + кэш cookie per-domain (com/ru);
 *   - скачивание случайной картинки нужного разрешения по теме;
 *   - избранное: favorite/unfavorite (локально + на сайте);
 *   - уведомления в трее (balloon) с подробностями;
 *   - автозапуск (реестр Run) переключается из меню;
 *   - режимы CLI: GoodFon.exe update|favorite|unfavorite — разовый запуск без трея.
 *
 * Сборка (MSVC):  cmake -B build && cmake --build build --config Release
 * Линкуется с:    winhttp shell32 ole32 user32 advapi32 shlwapi gdi32 windowscodecs
 * Кодировка:      исходник UTF-8; для MSVC включён /utf-8 в CMakeLists.
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define COBJMACROS
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <dpapi.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "resource.h"

/* ================= Константы ================= */

#define APP_NAME        L"GoodFon"
#define APP_VERSION     "2.2"
#define WM_TRAYICON     (WM_APP + 1)
#define TIMER_ID        1
#define UPD_TIMER_ID    2

/* Обновление с GitHub (raw): сравниваем хэш локального exe с удалённым. */
#define UPDATE_BASE  L"https://github.com/slfl/Random-Scripts/raw/refs/heads/main/GoodFon"
#if defined(_WIN64)
#define UPDATE_EXE_URL  UPDATE_BASE L"/GoodFon-x64.exe"
#else
#define UPDATE_EXE_URL  UPDATE_BASE L"/GoodFon-x86.exe"
#endif

/* ================= Язык интерфейса и логов ================= */
enum { LANG_RU = 0, LANG_EN = 1 };
static int g_lang = LANG_RU;
/* Вернуть строку по текущему языку. Литералы передаются парой (ru, en). */
static const char  *T (const char *ru, const char *en)  { return g_lang == LANG_EN ? en : ru; }
static const WCHAR *TW(const WCHAR *ru, const WCHAR *en) { return g_lang == LANG_EN ? en : ru; }

/* ================= Тема оформления окна настроек (светлая/тёмная) ========= */
enum { THEME_LIGHT = 0, THEME_DARK = 1 };
static int    g_ui_theme = THEME_LIGHT;
static HBRUSH g_br_bg  = NULL;   /* фон окна/статики */
static HBRUSH g_br_ctl = NULL;   /* фон полей ввода/списка */
static HBRUSH g_br_nav = NULL;   /* фон левой навигации */
static COLORREF cr_bg (void){ return g_ui_theme ? RGB(32,32,32)  : RGB(243,243,243); }
static COLORREF cr_nav(void){ return g_ui_theme ? RGB(43,43,43)  : RGB(235,235,235); }
static COLORREF cr_ctl(void){ return g_ui_theme ? RGB(45,45,45)  : RGB(255,255,255); }
static COLORREF cr_txt(void){ return g_ui_theme ? RGB(240,240,240): RGB(0,0,0);       }
static COLORREF cr_sel(void){ return g_ui_theme ? RGB(60,60,60)   : RGB(214,228,247); }
static COLORREF cr_accent(void){ return RGB(0,120,215); }
static COLORREF cr_border(void)  { return g_ui_theme ? RGB(82,82,82)  : RGB(205,205,205); }
static COLORREF cr_btnface(void) { return g_ui_theme ? RGB(55,55,55)  : RGB(251,251,251); }
static COLORREF cr_btnpress(void){ return g_ui_theme ? RGB(72,72,72)  : RGB(229,229,229); }
static void theme_brushes_rebuild(void)
{
    if (g_br_bg)  DeleteObject(g_br_bg);
    if (g_br_ctl) DeleteObject(g_br_ctl);
    if (g_br_nav) DeleteObject(g_br_nav);
    g_br_bg  = CreateSolidBrush(cr_bg());
    g_br_ctl = CreateSolidBrush(cr_ctl());
    g_br_nav = CreateSolidBrush(cr_nav());
}


/* Пункты меню */
#define IDM_UPDATE      100
#define IDM_FAVORITE        101
#define IDM_UNFAVORITE      102
#define IDM_PAUSE       103
#define IDM_EXIT        105
#define IDM_LOGIN       111   /* внутреннее действие: асинхронный вход */
#define IDM_SETTINGS    115   /* открыть окно настроек */

/* Контролы окна настроек */
#define IDC_NAV         3001
#define IDC_CB_THEME    3002
#define IDC_CB_INTERVAL 3003
#define IDC_CB_RES      3004
#define IDC_CB_FAVN     3005
#define IDC_ED_LOGIN    3006
#define IDC_ED_PASS     3007
#define IDC_BTN_SIGNIN  3008
#define IDC_BTN_SIGNOUT 3009
#define IDC_LNK_REG     3010
#define IDC_CHK_NOTIFY  3011
#define IDC_CHK_AUTORUN 3012
#define IDC_CB_LANG     3013
#define IDC_CB_APPTHEME 3014
#define IDC_BTN_UPDATE  3015
#define IDC_PGTITLE     3016
#define IDC_BTN_CLOSE   3017
#define IDC_CB_DOMAIN   3018
#define IDC_ST_STATUS   3019
#define IDC_CB_UPDINT   3020
#define IDC_ST_UPDATE   3021

#define MAX_THEMES      64
#define JAR_SIZE        4096
#define BODY_LIMIT      (2*1024*1024)   /* максимум HTML в память */
#define IMG_LIMIT       (64*1024*1024)  /* максимум картинки      */

static const int g_intervals[8] = { 5, 10, 30, 60, 120, 360, 720, 1440 };
#define INTERVAL_COUNT 8
static const int g_favorite_ns[4]   = { 5, 10, 15, 20 };
#define FAVN_COUNT 4

/* Разрешения для меню: отображаемое имя -> значение в конфиг.
 * "original" — особый режим: берётся любое (самое большое) доступное.  */
typedef struct { const wchar_t *name; const char *value; } ResDef;
static const ResDef g_reses[] = {
    { L"HD (1280x720)",       "1280x720"  },
    { L"Full HD (1920x1080)", "1920x1080" },
    { L"2K (2560x1440)",      "2560x1440" },
    { L"4K (3840x2160)",      "3840x2160" },
    { L"8K (7680x4320)",      "7680x4320" },
    { L"Оригинал (любое)",    "original"  },
};
#define RES_COUNT (int)(sizeof(g_reses)/sizeof(g_reses[0]))

/* Встроенный список тем: slug для URL + имя для меню (ru/en).
 * anime/auto на goodfon живут на поддоменах — здесь не включены.       */
typedef struct { const char *slug; const wchar_t *name_ru; const wchar_t *name_en; } ThemeDef;
static const ThemeDef g_themes_all[] = {
    { "erotic",      L"Эротика",      L"Erotic"       },
    { "girls",       L"Девушки",      L"Girls"        },
    { "nature",      L"Природа",      L"Nature"       },
    { "landscapes",  L"Пейзажи",      L"Landscapes"   },
    { "hi-tech",     L"Hi-Tech",      L"Hi-Tech"      },
    { "abstraction", L"Абстракции",   L"Abstraction"  },
    { "aviation",    L"Авиация",      L"Aviation"     },
    { "city",        L"Город",        L"City"         },
    { "food",        L"Еда",          L"Food"         },
    { "painting",    L"Живопись",     L"Painting"     },
    { "animals",     L"Животные",     L"Animals"      },
    { "games",       L"Игры",         L"Games"        },
    { "ai-art",      L"ИИ арт",       L"AI Art"       },
    { "interior",    L"Интерьер",     L"Interior"     },
    { "space",       L"Космос",       L"Space"        },
    { "cats",        L"Кошки",        L"Cats"         },
    { "Love",        L"Любовь",       L"Love"         },
    { "macro",       L"Макро",        L"Macro"        },
    { "minimalism",  L"Минимализм",   L"Minimalism"   },
    { "men",         L"Мужчины",      L"Men"          },
    { "music",       L"Музыка",       L"Music"        },
    { "mood",        L"Настроения",   L"Mood"         },
    { "new-year",    L"Новый год",    L"New Year"     },
    { "weapon",      L"Оружие",       L"Weapon"       },
    { "holidays",    L"Праздники",    L"Holidays"     },
    { "miscellanea", L"Разное",       L"Miscellanea"  },
    { "rendering",   L"Рендеринг",    L"Rendering"    },
    { "situations",  L"Ситуации",     L"Situations"   },
    { "dog",         L"Собаки",       L"Dogs"         },
    { "sports",      L"Спорт",        L"Sports"       },
    { "style",       L"Стиль",        L"Style"        },
    { "textures",    L"Текстуры",     L"Textures"     },
    { "fantasy",     L"Фантастика",   L"Fantasy"      },
    { "films",       L"Фильмы",       L"Films"        },
    { "flowers",     L"Цветы",        L"Flowers"      },
};
#define THEME_COUNT (int)(sizeof(g_themes_all)/sizeof(g_themes_all[0]))

/* Имя темы по текущему языку. */
static const wchar_t *theme_name(int i)
{ return g_lang == LANG_EN ? g_themes_all[i].name_en : g_themes_all[i].name_ru; }

/* Сортировка тем по алфавиту (по отображаемому имени, локале-зависимо).
 * Сортируем массив ИНДЕКСОВ, чтобы ID пунктов меню по-прежнему указывали
 * на исходные записи g_themes_all. */
static int theme_cmp(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    int r = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE,
                           theme_name(ia), -1, theme_name(ib), -1);
    return r - 2;   /* CSTR_LESS_THAN(1)->-1, EQUAL(2)->0, GREATER(3)->1 */
}

/* Маркеры ответа логина/квоты (UTF-8, как приходит с сайта) */
#define QUOTA_MARKER_RU  "\xD0\xB8\xD1\x81\xD1\x87\xD0\xB5\xD1\x80\xD0\xBF\xD0\xB0\xD0\xBB\xD0\xB8" /* "исчерпали" */
#define LOGIN_FAIL_RU    "\xD0\xBD\xD0\xB5 \xD1\x83\xD0\xB3\xD0\xB0\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xB8" /* "не угадали" */

/* ================= Конфигурация ================= */

typedef struct {
    char login[128];
    char password[128];
    char session_com[JAR_SIZE];
    char session_ru[JAR_SIZE];
    char resolution[32];
    char theme[64];                     /* активная тема */
    char save_dir[MAX_PATH];
    int  max_files;
    int  favorite_every_n;
    int  max_attempts;
    int  notify;
    char domain_pref[8];                /* com / ru / auto */
    int  interval_min;
    int  update_interval_min;           /* автопроверка обновлений: 0=выкл, 60, 1440, 10080 */
    int  counter;
} Config;

static Config g_cfg;
static WCHAR  g_favorite_dir[MAX_PATH];
static WCHAR  g_current_image[MAX_PATH];  /* точный файл, который сейчас на экране */

/* Cookie-джар per-domain (наш собственный, WinHTTP-cookies отключены) */
static char g_jar_com[JAR_SIZE];
static char g_jar_ru[JAR_SIZE];

/* Активный домен: 0 = com, 1 = ru; -1 = не выбран */
static int g_active_domain = -1;
static const char *g_hosts[2] = { "www.goodfon.com", "www.goodfon.ru" };

/* Трей / состояние */
static HWND  g_hwnd;
static HWND  g_set_hwnd = NULL;         /* окно настроек (объявлено рано: нужно worker'у) */
static int   g_update_status = 0;       /* 0 нет, 1 проверка, 2 актуально, 3 ставится, 4 ошибка */
#define WM_APP_LOGINRESULT  (WM_APP + 3)
#define WM_APP_UPDATERESULT (WM_APP + 4)
#define WM_APP_STATS        (WM_APP + 5)

/* Профильная статистика с сайта (для страницы "Аккаунт"). */
typedef struct {
    int     valid;            /* 1 если загружена с сайта */
    char    rating[16];       /* напр. "5.00" */
    long    walls, downloads, comments;
    char    avatar_url[256];  /* полный URL .webp */
    HBITMAP avatar_bmp;       /* декодированная 32x32 (или NULL) */
} ProfileStats;
static ProfileStats g_stats;
static void stats_refresh_async(void);   /* fwd: нужен worker_thread'у */
static HINSTANCE g_hinst;
static NOTIFYICONDATAW g_nid;
static volatile LONG g_busy = 0;
static int g_paused = 0;
static int g_tray_mode = 0;
static int g_debug = 0;
static FILE *g_log = NULL;
static WCHAR g_log_path[MAX_PATH];

/* ================= Логирование ================= */

/* Логи включаются только при запуске с флагом -debug.
 * Без него файл не создаётся и ничего не пишется (релизный режим). */
static void log_open(int debug)
{
    g_debug = debug;
    if (!debug) { g_log = NULL; return; }

    /* Консоль родителя (запуск из терминала) либо файл goodfon.log */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        g_log = stdout;
        SetConsoleOutputCP(CP_UTF8);
    } else {
        GetModuleFileNameW(NULL, g_log_path, MAX_PATH);
        PathRemoveFileSpecW(g_log_path);
        PathAppendW(g_log_path, L"goodfon.log");
        /* простое ограничение размера: >1 МБ — обнуляем */
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(g_log_path, GetFileExInfoStandard, &fad) &&
            fad.nFileSizeLow > (1u << 20))
            _wfopen_s(&g_log, g_log_path, L"w, ccs=UTF-8");
        else
            _wfopen_s(&g_log, g_log_path, L"a, ccs=UTF-8");
    }
}

static void logf_(const char *level, const char *fmt, ...)
{
    if (!g_log) return;
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    time_t t = time(NULL);
    struct tm tmv; localtime_s(&tmv, &t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    if (g_log == stdout)
        fprintf(g_log, "%s %s: %s\n", ts, level, msg);
    else {
        /* файл открыт как UTF-16 текст (ccs) — конвертируем */
        WCHAR w[1200];
        char line[1200];
        snprintf(line, sizeof(line), "%s %s: %s\n", ts, level, msg);
        MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 1200);
        fputws(w, g_log);
    }
    fflush(g_log);
}
#define LOG_INFO(...)  logf_("INFO", __VA_ARGS__)
#define LOG_WARN(...)  logf_("WARNING", __VA_ARGS__)
#define LOG_ERROR(...) logf_("ERROR", __VA_ARGS__)

/* ================= Утилиты строк ================= */

static void utf8_to_wide(const char *s, WCHAR *out, int outsz)
{ MultiByteToWideChar(CP_UTF8, 0, s, -1, out, outsz); }

static void wide_to_utf8(const WCHAR *s, char *out, int outsz)
{ WideCharToMultiByte(CP_UTF8, 0, s, -1, out, outsz, NULL, NULL); }

static void str_trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
}

static int str_icontains(const char *hay, const char *needle)
{ return StrStrIA(hay, needle) != NULL; }

/* Извлечь значение атрибута после позиции p (учитывает кавычки и их отсутствие).
 * attr — строка вида "href=" или "value=" или "data-add=".               */
static int extract_attr(const char *p, const char *attr, char *out, size_t outsz)
{
    const char *a = strstr(p, attr);
    if (!a) return 0;
    a += strlen(attr);
    char q = 0;
    if (*a == '"' || *a == '\'') { q = *a; a++; }
    size_t i = 0;
    while (*a && i + 1 < outsz) {
        char c = *a;
        if (q ? (c == q) : (c == ' ' || c == '>' || c == '"' || c == '\'')) break;
        out[i++] = c; a++;
    }
    out[i] = 0;
    return i > 0;
}

/* ============ Хранилище настроек: реестр HKCU\Software\GoodFon ============ */

#define REG_PATH L"Software\\GoodFon"

static HKEY reg_open(REGSAM access)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, NULL, 0,
                        access, NULL, &k, NULL) != ERROR_SUCCESS)
        return NULL;
    return k;
}

static void reg_set_str(const WCHAR *name, const char *utf8val)
{
    HKEY k = reg_open(KEY_WRITE);
    if (!k) return;
    WCHAR w[JAR_SIZE];
    utf8_to_wide(utf8val, w, JAR_SIZE);
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)w,
                   (DWORD)((wcslen(w) + 1) * sizeof(WCHAR)));
    RegCloseKey(k);
}

static int reg_get_str(const WCHAR *name, char *out, int outsz)
{
    out[0] = 0;
    HKEY k = reg_open(KEY_READ);
    if (!k) return 0;
    WCHAR w[JAR_SIZE]; DWORD sz = sizeof(w), type = 0;
    int ok = 0;
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)w, &sz) == ERROR_SUCCESS &&
        type == REG_SZ) {
        wide_to_utf8(w, out, outsz);
        ok = 1;
    }
    RegCloseKey(k);
    return ok;
}

static void reg_set_dword(const WCHAR *name, int val)
{
    HKEY k = reg_open(KEY_WRITE);
    if (!k) return;
    DWORD v = (DWORD)val;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
    RegCloseKey(k);
}

static int reg_get_dword(const WCHAR *name, int def)
{
    HKEY k = reg_open(KEY_READ);
    if (!k) return def;
    DWORD v = 0, sz = sizeof(v), type = 0;
    int ok = (RegQueryValueExW(k, name, NULL, &type, (BYTE *)&v, &sz) == ERROR_SUCCESS &&
              type == REG_DWORD);
    RegCloseKey(k);
    return ok ? (int)v : def;
}

/* Пароль: DPAPI-шифрование, привязанное к учётке Windows.
 * В реестре хранится шифртекст (REG_BINARY), не открытый пароль.        */
static int reg_set_password(const char *plain)
{
    DATA_BLOB in, out;
    in.pbData = (BYTE *)plain;
    in.cbData = (DWORD)strlen(plain) + 1;   /* включая завершающий 0 */
    if (!CryptProtectData(&in, L"GoodFon password", NULL, NULL, NULL, 0, &out))
        return 0;
    HKEY k = reg_open(KEY_WRITE);
    if (k) {
        RegSetValueExW(k, L"password_enc", 0, REG_BINARY, out.pbData, out.cbData);
        RegCloseKey(k);
    }
    LocalFree(out.pbData);
    return 1;
}

static int reg_get_password(char *out, int outsz)
{
    out[0] = 0;
    HKEY k = reg_open(KEY_READ);
    if (!k) return 0;
    DWORD sz = 0, type = 0;
    if (RegQueryValueExW(k, L"password_enc", NULL, &type, NULL, &sz) != ERROR_SUCCESS ||
        sz == 0 || type != REG_BINARY) { RegCloseKey(k); return 0; }
    BYTE *buf = (BYTE *)malloc(sz);
    if (!buf) { RegCloseKey(k); return 0; }
    RegQueryValueExW(k, L"password_enc", NULL, &type, buf, &sz);
    RegCloseKey(k);
    DATA_BLOB in, dec;
    in.pbData = buf; in.cbData = sz;
    int ok = 0;
    if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &dec)) {
        strncpy(out, (char *)dec.pbData, outsz - 1);
        out[outsz - 1] = 0;
        LocalFree(dec.pbData);
        ok = 1;
    }
    free(buf);
    return ok;
}

/* Записать значения по умолчанию (когда реестр пуст). */
static void settings_write_defaults(void)
{
    reg_set_str(L"login", "");
    reg_set_str(L"resolution", "1920x1080");
    reg_set_str(L"theme", "nature");
    reg_set_str(L"save_dir", "GoodFon");   /* относительный — резолвится к папке exe */
    reg_set_str(L"domain", "auto");
    reg_set_str(L"session_com", "");
    reg_set_str(L"session_ru", "");
    reg_set_dword(L"max_files", 10);
    reg_set_dword(L"favorite_every_n", 10);
    reg_set_dword(L"update_interval_min", 0);
    reg_set_dword(L"max_attempts", 3);
    reg_set_dword(L"notify", 1);
    reg_set_dword(L"interval_min", 10);
    reg_set_dword(L"counter", 0);
    reg_set_str(L"Language", "russian");
    reg_set_str(L"AppTheme", "light");
    /* password_enc не пишем — пароль появится, когда его введут через меню */
    LOG_INFO(T("Реестр пуст — созданы значения по умолчанию в HKCU\\Software\\GoodFon", "Registry empty — default values created in HKCU/Software/GoodFon"));
}

static int settings_load(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));

    /* Реестр пуст? Пишем значения по умолчанию. */
    char probe[64];
    if (!reg_get_str(L"theme", probe, sizeof(probe)) || !probe[0])
        settings_write_defaults();

    reg_get_str(L"login", g_cfg.login, sizeof(g_cfg.login));
    reg_get_password(g_cfg.password, sizeof(g_cfg.password));
    reg_get_str(L"session_com", g_cfg.session_com, JAR_SIZE);
    reg_get_str(L"session_ru", g_cfg.session_ru, JAR_SIZE);

    /* Язык — читаем как можно раньше, чтобы и первые логи были на нужном языке. */
    { char lang[16];
      if (reg_get_str(L"Language", lang, sizeof(lang)) && !_stricmp(lang, "english"))
          g_lang = LANG_EN;
      else g_lang = LANG_RU; }
    { char th[16];
      g_ui_theme = (reg_get_str(L"AppTheme", th, sizeof(th)) && !_stricmp(th, "dark"))
                   ? THEME_DARK : THEME_LIGHT; }

    if (!reg_get_str(L"resolution", g_cfg.resolution, sizeof(g_cfg.resolution)) || !g_cfg.resolution[0])
        strcpy(g_cfg.resolution, "1920x1080");
    if (!reg_get_str(L"theme", g_cfg.theme, sizeof(g_cfg.theme)) || !g_cfg.theme[0])
        strcpy(g_cfg.theme, "nature");
    reg_get_str(L"save_dir", g_cfg.save_dir, sizeof(g_cfg.save_dir));
    if (!reg_get_str(L"domain", g_cfg.domain_pref, sizeof(g_cfg.domain_pref)) || !g_cfg.domain_pref[0])
        strcpy(g_cfg.domain_pref, "auto");

    g_cfg.max_files    = reg_get_dword(L"max_files", 10);
    g_cfg.favorite_every_n = reg_get_dword(L"favorite_every_n", 10);
    g_cfg.update_interval_min = reg_get_dword(L"update_interval_min", 0);
    g_cfg.max_attempts = reg_get_dword(L"max_attempts", 3);
    g_cfg.notify       = reg_get_dword(L"notify", 1);
    g_cfg.interval_min = reg_get_dword(L"interval_min", 10);
    g_cfg.counter      = reg_get_dword(L"counter", 0);
    if (g_cfg.interval_min < 1) g_cfg.interval_min = 10;

    /* save_dir относительно папки exe */
    WCHAR wsave[MAX_PATH];
    if (g_cfg.save_dir[0]) utf8_to_wide(g_cfg.save_dir, wsave, MAX_PATH);
    else wcscpy(wsave, L"GoodFon");
    if (PathIsRelativeW(wsave)) {
        WCHAR exedir[MAX_PATH], full[MAX_PATH];
        GetModuleFileNameW(NULL, exedir, MAX_PATH);
        PathRemoveFileSpecW(exedir);
        PathCombineW(full, exedir, wsave);
        wcscpy(wsave, full);
    }
    wide_to_utf8(wsave, g_cfg.save_dir, sizeof(g_cfg.save_dir));

    WCHAR wtheme[64];
    utf8_to_wide(g_cfg.theme, wtheme, 64);
    wcscpy(g_favorite_dir, wsave);
    PathAppendW(g_favorite_dir, L"Favorite");
    PathAppendW(g_favorite_dir, wtheme);

    strncpy(g_jar_com, g_cfg.session_com, JAR_SIZE - 1);
    strncpy(g_jar_ru,  g_cfg.session_ru,  JAR_SIZE - 1);

    {
        char l8[MAX_PATH * 3]; wide_to_utf8(g_favorite_dir, l8, sizeof(l8));
        LOG_INFO(T("Настройки загружены из реестра. Тема: %s | save_dir: %s", "Settings loaded from registry. Theme: %s | save_dir: %s"),
                 g_cfg.theme, g_cfg.save_dir);
        LOG_INFO(T("Папка избранного: %s", "Favorites folder: %s"), l8);
    }
    return 1;
}

static void counter_save(void)
{
    reg_set_dword(L"counter", g_cfg.counter);
}

/* Есть ли у нас авторизация: заданные логин+пароль.
 * Используется для гейтинга раздела "Эротика" (доступен только после входа). */
static int is_authorized(void)
{
    /* Авторизован = в приложении заданы логин и пароль. Одного кэша cookie
     * без логина недостаточно (иначе старая сессия разблокировала бы разделы
     * даже после выхода / на «чистом» первом запуске). */
    return (g_cfg.login[0] && strcmp(g_cfg.login, "your_login") &&
            g_cfg.password[0] && strcmp(g_cfg.password, "your_password"));
}

static char *jar_for(int domain) { return domain == 1 ? g_jar_ru : g_jar_com; }

static void jar_merge(int domain, const char *set_cookie)
{
    /* set_cookie: "name=value; Path=/; ..." — берём только name=value */
    char nv[512];
    const char *sc = strchr(set_cookie, ';');
    size_t n = sc ? (size_t)(sc - set_cookie) : strlen(set_cookie);
    if (n >= sizeof(nv)) n = sizeof(nv) - 1;
    memcpy(nv, set_cookie, n); nv[n] = 0;
    str_trim(nv);
    char *eq = strchr(nv, '=');
    if (!eq) return;
    size_t name_len = (size_t)(eq - nv);

    char *jar = jar_for(domain);
    /* если cookie с таким именем уже есть — удалить старую */
    char *p = jar;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        char *end = strchr(p, ';');
        size_t plen = end ? (size_t)(end - p) : strlen(p);
        if (plen > name_len && !strncmp(p, nv, name_len) && p[name_len] == '=') {
            const char *rest = end ? end + 1 : p + plen;
            while (*rest == ' ') rest++;
            memmove(p, rest, strlen(rest) + 1);
            /* убрать хвостовые "; " */
            size_t jl = strlen(jar);
            while (jl && (jar[jl-1] == ';' || jar[jl-1] == ' ')) jar[--jl] = 0;
            continue;
        }
        p = end ? end + 1 : p + plen;
    }
    if (strlen(jar) + strlen(nv) + 3 < JAR_SIZE) {
        if (jar[0]) strcat(jar, "; ");
        strcat(jar, nv);
    }
}

static void jar_save(int domain)
{
    reg_set_str(domain == 1 ? L"session_ru" : L"session_com", jar_for(domain));
    LOG_INFO(T("Кэш сессии сохранён для домена %s", "Session cache saved for domain %s"), g_hosts[domain]);
}

/* ================= HTTP (WinHTTP) ================= */

static HINTERNET g_hsession = NULL;

static int http_init(void)
{
    g_hsession = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        L"(KHTML, like Gecko) Chrome/120 Safari/537.36",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_hsession) return 0;
    /* авто-cookie WinHTTP отключаем — у нас свой джар (нужен для кэша) */
    DWORD dis = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(g_hsession, WINHTTP_OPTION_DISABLE_FEATURE, &dis, sizeof(dis));
    return 1;
}

typedef struct {
    char  *body;      /* malloc; вызывающий делает free */
    size_t len;
    int    status;
    char   ctype[128];
} HttpResp;

/* Универсальный запрос. method: "GET"/"POST"; url: полный https://...
 * body: тело POST или NULL; extra: доп. заголовки CRLF-строкой или NULL.
 * recv_timeout_ms: таймаут получения ответа. limit: макс. размер тела.  */
static int http_request(const char *method, const char *url,
                        const char *body, const char *extra_headers,
                        int recv_timeout_ms, size_t limit, HttpResp *out)
{
    memset(out, 0, sizeof(*out));

    WCHAR wurl[1024];
    utf8_to_wide(url, wurl, 1024);
    URL_COMPONENTS uc; memset(&uc, 0, sizeof(uc));
    WCHAR host[256], path[768];
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;  uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path;  uc.dwUrlPathLength  = 768;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return 0;
    int https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    /* какой это домен для джара */
    char hostA[256]; wide_to_utf8(host, hostA, 256);
    int dom = strstr(hostA, "goodfon.ru") ? 1 : (strstr(hostA, "goodfon.com") ? 0 : -1);

    HINTERNET hc = WinHttpConnect(g_hsession, host, uc.nPort, 0);
    if (!hc) return 0;
    WCHAR wmethod[8]; utf8_to_wide(method, wmethod, 8);
    HINTERNET hr = WinHttpOpenRequest(hc, wmethod, path, NULL,
                                      WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      https ? WINHTTP_FLAG_SECURE : 0);
    if (!hr) { WinHttpCloseHandle(hc); return 0; }

    WinHttpSetTimeouts(hr, 8000, 8000, 15000, recv_timeout_ms);

    /* браузероподобные заголовки + cookie из джара */
    WCHAR hdr[JAR_SIZE + 2048];
    WCHAR wextra[1024] = L"";
    if (extra_headers) utf8_to_wide(extra_headers, wextra, 1024);
    WCHAR wcookie[JAR_SIZE] = L"";
    if (dom >= 0 && jar_for(dom)[0]) {
        WCHAR tmp[JAR_SIZE];
        utf8_to_wide(jar_for(dom), tmp, JAR_SIZE);
        _snwprintf(wcookie, JAR_SIZE, L"Cookie: %s\r\n", tmp);
    }
    _snwprintf(hdr, sizeof(hdr)/sizeof(WCHAR),
        L"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
        L"image/avif,image/webp,*/*;q=0.8\r\n"
        L"Accept-Language: ru-RU,ru;q=0.9,en-US;q=0.8,en;q=0.7\r\n"
        L"Upgrade-Insecure-Requests: 1\r\n%s%s", wcookie, wextra);

    DWORD blen = body ? (DWORD)strlen(body) : 0;
    int ok = 0, attempt;
    for (attempt = 0; attempt < 3; attempt++) {
        if (!WinHttpSendRequest(hr, hdr, (DWORD)-1L,
                                (LPVOID)body, blen, blen, 0)) break;
        if (!WinHttpReceiveResponse(hr, NULL)) break;

        DWORD status = 0, ssz = sizeof(status);
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &ssz,
                            WINHTTP_NO_HEADER_INDEX);
        out->status = (int)status;

        /* Content-Type */
        WCHAR wct[128]; DWORD ctsz = sizeof(wct);
        if (WinHttpQueryHeaders(hr, WINHTTP_QUERY_CONTENT_TYPE,
                                WINHTTP_HEADER_NAME_BY_INDEX, wct, &ctsz,
                                WINHTTP_NO_HEADER_INDEX))
            wide_to_utf8(wct, out->ctype, sizeof(out->ctype));

        /* Set-Cookie -> джар */
        if (dom >= 0) {
            DWORD idx = 0;
            for (;;) {
                WCHAR wsc[1024]; DWORD scsz = sizeof(wsc);
                if (!WinHttpQueryHeaders(hr, WINHTTP_QUERY_SET_COOKIE,
                                         WINHTTP_HEADER_NAME_BY_INDEX,
                                         wsc, &scsz, &idx)) break;
                char sc[1024]; wide_to_utf8(wsc, sc, sizeof(sc));
                jar_merge(dom, sc);
            }
        }

        /* Тело */
        size_t cap = 65536; out->body = (char *)malloc(cap); out->len = 0;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hr, &avail) || avail == 0) break;
            if (out->len + avail + 1 > cap) {
                while (out->len + avail + 1 > cap) cap *= 2;
                if (cap > limit) { cap = limit; }
                out->body = (char *)realloc(out->body, cap);
            }
            DWORD rd = 0;
            if (out->len + avail > limit) avail = (DWORD)(limit - out->len);
            if (avail == 0) break;
            if (!WinHttpReadData(hr, out->body + out->len, avail, &rd) || rd == 0) break;
            out->len += rd;
        }
        if (out->body) out->body[out->len] = 0;

        /* авто-повтор шлюзовых ошибок */
        if ((status == 502 || status == 503 || status == 504) && attempt < 2) {
            LOG_WARN(T("HTTP %d %s -> статус %lu, повтор через 2с", "HTTP %d %s -> status %lu, retry in 2s"),
                     attempt + 1, url, (unsigned long)status);
            free(out->body); out->body = NULL; out->len = 0;
            Sleep(2000);
            continue;
        }
        LOG_INFO(T("HTTP %s %s -> %lu (%lu байт)", "HTTP %s %s -> %lu (%lu bytes)"),
                 method, url, (unsigned long)status, (unsigned long)out->len);
        ok = 1;
        break;
    }
    if (!ok)
        LOG_WARN(T("HTTP %s %s -> сетевая ошибка %lu", "HTTP %s %s -> network error %lu"),
                 method, url, GetLastError());
    WinHttpCloseHandle(hr);
    WinHttpCloseHandle(hc);
    return ok;
}

/* ================= Загрузчик произвольного URL (для обновлений) =================
 * Простой GET по полному URL с автоследованием за редиректами (WinHTTP делает это
 * сам для https->https, что и нужно для github.com -> raw.githubusercontent.com).
 * Если outfile != NULL — тело пишется в файл; иначе в текстовый буфер txt.      */
static int fetch_url_raw(const WCHAR *wurl, const WCHAR *outfile, char *txt, int txtcap)
{
    URL_COMPONENTS uc; memset(&uc, 0, sizeof(uc));
    WCHAR host[256], path[1024];
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;  uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path;  uc.dwUrlPathLength  = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return 0;
    int https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hc = WinHttpConnect(g_hsession, host, uc.nPort, 0);
    if (!hc) return 0;
    HINTERNET hr = WinHttpOpenRequest(hc, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      https ? WINHTTP_FLAG_SECURE : 0);
    if (!hr) { WinHttpCloseHandle(hc); return 0; }
    WinHttpSetTimeouts(hr, 8000, 8000, 15000, 60000);

    int ok = 0;
    if (WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hr, NULL)) {
        DWORD status = 0, slen = sizeof(status);
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            NULL, &status, &slen, NULL);
        if (status == 200) {
            HANDLE hf = INVALID_HANDLE_VALUE;
            if (outfile) {
                hf = CreateFileW(outfile, GENERIC_WRITE, 0, NULL,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hf == INVALID_HANDLE_VALUE) {
                    WinHttpCloseHandle(hr); WinHttpCloseHandle(hc); return 0;
                }
            }
            int tlen = 0;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(hr, &avail) || !avail) break;
                char buf[16384];
                if (avail > sizeof(buf)) avail = sizeof(buf);
                DWORD rd = 0;
                if (!WinHttpReadData(hr, buf, avail, &rd) || !rd) break;
                if (outfile) { DWORD wr; WriteFile(hf, buf, rd, &wr, NULL); }
                else if (txt) {
                    int cp = (int)rd;
                    if (tlen + cp > txtcap - 1) cp = txtcap - 1 - tlen;
                    if (cp > 0) { memcpy(txt + tlen, buf, cp); tlen += cp; }
                }
            }
            if (outfile) CloseHandle(hf);
            else if (txt) txt[tlen] = 0;
            ok = 1;
        }
    }
    WinHttpCloseHandle(hr);
    WinHttpCloseHandle(hc);
    return ok;
}


/* wininet.h (где объявлен IActiveDesktop) конфликтует с winhttp.h,
 * поэтому объявляем минимальный интерфейс сами — порядок методов vtbl
 * соответствует официальному IActiveDesktop.                            */

#define AD_APPLY_ALL_ 0x00000007  /* SAVE | HTMLGEN | REFRESH */

typedef struct MyActiveDesktop MyActiveDesktop;
typedef struct {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(MyActiveDesktop *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(MyActiveDesktop *);
    ULONG   (STDMETHODCALLTYPE *Release)(MyActiveDesktop *);
    /* IActiveDesktop */
    HRESULT (STDMETHODCALLTYPE *ApplyChanges)(MyActiveDesktop *, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetWallpaper)(MyActiveDesktop *, PWSTR, UINT, DWORD);
    HRESULT (STDMETHODCALLTYPE *SetWallpaper)(MyActiveDesktop *, PCWSTR, DWORD);
    /* дальнейшие методы не нужны */
} MyActiveDesktopVtbl;
struct MyActiveDesktop { const MyActiveDesktopVtbl *lpVtbl; };

static const CLSID CLSID_ActiveDesktop_ =
{ 0x75048700, 0xEF1F, 0x11D0, {0x98,0x88,0x00,0x60,0x97,0xDE,0xAC,0xF9} };
static const IID IID_IActiveDesktop_ =
{ 0xF490EB00, 0x1240, 0x11D1, {0x98,0x88,0x00,0x60,0x97,0xDE,0xAC,0xF9} };

static void enable_active_desktop(void)
{
    HWND progman = FindWindowW(L"Progman", NULL);
    if (progman) {
        DWORD_PTR res;
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 500, &res);
    }
}

static void force_refresh(void)
{
    /* как в Python: user32!UpdatePerUserSystemParameters(1) */
    typedef BOOL (WINAPI *PFN_UPUSP)(DWORD);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        PFN_UPUSP fn = (PFN_UPUSP)GetProcAddress(u, "UpdatePerUserSystemParameters");
        if (fn) fn(1);
    }
}

static int set_wallpaper(const WCHAR *path)
{
    char p8[MAX_PATH * 3]; wide_to_utf8(path, p8, sizeof(p8));

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        LOG_ERROR(T("set_wallpaper: файл не существует: %s", "set_wallpaper: file does not exist: %s"), p8);
        return 0;
    }

    enable_active_desktop();
    HRESULT hrInit = CoInitialize(NULL);
    MyActiveDesktop *pad = NULL;
#ifdef __cplusplus
    HRESULT hr = CoCreateInstance(CLSID_ActiveDesktop_, NULL,
                                  CLSCTX_INPROC_SERVER,
                                  IID_IActiveDesktop_, (void **)&pad);
#else
    HRESULT hr = CoCreateInstance(&CLSID_ActiveDesktop_, NULL,
                                  CLSCTX_INPROC_SERVER,
                                  &IID_IActiveDesktop_, (void **)&pad);
#endif
    int ok = 0;
    if (SUCCEEDED(hr) && pad) {
        HRESULT h1 = pad->lpVtbl->SetWallpaper(pad, path, 0);
        HRESULT h2 = pad->lpVtbl->ApplyChanges(pad, AD_APPLY_ALL_);
        pad->lpVtbl->Release(pad);
        LOG_INFO("IActiveDesktop: SetWallpaper=0x%08lX ApplyChanges=0x%08lX",
                 (unsigned long)h1, (unsigned long)h2);
        ok = SUCCEEDED(h1) && SUCCEEDED(h2);
    } else {
        LOG_WARN(T("IActiveDesktop недоступен: CoCreateInstance=0x%08lX", "IActiveDesktop unavailable: CoCreateInstance=0x%08lX"),
                 (unsigned long)hr);
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();

    if (!ok) {
        /* запасной путь без плавности */
        if (SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)path,
                                  SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
            LOG_INFO(T("Fallback SPI_SETDESKWALLPAPER: успех", "Fallback SPI_SETDESKWALLPAPER: success"));
            ok = 1;
        } else {
            LOG_ERROR(T("SPI_SETDESKWALLPAPER: ошибка %lu", "SPI_SETDESKWALLPAPER: error %lu"), GetLastError());
        }
    } else {
        force_refresh();
        /* Проверяем, что система реально применила именно наш файл:
         * IActiveDesktop может вернуть S_OK, но молча не применить
         * (например, для .webp). Тогда форсим через SPI.            */
        WCHAR cur[MAX_PATH] = L"";
        SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, cur, 0);
        if (_wcsicmp(cur, path) != 0) {
            char c8[MAX_PATH * 3]; wide_to_utf8(cur, c8, sizeof(c8));
            LOG_WARN(T("IActiveDesktop применил не наш файл (сейчас: %s) — форсим SPI", "IActiveDesktop applied a different file (now: %s) — forcing SPI"), c8);
            if (SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)path,
                                      SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
                LOG_INFO(T("SPI_SETDESKWALLPAPER: успех", "SPI_SETDESKWALLPAPER: success"));
            else {
                LOG_ERROR(T("SPI_SETDESKWALLPAPER: ошибка %lu", "SPI_SETDESKWALLPAPER: error %lu"), GetLastError());
                ok = 0;
            }
        }
    }

    if (ok) {
        LOG_INFO(T("Обои выставлены: %s", "Wallpaper set: %s"), p8);
        wcsncpy(g_current_image, path, MAX_PATH - 1);  /* фиксируем текущую картинку */
        g_current_image[MAX_PATH - 1] = 0;
    }
    else    LOG_ERROR(T("Обои НЕ выставлены: %s", "Wallpaper NOT set: %s"), p8);
    return ok;
}

static void get_current_wallpaper(WCHAR *out, int outsz)
{
    out[0] = 0;
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, outsz, out, 0);
}

/* ================= Уведомления в трее ================= */

static void notify_user(const WCHAR *title, const WCHAR *text)
{
    if (!g_cfg.notify) return;
    if (g_tray_mode) {
        g_nid.uFlags = NIF_INFO;
        wcsncpy(g_nid.szInfoTitle, title, 63);
        wcsncpy(g_nid.szInfo, text, 255);
        g_nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
    /* в разовом CLI-режиме уведомление опускаем — есть логи */
}

/* ================= Файловые операции ================= */

typedef struct { WCHAR path[MAX_PATH]; FILETIME mt; } FileEnt;

static int is_image_file(const WCHAR *name)
{
    const WCHAR *dot = wcsrchr(name, L'.');
    if (!dot) return 0;
    return !_wcsicmp(dot, L".jpg")  || !_wcsicmp(dot, L".jpeg") ||
           !_wcsicmp(dot, L".png")  || !_wcsicmp(dot, L".bmp")  ||
           !_wcsicmp(dot, L".webp") || !_wcsicmp(dot, L".gif");
}

/* Сканирует папку в динамический массив (куча). Вызывающий делает free().
 * *outN = число файлов; возвращает массив или NULL при ошибке/пустой папке. */
static FileEnt *dir_scan(const WCHAR *dir, int *outN)
{
    *outN = 0;
    WCHAR pat[MAX_PATH];
    _snwprintf(pat, MAX_PATH, L"%s\\*.*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        char d8[MAX_PATH * 3]; wide_to_utf8(dir, d8, sizeof(d8));
        LOG_INFO(T("dir_scan: папка недоступна (код %lu): %s", "dir_scan: folder unavailable (code %lu): %s"), GetLastError(), d8);
        return NULL;
    }
    int cap = 128, n = 0;
    FileEnt *arr = (FileEnt *)malloc(cap * sizeof(FileEnt));
    if (!arr) { FindClose(h); return NULL; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!is_image_file(fd.cFileName)) continue;
        if (n >= cap) {
            cap *= 2;
            FileEnt *na = (FileEnt *)realloc(arr, cap * sizeof(FileEnt));
            if (!na) break;
            arr = na;
        }
        _snwprintf(arr[n].path, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        arr[n].mt = fd.ftLastWriteTime;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    *outN = n;
    return arr;
}

static int cmp_mtime(const void *a, const void *b)
{ return CompareFileTime(&((const FileEnt *)a)->mt, &((const FileEnt *)b)->mt); }

static void cleanup_old_images(void)
{
    WCHAR wdir[MAX_PATH]; utf8_to_wide(g_cfg.save_dir, wdir, MAX_PATH);
    int n = 0;
    FileEnt *files = dir_scan(wdir, &n);
    if (files && n > g_cfg.max_files) {
        qsort(files, n, sizeof(FileEnt), cmp_mtime);
        for (int i = 0; i < n - g_cfg.max_files; i++) {
            if (DeleteFileW(files[i].path)) {
                char p8[MAX_PATH * 3]; wide_to_utf8(files[i].path, p8, sizeof(p8));
                LOG_INFO(T("Удалён старый файл: %s", "Deleted old file: %s"), p8);
            }
        }
    }
    free(files);
}

/* Случайный файл из dir, исключая exclude (если возможно). */
static int random_file_excluding(const WCHAR *dir, const WCHAR *exclude,
                                 WCHAR *out, int outsz)
{
    int n = 0;
    FileEnt *files = dir_scan(dir, &n);
    if (!files || n == 0) { free(files); return 0; }
    int *idxs = (int *)malloc(n * sizeof(int)), m = 0;
    if (idxs) {
        for (int i = 0; i < n; i++)
            if (!exclude || _wcsicmp(files[i].path, exclude) != 0) idxs[m++] = i;
    }
    int pick = (idxs && m > 0) ? idxs[rand() % m] : (rand() % n);
    wcsncpy(out, files[pick].path, outsz - 1); out[outsz - 1] = 0;
    free(idxs); free(files);
    return 1;
}

static int dir_count(const WCHAR *dir)
{
    int n = 0;
    FileEnt *f = dir_scan(dir, &n);
    free(f);
    return n;
}

/* ================= Логика GoodFon ================= */

static void base_url(char *out, size_t sz)
{ snprintf(out, sz, "https://%s", g_hosts[g_active_domain]); }

static int domain_order(int order[2])
{
    if (!strcmp(g_cfg.domain_pref, "ru")) { order[0] = 1; order[1] = 0; }
    else                                  { order[0] = 0; order[1] = 1; }
    return 2;
}

/* Логин: честная классификация успех / неверный пароль / ошибка сервера.
 * Возврат: 1 = ок, 0 = сервер болеет (можно повторять), -1 = пароль.     */
static int do_login(void)
{
    char url[512], base[64];
    base_url(base, sizeof(base));
    snprintf(url, sizeof(url), "%s/auth/signin/", base);

    HttpResp r;
    if (!http_request("GET", url, NULL, NULL, 30000, BODY_LIMIT, &r) || !r.body) {
        free(r.body); return 0;
    }
    char token[128] = "";
    const char *inp = strstr(r.body, "csrfmiddlewaretoken");
    if (inp) extract_attr(inp, "value=", token, sizeof(token));
    free(r.body);

    /* form-urlencoded (логин/пароль ASCII — по нашей практике) */
    char body[512];
    snprintf(body, sizeof(body), "csrfmiddlewaretoken=%s&login=%s&password=%s",
             token, g_cfg.login, g_cfg.password);
    char extra[1024];
    snprintf(extra, sizeof(extra),
             "Content-Type: application/x-www-form-urlencoded\r\n"
             "Referer: %s/auth/signin/\r\nOrigin: %s\r\n"
             "X-Requested-With: XMLHttpRequest\r\n%s%s%s",
             base, base,
             token[0] ? "X-CSRFToken: " : "", token, token[0] ? "\r\n" : "");

    HttpResp p;
    if (!http_request("POST", url, body, extra, 30000, BODY_LIMIT, &p))
        return 0;
    int ok = 0, bad = 0;
    if (p.body) {
        CharLowerA(p.body);
        if (strstr(p.body, "\"success\"") || strstr(p.body, "result\": \"ok") ||
            strstr(p.body, "result\":\"ok")) ok = 1;
        if (strstr(p.body, "incorrect password") || strstr(p.body, "\"error\"") ||
            strstr(p.body, "\"fail\"") || strstr(p.body, LOGIN_FAIL_RU)) bad = 1;
    }
    int has_session = str_icontains(jar_for(g_active_domain), "sessionid=");
    free(p.body);

    if (bad) return -1;
    if (ok || has_session) {
        LOG_INFO(T("Авторизация успешна", "Authorization successful"));
        jar_save(g_active_domain);
        return 1;
    }
    LOG_WARN(T("Неожиданный ответ сервера при входе (статус %d)", "Unexpected server response on sign-in (status %d)"), p.status);
    return 0;
}

/* Выбор домена + сессии: кэш напрямую, иначе логин с повтором. 1 = ок */
static int ensure_session(void)
{
    /* Нет логина/пароля — не дёргаем сеть впустую, работаем из локального избранного. */
    if (!is_authorized()) {
        LOG_INFO(T("Вход не выполняется: логин и пароль не заданы.", "Sign-in skipped: login and password are not set."));
        return 0;
    }
    int order[2]; domain_order(order);
    for (int i = 0; i < 2; i++) {
        g_active_domain = order[i];
        if (jar_for(g_active_domain)[0]) {
            LOG_INFO(T("Используется кэш сессии: %s", "Using session cache: %s"), g_hosts[g_active_domain]);
            return 1;
        }
        for (int att = 0; att < 2; att++) {
            int res = do_login();
            if (res == 1) return 1;
            if (res == -1) {
                LOG_WARN(T("Не удалось войти на %s: неверный логин или пароль", "Failed to sign in on %s: invalid login or password"),
                         g_hosts[g_active_domain]);
                break;
            }
            LOG_WARN(T("Сетевая ошибка входа на %s (попытка %d)", "Network error signing in on %s (attempt %d)"),
                     g_hosts[g_active_domain], att + 1);
        }
    }
    g_active_domain = -1;
    return 0;
}

static int get_max_pages(void)
{
    char url[512], base[64];
    base_url(base, sizeof(base));
    snprintf(url, sizeof(url), "%s/%s/", base, g_cfg.theme);
    HttpResp r;
    if (!http_request("GET", url, NULL, NULL, 15000, BODY_LIMIT, &r) ||
        r.status != 200 || !r.body) { free(r.body); return 0; }
    int maxp = 1;
    const char *p = r.body;
    while ((p = strstr(p, "index-")) != NULL) {
        p += 6;
        int v = atoi(p);
        if (v > maxp) maxp = v;
    }
    free(r.body);
    return maxp > 1 ? maxp : 0;
}

/* Собрать до max ссылок на страницы картинок из HTML раздела. */
static int collect_links(const char *html, char links[][512], int max)
{
    int n = 0;
    const char *p = html;
    while (n < max && (p = strstr(p, "href=")) != NULL) {
        char href[512];
        if (extract_attr(p, "href=", href, sizeof(href))) {
            if (strstr(href, "/wallpaper-") &&
                !strstr(href, "wallpaper-download") &&
                strstr(href, ".html")) {
                int dup = 0;
                for (int i = 0; i < n; i++)
                    if (!strcmp(links[i], href)) { dup = 1; break; }
                if (!dup) strncpy(links[n++], href, 511);
            }
        }
        p += 5;
    }
    return n;
}

static void make_absolute(const char *href, char *out, size_t sz)
{
    if (!strncmp(href, "http", 4)) { strncpy(out, href, sz - 1); out[sz-1] = 0; return; }
    char base[64]; base_url(base, sizeof(base));
    snprintf(out, sz, "%s%s", base, href);
}

/* Границы тира разрешения: нижняя = выбранное, верхняя (исключительно) =
 * следующий стандартный тир. Напр. 2K -> [2560x1440 .. 3840x2160),
 * 4K -> [3840x2160 .. 8400x3600). Если значение нестандартное — верхней
 * границы нет (берётся всё, что >= выбранного).                          */
static void resolution_band(int *tw, int *th, int *nw, int *nh)
{
    static const int tiers[][2] = {
        {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160},
        {7680, 4320}, {10240, 5760}
    };
    const int ntiers = (int)(sizeof(tiers) / sizeof(tiers[0]));
    *tw = 0; *th = 0;
    sscanf(g_cfg.resolution, "%dx%d", tw, th);
    *nw = 1000000; *nh = 1000000;   /* по умолчанию верхней границы нет */
    for (int i = 0; i < ntiers; i++) {
        if (tiers[i][0] == *tw && tiers[i][1] == *th) {
            if (i + 1 < ntiers) { *nw = tiers[i + 1][0]; *nh = tiers[i + 1][1]; }
            break;
        }
    }
}

/* Найти прямой URL картинки нужного разрешения.
 * Возврат: 1 = найден (out), 0 = пропустить картинку, -1 = квота. */
static int find_image_url(const char *image_page_url, char *out, size_t outsz)
{
    HttpResp r;
    if (!http_request("GET", image_page_url, NULL, NULL, 15000, BODY_LIMIT, &r) ||
        r.status != 200 || !r.body) { free(r.body); return 0; }

    char dl_href[512] = "";
    if (!_stricmp(g_cfg.resolution, "original")) {
        /* режим "Оригинал": берём ссылку с самым большим WxH */
        long best = -1;
        const char *p = r.body;
        while ((p = strstr(p, "href=")) != NULL) {
            char href[512];
            if (extract_attr(p, "href=", href, sizeof(href))) {
                const char *d = strstr(href, "wallpaper-download-");
                if (d) {
                    int w = 0, hh = 0;
                    if (sscanf(d + 19, "%dx%d", &w, &hh) == 2) {
                        long area = (long)w * hh;
                        if (area > best) {
                            best = area;
                            strncpy(dl_href, href, sizeof(dl_href) - 1);
                            dl_href[sizeof(dl_href) - 1] = 0;
                        }
                    }
                }
            }
            p += 5;
        }
    } else {
        /* тир разрешения: берём наибольшее из доступных, которое >= выбранного,
         * но НЕ дотягивает до следующего тира (2K не хватает 4K, 4K не хватает 8K). */
        int tw, th, nw, nh;
        resolution_band(&tw, &th, &nw, &nh);
        long best = -1;
        int cw = 0, ch = 0;
        const char *p = r.body;
        while ((p = strstr(p, "href=")) != NULL) {
            char href[512];
            if (extract_attr(p, "href=", href, sizeof(href))) {
                const char *d = strstr(href, "wallpaper-download-");
                if (d) {
                    int w = 0, hh = 0;
                    if (sscanf(d + 19, "%dx%d", &w, &hh) == 2) {
                        int meets_target = (w >= tw && hh >= th);
                        int meets_next   = (w >= nw && hh >= nh);
                        if (meets_target && !meets_next) {
                            long area = (long)w * hh;
                            if (area > best) {   /* наибольшее в пределах тира */
                                best = area; cw = w; ch = hh;
                                strncpy(dl_href, href, sizeof(dl_href) - 1);
                                dl_href[sizeof(dl_href) - 1] = 0;
                            }
                        }
                    }
                }
            }
            p += 5;
        }
        if (dl_href[0] && (cw != tw || ch != th))
            LOG_INFO(T("Тир %s: выбрано %dx%d", "Tier %s: chose %dx%d"), g_cfg.resolution, cw, ch);
    }
    free(r.body);
    if (!dl_href[0]) {
        LOG_INFO(T("Разрешение %s недоступно для этой картинки, пропускаем.", "Resolution %s is not available for this image, skipping."), g_cfg.resolution);
        return 0;
    }

    char dl_url[600];
    make_absolute(dl_href, dl_url, sizeof(dl_url));
    HttpResp d;
    if (!http_request("GET", dl_url, NULL, NULL, 15000, BODY_LIMIT, &d) ||
        d.status != 200 || !d.body) { free(d.body); return 0; }

    if (strstr(d.body, QUOTA_MARKER_RU) || strstr(d.body, "download_limit")) {
        free(d.body);
        LOG_WARN(T("Превышен суточный лимит скачиваний на сайте.", "Daily download limit on the site exceeded."));
        return -1;
    }

    int found = 0;
    const char *a = strstr(d.body, "js-download_img");
    if (a) {
        /* откатиться к началу тега <a */
        const char *tag = a;
        while (tag > d.body && *tag != '<') tag--;
        char href[512];
        if (extract_attr(tag, "href=", href, sizeof(href)) &&
            strstr(href, "img.goodfon")) {
            strncpy(out, href, outsz - 1); out[outsz-1] = 0; found = 1;
        }
    }
    if (!found) {
        const char *im = d.body;
        while ((im = strstr(im, "<img")) != NULL) {
            char src[512];
            if (extract_attr(im, "src=", src, sizeof(src)) &&
                strstr(src, "img.goodfon")) {
                strncpy(out, src, outsz - 1); out[outsz-1] = 0; found = 1; break;
            }
            im += 4;
        }
    }
    free(d.body);
    if (!found)
        LOG_WARN(T("Ссылка на картинку не найдена на странице загрузки.", "Image link not found on the download page."));
    return found ? 1 : 0;
}

/* Скачивание с резервом домена img.com<->img.ru. Возврат malloc-буфера. */
static char *download_image(const char *url, size_t *outLen)
{
    const char *tries[2] = { url, NULL };
    char alt[600] = "";
    if (strstr(url, "img.goodfon.com")) {
        strncpy(alt, url, sizeof(alt) - 1);
        char *pos = strstr(alt, "img.goodfon.com");
        memcpy(pos, "img.goodfon.ru\0", 15);
        /* заменить .com на .ru со сдвигом */
        char fixed[600];
        const char *tail = strstr(url, "img.goodfon.com") + strlen("img.goodfon.com");
        snprintf(fixed, sizeof(fixed), "%.*simg.goodfon.ru%s",
                 (int)(strstr(url, "img.goodfon.com") - url), url, tail);
        strncpy(alt, fixed, sizeof(alt) - 1);
        tries[1] = alt;
    } else if (strstr(url, "img.goodfon.ru")) {
        const char *at = strstr(url, "img.goodfon.ru");
        snprintf(alt, sizeof(alt), "%.*simg.goodfon.com%s",
                 (int)(at - url), url, at + strlen("img.goodfon.ru"));
        tries[1] = alt;
    }
    for (int i = 0; i < 2 && tries[i]; i++) {
        HttpResp r;
        if (http_request("GET", tries[i], NULL, NULL, 20000, IMG_LIMIT, &r) &&
            r.status == 200 && str_icontains(r.ctype, "image") && r.len > 0) {
            if (i == 1) LOG_INFO(T("Использован резервный img домен", "Used fallback img domain"));
            *outLen = r.len;
            return r.body;
        }
        free(r.body);
    }
    return NULL;
}

static int save_image(const char *url, const char *data, size_t len,
                      WCHAR *outPath, int outsz)
{
    WCHAR wdir[MAX_PATH];
    utf8_to_wide(g_cfg.save_dir, wdir, MAX_PATH);
    SHCreateDirectoryExW(NULL, wdir, NULL);

    const char *slash = strrchr(url, '/');
    char name[256];
    strncpy(name, slash ? slash + 1 : url, sizeof(name) - 1); name[sizeof(name)-1] = 0;
    char *q = strchr(name, '?'); if (q) *q = 0;

    WCHAR wname[256]; utf8_to_wide(name, wname, 256);
    _snwprintf(outPath, outsz, L"%s\\%s", wdir, wname);

    HANDLE h = CreateFileW(outPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD wr; WriteFile(h, data, (DWORD)len, &wr, NULL);
    CloseHandle(h);
    char p8[MAX_PATH * 3]; wide_to_utf8(outPath, p8, sizeof(p8));
    LOG_INFO(T("Файл сохранён: %s", "File saved: %s"), p8);
    return 1;
}

/* ============ Избранное ============ */

static void page_url_for_file(const WCHAR *file, char *out, size_t sz)
{
    WCHAR name[256];
    wcsncpy(name, PathFindFileNameW(file), 255); name[255] = 0;
    PathRemoveExtensionW(name);
    char name8[512]; wide_to_utf8(name, name8, sizeof(name8));
    char base[64]; base_url(base, sizeof(base));
    snprintf(out, sz, "%s/%s/wallpaper-%s.html", base, g_cfg.theme, name8);
}

static int favorite_api(const char *page_url, int add)
{
    HttpResp r;
    if (!http_request("GET", page_url, NULL, NULL, 15000, BODY_LIMIT, &r) ||
        r.status != 200 || !r.body) { free(r.body); return 0; }
    const char *fav = strstr(r.body, "js-favorite");
    char api[256] = "";
    if (fav) {
        const char *tag = fav;
        while (tag > r.body && *tag != '<') tag--;
        extract_attr(tag, add ? "data-add=" : "data-del=", api, sizeof(api));
    }
    free(r.body);
    if (!api[0]) { LOG_WARN(T("Блок избранного не найден на странице.", "Favorites block not found on the page.")); return 0; }

    char api_url[600];
    make_absolute(api, api_url, sizeof(api_url));
    char extra[700];
    snprintf(extra, sizeof(extra), "Referer: %s\r\n", page_url);
    HttpResp a;
    int ok = http_request("GET", api_url, NULL, extra, 15000, BODY_LIMIT, &a) &&
             a.status == 200;
    free(a.body);
    return ok;
}

/* ============ Сценарии ============ */

static void fallback_local(int favorite_only)
{
    WCHAR cur[MAX_PATH]; get_current_wallpaper(cur, MAX_PATH);
    WCHAR chosen[MAX_PATH];
    int got = random_file_excluding(g_favorite_dir, cur, chosen, MAX_PATH);
    if (!got && !favorite_only) {
        WCHAR wdir[MAX_PATH]; utf8_to_wide(g_cfg.save_dir, wdir, MAX_PATH);
        got = random_file_excluding(wdir, cur, chosen, MAX_PATH);
    }
    if (!got) {
        LOG_WARN(T("Fallback: локальных картинок нет, обои не изменены.", "Fallback: no local images, wallpaper unchanged."));
        notify_user(TW(L"GoodFon: ошибка", L"GoodFon: error"), TW(L"Нет доступных картинок.", L"No images available."));
        return;
    }
    char p8[MAX_PATH * 3]; wide_to_utf8(chosen, p8, sizeof(p8));
    LOG_INFO(T("Fallback: устанавливаем локальную картинку: %s", "Fallback: setting local image: %s"), p8);
    set_wallpaper(chosen);
    WCHAR info[300];
    _snwprintf(info, 300, L"%s", PathFindFileNameW(chosen));
    notify_user(favorite_only ? TW(L"Обои обновлены — из избранного", L"Wallpaper updated — from favorites")
                          : TW(L"Обои обновлены — локально", L"Wallpaper updated — locally"), info);
}

static int set_wallpaper_from_favorite(void)
{
    char d8[MAX_PATH * 3]; wide_to_utf8(g_favorite_dir, d8, sizeof(d8));
    LOG_INFO(T("Favorite-папка: %s (найдено файлов: %d)", "Favorite folder: %s (files found: %d)"), d8, dir_count(g_favorite_dir));

    WCHAR cur[MAX_PATH]; get_current_wallpaper(cur, MAX_PATH);
    WCHAR chosen[MAX_PATH];
    if (!random_file_excluding(g_favorite_dir, cur, chosen, MAX_PATH)) {
        LOG_WARN(T("Папка Favorite/%s пуста, загружаем с сайта", "Folder Favorite/%s is empty, downloading from site"), g_cfg.theme);
        return 0;
    }
    set_wallpaper(chosen);
    char p8[MAX_PATH * 3]; wide_to_utf8(chosen, p8, sizeof(p8));
    LOG_INFO(T("Обои из папки Favorite/%s: %s", "Wallpaper from folder Favorite/%s: %s"), g_cfg.theme, p8);
    WCHAR info[300]; _snwprintf(info, 300, L"%s", PathFindFileNameW(chosen));
    notify_user(TW(L"Обои обновлены — из избранного", L"Wallpaper updated — from favorites"), info);
    return 1;
}

static void do_update(void)
{
    if (!ensure_session()) {
        if (is_authorized())
            LOG_ERROR(T("Ни один домен недоступен или вход не выполнен.", "No domain available or sign-in failed."));
        else
            LOG_INFO(T("Вход не выполнен (нет логина) — берём картинку локально.", "Not signed in (no login) — using a local image."));
        g_cfg.counter++; counter_save();
        fallback_local(0);
        return;
    }

    g_cfg.counter++;
    counter_save();
    LOG_INFO(T("Запуск #%d (из Favorite каждые %d)", "Run #%d (from Favorite every %d)"), g_cfg.counter, g_cfg.favorite_every_n);
    if (g_cfg.favorite_every_n > 0 && g_cfg.counter >= g_cfg.favorite_every_n) {
        g_cfg.counter = 0; counter_save();
        if (set_wallpaper_from_favorite()) return;
        LOG_INFO(T("Папка Favorite пуста, продолжаем загрузку с сайта", "Favorite folder is empty, continuing download from site"));
    }

    /* Ищем картинку с нужным разрешением. Перебираем много изображений
     * (и по несколько с каждой загруженной страницы), пока не найдём.
     * Сетевые сбои считаем отдельно, чтобы не долбить упавший сайт. */
    const int IMG_BUDGET = 40;   /* сколько картинок проверить максимум */
    int max_pages = 0;
    int images_tried = 0;
    int net_fails = 0;
    const int NET_FAIL_LIMIT = g_cfg.max_attempts + 4;

    char base[64]; base_url(base, sizeof(base));

    while (images_tried < IMG_BUDGET) {
        if (max_pages == 0) {
            max_pages = get_max_pages();
            if (max_pages == 0) {
                if (++net_fails >= NET_FAIL_LIMIT) break;
                LOG_WARN(T("Ошибка пагинации (сбой %d)", "Pagination error (failure %d)"), net_fails);
                Sleep(1000);
                continue;
            }
            LOG_INFO(T("Максимальное количество страниц: %d", "Maximum number of pages: %d"), max_pages);
        }

        int page = rand() % max_pages + 1;
        char page_url[512];
        if (page == 1)
            snprintf(page_url, sizeof(page_url), "%s/%s/", base, g_cfg.theme);
        else
            snprintf(page_url, sizeof(page_url), "%s/%s/index-%d.html",
                     base, g_cfg.theme, page);

        HttpResp r;
        if (!http_request("GET", page_url, NULL, NULL, 15000, BODY_LIMIT, &r) ||
            r.status != 200 || !r.body) {
            int st = r.status; free(r.body);
            if (++net_fails >= NET_FAIL_LIMIT) {
                LOG_WARN(T("Слишком много сетевых ошибок (последний статус %d)", "Too many network errors (last status %d)"), st);
                break;
            }
            LOG_WARN(T("Страница раздела не загрузилась (статус %d)", "Section page failed to load (status %d)"), st);
            continue;
        }
        static char links[64][512];
        int n = collect_links(r.body, links, 64);
        free(r.body);
        if (n == 0) { LOG_WARN(T("На странице нет обоев, пробуем другую", "No wallpapers on the page, trying another")); continue; }

        /* пробуем несколько картинок с этой страницы (без дублей) */
        int per_page = n < 6 ? n : 6;
        int start = rand() % n;
        for (int k = 0; k < per_page && images_tried < IMG_BUDGET; k++) {
            char image_page[600];
            make_absolute(links[(start + k) % n], image_page, sizeof(image_page));
            images_tried++;
            LOG_INFO(T("Проверка #%d: %s", "Check #%d: %s"), images_tried, image_page);

            char img_url[600];
            int fr = find_image_url(image_page, img_url, sizeof(img_url));
            if (fr == -1) {
                notify_user(TW(L"GoodFon: лимит исчерпан", L"GoodFon: limit reached"), TW(L"Загружаем из избранного.", L"Loading from favorites."));
                fallback_local(1);
                return;
            }
            if (fr == 0) continue;   /* разрешение не подошло — следующая */

            size_t len = 0;
            char *data = download_image(img_url, &len);
            if (!data) { LOG_WARN(T("Не удалось скачать картинку, пробуем другую", "Failed to download image, trying another")); continue; }

            WCHAR saved[MAX_PATH];
            int sok = save_image(img_url, data, len, saved, MAX_PATH);
            free(data);
            if (!sok) continue;

            cleanup_old_images();
            set_wallpaper(saved);
            LOG_INFO(T("Найдено за %d проверок.", "Found after %d checks."), images_tried);
            WCHAR info[300]; _snwprintf(info, 300, L"%s", PathFindFileNameW(saved));
            notify_user(TW(L"Обои обновлены — с сайта", L"Wallpaper updated — from site"), info);
            return;
        }
    }
    LOG_ERROR(T("Не найдено изображение с разрешением %s (проверено %d).", "No image found with resolution %s (checked %d)."),
              g_cfg.resolution, images_tried);
    fallback_local(0);
}

static void do_favorite(void)
{
    if (!ensure_session()) {
        notify_user(TW(L"GoodFon: сайт недоступен", L"GoodFon: site unavailable"), TW(L"Операция с избранным невозможна.", L"Favorites operation not possible."));
        return;
    }
    /* Работаем строго по зафиксированной текущей картинке, а не по «свежему
     * файлу в папке» — иначе можно добавить в избранное совсем другое фото. */
    WCHAR cur[MAX_PATH];
    wcsncpy(cur, g_current_image, MAX_PATH - 1); cur[MAX_PATH - 1] = 0;
    if (!cur[0] || GetFileAttributesW(cur) == INVALID_FILE_ATTRIBUTES) {
        LOG_ERROR(T("Нет текущей картинки для избранного.", "No current image to favorite."));
        notify_user(TW(L"GoodFon: ошибка", L"GoodFon: error"),
                    TW(L"Нет текущей картинки.", L"No current image."));
        return;
    }

    SHCreateDirectoryExW(NULL, g_favorite_dir, NULL);
    WCHAR dest[MAX_PATH];
    /* если картинка уже в папке избранного — не копируем, работаем по ней */
    if (StrStrIW(cur, g_favorite_dir) == cur) {
        wcscpy(dest, cur);
    } else {
        _snwprintf(dest, MAX_PATH, L"%s\\%s", g_favorite_dir, PathFindFileNameW(cur));
        if (GetFileAttributesW(dest) == INVALID_FILE_ATTRIBUTES)
            CopyFileW(cur, dest, FALSE);
        LOG_INFO(T("Изображение скопировано в папку Favorite/%s", "Image copied to folder Favorite/%s"), g_cfg.theme);
        set_wallpaper(dest); /* переносим «текущую» на копию из Favorite */
    }

    char page_url[600];
    page_url_for_file(dest, page_url, sizeof(page_url));
    if (favorite_api(page_url, 1)) {
        LOG_INFO(T("Изображение добавлено в избранное на сайте", "Image added to favorites on the site"));
        WCHAR info[300]; _snwprintf(info, 300, L"%s", PathFindFileNameW(dest));
        notify_user(TW(L"Добавлено в избранное", L"Added to favorites"), info);
    } else
        LOG_WARN(T("Не удалось добавить в избранное на сайте.", "Failed to add to favorites on the site."));
}

static void do_unfavorite(void)
{
    if (!ensure_session()) {
        notify_user(TW(L"GoodFon: сайт недоступен", L"GoodFon: site unavailable"), TW(L"Операция с избранным невозможна.", L"Favorites operation not possible."));
        return;
    }
    WCHAR cur[MAX_PATH];
    wcsncpy(cur, g_current_image, MAX_PATH - 1); cur[MAX_PATH - 1] = 0;
    if (!cur[0] || StrStrIW(cur, g_favorite_dir) != cur) {
        LOG_ERROR(T("Текущие обои не из папки Favorite — удаление из избранного невозможно.", "Current wallpaper is not from the Favorite folder — cannot remove from favorites."));
        notify_user(TW(L"GoodFon: ошибка", L"GoodFon: error"), TW(L"Текущие обои не из папки избранного.", L"Current wallpaper is not from the favorites folder."));
        return;
    }
    char page_url[600];
    page_url_for_file(cur, page_url, sizeof(page_url));
    if (favorite_api(page_url, 0))
        LOG_INFO(T("Изображение удалено из избранного на сайте", "Image removed from favorites on the site"));
    else
        LOG_WARN(T("Не удалось удалить из избранного на сайте.", "Failed to remove from favorites on the site."));
    if (DeleteFileW(cur)) {
        LOG_INFO(T("Файл удалён из папки Favorite", "File deleted from Favorite folder"));
        WCHAR info[300]; _snwprintf(info, 300, L"%s", PathFindFileNameW(cur));
        notify_user(TW(L"Удалено из избранного", L"Removed from favorites"), info);
    }
    do_update(); /* сразу ставим новую */
}

/* ================= Синхронизация избранного с сайтом ================= */

/* Простое множество slug'ов (динамический массив с дедупликацией). */
typedef struct { char (*a)[160]; int n, cap; } SlugSet;
static void set_init(SlugSet *s) { s->a = NULL; s->n = 0; s->cap = 0; }
static void set_free(SlugSet *s) { free(s->a); s->a = NULL; s->n = 0; s->cap = 0; }
static int  set_has(SlugSet *s, const char *x)
{ for (int i = 0; i < s->n; i++) if (!strcmp(s->a[i], x)) return 1; return 0; }
static void set_add(SlugSet *s, const char *x)
{
    if (!x[0] || set_has(s, x)) return;
    if (s->n >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 64;
        void *na = realloc(s->a, (size_t)nc * 160);
        if (!na) return;
        s->a = (char (*)[160])na; s->cap = nc;
    }
    strncpy(s->a[s->n], x, 159); s->a[s->n][159] = 0; s->n++;
}

/* Вытащить slug'и карточек (/wallpaper-<slug>.html) из HTML в множество. */
static void extract_fav_slugs(const char *html, SlugSet *out)
{
    const char *p = html;
    while ((p = strstr(p, "/wallpaper-")) != NULL) {
        const char *d = p + 11;               /* после "/wallpaper-" */
        if (!strncmp(d, "download-", 9)) { p = d; continue; }
        const char *e = strstr(d, ".html");
        if (e && e > d && (size_t)(e - d) < 159) {
            char slug[160];
            size_t len = (size_t)(e - d);
            memcpy(slug, d, len); slug[len] = 0;
            CharLowerA(slug);
            set_add(out, slug);
            p = e + 5;
        } else {
            p = d;
        }
    }
}

/* Односторонняя синхронизация: локально остаётся только то, что есть в
 * избранном на сайте; лишнее удаляется. Ничего не качает.
 * ЛЮБОЕ сомнение (сеть, неполная страница, пусто) -> отмена без удаления. */
static void sync_favorites(void)
{
    char login[128];
    strncpy(login, g_cfg.login, sizeof(login) - 1); login[sizeof(login) - 1] = 0;
    if (!login[0]) { LOG_INFO(T("Синк избранного пропущен: не задан логин.", "Favorites sync skipped: login not set.")); return; }
    if (!ensure_session()) { LOG_INFO(T("Синк избранного отменён: нет входа.", "Favorites sync cancelled: not signed in.")); return; }

    char base[64]; base_url(base, sizeof(base));
    char url[512];

    /* Число страниц берём ТОЛЬКО из доверенного чтения страницы 1 — где реально
     * присутствует пагинатор. Иначе страница могла прийти обрезанной (частый 502),
     * и мы недосчитаем страницы -> удалим то, что на самом деле в избранном. */
    int M = 0;
    for (int t = 0; t < 8; t++) {
        snprintf(url, sizeof(url), "%s/user/%s/favorite/", base, login);
        HttpResp r;
        if (!http_request("GET", url, NULL, NULL, 15000, BODY_LIMIT, &r) ||
            r.status != 200 || !r.body) { free(r.body); Sleep(400); continue; }
        /* пагинатор в самом низу страницы; нет его — страница обрезана, не доверяем */
        if (!strstr(r.body, "paginator")) {
            LOG_WARN(T("Синк: чтение пагинации неполное (%d байт), повтор.", "Sync: pagination read incomplete (%d bytes), retrying."), r.status ? (int)strlen(r.body) : 0);
            free(r.body); Sleep(500); continue;
        }
        int m = 1;
        /* из ссылок ?&page=N */
        const char *p = r.body;
        while ((p = strstr(p, "&page=")) != NULL) { int v = atoi(p + 6); if (v > m) m = v; p += 6; }
        /* и из "X из M" в блоке paginator__page (берём максимум чисел) */
        const char *pp = strstr(r.body, "paginator__page");
        if (pp) {
            const char *end = strstr(pp, "</div>");
            const char *q = pp + 15;
            while ((q = strpbrk(q, "0123456789")) != NULL && (!end || q < end)) {
                int v = atoi(q); if (v > m) m = v;
                while (*q >= '0' && *q <= '9') q++;
            }
        }
        free(r.body);
        M = m;
        break;
    }
    if (M == 0) {
        LOG_WARN(T("Синк избранного ОТМЕНЁН: не удалось надёжно прочитать пагинацию — ничего не удаляем.", "Favorites sync CANCELLED: could not reliably read pagination — deleting nothing."));
        return;
    }
    if (M > 1000) M = 1000;   /* защита от абсурда */
    LOG_INFO(T("Синк избранного: страниц на сайте %d", "Favorites sync: pages on site %d"), M);

    const int FULL_PAGE = 24, MAX_TRY = 10, CONVERGE = 3;
    SlugSet site; set_init(&site);

    for (int pg = 1; pg <= M; pg++) {
        SlugSet page; set_init(&page);
        int clean_seen = 0, stable = 0, ok200 = 0;

        for (int t = 0; t < MAX_TRY; t++) {
            if (pg == 1) snprintf(url, sizeof(url), "%s/user/%s/favorite/", base, login);
            else         snprintf(url, sizeof(url), "%s/user/%s/favorite/?&page=%d", base, login, pg);

            HttpResp rr;
            if (!http_request("GET", url, NULL, NULL, 15000, BODY_LIMIT, &rr) ||
                rr.status != 200 || !rr.body) { free(rr.body); Sleep(400); continue; }
            ok200 = 1;
            int dirty  = (strstr(rr.body, "Bad Gateway") != NULL);
            int before = page.n;
            extract_fav_slugs(rr.body, &page);
            free(rr.body);

            if (!dirty) clean_seen = 1;
            if (page.n == before) stable++; else stable = 0;

            if (pg <  M && page.n >= FULL_PAGE) break;               /* полная страница собрана */
            if (pg == M && (clean_seen || (stable >= CONVERGE && page.n > 0))) break;
            Sleep(400);
        }

        /* Проверка полноты. Не уверены -> отмена всего синка. */
        int complete;
        if (!ok200) complete = 0;
        else if (pg < M) complete = (page.n >= FULL_PAGE) || clean_seen;
        else complete = clean_seen || (stable >= CONVERGE && page.n > 0);

        if (!complete) {
            LOG_WARN(T("Синк избранного ОТМЕНЁН: страница %d прочитана неполно (собрано %d) — ничего не удаляем.", "Favorites sync CANCELLED: page %d read incompletely (collected %d) — deleting nothing."),
                     pg, page.n);
            set_free(&page); set_free(&site); return;
        }
        for (int i = 0; i < page.n; i++) set_add(&site, page.a[i]);
        LOG_INFO(T("Синк: страница %d прочитана (на ней %d, всего в базе %d)", "Sync: page %d read (on it %d, total %d)"), pg, page.n, site.n);
        set_free(&page);
    }

    if (site.n == 0) {
        LOG_WARN(T("Синк избранного отменён: список с сайта пуст.", "Favorites sync cancelled: site list is empty."));
        set_free(&site); return;
    }

    /* Второй рубеж: на M страницах должно быть минимум 24*(M-1)+1 картинок
     * (каждая НЕпоследняя страница = ровно 24). Собрали меньше -> где-то
     * недочитали, НЕ удаляем ничего. */
    if (site.n < 24 * (M - 1) + 1) {
        LOG_WARN(T("Синк избранного ОТМЕНЁН: собрано %d, ожидалось >= %d (страниц %d) — ничего не удаляем.", "Favorites sync CANCELLED: collected %d, expected >= %d (pages %d) — deleting nothing."),
                 site.n, 24 * (M - 1) + 1, M);
        set_free(&site); return;
    }

    /* Удаление локальных картинок, которых нет в избранном на сайте.
     * Чистим корень Favorite и все подпапки Favorite\<тема> (сайт плоский). */
    WCHAR wsave[MAX_PATH], favorite_base[MAX_PATH];
    utf8_to_wide(g_cfg.save_dir, wsave, MAX_PATH);
    wcscpy(favorite_base, wsave); PathAppendW(favorite_base, L"Favorite");

    int deleted = 0, kept = 0;

    /* список директорий для чистки: сам Favorite + его подпапки */
    WCHAR dirs[128][MAX_PATH]; int nd = 0;
    wcscpy(dirs[nd++], favorite_base);
    WCHAR pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%s\\*", favorite_base);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (nd < 128) _snwprintf(dirs[nd++], MAX_PATH, L"%s\\%s", favorite_base, fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    for (int di = 0; di < nd; di++) {
        int n = 0;
        FileEnt *files = dir_scan(dirs[di], &n);
        if (!files) continue;
        for (int i = 0; i < n; i++) {
            WCHAR wbase[MAX_PATH];
            wcscpy(wbase, PathFindFileNameW(files[i].path));
            WCHAR *dot = wcsrchr(wbase, L'.'); if (dot) *dot = 0;
            char slug8[160]; wide_to_utf8(wbase, slug8, sizeof(slug8)); CharLowerA(slug8);
            if (set_has(&site, slug8)) { kept++; continue; }
            if (DeleteFileW(files[i].path)) {
                deleted++;
                char f8[MAX_PATH * 3]; wide_to_utf8(files[i].path, f8, sizeof(f8));
                LOG_INFO(T("Удалено (нет в избранном на сайте): %s", "Deleted (not in favorites on site): %s"), f8);
            }
        }
        free(files);
    }

    LOG_INFO(T("Синк избранного завершён: на сайте %d, оставлено %d, удалено %d.", "Favorites sync finished: on site %d, kept %d, deleted %d."),
             site.n, kept, deleted);
    set_free(&site);
}

/* ================= Автозапуск (реестр) ================= */

static int autostart_enabled(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &k))
        return 0;
    DWORD type, sz = 0;
    int on = RegQueryValueExW(k, APP_NAME, NULL, &type, NULL, &sz) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

static void autostart_toggle(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
        KEY_READ | KEY_WRITE, &k))
        return;
    if (autostart_enabled())
        RegDeleteValueW(k, APP_NAME);
    else {
        WCHAR exe[MAX_PATH + 2];
        exe[0] = L'"';
        GetModuleFileNameW(NULL, exe + 1, MAX_PATH);
        wcscat(exe, L"\"");
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (BYTE *)exe,
                       (DWORD)((wcslen(exe) + 1) * sizeof(WCHAR)));
    }
    RegCloseKey(k);
}

/* ================= Рабочие потоки ================= */

static DWORD WINAPI worker_thread(LPVOID param)
{
    /* rand() в MSVC потоко-локальный — сидируем каждый поток отдельно,
     * иначе rand() стартует с сида 1 и всегда выдаёт одно и то же. */
    LARGE_INTEGER li; QueryPerformanceCounter(&li);
    srand((unsigned)(li.LowPart ^ li.HighPart ^ GetCurrentThreadId()));

    int action = (int)(INT_PTR)param;
    switch (action) {
        case IDM_UPDATE: do_update(); break;
        case IDM_FAVORITE:   do_favorite();   break;
        case IDM_UNFAVORITE: do_unfavorite(); break;
        case IDM_LOGIN:
            if (ensure_session()) {
                LOG_INFO(T("Авторизация через меню успешна.", "Sign-in from menu successful."));
                notify_user(L"GoodFon", TW(L"Авторизация успешна.", L"Authorization successful."));
                if (g_set_hwnd) PostMessageW(g_set_hwnd, WM_APP_LOGINRESULT, 1, 0);
                stats_refresh_async();
            } else {
                LOG_WARN(T("Авторизация через меню не удалась (проверьте логин и пароль).", "Sign-in from menu failed (check login and password)."));
                notify_user(L"GoodFon", TW(L"Не удалось войти: проверьте логин и пароль.", L"Sign-in failed: check login and password."));
                if (g_set_hwnd) PostMessageW(g_set_hwnd, WM_APP_LOGINRESULT, 2, 0);
            }
            break;
    }
    InterlockedExchange(&g_busy, 0);
    return 0;
}

static void run_async(int action)
{
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
        LOG_WARN(T("Предыдущая операция ещё выполняется, пропуск.", "Previous operation still running, skipping."));
        return;
    }
    HANDLE h = CreateThread(NULL, 0, worker_thread, (LPVOID)(INT_PTR)action, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_busy, 0);
}

/* Стартовый поток: сначала синхронизация избранного, затем первая смена обоев. */
static DWORD WINAPI startup_thread(LPVOID param)
{
    (void)param;
    LARGE_INTEGER li; QueryPerformanceCounter(&li);
    srand((unsigned)(li.LowPart ^ li.HighPart ^ GetCurrentThreadId()));

    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return 0;
    sync_favorites();     /* безопасно: при любом сомнении ничего не удаляет */
    do_update();          /* первая смена обоев */
    InterlockedExchange(&g_busy, 0);
    return 0;
}


/* Открыть страницу регистрации сайта в браузере (в приложении нельзя из-за
 * reCAPTCHA на форме регистрации). */
static void select_theme(int idx);   /* fwd */
static void stats_clear(void);        /* fwd */

/* Быстрый некриптографический хэш файла (FNV-1a 64) — для сравнения exe. */
static unsigned long long hash_file(const WCHAR *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    unsigned long long hsh = 1469598103934665603ULL;
    unsigned char buf[65536]; DWORD rd = 0;
    while (ReadFile(h, buf, sizeof(buf), &rd, NULL) && rd) {
        for (DWORD i = 0; i < rd; i++) { hsh ^= buf[i]; hsh *= 1099511628211ULL; }
    }
    CloseHandle(h);
    return hsh;
}

/* Проверка и установка обновления по ХЭШУ exe: качаем удалённый exe во временный
 * файл, сравниваем его хэш с текущим. Совпал — уже актуально; разошёлся — ставим.
 * Замена работающего файла: self -> <exe>.old, апдейт -> self, запуск, выход. */
static void upd_status(int code)
{
    g_update_status = code;
    if (g_set_hwnd) PostMessageW(g_set_hwnd, WM_APP_UPDATERESULT, code, 0);
}

static void check_update(int silent)
{
    WCHAR self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
    WCHAR dir[MAX_PATH];  wcscpy(dir, self); PathRemoveFileSpecW(dir);
    WCHAR upd[MAX_PATH];  PathCombineW(upd, dir, L"GoodFon-update.exe");

    upd_status(1);   /* проверяю */
    if (!silent) notify_user(APP_NAME, TW(L"Проверяю обновления…", L"Checking for updates…"));

    if (!fetch_url_raw(UPDATE_EXE_URL, upd, NULL, 0)) {
        upd_status(4);
        LOG_WARN(T("Обновление: не удалось скачать exe с GitHub.",
                   "Update: failed to download exe from GitHub."));
        if (!silent) notify_user(APP_NAME,
            TW(L"Не удалось проверить обновления.", L"Failed to check for updates."));
        DeleteFileW(upd);
        return;
    }

    /* убедимся, что это настоящий exe (MZ) и не подозрительно мал */
    int valid = 0;
    HANDLE hf = CreateFileW(upd, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        char mz[2] = {0}; DWORD rd = 0;
        LARGE_INTEGER sz; sz.QuadPart = 0; GetFileSizeEx(hf, &sz);
        ReadFile(hf, mz, 2, &rd, NULL);
        CloseHandle(hf);
        if (rd == 2 && mz[0] == 'M' && mz[1] == 'Z' && sz.QuadPart > 50000) valid = 1;
    }
    if (!valid) {
        upd_status(4);
        LOG_WARN(T("Обновление: скачанный файл не похож на exe, отмена.",
                   "Update: downloaded file is not a valid exe, aborting."));
        if (!silent) notify_user(APP_NAME,
            TW(L"Файл обновления повреждён.", L"Update file is corrupted."));
        DeleteFileW(upd);
        return;
    }

    unsigned long long h_self = hash_file(self);
    unsigned long long h_new  = hash_file(upd);
    LOG_INFO(T("Проверка обновлений: хэш текущего %08lX%08lX, нового %08lX%08lX",
               "Update check: current hash %08lX%08lX, new %08lX%08lX"),
             (unsigned long)(h_self >> 32), (unsigned long)h_self,
             (unsigned long)(h_new  >> 32), (unsigned long)h_new);

    if (h_new == 0) {   /* не смогли прочитать скачанный файл */
        upd_status(4);
        if (!silent) notify_user(APP_NAME,
            TW(L"Не удалось проверить обновления.", L"Failed to check for updates."));
        DeleteFileW(upd);
        return;
    }
    if (h_self == h_new) {          /* байт-в-байт совпадает — обновлять нечего */
        upd_status(2);
        DeleteFileW(upd);
        LOG_INFO(T("Обновлений нет: версия совпадает.", "No updates: version matches."));
        if (!silent) notify_user(APP_NAME,
            TW(L"У вас последняя версия.", L"You have the latest version."));
        return;
    }

    WCHAR old[MAX_PATH]; _snwprintf(old, MAX_PATH, L"%s.old", self);
    DeleteFileW(old);
    if (!MoveFileExW(self, old, MOVEFILE_REPLACE_EXISTING)) {
        LOG_ERROR(T("Обновление: не удалось переименовать текущий exe.",
                    "Update: failed to rename the current exe."));
        DeleteFileW(upd);
        return;
    }
    if (!MoveFileExW(upd, self, MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old, self, MOVEFILE_REPLACE_EXISTING);   /* откат */
        LOG_ERROR(T("Обновление: не удалось поставить новый exe, откат.",
                    "Update: failed to install the new exe, rolled back."));
        return;
    }

    LOG_INFO(T("Обновление установлено, перезапуск.", "Update installed, restarting."));
    upd_status(3);
    notify_user(APP_NAME, TW(L"Обновление установлено, перезапуск…",
                             L"Update installed, restarting…"));
    ShellExecuteW(NULL, L"open", self, NULL, dir, SW_SHOWNORMAL);
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    ExitProcess(0);
}

/* Запуск проверки обновлений в отдельном потоке (не блокирует трей и не мешает g_busy). */
static DWORD WINAPI update_thread(LPVOID p) { check_update((int)(INT_PTR)p); return 0; }
static void run_update_async(int silent)
{
    HANDLE t = CreateThread(NULL, 0, update_thread, (LPVOID)(INT_PTR)silent, 0, NULL);
    if (t) CloseHandle(t);
}

static void account_register(void)
{
    char base[64]; base_url(base, sizeof(base));
    char url8[128]; snprintf(url8, sizeof(url8), "%s/auth/registration/", base);
    WCHAR url[128]; utf8_to_wide(url8, url, 128);
    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    LOG_INFO(T("Открыта страница регистрации: %s", "Opened registration page: %s"), url8);
    notify_user(L"GoodFon",
                TW(L"Открыл регистрацию в браузере. После неё войдите в окне настроек, раздел «Аккаунт».", L"Opened registration in the browser. Then sign in from the Settings window, Account section."));
}

/* Локальный выход из аккаунта: чистим логин/пароль/сессии. Сайт не трогаем. */
static void account_logout(void)
{
    g_cfg.login[0] = 0; g_cfg.password[0] = 0;
    g_jar_com[0] = 0;   g_jar_ru[0] = 0;
    reg_set_str(L"login", "");
    reg_set_str(L"session_com", "");
    reg_set_str(L"session_ru", "");
    HKEY k = reg_open(KEY_WRITE);
    if (k) { RegDeleteValueW(k, L"password_enc"); RegCloseKey(k); }

    /* Раздел "Эротика" доступен только с авторизацией — при выходе уходим с него. */
    if (!_stricmp(g_cfg.theme, "erotic")) {
        int gi = -1;
        for (int i = 0; i < THEME_COUNT; i++)
            if (!_stricmp(g_themes_all[i].slug, "girls")) { gi = i; break; }
        if (gi >= 0) select_theme(gi);
        LOG_INFO(T("Тема переключена с \"erotic\" на \"girls\" (выход из аккаунта).", "Theme switched from 'erotic' to 'girls' (sign out)."));
    }

    LOG_INFO(T("Выход из аккаунта: локальные учётные данные и кэш сессий очищены.", "Signed out: local credentials and session cache cleared."));
    notify_user(L"GoodFon", TW(L"Выход из аккаунта выполнен.", L"Signed out."));

    stats_clear();
    { HKEY k = reg_open(KEY_WRITE); if (k) { RegDeleteValueW(k, L"profile_stats"); RegCloseKey(k); } }
}

/* ===================== Профильная статистика ===================== */

static void stats_clear(void)
{
    if (g_stats.avatar_bmp) { DeleteObject(g_stats.avatar_bmp); g_stats.avatar_bmp = NULL; }
    memset(&g_stats, 0, sizeof(g_stats));
}

static void stats_save_reg(void)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s|%ld|%ld|%ld|%s",
             g_stats.rating[0] ? g_stats.rating : "0",
             g_stats.walls, g_stats.downloads, g_stats.comments, g_stats.avatar_url);
    reg_set_str(L"profile_stats", buf);
}

static void stats_load_reg(void)
{
    char buf[512];
    if (!reg_get_str(L"profile_stats", buf, sizeof(buf)) || !buf[0]) return;
    char *ctx = NULL;
    char *r = strtok_s(buf, "|", &ctx);
    char *w = strtok_s(NULL, "|", &ctx);
    char *d = strtok_s(NULL, "|", &ctx);
    char *c = strtok_s(NULL, "|", &ctx);
    char *a = strtok_s(NULL, "|", &ctx);
    if (r) { strncpy(g_stats.rating, r, sizeof(g_stats.rating) - 1); g_stats.rating[sizeof(g_stats.rating)-1]=0; }
    g_stats.walls     = w ? atol(w) : 0;
    g_stats.downloads = d ? atol(d) : 0;
    g_stats.comments  = c ? atol(c) : 0;
    if (a) { strncpy(g_stats.avatar_url, a, sizeof(g_stats.avatar_url) - 1); g_stats.avatar_url[sizeof(g_stats.avatar_url)-1]=0; }
    g_stats.valid = 1;
}

/* Декодировать изображение (любой формат, поддержанный WIC, вкл. webp) в HBITMAP w×h. */
static HBITMAP load_image_hbitmap(const WCHAR *path, int W, int H)
{
    HBITMAP hbmp = NULL;
    IWICImagingFactory   *fac = NULL;
    IWICBitmapDecoder    *dec = NULL;
    IWICBitmapFrameDecode*frm = NULL;
    IWICBitmapScaler     *scl = NULL;
    IWICFormatConverter  *cnv = NULL;

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&fac)))
        return NULL;
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(fac, path, NULL, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &dec))) goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frm))) goto done;
    if (FAILED(IWICImagingFactory_CreateBitmapScaler(fac, &scl))) goto done;
    if (FAILED(IWICBitmapScaler_Initialize(scl, (IWICBitmapSource *)frm, W, H,
                WICBitmapInterpolationModeFant))) goto done;
    if (FAILED(IWICImagingFactory_CreateFormatConverter(fac, &cnv))) goto done;
    if (FAILED(IWICFormatConverter_Initialize(cnv, (IWICBitmapSource *)scl,
                &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
                WICBitmapPaletteTypeCustom))) goto done;
    {
        BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        HDC hdc = GetDC(NULL);
        hbmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        ReleaseDC(NULL, hdc);
        if (hbmp && bits) {
            if (FAILED(IWICFormatConverter_CopyPixels(cnv, NULL, W * 4, W * 4 * H, (BYTE *)bits))) {
                DeleteObject(hbmp); hbmp = NULL;
            }
        }
    }
done:
    if (cnv) IWICFormatConverter_Release(cnv);
    if (scl) IWICBitmapScaler_Release(scl);
    if (frm) IWICBitmapFrameDecode_Release(frm);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (fac) IWICImagingFactory_Release(fac);
    return hbmp;
}

/* Извлечь URL аватара рядом с якорным классом (в пределах окна),
 * терпит кавычки, url(), протокол-относительные // и экранированные \/. */
static void extract_avatar_near(const char *anchor, const char *base_body,
                                char *out, size_t outsz)
{
    (void)base_body;
    if (!anchor || out[0]) return;
    const char *a = strstr(anchor, "avatars");
    if (!a || (a - anchor) > 400) return;          /* только рядом с якорем */
    const char *s = a;
    while (s > anchor && *s != '"' && *s != '\'' && *s != '(' && *s != ' ' && *s != '=') s--;
    s++;
    char raw[300]; int n = 0; const char *e = s;
    while (*e && *e != '"' && *e != '\'' && *e != ')' && *e != ' ' && *e != '>'
           && *e != '\r' && *e != '\n' && n < 299) {
        if (e[0] == '\\' && e[1] == '/') { raw[n++] = '/'; e += 2; }
        else raw[n++] = *e++;
    }
    raw[n] = 0;
    if (strncmp(raw, "//", 2) == 0)      snprintf(out, outsz, "https:%s", raw);
    else if (raw[0] == '/')              snprintf(out, outsz, "https://img.goodfon.com%s", raw);
    else { strncpy(out, raw, outsz - 1); out[outsz - 1] = 0; }
}

/* Считать число из строки, пропуская пробелы-разделители тысяч: "119 236" -> 119236. */
static long parse_grouped_number(const char *p)
{
    long v = 0;
    while (*p && (*p == ' ' || (*p >= '0' && *p <= '9'))) {
        if (*p >= '0' && *p <= '9') v = v * 10 + (*p - '0');
        p++;
    }
    return v;
}

/* Загрузить и разобрать блок статистики с профиля. Возвращает 1 при успехе. */
static int fetch_profile_stats(void)
{
    if (!is_authorized() || !g_cfg.login[0]) return 0;

    char base[64]; base_url(base, sizeof(base));
    char url[160];
    HttpResp r; memset(&r, 0, sizeof(r));
    const char *blk = NULL;

    /* Полный блок профиля (аватар/рейтинг/счётчики) есть на странице избранного;
     * пустая страница /user/<login>/ его может не содержать. */
    snprintf(url, sizeof(url), "%s/user/%s/favorite/", base, g_cfg.login);
    if (http_request("GET", url, NULL, NULL, 30000, BODY_LIMIT, &r) && r.body)
        blk = strstr(r.body, "user_stat_block");
    if (!blk) {
        free(r.body); memset(&r, 0, sizeof(r));
        snprintf(url, sizeof(url), "%s/user/%s/", base, g_cfg.login);
        if (http_request("GET", url, NULL, NULL, 30000, BODY_LIMIT, &r) && r.body)
            blk = strstr(r.body, "user_stat_block");
    }
    if (!blk) { free(r.body); LOG_WARN(T("Статистика: блок user_stat_block не найден в HTML.",
                                          "Stats: user_stat_block not found in HTML.")); return 0; }
    LOG_INFO(T("Статистика: блок найден на %s", "Stats: block found at %s"), url);

    ProfileStats ns; memset(&ns, 0, sizeof(ns));
    strcpy(ns.rating, "0");

    /* аватарка: сначала по классу шапки headline__user__avatar (это всегда
     * текущий пользователь), затем по class="avatar" внутри блока профиля */
    {
        extract_avatar_near(strstr(r.body, "headline__user__avatar"), r.body,
                            ns.avatar_url, sizeof(ns.avatar_url));
        if (!ns.avatar_url[0])
            extract_avatar_near(strstr(blk, "class=\"avatar\""), r.body,
                                ns.avatar_url, sizeof(ns.avatar_url));
        if (!ns.avatar_url[0])
            LOG_INFO(T("Аватар: URL не найден по классам (грузится через JS?).",
                       "Avatar: URL not found by class (JS-loaded?)."));
    }
    /* рейтинг: "рейтинг 5.00" (RU) или "rating 5.00" (COM) */
    const char *rt = strstr(blk, "\xD1\x80\xD0\xB5\xD0\xB9\xD1\x82\xD0\xB8\xD0\xBD\xD0\xB3"); /* "рейтинг" */
    int mlen = 14;
    if (!rt) { rt = strstr(blk, "rating"); mlen = 6; }
    if (rt) {
        rt += mlen;
        int guard = 0;
        while (*rt && !(*rt >= '0' && *rt <= '9') && guard < 48) { rt++; guard++; }
        int i = 0;
        while (*rt && ((*rt >= '0' && *rt <= '9') || *rt == '.') && i < 15) ns.rating[i++] = *rt++;
        ns.rating[i] = 0;
        if (!ns.rating[0]) strcpy(ns.rating, "0");
    }
    /* нижний блок: обоев / скачиваний / комментариев */
    const char *bot = strstr(blk, "user_stat_block__bottom");
    if (bot) {
        const char *q = bot; long *dst[3] = { &ns.walls, &ns.downloads, &ns.comments };
        for (int i = 0; i < 3; i++) {
            q = strstr(q, "<div>");
            if (!q) break;
            q += 5;
            *dst[i] = parse_grouped_number(q);
        }
    }
    free(r.body);

    /* аватарку скачиваем и декодируем */
    HBITMAP bmp = NULL;
    if (ns.avatar_url[0]) {
        WCHAR self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
        WCHAR dir[MAX_PATH]; wcscpy(dir, self); PathRemoveFileSpecW(dir);
        WCHAR af[MAX_PATH];  PathCombineW(af, dir, L"avatar.img");
        WCHAR wurl[512]; utf8_to_wide(ns.avatar_url, wurl, 512);
        if (fetch_url_raw(wurl, af, NULL, 0)) {
            bmp = load_image_hbitmap(af, 32, 32);
            LOG_INFO(bmp ? T("Аватар декодирован: %s", "Avatar decoded: %s")
                         : T("Аватар не декодировался (нет WebP-кодека?): %s",
                             "Avatar decode failed (no WebP codec?): %s"), ns.avatar_url);
        } else {
            LOG_WARN(T("Аватар: не удалось скачать %s", "Avatar: download failed %s"), ns.avatar_url);
        }
        DeleteFileW(af);
    } else {
        LOG_INFO(T("Аватар: URL в блоке профиля не найден.", "Avatar: URL not found in profile block."));
    }

    ns.avatar_bmp = bmp;
    ns.valid = 1;

    /* заменяем глобальную статистику (старый bmp освобождаем) */
    HBITMAP old = g_stats.avatar_bmp;
    g_stats = ns;
    if (old && old != bmp) DeleteObject(old);

    stats_save_reg();
    LOG_INFO(T("Статистика профиля обновлена: рейтинг %s, обоев %ld, скачиваний %ld, комментариев %ld",
               "Profile stats updated: rating %s, wallpapers %ld, downloads %ld, comments %ld"),
             g_stats.rating, g_stats.walls, g_stats.downloads, g_stats.comments);
    return 1;
}

static DWORD WINAPI stats_thread(LPVOID p)
{
    (void)p;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int ok = fetch_profile_stats();
    CoUninitialize();
    if (ok && g_set_hwnd) PostMessageW(g_set_hwnd, WM_APP_STATS, 0, 0);
    return 0;
}
static void stats_refresh_async(void)
{
    HANDLE t = CreateThread(NULL, 0, stats_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* ================= Трей и меню ================= */

static void tray_add(void)
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APPICON),
                                    IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), 0);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy(g_nid.szTip, TW(L"GoodFon — смена обоев", L"GoodFon — wallpaper changer"));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

/* ============================ Окно настроек ============================ */
static void apply_interval(void);      /* fwd */
static void apply_update_interval(void); /* fwd */
static void open_settings(void);       /* fwd */

static int   g_set_page = 0;
static int   g_login_status = 0;   /* 0 нет, 1 успех, 2 ошибка — статус входа в окне */
static HFONT g_set_font = NULL, g_set_font_title = NULL, g_set_font_icon = NULL;

/* Подкласс комбобокса: в тёмной теме полностью рисуем его сами (фон, рамка,
 * текст выбранного пункта, стрелка) — чтобы не было белой рамки/кнопки. */
static LRESULT CALLBACK ComboSubProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (WNDPROC)GetPropW(h, L"gf_old");
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        int bw = GetSystemMetrics(SM_CXVSCROLL) + 4;

        FillRect(dc, &rc, g_br_bg);   /* фон окна — чтобы углы скругления слились */

        HBRUSH bg = CreateSolidBrush(cr_ctl());
        HPEN   pen = CreatePen(PS_SOLID, 1, cr_border());
        HGDIOBJ ob = SelectObject(dc, bg), op = SelectObject(dc, pen);
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 12, 12);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(bg); DeleteObject(pen);

        int sel = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
        if (sel >= 0) {
            WCHAR t[160]; SendMessageW(h, CB_GETLBTEXT, sel, (LPARAM)t);
            HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
            HGDIOBJ of = SelectObject(dc, f);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, cr_txt());
            RECT tr = rc; tr.left += 10; tr.right -= bw;
            DrawTextW(dc, t, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, of);
        }
        /* стрелка */
        int cx = rc.right - bw / 2 - 2, cy = (rc.top + rc.bottom) / 2;
        POINT p[3] = { { cx - 4, cy - 2 }, { cx + 4, cy - 2 }, { cx, cy + 3 } };
        HBRUSH ab = CreateSolidBrush(cr_txt());
        HPEN   ap = CreatePen(PS_SOLID, 1, cr_txt());
        HGDIOBJ ob2 = SelectObject(dc, ab), op2 = SelectObject(dc, ap);
        Polygon(dc, p, 3);
        SelectObject(dc, ob2); SelectObject(dc, op2);
        DeleteObject(ab); DeleteObject(ap);

        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        WNDPROC o = old;
        RemovePropW(h, L"gf_old");
        return CallWindowProcW(o, h, msg, wp, lp);
    }
    return CallWindowProcW(old, h, msg, wp, lp);
}
static void combo_subclass(HWND cb)
{
    WNDPROC old = (WNDPROC)SetWindowLongPtrW(cb, GWLP_WNDPROC, (LONG_PTR)ComboSubProc);
    SetPropW(cb, L"gf_old", (HANDLE)old);
}
static BOOL CALLBACK subclass_combos_cb(HWND c, LPARAM lp)
{
    (void)lp; WCHAR cls[16]; GetClassNameW(c, cls, 16);
    if (!lstrcmpiW(cls, L"COMBOBOX")) combo_subclass(c);
    return TRUE;
}

static void set_titlebar_dark(HWND h, int dark)
{
    BOOL v = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(h, 20, &v, sizeof(v));   /* DWMWA_USE_IMMERSIVE_DARK_MODE (2004+) */
    DwmSetWindowAttribute(h, 19, &v, sizeof(v));   /* старое значение (1809/1903)           */
}

/* Включить тёмный режим на уровне приложения (недокументированный uxtheme, ordinal 135).
 * Нужен, чтобы комбобоксы/кнопки/скроллбары рисовались тёмными. Мягко деградирует. */
static void enable_dark_mode_app(int dark)
{
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    typedef int  (WINAPI *SetPreferredAppMode_t)(int);
    typedef void (WINAPI *FlushMenuThemes_t)(void);
    SetPreferredAppMode_t setmode = (SetPreferredAppMode_t)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    FlushMenuThemes_t     flush   = (FlushMenuThemes_t)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setmode) setmode(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (flush)   flush();
    FreeLibrary(ux);
}

static BOOL CALLBACK theme_ctl_cb(HWND c, LPARAM lp)
{
    WCHAR cls[32]; GetClassNameW(c, cls, 32);
    int dark = (int)lp;
    if (!lstrcmpiW(cls, L"COMBOBOX") || !lstrcmpiW(cls, L"EDIT"))
        SetWindowTheme(c, dark ? L"DarkMode_CFD" : NULL, NULL);
    else
        SetWindowTheme(c, dark ? L"DarkMode_Explorer" : NULL, NULL);
    return TRUE;
}

static void settings_apply_theme(HWND dlg)
{
    theme_brushes_rebuild();
    enable_dark_mode_app(g_ui_theme);
    set_titlebar_dark(dlg, g_ui_theme);
    EnumChildWindows(dlg, theme_ctl_cb, (LPARAM)g_ui_theme);
    InvalidateRect(dlg, NULL, TRUE);
}

/* Показать контролы только текущей страницы (GWLP_USERDATA: -1 = всегда видим). */
static BOOL CALLBACK show_page_cb(HWND c, LPARAM lp)
{
    LONG_PTR pg = GetWindowLongPtrW(c, GWLP_USERDATA);
    ShowWindow(c, (pg == (LONG_PTR)-1 || pg == (LONG_PTR)lp) ? SW_SHOW : SW_HIDE);
    return TRUE;
}

static void settings_set_page(HWND dlg, int page)
{
    g_set_page = page;
    EnumChildWindows(dlg, show_page_cb, (LPARAM)page);
    const WCHAR *titles[4] = {
        TW(L"Настройки картинок", L"Image settings"),
        TW(L"Аккаунт",            L"Account"),
        TW(L"Дополнительно",      L"Additional"),
        TW(L"О приложении",       L"About"),
    };
    SetDlgItemTextW(dlg, IDC_PGTITLE, titles[page]);
    if (page == 1) {
        int a = is_authorized();
        ShowWindow(GetDlgItem(dlg, IDC_BTN_SIGNOUT), a ? SW_SHOW : SW_HIDE);
    }
    InvalidateRect(dlg, NULL, TRUE);
}

static HWND mk(HWND p, const WCHAR *cls, const WCHAR *txt, DWORD st,
               int x, int y, int w, int h, int id, int page)
{
    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | st, x, y, w, h,
                             p, (HMENU)(INT_PTR)id, g_hinst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_set_font, TRUE);
    SetWindowLongPtrW(c, GWLP_USERDATA, (LONG_PTR)page);
    return c;
}
static void cb_add(HWND cb, const WCHAR *t, INT_PTR data)
{
    int i = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
    SendMessageW(cb, CB_SETITEMDATA, i, (LPARAM)data);
}

static void settings_relaunch(void)   /* пересоздать окно (для смены языка/темы) */
{
    int pg = g_set_page;
    HWND o = g_set_hwnd; g_set_hwnd = NULL;
    if (o) DestroyWindow(o);
    open_settings();
    if (g_set_hwnd) {
        SendMessageW(GetDlgItem(g_set_hwnd, IDC_NAV), LB_SETCURSEL, pg, 0);
        settings_set_page(g_set_hwnd, pg);
    }
}

static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        if (g_set_page == 1) {
            int ids[2] = { IDC_ED_LOGIN, IDC_ED_PASS };
            for (int i = 0; i < 2; i++) {
                HWND e = GetDlgItem(h, ids[i]);
                if (!e) continue;
                RECT r; GetWindowRect(e, &r);
                POINT tl = { r.left, r.top }, br = { r.right, r.bottom };
                ScreenToClient(h, &tl); ScreenToClient(h, &br);
                RECT box = { tl.x - 8, tl.y - 7, br.x + 8, br.y + 1 };
                HBRUSH bg = CreateSolidBrush(cr_ctl());
                HPEN pen = CreatePen(PS_SOLID, 1, cr_border());
                HGDIOBJ ob = SelectObject(dc, bg), op = SelectObject(dc, pen);
                RoundRect(dc, box.left, box.top, box.right, box.bottom, 10, 10);
                SelectObject(dc, ob); SelectObject(dc, op);
                DeleteObject(bg); DeleteObject(pen);
            }
            /* ----- блок профильной статистики (сверху страницы) ----- */
            {
                int x = 186, y = 56, w = 372;
                /* аватарка 32x32: с сайта или иконка приложения */
                if (g_stats.avatar_bmp) {
                    HDC mdc = CreateCompatibleDC(dc);
                    HGDIOBJ ob = SelectObject(mdc, g_stats.avatar_bmp);
                    BitBlt(dc, x, y, 32, 32, mdc, 0, 0, SRCCOPY);
                    SelectObject(mdc, ob); DeleteDC(mdc);
                } else if (g_nid.hIcon) {
                    DrawIconEx(dc, x, y, g_nid.hIcon, 32, 32, 0, NULL, DI_NORMAL);
                }

                SetBkMode(dc, TRANSPARENT);
                /* имя пользователя */
                HGDIOBJ of = SelectObject(dc, g_set_font);
                SetTextColor(dc, cr_txt());
                WCHAR nm[130];
                if (is_authorized() && g_cfg.login[0]) utf8_to_wide(g_cfg.login, nm, 130);
                else wcscpy(nm, TW(L"Не выполнен вход", L"Not signed in"));
                RECT rn = { x + 44, y, x + w, y + 16 };
                DrawTextW(dc, nm, -1, &rn, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                /* рейтинг */
                SetTextColor(dc, g_ui_theme ? RGB(150,150,150) : RGB(110,110,110));
                WCHAR rr[48];
                _snwprintf(rr, 48, TW(L"рейтинг %hs", L"rating %hs"),
                           g_stats.valid && g_stats.rating[0] ? g_stats.rating : "0");
                RECT rrt = { x + 44, y + 16, x + w, y + 32 };
                DrawTextW(dc, rr, -1, &rrt, DT_LEFT | DT_SINGLELINE);
                /* счётчики */
                SetTextColor(dc, cr_txt());
                WCHAR cnt[160];
                _snwprintf(cnt, 160, TW(L"%ld обоев      %ld скачиваний      %ld комментариев",
                                        L"%ld wallpapers      %ld downloads      %ld comments"),
                           g_stats.walls, g_stats.downloads, g_stats.comments);
                RECT rc3 = { x, y + 44, x + w, y + 62 };
                DrawTextW(dc, cnt, -1, &rc3, DT_LEFT | DT_SINGLELINE);
                SelectObject(dc, of);

                /* разделитель под блоком (перед авторизацией) */
                HPEN sp = CreatePen(PS_SOLID, 1, cr_border());
                HGDIOBJ osp = SelectObject(dc, sp);
                MoveToEx(dc, x, y + 84, NULL); LineTo(dc, x + w, y + 84);
                SelectObject(dc, osp); DeleteObject(sp);
            }
        }
        else if (g_set_page == 3) {
            SetBkMode(dc, TRANSPARENT);
            int x = 186, w = 372, y = 52;
            /* название */
            HGDIOBJ of = SelectObject(dc, g_set_font_title);
            SetTextColor(dc, cr_txt());
            RECT rt = { x, y, x + w, y + 26 };
            DrawTextW(dc, L"GoodFon 2.2", -1, &rt, DT_LEFT | DT_SINGLELINE);
            y += 34;
            /* описание с переносом по словам */
            SelectObject(dc, g_set_font);
            const WCHAR *desc = TW(
                L"Автоматически меняет обои рабочего стола, подбирая изображения "
                L"с сайта goodfon.com / goodfon.ru по выбранным теме и разрешению.\r\n\r\n"
                L"Умеет добавлять понравившиеся обои в избранное и синхронизировать "
                L"его с вашим аккаунтом на сайте, работает по заданному интервалу, "
                L"живёт в системном трее и потребляет минимум ресурсов.\r\n\r\n"
                L"Написано на чистом C (WinAPI), без сторонних библиотек.",
                L"Automatically changes your desktop wallpaper, picking images from "
                L"goodfon.com / goodfon.ru by the chosen theme and resolution.\r\n\r\n"
                L"Can add favorite wallpapers and sync them with your account on the "
                L"site, runs on a set interval, lives in the system tray and uses "
                L"minimal resources.\r\n\r\n"
                L"Written in pure C (WinAPI), with no third-party libraries.");
            SetTextColor(dc, cr_txt());
            RECT rd = { x, y, x + w, y + 150 };
            DrawTextW(dc, desc, -1, &rd, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL);
            /* копирайт снизу */
            SetTextColor(dc, g_ui_theme ? RGB(150,150,150) : RGB(110,110,110));
            RECT rc2; GetClientRect(h, &rc2);
            RECT rcp = { x, rc2.bottom - 74, x + w, rc2.bottom - 50 };
            DrawTextW(dc, L"\u00A9 Mansi (slfl) \u00B7 slfl@mail.ru",
                      -1, &rcp, DT_LEFT | DT_SINGLELINE);
            SelectObject(dc, of);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp; RECT rc; GetClientRect(h, &rc);
        FillRect(dc, &rc, g_br_bg);
        RECT nav = { 0, 0, 170, rc.bottom };
        FillRect(dc, &nav, g_br_nav);
        return 1;
    }
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, cr_txt()); SetBkColor((HDC)wp, cr_nav());
        return (LRESULT)g_br_nav;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, cr_txt()); SetBkColor((HDC)wp, cr_ctl());
        return (LRESULT)g_br_ctl;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        int cid = GetDlgCtrlID((HWND)lp);
        SetBkMode(dc, TRANSPARENT);
        if (cid == IDC_ST_STATUS)
            SetTextColor(dc, g_login_status == 1 ? RGB(40,170,80)
                           : g_login_status == 2 ? RGB(220,70,70) : cr_txt());
        else if (cid == IDC_ST_UPDATE)
            SetTextColor(dc, g_update_status == 2 || g_update_status == 3 ? RGB(40,170,80)
                           : g_update_status == 4 ? RGB(220,70,70) : cr_txt());
        else
            SetTextColor(dc, cid == IDC_LNK_REG ? cr_accent() : cr_txt());
        SetBkColor(dc, cr_bg());
        return (LRESULT)g_br_bg;
    }
    case WM_CTLCOLORBTN:
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, cr_txt()); SetBkColor((HDC)wp, cr_bg());
        return (LRESULT)g_br_bg;

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT d = (LPDRAWITEMSTRUCT)lp;
        if (d->CtlID == IDC_NAV) {
            if ((int)d->itemID < 0) return TRUE;
            int sel = (d->itemState & ODS_SELECTED) != 0;
            HBRUSH sb = sel ? CreateSolidBrush(cr_sel()) : g_br_nav;
            FillRect(d->hDC, &d->rcItem, sb);
            if (sel) {
                DeleteObject(sb);
                RECT b = d->rcItem; b.right = b.left + 3;
                HBRUSH ab = CreateSolidBrush(cr_accent()); FillRect(d->hDC, &b, ab); DeleteObject(ab);
            }
            SetBkMode(d->hDC, TRANSPARENT);
            SetTextColor(d->hDC, cr_txt());
            /* иконка (Segoe MDL2 Assets): Картинки, Аккаунт, Дополнительно */
            const WCHAR *icons[4] = { L"\uE8B9", L"\uE77B", L"\uE713", L"\uE946" };
            if (g_set_font_icon && (int)d->itemID < 4) {
                HFONT of = (HFONT)SelectObject(d->hDC, g_set_font_icon);
                RECT ir = d->rcItem; ir.left += 16;
                DrawTextW(d->hDC, icons[d->itemID], -1, &ir, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SelectObject(d->hDC, of);
            }
            WCHAR t[64]; SendMessageW(d->hwndItem, LB_GETTEXT, d->itemID, (LPARAM)t);
            RECT tr = d->rcItem; tr.left += 46;
            DrawTextW(d->hDC, t, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        if (d->CtlType == ODT_BUTTON) {
            int pressed = (d->itemState & ODS_SELECTED) != 0;
            int primary = (d->CtlID == IDC_BTN_SIGNIN);
            COLORREF fill = primary ? (pressed ? RGB(0,90,170) : cr_accent())
                                    : (pressed ? cr_btnpress() : cr_btnface());
            COLORREF txt  = primary ? RGB(255,255,255) : cr_txt();
            COLORREF bord = primary ? fill : cr_border();
            FillRect(d->hDC, &d->rcItem, g_br_bg);      /* фон под скруглением */
            HBRUSH hb = CreateSolidBrush(fill);
            HPEN   hp = CreatePen(PS_SOLID, 1, bord);
            HGDIOBJ ob = SelectObject(d->hDC, hb), op = SelectObject(d->hDC, hp);
            RoundRect(d->hDC, d->rcItem.left, d->rcItem.top,
                      d->rcItem.right, d->rcItem.bottom, 12, 12);
            SelectObject(d->hDC, ob); SelectObject(d->hDC, op);
            DeleteObject(hb); DeleteObject(hp);
            WCHAR t[64]; GetWindowTextW(d->hwndItem, t, 64);
            SetBkMode(d->hDC, TRANSPARENT);
            SetTextColor(d->hDC, txt);
            DrawTextW(d->hDC, t, -1, &d->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        HWND ctl = (HWND)lp;
        /* после выбора/закрытия списка форсируем нашу перерисовку — иначе
         * combobox дорисовывает текст своим смещением и он "прыгает". */
        if (code == CBN_CLOSEUP || code == CBN_SELCHANGE)
            InvalidateRect(ctl, NULL, TRUE);
        if (id == IDC_NAV && code == LBN_SELCHANGE) {
            settings_set_page(h, (int)SendMessageW(ctl, LB_GETCURSEL, 0, 0));
        }
        else if (id == IDC_CB_THEME && code == CBN_SELCHANGE) {
            int s = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            int idx = (int)SendMessageW(ctl, CB_GETITEMDATA, s, 0);
            if (!_stricmp(g_themes_all[idx].slug, "erotic") && !is_authorized()) {
                MessageBoxW(h, TW(L"Доступно после авторизации.", L"Available after sign in."),
                            APP_NAME, MB_ICONINFORMATION);
                int cnt = (int)SendMessageW(ctl, CB_GETCOUNT, 0, 0);
                for (int i = 0; i < cnt; i++) {
                    int di = (int)SendMessageW(ctl, CB_GETITEMDATA, i, 0);
                    if (!_stricmp(g_themes_all[di].slug, g_cfg.theme)) {
                        SendMessageW(ctl, CB_SETCURSEL, i, 0); break;
                    }
                }
            } else select_theme(idx);
        }
        else if (id == IDC_CB_INTERVAL && code == CBN_SELCHANGE) {
            int s = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            g_cfg.interval_min = (int)SendMessageW(ctl, CB_GETITEMDATA, s, 0);
            reg_set_dword(L"interval_min", g_cfg.interval_min);
            apply_interval();
        }
        else if (id == IDC_CB_RES && code == CBN_SELCHANGE) {
            int s = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            int ri = (int)SendMessageW(ctl, CB_GETITEMDATA, s, 0);
            strncpy(g_cfg.resolution, g_reses[ri].value, sizeof(g_cfg.resolution) - 1);
            reg_set_str(L"resolution", g_cfg.resolution);
        }
        else if (id == IDC_CB_FAVN && code == CBN_SELCHANGE) {
            int s = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            g_cfg.favorite_every_n = (int)SendMessageW(ctl, CB_GETITEMDATA, s, 0);
            reg_set_dword(L"favorite_every_n", g_cfg.favorite_every_n);
        }
        else if (id == IDC_BTN_SIGNIN && code == BN_CLICKED) {
            char lg[128] = {0}, pw[128] = {0}; WCHAR w[128];
            GetWindowTextW(GetDlgItem(h, IDC_ED_LOGIN), w, 128); wide_to_utf8(w, lg, sizeof(lg));
            GetWindowTextW(GetDlgItem(h, IDC_ED_PASS),  w, 128); wide_to_utf8(w, pw, sizeof(pw));
            if (!lg[0] || !pw[0]) {
                MessageBoxW(h, TW(L"Введите логин и пароль.", L"Enter login and password."),
                            APP_NAME, MB_ICONINFORMATION);
            } else {
                strncpy(g_cfg.login, lg, sizeof(g_cfg.login) - 1);
                strncpy(g_cfg.password, pw, sizeof(g_cfg.password) - 1);
                reg_set_str(L"login", g_cfg.login);
                reg_set_password(g_cfg.password);
                g_jar_com[0] = 0; g_jar_ru[0] = 0;
                reg_set_str(L"session_com", ""); reg_set_str(L"session_ru", "");
                LOG_INFO(T("Логин/пароль сохранены, выполняю вход…", "Login/password saved, signing in…"));
                g_login_status = 0;
                SetDlgItemTextW(h, IDC_ST_STATUS, TW(L"Вход…", L"Signing in…"));
                run_async(IDM_LOGIN);
                settings_set_page(h, 1);
            }
        }
        else if (id == IDC_BTN_SIGNOUT && code == BN_CLICKED) {
            account_logout();
            SetDlgItemTextW(h, IDC_ED_LOGIN, L"");
            SetDlgItemTextW(h, IDC_ED_PASS,  L"");
            g_login_status = 0;
            SetDlgItemTextW(h, IDC_ST_STATUS, L"");
            settings_set_page(h, 1);
        }
        else if (id == IDC_LNK_REG && code == STN_CLICKED) {
            account_register();
        }
        else if (id == IDC_CHK_NOTIFY && code == BN_CLICKED) {
            g_cfg.notify = (int)SendMessageW(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED;
            reg_set_dword(L"notify", g_cfg.notify);
        }
        else if (id == IDC_CHK_AUTORUN && code == BN_CLICKED) {
            autostart_toggle();
            SendMessageW(ctl, BM_SETCHECK, autostart_enabled() ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        else if (id == IDC_CB_DOMAIN && code == CBN_SELCHANGE) {
            int s = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            const char *dv = s == 1 ? "ru" : s == 2 ? "com" : "auto";
            strncpy(g_cfg.domain_pref, dv, sizeof(g_cfg.domain_pref) - 1);
            g_cfg.domain_pref[sizeof(g_cfg.domain_pref) - 1] = 0;
            reg_set_str(L"domain", g_cfg.domain_pref);
            LOG_INFO(T("Предпочтение домена: %s", "Domain preference: %s"), g_cfg.domain_pref);
        }
        else if (id == IDC_CB_UPDINT && code == CBN_SELCHANGE) {
            int s = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            g_cfg.update_interval_min = (int)SendMessageW(ctl, CB_GETITEMDATA, s, 0);
            reg_set_dword(L"update_interval_min", g_cfg.update_interval_min);
            apply_update_interval();
            LOG_INFO(T("Автопроверка обновлений: %d мин", "Auto-update check: %d min"),
                     g_cfg.update_interval_min);
        }
        else if (id == IDC_CB_LANG && code == CBN_SELCHANGE) {
            int s = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            g_lang = s == 1 ? LANG_EN : LANG_RU;
            reg_set_str(L"Language", g_lang == LANG_EN ? "english" : "russian");
            wcscpy(g_nid.szTip, TW(L"GoodFon — смена обоев", L"GoodFon — wallpaper changer"));
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            settings_relaunch();
        }
        else if (id == IDC_CB_APPTHEME && code == CBN_SELCHANGE) {
            int s = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            g_ui_theme = s == 1 ? THEME_DARK : THEME_LIGHT;
            reg_set_str(L"AppTheme", g_ui_theme == THEME_DARK ? "dark" : "light");
            settings_relaunch();
        }
        else if (id == IDC_BTN_UPDATE && code == BN_CLICKED) {
            run_update_async(0);
        }
        else if (id == IDC_BTN_CLOSE && code == BN_CLICKED) {
            DestroyWindow(h);
        }
        return 0;
    }
    case WM_APP_STATS:
        if (g_set_page == 1) InvalidateRect(h, NULL, TRUE);
        return 0;
    case WM_APP_UPDATERESULT: {
        g_update_status = (int)wp;
        HWND st = GetDlgItem(h, IDC_ST_UPDATE);
        const WCHAR *txt =
            g_update_status == 1 ? TW(L"Проверяю обновления…", L"Checking for updates…") :
            g_update_status == 2 ? TW(L"У вас последняя версия.", L"You have the latest version.") :
            g_update_status == 3 ? TW(L"Найдено обновление, устанавливаю…", L"Update found, installing…") :
            g_update_status == 4 ? TW(L"Не удалось проверить (сайт недоступен?).", L"Check failed (site unavailable?).") : L"";
        SetWindowTextW(st, txt);
        InvalidateRect(st, NULL, TRUE);
        return 0;
    }
    case WM_APP_LOGINRESULT: {
        g_login_status = (int)wp;   /* 1 успех, 2 ошибка */
        HWND st = GetDlgItem(h, IDC_ST_STATUS);
        SetWindowTextW(st, g_login_status == 1
            ? TW(L"Авторизация успешна.", L"Signed in successfully.")
            : TW(L"Неверный логин или пароль.", L"Wrong login or password."));
        settings_set_page(h, 1);    /* обновить видимость «Выйти» */
        InvalidateRect(st, NULL, TRUE);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: g_set_hwnd = NULL; g_login_status = 0; g_update_status = 0; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void open_settings(void)
{
    if (g_set_hwnd) { SetForegroundWindow(g_set_hwnd); return; }

    if (!g_set_font) {
        g_set_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_set_font_title = CreateFontW(-17, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                       0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_set_font_icon = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                      0, 0, CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
    }
    static int reg = 0;
    if (!reg) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = g_hinst;
        wc.lpszClassName = L"GoodFonSettings";
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hIcon = LoadIconW(g_hinst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.hbrBackground = NULL;
        RegisterClassW(&wc);
        reg = 1;
    }
    theme_brushes_rebuild();

    int W = 584, H = 400;
    RECT wr = { 0, 0, W, H };
    AdjustWindowRect(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    HWND h = CreateWindowExW(0, L"GoodFonSettings",
                             TW(L"GoodFon — Настройки", L"GoodFon — Settings"),
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                             sx, sy, ww, wh, NULL, NULL, g_hinst, NULL);
    if (!h) return;
    g_set_hwnd = h;

    HWND nav = mk(h, L"LISTBOX", NULL,
                  LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_VISIBLE,
                  0, 0, 170, H, IDC_NAV, -1);
    SendMessageW(nav, LB_SETITEMHEIGHT, 0, 38);
    SendMessageW(nav, LB_ADDSTRING, 0, (LPARAM)TW(L"Картинки", L"Images"));
    SendMessageW(nav, LB_ADDSTRING, 0, (LPARAM)TW(L"Аккаунт", L"Account"));
    SendMessageW(nav, LB_ADDSTRING, 0, (LPARAM)TW(L"Дополнительно", L"Additional"));
    SendMessageW(nav, LB_ADDSTRING, 0, (LPARAM)TW(L"О приложении", L"About"));
    SendMessageW(nav, LB_SETCURSEL, 0, 0);

    HWND title = mk(h, L"STATIC", L"", SS_LEFT | WS_VISIBLE, 186, 14, 380, 26, IDC_PGTITLE, -1);
    SendMessageW(title, WM_SETFONT, (WPARAM)g_set_font_title, TRUE);

    const int CX = 186, VX = 306, VW = 250;
    int y;

    /* ---- страница 0: Картинки ---- */
    y = 52;
    mk(h, L"STATIC", TW(L"Тема", L"Theme"), SS_LEFT, CX, y+4, 110, 20, 0, 0);
    HWND cbTheme = mk(h, L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                      VX, y, VW, 240, IDC_CB_THEME, 0);
    { int order[THEME_COUNT];
      for (int i = 0; i < THEME_COUNT; i++) order[i] = i;
      qsort(order, THEME_COUNT, sizeof(int), theme_cmp);
      for (int k = 0; k < THEME_COUNT; k++) {
          int i = order[k];
          WCHAR lbl[96];
          if (!_stricmp(g_themes_all[i].slug, "erotic") && !is_authorized())
              _snwprintf(lbl, 96, L"%s  %s", theme_name(i), TW(L"— нужен вход", L"— sign in required"));
          else { wcsncpy(lbl, theme_name(i), 95); lbl[95] = 0; }
          cb_add(cbTheme, lbl, i);
          if (!_stricmp(g_themes_all[i].slug, g_cfg.theme))
              SendMessageW(cbTheme, CB_SETCURSEL, k, 0);
      } }

    y += 40;
    mk(h, L"STATIC", TW(L"Интервал смены", L"Change interval"), SS_LEFT, CX, y+4, 110, 20, 0, 0);
    HWND cbInt = mk(h, L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, VX, y, VW, 260, IDC_CB_INTERVAL, 0);
    for (int i = 0; i < INTERVAL_COUNT; i++) {
        int v = g_intervals[i]; WCHAR t[32];
        if (v < 60)          _snwprintf(t, 32, TW(L"%d минут", L"%d min"), v);
        else if (v == 1440)  wcscpy(t, TW(L"24 часа (сутки)", L"24 hours (day)"));
        else                 _snwprintf(t, 32, TW(L"%d часа", L"%d hours"), v / 60);
        cb_add(cbInt, t, v);
        if (g_cfg.interval_min == v) SendMessageW(cbInt, CB_SETCURSEL, i, 0);
    }

    y += 40;
    mk(h, L"STATIC", TW(L"Разрешение", L"Resolution"), SS_LEFT, CX, y+4, 110, 20, 0, 0);
    HWND cbRes = mk(h, L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, VX, y, VW, 200, IDC_CB_RES, 0);
    for (int i = 0; i < RES_COUNT; i++) {
        const WCHAR *rn = !strcmp(g_reses[i].value, "original")
                          ? TW(L"Оригинал (любое)", L"Original (any)") : g_reses[i].name;
        cb_add(cbRes, rn, i);
        if (!strcmp(g_cfg.resolution, g_reses[i].value)) SendMessageW(cbRes, CB_SETCURSEL, i, 0);
    }

    y += 40;
    mk(h, L"STATIC", TW(L"Из избранного", L"From favorites"), SS_LEFT, CX, y+4, 110, 20, 0, 0);
    HWND cbFav = mk(h, L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, VX, y, VW, 180, IDC_CB_FAVN, 0);
    cb_add(cbFav, TW(L"Без избранного", L"No favorites"), 0);
    if (g_cfg.favorite_every_n == 0) SendMessageW(cbFav, CB_SETCURSEL, 0, 0);
    for (int i = 0; i < FAVN_COUNT; i++) {
        WCHAR t[48]; _snwprintf(t, 48, TW(L"каждая %d-я", L"every %dth"), g_favorite_ns[i]);
        cb_add(cbFav, t, g_favorite_ns[i]);
        if (g_cfg.favorite_every_n == g_favorite_ns[i]) SendMessageW(cbFav, CB_SETCURSEL, i + 1, 0);
    }

    /* ---- страница 1: Аккаунт ---- */
    /* сверху рисуется блок профиля (в WM_PAINT), поэтому авторизация — ниже */
    WCHAR wlog[128]; utf8_to_wide(g_cfg.login, wlog, 128);
    WCHAR wpw[128];  utf8_to_wide(g_cfg.password, wpw, 128);
    int authed0 = is_authorized();
    const int EH = 20;   /* высота поля (невысокое — EDIT центрирует текст сам) */
    y = 168;
    mk(h, L"STATIC", TW(L"Логин", L"Login"), SS_LEFT, CX, y+4, 110, 20, 0, 1);
    HWND edLog = mk(h, L"EDIT", (g_cfg.login[0] && strcmp(g_cfg.login,"your_login")) ? wlog : L"",
       WS_TABSTOP | ES_AUTOHSCROLL, VX+8, y+4, VW-16, EH, IDC_ED_LOGIN, 1);
    y += 40;
    mk(h, L"STATIC", TW(L"Пароль", L"Password"), SS_LEFT, CX, y+4, 110, 20, 0, 1);
    HWND edPw = mk(h, L"EDIT", authed0 ? wpw : L"",
       WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, VX+8, y+4, VW-16, EH, IDC_ED_PASS, 1);
    (void)edLog; (void)edPw;
    y += 46;
    mk(h, L"BUTTON", TW(L"Войти", L"Sign in"), WS_TABSTOP | BS_OWNERDRAW, VX, y, 120, 28, IDC_BTN_SIGNIN, 1);
    mk(h, L"BUTTON", TW(L"Выйти из аккаунта", L"Sign out"), WS_TABSTOP | BS_OWNERDRAW, VX+130, y, 120, 28, IDC_BTN_SIGNOUT, 1);
    y += 44;
    mk(h, L"STATIC", TW(L"Регистрация на сайте", L"Register on site"), SS_NOTIFY, VX, y, VW, 20, IDC_LNK_REG, 1);
    y += 26;
    mk(h, L"STATIC", L"", SS_LEFT, VX, y, VW, 20, IDC_ST_STATUS, 1);

    /* ---- страница 2: Дополнительно ---- */
    y = 46;
    mk(h, L"BUTTON", TW(L"Включить уведомления", L"Enable notifications"),
       WS_TABSTOP | BS_AUTOCHECKBOX, CX, y, 360, 22, IDC_CHK_NOTIFY, 2);
    y += 28;
    mk(h, L"BUTTON", TW(L"Автозапуск с Windows", L"Start with Windows"),
       WS_TABSTOP | BS_AUTOCHECKBOX, CX, y, 360, 22, IDC_CHK_AUTORUN, 2);
    y += 34;
    mk(h, L"STATIC", TW(L"Домен сайта", L"Site domain"), SS_LEFT, CX, y+4, 110, 20, 0, 2);
    HWND cbDom = mk(h, L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, VX, y, VW, 120, IDC_CB_DOMAIN, 2);
    cb_add(cbDom, TW(L"Авто", L"Auto"), 0);
    cb_add(cbDom, L".ru", 1);
    cb_add(cbDom, L".com", 2);
    SendMessageW(cbDom, CB_SETCURSEL,
                 !strcmp(g_cfg.domain_pref, "ru") ? 1 : !strcmp(g_cfg.domain_pref, "com") ? 2 : 0, 0);
    y += 34;
    mk(h, L"STATIC", TW(L"Язык", L"Language"), SS_LEFT, CX, y+4, 110, 20, 0, 2);
    HWND cbLang = mk(h, L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, VX, y, VW, 120, IDC_CB_LANG, 2);
    cb_add(cbLang, L"Русский", 0); cb_add(cbLang, L"English", 1);
    SendMessageW(cbLang, CB_SETCURSEL, g_lang == LANG_EN ? 1 : 0, 0);
    y += 34;
    mk(h, L"STATIC", TW(L"Оформление", L"Appearance"), SS_LEFT, CX, y+4, 110, 20, 0, 2);
    HWND cbTh = mk(h, L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, VX, y, VW, 120, IDC_CB_APPTHEME, 2);
    cb_add(cbTh, TW(L"Светлая", L"Light"), 0); cb_add(cbTh, TW(L"Тёмная", L"Dark"), 1);
    SendMessageW(cbTh, CB_SETCURSEL, g_ui_theme == THEME_DARK ? 1 : 0, 0);
    y += 34;
    mk(h, L"STATIC", TW(L"Автопроверка", L"Auto-update"), SS_LEFT, CX, y+4, 110, 20, 0, 2);
    HWND cbUpd = mk(h, L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, VX, y, VW, 160, IDC_CB_UPDINT, 2);
    cb_add(cbUpd, TW(L"Выключена", L"Off"), 0);
    cb_add(cbUpd, TW(L"Раз в час", L"Hourly"), 60);
    cb_add(cbUpd, TW(L"Раз в день", L"Daily"), 1440);
    cb_add(cbUpd, TW(L"Раз в неделю", L"Weekly"), 10080);
    { int uv = g_cfg.update_interval_min, si = 0;
      if (uv == 60) si = 1; else if (uv == 1440) si = 2; else if (uv == 10080) si = 3;
      SendMessageW(cbUpd, CB_SETCURSEL, si, 0); }
    y += 36;
    mk(h, L"BUTTON", TW(L"Проверить обновления", L"Check for updates"),
       WS_TABSTOP | BS_OWNERDRAW, VX, y, 200, 28, IDC_BTN_UPDATE, 2);
    mk(h, L"STATIC", L"", SS_LEFT, VX, y + 32, 250, 34, IDC_ST_UPDATE, 2);

    SendMessageW(GetDlgItem(h, IDC_CHK_NOTIFY),  BM_SETCHECK, g_cfg.notify ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(h, IDC_CHK_AUTORUN), BM_SETCHECK, autostart_enabled() ? BST_CHECKED : BST_UNCHECKED, 0);

    /* ---- страница 3: О приложении — текст рисуется в WM_PAINT (перенос по словам) ---- */

    mk(h, L"BUTTON", TW(L"Закрыть", L"Close"), WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
       464, 360, 100, 28, IDC_BTN_CLOSE, -1);

    EnumChildWindows(h, subclass_combos_cb, 0);
    g_login_status = 0;
    g_update_status = 0;
    if (is_authorized()) {
        if (!g_stats.valid) stats_load_reg();   /* показать кэш сразу */
        stats_refresh_async();                  /* обновить с сайта в фоне */
    } else {
        stats_clear();                          /* нули + иконка приложения */
    }
    settings_apply_theme(h);
    settings_set_page(h, 0);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
}

/* ---- Тёмное меню трея (owner-draw). В светлой теме — обычные пункты. ---- */
typedef struct { const WCHAR *text; int checked; int disabled; int sep; } GfMenuItem;
static GfMenuItem g_mi[24];
static int   g_min = 0;
static HFONT g_menu_font = NULL;

static void menu_add(HMENU m, const WCHAR *txt, UINT id, int checked, int disabled)
{
    g_mi[g_min].text = txt; g_mi[g_min].checked = checked;
    g_mi[g_min].disabled = disabled; g_mi[g_min].sep = 0;
    AppendMenuW(m, MF_OWNERDRAW | (disabled ? MF_GRAYED : 0), id, (LPCWSTR)&g_mi[g_min]);
    g_min++;
}
static void menu_sep(HMENU m)
{
    g_mi[g_min].text = NULL; g_mi[g_min].checked = 0;
    g_mi[g_min].disabled = 1; g_mi[g_min].sep = 1;
    AppendMenuW(m, MF_OWNERDRAW | MF_GRAYED, 0xF000 + g_min, (LPCWSTR)&g_mi[g_min]);
    g_min++;
}

static void show_menu(void)
{
    if (!g_menu_font)
        g_menu_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                  0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_min = 0;
    HMENU m = CreatePopupMenu();
    menu_add(m, TW(L"Сменить обои сейчас", L"Change wallpaper now"), IDM_UPDATE, 0, 0);
    menu_add(m, TW(L"Добавить в избранное ♥", L"Add to favorites ♥"), IDM_FAVORITE, 0, 0);
    menu_add(m, TW(L"Убрать из избранного ♡", L"Remove from favorites ♡"), IDM_UNFAVORITE, 0, 0);
    menu_sep(m);
    menu_add(m, TW(L"Пауза", L"Pause"), IDM_PAUSE, g_paused, 0);
    menu_add(m, TW(L"Настройки", L"Settings"), IDM_SETTINGS, 0, 0);
    menu_sep(m);
    menu_add(m, TW(L"Выход", L"Exit"), IDM_EXIT, 0, 0);

    HBRUSH menubg = CreateSolidBrush(cr_bg());
    { MENUINFO mif; ZeroMemory(&mif, sizeof(mif));
      mif.cbSize = sizeof(mif);
      mif.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
      mif.hbrBack = menubg;
      SetMenuInfo(m, &mif); }

    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
    if (menubg) DeleteObject(menubg);
}

static void apply_interval(void)
{
    KillTimer(g_hwnd, TIMER_ID);
    if (!g_paused)
        SetTimer(g_hwnd, TIMER_ID, (UINT)g_cfg.interval_min * 60u * 1000u, NULL);
}

static void apply_update_interval(void)
{
    KillTimer(g_hwnd, UPD_TIMER_ID);
    if (g_cfg.update_interval_min > 0)
        SetTimer(g_hwnd, UPD_TIMER_ID, (UINT)g_cfg.update_interval_min * 60u * 1000u, NULL);
}

static void select_theme(int idx)
{
    if (idx < 0 || idx >= THEME_COUNT) return;
    strncpy(g_cfg.theme, g_themes_all[idx].slug, sizeof(g_cfg.theme) - 1);
    g_cfg.theme[sizeof(g_cfg.theme) - 1] = 0;
    reg_set_str(L"theme", g_cfg.theme);
    /* пересчёт Favorite-папки под новую тему */
    WCHAR wsave[MAX_PATH], wtheme[64];
    utf8_to_wide(g_cfg.save_dir, wsave, MAX_PATH);
    utf8_to_wide(g_cfg.theme, wtheme, 64);
    wcscpy(g_favorite_dir, wsave);
    PathAppendW(g_favorite_dir, L"Favorite");
    PathAppendW(g_favorite_dir, wtheme);
    LOG_INFO(T("Активная тема: %s (сменится по таймеру или вручную)", "Active theme: %s (will change on timer or manually)"), g_cfg.theme);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MEASUREITEM: {
        LPMEASUREITEMSTRUCT mis = (LPMEASUREITEMSTRUCT)lp;
        if (mis->CtlType == ODT_MENU) {
            GfMenuItem *it = (GfMenuItem *)mis->itemData;
            if (it && it->sep) { mis->itemHeight = 8; mis->itemWidth = 10; }
            else if (it) {
                HDC dc = GetDC(h); HFONT of = (HFONT)SelectObject(dc, g_menu_font);
                SIZE sz; GetTextExtentPoint32W(dc, it->text, lstrlenW(it->text), &sz);
                SelectObject(dc, of); ReleaseDC(h, dc);
                mis->itemWidth = sz.cx + 44; mis->itemHeight = 26;
            }
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT d = (LPDRAWITEMSTRUCT)lp;
        if (d->CtlType == ODT_MENU) {
            GfMenuItem *it = (GfMenuItem *)d->itemData;
            COLORREF bg = (d->itemState & ODS_SELECTED) ? cr_sel() : cr_bg();
            HBRUSH bb = CreateSolidBrush(bg); FillRect(d->hDC, &d->rcItem, bb); DeleteObject(bb);
            if (it && it->sep) {
                HPEN pn = CreatePen(PS_SOLID, 1, cr_border());
                HGDIOBJ op = SelectObject(d->hDC, pn);
                int my = (d->rcItem.top + d->rcItem.bottom) / 2;
                MoveToEx(d->hDC, d->rcItem.left + 8, my, NULL);
                LineTo(d->hDC, d->rcItem.right - 8, my);
                SelectObject(d->hDC, op); DeleteObject(pn);
                return TRUE;
            }
            if (it) {
                HFONT of = (HFONT)SelectObject(d->hDC, g_menu_font);
                SetBkMode(d->hDC, TRANSPARENT);
                SetTextColor(d->hDC, it->disabled ? (g_ui_theme ? RGB(140,140,140) : RGB(120,120,120))
                                                  : cr_txt());
                if (it->checked) {
                    RECT ck = d->rcItem; ck.left += 8;
                    DrawTextW(d->hDC, L"\u2713", -1, &ck, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }
                RECT tr = d->rcItem; tr.left += 30;
                DrawTextW(d->hDC, it->text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SelectObject(d->hDC, of);
            }
            return TRUE;
        }
        break;
    }
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
            show_menu();
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK)
            run_async(IDM_UPDATE);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ID && !g_paused) run_async(IDM_UPDATE);
        else if (wp == UPD_TIMER_ID) run_update_async(1);   /* тихая автопроверка */
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDM_UPDATE || id == IDM_FAVORITE || id == IDM_UNFAVORITE)
            run_async(id);
        else if (id == IDM_PAUSE) { g_paused = !g_paused; apply_interval(); }
        else if (id == IDM_SETTINGS) open_settings();
        else if (id == IDM_EXIT) DestroyWindow(h);
        return 0;
    }
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ================= main ================= */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdline, int show)
{
    (void)hPrev; (void)show;
    g_hinst = hInst;
    srand(GetTickCount());

    /* Разбор аргументов: -debug включает логи; update/favorite/unfavorite — разовый режим.
     * Порядок и наличие дефиса не важны: "GoodFon.exe update -debug" тоже ок.   */
    int debug = 0;
    WCHAR cmd[32] = L"";
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        WCHAR a[32]; wcsncpy(a, argv[i], 31); a[31] = 0;
        CharLowerW(a);
        WCHAR *p = a; while (*p == L'-') p++;
        if (!wcscmp(p, L"debug")) debug = 1;
        else if (!wcscmp(p, L"update") || !wcscmp(p, L"favorite") || !wcscmp(p, L"unfavorite"))
            wcsncpy(cmd, p, 31);
    }
    LocalFree(argv);

    log_open(debug);         /* без -debug логи не создаются вообще */
    LOG_INFO(T("GoodFon %s запущен (сборка %s %s)", "GoodFon %s started (build %s %s)"),
             APP_VERSION, __DATE__, __TIME__);
    settings_load();         /* из реестра */
    enable_dark_mode_app(g_ui_theme);   /* тёмный режим приложения по сохранённой теме */

    /* Убираем «хвост» прошлого обновления: <exe>.old больше не занят.
     * Старый процесс мог ещё завершаться — делаем несколько попыток. */
    { WCHAR self[MAX_PATH], old[MAX_PATH];
      GetModuleFileNameW(NULL, self, MAX_PATH);
      _snwprintf(old, MAX_PATH, L"%s.old", self);
      for (int i = 0; i < 10; i++) {
          if (DeleteFileW(old) || GetLastError() == ERROR_FILE_NOT_FOUND) break;
          Sleep(300);
      } }

    if (!http_init()) {
        LOG_ERROR(T("WinHTTP не инициализирован.", "WinHTTP not initialized."));
        return 1;
    }

    /* CLI-режим: update / favorite / unfavorite — разово и выйти */
    if (cmd[0]) {
        if (!wcscmp(cmd, L"update")) do_update();
        else if (!wcscmp(cmd, L"favorite"))   do_favorite();
        else if (!wcscmp(cmd, L"unfavorite")) do_unfavorite();
        return 0;
    }

    /* Трей-режим */
    g_tray_mode = 1;
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"GoodFonTrayWnd";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClassW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, APP_NAME, 0, 0, 0, 0, 0,
                           NULL, NULL, hInst, NULL);
    tray_add();
    apply_interval();
    apply_update_interval();
    /* синхронизация избранного + первая смена — в фоне, чтобы трей появился сразу */
    { HANDLE h = CreateThread(NULL, 0, startup_thread, NULL, 0, NULL); if (h) CloseHandle(h); }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (g_set_hwnd && IsDialogMessageW(g_set_hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
