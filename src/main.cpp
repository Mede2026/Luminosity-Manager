// Luminosity Manager - ajuste la luminosite de l'ecran selon la lumiere ambiante.
// Win32 natif, sans dependances. Ce fichier : reglages, historique, boucle de travail, demarrage.
#include "app.h"
#include <objbase.h>
#include <mfapi.h>
#include <limits.h>

static const int STEP = 5, HYSTERESIS = 3;
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
double g_camRef = 118;            // clarte de l'image a l'exposition par defaut pour ~300 lux
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

// Niveau de lumiere 0..1 -> pourcentage entre le minimum et le maximum choisis
static int LevelToPercent(double t) {
    if (t > 1) t = 1;
    if (t < 0) t = 0;
    return g_min + (int)lround(t * (g_max - g_min));
}

// Photo webcam -> lumiere estimee en lux (grace a l'exposition connue)
static bool CameraLux(bool calibrate, double *lux) {
    wchar_t pref[128];
    EnterCriticalSection(&g_lock);
    wcscpy(pref, g_camChoice);
    LeaveCriticalSection(&g_lock);
    long e = g_camExposure;
    CamShot shot;
    if (!WebcamMeasure(pref, &e, &shot)) return false;
    if (shot.manual && e != g_camExposure) { g_camExposure = e; RegPut(L"CamExposure", (DWORD)e); }

    // Clarte ramenee a l'exposition par defaut : si on a expose 2x moins longtemps, la piece est 2x plus claire
    double scene = shot.mean < 1 ? 1 : shot.mean;
    if (shot.hasExp) {
        if (shot.logUnits) scene *= pow(2.0, (double)(shot.expDef - shot.exp));
        else if (shot.exp > 0 && shot.expDef > 0) scene *= (double)shot.expDef / shot.exp;
    }
    if (calibrate) {                        // "la piece est eclairee normalement" = 300 lux
        g_camRef = scene;
        RegPut(L"CamRef", (DWORD)lround(scene * 100));
    }
    g_camLevel = shot.mean;
    g_camQuality = shot.manual ? 0 : (shot.hasExp ? 1 : 2);
    *lux = 300.0 * scene / g_camRef;
    return true;
}

