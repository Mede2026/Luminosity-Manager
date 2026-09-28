// Fenetre (4 onglets), graphique, miniature webcam, raccourcis clavier et icone pres de l'horloge.
#include "app.h"
#include <objidl.h>
#include <gdiplus.h>

enum {
    ID_TAB = 100, ID_OPEN, ID_MEASURE, ID_AUTO, ID_TOGGLEBTN, ID_BRIGHTER, ID_DARKER, ID_CAMERA, ID_STARTUP,
    ID_HIDE, ID_QUIT,
    // Accueil
    ID_S_BIGPCT, ID_S_STATE, ID_THUMB, ID_S_SOURCE, ID_S_MEASURE, ID_S_DETECTED, ID_GRAPH, ID_S_NOTICE,
    // Reglages
    ID_OFFSET, ID_S_OFFSET, ID_MIN, ID_MINSPIN, ID_MAX, ID_MAXSPIN, ID_CAMLIST, ID_INTERVAL, ID_SPIN, ID_S_NEXT,
    ID_CALIB, ID_S_CAMINFO,
    // Profils
    ID_PLIST, ID_PEXE, ID_PPICK, ID_PPCT, ID_PPCTSPIN, ID_PADD, ID_PDEL,
    // Plus
    ID_HOTKEY0, ID_HOTKEY1, ID_HOTKEY2, ID_HOTKEY3, ID_S_HOTKEY,
    ID_CITY, ID_CITYSEARCH, ID_S_CITY, ID_LAT, ID_LON, ID_LATLON,
    ID_UPDCHECK, ID_S_UPD, ID_UPDNOW_CHECK, ID_UPDNOW,
    ID_HINT = 900,          // textes d'aide (en gris) : ID_HINT et plus
};
enum { PAGE_HOME, PAGE_SETTINGS, PAGE_PROFILES, PAGE_MORE, PAGE_COUNT };
enum { TIMER_UPD_FIRST = 1, TIMER_UPD_DAILY, TIMER_NOTICE };

HWND g_hwnd;
WORD g_hotkeys[HK_COUNT];
static const WORD DEFAULT_HOTKEYS[HK_COUNT] = {
    MAKEWORD(VK_UP, HOTKEYF_CONTROL | HOTKEYF_ALT | HOTKEYF_EXT),
    MAKEWORD(VK_DOWN, HOTKEYF_CONTROL | HOTKEYF_ALT | HOTKEYF_EXT),
    MAKEWORD('M', HOTKEYF_CONTROL | HOTKEYF_ALT),
    MAKEWORD('A', HOTKEYF_CONTROL | HOTKEYF_ALT),
};
static const wchar_t *HOTKEY_NAMES[HK_COUNT] = {
    L"Plus clair (+5 %)", L"Plus sombre (−5 %)", L"Mesurer maintenant", L"Activer / désactiver l'app" };

static const COLORREF C_TEXT = RGB(32, 32, 32), C_HINT = RGB(100, 100, 100), C_GREEN = RGB(22, 140, 70),
                      C_GREY = RGB(120, 120, 120), C_ORANGE = RGB(200, 110, 0);

static NOTIFYICONDATAW g_nid;
static HICON g_iconOn, g_iconOff;
static UINT g_taskbarCreated;
static HFONT g_font, g_smallFont, g_bigFont;
static HBRUSH g_bgBrush;
static int g_dpi = 96;
static int g_page = PAGE_HOME;
static bool g_syncing;                 // ignore les EN_CHANGE provoques par nous-memes
static LONG g_drawnHist = -1;          // derniere minute dessinee dans le graphique
static LONG g_drawnThumb = -1;
static wchar_t g_notifiedVersion[32];
static ULONG_PTR g_gdiplus;

// Position de chaque controle en unites 96 ppp, pour tout replacer si l'ecran change d'echelle
struct Item { HWND h; short x, y, w, hh; BYTE font; };
static Item g_items[200];
static int g_itemCount;
enum { F_NORMAL, F_SMALL, F_BIG };

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static HFONT FontOf(BYTE f) { return f == F_BIG ? g_bigFont : (f == F_SMALL ? g_smallFont : g_font); }

// ---------- Polices (Segoe UI Variable sur Windows 11, sinon Segoe UI) ----------
static int CALLBACK FontFound(const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM found) {
    *(bool *)found = true;
    return 0;
}

static void CreateFonts() {
    static wchar_t face[LF_FACESIZE] = L"";
    if (!face[0]) {
        LOGFONTW lf = {};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy(lf.lfFaceName, L"Segoe UI Variable Text");
        bool found = false;
        HDC dc = GetDC(NULL);
        EnumFontFamiliesExW(dc, &lf, FontFound, (LPARAM)&found, 0);
        ReleaseDC(NULL, dc);
        wcscpy(face, found ? L"Segoe UI Variable Text" : L"Segoe UI");
    }
    if (g_font) DeleteObject(g_font);
    if (g_smallFont) DeleteObject(g_smallFont);
    if (g_bigFont) DeleteObject(g_bigFont);
    auto make = [&](int pt10, int weight) {
        return CreateFontW(-MulDiv(pt10, g_dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    g_font = make(90, FW_NORMAL);
    g_smallFont = make(80, FW_NORMAL);
    g_bigFont = make(280, FW_SEMIBOLD);
}

// ---------- Creation des controles ----------
// page : -1 = toujours visible, sinon l'onglet auquel appartient le controle
static HWND Ctl(int page, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h,
                int id, DWORD ex = 0, BYTE font = F_NORMAL) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | style, S(x), S(y), S(w), S(h),
                             g_hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)FontOf(font), FALSE);
    SetWindowLongPtrW(c, GWLP_USERDATA, page + 1);
    if (page < 0 || page == g_page) ShowWindow(c, SW_SHOWNA);
    if (g_itemCount < 200) g_items[g_itemCount++] = { c, (short)x, (short)y, (short)w, (short)h, font };
    return c;
}

static int g_hintId = ID_HINT;
static HWND Label(int page, const wchar_t *text, int x, int y, int w, int h = 18, int id = 0, DWORD style = 0,
                  BYTE font = F_NORMAL) {
    return Ctl(page, L"STATIC", text, SS_NOPREFIX | style, x, y, w, h, id, 0, font);
}

static HWND Hint(int page, const wchar_t *text, int x, int y, int w, int h = 18, int id = 0) {
    return Label(page, text, x, y, w, h, id ? id : g_hintId++);
}

static HWND Edit(int page, int x, int y, int w, int id, DWORD style = 0) {
    return Ctl(page, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | style, x, y, w, 23, id, WS_EX_CLIENTEDGE);
}

