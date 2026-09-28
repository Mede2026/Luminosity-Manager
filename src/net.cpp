// Internet : mises a jour (GitHub Releases) et recherche de ville (Open-Meteo).
// WinHTTP est inclus dans Windows : aucune dependance a ajouter.
#include "app.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

wchar_t g_newVersion[32];
wchar_t g_cityResult[160];
static wchar_t g_newUrl[512];
static volatile LONG g_busy;          // une seule mise a jour a la fois
static volatile LONG g_cityBusy;
static bool g_launchNew;

// Telecharge une adresse HTTPS en memoire (redirections suivies). A liberer avec free().
// Si location est donne : redirections desactivees, on renvoie l'adresse de la redirection (en-tete Location).
static bool HttpGet(const wchar_t *url, char **data, DWORD *size, DWORD maxSize, wchar_t *location = NULL,
                    DWORD locChars = 0) {
    if (data) *data = NULL;
    if (size) *size = 0;
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;  uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;   uc.dwUrlPathLength = 2048;
    wchar_t extra[1024];
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return false;
    if (uc.dwUrlPathLength + uc.dwExtraInfoLength >= 2048) return false;
    wcscat(path, extra);                           // chemin + ?parametres

    bool ok = false;
    HINTERNET s = WinHttpOpen(L"LuminosityManager/" APP_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (r && location) {
        DWORD off = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));
    }
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, NULL)) {
        DWORD status = 0, sl = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &sl, WINHTTP_NO_HEADER_INDEX);
        if (location) {
            DWORD bytes = locChars * sizeof(wchar_t);
            ok = (status == 301 || status == 302) &&
                 WinHttpQueryHeaders(r, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location, &bytes,
                                     WINHTTP_NO_HEADER_INDEX);
        } else if (status == 200) {
            DWORD cap = 64 * 1024, len = 0;
            char *buf = (char *)malloc(cap + 1);
            ok = buf != NULL;
            for (;;) {
                DWORD avail = 0, got = 0;
                if (!ok || !WinHttpQueryDataAvailable(r, &avail)) { ok = false; break; }
                if (avail == 0) break;
                if (len + avail > maxSize) { ok = false; break; }
                if (len + avail > cap) {
                    while (len + avail > cap) cap *= 2;
                    char *nb = (char *)realloc(buf, cap + 1);
                    if (!nb) { ok = false; break; }
                    buf = nb;
                }
                if (!WinHttpReadData(r, buf + len, avail, &got)) { ok = false; break; }
                len += got;
            }
            if (ok) { buf[len] = 0; *data = buf; *size = len; }
            else free(buf);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

// ---------- Mises a jour ----------
static DWORD WINAPI UpdateCheckThread(LPVOID) {
    // github.com/<repo>/releases/latest redirige vers .../releases/tag/vX.Y : on lit juste la redirection
    // (pas l'API GitHub, qui limite le nombre de requetes).
    wchar_t final[512];
    WPARAM result = 2;
    if (HttpGet(L"https://github.com/" REPO L"/releases/latest", NULL, NULL, 0, final, 512)) {
        const wchar_t *tag = wcsstr(final, L"/releases/tag/");
        result = 0;
        if (tag) {
            tag += 14;
            wchar_t clean[32];
            int n = 0;
            while (tag[n] && tag[n] != L'/' && tag[n] != L'?' && n < 31) { clean[n] = tag[n]; n++; }
            clean[n] = 0;
            if (n && IsNewerVersion(clean, APP_VERSION)) {
                wcscpy(g_newVersion, clean);
                swprintf(g_newUrl, 512, L"https://github.com/" REPO L"/releases/download/%ls/LuminosityManager.exe", clean);
                result = 1;
            }
        }
    }
    InterlockedExchange(&g_busy, 0);
    PostMessageW(g_hwnd, WM_UPDATE_FOUND, result, 0);
    return 0;
}

void StartUpdateCheck() {
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;
    HANDLE t = CreateThread(NULL, 0, UpdateCheckThread, NULL, 0, NULL);
    if (t) CloseHandle(t); else g_busy = 0;
}

// Telecharge le nouveau .exe, le met a la place de l'actuel (l'actuel devient .old).
// Empreinte SHA-256 (64 caracteres hexa) calculee par Windows (BCrypt)
static bool Sha256Hex(const void *data, DWORD len, char out[65]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    UCHAR hash[32];
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return false;
    bool ok = BCryptHash(alg, NULL, 0, (PUCHAR)data, len, hash, 32) == 0;
    BCryptCloseAlgorithmProvider(alg, 0);
    for (int i = 0; ok && i < 32; i++) snprintf(out + i * 2, 3, "%02x", hash[i]);
    return ok;
}

// Le fichier telecharge est-il exactement celui publie sur GitHub ? (empreinte dans le fichier .sha256)
static bool VerifyDownload(const char *bin, DWORD len) {
    wchar_t url[600];
    swprintf(url, 600, L"%ls.sha256", g_newUrl);
    char *txt;
    DWORD tlen;
    if (!HttpGet(url, &txt, &tlen, 4096)) return false;          // pas d'empreinte = on refuse
    char expected[65], actual[65];
    bool ok = ParseSha256(txt, expected) && Sha256Hex(bin, len, actual) && strcmp(expected, actual) == 0;
    free(txt);
    return ok;
}

static DWORD WINAPI UpdateDownloadThread(LPVOID) {
    char *bin;
    DWORD len;
    WPARAM ok = 0;
    if (HttpGet(g_newUrl, &bin, &len, 16 * 1024 * 1024)) {
        // vrai programme Windows ET empreinte SHA-256 identique a celle publiee
        if (len > 10 * 1024 && bin[0] == 'M' && bin[1] == 'Z' && VerifyDownload(bin, len)) {
            wchar_t exe[MAX_PATH], tmp[MAX_PATH + 8], old[MAX_PATH + 8];
            GetModuleFileNameW(NULL, exe, MAX_PATH);
            swprintf(tmp, MAX_PATH + 8, L"%ls.new", exe);
            swprintf(old, MAX_PATH + 8, L"%ls.old", exe);
            HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD put = 0;
                BOOL w = WriteFile(f, bin, len, &put, NULL) && put == len;
                CloseHandle(f);
                if (w && MoveFileExW(exe, old, MOVEFILE_REPLACE_EXISTING)) {
                    if (MoveFileExW(tmp, exe, MOVEFILE_REPLACE_EXISTING)) ok = 1;
                    else MoveFileExW(old, exe, MOVEFILE_REPLACE_EXISTING);   // on remet l'ancien
                }
                if (!ok) DeleteFileW(tmp);
            }
        }
        free(bin);
    }
    InterlockedExchange(&g_busy, 0);
    if (ok) g_launchNew = true;
    PostMessageW(g_hwnd, WM_UPDATE_READY, ok, 0);
    return 0;
}

