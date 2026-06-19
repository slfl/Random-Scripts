package main

import (
	"context"
	"encoding/base64"
	"errors"
	"fmt"
	"io"
	"math/rand"
	"net/http"
	"net/http/cookiejar"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unicode/utf16"
	"unsafe"
)

// ====== Глобальные настройки (из config.ini) ======

var (
	cfgLogin      string
	cfgPassword   string
	cfgResolution string
	cfgTheme      string
	cfgSaveDir    string
	cfgMaxFiles   int
	cfgLikeEveryN int
	cfgMaxAttempt int
	cfgNotify     bool
	cfgDomain     string
	cfgSessCom    string
	cfgSessRu     string

	activeBase     string
	likeDir        string
	sectionBaseURL string
	loginURL       string
	configPath     string
)

const (
	userAgent    = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36"
	quotaMarker  = "исчерпали возможное количество скачанных"
	quotaMarker2 = "download_limit"
	httpTimeout  = 30 * time.Second
	pageTimeout  = 15 * time.Second
	probeTimeout = 8 * time.Second
)

// errQuota — специальная ошибка превышения суточного лимита.
var errQuota = fmt.Errorf("download limit reached")

// errBadCreds — отказ авторизации из-за неверных логина/пароля (повторять бессмысленно).
var errBadCreds = errors.New("неверный логин или пароль")

// ====== Логирование ======

var logOut io.Writer = os.Stdout

func logf(level, format string, a ...interface{}) {
	ts := time.Now().Format("2006-01-02 15:04:05")
	fmt.Fprintf(logOut, "%s %s: %s\n", ts, level, fmt.Sprintf(format, a...))
}
func logInfo(f string, a ...interface{})  { logf("INFO", f, a...) }
func logWarn(f string, a ...interface{})  { logf("WARNING", f, a...) }
func logError(f string, a ...interface{}) { logf("ERROR", f, a...) }

// ====== Конфиг (INI, без внешних библиотек) ======

func exeDir() string {
	exe, err := os.Executable()
	if err != nil {
		wd, _ := os.Getwd()
		return wd
	}
	return filepath.Dir(exe)
}

func loadConfig() error {
	configPath = filepath.Join(exeDir(), "config.ini")
	data, err := os.ReadFile(configPath)
	if err != nil {
		return fmt.Errorf("файл конфигурации не найден: %s", configPath)
	}
	// Убираем BOM, если редактор его добавил.
	data = []byte(strings.TrimPrefix(string(data), "\ufeff"))

	sections := map[string]map[string]string{}
	cur := ""
	for _, line := range strings.Split(string(data), "\n") {
		t := strings.TrimSpace(line)
		if t == "" || strings.HasPrefix(t, ";") || strings.HasPrefix(t, "#") {
			continue
		}
		if strings.HasPrefix(t, "[") && strings.HasSuffix(t, "]") {
			cur = strings.ToLower(strings.TrimSpace(t[1 : len(t)-1]))
			sections[cur] = map[string]string{}
			continue
		}
		if eq := strings.Index(t, "="); eq >= 0 && cur != "" {
			k := strings.ToLower(strings.TrimSpace(t[:eq]))
			v := strings.TrimSpace(t[eq+1:])
			sections[cur][k] = v
		}
	}

	get := func(sec, key string) string {
		if m, ok := sections[sec]; ok {
			return m[key]
		}
		return ""
	}
	atoiOr := func(s string, def int) int {
		if n, err := strconv.Atoi(strings.TrimSpace(s)); err == nil {
			return n
		}
		return def
	}

	cfgLogin = get("auth", "login")
	cfgPassword = get("auth", "password")
	cfgResolution = get("settings", "resolution")
	cfgTheme = get("settings", "theme")
	cfgSaveDir = get("settings", "save_dir")
	cfgMaxFiles = atoiOr(get("settings", "max_files"), 10)
	cfgLikeEveryN = atoiOr(get("settings", "like_every_n"), 10)
	cfgMaxAttempt = atoiOr(get("settings", "max_attempts"), 3)
	cfgNotify = strings.EqualFold(strings.TrimSpace(get("settings", "notify")), "true")
	cfgDomain = strings.ToLower(strings.TrimSpace(get("settings", "domain")))
	cfgSessCom = strings.TrimSpace(get("auth", "session_com"))
	cfgSessRu = strings.TrimSpace(get("auth", "session_ru"))

	if cfgSaveDir == "" || cfgTheme == "" {
		return fmt.Errorf("в config.ini не заданы save_dir или theme")
	}

	likeDir = filepath.Join(cfgSaveDir, "Like", cfgTheme)
	initDomain("https://www.goodfon.com") // значение по умолчанию, переопределяется в main
	return nil
}

