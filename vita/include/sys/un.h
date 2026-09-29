// SPDX-License-Identifier: GPL-3.0-or-later
/* sys/un.h — stub for Vita (no Unix-domain sockets).
 *
 * cpp-httplib (src/net/httplib.h) includes <sys/un.h> on the non-Windows
 * path for optional AF_UNIX client support. Vita has no AF_UNIX; an empty
 * include guard satisfies the #include and the AF_UNIX code paths are
 * never reached (mkxp-z dials by hostname).
 *
 * Keep this header self-contained: no project includes.
 */
#ifndef VITA_SYS_UN_H
#define VITA_SYS_UN_H

#include <sys/socket.h>

#ifndef AF_UNIX
#define AF_UNIX 1
#endif

struct sockaddr_un {
	sa_family_t sun_family;
	char        sun_path[104];
};

#endif /* VITA_SYS_UN_H */
