// Installation dans un dossier stable, copie de secours, retour a la version precedente, desinstallation.
// Tout est dans le profil de l'utilisateur : pas besoin d'etre administrateur.
#include "app.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <stdio.h>

#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LuminosityManager"
#define EXE_NAME L"LuminosityManager.exe"

static bool EnvDir(const wchar_t *var, const wchar_t *sub, wchar_t *out) {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(var, base, MAX_PATH);
    if (!n || n > MAX_PATH - 64) return false;
    swprintf(out, MAX_PATH, L"%ls\\%ls", base, sub);
    return true;
}

// %LOCALAPPDATA%\Programs\LuminosityManager
bool InstallDir(wchar_t *dir) { return EnvDir(L"LOCALAPPDATA", L"Programs\\LuminosityManager", dir); }
// %LOCALAPPDATA%\LuminosityManager\backup
static bool BackupDir(wchar_t *dir) { return EnvDir(L"LOCALAPPDATA", L"LuminosityManager\\backup", dir); }

static bool FileExists(const wchar_t *path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Le fichier est-il encore la et lisible ? (Securite Windows peut l'effacer ou le bloquer juste apres l'ecriture)
bool ExeStillThere(const wchar_t *path, DWORD expectedSize) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    char mz[2] = {};
    DWORD got = 0, size = GetFileSize(f, NULL);
    bool ok = ReadFile(f, mz, 2, &got, NULL) && got == 2 && mz[0] == 'M' && mz[1] == 'Z' &&
              (!expectedSize || size == expectedSize);
    CloseHandle(f);
    return ok;
}

bool IsInstalled() {
    wchar_t dir[MAX_PATH], exe[MAX_PATH];
    if (!InstallDir(dir)) return false;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (!slash) return false;
    *slash = 0;
    return _wcsicmp(exe, dir) == 0;
}

