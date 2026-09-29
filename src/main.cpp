// Luminosity Manager - ajuste la luminosite de l'ecran selon la lumiere ambiante.
// Win32 natif, sans dependances. Ce fichier : reglages, historique, boucle de travail, demarrage.
#include "app.h"
#include <objbase.h>
#include <mfapi.h>
#include <limits.h>
#include <shellapi.h>

static const wchar_t *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *PROFILES_KEY = REG_KEY L"\\Profiles";

volatile LONG g_enabled = 1;
volatile LONG g_offset = 0;       // -50..+50, ajoute a la luminosite detectee
volatile LONG g_min = 10, g_max = 100;
volatile LONG g_lux = -1;
volatile LONG g_bright = -1;      // luminosite actuelle appliquee
volatile LONG g_detected = -1;    // luminosite calculee depuis la lumiere (avant l'ajout)
volatile LONG g_source = SRC_SUN;
volatile LONG g_useCam = 1;
volatile LONG g_camLevel = 0;     // 0..255
volatile LONG g_sunElev = 0;      // degres
volatile LONG g_camInterval = 30;
volatile LONG g_camNext = 0;
volatile LONG g_remeasure = 0;    // 1 = reprendre une photo tout de suite
volatile LONG g_force = 0;        // 1 = mesurer et ajuster immediatement
volatile LONG g_nudge = 0;        // +/- % demandes par raccourci quand l'auto est coupe
volatile LONG g_calibrate = 0;
volatile LONG g_lastMeasure = 0;
volatile LONG g_camQuality = 0;
volatile LONG g_camExposure = LONG_MIN;
double g_camRef = 0.18;           // lumiere reelle de l'image (exposition par defaut) pour ~300 lux
volatile LONG g_camLocked = 0;    // 1 = exposition verrouillee par l'utilisateur
volatile LONG g_camLockValue = LONG_MIN;
volatile LONG g_camExpMin = 0, g_camExpMax = 0, g_camExpStep = 1, g_camLogUnits = 0, g_camManualOk = 0, g_camClip = 0;
volatile LONG g_level = -1;       // niveau de lumiere lisse 0..1000 (position sur la courbe)
volatile LONG g_applied = -1;     // luminosite voulue (courbe + ajout + appris)
volatile LONG g_learnNow = 0;     // ajout appris a ce niveau de lumiere
double g_learn[LEARN_POINTS];     // ajouts appris a 0 %, 25 %, 50 %, 75 %, 100 % de la courbe
volatile LONG g_trueTone = 1, g_ttStrength = 60;
volatile LONG g_ambientK = -1, g_displayK = 6500;
volatile LONG g_ttSource = TT_NONE, g_ttOk = 1;
static double g_camKelvin = -1;   // couleur de la lumiere mesuree par la derniere photo
volatile LONG g_locked = 0, g_displayOff = 0, g_suspended = 0;
volatile LONG g_onBattery = 0, g_batterySaver = 0;
volatile LONG g_pauseFullscreen = 1, g_batteryMode = 1, g_batteryCut = 10;
volatile LONG g_readMode = 0, g_pauseReason = PAUSE_NONE, g_camBusy = 0;
volatile LONG g_osBright = -1, g_osBrightSeq = 0;
volatile LONG g_externalBrightness = 1;
volatile LONG g_ecoActive = 0;
volatile LONG g_uiVisible = 0;
wchar_t g_camChoice[128];
volatile LONG g_lat = 4559, g_lon = -7344;   // Boucherville
volatile LONG g_profilePct = -1;
volatile LONG g_updateCheck = 1;
CRITICAL_SECTION g_lock;
wchar_t g_profileApp[64];
wchar_t g_lastApp[64];
HANDLE g_wakeEvent;
static HANDLE g_quitEvent;

Profile g_profiles[MAX_PROFILES];
int g_profileCount;

Sample g_hist[HIST_LEN];
volatile LONG g_histLast = 0;

// ---------- Registre ----------
DWORD RegGet(const wchar_t *name, DWORD def) {
    DWORD v, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        return v;
    return def;
}
void RegPut(const wchar_t *name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
}
bool RegGetStr(const wchar_t *name, wchar_t *out, DWORD chars) {
    DWORD sz = chars * sizeof(wchar_t);
    return RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_SZ, NULL, out, &sz) == ERROR_SUCCESS;
}
void RegPutStr(const wchar_t *name, const wchar_t *v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}
bool StartupEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}
void SetStartup(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH], cmd[MAX_PATH + 16];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        swprintf(cmd, MAX_PATH + 16, L"\"%ls\" --tray", path);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

