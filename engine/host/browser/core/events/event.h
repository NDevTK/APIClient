/* THE EVENT INTERFACE — DOM §2.2. The object every listener receives, and the thing dispatchEvent takes. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_H
#include <stdbool.h>

#include "quickjs.h"

void event_init(JSContext *ctx);
/* §2.2's PROTOTYPE FOR ONE REALM. Run it where a realm's other intrinsics are added, exactly once per realm —
   the agent's first realm gets it from event_init, because every prototype derived from Event chains to that
   realm's and so it has to exist before them. */
void event_install_proto(JSContext *ctx);
void event_free(JSContext *ctx);
/* `Event` as a global: the interface object, its prototype, and §2.2's phase constants. */
void event_install(JSContext *ctx, JSValueConst global);

/* `Event.prototype`, for a DERIVED interface to chain from — `interface PromiseRejectionEvent : Event` is a
   real prototype chain a page can walk, not a flag on Event. */
/* PER REALM — §3.7 gives each its own, and here that decides ANSWERS and not just identities: a C member runs
   in the realm that DEFINED it, so one shared prototype answers every document out of whichever realm built it
   first. OWNED: the caller frees. */
JSValue event_proto(JSContext *ctx);
/* Mint an event the ENGINE fires (`load`, `abort`, `DOMContentLoaded`). isTrusted is true for these, which is
   exactly the difference from one the page constructs. Returns a new owned Event. */
JSValue event_new(JSContext *ctx, const char *type, bool bubbles, bool cancelable);
/* The same, isTrusted FALSE — a synthetic event the PAGE caused (§3.2.2's click()). */
JSValue event_new_untrusted(JSContext *ctx, const char *type, bool bubbles, bool cancelable);

/* §2.2's INITIALISE, with a DERIVED interface's prototype in place of Event.prototype — the base half of a
   subclass's constructor. `new MessageEvent(type, init)` runs Event's constructor steps and then its own, so
   the derived component builds the object through this and then hangs its own slots on it. Doing it the other
   way round — a derived interface minting a plain object and copying the base attributes onto it — is what
   event_target.c did before Event existed, and it produced an object that answered `instanceof Event` false
   and let a page assign to `defaultPrevented`.
   `type` is a VALUE, not a C string, because a derived constructor's `type` arrives already converted by the
   IDL declaration and re-stringifying it would run the page's toString a second time. */
/* `proto` is CONSUMED — quickjs's own convention for an owned argument (a plain JSValue where a borrowed one
   is JSValueConst), and it is consumed rather than borrowed because every caller gets it from a
   `<Interface>_proto(ctx)` that returns an OWNED reference. Borrowing left the release to each call site, and
   the site that matters most — the PAGE-facing constructor — forgot it: one `new MessageEvent('x')` leaked a
   reference to MessageEvent.prototype, which roots its realm, so the entire context and every object in it
   survived teardown and the runtime's leak walk reported 1597 anonymous Functions with no hint of the owner.
   Taking ownership here deletes the obligation instead of restating it at each caller. */
JSValue event_new_derived(JSContext *ctx, JSValue proto, JSValueConst type,
                          bool bubbles, bool cancelable, bool composed, bool trusted);

/* §2.2's initialise-an-existing-event steps — what `initEvent` performs, and what a derived interface's legacy
   initialiser (`initMessageEvent`) performs before its own. Answers false when the dispatch flag is set, which
   is the spec's early return the derived initialiser must honour before touching anything of its own. */
bool event_reinit(JSContext *ctx, JSValueConst ev, JSValueConst type, bool bubbles, bool cancelable);

/* The internal slots §2.2's algorithms read. NULL/false for anything that is not an Event, which is the brand
   check dispatchEvent performs — the slots live under a private Symbol, so a page cannot forge one. */
