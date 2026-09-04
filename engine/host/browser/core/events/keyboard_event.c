/* THE KeyboardEvent INTERFACE — UI Events §3.5.1.
 *
 *     [Exposed=Window]
 *     interface KeyboardEvent : UIEvent {
 *       constructor(DOMString type, optional KeyboardEventInit eventInitDict = {});
 *       const unsigned long DOM_KEY_LOCATION_STANDARD = 0x00;   // LEFT 0x01, RIGHT 0x02, NUMPAD 0x03
 *       readonly attribute DOMString key;   readonly attribute DOMString code;
 *       readonly attribute unsigned long location;
 *       readonly attribute boolean ctrlKey; readonly attribute boolean shiftKey;
 *       readonly attribute boolean altKey;  readonly attribute boolean metaKey;
 *       readonly attribute boolean repeat;  readonly attribute boolean isComposing;
 *       boolean getModifierState(DOMString keyArg);
 *     };
 *     dictionary KeyboardEventInit : EventModifierInit {
 *       DOMString key = ""; DOMString code = ""; unsigned long location = 0;
 *       boolean repeat = false; boolean isComposing = false;
 *     };
 *     partial interface KeyboardEvent { readonly attribute unsigned long charCode; keyCode; };   // §7.2
 *     partial dictionary KeyboardEventInit { unsigned long charCode = 0; keyCode = 0; };         // §7.3
 *
 * WHY IT EXISTS AND WHAT WAS BLOCKED ON IT. `onkeydown`/`onkeyup`/`onkeypress` are already installed as event
 * handler IDL attributes, and nothing could ever be fired at them: a keyboard event IS a KeyboardEvent, and
 * the interface did not exist — so a bundle's whole keyboard path (a shortcut table, a search-as-you-type
 * request, an Enter-submits handler) was unreachable code. §4.5's createEvent lists `keyboardevent` and had to
 * refuse it, and `new KeyboardEvent('keydown', {key: 'Enter'})` — how a page tests exactly that path — was a
 * missing global.
 *
 * THE LEGACY ATTRIBUTES ARE HERE ON PURPOSE. §7.2/§7.3 are non-normative, and `e.keyCode === 13` is still how
 * an enormous amount of shipped code reads a key. The spec's own words for this section are that these
 * features "MAY be present in user agents for compatibility with legacy content" and that "for
 * implementations which do support these attributes … it is suggested that the definitions provided in this
 * section be used" — so they are declared exactly as §7.3 declares them, initialized from the dictionary, and
 * NOT invented from `key` (the spec explicitly "does not define values for either keyCode or charCode").
 *
 * ctrlKey/shiftKey/altKey/metaKey AND getModifierState ARE NOT THIS FILE'S STATE — they are the shared
 * internal key modifier state EventModifierInit fills, which ui_event.c holds because MouseEvent declares the
 * same members over the same state. This file declares its own members over it, which is what the IDL does.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, for the reason event.c gives: a property write is
 * captured by the COW delta, so the event's state time-travels, and the symbol is a brand a page cannot forge.
 *
 * `initKeyboardEvent` is §6.1.2, with TEN arguments — the legacy initializer a bundle old enough to use
 * `document.createEvent('KeyboardEvent')` finishes making one with, and the only thing that sets §2.2's
 * initialized flag on an event §4.5 produced. */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/input_device_capabilities.h"
#include "core/events/keyboard_event.h"
#include "core/events/ui_event.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's own slots hang off */
static JSClassID g_ke_class;    /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;
static int       g_modifier_state_id = -1;
static int       g_init_kb_id = -1;

JSValue keyboard_event_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_ke_class);

    DCHECK(!JS_IsNull(proto),
           "KeyboardEvent.prototype was asked for in a realm that never ran keyboard_event_install_protos");
    return proto;   /* OWNED */
}

static JSValue ke_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "a KeyboardEvent's slots were asked for before keyboard_event_init ran");
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

