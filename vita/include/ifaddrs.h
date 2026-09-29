// SPDX-License-Identifier: GPL-3.0-or-later
/* ifaddrs.h — stub for Vita (newlib has no interface enumeration).
 *
 * cpp-httplib (src/net/httplib.h) includes <ifaddrs.h> and calls
 * getifaddrs/freeifaddrs in if2ip() when USE_IF2IP is defined (any
 * non-Windows/Android/AIX/MVS host). Vita has no getifaddrs; the stub
 * reports "no interfaces" so if2ip() returns an empty string, which is
 * the correct answer — mkxp-z's HTTP client dials by hostname and never
 * binds to a named interface on this port.
 *
 * Keep this header self-contained: no project includes.
 */
#ifndef VITA_IFADDRS_H
#define VITA_IFADDRS_H

#include <sys/socket.h>

struct ifaddrs {
	struct ifaddrs  *ifa_next;
	char            *ifa_name;
	unsigned int     ifa_flags;
	struct sockaddr *ifa_addr;
	struct sockaddr *ifa_netmask;
	union {
		struct sockaddr *ifu_broadaddr;
		struct sockaddr *ifu_dstaddr;
	} ifa_ifu;
	void            *ifa_data;
};
#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr   ifa_ifu.ifu_dstaddr

#ifdef __cplusplus
extern "C" {
#endif

static inline int getifaddrs(struct ifaddrs **ifap)
{
	if (ifap)
		*ifap = 0;
	return 0;
}

static inline void freeifaddrs(struct ifaddrs *ifa)
{
	(void)ifa;
}

#ifdef __cplusplus
}
#endif

#endif /* VITA_IFADDRS_H */
