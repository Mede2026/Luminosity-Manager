// Luminosity Manager - declarations partagees entre les fichiers.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // Windows 10+ (DPI par ecran)
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <math.h>
#include <wchar.h>
#include <stdlib.h>

// Version injectee par build.sh a partir du fichier VERSION
#ifndef APP_VERSION_STR
#define APP_VERSION_STR "0.0"
#endif
#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)
#define APP_VERSION WIDEN(APP_VERSION_STR)

#define APP_NAME L"Luminosity Manager"
#define REG_KEY  L"Software\\LuminosityManager"
#define REPO     L"Mede2026/Luminosity-Manager"

// Messages internes
#define WM_TRAY          (WM_APP + 1)
#define WM_UPDATE        (WM_APP + 2)   // nouvelle mesure (chaque seconde)
#define WM_NOSENSOR      (WM_APP + 3)
#define WM_UPDATE_FOUND  (WM_APP + 4)   // wParam : 1 = nouvelle version, 0 = a jour, 2 = erreur
#define WM_UPDATE_READY  (WM_APP + 5)   // wParam : 1 = nouveau .exe en place, 0 = echec
#define WM_CITY_FOUND    (WM_APP + 6)   // wParam : 1 = trouvee
#define WM_LEARNED       (WM_APP + 7)   // wParam : ecart appris (luminosite changee a la main)

enum { SRC_SENSOR, SRC_CAMERA, SRC_SUN };
enum { HK_UP, HK_DOWN, HK_MEASURE, HK_TOGGLE, HK_COUNT };

static const int MIN_INTERVAL = 5, MAX_INTERVAL = 3600;   // secondes entre 2 photos webcam
static const int HIST_LEN = 1440;                         // 24 h, un point par minute
static const BYTE HIST_NONE = 255;

// ---------- Etat partage (ecrit par un fil, lu par l'autre) ----------
extern volatile LONG g_enabled, g_offset, g_min, g_max;
extern volatile LONG g_lux, g_bright, g_detected, g_source;
extern volatile LONG g_useCam, g_camLevel, g_sunElev, g_camInterval, g_camNext;
extern volatile LONG g_remeasure, g_force, g_nudge;
extern volatile LONG g_calibrate;                // 1 = prendre la lumiere actuelle comme reference
extern volatile LONG g_lastMeasure;              // GetTickCount de la derniere mesure
extern volatile LONG g_camQuality;               // 0 = exposition fixee, 1 = auto lue, 2 = auto inconnue
extern volatile LONG g_camExposure;              // exposition utilisee a la derniere photo
extern double g_camRef;                          // clarte de reference (= 300 lux)
extern wchar_t g_camChoice[128];                 // camera choisie ("" = automatique)
extern volatile LONG g_camLocked, g_camLockValue;  // exposition verrouillee par l'utilisateur
extern volatile LONG g_camExpMin, g_camExpMax, g_camExpStep, g_camLogUnits, g_camManualOk;
extern volatile LONG g_camClip;                  // derniere photo : 0 ok, 1 trop sombre, 2 trop claire
extern volatile LONG g_level;                    // position sur la courbe 0..1000
extern volatile LONG g_applied, g_learnNow;      // luminosite voulue ; ajout appris a ce niveau

// La courbe lumiere -> luminosite, et ce que l'app a appris de toi
static const int CURVE_POINTS = 11, LEARN_POINTS = 5;
extern const double CURVE[CURVE_POINTS][2];      // { lux, position 0..1 }
extern double g_learn[LEARN_POINTS];             // ajouts appris a 0 %, 25 %, 50 %, 75 %, 100 % de la courbe
double CurveT(double lux);
void ResetLearning();
extern volatile LONG g_lat, g_lon;                // degres x100
extern volatile LONG g_profilePct;                // -1 = aucun profil actif
extern volatile LONG g_updateCheck;
extern CRITICAL_SECTION g_lock;                   // protege les textes et la liste de profils
extern wchar_t g_profileApp[64];                  // app du profil actif
extern wchar_t g_lastApp[64];                     // derniere app au premier plan (hors nous)
extern HWND g_hwnd;
extern HANDLE g_wakeEvent;

