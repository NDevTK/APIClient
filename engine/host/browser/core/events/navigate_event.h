/* THE NavigateEvent INTERFACE — HTML §7.2.6.10.1. See navigate_event.c.
 *
 * WHAT IT IS. Every navigation a Document can see fires ONE event before it commits, and this is that event.
 * §7.2.6.10.4's three wrappers build it — a traverse, a push/replace/reload, a download request — and the inner
 * navigate event firing algorithm dispatches it at the Navigation. A page's `navigate` listener is where a
 * router decides whether the navigation happens at all: `preventDefault()` on a cancelable one stops it, which
 * is why the event carries a real `cancelable` computed from §7.2.6.10.4's traverseCanBeCanceled and not a
 * constant.
 *
 * WHAT IS NOT HERE AND WHY. §7.2.6.10.1 declares TWO operations — `intercept(options)` and `scroll()` — and
 * neither is installed. They are the INTERCEPTION half of the navigation API: `intercept()` appends to the
 * event's navigation handler list and moves its INTERCEPTION STATE off "none", which is the one condition that
 * makes §7.2.6.10.4's transition, §7.2.6.10.4's commit switch, §7.2.6.10.5's finish/focus/scroll steps and
 * §7.2.6.8's NavigationTransition reachable at all. None of those exist, so the two operations are honestly
 * ABSENT and the IDL audit reports them — a shape-only `intercept()` that recorded handlers nothing runs would
 * be the stub §NO STUBS forbids, and it would silently swallow a router's navigation.
 * THE INTERCEPTION STATE IS NOT A FIELD HERE EITHER. It would be a slot with exactly one writer — its
 * initialisation to "none" — and every branch over it a branch with one arm: a fabricated datum in the sense
 * §Consumer-defaults describes. §7.2.6.10.4's inner algorithm asks about it with a `realm_awaits` over
 * `NavigateEvent.prototype.intercept` instead, so the day the operation lands the assertion fires at the step
 * whose other arm has to be written.
 *
 * THE ABORT CONTROLLER IS HELD AS ITS SIGNAL. §7.2.6.10.1 gives the event "an abort controller", and the only
 * two things the standard ever does with it are §7.2.6.8's "Signal abort on event's abort controller" and
 * "event's abort controller's signal". core/dom/abort.h states signal abort over the SIGNAL — it is one
 * operation, and
 * Streams §5.4's WritableStreamDefaultController reaches it the same way — so the controller object would be an
 * allocation nothing reads. `signal` is the IDL attribute over it, so the two are the same object.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, for the reason core/events/event.h gives: a slot written
 * as a property write is captured by the COW delta, so the event's contents time-travel with the flow that
 * fired it, and the symbol is a brand a page cannot forge.
 *
 * THERE IS NO createEvent ROW: DOM §4.5's table does not name this interface, so
 * `document.createEvent('NavigateEvent')` is step 3's NotSupportedError. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_NAVIGATE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_NAVIGATE_EVENT_H

#include <stdbool.h>

#include "quickjs.h"
#include "core/structured_clone.h"

void navigate_event_init(JSContext *ctx);
/* §3.7: THIS REALM's prototype and interface object — declared into core/realm.h's list. */
void navigate_event_install_protos(JSContext *ctx);
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private
   Symbol, a class id and this interface's member declarations — and every prototype it built is in
   some realm's class-proto slot and goes with that realm. Reached from core/events/event.c's
   event_free_subclasses, which is core/platform.c's `event` row. */
void navigate_event_free(JSRuntime *rt);

/* §7.2.6.10.4's "let event be the result of CREATING AN EVENT given NavigateEvent, in navigation's relevant
 * realm", with the initialisations its inner algorithm then performs — as ONE call, because §7.2.6.10.4
 * performs every one of them before the dispatch and an event observable between two of them would be an event
 * whose `type` is not yet "navigate".
 *
 * IT TAKES WHAT THE INNER ALGORITHM COMPUTED, not what its callers were given: `cancelable` is the result of
 * §7.2.6.10.4's traverseCanBeCanceled test, `can_intercept` of its can-have-its-URL-rewritten test, and
 * `hash_change` of its four-way conjunction — all three are the ALGORITHM's, so computing them here would be a
 * second copy of steps whose inputs (the navigable, the document's URL) this component does not hold.
 *   `destination` is BORROWED and dupped; the event holds it for the whole dispatch.
 *   `signal` is the event's abort controller's signal, BORROWED and dupped.
 *   `classic_state` is §7.2.6.10.1's CLASSIC HISTORY API STATE — the serialized bytes a `pushState` carries, or
 *      NULL for every other wrapper. BORROWED; the bytes are copied.
 *   `source_element` is `Element?`, JS_NULL when the navigation had no responsible element.
 * TRUSTED, because the user agent fired it — which is what §7.2.6.10.1's `intercept()` reads off `isTrusted`
 * when it lands. The result is OWNED. */
JSValue navigate_event_new_to_fire(JSContext *ctx, const char *navigation_type, JSValueConst destination,
                                   bool can_intercept, bool cancelable, bool user_initiated, bool hash_change,
                                   JSValueConst signal, JSValueConst source_element,
                                   const StructuredData *classic_state);

/* WEB IDL §3.7.6 Attributes' BRAND, as a question — the presence of this interface's OWN private-Symbol slot
   record,
   which is the same test every accessor in navigate_event.c makes and the one §7.2.6.10.4's algorithms make
   about a value they were handed. It takes a context because the record is a property read; a NON-OBJECT
   answers false rather than throwing, so an assertion may ask it. */
bool navigate_event_is(JSContext *ctx, JSValueConst v);

/* §7.2.6.10.1's ABORT CONTROLLER'S SIGNAL, which is the `signal` attribute over it. THREE algorithms read it
   back and each of them decides whether to do anything at all by asking whether it is aborted:
   §7.2.6.10.4's commit-a-navigate-event step 4, its commit handler success steps step 2, and its step 28. That
   is what makes this an export rather than a slot only this file reads — the value is the navigation's own
   cancellation, and every later stage of the navigation asks it. OWNED. */
JSValue navigate_event_signal(JSContext *ctx, JSValueConst ev);

/* THE OTHER INTERNAL SLOTS §7.2.6.10.4 READS BACK — the destination and the navigation type — ARE STILL NOT
   EXPORTED, and that is deliberate rather than an oversight: their readers are commit-a-navigate-event's step 5
   and its step 7 switch, both of which are reached only when the interception state is not "none". Exporting
   one before then would be an entry point with no caller, which is the same defect as a field nobody writes,
   one direction round. They come out with `intercept()`. */

#endif
