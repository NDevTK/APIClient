/* THE NavigationDestination INTERFACE — HTML §7.2.6.10.3.
 *
 *     [Exposed=Window]
 *     interface NavigationDestination {
 *       readonly attribute USVString url;
 *       readonly attribute DOMString key;
 *       readonly attribute DOMString id;
 *       readonly attribute long long index;
 *       readonly attribute boolean sameDocument;
 *       any getState();
 *     };
 *
 * THREE OF THE SIX MEMBERS ARE THE ENTRY'S OWN ANSWERS, NOT COPIES OF THEM. §7.2.6.10.3 writes each of `key`,
 * `id` and `index` as a two-step getter that defers to the entry — "The key getter steps are: If this's entry
 * is null, then return the empty string. Return this's entry's key." — and the entry is a NavigationHistoryEntry whose own
 * getters already answer those questions — including the not-fully-active answers §7.2.6.5 gives them, which
 * are observably different from this interface's null-entry answers only in that BOTH are reached here. So the
 * three are asked of core/frame/navigation_history_entry.c rather than re-derived off the session history
 * entry: two derivations of one member is where the second one goes wrong, and this one would go wrong in a
 * detached iframe, where the entry answers "" and −1 and a re-derivation would answer the real key.
 *
 * IT HAS NO CONSTRUCTOR. §7.2.6.10.3 declares none, so `new NavigationDestination()` is a TypeError and the
 * ONLY producer is §7.2.6.10.4's three navigate-event wrappers — which is what makes the assertions below
 * about `entry` (non-null if and only if the navigation is a "traverse") checkable rather than hopeful. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/frame/navigation_destination.h"
#include "core/frame/navigation_history_entry.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/structured_clone.h"

static JSValue   g_key;         /* the private Symbol this interface's own slots hang off */
static JSClassID g_dest_class;
static int       g_ready;
static int       g_id_get_state = -1;

#define DEST_URL      "url"
#define DEST_ENTRY    "entry"
#define DEST_STATE    "state"        /* the SERIALIZED bytes, as an ArrayBuffer — never a live value */
#define DEST_SAME_DOC "isSameDocument"

JSClassID navigation_destination_class(void)
{
    DCHECK(g_dest_class != 0,
           "NavigationDestination's class was asked for before navigation_destination_init declared it");
    return g_dest_class;
}

static JSValue dest_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_dest_class);

    DCHECK(!JS_IsNull(proto),
           "NavigationDestination.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* OWNED */
}

/* WEB IDL §3.7.6 Attributes' BRAND. The class is the check, so a getter pulled off the prototype and applied
   to something
   else is the TypeError a browser answers with rather than a read of a slot that is not there. */
static bool dest_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_ready, "a NavigationDestination member ran before navigation_destination_init");
    if (JS_GetClassID(this_val) == g_dest_class) return true;
    JS_ThrowTypeError(ctx, "a NavigationDestination member was reached on something that is not one");
    return false;
}

