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

LIKE_DIR         = os.path.join(SAVE_DIR, "Like", THEME)
SECTION_BASE_URL = f"https://www.goodfon.com/{THEME}/"
LOGIN_URL        = "https://www.goodfon.com/auth/signin/"
USER_AGENT       = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36")

DOWNLOAD_LIMIT_MARKER = "исчерпали возможное количество скачанных"
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

    # Ищем ссылку строго с нужным разрешением в блоке скачивания
    download_div = soup.find("div", class_="wallpaper__download")
    if not download_div:
        log.warning("Блок скачивания не найден на странице: %s", image_page_url)
        return None

    target_href = None
    for a in download_div.find_all("a", href=True):
        if f"wallpaper-download-{RESOLUTION}-" in a["href"]:
            target_href = a["href"]
            break

    if not target_href:
        log.info("Разрешение %s недоступно для этой картинки, пропускаем.", RESOLUTION)
        return None

    # Переходим на страницу загрузки
    download_page_url = urljoin(image_page_url, target_href)
    rr = session.get(download_page_url, headers={"Referer": image_page_url}, timeout=15)
    if rr.status_code != 200:
        return None

    # Принудительно указываем кодировку чтобы корректно читать русский текст
    rr.encoding = rr.apparent_encoding or "utf-8"

    # Проверяем превышение суточного лимита
    if DOWNLOAD_LIMIT_MARKER in rr.text or "download_limit" in rr.text:
        log.warning("Превышен суточный лимит скачиваний на сайте.")
        raise DownloadLimitReachedError()

    # Ищем ссылку на картинку
    soup2 = BeautifulSoup(rr.text, "html.parser")
    img = soup2.find("img", src=True)
    if img and "img.goodfon" in img["src"]:
        return urljoin(download_page_url, img["src"])

    log.warning("Ссылка на картинку не найдена на странице загрузки: %s", download_page_url)
    return None

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

def get_current_wallpaper_path() -> Optional[str]:
    """Возвращает путь к текущим обоям из реестра Windows."""
    try:
        import winreg
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                             r"Control Panel\Desktop")
        path, _ = winreg.QueryValueEx(key, "Wallpaper")
        winreg.CloseKey(key)
        return path if path else None
    except Exception as e:
        log.warning("Не удалось прочитать текущие обои из реестра: %s", e)
        return None

def set_wallpaper_from_like() -> bool:
    """Выбирает случайную картинку из папки Like/THEME и устанавливает её фоном."""
    files = [f for f in glob.glob(os.path.join(LIKE_DIR, "*.*")) if os.path.isfile(f)]
    if not files:
        log.warning("Папка Like/%s пуста, загружаем с сайта", THEME)
        return False
    chosen = random.choice(files)
    set_wallpaper(chosen)
    log.info("Обои из папки Like/%s: %s", THEME, chosen)
    notify("Обои обновлены ❤️", f"Из папки Like: {os.path.basename(chosen)}")
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
    add_url = fav.get("data-add")
    del_url = fav.get("data-del")
    return add_url, del_url

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

    # Переставляем обои на копию из Like/THEME чтобы unlike работал корректно
    set_wallpaper(dest_path)
    log.info("Обои переключены на копию из Like/%s", THEME)

    image_page_url = f"https://www.goodfon.com/{THEME}/wallpaper-{name_only}.html"
    add_url, _ = get_favorite_ids(session, image_page_url)
    if add_url:
        r = session.get(urljoin("https://www.goodfon.com", add_url),
                        headers={"Referer": image_page_url}, timeout=15)
        if r.status_code == 200:
            log.info("Изображение добавлено в избранное на сайте: %s", filename)
            notify("Добавлено в избранное ❤️", filename)
        else:
            log.warning("Ошибка добавления в избранное: статус %s", r.status_code)
    else:
        log.warning("Не найден элемент для добавления в избранное на странице.")

def remove_from_like(session: requests.Session):
    """Удаляет текущую картинку из Like/THEME и из избранного на сайте,
    затем загружает новую картинку с сайта."""
    current = get_current_wallpaper_path()
    if not current:
        log.error("Не удалось определить текущие обои.")
        return False

    # Проверяем что текущие обои находятся в папке Like/THEME
    if not current.lower().startswith(LIKE_DIR.lower()):
        log.error("Текущие обои не из папки Like/%s: %s", THEME, current)
        log.error("Unlike работает только когда установлена картинка из избранного.")
        notify("GoodFon: ошибка 😞", "Текущие обои не из папки избранного.")
        return False

    filename = os.path.basename(current)
    name_only = os.path.splitext(filename)[0]
    log.info("Удаляем из избранного: %s", filename)

    image_page_url = f"https://www.goodfon.com/{THEME}/wallpaper-{name_only}.html"
    _, del_url = get_favorite_ids(session, image_page_url)
    if del_url:
        r = session.get(urljoin("https://www.goodfon.com", del_url),
                        headers={"Referer": image_page_url}, timeout=15)
        if r.status_code == 200:
            log.info("Изображение удалено из избранного на сайте: %s", filename)
        else:
            log.warning("Ошибка удаления из избранного: статус %s", r.status_code)
    else:
        log.warning("Не найден элемент для удаления из избранного на странице.")

    # Удаляем локальный файл
    try:
        os.remove(current)
        log.info("Файл удалён из папки Like/%s: %s", THEME, filename)
        notify("Удалено из избранного 🗑️", filename)
    except Exception as e:
        log.error("Не удалось удалить файл %s: %s", current, e)
        return False

    return True


# ====== Исключение: лимит скачиваний ======

class DownloadLimitReachedError(Exception):
    """Превышен суточный лимит скачиваний на сайте."""


# ====== Локальный fallback ======

def fallback_local(like_only: bool = False):
    """Устанавливает случайную картинку из локальной папки.
    Если like_only=True — берёт только из папки Like."""
    if like_only:
        local_files = [f for f in glob.glob(os.path.join(LIKE_DIR, "*.*")) if os.path.isfile(f)]
    else:
        local_files = [f for f in glob.glob(os.path.join(SAVE_DIR, "*.*")) if os.path.isfile(f)]
        if not local_files:
            local_files = [f for f in glob.glob(os.path.join(LIKE_DIR, "*.*")) if os.path.isfile(f)]

    if local_files:
        chosen = random.choice(local_files)
        log.info("Fallback: устанавливаем локальную картинку: %s", chosen)
        set_wallpaper(chosen)
        notify("Обои обновлены 📁", f"Локально: {os.path.basename(chosen)}")
    else:
        log.warning("Fallback: локальных картинок нет, обои не изменены.")
        notify("GoodFon: ошибка 😞", "Нет доступных картинок.")


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

    if arg == "unlike":
        if remove_from_like(session):
            log.info("Загружаем новую картинку с сайта после удаления из избранного.")
        else:
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

        except DownloadLimitReachedError:
            log.warning("Суточный лимит скачиваний исчерпан, переходим на локальные картинки.")
            notify("GoodFon: лимит исчерпан ⚠️", "Суточный лимит исчерпан. Загружаем из избранного.")
            fallback_local(like_only=True)
            return

        except Exception as e:
            log.error("Ошибка при попытке %d: %s", attempt, e)

    log.error("Не удалось найти и скачать изображение после %d попыток.", MAX_ATTEMPTS)
    fallback_local()


if __name__ == "__main__":
    main()
