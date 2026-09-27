/* COOKIE STORE API §5.1 "The CookieChangeEvent interface" — the IDL, which standard this is, why this half
 * lands before the `onchange` handler and what a frozen array means here are all in cookie_change_event.h. */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/cookie_change_event.h"
#include "core/events/event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's own slot record hangs off */
static JSClassID g_cce_class;   /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

static JSValue cce_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_cce_class);

    DCHECK(!JS_IsNull(proto),
           "CookieChangeEvent.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* OWNED */
}

/* §5.1: "The changed and deleted attributes must return the value they were initialized to." `magic` is which
   of the two — they are ONE getter because they are one sentence of the standard and differ only in the slot
   they read, so a second body could only ever drift from this one. */
static JSValue js_cce_get_list(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue slots, v;

    DCHECK(g_ready, "a CookieChangeEvent attribute was read before cookie_change_event_init ran");
    DCHECK(magic == 0 || magic == 1, "a CookieChangeEvent attribute was installed with a magic naming neither "
                                     "of §5.1's two members");
    /* A RECEIVER IS PAGE-SUPPLIED INPUT AND NEVER AN INVARIANT — `CookieChangeEvent.prototype.changed` read
       through `Reflect.get` or a `.call` on a plain object is a TypeError Web IDL §3.7.6 Attributes requires,
       and asserting here would hand any page an abort switch for the whole engine. */
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "a CookieChangeEvent attribute was read on something that is not one");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_EXCEPTION;
    if (JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a CookieChangeEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, magic == 0 ? "changed" : "deleted");
    JS_FreeValue(ctx, slots);
    return v;
}

/* ONE of §5.1's two `FrozenArray<CookieListItem>` values, out of the dictionary the declaration already
 * converted. Returns an OWNED frozen Array, or JS_EXCEPTION with the throw live.
 *
 * AN ABSENT MEMBER IS « » AND NOT A HOLE SOMETHING FILLS IN: `CookieChangeEventInit` declares
 * `CookieList changed` with no default, the attribute's type is a NON-NULLABLE `FrozenArray<CookieListItem>`
 * which undefined is not a value of, and « » is what §7.4's "prepare lists from changes" steps 1 and 2 put
 * there for an empty change set. So the absence is READ as the positive statement it is.
 *
 * THE ASSERT STANDS ON THIS ENGINE'S OWN CONVERSION AND NOT ON THE PAGE'S BYTES. The member is declared
 * IDL_SEQUENCE_DICT, so Web IDL §3.2.21.1 has already refused everything that is not a sequence — with a
 * TypeError, before this body runs — and what reaches here is the list that conversion built. */
static JSValue cce_list(JSContext *ctx, JSValueConst init, const char *member)
{
    JSValue v = idl_dict_get(ctx, init, member);

    if (JS_IsException(v)) return v;
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        v = JS_NewArray(ctx);
        if (JS_IsException(v)) return v;
    }
    DCHECKF(JS_IsArray(v), "§5.1's `%s` was converted to something that is not a sequence — the member is "
                           "declared IDL_SEQUENCE_DICT, so Web IDL §3.2.21.1 Sequence types has already thrown "
                           "for every value that is not one and this is the conversion's own output", member);
    /* Web IDL §3.2.27's create-a-frozen-array over the sequence it is built out of — the whole of what
       `FrozenArray<CookieListItem>` adds to the list, and what `[SameObject]` needs a stored object for. */
    if (idl_freeze_array(ctx, v) < 0) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    return v;
}

/* The two own slots, on an event whose Event half is already built. CONSUMES `changed` and `deleted` on every
   path. Returns -1 with the throw live. */
