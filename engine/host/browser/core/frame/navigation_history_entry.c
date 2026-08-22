/* THE NavigationHistoryEntry INTERFACE — HTML §7.2.6.5.
 *
 *     [Exposed=Window]
 *     interface NavigationHistoryEntry : EventTarget {
 *       readonly attribute USVString? url;
 *       readonly attribute DOMString key;
 *       readonly attribute DOMString id;
 *       readonly attribute long long index;
 *       readonly attribute boolean sameDocument;
 *       any getState();
 *       attribute EventHandler ondispose;
 *     };
 *
 * IT IS A VIEW OVER A SESSION HISTORY ENTRY AND HOLDS NOTHING ELSE. §7.2.6.5: "Each NavigationHistoryEntry has
 * an associated session history entry" — and every one of the six members is a question answered off that entry
 * or off the Navigation that lists it. So there is no state to keep in step with the history: `key`, `id` and
 * the state come straight out of §7.4.1.1's own fields, `index` is a SEARCH of the live entry list, and
 * `sameDocument` compares §7.4.1.2's document against this realm's. An object that cached any of them would
 * answer for the history as it was when the wrapper was minted, which is exactly what a page uses these for.
 *
 * EVERY MEMBER OPENS WITH THE SAME FULLY-ACTIVE TEST AND EACH ONE ANSWERS DIFFERENTLY — the empty string, −1,
 * false, undefined, the empty string. That is not a family of defaults: §7.2.6.5 writes a different one per
 * member because the TYPES differ, and a detached iframe's entries are a thing a page reads on purpose (it is
 * how a router discovers its frame was removed). None of them throws, unlike §7.2.5's History, whose members
 * are a SecurityError in the same state — the two interfaces answer the same question differently and both
 * answers are in their own standards.
 *
 * THE SLOT IS AN OWN PROPERTY UNDER A PRIVATE SYMBOL, for the reason core/idl_slots.h gives: a slot written as
 * a property write is captured by the per-flow COW delta, so a wrapper minted in one arm of a fork is invisible
 * to its sibling and travels with the flow that made it. It matters here more than for most: §7.2.6.4 mints a
 * NavigationHistoryEntry on every push and every replace, so two forked routers hold two entry lists. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/events/event_target.h"
#include "core/frame/navigation.h"
#include "core/frame/navigation_history_entry.h"
#include "core/frame/session_history.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key;         /* the private Symbol this interface's own slot hangs off */
static JSClassID g_nhe_class;
static int       g_ready;
static int       g_id_get_state = -1;

#define NHE_SHE "sessionHistoryEntry"

JSClassID navigation_history_entry_class(void)
{
    DCHECK(g_nhe_class != 0, "NavigationHistoryEntry's class was asked for before "
                             "navigation_history_entry_init declared it");
    return g_nhe_class;
}

static JSValue nhe_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_nhe_class);

    DCHECK(!JS_IsNull(proto),
           "NavigationHistoryEntry.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* OWNED */
}

/* WEB IDL §3.7.5's BRAND. The class is the check, so a getter pulled off the prototype and applied to
   something else is the TypeError a browser answers with rather than a read of a slot that is not there. */
static bool nhe_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_ready, "a NavigationHistoryEntry member ran before navigation_history_entry_init");
    if (JS_GetClassID(this_val) == g_nhe_class) return true;
    JS_ThrowTypeError(ctx, "a NavigationHistoryEntry member was reached on something that is not one");
    return false;
}

