/* THE UIEvent INTERFACE — UI Events §3.2.1, plus §3.5.3's EventModifierInit state.
 *
 *     [Exposed=Window]
 *     interface UIEvent : Event {
 *       constructor(DOMString type, optional UIEventInit eventInitDict = {});
 *       readonly attribute Window? view;
 *       readonly attribute long detail;
 *     };
 *     dictionary UIEventInit : EventInit { Window? view = null; long detail = 0; };
 *     partial interface UIEvent { readonly attribute unsigned long which; };      // §7.1.1
 *     partial dictionary UIEventInit { unsigned long which = 0; };                // §7.1.2
 *     partial interface UIEvent { undefined initUIEvent(...); };                  // §6.1.1
 *
 * WHY IT EXISTS AND WHAT WAS BLOCKED ON IT. It is the base of every event a user interface produces, and
 * nothing that inherits it could exist without it: MouseEvent and KeyboardEvent are `: UIEvent`, so their
 * prototypes chain HERE and their dictionaries inherit UIEventInit's members. §4.5's createEvent lists
 * `uievent`/`uievents` in its table and had to refuse both. And event_target.c's §2.9 step 6.4 could only ask
 * the event's TYPE where the spec asks whether the object IS a MouseEvent — a question no engine without this
 * chain can ask.
 *
 * IT IS A REAL SUBCLASS, like MessageEvent: `UIEvent.prototype.__proto__ === Event.prototype`, so
 * `e instanceof Event` holds and `initEvent` works on one. The base half is event_new_derived's, and the half
 * this file adds is offered to ITS subclasses the same way, through ui_event_new_derived — a three-deep chain
 * is built one level at a time, each level adding only its own slots.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, for the reason event.c gives: a slot written as a
 * property write is captured by the COW delta, so the event's state time-travels with the flow that made it,
 * and the symbol is a brand a page cannot forge.
 *
 * WHY EventModifierInit IS HERE AND NOT IN EITHER SUBCLASS. It is a `dictionary EventModifierInit : UIEventInit`
 * that MouseEvent and KeyboardEvent BOTH derive from, and both declare `getModifierState()` and
 * ctrlKey/shiftKey/altKey/metaKey over the one internal key modifier state it fills. Putting it in either
 * subclass makes the other subclass depend on its sibling; putting it in a file of its own splits a dictionary
 * from the dictionary it inherits. It belongs with UIEventInit, and this file is where UIEventInit is.
 *
 * THE FOUR NAMED MODIFIER ATTRIBUTES ARE NOT A SECOND STATE. §3.5.3 states each of `ctrlKey`, `shiftKey`,
 * `altKey` and `metaKey` as "initializes the attribute … When true, implementations must also initialize the
 * event object's key modifier state such that getModifierState() … must return true", and Pointer Events 4's
 * "set mouse event modifiers" then derives the attributes back OUT of those flags. Two slots kept in step is
 * two answers to one question waiting to disagree, so there is one: the state, and four attributes that read
 * it. `{ctrlKey: true}` and `getModifierState("Control")` are then the same fact by construction. */
#include "check.h"
#include "quickjs.h"
#include "core/events/event.h"
#include "core/events/ui_event.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key;         /* the private Symbol this interface's own slots hang off */
/* PER REALM, for the reason event.c states: a C member runs in the realm that DEFINED it, so one shared
   prototype answers every document out of whichever realm built it first. The class exists for that per-realm
   prototype slot; nothing wears it. */
static JSClassID g_ui_class;
static int       g_ready;
static int       g_ctor_stepid = -1;
static int       g_init_ui_id = -1;

/* §3.5.3's fourteen members and the KEY MODIFIER NAME each one sets. The ten spelled `modifier<Name>` are
   Pointer Events 4's own rule — "the dictionary member's name excluding the prefix modifier" — and the four
   named for an attribute are §3.5.3's sentence about that attribute. One table, so the member list and the
   state can never name different sets. */
static const struct { const char *member; const char *modifier; } UI_MODIFIER[] = {
    { "ctrlKey",            "Control" },
    { "shiftKey",           "Shift" },
    { "altKey",             "Alt" },
    { "metaKey",            "Meta" },
    { "modifierAltGraph",   "AltGraph" },
    { "modifierCapsLock",   "CapsLock" },
    { "modifierFn",         "Fn" },
    { "modifierFnLock",     "FnLock" },
    { "modifierHyper",      "Hyper" },
    { "modifierNumLock",    "NumLock" },
    { "modifierScrollLock", "ScrollLock" },
    { "modifierSuper",      "Super" },
    { "modifierSymbol",     "Symbol" },
    { "modifierSymbolLock", "SymbolLock" },
};
#define UI_MODIFIER_N ((unsigned)(sizeof(UI_MODIFIER) / sizeof(UI_MODIFIER[0])))