// initDomain переключает все URL на указанный домен (com или ru).
func initDomain(base string) {
	activeBase = base
	sectionBaseURL = fmt.Sprintf("%s/%s/", base, cfgTheme)
	loginURL = base + "/auth/signin/"
}

// domainCandidates возвращает домены в порядке предпочтения (второй — запасной).
func domainCandidates() []string {
	com := "https://www.goodfon.com"
	ru := "https://www.goodfon.ru"
	switch cfgDomain {
	case "ru":
		return []string{ru, com}
	case "com":
		return []string{com, ru}
	default:
		return []string{com, ru}
	}
}

func readCounter() int {
	data, err := os.ReadFile(configPath)
	if err != nil {
		return 0
	}
	re := regexp.MustCompile(`(?mi)^\s*counter\s*=\s*(\d+)`)
	if m := re.FindStringSubmatch(string(data)); m != nil {
		n, _ := strconv.Atoi(m[1])
		return n
	}
	return 0
}

func writeCounter(value int) {
	data, err := os.ReadFile(configPath)
	if err != nil {
		return
	}
	text := string(data)
	re := regexp.MustCompile(`(?mi)^(\s*counter\s*=\s*)\d+`)
	if re.MatchString(text) {
		text = re.ReplaceAllString(text, "${1}"+strconv.Itoa(value))
	} else {
		// секции [state] нет — добавляем
		if !strings.Contains(strings.ToLower(text), "[state]") {
			text = strings.TrimRight(text, "\n") + "\n\n[state]\n"
		}
		text = strings.TrimRight(text, "\n") + "\ncounter = " + strconv.Itoa(value) + "\n"
	}
	_ = os.WriteFile(configPath, []byte(text), 0644)
}

// ====== Уведомления в трей (через встроенный в Windows toast) ======

func notify(title, msg string) {
	if !cfgNotify {
		return
	}
	// Экранируем для XML (имена файлов и заголовки)
	escXML := func(s string) string {
		s = strings.ReplaceAll(s, "&", "&amp;")
		s = strings.ReplaceAll(s, "<", "&lt;")
		s = strings.ReplaceAll(s, ">", "&gt;")
		s = strings.ReplaceAll(s, "\"", "&quot;")
		return s
	}

	script := `$ErrorActionPreference='SilentlyContinue'
$appId='GoodFon'
$reg="HKCU:\Software\Classes\AppUserModelId\$appId"
if(-not(Test-Path $reg)){New-Item -Path $reg -Force|Out-Null}
New-ItemProperty -Path $reg -Name DisplayName -Value 'GoodFon' -PropertyType String -Force|Out-Null
[Windows.UI.Notifications.ToastNotificationManager,Windows.UI.Notifications,ContentType=WindowsRuntime]>$null
[Windows.Data.Xml.Dom.XmlDocument,Windows.Data.Xml.Dom,ContentType=WindowsRuntime]>$null
$xmlString=@"
<toast>
  <visual>
    <binding template="ToastGeneric">
      <text>` + escXML(title) + `</text>
      <text>` + escXML(msg) + `</text>
    </binding>
  </visual>
</toast>
"@
$xml=New-Object Windows.Data.Xml.Dom.XmlDocument
$xml.LoadXml($xmlString)
$toast=[Windows.UI.Notifications.ToastNotification]::new($xml)
[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($appId).Show($toast)`

	// Кодируем скрипт в UTF-16LE base64 и передаём через -EncodedCommand:
	// без временных файлов, без BOM, корректная кириллица.
	u16 := utf16.Encode([]rune(script))
	raw := make([]byte, len(u16)*2)
	for i, c := range u16 {
		raw[i*2] = byte(c)
		raw[i*2+1] = byte(c >> 8)
	}
	encoded := base64.StdEncoding.EncodeToString(raw)
	cmd := exec.Command("powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-EncodedCommand", encoded)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	if err := cmd.Run(); err != nil {
		logWarn("Не удалось показать уведомление: %v", err)
	}
}

// ====== Установка обоев через user32.dll ======

var (
	user32              = syscall.NewLazyDLL("user32.dll")
	procSystemParamInfo = user32.NewProc("SystemParametersInfoW")

	kernel32          = syscall.NewLazyDLL("kernel32.dll")
	procAttachConsole = kernel32.NewProc("AttachConsole")
)

