// SPDX-License-Identifier: GPL-3.0-or-later
/* net/if.h — stub for Vita (newlib has no interface-name API).
 *
 * cpp-httplib (src/net/httplib.h) does `#include <net/if.h>` on the
 * non-Windows path. No symbols from it are referenced on the code paths
 * mkxp-z uses (the if2ip() helper is the only consumer of interface
 * enumeration, and our ifaddrs.h stub already reports zero interfaces).
 * An empty include guard satisfies the #include.
 *
 * Keep this header self-contained: no project includes.
 */
#ifndef VITA_NET_IF_H
#define VITA_NET_IF_H

#define IFNAMSIZ 16

#ifdef __cplusplus
extern "C" {
#endif

static inline unsigned int if_nametoindex(const char *ifname)
{
	(void)ifname;
	return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* VITA_NET_IF_H */
