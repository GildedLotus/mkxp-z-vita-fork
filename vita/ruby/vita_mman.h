// SPDX-License-Identifier: GPL-3.0-or-later
/* vita_mman.h — newlib-on-Vita stand-in for <sys/mman.h>.
 * Injected via -include from vita/scripts/build-ruby-vita.sh so MRI files that
 * unconditionally reference mmap/mprotect/munmap still compile.
 * Only anonymous, private, read/write storage is emulated. IO::Buffer's
 * anonymous buffers use it; GC uses aligned malloc, fibers use their arena,
 * and YJIT is disabled. Debug transient-heap mappings must not assume guards.
 */
#ifndef VITA_MMAN_H
#define VITA_MMAN_H

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>

#ifndef PROT_READ
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define PROT_NONE  0
#endif

#ifndef MAP_PRIVATE
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_SHARED 0x01
#define MAP_FAILED ((void *)-1)
#endif

#ifndef MADV_FREE_REUSE
/* leave undefined */
#endif

static inline void *
vita_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void)addr; /* A non-fixed address is only a hint. */
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
vita_munmap(void *addr, size_t length)
{
    (void)length;
    free(addr);
    return 0;
}

static inline int
vita_mprotect(void *addr, size_t length, int prot)
{
    (void)addr; (void)length; (void)prot;
    errno = ENOSYS;
    return -1; /* Heap storage cannot acquire page protections. */
}

#ifndef mmap
#define mmap vita_mmap
#endif
#ifndef munmap
#define munmap vita_munmap
#endif
#ifndef mprotect
#define mprotect vita_mprotect
#endif

/* SA_* bits missing from newlib's incomplete siginfo support. */
#ifndef SA_ONSTACK
#define SA_ONSTACK 0x08000000
#endif
#ifndef SA_SIGINFO
#define SA_SIGINFO 0x04
#endif
#ifndef SA_RESTART
#define SA_RESTART 0x10000000
#endif


/* GNU memmem prototype (implemented in vita_posix_shims.c). */
#ifdef __cplusplus
extern "C" {
#endif
void *memmem(const void *haystack, size_t haystacklen,
             const void *needle, size_t needlelen);
void *memrchr(const void *s, int c, size_t n);
#ifdef __cplusplus
}
#endif

#endif /* VITA_MMAN_H */

/* Newlib declares some functions that are missing or ABI-mismatched.
   Force the configure-chosen "no" paths so MRI uses its own fallbacks. */
#ifdef HAVE_PIPE2
#undef HAVE_PIPE2
#endif
#ifdef HAVE_QSORT_R
#undef HAVE_QSORT_R
#endif
#ifdef HAVE_GNU_QSORT_R
#undef HAVE_GNU_QSORT_R
#endif
#ifdef HAVE_BSD_QSORT_R
#undef HAVE_BSD_QSORT_R
#endif
#ifdef HAVE_SIGACTION
#undef HAVE_SIGACTION
#endif