bool keyboard_event_is(JSContext *ctx, JSValueConst v)
{
    JSValue slots = ke_slots(ctx, v);
    bool ok = JS_IsObject(slots);

    JS_FreeValue(ctx, slots);
    return ok;
}

/* A `DOMString` member whose IDL default is the empty string — which is also the un-initialized value of both
   attributes that take one here. The declaration has already converted it, so this reads the record it built
   and runs none of the page's code. Returns an OWNED string, or JS_EXCEPTION.
   BOTH CALLERS ARE IN ke_init_slots AND BOTH WANT THE SAME THING — `key` and `code` are one contract (store
   the member, hand it back from the getter), which is why one helper serves them and why the assert below can
   be written once without losing which member it is about: the caller's own `name` is in the message.
   UNKNOWN EXTERNAL INPUT IS CARRIED, AND THAT IS THE POINT RATHER THAN A HOLE. Web IDL §3.2.17 Dictionary
   types' member loop crosses an unknown as ITSELF before any type arm is asked, so what arrives here for
   `new KeyboardEvent("keydown", {key: location.hash})` wears an ordinary Object. This used to be asserted
   against, with a message saying the member "arrived unconverted" — which was the crossing being reported as
   a failure of the conversion that performs it, and the cost was not the false account but the ABORT: a
   DOMString member's whole job here is to be STORED and handed back by the `key` getter, so a crossed one
   rides the slot to whatever reads `ev.key`, and a flow that sinks it (`el.innerHTML = ev.key`) is an @S
   derivation this engine can solve. Refusing the value ends that flow at the constructor. Nothing coerces it
   on this path: the value is placed in the slot and js_ke_get returns the slot, so the taint survives the
   round trip intact. */
static JSValue ke_dict_str(JSContext *ctx, JSValueConst init, const char *name)
{
    JSValue v = idl_dict_get(ctx, init, name);

    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return JS_NewString(ctx, "");
    }
    DCHECKF(JS_IsString(v) || concolic_is(v),
            "KeyboardEventInit's `%s` is neither a string nor unknown external input — the declaration "
            "converts it and Web IDL §3.2.17 Dictionary types' member loop crosses an unknown one, so those "
            "are the two states that exist here and a third means the conversion was skipped",
            name);
    return v;
}

/* The seven own slots, placed on an event whose Event and UIEvent halves are already built. Returns -1 with
   the throw live. */