/* The slot record of a NavigationDestination. OWNED. */
static JSValue dest_slots(JSContext *ctx, JSValueConst dest)
{
    JSAtom k = JS_ValueToAtom(ctx, g_key);
    JSValue slots;

    CHECK(k != JS_ATOM_NULL, "NavigationDestination: the slot key could not be resolved to an atom");
    if (JS_GetOwnSlot(ctx, &slots, dest, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    DCHECK(JS_IsObject(slots), "a NavigationDestination carried no slot record — every instance is built by "
                               "navigation_destination_new, which places one before it returns");
    return slots;
}

static JSValue dest_field(JSContext *ctx, JSValueConst dest, const char *name)
{
    JSValue slots = dest_slots(ctx, dest), v = JS_GetPropertyStr(ctx, slots, name);

    JS_FreeValue(ctx, slots);
    return v;
}

JSValue navigation_destination_new(JSContext *ctx, const char *url, JSValueConst entry, JSValue state,
                                   bool same_doc)
{
    JSValue proto = dest_proto(ctx), obj, slots;
    JSAtom k;
    size_t state_len = 0;
    /* READ BEFORE THE ASSERT, not inside it. A DCHECK's condition is compiled out in release, so a call in one
       is a call that stops happening — and JS_GetArrayBuffer THROWS a TypeError for a value that is not one,
       which inside an assert would leave a pending exception behind in dev and none in release. */
    const uint8_t *state_bytes = JS_GetArrayBuffer(ctx, &state_len, state);

    DCHECK(url != NULL, "§7.2.6.10.4 built a NavigationDestination with no URL — all three of its wrappers set "
                        "destination's URL, two from a URL they were handed and one from the destination "
                        "session history entry's own");
    DCHECK(JS_IsNull(entry) || JS_GetClassID(entry) == navigation_history_entry_class(),
           "§7.2.6.10.3's `entry` was set to something that is not a NavigationHistoryEntry — it is "
           "\"a NavigationHistoryEntry or null\", and the one wrapper that sets it non-null takes it out of "
           "the Navigation's own entry list");
    DCHECK(state_bytes != NULL,
           "§7.2.6.10.3's `state` was set to something that is not the SERIALIZED bytes — a serialized state "
           "crosses a park and a session, so it is an ArrayBuffer here exactly as it is on §7.4.1.1's entry, "
           "and a live value handed over instead would be a value one flow could mutate under another");
    (void)state_len;
    obj = JS_NewObjectProtoClass(ctx, proto, g_dest_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) { JS_FreeValue(ctx, state); return obj; }
    slots = idl_slots_new(ctx);
    CHECK(!JS_IsException(slots), "a NavigationDestination's slot record could not be allocated");
    JS_SetPropertyStr(ctx, slots, DEST_URL, JS_NewString(ctx, url));
    JS_SetPropertyStr(ctx, slots, DEST_ENTRY, JS_DupValue(ctx, entry));
    JS_SetPropertyStr(ctx, slots, DEST_STATE, state);
    JS_SetPropertyStr(ctx, slots, DEST_SAME_DOC, JS_NewBool(ctx, same_doc));
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "NavigationDestination: the slot key could not be resolved to an atom");
    JS_SetProperty(ctx, obj, k, slots);
    JS_FreeAtom(ctx, k);
    return obj;
}

static JSValue navigation_destination_url(JSContext *ctx, JSValueConst dest)
{
    JSValue v = dest_field(ctx, dest, DEST_URL);

    DCHECK(JS_IsString(v), "§7.2.6.10.3's URL held something that is not a serialized URL");
    return v;
}

static bool navigation_destination_is_same_document(JSContext *ctx, JSValueConst dest)
{
    JSValue v = dest_field(ctx, dest, DEST_SAME_DOC);
    bool b;

    DCHECK(JS_IsBool(v), "§7.2.6.10.3's IS SAME DOCUMENT held something that is not a boolean");
    b = JS_ToBool(ctx, v) != 0;
    JS_FreeValue(ctx, v);
    return b;
}

static JSValue navigation_destination_entry(JSContext *ctx, JSValueConst dest)
{
    JSValue v = dest_field(ctx, dest, DEST_ENTRY);

    DCHECK(JS_IsNull(v) || JS_GetClassID(v) == navigation_history_entry_class(),
           "§7.2.6.10.3's `entry` held something that is neither a NavigationHistoryEntry nor null");
    return v;
}

/* ---- the five attributes --------------------------------------------------------------------------------- */

enum { DEST_M_URL = 0, DEST_M_KEY, DEST_M_ID, DEST_M_INDEX, DEST_M_SAME_DOCUMENT, DEST_M_N };
static const char *const DEST_NAME[] = { "url", "key", "id", "index", "sameDocument" };

static JSValue js_dest_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue entry, v;

    if (!dest_brand(ctx, this_val)) return JS_EXCEPTION;
    DCHECK(magic >= DEST_M_URL && magic < DEST_M_N,
           "a NavigationDestination accessor was installed with a magic this interface has no member for");
    /* "The url getter steps are to return this's URL, SERIALIZED" — and it is stored serialized, because the
       three wrappers that build one each already hold the URL as a string. */
    if (magic == DEST_M_URL) return navigation_destination_url(ctx, this_val);
    /* "The sameDocument getter steps are to return this's is same document." */
    if (magic == DEST_M_SAME_DOCUMENT) return JS_NewBool(ctx, navigation_destination_is_same_document(ctx, this_val));

    /* The other three open with the SAME null-entry test and each answers it differently — "" for `key`, "" for
       `id`, −1 for `index`. That is not one default written three times: §7.2.6.10.3 writes a different value
       per member because the TYPES differ, and a page reads all three on a "push" navigation, where the entry
       is legitimately null because no session history entry for the destination exists yet. */
    entry = navigation_destination_entry(ctx, this_val);
    if (JS_IsNull(entry)) {
        JS_FreeValue(ctx, entry);
        switch (magic) {
        case DEST_M_KEY:
        case DEST_M_ID:    return JS_NewStringLen(ctx, "", 0);
        case DEST_M_INDEX: return JS_NewInt32(ctx, -1);
        default:           break;
        }
    }
    switch (magic) {
    /* "Return this's entry's key / ID / index" — the ENTRY's own getter steps, so a destination over an entry
       whose Document is no longer fully active answers exactly what that entry answers. */
    case DEST_M_KEY:   v = navigation_history_entry_key(ctx, entry); break;
    case DEST_M_ID:    v = navigation_history_entry_id(ctx, entry); break;
    case DEST_M_INDEX: v = JS_NewInt64(ctx, navigation_history_entry_index(ctx, entry)); break;
    default:
        DFAIL("a NavigationDestination member was read with a magic no member of this file declares");
        v = JS_UNDEFINED;
        break;
    }
    JS_FreeValue(ctx, entry);
    return v;
}

