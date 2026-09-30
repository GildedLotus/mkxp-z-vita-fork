// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * game_scan.h — directory scan for the Vita launcher.
 *
 * Reads ux0:/data/mkxp-z/games/ and turns it into the list the launcher
 * draws; the folder layout is described in vita/docs/launcher.md.
 *
 * Constraints that shaped the API:
 *
 *  - No allocation. The caller owns the GameEntry array; 256 entries is about
 *    160 KB, so it belongs in static storage, never on a stack (the Vita's
 *    RGSS thread gets 8 MiB but the launcher's main thread does not).
 *  - GAME_SCAN_PATH_MAX is the LoadExec argument budget, not a guess: a path
 *    that does not fit here cannot be handed to the game process, so it is
 *    counted and skipped rather than silently cut.
 *  - VitaSDK's struct dirent carries the type the firmware already read while
 *    listing the directory, so an entry is stat'ed only when the listing did
 *    not classify it (a cold stat is ~9 ms).
 *  - No / or % by a non-constant anywhere: Cortex-A9 has no hardware integer
 *    divide, and the scan runs on a rescan keypress.
 *
 * C99; libc + <dirent.h> + <sys/stat.h> only. No SDL, no Vita headers, no
 * iconv, no globals.
 */
#ifndef VITA_GAME_SCAN_H
#define VITA_GAME_SCAN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAME_SCAN_MAX_ENTRIES 256
#define GAME_SCAN_PATH_MAX    256 /* bytes incl. NUL = LoadExec arg budget */
#define GAME_SCAN_FOLDER_MAX  128
#define GAME_SCAN_TITLE_MAX   256
#define GAME_SCAN_EXEC_MAX    128 /* execName basename budget, see below */

enum {
    GAME_ENTRY_TITLE_FROM_FOLDER = 1,  /* Game.ini had no Title= */
    GAME_ENTRY_TITLE_WAS_SJIS    = 2,  /* Title= was CP932, decoded here */
    GAME_ENTRY_HAS_ARCHIVE       = 4,  /* Game.rgssad / .rgss2a / .rgss3a */
    GAME_ENTRY_NESTED            = 8,  /* Game.ini one level down */
    GAME_ENTRY_WANTS_RTP         = 16, /* Game.ini names an RTP */
    GAME_ENTRY_HAS_MKXP_JSON     = 32, /* game ships its own mkxp.json */
    GAME_ENTRY_HAS_EXECNAME      = 64  /* launch must pass --execName (the
                                         * executable is not "Game"); the
                                         * name is in exec_name */
};

typedef struct GameEntry {
    char path[GAME_SCAN_PATH_MAX];  /* absolute, no trailing slash */
    char folder[GAME_SCAN_FOLDER_MAX]; /* last component of path */
    char title[GAME_SCAN_TITLE_MAX];   /* UTF-8, never empty */
    char exec_name[GAME_SCAN_EXEC_MAX]; /* basename without extension; empty
                                         * unless HAS_EXECNAME is set */
    int rgss_version;               /* 1/2/3, or 0 when nothing said */
    int flags;                      /* GAME_ENTRY_* */
} GameEntry;

typedef struct GameScanStats {
    int found;            /* == the return value of game_scan */
    int skipped_no_ini;   /* directory with no Game.ini and no usable child */
    int skipped_too_long; /* path would not fit GAME_SCAN_PATH_MAX (or a matched <name>.ini whose basename
                           * does not fit GAME_SCAN_EXEC_MAX) */
    int truncated;        /* 1 if at least one game did not fit `cap` */
    int root_missing;     /* 1 if `root` could not be opened at all */
    int incomplete;       /* read/enumeration/close error; no entries published */
    int fallback_stats;   /* entries whose type the listing did not supply, so
                           * the scan had to stat() them: 0 on a card where
                           * every entry carries a type */
    int skipped_bad_ini;  /* games dropped because their Game.ini is unreadable
                           * or over the 4 KB read cap; the scan still completes
                           * and the other games still list */
    char last_bad_ini_folder[GAME_SCAN_FOLDER_MAX]; /* most recently skipped
                           * such folder, UTF-8 sanitised, for the log line */
    int skipped_bad_name; /* folders whose path carries a control byte, which
                           * launch_path_is_valid would refuse */
    int skipped_ambiguous_ini; /* folders where several .ini files have a
                           * matching partner, so no single execName is the
                           * game */
    char last_ambiguous_folder[GAME_SCAN_FOLDER_MAX]; /* most recently skipped
                           * such folder, UTF-8 sanitised, for the log line */
} GameScanStats;

/*
 * Scan `root` for games and fill up to `cap` entries, sorted by title
 * (bytewise, ASCII case-insensitive) with ties broken by folder name.
 *
 * A directory under `root` is a game when it contains Game.ini (game.ini is
 * also tried, for case-sensitive host filesystems). Otherwise, if exactly one
 * of its sub-directories contains one, that sub-directory is the game and
 * GAME_ENTRY_NESTED is set — this is the documented user mistake of copying
 * the folder that *contains* the game folder.
 *
 * A folder with no Game.ini can still be a game that names itself: Pocket Mirror Classic ships "Pocket Mirror.ini/.exe/.rgss3a".
 * When the listing holds exactly one "<name>.ini" that at least one other
 * entry names — a basename match ending .exe/.rgssad/.rgss2a/.rgss3a (ASCII
 * case-insensitive) — the folder is listed with GAME_ENTRY_HAS_EXECNAME and
 * exec_name = "<name>"; the launch passes it as --execName and the engine
 * reads <name>.ini instead of Game.ini. A stray .ini with no partner
 * (desktop.ini, a settings file the game writes) is ignored;
 * the folder is ambiguous only when several .ini files have partners: it is
 * skipped and counted in skipped_ambiguous_ini under last_ambiguous_folder.
 * The rule is decided from the listing alone (two readdir passes), never
 * from a per-entry stat.
 *
 * Dot entries and non-directories are skipped. An empty Title= falls back to
 * the folder name. Titles and folder names are UTF-8 sanitised, so nothing
 * that reaches the renderer is malformed.
 *
 * `stats` may be NULL. Returns the number of entries written, or 0 with
 * stats->root_missing set when `root` cannot be opened.
 * A game whose Game.ini is unreadable or over the 4 KB read cap is dropped on
 * its own: it is counted in stats->skipped_bad_ini, its folder is left in
 * stats->last_bad_ini_folder for the log, and the other games still list.
 * Enumeration and probe I/O failures keep the conservative contract — return
 * 0 with stats->incomplete set and entries already scanned discarded. Only a
 * completed scan can prove a unique nested game; a partial search never
 * selects one.
 */
int game_scan(const char *root, GameEntry *out, int cap, GameScanStats *stats);

/*
 * Bytewise ASCII-case-insensitive compare, ties broken by folder. Exposed so
 * the front end can re-sort or binary-search the same way game_scan sorted.
 */
int game_entry_compare(const GameEntry *a, const GameEntry *b);

#ifdef __cplusplus
}
#endif

#endif /* VITA_GAME_SCAN_H */
