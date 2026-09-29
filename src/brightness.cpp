// Luminosite des ecrans : WMI (ecran de portable) et DDC/CI (ecrans externes).
#include "app.h"
#include <initguid.h>
#include <objbase.h>
#include <wbemidl.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>

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

// Le chemin WMI de chaque ecran de portable et la "fiche" de la methode sont gardes en memoire :
// on ne les recherche qu'une fois (ou quand les ecrans changent), pas a chaque petit pas.
static const int MAX_WMI = 4;
static BSTR g_wmiPaths[MAX_WMI];
static int g_wmiCount = -1;                  // -1 = a rechercher
static IWbemClassObject *g_inDef;

static void WmiForget() {
    for (int i = 0; i < MAX_WMI; i++) if (g_wmiPaths[i]) { SysFreeString(g_wmiPaths[i]); g_wmiPaths[i] = NULL; }
    if (g_inDef) { g_inDef->Release(); g_inDef = NULL; }
    g_wmiCount = -1;
}

static void WmiFind(IWbemServices *svc) {
    WmiForget();
    g_wmiCount = 0;
    IWbemClassObject *cls = NULL;
    BSTR clsName = SysAllocString(L"WmiMonitorBrightnessMethods");
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessMethods");
    if (SUCCEEDED(svc->GetObject(clsName, 0, NULL, &cls, NULL)) &&
        SUCCEEDED(cls->GetMethod(L"WmiSetBrightness", 0, &g_inDef, NULL))) {
        IEnumWbemClassObject *en = NULL;
        if (SUCCEEDED(svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en))) {
            IWbemClassObject *obj = NULL;
            ULONG got = 0;
            while (g_wmiCount < MAX_WMI && en->Next(2000, 1, &obj, &got) == S_OK && got) {
                VARIANT path;
                VariantInit(&path);
                if (SUCCEEDED(obj->Get(L"__PATH", 0, &path, NULL, NULL)) && path.vt == VT_BSTR)
                    g_wmiPaths[g_wmiCount++] = SysAllocString(path.bstrVal);
                VariantClear(&path);
                obj->Release();
            }
            en->Release();
        }
    }
    if (cls) cls->Release();
    SysFreeString(clsName); SysFreeString(lang); SysFreeString(q);
}

static bool WmiSetBrightness(IWbemServices *svc, int pct) {
    if (!svc) return false;
    if (g_wmiCount < 0) WmiFind(svc);
    if (!g_inDef) return false;
    bool any = false, failed = false;
    BSTR method = SysAllocString(L"WmiSetBrightness");
    for (int i = 0; i < g_wmiCount; i++) {
        IWbemClassObject *in = NULL;
        if (FAILED(g_inDef->SpawnInstance(0, &in))) continue;
        VARIANT t, b;
        t.vt = VT_I4;  t.lVal = 0;
        b.vt = VT_UI1; b.bVal = (BYTE)pct;
        in->Put(L"Timeout", 0, &t, 0);
        in->Put(L"Brightness", 0, &b, 0);
        if (SUCCEEDED(svc->ExecMethod(g_wmiPaths[i], method, 0, NULL, in, NULL, NULL))) any = true;
        else failed = true;
        in->Release();
    }
    SysFreeString(method);
    if (failed) g_wmiCount = -1;             // un ecran a disparu : on recherchera la prochaine fois
    return any;
}

// ---------- Luminosite : ecrans externes (DDC/CI) ----------
// Les ecrans et leur plage de luminosite sont gardes en memoire (la lecture DDC est lente, ~50 ms).
struct DdcMonitor { HANDLE h; DWORD mn, mx; };
static const int MAX_DDC = 8;
static DdcMonitor g_ddc[MAX_DDC];
static int g_ddcCount = -1;                  // -1 = a rechercher
static volatile LONG g_displaysChanged;

static void DdcForget() {
    for (int i = 0; i < g_ddcCount; i++) DestroyPhysicalMonitor(g_ddc[i].h);
    g_ddcCount = -1;
}

static BOOL CALLBACK DdcFindProc(HMONITOR mon, HDC, LPRECT, LPARAM) {
    DWORD n = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(mon, &n) || n == 0 || n > MAX_DDC) return TRUE;
    PHYSICAL_MONITOR pm[MAX_DDC];
    if (!GetPhysicalMonitorsFromHMONITOR(mon, n, pm)) return TRUE;
    for (DWORD i = 0; i < n; i++) {
        DWORD mn, cur, mx;
        if (g_ddcCount < MAX_DDC && GetMonitorBrightness(pm[i].hPhysicalMonitor, &mn, &cur, &mx) && mx > mn)
            g_ddc[g_ddcCount++] = { pm[i].hPhysicalMonitor, mn, mx };   // ecran compatible : on le garde
        else
            DestroyPhysicalMonitor(pm[i].hPhysicalMonitor);
    }
    return TRUE;
}

static void DdcSet(int pct) {
    if (g_ddcCount < 0) { g_ddcCount = 0; EnumDisplayMonitors(NULL, NULL, DdcFindProc, 0); }
    for (int i = 0; i < g_ddcCount; i++)
        if (!SetMonitorBrightness(g_ddc[i].h, g_ddc[i].mn + (g_ddc[i].mx - g_ddc[i].mn) * (DWORD)pct / 100)) {
            DdcForget();                     // ecran debranche : on recherchera la prochaine fois
            break;
        }
}

// Appele quand les ecrans changent (branchement, resolution...)
void BrightnessDisplaysChanged() { InterlockedExchange(&g_displaysChanged, 1); }

static IWbemServices *g_svc;

void BrightnessInit() { g_svc = ConnectWmi(); }
int BrightnessGet() { return WmiGetBrightness(g_svc); }
void BrightnessSet(int pct) {
    if (g_displaysChanged) { InterlockedExchange(&g_displaysChanged, 0); DdcForget(); g_wmiCount = -1; }
    if (g_externalBrightness) DdcSet(pct);
    WmiSetBrightness(g_svc, pct);
}
void BrightnessShutdown() {
    WmiForget();
    DdcForget();
    if (g_svc) { g_svc->Release(); g_svc = NULL; }
}

// ---------- Couleur de l'ecran (True Tone) ----------
// On change la "table de couleurs" (gamma ramp) de chaque ecran : chaque canal rouge / vert / bleu
// est multiplie par un facteur (0..1). Meme technique que f.lux.
static WORD g_ramp[3][256];
static int g_rampOk;

static BOOL CALLBACK GammaMonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    HDC dc = CreateDCW(L"DISPLAY", mi.szDevice, NULL, NULL);
    if (!dc) return TRUE;
    if (SetDeviceGammaRamp(dc, g_ramp)) g_rampOk++;
    DeleteDC(dc);
    return TRUE;
}

bool ColorApply(double r, double g, double b) {
    const double k[3] = { r, g, b };
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) {
            double v = i * 257.0 * k[c];
            g_ramp[c][i] = (WORD)(v > 65535 ? 65535 : v);
        }
    g_rampOk = 0;
    EnumDisplayMonitors(NULL, NULL, GammaMonitorProc, 0);
    return g_rampOk > 0;
}

void ColorReset() { ColorApply(1, 1, 1); }
