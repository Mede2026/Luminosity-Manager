// Sauvegarde : exporter / importer TOUTES les donnees de l'app dans un fichier .json
// (reglages, profils, apprentissage, historique 24 h, statistiques).
#include "app.h"
#include <commdlg.h>
#include <wincrypt.h>
#include <stdio.h>

static const wchar_t *PROFILES_KEY = REG_KEY L"\\Profiles";
static const wchar_t *FILES[2] = { L"history.bin", L"stats.bin" };
volatile LONG g_noSaveOnExit;                   // apres un import : ne pas ecraser les fichiers importes

// ---------- Outils ----------
static bool ReadWholeFile(const wchar_t *path, BYTE **data, DWORD *len) {
    *data = NULL;
    *len = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, NULL), got = 0;
    bool ok = size != INVALID_FILE_SIZE && size < 32 * 1024 * 1024;
    if (ok) {
        *data = (BYTE *)malloc(size + 1);
        ok = *data && ReadFile(f, *data, size, &got, NULL) && got == size;
        if (ok) { (*data)[size] = 0; *len = size; }
        else { free(*data); *data = NULL; }
    }
    CloseHandle(f);
    return ok;
}

static bool WriteWholeFile(const wchar_t *path, const void *data, DWORD len) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data, len, &put, NULL) && put == len;
    CloseHandle(f);
    return ok;
}

// Toutes les valeurs d'une cle du registre : nombres et textes
static void ExportKey(JsonOut &j, const wchar_t *key, const wchar_t *prefix) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; i++) {
        wchar_t name[128], full[160];
        BYTE data[1024];
        DWORD nameLen = 128, type, size = sizeof(data) - 2;
        LONG r = RegEnumValueW(k, i, name, &nameLen, NULL, &type, data, &size);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        swprintf(full, 160, L"%ls%ls", prefix, name);
        if (type == REG_DWORD) j.Num(full, (LONG)*(DWORD *)data);
        else if (type == REG_SZ) { data[size] = data[size + 1] = 0; j.KStr(full, (const wchar_t *)data); }
    }
    RegCloseKey(k);
}

static bool AskFile(bool save, wchar_t *path) {
    OPENFILENAMEW of = {};
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_hwnd;
    of.lpstrFilter = L"Sauvegarde Luminosity Manager (*.json)\0*.json\0Tous les fichiers\0*.*\0";
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH;
    of.lpstrDefExt = L"json";
    if (save) {
        of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        return GetSaveFileNameW(&of) != 0;
    }
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameW(&of) != 0;
}

// ---------- Exporter ----------
bool ExportData(wchar_t *result, int n) {
    HistSave();                                  // fichiers a jour avant de les copier
    StatsSave();
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"LuminosityManager-sauvegarde-%04d-%02d-%02d.json", t.wYear, t.wMonth, t.wDay);
    if (!AskFile(true, path)) { result[0] = 0; return false; }

    JsonOut j;
    j.Raw(L"{\n");
    j.KStr(L"app", APP_NAME);
    j.Num(L"format", 1);
    j.KStr(L"version", APP_VERSION);
    wchar_t when[32];
    swprintf(when, 32, L"%04d-%02d-%02d %02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    j.KStr(L"exported", when);
    ExportKey(j, REG_KEY, L"reg.");
    ExportKey(j, PROFILES_KEY, L"profile.");
    for (const wchar_t *name : FILES) {
        wchar_t fp[MAX_PATH], key[64];
        BYTE *data;
        DWORD len;
        if (!AppDataFile(name, fp, false) || !ReadWholeFile(fp, &data, &len)) continue;
        DWORD chars = 0;
        CryptBinaryToStringW(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &chars);
        wchar_t *b64 = (wchar_t *)malloc((chars + 1) * sizeof(wchar_t));
        if (b64 && CryptBinaryToStringW(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &chars)) {
            swprintf(key, 64, L"file.%ls", name);
            j.KStr(key, b64);
        }
        free(b64);
        free(data);
    }
    j.Raw(L"\n}\n");

    // Enregistre en UTF-8
    bool ok = false;
    if (j.buf) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, j.buf, -1, NULL, 0, NULL, NULL);
        char *utf8 = (char *)malloc(bytes);
        if (utf8) {
            WideCharToMultiByte(CP_UTF8, 0, j.buf, -1, utf8, bytes, NULL, NULL);
            ok = WriteWholeFile(path, utf8, (DWORD)strlen(utf8));
            free(utf8);
        }
    }
    free(j.buf);
    const wchar_t *file = wcsrchr(path, L'\\');
    swprintf(result, n, ok ? L"Sauvegarde enregistrée : %ls" : L"Impossible d'enregistrer %ls", file ? file + 1 : path);
    return ok;
}

