/* UIEvent — UI Events §3.2.1, and the two dictionaries its subclasses share. See ui_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_UI_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_UI_EVENT_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "core/idl_args.h"
/* UI_EVENT_INIT_MEMBERS names `window_proxy_is_window` as `view`'s §3.2.15 predicate, so the macro carries its
   own declaration rather than relying on each splicing file to have included it — a fifth dictionary that
   spliced the macro without this include would fail to compile, which is the good failure, but only because
   the name is a function and not a macro. The include is what makes the macro self-contained. */
#include "core/frame/window_proxy.h"

void ui_event_init(JSContext *ctx);            /* the slot key + the IDL declarations (agent init) */
void ui_event_install_protos(JSContext *ctx);  /* §3.7: one prototype AND one interface object per REALM */
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private
   Symbol, a class id and this interface's member declarations — and every prototype it built is in
   some realm's class-proto slot and goes with that realm. Reached from core/events/event.c's
   event_free_subclasses, which is core/platform.c's `event` row. */
void ui_event_free(JSRuntime *rt);
/* `UIEvent.prototype` for this realm — what a DERIVED interface (MouseEvent, KeyboardEvent) chains to.
   OWNED: the caller frees. */
JSValue ui_event_proto(JSContext *ctx);

/* DOM §2.5's CREATE AN EVENT using UIEvent: every attribute at its un-initialized value. §4.5's createEvent is
   the caller, and there is no second way to make one without a dictionary. */
JSValue ui_event_new(JSContext *ctx);

/* DOM §2.5 "Constructing events" with a DERIVED interface's prototype — the Event half and the UIEvent half of
   every interface that inherits this one, so a subclass's constructor adds only its OWN slots. `init` is the
   CONVERTED init dictionary (JS_UNDEFINED for an un-initialized event: Web IDL §3.2.17 makes every member of an absent
   dictionary absent, which is exactly the un-initialized value of each attribute). `proto` is CONSUMED,
   exactly as event_new_derived's is and for the same reason — every caller gets it from an owned
   `<Interface>_proto(ctx)`. Returns JS_EXCEPTION with the throw live. */
JSValue ui_event_new_derived(JSContext *ctx, JSValue proto, JSValueConst type, JSValueConst init, bool trusted);

/* Does this object carry UIEvent's own slot record — the brand a derived interface's members share. */
bool ui_event_is(JSContext *ctx, JSValueConst v);

/* §3.2.1's `view` AS THE REALM IT NAMES — the answer to "the event's associated Window object, IF THERE IS ONE"
 * that CSSOM VIEW §10's `pageX`/`pageY` step 2 asks, resolved to the JSContext every per-realm fact about that
 * Window is answered out of. NULL is the "or zero otherwise" arm and is a POSITIVE statement: the IDL's
 * `Window? view = null` means an event constructed without one has no Window, not that its Window is unknown.
 *
 * IT IS HERE RATHER THAN AT THE MEMBER THAT NEEDS IT because `view` is THIS interface's slot, and the two
 * shapes that can be IN that slot are decided by ONE statement of `Window`: `window_proxy_is_window`, which is
 * what UIEventInit's `view` row brands with (IdlDictMember::iface_is), what the three legacy initializers'
 * `viewArg` positions brand with (idl_arg_iface), and what the second arm below asks again. A reader that
 * re-derived which shapes are admissible could disagree with the type that admitted them.
 *
 * THE VIEWPORT IS PER REALM AND THAT IS THE WHOLE POINT — a child navigable's is 300 CSS pixels wide and the
 * top-level traversable's is 1280 (core/frame/viewport.h) — so `new MouseEvent('m', {view: frame.contentWindow})`
 * must be answered out of the FRAME's realm however the getter was reached. A member that read the running
 * realm instead would be the module-static defect CLAUDE.md names, with the wrong answer sourced from whichever
 * realm's prototype the call happened to go through. */
JSContext *ui_event_view_realm(JSContext *ctx, JSValueConst ev);

/* §6.1.1's initUIEvent, AS THE PREFIX EVERY LEGACY INITIALIZER SHARES. All three of them — initUIEvent,
   Pointer Events 4's initMouseEvent and §6.1.2's initKeyboardEvent — begin with the same four arguments and
   the same sentence ("this method has the same behavior as initEvent()", then `view`), so it is one
   implementation rather than the same four lines written in three files with three chances to drop the early
   return. `view` is CONSUMED — pass the `viewArg` position the DECLARATION converted, dup'd; it is the IDL
   null or a Window and its `Window?` brand is idl_arg_iface's, never a check the caller repeats.
   Answers FALSE when the event's DISPATCH FLAG is set: §2.2's initialise-an-existing-event returns early, and
   a derived initializer must honour that before writing a single slot of its own. */
bool ui_event_reinit(JSContext *ctx, JSValueConst ev, JSValueConst type, bool bubbles, bool cancelable,
                     JSValue view);

/* §3.2.1's `detail`, written by an initializer whose argument list HAS a detailArg — initUIEvent's and
   initMouseEvent's do; initKeyboardEvent's does not, and §6.1.2 says so in as many words ("the value of detail
   remains undefined"), which is why this is a call the member makes rather than part of the prefix above. */
