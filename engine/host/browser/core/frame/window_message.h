/* window.postMessage — HTML §9.4.4's window post message steps. See window_message.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_MESSAGE_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_MESSAGE_H
#include <stdint.h>

#include "quickjs.h"

/* Installs `postMessage` on the global and mints this navigable's one WindowProxy. `origin` is this document's
   serialized origin — the value every message this window sends carries as `event.origin`, and the one a
   page's handler checks before trusting `event.data`. */
/* THE AGENT'S HALF: §9.4.4's `postMessage`, declared once and installed on the shared WindowProxy prototype. */
void window_message_init(JSContext *ctx);

void window_message_install(JSContext *ctx, JSValueConst global, const char *origin, uint32_t doc_id);
void window_message_free(JSContext *ctx);


#endif
