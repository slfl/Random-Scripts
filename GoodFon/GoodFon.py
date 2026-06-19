import requests
from bs4 import BeautifulSoup
import random
import os
import ctypes
import logging
from urllib.parse import urljoin, urlparse
import glob
import sys
import shutil
import re
import configparser
import winreg
import pythoncom
import pywintypes
import win32gui
from win32com.shell import shell, shellcon
from typing import List, Optional

try:
    from winotify import Notification
    _winotify_available = True
except ImportError:
    _winotify_available = False

# ====== Расположение config.ini ======
# Под PyInstaller __file__ указывает на временную папку распаковки (_MEIxxxx),
# поэтому путь берётся от каталога с .exe, а не от __file__.
if getattr(sys, "frozen", False):
    BASE_DIR = os.path.dirname(sys.executable)
else:
    BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_FILE = os.path.join(BASE_DIR, "config.ini")

config = configparser.ConfigParser(interpolation=None)


def load_config():
    if not os.path.exists(CONFIG_FILE):
        raise FileNotFoundError(f"Файл конфигурации не найден: {CONFIG_FILE}")
    config.read(CONFIG_FILE, encoding="utf-8")


def get_config_str(section: str, key: str) -> str:
    return config[section][key].strip()


def get_config_int(section: str, key: str, fallback: int = 0) -> int:
    return config[section].getint(key, fallback=fallback)


# ====== Настройки (из config.ini) ======
load_config()

LOGIN        = get_config_str("auth", "login")
PASSWORD     = get_config_str("auth", "password")
RESOLUTION   = get_config_str("settings", "resolution")
THEME        = get_config_str("settings", "theme")
SAVE_DIR     = get_config_str("settings", "save_dir")
MAX_FILES    = get_config_int("settings", "max_files", fallback=10)
LIKE_EVERY_N = get_config_int("settings", "like_every_n", fallback=10)
MAX_ATTEMPTS = get_config_int("settings", "max_attempts", fallback=3)
NOTIFY       = config["settings"].getboolean("notify", fallback=True)
DOMAIN_PREF  = config["settings"].get("domain", "auto").strip().lower()
SESSION_COM  = config["auth"].get("session_com", "").strip()
SESSION_RU   = config["auth"].get("session_ru", "").strip()

LIKE_DIR  = os.path.join(SAVE_DIR, "Like", THEME)
USER_AGENT = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
              "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36")

# Активный домен задаётся в main() через init_domain(); здесь — значения по умолчанию.
BASE             = "https://www.goodfon.com"
SECTION_BASE_URL = f"{BASE}/{THEME}/"
LOGIN_URL        = f"{BASE}/auth/signin/"


def init_domain(base: str):
    """Переключает все URL на указанный домен (com или ru)."""
    global BASE, SECTION_BASE_URL, LOGIN_URL
    BASE = base
    SECTION_BASE_URL = f"{base}/{THEME}/"
    LOGIN_URL = f"{base}/auth/signin/"


def domain_candidates() -> List[str]:
    """Список доменов в порядке предпочтения. Второй — запасной."""
    com = "https://www.goodfon.com"
    ru = "https://www.goodfon.ru"
    if DOMAIN_PREF == "ru":
        return [ru, com]
    if DOMAIN_PREF == "com":
        return [com, ru]
    return [com, ru]  # auto

DOWNLOAD_LIMIT_MARKER = "исчерпали возможное количество скачанных"
# =====================

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s: %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("goodfon")

user32 = ctypes.windll.user32


class DownloadLimitReachedError(Exception):
    """Превышен суточный лимит скачиваний на сайте."""


# ====== Счётчик в config.ini ======

def read_counter() -> int:
    config.read(CONFIG_FILE, encoding="utf-8")
    return config["state"].getint("counter", fallback=0)


def write_counter(value: int):
    config["state"]["counter"] = str(value)
    with open(CONFIG_FILE, "w", encoding="utf-8") as f:
        config.write(f)


# ====== Уведомления в трей ======