bool    event_is(JSContext *ctx, JSValueConst v);
JSValue event_type(JSContext *ctx, JSValueConst ev);            /* a new owned string, or JS_UNDEFINED */
bool    event_canceled(JSContext *ctx, JSValueConst ev);        /* the canceled flag */
/* THE SAME FLAG, WRITTEN — and it is NOT §2.2's "set the canceled flag", which is an algorithm with two
   conditions on it (cancelable is true, and the in-passive-listener flag is unset) and whose one caller is
   preventDefault(). HTML §7.2.6.8's abort the ongoing navigation step 6 writes the FLAG: "if event's dispatch
   flag is set, then set event's canceled flag to true", with no condition, because a navigation aborted while
   its `navigate` event is still dispatching must report itself as canceled to the rest of §7.2.6.10.4 whatever
   the listener currently running is allowed to do. Routing that through preventDefault's algorithm would make a
   non-cancelable navigate event silently stay uncanceled. */
void    event_set_canceled(JSContext *ctx, JSValueConst ev, bool on);
/* DOM §2.2 Interface Event's SET THE CANCELED FLAG — the ALGORITHM the line above is deliberately not: "To set
   the canceled flag, given an event event, if event's cancelable attribute value is true and event's in passive
   listener flag is unset, then set event's canceled flag to true."
   THREE ALGORITHMS PERFORM IT AND NONE OF THEM MAY SPELL IT AGAIN. §2.2's preventDefault() ("The
   preventDefault() method steps are to set the canceled flag with this"), §2.2's legacy returnValue setter
   ("set the canceled flag with this if the given value is false"), and HTML §8.1.8.1's event handler processing
   algorithm step 6, which is where `return false` cancels. Two of those three were written out by hand and the
   second of them had dropped the in-passive-listener half — so `e.returnValue = false` inside a `{passive:true}`
   listener cancelled an event `preventDefault()` in the same listener could not, which is the one guarantee the
   passive flag exists to give the user agent, given away through the spelling nobody looks at. */
void    event_set_the_canceled_flag(JSContext *ctx, JSValueConst ev);
/* §2.2's currentTarget — "the object whose event listener's callback is currently being invoked", which §2.9's
   `invoke` writes at every path item. A new owned reference, or JS_NULL outside a dispatch. HTML §8.1.8.1 step
   4 reads it (its special-error-event-handling test is about the CURRENT target, not the event's target) and
   step 5 invokes the handler with it as the callback this value. */
JSValue event_current_target(JSContext *ctx, JSValueConst ev);
bool    event_stop_immediate(JSContext *ctx, JSValueConst ev);  /* the stop-immediate-propagation flag */
bool    event_bubbles(JSContext *ctx, JSValueConst ev);         /* does it travel up the propagation path */
bool    event_stop_propagation(JSContext *ctx, JSValueConst ev);
/* §2.2's initialized flag: false only for an event §4.5's createEvent produced and initEvent has not yet
   initialised. dispatchEvent throws InvalidStateError on one, which is why the flag is public here. */
bool    event_initialized(JSContext *ctx, JSValueConst ev);
/* §4.5 createEvent steps 6-8 over an event its interface has just built — empty type, untrusted, and the
   initialized flag UNSET. The factory owns the table; the slots are this component's. */
void    event_uninitialize(JSContext *ctx, JSValueConst ev);
void    event_set_phase(JSContext *ctx, JSValueConst ev, int phase);   /* AT_TARGET, then BUBBLING_PHASE */
/* §2.2's RELATEDTARGET and TOUCH TARGET LIST — EVERY event has both, and neither is an Event IDL member: §2.2
   gives the Event itself the associated values and UIEvents/Touch Events only DEFINE ATTRIBUTES over them
   ("other specifications use relatedTarget to define a relatedTarget attribute"). So they are this component's
   state whether or not an interface that fills them exists yet, and §2.9 retargets them at every path item
   regardless — a dispatch that skipped them because no MouseEvent exists would be a dispatch missing step 4's
   suppression and `invoke` steps 4-5.
   The getters are OWNED. The relatedTarget is JS_NULL when there is none; the touch target list is JS_NULL when
   it is EMPTY, which is the same state one allocation cheaper and the same spelling §2.9's path uses. */
