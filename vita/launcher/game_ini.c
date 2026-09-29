// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * game_ini.c — see game_ini.h.
 *
 * CP932 tables in sjis_table.h are generated from Python's cp932 codec.
 */
#include "game_ini.h"
#include "sjis_table.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* ---- CP932 -> UTF-8 ----------------------------------------------------- */

static int utf8_put(uint32_t cp, char *out, size_t cap, size_t *used)
{
    char buf[4];
    size_t n;

    if (cp < 0x80) {
        buf[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }

    if (*used + n + 1 > cap)
        return -1;
    memcpy(out + *used, buf, n);
    *used += n;
    return 0;
}

/*
 * Shared decoder. `truncate` = 0 keeps sjis_to_utf8's published contract
 * (overflow is an error); `truncate` = 1 stops at the last whole code point
 * that fits, which is what a title field wants.
 */
static int sjis_decode(const char *in, size_t in_len, char *out, size_t out_cap,
                       int truncate)
{
    size_t i = 0, used = 0;

    if (!out || out_cap == 0)
        return -1;
    out[0] = '\0';

    while (i < in_len) {
        unsigned char b = (unsigned char)in[i];
        uint32_t cp;

        if (b == 0)
            break; /* embedded NUL: treat as end (INI values are C strings) */

        if (b < 0x80) {
            cp = b;
            i += 1;
        } else {
            uint8_t li = sjis_lead_index[b];
            if (li != 0xFF && i + 1 < in_len) {
                uint8_t ti = sjis_trail_index[(unsigned char)in[i + 1]];
                if (ti != 0xFF) {
                    cp = sjis_double[li * SJIS_TRAIL_COUNT + ti];
                    i += 2;
                    if (cp == 0)
                        cp = 0xFFFD; /* valid trail but unmapped pair */
                } else {
                    cp = 0xFFFD;
                    i += 1;
                }
            } else {
                /* single-byte high: halfwidth katakana or invalid */
                cp = sjis_single[b];
                if (cp == 0)
                    cp = 0xFFFD;
                i += 1;
            }
        }

        if (utf8_put(cp, out, out_cap, &used) != 0) {
            out[used] = '\0';
            if (!truncate)
                return -1;
            break; /* stop on the last whole code point that fitted */
        }
    }

    out[used] = '\0';
    return (int)used;
}

int sjis_to_utf8(const char *in, size_t in_len, char *out, size_t out_cap)
{
    return sjis_decode(in, in_len, out, out_cap, 0);
}

/* ---- Library= / Scripts= -> rgss version -------------------------------- */

int rgss_version_from_library(const char *library)
{
    char up[128];
    size_t n, i;

    if (!library || !library[0])
        return 0;

    n = strlen(library);
    if (n >= sizeof(up))
        n = sizeof(up) - 1;
    for (i = 0; i < n; i++)
        up[i] = (char)toupper((unsigned char)library[i]);
    up[n] = '\0';

    /* Must look like an RGSS DLL at all. */
    if (strstr(up, "RGSS") == NULL)
        return 0;

    /* First digit after "RGSS" is the generation. */
    {
        const char *p = strstr(up, "RGSS");
        if (p && p[4] >= '1' && p[4] <= '3')
            return p[4] - '0';
    }
    return 0;
}

int rgss_version_from_scripts(const char *scripts)
{
    size_t n;

    if (!scripts || !scripts[0])
        return 0;

    n = strlen(scripts);
    /* mkxp-z walks back to the last '.' and strcmp's the suffix. */
    if (n >= 8 && strcmp(scripts + n - 8, ".rvdata2") == 0)
        return 3;
    if (n >= 7 && strcmp(scripts + n - 7, ".rvdata") == 0)
        return 2;
    /* Any other non-empty Scripts= (including .rxdata) is RGSS1. */
    return 1;
}

/* ---- INI parse ---------------------------------------------------------- */

/* mkxp-z's trim set, iniconfig.cpp: "\t\n\v\f\r ". */
static int is_ini_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
           c == '\r';
}