const attachParentProcess = ^uintptr(0) // (DWORD)-1

// parseArgs разбирает аргументы: действие (update/like/unlike) и флаг -logfile.
// Принимает как "like", так и "-like"; порядок не важен.
func parseArgs() (action string, logFile bool) {
	action = "update"
	actionSet := false
	for _, a := range os.Args[1:] {
		al := strings.ToLower(strings.TrimPrefix(strings.TrimPrefix(a, "-"), "-"))
		switch al {
		case "logfile", "log":
			logFile = true
		case "like", "unlike", "update":
			if !actionSet {
				action = al
				actionSet = true
			}
		}
	}
	return
}

// setupOutput направляет логи: в консоль терминала (если запущено из него),
// иначе — в файл goodfon.log при наличии флага -logfile, иначе никуда.
func setupOutput(logFile bool) {
	if r, _, _ := procAttachConsole.Call(attachParentProcess); r != 0 {
		if f, err := os.OpenFile("CONOUT$", os.O_WRONLY, 0); err == nil {
			logOut = f
			return
		}
	}
	if !logFile {
		logOut = io.Discard
		return
	}
	logPath := filepath.Join(exeDir(), "goodfon.log")
	// Простой ограничитель: если файл перерос 1 МБ — обнуляем перед записью.
	if fi, err := os.Stat(logPath); err == nil && fi.Size() > 1<<20 {
		_ = os.Truncate(logPath, 0)
	}
	if f, err := os.OpenFile(logPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644); err == nil {
		logOut = f
	} else {
		logOut = io.Discard
	}
}

const (
	spiSetDeskWallpaper = 0x0014
	spiGetDeskWallpaper = 0x0073
	spifUpdateINIFile   = 0x01
	spifSendChange      = 0x02
)

func setWallpaper(path string) error {
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return err
	}
	r, _, e := procSystemParamInfo.Call(
		spiSetDeskWallpaper, 0,
		uintptr(unsafe.Pointer(p)),
		spifUpdateINIFile|spifSendChange,
	)
	if r == 0 {
		return e
	}
	logInfo("Обои выставлены: %s", path)
	return nil
}

func getCurrentWallpaper() string {
	buf := make([]uint16, 520)
	procSystemParamInfo.Call(
		spiGetDeskWallpaper,
		uintptr(len(buf)),
		uintptr(unsafe.Pointer(&buf[0])),
		0,
	)
	return syscall.UTF16ToString(buf)
}

// ====== HTTP-клиент ======

func newClient() *http.Client {
	jar, _ := cookiejar.New(nil)
	return &http.Client{Jar: jar, Timeout: httpTimeout}
}

func setBrowserHeaders(req *http.Request) {
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8")
	req.Header.Set("Accept-Language", "ru-RU,ru;q=0.9,en-US;q=0.8,en;q=0.7")
	req.Header.Set("Upgrade-Insecure-Requests", "1")
	req.Header.Set("Sec-Fetch-Dest", "document")
	req.Header.Set("Sec-Fetch-Mode", "navigate")
	req.Header.Set("Sec-Fetch-Site", "none")
	req.Header.Set("Sec-Fetch-User", "?1")
}

func httpGet(c *http.Client, u, referer string) (string, int, error) {
	ctx, cancel := context.WithTimeout(context.Background(), pageTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, "GET", u, nil)
	if err != nil {
		return "", 0, err
	}
	setBrowserHeaders(req)
	if referer != "" {
		req.Header.Set("Referer", referer)
	}
	resp, err := c.Do(req)
	if err != nil {
		return "", 0, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return "", resp.StatusCode, err
	}
	return string(body), resp.StatusCode, nil
}

