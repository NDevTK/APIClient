/* THE WheelEvent INTERFACE — Pointer Events 4 §12.1 "WheelEvent interface".
 *
 *     [Exposed=Window]
 *     interface WheelEvent : MouseEvent {
 *       constructor(DOMString type, optional WheelEventInit eventInitDict = {});
 *       const unsigned long DOM_DELTA_PIXEL = 0x00;   // DOM_DELTA_LINE 0x01, DOM_DELTA_PAGE 0x02
 *       readonly attribute double deltaX;  readonly attribute double deltaY;  readonly attribute double deltaZ;
 *       readonly attribute unsigned long deltaMode;
 *       readonly attribute boolean momentum;
 *     };
 *     dictionary WheelEventInit : MouseEventInit {
 *       double deltaX = 0.0; double deltaY = 0.0; double deltaZ = 0.0;
 *       unsigned long deltaMode = 0; boolean momentum = false;
 *     };
 *
 * THE STANDARD IS POINTER EVENTS AND NOT UI EVENTS, WHICH IS WORTH STATING BECAUSE EVERY READER ARRIVES
 * EXPECTING THE OTHER. UI Events' own terms section says its user agent "[POINTEREVENTS4] defines the
 * following terms: MouseEvent, WheelEvent, button, click" — the same sentence mouse_event.c cites one level
 * down — so the interface, its dictionary and their prose live in Pointer Events 4 §12 "Wheel Events and
 * interfaces", and that is also where @webref/idl harvests them from, which is the member list engine/idlgen.mjs
 * diffs this file against. `momentum` exists ONLY in that reading: UI Events does not mention the word.
 *
 * WHAT WAS BLOCKED ON IT. `new WheelEvent('wheel', e)` is how a page RE-DISPATCHES a wheel it received at a
 * different element, and it is written UNGUARDED — a canvas app in this tree's own corpus reaches
 * `let o = new WheelEvent('wheel', r); o.isSpecialRedispatchedEvent = true; canvas.dispatchEvent(o)` with no
 * `typeof` test in front of it, because a bundle guards what it might not GET and does not guard what it only
 * means to NAME. A missing global there is a ReferenceError that ends the flow, and every endpoint and every
 * sink the rest of that module would have reached goes with it.
 *
 * EVERY OBSERVABLE THIS INTERFACE HAS IS WRITTEN BY ITS OWN CONSTRUCTOR, which is what makes one landing the
 * whole of it rather than a shape to fill in later. §12.1 states an un-initialized value for each of the five
 * attributes (0.0, 0.0, 0.0, 0 and false) and a dictionary member that places exactly that, and no other
 * algorithm anywhere writes one: §12.3 "wheel" is a user-agent DISPATCH from a rotating wheel device, and what
 * it sets is the same five members through the same dictionary. So a constructed WheelEvent is not a narrower
 * model of a real one — it is the same object by the same steps, which is §Headless-is-not-valueless' case and
 * not §NO STUBS' exception.
 *
 * THERE IS NO createEvent ROW AND ADDING ONE WOULD BE A REGRESSION. DOM §4.5 "Interface Document"'s
 * createEvent table names BeforeUnloadEvent, CompositionEvent, CustomEvent, DeviceMotionEvent,
 * DeviceOrientationEvent, DragEvent, Event, FocusEvent, HashChangeEvent, KeyboardEvent, MessageEvent,
 * MouseEvent, StorageEvent, TextEvent, TouchEvent and UIEvent — and not this one, which is why
 * core/events/create_event.c has no row and why `document.createEvent('WheelEvent')` is step 3's
 * NotSupportedError in every browser. The corpus agrees in writing: WPT's dom/nodes/Document-createEvent
 * carries WheelEvent in its `someNonCreateableEvents` list, so a row here would turn two PASSING subtests red.
 * THAT IS ALSO WHY THIS FILE EXPORTS NO `wheel_event_new`: DOM §2.5 "Constructing events"' create an event
 * using X is owed by an interface some algorithm MINTS, and nothing in this engine mints one.
 *
 * THERE IS NO LEGACY INITIALIZER EITHER. Pointer Events 4 §16 "Legacy Event Initializers" has exactly one
 * subsection, §16.1 "Initializers for interface MouseEvent" — there is no `initWheelEvent`, so unlike
 * KeyboardEvent this interface owes none.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, for the reason event.c gives: a property write is
 * captured by the COW delta, so the event's state time-travels with the flow that built it, and the symbol is
 * a brand a page cannot forge. */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/input_device_capabilities.h"
#include "core/events/mouse_event.h"
#include "core/events/ui_event.h"
#include "core/events/wheel_event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's own slots hang off */
static JSClassID g_we_class;    /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

