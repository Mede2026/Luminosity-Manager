// Statistiques : une ligne par jour (120 jours), une journee type (24 heures), temps par app, totaux.
// Enregistrees dans %APPDATA%\LuminosityManager\stats.bin (environ 7 Ko).
#include "app.h"
#include <stdio.h>
#include <stddef.h>
#include <psapi.h>

static const int STATS_DAYS = 120, STATS_APPS = 16;
static const DWORD STATS_MAGIC = 0x33534D4C;   // "LMS3" (v0.9.2 : + totaux depuis le debut pour « Tout »)
static const DWORD STATS_MAGIC_V2 = 0x32534D4C; // "LMS2" (v0.9 : + temps processeur de l'app)
static const DWORD STATS_MAGIC_V1 = 0x31534D4C; // "LMS1" : meme debut, sans les champs de la fin

struct DayStat {
    LONG day;                     // numero du jour (heure locale), 0 = vide
    WORD active, paused, off;     // minutes : actif, en pause, app desactivee
    WORD reading, battery, trueTone;
    WORD src[3];                  // minutes par source : capteur, webcam, soleil
    WORD cat[LIGHT_CATS];         // minutes par categorie de lumiere
    DWORD sumBright;              // somme des % de luminosite (1 par minute active)
    float sumLogLux;              // somme de log10(lux + 1)
    DWORD sumDisplayK;            // somme des Kelvin de l'ecran (minutes True Tone)
    WORD minLux, maxLux;
    WORD adjusts, manual, photos;
};
// Depuis le debut (jamais efface, meme apres 120 jours) : pour « Tout »
struct AllStat {
    DWORD active, paused, off, reading, battery, trueTone;
    DWORD src[3], cat[LIGHT_CATS];
    double sumBright, sumLogLux, sumDisplayK;
    DWORD minLux, maxLux, adjusts, manual, photos, cpuMs;
};
struct HourStat { DWORD minutes, sumBright; float sumLogLux; };
struct AppStat { wchar_t exe[40]; DWORD minutes; };
struct StatsData {
    DWORD magic;
    LONG firstDay;
    DayStat days[STATS_DAYS];
    HourStat hours[24];
    AppStat apps[STATS_APPS];
    DWORD totalMinutes, totalPhotos, totalAdjusts, totalManual, totalReading, totalPaused;
    // v0.9 (LMS2) : ajoutes a la fin, pour relire les anciens fichiers
    DWORD cpuMs[STATS_DAYS];      // temps processeur de l'app, par jour (meme case que days[])
    DWORD totalCpuMs, totalRunMinutes;
    AllStat all;                  // v0.9.2 (LMS3)
};
static const size_t STATS_V1_SIZE = offsetof(StatsData, cpuMs);
static const size_t STATS_V2_SIZE = offsetof(StatsData, all);

static StatsData g_st;
static bool g_dirty;

bool AppDataFile(const wchar_t *name, wchar_t *path, bool create) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    if (!n || n > MAX_PATH - 60) return false;
    wcscat(dir, L"\\LuminosityManager");
    if (create) CreateDirectoryW(dir, NULL);
    swprintf(path, MAX_PATH, L"%ls\\%ls", dir, name);
    return true;
}

// Ancien fichier (sans totaux « Tout ») : on les reconstruit avec les jours gardes et les totaux existants
static void RebuildAll() {
    AllStat &a = g_st.all;
    memset(&a, 0, sizeof(a));
    a.minLux = 0xFFFF;
    for (int i = 0; i < STATS_DAYS; i++) {
        const DayStat &d = g_st.days[i];
        if (!d.day) continue;
        a.active += d.active; a.paused += d.paused; a.off += d.off;
        a.reading += d.reading; a.battery += d.battery; a.trueTone += d.trueTone;
        for (int k = 0; k < 3; k++) a.src[k] += d.src[k];
        for (int k = 0; k < LIGHT_CATS; k++) a.cat[k] += d.cat[k];
        a.sumBright += d.sumBright; a.sumLogLux += d.sumLogLux; a.sumDisplayK += d.sumDisplayK;
        if (d.minLux < a.minLux) a.minLux = d.minLux;
        if (d.maxLux > a.maxLux) a.maxLux = d.maxLux;
        a.adjusts += d.adjusts; a.manual += d.manual; a.photos += d.photos;
        a.cpuMs += g_st.cpuMs[i];
    }
    // ces totaux-la existaient deja depuis le debut : plus justes que les 120 derniers jours
    if (g_st.totalMinutes > a.active) a.active = g_st.totalMinutes;
    if (g_st.totalPaused > a.paused) a.paused = g_st.totalPaused;
    if (g_st.totalReading > a.reading) a.reading = g_st.totalReading;
    if (g_st.totalPhotos > a.photos) a.photos = g_st.totalPhotos;
    if (g_st.totalAdjusts > a.adjusts) a.adjusts = g_st.totalAdjusts;
    if (g_st.totalManual > a.manual) a.manual = g_st.totalManual;
}