JSValue event_related_target(JSContext *ctx, JSValueConst ev);
void    event_set_related_target(JSContext *ctx, JSValueConst ev, JSValueConst related);
JSValue event_touch_target_list(JSContext *ctx, JSValueConst ev);
void    event_set_touch_target_list(JSContext *ctx, JSValueConst ev, JSValueConst list);
/* §2.9 step 11's CLEARTARGETS — target, relatedTarget and touch target list ALL back to their initial state,
   as ONE operation because the spec states them as one step and because the target half shipped alone: an
   event whose outermost target was inside a shadow tree kept handing out a retargeted relatedTarget after the
   walk, which is the same leak step 11 exists to close. */
void    event_clear_targets(JSContext *ctx, JSValueConst ev);
/* §2.9 "invoke" steps 3 and 7, which are two steps: the target the event was dispatched at (set once for the
   whole walk) and the object whose listeners are running right now (set at every path item). */
void    event_set_target(JSContext *ctx, JSValueConst ev, JSValueConst target);
void    event_set_current(JSContext *ctx, JSValueConst ev, JSValueConst current);
/* §2.9 step 3: an event the page dispatches is untrusted, whatever it was when constructed. */
void    event_set_trusted(JSContext *ctx, JSValueConst ev, bool trusted);
/* §2.9 steps 7-10 as ONE operation, because the spec groups them and because splitting them is how three of the
   four went missing: phase NONE, currentTarget null, path empty, and the dispatch/stop-propagation/
   stop-immediate-propagation flags unset. */
void    event_end_dispatch(JSContext *ctx, JSValueConst ev);
/* §2.9's PATH, which is the EVENT'S state — composedPath() reads "this's path". `path` is BORROWED (dup'd). */
void    event_set_path(JSContext *ctx, JSValueConst ev, JSValueConst path);
/* §2.2's IN PASSIVE LISTENER FLAG — set by "inner invoke" around a passive listener, and the reason that
   listener's preventDefault() does nothing. */
void    event_set_in_passive(JSContext *ctx, JSValueConst ev, bool on);
/* §2.2's COMPOSED FLAG and the INVOCATION TARGET of §2.9's FIRST EVENT PATH ITEM (OWNED) — the two facts DOM
   §4.8's shadow-root get-the-parent asks about the EVENT ("returns null if event's composed flag is unset and
   shadow root is the root of event's path's first event path item's INVOCATION TARGET"). The rest of that
   condition is about the TREE and is answered where nodes are known; this file never learns what a node is.
   It answers with the item's invocation target rather than the item, and rather than its shadow-adjusted
   target: an item has both, and for the entries §4.8's condition is about they are different objects. */
bool    event_composed(JSContext *ctx, JSValueConst ev);
JSValue event_path_first_invocation_target(JSContext *ctx, JSValueConst ev);
/* §2.9: an event that has been dispatched cannot be re-dispatched while in flight. */
bool    event_dispatch_flag(JSContext *ctx, JSValueConst ev);
void    event_set_dispatch_flag(JSContext *ctx, JSValueConst ev, bool on);

/* §2.9's legacyOutputDidListenersThrowFlag — "inner invoke" step 2.11 sets it when a listener throws, and it
 * is an OUTPUT of the dispatch rather than a state of the event.
 *
 * IT LIVES ON THE EVENT because that is the only thing the two sides share. The spec passes it as an optional
 * output parameter of `dispatch`, and this engine's dispatch is a step machine reached through a call request
 * whose one result is §2.9's own return value (`!canceled`) — a second out-parameter would have to be threaded
 * through the request's operand shape, which is the interpreter's and not this component's. The event is
 * created by the algorithm that fires it, dispatched once and dropped, so there is exactly one reader and
 * exactly one writer, and `event_end_dispatch` deliberately does NOT clear it: the whole point is that the
 * caller reads it AFTER the dispatch has returned.
 *
 * ONE STANDARD USES IT and its two algorithms are the reason this exists: Indexed Database §5.9 step 9.2 and
 * §5.10 step 9.2 ABORT the transaction when a listener threw — which is how a page whose `onsuccess` throws
 * gets its whole transaction rolled back with an "AbortError" rather than a silently committed one. Without
 * the flag that arm cannot be written at all, and the failure mode is invisible: the transaction commits and
 * every record it wrote stays. */
bool    event_listeners_threw(JSContext *ctx, JSValueConst ev);
void    event_set_listeners_threw(JSContext *ctx, JSValueConst ev, bool on);

#endif
