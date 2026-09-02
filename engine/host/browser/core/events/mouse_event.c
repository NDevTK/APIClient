/* THE MouseEvent INTERFACE — Pointer Events 4 §11.1 MouseEvent interface. The number is the published
 * Level 4 Recommendation's; the editor's draft carries the same heading unnumbered, and the TITLE is
 * what survives an edition the number does not. The citation here used to be the title reversed and
 * with no number at all.
 *
 * WHICH SPEC, AND WHY IT IS NOT UI EVENTS. MouseEvent used to live in UI Events; it does not any more. The
 * current UI Events draft defines UIEvent, FocusEvent, InputEvent, KeyboardEvent, CompositionEvent and
 * TextEvent, names no MouseEvent IDL at all, and lists under "[POINTEREVENTS4] defines the following terms:
 * MouseEvent, WheelEvent, button, click, screenX, screenY, getModifierState()". Its remaining MouseEvent
 * prose defers outright — §3.5.3: "The steps for constructing mouse events using this dictionary are defined
 * in the [pointerevents4] specification." So the IDL implemented here is Pointer Events 4's:
 *
 *     [Exposed=Window]
 *     interface MouseEvent : UIEvent {
 *       constructor(DOMString type, optional MouseEventInit eventInitDict = {});
 *       readonly attribute long screenX;  readonly attribute long screenY;
 *       readonly attribute long clientX;  readonly attribute long clientY;
 *       readonly attribute long layerX;   readonly attribute long layerY;
 *       readonly attribute boolean ctrlKey;  readonly attribute boolean shiftKey;
 *       readonly attribute boolean altKey;   readonly attribute boolean metaKey;
 *       readonly attribute short button;     readonly attribute unsigned short buttons;
 *       readonly attribute EventTarget? relatedTarget;
 *       boolean getModifierState(DOMString keyArg);
 *     };
 *     dictionary MouseEventInit : EventModifierInit {
 *       long screenX = 0; long screenY = 0; long clientX = 0; long clientY = 0;
 *       short button = 0; unsigned short buttons = 0; EventTarget? relatedTarget = null;
 *     };
 *
 * WHAT WAS BLOCKED ON IT. DOM §2.9 step 6.4 decides an ACTIVATION event by "event is a MouseEvent object and
 * event's type attribute is \"click\"" — a question about the OBJECT, which event_target.c could only ask of the type
 * string, so any `new Event('click')` was an activation event and `new MouseEvent('click')` was a missing
 * global. §4.5's createEvent had to refuse `mouseevent`/`mouseevents`, which is how a great deal of shipped
 * code synthesises a click. mouse_event_is is the answer to step 6.4's real question.
 *
 * THE COORDINATES COME FROM THE INIT DICTIONARY, WHICH IS THE IDL AND NOT A HEADLESS COMPROMISE. `screenX` is
 * a `long` that "MUST" be 0 un-initialized and is otherwise whatever MouseEventInit's `screenX` said. There is
 * nothing to sense and nothing to invent: a constructed event's coordinates are the ones it was constructed
 * with, in a browser with a mouse exactly as here.
 *
 * AND THE COORDINATE FAMILY IS NOT ALL ONE SPEC'S, WHICH IS THE PART THAT DECIDES WHERE EACH ONE IS DERIVED
 * FROM. Three standards put attributes on this interface and each names a different source of truth:
 *
 *   CSSOM VIEW §10 "Extensions to the MouseEvent Interface" adds `pageX`, `pageY`, `x`, `y`, `offsetX` and
 *   `offsetY` — all six DERIVED, none of them stored, and each stated as an algorithm over values this object
 *   or the viewport already has. It redefines `screenX`/`screenY`/`clientX`/`clientY` as `double` where
 *   Pointer Events 4 declares them `long`, and flags that clash as an OPEN ISSUE in its own text ("the object
 *   IDL fragment redefines some members. Can we resolve this somehow?"), so the declaration above is left as
 *   the one this file has always made and the divergence is a question for the specs rather than a fact to
 *   pick a side of here.
 *
 *   POINTER LOCK 2.0 §6 "Extensions to the MouseEvent Interface" adds `movementX` and `movementY`, and its §7
 *   "Extensions to the MouseEventInit Dictionary" adds `double movementX = 0` / `double movementY = 0`. They
 *   are NOT CSSOM VIEW's and putting them there would be a citation that sends the next reader to a section
 *   which does not mention them. They are STORED, like `clientX`: §6 states "the un-initialized value of
 *   movementX and movementY must be 0" and §7 declares the members that override it, so this engine answers
 *   them exactly as a browser with a mouse does for a constructed event — the deltas a real pointer would have
 *   produced are the ones the constructor was given, and there is no device in the derivation either way.
 *
 *   INPUT DEVICE CAPABILITIES adds `sourceCapabilities` to UIEvent, not to this interface, so a MouseEvent
 *   answers it out of ui_event.c's slot and MouseEventInit carries the member because it INHERITS
 *   UIEventInit. What this file owes it is the BRAND: the class an `InputDeviceCapabilities?` member tests
 *   against is stated once per DECLARATION, so MouseEventInit's declaration states it too.
 *
 * WHY `pageX` HAS ONE EXPRESSION AND NOT TWO BRANCHES. §10 writes it as three steps: with the dispatch flag
 * set, the coordinate of the position where the event occurred "relative to the origin of the INITIAL
 * CONTAINING BLOCK"; otherwise `scrollX` plus `clientX`. Those are the SAME NUMBER by CSSOM VIEW's own two
 * definitions and not by anything this engine models: §10 defines `clientX` as that same position "relative to
 * the origin of the VIEWPORT", and §4 defines `scrollX` as "the x-coordinate, RELATIVE TO THE INITIAL
 * CONTAINING BLOCK ORIGIN, of the left of the viewport" — which is precisely the vector between the two
 * origins. So step 1 is `clientX + scrollX` written in the other frame, the split exists because a real user
 * agent has a hit-test result at dispatch time that is more precise than the `long` it reported, and an `if`
 * over the dispatch flag here would be two spellings of one derivation with one chance to disagree.
 *
 * `offsetX` AND `offsetY` ARE THE TWO OF §10's SIX THAT ARE NOT HERE, and the reason is a POSITION and not a
 * policy. Their step 2 is `pageX`, which this file now has; their step 1, taken whenever the dispatch flag is
 * set — which is every read inside a listener, and therefore every read a library makes — is the position
 * "relative to the origin of the PADDING EDGE of the target node". That needs three things this engine does
 * not yet put in one place: DOM §2.9's `target` read out to C (event.c stores it and exposes no getter),
 * core/layout/flow_position.h's border-box origin for the target's box (which places in-flow block-level boxes
 * and crashes by name for a float, an out-of-flow box and an INLINE one — the last being most of what a page
 * clicks), and §8.1's leading border to reach the padding edge from it. And §10 states no answer at all for a
 * target that generates no box — a Document, a Window, a `display: none` element — which is a gap in the spec
 * and not in this engine, so it is a question to settle before a member is installed rather than after.
 *
 * `layerX`/`layerY` HAVE NO INIT MEMBER — the IDL declares the attributes and no dictionary member for them,
 * so their value is the un-initialized 0 for every event that is not produced by a hit-test against a laid-out
 * box. They are slots like every other attribute, written at creation, so the native-input path writes them
 * where it writes screenX rather than this file growing a special case.
 *
 * THREE PIECES OF STATE ARE NOT THIS INTERFACE'S, AND SAYING SO IS THE DESIGN.
 *   - `relatedTarget` is the EVENT'S. DOM §2.2 gives every Event an associated relatedTarget and says other
 *     specifications only "define a relatedTarget attribute" over it — which is why §2.9 step 4 RETARGETS it
 *     at every path item without knowing this interface exists. So the attribute reads Event's slot; a slot
 *     of its own would be a second value that retargeting never reaches.
 *   - `ctrlKey`/`shiftKey`/`altKey`/`metaKey` and `getModifierState` are the shared INTERNAL KEY MODIFIER
 *     STATE that EventModifierInit fills — ui_event.c's, because KeyboardEvent declares the same members over
 *     the same state.
 *   - The Event and UIEvent halves are event_new_derived's and ui_event_new_derived's. This file adds eight
 *     slots and nothing else.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, for the reason event.c gives: a property write is
 * captured by the COW delta, so the event's state time-travels, and the symbol is a brand a page cannot forge.
 *
 * `initMouseEvent` is Pointer Events 4's §"Legacy Event Initializers", and it is the widest member the platform
 * has: FIFTEEN declared arguments. It is how a bundle old enough to use `document.createEvent('MouseEvent')`
 * finishes making one, so the factory row and this member are one feature — without it §4.5 leaves the
 * initialized flag unset and the event can never be dispatched. */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/events/input_device_capabilities.h"
