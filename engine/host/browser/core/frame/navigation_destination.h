/* THE NavigationDestination INTERFACE — HTML §7.2.6.10.3. See navigation_destination.c.
 *
 * WHAT IT IS. A NavigateEvent has to say WHERE the navigation is going, and the place it is going to may not
 * exist yet: a "push" navigation's destination is a URL no session history entry has been built for, and a
 * "traverse" navigation's is an entry the list already holds. One interface answers both, which is why it is
 * NOT a NavigationHistoryEntry — §7.2.6.10.3 gives it the same five questions and a sixth answer for each of
 * them when there is no entry behind it ("" for `key` and `id`, −1 for `index`).
 *
 * IT IS FOUR FIELDS AND NOTHING ELSE — a URL, an ENTRY or null, a SERIALIZED STATE, and an IS SAME DOCUMENT
 * boolean — and every member is one of those read back or a question put to the entry. There is nothing here to
 * keep in step with the history, which is the point: a destination is a snapshot of what the navigation was
 * ASKED for, and §7.2.6.10.3's own note says so of `sameDocument` ("this property indicates the ORIGINAL nature
 * of the navigation. If a cross-document navigation is converted into a same-document navigation using
 * navigateEvent.intercept(), that will not change the value of this property").
 *
 * THE STATE IS BYTES, NOT A LIVE VALUE, for the reason core/frame/session_history.h gives for §7.4.1.1's own
 * fields: a serialized state crosses a park and a session, and `getState()` is a METHOD rather than an attribute
 * precisely because §7.2.6.10.3 deserializes it afresh on every call.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, for the reason core/idl_slots.h gives: a slot written as
 * a property write is captured by the per-flow COW delta, so a destination built in one arm of a fork is
 * invisible to its sibling and travels with the flow that built it. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATION_DESTINATION_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATION_DESTINATION_H

#include <stdbool.h>

#include "quickjs.h"

void navigation_destination_init(JSContext *ctx);
/* §3.7: THIS REALM's prototype and interface object — declared into core/realm.h's list. */
void navigation_destination_install_protos(JSContext *ctx);
void navigation_destination_free(JSContext *ctx);

/* The CLASS, for `required NavigationDestination destination` to brand against — §7.2.6.10.1's NavigateEventInit
   declares the member as an interface type, and idl_iface_brand takes the class that type means. */
JSClassID navigation_destination_class(void);

/* §7.2.6.10.4's "let destination be a new NavigationDestination created in navigation's relevant realm", with
   the four fields its three wrapper algorithms then set. They are set HERE rather than by four setters because
   every one of the three sets all four before anything can observe the object, and a field a caller may leave
   unwritten is a field the getters would have to default.
     `url`      — §7.2.6.10.3's URL, already serialized (the getter returns "this's URL, serialized").
     `entry`    — the destination NavigationHistoryEntry, or JS_NULL. Non-null IF AND ONLY IF the navigation is
                  a "traverse", which §7.2.6.10.3 states and which navigation_destination.c asserts against the
                  key it then answers with.
     `state`    — the SERIALIZED navigation API state, as the ArrayBuffer core/frame/session_history.c holds one
                  in. CONSUMED.
     `same_doc` — §7.2.6.10.3's is same document.
   `url` and `entry` are BORROWED. The result is OWNED. */
JSValue navigation_destination_new(JSContext *ctx, const char *url, JSValueConst entry, JSValue state,
                                   bool same_doc);

/* THE THREE FIELDS §7.2.6.10.4's INNER ALGORITHM BRANCHES ON — the URL (its canIntercept and hashChange
   computations), the IS SAME DOCUMENT flag, and the ENTRY (non-null exactly for a "traverse") — ARE NOT
   EXPORTED YET. Each is a static in navigation_destination.c serving this interface's own getters, and
   exporting one before the algorithm that reads it exists would be an entry point with no caller: the same
   defect as a field nobody writes, one direction round. They come out with that algorithm. */

#endif