/* THE SLOT NAMES, INDEXED BY THE MAGIC THE ACCESSOR TABLE DECLARES — one getter body over five members, so
   the five cannot drift into five spellings of one contract. */
enum {
    WE_DELTA_X,
    WE_DELTA_Y,
    WE_DELTA_Z,
    WE_DELTA_MODE,
    WE_MOMENTUM,
};
static const char *const WE_SLOT[] = { "deltaX", "deltaY", "deltaZ", "deltaMode", "momentum" };

static JSValue wheel_event_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_we_class);

    DCHECK(!JS_IsNull(proto),
           "WheelEvent.prototype was asked for in a realm that never ran wheel_event_install_protos");
    return proto;   /* OWNED */
}

static JSValue we_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "a WheelEvent's slots were asked for before wheel_event_init ran");
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

/* This interface's own five members, on an event whose Event, UIEvent and MouseEvent halves are already built.
 * Returns -1 with the throw live.
 *
 * THE FOUR NUMERIC MEMBERS AND `momentum` ARE READ BY TWO DIFFERENT KINDS OF READER AND THE DIFFERENCE IS THE
 * IDL's, not this file's. core/idl_args.h's idl_concolic_rule answers IDL_CONCOLIC_FORKS for Web IDL §3.2.3
 * "boolean", so the member loop has already forked `momentum` and what reaches idl_dict_bool is a real truth
 * value — which is exactly what that reader asserts. It answers IDL_CONCOLIC_CROSSES for `double` and
 * `unsigned long`, so an unknown at one of those is rewritten to IDL_ANY by the member loop and PLACED AS
 * ITSELF on the converted record, and the reader below meets it there.
 *
 * THAT CROSSED VALUE IS NOW CARRIED TO THE SLOT, which is what ui_event_dict_num is for and why these four
 * reads no longer ask for a C scalar. The residual that stood here is RETIRED, and one clause of it was wrong
 * in a way worth keeping rather than deleting, because the next reader will re-derive it the same way: it
 * said an unknown supplied for one of these members is `read back CONCRETE` and that the attribute
 * afterwards `answers the example`. IT NEVER DID. A C scalar reader takes the crossed value to ECMAScript
 * §7.1.4 ToNumber ( arg ), which is a boundary that owes C a real primitive — quickjs.c's JS_ToNumberHintFree
 * DFAILS on an unknown in dev and throws a TypeError in release, and these readers DISCARDED the conversion's
 * return, so the release answer was a swallowed exception and a 0 (or a NaN), never the example. The author
 * reasoned from the RETURN TYPE (`no C scalar can carry a concolic triple`, which is true) to the BEHAVIOUR
 * (`so it answers the example`, which the coercion boundary refuses), and a return type says nothing about
 * what the coercion under it does with a value it cannot represent. The remedy clause was right anyway, which
 * is the ordinary split: the spec half of a claim is checkable and the mechanism half is a guess. */
