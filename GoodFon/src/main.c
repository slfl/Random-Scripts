/*
 * GoodFon Wallpaper Changer — C edition (Win32)
 * ------------------------------------------------------------------
 * Резидентное приложение с иконкой в трее:
 *   - таймер смены обоев (интервал настраивается из меню);
 *   - плавная смена обоев через IActiveDesktop (как в Python-версии);
 *   - логин на goodfon (JSON-ответ) + кэш cookie per-domain (com/ru);
 *   - скачивание случайной картинки нужного разрешения по теме;
 *   - избранное: like/unlike (локально + на сайте);
 *   - уведомления в трее (balloon) с подробностями;
 *   - автозапуск (реестр Run) переключается из меню;
 *   - режимы CLI: GoodFon.exe update|like|unlike — разовый запуск без трея.
 *
 * Сборка (MSVC):  cmake -B build && cmake --build build --config Release
 * Линкуется с:    winhttp shell32 ole32 user32 advapi32 shlwapi gdi32
 * Кодировка:      исходник UTF-8; для MSVC включён /utf-8 в CMakeLists.
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <dpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "resource.h"

/* ================= Константы ================= */

#define APP_NAME        L"GoodFon"
#define WM_TRAYICON     (WM_APP + 1)
#define TIMER_ID        1

/* Пункты меню */
#define IDM_UPDATE      100
#define IDM_LIKE        101
#define IDM_UNLIKE      102
#define IDM_PAUSE       103
#define IDM_AUTOSTART   104
#define IDM_EXIT        105
#define IDM_SETCREDS    107
#define IDM_NOTIFY      108
#define IDM_INT_BASE    200   /* интервал смены: 5/10/30/60 мин   */
#define IDM_LIKEN_BASE  220   /* интервал избранного: 5/10/15/20  */
#define IDM_RES_BASE    240   /* разрешение: HD/FullHD/2K/4K/Ориг */
#define IDM_THEME_BASE  300   /* темы из встроенной таблицы       */

#define MAX_THEMES      64
#define JAR_SIZE        4096
#define BODY_LIMIT      (2*1024*1024)   /* максимум HTML в память */
#define IMG_LIMIT       (64*1024*1024)  /* максимум картинки      */

static const int g_intervals[4] = { 5, 10, 30, 60 };
static const int g_like_ns[4]   = { 5, 10, 15, 20 };

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

/* Встроенный список тем: slug для URL + русское имя для меню.
 * anime/auto на goodfon живут на поддоменах — здесь не включены.       */
typedef struct { const char *slug; const wchar_t *name; } ThemeDef;
static const ThemeDef g_themes_all[] = {
    { "erotic",      L"Эротика"      },
    { "girls",       L"Девушки"      },
    { "nature",      L"Природа"      },
    { "landscapes",  L"Пейзажи"      },
    { "hi-tech",     L"Hi-Tech"      },
    { "abstraction", L"Абстракции"   },
    { "aviation",    L"Авиация"      },
    { "city",        L"Город"        },
    { "food",        L"Еда"          },
    { "painting",    L"Живопись"     },
    { "animals",     L"Животные"     },
    { "games",       L"Игры"         },
    { "ai-art",      L"ИИ арт"       },
    { "interior",    L"Интерьер"     },
    { "space",       L"Космос"       },
    { "cats",        L"Кошки"        },
    { "Love",        L"Любовь"       },
    { "macro",       L"Макро"        },
    { "minimalism",  L"Минимализм"   },
    { "men",         L"Мужчины"      },
    { "music",       L"Музыка"       },
    { "mood",        L"Настроения"   },
    { "new-year",    L"Новый год"    },
    { "weapon",      L"Оружие"       },
    { "holidays",    L"Праздники"    },
    { "miscellanea", L"Разное"       },
    { "rendering",   L"Рендеринг"    },
    { "situations",  L"Ситуации"     },
    { "dog",         L"Собаки"       },
    { "sports",      L"Спорт"        },
    { "style",       L"Стиль"        },
    { "textures",    L"Текстуры"     },
    { "fantasy",     L"Фантастика"   },
    { "films",       L"Фильмы"       },
    { "flowers",     L"Цветы"        },
};
#define THEME_COUNT (int)(sizeof(g_themes_all)/sizeof(g_themes_all[0]))

/* Сортировка тем по алфавиту (по отображаемому имени, локале-зависимо).
 * Сортируем массив ИНДЕКСОВ, чтобы ID пунктов меню по-прежнему указывали
 * на исходные записи g_themes_all. */
static int theme_cmp(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    int r = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE,
                           g_themes_all[ia].name, -1, g_themes_all[ib].name, -1);
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
    int  like_every_n;
    int  max_attempts;
    int  notify;
    char domain_pref[8];                /* com / ru / auto */
    int  interval_min;
    int  counter;
} Config;

static Config g_cfg;
static WCHAR  g_config_path[MAX_PATH];
static WCHAR  g_like_dir[MAX_PATH];

