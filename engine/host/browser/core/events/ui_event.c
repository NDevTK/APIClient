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
 * `sourceCapabilities` IS NOT UI EVENTS' AND LOOKING FOR ITS SPEC HERE IS WHY THIS PARAGRAPH EXISTS. The
 * flattened IDL files it under UIEvent, and the UI Events draft contains neither that name nor
 * `InputDeviceCapabilities` anywhere in its text. It is the INPUT DEVICE CAPABILITIES specification's, whose
 * editor's draft numbers no sections and whose §"Extensions to the UIEvent interface and UIEventInit
 * dictionary" states the whole of it:
 *
 *     partial interface UIEvent   { readonly attribute InputDeviceCapabilities? sourceCapabilities; };
 *     partial dictionary UIEventInit { InputDeviceCapabilities? sourceCapabilities = null; };
 *
 * THE TYPE IS ITS OWN COMPONENT — core/events/input_device_capabilities.c, which had to exist before this slot
 * could hold anything: a `[NewObject]`-less nullable interface member has exactly two values a page can tell
 * apart, an instance of the interface and null, so installing the attribute over an interface that did not
 * exist would have made `null` the only answer it could ever give — and the spec's own null means "no input
 * device was responsible", a POSITIVE statement about a resize event that must not be indistinguishable from
 * an engine that cannot express anything else. The slot below is one of the four this interface writes, and
 * every subclass gets it from the same write, which is why one component cleared four audited rows.
 *
 * AN EVENT THIS ENGINE CREATES CARRIES `null`, AND THAT IS THE SPEC'S ANSWER RATHER THAN A HEADLESS ONE.
 * §"sourceCapabilities" says "the InputDeviceCapabilities responsible for the generation of this event, or
 * null if no input device was responsible", and DOM §2.5 Constructing events' create-an-event has no device
 * in it — exactly as a browser's own `new UIEvent('x')` and its `resize` both answer null. What a page
 * CONSTRUCTS with the member is carried unchanged, which is the whole of what the IDL defines.
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
#include "core/agent_state.h"
#include "core/events/event.h"
#include "core/events/input_device_capabilities.h"
#include "core/events/ui_event.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's own slots hang off */
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