// ---------- Importer ----------
struct ImportCtx { bool valid; int regCount, profileCount, fileCount; };

static void ApplyEntry(const wchar_t *key, bool isStr, const wchar_t *str, double num, void *user) {
    ImportCtx *c = (ImportCtx *)user;
    if (!wcsncmp(key, L"reg.", 4)) {
        if (isStr) RegPutStr(key + 4, str);
        else RegPut(key + 4, (DWORD)(LONG)lround(num));
        c->regCount++;
    } else if (!wcsncmp(key, L"profile.", 8) && !isStr) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, PROFILES_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
            DWORD v = (DWORD)lround(num);
            RegSetValueExW(k, key + 8, 0, REG_DWORD, (BYTE *)&v, sizeof(v));
            RegCloseKey(k);
            c->profileCount++;
        }
    } else if (!wcsncmp(key, L"file.", 5) && isStr) {
        bool known = false;
        for (const wchar_t *name : FILES) if (!wcscmp(key + 5, name)) known = true;   // pas d'autres fichiers
        if (!known) return;
        DWORD len = 0;
        if (!CryptStringToBinaryW(str, 0, CRYPT_STRING_BASE64, NULL, &len, NULL, NULL) || !len) return;
        BYTE *data = (BYTE *)malloc(len);
        wchar_t fp[MAX_PATH];
        if (data && CryptStringToBinaryW(str, 0, CRYPT_STRING_BASE64, data, &len, NULL, NULL) &&
            AppDataFile(key + 5, fp, true) && WriteWholeFile(fp, data, len))
            c->fileCount++;
        free(data);
    }
}

static void CheckEntry(const wchar_t *key, bool isStr, const wchar_t *str, double, void *user) {
    if (!wcscmp(key, L"app") && isStr && !wcscmp(str, APP_NAME)) ((ImportCtx *)user)->valid = true;
}

// Renvoie true si l'app doit redemarrer (donnees remplacees)
bool ImportData(wchar_t *result, int n) {
    wchar_t path[MAX_PATH] = L"";
    result[0] = 0;
    if (!AskFile(false, path)) return false;
    BYTE *data;
    DWORD len;
    if (!ReadWholeFile(path, &data, &len)) { swprintf(result, n, L"Impossible de lire ce fichier."); return false; }
    const char *text = (const char *)data;
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) text += 3;   // BOM UTF-8
    ImportCtx ctx = {};
    JsonForEach(text, CheckEntry, &ctx);
    if (!ctx.valid) {
        free(data);
        swprintf(result, n, L"Ce fichier n'est pas une sauvegarde de Luminosity Manager.");
        return false;
    }
    if (MessageBoxW(g_hwnd, L"Remplacer tous tes réglages, profils, apprentissages et statistiques par ceux de cette sauvegarde ?\n\n"
                            L"L'app va redémarrer.", APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) {
        free(data);
        return false;
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, REG_KEY);  // on repart de zero, puis on remet tout
    JsonForEach(text, ApplyEntry, &ctx);
    free(data);
    if (StartupEnabled()) SetStartup(true);
    g_noSaveOnExit = 1;
    swprintf(result, n, L"Sauvegarde importée : %d réglages, %d profils, %d fichiers. Redémarrage…",
             ctx.regCount, ctx.profileCount, ctx.fileCount);
    return true;
}