func httpGetBytes(c *http.Client, u, referer string) ([]byte, string, int, error) {
	ctx, cancel := context.WithTimeout(context.Background(), pageTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, "GET", u, nil)
	if err != nil {
		return nil, "", 0, err
	}
	setBrowserHeaders(req)
	if referer != "" {
		req.Header.Set("Referer", referer)
	}
	resp, err := c.Do(req)
	if err != nil {
		return nil, "", 0, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	return body, resp.Header.Get("Content-Type"), resp.StatusCode, err
}

// ====== Авторизация ======

var reCSRF = regexp.MustCompile(`(?s)<input[^>]*csrfmiddlewaretoken[^>]*>`)
var reValue = regexp.MustCompile(`value=["']?([^"'\s>]+)`)

func login(c *http.Client) error {
	html, _, err := httpGet(c, loginURL, "")
	if err != nil {
		return err
	}
	token := ""
	if tag := reCSRF.FindString(html); tag != "" {
		if m := reValue.FindStringSubmatch(tag); m != nil {
			token = m[1]
		}
	}

	form := url.Values{}
	form.Set("csrfmiddlewaretoken", token)
	form.Set("login", cfgLogin)
	form.Set("password", cfgPassword)

	req, _ := http.NewRequest("POST", loginURL, strings.NewReader(form.Encode()))
	setBrowserHeaders(req)
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("Referer", loginURL)
	req.Header.Set("Origin", activeBase)
	req.Header.Set("X-Requested-With", "XMLHttpRequest")
	if token != "" {
		req.Header.Set("X-CSRFToken", token)
	}
	resp, err := c.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	text := strings.ToLower(string(body))
	hasSession := false
	if u, e := url.Parse(loginURL); e == nil {
		for _, ck := range c.Jar.Cookies(u) {
			if ck.Name == "sessionid" && ck.Value != "" {
				hasSession = true
			}
		}
	}
	jsonOK := strings.Contains(text, `"success"`) ||
		strings.Contains(text, `result": "ok`) ||
		strings.Contains(text, `result":"ok`)
	failMark := strings.Contains(text, "incorrect password") ||
		strings.Contains(text, `"error"`) ||
		strings.Contains(text, `"fail"`) ||
		strings.Contains(text, "не угадали")
	if resp.StatusCode >= 400 || failMark || !(jsonOK || hasSession) {
		return errBadCreds
	}
	logInfo("Авторизация успешна")
	return nil
}

// loginWithRetry повторяет вход при сетевых ошибках/таймаутах,
// но не повторяет при неверных логине/пароле.
func loginWithRetry(c *http.Client) error {
	var err error
	for try := 1; try <= 2; try++ {
		err = login(c)
		if err == nil || errors.Is(err, errBadCreds) {
			return err
		}
		logWarn("Сетевая ошибка входа (попытка %d): %v", try, err)
	}
	return err
}

func isSiteAvailable() bool {
	c := &http.Client{Timeout: probeTimeout}
	req, _ := http.NewRequest("GET", sectionBaseURL, nil)
	req.Header.Set("User-Agent", userAgent)
	resp, err := c.Do(req)
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	return resp.StatusCode < 500
}

// ====== Кэш сессии (cookie) ======

// upsertConfigKey заменяет значение ключа (или вставляет его в нужную секцию),
// сохраняя остальное содержимое файла.
func upsertConfigKey(text, section, key, value string) string {
	re := regexp.MustCompile(`(?mi)^([ \t]*` + regexp.QuoteMeta(key) + `[ \t]*=).*$`)
	if re.MatchString(text) {
		return re.ReplaceAllString(text, "$1 "+value)
	}
	secRe := regexp.MustCompile(`(?mi)^\[` + regexp.QuoteMeta(section) + `\][^\n]*\n`)
	loc := secRe.FindStringIndex(text)
	if loc == nil {
		return strings.TrimRight(text, "\n") + "\n\n[" + section + "]\n" + key + " = " + value + "\n"
	}
	return text[:loc[1]] + key + " = " + value + "\n" + text[loc[1]:]
}

func cacheKeyForBase(base string) string {
	if strings.Contains(base, "goodfon.ru") {
		return "session_ru"
	}
	return "session_com"
}

func cacheForBase(base string) string {
	if strings.Contains(base, "goodfon.ru") {
		return cfgSessRu
	}
	return cfgSessCom
}

func saveSessionCache(c *http.Client, base string) {
	u, err := url.Parse(base)
	if err != nil {
		return
	}
	var parts []string
	for _, ck := range c.Jar.Cookies(u) {
		parts = append(parts, ck.Name+"="+ck.Value)
	}
	data, err := os.ReadFile(configPath)
	if err != nil {
		return
	}
	text := upsertConfigKey(string(data), "auth", cacheKeyForBase(base), strings.Join(parts, "; "))
	if err := os.WriteFile(configPath, []byte(text), 0644); err == nil {
		logInfo("Кэш сессии сохранён для домена %s", base)
	}
}

func buildSessionFromCache(cookieStr, base string) *http.Client {
	jar, _ := cookiejar.New(nil)
	u, _ := url.Parse(base)
	var cks []*http.Cookie
	for _, part := range strings.Split(cookieStr, ";") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		i := strings.Index(part, "=")
		if i < 0 {
			continue
		}
		cks = append(cks, &http.Cookie{
			Name:  strings.TrimSpace(part[:i]),
			Value: strings.TrimSpace(part[i+1:]),
		})
	}
	jar.SetCookies(u, cks)
	return &http.Client{Jar: jar, Timeout: httpTimeout}
}