static int cce_init_slots(JSContext *ctx, JSValueConst ev, JSValue changed, JSValue deleted)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        JS_FreeValue(ctx, changed);
        JS_FreeValue(ctx, deleted);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "changed", changed);
    JS_SetPropertyStr(ctx, slots, "deleted", deleted);
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional CookieChangeEventInit eventInitDict = {})`. CookieChangeEventInit
 * INHERITS EventInit, and Web IDL converts a dictionary's members with the INHERITED ones first and each level
 * lexicographically among itself — which is the order this list is in and the order a page pins by throwing
 * from one member's getter. `changed` sorts before every one of EventInit's three, so a single sorted list
 * would read the derived dictionary's members first: THE LEVEL is what makes this list the spec's read order.
 *
 * `CookieList` IS `sequence<CookieListItem>` — §3's `typedef sequence<CookieListItem> CookieList;` — so the
 * member's declared type is a sequence whose element is a DICTIONARY, and the element dictionary is named
 * beside the member exactly as core/idl_args.h's IDL_SEQUENCE_DICT requires. `CookieListItem`'s two members are
 * both plain `USVString` with no `required` and no default: §7.1's create-a-CookieListItem returns
 * «[ "name" → name, "value" → value ]» and NOTHING else, and that standard's own Note records that the wider
 * item of earlier drafts is gone ("One implementation is known to expose information beyond _name_ and
 * _value_."). A third member here would be a field no member of that API may return. */
static const IdlDictMember COOKIE_LIST_ITEM_MEMBERS[] = {
    { "name",  IDL_USVSTRING, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
    { "value", IDL_USVSTRING, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
};
static const IdlDictDecl COOKIE_LIST_ITEM_DECL = {
    "CookieListItem", COOKIE_LIST_ITEM_MEMBERS,
    (int)(sizeof COOKIE_LIST_ITEM_MEMBERS / sizeof *COOKIE_LIST_ITEM_MEMBERS)
};

static const IdlArgType CCE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember CCE_INIT[] = {
    { "bubbles",    IDL_BOOLEAN,       false, NULL, 0, NULL,                    IDL_DEFAULT_NONE, NULL },
    { "cancelable", IDL_BOOLEAN,       false, NULL, 0, NULL,                    IDL_DEFAULT_NONE, NULL },
    { "composed",   IDL_BOOLEAN,       false, NULL, 0, NULL,                    IDL_DEFAULT_NONE, NULL },
    { "changed",    IDL_SEQUENCE_DICT, false, NULL, 1, &COOKIE_LIST_ITEM_DECL,  IDL_DEFAULT_NONE, NULL },
    { "deleted",    IDL_SEQUENCE_DICT, false, NULL, 1, &COOKIE_LIST_ITEM_DECL,  IDL_DEFAULT_NONE, NULL },
};

static JSValue js_cce_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue ev, changed, deleted;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor CookieChangeEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "CookieChangeEvent constructor requires a type");
    changed = cce_list(ctx, init, "changed");
    if (JS_IsException(changed))
        return changed;
    deleted = cce_list(ctx, init, "deleted");
    if (JS_IsException(deleted)) {
        JS_FreeValue(ctx, changed);
        return deleted;
    }
    /* DOM §2.5 "Constructing events" with THIS interface's prototype — an event the PAGE constructs is
       untrusted, and its three EventInit members are the page's to state. */
    ev = event_new_derived(ctx, cce_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (JS_IsException(ev)) {
        JS_FreeValue(ctx, changed);
        JS_FreeValue(ctx, deleted);
        return ev;
    }
    if (cce_init_slots(ctx, ev, changed, deleted) < 0) {   /* CONSUMES both on every path */
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_cce_proto[] = {
    JS_CGETSET_MAGIC_DEF("changed", js_cce_get_list, NULL, 0),
    JS_CGETSET_MAGIC_DEF("deleted", js_cce_get_list, NULL, 1),
};

void cookie_change_event_init(JSContext *ctx)
{
    JSClassDef d = { "CookieChangeEvent" };

    DCHECK(!g_ready, "cookie_change_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "cookieChangeEventSlots", false);
    CHECK(!JS_IsException(g_key), "the CookieChangeEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_cce_class);
    JS_NewClass(JS_GetRuntime(ctx), g_cce_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, CCE_CTOR_ARGS, 2, CCE_INIT,
                                       (int)(sizeof CCE_INIT / sizeof *CCE_INIT), js_cce_ctor, 0);
    idl_optional_from(1);                       /* `optional CookieChangeEventInit eventInitDict = {}` */
    g_ready = 1;
    /* core/agent_state.h: a sub-component names the row whose RELEASE gives its slots back, which for every
       Event subclass is core/platform.c's `event` row — event_init calls this init and event_free calls this
       release. */
    agent_state_flag("event", &g_ready,
                     "Cookie Store API §5.1 The CookieChangeEvent interface's declaration latch");
    agent_state_class("event", &g_cce_class,
                      "Cookie Store API §5.1 The CookieChangeEvent interface's class, held for its per-realm "
                      "prototype slot");
    agent_state_value("event", &g_key,
                      "the private Symbol Cookie Store API §5.1 The CookieChangeEvent interface's slot record "
                      "hangs off");
    agent_state_id("event", &g_ctor_stepid,
                   "Cookie Store API §5.1 The CookieChangeEvent interface's `constructor(DOMString type, "
                   "optional CookieChangeEventInit eventInitDict = {})`");
    realm_declare_intrinsic(cookie_change_event_install_protos);
}

