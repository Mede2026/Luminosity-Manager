// Navigateur integre (WebView2, le moteur d'Edge deja installe dans Windows 10/11) qui affiche l'interface.
// Il n'existe que pendant que la fenetre est ouverte : fenetre cachee = 0 processus Edge = app legere.
#include "app.h"
#include <initguid.h>
#include <objbase.h>
#include "webview2/WebView2.h"

static ICoreWebView2Environment *g_env;
static ICoreWebView2Controller *g_ctrl;
static ICoreWebView2 *g_view;
static EventRegistrationToken g_msgToken;
static HWND g_parent;
static bool g_creating, g_cancel, g_transparent;
static COLORREF g_bg;

// ---------- Trouver le moteur WebView2 ----------
// Le "chargeur" officiel de Microsoft ne fonctionne qu'avec Visual Studio. On fait comme lui :
// on lit dans le registre ou Windows a installe WebView2, puis on appelle directement son DLL.
typedef HRESULT (STDMETHODCALLTYPE *CreateInternalFn)(bool, int, PCWSTR, IUnknown *,
                                                      ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
typedef HRESULT (STDMETHODCALLTYPE *CreateLoaderFn)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
                                                    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

static const wchar_t *RUNTIME_GUID = L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";   // WebView2 Runtime (stable)

static bool FileExists(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool RegStr(HKEY root, const wchar_t *sub, const wchar_t *name, wchar_t *out, DWORD chars) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS) return false;
    DWORD sz = chars * sizeof(wchar_t);
    bool ok = RegGetValueW(k, NULL, name, RRF_RT_REG_SZ, NULL, out, &sz) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

// "128.0.2739.42" > "127.0.1.0" ?
static bool VersionGreater(const wchar_t *a, const wchar_t *b) {
    for (;;) {
        wchar_t *ea, *eb;
        long x = wcstol(a, &ea, 10), y = wcstol(b, &eb, 10);
        if (x != y) return x > y;
        if (*ea != L'.' || *eb != L'.') return *ea == L'.';
        a = ea + 1; b = eb + 1;
    }
}

static bool FindClientDll(wchar_t *dll) {
    const wchar_t *arch = L"x64";
    wchar_t sub[160], dir[MAX_PATH], ver[64];
    HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };

    // 1) EdgeUpdate\ClientState\{guid} : valeur "EBWebView" = dossier du moteur
    swprintf(sub, 160, L"SOFTWARE\\Microsoft\\EdgeUpdate\\ClientState\\%ls", RUNTIME_GUID);
    for (HKEY root : roots)
        if (RegStr(root, sub, L"EBWebView", dir, MAX_PATH)) {
            swprintf(dll, MAX_PATH, L"%ls\\%ls\\EmbeddedBrowserWebView.dll", dir, arch);
            if (FileExists(dll)) return true;
        }

    // 2) EdgeUpdate\Clients\{guid} : "location" + "pv" (version)
    swprintf(sub, 160, L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\%ls", RUNTIME_GUID);
    for (HKEY root : roots)
        if (RegStr(root, sub, L"location", dir, MAX_PATH) && RegStr(root, sub, L"pv", ver, 64)) {
            swprintf(dll, MAX_PATH, L"%ls\\%ls\\EBWebView\\%ls\\EmbeddedBrowserWebView.dll", dir, ver, arch);
            if (FileExists(dll)) return true;
        }

    // 3) Dossier habituel : on prend la version la plus recente
    const wchar_t *vars[2] = { L"ProgramFiles(x86)", L"ProgramFiles" };
    bool found = false;
    wchar_t best[64] = L"";
    for (const wchar_t *v : vars) {
        wchar_t base[MAX_PATH], pattern[MAX_PATH];
        if (!GetEnvironmentVariableW(v, base, MAX_PATH)) continue;
        swprintf(pattern, MAX_PATH, L"%ls\\Microsoft\\EdgeWebView\\Application\\*", base);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] < L'0' || fd.cFileName[0] > L'9')
                continue;
            wchar_t cand[MAX_PATH];
            swprintf(cand, MAX_PATH, L"%ls\\Microsoft\\EdgeWebView\\Application\\%ls\\EBWebView\\%ls\\EmbeddedBrowserWebView.dll",
                     base, fd.cFileName, arch);
            if (FileExists(cand) && (!found || VersionGreater(fd.cFileName, best))) {
                wcscpy(dll, cand);
                wcsncpy(best, fd.cFileName, 63);
                found = true;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return found;
}

static HRESULT CreateEnvironment(const wchar_t *userData, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *h) {
    // a) WebView2Loader.dll a cote du .exe (si quelqu'un l'a mis), c'est la voie officielle
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) {
        wcscpy(slash + 1, L"WebView2Loader.dll");
        if (FileExists(path)) {
            HMODULE m = LoadLibraryW(path);
            CreateLoaderFn fn = m ? (CreateLoaderFn)(void *)GetProcAddress(m, "CreateCoreWebView2EnvironmentWithOptions") : NULL;
            if (fn) return fn(NULL, userData, NULL, h);
        }
    }
    // b) Directement le moteur installe dans Windows
    wchar_t dll[MAX_PATH];
    if (!FindClientDll(dll)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    HMODULE m = LoadLibraryExW(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    CreateInternalFn fn = m ? (CreateInternalFn)(void *)GetProcAddress(m, "CreateWebViewEnvironmentWithOptionsInternal") : NULL;
    if (!fn) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return fn(true, 0 /* runtime installe */, userData, NULL, h);
}

// ---------- Petits objets COM pour recevoir les reponses de WebView2 ----------
template <class I> struct Handler : I {
    LONG refs = 1;
    virtual ~Handler() {}
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(I)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
};

static void Fail() {
    g_creating = false;
    g_cancel = false;
    if (g_env) { g_env->Release(); g_env = NULL; }
    OnWebViewFailed();
}

struct MessageHandler : Handler<ICoreWebView2WebMessageReceivedEventHandler> {
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) override {
        LPWSTR json = NULL;
        if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
            int n = WideCharToMultiByte(CP_UTF8, 0, json, -1, NULL, 0, NULL, NULL);
            char *utf8 = (char *)malloc(n > 0 ? n : 1);
            if (utf8 && n > 0) {
                WideCharToMultiByte(CP_UTF8, 0, json, -1, utf8, n, NULL, NULL);
                OnPageMessage(utf8);
            }
            free(utf8);
            CoTaskMemFree(json);
        }
        return S_OK;
    }
};