/*
 * Trim in place, both ends, using mkxp-z's own whitespace set
 * ("\t\n\v\f\r " — iniconfig.cpp `trim`). Returns the new length.
 */
static size_t trim_inplace(char *s)
{
    size_t n = strlen(s);
    size_t a = 0, b;

    while (a < n && is_ini_space(s[a]))
        a++;
    b = n;
    while (b > a && is_ini_space(s[b - 1]))
        b--;
    if (a > 0)
        memmove(s, s + a, b - a);
    s[b - a] = '\0';
    return b - a;
}

/* Case-insensitive whole-string compare, ASCII only (no locale). */
static int key_eq(const char *a, const char *b)
{
    size_t i;

    for (i = 0; a[i] || b[i]; i++) {
        if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

/*
 * Validate UTF-8 and replace invalid bytes with U+FFFD, without splitting
 * output code points. Kept here so game_ini.c still links on its own.
 */
static void copy_utf8_truncating(const char *in, size_t len, char *out, size_t cap)
{
    size_t i = 0, used = 0;

    if (!out || cap == 0)
        return;
    out[0] = '\0';
    while (i < len && in[i]) {
        unsigned char b = (unsigned char)in[i];
        size_t n, k;
        uint32_t cp;
        int bad = 0;

        if (b < 0x80) {
            cp = b;
            n = 1;
        } else if (b >= 0xC2 && b < 0xE0) {
            cp = b & 0x1F;
            n = 2;
        } else if (b >= 0xE0 && b < 0xF0) {
            cp = b & 0x0F;
            n = 3;
        } else if (b >= 0xF0 && b < 0xF5) {
            cp = b & 0x07;
            n = 4;
        } else {
            cp = 0xFFFD;
            n = 1;
            bad = 1;
        }

        if (n > len - i) {
            bad = 1;
        } else {
            for (k = 1; k < n; k++) {
                unsigned char c = (unsigned char)in[i + k];
                if ((c & 0xC0) != 0x80) {
                    bad = 1;
                    break;
                }
                cp = (cp << 6) | (c & 0x3F);
            }
        }
        if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            bad = 1;
        if (bad) {
            cp = 0xFFFD;
            n = 1; /* resynchronise one byte at a time */
        }
        if (utf8_put(cp, out, cap, &used) != 0)
            break;
        i += n;
    }
    out[used] = '\0';
}

/*
 * Detect whether the title bytes look like CP932 rather than UTF-8.
 *
 * Heuristic: bytes without a UTF-8 lead/continuation shape use CP932.
 * ASCII and UTF-8-shaped input stay on the UTF-8 path, where scalar ranges
 * are validated separately. This preserves already-transcoded CJK titles.
 */
static int looks_like_sjis(const unsigned char *p, size_t n)
{
    size_t i = 0;

    /* UTF-8 shape scan; not a Unicode scalar-value validation. */
    while (i < n) {
        unsigned char b = p[i];
        size_t need;

        if (b < 0x80) {
            i++;
            continue;
        }
        if ((b & 0xE0) == 0xC0)
            need = 1;
        else if ((b & 0xF0) == 0xE0)
            need = 2;
        else if ((b & 0xF8) == 0xF0)
            need = 3;
        else
            return 1; /* not UTF-8: treat as CP932 */

        if (i + need >= n)
            return 1;
        {
            size_t k;
            for (k = 1; k <= need; k++) {
                if ((p[i + k] & 0xC0) != 0x80)
                    return 1;
            }
        }
        i += need + 1;
    }
    return 0; /* UTF-8-shaped (or pure ASCII) */
}

static void set_title(GameIni *out, const char *value)
{
    size_t n = strlen(value);

    if (looks_like_sjis((const unsigned char *)value, n)) {
        out->title_was_sjis = 1;
        sjis_decode(value, n, out->title, sizeof(out->title), 1);
    } else {
        out->title_was_sjis = 0;
        copy_utf8_truncating(value, n, out->title, sizeof(out->title));
    }
}

/* Store a trimmed value in a fixed field, dropping whatever does not fit. */
static void store(char *field, size_t cap, const char *value)
{
    size_t n = strlen(value);

    if (n >= cap)
        n = cap - 1;
    memcpy(field, value, n);
    field[n] = '\0';
}

int game_ini_parse(const char *data, size_t len, GameIni *out)
{
    char linebuf[512];
    size_t i = 0;
    int in_game = 0;

    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (!data)
        len = 0;

    /* UTF-8 BOM: mkxp-z chokes on it, we skip it. */
    if (len >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        i = 3;

    while (i < len) {
        size_t start = i, ll;
        char *eq, *value;

        while (i < len && data[i] != '\n')
            i++;
        ll = i - start;
        if (i < len)
            i++; /* consume '\n' */

        if (ll >= sizeof(linebuf))
            ll = sizeof(linebuf) - 1;
        memcpy(linebuf, data + start, ll);
        linebuf[ll] = '\0';
        ll = trim_inplace(linebuf); /* also eats the '\r' of a CRLF file */

        if (ll == 0 || linebuf[0] == ';' || linebuf[0] == '#')
            continue;

        if (linebuf[0] == '[') {
            /*
             * mkxp-z: name = line.substr(1, line.find_last_of(']') - 1), i.e.
             * everything between the '[' and the LAST ']'. With no ']' at all
             * it takes the rest of the line.
             */
            char name[64];
            char *close = strrchr(linebuf, ']');
            size_t nlen = close ? (size_t)(close - linebuf - 1) : ll - 1;

            if (nlen >= sizeof(name))
                nlen = sizeof(name) - 1;
            memcpy(name, linebuf + 1, nlen);
            name[nlen] = '\0';
            trim_inplace(name);
            in_game = key_eq(name, "Game");
            continue;
        }

        /* Only [Game] counts; a later section ends it. */
        if (!in_game)
            continue;

        eq = strchr(linebuf, '=');
        if (!eq)
            continue;
        *eq = '\0';
        value = eq + 1;
        trim_inplace(linebuf); /* key */
        trim_inplace(value);

        if (key_eq(linebuf, "Title")) {
            set_title(out, value);
        } else if (key_eq(linebuf, "Scripts")) {
            size_t k;
            store(out->scripts, sizeof(out->scripts), value);
            for (k = 0; out->scripts[k]; k++)
                if (out->scripts[k] == '\\')
                    out->scripts[k] = '/';
        } else if (key_eq(linebuf, "Library")) {
            store(out->library, sizeof(out->library), value);
        } else if (value[0] &&
                   (key_eq(linebuf, "RTP") || key_eq(linebuf, "RTP1") ||
                    key_eq(linebuf, "RTP2") || key_eq(linebuf, "RTP3"))) {
            out->rtp_wanted = 1;
        }
    }

    /* Detection order: Scripts first (matches mkxp-z), Library fallback. */
    out->rgss_version = rgss_version_from_scripts(out->scripts);
    if (out->rgss_version == 0)
        out->rgss_version = rgss_version_from_library(out->library);

    return 0;
}

int game_ini_read(const char *path, GameIni *out)
{
    FILE *f;
    char buf[4096];
    size_t n;
    int failed, more = 0;

    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (!path)
        return -1;

    f = fopen(path, "rb");
    if (!f)
        return -1;
    n = fread(buf, 1, sizeof(buf), f);
    failed = ferror(f) || (n < sizeof(buf) && !feof(f));
    if (!failed && n == sizeof(buf)) {
        more = fgetc(f) != EOF;
        failed = ferror(f) || (!more && !feof(f));
    }
    if (fclose(f) != 0)
        failed = 1;
    if (failed || more)
        return -1;

    return game_ini_parse(buf, n, out);
}
