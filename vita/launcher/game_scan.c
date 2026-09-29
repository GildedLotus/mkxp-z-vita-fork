// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * game_scan.c — see game_scan.h. Native entry types;
 * self-named (execless) games
 */
#include "game_scan.h"
#include "game_ini.h"
#include "utf8_util.h"

#include <dirent.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/*
 * A game path must fit GAME_SCAN_PATH_MAX because it is a LoadExec argument.
 * The probe paths built from it ("<dir>/Game.ini", "<dir>/Game.rgss3a") are
 * only ever opened locally, so they get their own slightly larger buffer and
 * never shrink what the user can launch.
 */
#define PROBE_PATH_MAX (GAME_SCAN_PATH_MAX + 32)

/* How many .ini names the execless analysis holds. A folder with more is
 * declined as ambiguous, never launched; eight covers any real layout many
 * times over (the game's own .ini plus strays such as desktop.ini). */
#define GAME_SCAN_INI_SLOTS 8

/* ---- small helpers ------------------------------------------------------ */

static char lower_ascii(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

/* Bytewise compare with ASCII case folding. Locale-free and byte-stable, so
 * UTF-8 sorts by code point and two runs over the same card agree. */
static int casecmp_ascii(const char *a, const char *b)
{
    size_t i;

    for (i = 0;; i++) {
        unsigned char ca = (unsigned char)lower_ascii(a[i]);
        unsigned char cb = (unsigned char)lower_ascii(b[i]);

        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (ca == '\0')
            return 0;
    }
}

/*
 * dst = dir + "/" + name, with no doubled or trailing slash.
 * Returns 0 on success, -1 if it would not fit.
 */
static int path_join(char *dst, size_t cap, const char *dir, const char *name)
{
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);

    while (dlen > 0 && dir[dlen - 1] == '/')
        dlen--;
    if (dlen + 1 + nlen + 1 > cap)
        return -1;
    memcpy(dst, dir, dlen);
    dst[dlen] = '/';
    memcpy(dst + dlen + 1, name, nlen);
    dst[dlen + 1 + nlen] = '\0';
    return 0;
}

static int is_dir(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return -1;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

/*
 * The entry type the directory listing already knows: 1 directory, 0 not a
 * directory, -1 nothing (stat it).
 *
 * VitaSDK's readdir() carries the firmware's own SceIoStat in struct dirent,
 * so a Vita scan costs no syscall per entry;
 * a hardware run measured ~9 ms for the first stat of an entry, against ~0.5 ms for the
 * repeat, and the lookup cache is too small for a whole card. Those mode bits
 * are SCE's, not POSIX's (SCE_S_IFDIR is 0010000, _IFDIR is 0040000), so this
 * uses SCE_S_ISDIR/SCE_S_ISREG — which <dirent.h> provides on this SDK —
 * and never S_ISDIR. Anything a listing did not classify (a symlink, an entry
 * firmware left untyped) still goes to stat(), so the answer is never a
 * guess.
 *
 * GAME_SCAN_LISTING_TYPE is the host-build seam: the host libc has no
 * d_stat, so a host build plants the type instead. Without it
 * the host answer is -1 and every entry takes the fallback.
 */
static int listing_type(const struct dirent *e)
{
#if defined(__vita__)
    unsigned int mode = (unsigned int)e->d_stat.st_mode;

    if (SCE_S_ISDIR(mode))
        return 1;
    if (SCE_S_ISREG(mode))
        return 0;
    return -1;
#elif defined(GAME_SCAN_LISTING_TYPE)
    return GAME_SCAN_LISTING_TYPE(e);
#else
    (void)e;
    return -1;
#endif
}

/*
 * Is the listing entry a directory? 1 yes, 0 no, -1 stat() failed.
 * `fallback_stats` counts the entries the listing did not classify: it stays
 * zero on a card whose entries all carry a type.
 */
static int entry_is_dir(const struct dirent *e, const char *path,
                        int *fallback_stats)
{
    int type = listing_type(e);

    if (type >= 0)
        return type;
    (*fallback_stats)++;
    return is_dir(path);
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f)
        return errno == ENOENT ? 0 : -1;
    return fclose(f) == 0 ? 1 : -1;
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

static struct dirent *next_entry(DIR *d, int *failed)
{
    struct dirent *e;

    /* Probes inside the loop may leave ENOENT behind. */
    errno = 0;
    e = readdir(d);
    if (!e && errno != 0)
        *failed = 1;
    return e;
}

/*
 * Does `dir` hold a Game.ini? Writes the winning path into `ini` (cap
 * PROBE_PATH_MAX). "game.ini" is tried too: ux0: is case-insensitive but a
 * host filesystem may not be.
 *
 * A Game.ini the process cannot open (EACCES) counts as present: the unreadable
 * file is that one game's problem, and game_ini_read fails the same way, so
 * fill_entry classifies the entry instead of the whole scan turning
 * incomplete. Every other probe error is still an I/O failure (-1).
 */
static int find_game_ini(const char *dir, char *ini)
{
    static const char *const names[] = {"Game.ini", "game.ini"};
    size_t k;

    for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        int found;

        if (path_join(ini, PROBE_PATH_MAX, dir, names[k]) != 0)
            continue;
        found = file_exists(ini);
        if (found != 0) {
            if (found < 0 && errno == EACCES)
                return 1;
            return found;
        }
    }
    ini[0] = '\0';
    return 0;
}

