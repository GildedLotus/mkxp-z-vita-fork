// SPDX-License-Identifier: GPL-3.0-or-later
/* Host-produced metadata only; loading never inspects game scripts or assets. */
#ifndef VITA_PREFLIGHT_H
#define VITA_PREFLIGHT_H

#define PREFLIGHT_FILE "vita-preflight.json"
#define PREFLIGHT_MAX_BYTES 2048
#define PREFLIGHT_WARNING_COUNT 11

typedef struct {
    unsigned counts[PREFLIGHT_WARNING_COUNT];
    char input_hash[65], scanner_hash[65], rulebook_hash[65], report_hash[65];
    char engine[6];
    int complete;
} PreflightSummary;

typedef enum {
    PREFLIGHT_NONE, PREFLIGHT_CONTINUE, PREFLIGHT_BACK, PREFLIGHT_REDRAW
} PreflightAction;
typedef enum {
    PREFLIGHT_CONFIRM, PREFLIGHT_CANCEL, PREFLIGHT_PREVIOUS, PREFLIGHT_NEXT
} PreflightButton;

/* 1: valid; 0: absent; -1: unreadable, invalid or future schema. Clears out on failure.
 * Caller supplies a bounded path, and must allow launch regardless of this result. */
int preflight_load(const char *path, PreflightSummary *out);
unsigned preflight_pages(const PreflightSummary *summary);
/* Shared CPU overlay; no GPU objects, allocation or asset decoding. */
void preflight_draw(const PreflightSummary *summary, unsigned page);
/* Call only on fresh button edges, after releasing the initial launch button. */
PreflightAction preflight_step(const PreflightSummary *summary, PreflightButton button,
                              unsigned *page);

#endif
