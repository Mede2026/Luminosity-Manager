// Fenetre (qui affiche l'interface HTML via WebView2), icone pres de l'horloge et raccourcis clavier.
// L'interface elle-meme est dans src/ui/ (HTML, CSS, JavaScript) ; ici on echange des messages JSON avec elle.
#include "app.h"
#include <dwmapi.h>
#include <wtsapi32.h>
#include <limits.h>
#include <stdio.h>

enum { ID_OPEN = 100, ID_MEASURE, ID_TOGGLE, ID_BRIGHTER, ID_DARKER, ID_READ, ID_QUIT };
enum { TIMER_UPD_FIRST = 1, TIMER_UPD_DAILY, TIMER_LOCATE };

HWND g_hwnd;
WORD g_hotkeys[HK_COUNT];
static const WORD DEFAULT_HOTKEYS[HK_COUNT] = {
    MAKEWORD(VK_UP, HOTKEYF_CONTROL | HOTKEYF_ALT),
    MAKEWORD(VK_DOWN, HOTKEYF_CONTROL | HOTKEYF_ALT),
    MAKEWORD('M', HOTKEYF_CONTROL | HOTKEYF_ALT),
    MAKEWORD('A', HOTKEYF_CONTROL | HOTKEYF_ALT),
    MAKEWORD('L', HOTKEYF_CONTROL | HOTKEYF_ALT),
};
// Economiseur de batterie de Windows (pas dans les en-tetes MinGW)
static const GUID GUID_POWER_SAVING = { 0xe00958c0, 0xc213, 0x4ace, { 0xac, 0x77, 0xfe, 0xcc, 0xed, 0x2e, 0xee, 0xa5 } };

static NOTIFYICONDATAW g_nid;
static HICON g_iconOn, g_iconOff;
static UINT g_taskbarCreated;
static bool g_mica;                     // effet Mica de Windows 11 actif
static bool g_pageReady;                // la page a fini de charger
static int g_hotkeyFailed;              // raccourcis refuses (bit par raccourci)
static LONG g_sentHist = -1, g_sentThumb = -1;
static wchar_t g_pendingPage[16];
static const wchar_t *g_updStatus = L"";
static wchar_t g_notifiedVersion[32];

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------- Theme Windows ----------
static bool IsDarkMode() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz);
    return v == 0;
}

static COLORREF ThemeBg() { return IsDarkMode() ? RGB(32, 32, 32) : RGB(243, 243, 243); }

static void AccentHex(wchar_t *out) {              // couleur d'accentuation choisie dans Windows
    DWORD abgr = 0, sz = sizeof(abgr);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                     NULL, &abgr, &sz) == ERROR_SUCCESS)
        swprintf(out, 8, L"#%02x%02x%02x", (unsigned)(abgr & 0xFF), (unsigned)((abgr >> 8) & 0xFF),
                 (unsigned)((abgr >> 16) & 0xFF));
    else
        wcscpy(out, L"#0067c0");
}

// Numero de version de Windows (22621 = Windows 11 22H2)
static DWORD WindowsBuild() {
    typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
    RtlGetVersionFn fn = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = {};
    v.dwOSVersionInfoSize = sizeof(v);
    return fn && fn(&v) == 0 ? v.dwBuildNumber : 0;
}

static void ApplyWindowTheme() {
    BOOL dark = IsDarkMode();
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    int backdrop = 2;                              // DWMSBT_MAINWINDOW = Mica (Windows 11 22H2+)
    g_mica = WindowsBuild() >= 22621 &&
             SUCCEEDED(DwmSetWindowAttribute(g_hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof(backdrop)));
    if (g_mica) {
        MARGINS m = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(g_hwnd, &m);
    }
    InvalidateRect(g_hwnd, NULL, TRUE);
}

// ---------- Icone pres de l'horloge ----------
static const wchar_t *SourceKey() {
    return g_source == SRC_SENSOR ? L"sensor" : g_source == SRC_CAMERA ? L"camera" : L"sun";
}

