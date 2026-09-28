// Luminosity Manager - ajuste la luminosite de l'ecran selon la lumiere ambiante.
// Win32 natif, sans dependances, icone dans la zone de notification uniquement.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <initguid.h>
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <propkeydef.h>
#include <sensorsapi.h>
#include <sensors.h>
#include <wbemidl.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <strmif.h>
#include <math.h>
#include <wchar.h>

#define WM_TRAY   (WM_APP + 1)
#define WM_UPDATE (WM_APP + 2)
#define WM_NOSENSOR (WM_APP + 3)

enum { ID_INFO = 100, ID_AUTO, ID_BRIGHTER, ID_DARKER, ID_RESET, ID_STARTUP, ID_CAMERA, ID_QUIT };

static const wchar_t *APP_NAME = L"Luminosity Manager";
static const wchar_t *REG_KEY  = L"Software\\LuminosityManager";
static const wchar_t *RUN_KEY  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static const int MIN_BRIGHT = 10, MAX_BRIGHT = 100;
static const int STEP = 5, HYSTERESIS = 3;

static HWND g_hwnd;
static NOTIFYICONDATAW g_nid;
static UINT g_taskbarCreated;
static HANDLE g_quitEvent, g_wakeEvent;

static volatile LONG g_enabled = 1;
static volatile LONG g_offset = 0;       // -50..+50
static volatile LONG g_lux = -1;         // -1 = pas de capteur
static volatile LONG g_bright = -1;      // luminosite actuelle appliquee
enum { SRC_SENSOR, SRC_CAMERA, SRC_SUN };
static volatile LONG g_source = SRC_SUN;
static volatile LONG g_useCam = 1;
static volatile LONG g_camLevel = 0;     // 0..255
static volatile LONG g_sunElev = 0;      // degres
static LONG g_lat = 4559, g_lon = -7344; // Boucherville (x100)

// ---------- Reglages (registre) ----------
static DWORD RegGet(const wchar_t *name, DWORD def) {
    DWORD v, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        return v;
    return def;
}
static void RegPut(const wchar_t *name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
}
static bool StartupEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}
static void SetStartup(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH], cmd[MAX_PATH + 4];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        swprintf(cmd, MAX_PATH + 4, L"\"%ls\"", path);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

// ---------- Capteur de lumiere ----------
static ISensor *OpenLightSensor() {
    ISensorManager *mgr = NULL;
    ISensorCollection *col = NULL;
    ISensor *sensor = NULL;
    if (FAILED(CoCreateInstance(CLSID_SensorManager, NULL, CLSCTX_INPROC_SERVER, IID_ISensorManager, (void **)&mgr)))
        return NULL;
    if (SUCCEEDED(mgr->GetSensorsByType(SENSOR_TYPE_AMBIENT_LIGHT, &col))) {
        ULONG n = 0;
        if (SUCCEEDED(col->GetCount(&n)) && n > 0) col->GetAt(0, &sensor);
        col->Release();
    }
    mgr->Release();
    return sensor;
}

static bool ReadLux(ISensor *s, double *lux) {
    ISensorDataReport *rep = NULL;
    if (FAILED(s->GetData(&rep))) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(rep->GetSensorValue(SENSOR_DATA_TYPE_LIGHT_LEVEL_LUX, &v))) {
        if (v.vt == VT_R4) { *lux = v.fltVal; ok = true; }
        else if (v.vt == VT_R8) { *lux = v.dblVal; ok = true; }
    }
    PropVariantClear(&v);
    rep->Release();
    return ok;
}

// ---------- Luminosite : ecran interne (WMI) ----------
static IWbemServices *ConnectWmi() {
    IWbemLocator *loc = NULL;
    IWbemServices *svc = NULL;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void **)&loc)))
        return NULL;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = loc->ConnectServer(ns, NULL, NULL, NULL, 0, NULL, NULL, &svc);
    SysFreeString(ns);
    loc->Release();
    if (FAILED(hr)) return NULL;
    CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    return svc;
}

static int WmiGetBrightness(IWbemServices *svc) {
    if (!svc) return -1;
    int result = -1;
    IEnumWbemClassObject *en = NULL;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(L"SELECT CurrentBrightness FROM WmiMonitorBrightness");
    if (SUCCEEDED(svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en))) {
        IWbemClassObject *obj = NULL;
        ULONG got = 0;
        if (en->Next(2000, 1, &obj, &got) == S_OK && got) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v, NULL, NULL))) {
                if (v.vt == VT_UI1) result = v.bVal;
                else if (v.vt == VT_I4) result = v.lVal;
            }
            VariantClear(&v);
            obj->Release();
        }
        en->Release();
    }
    SysFreeString(lang);
    SysFreeString(q);
    return result;
}