JSValue navigation_history_entry_she(JSContext *ctx, JSValueConst nhe)
{
    JSAtom k;
    JSValue slots, she;

    DCHECK(JS_GetClassID(nhe) == g_nhe_class,
           "the session history entry of something that is not a NavigationHistoryEntry was asked for — this "
           "entry point is reached from §7.2.6.3's and §7.2.6.4's list walks, whose lists this component is "
           "the only builder of");
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "NavigationHistoryEntry: the slot key could not be resolved to an atom");
    if (JS_GetOwnSlot(ctx, &slots, nhe, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    DCHECK(JS_IsObject(slots), "a NavigationHistoryEntry carried no slot record — every instance is built by "
                               "navigation_history_entry_new, which places one before it returns");
    she = JS_GetPropertyStr(ctx, slots, NHE_SHE);
    JS_FreeValue(ctx, slots);
    DCHECK(JS_IsObject(she), "a NavigationHistoryEntry's SESSION HISTORY ENTRY is not an entry — §7.4.1.1's "
                             "entries are the objects core/frame/session_history.c builds and nothing else "
                             "reaches this slot");
    return she;
}

JSValue navigation_history_entry_new(JSContext *ctx, JSValueConst she)
{
    JSValue proto = nhe_proto(ctx), obj, slots;
    JSAtom k;

    DCHECK(JS_IsObject(she), "a NavigationHistoryEntry was minted over something that is not a §7.4.1.1 "
                             "session history entry");
    obj = JS_NewObjectProtoClass(ctx, proto, g_nhe_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a NavigationHistoryEntry could not be allocated");
    slots = idl_slots_new(ctx);
    CHECK(!JS_IsException(slots), "a NavigationHistoryEntry's slot record could not be allocated");
    JS_SetPropertyStr(ctx, slots, NHE_SHE, JS_DupValue(ctx, she));
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "NavigationHistoryEntry: the slot key could not be resolved to an atom");
    JS_SetProperty(ctx, obj, k, slots);
    JS_FreeAtom(ctx, k);
    return obj;
}

/* ---- §7.2.6.5's `key`, `id` AND `index` GETTER STEPS, AS ALGORITHMS -------------------------------------------
 *
 * THEY ARE FUNCTIONS BECAUSE A SECOND INTERFACE IS DEFINED IN TERMS OF THEM. §7.2.6.10.3 writes three of
 * NavigationDestination's members as "return this's entry's key / ID / index" — the ENTRY'S GETTER, not the
 * session history entry's field — so the three live here, once, and both interfaces reach the same body. Each
 * opens with the same not-fully-active answer §7.2.6.5 gives it, which is exactly the part a re-derivation in
 * the other component would have lost.
 * The brand is NOT asked here: these are the getter STEPS, and Web IDL performs the brand check before the
 * steps run. js_nhe_get asks it for the attribute reach; navigation_destination.c holds an entry it took out of
 * this component's own list. Both are asserted below. */

JSValue navigation_history_entry_key(JSContext *ctx, JSValueConst nhe)
{
    JSValue she, v;

    DCHECK(JS_GetClassID(nhe) == g_nhe_class,
           "§7.2.6.5's `key` steps ran on something that is not a NavigationHistoryEntry");
    if (!document_fully_active(ctx)) return JS_NewStringLen(ctx, "", 0);
    she = navigation_history_entry_she(ctx, nhe);
    v = session_history_entry_nav_key(ctx, she);
    JS_FreeValue(ctx, she);
    return v;
}

JSValue navigation_history_entry_id(JSContext *ctx, JSValueConst nhe)
{
    JSValue she, v;

    DCHECK(JS_GetClassID(nhe) == g_nhe_class,
           "§7.2.6.5's `id` steps ran on something that is not a NavigationHistoryEntry");
    if (!document_fully_active(ctx)) return JS_NewStringLen(ctx, "", 0);
    she = navigation_history_entry_she(ctx, nhe);
    v = session_history_entry_nav_id(ctx, she);
    JS_FreeValue(ctx, she);
    return v;
}

int64_t navigation_history_entry_index(JSContext *ctx, JSValueConst nhe)
{
    JSValue she;
    int64_t i;

    DCHECK(JS_GetClassID(nhe) == g_nhe_class,
           "§7.2.6.5's `index` steps ran on something that is not a NavigationHistoryEntry");
    if (!document_fully_active(ctx)) return -1;
    /* "Return the result of getting the navigation API entry index of this's session history entry within
       this's relevant global object's navigation API" — a SEARCH of the live entry list every time, which is
       why −1 is a real answer here for an entry the list no longer holds. */
    she = navigation_history_entry_she(ctx, nhe);
    i = navigation_entry_index_of(ctx, she);
    JS_FreeValue(ctx, she);
    return i;
}

/* ---- the five attributes ------------------------------------------------------------------------------------
 *
 * `magic` IS the member, so the fully-active test and the brand are written once. What each member answers when
 * the Document is NOT fully active is §7.2.6.5's own per-member value and is listed at its case. */
enum { NHE_URL = 0, NHE_KEY, NHE_ID, NHE_INDEX, NHE_SAME_DOCUMENT, NHE_N };
static const char *const NHE_NAME[] = { "url", "key", "id", "index", "sameDocument" };

static JSValue js_nhe_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue she, v;

    if (!nhe_brand(ctx, this_val)) return JS_EXCEPTION;
    DCHECK(magic >= NHE_URL && magic < NHE_N,
           "a NavigationHistoryEntry accessor was installed with a magic this interface has no member for");
    /* THREE OF THE FIVE ARE ALGORITHMS OF THEIR OWN, because §7.2.6.10.3 defines NavigationDestination's `key`,
       `id` and `index` as these members' steps — including the not-fully-active answers, which are inside the
       three bodies rather than in a shared prologue here for exactly that reason. */
    switch (magic) {
    case NHE_KEY:   return navigation_history_entry_key(ctx, this_val);
    case NHE_ID:    return navigation_history_entry_id(ctx, this_val);
    case NHE_INDEX: return JS_NewInt64(ctx, navigation_history_entry_index(ctx, this_val));
    default:        break;
    }
    if (!document_fully_active(ctx)) {
        /* §7.2.6.5 answers each of these separately for a Document that is not fully active, and the five
           answers are not one default: "" for `url`, "" for `key`, "" for `id`, −1 for `index`, false for
           `sameDocument`. The two remaining here are the two whose steps nothing else is defined in terms of. */
        switch (magic) {
        case NHE_URL:           return JS_NewStringLen(ctx, "", 0);
        case NHE_SAME_DOCUMENT: return JS_FALSE;
        default:                break;
        }
    }
    she = navigation_history_entry_she(ctx, this_val);
    switch (magic) {
    case NHE_URL:
        /* STEP 4: "If she's document does not equal document, AND she's document state's REQUEST REFERRER
           POLICY is 'no-referrer' or 'origin', then return null." The first conjunct is evaluated rather than
           assumed away: every entry this build creates shares the ACTIVE entry's document state (§7.4.4 steps
           1-3) and therefore names this Document, so the conjunction is false — and the day an entry names
           another one, the field the second conjunct reads has to exist. */
        DCHECK(session_history_entry_is_this_document(ctx, she),
               "§7.2.6.5's `url` reached a session history entry naming ANOTHER Document, and the branch that "
               "answers for one needs a field §7.4.1.2's document state does not carry here: its REQUEST "
               "REFERRER POLICY. Add it in core/frame/session_history.c beside SH_D_ORIGIN — it is the "
               "referrer policy of the request that fetched the entry's document, so §7.4.5's "
               "populate-a-session-history-entry is what writes it — and this member then returns null for "
               "\"no-referrer\" and \"origin\", which is how a same-origin page is kept from reading a URL "
               "another Document is hiding");
        v = session_history_entry_url(ctx, she);
        break;
    case NHE_SAME_DOCUMENT:
        v = JS_NewBool(ctx, session_history_entry_is_this_document(ctx, she));
        break;
    default:
        DFAIL("a NavigationHistoryEntry member was read with a magic no member of this file declares");
        v = JS_UNDEFINED;
        break;
    }
    JS_FreeValue(ctx, she);
    return v;
}

