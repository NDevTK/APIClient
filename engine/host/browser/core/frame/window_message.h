/* window.postMessage — HTML §9.3.3 "Posting messages", the WINDOW POST MESSAGE STEPS. See window_message.c.
   THE NUMBER WAS §9.4.4 ON EVERY LINE OF THIS HEADER, AND §9.4.4 IS "Message ports" — a whole section away,
   and the one core/events/message_port.c legitimately cites. §9.4 is "Channel messaging"; the window steps are
   in §9.3 "Cross-document messaging". The .c beside this file was corrected and the header was not, which is
   the failure mode a title beside the number exists to make visible: a reader who looked up §9.4.4 from here
   landed in MessagePort's algorithm and found a `postMessage` there to agree with. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_MESSAGE_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_MESSAGE_H
#include <stdint.h>

#include "quickjs.h"

/* Installs `postMessage` on the global and mints this navigable's one WindowProxy. `origin` is this document's
   serialized origin — the value every message this window sends carries as `event.origin`, and the one a
   page's handler checks before trusting `event.data`. */
/* THE AGENT'S HALF: §9.3.3's `postMessage`, declared once and installed on the shared WindowProxy prototype. */
void window_message_init(JSContext *ctx);
/* §9.3.3's postMessage on ONE realm's WindowProxy prototype — declared into core/realm.h's list above. */
void window_message_install_proto(JSContext *ctx);

void window_message_install(JSContext *ctx, JSValueConst global, const char *origin);

/* §9.3.3 STEP 8 FROM ANOTHER INSTANCE — the host routed it here and stamped the sender's origin, which the
   engine may not do for itself. `target_origin` is what the SENDER asked for ("*" or NULL for any); the check
   against it happens at delivery, here, because the target may have navigated since the post.
   THE STEP NUMBER WAS 7, WHICH IS THE SERIALIZE. §9.3.3 has eight top-level steps and only the LAST is the
   queued task ("Queue a global task on the posted message task source given targetWindow"); step 7 is
   StructuredSerializeWithTransfer, which happens in the SENDER at the call and has already happened by the
   time this is reached. Pointing the reader at 7 pointed them at the half this function is the other side of.
   AND THIS IS WHERE STEP 8 IS QUEUED FOR A ROUTED MESSAGE, which is what makes the two branches of
   js_window_post agree rather than differ: the local branch enqueues the delivery task in this instance, and
   the remote branch emits a transport notice whose task is enqueued HERE, in the instance that holds
   targetWindow — which is the event loop step 8 names. Neither branch fires the event at the call. */
void window_message_deliver_remote(JSContext *ctx, const char *sender_doc, const char *sender_origin,
                                   const char *target_origin, const uint8_t *bytes, size_t len);
/* THE WIRE HALF of the above: take apart the part of a routed `windowproxy.post` record that belongs to this
   file (`<targetOrigin>\t<base64>`) and deliver it. The transport's own fields — which instance, whose world —
   are the router's and are already consumed by the time this is called. One reader, beside the writer. */
void window_message_route(JSContext *ctx, const char *tail, const char *sender_doc, const char *sender_origin);
void window_message_free(JSRuntime *rt);


#endif
