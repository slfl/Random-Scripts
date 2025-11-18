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
import pythoncom
import pywintypes
import win32gui
from win32com.shell import shell, shellcon
from typing import List

# ====== Настройки ======
LOGIN = "ЛОГИН"
PASSWORD = "ПАРОЛЬ"
RESOLUTION = "1920x1080"
THEME = "erotic"
SAVE_DIR = r"C:\Users\Mansi\Pictures\GoodFon"
LIKE_DIR = os.path.join(SAVE_DIR, "Like")
SECTION_BASE_URL = f"https://www.goodfon.com/{THEME}/"
LOGIN_URL = "https://www.goodfon.com/auth/signin/"
USER_AGENT = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
              "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36")
MAX_FILES = 10
# =====================

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s: %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S"
)
log = logging.getLogger("goodfon")

user32 = ctypes.windll.user32

def _make_filter(class_name: str, title: str):
    def enum_windows(handle: int, h_list: list):
        if not (class_name or title):
            h_list.append(handle)
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
        progman = find_window_handles(window_class='Progman')[0]
        cryptic_params = (0x52c, 0, 0, 0, 500, None)
        user32.SendMessageTimeoutW(progman, *cryptic_params)
    except IndexError as e:
        raise WindowsError('Cannot enable Active Desktop') from e

def set_wallpaper(image_path: str, use_activedesktop: bool = True):
    if use_activedesktop:
        enable_activedesktop()
    pythoncom.CoInitialize()
    iad = pythoncom.CoCreateInstance(shell.CLSID_ActiveDesktop,
                                     None,
                                     pythoncom.CLSCTX_INPROC_SERVER,
                                     shell.IID_IActiveDesktop)
    iad.SetWallpaper(str(image_path), 0)
    iad.ApplyChanges(shellcon.AD_APPLY_ALL)
    force_refresh()
    log.info("Обои выставлены с плавным переходом.")

def get_csrf_token_from_page(html: str):
    soup = BeautifulSoup(html, "html.parser")
    inp = soup.find("input", {"name": "csrfmiddlewaretoken"})
    return inp["value"] if inp and inp.get("value") else None

def login_session() -> requests.Session:
    s = requests.Session()
    s.headers.update({"User-Agent": USER_AGENT})
    r = s.get(LOGIN_URL, timeout=15)
    token = get_csrf_token_from_page(r.text) or ""
    payload = {
        "csrfmiddlewaretoken": token,
        "login": LOGIN,
        "password": PASSWORD
    }
    headers = {"Referer": LOGIN_URL}
    if token:
        headers["X-CSRFToken"] = token
    resp = s.post(LOGIN_URL, data=payload, headers=headers, allow_redirects=True, timeout=15)
    if resp.status_code >= 400 or "Incorrect password" in resp.text or "Incorrect password or login" in resp.text:
        raise RuntimeError("Неверный логин или пароль / ошибка логина.")
    log.info("Авторизация успешна")
    return s

def get_max_pages(session: requests.Session) -> int:
    r = session.get(SECTION_BASE_URL, timeout=15)
    if r.status_code != 200:
        raise ValueError(f"Не удалось загрузить первую страницу раздела: статус {r.status_code}")
    
    soup = BeautifulSoup(r.text, "html.parser")
    
    paginator = soup.find("div", class_="paginator")
    if paginator:
        page_links = paginator.find_all("a", href=True)
        max_page = 1
        for link in page_links:
            match = re.search(r'index-(\d+)\.html', link["href"])
            if match:
                page = int(match.group(1))
                if page > max_page:
                    max_page = page
        if max_page > 1:
            return max_page
    
    last_link = soup.find("a", string=re.compile("(Последняя|Last)", re.IGNORECASE))
    if last_link and "href" in last_link.attrs:
        href = last_link["href"]
        match = re.search(r'index-(\d+)\.html', href)
        if match:
            return int(match.group(1))
    
    page_div = soup.find("div", class_="paginator__page")
    if page_div:
        text = page_div.text.strip()
        parts = re.split(r'\s*(из|of)\s*', text, flags=re.IGNORECASE)
        if len(parts) >= 3:
            total_str = parts[2].strip().replace(" ", "").replace(",", "")
            if total_str.isdigit():
                return int(total_str)
    
    raise ValueError("Не удалось найти пагинацию на странице. Проверь структуру сайта или язык интерфейса.")