// ---------- Copie de secours (avant chaque mise a jour) ----------
// Une seule copie : LuminosityManager-<version>.exe
bool BackupCurrent() {
    wchar_t dir[MAX_PATH], parent[MAX_PATH], exe[MAX_PATH], dst[MAX_PATH], pattern[MAX_PATH];
    if (!BackupDir(dir)) return false;
    wcscpy(parent, dir);
    *wcsrchr(parent, L'\\') = 0;
    CreateDirectoryW(parent, NULL);
    CreateDirectoryW(dir, NULL);
    swprintf(pattern, MAX_PATH, L"%ls\\LuminosityManager-*.exe", dir);   // on retire l'ancienne copie
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            swprintf(dst, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            DeleteFileW(dst);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(dst, MAX_PATH, L"%ls\\LuminosityManager-%ls.exe", dir, APP_VERSION);
    return CopyFileW(exe, dst, FALSE) != 0;
}

// Copie de secours disponible : sa version et son chemin
bool BackupFind(wchar_t *version, int vn, wchar_t *path) {
    wchar_t dir[MAX_PATH], pattern[MAX_PATH];
    if (!BackupDir(dir)) return false;
    swprintf(pattern, MAX_PATH, L"%ls\\LuminosityManager-*.exe", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    const wchar_t *v = fd.cFileName + 18;                  // apres "LuminosityManager-"
    int n = 0;
    while (v[n] && wcscmp(v + n, L".exe") != 0 && n < vn - 1) { version[n] = v[n]; n++; }
    version[n] = 0;
    if (path) swprintf(path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
    return n > 0;
}

// Met newFile a la place de l'app (l'actuelle devient .old). newFile est deplace.
bool SwapInExe(const wchar_t *newFile) {
    wchar_t exe[MAX_PATH], old[MAX_PATH + 8];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(old, MAX_PATH + 8, L"%ls.old", exe);
    if (!MoveFileExW(exe, old, MOVEFILE_REPLACE_EXISTING)) return false;
    if (MoveFileExW(newFile, exe, MOVEFILE_REPLACE_EXISTING)) return true;
    MoveFileExW(old, exe, MOVEFILE_REPLACE_EXISTING);        // on remet l'ancienne
    return false;
}

// La nouvelle version n'a pas demarre : on remet l'ancienne (.old, sinon la copie de secours)
bool RestorePrevious() {
    wchar_t exe[MAX_PATH], old[MAX_PATH + 8], backup[MAX_PATH], ver[32];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(old, MAX_PATH + 8, L"%ls.old", exe);
    for (int i = 0; i < 10; i++) {                           // le fichier peut etre encore verrouille un instant
        if (FileExists(old) && MoveFileExW(old, exe, MOVEFILE_REPLACE_EXISTING)) return true;
        if (BackupFind(ver, 32, backup) && CopyFileW(backup, exe, FALSE)) return true;
        Sleep(500);
    }
    return false;
}

// Remet la copie de secours a la place de l'app (bouton « Revenir a la version precedente »)
bool RollbackPrepare(wchar_t *msg, int n) {
    wchar_t ver[32], backup[MAX_PATH], exe[MAX_PATH], tmp[MAX_PATH + 8];
    if (!BackupFind(ver, 32, backup)) { swprintf(msg, n, L"Aucune copie de secours."); return false; }
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(tmp, MAX_PATH + 8, L"%ls.new", exe);
    if (!CopyFileW(backup, tmp, FALSE) || !SwapInExe(tmp)) {
        DeleteFileW(tmp);
        swprintf(msg, n, L"Impossible de remettre la version %ls.", ver);
        return false;
    }
    swprintf(msg, n, L"Retour à la version %ls…", ver);
    return true;
}

// ---------- Installation ----------
static bool StartMenuShortcut(wchar_t *lnk) {
    wchar_t dir[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, dir) != S_OK) return false;
    swprintf(lnk, MAX_PATH, L"%ls\\%ls.lnk", dir, APP_NAME);
    return true;
}

static bool CreateShortcut(const wchar_t *target, const wchar_t *workDir) {
    wchar_t lnk[MAX_PATH];
    if (!StartMenuShortcut(lnk)) return false;
    IShellLinkW *sl = NULL;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **)&sl))) return false;
    sl->SetPath(target);
    sl->SetWorkingDirectory(workDir);
    sl->SetDescription(L"Luminosité automatique de l'écran");
    sl->SetIconLocation(target, 0);
    IPersistFile *pf = NULL;
    bool ok = SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void **)&pf)) && SUCCEEDED(pf->Save(lnk, TRUE));
    if (pf) pf->Release();
    sl->Release();
    return ok;
}

// Entree dans Parametres > Applications (pour desinstaller comme une app normale)
static void RegisterUninstall(const wchar_t *dir, const wchar_t *target) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    wchar_t cmd[MAX_PATH + 32];
    swprintf(cmd, MAX_PATH + 32, L"\"%ls\" --uninstall", target);
    const wchar_t *strs[][2] = { { L"DisplayName", APP_NAME }, { L"DisplayVersion", APP_VERSION },
                                 { L"Publisher", L"Mede2026" }, { L"DisplayIcon", target },
                                 { L"InstallLocation", dir }, { L"UninstallString", cmd },
                                 { L"URLInfoAbout", L"https://github.com/" REPO } };
    for (auto &s : strs)
        RegSetValueExW(k, s[0], 0, REG_SZ, (const BYTE *)s[1], (DWORD)((wcslen(s[1]) + 1) * sizeof(wchar_t)));
    DWORD one = 1, size = 600;                                 // taille approximative en Ko
    RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE *)&one, 4);
    RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE *)&one, 4);
    RegSetValueExW(k, L"EstimatedSize", 0, REG_DWORD, (const BYTE *)&size, 4);
    RegCloseKey(k);
}

// Met a jour la version affichee dans Parametres > Applications (apres une mise a jour)
void RefreshUninstallEntry() {
    wchar_t dir[MAX_PATH], exe[MAX_PATH];
    if (!IsInstalled() || !InstallDir(dir)) return;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    RegisterUninstall(dir, exe);
    wchar_t lnk[MAX_PATH];
    if (StartMenuShortcut(lnk) && !FileExists(lnk)) CreateShortcut(exe, dir);
}

