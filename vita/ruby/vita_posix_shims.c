// SPDX-License-Identifier: GPL-3.0-or-later
/* vita_posix_shims.c — stubs ONLY for POSIX bits MRI references that
 * Vita newlib genuinely does not provide. Do NOT stub symbols that exist
 * in libc.a (fork, wait, system, pipe, pipe2, select, poll, signal,
 * getrlimit, setrlimit, getrusage, nl_langinfo, memmem, memrchr, ...):
 * a static-archive stub would silently override the real implementation.
 *
 * Prefer ac_cv_func_*=no in config.site so most of these
 * are never called. This file is the safety net for unconditional refs.
 *
 * Verified present in $VITASDK/arm-vita-eabi/lib/libc.a (nm):
 *   fork, wait, execve, system, pipe, pipe2, select, poll, signal,
 *   getrlimit, setrlimit, getrusage, nl_langinfo, memmem, memrchr,
 *   dup, close, open, fcntl
 * Absent: vfork, execv, execvp, execl, execle, waitpid, sigaction,
 *   sigprocmask, mmap family, getlogin, getpwuid, dlopen family, ppoll,
 *   umask, dup2, getpagesize, popen, pclose, getppid
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

/* --- process: missing entry points (keep execve/wait/system out — libc has them) --- */
pid_t vfork(void) { errno = ENOSYS; return -1; }
int execv(const char *path, char *const argv[]) { (void)path; (void)argv; errno = ENOSYS; return -1; }
int execvp(const char *file, char *const argv[]) { (void)file; (void)argv; errno = ENOSYS; return -1; }
int execl(const char *path, const char *arg, ...) { (void)path; (void)arg; errno = ENOSYS; return -1; }
int execle(const char *path, const char *arg, ...) { (void)path; (void)arg; errno = ENOSYS; return -1; }
pid_t waitpid(pid_t pid, int *status, int options) { (void)pid; (void)status; (void)options; errno = ENOSYS; return -1; }
pid_t getppid(void) { return 1; }

/* --- resource: not in newlib --- */
/* (getrlimit/setrlimit/getrusage ARE in Vita libc — do not stub.) */

/* --- mmap family: newlib has no sys/mman.h / mmap --- */
#if defined(VITA_PROVIDE_MMAP_STUBS)
#include "vita_mman.h"
#undef mmap
#undef munmap
#undef mprotect
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  return vita_mmap(addr, length, prot, flags, fd, offset);
}
int munmap(void *addr, size_t length) { return vita_munmap(addr, length); }
int mprotect(void *addr, size_t length, int prot) { return vita_mprotect(addr, length, prot); }
#endif

/* --- identity --- */
char *getlogin(void) { return (char *)"vita"; }
void *getpwuid(int uid) { (void)uid; return 0; }

/* --- dlopen --- */
void *dlopen(const char *file, int mode) { (void)file; (void)mode; return 0; }
void *dlsym(void *handle, const char *symbol) { (void)handle; (void)symbol; return 0; }
int dlclose(void *handle) { (void)handle; return -1; }
char *dlerror(void) { return (char *)"dlopen not supported on Vita"; }

/* --- signals: newlib DECLARES these but libc.a does not define them --- */
/* signal() itself is in libc; do not override it. Signatures must match
 * sys/signal.h or the shim fails to compile against the prototypes. */
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
  (void)signum; (void)act; (void)oldact; errno = ENOSYS; return -1;
}
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
  (void)how; (void)set; (void)oldset; errno = ENOSYS; return -1;
}

/* --- fs / fd helpers newlib lacks (all verified absent from Vita libc.a) --- */
/* umask: no process umask on Vita; accept and report previous = 0. */
mode_t umask(mode_t mask) { (void)mask; return 0; }

/* dup2: implement via close(newfd)+fcntl(oldfd, F_DUPFD, newfd). Vita newlib
 * has dup/close/open/fcntl but not dup2. MRI uses dup2 unconditionally in
 * ruby_sysinit/pipe paths even when configure says no. */
int dup2(int oldfd, int newfd) {
  int r;
  if (oldfd == newfd)
    return newfd;
  close(newfd);
  r = fcntl(oldfd, F_DUPFD, newfd);
  if (r < 0)
    return -1;
  if (r != newfd) {
    close(r);
    errno = EBADF;
    return -1;
  }
  return r;
}

/* getpagesize: declared `int getpagesize(void)` in newlib; Vita page = 4 KiB. */
int getpagesize(void) { return 4096; }

/* popen/pclose: no shell on Vita. */
FILE *popen(const char *command, const char *type) {
  (void)command; (void)type; errno = ENOSYS; return NULL;
}
int pclose(FILE *stream) { (void)stream; errno = ENOSYS; return -1; }
