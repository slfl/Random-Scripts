"""
Диагностика входа GoodFon.
Читает config.ini рядом с собой, открывает форму входа на com и ru,
печатает реальные поля формы и результат POST-запроса.
Пароль в выводе маскируется. Ответы сохраняются в файлы для анализа.

Запуск:  py diag_login.py
"""

import os
import sys
import configparser
import requests
from bs4 import BeautifulSoup
from urllib.parse import urljoin

if getattr(sys, "frozen", False):
    BASE_DIR = os.path.dirname(sys.executable)
else:
    BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_FILE = os.path.join(BASE_DIR, "config.ini")

cfg = configparser.ConfigParser(interpolation=None)
cfg.read(CONFIG_FILE, encoding="utf-8")

LOGIN = cfg["auth"]["login"].strip()
PASSWORD = cfg["auth"]["password"].strip()

USER_AGENT = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
              "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36")

DOMAINS = ["https://www.goodfon.com", "https://www.goodfon.ru"]


def line(c="-"):
    print(c * 70)


def inspect_domain(base: str):
    line("=")
    print(f"ДОМЕН: {base}")
    line("=")

    login_url = f"{base}/auth/signin/"
    s = requests.Session()
    s.headers.update({"User-Agent": USER_AGENT})

    # 1) GET страницы входа
    try:
        r = s.get(login_url, timeout=20)
    except Exception as e:
        print(f"  GET {login_url} -> ОШИБКА: {e}")
        return
    print(f"  GET {login_url} -> статус {r.status_code}, размер {len(r.text)}")

    # сохраняем страницу входа
    page_path = os.path.join(BASE_DIR, f"diag_signin_{'ru' if 'ru' in base else 'com'}.html")
    with open(page_path, "w", encoding="utf-8") as f:
        f.write(r.text)
    print(f"  Страница входа сохранена: {page_path}")

    soup = BeautifulSoup(r.text, "html.parser")

    # 2) Ищем форму с полем пароля
    forms = soup.find_all("form")
    print(f"  Найдено форм на странице: {len(forms)}")
    target_form = None
    for i, form in enumerate(forms):
        has_pw = form.find("input", {"type": "password"}) is not None
        action = form.get("action", "(нет action)")
        method = (form.get("method") or "get").upper()
        print(f"    форма #{i}: method={method}, action={action}, "
              f"поле пароля={'да' if has_pw else 'нет'}")
        if has_pw and target_form is None:
            target_form = form

    if target_form is None:
        print("  !! Форма с полем пароля НЕ найдена — возможно, вход через JS/капчу.")
        # всё равно покажем все input на странице
        target_form = soup

    # 3) Печатаем все поля формы
    print("  Поля формы (input):")
    for inp in target_form.find_all("input"):
        name = inp.get("name", "(без имени)")
        itype = inp.get("type", "text")
        value = inp.get("value", "")
        if itype == "password" or name == "password":
            value = "***"
        elif name and "csrf" in name.lower() and value:
            value = value[:8] + "...(обрезано)"
        print(f"    name={name!r:30} type={itype!r:12} value={value!r}")

    # 4) Пробуем POST по текущей логике скрипта
    csrf_inp = soup.find("input", {"name": "csrfmiddlewaretoken"})
    token = csrf_inp["value"] if csrf_inp and csrf_inp.get("value") else ""
    payload = {"csrfmiddlewaretoken": token, "login": LOGIN, "password": PASSWORD}
    headers = {"Referer": login_url}
    if token:
        headers["X-CSRFToken"] = token

    print("  Пробуем POST (поля: csrfmiddlewaretoken, login, password)...")
    try:
        resp = s.post(login_url, data=payload, headers=headers, allow_redirects=True, timeout=20)
    except Exception as e:
        print(f"    POST -> ОШИБКА: {e}")
        return

    print(f"    POST статус: {resp.status_code}")
    print(f"    Итоговый URL после редиректов: {resp.url}")
    print(f"    'Incorrect password' в ответе: {'да' if 'Incorrect password' in resp.text else 'нет'}")
    print(f"    'auth/logout' в ответе (признак входа): {'да' if 'auth/logout' in resp.text else 'нет'}")
    print(f"    cookie после POST: {[c.name for c in s.cookies]}")

    resp_path = os.path.join(BASE_DIR, f"diag_post_{'ru' if 'ru' in base else 'com'}.html")
    with open(resp_path, "w", encoding="utf-8") as f:
        f.write(resp.text)
    print(f"    Ответ на POST сохранён: {resp_path}")

    # 5) Проверка: реально ли вошли (запрос раздела)
    try:
        check = s.get(f"{base}/", timeout=20)
        logged = "auth/logout" in check.text
        print(f"    Проверка главной: 'auth/logout' = {'да (вход выполнен)' if logged else 'нет (вход НЕ выполнен)'}")
    except Exception as e:
        print(f"    Проверка главной -> ОШИБКА: {e}")


def main():
    print(f"Конфиг: {CONFIG_FILE}")
    print(f"Логин: {LOGIN}")
    print(f"Пароль: {'(задан)' if PASSWORD else '(пусто!)'}")
    for base in DOMAINS:
        inspect_domain(base)
    line("=")
    print("Готово. Пришлите вывод выше и, при необходимости, файлы diag_signin_*.html")


if __name__ == "__main__":
    main()