DWORD WINAPI Worker(LPVOID) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    BrightnessInit();

    double smooth = -1;      // niveau lisse 0..1
    double camLux = -1;
    int current = BrightnessGet();
    int expectRead = -2;     // valeur relue apres notre dernier reglage (-2 = a resynchroniser)
    bool moving = false, warned = false, first = true;
    int camTimer = 0;
    LONG lastHistMinute = 0;
    HANDLE evs[2] = { g_quitEvent, g_wakeEvent };

    for (;;) {
        bool force = InterlockedExchange(&g_force, 0) != 0;
        bool calib = InterlockedExchange(&g_calibrate, 0) != 0;
        if (force || calib) g_remeasure = 1;

        // Raccourcis +/- : fonctionnent meme quand l'app est desactivee
        LONG nudge = InterlockedExchange(&g_nudge, 0);
        if (nudge && current >= 0 && !(g_enabled && g_profilePct < 0)) {
            current = Clamp(current + nudge, 1, 100);
            BrightnessSet(current);
            expectRead = -2;
            if (g_enabled && g_profilePct >= 0) {        // profil actif : on retient le nouveau %
                EnterCriticalSection(&g_lock);
                for (int i = 0; i < g_profileCount; i++)
                    if (_wcsicmp(g_profiles[i].exe, g_profileApp) == 0) g_profiles[i].pct = current;
                LeaveCriticalSection(&g_lock);
                SaveProfiles();
                g_profilePct = current;
                PostMessageW(g_hwnd, WM_LEARNED, (WPARAM)nudge, 1);
            }
        }

        // Luminosite reelle de l'ecran (portable) : detecte un changement fait a la main
        int actual = BrightnessGet();
        if (!g_enabled) {                    // app desactivee : on ne touche a rien, pas de camera
            if (actual >= 0) current = actual;
            expectRead = -2;
            g_bright = current;
            PostMessageW(g_hwnd, WM_UPDATE, 0, 0);
            if (WaitForMultipleObjects(2, evs, FALSE, 1000) == WAIT_OBJECT_0) break;
            continue;
        }
        if (actual >= 0) {
            if (expectRead == -2) expectRead = actual;
            else if (expectRead >= 0 && abs(actual - expectRead) >= 2) {
                // L'utilisateur a change la luminosite lui-meme : on apprend l'ecart
                int delta = actual - expectRead;
                current = expectRead = actual;
                moving = false;
                if (g_profilePct >= 0) {
                    EnterCriticalSection(&g_lock);
                    for (int i = 0; i < g_profileCount; i++)
                        if (_wcsicmp(g_profiles[i].exe, g_profileApp) == 0) g_profiles[i].pct = actual;
                    LeaveCriticalSection(&g_lock);
                    SaveProfiles();
                    g_profilePct = actual;
                } else {
                    g_offset = Clamp(g_offset + delta, -50, 50);
                    RegPut(L"Offset", (DWORD)g_offset);
                }
                PostMessageW(g_hwnd, WM_LEARNED, (WPARAM)delta, 0);
            }
        }

        // 1) Mesurer la lumiere
        double level = -1, lux;
        if (SensorReadLux(&lux, force || first)) {
            level = log10(lux + 1.0) / log10(1001.0);   // 0 lux -> 0, 1000 lux -> 1
            g_lux = (LONG)lround(lux);
            g_source = SRC_SENSOR;
            g_lastMeasure = (LONG)GetTickCount();
        } else {
            if (g_useCam) {
                if (camTimer > g_camInterval) camTimer = g_camInterval;   // intervalle raccourci
                if (g_remeasure || --camTimer <= 0 || g_source != SRC_CAMERA) {
                    g_remeasure = 0;
                    camTimer = g_camInterval;            // une photo toutes les N secondes
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
            if (g_source == SRC_CAMERA) {
                level = log10(camLux + 1.0) / log10(1001.0);
                g_lux = (LONG)lround(camLux);
            } else {
                double e = SunElevation(g_lat / 100.0, g_lon / 100.0);
                g_sunElev = (LONG)lround(e);
                g_lux = -1;
                level = (e + 6) / 36.0;                  // nuit -> 0, soleil a 30 deg -> 1
                g_lastMeasure = (LONG)GetTickCount();
            }
            if (!warned) { warned = true; PostMessageW(g_hwnd, WM_NOSENSOR, 0, 0); }
        }
        first = false;

        if (level >= 0) {
            if (level > 1) level = 1;
            smooth = (smooth < 0 || force || calib) ? level : smooth * 0.7 + level * 0.3;
        }
        g_detected = smooth >= 0 ? LevelToPercent(smooth) : -1;

        // 2) Choisir la luminosite voulue : profil d'app > mode auto
        int profile = CheckProfiles();
        g_profilePct = profile;
        int target = -1;
        if (profile >= 0) target = profile;
        else if (g_detected >= 0) target = Clamp(g_detected + g_offset, g_min, g_max);

        // 3) Appliquer (immediat si force ou 1re fois, sinon en douceur)
        if (target >= 0) {
            target = Clamp(target, 1, 100);
            int before = current;
            if (current < 0 || force || calib) {
                current = target;
                moving = false;
            } else {
                int diff = target - current;
                if (abs(diff) >= HYSTERESIS) moving = true;
                if (moving) {
                    if (diff == 0) moving = false;
                    else current += diff > 0 ? (diff < STEP ? diff : STEP) : (diff > -STEP ? diff : -STEP);
                }
            }
            if (current != before || force || calib) {
                BrightnessSet(current);
                int r = BrightnessGet();       // certains ecrans arrondissent (ex. 43 -> 40)
                expectRead = r >= 0 ? r : -2;
            }
        }
        g_bright = current;

        // 4) Historique : un point par minute
        LONG m = NowMinute();
        if (m != lastHistMinute) {
            lastHistMinute = m;
            HistAdd(smooth >= 0 ? (int)lround(smooth * 100) : -1, current);
        }

        PostMessageW(g_hwnd, WM_UPDATE, 0, 0);
        if (WaitForMultipleObjects(2, evs, FALSE, 1000) == WAIT_OBJECT_0) break;
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
    DWORD ref = RegGet(L"CamRef", 0);
    if (ref >= 100 && ref < 100000000) g_camRef = ref / 100.0;
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