// ====== Пагинация и сбор ссылок ======

var reIndexPage = regexp.MustCompile(`index-(\d+)\.html`)

func getMaxPages(c *http.Client) (int, error) {
	html, status, err := httpGet(c, sectionBaseURL, "")
	if err != nil {
		return 0, err
	}
	if status != 200 {
		return 0, fmt.Errorf("первая страница вернула статус %d", status)
	}
	max := 1
	for _, m := range reIndexPage.FindAllStringSubmatch(html, -1) {
		if n, e := strconv.Atoi(m[1]); e == nil && n > max {
			max = n
		}
	}
	if max <= 1 {
		return 0, fmt.Errorf("пагинация не найдена")
	}
	return max, nil
}

func randomPageURL(maxPages int) string {
	n := rand.Intn(maxPages) + 1
	if n == 1 {
		return sectionBaseURL
	}
	return fmt.Sprintf("%sindex-%d.html", sectionBaseURL, n)
}

var reWallpaperLink = regexp.MustCompile(`href=["']?([^"'\s>]*?/wallpaper-[^"'\s>]*?\.html)`)

func collectWallpaperLinks(html string) []string {
	seen := map[string]bool{}
	var out []string
	for _, m := range reWallpaperLink.FindAllStringSubmatch(html, -1) {
		href := m[1]
		if strings.Contains(href, "wallpaper-download") {
			continue
		}
		if !seen[href] {
			seen[href] = true
			out = append(out, href)
		}
	}
	return out
}

// ====== Поиск и скачивание картинки ======

func findDownloadURL(c *http.Client, imagePageURL string) (string, error) {
	html, status, err := httpGet(c, imagePageURL, sectionBaseURL)
	if err != nil {
		return "", err
	}
	if status != 200 {
		logWarn("Страница изображения вернула статус %d: %s", status, imagePageURL)
		return "", nil
	}

	// Строго ищем ссылку с нужным разрешением (с дефисом в конце).
	pat := regexp.MustCompile(`href=["']?([^"'\s>]*wallpaper-download-` + regexp.QuoteMeta(cfgResolution) + `-[^"'\s>]*\.html)`)
	m := pat.FindStringSubmatch(html)
	if m == nil {
		logInfo("Разрешение %s недоступно для этой картинки, пропускаем.", cfgResolution)
		return "", nil
	}
	downloadPageURL := resolveURL(imagePageURL, m[1])

	dhtml, dstatus, err := httpGet(c, downloadPageURL, imagePageURL)
	if err != nil {
		return "", err
	}
	if dstatus != 200 {
		return "", nil
	}

	if strings.Contains(dhtml, quotaMarker) || strings.Contains(dhtml, quotaMarker2) {
		logWarn("Превышен суточный лимит скачиваний на сайте.")
		return "", errQuota
	}

	// Ссылка на оригинал в <a class="js-download_img" href="...">
	reDl := regexp.MustCompile(`(<a[^>]*js-download_img[^>]*>)`)
	if tag := reDl.FindString(dhtml); tag != "" {
		if hm := regexp.MustCompile(`href=["']?([^"'\s>]+)`).FindStringSubmatch(tag); hm != nil {
			if strings.Contains(hm[1], "img.goodfon") {
				return hm[1], nil
			}
		}
	}
	// Запасной вариант — img с img.goodfon
	if im := regexp.MustCompile(`<img[^>]*src=["']?([^"'\s>]*img\.goodfon[^"'\s>]*)`).FindStringSubmatch(dhtml); im != nil {
		return im[1], nil
	}

	logWarn("Ссылка на картинку не найдена на странице загрузки: %s", downloadPageURL)
	return "", nil
}