def notify(title: str, message: str):
    """Показывает уведомление в трее Windows, если включено в конфиге."""
    if not NOTIFY:
        return
    if not _winotify_available:
        log.warning("winotify не установлен. Запустите: pip install winotify")
        return
    try:
        Notification(app_id="GoodFon", title=title, msg=message, duration="short").show()
    except Exception as e:
        log.warning("Не удалось показать уведомление: %s", e)


# ====== Windows: установка обоев ======

def _make_filter(class_name: str, title: str):
    def enum_windows(handle: int, h_list: list):
        if not (class_name or title):
            h_list.append(handle)
            return True
        if class_name and class_name not in win32gui.GetClassName(handle):
            return True
        if title and title not in win32gui.GetWindowText(handle):
            return True
        h_list.append(handle)
    return enum_windows


def find_window_handles(parent=None, window_class=None, title=None) -> List[int]:
    cb = _make_filter(window_class, title)
    try:
        handle_list: List[int] = []
        if parent:
            win32gui.EnumChildWindows(parent, cb, handle_list)
        else:
            win32gui.EnumWindows(cb, handle_list)
        return handle_list
    except pywintypes.error:
        return []


def force_refresh():
    user32.UpdatePerUserSystemParameters(1)


def enable_activedesktop():
    try:
        progman = find_window_handles(window_class="Progman")[0]
        cryptic_params = (0x52c, 0, 0, 0, 500, None)
        user32.SendMessageTimeoutW(progman, *cryptic_params)
    except IndexError as e:
        raise WindowsError("Cannot enable Active Desktop") from e


def set_wallpaper(image_path: str, use_activedesktop: bool = True):
    if use_activedesktop:
        enable_activedesktop()
    pythoncom.CoInitialize()
    iad = pythoncom.CoCreateInstance(
        shell.CLSID_ActiveDesktop,
        None,
        pythoncom.CLSCTX_INPROC_SERVER,
        shell.IID_IActiveDesktop,
    )
    iad.SetWallpaper(str(image_path), 0)
    iad.ApplyChanges(shellcon.AD_APPLY_ALL)
    force_refresh()
    log.info("Обои выставлены: %s", image_path)


def get_current_wallpaper_path() -> Optional[str]:
    """Возвращает путь к текущим обоям из реестра Windows."""
    try:
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Control Panel\Desktop")
        path, _ = winreg.QueryValueEx(key, "Wallpaper")
        winreg.CloseKey(key)
        return path if path else None
    except Exception as e:
        log.warning("Не удалось прочитать текущие обои из реестра: %s", e)
        return None


# ====== Авторизация ======

def get_csrf_token_from_page(html: str) -> str:
    soup = BeautifulSoup(html, "html.parser")
    inp = soup.find("input", {"name": "csrfmiddlewaretoken"})
    return inp["value"] if inp and inp.get("value") else ""


def login_session() -> requests.Session:
    s = requests.Session()
    s.headers.update({"User-Agent": USER_AGENT})
    r = s.get(LOGIN_URL, timeout=15)
    token = get_csrf_token_from_page(r.text)
    payload = {"csrfmiddlewaretoken": token, "login": LOGIN, "password": PASSWORD}
    headers = {"Referer": LOGIN_URL}
    if token:
        headers["X-CSRFToken"] = token
    resp = s.post(LOGIN_URL, data=payload, headers=headers, allow_redirects=True, timeout=15)
    if resp.status_code >= 400 or "Incorrect password" in resp.text:
        raise RuntimeError("Неверный логин или пароль.")
    log.info("Авторизация успешна")
    return s


def login_session_retry() -> requests.Session:
    """Повторяет вход при сетевых ошибках/таймаутах; при неверном пароле — сразу падает."""
    last = None
    for attempt in range(1, 3):
        try:
            return login_session()
        except RuntimeError:
            raise  # неверные логин/пароль — повторять бессмысленно
        except Exception as e:
            last = e
            log.warning("Сетевая ошибка входа (попытка %d): %s", attempt, e)
    raise last


def is_site_available() -> bool:
    """Быстрая проверка доступности сайта перед логином."""
    try:
        r = requests.get(SECTION_BASE_URL, timeout=8, headers={"User-Agent": USER_AGENT})
        return r.status_code < 500
    except Exception:
        return False


# ====== Кэш сессии (cookie) — отдельный для каждого домена ======

