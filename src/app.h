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
#include "core.h"

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
#define WM_UPDATE_READY  (WM_APP + 5)   // wParam : 1 = nouveau .exe en place, 0 = echec, 2 = bloque par Securite Windows
#define WM_CITY_FOUND    (WM_APP + 6)   // wParam : 1 = trouvee
#define WM_LEARNED       (WM_APP + 7)   // wParam : ecart appris (luminosite changee a la main)
#define WM_IDLE_WATCH    (WM_APP + 9)   // wParam : 1 = ecouter la souris (absent), 0 = arreter
#define WM_QUIT_APP      (WM_APP + 8)   // quitter (demande par le desinstalleur)
#define STARTED_EVENT    L"Local\\LuminosityManager_Started"   // la nouvelle version a bien demarre

enum { SRC_SENSOR, SRC_CAMERA, SRC_SUN };
enum { HK_UP, HK_DOWN, HK_MEASURE, HK_TOGGLE, HK_READ, HK_COUNT };

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
extern volatile LONG g_trueTone, g_ttStrength;   // True Tone active ; intensite 0..100
extern volatile LONG g_ambientK, g_displayK;     // couleur de la lumiere / du blanc de l'ecran (Kelvin)
extern volatile LONG g_ttSource, g_ttOk;         // 1 capteur, 2 webcam, 3 heure ; 0 = Windows a refuse

// Pauses, batterie, mode lecture (notifications de Windows recues par la fenetre)
extern volatile LONG g_locked, g_displayOff, g_suspended;   // session verrouillee / ecran eteint / veille
extern volatile LONG g_onBattery, g_batterySaver;
extern volatile LONG g_pauseFullscreen;          // reglage : pause pendant les jeux / videos en plein ecran
extern volatile LONG g_batteryMode, g_batteryCut;// economie d'energie : 0 jamais, 1 sur batterie, 2 economiseur ; % en moins
extern volatile LONG g_ecoActive;                // economie d'energie en cours
extern volatile LONG g_uiVisible;                // la fenetre est ouverte (mesures plus frequentes)
extern volatile LONG g_readMode;                 // mode lecture actif
extern volatile LONG g_pauseReason;              // 0 aucune, 1 verrouille, 2 ecran eteint, 3 veille, 4 plein ecran, 5 jeu
extern volatile LONG g_camBusy;                  // la webcam est utilisee par une autre app
extern volatile LONG g_osBright, g_osBrightSeq;  // luminosite annoncee par Windows (notification)
enum { PAUSE_NONE, PAUSE_LOCKED, PAUSE_SCREEN_OFF, PAUSE_SLEEP, PAUSE_FULLSCREEN, PAUSE_GAME, PAUSE_IDLE };
extern volatile LONG g_idlePause, g_idleArmed;   // pas de photo quand tu n'es pas la ; souris ecoutee
void ForegroundChanged();                        // main.cpp : l'app au premier plan a change (Windows nous previent)
enum { TT_NONE, TT_SENSOR, TT_CAMERA, TT_SUN };

// La courbe lumiere -> luminosite (core.h), et ce que l'app a appris de toi
extern double g_learn[LEARN_POINTS];             // ajouts appris a 0 %, 25 %, 50 %, 75 %, 100 % de la courbe
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

// stats.cpp : statistiques
struct StatsSample {
    bool off, paused, reading, battery;
    int bright;                   // % de luminosite
    double lux;                   // lumiere (-1 = inconnue)
    int source;                   // 0 capteur, 1 webcam, 2 soleil
    int trueToneK;                // blanc de l'ecran (0 = True Tone coupe)
    const wchar_t *app;           // app du profil actif (ou NULL)
};
enum { STAT_PHOTO, STAT_ADJUST, STAT_MANUAL };
void StatsLoad();
void StatsSave();
void StatsMinute(const StatsSample &s);
void StatsEvent(int type);
void StatsReset();
void StatsAppUsage(double *cpuPct, double *photosPerHour, double *memMb);
struct JsonOut;
void StatsJson(JsonOut &j);
bool AppDataFile(const wchar_t *name, wchar_t *path, bool create);   // %APPDATA%\LuminosityManager\name

// backup.cpp : exporter / importer toutes les donnees
bool ExportData(wchar_t *result, int n);
bool ImportData(wchar_t *result, int n);         // true = l'app doit redemarrer
extern volatile LONG g_noSaveOnExit;
void RequestRestart();                            // net.cpp : relancer l'app en quittant