static void UpdateTip() {
    wchar_t mes[64];
    if (g_source == SRC_SUN) swprintf(mes, 64, L"Soleil à %+ld°", (long)g_sunElev);
    else swprintf(mes, 64, L"%ls%ld lux", g_source == SRC_CAMERA ? L"≈ " : L"", (long)g_lux);
    if (!g_enabled) swprintf(g_nid.szTip, 128, L"%ls\nDésactivé", APP_NAME);
    else swprintf(g_nid.szTip, 128, L"%ls\n%ls\nÉcran : %ld %%", APP_NAME, mes, g_bright < 0 ? 0L : (long)g_bright);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void UpdateTrayIcon() {
    g_nid.hIcon = g_enabled ? g_iconOn : g_iconOff;
    g_nid.uFlags = NIF_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void Balloon(const wchar_t *text) {
    g_nid.uFlags = NIF_INFO;
    wcscpy(g_nid.szInfoTitle, APP_NAME);
    wcsncpy(g_nid.szInfo, text, 255);
    g_nid.szInfo[255] = 0;
    g_nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ---------- Messages vers la page ----------
static void Post(JsonOut &j) {
    j.Raw(L"}");
    if (j.buf) WebViewPost(j.buf);
    free(j.buf);
}

static void Begin(JsonOut &j, const wchar_t *type) {
    j.Raw(L"{");
    j.KStr(L"type", type);
}

static void SendState() {
    if (!g_pageReady) return;
    JsonOut j;
    Begin(j, L"state");
    j.Bool(L"enabled", g_enabled);
    j.Num(L"bright", g_bright);
    j.Num(L"lux", g_lux);
    j.KStr(L"source", SourceKey());
    j.Num(L"sunElev", g_sunElev);
    j.Num(L"detected", g_detected);
    j.Num(L"offset", g_offset);
    j.Num(L"min", g_min);
    j.Num(L"max", g_max);
    j.Num(L"applied", g_applied);
    j.Num(L"learnNow", g_learnNow);
    j.Num(L"level", g_level / 1000.0);
    j.Key(L"learn");
    j.Raw(L"[");
    for (int i = 0; i < LEARN_POINTS; i++) j.Raw(i ? L",%.1f" : L"%.1f", g_learn[i]);
    j.Raw(L"]");
    j.Bool(L"camLocked", g_camLocked);
    j.Num(L"camLockValue", g_camLockValue == LONG_MIN ? g_camExposure : g_camLockValue);
    j.Num(L"camExposure", g_camExposure == LONG_MIN ? 0 : g_camExposure);
    j.Num(L"camExpMin", g_camExpMin);
    j.Num(L"camExpMax", g_camExpMax);
    j.Num(L"camExpStep", g_camExpStep);
    j.Bool(L"camLogUnits", g_camLogUnits);
    j.Bool(L"camManualOk", g_camManualOk);
    j.Num(L"camClip", g_camClip);
    j.Bool(L"trueTone", g_trueTone);
    j.Num(L"ttStrength", g_ttStrength);
    j.Num(L"ambientK", g_ambientK);
    j.Num(L"displayK", g_displayK);
    j.Num(L"ttSource", g_ttSource);
    j.Bool(L"ttOk", g_ttOk);
    j.Num(L"pauseReason", g_pauseReason);
    j.Bool(L"camBusy", g_camBusy);
    j.Bool(L"onBattery", g_onBattery);
    j.Bool(L"batterySaver", g_batterySaver);
    j.Bool(L"ecoActive", g_ecoActive);
    j.Num(L"batteryMode", g_batteryMode);
    j.Num(L"batteryCut", g_batteryCut);
    j.Bool(L"pauseFullscreen", g_pauseFullscreen);
    j.Bool(L"readMode", g_readMode);
    j.Bool(L"external", g_externalBrightness);
    j.Num(L"ago", g_lastMeasure ? (double)((GetTickCount() - (DWORD)g_lastMeasure) / 1000) : -1);
    j.Num(L"camNext", g_camNext);
    j.Bool(L"useCam", g_useCam);
    j.Num(L"camInterval", g_camInterval);
    j.Num(L"camQuality", g_camQuality);
    j.Num(L"camLevel", g_camLevel * 100 / 255);
    j.Bool(L"startup", StartupEnabled());
    j.Bool(L"updateCheck", g_updateCheck);
    j.Num(L"lat", g_lat / 100.0);
    j.Num(L"lon", g_lon / 100.0);
    EnterCriticalSection(&g_lock);
    j.KStr(L"camUsed", g_camUsed);
    j.KStr(L"profileApp", g_profilePct >= 0 ? g_profileApp : L"");
    LeaveCriticalSection(&g_lock);
    j.Num(L"profilePct", g_profilePct);
    Post(j);
}

// Historique 24 h : un point par minute, -1 = pas de donnee
static void SendHist() {
    if (!g_pageReady) return;
    LONG now = NowMinute(), last = g_histLast;
    JsonOut j;
    Begin(j, L"hist");
    j.Num(L"nowMin", now % 1440);              // minute de la journee (pour placer les heures)
    for (int pass = 0; pass < 2; pass++) {
        j.Key(pass == 0 ? L"light" : L"bright");
        j.Raw(L"[");
        for (int i = 0; i < HIST_LEN; i++) {
            LONG t = now - (HIST_LEN - 1) + i;
            int v = -1;
            if (last && t <= last && t > last - HIST_LEN) {
                BYTE b = pass == 0 ? g_hist[t % HIST_LEN].light : g_hist[t % HIST_LEN].bright;
                if (b <= 100) v = b;
            }
            j.Raw(i ? L",%d" : L"%d", v);
        }
        j.Raw(L"]");
    }
    Post(j);
    g_sentHist = last;
}

// Miniature webcam 160x120 en couleur (RGB), encodee en base64
static void SendThumb() {
    if (!g_pageReady || !g_thumbValid || !g_thumb) return;
    static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int n = THUMB_W * THUMB_H * 3, o = 0;
    wchar_t *enc = (wchar_t *)malloc(((n + 2) / 3 * 4 + 1) * sizeof(wchar_t));
    if (!enc) return;
    for (int i = 0; i < n; i += 3) {
        unsigned v = g_thumb[i] << 16 | (i + 1 < n ? g_thumb[i + 1] << 8 : 0) | (i + 2 < n ? g_thumb[i + 2] : 0);
        enc[o++] = B64[(v >> 18) & 63];
        enc[o++] = B64[(v >> 12) & 63];
        enc[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        enc[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    enc[o] = 0;
    JsonOut j;
    Begin(j, L"thumb");
    j.Num(L"w", THUMB_W);
    j.Num(L"h", THUMB_H);
    j.KStr(L"data", enc);
    free(enc);
    Post(j);
    g_sentThumb = g_lastMeasure;
}

static void SendProfiles() {
    if (!g_pageReady) return;
    JsonOut j;
    Begin(j, L"profiles");
    j.Key(L"list");
    j.Raw(L"[");
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_profileCount; i++) {
        j.Raw(i ? L",{" : L"{");
        j.KStr(L"exe", g_profiles[i].exe);
        j.Num(L"pct", g_profiles[i].pct);
        j.Raw(L"}");
    }
    LeaveCriticalSection(&g_lock);
    j.Raw(L"]");
    Post(j);
}

static void SendCameras() {
    if (!g_pageReady) return;
    static wchar_t names[8][128];
    int n = ListCameras(names, 8);
    JsonOut j;
    Begin(j, L"cameras");
    j.Key(L"list");
    j.Raw(L"[");
    for (int i = 0; i < n; i++) { if (i) j.Raw(L","); j.Str(names[i]); }
    j.Raw(L"]");
    EnterCriticalSection(&g_lock);
    j.KStr(L"choice", g_camChoice);
    LeaveCriticalSection(&g_lock);
    Post(j);
}

static void SendCity(bool ok, bool searched) {
    if (!g_pageReady) return;
    wchar_t name[160];
    EnterCriticalSection(&g_lock);
    if (g_cityResult[0]) wcscpy(name, g_cityResult);
    else if (!RegGetStr(L"City", name, 160)) wcscpy(name, g_lat == 4559 && g_lon == -7344 ? L"Boucherville" : L"Position personnalisée");
    LeaveCriticalSection(&g_lock);
    JsonOut j;
    Begin(j, L"city");
    j.Bool(L"ok", ok);
    j.Bool(L"searched", searched);
    j.KStr(L"name", name);
    j.Num(L"lat", g_lat / 100.0);
    j.Num(L"lon", g_lon / 100.0);
    Post(j);
}

static void SendUpdate() {
    if (!g_pageReady) return;
    JsonOut j;
    Begin(j, L"update");
    j.KStr(L"status", g_updStatus);
    j.KStr(L"version", g_newVersion);
    j.KStr(L"current", APP_VERSION);
    Post(j);
}

static void SendHotkeys() {
    if (!g_pageReady) return;
    JsonOut j;
    Begin(j, L"hotkeys");
    j.Key(L"list");
    j.Raw(L"[");
    for (int i = 0; i < HK_COUNT; i++) {
        BYTE f = HIBYTE(g_hotkeys[i]);
        j.Raw(i ? L",{" : L"{");
        j.Num(L"vk", LOBYTE(g_hotkeys[i]));
        j.Num(L"mods", f & (HOTKEYF_SHIFT | HOTKEYF_CONTROL | HOTKEYF_ALT));   // 1 Maj, 2 Ctrl, 4 Alt
        j.Bool(L"failed", (g_hotkeyFailed >> i) & 1);
        j.Raw(L"}");
    }
    j.Raw(L"]");
    Post(j);
}

static void Notice(const wchar_t *text) {
    if (!g_pageReady) return;
    JsonOut j;
    Begin(j, L"notice");
    j.KStr(L"text", text);
    Post(j);
}

static void SendStats() {
    if (!g_pageReady) return;
    JsonOut j;
    Begin(j, L"stats");
    StatsJson(j);
    Post(j);
}

static void ToggleRead() {
    g_readMode = !g_readMode;
    g_force = 1;                                 // on applique tout de suite
    SetEvent(g_wakeEvent);
    SendState();
}

static void SendInit() {
    wchar_t accent[8];
    AccentHex(accent);
    JsonOut j;
    Begin(j, L"init");
    j.KStr(L"version", APP_VERSION);
    j.Bool(L"mica", g_mica);
    j.KStr(L"accent", accent);
    j.Key(L"curve");                            // la courbe lux -> position, pour le dessin
    j.Raw(L"[");
    for (int i = 0; i < CURVE_POINTS; i++) j.Raw(i ? L",[%g,%g]" : L"[%g,%g]", CURVE[i][0], CURVE[i][1]);
    j.Raw(L"]");
    Post(j);
    SendState();
    SendHist();
    SendThumb();
    SendProfiles();
    SendCameras();
    SendCity(true, false);
    SendUpdate();
    SendHotkeys();
    if (g_pendingPage[0]) {
        JsonOut n;
        Begin(n, L"nav");
        n.KStr(L"page", g_pendingPage);
        Post(n);
        g_pendingPage[0] = 0;
    }
}

// ---------- Raccourcis clavier ----------
void RegisterHotkeys() {
    g_hotkeyFailed = 0;
    for (int i = 0; i < HK_COUNT; i++) {
        UnregisterHotKey(g_hwnd, i + 1);
        WORD hk = g_hotkeys[i];
        if (!LOBYTE(hk)) continue;
        BYTE f = HIBYTE(hk);
        UINT mods = MOD_NOREPEAT | (f & HOTKEYF_CONTROL ? MOD_CONTROL : 0) | (f & HOTKEYF_ALT ? MOD_ALT : 0) |
                    (f & HOTKEYF_SHIFT ? MOD_SHIFT : 0);
        if (!RegisterHotKey(g_hwnd, i + 1, mods, LOBYTE(hk))) g_hotkeyFailed |= 1 << i;
    }
    SendHotkeys();
}

static void UnregisterHotkeys() {
    for (int i = 0; i < HK_COUNT; i++) UnregisterHotKey(g_hwnd, i + 1);
}

// ---------- Actions ----------
static void SetOffset(LONG o) {
    g_offset = Clamp(o, -50, 50);
    RegPut(L"Offset", (DWORD)g_offset);
    SetEvent(g_wakeEvent);
    SendState();
}

static void ToggleEnabled(bool on) {
    g_enabled = on;
    RegPut(L"Enabled", g_enabled);
    if (g_enabled) g_force = 1;                  // on remesure tout de suite en revenant
    UpdateTrayIcon();
    SetEvent(g_wakeEvent);
    SendState();
}

// +/- 5 % : appris a ce niveau de lumiere (ou profil mis a jour, ou direct si l'app est desactivee)
static void Nudge(int delta) {
    InterlockedExchangeAdd(&g_nudge, delta);
    SetEvent(g_wakeEvent);
}

static void ShowMainWindow(const wchar_t *page = NULL) {
    ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
    g_uiVisible = 1;
    SetEvent(g_wakeEvent);
    SetForegroundWindow(g_hwnd);
    if (page) {
        wcsncpy(g_pendingPage, page, 15);
        if (g_pageReady) SendInit();             // envoie aussi la page demandee
    }
    if (!WebViewAlive() && !WebViewCreate(g_hwnd, g_mica, ThemeBg())) OnWebViewFailed();
}

// Cacher la fenetre = fermer le navigateur (la memoire est rendue a Windows)
static void HideMainWindow() {
    ShowWindow(g_hwnd, SW_HIDE);
    g_uiVisible = 0;
    g_pageReady = false;
    WebViewDestroy();
    RegisterHotkeys();                           // au cas ou une saisie de raccourci etait en cours
}

void OnWebViewFailed() {
    g_pageReady = false;
    g_uiVisible = 0;
    ShowWindow(g_hwnd, SW_HIDE);
    if (MessageBoxW(NULL, L"La fenêtre a besoin de Microsoft Edge WebView2, qui n'est pas installé sur ce PC.\n\n"
                          L"L'app continue de fonctionner près de l'horloge.\n"
                          L"Veux-tu ouvrir la page de téléchargement de WebView2 (gratuit, Microsoft) ?",
                    APP_NAME, MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST) == IDYES)
        ShellExecuteW(NULL, L"open", L"https://go.microsoft.com/fwlink/p/?LinkId=2124703", NULL, NULL, SW_SHOWNORMAL);
}

// ---------- Messages recus de la page ----------
static int Num(const char *json, const char *key, int def = 0) {
    double v;
    return JsonNumber(json, key, &v) ? (int)lround(v) : def;
}

static void AddProfile(const char *json) {
    wchar_t exe[64] = L"";
    JsonString(json, "exe", exe, 60);
    int pct = Num(json, "pct", 100);
    if (!exe[0] || pct < 0 || pct > 100) return;          // 0 = jeu : l'app se desactive
    if (!wcschr(exe, L'.')) wcscat(exe, L".exe");
    EnterCriticalSection(&g_lock);
    int i = 0;
    while (i < g_profileCount && _wcsicmp(g_profiles[i].exe, exe) != 0) i++;
    if (i < MAX_PROFILES) {
        wcscpy(g_profiles[i].exe, exe);
        g_profiles[i].pct = pct;
        if (i == g_profileCount) g_profileCount++;
    }
    LeaveCriticalSection(&g_lock);
    SaveProfiles();
    SetEvent(g_wakeEvent);
}

static void DeleteProfile(const wchar_t *exe) {
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_profileCount; i++)
        if (_wcsicmp(g_profiles[i].exe, exe) == 0) {
            for (int k = i; k < g_profileCount - 1; k++) g_profiles[k] = g_profiles[k + 1];
            g_profileCount--;
            break;
        }
    LeaveCriticalSection(&g_lock);
    SaveProfiles();
    SetEvent(g_wakeEvent);
}

static void SaveHotkey(int i, WORD hk) {
    g_hotkeys[i] = hk;
    wchar_t name[16];
    swprintf(name, 16, L"Hotkey%d", i);
    RegPut(name, hk);
}

void OnPageMessage(const char *json) {
    wchar_t cmd[32] = L"", text[128] = L"";
    JsonString(json, "cmd", cmd, 32);
    JsonString(json, "text", text, 128);
    int value = Num(json, "value");

    if (!wcscmp(cmd, L"ready"))            { g_pageReady = true; SendInit(); }
    else if (!wcscmp(cmd, L"measure"))     { g_force = 1; SetEvent(g_wakeEvent); Notice(L"Mesure en cours…"); }
    else if (!wcscmp(cmd, L"setEnabled"))  ToggleEnabled(value != 0);
    else if (!wcscmp(cmd, L"setOffset"))   SetOffset(value);
    else if (!wcscmp(cmd, L"setMinMax")) {
        int mn = Num(json, "min", g_min), mx = Num(json, "max", g_max);
        if (mn >= 1 && mx <= 100 && mn < mx) {
            g_min = mn; g_max = mx;
            RegPut(L"MinBrightness", mn);
            RegPut(L"MaxBrightness", mx);
            SetEvent(g_wakeEvent);
        }
        SendState();
    }
    else if (!wcscmp(cmd, L"setStartup"))  { SetStartup(value != 0); SendState(); }
    else if (!wcscmp(cmd, L"setUseCam")) {
        g_useCam = value != 0;
        RegPut(L"UseCamera", g_useCam);
        g_remeasure = 1;
        SetEvent(g_wakeEvent);
        SendState();
    }
    else if (!wcscmp(cmd, L"setInterval")) {
        g_camInterval = Clamp(value, MIN_INTERVAL, MAX_INTERVAL);
        RegPut(L"CameraInterval", g_camInterval);
        SetEvent(g_wakeEvent);
        SendState();
    }
    else if (!wcscmp(cmd, L"setCamera")) {
        EnterCriticalSection(&g_lock);
        wcscpy(g_camChoice, text);
        LeaveCriticalSection(&g_lock);
        RegPutStr(L"Camera", text);
        g_camExposure = LONG_MIN;                // autre camera : on repart de son exposition par defaut
        g_remeasure = 1;
        SetEvent(g_wakeEvent);
    }
    else if (!wcscmp(cmd, L"listCameras")) SendCameras();
    else if (!wcscmp(cmd, L"setCamLocked")) {
        g_camLocked = value != 0;
        if (g_camLocked && g_camLockValue == LONG_MIN) g_camLockValue = g_camExposure;   // part de l'actuelle
        RegPut(L"CamLocked", g_camLocked);
        RegPut(L"CamLockValue", (DWORD)g_camLockValue);
        g_remeasure = 1;
        SetEvent(g_wakeEvent);
        SendState();
    }
    else if (!wcscmp(cmd, L"setCamExposure")) {
        g_camLockValue = value;
        RegPut(L"CamLockValue", (DWORD)value);
        g_remeasure = 1;                         // nouvelle photo avec cette exposition
        SetEvent(g_wakeEvent);
        SendState();
    }
    else if (!wcscmp(cmd, L"setTrueTone")) {
        g_trueTone = value != 0;
        RegPut(L"TrueTone", g_trueTone);
        SetEvent(g_wakeEvent);
        SendState();
    }
    else if (!wcscmp(cmd, L"setTTStrength")) {
        g_ttStrength = Clamp(value, 0, 100);
        RegPut(L"TrueToneStrength", g_ttStrength);
        SetEvent(g_wakeEvent);
        SendState();
    }
    else if (!wcscmp(cmd, L"setReadMode"))   { if ((value != 0) != (g_readMode != 0)) ToggleRead(); }
    else if (!wcscmp(cmd, L"setPauseFullscreen")) { g_pauseFullscreen = value != 0; RegPut(L"PauseFullscreen", g_pauseFullscreen); SendState(); }
    else if (!wcscmp(cmd, L"setBatteryMode")) { g_batteryMode = Clamp(value, 0, 2); RegPut(L"BatteryMode", g_batteryMode); SetEvent(g_wakeEvent); SendState(); }
    else if (!wcscmp(cmd, L"setBatteryCut"))  { g_batteryCut = Clamp(value, 0, 50); RegPut(L"BatteryCut", g_batteryCut); SetEvent(g_wakeEvent); SendState(); }
    else if (!wcscmp(cmd, L"setExternal"))    { g_externalBrightness = value != 0; RegPut(L"ExternalBrightness", g_externalBrightness); SendState(); }
    else if (!wcscmp(cmd, L"getStats"))       SendStats();
    else if (!wcscmp(cmd, L"resetStats"))     { StatsReset(); SendStats(); Notice(L"Statistiques remises à zéro."); }
    else if (!wcscmp(cmd, L"locate"))         StartLocate();
    else if (!wcscmp(cmd, L"exportData")) {
        wchar_t msg[300];
        ExportData(msg, 300);
        if (msg[0]) Notice(msg);
    }
    else if (!wcscmp(cmd, L"importData")) {
        wchar_t msg[300];
        bool restart = ImportData(msg, 300);
        if (msg[0]) Notice(msg);
        if (restart) { RequestRestart(); DestroyWindow(g_hwnd); }
    }
    else if (!wcscmp(cmd, L"resetLearning")) {
        ResetLearning();
        Notice(L"L'app a oublié ce qu'elle avait appris : retour à la courbe de base.");
        SendState();
    }
    else if (!wcscmp(cmd, L"calibrate")) {
        g_calibrate = 1;
        SetEvent(g_wakeEvent);
        Notice(L"Calibration : la lumière actuelle devient la référence « pièce normale ».");
    }
    else if (!wcscmp(cmd, L"addProfile"))    { AddProfile(json); SendProfiles(); }
    else if (!wcscmp(cmd, L"deleteProfile")) { DeleteProfile(text); SendProfiles(); }
    else if (!wcscmp(cmd, L"pickApp")) {
        wchar_t app[64];
        EnterCriticalSection(&g_lock);
        wcscpy(app, g_lastApp);
        LeaveCriticalSection(&g_lock);
        JsonOut j;
        Begin(j, L"pickedApp");
        j.KStr(L"app", app);
        Post(j);
    }
    else if (!wcscmp(cmd, L"hotkeyCapture")) { if (value) UnregisterHotkeys(); else RegisterHotkeys(); }
    else if (!wcscmp(cmd, L"setHotkey")) {
        int i = Num(json, "index", -1), vk = Num(json, "vk"), mods = Num(json, "mods");
        if (i >= 0 && i < HK_COUNT && vk >= 0 && vk < 256)
            SaveHotkey(i, MAKEWORD(vk, mods & (HOTKEYF_SHIFT | HOTKEYF_CONTROL | HOTKEYF_ALT)));
        RegisterHotkeys();
    }
    else if (!wcscmp(cmd, L"resetHotkeys")) {
        for (int i = 0; i < HK_COUNT; i++) SaveHotkey(i, DEFAULT_HOTKEYS[i]);
        RegisterHotkeys();
    }
    else if (!wcscmp(cmd, L"searchCity"))  StartCitySearch(text);
    else if (!wcscmp(cmd, L"setLatLon")) {
        double lat, lon;
        if (JsonNumber(json, "lat", &lat) && JsonNumber(json, "lon", &lon) && fabs(lat) <= 90 && fabs(lon) <= 180) {
            g_lat = (LONG)lround(lat * 100);
            g_lon = (LONG)lround(lon * 100);
            RegPut(L"Latitude", (DWORD)g_lat);
            RegPut(L"Longitude", (DWORD)g_lon);
            EnterCriticalSection(&g_lock);
            wcscpy(g_cityResult, L"Position personnalisée");
            RegPutStr(L"City", g_cityResult);
            LeaveCriticalSection(&g_lock);
            SetEvent(g_wakeEvent);
            SendCity(true, true);
        } else SendCity(false, true);
    }
    else if (!wcscmp(cmd, L"setUpdateCheck")) { g_updateCheck = value != 0; RegPut(L"UpdateCheck", g_updateCheck); SendState(); }
    else if (!wcscmp(cmd, L"checkUpdate")) { g_updStatus = L"checking"; SendUpdate(); StartUpdateCheck(); }
    else if (!wcscmp(cmd, L"doUpdate"))    { g_updStatus = L"downloading"; SendUpdate(); StartUpdateDownload(); }
    else if (!wcscmp(cmd, L"openRepo"))    ShellExecuteW(NULL, L"open", L"https://github.com/" REPO, NULL, NULL, SW_SHOWNORMAL);
    else if (!wcscmp(cmd, L"hide"))        HideMainWindow();
    else if (!wcscmp(cmd, L"quit"))        DestroyWindow(g_hwnd);
}

// ---------- Menu de l'icone ----------
static void ShowMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN, L"Ouvrir Luminosity Manager");
    AppendMenuW(m, MF_STRING | (g_enabled ? 0 : MF_GRAYED), ID_MEASURE, L"Mesurer maintenant");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_TOGGLE, L"Activé");
    AppendMenuW(m, MF_STRING | (g_readMode ? MF_CHECKED : 0), ID_READ, L"Mode lecture");
    AppendMenuW(m, MF_STRING, ID_BRIGHTER, L"Plus clair (+5 %)");
    AppendMenuW(m, MF_STRING, ID_DARKER, L"Plus sombre (−5 %)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quitter");
    SetMenuDefaultItem(m, ID_OPEN, FALSE);

    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void AddTrayIcon() {
    g_nid.hIcon = g_enabled ? g_iconOn : g_iconOff;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

// ---------- Messages de la fenetre ----------
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) ShowMainWindow();
        else if (LOWORD(lp) == WM_RBUTTONUP) ShowMenu();
        else if (LOWORD(lp) == NIN_BALLOONUSERCLICK) ShowMainWindow(g_newVersion[0] ? L"updates" : NULL);
        return 0;
    case WM_UPDATE:                              // chaque seconde, depuis la boucle de mesure
        UpdateTip();
        if (g_pageReady) {
            SendState();
            if (g_histLast != g_sentHist) SendHist();
            if (g_source == SRC_CAMERA && g_lastMeasure != g_sentThumb) SendThumb();
        }
        return 0;
    case WM_NOSENSOR:
        Balloon(g_source == SRC_CAMERA ? L"Pas de capteur de lumière : mesure avec la webcam."
                                       : L"Pas de capteur ni de webcam : réglage selon l'heure et le soleil.");
        return 0;
    case WM_LEARNED: {
        wchar_t buf[160];
        if (lp) swprintf(buf, 160, L"Profil mis à jour : %ld %%", (long)g_profilePct);
        else swprintf(buf, 160, L"C'est retenu : %+d %% pour cette lumière (appris : %+ld %%).", (int)wp, (long)g_learnNow);
        Notice(buf);
        SendState();
        if (lp) SendProfiles();
        return 0;
    }
    case WM_HOTKEY:
        switch (wp - 1) {
        case HK_UP:      Nudge(+5); break;
        case HK_DOWN:    Nudge(-5); break;
        case HK_MEASURE: g_force = 1; SetEvent(g_wakeEvent); break;
        case HK_TOGGLE:
            ToggleEnabled(!g_enabled);
            Balloon(g_enabled ? L"Luminosity Manager : activé" : L"Luminosity Manager : désactivé");
            break;
        case HK_READ:
            ToggleRead();
            Balloon(g_readMode ? L"Mode lecture : activé" : L"Mode lecture : désactivé");
            break;
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_LOCATE) { KillTimer(h, TIMER_LOCATE); StartLocate(); return 0; }
        if (wp == TIMER_UPD_FIRST) KillTimer(h, TIMER_UPD_FIRST);
        if (g_updateCheck) StartUpdateCheck();
        return 0;
    case WM_UPDATE_FOUND:
        g_updStatus = wp == 1 ? L"available" : wp == 0 ? L"latest" : L"error";
        if (wp == 1 && wcscmp(g_notifiedVersion, g_newVersion) != 0) {   // une notification par version
            wcscpy(g_notifiedVersion, g_newVersion);
            wchar_t buf[128];
            swprintf(buf, 128, L"La version %ls est disponible. Clique ici pour mettre à jour.", g_newVersion);
            Balloon(buf);
        }
        SendUpdate();
        return 0;
    case WM_UPDATE_READY:
        if (wp) DestroyWindow(h);                // la nouvelle version se lance a la fermeture
        else { g_updStatus = L"failed"; SendUpdate(); }
        return 0;
    case WM_CITY_FOUND:
        if (wp) SetEvent(g_wakeEvent);
        SendCity(wp != 0, true);
        return 0;
    case WM_POWERBROADCAST:                      // veille, batterie, ecran eteint, luminosite changee
        if (wp == PBT_APMSUSPEND) g_suspended = 1;
        else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            g_suspended = 0;
            g_force = 1;                         // au reveil : nouvelle mesure tout de suite
            SetEvent(g_wakeEvent);
        } else if (wp == PBT_POWERSETTINGCHANGE) {
            const POWERBROADCAST_SETTING *ps = (const POWERBROADCAST_SETTING *)lp;
            DWORD v = ps->DataLength >= sizeof(DWORD) ? *(const DWORD *)ps->Data : 0;
            if (IsEqualGUID(ps->PowerSetting, GUID_VIDEO_CURRENT_MONITOR_BRIGHTNESS)) {
                g_osBright = (LONG)v;                // Windows nous previent : plus besoin de demander chaque seconde
                InterlockedIncrement(&g_osBrightSeq);
                SetEvent(g_wakeEvent);
            } else if (IsEqualGUID(ps->PowerSetting, GUID_CONSOLE_DISPLAY_STATE)) {
                g_displayOff = v != 1;               // 0 eteint, 1 allume, 2 attenue
                if (v == 1) { g_force = 1; SetEvent(g_wakeEvent); }
            } else if (IsEqualGUID(ps->PowerSetting, GUID_ACDC_POWER_SOURCE)) {
                g_onBattery = v != 0;                // 0 = branche
                SetEvent(g_wakeEvent);
            } else if (IsEqualGUID(ps->PowerSetting, GUID_POWER_SAVING)) {
                g_batterySaver = v != 0;
                SetEvent(g_wakeEvent);
            }
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:                   // ordi verrouille / deverrouille
        if (wp == WTS_SESSION_LOCK) g_locked = 1;
        else if (wp == WTS_SESSION_UNLOCK) { g_locked = 0; g_force = 1; SetEvent(g_wakeEvent); }
        return 0;
    case WM_DISPLAYCHANGE:                       // ecran branche / debranche
        BrightnessDisplaysChanged();
        return DefWindowProcW(h, msg, wp, lp);
    case WM_SIZE:
        WebViewResize();
        return 0;
    case WM_MOVE:
        WebViewMoved();
        return 0;
    case WM_GETMINMAXINFO: {                     // taille minimale de la fenetre
        UINT dpi = GetDpiForWindow(h);
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = MulDiv(760, dpi ? dpi : 96, 96);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = MulDiv(540, dpi ? dpi : 96, 96);
        return 0;
    }
    case WM_DPICHANGED: {                        // fenetre deplacee sur un ecran a une autre echelle
        const RECT *r = (const RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETTINGCHANGE:                       // theme clair / sombre change dans Windows
        if (lp && !wcscmp((const wchar_t *)lp, L"ImmersiveColorSet")) {
            ApplyWindowTheme();
            if (g_pageReady) SendInit();
        }
        return 0;
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(g_mica ? RGB(0, 0, 0) : ThemeBg());   // noir = transparent avec Mica
        FillRect((HDC)wp, &r, b);
        DeleteObject(b);
        return 1;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_OPEN:     ShowMainWindow(); break;
        case ID_MEASURE:  g_force = 1; SetEvent(g_wakeEvent); break;
        case ID_TOGGLE:   ToggleEnabled(!g_enabled); break;
        case ID_READ:     ToggleRead(); break;
        case ID_BRIGHTER: Nudge(+5); break;
        case ID_DARKER:   Nudge(-5); break;
        case ID_QUIT:     DestroyWindow(h); break;
        }
        return 0;
    case WM_CLOSE:                               // la croix cache la fenetre, l'app reste pres de l'horloge
        HideMainWindow();
        return 0;
    case WM_DESTROY:
        WTSUnRegisterSessionNotification(h);
        WebViewDestroy();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_taskbarCreated && msg) { AddTrayIcon(); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

bool CreateMainWindow(HINSTANCE inst, bool showWindow, int) {
    for (int i = 0; i < HK_COUNT; i++) {
        wchar_t name[16];
        swprintf(name, 16, L"Hotkey%d", i);
        g_hotkeys[i] = (WORD)RegGet(name, DEFAULT_HOTKEYS[i]);
    }

    // Icones nettes a toutes les tailles
    HICON bigIcon = NULL;
    LoadIconMetric(inst, MAKEINTRESOURCEW(1), LIM_LARGE, &bigIcon);
    LoadIconMetric(inst, MAKEINTRESOURCEW(1), LIM_SMALL, &g_iconOn);
    LoadIconMetric(inst, MAKEINTRESOURCEW(2), LIM_SMALL, &g_iconOff);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = bigIcon;
    wc.hIconSm = g_iconOn;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"LuminosityManagerWnd";
    if (!RegisterClassExW(&wc)) return false;

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1000, 680, NULL, NULL, inst, NULL);
    if (!g_hwnd) return false;
    // Taille selon l'echelle de l'ecran (texte net, pas de pixels)
    UINT dpi = GetDpiForWindow(g_hwnd);
    if (!dpi) dpi = 96;
    RECT r = { 0, 0, MulDiv(1000, dpi, 96), MulDiv(680, dpi, 96) };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ApplyWindowTheme();

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uCallbackMessage = WM_TRAY;
    wcscpy(g_nid.szTip, APP_NAME);
    AddTrayIcon();
    RegisterHotkeys();

    // Windows nous previent : veille, ecran eteint, batterie, economiseur, luminosite changee, verrouillage
    const GUID *powerGuids[5] = { &GUID_VIDEO_CURRENT_MONITOR_BRIGHTNESS, &GUID_CONSOLE_DISPLAY_STATE,
                                  &GUID_ACDC_POWER_SOURCE, &GUID_POWER_SAVING, NULL };
    for (int i = 0; powerGuids[i]; i++) RegisterPowerSettingNotification(g_hwnd, powerGuids[i], DEVICE_NOTIFY_WINDOW_HANDLE);
    WTSRegisterSessionNotification(g_hwnd, NOTIFY_FOR_THIS_SESSION);
    SYSTEM_POWER_STATUS ps;
    if (GetSystemPowerStatus(&ps)) {
        g_onBattery = ps.ACLineStatus == 0;
        g_batterySaver = ps.SystemStatusFlag == 1;
    }
    // 1er lancement : on trouve la ville tout seul (pour le mode soleil)
    if (!RegGet(L"AutoLocated", 0) && !RegGet(L"Latitude", 0)) SetTimer(g_hwnd, TIMER_LOCATE, 3000, NULL);

    SetTimer(g_hwnd, TIMER_UPD_FIRST, 10 * 1000, NULL);          // 1re verification 10 s apres le lancement
    SetTimer(g_hwnd, TIMER_UPD_DAILY, 24 * 60 * 60 * 1000, NULL);

    if (showWindow) ShowMainWindow();
    return true;
}
