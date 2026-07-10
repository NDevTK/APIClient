/* WebSocket + EventSource constructor — Blink modules/websockets + modules/eventsource. The WS/SSE handshake
 * URL is a GET endpoint the analyzer emits; the object's onmessage handler is a scheduler flow. See websocket.c. */
#ifndef ENGINE_HOST_BROWSER_WEBSOCKET_H
#define ENGINE_HOST_BROWSER_WEBSOCKET_H
#include "quickjs.h"
JSValue js_ws_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);   /* new WebSocket(url) / new EventSource(url) */
#endif