void StartUpdateDownload() {
    if (!g_newUrl[0] || InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;
    HANDLE t = CreateThread(NULL, 0, UpdateDownloadThread, NULL, 0, NULL);
    if (t) CloseHandle(t); else g_busy = 0;
}

// Appele a la fermeture : relance la nouvelle version si elle vient d'etre installee.
bool LaunchUpdatedAndExit() {
    if (!g_launchNew) return false;
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = NULL; }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    return (INT_PTR)ShellExecuteW(NULL, L"open", exe, NULL, NULL, SW_SHOWNORMAL) > 32;
}

// ---------- Recherche de ville ----------
static wchar_t g_cityQuery[128];

static DWORD WINAPI CityThread(LPVOID) {
    // encode le nom en UTF-8 pour l'adresse (%C3%A9 pour e accent, etc.)
    char utf8[512], enc[1536];
    WideCharToMultiByte(CP_UTF8, 0, g_cityQuery, -1, utf8, sizeof(utf8), NULL, NULL);
    int e = 0;
    for (unsigned char *p = (unsigned char *)utf8; *p && e < (int)sizeof(enc) - 4; p++) {
        if (isalnum(*p) || *p == '-' || *p == '.' || *p == '_') enc[e++] = (char)*p;
        else e += snprintf(enc + e, 4, "%%%02X", *p);
    }
    enc[e] = 0;
    wchar_t url[1800];
    swprintf(url, 1800, L"https://geocoding-api.open-meteo.com/v1/search?count=1&language=fr&format=json&name=%hs", enc);

    char *json;
    DWORD len;
    WPARAM found = 0;
    if (HttpGet(url, &json, &len, 256 * 1024)) {
        const char *res = strstr(json, "\"results\"");
        double lat, lon;
        wchar_t name[64], region[64], country[64];
        if (res && JsonString(res, "name", name, 64) && JsonNumber(res, "latitude", &lat) &&
            JsonNumber(res, "longitude", &lon)) {
            region[0] = country[0] = 0;
            JsonString(res, "admin1", region, 64);
            JsonString(res, "country", country, 64);
            g_lat = (LONG)lround(lat * 100);
            g_lon = (LONG)lround(lon * 100);
            RegPut(L"Latitude", (DWORD)g_lat);
            RegPut(L"Longitude", (DWORD)g_lon);
            EnterCriticalSection(&g_lock);
            swprintf(g_cityResult, 160, L"%ls%ls%ls%ls%ls", name, region[0] ? L", " : L"", region,
                     country[0] ? L", " : L"", country);
            RegPutStr(L"City", g_cityResult);
            LeaveCriticalSection(&g_lock);
            found = 1;
        }
        free(json);
    }
    InterlockedExchange(&g_cityBusy, 0);
    PostMessageW(g_hwnd, WM_CITY_FOUND, found, 0);
    return 0;
}

void StartCitySearch(const wchar_t *city) {
    if (!city[0] || InterlockedCompareExchange(&g_cityBusy, 1, 0) != 0) return;
    wcsncpy(g_cityQuery, city, 127);
    g_cityQuery[127] = 0;
    HANDLE t = CreateThread(NULL, 0, CityThread, NULL, 0, NULL);
    if (t) CloseHandle(t); else g_cityBusy = 0;
}
