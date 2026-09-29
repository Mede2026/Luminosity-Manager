// Calculs "purs" de Luminosity Manager (sans Windows) : testes automatiquement sur GitHub (tests/).
#include "core.h"
#include <math.h>
#include <ctype.h>
#include <stdlib.h>

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

// Chaque point appris couvre une zone de la courbe : un reglage fait dans le noir ne change
// que la partie "noir" de la courbe.
double LearnAt(const double *learn, double t) {
    double pos = t * (LEARN_POINTS - 1);
    int i = (int)pos;
    if (i >= LEARN_POINTS - 1) return learn[LEARN_POINTS - 1];
    if (i < 0) return learn[0];
    return learn[i] + (learn[i + 1] - learn[i]) * (pos - i);
}

void LearnApply(double *learn, double delta, double t) {
    // Poids en triangle autour de la position t. On divise par la somme des carres des poids pour que
    // la valeur lue a cette position (LearnAt) augmente d'exactement "delta" : l'app retient tout ton reglage.
    double w[LEARN_POINTS], sum2 = 0;
    for (int i = 0; i < LEARN_POINTS; i++) {
        w[i] = 1 - fabs(t * (LEARN_POINTS - 1) - i);
        if (w[i] < 0) w[i] = 0;
        sum2 += w[i] * w[i];
    }
    for (int i = 0; i < LEARN_POINTS; i++) {
        if (w[i] > 0) {
            learn[i] += delta * w[i] / sum2;
            if (learn[i] > 60) learn[i] = 60;
            if (learn[i] < -60) learn[i] = -60;
        }
    }
}

// Hauteur du soleil (degres) a une position et une heure (UTC) donnees
double SunElevationAt(double lat, double lon, int month, int day, int hourUtc, int minuteUtc) {
    static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int d = cum[(month - 1) % 12] + day;
    const double PI = 3.14159265358979, R = PI / 180;
    double decl = 23.44 * sin(2 * PI * (284 + d) / 365.0);
    double B = 2 * PI * (d - 81) / 364.0;
    double eot = 9.87 * sin(2 * B) - 7.53 * cos(B) - 1.5 * sin(B);
    double solarMin = hourUtc * 60 + minuteUtc + lon * 4 + eot;
    double ha = solarMin / 4 - 180;
    double s = sin(lat * R) * sin(decl * R) + cos(lat * R) * cos(decl * R) * cos(ha * R);
    return asin(s) / R;
}

// Soleil -> lumiere typique d'une piece (nuit ~8 lux, soleil haut ~1000 lux)
double SunLux(double elevation) {
    double k = (elevation + 6) / 36;
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    return pow(10.0, 0.9 + 2.1 * k);
}

// Lampes chaudes la nuit, lumiere du jour a midi
double SunKelvin(double e) {
    static const double P[][2] = { { -6, 2900 }, { 0, 3600 }, { 15, 5500 }, { 40, 6500 } };
    if (e <= P[0][0]) return P[0][1];
    for (int i = 1; i < 4; i++)
        if (e <= P[i][0]) return P[i - 1][1] + (P[i][1] - P[i - 1][1]) * (e - P[i - 1][0]) / (P[i][0] - P[i - 1][0]);
    return 6500;
}

// Couleur d'un corps chauffe a k Kelvin (approximation de Tanner Helland), 0..1 par canal
void KelvinRgb(double k, double rgb[3]) {
    double t = k / 100, r, g, b;
    if (t <= 66) { r = 255; g = 99.4708025861 * log(t) - 161.1195681661; }
    else { r = 329.698727446 * pow(t - 60, -0.1332047592); g = 288.1221695283 * pow(t - 60, -0.0755148492); }
    if (t >= 66) b = 255;
    else if (t <= 19) b = 0;
    else b = 138.5177312231 * log(t - 10) - 305.0447927307;
    double v[3] = { r, g, b };
    for (int i = 0; i < 3; i++) rgb[i] = (v[i] < 0 ? 0 : v[i] > 255 ? 255 : v[i]) / 255;
}

// Blanc de l'ecran a k Kelvin : facteurs par rapport au blanc normal (6500 K), sans jamais depasser 1
void DisplayFactors(double k, double f[3]) {
    double c[3], ref[3], mx = 0;
    KelvinRgb(k, c);
    KelvinRgb(6500, ref);
    for (int i = 0; i < 3; i++) { f[i] = c[i] / ref[i]; if (f[i] > mx) mx = f[i]; }
    for (int i = 0; i < 3; i++) f[i] /= mx;
}

double RgbToKelvin(double r, double g, double b) {
    double X = 0.4124 * r + 0.3576 * g + 0.1805 * b;
    double Y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    double Z = 0.0193 * r + 0.1192 * g + 0.9505 * b;
    double sum = X + Y + Z;
    if (sum <= 0) return -1;
    double x = X / sum, y = Y / sum;
    double nn = (x - 0.3320) / (0.1858 - y);
    return 449 * nn * nn * nn + 3525 * nn * nn + 6823.3 * nn + 5520.33;
}

// Le visage est en general au centre, un peu vers le bas : ellipse de 50 % de large sur 75 % de haut.
// Tout le reste (plafond, murs, cotes) sert a mesurer la lumiere de la piece.
bool InFaceZone(double x, double y) {
    double dx = (x - 0.5) / 0.25, dy = (y - 0.58) / 0.40;
    return dx * dx + dy * dy <= 1;
}

int LightCategory(double lux) {
    static const double limits[LIGHT_CATS - 1] = { 10, 80, 400, 2000 };   // lux
    int c = 0;
    while (c < LIGHT_CATS - 1 && lux >= limits[c]) c++;
    return c;
}

bool IsNewerVersion(const wchar_t *remote, const wchar_t *local) {
    if (*remote == L'v' || *remote == L'V') remote++;
    if (*local == L'v' || *local == L'V') local++;
    for (;;) {
        wchar_t *e1, *e2;
        long a = wcstol(remote, &e1, 10), b = wcstol(local, &e2, 10);
        if (a != b) return a > b;
        if (*e1 != L'.' && *e2 != L'.') return false;
        remote = *e1 == L'.' ? e1 + 1 : e1;
        local = *e2 == L'.' ? e2 + 1 : e2;
    }
}

bool ParseSha256(const char *text, char out[65]) {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    for (int i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)text[i])) return false;
        out[i] = (char)tolower((unsigned char)text[i]);
    }
    if (isxdigit((unsigned char)text[64])) return false;
    out[64] = 0;
    return true;
}

bool QuickPhotoOk(double prevLux, double lastLux, int clip) {
    if (prevLux < 0 || lastLux < 0 || clip != 0) return false;
    return fabs(log10((lastLux + 1) / (prevLux + 1))) < log10(1.15);
}

int EcoStep(int diff) {
    int d = diff < 0 ? -diff : diff;
    int s = (d + 2) / 3;                                  // arrondi au-dessus
    return s < 1 ? 1 : s;
}

unsigned NextWakeMs(bool visible, bool sensor, bool eco, unsigned msToCamera, unsigned msToMinute) {
    if (visible) return 1000;
    if (sensor) return eco ? 4000 : 2000;
    unsigned w = eco ? 30000 : 15000;
    if (msToCamera < w) w = msToCamera;
    if (msToMinute < w) w = msToMinute;
    return w < 500 ? 500 : w;
}

double CpuPercent(double cpuMs, double minutes) {
    if (minutes <= 0 || cpuMs <= 0) return 0;
    return cpuMs / (minutes * 60000.0) * 100.0;
}