static int we_init_slots(JSContext *ctx, JSValueConst ev, JSValueConst init)
{
    JSValue slots;
    JSAtom k;

    DCHECK(g_ready, "a WheelEvent was minted before wheel_event_init declared the interface — the slot key it "
                    "hangs its state off is made there");
    slots = idl_slots_new(ctx);
    k = JS_ValueToAtom(ctx, g_key);
    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    /* An ABSENT dictionary places no default at all, so the un-initialized value each attribute carries is
       §12.1's own — 0.0 for each delta and 0 for `deltaMode` — and it is written HERE because that is the only
       place that knows it. It agrees with the dictionary's `= 0.0` / `= 0` on all four, which is §12.1 agreeing
       with itself and not a fact either reader could derive: PointerEvent's `width` and `height` are the same
       two requirements DISAGREEING, at 1 rather than 0, which is why ui_event_dict_num takes the value rather
       than choosing one from the member's declared type. */
    JS_SetPropertyStr(ctx, slots, WE_SLOT[WE_DELTA_X],
                      ui_event_dict_num(ctx, init, "deltaX", JS_NewFloat64(ctx, 0.0)));
    JS_SetPropertyStr(ctx, slots, WE_SLOT[WE_DELTA_Y],
                      ui_event_dict_num(ctx, init, "deltaY", JS_NewFloat64(ctx, 0.0)));
    JS_SetPropertyStr(ctx, slots, WE_SLOT[WE_DELTA_Z],
                      ui_event_dict_num(ctx, init, "deltaZ", JS_NewFloat64(ctx, 0.0)));
    /* `unsigned long deltaMode` and NOT a `long` — §12.1's three DeltaModeCode constants are 0x00, 0x01 and
       0x02, and the type is what decides that `new WheelEvent('wheel', {deltaMode: -1})` reads back
       4294967295 rather than -1. The conversion is the DECLARATION's (IDL_UNSIGNED_LONG's modulo), so this
       reads the record it built and must not narrow it a second time. */
    JS_SetPropertyStr(ctx, slots, WE_SLOT[WE_DELTA_MODE],
                      ui_event_dict_num(ctx, init, "deltaMode", JS_NewUint32(ctx, 0)));
    JS_SetPropertyStr(ctx, slots, WE_SLOT[WE_MOMENTUM], JS_NewBool(ctx, idl_dict_bool(ctx, init, "momentum")));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

/* ---- the attributes ---------------------------------------------------------------------------------------- */

/* Web IDL §3.7.6 "Attributes" makes an interface's accessor check that its receiver implements the interface,
   so `WheelEvent.prototype.deltaX` read on the PROTOTYPE OBJECT — which carries no slot record — is a
   TypeError and not 0. It is a THROW and never a DCHECK: a receiver is page-supplied input, and asserting on
   one would hand any page an abort switch for the whole engine. */
static JSValue js_we_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = we_slots(ctx, this_val), v;

    DCHECK(magic >= 0 && magic < (int)(sizeof(WE_SLOT) / sizeof(WE_SLOT[0])),
           "a WheelEvent attribute was declared with a magic the slot table does not name");
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a WheelEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, WE_SLOT[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional WheelEventInit eventInitDict = {})`. WheelEventInit inherits
 * MouseEventInit inherits EventModifierInit inherits UIEventInit inherits EventInit, which is why this list
 * splices all three shared macros and appends its own five at LEVEL 4 — the depth mouse_event.h names as the
 * one a `: MouseEventInit` dictionary must use. Web IDL §3.2.17 "Dictionary types" reads the INHERITED members
 * first and each dictionary's own LEXICOGRAPHICALLY among themselves, so these five sort deltaMode, deltaX,
 * deltaY, deltaZ, momentum whatever order they are about — and the order is observable, because a page pins it
 * by throwing from one member's getter. */
static const IdlArgType WE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember WE_INIT[] = {
    UI_EVENT_INIT_MEMBERS,
    EVENT_MODIFIER_INIT_MEMBERS,
    MOUSE_EVENT_INIT_MEMBERS,
    { "deltaMode", IDL_UNSIGNED_LONG, false, NULL, 4, NULL, IDL_DEFAULT_ZERO },
    { "deltaX", IDL_DOUBLE, false, NULL, 4, NULL, IDL_DEFAULT_ZERO },
    { "deltaY", IDL_DOUBLE, false, NULL, 4, NULL, IDL_DEFAULT_ZERO },
    { "deltaZ", IDL_DOUBLE, false, NULL, 4, NULL, IDL_DEFAULT_ZERO },
    { "momentum", IDL_BOOLEAN, false, NULL, 4, NULL, IDL_DEFAULT_FALSE },
};

static JSValue js_we_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue ev;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor WheelEvent requires 'new'");
    DCHECK(argc >= 1, "the WheelEvent constructor body ran with no type argument — Web IDL §3.6 step 5 is the "
                      "declaration's and throws before any body is entered");
    /* An event the PAGE constructs is untrusted. */
    ev = mouse_event_new_derived(ctx, wheel_event_proto(ctx), argv[0], argc > 1 ? argv[1] : JS_UNDEFINED,
                                 /*trusted*/ false);
    if (JS_IsException(ev))
        return ev;
    if (we_init_slots(ctx, ev, argc > 1 ? argv[1] : JS_UNDEFINED) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_we_proto[] = {
    JS_CGETSET_MAGIC_DEF("deltaX", js_we_get, NULL, WE_DELTA_X),
    JS_CGETSET_MAGIC_DEF("deltaY", js_we_get, NULL, WE_DELTA_Y),
    JS_CGETSET_MAGIC_DEF("deltaZ", js_we_get, NULL, WE_DELTA_Z),
    JS_CGETSET_MAGIC_DEF("deltaMode", js_we_get, NULL, WE_DELTA_MODE),
    JS_CGETSET_MAGIC_DEF("momentum", js_we_get, NULL, WE_MOMENTUM),
};

/* §12.1's DeltaModeCode constants. Web IDL puts a `const` on the interface PROTOTYPE object AND on the
   interface object, so one table installs both — reached by name, the way keyboard_event.c installs
   §3.5.1's KeyLocationCodes. */
static const JSCFunctionListEntry js_we_consts[] = {
    JS_PROP_INT32_DEF("DOM_DELTA_PIXEL", 0x00, IDL_CONSTANT_PROP_FLAGS),
    JS_PROP_INT32_DEF("DOM_DELTA_LINE", 0x01, IDL_CONSTANT_PROP_FLAGS),
    JS_PROP_INT32_DEF("DOM_DELTA_PAGE", 0x02, IDL_CONSTANT_PROP_FLAGS),
};

void wheel_event_init(JSContext *ctx)
{
    JSClassDef d = { "WheelEvent" };

    DCHECK(!g_ready, "wheel_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "wheelEventSlots", false);
    CHECK(!JS_IsException(g_key), "the WheelEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_we_class);
    JS_NewClass(JS_GetRuntime(ctx), g_we_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, WE_CTOR_ARGS, 2, WE_INIT,
                                       (int)(sizeof(WE_INIT) / sizeof(WE_INIT[0])), js_we_ctor, 0);
    idl_optional_from(1);   /* `constructor(DOMString type, optional WheelEventInit eventInitDict = {})` */
    /* THE DECLARATION-WIDE CLASS, the brand of exactly ONE of this dictionary's three interface-typed members:
       UIEventInit's `sourceCapabilities`. The other two — UIEventInit's `view` and MouseEventInit's
       `relatedTarget` — state Web IDL §3.2.15 "Interface types"' `I` as their OWN realm-taking predicate,
       which idl_member_implements takes in preference to the class, so this line never decides for them. */
    idl_iface_brand(input_device_capabilities_class());
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — AND IT NAMES THE `event` ROW, NOT THIS FILE.
       core/agent_state.h: a sub-component names the row whose RELEASE gives its slots back, which for every
       Event subclass is core/platform.c's `event` row — event_init calls this init and event_free calls this
       release. */
    agent_state_flag("event", &g_ready,
                     "Pointer Events 4 §12.1 WheelEvent interface's declaration latch");
    agent_state_class("event", &g_we_class,
                      "Pointer Events 4 §12.1 WheelEvent interface's class, held for its per-realm prototype "
                      "slot");
    agent_state_value("event", &g_key,
                      "the private Symbol Pointer Events 4 §12.1 WheelEvent interface's slot record hangs off");
    agent_state_id("event", &g_ctor_stepid,
                   "Pointer Events 4 §12.1 WheelEvent interface's `constructor(DOMString type, optional "
                   "WheelEventInit eventInitDict = {})`");
    realm_declare_intrinsic(wheel_event_install_protos);
}