// Profils par application
struct Profile { wchar_t exe[64]; int pct; };
static const int MAX_PROFILES = 32;
extern Profile g_profiles[MAX_PROFILES];
extern int g_profileCount;
void LoadProfiles();
void SaveProfiles();

// Historique (graphique)
struct Sample { BYTE light, bright; };            // 0..100, HIST_NONE = pas de donnee
extern Sample g_hist[HIST_LEN];
extern volatile LONG g_histLast;                  // derniere minute ecrite (minutes locales)
LONG NowMinute();
void HistLoad();
void HistSave();

// Registre
DWORD RegGet(const wchar_t *name, DWORD def);
void RegPut(const wchar_t *name, DWORD v);
bool RegGetStr(const wchar_t *name, wchar_t *out, DWORD chars);
void RegPutStr(const wchar_t *name, const wchar_t *v);
bool StartupEnabled();
void SetStartup(bool on);

// light.cpp : mesure de la lumiere
bool SensorReadLux(double *lux, bool retryNow);
void SensorClose();
static const int THUMB_W = 64, THUMB_H = 48;
struct CamShot {
    int mean;                  // clarte moyenne de l'image (0..255)
    double lin;                // lumiere reelle moyenne de l'image (0..1, sans gamma, lampes ignorees)
    int clip;                  // 0 = bien exposee, 1 = trop sombre, 2 = trop claire
    bool hasExp, manual, logUnits;
    long exp, expDef, expMin, expMax, expStep;
};
extern BYTE g_thumb[THUMB_W * THUMB_H];          // miniature en gris de la derniere photo
extern volatile LONG g_thumbValid;
extern wchar_t g_camUsed[128];                   // camera reellement utilisee
int ListCameras(wchar_t names[][128], int max);
bool WebcamMeasure(const wchar_t *preferred, long *exposure, bool lockExposure, CamShot *out);
double SunElevation(double lat, double lon);

// brightness.cpp : luminosite des ecrans
void BrightnessInit();
int BrightnessGet();
void BrightnessSet(int pct);
void BrightnessShutdown();

// net.cpp : internet (mises a jour, ville)
extern wchar_t g_newVersion[32];
extern wchar_t g_cityResult[160];
void StartUpdateCheck();
void StartUpdateDownload();
void StartCitySearch(const wchar_t *city);
bool LaunchUpdatedAndExit();

// json.cpp : lire / ecrire du JSON simple
bool JsonString(const char *src, const char *key, wchar_t *out, int n);
bool JsonNumber(const char *src, const char *key, double *v);
struct JsonOut {                                  // construit un texte JSON (a liberer avec free(buf))
    wchar_t *buf = NULL;
    size_t len = 0, cap = 0;
    void Grow(size_t need);
    void Raw(const wchar_t *fmt, ...);
    void Str(const wchar_t *s);
    void Key(const wchar_t *k);
    void Num(const wchar_t *k, double v);
    void Bool(const wchar_t *k, bool v);
    void KStr(const wchar_t *k, const wchar_t *v);
};

// webview.cpp : le navigateur integre (WebView2) qui affiche l'interface
bool WebViewCreate(HWND parent, bool transparent, COLORREF bg);   // false = WebView2 introuvable
void WebViewDestroy();
void WebViewResize();
void WebViewMoved();
bool WebViewPost(const wchar_t *json);            // envoie un message a la page
bool WebViewAlive();
void OnPageMessage(const char *json);             // ui.cpp : message recu de la page (UTF-8)
void OnWebViewFailed();                           // ui.cpp : WebView2 n'a pas pu demarrer

// ui.cpp : fenetre et icone
bool CreateMainWindow(HINSTANCE inst, bool showWindow, int show);
void RegisterHotkeys();
extern WORD g_hotkeys[HK_COUNT];
extern HANDLE g_mutex;

// main.cpp
DWORD WINAPI Worker(LPVOID);
