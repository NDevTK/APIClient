/* ErrorEvent — HTML §8.1.4.6 "Runtime script errors", which is where the standard both DECLARES
   `[Exposed=*] interface ErrorEvent : Event` and fires it. See error_event.c.
   THIS LINE STATED A TITLE OF "Interface ErrorEvent", AND HTML CARRIES NO SECTION SO TITLED — every one
   of this component's own citations already says HTML §8.1.4.6 "Runtime script errors" and says it correctly,
   so the banner was one
   equation written at two sites with the checkable half of it fabricated. A title a standard does not
   carry is worse than an absent one: it reads as authoritative and there is nothing to look up. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_ERROR_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_ERROR_EVENT_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

void error_event_init(JSContext *ctx);              /* the slot key + the IDL declarations (agent init) */
/* HTML §8.1.4.6 Runtime script errors' ErrorEvent prototype, its Web IDL §3.7.1 Interface object's interface
   object and the Web IDL §3.8 Platform objects implementing interfaces property reference for it — for ONE
   realm, declared into core/realm.h's list.
   ONE entry because Web IDL §3.8 Platform objects implementing interfaces' `define the global property
   references` is given "target" and "a realm realm" and its step 1 population is "every interface that is
   exposed in realm": no Document appears in it. html.idl declares the interface `[Exposed=*]`, so EVERY realm
   owes the name — and while the interface object was installed from core/platform.c's per-document column, a
   worker realm, which reaches no platform_document_install, received neither. */
void error_event_install_realm(JSContext *ctx);
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private
   Symbol, a class id and this interface's member declarations — and every prototype it built is in
   some realm's class-proto slot and goes with that realm. Reached from core/events/event.c's
   event_free_subclasses, which is core/platform.c's `event` row. */
void error_event_free(JSRuntime *rt);
/* `ErrorEvent.prototype` for this realm. OWNED: the caller frees. */
JSValue error_event_proto(JSContext *ctx);

/* AN ErrorEvent THE ENGINE FIRES, carrying the error information. TRUSTED — the engine fired it, not the page.
 * Every JSValue argument is BORROWED; the result is a new owned ErrorEvent.
 *
 * THE TYPE AND THE CANCELABILITY BELONG TO THE FIRE AND NOT TO THE INTERFACE, which is why they are arguments.
 * Two algorithms dispatch this interface and they disagree on both: HTML §8.1.4.6's report-an-exception step 6.2
 * fires `error` "with the cancelable attribute initialized to true", because an `onerror` returning true is how
 * a page says it HANDLED the exception; HTML §7.2.6.8's abort-a-NavigateEvent step 6 fires `navigateerror` with
 * DOM's plain fire-an-event and no flags at all, because nothing branches on its result. Hardcoding the first
 * caller's pair here is what made the second one unable to reach this constructor. It does not BUBBLE in either
 * case, and it is never composed. */
JSValue error_event_new(JSContext *ctx, const char *type, bool cancelable, JSValueConst message,
                        JSValueConst filename, uint32_t lineno, uint32_t colno, JSValueConst error);

/* IS THIS AN ErrorEvent — the slot record IS the brand, for the reason event.h's `event_is` gives: the record
   hangs off a private Symbol a page cannot forge, so an object carrying five same-named properties is still not
   one. HTML §8.1.8.1's event handler processing algorithm step 4 asks it: "let special error event handling be
   true if event IS AN ErrorEvent OBJECT, …", and that test is what decides whether `window.onerror` is invoked
   with five arguments or with the event. A `typeof`-shaped answer would hand a page's hand-rolled object the
   legacy five-argument shape. */
bool error_event_is(JSContext *ctx, JSValueConst ev);

/* THE FIVE ARGUMENTS §8.1.8.1 STEP 5 INVOKES AN OnErrorEventHandler WITH, in the standard's own order:
 * "« event's message, event's filename, event's lineno, event's colno, event's error »". `out` is five slots
 * and receives five NEW OWNED values, which the caller releases.
 *
 * THEY ARE THE INTERNAL VALUES AND NOT THE IDL GETTERS' ANSWERS, which matters because a page may shadow
 * `ErrorEvent.prototype.message`: §8.1.8.1 reads the attribute VALUES, so this reads the slot record, and going
 * through the prototype chain would let the page rewrite the arguments its own handler is called with — and
 * would make a host algorithm run the page's code from C, which is the drive-to-completion this engine aborts
 * on rather than a subtle infidelity. */
void error_event_handler_arguments(JSContext *ctx, JSValueConst ev, JSValue *out);

#endif