/* Cookie-джар per-domain (наш собственный, WinHTTP-cookies отключены) */
static char g_jar_com[JAR_SIZE];
static char g_jar_ru[JAR_SIZE];

/* Активный домен: 0 = com, 1 = ru; -1 = не выбран */
static int g_active_domain = -1;
static const char *g_hosts[2] = { "www.goodfon.com", "www.goodfon.ru" };

/* Трей / состояние */
static HWND  g_hwnd;
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

/* ================= Конфиг: чтение / запись ================= */

static void config_paths_init(void)
{
    GetModuleFileNameW(NULL, g_config_path, MAX_PATH);
    PathRemoveFileSpecW(g_config_path);
    PathAppendW(g_config_path, L"config.ini");
}

static char *read_file_utf8(const WCHAR *path, size_t *outLen)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL), rd = 0;
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    ReadFile(h, buf, sz, &rd, NULL);
    CloseHandle(h);
    buf[rd] = 0;
    /* срезаем BOM, если есть */
    if (rd >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
        memmove(buf, buf + 3, rd - 2);
    if (outLen) *outLen = strlen(buf);
    return buf;
}

/* ============ Хранилище настроек: реестр HKCU\Software\GoodFon ============ */

#define REG_PATH L"Software\\GoodFon"

static int config_get(const char *section, const char *key, char *out, size_t outsz); /* fwd */

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

/* Разовый импорт старого config.ini в реестр (при первом запуске новой версии). */
static int migrate_ini_to_registry(void)
{
    if (GetFileAttributesW(g_config_path) == INVALID_FILE_ATTRIBUTES) return 0;
    char v[JAR_SIZE];
    if (!config_get("settings", "theme", v, sizeof(v)) || !v[0]) return 0;

    reg_set_str(L"theme", v);
    if (config_get("auth", "login", v, sizeof(v)) && strcmp(v, "your_login"))
        reg_set_str(L"login", v);
    if (config_get("auth", "password", v, sizeof(v)) && v[0] && strcmp(v, "your_password"))
        reg_set_password(v);
    if (config_get("auth", "session_com", v, sizeof(v))) reg_set_str(L"session_com", v);
    if (config_get("auth", "session_ru", v, sizeof(v)))  reg_set_str(L"session_ru", v);
    if (config_get("settings", "resolution", v, sizeof(v))) reg_set_str(L"resolution", v);
    if (config_get("settings", "save_dir", v, sizeof(v)))   reg_set_str(L"save_dir", v);
    if (config_get("settings", "domain", v, sizeof(v)))     reg_set_str(L"domain", v);
    if (config_get("settings", "max_files", v, sizeof(v)))    reg_set_dword(L"max_files", atoi(v));
    if (config_get("settings", "like_every_n", v, sizeof(v))) reg_set_dword(L"like_every_n", atoi(v));
    if (config_get("settings", "max_attempts", v, sizeof(v))) reg_set_dword(L"max_attempts", atoi(v));
    if (config_get("settings", "notify", v, sizeof(v)))       reg_set_dword(L"notify", !_stricmp(v, "true"));
    if (config_get("settings", "interval_min", v, sizeof(v))) reg_set_dword(L"interval_min", atoi(v));
    if (config_get("state", "counter", v, sizeof(v)))         reg_set_dword(L"counter", atoi(v));
    LOG_INFO("Настройки перенесены из config.ini в реестр HKCU\\Software\\GoodFon");
    return 1;
}

/* Записать значения по умолчанию (когда реестр пуст и мигрировать нечего). */
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
    reg_set_dword(L"like_every_n", 10);
    reg_set_dword(L"max_attempts", 3);
    reg_set_dword(L"notify", 1);
    reg_set_dword(L"interval_min", 10);
    reg_set_dword(L"counter", 0);
    /* password_enc не пишем — пароль появится, когда его введут через меню */
    LOG_INFO("Реестр пуст — созданы значения по умолчанию в HKCU\\Software\\GoodFon");
}

static int settings_load(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));

    /* Реестр пуст? Сначала пробуем импорт config.ini, иначе пишем дефолты. */
    char probe[64];
    if (!reg_get_str(L"theme", probe, sizeof(probe)) || !probe[0]) {
        if (!migrate_ini_to_registry())
            settings_write_defaults();
    }

    reg_get_str(L"login", g_cfg.login, sizeof(g_cfg.login));
    reg_get_password(g_cfg.password, sizeof(g_cfg.password));
    reg_get_str(L"session_com", g_cfg.session_com, JAR_SIZE);
    reg_get_str(L"session_ru", g_cfg.session_ru, JAR_SIZE);

    if (!reg_get_str(L"resolution", g_cfg.resolution, sizeof(g_cfg.resolution)) || !g_cfg.resolution[0])
        strcpy(g_cfg.resolution, "1920x1080");
    if (!reg_get_str(L"theme", g_cfg.theme, sizeof(g_cfg.theme)) || !g_cfg.theme[0])
        strcpy(g_cfg.theme, "nature");
    reg_get_str(L"save_dir", g_cfg.save_dir, sizeof(g_cfg.save_dir));
    if (!reg_get_str(L"domain", g_cfg.domain_pref, sizeof(g_cfg.domain_pref)) || !g_cfg.domain_pref[0])
        strcpy(g_cfg.domain_pref, "auto");

    g_cfg.max_files    = reg_get_dword(L"max_files", 10);
    g_cfg.like_every_n = reg_get_dword(L"like_every_n", 10);
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
    wcscpy(g_like_dir, wsave);
    PathAppendW(g_like_dir, L"Like");
    PathAppendW(g_like_dir, wtheme);

    strncpy(g_jar_com, g_cfg.session_com, JAR_SIZE - 1);
    strncpy(g_jar_ru,  g_cfg.session_ru,  JAR_SIZE - 1);

    {
        char l8[MAX_PATH * 3]; wide_to_utf8(g_like_dir, l8, sizeof(l8));
        LOG_INFO("Настройки загружены из реестра. Тема: %s | save_dir: %s",
                 g_cfg.theme, g_cfg.save_dir);
        LOG_INFO("Папка избранного: %s", l8);
    }
    return 1;
}