/* Archive next to Game.ini -> RGSS version, 0 when there is none. */
static int archive_version(const char *dir)
{
    static const struct {
        const char *name;
        int version;
    } archives[] = {
        {"Game.rgssad", 1}, {"game.rgssad", 1},
        {"Game.rgss2a", 2}, {"game.rgss2a", 2},
        {"Game.rgss3a", 3}, {"game.rgss3a", 3},
    };
    char probe[PROBE_PATH_MAX];
    size_t k;

    for (k = 0; k < sizeof(archives) / sizeof(archives[0]); k++) {
        int found;

        if (path_join(probe, sizeof(probe), dir, archives[k].name) != 0)
            continue;
        found = file_exists(probe);
        if (found != 0)
            return found < 0 ? -1 : archives[k].version;
    }
    return 0;
}

static int has_mkxp_json(const char *dir)
{
    char probe[PROBE_PATH_MAX];

    if (path_join(probe, sizeof(probe), dir, "mkxp.json") != 0)
        return 0;
    return file_exists(probe);
}

/* ASCII case-insensitive suffix test; `suffix` is lowercase. */
static int suffix_is(const char *name, const char *suffix)
{
    size_t n = strlen(name), s = strlen(suffix), i;

    if (n < s)
        return 0;
    for (i = 0; i < s; i++)
        if (lower_ascii(name[n - s + i]) != (unsigned char)suffix[i])
            return 0;
    return 1;
}

/* The partner extensions a self-named game may pair with its <name>.ini,
 * and the RGSS version an archive suffix carries ("exe" pairs without one). */
static const char *partner_ext(const char *name, int *version)
{
    static const struct {
        const char *ext;
        int version;
    } known[] = {
        {".rgss3a", 3}, {".rgss2a", 2}, {".rgssad", 1}, {".exe", 0},
    };
    size_t k;

    for (k = 0; k < sizeof(known) / sizeof(known[0]); k++) {
        if (suffix_is(name, known[k].ext)) {
            *version = known[k].version;
            return known[k].ext;
        }
    }
    return NULL;
}

/*
 * Execless detection: a folder with no Game.ini may name
 * itself, as Pocket Mirror Classic does with "Pocket Mirror.ini/.exe/.rgss3a".
 * The folder is a game when exactly one "<name>.ini" in the listing has a
 * partner whose basename matches it (ASCII case-insensitive) and whose
 * extension is .rgssad/.rgss2a/.rgss3a (version) or .exe. A stray .ini with
 * no partner — desktop.ini, a settings file the game writes — cannot hide
 * the game's own; the folder is ambiguous only when several
 * .ini files have partners, or when more .ini files exist than
 * GAME_SCAN_INI_SLOTS can hold. Everything is decided from the listing alone:
 * two readdir passes, never a per-entry stat.
 *
 * On success writes the winning <name>.ini probe path into `ini` (cap
 * PROBE_PATH_MAX), the exact basename bytes into `exec` (cap
 * GAME_SCAN_EXEC_MAX) and the archive version (0 when the partner was the
 * .exe) into `*version`. Returns 1 on success, 0 when the folder is not a
 * self-named game (no .ini with a partner counts as no game here), 2 when
 * the folder was counted here as ambiguous — the caller must not count it
 * again — and -1 on an I/O failure. A basename that does not fit
 * GAME_SCAN_EXEC_MAX, or one carrying a control byte (it becomes a
 * process-crossing argv string), sets *too_long and returns 0.
 */