double ui_event_dict_f64(JSContext *ctx, JSValueConst init, const char *name)
{
    JSValue v = idl_dict_get(ctx, init, name);
    double n = 0.0;

    if (!JS_IsUndefined(v))
        JS_ToFloat64(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

/* `Window? view = null` — UI Events §3.2.1.2 UIEventInit's member, READ OFF THE CONVERTED DICTIONARY. §3.2.15's
   brand and §3.2.20's null rule are the DECLARATION's: the row in ui_event.h states `I` as
   `window_proxy_is_window`, the same predicate the three legacy initializers' `viewArg` positions state, so a
   Window (which in this engine is the realm's global, and is also what `window` hands a page) or a WindowProxy
   crosses as itself and everything else is Web IDL's own TypeError — thrown inside §3.2.17's member loop, at
   `view`'s own place in the read order, before `which`'s step 4.1.3.1 Get and long before this body runs.
   THE ONE SHAPE THE DECLARATION DOES NOT ANSWER FOR IS AN ABSENT DICTIONARY, and it is a POSITIVE statement
   rather than a hole: DOM §2.5 Constructing events' create an event passes JS_UNDEFINED as the init, so no
   walk ran and step 4.1.5 placed no default — and §3.2.1's own un-initialized value for `view` is the null
   that `= null` would have placed. The same reading input_device_capabilities_of_dict makes of the same
   absence, over the member that sorts immediately before this one. */
static JSValue ue_view_of_dict(JSContext *ctx, JSValueConst init)
{
    JSValue v = idl_dict_get(ctx, init, "view");

    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return JS_NULL;
    }
    /* THE BRAND REFUSED A WRONG VALUE BEFORE THIS BODY WAS ENTERED AND DOES NOT SEE AN UNKNOWN ONE: the
       §3.2.17 member loop rewrites a CONCOLIC member's type to IDL_ANY before IDL_INTERFACE_NULLABLE's arm is
       asked, so `{view: <unknown>}` is the one shape that reaches here having passed no brand at all. */
    IDL_DCHECK_MEMBER(JS_IsNull(v) || window_proxy_is_window(ctx, v), v, "view",
                      "`Window?` with a `= null` default — UI Events §3.2.1.2 UIEventInit writes "
                      "`Window? view = null` — branded per member by IdlDictMember::iface_is over "
                      "IDL_INTERFACE_NULLABLE, with the predicate core/frame/window_proxy.h owns");
    return v;
}

/* §3.2.1's `view`, RESOLVED TO ITS REALM — see ui_event.h for why the resolution lives beside the reader that
   wrote the slot. NULL is the IDL null and is a statement, never an absence. */
JSContext *ui_event_view_realm(JSContext *ctx, JSValueConst ev)
{
    JSValue slots = ue_slots(ctx, ev), view;
    JSContext *realm;
    bool own_global;

    DCHECK(JS_IsObject(slots),
           "a UIEvent's `view` was asked for as a realm on something with no UIEvent slots — the interface that "
           "declares the member reading it brand-checks its own receiver first, which is what makes this "
           "impossible");
    view = JS_GetPropertyStr(ctx, slots, "view");
    JS_FreeValue(ctx, slots);
    if (JS_IsNull(view)) {
        JS_FreeValue(ctx, view);
        return NULL;
    }
    /* A WindowProxy NAMES A NAVIGABLE, so the realm is that navigable's active document's — and asking for it
       CRASHES when the navigable belongs to another WASM instance, which is the correct crash and not a gap
       this member may route around: a cross-instance read is a SUSPEND of the reading flow, and a C activation
       inside a getter is exactly where CLAUDE.md §Security says an answer that has to suspend cannot be
       produced by reading a property from C. */
    if (window_proxy_is(view)) {
        realm = window_proxy_realm(ctx, view);
        JS_FreeValue(ctx, view);
        DCHECK(realm != NULL,
               "a WindowProxy this agent holds resolved to no realm — window_proxy_realm materializes the "
               "active document's realm and crashes for a peer's, so a NULL here is a third state neither of "
               "those two produces");
        return realm;
    }
    /* The only other shape `Window?` admits is a realm's OWN GLOBAL, and window_proxy_is_window admits one
       only for the realm it is asked in — so when this holds, `ctx` IS that Window's realm and nothing else
       can be. */
    own_global = window_proxy_is_window(ctx, view);
    JS_FreeValue(ctx, view);
    DCHECK(own_global,
           "a UIEvent's `view` is a Window that is neither a WindowProxy nor THIS realm's global, and a bare "
           "global carries no route back to the realm it belongs to. It reaches here when a `view` set in one "
           "realm is read through another realm's prototype. THE MISSING PRIMITIVE IS A Window -> JSContext MAP "
           "FOR A REALM'S OWN GLOBAL — a WindowProxy already has one (window_proxy_realm) because it names a "
           "navigable, and a global does not. Answering with the RUNNING realm would report the reader's "
           "viewport as the event's Window's, which is the one-fact-answered-from-one-place defect itself");
    return ctx;
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
    JSValue slots, view, mods, source;
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
    /* `Window? view = null`, READ. The declaration has branded it, so this cannot throw and needs no unwind
       arm — the same standing as the `sourceCapabilities` read below, which is what the two members having the
       same kind of declared type is supposed to mean. */
    view = ue_view_of_dict(ctx, init);
    mods = ui_modifiers_of(ctx, init);
    if (JS_IsException(mods)) {
        JS_FreeValue(ctx, view);
        JS_FreeValue(ctx, slots);
        JS_FreeAtom(ctx, k);
        return -1;
    }
    /* INPUT DEVICE CAPABILITIES §"Extensions to the UIEvent interface and UIEventInit dictionary". The
       declaration has already branded it, so this read runs none of the page's code and cannot throw — which
       is now true of `view` too, and the one allocation left that can fail is the modifier record's. */
    source = input_device_capabilities_of_dict(ctx, init, "sourceCapabilities");
    JS_SetPropertyStr(ctx, slots, "view", view);
    JS_SetPropertyStr(ctx, slots, "detail", JS_NewInt32(ctx, ui_event_dict_i32(ctx, init, "detail")));
    JS_SetPropertyStr(ctx, slots, "which", JS_NewUint32(ctx, ui_event_dict_u32(ctx, init, "which")));
    JS_SetPropertyStr(ctx, slots, "sourceCapabilities", source);
    JS_SetPropertyStr(ctx, slots, "modifiers", mods);
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue ui_event_new_derived(JSContext *ctx, JSValue proto, JSValueConst type, JSValueConst init, bool trusted)
{
    /* DOM §2.5 "Constructing events" first, with the DERIVED interface's prototype — the three EventInit members are
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
       true. Web IDL §3.2.17 makes every member of an ABSENT dictionary absent, and each of this interface's members
       defaults to exactly its un-initialized value — so there is one construction path and no second table of
       defaults to disagree with the first. */
    ev = ui_event_new_derived(ctx, ui_event_proto(ctx), type, JS_UNDEFINED, /*trusted*/ true);
    JS_FreeValue(ctx, type);
    return ev;
}

/* ---- the attributes ----------------------------------------------------------------------------------------- */

enum { UE_VIEW = 0, UE_DETAIL, UE_WHICH, UE_SOURCE_CAPABILITIES };
static const char *const UE_SLOT[] = { "view", "detail", "which", "sourceCapabilities" };

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
/* `optional Window? viewArg = null` IS A DECLARED TYPE and it was IDL_ANY, so §3.2.15's brand test and
   §3.2.20's null rule were both the body's — the brand written out in a body that a declared type exists to
   replace. `Window` is one of the interfaces no JSClassID names (the realm's own global OR a WindowProxy —
   core/frame/window_proxy.h states why those are one type test), so the position states its interface as
   idl_arg_iface's PREDICATE rather than as a class. UIEventInit's `view` states the SAME predicate as
   IdlDictMember::iface_is, so this engine has one answer to "is this a Window" and no body left that asks it
   by hand. */
static const IdlArgType UE_INIT_UI_ARGS[5] = {
    IDL_DOMSTRING, IDL_BOOLEAN, IDL_BOOLEAN, IDL_INTERFACE_NULLABLE, IDL_LONG,
};

/* §2.2's initialise-an-existing-event steps and this interface's `view`, INCLUDING the early return: an event
   whose dispatch flag is set is left alone, and a derived initializer must not write its own half either. It
   is the prefix all three legacy initializers share — see ui_event.h. */
bool ui_event_reinit(JSContext *ctx, JSValueConst ev, JSValueConst type, bool bubbles, bool cancelable,
                     JSValue view)
{
    JSValue slots;

    DCHECK(ui_event_is(ctx, ev),
           "the shared legacy-initializer prefix was reached on something with no UIEvent slots — the interface "
           "that declares the member brand-checks its own receiver first, which is what makes that impossible");
    if (!event_reinit(ctx, ev, type, bubbles, cancelable)) {
        JS_FreeValue(ctx, view);
        return false;
    }
    slots = ue_slots(ctx, ev);
    DCHECK(JS_IsObject(slots), "a legacy initializer passed its brand check and then found no UIEvent slots");
    JS_SetPropertyStr(ctx, slots, "view", view);
    JS_FreeValue(ctx, slots);
    return true;
}

void ui_event_set_detail(JSContext *ctx, JSValueConst ev, int32_t detail)
{
    JSValue slots = ue_slots(ctx, ev);

    DCHECK(JS_IsObject(slots), "a UIEvent's `detail` was written on something with no UIEvent slots");
    JS_SetPropertyStr(ctx, slots, "detail", JS_NewInt32(ctx, detail));
    JS_FreeValue(ctx, slots);
}

void ui_event_set_modifier_state(JSContext *ctx, JSValueConst ev, const char *name, bool on)
{
    JSValue slots = ue_slots(ctx, ev), mods;

    DCHECK(name != NULL && *name, "the internal key modifier state was written with no key modifier name");
    DCHECK(JS_IsObject(slots), "the internal key modifier state was written on something with no UIEvent slots");
    mods = JS_GetPropertyStr(ctx, slots, "modifiers");
    JS_FreeValue(ctx, slots);
    DCHECK(JS_IsObject(mods), "a UIEvent carried no internal key modifier state — every one is built with the "
                              "record, empty when the dictionary set no modifier");
    /* THE RECORD IS THE SET OF ACTIVE NAMES, so switching a modifier OFF removes its key rather than storing a
       false beside the true ones — the same shape `ui_modifiers_of` builds, so the state has one spelling
       whichever route wrote it. A delete is captured by the per-flow COW delta exactly as a write is
       (quickjs.c's delete_property records the slot's pre-delete value first), so an initializer running in one
       flow does not clear a modifier in its sibling. */
    if (on) {
        JS_SetPropertyStr(ctx, mods, name, JS_TRUE);
    } else {
        JSAtom a = JS_NewAtom(ctx, name);

        DCHECK(a != JS_ATOM_NULL, "a key modifier name could not be interned");
        JS_DeleteProperty(ctx, mods, a, 0);
        JS_FreeAtom(ctx, a);
    }
    JS_FreeValue(ctx, mods);
}

static JSValue js_ue_init_ui_event(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    JSValue view;
    int32_t detail = 0;

    (void)magic;
    if (!ui_event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "initUIEvent called on something that is not a UIEvent");
    /* Only `typeArg` is required, and §3.6 step 5's check for it is the declaration's. The other four are
       optional and genuinely absent when the page passes fewer. */
    DCHECK(argc >= 1, "initUIEvent's body ran with no typeArg");
    /* §3.2.15's brand and §3.2.20's null are the DECLARATION's — what arrives is the IDL null or a Window, and
       §3.6 step 16.1 places the declared `= null` for a call that never reached the position, which is why the
       position is here whatever the page passed. `view` is CONSUMED by ui_event_reinit, so it is dup'd. */
    DCHECK(argc > 3, "initUIEvent's `viewArg` declares §6.1.1's own `= null`, so §3.6 step 16.1 extends the "
                     "conversion to it and the body is handed the position for every call");
    view = JS_DupValue(ctx, argv[3]);
    if (argc > 4 && JS_ToInt32(ctx, &detail, argv[4]) < 0) {
        JS_FreeValue(ctx, view);
        return JS_EXCEPTION;
    }
    if (!ui_event_reinit(ctx, this_val, argv[0],
                         argc > 1 && JS_ToBool(ctx, argv[1]), argc > 2 && JS_ToBool(ctx, argv[2]), view))
        return JS_UNDEFINED;
    ui_event_set_detail(ctx, this_val, detail);
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
    /* §3.6 step 5's required-argument check is the DECLARATION's — it throws before any body is entered, so
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
    /* INPUT DEVICE CAPABILITIES §"Extensions to the UIEvent interface and UIEventInit dictionary" — declared
       on UIEvent, so FocusEvent, MouseEvent and KeyboardEvent inherit it from this one install. */
    JS_CGETSET_MAGIC_DEF("sourceCapabilities", js_ue_get, NULL, UE_SOURCE_CAPABILITIES),
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
    /* UI Events §6.1.1 Initializers for interface UIEvent: `optional Window? viewArg = null`. The predicate is
       core/frame/window_proxy.h's, which is the component that owns what a Window IS in this engine. */
    idl_arg_iface(3, window_proxy_is_window, "Window");
    /* AND ITS `= null`, so §3.6 step 16.1 places the IDL's own default for a call that never reached the
       position — the body would otherwise have to decide what an absent `viewArg` means, which is the IDL's
       declaration re-derived in a body. */
    idl_arg_default(3, IDL_DEFAULT_NULL, NULL);
    g_ctor_stepid = idl_method_id_dict(ctx, UE_CTOR_ARGS, 2, UE_INIT,
                                       (int)(sizeof(UE_INIT) / sizeof(UE_INIT[0])), js_ue_ctor, 0);
    idl_optional_from(1);   /* §3.2.1: `constructor(DOMString type, optional UIEventInit init = {})` */
    /* THE DECLARATION-WIDE CLASS, the brand of exactly ONE of UIEventInit's two interface-typed members:
       `sourceCapabilities`. `view` states §3.2.15's `I` as its own realm-taking predicate on the member row
       (ui_event.h), which idl_member_implements takes in preference to the class — so this line and that row
       never decide the same member and the seal's "twice" refusal has nothing to refuse. */
    idl_iface_brand(input_device_capabilities_class());
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — AND IT NAMES THE `event` ROW, NOT THIS FILE.
       core/agent_state.h: a sub-component names the row whose RELEASE gives its slots back, which for every
       Event subclass is core/platform.c's `event` row — event_init calls this init and event_free calls this
       release. Nothing here was declared at all, so the pairing's own arm — does anybody release this? — was
       never asked about any of these. */
    agent_state_flag("event", &g_ready,
                     "UI Events §3.2.1 Interface UIEvent's declaration latch");
    agent_state_class("event", &g_ui_class,
                      "UI Events §3.2.1 Interface UIEvent's class, held for its per-realm prototype slot");
    agent_state_value("event", &g_key,
                      "the private Symbol UI Events §3.2.1 Interface UIEvent's slot record hangs off");
    agent_state_id("event", &g_ctor_stepid,
                   "UI Events §3.2.1 Interface UIEvent's `constructor(DOMString type, optional UIEventInit "
                   "eventInitDict = {})`");
    agent_state_id("event", &g_init_ui_id,
                   "UI Events §6.1.1 Initializers for interface UIEvent's `initUIEvent(typeArg, bubblesArg, "
                   "cancelableArg, viewArg, detailArg)`");
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
    idl_install_method(ctx, proto, "initUIEvent", g_init_ui_id);
    JS_SetClassProto(ctx, g_ui_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object, on THIS realm's global — one `UIEvent` per realm, whose `prototype` is the
       prototype this same install just built. Declared into realm.h's ONE list rather than installed from a
       host's hand-written list of globals: a component missing from one host's copy is silently absent in that
       realm with nothing to say so. */
    ctor = idl_step_constructor(ctx, "UIEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the UIEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "UIEvent", ctor);
    JS_FreeValue(ctx, global);
}

/* THE RUNTIME, NOT A REALM — core/platform.h's release column, reached through event_free. What this
   gives back is the AGENT's: a private Symbol, a class id and this interface's member declarations; every
   prototype it built is in some realm's class-proto slot and goes with that realm. */
void ui_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. core/events/event.c's event_init calls this component's init on the ONE
       declaration pass and its event_free — which has already asserted its own latch — calls this release
       unconditionally, so the test could never be true and what it could do was hide a release that left the
       latch set. */
    DCHECK(g_ready, "UI Events §3.2.1 Interface UIEvent was released in an agent that never declared it — "
                    "event_init declares every Event subclass on the one unconditional pass");
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because the id doubles
       as the init latch and a carried one names a class in a runtime that is gone. Nothing WEARS this class —
       it exists for its per-realm prototype slot, and every event in this engine is minted by
       core/events/event.c's event_make_proto through JS_NewObjectProto — so there is no finalizer and no
       gc_mark here to owe the JS_GetAnyOpaque the zeroing costs a component whose objects do wear one. */
    g_ui_class = 0;
    g_ctor_stepid = g_init_ui_id = -1;
}
