/* MouseEvent — Pointer Events 4 "Interface MouseEvent" (UI Events hands the interface to it). See
   mouse_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_MOUSE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_MOUSE_EVENT_H
#include <stdbool.h>

#include "quickjs.h"
#include "core/idl_args.h"
/* MOUSE_EVENT_INIT_MEMBERS names `event_target_is_value` as `relatedTarget`'s §3.2.15 predicate, so the macro
   carries its own declaration rather than relying on each splicing file to have included it — the same reason
   ui_event.h includes core/frame/window_proxy.h for UI_EVENT_INIT_MEMBERS' `view`. */
#include "core/events/event_target.h"

void mouse_event_init(JSContext *ctx);             /* the slot key + the IDL declarations (agent init) */
void mouse_event_install_protos(JSContext *ctx);   /* §3.7: one prototype AND one interface object per REALM */
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private
   Symbol, a class id and this interface's member declarations — and every prototype it built is in
   some realm's class-proto slot and goes with that realm. Reached from core/events/event.c's
   event_free_subclasses, which is core/platform.c's `event` row. */
void mouse_event_free(JSRuntime *rt);
/* `MouseEvent.prototype` for this realm — what a derived interface (PointerEvent, WheelEvent, DragEvent)
   chains to. OWNED: the caller frees. */
JSValue mouse_event_proto(JSContext *ctx);

/* DOM §2.5's CREATE AN EVENT using MouseEvent: every attribute at its un-initialized value. §4.5's createEvent
   is the caller. */
JSValue mouse_event_new(JSContext *ctx);

/* IS THIS OBJECT A MouseEvent — DOM §2.9 step 6.4's own question ("event is a MouseEvent object and event's
   type attribute is \"click\""), and the brand this interface's members check. It is the slot record and not the class,
   so it stays true across an interface that inherits this one. */
bool mouse_event_is(JSContext *ctx, JSValueConst v);

/* DOM §2.5 "Constructing events" with a DERIVED interface's prototype — the Event, UIEvent and MouseEvent
   halves of every interface that inherits this one, so a subclass's constructor adds only its OWN slots. It is
   ui_event.h's `ui_event_new_derived` one level down and takes its arguments the same way: `init` is the
   CONVERTED init dictionary (JS_UNDEFINED for an un-initialized event), `proto` is CONSUMED, and the return is
   a new owned event or JS_EXCEPTION with the throw live.
   PointerEvent is its first outside caller and DragEvent and WheelEvent are the next two — each is a
   `: MouseEvent` whose own dictionary SPLICES the macro below, so the halves this builds and the members that
   list declares are one statement of MouseEventInit rather than two. */
JSValue mouse_event_new_derived(JSContext *ctx, JSValue proto, JSValueConst type, JSValueConst init,
                                bool trusted);

/* MouseEventInit's OWN members — Pointer Events 4 §11.1 "MouseEvent interface"'s dictionary at level 3, plus
   Pointer Lock 2.0 §7 "Extensions to the MouseEventInit Dictionary"'s two, SPLICED into each derived
   dictionary's list rather than written twice. It is UI_EVENT_INIT_MEMBERS one level down and exists for the
   identical reason: Web IDL §3.2.17 "Dictionary types" reads the INHERITED members first and each dictionary's
   own lexicographically among THEMSELVES, so a derived dictionary that restated these could state them in a
   different order or at a different type and no instrument here would see the two disagree.
   THE LEVEL IS 3 AND IT IS NOT A PARAMETER: every dictionary that splices this derives through the same chain
   (`MouseEventInit : EventModifierInit : UIEventInit : EventInit`), which is what makes a spliceable macro
   correct at all — a dictionary reaching MouseEventInit by a different depth would need different numbers and
   must not use this. A derived dictionary appends its OWN members at level 4. */
#define MOUSE_EVENT_INIT_MEMBERS                                                                          \
    { "button", IDL_UNSIGNED_SHORT, false, NULL, 3 }, { "buttons", IDL_UNSIGNED_SHORT, false, NULL, 3 },  \
    { "clientX", IDL_LONG, false, NULL, 3 }, { "clientY", IDL_LONG, false, NULL, 3 },                     \
    /* Pointer Lock 2.0 §7's two members. A PARTIAL DICTIONARY's members are members of the dictionary       \
       itself, so they sort among MouseEventInit's OWN at level 3 — between `clientY` and `relatedTarget` —  \
       and not in a block of their own after them. Which spec contributed a member is not something          \
       §3.2.17's read order can see. */                                                                   \
    { "movementX", IDL_DOUBLE, false, NULL, 3 }, { "movementY", IDL_DOUBLE, false, NULL, 3 },             \
    /* Pointer Events 4 §11.1 "MouseEvent interface": `EventTarget? relatedTarget = null`. §3.2.15's `I` is  \
       this member's own PREDICATE and not a class — EventTarget is implemented by every node wrapper, by a  \
       Window and by an XMLHttpRequest, so no JSClassID names it and the test is a walk to THIS realm's       \
       EventTarget.prototype (core/events/event_target.h).                                                   \
       THIS ROW IS WHERE THE TYPE'S READ ORDER BECOMES OBSERVABLE, which is why the member had to move off    \
       IDL_ANY rather than keep a body-side check: `screenX` and `screenY` sort AFTER it among this           \
       dictionary's own members, so with the brand in the body                                               \
       `new MouseEvent("m", {relatedTarget: 42, get screenX(){ throw new Error("ran"); }})` ran that getter — \
       §3.2.17 step 4.1.3.1's Get for a member a browser never reaches, because its step 4.1.4.1 threw one    \
       member earlier. */                                                                                  \
    { "relatedTarget", IDL_INTERFACE_NULLABLE, false, NULL, 3, NULL, IDL_DEFAULT_NULL,                    \
      .iface_is = event_target_is_value, .iface_name = "EventTarget" },                                   \
    { "screenX", IDL_LONG, false, NULL, 3 }, { "screenY", IDL_LONG, false, NULL, 3 }

#endif