static void counter_save(void)
{
    reg_set_dword(L"counter", g_cfg.counter);
}

/* Прочитать одно значение key из секции section прямо из файла.
 * Нужно, чтобы диалог логина показывал актуальные данные, даже если
 * их поменяли в config.ini вручную во время работы приложения.      */
static int config_get(const char *section, const char *key, char *out, size_t outsz)
{
    out[0] = 0;
    size_t len;
    char *text = read_file_utf8(g_config_path, &len);
    if (!text) return 0;

    char cur[32] = "";
    int found = 0;
    char *ctx = NULL;
    char *line = strtok_s(text, "\n", &ctx);
    while (line) {
        char buf[JAR_SIZE + 64];
        strncpy(buf, line, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        str_trim(buf);
        if (buf[0] == '[') {
            char *e = strchr(buf, ']');
            if (e) { *e = 0; strncpy(cur, buf + 1, sizeof(cur) - 1); CharLowerA(cur); }
        } else if (buf[0] && buf[0] != ';' && buf[0] != '#' && !_stricmp(cur, section)) {
            char *eq = strchr(buf, '=');
            if (eq) {
                *eq = 0;
                char k[64]; strncpy(k, buf, sizeof(k) - 1); k[sizeof(k) - 1] = 0;
                str_trim(k); CharLowerA(k);
                if (!strcmp(k, key)) {
                    char *v = eq + 1; str_trim(v);
                    strncpy(out, v, outsz - 1); out[outsz - 1] = 0;
                    found = 1; break;
                }
            }
        }
        line = strtok_s(NULL, "\n", &ctx);
    }
    free(text);
    return found;
}

/* Есть ли у нас авторизация: живой кэш сессии или заданные логин+пароль.
 * Используется для гейтинга раздела "Эротика" (доступен только после входа). */
static int is_authorized(void)
{
    if (g_jar_com[0] || g_jar_ru[0]) return 1;
    if (g_cfg.login[0] && strcmp(g_cfg.login, "your_login") &&
        g_cfg.password[0] && strcmp(g_cfg.password, "your_password"))
        return 1;
    return 0;
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
    LOG_INFO("Кэш сессии сохранён для домена %s", g_hosts[domain]);
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
            LOG_WARN("HTTP %d %s -> статус %lu, повтор через 2с",
                     attempt + 1, url, (unsigned long)status);
            free(out->body); out->body = NULL; out->len = 0;
            Sleep(2000);
            continue;
        }
        LOG_INFO("HTTP %s %s -> %lu (%lu байт)",
                 method, url, (unsigned long)status, (unsigned long)out->len);
        ok = 1;
        break;
    }
    if (!ok)
        LOG_WARN("HTTP %s %s -> сетевая ошибка %lu",
                 method, url, GetLastError());
    WinHttpCloseHandle(hr);
    WinHttpCloseHandle(hc);
    return ok;
}

/* ================= Обои: IActiveDesktop (плавная смена) ================= */
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
        LOG_ERROR("set_wallpaper: файл не существует: %s", p8);
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
        LOG_WARN("IActiveDesktop недоступен: CoCreateInstance=0x%08lX",
                 (unsigned long)hr);
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();

    if (!ok) {
        /* запасной путь без плавности */
        if (SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)path,
                                  SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
            LOG_INFO("Fallback SPI_SETDESKWALLPAPER: успех");
            ok = 1;
        } else {
            LOG_ERROR("SPI_SETDESKWALLPAPER: ошибка %lu", GetLastError());
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
            LOG_WARN("IActiveDesktop применил не наш файл (сейчас: %s) — форсим SPI", c8);
            if (SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)path,
                                      SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
                LOG_INFO("SPI_SETDESKWALLPAPER: успех");
            else {
                LOG_ERROR("SPI_SETDESKWALLPAPER: ошибка %lu", GetLastError());
                ok = 0;
            }
        }
    }

    if (ok) LOG_INFO("Обои выставлены: %s", p8);
    else    LOG_ERROR("Обои НЕ выставлены: %s", p8);
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
        LOG_INFO("dir_scan: папка недоступна (код %lu): %s", GetLastError(), d8);
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
                LOG_INFO("Удалён старый файл: %s", p8);
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

