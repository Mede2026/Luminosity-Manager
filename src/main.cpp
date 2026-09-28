// Luminosity Manager - ajuste la luminosite de l'ecran selon la lumiere ambiante.
// Win32 natif, sans dependances. Ce fichier : reglages, historique, boucle de travail, demarrage.
#include "app.h"
#include <objbase.h>
#include <mfapi.h>
#include <limits.h>

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
            if (r != ERROR_SUCCESS || type != REG_DWORD || v < 1 || v > 100) continue;
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

static bool HistPath(wchar_t *path, bool create) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    if (!n || n > MAX_PATH - 40) return false;
    wcscat(dir, L"\\LuminosityManager");
    if (create) CreateDirectoryW(dir, NULL);
    swprintf(path, MAX_PATH, L"%ls\\history.bin", dir);
    return true;
}

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

// ---------- Boucle de travail (fil separe) ----------
static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------- La courbe : lumiere de la piece (lux) -> position 0..1 ----------
// Inspiree des telephones : on voit la lumiere de facon "logarithmique" (10 -> 100 lux = aussi
// marquant que 100 -> 1000 lux). Reperes : nuit ~3 lux, piece sombre ~30, salon ~200, bureau ~400,
// pres d'une fenetre ~1500, plein jour 8000+.
const double CURVE[CURVE_POINTS][2] = {
    { 0, 0 }, { 3, 0.04 }, { 10, 0.12 }, { 30, 0.25 }, { 80, 0.38 }, { 200, 0.52 },
    { 400, 0.64 }, { 800, 0.77 }, { 1500, 0.88 }, { 3000, 0.96 }, { 8000, 1 },
};

double CurveT(double lux) {
    double x = log10((lux < 0 ? 0 : lux) + 1);
    for (int i = 1; i < CURVE_POINTS; i++) {
        double x0 = log10(CURVE[i - 1][0] + 1), x1 = log10(CURVE[i][0] + 1);
        if (x <= x1) return CURVE[i - 1][1] + (CURVE[i][1] - CURVE[i - 1][1]) * (x - x0) / (x1 - x0);
    }
    return 1;
}

// ---------- Apprentissage par niveau de lumiere ----------
// Chaque point appris couvre une zone de la courbe ; un reglage fait dans le noir ne change
// que la partie "noir" de la courbe.
static double LearnAt(double t) {
    double pos = t * (LEARN_POINTS - 1);
    int i = (int)pos;
    if (i >= LEARN_POINTS - 1) return g_learn[LEARN_POINTS - 1];
    return g_learn[i] + (g_learn[i + 1] - g_learn[i]) * (pos - i);
}

static void SaveLearn() {
    for (int i = 0; i < LEARN_POINTS; i++) {
        wchar_t name[16];
        swprintf(name, 16, L"Learn%d", i);
        RegPut(name, (DWORD)(LONG)lround(g_learn[i] * 10));
    }
}

static void Learn(double delta, double t) {
    for (int i = 0; i < LEARN_POINTS; i++) {
        double w = 1 - fabs(t * (LEARN_POINTS - 1) - i);   // poids en triangle : la somme fait 1
        if (w > 0) {
            g_learn[i] += delta * w;
            if (g_learn[i] > 60) g_learn[i] = 60;
            if (g_learn[i] < -60) g_learn[i] = -60;
        }
    }
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
    *lux = 300.0 * scene / g_camRef;
    return true;
}