static bool WmiSetBrightness(IWbemServices *svc, int pct) {
    if (!svc) return false;
    bool any = false;
    IWbemClassObject *cls = NULL, *inDef = NULL;
    BSTR clsName = SysAllocString(L"WmiMonitorBrightnessMethods");
    BSTR method = SysAllocString(L"WmiSetBrightness");
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessMethods");
    if (SUCCEEDED(svc->GetObject(clsName, 0, NULL, &cls, NULL)) &&
        SUCCEEDED(cls->GetMethod(method, 0, &inDef, NULL))) {
        IEnumWbemClassObject *en = NULL;
        if (SUCCEEDED(svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en))) {
            IWbemClassObject *obj = NULL;
            ULONG got = 0;
            while (en->Next(2000, 1, &obj, &got) == S_OK && got) {
                VARIANT path;
                VariantInit(&path);
                if (SUCCEEDED(obj->Get(L"__PATH", 0, &path, NULL, NULL)) && path.vt == VT_BSTR) {
                    IWbemClassObject *in = NULL;
                    if (SUCCEEDED(inDef->SpawnInstance(0, &in))) {
                        VARIANT t, b;
                        t.vt = VT_I4;  t.lVal = 0;
                        b.vt = VT_UI1; b.bVal = (BYTE)pct;
                        in->Put(L"Timeout", 0, &t, 0);
                        in->Put(L"Brightness", 0, &b, 0);
                        if (SUCCEEDED(svc->ExecMethod(path.bstrVal, method, 0, NULL, in, NULL, NULL))) any = true;
                        in->Release();
                    }
                }
                VariantClear(&path);
                obj->Release();
            }
            en->Release();
        }
    }
    if (inDef) inDef->Release();
    if (cls) cls->Release();
    SysFreeString(clsName); SysFreeString(method); SysFreeString(lang); SysFreeString(q);
    return any;
}

// ---------- Luminosite : ecrans externes (DDC/CI) ----------
static BOOL CALLBACK DdcMonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    int pct = (int)lp;
    DWORD n = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(mon, &n) || n == 0 || n > 8) return TRUE;
    PHYSICAL_MONITOR pm[8];
    if (!GetPhysicalMonitorsFromHMONITOR(mon, n, pm)) return TRUE;
    for (DWORD i = 0; i < n; i++) {
        DWORD mn, cur, mx;
        if (GetMonitorBrightness(pm[i].hPhysicalMonitor, &mn, &cur, &mx) && mx > mn)
            SetMonitorBrightness(pm[i].hPhysicalMonitor, mn + (mx - mn) * (DWORD)pct / 100);
    }
    DestroyPhysicalMonitors(n, pm);
    return TRUE;
}

static void ApplyBrightness(IWbemServices *svc, int pct) {
    WmiSetBrightness(svc, pct);
    EnumDisplayMonitors(NULL, NULL, DdcMonitorProc, (LPARAM)pct);
}

// ---------- Webcam (plan B sans capteur) ----------
// Prend quelques images, garde la derniere et calcule sa luminosite moyenne (0..255).
// Exposition fixee en manuel quand la camera le permet, sinon l'auto-exposition
// "corrige" l'image et fausse la mesure.
static int WebcamLevel() {
    IMFAttributes *attr = NULL;
    IMFActivate **devs = NULL;
    UINT32 count = 0;
    IMFMediaSource *src = NULL;
    if (FAILED(MFCreateAttributes(&attr, 1))) return -1;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    HRESULT hr = MFEnumDeviceSources(attr, &devs, &count);
    attr->Release();
    if (FAILED(hr)) return -1;
    if (count > 0) devs[0]->ActivateObject(IID_IMFMediaSource, (void **)&src);
    for (UINT32 i = 0; i < count; i++) devs[i]->Release();
    CoTaskMemFree(devs);
    if (!src) return -1;

    IAMCameraControl *cc = NULL;
    if (SUCCEEDED(src->QueryInterface(IID_IAMCameraControl, (void **)&cc))) {
        long mn, mx, step, def, caps;
        if (SUCCEEDED(cc->GetRange(CameraControl_Exposure, &mn, &mx, &step, &def, &caps)) &&
            (caps & CameraControl_Flags_Manual))
            cc->Set(CameraControl_Exposure, def, CameraControl_Flags_Manual);
        cc->Release();
    }

    int level = -1;
    IMFSourceReader *reader = NULL;
    IMFAttributes *ra = NULL;
    MFCreateAttributes(&ra, 1);
    if (ra) ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(src, ra, &reader))) {
        IMFMediaType *mt = NULL;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, mt))) {
            IMFSample *last = NULL;
            int frames = 0;
            for (int tries = 0; tries < 40 && frames < 8; tries++) {   // les 1res images sont sombres
                DWORD idx, flags; LONGLONG ts; IMFSample *smp = NULL;
                if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &smp))) break;
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
                if (smp) { if (last) last->Release(); last = smp; frames++; }
            }
            if (last) {
                IMFMediaBuffer *buf = NULL;
                if (SUCCEEDED(last->ConvertToContiguousBuffer(&buf))) {
                    BYTE *p; DWORD len;
                    if (SUCCEEDED(buf->Lock(&p, NULL, &len))) {
                        unsigned long long sum = 0, n = 0;
                        for (DWORD i = 0; i + 3 < len; i += 4 * 16) {   // 1 pixel sur 16 suffit
                            sum += (p[i + 2] * 77 + p[i + 1] * 150 + p[i] * 29) >> 8;
                            n++;
                        }
                        if (n) level = (int)(sum / n);
                        buf->Unlock();
                    }
                    buf->Release();
                }
                last->Release();
            }
        }
        mt->Release();
        reader->Release();
    }
    if (ra) ra->Release();
    src->Shutdown();
    src->Release();
    return level;
}