def _cache_key_for(base: str) -> str:
    return "session_ru" if "goodfon.ru" in base else "session_com"


def cache_for(base: str) -> str:
    return SESSION_RU if "goodfon.ru" in base else SESSION_COM


def save_session_cache(session: requests.Session, base: str):
    """Сохраняет cookie сессии в поле своего домена, не трогая другой кэш."""
    cookies = "; ".join(f"{c.name}={c.value}" for c in session.cookies)
    config.read(CONFIG_FILE, encoding="utf-8")
    if not config.has_section("auth"):
        config.add_section("auth")
    config["auth"][_cache_key_for(base)] = cookies
    with open(CONFIG_FILE, "w", encoding="utf-8") as f:
        config.write(f)
    log.info("Кэш сессии сохранён для домена %s", base)


def build_session_from_cache(cookie_str: str, base: str) -> requests.Session:
    """Поднимает сессию из сохранённых cookie указанного домена."""
    s = requests.Session()
    s.headers.update({"User-Agent": USER_AGENT})
    host = urlparse(base).hostname
    for part in cookie_str.split(";"):
        part = part.strip()
        if not part or "=" not in part:
            continue
        name, value = part.split("=", 1)
        s.cookies.set(name.strip(), value.strip(), domain=host)
    return s


def is_logged_in(session: requests.Session) -> bool:
    """Проверяет, что сессия авторизована (по наличию ссылки выхода)."""
    try:
        r = session.get(SECTION_BASE_URL, timeout=15)
    except Exception:
        return False
    return r.status_code == 200 and "auth/logout" in r.text


# ====== Пагинация и сбор ссылок ======

def get_max_pages(session: requests.Session) -> int:
    r = session.get(SECTION_BASE_URL, timeout=15)
    if r.status_code != 200:
        raise ValueError(f"Не удалось загрузить первую страницу: статус {r.status_code}")

    soup = BeautifulSoup(r.text, "html.parser")

    paginator = soup.find("div", class_="paginator")
    if paginator:
        max_page = 1
        for link in paginator.find_all("a", href=True):
            m = re.search(r"index-(\d+)\.html", link["href"])
            if m:
                page = int(m.group(1))
                if page > max_page:
                    max_page = page
        if max_page > 1:
            return max_page

    last_link = soup.find("a", string=re.compile(r"(Последняя|Last)", re.IGNORECASE))
    if last_link and "href" in last_link.attrs:
        m = re.search(r"index-(\d+)\.html", last_link["href"])
        if m:
            return int(m.group(1))

    raise ValueError("Пагинация не найдена")


def get_random_wallpaper_page_url(max_pages: int) -> str:
    page_num = random.randint(1, max_pages)
    return SECTION_BASE_URL if page_num == 1 else f"{SECTION_BASE_URL}index-{page_num}.html"


def collect_wallpaper_links(section_html: str) -> List[str]:
    soup = BeautifulSoup(section_html, "html.parser")
    links = [
        a["href"]
        for a in soup.find_all("a", href=True)
        if "/wallpaper-" in a["href"]
        and a["href"].endswith(".html")
        and "wallpaper-download" not in a["href"]
    ]
    return list(set(links))


# ====== Поиск и скачивание картинки ======

def find_download_href_on_image_page(session: requests.Session, image_page_url: str) -> Optional[str]:
    r = session.get(image_page_url, headers={"Referer": SECTION_BASE_URL}, timeout=15)
    if r.status_code != 200:
        log.warning("Не удалось загрузить страницу изображения: %s (статус %s)", image_page_url, r.status_code)
        return None

    soup = BeautifulSoup(r.text, "html.parser")

    # Строго ищем ссылку с нужным разрешением (с дефисом в конце).
    target = None
    for a in soup.find_all("a", href=True):
        if f"wallpaper-download-{RESOLUTION}-" in a["href"]:
            target = a["href"]
            break

    if not target:
        log.info("Разрешение %s недоступно для этой картинки, пропускаем.", RESOLUTION)
        return None

    download_page_url = urljoin(image_page_url, target)
    rr = session.get(download_page_url, headers={"Referer": image_page_url}, timeout=15)
    if rr.status_code != 200:
        return None

    rr.encoding = rr.apparent_encoding or "utf-8"
    if DOWNLOAD_LIMIT_MARKER in rr.text or "download_limit" in rr.text:
        log.warning("Превышен суточный лимит скачиваний на сайте.")
        raise DownloadLimitReachedError()

    soup2 = BeautifulSoup(rr.text, "html.parser")

    # Прямая ссылка на оригинал в <a class="js-download_img" href="...">
    a_dl = soup2.find("a", {"class": "js-download_img"})
    if a_dl and a_dl.get("href") and "img.goodfon" in a_dl["href"]:
        return a_dl["href"]

    # Запасной вариант — img с img.goodfon
    img = soup2.find("img", src=lambda s: s and "img.goodfon" in s)
    if img:
        return img["src"]

    log.warning("Ссылка на картинку не найдена на странице загрузки: %s", download_page_url)
    return None


