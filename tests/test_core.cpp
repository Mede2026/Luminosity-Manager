// Tests automatiques des calculs de Luminosity Manager (lances par GitHub Actions a chaque envoi).
// Compiler et lancer : ./tests/run.sh
#include "../src/core.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail, g_count;
#define CHECK(cond) do { g_count++; if (!(cond)) { g_fail++; printf("ECHEC ligne %d : %s\n", __LINE__, #cond); } } while (0)
static bool Near(double a, double b, double tol) { return fabs(a - b) <= tol; }

static void TestCurve() {
    CHECK(Near(CurveT(0), 0, 1e-9));
    CHECK(Near(CurveT(200), 0.52, 1e-9));           // points de la courbe respectes
    CHECK(Near(CurveT(8000), 1, 1e-9));
    CHECK(Near(CurveT(1e6), 1, 1e-9));               // au-dela : reste a 1
    CHECK(Near(CurveT(-5), 0, 1e-9));                // valeur impossible : 0
    double prev = -1;                                // toujours croissante
    for (double lux = 0; lux <= 10000; lux += 7) { double t = CurveT(lux); CHECK(t >= prev); prev = t; }
}

static void TestLearning() {
    double learn[LEARN_POINTS] = {};
    for (double t = 0; t <= 1; t += 0.05) {          // apprendre +10 a une position donne +10 a cette position
        double l[LEARN_POINTS] = {};
        LearnApply(l, 10, t);
        CHECK(Near(LearnAt(l, t), 10, 1e-9));
    }
    LearnApply(learn, 20, 0);                        // regle dans le noir...
    CHECK(Near(LearnAt(learn, 0), 20, 1e-9));
    CHECK(Near(LearnAt(learn, 1), 0, 1e-9));         // ...le plein jour ne change pas
    for (int i = 0; i < 10; i++) LearnApply(learn, 30, 0);
    CHECK(learn[0] <= 60);                           // limite +60 %
}

static void TestSun() {
    // Boucherville, 21 juin, midi solaire (~16h55 UTC) : soleil haut ; minuit : sous l'horizon
    CHECK(SunElevationAt(45.59, -73.44, 6, 21, 16, 55) > 60);
    CHECK(SunElevationAt(45.59, -73.44, 6, 21, 5, 0) < 0);
    CHECK(SunElevationAt(45.59, -73.44, 12, 21, 17, 0) < 25);   // hiver : soleil bas
    CHECK(SunLux(-20) < SunLux(0) && SunLux(0) < SunLux(40));
    CHECK(Near(SunKelvin(-30), 2900, 1e-9) && Near(SunKelvin(60), 6500, 1e-9));
}

static void TestColor() {
    double f[3];
    DisplayFactors(6500, f);                         // blanc normal : pas de changement
    CHECK(Near(f[0], 1, 1e-6) && Near(f[1], 1, 1e-6) && Near(f[2], 1, 1e-6));
    DisplayFactors(4700, f);                         // plus chaud : moins de bleu, jamais au-dessus de 1
    CHECK(f[2] < f[1] && f[1] <= f[0] && f[0] <= 1 && f[2] > 0.5);
    DisplayFactors(7200, f);                         // plus froid : moins de rouge
    CHECK(f[0] < f[2] && f[2] <= 1);
    double k = RgbToKelvin(1, 1, 1);                 // blanc sRGB = environ 6500 K
    CHECK(k > 6000 && k < 7000);
    CHECK(RgbToKelvin(1, 0.6, 0.3) < 4000);          // lumiere orangee = chaude
}

static void TestFaceZone() {
    CHECK(InFaceZone(0.5, 0.55));                    // centre : visage ignore
    CHECK(!InFaceZone(0.05, 0.05));                  // coins et bords : mesures
    CHECK(!InFaceZone(0.95, 0.5));
    CHECK(!InFaceZone(0.5, 0.05));                   // plafond
}

static void TestCategories() {
    CHECK(LightCategory(2) == 0 && LightCategory(30) == 1 && LightCategory(200) == 2);
    CHECK(LightCategory(1000) == 3 && LightCategory(50000) == 4);
    CHECK(LightCategory(10) == 1 && LightCategory(9.9) == 0);        // limites
}

static void TestVersions() {
    CHECK(IsNewerVersion(L"v0.10", L"0.9"));
    CHECK(IsNewerVersion(L"v0.7", L"0.6"));
    CHECK(!IsNewerVersion(L"v0.6", L"0.6"));
    CHECK(!IsNewerVersion(L"v0.5", L"0.6"));
    CHECK(IsNewerVersion(L"v1.0", L"0.99"));
    CHECK(IsNewerVersion(L"v0.6.1", L"0.6"));
}

static void TestSha() {
    char h[65];
    CHECK(ParseSha256("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855  LuminosityManager.exe\n", h));
    CHECK(strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    CHECK(!ParseSha256("pas une empreinte", h));
    CHECK(!ParseSha256("e3b0c442", h));               // trop court
}

int main() {
    TestCurve();
    TestLearning();
    TestSun();
    TestColor();
    TestFaceZone();
    TestCategories();
    TestVersions();
    TestSha();
    printf("%d verifications, %d echec(s)\n", g_count, g_fail);
    return g_fail ? 1 : 0;
}