void StatsLoad() {
    memset(&g_st, 0, sizeof(g_st));
    wchar_t path[MAX_PATH];
    if (AppDataFile(L"stats.bin", path, false)) {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD got = 0;
            StatsData *tmp = (StatsData *)calloc(1, sizeof(StatsData));
            if (tmp && ReadFile(f, tmp, sizeof(StatsData), &got, NULL) &&
                ((got == sizeof(StatsData) && tmp->magic == STATS_MAGIC) ||
                 (got == STATS_V2_SIZE && tmp->magic == STATS_MAGIC_V2) ||
                 (got == STATS_V1_SIZE && tmp->magic == STATS_MAGIC_V1))) {  // ancien fichier : nouveaux champs a 0
                g_st = *tmp;
                if (tmp->magic != STATS_MAGIC) RebuildAll();
            }
            free(tmp);
            CloseHandle(f);
        }
    }
    g_st.magic = STATS_MAGIC;
    if (!g_st.firstDay) { g_st.firstDay = NowMinute() / 1440; g_st.all.minLux = 0xFFFF; }
}

void StatsSave() {
    wchar_t path[MAX_PATH];
    if (!AppDataFile(L"stats.bin", path, true)) return;
    EnterCriticalSection(&g_lock);
    StatsData copy = g_st;
    g_dirty = false;
    LeaveCriticalSection(&g_lock);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put;
    WriteFile(f, &copy, sizeof(copy), &put, NULL);
    CloseHandle(f);
}

// Ligne du jour (la cree si besoin ; a appeler avec g_lock pris)
static DayStat &Today() {
    LONG day = NowMinute() / 1440;
    DayStat &d = g_st.days[day % STATS_DAYS];
    if (d.day != day) { memset(&d, 0, sizeof(d)); d.day = day; d.minLux = 0xFFFF; g_st.cpuMs[day % STATS_DAYS] = 0; }
    return d;
}

static void Inc(WORD &w) { if (w < 0xFFFF) w++; }

