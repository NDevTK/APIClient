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
/* §2.2's COMPOSED FLAG and §2.9's FIRST EVENT PATH ITEM (OWNED, JS_UNDEFINED outside a dispatch) — the two
   facts DOM §4.8's shadow-root get-the-parent asks about the EVENT. The rest of that condition is about the
   TREE and is answered where nodes are known; this file never learns what a node is. */
bool    event_composed(JSContext *ctx, JSValueConst ev);
JSValue event_path_first(JSContext *ctx, JSValueConst ev);
/* §2.9: an event that has been dispatched cannot be re-dispatched while in flight. */
bool    event_dispatch_flag(JSContext *ctx, JSValueConst ev);
void    event_set_dispatch_flag(JSContext *ctx, JSValueConst ev, bool on);

#endif