static WCHAR *last_file_in(const WCHAR *dir, WCHAR *out, int outsz)
{
    int n = 0;
    FileEnt *files = dir_scan(dir, &n);
    if (!files || n == 0) { free(files); return NULL; }
    qsort(files, n, sizeof(FileEnt), cmp_mtime);
    wcsncpy(out, files[n - 1].path, outsz - 1); out[outsz - 1] = 0;
    free(files);
    return out;
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
        LOG_INFO("Авторизация успешна");
        jar_save(g_active_domain);
        return 1;
    }
    LOG_WARN("Неожиданный ответ сервера при входе (статус %d)", p.status);
    return 0;
}

/* Выбор домена + сессии: кэш напрямую, иначе логин с повтором. 1 = ок */
static int ensure_session(void)
{
    int order[2]; domain_order(order);
    for (int i = 0; i < 2; i++) {
        g_active_domain = order[i];
        if (jar_for(g_active_domain)[0]) {
            LOG_INFO("Используется кэш сессии: %s", g_hosts[g_active_domain]);
            return 1;
        }
        for (int att = 0; att < 2; att++) {
            int res = do_login();
            if (res == 1) return 1;
            if (res == -1) {
                LOG_WARN("Не удалось войти на %s: неверный логин или пароль",
                         g_hosts[g_active_domain]);
                break;
            }
            LOG_WARN("Сетевая ошибка входа на %s (попытка %d)",
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
            LOG_INFO("Тир %s: выбрано %dx%d", g_cfg.resolution, cw, ch);
    }
    free(r.body);
    if (!dl_href[0]) {
        LOG_INFO("Разрешение %s недоступно для этой картинки, пропускаем.", g_cfg.resolution);
        return 0;
    }

    char dl_url[600];
    make_absolute(dl_href, dl_url, sizeof(dl_url));
    HttpResp d;
    if (!http_request("GET", dl_url, NULL, NULL, 15000, BODY_LIMIT, &d) ||
        d.status != 200 || !d.body) { free(d.body); return 0; }

    if (strstr(d.body, QUOTA_MARKER_RU) || strstr(d.body, "download_limit")) {
        free(d.body);
        LOG_WARN("Превышен суточный лимит скачиваний на сайте.");
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
        LOG_WARN("Ссылка на картинку не найдена на странице загрузки.");
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
            if (i == 1) LOG_INFO("Использован резервный img домен");
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
    LOG_INFO("Файл сохранён: %s", p8);
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
    if (!api[0]) { LOG_WARN("Блок избранного не найден на странице."); return 0; }

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

static void fallback_local(int like_only)
{
    WCHAR cur[MAX_PATH]; get_current_wallpaper(cur, MAX_PATH);
    WCHAR chosen[MAX_PATH];
    int got = random_file_excluding(g_like_dir, cur, chosen, MAX_PATH);
    if (!got && !like_only) {
        WCHAR wdir[MAX_PATH]; utf8_to_wide(g_cfg.save_dir, wdir, MAX_PATH);
        got = random_file_excluding(wdir, cur, chosen, MAX_PATH);
    }
    if (!got) {
        LOG_WARN("Fallback: локальных картинок нет, обои не изменены.");
        notify_user(L"GoodFon: ошибка", L"Нет доступных картинок.");
        return;
    }
    char p8[MAX_PATH * 3]; wide_to_utf8(chosen, p8, sizeof(p8));
    LOG_INFO("Fallback: устанавливаем локальную картинку: %s", p8);
    set_wallpaper(chosen);
    WCHAR info[300];
    _snwprintf(info, 300, L"%s", PathFindFileNameW(chosen));
    notify_user(like_only ? L"Обои обновлены — из избранного"
                          : L"Обои обновлены — локально", info);
}

static int set_wallpaper_from_like(void)
{
    char d8[MAX_PATH * 3]; wide_to_utf8(g_like_dir, d8, sizeof(d8));
    LOG_INFO("Like-папка: %s (найдено файлов: %d)", d8, dir_count(g_like_dir));

    WCHAR cur[MAX_PATH]; get_current_wallpaper(cur, MAX_PATH);
    WCHAR chosen[MAX_PATH];
    if (!random_file_excluding(g_like_dir, cur, chosen, MAX_PATH)) {
        LOG_WARN("Папка Like/%s пуста, загружаем с сайта", g_cfg.theme);
        return 0;
    }
    set_wallpaper(chosen);
    char p8[MAX_PATH * 3]; wide_to_utf8(chosen, p8, sizeof(p8));
    LOG_INFO("Обои из папки Like/%s: %s", g_cfg.theme, p8);
    WCHAR info[300]; _snwprintf(info, 300, L"%s", PathFindFileNameW(chosen));
    notify_user(L"Обои обновлены — из избранного", info);
    return 1;
}

static void do_update(void)
{
    if (!ensure_session()) {
        LOG_ERROR("Ни один домен недоступен или вход не выполнен.");
        g_cfg.counter++; counter_save();
        fallback_local(0);
        return;
    }

    g_cfg.counter++;
    counter_save();
    LOG_INFO("Запуск #%d (из Like каждые %d)", g_cfg.counter, g_cfg.like_every_n);
    if (g_cfg.counter >= g_cfg.like_every_n) {
        g_cfg.counter = 0; counter_save();
        if (set_wallpaper_from_like()) return;
        LOG_INFO("Папка Like пуста, продолжаем загрузку с сайта");
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
                LOG_WARN("Ошибка пагинации (сбой %d)", net_fails);
                Sleep(1000);
                continue;
            }
            LOG_INFO("Максимальное количество страниц: %d", max_pages);
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
                LOG_WARN("Слишком много сетевых ошибок (последний статус %d)", st);
                break;
            }
            LOG_WARN("Страница раздела не загрузилась (статус %d)", st);
            continue;
        }
        static char links[64][512];
        int n = collect_links(r.body, links, 64);
        free(r.body);
        if (n == 0) { LOG_WARN("На странице нет обоев, пробуем другую"); continue; }

        /* пробуем несколько картинок с этой страницы (без дублей) */
        int per_page = n < 6 ? n : 6;
        int start = rand() % n;
        for (int k = 0; k < per_page && images_tried < IMG_BUDGET; k++) {
            char image_page[600];
            make_absolute(links[(start + k) % n], image_page, sizeof(image_page));
            images_tried++;
            LOG_INFO("Проверка #%d: %s", images_tried, image_page);

            char img_url[600];
            int fr = find_image_url(image_page, img_url, sizeof(img_url));
            if (fr == -1) {
                notify_user(L"GoodFon: лимит исчерпан", L"Загружаем из избранного.");
                fallback_local(1);
                return;
            }
            if (fr == 0) continue;   /* разрешение не подошло — следующая */

            size_t len = 0;
            char *data = download_image(img_url, &len);
            if (!data) { LOG_WARN("Не удалось скачать картинку, пробуем другую"); continue; }

            WCHAR saved[MAX_PATH];
            int sok = save_image(img_url, data, len, saved, MAX_PATH);
            free(data);
            if (!sok) continue;

            cleanup_old_images();
            set_wallpaper(saved);
            LOG_INFO("Найдено за %d проверок.", images_tried);
            WCHAR info[300]; _snwprintf(info, 300, L"%s", PathFindFileNameW(saved));
            notify_user(L"Обои обновлены — с сайта", info);
            return;
        }
    }
    LOG_ERROR("Не найдено изображение с разрешением %s (проверено %d).",
              g_cfg.resolution, images_tried);
    fallback_local(0);
}

static void do_like(void)
{
    if (!ensure_session()) {
        notify_user(L"GoodFon: сайт недоступен", L"Операция с избранным невозможна.");
        return;
    }
    WCHAR wdir[MAX_PATH]; utf8_to_wide(g_cfg.save_dir, wdir, MAX_PATH);
    WCHAR last[MAX_PATH];
    if (!last_file_in(wdir, last, MAX_PATH)) {
        LOG_ERROR("Нет последнего скачанного файла.");
        return;
    }
    SHCreateDirectoryExW(NULL, g_like_dir, NULL);
    WCHAR dest[MAX_PATH];
    _snwprintf(dest, MAX_PATH, L"%s\\%s", g_like_dir, PathFindFileNameW(last));
    if (GetFileAttributesW(dest) == INVALID_FILE_ATTRIBUTES) {
        CopyFileW(last, dest, FALSE);
        LOG_INFO("Изображение скопировано в папку Like/%s", g_cfg.theme);
    }
    set_wallpaper(dest); /* чтобы unlike корректно определил текущую */

    char page_url[600];
    page_url_for_file(dest, page_url, sizeof(page_url));
    if (favorite_api(page_url, 1)) {
        LOG_INFO("Изображение добавлено в избранное на сайте");
        WCHAR info[300]; _snwprintf(info, 300, L"%s", PathFindFileNameW(dest));
        notify_user(L"Добавлено в избранное", info);
    } else
        LOG_WARN("Не удалось добавить в избранное на сайте.");
}

static void do_unlike(void)
{
    if (!ensure_session()) {
        notify_user(L"GoodFon: сайт недоступен", L"Операция с избранным невозможна.");
        return;
    }
    WCHAR cur[MAX_PATH]; get_current_wallpaper(cur, MAX_PATH);
    if (!cur[0] || StrStrIW(cur, g_like_dir) != cur) {
        LOG_ERROR("Текущие обои не из папки Like — unlike невозможен.");
        notify_user(L"GoodFon: ошибка", L"Текущие обои не из папки избранного.");
        return;
    }
    char page_url[600];
    page_url_for_file(cur, page_url, sizeof(page_url));
    if (favorite_api(page_url, 0))
        LOG_INFO("Изображение удалено из избранного на сайте");
    else
        LOG_WARN("Не удалось удалить из избранного на сайте.");
    if (DeleteFileW(cur)) {
        LOG_INFO("Файл удалён из папки Like");
        WCHAR info[300]; _snwprintf(info, 300, L"%s", PathFindFileNameW(cur));
        notify_user(L"Удалено из избранного", info);
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
    if (!login[0]) { LOG_INFO("Синк избранного пропущен: не задан логин."); return; }
    if (!ensure_session()) { LOG_INFO("Синк избранного отменён: нет входа."); return; }

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
            LOG_WARN("Синк: чтение пагинации неполное (%d байт), повтор.", r.status ? (int)strlen(r.body) : 0);
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
        LOG_WARN("Синк избранного ОТМЕНЁН: не удалось надёжно прочитать пагинацию — ничего не удаляем.");
        return;
    }
    if (M > 1000) M = 1000;   /* защита от абсурда */
    LOG_INFO("Синк избранного: страниц на сайте %d", M);

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
            LOG_WARN("Синк избранного ОТМЕНЁН: страница %d прочитана неполно (собрано %d) — ничего не удаляем.",
                     pg, page.n);
            set_free(&page); set_free(&site); return;
        }
        for (int i = 0; i < page.n; i++) set_add(&site, page.a[i]);
        LOG_INFO("Синк: страница %d прочитана (на ней %d, всего в базе %d)", pg, page.n, site.n);
        set_free(&page);
    }

    if (site.n == 0) {
        LOG_WARN("Синк избранного отменён: список с сайта пуст.");
        set_free(&site); return;
    }

    /* Второй рубеж: на M страницах должно быть минимум 24*(M-1)+1 картинок
     * (каждая НЕпоследняя страница = ровно 24). Собрали меньше -> где-то
     * недочитали, НЕ удаляем ничего. */
    if (site.n < 24 * (M - 1) + 1) {
        LOG_WARN("Синк избранного ОТМЕНЁН: собрано %d, ожидалось >= %d (страниц %d) — ничего не удаляем.",
                 site.n, 24 * (M - 1) + 1, M);
        set_free(&site); return;
    }

    /* Удаление локальных картинок, которых нет в избранном на сайте.
     * Чистим корень Like и все подпапки Like\<тема> (сайт плоский). */
    WCHAR wsave[MAX_PATH], like_base[MAX_PATH];
    utf8_to_wide(g_cfg.save_dir, wsave, MAX_PATH);
    wcscpy(like_base, wsave); PathAppendW(like_base, L"Like");

    int deleted = 0, kept = 0;

    /* список директорий для чистки: сам Like + его подпапки */
    WCHAR dirs[128][MAX_PATH]; int nd = 0;
    wcscpy(dirs[nd++], like_base);
    WCHAR pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%s\\*", like_base);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (nd < 128) _snwprintf(dirs[nd++], MAX_PATH, L"%s\\%s", like_base, fd.cFileName);
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
                LOG_INFO("Удалено (нет в избранном на сайте): %s", f8);
            }
        }
        free(files);
    }

    LOG_INFO("Синк избранного завершён: на сайте %d, оставлено %d, удалено %d.",
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
        case IDM_LIKE:   do_like();   break;
        case IDM_UNLIKE: do_unlike(); break;
    }
    InterlockedExchange(&g_busy, 0);
    return 0;
}