static HWND Spin(int page, int buddyId, int id, int lo, int hi) {
    HWND sp = Ctl(page, UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                  0, 0, 0, 0, id);
    SendMessageW(sp, UDM_SETBUDDY, (WPARAM)GetDlgItem(g_hwnd, buddyId), 0);
    SendMessageW(sp, UDM_SETRANGE32, lo, hi);
    return sp;
}

static HWND Check(int page, const wchar_t *text, int x, int y, int w, int id) {
    return Ctl(page, L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 22, id);
}

static HWND Button(int page, const wchar_t *text, int x, int y, int w, int h, int id) {
    return Ctl(page, L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, h, id);
}

static void Group(int page, const wchar_t *text, int x, int y, int w, int h) {
    Ctl(page, L"BUTTON", text, BS_GROUPBOX, x, y, w, h, 0);
}

// Replace et redimensionne tout (changement d'echelle de l'ecran)
static void Relayout() {
    CreateFonts();
    for (int i = 0; i < g_itemCount; i++) {
        Item &it = g_items[i];
        SendMessageW(it.h, WM_SETFONT, (WPARAM)FontOf(it.font), FALSE);
        if (it.w) SetWindowPos(it.h, NULL, S(it.x), S(it.y), S(it.w), S(it.hh), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (int i = 0; i < g_itemCount; i++) {         // les fleches se recollent a leur champ
        wchar_t cls[32];
        GetClassNameW(g_items[i].h, cls, 32);
        if (wcscmp(cls, UPDOWN_CLASSW) == 0)
            SendMessageW(g_items[i].h, UDM_SETBUDDY, SendMessageW(g_items[i].h, UDM_GETBUDDY, 0, 0), 0);
    }
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static BOOL CALLBACK ShowPageProc(HWND c, LPARAM page) {
    LONG_PTR tag = GetWindowLongPtrW(c, GWLP_USERDATA);
    if (tag > 0) ShowWindow(c, tag - 1 == page ? SW_SHOWNA : SW_HIDE);
    return TRUE;
}

static void FillCameraList();

static void ShowPage(int page) {
    g_page = page;
    TabCtrl_SetCurSel(GetDlgItem(g_hwnd, ID_TAB), page);
    EnumChildWindows(g_hwnd, ShowPageProc, page);
    if (page == PAGE_HOME) {
        InvalidateRect(GetDlgItem(g_hwnd, ID_GRAPH), NULL, FALSE);
        InvalidateRect(GetDlgItem(g_hwnd, ID_THUMB), NULL, FALSE);
    }
    if (page == PAGE_SETTINGS) FillCameraList();
}

// ---------- Textes ----------
static void SetTextIfChanged(int id, const wchar_t *text) {
    wchar_t old[256];
    GetDlgItemTextW(g_hwnd, id, old, 256);
    if (wcscmp(old, text) != 0) SetDlgItemTextW(g_hwnd, id, text);
}

static void SourceName(wchar_t *out, int n) {
    if (g_source == SRC_SENSOR)      wcsncpy(out, L"Capteur de lumière", n);
    else if (g_source == SRC_CAMERA) wcsncpy(out, L"Webcam", n);
    else                             wcsncpy(out, L"Soleil (heure du jour)", n);
}

static void MeasureText(wchar_t *out, int n) {
    if (g_source == SRC_SENSOR)
        swprintf(out, n, L"%ld lux", (long)g_lux);
    else if (g_source == SRC_CAMERA)
        swprintf(out, n, L"≈ %ld lux (estimé)", (long)g_lux);
    else
        swprintf(out, n, L"Soleil à %+ld° de l'horizon", (long)g_sunElev);
}

static void FormatCoord(wchar_t *out, int n, LONG v) {   // 4559 -> "45,59"
    swprintf(out, n, L"%.2f", v / 100.0);
    for (wchar_t *p = out; *p; p++) if (*p == L'.') *p = L',';
}

static void Notice(const wchar_t *text) {               // message temporaire sur l'accueil (10 s)
    SetTextIfChanged(ID_S_NOTICE, text);
    SetTimer(g_hwnd, TIMER_NOTICE, 10000, NULL);
}

static void RefreshCity() {
    wchar_t lat[16], lon[16], name[160], buf[256];
    FormatCoord(lat, 16, g_lat);
    FormatCoord(lon, 16, g_lon);
    EnterCriticalSection(&g_lock);
    if (g_cityResult[0]) wcscpy(name, g_cityResult);
    else if (!RegGetStr(L"City", name, 160)) wcscpy(name, g_lat == 4559 && g_lon == -7344 ? L"Boucherville" : L"Position personnalisée");
    LeaveCriticalSection(&g_lock);
    swprintf(buf, 256, L"Position : %ls (%ls ; %ls)", name, lat, lon);
    SetTextIfChanged(ID_S_CITY, buf);
    g_syncing = true;
    SetDlgItemTextW(g_hwnd, ID_LAT, lat);
    SetDlgItemTextW(g_hwnd, ID_LON, lon);
    g_syncing = false;
}

// ---------- Etat (rafraichi chaque seconde quand la fenetre est visible) ----------
static void RefreshStatus() {
    wchar_t buf[256];
    long b = g_bright < 0 ? 0 : g_bright;
    swprintf(buf, 256, L"%ld %%", b);
    SetTextIfChanged(ID_S_BIGPCT, buf);

    long ago = g_lastMeasure ? (long)((GetTickCount() - (DWORD)g_lastMeasure) / 1000) : -1;
    if (!g_enabled) wcscpy(buf, L"●  Désactivé : la luminosité ne change plus");
    else if (g_profilePct >= 0) {
        EnterCriticalSection(&g_lock);
        swprintf(buf, 256, L"●  Profil %ls : %ld %%", g_profileApp, (long)g_profilePct);
        LeaveCriticalSection(&g_lock);
    } else if (ago < 0) wcscpy(buf, L"●  Première mesure en cours…");
    else if (ago < 2) wcscpy(buf, L"●  Actif · mesuré à l'instant");
    else swprintf(buf, 256, L"●  Actif · mesuré il y a %ld s", ago);
    SetTextIfChanged(ID_S_STATE, buf);
    SetTextIfChanged(ID_TOGGLEBTN, g_enabled ? L"Désactiver" : L"Activer");

    SourceName(buf, 256);
    SetTextIfChanged(ID_S_SOURCE, buf);
    MeasureText(buf, 256);
    SetTextIfChanged(ID_S_MEASURE, buf);
    if (g_detected < 0) wcscpy(buf, L"—");
    else {
        long applied = Clamp(g_detected + g_offset, g_min, g_max);
        swprintf(buf, 256, L"%ld %%  %+ld %%  →  %ld %%", (long)g_detected, (long)g_offset, applied);
    }
    SetTextIfChanged(ID_S_DETECTED, buf);

    if (!g_enabled) wcscpy(buf, L"App désactivée : pas de photo");
    else if (g_source == SRC_CAMERA) swprintf(buf, 256, L"Prochaine photo dans %ld s", (long)g_camNext);
    else if (g_source == SRC_SENSOR) wcscpy(buf, L"Capteur détecté : la webcam n'est pas utilisée");
    else if (g_useCam) wcscpy(buf, L"Aucune webcam trouvée");
    else wcscpy(buf, L"Webcam désactivée");
    SetTextIfChanged(ID_S_NEXT, buf);

    if (g_source == SRC_CAMERA) {
        wchar_t cam[128];
        EnterCriticalSection(&g_lock);
        wcscpy(cam, g_camUsed);
        LeaveCriticalSection(&g_lock);
        const wchar_t *mode = g_camQuality == 0 ? L"exposition fixée par l'app (précis)"
                            : g_camQuality == 1 ? L"exposition auto de la caméra (moyen)"
                                                : L"la caméra ne donne pas son exposition (peu précis)";
        swprintf(buf, 256, L"%ls : %ls. Clarté de l'image : %ld %%", cam, mode, (long)(g_camLevel * 100 / 255));
    } else buf[0] = 0;
    SetTextIfChanged(ID_S_CAMINFO, buf);

    if (g_page == PAGE_HOME) {
        if (g_histLast != g_drawnHist) InvalidateRect(GetDlgItem(g_hwnd, ID_GRAPH), NULL, FALSE);
        if (g_lastMeasure != g_drawnThumb) InvalidateRect(GetDlgItem(g_hwnd, ID_THUMB), NULL, FALSE);
    }
}

static void UpdateTrayIcon() {
    g_nid.hIcon = g_enabled ? g_iconOn : g_iconOff;
    g_nid.uFlags = NIF_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void SyncControls() {
    g_syncing = true;
    CheckDlgButton(g_hwnd, ID_AUTO, g_enabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hwnd, ID_CAMERA, g_useCam ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hwnd, ID_STARTUP, StartupEnabled() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hwnd, ID_UPDCHECK, g_updateCheck ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(g_hwnd, ID_OFFSET, TBM_SETPOS, TRUE, g_offset);
    wchar_t buf[16];
    swprintf(buf, 16, L"%+ld %%", (long)g_offset);
    SetTextIfChanged(ID_S_OFFSET, buf);
    SendDlgItemMessageW(g_hwnd, ID_MINSPIN, UDM_SETPOS32, 0, g_min);
    SendDlgItemMessageW(g_hwnd, ID_MAXSPIN, UDM_SETPOS32, 0, g_max);
    SendDlgItemMessageW(g_hwnd, ID_SPIN, UDM_SETPOS32, 0, g_camInterval);
    const int camIds[] = { ID_CAMLIST, ID_INTERVAL, ID_SPIN, ID_CALIB };
    for (int id : camIds) EnableWindow(GetDlgItem(g_hwnd, id), g_useCam);
    g_syncing = false;
    UpdateTrayIcon();
    if (IsWindowVisible(g_hwnd)) RefreshStatus();
}

// ---------- Dessins : graphique 24 h et miniature webcam ----------
static void DrawGraph(const DRAWITEMSTRUCT *d) {
    using namespace Gdiplus;
    RECT rc = d->rcItem;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, g_smallFont);
    SetBkMode(dc, TRANSPARENT);
    RECT all = { 0, 0, w, h };
    FillRect(dc, &all, g_bgBrush);

    const Color LIGHT(255, 245, 158, 11), SCREEN(255, 37, 99, 235);
    int left = S(34), right = w - S(8), top = S(24), bottom = h - S(20);
    int pw = right - left, ph = bottom - top;
    LONG now = NowMinute(), last = g_histLast;

    // Textes (GDI, nets avec ClearType)
    SetTextColor(dc, C_HINT);
    TextOutW(dc, left + S(18), S(3), L"Lumière ambiante", 16);
    TextOutW(dc, left + S(158), S(3), L"Luminosité de l'écran", 21);
    SetTextAlign(dc, TA_RIGHT | TA_TOP);
    for (int v = 0; v <= 100; v += 50) {
        wchar_t t[8]; int n = swprintf(t, 8, L"%d%%", v);
        TextOutW(dc, left - S(4), bottom - v * ph / 100 - S(7), t, n);
    }
    SetTextAlign(dc, TA_CENTER | TA_TOP);
    for (int i = 0; i < HIST_LEN; i++) {
        LONG t = now - (HIST_LEN - 1) + i;
        if (t % 360 != 0) continue;
        wchar_t lbl[8]; int n = swprintf(lbl, 8, L"%dh", (int)((t / 60) % 24));
        TextOutW(dc, left + i * pw / (HIST_LEN - 1), bottom + S(3), lbl, n);
    }
    if (!last) TextOutW(dc, left + pw / 2, top + ph / 2 - S(8), L"Les données apparaîtront ici minute par minute", 46);

    // Lignes (GDI+, lissees)
    {
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        Pen grid(Color(255, 232, 232, 232), 1);
        for (int v = 0; v <= 100; v += 50) g.DrawLine(&grid, left, bottom - v * ph / 100, right, bottom - v * ph / 100);
        for (int i = 0; i < HIST_LEN; i++)
            if ((now - (HIST_LEN - 1) + i) % 360 == 0) {
                int x = left + i * pw / (HIST_LEN - 1);
                g.DrawLine(&grid, x, top, x, bottom);
            }
        SolidBrush lb(LIGHT), sb(SCREEN);
        g.FillEllipse(&lb, (REAL)left, (REAL)S(7), (REAL)S(9), (REAL)S(9));
        g.FillEllipse(&sb, (REAL)(left + S(140)), (REAL)S(7), (REAL)S(9), (REAL)S(9));

        static PointF pts[HIST_LEN];
        for (int pass = 0; pass < 2; pass++) {
            Pen pen(pass == 0 ? LIGHT : SCREEN, (REAL)S(2));
            pen.SetLineJoin(LineJoinRound);
            int n = 0;
            for (int i = 0; i <= HIST_LEN; i++) {
                BYTE v = HIST_NONE;
                if (i < HIST_LEN) {
                    LONG t = now - (HIST_LEN - 1) + i;
                    if (last && t <= last && t > last - HIST_LEN) {
                        const Sample &s = g_hist[t % HIST_LEN];
                        v = pass == 0 ? s.light : s.bright;
                    }
                }
                if (v != HIST_NONE && v <= 100) {
                    pts[n++] = PointF((REAL)(left + (double)i * pw / (HIST_LEN - 1)), (REAL)(bottom - v * ph / 100.0));
                    continue;
                }
                if (n == 1) g.FillEllipse(pass == 0 ? &lb : &sb, pts[0].X - S(2), pts[0].Y - S(2), (REAL)S(4), (REAL)S(4));
                else if (n > 1) g.DrawLines(&pen, pts, n);
                n = 0;
            }
        }
    }
    g_drawnHist = last;

    BitBlt(d->hDC, rc.left, rc.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static void DrawThumb(const DRAWITEMSTRUCT *d) {
    using namespace Gdiplus;
    RECT rc = d->rcItem;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    Bitmap canvas(w, h, PixelFormat32bppARGB);
    Graphics g(&canvas);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(255, GetRValue(GetSysColor(COLOR_WINDOW)), GetGValue(GetSysColor(COLOR_WINDOW)),
                  GetBValue(GetSysColor(COLOR_WINDOW))));

    // Cadre arrondi
    GraphicsPath path;
    REAL r = (REAL)S(8), x0 = 0.5f, y0 = 0.5f, x1 = w - 1.5f, y1 = h - 1.5f;
    path.AddArc(x0, y0, r * 2, r * 2, 180, 90);
    path.AddArc(x1 - r * 2, y0, r * 2, r * 2, 270, 90);
    path.AddArc(x1 - r * 2, y1 - r * 2, r * 2, r * 2, 0, 90);
    path.AddArc(x0, y1 - r * 2, r * 2, r * 2, 90, 90);
    path.CloseFigure();

    bool photo = g_source == SRC_CAMERA && g_thumbValid;
    if (photo) {
        Bitmap img(THUMB_W, THUMB_H, PixelFormat32bppRGB);
        BitmapData bd;
        Rect all(0, 0, THUMB_W, THUMB_H);
        if (img.LockBits(&all, ImageLockModeWrite, PixelFormat32bppRGB, &bd) == Ok) {
            for (int y = 0; y < THUMB_H; y++) {
                DWORD *row = (DWORD *)((BYTE *)bd.Scan0 + y * bd.Stride);
                for (int x = 0; x < THUMB_W; x++) {
                    BYTE v = g_thumb[y * THUMB_W + x];
                    row[x] = 0xFF000000 | (v << 16) | (v << 8) | v;
                }
            }
            img.UnlockBits(&bd);
        }
        g.SetClip(&path);
        g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        g.DrawImage(&img, Rect(0, 0, w, h));
        g.ResetClip();
    } else {
        SolidBrush bg(Color(255, 243, 244, 246));
        g.FillPath(&bg, &path);
    }
    Pen border(Color(255, 210, 210, 214), 1);
    g.DrawPath(&border, &path);

    if (!photo) {
        HDC sdc = GetDC(g_hwnd);
        Font font(sdc, g_smallFont);
        ReleaseDC(g_hwnd, sdc);
        SolidBrush tb(Color(255, GetRValue(C_HINT), GetGValue(C_HINT), GetBValue(C_HINT)));
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        const wchar_t *t = g_source == SRC_SENSOR ? L"Capteur de lumière" : g_source == SRC_SUN ? L"Mode soleil" : L"Pas encore de photo";
        g.DrawString(t, -1, &font, RectF(0, 0, (REAL)w, (REAL)h), &sf, &tb);
    }
    Graphics screen(d->hDC);
    screen.DrawImage(&canvas, (INT)rc.left, (INT)rc.top);
    g_drawnThumb = g_lastMeasure;
}

// ---------- Cameras ----------
static void FillCameraList() {
    HWND cb = GetDlgItem(g_hwnd, ID_CAMLIST);
    static wchar_t names[8][128];
    int n = ListCameras(names, 8);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Automatique (évite la caméra infrarouge)");
    int sel = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < n; i++) {
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)names[i]);
        if (g_camChoice[0] && wcscmp(names[i], g_camChoice) == 0) sel = i + 1;
    }
    LeaveCriticalSection(&g_lock);
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

static void CameraChosen() {
    HWND cb = GetDlgItem(g_hwnd, ID_CAMLIST);
    int sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    wchar_t name[128] = L"";
    if (sel > 0) SendMessageW(cb, CB_GETLBTEXT, sel, (LPARAM)name);
    EnterCriticalSection(&g_lock);
    wcscpy(g_camChoice, name);
    LeaveCriticalSection(&g_lock);
    RegPutStr(L"Camera", name);
    g_camExposure = LONG_MIN;                    // autre camera : on repart de son exposition par defaut
    g_remeasure = 1;
    SetEvent(g_wakeEvent);
}

// ---------- Profils ----------
static void FillProfileList() {
    HWND lb = GetDlgItem(g_hwnd, ID_PLIST);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_profileCount; i++) {
        wchar_t buf[96];
        swprintf(buf, 96, L"%ls  →  %d %%", g_profiles[i].exe, g_profiles[i].pct);
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)buf);
    }
    LeaveCriticalSection(&g_lock);
    EnableWindow(GetDlgItem(g_hwnd, ID_PDEL), FALSE);
}