#include "core/events/mouse_event.h"
#include "core/events/ui_event.h"
#include "core/frame/viewport.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's own slots hang off */
static JSClassID g_me_class;    /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;
static int       g_modifier_state_id = -1;
static int       g_init_mouse_id = -1;

JSValue mouse_event_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_me_class);

    DCHECK(!JS_IsNull(proto),
           "MouseEvent.prototype was asked for in a realm that never ran mouse_event_install_protos");
    return proto;   /* OWNED */
}

static JSValue md_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "a MouseEvent's slots were asked for before mouse_event_init ran");
    if (!JS_IsObject(ev))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &slots, ev, k) <= 0)   /* an own SLOT, never a lookup — see ui_event.c */
        slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return slots;
}

bool mouse_event_is(JSContext *ctx, JSValueConst v)
{
    JSValue slots = md_slots(ctx, v);
    bool ok = JS_IsObject(slots);

    JS_FreeValue(ctx, slots);
    return ok;
}

/* `short button` — Web IDL §3.2.4.3's conversion, whose FIRST half the declaration already performed. Every integer type
   in Web IDL is the same arithmetic modulo 2^width; `short` and `unsigned short` are one width and differ only
   in the final fold into the signed range, and the pool declares no `short`, so the member is declared with the
   width it has and folded here. The DCHECK is the contract that makes that exact: what arrives must already be
   the 16-bit modulo, so the day the pool gains the type this fold is deleted and nothing else moves. */