/* §7.2.6.5's `any getState()`: "Return StructuredDeserialize(this's session history entry's NAVIGATION API
   STATE). Rethrow any exceptions."
   A FRESH DESERIALIZATION EACH CALL, which the standard's own note pins: "unless the state value is a
   primitive, entry.getState() !== entry.getState()". That is why this is a METHOD and `history.state` is an
   attribute — the classic API's state is a field the history object holds (§7.4.6.2's restore-the-history-
   object-state writes it), and this one is bytes deserialized on demand. The two are unrelated stores over one
   entry, which the standard says in as many words: "This state is unrelated to the classic history API's
   history.state." */
static JSValue js_nhe_get_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue she, v;

    (void)argc; (void)argv; (void)magic;
    if (!nhe_brand(ctx, this_val)) return JS_EXCEPTION;
    /* STEP 1: "If this's relevant global object's associated Document is not fully active, then return
       undefined." */
    if (!document_fully_active(ctx)) return JS_UNDEFINED;
    she = navigation_history_entry_she(ctx, this_val);
    v = session_history_entry_nav_state(ctx, she);
    JS_FreeValue(ctx, she);
    return v;
}

/* ---- install --------------------------------------------------------------------------------------------- */

void navigation_history_entry_init(JSContext *ctx)
{
    JSClassDef d = { "NavigationHistoryEntry" };

    DCHECK(!g_ready, "navigation_history_entry_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "navigationHistoryEntrySlots", false);
    CHECK(!JS_IsException(g_key), "the NavigationHistoryEntry slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_nhe_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_nhe_class, &d) == 0,
          "NavigationHistoryEntry: the per-realm prototype slot could not be declared");
    g_id_get_state = idl_method_id(ctx, NULL, 0, js_nhe_get_state, 0);
    g_ready = 1;
    realm_declare_intrinsic(navigation_history_entry_install_protos);
}

void navigation_history_entry_install_protos(JSContext *ctx)
{
    JSValue proto, prev, global;
    int i;

    DCHECK(g_ready, "a realm asked for NavigationHistoryEntry before navigation_history_entry_init");
    prev = JS_GetClassProto(ctx, g_nhe_class);
    DCHECK(JS_IsNull(prev), "navigation_history_entry_install_protos ran twice in one realm — everything "
                            "already holding the first prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "NavigationHistoryEntry.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "NavigationHistoryEntry");
    /* `interface NavigationHistoryEntry : EventTarget` — a real prototype chain, so the `dispose` listener a
       page registers with `addEventListener` is §2.7's registration and not a second list. */
    event_target_chain(ctx, proto);
    for (i = 0; i < NHE_N; i++)
        idl_install_accessor(ctx, proto, NHE_NAME[i], js_nhe_get, i, -1);
    idl_install_method(ctx, proto, "getState", 0, g_id_get_state);
    /* §7.2.6.5's ONE event handler IDL attribute, declared ON this interface — the mixin bit is what says so. */
    event_target_install_handlers(ctx, proto, EH_NAVIGATION_HISTORY_ENTRY);
    JS_SetClassProto(ctx, g_nhe_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT. NavigationHistoryEntry declares no constructor, so `new
       NavigationHistoryEntry()` is a TypeError — and its PRESENCE is what `entry instanceof
       NavigationHistoryEntry` needs. */
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "NavigationHistoryEntry",
                      idl_interface_object(ctx, "NavigationHistoryEntry", proto));
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, proto);
}

void navigation_history_entry_free(JSContext *ctx)
{
    /* The prototypes and the interface objects are the REALMS' — each is released with its context. What the
       agent holds is the Symbol it minted, which is a runtime-lifetime value this component owns. */
    JS_FreeValue(ctx, g_key);
    g_key = JS_UNDEFINED;
    g_id_get_state = -1;
    g_ready = 0;
}