static void AddProfile() {
    wchar_t exe[64];
    GetDlgItemTextW(g_hwnd, ID_PEXE, exe, 60);
    wchar_t *s = exe;
    while (*s == L' ') s++;
    size_t n = wcslen(s);
    while (n && s[n - 1] == L' ') s[--n] = 0;
    if (!n) { MessageBoxW(g_hwnd, L"Écris le nom de l'application (ex. LumaFusion.exe) ou clique sur « App active ».",
                          APP_NAME, MB_ICONINFORMATION); return; }
    if (!wcschr(s, L'.')) wcscat(s, L".exe");
    BOOL ok;
    int pct = (int)GetDlgItemInt(g_hwnd, ID_PPCT, &ok, FALSE);
    if (!ok || pct < 1 || pct > 100) { MessageBoxW(g_hwnd, L"La luminosité doit être entre 1 et 100 %.", APP_NAME, MB_ICONINFORMATION); return; }

    EnterCriticalSection(&g_lock);
    int i = 0;
    while (i < g_profileCount && _wcsicmp(g_profiles[i].exe, s) != 0) i++;
    bool full = i == g_profileCount && g_profileCount >= MAX_PROFILES;
    if (!full) {
        wcsncpy(g_profiles[i].exe, s, 63);
        g_profiles[i].exe[63] = 0;
        g_profiles[i].pct = pct;
        if (i == g_profileCount) g_profileCount++;
    }
    LeaveCriticalSection(&g_lock);
    if (full) { MessageBoxW(g_hwnd, L"Maximum 32 profils.", APP_NAME, MB_ICONINFORMATION); return; }
    SaveProfiles();
    FillProfileList();
    SetEvent(g_wakeEvent);
}

