/* NetworkInformation (navigator.connection) — the Network Information API as a real Blink-style module
 * (modules/netinfo/). The connection's properties are genuinely unknown headless, so each is a CONCOLIC value
 * carrying a realistic desktop EXAMPLE and FORKING at a feature-detection branch — a page that adapts on
 * `if (navigator.connection.saveData)` or `connection.effectiveType==='slow-2g'` explores BOTH the data-saver and
 * full arms (each may ship a different resource/endpoint), never pinned to one world. The 'change' listener
 * becomes an orphan flow. Split out of navigator.c; unbuilt members DFAIL via idl_dfail_wrap. */
#ifndef ENGINE_HOST_BROWSER_MODULES_NETINFO_NETWORK_INFORMATION_H
#define ENGINE_HOST_BROWSER_MODULES_NETINFO_NETWORK_INFORMATION_H
#include "quickjs.h"

/* The navigator.connection object (a NetworkInformation instance). */
JSValue network_information_make(JSContext *ctx);

#endif
