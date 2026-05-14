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

# ====== Загрузка конфига ======
CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.ini")

config = configparser.ConfigParser()

def load_config():
    if not os.path.exists(CONFIG_FILE):
        raise FileNotFoundError(f"Файл конфигурации не найден: {CONFIG_FILE}")
    config.read(CONFIG_FILE, encoding="utf-8")

def get_config_str(section: str, key: str) -> str:
    return config[section][key].strip()

def get_config_int(section: str, key: str, fallback: int = 0) -> int:
    return config[section].getint(key, fallback=fallback)

# ====== Настройки (читаются из config.ini) ======
load_config()

LOGIN          = get_config_str("auth", "login")
PASSWORD       = get_config_str("auth", "password")
RESOLUTION     = get_config_str("settings", "resolution")
THEME          = get_config_str("settings", "theme")
SAVE_DIR       = get_config_str("settings", "save_dir")
MAX_FILES      = get_config_int("settings", "max_files", fallback=10)
LIKE_EVERY_N   = get_config_int("settings", "like_every_n", fallback=10)
MAX_ATTEMPTS   = get_config_int("settings", "max_attempts", fallback=3)
NOTIFY         = config["settings"].getboolean("notify", fallback=True)

LIKE_DIR         = os.path.join(SAVE_DIR, "Like")
SECTION_BASE_URL = f"https://www.goodfon.com/{THEME}/"
LOGIN_URL        = "https://www.goodfon.com/auth/signin/"
USER_AGENT       = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36")
# =====================

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s: %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S"
)
log = logging.getLogger("goodfon")

user32 = ctypes.windll.user32


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
        toast = Notification(
            app_id="GoodFon",
            title=title,
            msg=message,
            duration="short"
        )
        toast.show()
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

def find_window_handles(parent: int = None, window_class: str = None, title: str = None) -> List[int]:
    cb = _make_filter(window_class, title)
    try:
        handle_list = []
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
        shell.IID_IActiveDesktop
    )
    iad.SetWallpaper(str(image_path), 0)
    iad.ApplyChanges(shellcon.AD_APPLY_ALL)
    force_refresh()
    log.info("Обои выставлены: %s", image_path)


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
    payload = {
        "csrfmiddlewaretoken": token,
        "login": LOGIN,
        "password": PASSWORD
    }
    headers = {"Referer": LOGIN_URL}
    if token:
        headers["X-CSRFToken"] = token
    resp = s.post(LOGIN_URL, data=payload, headers=headers, allow_redirects=True, timeout=15)
    if resp.status_code >= 400 or "Incorrect password" in resp.text:
        raise RuntimeError("Неверный логин или пароль.")
    log.info("Авторизация успешна")
    return s


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
            match = re.search(r"index-(\d+)\.html", link["href"])
            if match:
                page = int(match.group(1))
                if page > max_page:
                    max_page = page
        if max_page > 1:
            return max_page

    last_link = soup.find("a", string=re.compile(r"(Последняя|Last)", re.IGNORECASE))
    if last_link and "href" in last_link.attrs:
        match = re.search(r"index-(\d+)\.html", last_link["href"])
        if match:
            return int(match.group(1))

    page_div = soup.find("div", class_="paginator__page")
    if page_div:
        parts = re.split(r"\s*(из|of)\s*", page_div.text.strip(), flags=re.IGNORECASE)
        if len(parts) >= 3:
            total_str = parts[2].strip().replace(" ", "").replace(",", "")
            if total_str.isdigit():
                return int(total_str)

    log.warning("Пагинация не найдена, используем 1 страницу как fallback")
    return 1

def get_random_wallpaper_page_url(max_pages: int) -> str:
    page_num = random.randint(1, max_pages)
    return SECTION_BASE_URL if page_num == 1 else f"{SECTION_BASE_URL}index-{page_num}.html"

def collect_wallpaper_links(section_html: str) -> List[str]:
    soup = BeautifulSoup(section_html, "html.parser")
    links = [
        a["href"] for a in soup.find_all("a", href=True)
        if "/wallpaper-" in a["href"] and a["href"].endswith(".html")
    ]
    return list(set(links))


# ====== Скачивание картинки ======

def find_download_href_on_image_page(session: requests.Session, image_page_url: str) -> Optional[str]:
    r = session.get(image_page_url, headers={"Referer": SECTION_BASE_URL}, timeout=15)
    if r.status_code != 200:
        log.warning("Не удалось загрузить страницу изображения: %s (статус %s)", image_page_url, r.status_code)
        return None

    soup = BeautifulSoup(r.text, "html.parser")

    for a in soup.find_all("a", href=True):
        if f"wallpaper-download-{RESOLUTION}" in a["href"]:
            download_page_url = urljoin(image_page_url, a["href"])
            rr = session.get(download_page_url, headers={"Referer": image_page_url}, timeout=15)
            if rr.status_code != 200:
                return None
            soup2 = BeautifulSoup(rr.text, "html.parser")
            img = soup2.find("img", src=True)
            if img and "img.goodfon.com" in img["src"]:
                return urljoin(download_page_url, img["src"])

    # Fallback: прямая картинка на странице
    available = [a["href"] for a in soup.find_all("a", href=True) if "wallpaper-download-" in a["href"]]
    if available:
        log.warning("Разрешение %s не найдено. Доступные: %s", RESOLUTION, available)

    img = soup.find("img", src=True)
    return urljoin(image_page_url, img["src"]) if img and "img.goodfon.com" in img["src"] else None

