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

static IWbemServices *g_svc;

void BrightnessInit() { g_svc = ConnectWmi(); }
int BrightnessGet() { return WmiGetBrightness(g_svc); }
void BrightnessSet(int pct) { ApplyBrightness(g_svc, pct); }
void BrightnessShutdown() {
    if (g_svc) { g_svc->Release(); g_svc = NULL; }
}