// ---------- Soleil (plan C : ni capteur ni webcam) ----------
// Hauteur du soleil (degres) a la position donnee, pour l'heure actuelle.
static double SunElevation(double lat, double lon) {
    SYSTEMTIME t;
    GetSystemTime(&t);
    static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int day = cum[t.wMonth - 1] + t.wDay;
    const double PI = 3.14159265358979, R = PI / 180;
    double decl = 23.44 * sin(2 * PI * (284 + day) / 365.0);
    double B = 2 * PI * (day - 81) / 364.0;
    double eot = 9.87 * sin(2 * B) - 7.53 * cos(B) - 1.5 * sin(B);
    double solarMin = t.wHour * 60 + t.wMinute + lon * 4 + eot;
    double ha = solarMin / 4 - 180;
    double s = sin(lat * R) * sin(decl * R) + cos(lat * R) * cos(decl * R) * cos(ha * R);
    return asin(s) / R;
}

// Niveau de lumiere ambiante 0..1 -> pourcentage de luminosite
static int LevelToPercent(double t) {
    if (t > 1) t = 1;
    if (t < 0) t = 0;
    return MIN_BRIGHT + (int)lround(t * (MAX_BRIGHT - MIN_BRIGHT));
}

// ---------- Boucle de travail (fil separe) ----------
static DWORD WINAPI Worker(LPVOID) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         NULL, EOAC_NONE, NULL);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    IWbemServices *svc = ConnectWmi();
    ISensor *sensor = OpenLightSensor();

    double smooth = -1;      // niveau lisse 0..1
    int current = WmiGetBrightness(svc);
    bool moving = false, warned = false;
    int retry = 0, camTimer = 0;
    HANDLE evs[2] = { g_quitEvent, g_wakeEvent };

    for (;;) {
        if (!sensor && ++retry >= 15) { retry = 0; sensor = OpenLightSensor(); }

        double level = -1;
        double lux;
        if (sensor && ReadLux(sensor, &lux)) {
            level = log10(lux + 1.0) / log10(1001.0);   // 0 lux -> 0, 1000 lux -> 1
            g_lux = (LONG)lround(lux);
            g_source = SRC_SENSOR;
        } else {
            if (sensor) { sensor->Release(); sensor = NULL; }
            g_lux = -1;
            if (g_useCam) {
                if (--camTimer <= 0 || g_source != SRC_CAMERA) {
                    camTimer = 30;                       // une mesure toutes les 30 s
                    int y = WebcamLevel();
                    if (y >= 0) { g_camLevel = y; g_source = SRC_CAMERA; }
                    else g_source = SRC_SUN;
                }
            } else {
                g_source = SRC_SUN;
            }
            if (g_source == SRC_CAMERA) level = sqrt(g_camLevel / 200.0);
            else {
                double e = SunElevation(g_lat / 100.0, g_lon / 100.0);
                g_sunElev = (LONG)lround(e);
                level = (e + 6) / 36.0;                  // nuit -> 0, soleil a 30 deg -> 1
            }
            if (!warned) { warned = true; PostMessageW(g_hwnd, WM_NOSENSOR, 0, 0); }
        }

        if (level >= 0) {
            if (level > 1) level = 1;
            smooth = smooth < 0 ? level : smooth * 0.7 + level * 0.3;
        }

        if (g_enabled && smooth >= 0) {
            int target = LevelToPercent(smooth) + g_offset;
            if (target < 1) target = 1;
            if (target > 100) target = 100;
            if (current < 0) {
                current = target;
                ApplyBrightness(svc, current);
            } else {
                int diff = target - current;
                if (abs(diff) >= HYSTERESIS) moving = true;
                if (moving) {
                    if (diff == 0) moving = false;
                    else {
                        current += diff > 0 ? (diff < STEP ? diff : STEP) : (diff > -STEP ? diff : -STEP);
                        ApplyBrightness(svc, current);
                    }
                }
            }
        }
        g_bright = current;
        PostMessageW(g_hwnd, WM_UPDATE, 0, 0);

        DWORD w = WaitForMultipleObjects(2, evs, FALSE, 1000);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_OBJECT_0 + 1) camTimer = 0;       // reglage change : remesurer
    }

    if (sensor) sensor->Release();
    if (svc) svc->Release();
    MFShutdown();
    CoUninitialize();
    return 0;
}

