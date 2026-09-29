// SPDX-License-Identifier: GPL-3.0-or-later
#include "preflight.h"
#include "overlay.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char *const warning_keys[PREFLIGHT_WARNING_COUNT] = {
    "native", "threads", "process", "save", "resize", "midi", "audio", "tileset",
    "image", "dynamic", "incomplete"
};
static const char *const warning_text[PREFLIGHT_WARNING_COUNT] = {
    "Windows/native features may be unavailable",
    "Thread use needs game testing",
    "Shell or network features may be unavailable",
    "Save recovery not verified",
    "Screen resize needs game testing",
    "MIDI music may be silent",
    "Some audio formats may be silent",
    "Tall XP tilesets need the atlas path",
    "Large images may exceed memory or texture limits",
    "Dynamic code or dependencies need review",
    "Scan incomplete; other problems may exist"
};

typedef struct { const char *at, *end; } Reader;

static void space(Reader *r)
{
    while (r->at < r->end && (*r->at == ' ' || *r->at == '\n' ||
           *r->at == '\r' || *r->at == '\t')) ++r->at;
}

static int punct(Reader *r, char value)
{
    space(r);
    if (r->at == r->end || *r->at != value) return 0;
    ++r->at;
    return 1;
}

static int word(Reader *r, char *out, size_t cap)
{
    size_t n = 0;
    if (!punct(r, '"')) return 0;
    while (r->at < r->end && *r->at != '"') {
        unsigned char c = (unsigned char)*r->at++;
        /* The schema uses only literal ASCII keys, enums and hexadecimal hashes. */
        if (c < 32 || c > 126 || c == '\\' || n + 1 >= cap) return 0;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return punct(r, '"');
}

static int number(Reader *r, unsigned *out)
{
    unsigned value = 0;
    const char *start;
    space(r);
    start = r->at;
    while (r->at < r->end && *r->at >= '0' && *r->at <= '9') {
        if (r->at > start && *start == '0') return 0;
        value = value * 10 + (unsigned)(*r->at++ - '0');
        if (value > 20000) return 0;
    }
    if (start == r->at) return 0;
    *out = value;
    return 1;
}

static int truth(Reader *r, int *out)
{
    space(r);
    if (r->end - r->at >= 4 && !memcmp(r->at, "true", 4)) {
        *out = 1; r->at += 4; return 1;
    }
    if (r->end - r->at >= 5 && !memcmp(r->at, "false", 5)) {
        *out = 0; r->at += 5; return 1;
    }
    return 0;
}

static int hash(Reader *r, char out[65])
{
    size_t i;
    if (!word(r, out, 65) || strlen(out) != 64) return 0;
    for (i = 0; i < 64; ++i)
        if (!((out[i] >= '0' && out[i] <= '9') ||
              (out[i] >= 'a' && out[i] <= 'f'))) return 0;
    return 1;
}

static int warnings(Reader *r, PreflightSummary *out)
{
    unsigned seen = 0, i;
    char key[24];
    if (!punct(r, '{')) return 0;
    if (punct(r, '}')) return 1;
    do {
        if (!word(r, key, sizeof(key)) || !punct(r, ':')) return 0;
        for (i = 0; i < PREFLIGHT_WARNING_COUNT; ++i)
            if (!strcmp(key, warning_keys[i])) break;
        if (i == PREFLIGHT_WARNING_COUNT || (seen & (1u << i)) ||
            !number(r, &out->counts[i]) || !out->counts[i]) return 0;
        seen |= 1u << i;
        if (punct(r, '}')) return 1;
    } while (punct(r, ','));
    return 0;
}

static int parse(Reader *r, PreflightSummary *out)
{
    static const char *const keys[] = {"schema_version", "engine", "assessment",
        "input_sha256", "scanner_sha256", "rulebook_sha256", "report_sha256",
        "complete", "warnings"};
    unsigned seen = 0, i, version;
    char key[32], value[24];
    if (!punct(r, '{')) return 0;
    do {
        if (!word(r, key, sizeof(key)) || !punct(r, ':')) return 0;
        for (i = 0; i < 9; ++i) if (!strcmp(key, keys[i])) break;
        if (i == 9 || (seen & (1u << i))) return 0;
        seen |= 1u << i;
        switch (i) {
        case 0: if (!number(r, &version) || version != 1) return 0; break;
        case 1:
            if (!word(r, out->engine, sizeof(out->engine)) ||
                (strcmp(out->engine, "XP") && strcmp(out->engine, "VX") &&
                 strcmp(out->engine, "VXAce"))) return 0;
            break;
        case 2:
            if (!word(r, value, sizeof(value)) || strcmp(value, "not_tested")) return 0;
            break;
        case 3: if (!hash(r, out->input_hash)) return 0; break;
        case 4: if (!hash(r, out->scanner_hash)) return 0; break;
        case 5: if (!hash(r, out->rulebook_hash)) return 0; break;
        case 6: if (!hash(r, out->report_hash)) return 0; break;
        case 7: if (!truth(r, &out->complete)) return 0; break;
        case 8: if (!warnings(r, out)) return 0; break;
        }
        if (punct(r, '}')) {
            space(r);
            return seen == 511 && r->at == r->end &&
                (out->complete ? !out->counts[10] : out->counts[10] != 0);
        }
    } while (punct(r, ','));
    return 0;
}

int preflight_load(const char *path, PreflightSummary *out)
{
    char data[PREFLIGHT_MAX_BYTES + 1];
    PreflightSummary candidate;
    Reader reader;
    size_t n;
    int bad;
    FILE *file;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!path || strlen(path) >= 1024) return -1;
    file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? 0 : -1;
    n = fread(data, 1, sizeof(data), file);
    bad = ferror(file);
    if (fclose(file)) bad = 1;
    if (bad || n > PREFLIGHT_MAX_BYTES) return -1;
    memset(&candidate, 0, sizeof(candidate));
    reader.at = data; reader.end = data + n;
    if (!parse(&reader, &candidate)) return -1;
    *out = candidate;
    return 1;
}

