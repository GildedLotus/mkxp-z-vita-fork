// SPDX-License-Identifier: GPL-3.0-or-later
/* sys/mman.h — stub for Vita (newlib has no mmap).
 *
 * cpp-httplib (src/net/httplib.h) includes <sys/mman.h> for its mmap
 * class used by the static-file server. File mappings fail explicitly.
 * Only anonymous private read/write storage maps onto malloc/free, as in
 * vita/ruby/vita_mman.h; neither header provides page protections.
 *
 * Keep this header self-contained: no project includes.
 */
#ifndef VITA_SYS_MMAN_H
#define VITA_SYS_MMAN_H

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifndef PROT_READ
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define PROT_NONE  0
#endif

#ifndef MAP_PRIVATE
#define MAP_PRIVATE  0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON     MAP_ANONYMOUS
#define MAP_SHARED   0x01
#define MAP_FAILED   ((void *)-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline void *
mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	(void)addr;
	if (fd != -1 || offset != 0 || prot != (PROT_READ | PROT_WRITE) ||
	    flags != (MAP_PRIVATE | MAP_ANONYMOUS)) {
		errno = ENOSYS;
		return MAP_FAILED;
	}
	if (!length) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	void *p = malloc(length);
	if (!p) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	memset(p, 0, length);
	return p;
}

static inline int
munmap(void *addr, size_t length)
{
	(void)length;
	free(addr);
	return 0;
}

static inline int
mprotect(void *addr, size_t length, int prot)
{
	(void)addr; (void)length; (void)prot;
	errno = ENOSYS;
	return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* VITA_SYS_MMAN_H */