void wheel_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for WheelEvent before wheel_event_init declared it");
    prev = JS_GetClassProto(ctx, g_we_class);
    DCHECK(JS_IsNull(prev), "wheel_event_install_protos ran twice in one realm — Web IDL §3.7 \"Interfaces\" "
                            "gives a realm ONE WheelEvent.prototype, and a second leaves every event already "
                            "chained to the first answering out of a discarded object");
    JS_FreeValue(ctx, prev);
    /* `interface WheelEvent : MouseEvent` — THIS realm's MouseEvent.prototype, which the intrinsic declared
       before this one has already built. */
    base = mouse_event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "WheelEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "WheelEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_we_proto, (int)(sizeof(js_we_proto) / sizeof(js_we_proto[0])));
    JS_SetPropertyFunctionList(ctx, proto, js_we_consts,
                               (int)(sizeof(js_we_consts) / sizeof(js_we_consts[0])));
    JS_SetClassProto(ctx, g_we_class, JS_DupValue(ctx, proto));

    /* Web IDL §3.7.1 "Interface object" on THIS realm's global — see ui_event.c. */
    ctor = idl_step_constructor(ctx, "WheelEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the WheelEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyFunctionList(ctx, ctor, js_we_consts,
                               (int)(sizeof(js_we_consts) / sizeof(js_we_consts[0])));
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "WheelEvent", ctor);
    JS_FreeValue(ctx, global);
}

/* THE RUNTIME, NOT A REALM — core/platform.h's release column, reached through event_free. What this gives
   back is the AGENT's: a private Symbol, a class id and this interface's member declarations; every prototype
   it built is in some realm's class-proto slot and goes with that realm. */
void wheel_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. core/events/event.c's event_init calls this component's init on the ONE
       declaration pass and its event_free — which has already asserted its own latch — calls this release
       unconditionally, so the test could never be true and what it could do was hide a release that left the
       latch set. */
    DCHECK(g_ready, "Pointer Events 4 §12.1 WheelEvent interface was released in an agent that never declared "
                    "it — event_init declares every Event subclass on the one unconditional pass");
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because the id doubles
       as the init latch and a carried one names a class in a runtime that is gone. Nothing WEARS this class —
       it exists for its per-realm prototype slot, and every event in this engine is minted by
       core/events/event.c's event_make_proto through JS_NewObjectProto — so there is no finalizer and no
       gc_mark here to owe the JS_GetAnyOpaque the zeroing costs a component whose objects do wear one. */
    g_we_class = 0;
    g_ctor_stepid = -1;
}