// Soleil -> lumiere typique d'une piece (nuit ~8 lux, soleil haut ~1000 lux)
static double SunLux(double elevation) {
    double k = (elevation + 6) / 36;
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    return pow(10.0, 0.9 + 2.1 * k);
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
    int target = -1;
    bool warned = false, first = true;
    int camTimer = 0;
    LONG lastHistMinute = 0;
    DWORD lastTick = 0;
    HANDLE evs[2] = { g_quitEvent, g_wakeEvent };

    for (;;) {
        bool force = InterlockedExchange(&g_force, 0) != 0;
        bool calib = InterlockedExchange(&g_calibrate, 0) != 0;
        LONG nudge = InterlockedExchange(&g_nudge, 0);
        bool tick = first || force || calib || nudge || GetTickCount() - lastTick >= 1000;

        if (tick) {
            lastTick = GetTickCount();
            if (force || calib) g_remeasure = 1;
            int actual = BrightnessGet();

            // App desactivee : on ne touche a rien (sauf les raccourcis +/-), pas de camera
            if (!g_enabled) {
                if (nudge && current >= 0) { current = Clamp(current + nudge, 1, 100); BrightnessSet(current); }
                else if (actual >= 0) current = actual;
                expectRead = -2;
                target = -1;
                g_bright = current;
                PostMessageW(g_hwnd, WM_UPDATE, 0, 0);
                if (WaitForMultipleObjects(2, evs, FALSE, 1000) == WAIT_OBJECT_0) break;
                continue;
            }

            // 1) Mesurer la lumiere (lux)
            double lux = -1;
            if (SensorReadLux(&lux, force || first)) {
                g_source = SRC_SENSOR;
                g_lastMeasure = (LONG)GetTickCount();
            } else {
                if (g_useCam) {
                    if (camTimer > g_camInterval) camTimer = g_camInterval;   // intervalle raccourci
                    if (g_remeasure || --camTimer <= 0 || g_source != SRC_CAMERA) {
                        g_remeasure = 0;
                        camTimer = g_camInterval;        // une photo toutes les N secondes
                        if (CameraLux(calib, &camLux)) {
                            g_source = SRC_CAMERA;
                            g_lastMeasure = (LONG)GetTickCount();
                        } else {
                            g_source = SRC_SUN;
                        }
                    }
                } else {
                    g_source = SRC_SUN;
                }
                g_camNext = camTimer;
                if (g_source == SRC_CAMERA) lux = camLux;
                else {
                    double e = SunElevation(g_lat / 100.0, g_lon / 100.0);
                    g_sunElev = (LONG)lround(e);
                    lux = SunLux(e);
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
            int profile = CheckProfiles();
            g_profilePct = profile;
            if (actual >= 0) {
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
            }

            // 4) Luminosite voulue = courbe (entre min et max) + ajout + ce que l'app a appris
            int detected = g_min + (int)lround(t * (g_max - g_min));
            double learned = LearnAt(t);
            g_detected = detected;
            g_learnNow = (LONG)lround(learned);
            g_applied = Clamp(detected + g_offset + (int)lround(learned), g_min, g_max);
            target = profile >= 0 ? profile : g_applied;
            target = Clamp(target, 1, 100);

            if (current < 0 || force || calib) {       // tout de suite, sans transition
                current = target;
                BrightnessSet(current);
                int r = BrightnessGet();                // certains ecrans arrondissent (ex. 43 -> 40)
                expectRead = r >= 0 ? r : -2;
            }

            // 5) Historique : un point par minute
            LONG m = NowMinute();
            if (m != lastHistMinute) {
                lastHistMinute = m;
                HistAdd((int)lround(CurveT(pow(10.0, smoothLog) - 1) * 100), current);
            }
            g_bright = current;
            PostMessageW(g_hwnd, WM_UPDATE, 0, 0);
        }

        // 6) Transition douce : petits pas toutes les 0,15 s (plus rapide si l'ecart est grand)
        if (target >= 0 && current >= 0 && current != target) {
            int diff = target - current;
            int step = Clamp(abs(diff) / 3, 1, 4);
            current += diff > 0 ? step : -step;
            BrightnessSet(current);
            int r = BrightnessGet();
            expectRead = r >= 0 ? r : -2;
            g_bright = current;
        }

        DWORD elapsed = GetTickCount() - lastTick;
        DWORD wait = (target >= 0 && current != target) ? 150 : (elapsed >= 1000 ? 0 : 1000 - elapsed);
        if (WaitForMultipleObjects(2, evs, FALSE, wait) == WAIT_OBJECT_0) break;
    }

    SensorClose();
    BrightnessShutdown();
    MFShutdown();
    CoUninitialize();
    return 0;
}

// ---------- Demarrage ----------
HANDLE g_mutex;

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int show) {
    g_mutex = CreateMutexW(NULL, TRUE, L"LuminosityManager_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // deja lance : on affiche la fenetre existante
        HWND other = FindWindowW(L"LuminosityManagerWnd", NULL);
        if (other) PostMessageW(other, WM_TRAY, 0, WM_LBUTTONUP);
        return 0;
    }
    InitializeCriticalSection(&g_lock);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         NULL, EOAC_NONE, NULL);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);           // pour lister les cameras dans la fenetre

    // Nettoyage apres une mise a jour
    wchar_t exe[MAX_PATH], old[MAX_PATH + 8];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(old, MAX_PATH + 8, L"%ls.old", exe);
    DeleteFileW(old);

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

    g_quitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_wakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);

    // Lance au demarrage de Windows (--tray) : on reste discret pres de l'horloge
    if (!CreateMainWindow(inst, !wcsstr(cmdLine, L"--tray"), show)) return 1;

    HANDLE th = CreateThread(NULL, 0, Worker, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SetEvent(g_quitEvent);
    WaitForSingleObject(th, 5000);
    HistSave();
    MFShutdown();
    LaunchUpdatedAndExit();
    if (g_mutex) ReleaseMutex(g_mutex);
    return 0;
}
