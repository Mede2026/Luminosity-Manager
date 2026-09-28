// Petits outils JSON : lire une valeur dans un objet simple, ecrire du JSON (pour la fenetre).
#include "app.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// Lit la chaine JSON qui suit "key": dans src (gere \" \\ \/ et \uXXXX). Renvoie false si absente.
bool JsonString(const char *src, const char *key, wchar_t *out, int n) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(src, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    char tmp[1024];
    int len = 0;
    while (*p && *p != '"' && len < (int)sizeof(tmp) - 4) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {        // \uXXXX -> UTF-8
                char hex[5] = { p[1], p[2], p[3], p[4], 0 };
                unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                if (cp < 0x80) tmp[len++] = (char)cp;
                else if (cp < 0x800) { tmp[len++] = (char)(0xC0 | (cp >> 6)); tmp[len++] = (char)(0x80 | (cp & 0x3F)); }
                else { tmp[len++] = (char)(0xE0 | (cp >> 12)); tmp[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                       tmp[len++] = (char)(0x80 | (cp & 0x3F)); }
                p += 5;
                continue;
            }
            tmp[len++] = *p == 'n' ? ' ' : *p;
            p++;
            continue;
        }
        tmp[len++] = *p++;
    }
    tmp[len] = 0;
    MultiByteToWideChar(CP_UTF8, 0, tmp, -1, out, n);
    out[n - 1] = 0;
    return true;
}

bool JsonNumber(const char *src, const char *key, double *v) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(src, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    char *end;
    *v = strtod(p, &end);
    return end != p;
}

// ---------- Ecriture ----------
void JsonOut::Grow(size_t need) {
    if (len + need + 1 <= cap) return;
    size_t nc = cap ? cap : 1024;
    while (len + need + 1 > nc) nc *= 2;
    wchar_t *nb = (wchar_t *)realloc(buf, nc * sizeof(wchar_t));
    if (!nb) return;
    buf = nb;
    cap = nc;
}

void JsonOut::Raw(const wchar_t *fmt, ...) {
    wchar_t tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vswprintf(tmp, 512, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    Grow((size_t)n);
    if (len + n + 1 > cap) return;
    memcpy(buf + len, tmp, n * sizeof(wchar_t));
    len += n;
    buf[len] = 0;
}

// Chaine entre guillemets, avec les caracteres speciaux echappes
void JsonOut::Str(const wchar_t *s) {
    Grow(wcslen(s) * 6 + 2);
    if (!buf || len + wcslen(s) * 6 + 3 > cap) return;
    buf[len++] = L'"';
    for (; *s; s++) {
        if (*s == L'"' || *s == L'\\') { buf[len++] = L'\\'; buf[len++] = *s; }
        else if (*s < 32) len += swprintf(buf + len, 7, L"\\u%04x", (unsigned)*s);
        else buf[len++] = *s;
    }
    buf[len++] = L'"';
    buf[len] = 0;
}

void JsonOut::Key(const wchar_t *k) {
    if (len && buf[len - 1] != L'{' && buf[len - 1] != L'[') Raw(L",");
    Str(k);
    Raw(L":");
}

void JsonOut::Num(const wchar_t *k, double v) {
    Key(k);
    if (v == (double)(long long)v) Raw(L"%lld", (long long)v);
    else Raw(L"%.3f", v);
}

void JsonOut::Bool(const wchar_t *k, bool v) { Key(k); Raw(v ? L"true" : L"false"); }
void JsonOut::KStr(const wchar_t *k, const wchar_t *v) { Key(k); Str(v); }