static int32_t md_button_fold(uint32_t u)
{
    DCHECK(u <= 0xFFFFu, "`button` reached its fold outside the 16-bit range the declared type produces — the "
                         "modulo is the declaration's half of Web IDL §3.2.4.3 and this is the signed fold that completes "
                         "it");
    return u >= 0x8000u ? (int32_t)u - 0x10000 : (int32_t)u;
}

static int32_t md_button_of(JSContext *ctx, JSValueConst init)
{
    return md_button_fold(ui_event_dict_u32(ctx, init, "button"));
}

/* AN ARGUMENT THE DECLARATION HAS ALREADY CONVERTED, read back out. An absent one is the IDL's `= 0` default,
   which is also the un-initialized value of every attribute `initMouseEvent` writes — so there is one number
   here and no second table of defaults. It runs none of the page's code: what arrives is a value the args
   machine produced, or unknown external input, which crosses every conversion as itself. */
static int32_t md_arg_i32(JSContext *ctx, int argc, JSValueConst *argv, int i)
{
    int32_t n = 0;

    if (i < argc && !JS_IsUndefined(argv[i]))
        JS_ToInt32(ctx, &n, argv[i]);
    return n;
}

static uint32_t md_arg_u32(JSContext *ctx, int argc, JSValueConst *argv, int i)
{
    uint32_t n = 0;

    if (i < argc && !JS_IsUndefined(argv[i]))
        JS_ToUint32(ctx, &n, argv[i]);
    return n;
}

static bool md_arg_bool(JSContext *ctx, int argc, JSValueConst *argv, int i)
{
    return i < argc && JS_ToBool(ctx, argv[i]);
}

/* The eight own slots, placed on an event whose Event and UIEvent halves are already built, plus the ninth
   value that is the EVENT'S: §2.2's relatedTarget. Returns -1 with the throw live. */
static int md_init_slots(JSContext *ctx, JSValueConst ev, JSValueConst init)
{
    JSValue slots, given, related;
    JSAtom k;

    DCHECK(g_ready, "a MouseEvent was minted before mouse_event_init declared the interface — the slot key it "
                    "hangs its state off is made there");
    slots = idl_slots_new(ctx);
    k = JS_ValueToAtom(ctx, g_key);
    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    /* `EventTarget? relatedTarget = null` — the type's own conversion, which is event_target.c's because the
       question it asks is "does this implement EventTarget", and because FocusEventInit declares the same
       member over the same §2.2 value. */
    given = idl_dict_get(ctx, init, "relatedTarget");
    related = event_target_nullable_of(ctx, given, "a MouseEvent's `relatedTarget`");
    JS_FreeValue(ctx, given);
    if (JS_IsException(related)) {
        JS_FreeValue(ctx, slots);
        JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "screenX", JS_NewInt32(ctx, ui_event_dict_i32(ctx, init, "screenX")));
    JS_SetPropertyStr(ctx, slots, "screenY", JS_NewInt32(ctx, ui_event_dict_i32(ctx, init, "screenY")));
    JS_SetPropertyStr(ctx, slots, "clientX", JS_NewInt32(ctx, ui_event_dict_i32(ctx, init, "clientX")));
    JS_SetPropertyStr(ctx, slots, "clientY", JS_NewInt32(ctx, ui_event_dict_i32(ctx, init, "clientY")));
    /* No dictionary member declares these two: the IDL gives them attributes and no initializer, so what is
       written is the un-initialized value the spec states for them. */
    JS_SetPropertyStr(ctx, slots, "layerX", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, slots, "layerY", JS_NewInt32(ctx, 0));
    /* Pointer Lock 2.0 §7's two `double` members over §6's two attributes, whose un-initialized value §6
       states as 0 — which is the dictionary's own default, so there is one number and no second table. */
    JS_SetPropertyStr(ctx, slots, "movementX",
                      JS_NewFloat64(ctx, ui_event_dict_f64(ctx, init, "movementX")));
    JS_SetPropertyStr(ctx, slots, "movementY",
                      JS_NewFloat64(ctx, ui_event_dict_f64(ctx, init, "movementY")));
    JS_SetPropertyStr(ctx, slots, "button", JS_NewInt32(ctx, md_button_of(ctx, init)));
    JS_SetPropertyStr(ctx, slots, "buttons", JS_NewUint32(ctx, ui_event_dict_u32(ctx, init, "buttons")));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    /* §2.2's associated relatedTarget, on the EVENT — see the file comment. */
    event_set_related_target(ctx, ev, related);
    JS_FreeValue(ctx, related);
    return 0;
}