// Temps processeur utilise par l'app depuis son lancement (ms)
static ULONGLONG ProcessCpuMs() {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    return ((((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10000;
}

void StatsMinute(const StatsSample &s) {
    static ULONGLONG lastCpu;
    ULONGLONG cpu = ProcessCpuMs();
    DWORD dCpu = lastCpu && cpu > lastCpu ? (DWORD)(cpu - lastCpu) : 0;
    lastCpu = cpu;
    EnterCriticalSection(&g_lock);
    DayStat &d = Today();
    AllStat &a = g_st.all;
    g_st.cpuMs[d.day % STATS_DAYS] += dCpu;
    g_st.totalCpuMs += dCpu;
    a.cpuMs += dCpu;
    g_st.totalRunMinutes++;
    if (s.off) { Inc(d.off); a.off++; }
    else if (s.paused) { Inc(d.paused); g_st.totalPaused++; a.paused++; }
    else {
        Inc(d.active);
        a.active++;
        a.sumBright += s.bright < 0 ? 0 : s.bright;
        if (s.source >= 0 && s.source < 3) a.src[s.source]++;
        if (s.trueToneK > 0) { a.trueTone++; a.sumDisplayK += s.trueToneK; }
        if (s.reading) a.reading++;
        if (s.battery) a.battery++;
        g_st.totalMinutes++;
        d.sumBright += s.bright < 0 ? 0 : s.bright;
        if (s.source >= 0 && s.source < 3) Inc(d.src[s.source]);
        if (s.lux >= 0) {
            double lg = log10(s.lux + 1);
            d.sumLogLux += (float)lg;
            Inc(d.cat[LightCategory(s.lux)]);
            a.cat[LightCategory(s.lux)]++;
            a.sumLogLux += lg;
            WORD l = (WORD)(s.lux > 65000 ? 65000 : s.lux);
            if (l < a.minLux) a.minLux = l;
            if (l > a.maxLux) a.maxLux = l;
            if (l < d.minLux) d.minLux = l;
            if (l > d.maxLux) d.maxLux = l;
            HourStat &h = g_st.hours[(NowMinute() % 1440) / 60];
            h.minutes++;
            h.sumBright += s.bright < 0 ? 0 : s.bright;
            h.sumLogLux += (float)lg;
        }
        if (s.trueToneK > 0) { Inc(d.trueTone); d.sumDisplayK += s.trueToneK; }
        if (s.reading) { Inc(d.reading); g_st.totalReading++; }
        if (s.battery) Inc(d.battery);
        if (s.app && s.app[0]) {                 // temps passe avec un profil d'app
            int i = 0, freeSlot = -1, smallest = 0;
            for (; i < STATS_APPS; i++) {
                if (!g_st.apps[i].exe[0]) { if (freeSlot < 0) freeSlot = i; continue; }
                if (_wcsicmp(g_st.apps[i].exe, s.app) == 0) break;
                if (g_st.apps[i].minutes < g_st.apps[smallest].minutes) smallest = i;
            }
            if (i == STATS_APPS) {               // nouvelle app (remplace la moins utilisee si plein)
                i = freeSlot >= 0 ? freeSlot : smallest;
                wcsncpy(g_st.apps[i].exe, s.app, 39);
                g_st.apps[i].exe[39] = 0;
                g_st.apps[i].minutes = 0;
            }
            g_st.apps[i].minutes++;
        }
    }
    g_dirty = true;
    LeaveCriticalSection(&g_lock);
    if (NowMinute() % 10 == 0) StatsSave();     // sauvegarde toutes les 10 minutes
}

void StatsEvent(int type) {
    EnterCriticalSection(&g_lock);
    DayStat &d = Today();
    if (type == STAT_PHOTO) { Inc(d.photos); g_st.totalPhotos++; g_st.all.photos++; }
    else if (type == STAT_ADJUST) { Inc(d.adjusts); g_st.totalAdjusts++; g_st.all.adjusts++; }
    else if (type == STAT_MANUAL) { Inc(d.manual); g_st.totalManual++; g_st.all.manual++; }
    g_dirty = true;
    LeaveCriticalSection(&g_lock);
}

static void DayDate(LONG day, wchar_t *out) {
    ULONGLONG t = (ULONGLONG)day * 1440ULL * 600000000ULL;
    FILETIME ft = { (DWORD)t, (DWORD)(t >> 32) };
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    swprintf(out, 16, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
}

// Tout pour la page Statistiques (90 derniers jours)
void StatsJson(JsonOut &j) {
    EnterCriticalSection(&g_lock);
    LONG today = NowMinute() / 1440;
    wchar_t date[16];
    DayDate(today, date);
    j.KStr(L"today", date);
    DayDate(g_st.firstDay, date);
    j.KStr(L"since", date);
    j.Key(L"days");
    j.Raw(L"[");
    bool first = true;
    for (LONG day = today - (STATS_DAYS - 1); day <= today; day++) {
        const DayStat &d = g_st.days[day % STATS_DAYS];
        if (d.day != day) continue;
        j.Raw(first ? L"{" : L",{");
        first = false;
        DayDate(day, date);
        j.KStr(L"date", date);
        j.Num(L"ago", today - day);
        j.Num(L"active", d.active);
        j.Num(L"paused", d.paused);
        j.Num(L"off", d.off);
        j.Num(L"reading", d.reading);
        j.Num(L"battery", d.battery);
        j.Num(L"trueTone", d.trueTone);
        j.Num(L"bright", d.active ? (double)d.sumBright / d.active : -1);
        int luxMin = 0;
        for (int c = 0; c < LIGHT_CATS; c++) luxMin += d.cat[c];
        j.Num(L"lux", luxMin ? pow(10.0, d.sumLogLux / luxMin) - 1 : -1);
        j.Num(L"minLux", d.minLux == 0xFFFF ? -1 : d.minLux);
        j.Num(L"maxLux", d.maxLux);
        j.Num(L"displayK", d.trueTone ? (double)d.sumDisplayK / d.trueTone : -1);
        j.Key(L"src");
        j.Raw(L"[%u,%u,%u]", d.src[0], d.src[1], d.src[2]);
        j.Key(L"cat");
        j.Raw(L"[");
        for (int c = 0; c < LIGHT_CATS; c++) j.Raw(c ? L",%u" : L"%u", d.cat[c]);
        j.Raw(L"]");
        j.Num(L"adjusts", d.adjusts);
        j.Num(L"manual", d.manual);
        j.Num(L"photos", d.photos);
        j.Num(L"cpuMs", g_st.cpuMs[day % STATS_DAYS]);
        j.Raw(L"}");
    }
    j.Raw(L"]");
    j.Key(L"hours");
    j.Raw(L"[");
    for (int h = 0; h < 24; h++) {
        const HourStat &x = g_st.hours[h];
        j.Raw(h ? L",[%.1f,%.1f]" : L"[%.1f,%.1f]", x.minutes ? (double)x.sumBright / x.minutes : -1.0,
              x.minutes ? pow(10.0, x.sumLogLux / x.minutes) - 1 : -1.0);
    }
    j.Raw(L"]");
    j.Key(L"apps");
    j.Raw(L"[");
    first = true;
    for (int i = 0; i < STATS_APPS; i++) {
        if (!g_st.apps[i].exe[0]) continue;
        j.Raw(first ? L"{" : L",{");
        first = false;
        j.KStr(L"exe", g_st.apps[i].exe);
        j.Num(L"minutes", g_st.apps[i].minutes);
        j.Raw(L"}");
    }
    j.Raw(L"]");
    j.Num(L"totalMinutes", g_st.totalMinutes);
    j.Num(L"totalPhotos", g_st.totalPhotos);
    j.Num(L"totalAdjusts", g_st.totalAdjusts);
    j.Num(L"totalManual", g_st.totalManual);
    j.Num(L"totalReading", g_st.totalReading);
    j.Num(L"totalPaused", g_st.totalPaused);
    // « Tout » : depuis le debut, meme format qu'un jour
    const AllStat &a = g_st.all;
    int luxMin = 0;
    for (int c = 0; c < LIGHT_CATS; c++) luxMin += a.cat[c];
    j.Key(L"all");
    j.Raw(L"{");
    j.Num(L"ago", 0);
    j.Num(L"active", a.active);
    j.Num(L"paused", a.paused);
    j.Num(L"off", a.off);
    j.Num(L"reading", a.reading);
    j.Num(L"battery", a.battery);
    j.Num(L"trueTone", a.trueTone);
    j.Num(L"bright", a.active ? a.sumBright / a.active : -1);
    j.Num(L"lux", luxMin ? pow(10.0, a.sumLogLux / luxMin) - 1 : -1);
    j.Num(L"minLux", a.minLux == 0xFFFF ? -1 : (double)a.minLux);
    j.Num(L"maxLux", a.maxLux);
    j.Num(L"displayK", a.trueTone ? a.sumDisplayK / a.trueTone : -1);
    j.Key(L"src");
    j.Raw(L"[%lu,%lu,%lu]", a.src[0], a.src[1], a.src[2]);
    j.Key(L"cat");
    j.Raw(L"[");
    for (int c = 0; c < LIGHT_CATS; c++) j.Raw(c ? L",%lu" : L"%lu", a.cat[c]);
    j.Raw(L"]");
    j.Num(L"adjusts", a.adjusts);
    j.Num(L"manual", a.manual);
    j.Num(L"photos", a.photos);
    j.Num(L"cpuMs", a.cpuMs);
    j.Raw(L"}");
    j.Num(L"totalCpuMs", g_st.totalCpuMs);
    j.Num(L"totalRunMinutes", g_st.totalRunMinutes);
    LeaveCriticalSection(&g_lock);
}

void StatsReset() {
    EnterCriticalSection(&g_lock);
    memset(&g_st, 0, sizeof(g_st));
    g_st.magic = STATS_MAGIC;
    g_st.firstDay = NowMinute() / 1440;
    g_st.all.minLux = 0xFFFF;
    LeaveCriticalSection(&g_lock);
    StatsSave();
}

// Consommation de l'app aujourd'hui : processeur (%), photos par heure active, memoire (Mo)
void StatsAppUsage(double *cpuPct, double *photosPerHour, double *memMb) {
    EnterCriticalSection(&g_lock);
    DayStat &d = Today();
    double run = (double)d.active + d.paused + d.off;
    *cpuPct = CpuPercent(g_st.cpuMs[d.day % STATS_DAYS], run);
    *photosPerHour = d.active ? d.photos * 60.0 / d.active : 0;
    LeaveCriticalSection(&g_lock);
    PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
    *memMb = K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? pmc.WorkingSetSize / 1048576.0 : 0;
}