// Page de l'interface : fichier HTML integre dans le .exe (ressource 3)
static wchar_t *LoadPage() {
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(3), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    const char *data = g ? (const char *)LockResource(g) : NULL;
    DWORD size = r ? SizeofResource(NULL, r) : 0;
    if (!data || !size) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, data, (int)size, NULL, 0);
    wchar_t *html = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!html) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, data, (int)size, html, n);
    html[n] = 0;
    return html;
}

struct ControllerDone : Handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller *ctrl) override {
        if (FAILED(hr) || !ctrl) { Fail(); return S_OK; }
        if (g_cancel) {                          // fenetre fermee pendant le demarrage
            ctrl->Close();
            g_creating = g_cancel = false;
            if (g_env) { g_env->Release(); g_env = NULL; }
            return S_OK;
        }
        g_ctrl = ctrl;
        g_ctrl->AddRef();
        g_ctrl->get_CoreWebView2(&g_view);

        ICoreWebView2Settings *s = NULL;
        if (g_view && SUCCEEDED(g_view->get_Settings(&s))) {
            s->put_AreDefaultContextMenusEnabled(FALSE);    // pas de menu "Inspecter" au clic droit
            s->put_AreDevToolsEnabled(FALSE);
            s->put_IsStatusBarEnabled(FALSE);
            s->put_IsZoomControlEnabled(FALSE);
            s->put_IsWebMessageEnabled(TRUE);
            s->Release();
        }
        // Fond : transparent pour laisser voir l'effet Mica de Windows 11, sinon couleur du theme
        ICoreWebView2Controller2 *c2 = NULL;
        if (SUCCEEDED(g_ctrl->QueryInterface(IID_ICoreWebView2Controller2, (void **)&c2))) {
            COREWEBVIEW2_COLOR c = { (BYTE)(g_transparent ? 0 : 255), GetRValue(g_bg), GetGValue(g_bg), GetBValue(g_bg) };
            c2->put_DefaultBackgroundColor(c);
            c2->Release();
        }
        if (g_view) {
            MessageHandler *mh = new MessageHandler();
            g_view->add_WebMessageReceived(mh, &g_msgToken);
            mh->Release();
            wchar_t *html = LoadPage();
            g_view->NavigateToString(html ? html : L"<p>Interface introuvable</p>");
            free(html);
        }
        WebViewResize();
        g_ctrl->put_IsVisible(TRUE);
        g_ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        g_creating = false;
        return S_OK;
    }
};

struct EnvironmentDone : Handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment *env) override {
        if (FAILED(hr) || !env) { Fail(); return S_OK; }
        if (g_cancel) { g_creating = g_cancel = false; return S_OK; }
        g_env = env;
        g_env->AddRef();
        ControllerDone *cd = new ControllerDone();
        if (FAILED(g_env->CreateCoreWebView2Controller(g_parent, cd))) Fail();
        cd->Release();
        return S_OK;
    }
};

// ---------- Utilise par la fenetre ----------
bool WebViewCreate(HWND parent, bool transparent, COLORREF bg) {
    g_cancel = false;
    if (g_view || g_creating) return true;
    g_parent = parent;
    g_transparent = transparent;
    g_bg = bg;
    wchar_t data[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", data, MAX_PATH - 40)) return false;
    wcscat(data, L"\\LuminosityManager");
    CreateDirectoryW(data, NULL);
    wcscat(data, L"\\WebView2");
    g_creating = true;
    EnvironmentDone *ed = new EnvironmentDone();
    HRESULT hr = CreateEnvironment(data, ed);
    ed->Release();
    if (FAILED(hr)) { g_creating = false; return false; }
    return true;
}

// Ferme le navigateur : les processus Edge s'arretent, la memoire est rendue
void WebViewDestroy() {
    if (g_creating) g_cancel = true;
    if (g_view) {
        g_view->remove_WebMessageReceived(g_msgToken);
        g_view->Release();
        g_view = NULL;
    }
    if (g_ctrl) {
        g_ctrl->Close();
        g_ctrl->Release();
        g_ctrl = NULL;
    }
    if (g_env && !g_creating) { g_env->Release(); g_env = NULL; }
}

void WebViewResize() {
    if (!g_ctrl) return;
    RECT r;
    GetClientRect(g_parent, &r);
    g_ctrl->put_Bounds(r);
}

void WebViewMoved() {
    if (g_ctrl) g_ctrl->NotifyParentWindowPositionChanged();
}

bool WebViewPost(const wchar_t *json) {
    return g_view && SUCCEEDED(g_view->PostWebMessageAsJson(json));
}

bool WebViewAlive() { return g_view != NULL; }

// Donne le clavier a la page (sinon, apres Alt+Tab ou l'icone, le curseur clignote mais les touches se perdent)
void WebViewFocus() {
    if (g_ctrl) g_ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}