def download_final_image(session: requests.Session, final_url: str) -> Optional[bytes]:
    """Скачивает картинку, при недоступности img.goodfon.com пробует img.goodfon.ru (и наоборот)."""
    urls_to_try = [final_url]
    for primary, fallback in [("img.goodfon.com", "img.goodfon.ru"),
                              ("img.goodfon.ru", "img.goodfon.com")]:
        if primary in final_url:
            urls_to_try.append(final_url.replace(primary, fallback))
            break

    for url in urls_to_try:
        try:
            r = session.get(url, headers={"User-Agent": USER_AGENT}, allow_redirects=True, timeout=20)
            if r.status_code == 200 and "image" in r.headers.get("Content-Type", ""):
                if url != final_url:
                    log.info("Использован резервный img домен: %s", url)
                return r.content
        except Exception as e:
            log.warning("Не удалось скачать с %s: %s", url, e)
    return None


def save_image_and_get_path(final_url: str, content: bytes) -> str:
    os.makedirs(SAVE_DIR, exist_ok=True)
    parsed = urlparse(final_url)
    filename = os.path.basename(parsed.path).split("?")[0].replace(" ", "_")
    file_path = os.path.join(SAVE_DIR, filename)
    with open(file_path, "wb") as f:
        f.write(content)
    log.info("Файл сохранён: %s", file_path)
    return file_path


def cleanup_old_images():
    files = sorted(
        [f for f in glob.glob(os.path.join(SAVE_DIR, "*.*")) if os.path.isfile(f)],
        key=os.path.getmtime,
    )
    if len(files) > MAX_FILES:
        for f in files[:-MAX_FILES]:
            try:
                os.remove(f)
                log.info("Удалён старый файл: %s", f)
            except Exception as e:
                log.warning("Не удалось удалить файл %s: %s", f, e)


# ====== Like / Unlike ======

def get_last_downloaded_file() -> Optional[str]:
    files = sorted(
        [f for f in glob.glob(os.path.join(SAVE_DIR, "*.*")) if os.path.isfile(f)],
        key=os.path.getmtime,
    )
    return files[-1] if files else None


def _random_excluding(files: List[str], exclude: Optional[str]) -> str:
    exclude = (exclude or "").lower()
    candidates = [f for f in files if f.lower() != exclude]
    return random.choice(candidates if candidates else files)


def set_wallpaper_from_like() -> bool:
    """Случайная картинка из Like/THEME (без повтора текущей)."""
    files = [f for f in glob.glob(os.path.join(LIKE_DIR, "*.*")) if os.path.isfile(f)]
    if not files:
        log.warning("Папка Like/%s пуста, загружаем с сайта", THEME)
        return False
    chosen = _random_excluding(files, get_current_wallpaper_path())
    set_wallpaper(chosen)
    log.info("Обои из папки Like/%s: %s", THEME, chosen)
    notify("Обои обновлены — из избранного", os.path.basename(chosen))
    return True


def get_favorite_ids(session: requests.Session, image_page_url: str):
    """Возвращает (add_url, del_url) из блока избранного на странице картинки."""
    r = session.get(image_page_url, timeout=15)
    if r.status_code != 200:
        log.warning("Не удалось открыть страницу картинки: %s", image_page_url)
        return None, None
    soup = BeautifulSoup(r.text, "html.parser")
    fav = soup.find("a", {"class": "js-favorite"})
    if not fav:
        log.warning("Блок избранного не найден на странице: %s", image_page_url)
        return None, None
    return fav.get("data-add"), fav.get("data-del")


