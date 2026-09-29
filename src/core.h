// Calculs "purs" de Luminosity Manager (sans Windows) : testes automatiquement sur GitHub (tests/).
#pragma once
#include <wchar.h>

// La courbe : lumiere de la piece (lux) -> position 0..1
static const int CURVE_POINTS = 11, LEARN_POINTS = 5;
extern const double CURVE[CURVE_POINTS][2];      // { lux, position 0..1 }
double CurveT(double lux);

// Apprentissage par niveau de lumiere : LEARN_POINTS ajouts (en %) repartis sur la courbe
double LearnAt(const double *learn, double t);
void LearnApply(double *learn, double delta, double t);   // ajoute delta a la position t (limite +/-60)

// Soleil
double SunElevationAt(double lat, double lon, int month, int day, int hourUtc, int minuteUtc);
double SunLux(double elevation);                 // lumiere typique d'une piece selon le soleil
double SunKelvin(double elevation);              // couleur typique de la lumiere selon le soleil

// Couleurs (True Tone)
void KelvinRgb(double k, double rgb[3]);         // couleur d'une lumiere a k Kelvin (0..1 par canal)
void DisplayFactors(double k, double f[3]);      // facteurs rouge/vert/bleu pour un blanc a k Kelvin (max 1)
double RgbToKelvin(double r, double g, double b);// couleur moyenne (RGB lineaire) -> Kelvin (McCamy)

// Webcam : zone du visage (au centre) ignoree, on mesure les bords. x, y entre 0 et 1.
bool InFaceZone(double x, double y);

// Statistiques : categorie de lumiere (0 nuit, 1 sombre, 2 interieur, 3 lumineux, 4 plein jour)
static const int LIGHT_CATS = 5;
int LightCategory(double lux);

// Versions : "v0.10" > "0.9"
bool IsNewerVersion(const wchar_t *remote, const wchar_t *local);

// Mises a jour : lit l'empreinte SHA-256 (64 caracteres hexa) au debut d'un fichier .sha256
bool ParseSha256(const char *text, char out[65]);

// Economie de batterie
bool QuickPhotoOk(double prevLux, double lastLux, int clip);   // lumiere stable (< 15 %) et image bien exposee
int EcoStep(int diff);                           // pas de la transition en economie d'energie (3 pas)
// Attente du fil de travail (ms) : fenetre ouverte 1 s ; capteur 2 s ; sinon jusqu'a la prochaine photo
// ou la prochaine minute (statistiques), 15 s max (30 s en economie d'energie)
unsigned NextWakeMs(bool visible, bool sensor, bool eco, unsigned msToCamera, unsigned msToMinute);
double CpuPercent(double cpuMs, double minutes); // part du processeur utilisee par l'app (%)
