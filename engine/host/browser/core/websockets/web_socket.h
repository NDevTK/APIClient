/* WebSocket — WebSockets §3 "The WebSocket interface". See web_socket.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_WEBSOCKETS_WEB_SOCKET_H
#define ENGINE_HOST_BROWSER_CORE_WEBSOCKETS_WEB_SOCKET_H

#include "quickjs.h"

/* Declares the interface for the agent: the class, the member pool entries, §4's task machine and the
   per-realm slot holding a realm's live sockets. Once per agent, from core/platform.c's list. */
void web_socket_init(JSContext *ctx);
/* This realm's prototype, interface object and socket list; declared into realm.h's list by the init. */
void web_socket_install_realm(JSContext *ctx);
void web_socket_free(JSRuntime *rt);

/* WebSockets §7 "Garbage collection"'s make disappear, for every WebSocket object of `ctx`'s realm: HTML
   §7.5.9's unloading document cleanup steps call it. A connection not yet established is failed, which
   withdraws its parked handshake; no task is queued, since the document that would run it is going away.
   Runs inside a flow. */
void web_socket_make_disappear_all(JSContext *ctx);

#endif