/* §7.2.6.10.3's `any getState()`: "return StructuredDeserialize(this's state)".
 *
 * A FRESH DESERIALIZATION EACH CALL, exactly as §7.2.6.5's is and for the same reason — the state is BYTES and
 * the value a page reads is built from them on demand. Unlike §7.2.6.5's, it has NO fully-active early return:
 * §7.2.6.10.3 writes one step and that step is the deserialization, because a NavigationDestination is a
 * snapshot handed to a listener rather than a live view of the session history. */
static JSValue js_dest_get_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue buf, v;
    StructuredData d;

    (void)argc; (void)argv; (void)magic;
    if (!dest_brand(ctx, this_val)) return JS_EXCEPTION;
    buf = dest_field(ctx, this_val, DEST_STATE);
    d.buf = JS_GetArrayBuffer(ctx, &d.len, buf);
    DCHECK(d.buf != NULL, "§7.2.6.10.3's `state` held something that is not the serialized bytes — "
                          "navigation_destination_new is the only writer and it asserts the same thing about "
                          "what it is handed");
    v = structured_deserialize(ctx, &d);
    JS_FreeValue(ctx, buf);
    return v;
}

/* ---- install --------------------------------------------------------------------------------------------- */

void navigation_destination_init(JSContext *ctx)
{
    JSClassDef d = { "NavigationDestination" };

    DCHECK(!g_ready, "navigation_destination_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "navigationDestinationSlots", false);
    CHECK(!JS_IsException(g_key), "the NavigationDestination slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_dest_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_dest_class, &d) == 0,
          "NavigationDestination: the per-realm prototype slot could not be declared");
    g_id_get_state = idl_method_id(ctx, NULL, 0, js_dest_get_state, 0);
    g_ready = 1;
    agent_state_flag("navigation_destination", &g_ready, "the declaration latch");
    agent_state_value("navigation_destination", &g_key,
                      "§7.2.6.10.3's internal-slot key, the Symbol this component minted for the agent");
    agent_state_id("navigation_destination", &g_id_get_state, "§7.2.6.10.3's getState declaration");
    realm_declare_intrinsic(navigation_destination_install_protos);
}

void navigation_destination_install_protos(JSContext *ctx)
{
    JSValue proto, prev, global;
    int i;

    DCHECK(g_ready, "a realm asked for NavigationDestination before navigation_destination_init");
    prev = JS_GetClassProto(ctx, g_dest_class);
    DCHECK(JS_IsNull(prev), "navigation_destination_install_protos ran twice in one realm — everything already "
                            "holding the first prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "NavigationDestination.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "NavigationDestination");
    /* `interface NavigationDestination` — it inherits nothing, so the prototype's own is Object.prototype and
       there is no chain call here. It is NOT an EventTarget: nothing is ever fired at a destination. */
    for (i = 0; i < DEST_M_N; i++)
        idl_install_accessor(ctx, proto, DEST_NAME[i], js_dest_get, i, -1);
    idl_install_method(ctx, proto, "getState", g_id_get_state);
    JS_SetClassProto(ctx, g_dest_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT. NavigationDestination declares no constructor, so `new NavigationDestination()`
       is a TypeError — and its PRESENCE is what `event.destination instanceof NavigationDestination` needs. */
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "NavigationDestination",
                                         idl_interface_object(ctx, "NavigationDestination", proto));
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, proto);
}

void navigation_destination_free(JSRuntime *rt)
{
    /* The prototypes and the interface objects are the REALMS' — each is released with its context. What the
       agent holds is the Symbol it minted, which is a runtime-lifetime value this component owns. */
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    g_id_get_state = -1;
    g_ready = 0;
}
