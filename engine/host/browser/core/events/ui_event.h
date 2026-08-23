/* UIEvent — UI Events §3.2.1, and the two dictionaries its subclasses share. See ui_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_UI_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_UI_EVENT_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "core/idl_args.h"

void ui_event_init(JSContext *ctx);            /* the slot key + the IDL declarations (agent init) */
void ui_event_install_protos(JSContext *ctx);  /* §3.7: one prototype AND one interface object per REALM */
void ui_event_free(JSContext *ctx);
/* `UIEvent.prototype` for this realm — what a DERIVED interface (MouseEvent, KeyboardEvent) chains to.
   OWNED: the caller frees. */
JSValue ui_event_proto(JSContext *ctx);

/* DOM §2.5's CREATE AN EVENT using UIEvent: every attribute at its un-initialized value. §4.5's createEvent is
   the caller, and there is no second way to make one without a dictionary. */
JSValue ui_event_new(JSContext *ctx);

/* §3.2.1's constructor steps with a DERIVED interface's prototype — the Event half and the UIEvent half of
   every interface that inherits this one, so a subclass's constructor adds only its OWN slots. `init` is the
   CONVERTED init dictionary (JS_UNDEFINED for an un-initialized event: Web IDL §3.2.17 makes every member of an absent
   dictionary absent, which is exactly the un-initialized value of each attribute). `proto` is CONSUMED,
   exactly as event_new_derived's is and for the same reason — every caller gets it from an owned
   `<Interface>_proto(ctx)`. Returns JS_EXCEPTION with the throw live. */
JSValue ui_event_new_derived(JSContext *ctx, JSValue proto, JSValueConst type, JSValueConst init, bool trusted);

/* Does this object carry UIEvent's own slot record — the brand a derived interface's members share. */
bool ui_event_is(JSContext *ctx, JSValueConst v);

/* §3.2.1's `Window?` CONVERSION, over `view` and over every legacy initializer's `viewArg`. null and undefined
   are the IDL null, a Window crosses as itself, and anything else is Web IDL's own TypeError. It is public
   because it is a CONVERSION and not an algorithm step: Web IDL performs it before the initializer's first
   step, so an initializer whose other arguments also need converting has to be able to order the two.
   Answers JS_NULL, an owned dup, or JS_EXCEPTION with the TypeError live. */
JSValue ui_event_view_of(JSContext *ctx, JSValueConst v);

/* §3.2.1's `view` AS THE REALM IT NAMES — the answer to "the event's associated Window object, IF THERE IS ONE"
 * that CSSOM VIEW §10's `pageX`/`pageY` step 2 asks, resolved to the JSContext every per-realm fact about that
 * Window is answered out of. NULL is the "or zero otherwise" arm and is a POSITIVE statement: the IDL's
 * `Window? view = null` means an event constructed without one has no Window, not that its Window is unknown.
 *
 * IT IS HERE RATHER THAN AT THE MEMBER THAT NEEDS IT because `view` is THIS interface's slot and the mapping
 * from a Window value to its realm is this interface's business twice over: `ui_event_view_of` is what put
 * either shape in the slot, so the two functions are the two halves of one type and a second reader that
 * re-derived the mapping could disagree with the writer about which shapes are admissible.
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
   return. `view` is CONSUMED (pass what ui_event_view_of answered).
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
   members at level 3, lexicographically. */
#define UI_EVENT_INIT_MEMBERS                                                              \
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN }, \
    { "detail", IDL_LONG, false, NULL, 1 }, { "view", IDL_ANY, false, NULL, 1 },            \
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

/* A `long` / `unsigned long` member of a dictionary the declaration has ALREADY converted — the read back out
   of the record it built, which runs none of the page's code because nothing of the page's is on that record.
   An absent member is the IDL's `= 0` default, which is also every one of these attributes' un-initialized
   value. Shared by the three interfaces because all three read integer members the same way. */
int32_t  ui_event_dict_i32(JSContext *ctx, JSValueConst init, const char *name);
uint32_t ui_event_dict_u32(JSContext *ctx, JSValueConst init, const char *name);
/* A `double` member, read the same way and for the same reason — Pointer Lock 2.0 §7's `movementX`/`movementY`
   are `double` where §"Interface MouseEvent"'s coordinates are `long`, and the type a member is DECLARED with
   is what the conversion already ran, so the read back out must not narrow it a second time. */
double   ui_event_dict_f64(JSContext *ctx, JSValueConst init, const char *name);

#endif
