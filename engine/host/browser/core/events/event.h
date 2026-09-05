/* THE EVENT INTERFACE — DOM §2.2. The object every listener receives, and the thing dispatchEvent takes. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_H
#include <stdbool.h>

#include "quickjs.h"

void event_init(JSContext *ctx);
/* §2.2's PROTOTYPE AND §3.8's GLOBAL PROPERTY REFERENCE, FOR ONE REALM. Run it where a realm's other
   intrinsics are added, exactly once per realm — the agent's first realm gets it from event_init, because
   every prototype derived from Event chains to that realm's and so it has to exist before them.
   ONE ENTRY BECAUSE §3.8 IS GIVEN A REALM. `define the global property references` takes "target" and "a realm
   realm", and step 1's population is "every interface that is exposed in realm" — no Document appears in the
   algorithm. `Event` is `[Exposed=*]`, so it belongs in EVERY realm; while its interface object was installed
   from core/platform.c's per-document column it was absent from every realm no Document was installed over. */
void event_install_realm(JSContext *ctx);
/* Undone ONCE PER AGENT, from core/platform.c's release column — which takes the RUNTIME, because what
   this gives back is the AGENT's and not any realm's. It also releases the thirteen subclasses
   event_init declares, so the whole family is one row. */
void event_free(JSRuntime *rt);
/* `Event.prototype`, for a DERIVED interface to chain from — `interface PromiseRejectionEvent : Event` is a
   real prototype chain a page can walk, not a flag on Event. */
/* PER REALM — §3.7 gives each its own, and here that decides ANSWERS and not just identities: a C member runs
   in the realm that DEFINED it, so one shared prototype answers every document out of whichever realm built it
   first. OWNED: the caller frees. */
JSValue event_proto(JSContext *ctx);
/* Mint an event the ENGINE fires (`load`, `abort`, `DOMContentLoaded`). isTrusted is true for these, which is
   exactly the difference from one the page constructs. Returns a new owned Event. */
JSValue event_new(JSContext *ctx, const char *type, bool bubbles, bool cancelable);
/* The same, isTrusted FALSE — a synthetic event the PAGE caused. It is NOT what HTML §6.5 Activation behavior
   of elements' click() fires: that method's step 4 fires a synthetic POINTER event, so the object it builds
   has to answer DOM §2.9 step 6.4's MouseEvent brand and comes from mouse_event_new_synthetic instead. (The
   number this line used to give for click() was §3.2.2, which is "Elements in the DOM".) */
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
/* DOES THIS EVENT'S TYPE EQUAL `want` — one string compare, spelled ONCE for the whole tree because several
   standards' steps make one and every extra spelling is another place for it to be case-folded, truncated or
   applied to the wrong half of a conjunction. Three algorithms ask it and each pairs it with a BRAND:
   DOM §2.9 dispatch step 6.4 ("event is a MouseEvent object and event's type attribute is `click`"), and
   HTML §8.1.8.1 Event handlers' step 4 (ErrorEvent) and step 6's first arm (BeforeUnloadEvent). It answers
   the TYPE half only: an interface question is `<interface>_is`, and the two are never substituted for each
   other — an event's type is a page-supplied string, so it decides nothing about what the object IS. */
bool    event_type_is(JSContext *ctx, JSValueConst ev, const char *want);
bool    event_canceled(JSContext *ctx, JSValueConst ev);        /* the canceled flag */
/* THE SAME FLAG, WRITTEN — and it is NOT §2.2's "set the canceled flag", which is an algorithm with two
   conditions on it (cancelable is true, and the in-passive-listener flag is unset).
   TWO SPECS WRITE THIS FLAG DIRECTLY, and each says so in the only way that settles it — by LINKING the flag
   rather than the algorithm. DOM gives the two dfns separate anchors (`#canceled-flag` for the flag,
   `#set-the-canceled-flag` for the gated algorithm), so which one a step means is not a matter of reading its
   prose sympathetically; it is in the href.
     HTML §7.2.6.8 Ongoing navigation tracking's abort the ongoing navigation step 6 — "If event's dispatch flag
     is set, then set event's canceled flag to true" — links `#canceled-flag`. A navigation aborted while its
     `navigate` event is still dispatching must report itself as canceled to the rest of §7.2.6.10.4 whatever
     the listener currently running is allowed to do.
     HTML §8.1.8.1 Event handlers' event handler processing algorithm step 6 ("Process return value as
     follows") links `#canceled-flag` in ALL THREE of its arms — the BeforeUnloadEvent arm's "Set event's
     canceled flag", the special-error arm's "If return value is true, then set event's canceled flag", and the
     otherwise arm's "If return value is false, then set event's canceled flag". That whole page references
     `#set-the-canceled-flag` ZERO times.
   Routing either through preventDefault's algorithm silently narrows it: a non-cancelable event, or one being
   handled by a passive listener, would stay uncanceled where the standard cancels it. */
