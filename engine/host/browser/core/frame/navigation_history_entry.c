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
#include "core/agent_state.h"
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
/* THE RUNTIME THIS INTERFACE WAS DECLARED IN, AND THE ONLY SLOT THAT SAYS "DECLARED". It replaces the `g_ready`
   flag that used to stand here, and the replacement is not a rename: a flag answers "did init run", while this
   answers "did init run, AND in which runtime" — which is strictly more, is what the release asserts it is
   undoing, and is what nhe_is asks so that no BRAND has to answer a declaration question. Keeping both would be
   one fact with two spellings free to disagree, which is the shape core/agent_state.h opens by describing. */
static JSRuntime *g_nhe_rt;
static int       g_id_get_state = -1;

#define NHE_SHE "sessionHistoryEntry"

/* WEB IDL §3.7.5's BRAND AS A FACT ABOUT THE OBJECT, WITH THE DECLARATION ASKED SEPARATELY — and this file is
 * one predicate rather than five because it had FIVE `JS_GetClassID(x) == g_nhe_class` sites: nhe_brand's
 * `if`, which asked the declaration question first as `DCHECK(g_ready, …)`, and FOUR DCHECKs which asked it
 * not at all.
 * THE COMPARISON INVERTS THE MOMENT THE RELEASE GIVES THE ID BACK, and it inverts in the direction that makes
 * the asserts VACUOUS rather than noisy: quickjs.h defines JS_INVALID_CLASS_ID as 0 and JS_GetClassID returns
 * it for everything that is not an object, so with `g_nhe_class` back at 0 the comparison is TRUE of
 * `undefined`, of a number and of a string alike. The four DCHECKs that assert "this IS a
 * NavigationHistoryEntry" would pass for every primitive there is, and nhe_brand would ADMIT one. None of the
 * five is reachable from any release (see navigation_history_entry_free), which is what makes the declaration
 * an assert rather than a new failure mode — but the predicate must not be the thing that decides that. */
static bool nhe_is(JSValueConst v)
{
    DCHECK(g_nhe_rt != NULL,
           "§7.2.6.5's NavigationHistoryEntry brand was asked before navigation_history_entry_init registered "
           "the class or after navigation_history_entry_free gave it back — with no class there is no answer, "
           "and comparing against the id anyway reports every PRIMITIVE as an entry, because "
           "JS_INVALID_CLASS_ID is 0 and so is a released class id");
    return JS_GetClassID(v) == g_nhe_class;
}

JSClassID navigation_history_entry_class(void)
{
    DCHECK(g_nhe_rt != NULL, "NavigationHistoryEntry's class was asked for before "
                             "navigation_history_entry_init declared it, or after "
                             "navigation_history_entry_free gave it back — and the `g_nhe_class != 0` that "
                             "used to stand here asked the question by reading the value it was about to hand "
                             "out, which cannot tell those two apart from a class id that is simply 0");
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
    if (nhe_is(this_val)) return true;
    JS_ThrowTypeError(ctx, "a NavigationHistoryEntry member was reached on something that is not one");
    return false;
}