static int find_execless_game(const char *dir, char *ini, char *exec,
                              int *version, int *too_long, GameScanStats *stats)
{
    DIR *d;
    struct dirent *e;
    char inis[GAME_SCAN_INI_SLOTS][PROBE_PATH_MAX];
    unsigned char has_partner[GAME_SCAN_INI_SLOTS];
    int archive_of[GAME_SCAN_INI_SLOTS];
    size_t base_len;
    int ini_count = 0;
    int overflow = 0;
    int failed = 0;
    int matches = 0;
    int winner = -1;
    int k;

    *version = 0;
    memset(has_partner, 0, sizeof(has_partner));
    memset(archive_of, 0, sizeof(archive_of));

    /* Pass one: the .ini names. A dot entry is never one. */
    d = opendir(dir);
    if (!d)
        return -1;
    while ((e = next_entry(d, &failed)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (!suffix_is(e->d_name, ".ini"))
            continue;
        if (ini_count == GAME_SCAN_INI_SLOTS) {
            overflow = 1;
            break;
        }
        memcpy(inis[ini_count], e->d_name, strlen(e->d_name) + 1);
        ini_count++;
    }
    /* A listing that could not be finished never decides anything, not even
     * a rejection: the ambiguity is only ever reported from a full pass. */
    if (failed || closedir(d) != 0)
        return -1;

    if (ini_count == 0)
        return 0;

    /* Pass two, a fresh listing of the same folder: mark every recorded .ini
     * that a partner names. An archive outweighs the exe, whichever comes
     * first in the listing. */
    d = opendir(dir);
    if (!d)
        return -1;
    while ((e = next_entry(d, &failed)) != NULL) {
        const char *ext;
        int ver = 0;
        size_t n, i;

        if (e->d_name[0] == '.')
            continue;
        ext = partner_ext(e->d_name, &ver);
        if (!ext)
            continue;
        n = strlen(e->d_name) - strlen(ext);
        for (k = 0; k < ini_count; k++) {
            /* minus ".ini"; never 0: ".ini" is a dot entry and pass one
             * skips it */
            size_t ini_base = strlen(inis[k]) - 4;
            int same = n == ini_base;

            for (i = 0; same && i < n; i++) {
                if (lower_ascii(inis[k][i]) != lower_ascii(e->d_name[i]))
                    same = 0;
            }
            if (!same)
                continue;
            has_partner[k] = 1;
            if (ver > 0 && archive_of[k] == 0)
                archive_of[k] = ver;
        }
    }
    if (closedir(d) != 0)
        failed = 1;
    if (failed)
        return -1;

    for (k = 0; k < ini_count; k++) {
        if (!has_partner[k])
            continue;
        matches++;
        winner = k;
    }

    if (overflow || matches > 1) {
        const char *folder = basename_of(dir);

        stats->skipped_ambiguous_ini++;
        utf8_sanitize(folder, strlen(folder), stats->last_ambiguous_folder,
                      sizeof(stats->last_ambiguous_folder));
        return 2;
    }
    if (matches == 0)
        return 0;

    base_len = strlen(inis[winner]) - 4;
    *version = archive_of[winner];

    {
        size_t i;

        for (i = 0; i < base_len; i++) {
            unsigned char c = (unsigned char)inis[winner][i];

            if (c < 0x20 || c == 0x7F) {
                *too_long = 1; /* not offered as an argv string at all */
                return 0;
            }
        }
        if (base_len + 1 > GAME_SCAN_EXEC_MAX) {
            *too_long = 1;
            return 0;
        }
    }

    if (path_join(ini, PROBE_PATH_MAX, dir, inis[winner]) != 0) {
        *too_long = 1;
        return 0;
    }
    memcpy(exec, inis[winner], base_len);
    exec[base_len] = '\0';
    return 1;
}

/* ---- nested lookup ------------------------------------------------------ */

/*
 * The user copied the folder that CONTAINS the game folder. Accept it only
 * when the answer is unambiguous: exactly one sub-directory holds a Game.ini.
 * Two or more is a collection, not a game, and is reported as skipped.
 *
 * Returns 1 and writes the child's path into `child` (cap GAME_SCAN_PATH_MAX),
 * 0 when absent/ambiguous, -1 on I/O failure. `too_long` is set when a child
 * path did not fit; that also prevents a unique-child result.
 * `fallback_stats` counts the entries that needed a stat() (see entry_is_dir).
 */
static int find_nested_game(const char *dir, char *child, int *too_long,
                            int *fallback_stats)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char candidate[GAME_SCAN_PATH_MAX];
    char ini[PROBE_PATH_MAX];
    int hits = 0;
    int failed = 0;

    child[0] = '\0';
    if (!d)
        return -1;

    while ((e = next_entry(d, &failed)) != NULL) {
        int found;

        if (e->d_name[0] == '.')
            continue;
        if (path_join(candidate, sizeof(candidate), dir, e->d_name) != 0) {
            *too_long = 1;
            continue;
        }
        found = entry_is_dir(e, candidate, fallback_stats);
        if (found < 0) {
            failed = 1;
            break;
        }
        if (!found)
            continue;
        found = find_game_ini(candidate, ini);
        if (found < 0) {
            failed = 1;
            break;
        }
        if (!found)
            continue;
        hits++;
        if (hits > 1)
            break;
        memcpy(child, candidate, strlen(candidate) + 1);
    }
    if (closedir(d) != 0)
        failed = 1;

    if (failed || hits != 1 || *too_long) {
        child[0] = '\0';
        return failed ? -1 : 0;
    }
    return 1;
}

