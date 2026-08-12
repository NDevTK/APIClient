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
 * `initKeyboardEvent` IS NOT INSTALLED. §6.1.2 declares it with TEN arguments and core/idl_args.h's
 * IDL_MAX_DECLARED is 8; a member cannot be declared past that ceiling, and converting the arguments past it
 * inside the body would run the page's valueOf from C. It is honestly ABSENT — a page calling it gets its own
 * TypeError — until that ceiling is the platform's real widest (initMouseEvent's fifteen). */
#include "check.h"
#include "quickjs.h"
#include "core/events/keyboard_event.h"
#include "core/events/ui_event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key;         /* the private Symbol this interface's own slots hang off */
static JSClassID g_ke_class;    /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;
static int       g_modifier_state_id = -1;

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
   and runs none of the page's code. Returns an OWNED string, or JS_EXCEPTION. */
static JSValue ke_dict_str(JSContext *ctx, JSValueConst init, const char *name)
{
    JSValue v = idl_dict_get(ctx, init, name);

    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return JS_NewString(ctx, "");
    }
    DCHECK(JS_IsString(v), "a KeyboardEventInit DOMString member arrived unconverted — the declaration is what "
                           "converts it, and a body that stringifies one runs the page's toString from C");
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
    /* `keyArg` is not optional, so §3.6.2 step 1's check is the DECLARATION's and throws before this runs. */
    DCHECK(argc >= 1, "getModifierState's body ran with no keyArg");
    return ui_event_get_modifier_state(ctx, this_val, argv[0]);
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional KeyboardEventInit eventInitDict = {})`. KeyboardEventInit inherits
 * EventModifierInit inherits UIEventInit inherits EventInit, and §3.2.18 reads the INHERITED members first and
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
    DCHECK(argc >= 1, "the KeyboardEvent constructor body ran with no type argument — §3.6.2 step 1 is the "
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
    JS_PROP_INT32_DEF("DOM_KEY_LOCATION_STANDARD", 0x00, 0),
    JS_PROP_INT32_DEF("DOM_KEY_LOCATION_LEFT", 0x01, 0),
    JS_PROP_INT32_DEF("DOM_KEY_LOCATION_RIGHT", 0x02, 0),
    JS_PROP_INT32_DEF("DOM_KEY_LOCATION_NUMPAD", 0x03, 0),
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
    g_ctor_stepid = idl_method_id_dict(ctx, KE_CTOR_ARGS, 2, KE_INIT,
                                       (int)(sizeof(KE_INIT) / sizeof(KE_INIT[0])), js_ke_ctor, 0);
    idl_optional_from(1);   /* `constructor(DOMString type, optional KeyboardEventInit eventInitDict = {})` */
    g_ready = 1;
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
    idl_install_method(ctx, proto, "getModifierState", 1, g_modifier_state_id);
    JS_SetClassProto(ctx, g_ke_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object on THIS realm's global — see ui_event.c. */
    ctor = idl_step_constructor(ctx, "KeyboardEvent", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the KeyboardEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyFunctionList(ctx, ctor, js_ke_consts,
                               (int)(sizeof(js_ke_consts) / sizeof(js_ke_consts[0])));
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "KeyboardEvent", ctor);
    JS_FreeValue(ctx, global);
}

void keyboard_event_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = g_modifier_state_id = -1;
}
