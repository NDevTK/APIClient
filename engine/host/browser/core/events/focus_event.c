/* THE FocusEvent INTERFACE — UI Events §3.3.1.
 *
 *     [Exposed=Window]
 *     interface FocusEvent : UIEvent {
 *       constructor(DOMString type, optional FocusEventInit eventInitDict = {});
 *       readonly attribute EventTarget? relatedTarget;
 *     };
 *     dictionary FocusEventInit : UIEventInit {
 *       EventTarget? relatedTarget = null;
 *     };
 *
 * WHAT WAS BLOCKED ON IT: THE WHOLE OF HTML §6.6. "To fire a focus event named e at an element t with a given
 * related target r, fire an event named e at t, USING FocusEvent, with the relatedTarget attribute initialized
 * to r, the view attribute initialized to t's node document's relevant global object, and the composed flag
 * set" — so every focus change in §6.6.4's focus update steps mints one of these, and core/html/focus.c
 * therefore ABORTED at that mint. `element.focus()`, `element.blur()` and `window.focus()` all end in those
 * steps, which means the focus model landed complete and could not fire a single event: a page's `focus`,
 * `blur`, `focusin` and `focusout` handlers — a modal's focus trap, a form's validate-on-blur, a router's
 * restore-focus-on-navigate — were unreachable code. §4.5's createEvent lists `focusevent` and had to refuse it.
 *
 * IT INHERITS UIEvent, WHICH IS WHERE ITS OTHER ATTRIBUTES ARE. `view` and `detail` are UIEvent's, filled by
 * ui_event_new_derived from the same dictionary, because FocusEventInit inherits UIEventInit. This file adds
 * ONE member, and the value that member reads is not this file's either:
 *
 * relatedTarget IS THE EVENT'S, exactly as it is for MouseEvent. DOM §2.2 gives every Event an associated
 * relatedTarget and says other specifications only "define a relatedTarget attribute" over it — which is why
 * §2.9 step 4 RETARGETS it at every path item without knowing this interface exists. A `focus` event whose
 * related target is inside a shadow tree must report the tree's HOST to a listener outside it, and that is a
 * property of the value being the EVENT'S and not this interface's. A slot of its own would be a second value
 * retargeting never reaches, and the two would disagree exactly where the standard is careful.
 *
 * SO THIS INTERFACE HAS NO ATTRIBUTE STATE, AND ITS SLOT RECORD IS THE BRAND AND NOTHING ELSE. Web IDL
 * §3.7.6 Attributes makes an interface's accessor check that its receiver implements it — `FocusEvent
 * .prototype`'s
 * relatedTarget getter called on a `new UIEvent('x')` is a TypeError and not `null` — and the brand a page
 * cannot forge is an own slot under a private Symbol, for the reason event.c gives. The record is EMPTY
 * because the interface adds no state; it exists because the QUESTION "is this a FocusEvent" has to have an
 * answer, and an object's prototype is not one (a page can Object.create anything). */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/events/input_device_capabilities.h"
#include "core/events/focus_event.h"
#include "core/events/ui_event.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's brand hangs off */
static JSClassID g_fe_class;    /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

JSValue focus_event_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_fe_class);

    DCHECK(!JS_IsNull(proto),
           "FocusEvent.prototype was asked for in a realm that never ran focus_event_install_protos");
    return proto;   /* OWNED */
}

static JSValue fe_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "a FocusEvent's brand was asked for before focus_event_init ran");
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

bool focus_event_is(JSContext *ctx, JSValueConst v)
{
    JSValue slots = fe_slots(ctx, v);
    bool ok = JS_IsObject(slots);

    JS_FreeValue(ctx, slots);
    return ok;
}

/* The brand, on an event whose Event and UIEvent halves are already built, plus the one value that is the
   EVENT'S: §2.2's relatedTarget. Returns -1 with the throw live. */