func downloadImage(c *http.Client, finalURL string) []byte {
	tryURLs := []string{finalURL}
	if strings.Contains(finalURL, "img.goodfon.com") {
		tryURLs = append(tryURLs, strings.Replace(finalURL, "img.goodfon.com", "img.goodfon.ru", 1))
	} else if strings.Contains(finalURL, "img.goodfon.ru") {
		tryURLs = append(tryURLs, strings.Replace(finalURL, "img.goodfon.ru", "img.goodfon.com", 1))
	}

	for _, u := range tryURLs {
		body, ctype, status, err := httpGetBytes(c, u, "")
		if err != nil {
			logWarn("Не удалось скачать с %s: %v", u, err)
			continue
		}
		if status == 200 && strings.Contains(ctype, "image") {
			if u != finalURL {
				logInfo("Использован резервный img домен: %s", u)
			}
			return body
		}
	}
	return nil
}

func saveImage(finalURL string, content []byte) (string, error) {
	if err := os.MkdirAll(cfgSaveDir, 0755); err != nil {
		return "", err
	}
	u, _ := url.Parse(finalURL)
	name := filepath.Base(u.Path)
	if i := strings.Index(name, "?"); i >= 0 {
		name = name[:i]
	}
	name = strings.ReplaceAll(name, " ", "_")
	path := filepath.Join(cfgSaveDir, name)
	if err := os.WriteFile(path, content, 0644); err != nil {
		return "", err
	}
	logInfo("Файл сохранён: %s", path)
	return path, nil
}

func cleanupOldImages() {
	files := filesInDir(cfgSaveDir)
	sort.Slice(files, func(i, j int) bool { return files[i].mod.Before(files[j].mod) })
	if len(files) > cfgMaxFiles {
		for _, f := range files[:len(files)-cfgMaxFiles] {
			if err := os.Remove(f.path); err != nil {
				logWarn("Не удалось удалить файл %s: %v", f.path, err)
			} else {
				logInfo("Удалён старый файл: %s", f.path)
			}
		}
	}
}

// ====== Вспомогательные функции по файлам ======

type fileInfo struct {
	path string
	mod  time.Time
}

func filesInDir(dir string) []fileInfo {
	var out []fileInfo
	entries, err := os.ReadDir(dir)
	if err != nil {
		return out
	}
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		out = append(out, fileInfo{filepath.Join(dir, e.Name()), info.ModTime()})
	}
	return out
}

func lastInDir(dir string) string {
	files := filesInDir(dir)
	if len(files) == 0 {
		return ""
	}
	sort.Slice(files, func(i, j int) bool { return files[i].mod.Before(files[j].mod) })
	return files[len(files)-1].path
}

func randomFileExcluding(dir, exclude string) string {
	files := filesInDir(dir)
	if len(files) == 0 {
		return ""
	}
	var cand []string
	for _, f := range files {
		if !strings.EqualFold(f.path, exclude) {
			cand = append(cand, f.path)
		}
	}
	if len(cand) == 0 {
		return files[rand.Intn(len(files))].path
	}
	return cand[rand.Intn(len(cand))]
}

// ====== Like / Unlike ======

func resolveURL(base, ref string) string {
	b, err := url.Parse(base)
	if err != nil {
		return ref
	}
	r, err := url.Parse(ref)
	if err != nil {
		return ref
	}
	return b.ResolveReference(r).String()
}

var reAddURL = regexp.MustCompile(`data-add=["']?([^"'\s>]+)`)
var reDelURL = regexp.MustCompile(`data-del=["']?([^"'\s>]+)`)

func getFavoriteIDs(c *http.Client, imagePageURL string) (addURL, delURL string) {
	html, status, err := httpGet(c, imagePageURL, "")
	if err != nil || status != 200 {
		logWarn("Не удалось открыть страницу картинки: %s", imagePageURL)
		return "", ""
	}
	// Берём блок js-favorite
	favRe := regexp.MustCompile(`(<a[^>]*js-favorite[^>]*>)`)
	tag := favRe.FindString(html)
	if tag == "" {
		logWarn("Блок избранного не найден на странице: %s", imagePageURL)
		return "", ""
	}
	if m := reAddURL.FindStringSubmatch(tag); m != nil {
		addURL = m[1]
	}
	if m := reDelURL.FindStringSubmatch(tag); m != nil {
		delURL = m[1]
	}
	return addURL, delURL
}

func nameOnly(path string) string {
	b := filepath.Base(path)
	return strings.TrimSuffix(b, filepath.Ext(b))
}