static int ke_init_slots(JSContext *ctx, JSValueConst ev, JSValueConst init)
{
    JSValue slots, key_v, code_v;
    JSAtom k;

    DCHECK(g_ready, "a KeyboardEvent was minted before keyboard_event_init declared the interface — the slot "
                    "key it hangs its state off is made there");
    slots = idl_slots_new(ctx);
    k = JS_ValueToAtom(ctx, g_key);
    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    key_v = ke_dict_str(ctx, init, "key");
    code_v = ke_dict_str(ctx, init, "code");
    if (JS_IsException(key_v) || JS_IsException(code_v)) {
        JS_FreeValue(ctx, key_v);
        JS_FreeValue(ctx, code_v);
        JS_FreeValue(ctx, slots);
        JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "key", key_v);
    JS_SetPropertyStr(ctx, slots, "code", code_v);
    JS_SetPropertyStr(ctx, slots, "location", JS_NewUint32(ctx, ui_event_dict_u32(ctx, init, "location")));
    JS_SetPropertyStr(ctx, slots, "repeat", JS_NewBool(ctx, idl_dict_bool(ctx, init, "repeat")));
    JS_SetPropertyStr(ctx, slots, "isComposing", JS_NewBool(ctx, idl_dict_bool(ctx, init, "isComposing")));
    JS_SetPropertyStr(ctx, slots, "charCode", JS_NewUint32(ctx, ui_event_dict_u32(ctx, init, "charCode")));
    JS_SetPropertyStr(ctx, slots, "keyCode", JS_NewUint32(ctx, ui_event_dict_u32(ctx, init, "keyCode")));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

static JSValue keyboard_event_new_derived(JSContext *ctx, JSValue proto, JSValueConst type, JSValueConst init,
                                          bool trusted)
{
    JSValue ev = ui_event_new_derived(ctx, proto, type, init, trusted);

    if (JS_IsException(ev))
        return ev;
    if (ke_init_slots(ctx, ev, init) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

JSValue keyboard_event_new(JSContext *ctx)
{
    JSValue type = JS_NewString(ctx, ""), ev;

    if (JS_IsException(type))
        return type;
    /* DOM §2.5: every attribute at its un-initialized value, isTrusted true — which an ABSENT dictionary
       gives, member for member. */
    ev = keyboard_event_new_derived(ctx, keyboard_event_proto(ctx), type, JS_UNDEFINED, /*trusted*/ true);
    JS_FreeValue(ctx, type);
    return ev;
}

/* ---- the attributes ----------------------------------------------------------------------------------------- */

enum { KE_KEY = 0, KE_CODE, KE_LOCATION, KE_REPEAT, KE_IS_COMPOSING, KE_CHAR_CODE, KE_KEY_CODE };
static const char *const KE_SLOT[] = {
    "key", "code", "location", "repeat", "isComposing", "charCode", "keyCode",
};

static JSValue js_ke_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = ke_slots(ctx, this_val), v;

    DCHECK(magic >= 0 && magic < (int)(sizeof(KE_SLOT) / sizeof(KE_SLOT[0])),
           "a KeyboardEvent attribute was declared with a magic the slot table does not name");
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a KeyboardEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, KE_SLOT[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* The four named modifier attributes, over the shared internal key modifier state. magic indexes KE_MODIFIER,
   whose entries are the KEY MODIFIER NAMES §3.5.3 pairs each attribute with. */
enum { KE_CTRL = 0, KE_SHIFT, KE_ALT, KE_META };
static const char *const KE_MODIFIER[] = { "Control", "Shift", "Alt", "Meta" };

static JSValue js_ke_get_modifier(JSContext *ctx, JSValueConst this_val, int magic)
{
    DCHECK(magic >= 0 && magic < (int)(sizeof(KE_MODIFIER) / sizeof(KE_MODIFIER[0])),
           "a KeyboardEvent modifier attribute was declared with a magic the modifier table does not name");
    if (!keyboard_event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx,
                                 "a KeyboardEvent modifier attribute was read on something that is not one");
    return JS_NewBool(ctx, ui_event_modifier_state(ctx, this_val, KE_MODIFIER[magic]));
}

static JSValue js_ke_get_modifier_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                        int magic)
{
    (void)magic;
    if (!keyboard_event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "KeyboardEvent.getModifierState called on something that is not one");
    /* `keyArg` is not optional, so §3.6 step 5's check is the DECLARATION's and throws before this runs. */
    DCHECK(argc >= 1, "getModifierState's body ran with no keyArg");
    return ui_event_get_modifier_state(ctx, this_val, argv[0]);
}

/* ---- §6.1.2's legacy initializer ------------------------------------------------------------------------------
 *
 *     partial interface KeyboardEvent {
 *       undefined initKeyboardEvent(DOMString typeArg,
 *         optional boolean bubblesArg = false,   optional boolean cancelableArg = false,
 *         optional Window? viewArg = null,       optional DOMString keyArg = "",
 *         optional unsigned long locationArg = 0,
 *         optional boolean ctrlKey = false,      optional boolean altKey = false,
 *         optional boolean shiftKey = false,     optional boolean metaKey = false);
 *     };
 *
 * WHAT THIS ARGUMENT LIST DOES NOT HAVE IS PART OF THE SPEC, and §6.1.2 opens by saying so: "the argument list
 * to this legacy KeyboardEvent initializer does not include the detailArg (present in other initializers) and
 * adds the locale argument; it is necessary to preserve this inconsistency for compatibility with existing
 * implementations." So `detail` is NOT written — "the value of detail remains undefined" — and neither are
 * `code`, `repeat`, `isComposing`, `charCode` or `keyCode`, none of which the list names. Writing a zero into
 * any of them would be inventing an initialization the spec does not perform, which a page that constructed the
 * event with a dictionary and then re-initialized it can see. (The `locale` the sentence mentions is not in the
 * IDL block the section then gives, and this implements the IDL.)
 *
 * The four modifier arguments arrive ctrl, ALT, SHIFT, meta — not the order KE_MODIFIER is written in, so the
 * pairing is stated once, below. */
/* `optional Window? viewArg = null` IS A DECLARED TYPE and it was IDL_ANY, so §3.2.15's brand test and
   §3.2.20's null rule were both the body's. `Window` is one of the interfaces no JSClassID names (the realm's
   own global OR a WindowProxy), so the position states its interface as idl_arg_iface's PREDICATE rather than
   as a class — the same predicate KeyboardEventInit's inherited `view` member states as
   IdlDictMember::iface_is. */
static const IdlArgType KE_INIT_KB_ARGS[10] = {
    IDL_DOMSTRING, IDL_BOOLEAN, IDL_BOOLEAN, IDL_INTERFACE_NULLABLE,
    IDL_DOMSTRING, IDL_UNSIGNED_LONG,
    IDL_BOOLEAN, IDL_BOOLEAN, IDL_BOOLEAN, IDL_BOOLEAN,
};

static const struct { int arg; int modifier; } KE_INIT_KB_MODIFIERS[] = {
    { 6, KE_CTRL }, { 7, KE_ALT }, { 8, KE_SHIFT }, { 9, KE_META },
};

static JSValue js_ke_init_keyboard_event(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic)
{
    JSValue view, key_v, slots;
    uint32_t location = 0;
    unsigned i;

    (void)magic;
    if (!keyboard_event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "initKeyboardEvent called on something that is not a KeyboardEvent");
    /* Only `typeArg` is required, and §3.6 step 5's check for it is the declaration's. */
    DCHECK(argc >= 1, "initKeyboardEvent's body ran with no typeArg");
    /* `Window?` is a CONVERSION, so it runs — and throws — before the algorithm's first step and before the
       dispatch-flag early return. It is the DECLARATION's: what arrives is the IDL null or a Window, and §3.6
       step 16.1 places §6.1.2's own `= null` for a call that never reached the position. `view` is CONSUMED by
       ui_event_reinit, so it is dup'd. */
    DCHECK(argc > 3, "initKeyboardEvent's `viewArg` declares §6.1.2's own `= null`, so §3.6 step 16.1 extends "
                     "the conversion to it and the body is handed the position for every call");
    view = JS_DupValue(ctx, argv[3]);
    /* `= ""` is the IDL's default for an absent keyArg, and it is also this attribute's un-initialized value.
       It is built HERE, before anything is written, because an allocation that fails after the event is half
       re-initialized leaves a state neither the old nor the new one. What a PRESENT keyArg holds is whatever
       the declaration's DOMString conversion produced — a real string, or unknown external input, which crosses
       every conversion as itself and must reach the slot untouched. */
    key_v = (argc > 4 && !JS_IsUndefined(argv[4])) ? JS_DupValue(ctx, argv[4]) : JS_NewString(ctx, "");
    if (JS_IsException(key_v)) {
        JS_FreeValue(ctx, view);
        return key_v;
    }
    /* `= 0` for an absent locationArg, which is also this attribute's un-initialized value. What a present one
       holds is the `unsigned long` the declaration already produced, so reading it runs none of the page's. */
    if (argc > 5 && !JS_IsUndefined(argv[5]))
        JS_ToUint32(ctx, &location, argv[5]);
    /* "the same behavior as UIEvent.initUIEvent()" — with its early return, which every half must honour. */
    if (!ui_event_reinit(ctx, this_val, argv[0], argc > 1 && JS_ToBool(ctx, argv[1]),
                         argc > 2 && JS_ToBool(ctx, argv[2]), view)) {
        JS_FreeValue(ctx, key_v);
        return JS_UNDEFINED;
    }
    slots = ke_slots(ctx, this_val);
    DCHECK(JS_IsObject(slots),
           "initKeyboardEvent passed its brand check and then found no KeyboardEvent slot record");
    JS_SetPropertyStr(ctx, slots, "key", key_v);
    JS_SetPropertyStr(ctx, slots, "location", JS_NewUint32(ctx, location));
    JS_FreeValue(ctx, slots);
    for (i = 0; i < sizeof(KE_INIT_KB_MODIFIERS) / sizeof(KE_INIT_KB_MODIFIERS[0]); i++)
        ui_event_set_modifier_state(ctx, this_val, KE_MODIFIER[KE_INIT_KB_MODIFIERS[i].modifier],
                                    KE_INIT_KB_MODIFIERS[i].arg < argc &&
                                    JS_ToBool(ctx, argv[KE_INIT_KB_MODIFIERS[i].arg]));
    return JS_UNDEFINED;
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional KeyboardEventInit eventInitDict = {})`. KeyboardEventInit inherits
 * EventModifierInit inherits UIEventInit inherits EventInit, and Web IDL §3.2.17 reads the INHERITED members first and
 * each dictionary's own lexicographically among THEMSELVES — which is the order this list is in, and the order
 * a page pins by throwing from one member's getter. §7.3's two legacy members are members of THIS dictionary,
 * so they sort with its own and not after them. The three inherited levels are spliced from ui_event.h. */
static const IdlArgType KE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember KE_INIT[] = {
    UI_EVENT_INIT_MEMBERS,
    EVENT_MODIFIER_INIT_MEMBERS,
    { "charCode", IDL_UNSIGNED_LONG, false, NULL, 3 }, { "code", IDL_DOMSTRING, false, NULL, 3 },
    { "isComposing", IDL_BOOLEAN, false, NULL, 3 }, { "key", IDL_DOMSTRING, false, NULL, 3 },
    { "keyCode", IDL_UNSIGNED_LONG, false, NULL, 3 }, { "location", IDL_UNSIGNED_LONG, false, NULL, 3 },
    { "repeat", IDL_BOOLEAN, false, NULL, 3 },
};

static JSValue js_ke_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor KeyboardEvent requires 'new'");
    DCHECK(argc >= 1, "the KeyboardEvent constructor body ran with no type argument — §3.6 step 5 is the "
                      "declaration's and throws before any body is entered");
    /* An event the PAGE constructs is untrusted. */
    return keyboard_event_new_derived(ctx, keyboard_event_proto(ctx), argv[0],
                                      argc > 1 ? argv[1] : JS_UNDEFINED, /*trusted*/ false);
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_ke_proto[] = {
    JS_CGETSET_MAGIC_DEF("key", js_ke_get, NULL, KE_KEY),
    JS_CGETSET_MAGIC_DEF("code", js_ke_get, NULL, KE_CODE),
    JS_CGETSET_MAGIC_DEF("location", js_ke_get, NULL, KE_LOCATION),
    JS_CGETSET_MAGIC_DEF("repeat", js_ke_get, NULL, KE_REPEAT),
    JS_CGETSET_MAGIC_DEF("isComposing", js_ke_get, NULL, KE_IS_COMPOSING),
    JS_CGETSET_MAGIC_DEF("charCode", js_ke_get, NULL, KE_CHAR_CODE),
    JS_CGETSET_MAGIC_DEF("keyCode", js_ke_get, NULL, KE_KEY_CODE),
    JS_CGETSET_MAGIC_DEF("ctrlKey", js_ke_get_modifier, NULL, KE_CTRL),
    JS_CGETSET_MAGIC_DEF("shiftKey", js_ke_get_modifier, NULL, KE_SHIFT),
    JS_CGETSET_MAGIC_DEF("altKey", js_ke_get_modifier, NULL, KE_ALT),
    JS_CGETSET_MAGIC_DEF("metaKey", js_ke_get_modifier, NULL, KE_META),
};

/* §3.5.1's KeyLocationCode constants. Web IDL puts a `const` on the interface PROTOTYPE object AND on the
   interface object, so one table installs both — reached by name, the way event.c installs Event's phases. */
static const JSCFunctionListEntry js_ke_consts[] = {
    JS_PROP_INT32_DEF("DOM_KEY_LOCATION_STANDARD", 0x00, IDL_CONSTANT_PROP_FLAGS),
    JS_PROP_INT32_DEF("DOM_KEY_LOCATION_LEFT", 0x01, IDL_CONSTANT_PROP_FLAGS),
    JS_PROP_INT32_DEF("DOM_KEY_LOCATION_RIGHT", 0x02, IDL_CONSTANT_PROP_FLAGS),
    JS_PROP_INT32_DEF("DOM_KEY_LOCATION_NUMPAD", 0x03, IDL_CONSTANT_PROP_FLAGS),
};

void keyboard_event_init(JSContext *ctx)
{
    JSClassDef d = { "KeyboardEvent" };
    static const IdlArgType MODIFIER_STATE_ARGS[1] = { IDL_DOMSTRING };

    DCHECK(!g_ready, "keyboard_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "keyboardEventSlots", false);
    CHECK(!JS_IsException(g_key), "the KeyboardEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_ke_class);
    JS_NewClass(JS_GetRuntime(ctx), g_ke_class, &d);
    /* A SECOND DECLARATION OF getModifierState, and not a shared one: the IDL declares the member on this
       interface and on MouseEvent separately, so `MouseEvent.prototype.getModifierState` and this one are two
       function objects — which is what makes calling one on the other's instance a TypeError. The BODY they
       reach is one, in ui_event.c, because the state is one. */
    g_modifier_state_id = idl_method_id(ctx, MODIFIER_STATE_ARGS, 1, js_ke_get_modifier_state, 0);
    g_init_kb_id = idl_method_id(ctx, KE_INIT_KB_ARGS, 10, js_ke_init_keyboard_event, 0);
    idl_optional_from(1);   /* §6.1.2: every argument but `typeArg` is optional */
    /* UI Events §6.1.2 Initializers for interface KeyboardEvent: `optional Window? viewArg = null`. The
       predicate is core/frame/window_proxy.h's, which owns what a Window IS in this engine; the default is the
       IDL's own, so §3.6 step 16.1 places it rather than the body deciding what an absent `viewArg` means. */
    idl_arg_iface(3, window_proxy_is_window, "Window");
    idl_arg_default(3, IDL_DEFAULT_NULL, NULL);
    g_ctor_stepid = idl_method_id_dict(ctx, KE_CTOR_ARGS, 2, KE_INIT,
                                       (int)(sizeof(KE_INIT) / sizeof(KE_INIT[0])), js_ke_ctor, 0);
    idl_optional_from(1);   /* `constructor(DOMString type, optional KeyboardEventInit eventInitDict = {})` */
    /* THE DECLARATION-WIDE CLASS, the brand of exactly ONE of this dictionary's two interface-typed members:
       UIEventInit's `sourceCapabilities`. The other, UIEventInit's `view`, states §3.2.15's `I` as its own
       realm-taking predicate, which idl_member_implements takes in preference to the class. */
    idl_iface_brand(input_device_capabilities_class());
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — AND IT NAMES THE `event` ROW, NOT THIS FILE.
       core/agent_state.h: a sub-component names the row whose RELEASE gives its slots back, which for every
       Event subclass is core/platform.c's `event` row — event_init calls this init and event_free calls this
       release. Nothing here was declared at all, so the pairing's own arm — does anybody release this? — was
       never asked about any of these. */
    agent_state_flag("event", &g_ready,
                     "UI Events §3.5.1 Interface KeyboardEvent's declaration latch");
    agent_state_class("event", &g_ke_class,
                      "UI Events §3.5.1 Interface KeyboardEvent's class, held for its per-realm prototype slot");
    agent_state_value("event", &g_key,
                      "the private Symbol UI Events §3.5.1 Interface KeyboardEvent's slot record hangs off");
    agent_state_id("event", &g_ctor_stepid,
                   "UI Events §3.5.1 Interface KeyboardEvent's `constructor(DOMString type, optional "
                   "KeyboardEventInit eventInitDict = {})`");
    agent_state_id("event", &g_modifier_state_id,
                   "UI Events §3.5.1 Interface KeyboardEvent's `getModifierState(DOMString keyArg)` — this "
                   "interface's OWN declaration of it, which is what makes calling MouseEvent's on a KeyboardEvent "
                   "a TypeError");
    agent_state_id("event", &g_init_kb_id,
                   "UI Events §6.1.2 Initializers for interface KeyboardEvent's `initKeyboardEvent(...)`");
    realm_declare_intrinsic(keyboard_event_install_protos);
}

void keyboard_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for KeyboardEvent before keyboard_event_init declared it");
    prev = JS_GetClassProto(ctx, g_ke_class);
    DCHECK(JS_IsNull(prev), "keyboard_event_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    /* `interface KeyboardEvent : UIEvent` — THIS realm's UIEvent.prototype. */
    base = ui_event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "KeyboardEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "KeyboardEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_ke_proto, (int)(sizeof(js_ke_proto) / sizeof(js_ke_proto[0])));
    JS_SetPropertyFunctionList(ctx, proto, js_ke_consts,
                               (int)(sizeof(js_ke_consts) / sizeof(js_ke_consts[0])));
    idl_install_method(ctx, proto, "getModifierState", g_modifier_state_id);
    idl_install_method(ctx, proto, "initKeyboardEvent", g_init_kb_id);
    JS_SetClassProto(ctx, g_ke_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object on THIS realm's global — see ui_event.c. */
    ctor = idl_step_constructor(ctx, "KeyboardEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the KeyboardEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyFunctionList(ctx, ctor, js_ke_consts,
                               (int)(sizeof(js_ke_consts) / sizeof(js_ke_consts[0])));
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "KeyboardEvent", ctor);
    JS_FreeValue(ctx, global);
}

/* THE RUNTIME, NOT A REALM — core/platform.h's release column, reached through event_free. What this
   gives back is the AGENT's: a private Symbol, a class id and this interface's member declarations; every
   prototype it built is in some realm's class-proto slot and goes with that realm. */
void keyboard_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. core/events/event.c's event_init calls this component's init on the ONE
       declaration pass and its event_free — which has already asserted its own latch — calls this release
       unconditionally, so the test could never be true and what it could do was hide a release that left the
       latch set. */
    DCHECK(g_ready, "UI Events §3.5.1 Interface KeyboardEvent was released in an agent that never declared it — "
                    "event_init declares every Event subclass on the one unconditional pass");
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because the id doubles
       as the init latch and a carried one names a class in a runtime that is gone. Nothing WEARS this class —
       it exists for its per-realm prototype slot, and every event in this engine is minted by
       core/events/event.c's event_make_proto through JS_NewObjectProto — so there is no finalizer and no
       gc_mark here to owe the JS_GetAnyOpaque the zeroing costs a component whose objects do wear one. */
    g_ke_class = 0;
    g_ctor_stepid = g_modifier_state_id = g_init_kb_id = -1;
}
