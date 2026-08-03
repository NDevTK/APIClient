/* THE EVENT INTERFACE — DOM §2.2. The object every listener receives, and the thing dispatchEvent takes. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_H
#include "quickjs.h"

void event_init(JSContext *ctx);
void event_free(JSContext *ctx);
/* `Event` as a global: the interface object, its prototype, and §2.2's phase constants. */
void event_install(JSContext *ctx, JSValueConst global);

/* Mint an event the ENGINE fires (`load`, `abort`, `DOMContentLoaded`). isTrusted is true for these, which is
   exactly the difference from one the page constructs. Returns a new owned Event. */
JSValue event_new(JSContext *ctx, const char *type, bool bubbles, bool cancelable);
/* The same, isTrusted FALSE — a synthetic event the PAGE caused (§3.2.2's click()). */
JSValue event_new_untrusted(JSContext *ctx, const char *type, bool bubbles, bool cancelable);

/* The internal slots §2.2's algorithms read. NULL/false for anything that is not an Event, which is the brand
   check dispatchEvent performs — the slots live under a private Symbol, so a page cannot forge one. */
bool    event_is(JSContext *ctx, JSValueConst v);
JSValue event_type(JSContext *ctx, JSValueConst ev);            /* a new owned string, or JS_UNDEFINED */
bool    event_canceled(JSContext *ctx, JSValueConst ev);        /* the canceled flag */
bool    event_stop_immediate(JSContext *ctx, JSValueConst ev);  /* the stop-immediate-propagation flag */
/* §2.9 "dispatch" sets these on the event as it walks: the target it was dispatched at, and the object whose
   listeners are running right now. */
void    event_set_targets(JSContext *ctx, JSValueConst ev, JSValueConst target, JSValueConst current);
/* §2.9 step 3 and "clean up": an event the page dispatches is untrusted, and the walk leaves no current target. */
void    event_set_trusted(JSContext *ctx, JSValueConst ev, bool trusted);
void    event_clear_current(JSContext *ctx, JSValueConst ev);
/* §2.9: an event that has been dispatched cannot be re-dispatched while in flight. */
bool    event_dispatch_flag(JSContext *ctx, JSValueConst ev);
void    event_set_dispatch_flag(JSContext *ctx, JSValueConst ev, bool on);

#endif