static JSValue mouse_event_new_derived(JSContext *ctx, JSValue proto, JSValueConst type, JSValueConst init,
                                       bool trusted)
{
    JSValue ev = ui_event_new_derived(ctx, proto, type, init, trusted);

    if (JS_IsException(ev))
        return ev;
    if (md_init_slots(ctx, ev, init) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

JSValue mouse_event_new(JSContext *ctx)
{
    JSValue type = JS_NewString(ctx, ""), ev;

    if (JS_IsException(type))
        return type;
    /* DOM §2.5: every attribute at its un-initialized value, isTrusted true — which an ABSENT dictionary
       gives, member for member, so there is one construction path and no second table of defaults. */
    ev = mouse_event_new_derived(ctx, mouse_event_proto(ctx), type, JS_UNDEFINED, /*trusted*/ true);
    JS_FreeValue(ctx, type);
    return ev;
}

JSValue mouse_event_new_synthetic(JSContext *ctx, const char *type, JSValueConst view)
{
    JSValue init, t, ev;

    DCHECK(g_ready, "HTML §8.1.8.3 Event firing's fire a synthetic pointer event ran before mouse_event_init "
                    "declared the interface — the event it builds is a MouseEvent and its slot key is made there");
    DCHECK(type != NULL && *type,
           "HTML §8.1.8.3 Event firing's fire a synthetic pointer event was given no event name — step 2 "
           "initializes the type attribute to the name the caller fires, and there is no unnamed one");
    /* STEP 7's `view`, asserted rather than trusted: it is "target's node document's Window object, if any",
       so the two admissible values are a Window and null. Anything else is a caller that read the wrong
       object, and UIEvent's `Window?` conversion would report it as the page's TypeError instead. */
    DCHECK(JS_IsNull(view) || window_proxy_is_window(ctx, view),
           "HTML §8.1.8.3 Event firing's fire a synthetic pointer event was given a `view` that is neither a "
           "Window nor null — step 7 initializes it to the TARGET's node document's Window object, and null "
           "is the spec's own answer when that document has none");
    /* THE CONVERTED DICTIONARY, exactly as focus_event.c builds one for HTML §6.6.4's fire a focus event: a
       null-prototyped record carrying the members that EXIST, each already an engine value of its IDL type.
       Nothing of the page's is on it, so building it runs none of the page's code — and it is the same record
       `new MouseEvent(type, init)` reaches mouse_event_new_derived with, so a synthetic click and a
       constructed one are ONE construction path.
       Steps 3, 4 and 7 are these three members. HTML §8.1.8.3 Event firing's step 6 — "according to the
       current state of the key input device, if any (false for any keys that are not available)" — and its
       step 8's getModifierState are the un-initialized key modifier state ui_event.c writes for an absent
       dictionary member: false for every key, which is what a headless agent's key input device makes them
       and not a value invented here. */
    init = idl_slots_new(ctx);
    if (JS_IsException(init))
        return init;
    JS_SetPropertyStr(ctx, init, "bubbles", JS_TRUE);              /* step 3 */
    JS_SetPropertyStr(ctx, init, "cancelable", JS_TRUE);           /* step 3 */
    JS_SetPropertyStr(ctx, init, "composed", JS_TRUE);             /* step 4: "Set event's composed flag" */
    JS_SetPropertyStr(ctx, init, "view", JS_DupValue(ctx, view));  /* step 7 */
    t = JS_NewString(ctx, type);                                   /* step 2 */
    if (JS_IsException(t)) {
        JS_FreeValue(ctx, init);
        return t;
    }
    /* STEP 5: "If the not trusted flag is set, initialize event's isTrusted attribute to false." The one
       caller sets it, so the flag is not an argument — see the header for why widening it would be a
       parameter no step supplies. */
    ev = mouse_event_new_derived(ctx, mouse_event_proto(ctx), t, init, /*trusted*/ false);
    JS_FreeValue(ctx, t);
    JS_FreeValue(ctx, init);
    return ev;
}

/* ---- the attributes ----------------------------------------------------------------------------------------- */

enum { MD_SCREEN_X = 0, MD_SCREEN_Y, MD_CLIENT_X, MD_CLIENT_Y, MD_LAYER_X, MD_LAYER_Y, MD_BUTTON, MD_BUTTONS,
       MD_MOVEMENT_X, MD_MOVEMENT_Y };
static const char *const MD_SLOT[] = {
    "screenX", "screenY", "clientX", "clientY", "layerX", "layerY", "button", "buttons",
    "movementX", "movementY",
};

static JSValue js_md_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = md_slots(ctx, this_val), v;

    DCHECK(magic >= 0 && magic < (int)(sizeof(MD_SLOT) / sizeof(MD_SLOT[0])),
           "a MouseEvent attribute was declared with a magic the slot table does not name");
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a MouseEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, MD_SLOT[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* CSSOM VIEW §10's `pageX`/`pageY` — the ONE derivation their three steps state; see the file comment for why
   the dispatch-flag branch is not a second expression. `magic` is MD_CLIENT_X or MD_CLIENT_Y: the coordinate
   this member is the page-relative form OF, so the axis and the operand are ONE fact here rather than two that
   a later edit could pair the wrong way round. */
static JSValue js_md_get_page(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = md_slots(ctx, this_val), v;
    JSContext *view;
    double client = 0.0, offset;

    DCHECK(magic == MD_CLIENT_X || magic == MD_CLIENT_Y,
           "a §10 page coordinate was declared over a slot that is not one of the two client coordinates its "
           "steps add the scroll offset to");
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a MouseEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, MD_SLOT[magic]);
    JS_FreeValue(ctx, slots);
    /* TWO-SIDED, and it is the seam this member sits on. The sum below runs on the EXAMPLE, in C, which is
       right exactly while the slot holds a number the declaration computed. The day a concolic reaches this
       slot — the day MouseEventInit's coordinates carry unknown external input across their conversion — the
       sum must stop being C arithmetic and be minted through core/frame/viewport.h's one seam instead, or the
       domain is dropped at the `+`. This is where that day is noticed. */
    DCHECK(JS_IsNumber(v),
           "a MouseEvent's client coordinate slot holds something that is not a number — CSSOM VIEW §10's page "
           "coordinate sums it in C, so a value carrying a domain would be collapsed to its example here");
    JS_ToFloat64(ctx, &client, v);
    JS_FreeValue(ctx, v);
    /* "Let offset be the value of the scrollX attribute of the event's ASSOCIATED WINDOW OBJECT, if there is
       one, or zero otherwise." UI Events §3.2.1's `view` is that Window — it is the only Window a UIEvent is
       associated with — and the ATTRIBUTE is what is invoked, not the derivation under it, because §2 says an
       algorithm said to call an attribute must invoke its internal API. `viewport_window_scroll` is that
       attribute whole, including its own "or zero if there is no viewport", which is a DIFFERENT absence from
       this step's "if there is one" and must not be folded into it: an event with a `view` whose document is no
       longer presented has a Window and no viewport. */
    view = ui_event_view_realm(ctx, this_val);
    offset = view ? viewport_window_scroll(view, /*vertical*/ magic == MD_CLIENT_Y) : 0.0;
    return JS_NewFloat64(ctx, offset + client);
}

/* §2.2's relatedTarget, read off the EVENT — the value §2.9 step 4 retargets. */
static JSValue js_md_get_related_target(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!mouse_event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "MouseEvent.relatedTarget was read on something that is not one");
    return event_related_target(ctx, this_val);
}

/* The four named modifier attributes, over the shared internal key modifier state. magic indexes MD_MODIFIER,
   whose entries are the KEY MODIFIER NAMES §3.5.3 pairs each attribute with. */
enum { MD_CTRL = 0, MD_SHIFT, MD_ALT, MD_META };
static const char *const MD_MODIFIER[] = { "Control", "Shift", "Alt", "Meta" };

static JSValue js_md_get_modifier(JSContext *ctx, JSValueConst this_val, int magic)
{
    DCHECK(magic >= 0 && magic < (int)(sizeof(MD_MODIFIER) / sizeof(MD_MODIFIER[0])),
           "a MouseEvent modifier attribute was declared with a magic the modifier table does not name");
    if (!mouse_event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "a MouseEvent modifier attribute was read on something that is not one");
    return JS_NewBool(ctx, ui_event_modifier_state(ctx, this_val, MD_MODIFIER[magic]));
}

static JSValue js_md_get_modifier_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                        int magic)
{
    (void)magic;
    if (!mouse_event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "MouseEvent.getModifierState called on something that is not one");
    /* `keyArg` is not optional, so §3.6 step 5's check is the DECLARATION's and throws before this runs. */
    DCHECK(argc >= 1, "getModifierState's body ran with no keyArg");
    return ui_event_get_modifier_state(ctx, this_val, argv[0]);
}

