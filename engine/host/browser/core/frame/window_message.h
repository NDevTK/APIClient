/* window.postMessage — HTML §9.4.4's window post message steps. See window_message.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_MESSAGE_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_MESSAGE_H
#include "quickjs.h"

/* Installs `postMessage` on the global and mints this navigable's one WindowProxy. `origin` is this document's
   serialized origin — the value every message this window sends carries as `event.origin`, and the one a
   page's handler checks before trusting `event.data`. */
void window_message_install(JSContext *ctx, JSValueConst global, const char *origin);
void window_message_free(JSContext *ctx);

/* This navigable's WindowProxy — §7.2.5.1 says there is ONE, so `event.source` compares equal across messages.
   BORROWED. */
JSValueConst window_message_proxy(void);

#endif