static void run_async(int action)
{
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
        LOG_WARN("Предыдущая операция ещё выполняется, пропуск.");
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

/* ================= Диалог ввода логина/пароля ================= */

static WCHAR g_dlg_login[128];
static WCHAR g_dlg_pass[128];
static int   g_dlg_result;   /* -1 идёт, 0 отмена, 1 сохранено */

static LRESULT CALLBACK CredProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    static HWND eLogin, ePass;
    switch (msg) {
    case WM_CREATE:
        CreateWindowW(L"STATIC", L"Логин:", WS_CHILD | WS_VISIBLE,
                      12, 14, 70, 20, h, NULL, g_hinst, NULL);
        eLogin = CreateWindowW(L"EDIT", g_dlg_login,
                      WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                      90, 12, 190, 24, h, (HMENU)1001, g_hinst, NULL);
        CreateWindowW(L"STATIC", L"Пароль:", WS_CHILD | WS_VISIBLE,
                      12, 48, 70, 20, h, NULL, g_hinst, NULL);
        ePass = CreateWindowW(L"EDIT", g_dlg_pass,
                      WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD,
                      90, 46, 190, 24, h, (HMENU)1002, g_hinst, NULL);
        CreateWindowW(L"BUTTON", L"Сохранить",
                      WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                      90, 84, 100, 28, h, (HMENU)IDOK, g_hinst, NULL);
        CreateWindowW(L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE,
                      195, 84, 85, 28, h, (HMENU)IDCANCEL, g_hinst, NULL);
        SetFocus(eLogin);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            GetWindowTextW(eLogin, g_dlg_login, 128);
            GetWindowTextW(ePass, g_dlg_pass, 128);
            g_dlg_result = 1;
            DestroyWindow(h);
        } else if (LOWORD(wp) == IDCANCEL) {
            g_dlg_result = 0;
            DestroyWindow(h);
        }
        return 0;
    case WM_CLOSE:
        g_dlg_result = 0;
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void prompt_credentials(void)
{
    static int registered = 0;
    const WCHAR *cls = L"GoodFonCredWnd";
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = CredProc;
        wc.hInstance = g_hinst;
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = LoadIconW(g_hinst, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassW(&wc);
        registered = 1;
    }

    /* префилл АКТУАЛЬНЫМИ значениями из реестра (пароль расшифровывается DPAPI) */
    char lf[128] = "", pf[128] = "";
    reg_get_str(L"login", lf, sizeof(lf));
    reg_get_password(pf, sizeof(pf));
    strncpy(g_cfg.login, lf, sizeof(g_cfg.login) - 1);
    strncpy(g_cfg.password, pf, sizeof(g_cfg.password) - 1);
    utf8_to_wide(lf, g_dlg_login, 128);
    utf8_to_wide(pf, g_dlg_pass, 128);
    g_dlg_result = -1;

    int w = 300, ht = 155;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - ht) / 2;

    HWND dh = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, cls,
                              L"GoodFon — логин и пароль",
                              WS_POPUP | WS_CAPTION | WS_SYSMENU,
                              x, y, w, ht, g_hwnd, NULL, g_hinst, NULL);
    if (!dh) return;
    ShowWindow(dh, SW_SHOW);
    SetForegroundWindow(dh);

    /* локальный модальный цикл (без PostQuitMessage, чтобы не убить главный) */
    MSG m;
    while (IsWindow(dh) && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dh, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (g_dlg_result == 1) {
        char l8[256], p8[256];
        wide_to_utf8(g_dlg_login, l8, sizeof(l8));
        wide_to_utf8(g_dlg_pass, p8, sizeof(p8));
        strncpy(g_cfg.login, l8, sizeof(g_cfg.login) - 1);
        strncpy(g_cfg.password, p8, sizeof(g_cfg.password) - 1);
        reg_set_str(L"login", g_cfg.login);
        reg_set_password(g_cfg.password);
        /* смена логина делает старый кэш недействительным — чистим */
        g_jar_com[0] = 0; g_jar_ru[0] = 0;
        reg_set_str(L"session_com", "");
        reg_set_str(L"session_ru", "");
        LOG_INFO("Логин/пароль обновлены через меню, кэш сессий сброшен.");
        notify_user(L"GoodFon", L"Логин и пароль сохранены.");
    }
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
    wcscpy(g_nid.szTip, L"GoodFon — смена обоев");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void show_menu(void)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_UPDATE, L"Сменить обои сейчас");
    AppendMenuW(m, MF_STRING, IDM_LIKE,   L"Добавить в избранное ♥");
    AppendMenuW(m, MF_STRING, IDM_UNLIKE, L"Убрать из избранного ♡");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    HMENU mi = CreatePopupMenu();
    for (int i = 0; i < 4; i++) {
        WCHAR t[32]; _snwprintf(t, 32, L"%d минут", g_intervals[i]);
        UINT fl = MF_STRING | (g_cfg.interval_min == g_intervals[i] ? MF_CHECKED : 0);
        AppendMenuW(mi, fl, IDM_INT_BASE + i, t);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mi, L"Интервал смены");

    HMENU ml = CreatePopupMenu();
    for (int i = 0; i < 4; i++) {
        WCHAR t[48]; _snwprintf(t, 48, L"каждая %d-я", g_like_ns[i]);
        UINT fl = MF_STRING | (g_cfg.like_every_n == g_like_ns[i] ? MF_CHECKED : 0);
        AppendMenuW(ml, fl, IDM_LIKEN_BASE + i, t);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)ml, L"Из избранного");

    HMENU mr = CreatePopupMenu();
    for (int i = 0; i < RES_COUNT; i++) {
        UINT fl = MF_STRING | (!strcmp(g_cfg.resolution, g_reses[i].value) ? MF_CHECKED : 0);
        AppendMenuW(mr, fl, IDM_RES_BASE + i, g_reses[i].name);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mr, L"Разрешение");

    HMENU mt = CreatePopupMenu();
    int authed = is_authorized();
    int order[THEME_COUNT];
    for (int i = 0; i < THEME_COUNT; i++) order[i] = i;
    qsort(order, THEME_COUNT, sizeof(int), theme_cmp);
    for (int k = 0; k < THEME_COUNT; k++) {
        int i = order[k];
        UINT fl = MF_STRING |
            (!_stricmp(g_themes_all[i].slug, g_cfg.theme) ? MF_CHECKED : 0);
        /* "Эротика" доступна только после авторизации */
        if (!_stricmp(g_themes_all[i].slug, "erotic") && !authed)
            fl |= MF_GRAYED;
        AppendMenuW(mt, fl, IDM_THEME_BASE + i, g_themes_all[i].name);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mt, L"Тема");

    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_SETCREDS, L"Изменить логин и пароль…");
    AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), IDM_PAUSE, L"Пауза");
    AppendMenuW(m, MF_STRING | (g_cfg.notify ? MF_CHECKED : 0),
                IDM_NOTIFY, L"Включить уведомления");
    AppendMenuW(m, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"Автозапуск с Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, L"By Mansi / slfl@mail.ru");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Выход");

    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
}