def download_final_image(session: requests.Session, final_url: str) -> Optional[bytes]:
    r = session.get(final_url, headers={"User-Agent": USER_AGENT}, allow_redirects=True, timeout=20)
    if r.status_code == 200 and "image" in r.headers.get("Content-Type", ""):
        return r.content
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
        key=os.path.getmtime
    )
    if len(files) > MAX_FILES:
        for f in files[:-MAX_FILES]:
            try:
                os.remove(f)
                log.info("Удалён старый файл: %s", f)
            except Exception as e:
                log.warning("Не удалось удалить файл %s: %s", f, e)


# ====== Like ======

def get_last_downloaded_file() -> Optional[str]:
    files = sorted(
        [f for f in glob.glob(os.path.join(SAVE_DIR, "*.*")) if os.path.isfile(f)],
        key=os.path.getmtime
    )
    return files[-1] if files else None

def set_wallpaper_from_like() -> bool:
    """Выбирает случайную картинку из папки Like и устанавливает её фоном.
    Возвращает True при успехе, False если папка пуста."""
    files = [f for f in glob.glob(os.path.join(LIKE_DIR, "*.*")) if os.path.isfile(f)]
    if not files:
        log.warning("Папка Like пуста, загружаем с сайта")
        return False
    chosen = random.choice(files)
    set_wallpaper(chosen)
    log.info("Обои из папки Like: %s", chosen)
    notify("Обои обновлены ❤️", f"Из папки Like: {os.path.basename(chosen)}")
    return True

def add_to_like(session: requests.Session):
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
        log.info("Изображение скопировано в папку Like: %s", dest_path)

    image_page_url = f"https://www.goodfon.com/{THEME}/wallpaper-{name_only}.html"
    r = session.get(image_page_url, timeout=15)
    if r.status_code != 200:
        log.warning("Не удалось открыть страницу для добавления в избранное: %s", image_page_url)
        return

    soup = BeautifulSoup(r.text, "html.parser")
    fav = soup.find("a", {"class": "wallpaper__favorite"})
    if fav and fav.get("data-add"):
        add_url = urljoin("https://www.goodfon.com", fav["data-add"])
        rr = session.get(add_url, headers={"Referer": image_page_url}, timeout=15)
        if rr.status_code == 200:
            log.info("Изображение добавлено в избранное на сайте: %s", filename)
        else:
            log.warning("Ошибка добавления в избранное: статус %s", rr.status_code)
    else:
        log.warning("Не найден элемент для добавления в избранное на странице.")


# ====== Основной запуск ======

def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "update"

    try:
        session = login_session()
    except Exception as e:
        log.error("Ошибка логина: %s", e)
        return

    if arg == "like":
        add_to_like(session)
        return

    # Счётчик: каждый LIKE_EVERY_N-й запуск берём картинку из Like
    counter = read_counter() + 1
    write_counter(counter)
    log.info("Запуск #%d (из Like каждые %d)", counter, LIKE_EVERY_N)

    if counter >= LIKE_EVERY_N:
        write_counter(0)
        if set_wallpaper_from_like():
            return
        log.info("Папка Like пуста, продолжаем загрузку с сайта")

    # Загрузка с сайта
    try:
        max_pages = get_max_pages(session)
        log.info("Максимальное количество страниц: %d", max_pages)
    except ValueError as e:
        log.error("Ошибка пагинации: %s", e)
        return

    for attempt in range(1, MAX_ATTEMPTS + 1):
        try:
            log.info("Попытка %d из %d", attempt, MAX_ATTEMPTS)
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
            notify("Обои обновлены 🖼️", f"С сайта: {os.path.basename(saved_path)}")
            return

        except Exception as e:
            log.error("Ошибка при попытке %d: %s", attempt, e)

    log.error("Не удалось найти и скачать изображение после %d попыток.", MAX_ATTEMPTS)

    # Fallback: берём случайную картинку из локальной папки
    local_files = [f for f in glob.glob(os.path.join(SAVE_DIR, "*.*")) if os.path.isfile(f)]
    if not local_files:
        local_files = [f for f in glob.glob(os.path.join(LIKE_DIR, "*.*")) if os.path.isfile(f)]

    if local_files:
        chosen = random.choice(local_files)
        log.info("Fallback: устанавливаем локальную картинку: %s", chosen)
        set_wallpaper(chosen)
        notify("Обои обновлены 📁", f"Локально (сайт недоступен): {os.path.basename(chosen)}")
    else:
        log.warning("Fallback: локальных картинок нет, обои не изменены.")
        notify("GoodFon: ошибка 😞", "Сайт недоступен и локальных картинок нет.")


if __name__ == "__main__":
    main()