/* ---- Pointer Events 4 §"Initializers for interface MouseEvent" ----------------------------------------------
 *
 *     partial interface MouseEvent {
 *       undefined initMouseEvent(DOMString typeArg,
 *         optional boolean bubblesArg = false,      optional boolean cancelableArg = false,
 *         optional Window? viewArg = null,          optional long detailArg = 0,
 *         optional long screenXArg = 0,             optional long screenYArg = 0,
 *         optional long clientXArg = 0,             optional long clientYArg = 0,
 *         optional boolean ctrlKeyArg = false,      optional boolean altKeyArg = false,
 *         optional boolean shiftKeyArg = false,     optional boolean metaKeyArg = false,
 *         optional short buttonArg = 0,             optional EventTarget? relatedTargetArg = null);
 *     };
 *
 * THE ARGUMENT LIST IS THE SPEC'S, IN ITS ORDER, AND IT IS NOT THE ATTRIBUTE ORDER. The four modifiers arrive
 * ctrl, ALT, SHIFT, meta — a different order from the one §3.5.3 pairs the attributes in, and from the one
 * MD_MODIFIER is written in — so the pairing is stated once, below, rather than being read off a table that
 * means something else. `buttons`, `layerX`, `layerY` and Pointer Lock 2.0 §6's `movementX`/`movementY` have no
 * argument here and are left exactly as they were, which is what an argument list that does not name them
 * means — and Pointer Lock declares no legacy initializer of its own to write them either.
 *
 * `short buttonArg` is declared IDL_UNSIGNED_SHORT and folded, for the reason md_button_fold gives: the pool
 * declares no `short`, and Web IDL §3.2.4.3's two halves are the modulo (the declaration's) and the signed fold (this
 * file's), so the member is declared with the WIDTH it has. */
/* THE TWO INTERFACE POSITIONS ARE TWO DIFFERENT INTERFACES, which is what a single brand per declaration
   cannot say and why idl_arg_iface exists: `viewArg` is `Window?` at 3 and `relatedTargetArg` is
   `EventTarget?` at 14. Both were IDL_ANY, so both crossed unconverted and the body ran §3.2.15's brand test
   by hand — twice, in argument order, to keep the order Web IDL's left-to-right conversion already states.
   NEITHER INTERFACE IS A CLASS. A Window is the realm's own global OR a WindowProxy; an EventTarget is every
   Node, every Window, every MessagePort, every AbortSignal and every `new EventTarget()`. So each position
   states the PREDICATE its owning component already publishes, which is what §3.2.15's word "implements"
   means for an interface anything inherits from. */