unsigned preflight_pages(const PreflightSummary *summary)
{
    return summary ? 2 : 1; /* Player warnings, then optional technical details. */
}

/* Compact CPU diagnostic view; the launcher presents its own TrueType view. */
void preflight_draw(const PreflightSummary *summary, unsigned page)
{
    char line[65];
    unsigned i, at = 0, row = 0, pages = preflight_pages(summary);
    overlay_clear();
    overlay_box(0, 0, OVERLAY_W, OVERLAY_H, OVERLAY_BACKDROP);
    overlay_text(0, 0, "Game preflight - not tested on this build");
    if (!summary) {
        overlay_text(0, 24, "No readable host preflight summary");
    } else {
        page %= pages;
        snprintf(line, sizeof(line), "%s  Host scan %s  %s", summary->engine,
                 summary->complete ? "complete" : "incomplete", page ? "Details" : "Warnings");
        overlay_text(0, 8, line);
        for (i = 0; i < PREFLIGHT_WARNING_COUNT; ++i) {
            if (!summary->counts[i]) continue;
            ++at;
            if (!page) overlay_text(0, 16 + (int)row++ * 8, warning_text[i]);
            else {
                snprintf(line, sizeof(line), "%s: %u", warning_keys[i],
                         summary->counts[i]);
                overlay_text((int)(row % 2) * 256, 16 + (int)(row / 2) * 8, line);
                ++row;
            }
        }
        if (!at) overlay_text(0, 24, "No listed warnings; gameplay still needs testing");
        if (page) {
            snprintf(line, sizeof(line), "Report %.12s | Inputs %.12s", summary->report_hash, summary->input_hash);
            overlay_text(0, 88, line);
            snprintf(line, sizeof(line), "Scanner %.12s | Rules %.12s", summary->scanner_hash, summary->rulebook_hash);
            overlay_text(0, 96, line);
        }
    }
    overlay_text(0, 112, "Cross/Circle: Continue  Square: Back");
    overlay_text(0, 120, "Left/Right: Details | Hashes identify the scan");
}

PreflightAction preflight_step(const PreflightSummary *summary, PreflightButton button,
                              unsigned *page)
{
    unsigned pages = preflight_pages(summary);
    if (button == PREFLIGHT_CONFIRM) return PREFLIGHT_CONTINUE;
    if (button == PREFLIGHT_CANCEL) return PREFLIGHT_BACK;
    if (!page || pages == 1) return PREFLIGHT_NONE;
    if (button == PREFLIGHT_NEXT) *page = (*page % pages + 1) % pages;
    else if (button == PREFLIGHT_PREVIOUS) *page = (*page % pages + pages - 1) % pages;
    else return PREFLIGHT_NONE;
    return PREFLIGHT_REDRAW;
}
