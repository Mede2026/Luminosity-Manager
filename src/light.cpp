// Mesure de la lumiere : capteur, webcam, soleil.
#include "app.h"
#include <initguid.h>
#include <objbase.h>
#include <propkeydef.h>
#include <sensorsapi.h>
#include <sensors.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <strmif.h>
#include <mfobjects.h>
#include <limits.h>

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

static bool ReadValue(ISensorDataReport *rep, REFPROPERTYKEY key, double *out) {
    PROPVARIANT v;
    PropVariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(rep->GetSensorValue(key, &v))) {
        if (v.vt == VT_R4) { *out = v.fltVal; ok = true; }
        else if (v.vt == VT_R8) { *out = v.dblVal; ok = true; }
        else if (v.vt == VT_UI4) { *out = v.ulVal; ok = true; }
    }
    PropVariantClear(&v);
    return ok;
}

// Lumiere (lux) et, si le capteur sait la mesurer, sa couleur (Kelvin)
static bool ReadLux(ISensor *s, double *lux, double *kelvin) {
    ISensorDataReport *rep = NULL;
    if (FAILED(s->GetData(&rep))) return false;
    bool ok = ReadValue(rep, SENSOR_DATA_TYPE_LIGHT_LEVEL_LUX, lux);
    double k;
    if (ReadValue(rep, SENSOR_DATA_TYPE_LIGHT_TEMPERATURE_KELVIN, &k) && k >= 1500 && k <= 15000) *kelvin = k;
    rep->Release();
    return ok;
}

static ISensor *g_sensor;
static int g_sensorRetry;

// Lit le capteur ; le recherche de nouveau toutes les 15 s s'il n'y en a pas.
bool SensorReadLux(double *lux, double *kelvin, bool retryNow) {
    *kelvin = -1;
    if (!g_sensor && (retryNow || ++g_sensorRetry >= 15)) {
        g_sensorRetry = 0;
        g_sensor = OpenLightSensor();
    }
    if (g_sensor && ReadLux(g_sensor, lux, kelvin)) return true;
    SensorClose();
    return false;
}

void SensorClose() {
    if (g_sensor) { g_sensor->Release(); g_sensor = NULL; }
}

// ---------- Webcam (plan B sans capteur) ----------
// L'app regle elle-meme l'exposition (temps pendant lequel la camera capte la lumiere) :
// comme elle connait l'exposition utilisee, elle peut calculer la vraie lumiere de la piece.
// Sans ca, l'auto-exposition de la camera "corrige" l'image et la mesure ne veut plus rien dire.
BYTE g_thumb[THUMB_W * THUMB_H];
volatile LONG g_thumbValid;
wchar_t g_camUsed[128];

// Camera infrarouge (Windows Hello) : image presque noire, inutilisable pour mesurer la lumiere
static bool IsIrCamera(const wchar_t *name) {
    return wcsstr(name, L"IR ") == name || wcsstr(name, L" IR ") || wcsstr(name, L"Hello") ||
           (wcslen(name) > 3 && wcscmp(name + wcslen(name) - 3, L" IR") == 0);
}

static IMFActivate **EnumCameras(UINT32 *count) {
    IMFAttributes *attr = NULL;
    IMFActivate **devs = NULL;
    *count = 0;
    if (FAILED(MFCreateAttributes(&attr, 1))) return NULL;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(MFEnumDeviceSources(attr, &devs, count))) { devs = NULL; *count = 0; }
    attr->Release();
    return devs;
}

static void FreeCameras(IMFActivate **devs, UINT32 count) {
    for (UINT32 i = 0; i < count; i++) devs[i]->Release();
    CoTaskMemFree(devs);
}

static void CameraName(IMFActivate *dev, wchar_t *out, int n) {
    WCHAR *name = NULL;
    UINT32 len = 0;
    out[0] = 0;
    if (SUCCEEDED(dev->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len))) {
        wcsncpy(out, name, n - 1);
        out[n - 1] = 0;
        CoTaskMemFree(name);
    }
}

int ListCameras(wchar_t names[][128], int max) {
    UINT32 count;
    IMFActivate **devs = EnumCameras(&count);
    int n = 0;
    for (UINT32 i = 0; i < count && n < max; i++) CameraName(devs[i], names[n++], 128);
    if (devs) FreeCameras(devs, count);
    return n;
}