/* ---- entry construction ------------------------------------------------- */

/*
 * 0 on success, 1 when this game's own ini cannot be read (unreadable or over
 * the 4 KB cap — the entry is dropped, the scan goes on), -1 on an I/O failure
 * while probing the game directory (the scan stays conservative).
 *
 * `exec_name` is NULL for a Game.ini game; for a self-named one
 * it is the exact basename the listing paired with its <name>.ini, and
 * `listed_archive` is the version already derived from that listing (0 when
 * the partner was the .exe), so the Game.* archive probes are skipped.
 */
static int fill_entry(GameEntry *ent, const char *game_dir, const char *ini,
                       const char *exec_name, int listed_archive, int nested)
{
    GameIni g;
    const char *folder;
    int archive, json;

    if (game_ini_read(ini, &g) != 0)
        return 1;
    if (exec_name) {
        archive = listed_archive;
        json = has_mkxp_json(game_dir);
    } else {
        archive = archive_version(game_dir);
        json = has_mkxp_json(game_dir);
    }
    if (archive < 0 || json < 0)
        return -1;

    memset(ent, 0, sizeof(*ent));
    memcpy(ent->path, game_dir, strlen(game_dir) + 1);
    /* readdir gave us these bytes; nothing downstream may assume they are
     * well-formed UTF-8 until they have been through here. */
    folder = basename_of(game_dir);
    utf8_sanitize(folder, strlen(folder), ent->folder, sizeof(ent->folder));

    if (g.title[0]) {
        utf8_sanitize(g.title, strlen(g.title), ent->title, sizeof(ent->title));
        if (g.title_was_sjis)
            ent->flags |= GAME_ENTRY_TITLE_WAS_SJIS;
    }
    if (ent->title[0] == '\0') {
        /* Folder name is the fallback; it is already sanitised. */
        memcpy(ent->title, ent->folder, strlen(ent->folder) + 1);
        ent->flags |= GAME_ENTRY_TITLE_FROM_FOLDER;
    }

    ent->rgss_version = g.rgss_version;
    if (g.rtp_wanted)
        ent->flags |= GAME_ENTRY_WANTS_RTP;
    if (nested)
        ent->flags |= GAME_ENTRY_NESTED;
    if (json)
        ent->flags |= GAME_ENTRY_HAS_MKXP_JSON;
    if (exec_name) {
        /* Exact bytes, never sanitised or truncated: the launch hands them to
         * the player as --execName and the engine opens <exec_name>.ini. */
        memcpy(ent->exec_name, exec_name, strlen(exec_name) + 1);
        ent->flags |= GAME_ENTRY_HAS_EXECNAME;
    }

    if (archive) {
        ent->flags |= GAME_ENTRY_HAS_ARCHIVE;
        if (ent->rgss_version == 0)
            ent->rgss_version = archive;
    }
    return 0;
}