static void DeleteProfile() {
    int sel = (int)SendDlgItemMessageW(g_hwnd, ID_PLIST, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    EnterCriticalSection(&g_lock);
    if (sel < g_profileCount) {
        for (int i = sel; i < g_profileCount - 1; i++) g_profiles[i] = g_profiles[i + 1];
        g_profileCount--;
    }
    LeaveCriticalSection(&g_lock);
    SaveProfiles();
    FillProfileList();
    SetEvent(g_wakeEvent);
}

static void SelectProfile() {
    int sel = (int)SendDlgItemMessageW(g_hwnd, ID_PLIST, LB_GETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(g_hwnd, ID_PDEL), sel >= 0);
    if (sel < 0) return;
    EnterCriticalSection(&g_lock);
    if (sel < g_profileCount) {
        SetDlgItemTextW(g_hwnd, ID_PEXE, g_profiles[sel].exe);
        SendDlgItemMessageW(g_hwnd, ID_PPCTSPIN, UDM_SETPOS32, 0, g_profiles[sel].pct);
    }
    LeaveCriticalSection(&g_lock);
}

// ---------- Raccourcis clavier ----------
void RegisterHotkeys() {
    wchar_t failed[160] = L"";
    for (int i = 0; i < HK_COUNT; i++) {
        UnregisterHotKey(g_hwnd, i + 1);
        WORD hk = g_hotkeys[i];
        if (!LOBYTE(hk)) continue;
        BYTE f = HIBYTE(hk);
        UINT mods = MOD_NOREPEAT | (f & HOTKEYF_CONTROL ? MOD_CONTROL : 0) | (f & HOTKEYF_ALT ? MOD_ALT : 0) |
                    (f & HOTKEYF_SHIFT ? MOD_SHIFT : 0);
        if (!RegisterHotKey(g_hwnd, i + 1, mods, LOBYTE(hk))) {
            if (failed[0]) wcscat(failed, L", ");
            wcscat(failed, HOTKEY_NAMES[i]);
        }
    }
    wchar_t buf[256];
    if (failed[0]) swprintf(buf, 256, L"Déjà utilisé par une autre app : %ls", failed);
    else wcscpy(buf, L"Fonctionnent partout, même fenêtre cachée.");
    SetTextIfChanged(ID_S_HOTKEY, buf);
}

static void Balloon(const wchar_t *text) {
    g_nid.uFlags = NIF_INFO;
    wcscpy(g_nid.szInfoTitle, APP_NAME);
    wcsncpy(g_nid.szInfo, text, 255);
    g_nid.szInfo[255] = 0;
    g_nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ---------- Actions ----------
static void SetOffset(LONG o) {
    g_offset = Clamp(o, -50, 50);
    RegPut(L"Offset", (DWORD)g_offset);
    SyncControls();
    SetEvent(g_wakeEvent);
}

static void ToggleEnabled() {
    g_enabled = !g_enabled;
    RegPut(L"Enabled", g_enabled);
    if (g_enabled) g_force = 1;                  // on remesure tout de suite en revenant
    SyncControls();
    SetEvent(g_wakeEvent);
}

static void Nudge(int delta) {
    if (g_enabled && g_profilePct < 0) SetOffset(g_offset + delta);   // mode auto : on decale la courbe
    else { InterlockedExchangeAdd(&g_nudge, delta); SetEvent(g_wakeEvent); }
}

static void ReadIntField(int id, int lo, int hi, volatile LONG *target, const wchar_t *reg) {
    BOOL ok;
    int v = (int)GetDlgItemInt(g_hwnd, id, &ok, FALSE);
    if (!ok || v < lo || v > hi || v == *target) return;
    *target = v;
    RegPut(reg, (DWORD)v);
    SetEvent(g_wakeEvent);
}

static void ReadMinMax() {
    BOOL ok1, ok2;
    int mn = (int)GetDlgItemInt(g_hwnd, ID_MIN, &ok1, FALSE), mx = (int)GetDlgItemInt(g_hwnd, ID_MAX, &ok2, FALSE);
    if (!ok1 || !ok2 || mn < 1 || mx > 100 || mn >= mx) return;     // min doit rester sous max
    if (mn == g_min && mx == g_max) return;
    g_min = mn; g_max = mx;
    RegPut(L"MinBrightness", mn);
    RegPut(L"MaxBrightness", mx);
    SetEvent(g_wakeEvent);
}

static bool ParseCoord(int id, double lo, double hi, LONG *out) {
    wchar_t buf[32];
    GetDlgItemTextW(g_hwnd, id, buf, 32);
    for (wchar_t *p = buf; *p; p++) if (*p == L',') *p = L'.';
    wchar_t *end;
    double v = wcstod(buf, &end);
    if (end == buf || v < lo || v > hi) return false;
    *out = (LONG)lround(v * 100);
    return true;
}

static void ApplyLatLon() {
    LONG lat, lon;
    if (!ParseCoord(ID_LAT, -90, 90, &lat) || !ParseCoord(ID_LON, -180, 180, &lon)) {
        MessageBoxW(g_hwnd, L"Latitude : entre −90 et 90.\nLongitude : entre −180 et 180.\nEx. Boucherville : 45,59 et −73,44",
                    APP_NAME, MB_ICONINFORMATION);
        return;
    }
    g_lat = lat; g_lon = lon;
    RegPut(L"Latitude", (DWORD)lat);
    RegPut(L"Longitude", (DWORD)lon);
    EnterCriticalSection(&g_lock);
    wcscpy(g_cityResult, L"Position personnalisée");
    RegPutStr(L"City", g_cityResult);
    LeaveCriticalSection(&g_lock);
    RefreshCity();
    SetEvent(g_wakeEvent);
}

static void SearchCity() {
    wchar_t city[128];
    GetDlgItemTextW(g_hwnd, ID_CITY, city, 128);
    if (!city[0]) return;
    SetTextIfChanged(ID_S_CITY, L"Recherche en cours…");
    StartCitySearch(city);
}

static void ShowMainWindow(int page = -1) {
    ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_hwnd);
    if (page >= 0) ShowPage(page);
    SyncControls();
    RefreshStatus();
}

// ---------- Icone pres de l'horloge ----------
static void UpdateTip() {
    wchar_t src[48], mes[64];
    SourceName(src, 48);
    MeasureText(mes, 64);
    if (!g_enabled) swprintf(g_nid.szTip, 128, L"%ls\nDésactivé", APP_NAME);
    else swprintf(g_nid.szTip, 128, L"%ls\n%ls : %ls\nÉcran : %ld %%", APP_NAME, src, mes,
                  g_bright < 0 ? 0L : (long)g_bright);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void AddTrayIcon() {
    g_nid.hIcon = g_enabled ? g_iconOn : g_iconOff;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void ShowMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN, L"Ouvrir Luminosity Manager");
    AppendMenuW(m, MF_STRING | (g_enabled ? 0 : MF_GRAYED), ID_MEASURE, L"Mesurer maintenant");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_AUTO, L"Activé");
    AppendMenuW(m, MF_STRING, ID_BRIGHTER, L"Plus clair (+5 %)");
    AppendMenuW(m, MF_STRING, ID_DARKER, L"Plus sombre (−5 %)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quitter");
    SetMenuDefaultItem(m, ID_OPEN, FALSE);

    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ---------- Construction de la fenetre ----------
static void CreateControls() {
    CreateFonts();

    HWND tab = Ctl(-1, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, 8, 8, 384, 488, ID_TAB);
    const wchar_t *names[PAGE_COUNT] = { L"Accueil", L"Réglages", L"Profils", L"Plus" };
    for (int i = 0; i < PAGE_COUNT; i++) {
        TCITEMW it = {};
        it.mask = TCIF_TEXT;
        it.pszText = (LPWSTR)names[i];
        TabCtrl_InsertItem(tab, i, &it);
    }

    // --- Accueil ---
    const int P0 = PAGE_HOME;
    Label(P0, L"", 26, 42, 200, 56, ID_S_BIGPCT, 0, F_BIG);
    Hint(P0, L"Luminosité de l'écran", 30, 98, 200);
    Label(P0, L"", 28, 122, 232, 18, ID_S_STATE);
    Ctl(P0, L"STATIC", L"", SS_OWNERDRAW, 268, 46, 108, 81, ID_THUMB);
    Group(P0, L"Détails", 16, 148, 368, 104);
    Label(P0, L"Source :", 28, 170, 128);
    Label(P0, L"", 160, 170, 212, 18, ID_S_SOURCE);
    Label(P0, L"Mesure :", 28, 194, 128);
    Label(P0, L"", 160, 194, 212, 18, ID_S_MEASURE);
    Label(P0, L"Détectée + ajout :", 28, 218, 128);
    Label(P0, L"", 160, 218, 212, 18, ID_S_DETECTED);
    Ctl(P0, L"STATIC", L"", SS_OWNERDRAW, 16, 258, 368, 152, ID_GRAPH);
    Button(P0, L"Mesurer maintenant et ajuster", 16, 416, 240, 32, ID_MEASURE);
    Button(P0, L"Désactiver", 262, 416, 122, 32, ID_TOGGLEBTN);
    Hint(P0, L"", 16, 454, 368, 36, ID_S_NOTICE);

    // --- Reglages ---
    const int P1 = PAGE_SETTINGS;
    Check(P1, L"Luminosity Manager activé", 28, 46, 340, ID_AUTO);
    Label(P1, L"Ajout à la luminosité détectée :", 28, 76, 128, 34);
    HWND tb = Ctl(P1, TRACKBAR_CLASSW, L"", TBS_AUTOTICKS | WS_TABSTOP, 160, 76, 166, 28, ID_OFFSET);
    SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(-50, 50));
    SendMessageW(tb, TBM_SETTICFREQ, 10, 0);
    SendMessageW(tb, TBM_SETLINESIZE, 0, 1);
    SendMessageW(tb, TBM_SETPAGESIZE, 0, 10);
    Label(P1, L"", 326, 82, 48, 18, ID_S_OFFSET, SS_RIGHT);
    Hint(P1, L"Change aussi tout seul quand tu règles la luminosité toi-même.", 28, 110, 344);
    Label(P1, L"Minimum :", 28, 140, 70);
    Edit(P1, 100, 137, 56, ID_MIN, ES_NUMBER);
    Spin(P1, ID_MIN, ID_MINSPIN, 1, 99);
    Label(P1, L"%", 160, 140, 20);
    Label(P1, L"Maximum :", 200, 140, 70);
    Edit(P1, 272, 137, 56, ID_MAX, ES_NUMBER);
    Spin(P1, ID_MAX, ID_MAXSPIN, 2, 100);
    Label(P1, L"%", 332, 140, 20);
    Check(P1, L"Utiliser la webcam s'il n'y a pas de capteur", 28, 176, 340, ID_CAMERA);
    Label(P1, L"Caméra :", 46, 207, 60);
    Ctl(P1, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 110, 203, 262, 200, ID_CAMLIST);
    Label(P1, L"Photo toutes les", 46, 239, 110);
    Edit(P1, 160, 236, 64, ID_INTERVAL, ES_NUMBER);
    Spin(P1, ID_INTERVAL, ID_SPIN, MIN_INTERVAL, MAX_INTERVAL);
    Label(P1, L"secondes", 232, 239, 100);
    Hint(P1, L"", 46, 264, 326, 18, ID_S_NEXT);
    Button(P1, L"Calibrer : la pièce est éclairée normalement", 46, 288, 326, 30, ID_CALIB);
    Hint(P1, L"", 46, 324, 326, 50, ID_S_CAMINFO);
    Check(P1, L"Lancer au démarrage de Windows", 28, 386, 340, ID_STARTUP);

    // --- Profils ---
    const int P2 = PAGE_PROFILES;
    Label(P2, L"Luminosité fixe quand une de ces applications est au premier plan "
              L"(ex. LumaFusion.exe → 100 %).", 28, 44, 344, 34);
    Ctl(P2, L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, 28, 80, 344, 200,
        ID_PLIST, WS_EX_CLIENTEDGE);
    Label(P2, L"Application :", 28, 294, 84);
    Edit(P2, 114, 291, 150, ID_PEXE);
    Button(P2, L"App active", 270, 290, 102, 25, ID_PPICK);
    Label(P2, L"Luminosité :", 28, 326, 84);
    Edit(P2, 114, 323, 56, ID_PPCT, ES_NUMBER);
    Spin(P2, ID_PPCT, ID_PPCTSPIN, 1, 100);
    SendDlgItemMessageW(g_hwnd, ID_PPCTSPIN, UDM_SETPOS32, 0, 100);
    Label(P2, L"%", 174, 326, 16);
    Button(P2, L"Ajouter / modifier", 28, 358, 170, 30, ID_PADD);
    Button(P2, L"Supprimer", 204, 358, 168, 30, ID_PDEL);
    Hint(P2, L"« App active » prend la dernière application utilisée avant d'ouvrir cette fenêtre. "
             L"Un profil s'applique tant que l'app est activée.", 28, 400, 344, 50);

    // --- Plus ---
    const int P3 = PAGE_MORE;
    Group(P3, L"Raccourcis clavier", 16, 40, 368, 154);
    for (int i = 0; i < HK_COUNT; i++) {
        Label(P3, HOTKEY_NAMES[i], 28, 64 + i * 28, 168);
        HWND hk = Ctl(P3, HOTKEY_CLASSW, L"", WS_TABSTOP, 200, 61 + i * 28, 172, 23, ID_HOTKEY0 + i, WS_EX_CLIENTEDGE);
        SendMessageW(hk, HKM_SETRULES, HKCOMB_NONE | HKCOMB_S, MAKELPARAM(HOTKEYF_CONTROL | HOTKEYF_ALT, 0));
        SendMessageW(hk, HKM_SETHOTKEY, g_hotkeys[i], 0);
    }
    Hint(P3, L"", 28, 172, 344, 18, ID_S_HOTKEY);

    Group(P3, L"Ville (pour le mode Soleil)", 16, 202, 368, 118);
    Edit(P3, 28, 224, 250, ID_CITY);
    Button(P3, L"Chercher", 284, 223, 88, 25, ID_CITYSEARCH);
    Hint(P3, L"", 28, 254, 344, 18, ID_S_CITY);
    Label(P3, L"Latitude", 28, 286, 54);
    Edit(P3, 84, 283, 64, ID_LAT);
    Label(P3, L"Longitude", 158, 286, 62);
    Edit(P3, 222, 283, 64, ID_LON);
    Button(P3, L"Appliquer", 294, 282, 78, 25, ID_LATLON);

    Group(P3, L"Mises à jour", 16, 328, 368, 134);
    Check(P3, L"Vérifier automatiquement (une fois par jour)", 28, 350, 344, ID_UPDCHECK);
    wchar_t ver[96];
    swprintf(ver, 96, L"Version actuelle : %ls", APP_VERSION);
    Hint(P3, ver, 28, 376, 344, 18, ID_S_UPD);
    Button(P3, L"Vérifier maintenant", 28, 400, 160, 30, ID_UPDNOW_CHECK);
    Button(P3, L"Mettre à jour", 196, 400, 176, 30, ID_UPDNOW);
    EnableWindow(GetDlgItem(g_hwnd, ID_UPDNOW), FALSE);
    Hint(P3, L"github.com/" REPO, 28, 438, 344);

    // --- Toujours visibles ---
    Button(-1, L"Réduire dans la barre", 164, 504, 128, 30, ID_HIDE);
    Button(-1, L"Quitter", 298, 504, 94, 30, ID_QUIT);

    // L'onglet doit etre sous les autres controles, sinon il efface leurs bordures
    SetWindowPos(tab, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    FillProfileList();
    FillCameraList();
    RefreshCity();
    SyncControls();
    RefreshStatus();
    RegisterHotkeys();
}

// ---------- Messages ----------
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) ShowMainWindow();
        else if (LOWORD(lp) == WM_RBUTTONUP) ShowMenu();
        else if (LOWORD(lp) == NIN_BALLOONUSERCLICK) ShowMainWindow(g_newVersion[0] ? PAGE_MORE : -1);
        return 0;
    case WM_UPDATE:
        UpdateTip();
        if (IsWindowVisible(h) && !IsIconic(h)) RefreshStatus();
        return 0;
    case WM_NOSENSOR:
        Balloon(g_source == SRC_CAMERA ? L"Pas de capteur de lumière : mesure avec la webcam."
                                       : L"Pas de capteur ni de webcam : réglage selon l'heure et le soleil.");
        return 0;
    case WM_LEARNED: {
        wchar_t buf[160];
        if (lp) swprintf(buf, 160, L"Profil mis à jour : %ld %%", (long)g_profilePct);
        else swprintf(buf, 160, L"Tu as changé la luminosité : c'est retenu (ajout %+ld %%).", (long)g_offset);
        Notice(buf);
        SyncControls();
        if (lp) FillProfileList();
        return 0;
    }
    case WM_HOTKEY:
        switch (wp - 1) {
        case HK_UP:      Nudge(+5); break;
        case HK_DOWN:    Nudge(-5); break;
        case HK_MEASURE: g_force = 1; SetEvent(g_wakeEvent); break;
        case HK_TOGGLE:
            ToggleEnabled();
            Balloon(g_enabled ? L"Luminosity Manager : activé" : L"Luminosity Manager : désactivé");
            break;
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_NOTICE) { KillTimer(h, TIMER_NOTICE); SetTextIfChanged(ID_S_NOTICE, L""); return 0; }
        if (wp == TIMER_UPD_FIRST) KillTimer(h, TIMER_UPD_FIRST);
        if (g_updateCheck) StartUpdateCheck();
        return 0;
    case WM_UPDATE_FOUND: {
        wchar_t buf[128];
        if (wp == 1) {
            swprintf(buf, 128, L"Nouvelle version disponible : %ls", g_newVersion);
            SetTextIfChanged(ID_S_UPD, buf);
            swprintf(buf, 128, L"Mettre à jour vers %ls", g_newVersion);
            SetDlgItemTextW(h, ID_UPDNOW, buf);
            EnableWindow(GetDlgItem(h, ID_UPDNOW), TRUE);
            if (wcscmp(g_notifiedVersion, g_newVersion) != 0) {     // une seule notification par version
                wcscpy(g_notifiedVersion, g_newVersion);
                swprintf(buf, 128, L"La version %ls est disponible. Clique ici pour mettre à jour.", g_newVersion);
                Balloon(buf);
            }
        } else if (wp == 0) {
            swprintf(buf, 128, L"Tu as la dernière version (%ls)", APP_VERSION);
            SetTextIfChanged(ID_S_UPD, buf);
        } else {
            SetTextIfChanged(ID_S_UPD, L"Impossible de vérifier (pas d'internet ?)");
        }
        EnableWindow(GetDlgItem(h, ID_UPDNOW_CHECK), TRUE);
        return 0;
    }
    case WM_UPDATE_READY:
        if (wp) DestroyWindow(h);              // la nouvelle version se lance a la fermeture
        else {
            SetTextIfChanged(ID_S_UPD, L"Échec de la mise à jour. Réessaie plus tard.");
            EnableWindow(GetDlgItem(h, ID_UPDNOW), TRUE);
        }
        return 0;
    case WM_CITY_FOUND:
        if (wp) { RefreshCity(); SetEvent(g_wakeEvent); }
        else SetTextIfChanged(ID_S_CITY, L"Ville introuvable (ou pas d'internet).");
        return 0;
    case WM_DPICHANGED: {                      // fenetre deplacee sur un ecran a une autre echelle
        g_dpi = HIWORD(wp);
        const RECT *r = (const RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Relayout();
        return 0;
    }
    case WM_NOTIFY:
        if (((NMHDR *)lp)->idFrom == ID_TAB && ((NMHDR *)lp)->code == TCN_SELCHANGE)
            ShowPage(TabCtrl_GetCurSel(GetDlgItem(h, ID_TAB)));
        return 0;
    case WM_DRAWITEM:
        if (wp == ID_GRAPH) { DrawGraph((DRAWITEMSTRUCT *)lp); return TRUE; }
        if (wp == ID_THUMB) { DrawThumb((DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        int id = GetDlgCtrlID((HWND)lp);
        COLORREF c = C_TEXT;
        if (id >= ID_HINT || id == ID_S_NOTICE || id == ID_S_NEXT || id == ID_S_CAMINFO || id == ID_S_HOTKEY ||
            id == ID_S_CITY || id == ID_S_UPD) c = C_HINT;
        if (id == ID_S_STATE) c = !g_enabled ? C_GREY : (g_profilePct >= 0 ? C_ORANGE : C_GREEN);
        SetTextColor(dc, c);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        return (LRESULT)g_bgBrush;
    }
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(h, ID_OFFSET))
            SetOffset((LONG)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0));
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= ID_HOTKEY0 && id < ID_HOTKEY0 + HK_COUNT) {
            if (code == EN_CHANGE) {
                int i = id - ID_HOTKEY0;
                g_hotkeys[i] = (WORD)SendMessageW((HWND)lp, HKM_GETHOTKEY, 0, 0);
                wchar_t name[16];
                swprintf(name, 16, L"Hotkey%d", i);
                RegPut(name, g_hotkeys[i]);
                RegisterHotkeys();
            }
            return 0;
        }
        switch (id) {
        case IDOK: {                            // Entree : action du champ ou se trouve le curseur
            int f = GetDlgCtrlID(GetFocus());
            if (f == ID_CITY) SearchCity();
            else if (f == ID_LAT || f == ID_LON) ApplyLatLon();
            else if (f == ID_PEXE || f == ID_PPCT) AddProfile();
            break;
        }
        case IDCANCEL:    ShowWindow(h, SW_HIDE); break;
        case ID_OPEN:     ShowMainWindow(); break;
        case ID_MEASURE:
            g_force = 1; SetEvent(g_wakeEvent);
            Notice(L"Mesure en cours…");
            break;
        case ID_AUTO: case ID_TOGGLEBTN: ToggleEnabled(); break;
        case ID_BRIGHTER: Nudge(+5); break;
        case ID_DARKER:   Nudge(-5); break;
        case ID_CAMERA:
            g_useCam = !g_useCam; RegPut(L"UseCamera", g_useCam); g_remeasure = 1;
            SyncControls(); SetEvent(g_wakeEvent);
            break;
        case ID_CAMLIST:  if (code == CBN_SELCHANGE) CameraChosen(); break;
        case ID_CALIB:
            g_calibrate = 1; SetEvent(g_wakeEvent);
            Notice(L"Calibration : la lumière actuelle devient la référence « pièce normale ».");
            break;
        case ID_STARTUP:  SetStartup(!StartupEnabled()); SyncControls(); break;
        case ID_UPDCHECK: g_updateCheck = !g_updateCheck; RegPut(L"UpdateCheck", g_updateCheck); break;
        case ID_INTERVAL:
            if (g_syncing) break;
            if (code == EN_CHANGE) ReadIntField(ID_INTERVAL, MIN_INTERVAL, MAX_INTERVAL, &g_camInterval, L"CameraInterval");
            else if (code == EN_KILLFOCUS) SyncControls();
            break;
        case ID_MIN: case ID_MAX:
            if (g_syncing) break;
            if (code == EN_CHANGE) ReadMinMax();
            else if (code == EN_KILLFOCUS) SyncControls();
            break;
        case ID_PLIST:    if (code == LBN_SELCHANGE) SelectProfile(); break;
        case ID_PPICK: {
            wchar_t app[64];
            EnterCriticalSection(&g_lock);
            wcscpy(app, g_lastApp);
            LeaveCriticalSection(&g_lock);
            if (app[0]) SetDlgItemTextW(h, ID_PEXE, app);
            else MessageBoxW(h, L"Ouvre d'abord l'application voulue, puis reviens ici.", APP_NAME, MB_ICONINFORMATION);
            break;
        }
        case ID_PADD:     AddProfile(); break;
        case ID_PDEL:     DeleteProfile(); break;
        case ID_CITYSEARCH: SearchCity(); break;
        case ID_LATLON:   ApplyLatLon(); break;
        case ID_UPDNOW_CHECK:
            SetTextIfChanged(ID_S_UPD, L"Vérification…");
            EnableWindow(GetDlgItem(h, ID_UPDNOW_CHECK), FALSE);
            StartUpdateCheck();
            break;
        case ID_UPDNOW:
            SetTextIfChanged(ID_S_UPD, L"Téléchargement de la nouvelle version…");
            EnableWindow(GetDlgItem(h, ID_UPDNOW), FALSE);
            StartUpdateDownload();
            break;
        case ID_HIDE:     ShowWindow(h, SW_HIDE); break;
        case ID_QUIT:     DestroyWindow(h); break;
        }
        return 0;
    }
    case WM_CLOSE:        // la croix cache la fenetre, l'app reste pres de l'horloge
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        Gdiplus::GdiplusShutdown(g_gdiplus);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_taskbarCreated && msg) { AddTrayIcon(); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

bool CreateMainWindow(HINSTANCE inst, bool showWindow, int show) {
    for (int i = 0; i < HK_COUNT; i++) {
        wchar_t name[16];
        swprintf(name, 16, L"Hotkey%d", i);
        g_hotkeys[i] = (WORD)RegGet(name, DEFAULT_HOTKEYS[i]);
    }

    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&g_gdiplus, &gsi, NULL);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_PROGRESS_CLASS |
                                 ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_HOTKEY_CLASS };
    InitCommonControlsEx(&icc);
    g_bgBrush = GetSysColorBrush(COLOR_WINDOW);

    // Icones nettes a toutes les tailles (LoadIconMetric choisit la bonne image)
    HICON bigIcon = NULL;
    LoadIconMetric(inst, MAKEINTRESOURCEW(1), LIM_LARGE, &bigIcon);
    LoadIconMetric(inst, MAKEINTRESOURCEW(1), LIM_SMALL, &g_iconOn);
    LoadIconMetric(inst, MAKEINTRESOURCEW(2), LIM_SMALL, &g_iconOff);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = bigIcon;
    wc.hIconSm = g_iconOn;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"LuminosityManagerWnd";
    if (!RegisterClassExW(&wc)) return false;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g_hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, APP_NAME L" " APP_VERSION, style,
                             CW_USEDEFAULT, CW_USEDEFAULT, 400, 540, NULL, NULL, inst, NULL);
    if (!g_hwnd) return false;
    // Taille exacte selon l'echelle de l'ecran ou la fenetre s'ouvre
    g_dpi = GetDpiForWindow(g_hwnd);
    if (!g_dpi) g_dpi = 96;
    RECT r = { 0, 0, S(400), S(542) };
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_CONTROLPARENT, g_dpi);
    SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    CreateControls();

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uCallbackMessage = WM_TRAY;
    wcscpy(g_nid.szTip, APP_NAME);
    AddTrayIcon();

    SetTimer(g_hwnd, TIMER_UPD_FIRST, 10 * 1000, NULL);          // 1re verification 10 s apres le lancement
    SetTimer(g_hwnd, TIMER_UPD_DAILY, 24 * 60 * 60 * 1000, NULL);

    if (showWindow) ShowWindow(g_hwnd, show);
    return true;
}
