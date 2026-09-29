// SPDX-License-Identifier: GPL-3.0-or-later
/* vita_posix_compat.h — force-included for mkxp-z on arm-vita-eabi.
 *
 * Vita newlib lacks a handful of POSIX entry points that ghc/filesystem.hpp
 * and cpp-httplib reference unconditionally on the non-Windows path:
 *   symlink, readlink
 * plus the AT_* constants used with utimensat (which newlib DOES declare
 * in sys/stat.h) and a visible strerror_r (feature-test gated) and usleep
 * (hidden by _POSIX_C_SOURCE=200809).
 *
 * Stubs fail with ENOSYS/EINVAL rather than crashing. mkxp-z's
 * filesystemImpl only needs exists/is_directory/current_path/equivalent,
 * none of which go through the missing calls on the code paths we hit.
 *
 * Force-included with -include by vita/scripts/configure-vita.sh.
 * Keep this header self-contained: no project includes.
 */
#ifndef VITA_POSIX_COMPAT_H
#define VITA_POSIX_COMPAT_H

/* Expose POSIX 2008 so newlib's XPG strerror_r is declared (ghc uses it). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ghc/filesystem.hpp last_write_time() path calls
 *   utimensat(AT_FDCWD, ..., AT_SYMLINK_NOFOLLOW)
 * newlib declares utimensat in sys/stat.h but not these flags. */
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

/* No symlinks on Vita's FAT/exFAT volumes (ux0:/app0:). */
static inline int vita_compat_symlink(const char *target, const char *linkpath)
{
	(void)target; (void)linkpath;
	errno = ENOSYS;
	return -1;
}
static inline ssize_t vita_compat_readlink(const char *path, char *buf, size_t bufsiz)
{
	(void)path; (void)buf; (void)bufsiz;
	errno = EINVAL;
	return -1;
}

#ifndef symlink
#define symlink vita_compat_symlink
#endif
#ifndef readlink
#define readlink vita_compat_readlink
#endif

/* usleep lives in newlib but is hidden by _POSIX_C_SOURCE=200809
 * (it was removed from POSIX.1-2008). theoraplay.c and others use it. */
#ifndef usleep
int usleep(unsigned int usec);
#endif

/* cpp-httplib calls strcasecmp without including <strings.h>. */
#ifndef strcasecmp
int strcasecmp(const char *s1, const char *s2);
#endif

/* Vita's <netinet/in.h> lacks the IN6_IS_ADDR_* predicate macros. */
#ifndef IN6_IS_ADDR_LINKLOCAL
#define IN6_IS_ADDR_LINKLOCAL(a) \
	(((const unsigned char *)(a))[0] == 0xfe && \
	 (((const unsigned char *)(a))[1] & 0xc0) == 0x80)
#endif

#ifdef __cplusplus
}
#endif

#endif /* VITA_POSIX_COMPAT_H */