def get_random_wallpaper_page_url(max_pages: int):
    page_num = random.randint(1, max_pages)
    return SECTION_BASE_URL if page_num == 1 else f"{SECTION_BASE_URL}index-{page_num}.html"

def collect_wallpaper_links(section_html: str):
    soup = BeautifulSoup(section_html, "html.parser")
    links = [a["href"] for a in soup.find_all("a", href=True) 
             if "/wallpaper-" in a["href"] and a["href"].endswith(".html")]
    return list(set(links))

def find_download_href_on_image_page(session: requests.Session, image_page_url: str):
    r = session.get(image_page_url, headers={"Referer": SECTION_BASE_URL}, timeout=15)
    if r.status_code != 200:
        log.warning("Не удалось загрузить страницу изображения: %s (status %s)", image_page_url, r.status_code)
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

    img = soup.find("img", src=True)
    return urljoin(image_page_url, img["src"]) if img and "img.goodfon.com" in img["src"] else None

def download_final_image(session: requests.Session, final_url: str):
    headers = {"User-Agent": USER_AGENT}
    r = session.get(final_url, headers=headers, allow_redirects=True, timeout=20)
    return r.content if r.status_code == 200 and "image" in r.headers.get("Content-Type", "") else None

def save_image_and_get_path(final_url: str, content: bytes):
    os.makedirs(SAVE_DIR, exist_ok=True)
    parsed = urlparse(final_url)
    filename = os.path.basename(parsed.path).split("?")[0].replace(" ", "_")
    file_path = os.path.join(SAVE_DIR, filename)
    with open(file_path, "wb") as f:
        f.write(content)
    log.info("Файл сохранён: %s", file_path)
    return file_path

def cleanup_old_images():
    files = sorted(glob.glob(os.path.join(SAVE_DIR, "*.*")), key=os.path.getmtime)
    if len(files) > MAX_FILES:
        for f in files[:-MAX_FILES]:
            try:
                os.remove(f)
                log.info("Удалён старый файл: %s", f)
            except Exception as e:
                log.warning("Не удалось удалить файл %s: %s", f, e)

# ===== Работа с Like =====
def get_last_downloaded_file():
    files = sorted([f for f in glob.glob(os.path.join(SAVE_DIR, "*.*")) 
                    if os.path.isfile(f)], key=os.path.getmtime)
    return files[-1] if files else None

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
        rr = session.get(add_url, headers={"Referer": image_page_url})
        if rr.status_code == 200:
            log.info("Изображение добавлено в избранное на сайте: %s", filename)
        else:
            log.warning("Ошибка добавления в избранное: %s", rr.status_code)
    else:
        log.warning("Не найден элемент для добавления в избранное на странице.")

# ===== Основной запуск =====
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

    try:
        max_pages = get_max_pages(session)
        log.info(f"Обнаружено максимальное количество страниц: {max_pages}")
    except ValueError as e:
        log.error(f"Ошибка при определении пагинации: {e}")
        return

    for attempt in range(2):
        try:
            page_url = get_random_wallpaper_page_url(max_pages)
            log.info("Выбрана страница раздела: %s", page_url)
            r = session.get(page_url, timeout=15)
            if r.status_code != 200:
                log.warning("Страница вернула статус %s", r.status_code)
                continue
            links = collect_wallpaper_links(r.text)
            if not links:
                log.warning("На странице нет обоев, пробуем другую страницу")
                continue
            abs_links = [urljoin(SECTION_BASE_URL, href) for href in links]
            image_page_url = random.choice(abs_links)
            log.info("Выбрана страница изображения: %s", image_page_url)
            final_url = find_download_href_on_image_page(session, image_page_url)
            if not final_url:
                log.warning("Не удалось найти ссылку на скачивание, пробуем другую страницу")
                continue
            content = download_final_image(session, final_url)
            if not content:
                log.warning("Не удалось скачать картинку, пробуем другую страницу")
                continue
            saved_path = save_image_and_get_path(final_url, content)
            cleanup_old_images()
            set_wallpaper(saved_path)
            return
        except Exception as e:
            log.error("Ошибка при попытке скачивания: %s", e)
    log.error("Не удалось найти и скачать изображение после нескольких попыток.")

if __name__ == "__main__":
    main()