// ---------- Profils (HKCU\Software\LuminosityManager\Profiles : "app.exe" = %) ----------
void LoadProfiles() {
    EnterCriticalSection(&g_lock);
    g_profileCount = 0;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, PROFILES_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        for (DWORD i = 0; g_profileCount < MAX_PROFILES; i++) {
            Profile &p = g_profiles[g_profileCount];
            DWORD nameLen = 64, type, v, sz = sizeof(v);
            LONG r = RegEnumValueW(k, i, p.exe, &nameLen, NULL, &type, (BYTE *)&v, &sz);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r != ERROR_SUCCESS || type != REG_DWORD || v > 100) continue;   // 0 = jeu : l'app se desactive
            p.pct = (int)v;
            g_profileCount++;
        }
        RegCloseKey(k);
    }
    LeaveCriticalSection(&g_lock);
}

void SaveProfiles() {
    RegDeleteKeyW(HKEY_CURRENT_USER, PROFILES_KEY);
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, PROFILES_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_profileCount; i++) {
        DWORD v = (DWORD)g_profiles[i].pct;
        RegSetValueExW(k, g_profiles[i].exe, 0, REG_DWORD, (BYTE *)&v, sizeof(v));
    }
    LeaveCriticalSection(&g_lock);
    RegCloseKey(k);
}

// Nom du .exe de l'application au premier plan ("" si c'est nous ou inconnu)
static void ForegroundExe(wchar_t *out, int n) {
    out[0] = 0;
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (!w || !GetWindowThreadProcessId(w, &pid) || pid == GetCurrentProcessId()) return;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return;
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &len)) {
        const wchar_t *name = wcsrchr(path, L'\\');
        wcsncpy(out, name ? name + 1 : path, n - 1);
        out[n - 1] = 0;
    }
    CloseHandle(p);
}

// Met a jour g_lastApp et renvoie le % du profil de l'app active (-1 = aucun)
static int CheckProfiles() {
    wchar_t exe[64];
    ForegroundExe(exe, 64);
    int pct = -1;
    EnterCriticalSection(&g_lock);
    if (exe[0]) {
        wcscpy(g_lastApp, exe);
        g_profileApp[0] = 0;
        for (int i = 0; i < g_profileCount; i++)
            if (_wcsicmp(g_profiles[i].exe, exe) == 0) {
                pct = g_profiles[i].pct;
                wcscpy(g_profileApp, g_profiles[i].exe);
                break;
            }
    } else if (g_profilePct >= 0) {
        pct = g_profilePct;           // notre fenetre au premier plan : on garde le profil en cours
    }
    LeaveCriticalSection(&g_lock);
    return pct;
}

// ---------- Historique 24 h ----------
LONG NowMinute() {
    FILETIME ft, lt;
    GetSystemTimeAsFileTime(&ft);
    FileTimeToLocalFileTime(&ft, &lt);
    ULONGLONG t = ((ULONGLONG)lt.dwHighDateTime << 32) | lt.dwLowDateTime;
    return (LONG)(t / 600000000ULL);   // minutes depuis 1601 (heure locale)
}

static void HistAdd(int light, int bright) {
    LONG m = NowMinute();
    LONG last = g_histLast;
    if (last == 0 || m - last >= HIST_LEN || m < last) {
        for (int i = 0; i < HIST_LEN; i++) g_hist[i].light = g_hist[i].bright = HIST_NONE;
    } else {
        for (LONG t = last + 1; t < m; t++) g_hist[t % HIST_LEN].light = g_hist[t % HIST_LEN].bright = HIST_NONE;
    }
    g_hist[m % HIST_LEN].light = light < 0 ? HIST_NONE : (BYTE)light;
    g_hist[m % HIST_LEN].bright = bright < 0 ? HIST_NONE : (BYTE)bright;
    g_histLast = m;
}

static bool HistPath(wchar_t *path, bool create) { return AppDataFile(L"history.bin", path, create); }

void HistLoad() {
    for (int i = 0; i < HIST_LEN; i++) g_hist[i].light = g_hist[i].bright = HIST_NONE;
    wchar_t path[MAX_PATH];
    if (!HistPath(path, false)) return;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD hdr[2], got;
    if (ReadFile(f, hdr, sizeof(hdr), &got, NULL) && got == sizeof(hdr) && hdr[0] == 0x31484D4C /* "LMH1" */) {
        Sample tmp[HIST_LEN];
        if (ReadFile(f, tmp, sizeof(tmp), &got, NULL) && got == sizeof(tmp)) {
            memcpy(g_hist, tmp, sizeof(tmp));
            g_histLast = (LONG)hdr[1];
        }
    }
    CloseHandle(f);
}