void ui_event_set_detail(JSContext *ctx, JSValueConst ev, int32_t detail);

/* THE INTERNAL KEY MODIFIER STATE, WRITTEN. The legacy initializers take four booleans each, and §6.1.2 states
   every one of them as "specifies whether the <X> key modifier is active" — so a FALSE is as much a statement
   as a true and clears the modifier, which is why this takes the flag rather than only adding names. */
void ui_event_set_modifier_state(JSContext *ctx, JSValueConst ev, const char *name, bool on);

/* THE INTERNAL KEY MODIFIER STATE — Pointer Events 4 "Constructing Mouse Events": a set of key modifier names,
   set from EventModifierInit and queried by getModifierState(). It lives with the dictionary that fills it
   rather than with either interface that declares the member, because BOTH declare it over the same state.
   `ui_event_modifier_state` is what MouseEvent's and KeyboardEvent's ctrlKey/shiftKey/altKey/metaKey answer
   from: §3.5.3 states each of those four dictionary members as "initializes the attribute AND the key modifier
   state", so the attribute and the query are one fact and there is one place holding it.
   `ui_event_get_modifier_state` is the member's body, shared by the two interfaces that declare it; the caller
   has already brand-checked its own interface, which is what makes
   `MouseEvent.prototype.getModifierState.call(new KeyboardEvent('k'), 'Shift')` the TypeError it is. */
bool    ui_event_modifier_state(JSContext *ctx, JSValueConst ev, const char *name);
JSValue ui_event_get_modifier_state(JSContext *ctx, JSValueConst ev, JSValueConst key_arg);

/* THE SHARED DICTIONARY LEVELS, SPLICED INTO EACH DERIVED DICTIONARY'S LIST RATHER THAN WRITTEN TWICE.
   Web IDL §3.2.17 reads the INHERITED members first and each dictionary's own lexicographically among themselves, so a
   member's LEVEL is its inheritance depth — and both dictionaries that derive from EventModifierInit derive
   through the same chain (`MouseEventInit : EventModifierInit : UIEventInit : EventInit`, and KeyboardEventInit
   likewise), which is why the levels below are the same numbers in both. A derived dictionary appends its own
   members at level 3, lexicographically.

   INPUT DEVICE CAPABILITIES' `InputDeviceCapabilities? sourceCapabilities = null` IS ONE OF UIEventInit's OWN
   MEMBERS AND SORTS AMONG THEM, between `detail` and `view` — §3.2.17's lexicographic order is over the
   dictionary's members and knows nothing about which specification wrote each one.

   TWO OF THESE MEMBERS HAVE AN INTERFACE TYPE AND THEY STATE §3.2.15's `I` IN THE TWO DIFFERENT SPELLINGS,
   which is the whole of why one is written on the row and the other is not.
     - `sourceCapabilities` is one CLASS, so it is branded once per DECLARATION with
       `idl_iface_brand(input_device_capabilities_class())` — which every constructor that splices this macro in
       must state, and a declaration that forgets brands against class zero, which the conversion asserts on.
     - `Window? view = null` is not. `Window` is a realm's own global OR a WindowProxy (core/frame/window_proxy.h
       states why those are one type test), so no JSClassID names it and the question takes a JSContext. The row
       therefore carries `IdlDictMember::iface_is` — §3.2.15's `I` as a PREDICATE — and idl_member_implements
       takes it in preference to the declaration's class, so the two never decide the same member.
     The predicate is the SAME one the three legacy initializers state at their `viewArg` position with
     idl_arg_iface, so a `view` written through a constructor and a `viewArg` passed to initUIEvent are one
     test, and this member no longer reaches a body unconverted for the body to brand by hand — which ran
     `which`'s getter for `new UIEvent("x", {view: 42, get which(){ throw new Error("ran"); }})`, one member
     past the §3.2.17 step 4.1.4.1 conversion a browser throws at. */
#define UI_EVENT_INIT_MEMBERS                                                                             \
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },                \
    { "detail", IDL_LONG, false, NULL, 1 },                                                               \
    { "sourceCapabilities", IDL_INTERFACE_NULLABLE, false, NULL, 1, NULL, IDL_DEFAULT_NULL },             \
    { "view", IDL_INTERFACE_NULLABLE, false, NULL, 1, NULL, IDL_DEFAULT_NULL,                             \
      .iface_is = window_proxy_is_window, .iface_name = "Window" },                                        \
    { "which", IDL_UNSIGNED_LONG, false, NULL, 1 }
/* §3.5.3's fourteen: the four named for the attribute they also initialize, and the ten `modifier<Name>` ones
   whose key modifier name is the member's name minus that prefix. */
