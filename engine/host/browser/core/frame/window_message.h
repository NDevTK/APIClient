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
/* §9.4.4's postMessage on ONE realm's WindowProxy prototype — declared into core/realm.h's list above. */
void window_message_install_proto(JSContext *ctx);

void window_message_install(JSContext *ctx, JSValueConst global, const char *origin);

/* §9.4.4 STEP 7 FROM ANOTHER INSTANCE — the host routed it here and stamped the sender's origin, which the
   engine may not do for itself. `target_origin` is what the SENDER asked for ("*" or NULL for any); the check
   against it happens at delivery, here, because the target may have navigated since the post. */
void window_message_deliver_remote(JSContext *ctx, const char *sender_doc, const char *sender_origin,
                                   const char *target_origin, const uint8_t *bytes, size_t len);
/* THE WIRE HALF of the above: take apart the part of a routed `windowproxy.post` record that belongs to this
   file (`<targetOrigin>\t<base64>`) and deliver it. The transport's own fields — which instance, whose world —
   are the router's and are already consumed by the time this is called. One reader, beside the writer. */
void window_message_route(JSContext *ctx, const char *tail, const char *sender_doc, const char *sender_origin);
void window_message_free(JSContext *ctx);


#endif