static int fe_init_slots(JSContext *ctx, JSValueConst ev, JSValueConst init)
{
    JSValue slots, given, related;
    JSAtom k;

    DCHECK(g_ready, "a FocusEvent was minted before focus_event_init declared the interface — the slot key it "
                    "hangs its brand off is made there");
    slots = idl_slots_new(ctx);
    k = JS_ValueToAtom(ctx, g_key);
    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    /* `EventTarget? relatedTarget = null` — the type's own conversion, which is event_target.c's because the
       question it asks is "does this implement EventTarget". */
    given = idl_dict_get(ctx, init, "relatedTarget");
    related = event_target_nullable_of(ctx, given, "a FocusEvent's `relatedTarget`");
    JS_FreeValue(ctx, given);
    if (JS_IsException(related)) {
        JS_FreeValue(ctx, slots);
        JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    /* §2.2's associated relatedTarget, on the EVENT — see the file comment. */
    event_set_related_target(ctx, ev, related);
    JS_FreeValue(ctx, related);
    return 0;
}

static JSValue focus_event_new_derived(JSContext *ctx, JSValue proto, JSValueConst type, JSValueConst init,
                                       bool trusted)
{
    JSValue ev = ui_event_new_derived(ctx, proto, type, init, trusted);

    if (JS_IsException(ev))
        return ev;
    if (fe_init_slots(ctx, ev, init) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

JSValue focus_event_new(JSContext *ctx)
{
    JSValue type = JS_NewString(ctx, ""), ev;

    if (JS_IsException(type))
        return type;
    /* DOM §2.5: every attribute at its un-initialized value, isTrusted true — which an ABSENT dictionary
       gives, member for member (Web IDL §3.2.17), so there is one construction path and no second table of defaults.
       §3.3.1 states the un-initialized value of `relatedTarget` as null, which is what FocusEventInit's own
       `= null` produces here. */
    ev = focus_event_new_derived(ctx, focus_event_proto(ctx), type, JS_UNDEFINED, /*trusted*/ true);
    JS_FreeValue(ctx, type);
    return ev;
}

JSValue focus_event_new_to_fire(JSContext *ctx, const char *type, bool bubbles,
                                JSValueConst related, JSValueConst view)
{
    JSValue init, t, ev;

    DCHECK(g_ready, "HTML §6.6.4's fire a focus event ran before focus_event_init declared the interface");
    DCHECK(type != NULL && *type, "HTML §6.6.4's fire a focus event was given no event name");
    /* THE VIEW IS THIS REALM'S OWN GLOBAL, which is what makes minting here right rather than convenient:
       §6.6.4 initializes `view` to t's node document's relevant global object, and UIEvent's `Window?`
       conversion accepts a Window of the realm it runs in. A mint from the wrong realm fails that conversion,
       so the assert names the actual invariant instead of letting a TypeError report it as the page's fault. */
    DCHECK(JS_IsNull(view) || window_proxy_is_window(ctx, view),
           "HTML §6.6.4's fire a focus event was given a `view` that is not this realm's Window — the event "
           "belongs to the relevant realm of its target, and `view` is that document's relevant global object");
    /* THE CONVERTED DICTIONARY, which is what a declared member's conversion hands a body: a null-prototyped
       record carrying the members that EXIST, each already an engine value of its IDL type. Building it here
       runs none of the page's code, because nothing of the page's is on it — and it is the same record
       `new FocusEvent(type, init)` reaches ui_event_new_derived with, so the engine's own focus events and a
       page's constructed ones are one construction path. */
    init = idl_slots_new(ctx);
    if (JS_IsException(init))
        return init;
    JS_SetPropertyStr(ctx, init, "bubbles", JS_NewBool(ctx, bubbles));
    JS_SetPropertyStr(ctx, init, "composed", JS_TRUE);            /* "and the composed flag set" */
    JS_SetPropertyStr(ctx, init, "view", JS_DupValue(ctx, view));
    JS_SetPropertyStr(ctx, init, "relatedTarget", JS_DupValue(ctx, related));
    t = JS_NewString(ctx, type);
    if (JS_IsException(t)) {
        JS_FreeValue(ctx, init);
        return t;
    }
    /* An event the ENGINE fires is TRUSTED — the one thing that tells a page a focus really moved. */
    ev = focus_event_new_derived(ctx, focus_event_proto(ctx), t, init, /*trusted*/ true);
    JS_FreeValue(ctx, t);
    JS_FreeValue(ctx, init);
    return ev;
}

/* ---- the attribute ------------------------------------------------------------------------------------------ */

/* §2.2's relatedTarget, read off the EVENT — the value §2.9 step 4 retargets. */
static JSValue js_fe_get_related_target(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!focus_event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "FocusEvent.relatedTarget was read on something that is not one");
    return event_related_target(ctx, this_val);
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional FocusEventInit eventInitDict = {})`. FocusEventInit inherits UIEventInit
 * inherits EventInit — and NOT EventModifierInit, which is the difference from MouseEventInit and KeyboardEventInit
 * and is why this list splices only the one shared macro. §3.2.17 reads the INHERITED members first and each
 * dictionary's own lexicographically among THEMSELVES, so `relatedTarget` sorts after every inherited member
 * whatever its spelling: THE LEVEL is what makes this list the spec's read order, and the order is observable
 * because a page pins it by throwing from one member's getter. */
static const IdlArgType FE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember FE_INIT[] = {
    UI_EVENT_INIT_MEMBERS,
    { "relatedTarget", IDL_ANY, false, NULL, 2 },
};

static JSValue js_fe_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor FocusEvent requires 'new'");
    DCHECK(argc >= 1, "the FocusEvent constructor body ran with no type argument — §3.6 step 5 is the "
                      "declaration's and throws before any body is entered");
    /* An event the PAGE constructs is untrusted. */
    return focus_event_new_derived(ctx, focus_event_proto(ctx), argv[0], argc > 1 ? argv[1] : JS_UNDEFINED,
                                   /*trusted*/ false);
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_fe_proto[] = {
    JS_CGETSET_MAGIC_DEF("relatedTarget", js_fe_get_related_target, NULL, 0),
};

void focus_event_init(JSContext *ctx)
{
    JSClassDef d = { "FocusEvent" };

    DCHECK(!g_ready, "focus_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "focusEventSlots", false);
    CHECK(!JS_IsException(g_key), "the FocusEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_fe_class);
    JS_NewClass(JS_GetRuntime(ctx), g_fe_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, FE_CTOR_ARGS, 2, FE_INIT,
                                       (int)(sizeof(FE_INIT) / sizeof(FE_INIT[0])), js_fe_ctor, 0);
    idl_optional_from(1);   /* `constructor(DOMString type, optional FocusEventInit eventInitDict = {})` */
    idl_iface_brand(input_device_capabilities_class());   /* FocusEventInit's one interface-typed member,
                                                             UIEventInit's `sourceCapabilities` */
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — AND IT NAMES THE `event` ROW, NOT THIS FILE.
       core/agent_state.h: a sub-component names the row whose RELEASE gives its slots back, which for every
       Event subclass is core/platform.c's `event` row — event_init calls this init and event_free calls this
       release. Nothing here was declared at all, so the pairing's own arm — does anybody release this? — was
       never asked about any of these. */
    agent_state_flag("event", &g_ready,
                     "UI Events §3.3.1 Interface FocusEvent's declaration latch");
    agent_state_class("event", &g_fe_class,
                      "UI Events §3.3.1 Interface FocusEvent's class, held for its per-realm prototype slot");
    agent_state_value("event", &g_key,
                      "the private Symbol UI Events §3.3.1 Interface FocusEvent's brand hangs off");
    agent_state_id("event", &g_ctor_stepid,
                   "UI Events §3.3.1 Interface FocusEvent's `constructor(DOMString type, optional FocusEventInit "
                   "eventInitDict = {})`");
    realm_declare_intrinsic(focus_event_install_protos);
}

void focus_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for FocusEvent before focus_event_init declared it");
    prev = JS_GetClassProto(ctx, g_fe_class);
    DCHECK(JS_IsNull(prev), "focus_event_install_protos ran twice in one realm — §3.7 gives a realm ONE "
                            "FocusEvent.prototype, and a second leaves every event already chained to the "
                            "first answering out of a discarded object");
    JS_FreeValue(ctx, prev);
    /* `interface FocusEvent : UIEvent` — THIS realm's UIEvent.prototype, which the intrinsic declared before
       this one has already built. */
    base = ui_event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "FocusEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "FocusEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_fe_proto, (int)(sizeof(js_fe_proto) / sizeof(js_fe_proto[0])));
    JS_SetClassProto(ctx, g_fe_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object on THIS realm's global — see ui_event.c. */
    ctor = idl_step_constructor(ctx, "FocusEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the FocusEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "FocusEvent", ctor);
    JS_FreeValue(ctx, global);
}

/* THE RUNTIME, NOT A REALM — core/platform.h's release column, reached through event_free. What this
   gives back is the AGENT's: a private Symbol, a class id and this interface's member declarations; every
   prototype it built is in some realm's class-proto slot and goes with that realm. */
void focus_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. core/events/event.c's event_init calls this component's init on the ONE
       declaration pass and its event_free — which has already asserted its own latch — calls this release
       unconditionally, so the test could never be true and what it could do was hide a release that left the
       latch set. */
    DCHECK(g_ready, "UI Events §3.3.1 Interface FocusEvent was released in an agent that never declared it — "
                    "event_init declares every Event subclass on the one unconditional pass");
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because the id doubles
       as the init latch and a carried one names a class in a runtime that is gone. Nothing WEARS this class —
       it exists for its per-realm prototype slot, and every event in this engine is minted by
       core/events/event.c's event_make_proto through JS_NewObjectProto — so there is no finalizer and no
       gc_mark here to owe the JS_GetAnyOpaque the zeroing costs a component whose objects do wear one. */
    g_fe_class = 0;
    g_ctor_stepid = -1;
}