#define EVENT_MODIFIER_INIT_MEMBERS                                                                   \
    { "altKey", IDL_BOOLEAN, false, NULL, 2 }, { "ctrlKey", IDL_BOOLEAN, false, NULL, 2 },            \
    { "metaKey", IDL_BOOLEAN, false, NULL, 2 },                                                       \
    { "modifierAltGraph", IDL_BOOLEAN, false, NULL, 2 },                                              \
    { "modifierCapsLock", IDL_BOOLEAN, false, NULL, 2 },                                              \
    { "modifierFn", IDL_BOOLEAN, false, NULL, 2 },                                                    \
    { "modifierFnLock", IDL_BOOLEAN, false, NULL, 2 },                                                \
    { "modifierHyper", IDL_BOOLEAN, false, NULL, 2 },                                                 \
    { "modifierNumLock", IDL_BOOLEAN, false, NULL, 2 },                                               \
    { "modifierScrollLock", IDL_BOOLEAN, false, NULL, 2 },                                            \
    { "modifierSuper", IDL_BOOLEAN, false, NULL, 2 },                                                 \
    { "modifierSymbol", IDL_BOOLEAN, false, NULL, 2 },                                                \
    { "modifierSymbolLock", IDL_BOOLEAN, false, NULL, 2 },                                            \
    { "shiftKey", IDL_BOOLEAN, false, NULL, 2 }

/* AN INTEGER OR `double` MEMBER OF A DICTIONARY THE DECLARATION HAS ALREADY CONVERTED, HANDED BACK AS A C
   SCALAR — the read back out of the record it built, which runs none of the page's code because nothing of the
   page's is on that record, and which must not narrow the value a second time: the type a member is DECLARED
   with is what the conversion already ran. An absent member reads as zero.
   THEY ARE FOR A CALLER THAT DOES ARITHMETIC AND FOR NO OTHER, which is what separates them from
   ui_event_dict_num below and is a fact about the CALLER rather than about the member — `button` and the four
   Pointer Events 4 §3.1.5 orientation members are read here, and every member that is only PLACED on a slot
   is read there. A caller that asks for a scalar is asking ECMAScript §7.1.4 ToNumber ( arg ) a question it
   refuses over unknown external input, so these three are the readers a crossed member CANNOT survive; the
   two sites that still read a placed member this way say at their own lines why, and each is a residual with
   the work it is waiting on named. */
int32_t  ui_event_dict_i32(JSContext *ctx, JSValueConst init, const char *name);
uint32_t ui_event_dict_u32(JSContext *ctx, JSValueConst init, const char *name);
double   ui_event_dict_f64(JSContext *ctx, JSValueConst init, const char *name);

/* THE SAME MEMBERS READ TO BE STORED RATHER THAN TO BE COMPUTED WITH, AND IT IS A SECOND QUESTION OVER ONE
   FACT rather than a second answer to the first. The three readers above hand C a scalar because their
   callers do ARITHMETIC with one — Web IDL §3.2.4.3 "short"'s signed fold for `button`, and Pointer Events 4
   §3.1.5's tilt-to-spherical conversion. A caller that only PLACES the member on the slot record its
   attribute reads back wants no number at all: it wants the VALUE, and asking such a caller's question with a
   scalar is what DELETES THE FORK.
   THE CHAIN IS THE CROSSING'S AND ENDS AT A BOUNDARY THAT OWES C A REAL PRIMITIVE. core/idl_args.h's
   idl_concolic_rule answers IDL_CONCOLIC_CROSSES for every numeric type, so §3.2.17 "Dictionary types"'s member
   loop rewrites an unknown-valued member to IDL_ANY before any type arm is asked and PLACES IT AS ITSELF —
   deliberately, so opacity survives the boundary. The scalar readers then take that value to ECMAScript
   §7.1.4 ToNumber ( arg ), which refuses an unknown BY NAME (quickjs.c's JS_ToNumberHintFree: a DFAIL naming
   the coercing site in dev, a TypeError in release). Handing the value back instead is what lets
   `if (ev.deltaY > 0)` reach 13.5.4's own concolic hook and FORK, which is the whole of why the member loop
   crosses.
   `uninitialized` IS THE ATTRIBUTE'S OWN SPEC FACT AND IS WRITTEN AT THE CALL, in the representation that
   attribute has — Pointer Events 4 §3.1's `1` for `width`/`height`, Pointer Events 4 §12.1's `0.0` for the three
   deltas, the `= 0` every other one of these attributes is un-initialized to. It cannot be derived from the
   declared type here, because the dictionary's default and the attribute's un-initialized value are two
   requirements of one standard that only happen to agree on most members. It is TAKEN OVER: returned for an
   absent member and freed for a present one.
   IT IS A MACRO OVER AN `_at` ENTRY for the reason core/idl_args.h states at idl_dict_bool: one assert under
   every numeric member of five interfaces can state the RULE and could not state the ADDRESS, and one member
   name (`width`, `which`) is declared by more than one dictionary, so only the pair of the name and the SITE
   says which read refused. */
JSValue  ui_event_dict_num_at(JSContext *ctx, JSValueConst init, const char *name, JSValue uninitialized,
                              const char *file, int line);
#define ui_event_dict_num(ctx, init, name, uninitialized) \
    ui_event_dict_num_at((ctx), (init), (name), (uninitialized), __FILE__, __LINE__)

#endif