static void apply_interval(void)
{
    KillTimer(g_hwnd, TIMER_ID);
    if (!g_paused)
        SetTimer(g_hwnd, TIMER_ID, (UINT)g_cfg.interval_min * 60u * 1000u, NULL);
}

static void select_theme(int idx)
{
    if (idx < 0 || idx >= THEME_COUNT) return;
    strncpy(g_cfg.theme, g_themes_all[idx].slug, sizeof(g_cfg.theme) - 1);
    g_cfg.theme[sizeof(g_cfg.theme) - 1] = 0;
    reg_set_str(L"theme", g_cfg.theme);
    /* пересчёт Like-папки под новую тему */
    WCHAR wsave[MAX_PATH], wtheme[64];
    utf8_to_wide(g_cfg.save_dir, wsave, MAX_PATH);
    utf8_to_wide(g_cfg.theme, wtheme, 64);
    wcscpy(g_like_dir, wsave);
    PathAppendW(g_like_dir, L"Like");
    PathAppendW(g_like_dir, wtheme);
    LOG_INFO("Активная тема: %s (сменится по таймеру или вручную)", g_cfg.theme);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
            show_menu();
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK)
            run_async(IDM_UPDATE);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ID && !g_paused) run_async(IDM_UPDATE);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDM_UPDATE || id == IDM_LIKE || id == IDM_UNLIKE)
            run_async(id);
        else if (id == IDM_SETCREDS) prompt_credentials();
        else if (id == IDM_PAUSE) { g_paused = !g_paused; apply_interval(); }
        else if (id == IDM_NOTIFY) {
            g_cfg.notify = !g_cfg.notify;
            reg_set_dword(L"notify", g_cfg.notify);
            LOG_INFO("Уведомления: %s", g_cfg.notify ? "вкл" : "выкл");
        }
        else if (id == IDM_AUTOSTART) autostart_toggle();
        else if (id == IDM_EXIT) DestroyWindow(h);
        else if (id >= IDM_INT_BASE && id < IDM_INT_BASE + 4) {
            g_cfg.interval_min = g_intervals[id - IDM_INT_BASE];
            reg_set_dword(L"interval_min", g_cfg.interval_min);
            apply_interval();
            LOG_INFO("Интервал смены: %d мин", g_cfg.interval_min);
        }
        else if (id >= IDM_LIKEN_BASE && id < IDM_LIKEN_BASE + 4) {
            g_cfg.like_every_n = g_like_ns[id - IDM_LIKEN_BASE];
            reg_set_dword(L"like_every_n", g_cfg.like_every_n);
            LOG_INFO("Из избранного: каждая %d-я картинка", g_cfg.like_every_n);
        }
        else if (id >= IDM_RES_BASE && id < IDM_RES_BASE + RES_COUNT) {
            strncpy(g_cfg.resolution, g_reses[id - IDM_RES_BASE].value,
                    sizeof(g_cfg.resolution) - 1);
            reg_set_str(L"resolution", g_cfg.resolution);
            LOG_INFO("Разрешение: %s (применится при следующей смене)", g_cfg.resolution);
        }
        else if (id >= IDM_THEME_BASE && id < IDM_THEME_BASE + THEME_COUNT) {
            int idx = id - IDM_THEME_BASE;
            if (!_stricmp(g_themes_all[idx].slug, "erotic") && !is_authorized()) {
                LOG_WARN("Раздел \"Эротика\" доступен только после авторизации.");
                notify_user(L"GoodFon", L"Раздел «Эротика» доступен только после входа.");
            } else
                select_theme(idx);
        }
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

    /* Разбор аргументов: -debug включает логи; update/like/unlike — разовый режим.
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
        else if (!wcscmp(p, L"update") || !wcscmp(p, L"like") || !wcscmp(p, L"unlike"))
            wcsncpy(cmd, p, 31);
    }
    LocalFree(argv);

    log_open(debug);         /* без -debug логи не создаются вообще */
    config_paths_init();     /* путь к config.ini нужен только для разовой миграции */
    settings_load();         /* из реестра (с импортом старого config.ini при первом запуске) */
    if (!http_init()) {
        LOG_ERROR("WinHTTP не инициализирован.");
        return 1;
    }

    /* CLI-режим: update / like / unlike — разово и выйти */
    if (cmd[0]) {
        if (!wcscmp(cmd, L"update")) do_update();
        else if (!wcscmp(cmd, L"like"))   do_like();
        else if (!wcscmp(cmd, L"unlike")) do_unlike();
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
    /* синхронизация избранного + первая смена — в фоне, чтобы трей появился сразу */
    { HANDLE h = CreateThread(NULL, 0, startup_thread, NULL, 0, NULL); if (h) CloseHandle(h); }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