void    event_set_canceled(JSContext *ctx, JSValueConst ev, bool on);
/* DOM §2.2 Interface Event's SET THE CANCELED FLAG — the ALGORITHM the line above is deliberately not: "To set
   the canceled flag, given an event event, if event's cancelable attribute value is true and event's in passive
   listener flag is unset, then set event's canceled flag, and do nothing otherwise."
   EXACTLY TWO ALGORITHMS PERFORM IT AND NEITHER MAY SPELL IT AGAIN — both in §2.2, and both linking
   `#set-the-canceled-flag`: preventDefault() ("The preventDefault() method steps are to set the canceled flag
   with this") and the legacy returnValue setter ("The returnValue setter steps are to set the canceled flag
   with this if the given value is false; otherwise do nothing"). Both were once written out by hand and the
   setter had dropped the in-passive-listener half — so `e.returnValue = false` inside a `{passive:true}`
   listener cancelled an event `preventDefault()` in the same listener could not, which is the one guarantee the
   passive flag exists to give the user agent, given away through the spelling nobody looks at.
   HTML §8.1.8.1 step 6 IS NOT A THIRD PERFORMER, and listing it here as one is what put a gated call into the
   event-handler path: `<body onload="return false">` and `document.body.onwheel = () => false` are precisely
   the two cases the gate swallows and the standard does not. */
void    event_set_the_canceled_flag(JSContext *ctx, JSValueConst ev);
/* §2.2's currentTarget — "the object whose event listener's callback is currently being invoked", which §2.9's
   `invoke` writes at every path item. A new owned reference, or JS_NULL outside a dispatch. HTML §8.1.8.1 step
   4 reads it (its special-error-event-handling test is about the CURRENT target, not the event's target) and
   step 5 invokes the handler with it as the callback this value. */
JSValue event_current_target(JSContext *ctx, JSValueConst ev);
/* §2.2's TARGET, read — "the object to which event is dispatched", which §2.9's `invoke` step 3 writes at every
   path item out of the nearest shadow-adjusted target at or before that item. A new owned reference, or JS_NULL
   for an event that has never been dispatched and after §2.9 step 11's clearTargets; both of those are POSITIVE
   answers and not misses, and one of them is the state CSSOM VIEW §10 "Extensions to the MouseEvent Interface"
   writes `offsetX` step 2 for.
   IT IS NOT `event_current_target`, AND SUBSTITUTING EITHER FOR THE OTHER IS WRONG AT EVERY SITE THAT ASKS ABOUT
   A NODE'S GEOMETRY OR ITS TREE. §10 states `offsetX` step 1 over "the padding edge of the target node", and one
   dispatch separates the two five ways: in WPT shadow-dom/MouseEvent-prototype-offsetX-offsetY.html a single
   `mousedown` reports ONE offset to listeners on the inner node, on its container, on the shadow root, on the
   host and on the document body, because the current target is five objects over that walk and the target is
   the one node the event was dispatched at.
   The setter beside it shipped alone for as long as no member asked §2.9's own question of the target — a name
   written everywhere and read nowhere, which is a broken contract rather than an unused API. */
JSValue event_target(JSContext *ctx, JSValueConst ev);
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
/* §2.9 "invoke" steps 3 and 7, which are two steps and BOTH RUN AT EVERY PATH ITEM. This line used to say the
   target was set once for the whole walk (this file's retired sentence, so it carries no quotation mark — that
   mark is a standard's here, and the citation audit judges anything wearing it against the section cited beside
   it), and its ONE caller's own comment says the opposite in as many words:
   steps 1-2 walk BACKWARD from this item to the nearest one whose shadow-adjusted target is non-null and step 3
   writes THAT, so a listener outside a closed shadow tree reads the HOST where a listener inside reads the node
   the event was dispatched at — which is the whole of what retargeting is. What IS written once is neither of
   them: it is each path ITEM's shadow-adjusted target, fixed while §2.9 built the path.
   The retired sentence is kept here because it is the one a reader re-derives from the IDL, where `target` looks
   like a property of the dispatch: acting on it would cache the target across the walk and hand every outer
   listener a node from a tree the page cannot see, which is the leak step 11 exists to close arriving earlier. */
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