def add_to_like(session: requests.Session):
    """Добавляет текущую картинку в Like/THEME и в избранное на сайте."""
    last_file = get_last_downloaded_file()
    if not last_file:
        log.error("Нет последнего скачанного файла.")
        return

    filename = os.path.basename(last_file)
    name_only = os.path.splitext(filename)[0]

    os.makedirs(LIKE_DIR, exist_ok=True)
    dest_path = os.path.join(LIKE_DIR, filename)
    if not os.path.exists(dest_path):
        shutil.copy2(last_file, dest_path)
        log.info("Изображение скопировано в папку Like/%s: %s", THEME, dest_path)
    else:
        log.info("Изображение уже есть в папке Like/%s", THEME)

    # Переставляем обои на копию из Like, чтобы unlike работал корректно
    set_wallpaper(dest_path)
    log.info("Обои переключены на копию из Like/%s", THEME)

    image_page_url = f"{BASE}/{THEME}/wallpaper-{name_only}.html"
    add_url, _ = get_favorite_ids(session, image_page_url)
    if add_url:
        rr = session.get(urljoin(BASE, add_url),
                         headers={"Referer": image_page_url}, timeout=15)
        if rr.status_code == 200:
            log.info("Изображение добавлено в избранное на сайте: %s", filename)
            notify("Добавлено в избранное", filename)
        else:
            log.warning("Ошибка добавления в избранное: статус %s", rr.status_code)
    else:
        log.warning("Не найден элемент для добавления в избранное на странице.")


def remove_from_like(session: requests.Session) -> bool:
    """Удаляет текущую картинку из Like/THEME и из избранного на сайте."""
    current = get_current_wallpaper_path()
    if not current:
        log.error("Не удалось определить текущие обои.")
        return False

    if not current.lower().startswith(LIKE_DIR.lower()):
        log.error("Текущие обои не из папки Like/%s: %s", THEME, current)
        log.error("Unlike работает только когда установлена картинка из избранного.")
        notify("GoodFon: ошибка", "Текущие обои не из папки избранного.")
        return False

    filename = os.path.basename(current)
    name_only = os.path.splitext(filename)[0]
    log.info("Удаляем из избранного: %s", filename)

    image_page_url = f"{BASE}/{THEME}/wallpaper-{name_only}.html"
    _, del_url = get_favorite_ids(session, image_page_url)
    if del_url:
        rr = session.get(urljoin(BASE, del_url),
                         headers={"Referer": image_page_url}, timeout=15)
        if rr.status_code == 200:
            log.info("Изображение удалено из избранного на сайте: %s", filename)
        else:
            log.warning("Ошибка удаления из избранного: статус %s", rr.status_code)
    else:
        log.warning("Не найден элемент для удаления из избранного на странице.")

    try:
        os.remove(current)
        log.info("Файл удалён из папки Like/%s: %s", THEME, filename)
        notify("Удалено из избранного", filename)
    except Exception as e:
        log.error("Не удалось удалить файл %s: %s", current, e)
        return False

    return True


# ====== Локальный fallback ======

def fallback_local(like_only: bool = False):
    """Случайная картинка из локальной папки.
    like_only=True — только из Like/THEME; иначе Like/THEME, затем основная папка."""
    files = [f for f in glob.glob(os.path.join(LIKE_DIR, "*.*")) if os.path.isfile(f)]
    if not like_only and not files:
        files = [f for f in glob.glob(os.path.join(SAVE_DIR, "*.*")) if os.path.isfile(f)]

    if not files:
        log.warning("Fallback: локальных картинок нет, обои не изменены.")
        notify("GoodFon: ошибка", "Нет доступных картинок.")
        return

    chosen = _random_excluding(files, get_current_wallpaper_path())
    log.info("Fallback: устанавливаем локальную картинку: %s", chosen)
    set_wallpaper(chosen)
    if like_only:
        notify("Обои обновлены — из избранного", os.path.basename(chosen))
    else:
        notify("Обои обновлены — локально", os.path.basename(chosen))