void cookie_change_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for CookieChangeEvent before cookie_change_event_init declared it");
    prev = JS_GetClassProto(ctx, g_cce_class);
    DCHECK(JS_IsNull(prev), "cookie_change_event_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "CookieChangeEvent.prototype could not be allocated");
    /* §3.7.3's @@toStringTag, which also ASSERTS §3.7.3's proto step against browser/idl_inheritance.h — that
       table already carries { "CookieChangeEvent", "Event", IDL_PROTO_INHERITS }, so this is what checks that
       the prototype above really was built over this realm's Event.prototype rather than merely named as
       though it had been. */
    idl_interface_tag(ctx, proto, "CookieChangeEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_cce_proto, (int)(sizeof js_cce_proto / sizeof *js_cce_proto));
    JS_SetClassProto(ctx, g_cce_class, JS_DupValue(ctx, proto));

    ctor = idl_step_constructor(ctx, "CookieChangeEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the CookieChangeEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    /* §5.1's interface is `[Exposed=Window, SecureContext]`, and BOTH halves are the door's to ask: the
       exposure SET is keyed by the identifier out of the corpus, and [SecureContext] is the conditional
       attribute only this component knows it carries, so it is stated here as DATA. §3.3.13 REMOVES the
       construct — `"CookieChangeEvent" in window` is false in a non-secure realm rather than undefined or
       throwing, which is the same branch core/cookie_store/cookie_store.c's own interface takes there. */
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference_exposed(ctx, global, "CookieChangeEvent", ctor, IDL_SECURE_CONTEXT);
    JS_FreeValue(ctx, global);
}

/* THE RUNTIME, NOT A REALM — core/platform.h's release column, reached through event_free. What this gives
   back is the AGENT's: a private Symbol, a class id and this interface's member declarations; every prototype
   it built is in some realm's class-proto slot and goes with that realm. */
void cookie_change_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. core/events/event.c's event_init calls this component's init on the ONE
       declaration pass and its event_free — which has already asserted its own latch — calls this release
       unconditionally, so the test could never be true and what it could do was hide a release that left the
       latch set. */
    DCHECK(g_ready, "Cookie Store API §5.1 The CookieChangeEvent interface was released in an agent that never "
                    "declared it — event_init declares every Event subclass on the one unconditional pass");
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because the id doubles
       as the init latch and a carried one names a class in a runtime that is gone. Nothing WEARS this class —
       it exists for its per-realm prototype slot, and every event in this engine is minted through
       core/events/event.c's event_make_proto — so there is no finalizer and no gc_mark here. */
    g_cce_class = 0;
    g_ctor_stepid = -1;
}