// ---------- Icone de notification ----------
static void DescribeSource(wchar_t *out, int n) {
    if (g_source == SRC_SENSOR)
        swprintf(out, n, L"Capteur : %ld lux", (long)g_lux);
    else if (g_source == SRC_CAMERA)
        swprintf(out, n, L"Webcam : lumière %ld %%", (long)(g_camLevel * 100 / 255));
    else
        swprintf(out, n, L"Soleil : %+ld° au-dessus de l'horizon", (long)g_sunElev);
}

static void UpdateTip() {
    wchar_t src[80];
    DescribeSource(src, 80);
    swprintf(g_nid.szTip, 128, L"%ls\n%ls\nÉcran : %ld %%", APP_NAME, src, g_bright < 0 ? 0L : (long)g_bright);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void AddTrayIcon() {
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void ShowMenu() {
    wchar_t src[80], info[128];
    DescribeSource(src, 80);
    swprintf(info, 128, L"%ls  —  Écran : %ld %%", src, g_bright < 0 ? 0L : (long)g_bright);
    wchar_t off[64];
    swprintf(off, 64, L"Réinitialiser le réglage (actuel : %+ld %%)", (long)g_offset);

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, ID_INFO, info);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_AUTO, L"Adaptation automatique");
    AppendMenuW(m, MF_STRING, ID_BRIGHTER, L"Plus clair (+10 %)");
    AppendMenuW(m, MF_STRING, ID_DARKER, L"Plus sombre (−10 %)");
    AppendMenuW(m, MF_STRING | (g_offset ? 0 : MF_GRAYED), ID_RESET, off);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_useCam ? MF_CHECKED : 0), ID_CAMERA, L"Utiliser la webcam si pas de capteur");
    AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Lancer au démarrage de Windows");
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quitter");

    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void ChangeOffset(LONG delta) {
    LONG o = delta == 0 ? 0 : g_offset + delta;
    if (o > 50) o = 50;
    if (o < -50) o = -50;
    g_offset = o;
    RegPut(L"Offset", (DWORD)o);
    SetEvent(g_wakeEvent);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) ShowMenu();
        return 0;
    case WM_UPDATE:
        UpdateTip();
        return 0;
    case WM_NOSENSOR:
        g_nid.uFlags = NIF_INFO;
        wcscpy(g_nid.szInfoTitle, APP_NAME);
        wcscpy(g_nid.szInfo, g_source == SRC_CAMERA
            ? L"Pas de capteur de lumière : mesure avec la webcam toutes les 30 s."
            : L"Pas de capteur ni de webcam : réglage selon l'heure et le soleil.");
        g_nid.dwInfoFlags = NIIF_WARNING;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_AUTO:     g_enabled = !g_enabled; RegPut(L"Enabled", g_enabled); SetEvent(g_wakeEvent); break;
        case ID_BRIGHTER: ChangeOffset(+10); break;
        case ID_DARKER:   ChangeOffset(-10); break;
        case ID_RESET:    ChangeOffset(0); break;
        case ID_CAMERA:   g_useCam = !g_useCam; RegPut(L"UseCamera", g_useCam); SetEvent(g_wakeEvent); break;
        case ID_STARTUP:  SetStartup(!StartupEnabled()); break;
        case ID_QUIT:     DestroyWindow(h); break;
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_taskbarCreated && msg) { AddTrayIcon(); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"LuminosityManager_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_enabled = RegGet(L"Enabled", 1) ? 1 : 0;
    g_offset = (LONG)RegGet(L"Offset", 0);
    if (g_offset > 50 || g_offset < -50) g_offset = 0;
    g_useCam = RegGet(L"UseCamera", 1) ? 1 : 0;
    g_lat = (LONG)RegGet(L"Latitude", (DWORD)g_lat);    // degres x100
    g_lon = (LONG)RegGet(L"Longitude", (DWORD)g_lon);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"LuminosityManagerWnd";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, APP_NAME, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy(g_nid.szTip, APP_NAME);
    AddTrayIcon();

    g_quitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_wakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, Worker, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SetEvent(g_quitEvent);
    WaitForSingleObject(th, 5000);
    ReleaseMutex(mutex);
    return 0;
}