func addToLike(c *http.Client) {
	last := lastInDir(cfgSaveDir)
	if last == "" {
		logError("Нет последнего скачанного файла.")
		return
	}
	name := filepath.Base(last)
	_ = os.MkdirAll(likeDir, 0755)
	dest := filepath.Join(likeDir, name)
	if _, err := os.Stat(dest); os.IsNotExist(err) {
		if err := copyFile(last, dest); err != nil {
			logError("Не удалось скопировать в Like: %v", err)
			return
		}
		logInfo("Изображение скопировано в папку Like/%s: %s", cfgTheme, dest)
	} else {
		logInfo("Изображение уже есть в папке Like/%s", cfgTheme)
	}

	// Переставляем обои на копию из Like, чтобы unlike работал корректно
	_ = setWallpaper(dest)
	logInfo("Обои переключены на копию из Like/%s", cfgTheme)

	imagePageURL := fmt.Sprintf("%s/%s/wallpaper-%s.html", activeBase, cfgTheme, nameOnly(name))
	addURL, _ := getFavoriteIDs(c, imagePageURL)
	if addURL != "" {
		_, status, _ := httpGet(c, resolveURL(activeBase, addURL), imagePageURL)
		if status == 200 {
			logInfo("Изображение добавлено в избранное на сайте: %s", name)
			notify("Добавлено в избранное", name)
		} else {
			logWarn("Ошибка добавления в избранное: статус %d", status)
		}
	} else {
		logWarn("Не найден элемент для добавления в избранное.")
	}
}

func removeFromLike(c *http.Client) bool {
	current := getCurrentWallpaper()
	if current == "" {
		logError("Не удалось определить текущие обои.")
		return false
	}
	if !strings.HasPrefix(strings.ToLower(current), strings.ToLower(likeDir)) {
		logError("Текущие обои не из папки Like/%s: %s", cfgTheme, current)
		logError("Unlike работает только когда установлена картинка из избранного.")
		notify("GoodFon: ошибка", "Текущие обои не из папки избранного.")
		return false
	}

	name := filepath.Base(current)
	logInfo("Удаляем из избранного: %s", name)

	imagePageURL := fmt.Sprintf("%s/%s/wallpaper-%s.html", activeBase, cfgTheme, nameOnly(name))
	_, delURL := getFavoriteIDs(c, imagePageURL)
	if delURL != "" {
		_, status, _ := httpGet(c, resolveURL(activeBase, delURL), imagePageURL)
		if status == 200 {
			logInfo("Изображение удалено из избранного на сайте: %s", name)
		} else {
			logWarn("Ошибка удаления из избранного: статус %d", status)
		}
	} else {
		logWarn("Не найден элемент для удаления из избранного.")
	}

	if err := os.Remove(current); err != nil {
		logError("Не удалось удалить файл %s: %v", current, err)
		return false
	}
	logInfo("Файл удалён из папки Like/%s: %s", cfgTheme, name)
	notify("Удалено из избранного", name)
	return true
}

func copyFile(src, dst string) error {
	data, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	return os.WriteFile(dst, data, 0644)
}

// ====== Локальный fallback ======

func setWallpaperFromLike() bool {
	files := filesInDir(likeDir)
	if len(files) == 0 {
		logWarn("Папка Like/%s пуста, загружаем с сайта", cfgTheme)
		return false
	}
	chosen := randomFileExcluding(likeDir, getCurrentWallpaper())
	_ = setWallpaper(chosen)
	logInfo("Обои из папки Like/%s: %s", cfgTheme, chosen)
	notify("Обои обновлены — из избранного", filepath.Base(chosen))
	return true
}

func fallbackLocal(likeOnly bool) {
	dir := likeDir
	files := filesInDir(likeDir)
	if !likeOnly && len(files) == 0 {
		dir = cfgSaveDir
		files = filesInDir(cfgSaveDir)
	}
	if len(files) == 0 {
		logWarn("Fallback: локальных картинок нет, обои не изменены.")
		notify("GoodFon: ошибка", "Нет доступных картинок.")
		return
	}
	chosen := randomFileExcluding(dir, getCurrentWallpaper())
	logInfo("Fallback: устанавливаем локальную картинку: %s", chosen)
	_ = setWallpaper(chosen)
	if likeOnly {
		notify("Обои обновлены — из избранного", filepath.Base(chosen))
	} else {
		notify("Обои обновлены — локально", filepath.Base(chosen))
	}
}

// ====== main ======