// Registre
DWORD RegGet(const wchar_t *name, DWORD def);
void RegPut(const wchar_t *name, DWORD v);
bool RegGetStr(const wchar_t *name, wchar_t *out, DWORD chars);
void RegPutStr(const wchar_t *name, const wchar_t *v);
bool StartupEnabled();
void SetStartup(bool on);

// light.cpp : mesure de la lumiere
bool SensorReadLux(double *lux, double *kelvin, bool retryNow);   // kelvin = -1 si inconnu
void SensorClose();
static const int THUMB_W = 160, THUMB_H = 120;   // miniature webcam en couleur (RGB)
struct CamShot {
    int mean;                  // clarte moyenne de l'image (0..255)
    double lin;                // lumiere reelle moyenne de l'image (0..1, sans gamma, lampes ignorees)
    int clip;                  // 0 = bien exposee, 1 = trop sombre, 2 = trop claire
    bool hasExp, manual, logUnits;
    long exp, expDef, expMin, expMax, expStep;
    double kelvin;             // couleur de la lumiere (Kelvin), -1 si inconnue
    bool kelvinFromCam;        // mesuree par la balance des blancs de la camera
};
extern BYTE *g_thumb;                            // miniature couleur (R, G, B) de la derniere photo
extern volatile LONG g_thumbValid;
extern wchar_t g_camUsed[128];                   // camera reellement utilisee
int ListCameras(wchar_t names[][128], int max);
bool WebcamMeasure(const wchar_t *preferred, long *exposure, bool lockExposure, bool quick, CamShot *out);  // quick = lumiere stable, moins d'images
double SunElevation(double lat, double lon);
bool WebcamBusyElsewhere();                      // une autre app utilise la camera (Teams, Discord...)

// brightness.cpp : luminosite des ecrans
void BrightnessInit();
int BrightnessGet();
void BrightnessSet(int pct);
void BrightnessShutdown();
void BrightnessDisplaysChanged();                  // ecrans branches / debranches
extern volatile LONG g_externalBrightness;        // regler aussi les ecrans externes (DDC/CI)
bool ColorApply(double r, double g, double b);     // True Tone : facteurs rouge / vert / bleu (0..1)
void ColorReset();                                 // couleurs normales

// net.cpp : internet (mises a jour, ville)
extern wchar_t g_newVersion[32];
extern wchar_t g_cityResult[160];
void StartUpdateCheck();
void StartUpdateDownload();
void StartCitySearch(const wchar_t *city);
void StartLocate();                              // position automatique (d'apres la connexion internet)
bool LaunchUpdatedAndExit();
enum { LAUNCH_PLAIN, LAUNCH_UPDATE, LAUNCH_INSTALL };  // si la nouvelle app ne demarre pas : rien / remettre l'ancienne / relancer celle-ci
void RequestLaunch(const wchar_t *path, const wchar_t *args, int mode);

// install.cpp : installation, copie de secours, retour arriere, desinstallation
bool InstallDir(wchar_t *dir);                   // %LOCALAPPDATA%\Programs\LuminosityManager
bool IsInstalled();                              // l'app tourne depuis ce dossier
bool InstallApp(wchar_t *target, wchar_t *msg, int n);
void UninstallApp(bool keepData);
void RefreshUninstallEntry();
bool BackupCurrent();
bool BackupFind(wchar_t *version, int vn, wchar_t *path);
bool SwapInExe(const wchar_t *newFile);
bool RestorePrevious();
bool RollbackPrepare(wchar_t *msg, int n);
bool ExeStillThere(const wchar_t *path, DWORD expectedSize);
void DeleteOldCopy(const wchar_t *path);

// json.cpp : lire / ecrire du JSON simple
bool JsonString(const char *src, const char *key, wchar_t *out, int n);
bool JsonNumber(const char *src, const char *key, double *v);
typedef void (*JsonEntryFn)(const wchar_t *key, bool isStr, const wchar_t *str, double num, void *user);
void JsonForEach(const char *json, JsonEntryFn fn, void *user);   // chaque "cle": valeur d'un objet plat
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
void WebViewFocus();                              // le clavier va a la page
void OnPageMessage(const char *json);             // ui.cpp : message recu de la page (UTF-8)
void OnWebViewFailed();                           // ui.cpp : WebView2 n'a pas pu demarrer

// ui.cpp : fenetre et icone
bool CreateMainWindow(HINSTANCE inst, bool showWindow, int show);
void RegisterHotkeys();
extern WORD g_hotkeys[HK_COUNT];
extern HANDLE g_mutex;
void StartNotice(const wchar_t *text, const wchar_t *page, bool balloon);

// main.cpp
DWORD WINAPI Worker(LPVOID);