# ====== Основной запуск ======

def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "update"

    session = None

    # Для каждого домена (в порядке предпочтения): сначала его кэш, затем вход.
    for base in domain_candidates():
        init_domain(base)

        cached_cookies = cache_for(base)
        if cached_cookies:
            cached = build_session_from_cache(cached_cookies, base)
            if is_logged_in(cached):
                session = cached
                log.info("Используется кэш сессии: %s", base)
                break
            log.info("Кэш для %s недействителен, выполняем вход.", base)

        if not is_site_available():
            log.warning("Домен недоступен: %s", base)
            continue
        try:
            session = login_session_retry()
            log.info("Активный домен: %s", base)
            save_session_cache(session, base)
            break
        except Exception as e:
            log.warning("Не удалось войти на %s: %s", base, e)
            session = None

    if session is None:
        log.error("Ни один домен недоступен или вход не выполнен.")
        if arg in ("like", "unlike"):
            notify("GoodFon: сайт недоступен", "Операция с избранным невозможна.")
            return
        write_counter(read_counter() + 1)
        fallback_local()
        return

    if arg == "like":
        try:
            add_to_like(session)
        except Exception as e:
            log.error("Ошибка при добавлении в избранное: %s", e)
            notify("GoodFon: ошибка", "Не удалось добавить в избранное.")
        return

    if arg == "unlike":
        try:
            if remove_from_like(session):
                log.info("Загружаем новую картинку с сайта после удаления из избранного.")
            else:
                return
        except Exception as e:
            log.error("Ошибка при удалении из избранного: %s", e)
            notify("GoodFon: ошибка", "Не удалось удалить из избранного.")
            return

    # Счётчик: каждый LIKE_EVERY_N-й запуск берём из Like
    counter = read_counter() + 1
    write_counter(counter)
    log.info("Запуск #%d (из Like каждые %d)", counter, LIKE_EVERY_N)

    if counter >= LIKE_EVERY_N:
        write_counter(0)
        if set_wallpaper_from_like():
            return
        log.info("Папка Like пуста, продолжаем загрузку с сайта")

    # Загрузка с сайта
    max_pages = None
    for attempt in range(1, MAX_ATTEMPTS + 1):
        try:
            log.info("Попытка %d из %d", attempt, MAX_ATTEMPTS)

            if max_pages is None:
                max_pages = get_max_pages(session)
                log.info("Максимальное количество страниц: %d", max_pages)

            page_url = get_random_wallpaper_page_url(max_pages)
            log.info("Выбрана страница раздела: %s", page_url)

            r = session.get(page_url, timeout=15)
            if r.status_code != 200:
                log.warning("Страница вернула статус %s", r.status_code)
                continue

            links = collect_wallpaper_links(r.text)
            if not links:
                log.warning("На странице нет обоев, пробуем другую")
                continue

            abs_links = [urljoin(SECTION_BASE_URL, href) for href in links]
            image_page_url = random.choice(abs_links)
            log.info("Выбрана страница изображения: %s", image_page_url)

            final_url = find_download_href_on_image_page(session, image_page_url)
            if not final_url:
                log.warning("Ссылка на скачивание не найдена, пробуем другую")
                continue

            content = download_final_image(session, final_url)
            if not content:
                log.warning("Не удалось скачать картинку, пробуем другую")
                continue

            saved_path = save_image_and_get_path(final_url, content)
            cleanup_old_images()
            set_wallpaper(saved_path)
            notify("Обои обновлены — с сайта", os.path.basename(saved_path))
            return

        except DownloadLimitReachedError:
            log.warning("Суточный лимит скачиваний исчерпан, переходим на локальные картинки.")
            notify("GoodFon: лимит исчерпан", "Загружаем из избранного.")
            fallback_local(like_only=True)
            return

        except ValueError as e:
            log.warning("Ошибка пагинации при попытке %d: %s", attempt, e)
            max_pages = None

        except Exception as e:
            log.error("Ошибка при попытке %d: %s", attempt, e)

    log.error("Не удалось найти и скачать изображение после %d попыток.", MAX_ATTEMPTS)
    fallback_local()


if __name__ == "__main__":
    main()