JSValue ui_event_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_ui_class);

    DCHECK(!JS_IsNull(proto),
           "UIEvent.prototype was asked for in a realm that never ran ui_event_install_protos — a realm whose "
           "intrinsics were not all installed answers a derived interface's chain out of another document");
    return proto;   /* OWNED */
}

/* §3.2.1's own slot record. An own SLOT, never a property lookup, for the reason event.c gives: a lookup walks
   the prototype chain into the solver's absent-state seam. */
static JSValue ue_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "a UIEvent's slots were asked for before ui_event_init ran");
    if (!JS_IsObject(ev))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &slots, ev, k) <= 0)
        slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return slots;
}

bool ui_event_is(JSContext *ctx, JSValueConst v)
{
    JSValue slots = ue_slots(ctx, v);
    bool ok = JS_IsObject(slots);

    JS_FreeValue(ctx, slots);
    return ok;
}

int32_t ui_event_dict_i32(JSContext *ctx, JSValueConst init, const char *name)
{
    JSValue v = idl_dict_get(ctx, init, name);
    int32_t n = 0;

    /* The declaration has already converted the member to a NUMBER in the type's range, so this runs none of
       the page's code and cannot itself throw. */
    if (!JS_IsUndefined(v))
        JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

uint32_t ui_event_dict_u32(JSContext *ctx, JSValueConst init, const char *name)
{
    JSValue v = idl_dict_get(ctx, init, name);
    uint32_t n = 0;

    if (!JS_IsUndefined(v))
        JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

/* `Window?` — Web IDL §3.2.16 over the `view` member and over initUIEvent's `viewArg`. null and undefined are
   the IDL null; a Window (which in this engine is the realm's global, and is also what `window` hands a page)
   crosses as itself; anything else matches the type not at all, which is Web IDL's own TypeError and not a
   rule this file invents. Answers JS_NULL / an owned dup, or JS_EXCEPTION with the throw live. */
static JSValue ui_view_of(JSContext *ctx, JSValueConst v)
{
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return JS_NULL;
    if (window_proxy_is_window(ctx, v))
        return JS_DupValue(ctx, v);
    return JS_ThrowTypeError(ctx, "a UIEvent's `view` must be a Window or null");
}

/* THE KEY MODIFIER STATE as its own record: a null-prototype object whose keys are the key modifier names that
   are ACTIVE. A null prototype is what makes reading an arbitrary page-supplied name safe — getModifierState
   takes the page's string, and on a record with no prototype a miss is a miss rather than a walk into
   Object.prototype (or into a getter a page put there). Returns JS_EXCEPTION. */
static JSValue ui_modifiers_of(JSContext *ctx, JSValueConst init)
{
    JSValue mods = idl_slots_new(ctx);
    unsigned i;

    if (JS_IsException(mods))
        return mods;
    for (i = 0; i < UI_MODIFIER_N; i++)
        if (idl_dict_bool(ctx, init, UI_MODIFIER[i].member))
            JS_SetPropertyStr(ctx, mods, UI_MODIFIER[i].modifier, JS_TRUE);
    return mods;
}

/* The four own slots, placed on an event whose Event half is already built. Returns -1 with the throw live. */
static int ui_init_slots(JSContext *ctx, JSValueConst ev, JSValueConst init)
{
    JSValue slots, given, view, mods;
    JSAtom k;

    DCHECK(g_ready, "a UIEvent was minted before ui_event_init declared the interface — the slot key it hangs "
                    "its state off is made there");
    slots = idl_slots_new(ctx);
    k = JS_ValueToAtom(ctx, g_key);
    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    given = idl_dict_get(ctx, init, "view");
    view = ui_view_of(ctx, given);
    JS_FreeValue(ctx, given);
    if (JS_IsException(view)) {
        JS_FreeValue(ctx, slots);
        JS_FreeAtom(ctx, k);
        return -1;
    }
    mods = ui_modifiers_of(ctx, init);
    if (JS_IsException(mods)) {
        JS_FreeValue(ctx, view);
        JS_FreeValue(ctx, slots);
        JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "view", view);
    JS_SetPropertyStr(ctx, slots, "detail", JS_NewInt32(ctx, ui_event_dict_i32(ctx, init, "detail")));
    JS_SetPropertyStr(ctx, slots, "which", JS_NewUint32(ctx, ui_event_dict_u32(ctx, init, "which")));
    JS_SetPropertyStr(ctx, slots, "modifiers", mods);
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue ui_event_new_derived(JSContext *ctx, JSValue proto, JSValueConst type, JSValueConst init, bool trusted)
{
    /* §2.2's constructor steps first, with the DERIVED interface's prototype — the three EventInit members are
       read off the same dictionary, because a derived init dictionary inherits them. */
    JSValue ev = event_new_derived(ctx, proto, type,
                                   idl_dict_bool(ctx, init, "bubbles"),
                                   idl_dict_bool(ctx, init, "cancelable"),
                                   idl_dict_bool(ctx, init, "composed"), trusted);

    if (JS_IsException(ev))
        return ev;
    if (ui_init_slots(ctx, ev, init) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

JSValue ui_event_new(JSContext *ctx)
{
    JSValue type = JS_NewString(ctx, ""), ev;

    if (JS_IsException(type))
        return type;
    /* DOM §2.5: creating an event initializes every attribute to its UN-INITIALIZED value and isTrusted to
       true. §3.2.18 makes every member of an ABSENT dictionary absent, and each of this interface's members
       defaults to exactly its un-initialized value — so there is one construction path and no second table of
       defaults to disagree with the first. */
    ev = ui_event_new_derived(ctx, ui_event_proto(ctx), type, JS_UNDEFINED, /*trusted*/ true);
    JS_FreeValue(ctx, type);
    return ev;
}

/* ---- the attributes ----------------------------------------------------------------------------------------- */

enum { UE_VIEW = 0, UE_DETAIL, UE_WHICH };
static const char *const UE_SLOT[] = { "view", "detail", "which" };

static JSValue js_ue_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = ue_slots(ctx, this_val), v;

    DCHECK(magic >= 0 && magic < (int)(sizeof(UE_SLOT) / sizeof(UE_SLOT[0])),
           "a UIEvent attribute was declared with a magic the slot table does not name");
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a UIEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, UE_SLOT[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

bool ui_event_modifier_state(JSContext *ctx, JSValueConst ev, const char *name)
{
    JSValue slots = ue_slots(ctx, ev), mods, v;
    bool on;

    DCHECK(name != NULL && *name, "the internal key modifier state was queried with no key modifier name");
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return false;
    }
    mods = JS_GetPropertyStr(ctx, slots, "modifiers");
    JS_FreeValue(ctx, slots);
    DCHECK(JS_IsObject(mods), "a UIEvent carried no internal key modifier state — every one is built with the "
                              "record, empty when the dictionary set no modifier");
    v = JS_GetPropertyStr(ctx, mods, name);
    JS_FreeValue(ctx, mods);
    on = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return on;
}

JSValue ui_event_get_modifier_state(JSContext *ctx, JSValueConst ev, JSValueConst key_arg)
{
    const char *name;
    bool on;

    DCHECK(ui_event_is(ctx, ev),
           "the shared getModifierState body was reached on something with no UIEvent slots — the interface "
           "that declares the member brand-checks its own receiver first, which is what makes that impossible");
    /* The declaration has already converted `keyArg` to a DOMString, so reading it out runs none of the page's
       code. A name that is not a modifier key is simply not in the state, which is the member's own "false
       otherwise". */
    name = JS_ToCString(ctx, key_arg);
    if (!name)
        return JS_EXCEPTION;
    on = ui_event_modifier_state(ctx, ev, name);
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, on);
}

/* ---- §6.1.1's legacy initializer ----------------------------------------------------------------------------
 *
 * `initUIEvent(typeArg, bubblesArg, cancelableArg, viewArg, detailArg)` — "the same behavior as initEvent()",
 * and then this interface's two attributes. It is how a bundle old enough to use createEvent finishes making
 * one, so the factory row and this member are one feature: without it `createEvent('UIEvent')` answers an
 * event that can never be dispatched, because §4.5 leaves the initialized flag unset and only an initializer
 * sets it. Every interface that inherits UIEvent inherits this member too, which is exactly §6's point that
 * "initializing all the attributes requires calls to two initializer methods". */
static const IdlArgType UE_INIT_UI_ARGS[5] = {
    IDL_DOMSTRING, IDL_BOOLEAN, IDL_BOOLEAN, IDL_ANY /* Window? */, IDL_LONG,
};

static JSValue js_ue_init_ui_event(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    JSValue slots, view;
    int32_t detail = 0;

    (void)magic;
    if (!ui_event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "initUIEvent called on something that is not a UIEvent");
    /* Only `typeArg` is required, and §3.6.2 step 1's check for it is the declaration's. The other four are
       optional and genuinely absent when the page passes fewer. */
    DCHECK(argc >= 1, "initUIEvent's body ran with no typeArg");
    view = ui_view_of(ctx, argc > 3 ? argv[3] : JS_UNDEFINED);
    if (JS_IsException(view))
        return view;
    if (argc > 4 && JS_ToInt32(ctx, &detail, argv[4]) < 0) {
        JS_FreeValue(ctx, view);
        return JS_EXCEPTION;
    }
    /* §2.2's initialise-an-existing-event steps, INCLUDING their early return: an event whose dispatch flag is
       set is left alone, and a derived initializer must not write its own half either. */
    if (!event_reinit(ctx, this_val, argv[0],
                      argc > 1 && JS_ToBool(ctx, argv[1]), argc > 2 && JS_ToBool(ctx, argv[2]))) {
        JS_FreeValue(ctx, view);
        return JS_UNDEFINED;
    }
    slots = ue_slots(ctx, this_val);
    DCHECK(JS_IsObject(slots), "initUIEvent passed its brand check and then found no UIEvent slot record");
    JS_SetPropertyStr(ctx, slots, "view", view);
    JS_SetPropertyStr(ctx, slots, "detail", JS_NewInt32(ctx, detail));
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;
}

/* ---- the constructor ---------------------------------------------------------------------------------------- */

static const IdlArgType UE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember UE_INIT[] = { UI_EVENT_INIT_MEMBERS };

static JSValue js_ue_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor UIEvent requires 'new'");
    /* §3.6.2 step 1's required-argument check is the DECLARATION's — it throws before any body is entered, so
       a body that tested `argc` again would be stating the type system's rule a second time. */
    DCHECK(argc >= 1, "the UIEvent constructor body ran with no type argument");
    /* An event the PAGE constructs is untrusted. */
    return ui_event_new_derived(ctx, ui_event_proto(ctx), argv[0], argc > 1 ? argv[1] : JS_UNDEFINED,
                                /*trusted*/ false);
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_ue_proto[] = {
    JS_CGETSET_MAGIC_DEF("view", js_ue_get, NULL, UE_VIEW),
    JS_CGETSET_MAGIC_DEF("detail", js_ue_get, NULL, UE_DETAIL),
    JS_CGETSET_MAGIC_DEF("which", js_ue_get, NULL, UE_WHICH),
};

void ui_event_init(JSContext *ctx)
{
    JSClassDef d = { "UIEvent" };

    DCHECK(!g_ready, "ui_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "uiEventSlots", false);
    CHECK(!JS_IsException(g_key), "the UIEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_ui_class);
    JS_NewClass(JS_GetRuntime(ctx), g_ui_class, &d);
    g_init_ui_id = idl_method_id(ctx, UE_INIT_UI_ARGS, 5, js_ue_init_ui_event, 0);
    idl_optional_from(1);   /* §6.1.1: every argument but `typeArg` is optional */
    g_ctor_stepid = idl_method_id_dict(ctx, UE_CTOR_ARGS, 2, UE_INIT,
                                       (int)(sizeof(UE_INIT) / sizeof(UE_INIT[0])), js_ue_ctor, 0);
    idl_optional_from(1);   /* §3.2.1: `constructor(DOMString type, optional UIEventInit init = {})` */
    g_ready = 1;
    realm_declare_intrinsic(ui_event_install_protos);
}

void ui_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for UIEvent before ui_event_init declared it");
    prev = JS_GetClassProto(ctx, g_ui_class);
    DCHECK(JS_IsNull(prev), "ui_event_install_protos ran twice in one realm — §3.7 gives a realm ONE "
                            "UIEvent.prototype, and a second leaves every event already chained to the first "
                            "answering out of a discarded object");
    JS_FreeValue(ctx, prev);
    /* THE PROTOTYPE CHAIN IS THE SUBCLASSING, and THIS realm's Event.prototype, because a chain to another
       document's is the same defect one link up. */
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "UIEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "UIEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_ue_proto, (int)(sizeof(js_ue_proto) / sizeof(js_ue_proto[0])));
    idl_install_method(ctx, proto, "initUIEvent", 1, g_init_ui_id);
    JS_SetClassProto(ctx, g_ui_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object, on THIS realm's global — one `UIEvent` per realm, whose `prototype` is the
       prototype this same install just built. Declared into realm.h's ONE list rather than installed from a
       host's hand-written list of globals: a component missing from one host's copy is silently absent in that
       realm with nothing to say so. */
    ctor = idl_step_constructor(ctx, "UIEvent", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the UIEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "UIEvent", ctor);
    JS_FreeValue(ctx, global);
}

void ui_event_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = g_init_ui_id = -1;
}
