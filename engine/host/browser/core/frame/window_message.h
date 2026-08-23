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

/* THIS COMPONENT'S NAME, spelled once and used for both things a name is used for here: the slots it declares
   to core/agent_state.h and the two rows it claims in solver/concolic.c's source registry. The registry's
   give-back is keyed by it, so a declaration and a release naming two different owners is the one way that can
   go wrong — and a literal typed out five times is how it would. */
#define WM_COMPONENT "window_message"

/* THE TWO ATTACKER SOURCES THIS COMPONENT OWNS, SPELLED ONCE. A source is TWO strings that must agree — the
   PROVENANCE `message.data` that concolic_declare_source registers and every @S record names, and the DISPLAY
   SHAPE `{message.data}` the @H surface prints a value as — and the shape is COMPOSED from the provenance here
   so the pair cannot drift; solver/concolic.c asserts that composition at the mint for every declared source.
   A consumer of either half uses these and never a literal of its own, which is the rule the deleted
   `{hash}|{search}|{pm}|{reply}` taxonomy broke: the offscreen matched a `{pm}` shape that no component has
   ever emitted, so live verify could build a PoC for none of these findings.
   THEY ARE `message.*` AND NOT `postMessage.*` BECAUSE THE RECEIVER IS WHO READS THEM. What a page's handler
   holds is a §9.1 MessageEvent, and `event.data` / `event.origin` are the two members it branches on; naming
   them for the SENDER's method would name the one side of this that the victim's code never sees. */
#define MESSAGE_DATA_SRC     "message.data"
#define MESSAGE_DATA_SHAPE   "{" MESSAGE_DATA_SRC "}"
#define MESSAGE_ORIGIN_SRC   "message.origin"
#define MESSAGE_ORIGIN_SHAPE "{" MESSAGE_ORIGIN_SRC "}"

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