// ---------- Analyse d'une image ----------
// Les images de webcam sont "compressees" (gamma) : une valeur 2x plus grande ne veut pas dire 2x plus de
// lumiere. On revient donc a la lumiere reelle (lineaire) avant de calculer.
static float g_linear[256];
static void InitLinear() {
    if (g_linear[255] > 0) return;
    for (int i = 0; i < 256; i++) {
        double v = i / 255.0;
        g_linear[i] = (float)(v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4));
    }
}

struct FrameStats { int mean; double lin; double dark, bright; double r, g, b; };   // dark / bright : part de pixels noirs / blancs ; r g b : couleur moyenne

static bool AnalyzeFrame(IMFSample *smp, UINT32 w, UINT32 h, bool bottomUp, bool thumb, FrameStats *st) {
    IMFMediaBuffer *buf = NULL;
    if (FAILED(smp->GetBufferByIndex(0, &buf))) return false;
    bool ok = false;
    BYTE *scan0 = NULL, *p = NULL;
    LONG pitch = 0;
    DWORD len = 0;
    IMF2DBuffer *b2 = NULL;
    bool locked2d = false, locked = false;
    if (SUCCEEDED(buf->QueryInterface(IID_IMF2DBuffer, (void **)&b2)) && SUCCEEDED(b2->Lock2D(&scan0, &pitch)))
        locked2d = true;
    else if (SUCCEEDED(buf->Lock(&p, NULL, &len)) && len >= w * h * 4) {
        locked = true;
        pitch = (LONG)(w * 4);
        scan0 = p;
        if (bottomUp) { scan0 = p + (h - 1) * w * 4; pitch = -pitch; }   // image stockee a l'envers
    }
    if (scan0 && w >= 8 && h >= 8) {
        // Histogramme de clarte (1 pixel sur 16)
        unsigned hist[256] = {};
        unsigned n = 0, nc = 0;
        double rs = 0, gs = 0, bs = 0;
        for (UINT32 y = 0; y < h; y += 4)
            for (UINT32 x = 0; x < w; x += 4) {
                const BYTE *px = scan0 + (LONG)y * pitch + x * 4;
                int luma = (px[2] * 77 + px[1] * 150 + px[0] * 29) >> 8;
                hist[luma]++;
                n++;
                // couleur de la lumiere : seulement les pixels ni trop sombres ni satures
                if (luma > 20 && luma < 235 && px[0] < 250 && px[1] < 250 && px[2] < 250) {
                    rs += g_linear[px[2]]; gs += g_linear[px[1]]; bs += g_linear[px[0]];
                    nc++;
                }
            }
        st->r = nc ? rs / nc : 0;
        st->g = nc ? gs / nc : 0;
        st->b = nc ? bs / nc : 0;
        // Moyenne "robuste" : on ignore les 5 % les plus sombres et les 5 % les plus clairs
        // (une lampe ou une fenetre dans l'image ne doit pas tout fausser)
        unsigned lo = n * 5 / 100, hi = n - n * 5 / 100, seen = 0;
        double linSum = 0, sum8 = 0, cnt = 0;
        for (int v = 0; v < 256; v++) {
            unsigned c = hist[v];
            unsigned from = seen > lo ? seen : lo, to = seen + c < hi ? seen + c : hi;
            if (to > from) {
                linSum += (double)(to - from) * g_linear[v];
                sum8 += (double)(to - from) * v;
                cnt += to - from;
            }
            seen += c;
        }
        if (cnt > 0) {
            unsigned dark = 0, bright = 0;
            for (int v = 0; v < 12; v++) dark += hist[v];
            for (int v = 244; v < 256; v++) bright += hist[v];
            st->lin = linSum / cnt;
            st->mean = (int)(sum8 / cnt);
            st->dark = (double)dark / n;
            st->bright = (double)bright / n;
            ok = true;
        }
        if (thumb) {
            for (int ty = 0; ty < THUMB_H; ty++)
                for (int tx = 0; tx < THUMB_W; tx++) {
                    const BYTE *px = scan0 + (LONG)(ty * h / THUMB_H) * pitch + (tx * w / THUMB_W) * 4;
                    g_thumb[ty * THUMB_W + tx] = (BYTE)((px[2] * 77 + px[1] * 150 + px[0] * 29) >> 8);
                }
            g_thumbValid = 1;
        }
    }
    if (locked2d) b2->Unlock2D();
    if (b2) b2->Release();
    if (locked) buf->Unlock();
    buf->Release();
    return ok;
}