// Copie l'app dans %LOCALAPPDATA%\Programs\LuminosityManager, cree le raccourci du menu Demarrer.
// target recoit le chemin de la copie installee (a lancer).
bool InstallApp(wchar_t *target, wchar_t *msg, int n) {
    wchar_t dir[MAX_PATH], parent[MAX_PATH], exe[MAX_PATH];
    if (!InstallDir(dir)) { swprintf(msg, n, L"Dossier d'installation introuvable."); return false; }
    wcscpy(parent, dir);
    *wcsrchr(parent, L'\\') = 0;
    CreateDirectoryW(parent, NULL);                          // ...\Programs
    CreateDirectoryW(dir, NULL);
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(target, MAX_PATH, L"%ls\\" EXE_NAME, dir);
    HANDLE f = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size = f != INVALID_HANDLE_VALUE ? GetFileSize(f, NULL) : 0;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!CopyFileW(exe, target, FALSE)) { swprintf(msg, n, L"Impossible de copier l'app (erreur %lu).", GetLastError()); return false; }
    Sleep(1500);                                             // Securite Windows analyse la copie
    if (!ExeStillThere(target, size)) {
        swprintf(msg, n, L"Sécurité Windows a bloqué la copie. Voir la page Mises à jour.");
        return false;
    }
    CreateShortcut(target, dir);
    RegisterUninstall(dir, target);
    swprintf(msg, n, L"Installation…");
    return true;
}

// Desinstalle : raccourci, demarrage, entree Applications, copie de secours, (reglages), puis le dossier.
void UninstallApp(bool keepData) {
    wchar_t lnk[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    SetStartup(false);
    if (StartMenuShortcut(lnk)) DeleteFileW(lnk);
    RegDeleteTreeW(HKEY_CURRENT_USER, UNINSTALL_KEY);
    if (!keepData) {
        RegDeleteTreeW(HKEY_CURRENT_USER, REG_KEY);
        g_noSaveOnExit = 1;
    }
    // Dossiers a effacer une fois l'app fermee (un .exe en marche ne peut pas etre efface)
    wchar_t cmd[4 * MAX_PATH + 200];
    int len = swprintf(cmd, 4 * MAX_PATH + 200, L"cmd.exe /d /c ping -n 6 127.0.0.1 >nul");
    if (EnvDir(L"LOCALAPPDATA", L"LuminosityManager", path))
        len += swprintf(cmd + len, 4 * MAX_PATH + 200 - len, L" & rmdir /s /q \"%ls\"", path);
    if (!keepData && EnvDir(L"APPDATA", L"LuminosityManager", path))
        len += swprintf(cmd + len, 4 * MAX_PATH + 200 - len, L" & rmdir /s /q \"%ls\"", path);
    if (IsInstalled() && InstallDir(dir))
        len += swprintf(cmd + len, 4 * MAX_PATH + 200 - len, L" & rmdir /s /q \"%ls\"", dir);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, sys, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// Apres « Installer » : la copie installee efface le fichier telecharge (quand l'ancienne app est fermee)
static wchar_t g_oldCopy[MAX_PATH];
static DWORD WINAPI DeleteOldCopyThread(LPVOID) {
    for (int i = 0; i < 20; i++) {
        if (DeleteFileW(g_oldCopy) || GetLastError() == ERROR_FILE_NOT_FOUND) return 0;
        Sleep(500);
    }
    return 0;
}

void DeleteOldCopy(const wchar_t *path) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    size_t n = wcslen(path);
    // uniquement un .exe, et jamais l'app en marche
    if (n < 5 || n >= MAX_PATH || _wcsicmp(path + n - 4, L".exe") != 0 || _wcsicmp(path, exe) == 0) return;
    wcscpy(g_oldCopy, path);
    HANDLE t = CreateThread(NULL, 0, DeleteOldCopyThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