int game_entry_compare(const GameEntry *a, const GameEntry *b)
{
    int c = casecmp_ascii(a->title, b->title);

    if (c != 0)
        return c;
    return casecmp_ascii(a->folder, b->folder);
}

static int compare_thunk(const void *a, const void *b)
{
    return game_entry_compare((const GameEntry *)a, (const GameEntry *)b);
}

/* ---- the scan ----------------------------------------------------------- */

int game_scan(const char *root, GameEntry *out, int cap, GameScanStats *stats)
{
    GameScanStats local;
    DIR *d;
    struct dirent *e;
    char dir[GAME_SCAN_PATH_MAX];
    char child[GAME_SCAN_PATH_MAX];
    char ini[PROBE_PATH_MAX];
    int count = 0;

    memset(&local, 0, sizeof(local));
    if (!stats)
        stats = &local;
    memset(stats, 0, sizeof(*stats));

    /* A caller bug, not a card state: no flag, just nothing found. */
    if (!out || cap <= 0)
        return 0;

    if (!root) {
        stats->root_missing = 1;
        return 0;
    }

    d = opendir(root);
    if (!d) {
        stats->root_missing = 1;
        stats->incomplete = errno != ENOENT;
        return 0;
    }

    while ((e = next_entry(d, &stats->incomplete)) != NULL) {
        int nested = 0;
        int found;
        int filled;
        const char *game_dir;
        const char *folder;
        const char *exec_name = NULL;
        char exec[GAME_SCAN_EXEC_MAX];
        int listed_archive = 0;

        if (e->d_name[0] == '.') /* ".", "..", and hidden entries */
            continue;
        if (path_join(dir, sizeof(dir), root, e->d_name) != 0) {
            stats->skipped_too_long++;
            continue;
        }
        /* The listing already knows the type of most entries; only one it
         * did not classify costs a stat() (see entry_is_dir). */
        found = entry_is_dir(e, dir, &stats->fallback_stats);
        if (found < 0) {
            stats->incomplete = 1;
            break;
        }
        if (!found)
            continue;

        game_dir = dir;
        found = find_game_ini(dir, ini);
        if (found < 0) {
            stats->incomplete = 1;
            break;
        }
        if (!found) {
            int too_long = 0;

            found = find_nested_game(dir, child, &too_long,
                                     &stats->fallback_stats);
            if (found < 0) {
                stats->incomplete = 1;
                break;
            }
            if (found) {
                if (find_game_ini(child, ini) != 1) {
                    stats->incomplete = 1;
                    break;
                }
                game_dir = child;
                nested = 1;
            } else {
                /* No Game.ini anywhere: the folder may still name its own
                 * game, as Pocket Mirror Classic does. */
                found = find_execless_game(dir, ini, exec, &listed_archive,
                                           &too_long, stats);
                if (found < 0) {
                    stats->incomplete = 1;
                    break;
                }
                if (found == 1) {
                    exec_name = exec;
                } else {
                    if (too_long)
                        stats->skipped_too_long++;
                    else if (found == 0)
                        stats->skipped_no_ini++;
                    /* found == 2: already counted as ambiguous */
                    continue;
                }
            }
        }

        if (count >= cap) {
            stats->truncated = 1;
            continue; /* keep counting what we skipped, store nothing */
        }
        filled = fill_entry(&out[count], game_dir, ini, exec_name,
                            listed_archive, nested);
        if (filled != 0) {
            if (filled < 0) {
                stats->incomplete = 1;
                break;
            }
            /* One game's Game.ini is unreadable or over the read cap: drop
             * that game, keep the rest, and keep the folder for the log. */
            folder = basename_of(game_dir);
            stats->skipped_bad_ini++;
            utf8_sanitize(folder, strlen(folder), stats->last_bad_ini_folder,
                          sizeof(stats->last_bad_ini_folder));
            continue;
        }
        count++;
    }
    if (closedir(d) != 0)
        stats->incomplete = 1;
    if (stats->incomplete) {
        memset(out, 0, (size_t)count * sizeof(out[0]));
        return 0;
    }

    if (count > 1)
        qsort(out, (size_t)count, sizeof(out[0]), compare_thunk);

    stats->found = count;
    return count;
}
