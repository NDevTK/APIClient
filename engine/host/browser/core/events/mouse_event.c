/* THE MouseEvent INTERFACE — Pointer Events 4 §"Interface MouseEvent".
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
 * event's type is click" — a question about the OBJECT, which event_target.c could only ask of the type
 * string, so any `new Event('click')` was an activation event and `new MouseEvent('click')` was a missing
 * global. §4.5's createEvent had to refuse `mouseevent`/`mouseevents`, which is how a great deal of shipped
 * code synthesises a click. mouse_event_is is the answer to step 6.4's real question.
 *
 * THE COORDINATES COME FROM THE INIT DICTIONARY, WHICH IS THE IDL AND NOT A HEADLESS COMPROMISE. `screenX` is
 * a `long` that "MUST" be 0 un-initialized and is otherwise whatever MouseEventInit's `screenX` said. There is
 * nothing to sense and nothing to invent: a constructed event's coordinates are the ones it was constructed
 * with, in a browser with a mouse exactly as here.
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
 * `initMouseEvent` IS NOT INSTALLED, and it is the one member of this interface that is not. Pointer Events 4
 * declares it with FIFTEEN arguments; core/idl_args.h's IDL_MAX_DECLARED is 8, and its comment states that
 * eight "is the widest the platform has" — which initMouseEvent(15) and initKeyboardEvent(10) contradict. A
 * member cannot be declared past that ceiling, and converting arguments 9..15 inside the body would run the
 * page's valueOf from C, which is the drive-to-completion this engine aborts on. So the member is honestly
 * ABSENT — a page calling it gets its own TypeError — until that ceiling is the platform's real widest. */
#include "check.h"
#include "quickjs.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/events/mouse_event.h"
#include "core/events/ui_event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key;         /* the private Symbol this interface's own slots hang off */
static JSClassID g_me_class;    /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;
static int       g_modifier_state_id = -1;

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

/* `short button` — §3.2.5's conversion, whose FIRST half the declaration already performed. Every integer type
   in Web IDL is the same arithmetic modulo 2^width; `short` and `unsigned short` are one width and differ only
   in the final fold into the signed range, and the pool declares no `short`, so the member is declared with the
   width it has and folded here. The DCHECK is the contract that makes that exact: what arrives must already be
   the 16-bit modulo, so the day the pool gains the type this fold is deleted and nothing else moves. */
static int32_t md_button_of(JSContext *ctx, JSValueConst init)
{
    uint32_t u = ui_event_dict_u32(ctx, init, "button");

    DCHECK(u <= 0xFFFFu, "MouseEventInit's `button` reached its fold outside the 16-bit range the declared "
                         "type produces — the modulo is the declaration's half of §3.2.5 and this is the "
                         "signed fold that completes it");
    return u >= 0x8000u ? (int32_t)u - 0x10000 : (int32_t)u;
}

/* `EventTarget?` — Web IDL §3.2.16 over the `relatedTarget` member. null and undefined are the IDL null; any
   other value must IMPLEMENT EventTarget, and there is no single class to brand against because every Node,
   every Window, every MessagePort and every `new EventTarget()` is one. So the question is asked of the
   object's PROTOTYPE CHAIN, which is where an interface's members actually live: a platform object implements
   EventTarget exactly when this realm's EventTarget.prototype is on its chain.
   THE WALK NEVER TOUCHES A PROXY. JS_GetPrototype on a Proxy runs its getPrototypeOf trap — the page's code,
   from inside a C activation — and a Proxy is not a platform object implementing the interface anyway, so a
   link that is one ends the walk instead of being asked. Answers JS_NULL / an owned dup, or JS_EXCEPTION with
   the TypeError live. */
static JSValue md_related_of(JSContext *ctx, JSValueConst v)
{
    JSValue p, target;
    bool ok = false;

    if (JS_IsUndefined(v) || JS_IsNull(v))
        return JS_NULL;
    if (!JS_IsObject(v) || JS_IsProxy(v))
        return JS_ThrowTypeError(ctx, "a MouseEvent's `relatedTarget` must be an EventTarget or null");
    target = event_target_proto(ctx);
    p = JS_GetPrototype(ctx, v);
    while (JS_IsObject(p) && !JS_IsProxy(p)) {
        JSValue next;
        if (JS_VALUE_GET_PTR(p) == JS_VALUE_GET_PTR(target)) { ok = true; break; }
        next = JS_GetPrototype(ctx, p);
        JS_FreeValue(ctx, p);
        p = next;
    }
    JS_FreeValue(ctx, p);
    JS_FreeValue(ctx, target);
    if (!ok)
        return JS_ThrowTypeError(ctx, "a MouseEvent's `relatedTarget` must be an EventTarget or null");
    return JS_DupValue(ctx, v);
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
    given = idl_dict_get(ctx, init, "relatedTarget");
    related = md_related_of(ctx, given);
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

/* ---- the attributes ----------------------------------------------------------------------------------------- */

enum { MD_SCREEN_X = 0, MD_SCREEN_Y, MD_CLIENT_X, MD_CLIENT_Y, MD_LAYER_X, MD_LAYER_Y, MD_BUTTON, MD_BUTTONS };
static const char *const MD_SLOT[] = {
    "screenX", "screenY", "clientX", "clientY", "layerX", "layerY", "button", "buttons",
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
    /* `keyArg` is not optional, so §3.6.2 step 1's check is the DECLARATION's and throws before this runs. */
    DCHECK(argc >= 1, "getModifierState's body ran with no keyArg");
    return ui_event_get_modifier_state(ctx, this_val, argv[0]);
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional MouseEventInit eventInitDict = {})`. MouseEventInit inherits
 * EventModifierInit inherits UIEventInit inherits EventInit, and §3.2.18 reads the INHERITED members first and
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
    { "relatedTarget", IDL_ANY, false, NULL, 3 },
    { "screenX", IDL_LONG, false, NULL, 3 }, { "screenY", IDL_LONG, false, NULL, 3 },
};

static JSValue js_md_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor MouseEvent requires 'new'");
    DCHECK(argc >= 1, "the MouseEvent constructor body ran with no type argument — §3.6.2 step 1 is the "
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
    g_ctor_stepid = idl_method_id_dict(ctx, MD_CTOR_ARGS, 2, MD_INIT,
                                       (int)(sizeof(MD_INIT) / sizeof(MD_INIT[0])), js_md_ctor, 0);
    idl_optional_from(1);   /* `constructor(DOMString type, optional MouseEventInit eventInitDict = {})` */
    g_ready = 1;
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
    idl_install_method(ctx, proto, "getModifierState", 1, g_modifier_state_id);
    JS_SetClassProto(ctx, g_me_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object on THIS realm's global — see ui_event.c. */
    ctor = idl_step_constructor(ctx, "MouseEvent", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the MouseEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "MouseEvent", ctor);
    JS_FreeValue(ctx, global);
}

void mouse_event_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = g_modifier_state_id = -1;
}