// Lit des images, laisse la camera se stabiliser, puis fait la moyenne des 3 dernieres (moins de bruit)
static bool ReadFrames(IMFSourceReader *reader, int frames, UINT32 w, UINT32 h, bool bottomUp, FrameStats *out) {
    FrameStats acc = {};
    int used = 0, got = 0;
    for (int tries = 0; tries < frames * 4 && got < frames; tries++) {
        DWORD idx, flags; LONGLONG ts; IMFSample *smp = NULL;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &smp))) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!smp) continue;
        got++;
        FrameStats st;
        if (got > frames - 3 && AnalyzeFrame(smp, w, h, bottomUp, got == frames, &st)) {
            acc.lin += st.lin; acc.mean += st.mean; acc.dark += st.dark; acc.bright += st.bright;
            acc.r += st.r; acc.g += st.g; acc.b += st.b;
            used++;
        }
        smp->Release();
    }
    if (!used) return false;
    out->lin = acc.lin / used;
    out->mean = acc.mean / used;
    out->dark = acc.dark / used;
    out->bright = acc.bright / used;
    out->r = acc.r / used;
    out->g = acc.g / used;
    out->b = acc.b / used;
    return true;
}

bool WebcamMeasure(const wchar_t *preferred, long *exposure, bool lockExposure, CamShot *out) {
    memset(out, 0, sizeof(*out));
    InitLinear();
    UINT32 count;
    IMFActivate **devs = EnumCameras(&count);
    if (!devs || !count) { if (devs) FreeCameras(devs, count); return false; }

    // Camera choisie par l'utilisateur, sinon la premiere qui n'est pas infrarouge
    int pick = -1, firstNormal = -1;
    wchar_t name[128];
    for (UINT32 i = 0; i < count; i++) {
        CameraName(devs[i], name, 128);
        if (preferred[0] && wcscmp(name, preferred) == 0) pick = (int)i;
        if (firstNormal < 0 && !IsIrCamera(name)) firstNormal = (int)i;
    }
    if (pick < 0) pick = firstNormal >= 0 ? firstNormal : 0;
    CameraName(devs[pick], name, 128);
    EnterCriticalSection(&g_lock);
    wcscpy(g_camUsed, name);
    LeaveCriticalSection(&g_lock);

    IMFMediaSource *src = NULL;
    devs[pick]->ActivateObject(IID_IMFMediaSource, (void **)&src);
    FreeCameras(devs, count);
    if (!src) return false;

    // Exposition : on la met en manuel, a la valeur choisie par l'app (ou verrouillee par l'utilisateur)
    IAMCameraControl *cc = NULL;
    IAMVideoProcAmp *amp = NULL;
    long mn = 0, mx = 0, step = 1, def = 0, caps = 0;
    bool hasRange = false;
    if (SUCCEEDED(src->QueryInterface(IID_IAMCameraControl, (void **)&cc)))
        hasRange = SUCCEEDED(cc->GetRange(CameraControl_Exposure, &mn, &mx, &step, &def, &caps)) && mx > mn;
    out->manual = hasRange && (caps & CameraControl_Flags_Manual);
    out->logUnits = hasRange && mx <= 10 && mn >= -20;       // unites DirectShow : 2^valeur secondes
    out->expDef = def;
    out->expMin = mn;
    out->expMax = mx;
    out->expStep = step > 0 ? step : 1;
    long e = (*exposure >= mn && *exposure <= mx && *exposure != LONG_MIN) ? *exposure : def;
    long gmn, gmx, gstep, gdef, gcaps;
    bool gainFixed = false;
    if (out->manual) {
        cc->Set(CameraControl_Exposure, e, CameraControl_Flags_Manual);
        // gain (amplification) fixe aussi, sinon la camera compense
        if (SUCCEEDED(src->QueryInterface(IID_IAMVideoProcAmp, (void **)&amp)) &&
            SUCCEEDED(amp->GetRange(VideoProcAmp_Gain, &gmn, &gmx, &gstep, &gdef, &gcaps)) &&
            (gcaps & VideoProcAmp_Flags_Manual))
            gainFixed = SUCCEEDED(amp->Set(VideoProcAmp_Gain, gdef, VideoProcAmp_Flags_Manual));
    }

    // Balance des blancs (couleur de la lumiere) : en auto, la camera la calcule en Kelvin ; on la lira
    // apres les photos. Si elle ne le permet pas, on la fixe et on calcule la couleur nous-memes.
    long wmn = 0, wmx = 0, wstep, wdef = 0, wcaps = 0;
    bool wbRange = false, wbFixed = false;
    if (!amp) src->QueryInterface(IID_IAMVideoProcAmp, (void **)&amp);
    if (amp) wbRange = SUCCEEDED(amp->GetRange(VideoProcAmp_WhiteBalance, &wmn, &wmx, &wstep, &wdef, &wcaps)) && wmx > wmn;
    bool wbAuto = wbRange && (wcaps & VideoProcAmp_Flags_Auto) && wmn >= 1000 && wmx <= 15000;
    if (wbAuto) amp->Set(VideoProcAmp_WhiteBalance, wdef, VideoProcAmp_Flags_Auto);
    else if (wbRange && (wcaps & VideoProcAmp_Flags_Manual))
        wbFixed = SUCCEEDED(amp->Set(VideoProcAmp_WhiteBalance, wdef, VideoProcAmp_Flags_Manual));

    bool ok = false;
    IMFSourceReader *reader = NULL;
    IMFAttributes *ra = NULL;
    MFCreateAttributes(&ra, 1);
    if (ra) ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(src, ra, &reader))) {
        IMFMediaType *mt = NULL, *cur = NULL;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, mt)) &&
            SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) &&
            SUCCEEDED(MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h))) {
            // pas negatif = image stockee de bas en haut (par defaut en RGB si non indique)
            UINT32 stride = 0;
            bool bottomUp = FAILED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) || (INT32)stride < 0;
            FrameStats st;
            bool have = ReadFrames(reader, 10, w, h, bottomUp, &st);
            // Notre propre auto-exposition (sauf si verrouillee) : image ni noire ni blanche, exposition connue
            for (int i = 0; have && out->manual && !lockExposure && i < 6; i++) {
                long ne = e;
                if ((st.mean > 200 || st.bright > 0.25) && e > mn)
                    ne = out->logUnits ? e - 1 : (e / 2 > mn ? e / 2 : mn);
                else if ((st.mean < 45 || st.dark > 0.5) && e < mx)
                    ne = out->logUnits ? e + 1 : (e * 2 < mx ? e * 2 : mx);
                if (ne == e) break;
                e = ne;
                cc->Set(CameraControl_Exposure, e, CameraControl_Flags_Manual);
                have = ReadFrames(reader, 6, w, h, bottomUp, &st);
            }
            if (have) {
                out->mean = st.mean;
                out->lin = st.lin;
                out->kelvin = -1;
                long wv, wf;
                if (wbAuto && SUCCEEDED(amp->Get(VideoProcAmp_WhiteBalance, &wv, &wf)) && wv >= 1500 && wv <= 15000) {
                    out->kelvin = wv;                    // la camera a mesure la couleur de la lumiere
                    out->kelvinFromCam = true;
                } else if (st.r > 0 && st.g > 0 && st.b > 0) {
                    // Image a balance des blancs fixe : on calcule la couleur moyenne de la piece
                    double k = RgbToKelvin(st.r, st.g, st.b);
                    if (wbFixed && wdef >= 2000 && wdef <= 10000) k *= wdef / 6500.0;
                    if (k >= 1500 && k <= 15000) out->kelvin = k;
                }
                out->clip = st.dark > 0.6 || st.mean < 12 ? 1 : (st.bright > 0.4 || st.mean > 240 ? 2 : 0);
                ok = true;
                if (out->manual) { out->exp = e; out->hasExp = true; }
                else {
                    long v, f;       // camera en auto : on lit l'exposition qu'elle a choisie, si elle le dit
                    if (cc && SUCCEEDED(cc->Get(CameraControl_Exposure, &v, &f)) && hasRange) {
                        out->exp = v;
                        out->hasExp = true;
                    }
                }
            }
        }
        if (cur) cur->Release();
        mt->Release();
        reader->Release();
    }
    if (ra) ra->Release();

    // On rend la camera en automatique pour les autres applications (Teams, Camera...)
    if (out->manual) cc->Set(CameraControl_Exposure, def, CameraControl_Flags_Auto);
    if (gainFixed) amp->Set(VideoProcAmp_Gain, gdef, VideoProcAmp_Flags_Auto);
    if (wbFixed) amp->Set(VideoProcAmp_WhiteBalance, wdef, VideoProcAmp_Flags_Auto);
    if (amp) amp->Release();
    if (cc) cc->Release();
    src->Shutdown();
    src->Release();
    if (out->manual && !lockExposure) *exposure = e;
    return ok;
}

// ---------- Soleil (plan C : ni capteur ni webcam) ----------
// Hauteur du soleil (degres) a la position donnee, pour l'heure actuelle.
double SunElevation(double lat, double lon) {
    SYSTEMTIME t;
    GetSystemTime(&t);
    return SunElevationAt(lat, lon, t.wMonth, t.wDay, t.wHour, t.wMinute);
}