JSValue navigation_history_entry_she(JSContext *ctx, JSValueConst nhe)
{
    JSAtom k;
    JSValue slots, she;

    DCHECK(nhe_is(nhe),
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

    DCHECK(nhe_is(nhe),
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

    DCHECK(nhe_is(nhe),
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

    DCHECK(nhe_is(nhe),
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

    /* THE LATCH, AND IT COMPILES OUT IN RELEASE — which is why the class id may not be carried past the
       release. Where a component latches on the CLASS ID itself, a second agent's `_init` returns before
       re-registering; here there is no early return at all, so in a release build a second agent runs this
       whole body, and JS_NewClassID hands a NON-ZERO slot back unchanged rather than allocating. The
       CHECK below is what that turns into: JS_NewClass1 refuses an id already taken, and against a fresh
       runtime it takes one the new `js_class_id_alloc` never issued and will issue to whoever asks next. */
    DCHECK(g_nhe_rt == NULL,
           "navigation_history_entry_init ran twice — the interface is declared once per AGENT");
    g_nhe_rt = JS_GetRuntime(ctx);
    g_key = JS_NewSymbol(ctx, "navigationHistoryEntrySlots", false);
    CHECK(!JS_IsException(g_key), "the NavigationHistoryEntry slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_nhe_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_nhe_class, &d) == 0,
          "NavigationHistoryEntry: the per-realm prototype slot could not be declared");
    g_id_get_state = idl_method_id(ctx, NULL, 0, js_nhe_get_state, 0);
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. It declared NOTHING while its
       row's release column was EMPTY, which is the pair of silences core/platform.c's list reads as agreement:
       a component holding everything and giving none of it back produces character-for-character the report a
       component holding nothing produces. It held four slots, and the class id was given back by nothing. */
    agent_state_ptr("navigation_history_entry", &g_nhe_rt,
                    "the runtime HTML §7.2.6.5 The NavigationHistoryEntry interface's class and its member "
                    "declaration were registered in");
    agent_state_class("navigation_history_entry", &g_nhe_class,
                      "HTML §7.2.6.5 The NavigationHistoryEntry interface's per-realm prototype slot, and the "
                      "brand §7.2.7.1's `required NavigationHistoryEntry from` is declared against");
    agent_state_value("navigation_history_entry", &g_key,
                      "§7.2.6.5's internal-slot key, the Symbol this component minted for the agent");
    agent_state_id("navigation_history_entry", &g_id_get_state,
                   "HTML §7.2.6.5 The NavigationHistoryEntry interface's `getState()` declaration");
    realm_declare_intrinsic(navigation_history_entry_install_protos);
}

void navigation_history_entry_install_protos(JSContext *ctx)
{
    JSValue proto, prev, global;
    int i;

    DCHECK(g_nhe_rt != NULL, "a realm asked for NavigationHistoryEntry before navigation_history_entry_init");
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

/* THE AGENT'S HALF, UNDONE — core/platform.h's third column, and it takes the RUNTIME because that is what an
 * agent is. It took a JSContext until this diff and used it for nothing but JS_FreeValue, which is
 * JS_FreeValueRT(ctx->rt, v); that signature is the whole of what kept this component off the column and made
 * it a hand-written line in three hosts instead.
 *
 * NOTHING READS THIS COMPONENT AFTER IT RUNS, and that is a claim about a list rather than a hope. The five
 * brand sites nhe_is now answers are all page-visible algorithms — §7.2.6.5's own members and the entry-list
 * walks in navigation.c — and the two OUTSIDE readers of navigation_history_entry_class() are
 * core/frame/navigation_destination.c's two member DCHECKs and
 * core/events/navigation_current_entry_change_event.c's idl_iface_brand, which captures the id at its own
 * INIT. Every row that releases after this one on that column gives back ids or frees its own values; not one
 * names a static of this file. And there is no collector entry to worry about: `JSClassDef d = {
 * "NavigationHistoryEntry" }` leaves finalizer and gc_mark null, so core/agent_state.h's closing obligation —
 * a finalizer running after the release column must reach its record through JS_GetAnyOpaque — has nothing to
 * apply to here. */
void navigation_history_entry_free(JSRuntime *rt)
{
    /* NOT a null check. This is a row on core/platform.c's list, whose declare pass is unconditional and which
       runs only where platform_agent_init ran, so a null runtime here is a host tearing down a browser it
       never built — and a silent return would make that indistinguishable from a release that worked. */
    DCHECK(g_nhe_rt != NULL,
           "§7.2.6.5's NavigationHistoryEntry was released in an agent that never declared it — "
           "navigation_history_entry_init is a row on core/platform.c's declare column, so reaching here "
           "without it is a teardown of a browser that was never brought up");
    DCHECK(g_nhe_rt == rt,
           "§7.2.6.5's NavigationHistoryEntry was released against a RUNTIME other than the one it was "
           "declared in — its class and its member declaration are registrations in that runtime, and zeroing "
           "them against another leaves both standing in the runtime that issued them");
    /* The prototypes and the interface objects are the REALMS' — each is released with its context. What the
       agent holds is the Symbol it minted, which is a runtime-lifetime value this component owns. */
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    g_id_get_state = -1;
    /* AND THE CLASS ID, which this release kept. core/agent_state.h settles it: a class is registered in a
       RUNTIME, so a carried id names a class in a runtime that is gone — and because JS_NewClassID returns a
       non-zero slot UNCHANGED rather than allocating, a second agent's init would hand JS_NewClass a number
       the new runtime's own allocator never issued and will issue to whichever component asks next. nhe_is is
       what makes zeroing it safe to state: the fold it replaced would have reported every primitive as an
       entry from this line onward. */
    g_nhe_class = 0;
    g_nhe_rt = NULL;
}