void HistSave() {
    wchar_t path[MAX_PATH];
    if (!g_histLast || !HistPath(path, true)) return;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD hdr[2] = { 0x31484D4C, (DWORD)g_histLast }, put;
    WriteFile(f, hdr, sizeof(hdr), &put, NULL);
    WriteFile(f, g_hist, sizeof(g_hist), &put, NULL);
    CloseHandle(f);
}

// ---------- True Tone ----------
// Blanc de l'ecran a k Kelvin : facteurs par rapport au blanc normal (6500 K), sans jamais depasser 1
static bool ApplyDisplayKelvin(double k) {
    double f[3];
    DisplayFactors(k, f);
    return ColorApply(f[0], f[1], f[2]);
}

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------- Apprentissage par niveau de lumiere (calculs dans core.cpp) ----------
static void SaveLearn() {
    for (int i = 0; i < LEARN_POINTS; i++) {
        wchar_t name[16];
        swprintf(name, 16, L"Learn%d", i);
        RegPut(name, (DWORD)(LONG)lround(g_learn[i] * 10));
    }
}

static void Learn(double delta, double t) {
    LearnApply(g_learn, delta, t);
    SaveLearn();
}

void ResetLearning() {
    for (int i = 0; i < LEARN_POINTS; i++) g_learn[i] = 0;
    SaveLearn();
    SetEvent(g_wakeEvent);
}

// ---------- Mesures ----------
// Photo webcam -> lumiere estimee en lux (grace a l'exposition connue)
static bool CameraLux(bool calibrate, double *lux) {
    wchar_t pref[128];
    EnterCriticalSection(&g_lock);
    wcscpy(pref, g_camChoice);
    LeaveCriticalSection(&g_lock);
    bool locked = g_camLocked != 0;
    long e = locked ? g_camLockValue : g_camExposure;
    CamShot shot;
    if (!WebcamMeasure(pref, &e, locked, &shot)) return false;
    if (!locked && shot.manual && e != g_camExposure) { g_camExposure = e; RegPut(L"CamExposure", (DWORD)e); }
    g_camExpMin = shot.expMin;
    g_camExpMax = shot.expMax;
    g_camExpStep = shot.expStep;
    g_camLogUnits = shot.logUnits;
    g_camManualOk = shot.manual;
    g_camClip = shot.clip;
    if (shot.hasExp) g_camExposure = shot.exp;

    // Lumiere ramenee a l'exposition par defaut : si on a expose 2x moins longtemps, la piece est 2x plus claire
    double scene = shot.lin < 0.0005 ? 0.0005 : shot.lin;
    if (shot.hasExp) {
        if (shot.logUnits) scene *= pow(2.0, (double)(shot.expDef - shot.exp));
        else if (shot.exp > 0 && shot.expDef > 0) scene *= (double)shot.expDef / shot.exp;
    }
    if (calibrate) {                        // "la piece est eclairee normalement" = 300 lux
        g_camRef = scene;
        RegPut(L"CamRefLin", (DWORD)lround(scene * 1000000));
    }
    g_camLevel = shot.mean;
    g_camQuality = shot.manual ? 0 : (shot.hasExp ? 1 : 2);
    g_camKelvin = shot.kelvin;
    *lux = 300.0 * scene / g_camRef;
    return true;
}

