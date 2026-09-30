// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vita_publish.h — one publication policy for every generation-preserving
 * file writer (fatal report, settings, launcher state, last-error, log
 * rotation).
 *
 * The policy: a writer never destroys the previous complete generation.
 * New bytes land in "<path>.tmp", are flushed, synced and closed through
 * vita_publish_finalize(), and only then published with
 * vita_publish_commit(), which moves the current generation aside to
 * "<path>.bak" first. Readers try the current generation, then the backup,
 * in the same order (vita_publish_read()).
 *
 * Why not plain rename(temp, final): on this libc rename() removes an
 * occupied destination before sceIoRename runs, so an interrupted
 * publish can lose the destination. commit() keeps the rename pair ordered
 * so every failure leaves a complete generation reachable: the previous one
 * at `final` or `backup`, the new one at `tmp`.
 *
 * Pure C99 + POSIX stdio: no allocation on any path, no kernel objects, no
 * Vita headers — the launcher library, the glue and the engine all link it.
 */
#ifndef VITA_PUBLISH_H
#define VITA_PUBLISH_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compose "<path>.tmp" / "<path>.bak"; -1 when it does not fit. */
int vita_publish_tmp_path(char *out, size_t cap, const char *path);
int vita_publish_bak_path(char *out, size_t cap, const char *path);

/* fflush + fsync + fclose, each checked; the stream is closed either way.
 * fsync is skipped once fflush failed. -1 when any step failed. */
int vita_publish_finalize(FILE *f);

/* Publish `tmp` as the new generation of `final`, moving the previous
 * generation aside to `backup` when one exists. On failure every surviving
 * file is left in place; the caller decides whether to retry or remove
 * `tmp`. -1 on failure. */
int vita_publish_commit(const char *tmp, const char *final, const char *backup);

/* vita_publish_commit() for a writer that can judge a generation: the
 * current `final` is rotated into `backup` only when `usable(final)` returns
 * nonzero. An unusable one is moved to `quarantine` (as for vita_publish_move:
 * an occupied quarantine slot is removed first, so a failure can lose it) and
 * `backup` is left alone, so damage can never displace the last good
 * generation. -1 on failure, with every surviving file left in place. */
int vita_publish_commit_checked(const char *tmp, const char *final,
                                const char *backup, const char *quarantine,
                                int (*usable)(const char *path));

/* Plain rename, for moves whose destination is disposable (quarantines,
 * retained-evidence rotations). An occupied `to` is removed
 * first, so a failure can lose it. `from` == `to` is refused. -1 on
 * failure. */
int vita_publish_move(const char *from, const char *to);

/* Move generation `from` onto its successor slot `to`, refusing an occupied
 * destination (EEXIST) instead of replacing it — the sceIoRename semantics
 * generation chains rely on, where a failed shift must not cost the log the
 * rotation meant to keep. -1 on failure. */
int vita_publish_shift(const char *from, const char *to);

/* Read-side half of the policy, in the writer's order: `try_read` runs on
 * the current generation, then on the backup. `try_read` returns 1 when the
 * generation yielded a usable value, 0 when it is absent or unusable, and
 * -1 on an I/O error, which stops the fallback (an I/O error is not
 * evidence the backup is better). Returns the last call's result. */
int vita_publish_read(const char *path, const char *backup,
                      int (*try_read)(const char *path, void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif

#endif