static const IdlArgType MD_INIT_MOUSE_ARGS[15] = {
    IDL_DOMSTRING, IDL_BOOLEAN, IDL_BOOLEAN, IDL_INTERFACE_NULLABLE /* Window? */, IDL_LONG,
    IDL_LONG, IDL_LONG, IDL_LONG, IDL_LONG,
    IDL_BOOLEAN, IDL_BOOLEAN, IDL_BOOLEAN, IDL_BOOLEAN,
    IDL_UNSIGNED_SHORT /* `short`, folded below */, IDL_INTERFACE_NULLABLE /* EventTarget? */,
};

/* WHICH ARGUMENT SETS WHICH KEY MODIFIER — the spec's list order paired with this file's own modifier table,
   in the one place both are in hand. */
static const struct { int arg; int modifier; } MD_INIT_MOUSE_MODIFIERS[] = {
    { 9, MD_CTRL }, { 10, MD_ALT }, { 11, MD_SHIFT }, { 12, MD_META },
};

static JSValue js_md_init_mouse_event(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    JSValue view, related, slots;
    unsigned i;

    (void)magic;
    if (!mouse_event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "initMouseEvent called on something that is not a MouseEvent");
    /* Only `typeArg` is required, and §3.6 step 5's check for it is the declaration's. */
    DCHECK(argc >= 1, "initMouseEvent's body ran with no typeArg");
    /* THE TWO INTERFACE-TYPED POSITIONS ARE CONVERSIONS, so they run BEFORE the algorithm's first step and in
       ARGUMENT ORDER — `viewArg` is argument 4 and `relatedTargetArg` argument 15, and a call that gets both
       wrong reports the first one, which is what Web IDL's left-to-right conversion means. Both of those are
       now the DECLARATION's: the machine converts positions left to right and throws there, ahead of this body
       and therefore ahead of the dispatch-flag early return below, which is the same order for the same reason
       — a conversion is not a step of the algorithm. What arrives is the IDL null or a value implementing the
       declared interface, and §3.6 step 16.1 places §16.1's own `= null` at both for a call that reached
       neither. `view` and `related` are both CONSUMED below, so both are dup'd. */
    DCHECK(argc > 14, "initMouseEvent's `viewArg` and `relatedTargetArg` both declare Pointer Events 4 §16.1's "
                      "own `= null`, so §3.6 step 16.1 extends the conversion to the last of them and the body "
                      "is handed every position for every call");
    view = JS_DupValue(ctx, argv[3]);
    related = JS_DupValue(ctx, argv[14]);
    /* "the same behavior as UIEvent.initUIEvent()" — §2.2's initialise-an-existing-event and `view`, with its
       early return: an event being dispatched is left exactly as it is, in every half. */
    if (!ui_event_reinit(ctx, this_val, argv[0], md_arg_bool(ctx, argc, argv, 1),
                         md_arg_bool(ctx, argc, argv, 2), view)) {
        JS_FreeValue(ctx, related);
        return JS_UNDEFINED;
    }
    ui_event_set_detail(ctx, this_val, md_arg_i32(ctx, argc, argv, 4));
    slots = md_slots(ctx, this_val);
    DCHECK(JS_IsObject(slots), "initMouseEvent passed its brand check and then found no MouseEvent slot record");
    JS_SetPropertyStr(ctx, slots, "screenX", JS_NewInt32(ctx, md_arg_i32(ctx, argc, argv, 5)));
    JS_SetPropertyStr(ctx, slots, "screenY", JS_NewInt32(ctx, md_arg_i32(ctx, argc, argv, 6)));
    JS_SetPropertyStr(ctx, slots, "clientX", JS_NewInt32(ctx, md_arg_i32(ctx, argc, argv, 7)));
    JS_SetPropertyStr(ctx, slots, "clientY", JS_NewInt32(ctx, md_arg_i32(ctx, argc, argv, 8)));
    JS_SetPropertyStr(ctx, slots, "button",
                      JS_NewInt32(ctx, md_button_fold(md_arg_u32(ctx, argc, argv, 13))));
    JS_FreeValue(ctx, slots);
    for (i = 0; i < sizeof(MD_INIT_MOUSE_MODIFIERS) / sizeof(MD_INIT_MOUSE_MODIFIERS[0]); i++)
        ui_event_set_modifier_state(ctx, this_val, MD_MODIFIER[MD_INIT_MOUSE_MODIFIERS[i].modifier],
                                    md_arg_bool(ctx, argc, argv, MD_INIT_MOUSE_MODIFIERS[i].arg));
    /* §2.2's associated relatedTarget, on the EVENT — see the file comment. */
    event_set_related_target(ctx, this_val, related);
    JS_FreeValue(ctx, related);
    return JS_UNDEFINED;
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional MouseEventInit eventInitDict = {})`. MouseEventInit inherits
 * EventModifierInit inherits UIEventInit inherits EventInit, and Web IDL §3.2.17 reads the INHERITED members first and
 * each dictionary's own lexicographically among THEMSELVES — which is the order this list is in, and the order
 * a page pins by throwing from one member's getter. `button` sorts before `cancelable` and `altKey` before
 * `bubbles`, so a single sorted list would read a derived dictionary's members before its base's: THE LEVEL is
 * what makes this list the spec's read order. The two inherited levels are spliced from ui_event.h rather than
 * written again here, so this dictionary and KeyboardEventInit cannot state them differently. */
static const IdlArgType MD_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember MD_INIT[] = {
    UI_EVENT_INIT_MEMBERS,
    EVENT_MODIFIER_INIT_MEMBERS,
    { "button", IDL_UNSIGNED_SHORT, false, NULL, 3 }, { "buttons", IDL_UNSIGNED_SHORT, false, NULL, 3 },
    { "clientX", IDL_LONG, false, NULL, 3 }, { "clientY", IDL_LONG, false, NULL, 3 },
    /* Pointer Lock 2.0 §7's two members. A PARTIAL DICTIONARY's members are members of the dictionary itself,
       so they sort among MouseEventInit's OWN at level 3 — between `clientY` and `relatedTarget` — and not in
       a block of their own after them. Which spec contributed a member is not something §3.2.17's read order
       can see. */
    { "movementX", IDL_DOUBLE, false, NULL, 3 }, { "movementY", IDL_DOUBLE, false, NULL, 3 },
    { "relatedTarget", IDL_ANY, false, NULL, 3 },
    { "screenX", IDL_LONG, false, NULL, 3 }, { "screenY", IDL_LONG, false, NULL, 3 },
};

static JSValue js_md_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor MouseEvent requires 'new'");
    DCHECK(argc >= 1, "the MouseEvent constructor body ran with no type argument — §3.6 step 5 is the "
                      "declaration's and throws before any body is entered");
    /* An event the PAGE constructs is untrusted. */
    return mouse_event_new_derived(ctx, mouse_event_proto(ctx), argv[0], argc > 1 ? argv[1] : JS_UNDEFINED,
                                   /*trusted*/ false);
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_md_proto[] = {
    JS_CGETSET_MAGIC_DEF("screenX", js_md_get, NULL, MD_SCREEN_X),
    JS_CGETSET_MAGIC_DEF("screenY", js_md_get, NULL, MD_SCREEN_Y),
    JS_CGETSET_MAGIC_DEF("clientX", js_md_get, NULL, MD_CLIENT_X),
    JS_CGETSET_MAGIC_DEF("clientY", js_md_get, NULL, MD_CLIENT_Y),
    /* CSSOM VIEW §10: "the x attribute must return the value of clientX" and "the y attribute must return the
       value of clientY". An ALIAS, stated by the spec as one — so it is the same getter over the same slot,
       and there is no second value to keep in step or to disagree about `layerX`-style un-initialization. */
    JS_CGETSET_MAGIC_DEF("x", js_md_get, NULL, MD_CLIENT_X),
    JS_CGETSET_MAGIC_DEF("y", js_md_get, NULL, MD_CLIENT_Y),
    /* §10's page coordinates, DERIVED — the axis is named by the client coordinate each one is built on. */
    JS_CGETSET_MAGIC_DEF("pageX", js_md_get_page, NULL, MD_CLIENT_X),
    JS_CGETSET_MAGIC_DEF("pageY", js_md_get_page, NULL, MD_CLIENT_Y),
    /* Pointer Lock 2.0 §6's two, STORED — §7 declares the dictionary members that fill them and §6 states the
       un-initialized 0 they otherwise carry, so they are slots exactly as `clientX` is. */
    JS_CGETSET_MAGIC_DEF("movementX", js_md_get, NULL, MD_MOVEMENT_X),
    JS_CGETSET_MAGIC_DEF("movementY", js_md_get, NULL, MD_MOVEMENT_Y),
    JS_CGETSET_MAGIC_DEF("layerX", js_md_get, NULL, MD_LAYER_X),
    JS_CGETSET_MAGIC_DEF("layerY", js_md_get, NULL, MD_LAYER_Y),
    JS_CGETSET_MAGIC_DEF("button", js_md_get, NULL, MD_BUTTON),
    JS_CGETSET_MAGIC_DEF("buttons", js_md_get, NULL, MD_BUTTONS),
    JS_CGETSET_MAGIC_DEF("ctrlKey", js_md_get_modifier, NULL, MD_CTRL),
    JS_CGETSET_MAGIC_DEF("shiftKey", js_md_get_modifier, NULL, MD_SHIFT),
    JS_CGETSET_MAGIC_DEF("altKey", js_md_get_modifier, NULL, MD_ALT),
    JS_CGETSET_MAGIC_DEF("metaKey", js_md_get_modifier, NULL, MD_META),
    JS_CGETSET_MAGIC_DEF("relatedTarget", js_md_get_related_target, NULL, 0),
};

void mouse_event_init(JSContext *ctx)
{
    JSClassDef d = { "MouseEvent" };
    static const IdlArgType MODIFIER_STATE_ARGS[1] = { IDL_DOMSTRING };

    DCHECK(!g_ready, "mouse_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "mouseEventSlots", false);
    CHECK(!JS_IsException(g_key), "the MouseEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_me_class);
    JS_NewClass(JS_GetRuntime(ctx), g_me_class, &d);
    /* DECLARED HERE, at agent init, and not from the per-realm install: a fresh id minted per realm is a member
       being minted per realm, which idl_declared_before_seal exists to catch. */
    g_modifier_state_id = idl_method_id(ctx, MODIFIER_STATE_ARGS, 1, js_md_get_modifier_state, 0);
    g_init_mouse_id = idl_method_id(ctx, MD_INIT_MOUSE_ARGS, 15, js_md_init_mouse_event, 0);
    idl_optional_from(1);   /* every argument but `typeArg` is optional */
    /* Pointer Events 4 §16.1 Initializers for interface MouseEvent: `optional Window? viewArg = null` and
       `optional EventTarget? relatedTargetArg = null`. TWO interfaces in one argument list, so each position
       states its own — the case idl_iface_brand's one-class-per-declaration walks past. Each predicate is the
       owning component's, and each default is the IDL's own so §3.6 step 16.1 places it rather than the body
       deciding what an absent position means. */
    idl_arg_iface(3, window_proxy_is_window, "Window");
    idl_arg_default(3, IDL_DEFAULT_NULL, NULL);
    idl_arg_iface(14, event_target_is_value, "EventTarget");
    idl_arg_default(14, IDL_DEFAULT_NULL, NULL);
    g_ctor_stepid = idl_method_id_dict(ctx, MD_CTOR_ARGS, 2, MD_INIT,
                                       (int)(sizeof(MD_INIT) / sizeof(MD_INIT[0])), js_md_ctor, 0);
    idl_optional_from(1);   /* `constructor(DOMString type, optional MouseEventInit eventInitDict = {})` */
    idl_iface_brand(input_device_capabilities_class());   /* MouseEventInit's one interface-typed member,
                                                             UIEventInit's `sourceCapabilities` */
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — AND IT NAMES THE `event` ROW, NOT THIS FILE.
       core/agent_state.h: a sub-component names the row whose RELEASE gives its slots back, which for every
       Event subclass is core/platform.c's `event` row — event_init calls this init and event_free calls this
       release. Nothing here was declared at all, so the pairing's own arm — does anybody release this? — was
       never asked about any of these. */
    agent_state_flag("event", &g_ready,
                     "Pointer Events 4 §11.1 MouseEvent interface's declaration latch");
    agent_state_class("event", &g_me_class,
                      "Pointer Events 4 §11.1 MouseEvent interface's class, held for its per-realm prototype slot");
    agent_state_value("event", &g_key,
                      "the private Symbol Pointer Events 4 §11.1 MouseEvent interface's slot record hangs off");
    agent_state_id("event", &g_ctor_stepid,
                   "Pointer Events 4 §11.1 MouseEvent interface's `constructor(DOMString type, optional "
                   "MouseEventInit eventInitDict = {})`");
    agent_state_id("event", &g_modifier_state_id,
                   "Pointer Events 4 §11.1 MouseEvent interface's `getModifierState(DOMString keyArg)` — this "
                   "interface's OWN declaration of it, which is what makes calling KeyboardEvent's on a MouseEvent "
                   "a TypeError");
    agent_state_id("event", &g_init_mouse_id,
                   "Pointer Events 4 §16.1 Initializers for interface MouseEvent's `initMouseEvent(...)`");
    realm_declare_intrinsic(mouse_event_install_protos);
}

