/* Channel messaging — Blink core/messaging (MessageChannel, MessagePort) + BroadcastChannel. postMessage-style
 * objects whose message handler is registered as a scheduler FLOW (js_add_listener). See messaging.c. */
#ifndef ENGINE_HOST_BROWSER_MESSAGING_H
#define ENGINE_HOST_BROWSER_MESSAGING_H
#include "quickjs.h"
JSValue js_msg_channel_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);   /* new MessageChannel() -> {port1,port2} */
JSValue js_broadcast_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);      /* new BroadcastChannel(name) */
#endif