func main() {
	arg, logFile := parseArgs()
	setupOutput(logFile)
	rand.Seed(time.Now().UnixNano())

	if err := loadConfig(); err != nil {
		logError("%v", err)
		return
	}

	// Для каждого домена: если есть кэш cookie — используем напрямую,
	// не обращаясь к эндпоинту логина (сайт может его подвешивать).
	var client *http.Client
	for _, base := range domainCandidates() {
		initDomain(base)

		if cookieStr := cacheForBase(base); cookieStr != "" {
			client = buildSessionFromCache(cookieStr, base)
			logInfo("Используется кэш сессии: %s", base)
			break
		}

		if !isSiteAvailable() {
			logWarn("Домен недоступен: %s", base)
			continue
		}
		c := newClient()
		if err := loginWithRetry(c); err != nil {
			logWarn("Не удалось войти на %s: %v", base, err)
			continue
		}
		client = c
		logInfo("Активный домен: %s", base)
		saveSessionCache(c, base)
		break
	}

	if client == nil {
		logError("Ни один домен недоступен или вход не выполнен.")
		if arg == "like" || arg == "unlike" {
			notify("GoodFon: сайт недоступен", "Операция с избранным невозможна.")
			return
		}
		writeCounter(readCounter() + 1)
		fallbackLocal(false)
		return
	}

	switch arg {
	case "like":
		func() {
			defer func() {
				if r := recover(); r != nil {
					logError("Ошибка при добавлении в избранное: %v", r)
					notify("GoodFon: ошибка", "Не удалось добавить в избранное.")
				}
			}()
			addToLike(client)
		}()
		return
	case "unlike":
		ok := func() bool {
			defer func() {
				if r := recover(); r != nil {
					logError("Ошибка при удалении из избранного: %v", r)
				}
			}()
			return removeFromLike(client)
		}()
		if !ok {
			return
		}
		logInfo("Загружаем новую картинку с сайта после удаления из избранного.")
	}

	// Счётчик: каждый LIKE_EVERY_N-й запуск берём из Like
	counter := readCounter() + 1
	writeCounter(counter)
	logInfo("Запуск #%d (из Like каждые %d)", counter, cfgLikeEveryN)

	if counter >= cfgLikeEveryN {
		writeCounter(0)
		if setWallpaperFromLike() {
			return
		}
		logInfo("Папка Like пуста, продолжаем загрузку с сайта")
	}

	// Загрузка с сайта
	maxPages := 0
	for attempt := 1; attempt <= cfgMaxAttempt; attempt++ {
		logInfo("Попытка %d из %d", attempt, cfgMaxAttempt)

		if maxPages == 0 {
			mp, err := getMaxPages(client)
			if err != nil {
				logWarn("Ошибка пагинации при попытке %d: %v", attempt, err)
				continue
			}
			maxPages = mp
			logInfo("Максимальное количество страниц: %d", maxPages)
		}

		pageURL := randomPageURL(maxPages)
		logInfo("Выбрана страница раздела: %s", pageURL)

		html, status, err := httpGet(client, pageURL, "")
		if err != nil {
			logError("Ошибка при попытке %d: %v", attempt, err)
			continue
		}
		if status != 200 {
			logWarn("Страница вернула статус %d", status)
			continue
		}

		links := collectWallpaperLinks(html)
		if len(links) == 0 {
			logWarn("На странице нет обоев, пробуем другую")
			continue
		}

		imagePageURL := resolveURL(sectionBaseURL, links[rand.Intn(len(links))])
		logInfo("Выбрана страница изображения: %s", imagePageURL)

		finalURL, err := findDownloadURL(client, imagePageURL)
		if err == errQuota {
			logWarn("Суточный лимит скачиваний исчерпан, переходим на локальные картинки.")
			notify("GoodFon: лимит исчерпан", "Загружаем из избранного.")
			fallbackLocal(true)
			return
		}
		if err != nil {
			logError("Ошибка при попытке %d: %v", attempt, err)
			continue
		}
		if finalURL == "" {
			logWarn("Ссылка на скачивание не найдена, пробуем другую")
			continue
		}

		content := downloadImage(client, finalURL)
		if content == nil {
			logWarn("Не удалось скачать картинку, пробуем другую")
			continue
		}

		savedPath, err := saveImage(finalURL, content)
		if err != nil {
			logError("Ошибка сохранения: %v", err)
			continue
		}
		cleanupOldImages()
		_ = setWallpaper(savedPath)
		notify("Обои обновлены — с сайта", filepath.Base(savedPath))
		return
	}

	logError("Не удалось найти и скачать изображение после %d попыток.", cfgMaxAttempt)
	fallbackLocal(false)
}