void mouse_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for MouseEvent before mouse_event_init declared it");
    prev = JS_GetClassProto(ctx, g_me_class);
    DCHECK(JS_IsNull(prev), "mouse_event_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    /* `interface MouseEvent : UIEvent` — THIS realm's UIEvent.prototype, which the intrinsic declared before
       this one has already built. */
    base = ui_event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "MouseEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "MouseEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_md_proto, (int)(sizeof(js_md_proto) / sizeof(js_md_proto[0])));
    idl_install_method(ctx, proto, "getModifierState", g_modifier_state_id);
    idl_install_method(ctx, proto, "initMouseEvent", g_init_mouse_id);
    JS_SetClassProto(ctx, g_me_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object on THIS realm's global — see ui_event.c. */
    ctor = idl_step_constructor(ctx, "MouseEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the MouseEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "MouseEvent", ctor);
    JS_FreeValue(ctx, global);
}

/* THE RUNTIME, NOT A REALM — core/platform.h's release column, reached through event_free. What this
   gives back is the AGENT's: a private Symbol, a class id and this interface's member declarations; every
   prototype it built is in some realm's class-proto slot and goes with that realm. */
void mouse_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. core/events/event.c's event_init calls this component's init on the ONE
       declaration pass and its event_free — which has already asserted its own latch — calls this release
       unconditionally, so the test could never be true and what it could do was hide a release that left the
       latch set. */
    DCHECK(g_ready, "Pointer Events 4 §11.1 MouseEvent interface was released in an agent that never declared it — "
                    "event_init declares every Event subclass on the one unconditional pass");
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because the id doubles
       as the init latch and a carried one names a class in a runtime that is gone. Nothing WEARS this class —
       it exists for its per-realm prototype slot, and every event in this engine is minted by
       core/events/event.c's event_make_proto through JS_NewObjectProto — so there is no finalizer and no
       gc_mark here to owe the JS_GetAnyOpaque the zeroing costs a component whose objects do wear one. */
    g_me_class = 0;
    g_ctor_stepid = g_modifier_state_id = g_init_mouse_id = -1;
}
