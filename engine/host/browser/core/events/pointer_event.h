/* PointerEvent — Pointer Events 4 §3.1 "PointerEvent interface". See pointer_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_POINTER_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_POINTER_EVENT_H
#include <stdbool.h>

#include "quickjs.h"

void pointer_event_init(JSContext *ctx);             /* the slot key + the IDL declarations (agent init) */
/* Web IDL §3.7 "Interfaces": one prototype AND one interface object per REALM. */
void pointer_event_install_protos(JSContext *ctx);
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private
   Symbol, a class id and this interface's member declarations — and every prototype it built is in
   some realm's class-proto slot and goes with that realm. Reached from core/events/event.c's
   event_free_subclasses, which is core/platform.c's `event` row. */
void pointer_event_free(JSRuntime *rt);
/* `PointerEvent.prototype` for this realm. OWNED: the caller frees. */
JSValue pointer_event_proto(JSContext *ctx);

/* DOES THIS OBJECT CARRY PointerEvent's OWN SLOT RECORD — Web IDL §3.2.15 "Interface types"' `I` for this
 * interface, stated as a PREDICATE because no class id can state it. Every Event subclass in this engine has a
 * class that NOTHING WEARS: the class exists for its per-realm prototype slot and every event is minted through
 * core/events/event.c's event_make_proto (JS_NewObjectProto), so `JS_GetClassID` answers the same for a
 * PointerEvent, a MouseEvent and a plain Event. `sequence<PointerEvent>` therefore cannot be branded by a class
 * at all, which is why PointerEventInit's two sequence members carry `IdlDictMember::iface_is` and why the
 * dictionary walk resolves a sequence element through idl_member_implements rather than through the
 * declaration's class.
 * IT IS THE SLOT RECORD AND NOT THE PROTOTYPE, so it stays true for an interface that inherits this one — the
 * same rule mouse_event_is states, one level down. */
bool pointer_event_is_value(JSContext *ctx, JSValueConst v);

/* HTML §8.1.8.3 "Event firing"'s FIRE A SYNTHETIC POINTER EVENT, minus the dispatch — steps 1-8 build the
 * event and step 9 dispatches it, and the caller is the dispatch. `view` is step 7's ("target's node document's
 * Window object, if any, and null otherwise"), so JS_NULL is a POSITIVE answer and not an unknown. The not
 * trusted flag is always set here because the only caller is HTML §6.5 "Activation behavior of elements"'
 * `click()`, whose step 4 is "Fire a synthetic pointer event named click at this element, with the not trusted
 * flag set".
 *
 * IT LIVES HERE AND NOT IN mouse_event.c BECAUSE STEP 1 NAMES THIS INTERFACE — "Let event be the result of
 * creating an event using PointerEvent". It was a MouseEvent for as long as this engine had no PointerEvent,
 * under a named residual in mouse_event.h that said so; moving it makes the base component stop depending on
 * the derived one, which is the direction the dependency has to run.
 * WHAT DID NOT CHANGE IS DOM §2.9 step 6.4's ACTIVATION BRAND. Its question is "event is a MouseEvent object",
 * and mouse_event_is answers off the MouseEvent slot record this event still carries — PointerEvent inherits
 * MouseEvent and is built through mouse_event_new_derived — so a synthetic click is an activation event exactly
 * as it was.
 *
 * THE RETIRED RESIDUAL WAS RIGHT ABOUT THE SPEC AND WRONG ABOUT THE BROWSER, AND THAT IS RECORDED HERE BECAUSE
 * IT IS THE PART A READER WOULD OTHERWISE COPY. Its HOW-ITS-ABSENCE-SHOWS clause said a `click` listener
 * reading `event.pointerType` gets `undefined` where, in its own words, `a browser answers "mouse"`.
 * Real Chrome answers the EMPTY STRING, and `pointerId` -1: measured on the live harness, one construction,
 * `el.click()` with a listener reading the event back. Nothing in HTML §8.1.8.3 sets either member, so ""
 * is Pointer Events 4 §3.1's own un-initialized value and -1 is its own reserved value for an event no
 * pointing device generated. CLAUDE.md rates a residual's next-diff and absence-shows clauses as the pair
 * most worth checking, and this one's spec half was exactly right while its browser half had never been run.
 * Returns a new owned PointerEvent, or JS_EXCEPTION with the throw live. */
JSValue pointer_event_new_synthetic(JSContext *ctx, const char *type, JSValueConst view);

#endif