// Une app est-elle en plein ecran (jeu, video, presentation) ?
static bool FullscreenApp() {
    QUERY_USER_NOTIFICATION_STATE q;
    if (SUCCEEDED(SHQueryUserNotificationState(&q)) &&
        (q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE || q == QUNS_BUSY))
        return true;
    // Jeux "plein ecran sans bordure" : la fenetre active couvre tout l'ecran
    HWND w = GetForegroundWindow();
    if (!w || w == g_hwnd) return false;
    wchar_t cls[64];
    GetClassNameW(w, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    RECT r;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetWindowRect(w, &r) || !GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

// Statistiques : une ligne par minute
static void RecordMinute(bool off, bool paused, int bright, double lux, int ttK) {
    static LONG lastMinute;
    LONG m = NowMinute();
    if (m == lastMinute) return;
    lastMinute = m;
    wchar_t app[64] = L"";
    if (g_profilePct >= 0) {
        EnterCriticalSection(&g_lock);
        wcscpy(app, g_profileApp);
        LeaveCriticalSection(&g_lock);
    }
    StatsSample s = { off, paused, g_readMode != 0, g_ecoActive != 0, bright, lux,
                      g_source == SRC_SENSOR ? 0 : g_source == SRC_CAMERA ? 1 : 2, ttK, app };
    StatsMinute(s);
}

// ---------- Boucle de travail (fil separe) ----------
DWORD WINAPI Worker(LPVOID) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    BrightnessInit();

    double smoothLog = -1;   // lumiere lissee, en log10(lux + 1)
    double anchorLog = -1;   // lumiere utilisee pour la cible (zone morte : pas de micro-changements)
    double camLux = -1;
    int current = BrightnessGet();
    int expectRead = -2;     // valeur relue apres notre dernier reglage (-2 = a resynchroniser)
    int target = -1, lastStatTarget = -1;
    bool warned = false, first = true;
    DWORD camDue = 0;        // moment de la prochaine photo (GetTickCount)
    LONG lastPostedBright = -2, lastPostedLux = -2;
    LONG lastHistMinute = 0, seenSeq = g_osBrightSeq;
    DWORD lastTick = 0, lastPoll = 0, lastSetTick = 0, lastFsCheck = 0, lastBusyCheck = 0;
    bool notified = false, fullscreen = false, camBusy = false, wasPaused = false;
    int camFails = 0;        // photos ratees de suite (la webcam met parfois quelques secondes a se reveiller)
    double ambMired = -1;    // couleur de la piece lissee (en "mired" = 1 000 000 / Kelvin, comme l'oeil la percoit)
    double dispMired = 1e6 / 6500, appliedMired = 1e6 / 6500;
    bool colorChanged = false;
    DWORD lastColorApply = 0;
    HANDLE evs[2] = { g_quitEvent, g_wakeEvent };

    auto setBrightness = [&](int pct) {
        BrightnessSet(pct);
        lastSetTick = GetTickCount();
        int r = BrightnessGet();             // certains ecrans arrondissent (ex. 43 -> 40)
        expectRead = r >= 0 ? r : -2;
    };
    auto resetColor = [&]() {
        if (colorChanged) ColorReset();
        colorChanged = false;
        dispMired = appliedMired = 1e6 / 6500;
        g_displayK = 6500;
    };

    for (;;) {
        bool force = InterlockedExchange(&g_force, 0) != 0;
        bool calib = InterlockedExchange(&g_calibrate, 0) != 0;
        LONG nudge = InterlockedExchange(&g_nudge, 0);
        DWORD now = GetTickCount();
        // Cadence : 1 s quand la fenetre est ouverte ; sinon 2 s (capteur) ou 5 s (webcam / soleil : la lumiere
        // ne change pas si vite). x2 en economie d'energie. Moins de reveils = moins de batterie.
        DWORD period = g_uiVisible ? 1000 : (g_source == SRC_SENSOR ? 2000 : 5000);
        if (g_ecoActive && !g_uiVisible) period *= 2;
        bool tick = first || force || calib || nudge || now - lastTick >= period;

        if (tick) {
            lastTick = now;
            if (force || calib) g_remeasure = 1;

            // Luminosite reelle : Windows nous previent quand elle change ; sinon on la lit toutes les 10 s
            int actual = -1;
            LONG seq = g_osBrightSeq;
            if (seq != seenSeq) { seenSeq = seq; notified = true; actual = g_osBright; }
            else if (!notified && now - lastPoll >= 10000) { lastPoll = now; actual = BrightnessGet(); }
            bool ownChange = now - lastSetTick < 2000;       // c'est nous qui venons de la changer

            // Economie d'energie : 1 = sur batterie (ou economiseur), 2 = seulement avec l'economiseur de Windows
            g_ecoActive = (g_batteryMode == 1 && (g_onBattery || g_batterySaver)) || (g_batteryMode == 2 && g_batterySaver);

            // Pause : ordi verrouille, ecran eteint, veille, jeu (plein ecran ou marque "Jeu" dans les profils)
            int profile = CheckProfiles();
            g_profilePct = profile;
            if (now - lastFsCheck >= 2000) { lastFsCheck = now; fullscreen = g_pauseFullscreen && FullscreenApp(); }
            int pause = g_suspended ? PAUSE_SLEEP : g_locked ? PAUSE_LOCKED : g_displayOff ? PAUSE_SCREEN_OFF
                      : profile == 0 ? PAUSE_GAME : fullscreen ? PAUSE_FULLSCREEN : PAUSE_NONE;
            g_pauseReason = pause;
            // Fin d'une pause (reveil, deverrouillage, ecran rallume, fin du jeu, app reactivee) :
            // nouvelle mesure tout de suite, appliquee sans transition
            if (pause || !g_enabled) wasPaused = true;
            else if (wasPaused) { wasPaused = false; force = true; g_remeasure = 1; camFails = 0; }

            // App desactivee ou en pause : on ne touche a rien (sauf les raccourcis +/-), pas de camera.
            // Pendant un jeu, les couleurs redeviennent normales (les vraies couleurs du jeu).
            if (!g_enabled || pause) {
                if (!g_enabled || pause == PAUSE_GAME || pause == PAUSE_FULLSCREEN) resetColor();
                if (nudge && current >= 0) setBrightness(current = Clamp(current + nudge, 1, 100));
                else if (actual >= 0) current = actual;
                expectRead = -2;
                target = -1;
                g_bright = current;
                RecordMinute(!g_enabled, pause != 0, current, -1, 0);
                PostMessageW(g_hwnd, WM_UPDATE, 0, 0);
                // en pause, fenetre cachee : on se reveille rarement (Windows nous previent au deverrouillage)
                if (WaitForMultipleObjects(2, evs, FALSE, g_uiVisible ? 1000 : 3000) == WAIT_OBJECT_0) break;
                continue;
            }

            // 1) Mesurer la lumiere (lux)
            double lux = -1, sensorK = -1;
            double sunE = SunElevation(g_lat / 100.0, g_lon / 100.0);
            g_sunElev = (LONG)lround(sunE);
            if (SensorReadLux(&lux, &sensorK, force || first)) {
                g_source = SRC_SENSOR;
                g_camBusy = 0;
                g_lastMeasure = (LONG)GetTickCount();
            } else {
                // Economie d'energie : 4x moins de photos (la camera consomme de la batterie)
                int interval = g_ecoActive ? (g_camInterval * 4 > MAX_INTERVAL ? MAX_INTERVAL : g_camInterval * 4) : g_camInterval;
                if (g_useCam) {
                    if ((LONG)(camDue - now) > interval * 1000) camDue = now + interval * 1000;   // intervalle raccourci
                    if (g_remeasure || (LONG)(now - camDue) >= 0 || (g_source != SRC_CAMERA && camLux < 0)) {
                        // Une autre app utilise la webcam (Teams, Discord...) : on n'y touche pas
                        if (g_remeasure || now - lastBusyCheck >= 3000) { lastBusyCheck = now; camBusy = WebcamBusyElsewhere(); }
                        g_camBusy = camBusy;
                        if (camBusy) {
                            camDue = now + 3000;         // on reessaie dans 3 s, en gardant la derniere mesure
                            if (camLux < 0) g_source = SRC_SUN;
                        } else {
                            g_remeasure = 0;
                            camDue = GetTickCount() + interval * 1000;   // une photo toutes les N secondes
                            if (CameraLux(calib, &camLux)) {
                                g_source = SRC_CAMERA;
                                g_lastMeasure = (LONG)GetTickCount();
                                StatsEvent(STAT_PHOTO);
                                camFails = 0;
                            } else if (camLux >= 0 && ++camFails <= 5) {
                                camDue = GetTickCount() + 2000;   // webcam pas encore prete : on garde la derniere mesure
                            } else {
                                g_source = SRC_SUN;
                                camLux = -1;
                            }
                        }
                    }
                } else {
                    g_source = SRC_SUN;
                    g_camBusy = 0;
                }
                g_camNext = (LONG)(camDue - GetTickCount()) > 0 ? (LONG)(camDue - GetTickCount() + 999) / 1000 : 0;
                if (g_source == SRC_CAMERA && camLux >= 0) lux = camLux;
                else {
                    g_source = SRC_SUN;
                    lux = SunLux(sunE);
                    g_lastMeasure = (LONG)GetTickCount();
                }
                if (!warned) { warned = true; PostMessageW(g_hwnd, WM_NOSENSOR, 0, 0); }
            }
            g_lux = (LONG)lround(lux);

            // 2) Lisser : plus clair = vite (pour lire), plus sombre = lentement (une ombre qui passe
            //    ne doit pas assombrir l'ecran)
            double lg = log10(lux + 1);
            if (smoothLog < 0 || force || calib || first) smoothLog = lg;
            else smoothLog += (lg - smoothLog) * (lg > smoothLog ? 0.5 : 0.15);
            // Zone morte : on ne change la cible que si la lumiere a vraiment change (~12 %)
            if (anchorLog < 0 || force || calib || first || fabs(smoothLog - anchorLog) > 0.05) anchorLog = smoothLog;
            double t = CurveT(pow(10.0, anchorLog) - 1);
            g_level = (LONG)lround(t * 1000);
            first = false;

            // 3) Tu as change la luminosite toi-meme (touches Fn / Windows) : on l'apprend a ce niveau de lumiere
            if (actual >= 0 && !ownChange) {
                if (expectRead == -2) expectRead = actual;
                else if (expectRead >= 0 && abs(actual - expectRead) >= 2) {
                    int delta = actual - expectRead;
                    current = expectRead = target = actual;
                    if (profile >= 0) {
                        EnterCriticalSection(&g_lock);
                        for (int i = 0; i < g_profileCount; i++)
                            if (_wcsicmp(g_profiles[i].exe, g_profileApp) == 0) g_profiles[i].pct = actual;
                        LeaveCriticalSection(&g_lock);
                        SaveProfiles();
                        g_profilePct = profile = actual;
                        PostMessageW(g_hwnd, WM_LEARNED, (WPARAM)delta, 1);
                    } else {
                        Learn(delta, t);
                        PostMessageW(g_hwnd, WM_LEARNED, (WPARAM)delta, 0);
                    }
                    StatsEvent(STAT_MANUAL);
                }
            }
            // Raccourcis +/- : meme chose (profil ou apprentissage)
            if (nudge) {
                if (profile >= 0) {
                    int pct = Clamp(profile + nudge, 1, 100);
                    EnterCriticalSection(&g_lock);
                    for (int i = 0; i < g_profileCount; i++)
                        if (_wcsicmp(g_profiles[i].exe, g_profileApp) == 0) g_profiles[i].pct = pct;
                    LeaveCriticalSection(&g_lock);
                    SaveProfiles();
                    g_profilePct = profile = pct;
                    PostMessageW(g_hwnd, WM_LEARNED, (WPARAM)nudge, 1);
                } else {
                    Learn(nudge, t);
                    PostMessageW(g_hwnd, WM_LEARNED, (WPARAM)nudge, 0);
                }
                StatsEvent(STAT_MANUAL);
            }

            // 4) Luminosite voulue = courbe (entre min et max) + ajout + appris - batterie ; mode lecture : 70 %
            int detected = g_min + (int)lround(t * (g_max - g_min));
            double learned = LearnAt(g_learn, t);
            int cut = g_ecoActive ? g_batteryCut + (g_batterySaver ? 5 : 0) : 0;
            g_detected = detected;
            g_learnNow = (LONG)lround(learned);
            g_applied = Clamp(detected + g_offset + (int)lround(learned) - cut, g_min, g_max);
            target = profile >= 0 ? profile : g_applied;
            if (g_readMode) target = (int)lround(target * 0.7);
            target = Clamp(target, 1, 100);
            if (lastStatTarget >= 0 && abs(target - lastStatTarget) >= 2 && !nudge) StatsEvent(STAT_ADJUST);
            lastStatTarget = target;

            if (current < 0 || force || calib) setBrightness(current = target);   // tout de suite, sans transition

            // 5) True Tone : la couleur du blanc de l'ecran suit (en partie) la couleur de la lumiere
            double ambK = -1;
            int ttSrc = TT_NONE;
            if (g_source == SRC_SENSOR && sensorK > 0) { ambK = sensorK; ttSrc = TT_SENSOR; }
            else if (g_source == SRC_CAMERA && g_camKelvin > 0) { ambK = g_camKelvin; ttSrc = TT_CAMERA; }
            else { ambK = SunKelvin(sunE); ttSrc = TT_SUN; }
            g_ttSource = ttSrc;
            double mAmb = 1e6 / ambK;
            // tres lent, comme sur iOS (environ 30 s pour suivre un changement)
            ambMired = (ambMired < 0 || force || calib) ? mAmb : ambMired + (mAmb - ambMired) * 0.08;
            g_ambientK = (LONG)lround(1e6 / ambMired);
            if (g_trueTone || g_readMode) {
                const double m6500 = 1e6 / 6500;
                double targetM;
                if (g_readMode) targetM = 1e6 / 3800;        // mode lecture : ecran chaud, doux pour les yeux
                else {
                    // l'ecran va vers la couleur de la piece, sans la copier (70 % max, entre 4700 K et 7200 K)
                    targetM = m6500 + (ambMired - m6500) * (g_ttStrength / 100.0) * 0.7;
                    if (targetM > 1e6 / 4700) targetM = 1e6 / 4700;
                    if (targetM < 1e6 / 7200) targetM = 1e6 / 7200;
                }
                dispMired += (targetM - dispMired) * ((force || calib) ? 1.0 : g_readMode ? 0.3 : 0.12);
                g_displayK = (LONG)lround(1e6 / dispMired);
                // on reapplique si ca a change, ou toutes les 10 s (Windows remet parfois les couleurs a zero)
                bool moved = fabs(dispMired - appliedMired) > 0.3;
                if (moved || (colorChanged && GetTickCount() - lastColorApply >= 10000)) {
                    g_ttOk = ApplyDisplayKelvin(1e6 / dispMired);
                    colorChanged = true;
                    appliedMired = dispMired;
                    lastColorApply = GetTickCount();
                }
            } else {
                resetColor();
            }

            // 6) Historique et statistiques : un point par minute
            LONG m = NowMinute();
            if (m != lastHistMinute) {
                lastHistMinute = m;
                HistAdd((int)lround(CurveT(pow(10.0, smoothLog) - 1) * 100), current);
            }
            RecordMinute(false, false, current, lux, (g_trueTone || g_readMode) ? (int)g_displayK : 0);
            g_bright = current;
            // Fenetre cachee : on ne la reveille que si quelque chose a change (texte de l'icone)
            if (g_uiVisible || current != lastPostedBright || g_lux != lastPostedLux) {
                lastPostedBright = current;
                lastPostedLux = g_lux;
                PostMessageW(g_hwnd, WM_UPDATE, 0, 0);
            }
        }

        // 7) Transition douce : petits pas toutes les 0,15 s (plus rapide si l'ecart est grand)
        if (target >= 0 && current >= 0 && current != target) {
            int diff = target - current;
            // economie d'energie : moins de pas (moins de travail pour le processeur et l'ecran)
            int step = g_ecoActive ? Clamp(abs(diff) / 2, 2, 8) : Clamp(abs(diff) / 3, 1, 4);
            current += diff > 0 ? step : -step;
            setBrightness(current);
            g_bright = current;
        }

        DWORD elapsed = GetTickCount() - lastTick;
        DWORD wait = (target >= 0 && current != target) ? (g_ecoActive ? 300 : 150) : (elapsed >= period ? 0 : period - elapsed);
        if (WaitForMultipleObjects(2, evs, FALSE, wait) == WAIT_OBJECT_0) break;
    }

    resetColor();                                    // couleurs normales en quittant
    SensorClose();
    BrightnessShutdown();
    MFShutdown();
    CoUninitialize();
    return 0;
}

// ---------- Demarrage ----------
HANDLE g_mutex;

// « Desinstaller » depuis Parametres > Applications (LuminosityManager.exe --uninstall)
static int UninstallCommand() {
    UINT q = MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST;
    if (MessageBoxW(NULL, L"Désinstaller Luminosity Manager ?", APP_NAME, q) != IDYES) return 0;
    bool keep = MessageBoxW(NULL, L"Garder tes réglages et tes statistiques (utile si tu la réinstalles) ?",
                            APP_NAME, q) == IDYES;
    HWND other = FindWindowW(L"LuminosityManagerWnd", NULL);  // l'app tourne : on la ferme d'abord
    if (other) {
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(other, WM_QUIT_APP, 0, 0);
        if (p) { WaitForSingleObject(p, 10000); CloseHandle(p); }
    }
    MessageBoxW(NULL, L"Luminosity Manager est désinstallée. Merci de l'avoir utilisée !", APP_NAME,
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    UninstallApp(keep);                              // efface le dossier quelques secondes apres la fermeture
    return 0;
}

// La fenetre existe : on previent l'ancienne version (mise a jour / installation) que tout va bien
static void AfterStart(const wchar_t *cmdLine) {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, STARTED_EVENT);
    if (ev) { SetEvent(ev); CloseHandle(ev); }
    wchar_t exe[MAX_PATH], old[MAX_PATH + 8];            // nettoyage apres une mise a jour
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(old, MAX_PATH + 8, L"%ls.old", exe);
    DeleteFileW(old);
    RefreshUninstallEntry();                              // version a jour dans Parametres > Applications

    wchar_t msg[300];
    const wchar_t *inst = wcsstr(cmdLine, L"--installed \"");
    if (inst) {
        wchar_t path[MAX_PATH];
        inst += 13;
        int n = 0;
        while (inst[n] && inst[n] != L'"' && n < MAX_PATH - 1) { path[n] = inst[n]; n++; }
        path[n] = 0;
        DeleteOldCopy(path);
        StartNotice(L"Luminosity Manager est installée ✓ Tu la trouveras dans le menu Démarrer. "
                    L"Le fichier téléchargé a été supprimé.", NULL, false);
    } else if (wcsstr(cmdLine, L"--update-failed")) {
        StartNotice(L"La nouvelle version n'a pas pu démarrer (Sécurité Windows l'a peut-être bloquée). "
                    L"Ta version a été remise.", L"updates", true);
    } else if (wcsstr(cmdLine, L"--install-failed")) {
        StartNotice(L"L'app installée n'a pas pu démarrer (Sécurité Windows l'a peut-être bloquée). "
                    L"L'app continue depuis ce dossier.", L"updates", true);
    } else if (wcsstr(cmdLine, L"--updated")) {
        swprintf(msg, 300, L"Luminosity Manager %ls est prête ✓", APP_VERSION);
        StartNotice(msg, NULL, false);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int show) {
    if (wcsstr(cmdLine, L"--uninstall")) return UninstallCommand();
    g_mutex = CreateMutexW(NULL, TRUE, L"LuminosityManager_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // deja lance : on affiche la fenetre existante
        HWND other = FindWindowW(L"LuminosityManagerWnd", NULL);
        if (other) PostMessageW(other, WM_TRAY, 0, WM_LBUTTONUP);
        return 0;
    }
    InitializeCriticalSection(&g_lock);
    // Mode efficacite (Windows 11) : coeurs basse consommation, vitesse reduite. L'app fait tres peu de calculs.
    PROCESS_POWER_THROTTLING_STATE eco = {};
    eco.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    eco.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    eco.StateMask = eco.ControlMask;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &eco, sizeof(eco));
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         NULL, EOAC_NONE, NULL);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);           // pour lister les cameras dans la fenetre

    g_enabled = RegGet(L"Enabled", 1) ? 1 : 0;
    g_offset = (LONG)RegGet(L"Offset", 0);
    if (g_offset > 50 || g_offset < -50) g_offset = 0;
    g_min = (LONG)RegGet(L"MinBrightness", 10);
    g_max = (LONG)RegGet(L"MaxBrightness", 100);
    if (g_min < 1 || g_max > 100 || g_min >= g_max) { g_min = 10; g_max = 100; }
    g_useCam = RegGet(L"UseCamera", 1) ? 1 : 0;
    g_camInterval = (LONG)RegGet(L"CameraInterval", 30);
    if (g_camInterval < MIN_INTERVAL || g_camInterval > MAX_INTERVAL) g_camInterval = 30;
    g_lat = (LONG)RegGet(L"Latitude", (DWORD)g_lat);
    g_lon = (LONG)RegGet(L"Longitude", (DWORD)g_lon);
    g_updateCheck = RegGet(L"UpdateCheck", 1) ? 1 : 0;
    g_camExposure = (LONG)RegGet(L"CamExposure", (DWORD)LONG_MIN);
    DWORD ref = RegGet(L"CamRefLin", 0);                 // reference de calibration (lumiere reelle x 1e6)
    if (ref >= 50 && ref < 100000000) g_camRef = ref / 1000000.0;
    g_camLocked = RegGet(L"CamLocked", 0) ? 1 : 0;
    g_trueTone = RegGet(L"TrueTone", 1) ? 1 : 0;
    g_pauseFullscreen = RegGet(L"PauseFullscreen", 1) ? 1 : 0;
    g_batteryMode = (LONG)RegGet(L"BatteryMode", 1);   // 0 jamais, 1 sur batterie, 2 economiseur Windows
    if (g_batteryMode < 0 || g_batteryMode > 2) g_batteryMode = 1;
    g_batteryCut = (LONG)RegGet(L"BatteryCut", 10);
    if (g_batteryCut < 0 || g_batteryCut > 50) g_batteryCut = 10;
    g_externalBrightness = RegGet(L"ExternalBrightness", 1) ? 1 : 0;
    g_ttStrength = (LONG)RegGet(L"TrueToneStrength", 60);
    if (g_ttStrength < 0 || g_ttStrength > 100) g_ttStrength = 60;
    g_camLockValue = (LONG)RegGet(L"CamLockValue", (DWORD)LONG_MIN);
    for (int i = 0; i < LEARN_POINTS; i++) {
        wchar_t name[16];
        swprintf(name, 16, L"Learn%d", i);
        g_learn[i] = (LONG)RegGet(name, 0) / 10.0;
        if (fabs(g_learn[i]) > 60) g_learn[i] = 0;
    }
    RegGetStr(L"Camera", g_camChoice, 128);
    if (StartupEnabled()) SetStartup(true);             // garde le chemin a jour (et --tray)
    LoadProfiles();
    HistLoad();
    StatsLoad();

    g_quitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_wakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);

    // Lance au demarrage de Windows (--tray) : on reste discret pres de l'horloge
    if (!CreateMainWindow(inst, !wcsstr(cmdLine, L"--tray"), show)) return 1;
    AfterStart(cmdLine);

    HANDLE th = CreateThread(NULL, 0, Worker, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SetEvent(g_quitEvent);
    WaitForSingleObject(th, 5000);
    if (!g_noSaveOnExit) { HistSave(); StatsSave(); }   // apres un import, on garde les fichiers importes
    MFShutdown();
    LaunchUpdatedAndExit();
    if (g_mutex) ReleaseMutex(g_mutex);
    return 0;
}
