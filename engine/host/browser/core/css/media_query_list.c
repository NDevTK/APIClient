/* `MediaQueryList`, `MediaQueryListEvent`, `Window.matchMedia` — CSSOM VIEW §4.2. See media_query_list.h for why
   `matches` is concolic with a real example, and why the reported-state latch time-travels. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/css/media_query.h"
#include "core/css/media_query_list.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"
#include "solver/cow.h"

static JSClassID g_mql_class, g_ev_class;
static int  g_slot = -1;                      /* this document's collection, in creation order */
static int  g_id_match = -1, g_id_add = -1, g_id_remove = -1, g_id_ev_ctor = -1;
static JSValue g_ev_key = JS_UNDEFINED;       /* the private Symbol MediaQueryListEvent's two slots live under */
/* THE RUNTIME THIS COMPONENT WAS DECLARED IN, and the only slot that says "declared". It replaces a `g_ready`
   flag, and that is not a rename: a flag answers "did init run", this answers "did init run, AND in which
   runtime" — which is strictly more, and is exactly what media_query_list_free asserts it is undoing. Keeping
   both would be one fact with two spellings free to disagree, which is the shape core/agent_state.h opens by
   describing. */
static JSRuntime *g_rt;

/* ---- the record --------------------------------------------------------------------------------------------- */

typedef struct {
    MediaQuerySet *set;
    char          *media;         /* the serialization — computed once, immutable, so it never time-travels */
    /* CSSOM View §4.2 The MediaQueryList Interface's "the last time these steps were run" state. THE one
       mutable field, and a POD latch, which is exactly what cow_capture_host_state is for (cow.h: a byte
       copy of a JSValue would make an uncounted reference, and there is no JSValue in this record). */
    bool           last_matches;
} MqlData;

/* THE COLLECTOR'S ENTRY REACHES THE RECORD FROM THE OBJECT AND READS NO STATIC OF THIS FILE — core/agent_state.h's
 * note on what a release owes a finalizer, and this was the LIVE half of the group that landed it here.
 * media_query_list_free is a row on core/platform.h's release column now and gives g_mql_class back to 0, and
 * every host's teardown is platform_agent_free() … JS_RunGC … JS_FreeRuntime, so this entry runs with the id
 * already zero. `JS_GetOpaque(val, 0)` answers NULL for every object there is — no object wears class 0 — so
 * this would have returned NULL for every MediaQueryList a page still held, and `if (!d) return;` was the
 * swallow that made it silent: a MediaQuerySet, its serialization and the record itself, leaked per live list.
 * All three are malloc'd blocks, which appear in NEITHER of JS_FreeRuntime's censuses — not the gc_obj_list
 * walk, which reports objects that were not given back, and not the atom census — so nothing this tree has
 * would have reported it. `matchMedia` is the surface that mints them, which is to say a responsive bundle
 * holds one per media query it watches.
 * The id is not read because it is not needed: the collector dispatched HERE THROUGH the class, so the class is
 * a fact this function already has. */
static void mql_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    MqlData *d = JS_GetAnyOpaque(val, &id);

    (void)rt;
    (void)id;
    /* NOT `if (!d) return;`. js_match_media is the one mint, and between JS_NewObjectProtoClass and
       JS_SetOpaque it allocates nothing on the JS heap and runs no page code — so there is no half-built
       MediaQueryList for this entry to meet, and a NULL here means an object wearing §4.2's class was built
       somewhere that is not this file. */
    DCHECK(d != NULL,
           "a MediaQueryList was finalized with no record — js_match_media attaches it before the object can "
           "reach anything that would free it");
    media_query_free(d->set);
    free(d->media);
    free(d);
}

/* THE RECORD, captured where the flow REACHES it (CLAUDE.md §COW). A flow that has reached this record is one
   that may write its latch, the delta dedups to one entry, and there is then no write site left to miss —
   which is the only way this goes wrong. */
static MqlData *mql_data(JSContext *ctx, JSValueConst obj)
{
    MqlData *d;

    /* WEB IDL §3.7.5's BRAND IS A FACT ABOUT THE OBJECT, AND THE DECLARATION IS A SEPARATE QUESTION — asked
       here rather than folded into the comparison below, because the comparison cannot tell the two apart.
       JS_GetOpaque2 answers NULL and throws for a class id that does not match, so once media_query_list_free
       has given g_mql_class back to 0 this brand does not admit anything it should reject; it does the other
       thing, and reports every LIVE MediaQueryList as not being one. That is a guaranteed FALSE TypeError,
       which is core/frame/remote_object.c's shape and is precisely why the rule is "reads no static its own
       release resets" rather than "the comparison is safe". No release reaches this function — every caller is
       a §4.2 member or update-the-rendering step 10 — which is what makes this an assert and not a new failure
       mode, but the predicate must not be the thing that decides that. */
    DCHECK(g_rt != NULL,
           "a MediaQueryList's brand was asked before media_query_list_init registered CSSOM View §4.2's class "
           "or after media_query_list_free gave it back — with no class there is no answer, and asking "
           "JS_GetOpaque2 against a released id reports every live MediaQueryList as something else");
    d = JS_GetOpaque2(ctx, obj, g_mql_class);
    if (!d) return NULL;
    cow_capture_host_state(ctx, obj, &d->last_matches, sizeof d->last_matches);
    return d;
}

/* ---- `matches` ----------------------------------------------------------------------------------------------
 *
 * THE VALUE AND THE ENGINE's OWN READ OF IT ARE core/css/media_query.h's, and they moved there when a SECOND
 * member started asking the same question: CSS Conditional §7.3's `@media` rule reports whether its condition holds and
 * the cascade decides whether the rule applies at all, and those are the same fact about the same document as
 * this `matches` is. Two spellings of one source identity fork one predicate twice, so the arm that answered
 * `true` here would have resolved the cascade as though it were false. What stays this file's is §4.2's
 * OBJECT — its listener list, its reported-state latch, and the change algorithm below. */

static bool mql_matches_now(JSContext *ctx, JSValueConst obj)
{
    MqlData *d = mql_data(ctx, obj);

    DCHECK(d != NULL, "update-the-rendering step 10 read a MediaQueryList that is not one");
    return media_query_matches_now(ctx, d->set);
}

static JSValue js_mql_matches(JSContext *ctx, JSValueConst this_val, int magic)
{
    MqlData *d = mql_data(ctx, this_val);

    (void)magic;
    if (!d) return JS_EXCEPTION;
    return media_query_matches_value(ctx, d->set);
}

static JSValue js_mql_media(JSContext *ctx, JSValueConst this_val, int magic)
{
    MqlData *d = mql_data(ctx, this_val);

    (void)magic;
    if (!d) return JS_EXCEPTION;
    return JS_NewString(ctx, d->media);
}

/* §4.2: `addListener`/`removeListener` are DEFINED as add/remove an event listener for `change`. They are the
   pre-`EventTarget` spelling that the standard keeps for compatibility, and stating them as the same algorithm
   is the whole point — a second listener list would make `mql.addListener(f)` invisible to
   `mql.removeEventListener("change", f)`, which every browser treats as the same registration. */
static JSValue js_mql_add_listener(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    DCHECK(argc >= 1, "MediaQueryList.addListener reached its body with no callback — §3.6 step 5's TypeError "
                      "for a missing required argument is the declaration's");
    if (!mql_data(ctx, this_val)) return JS_EXCEPTION;
    /* `EventListener? callback` — a null callback registers nothing, which is what §2.7's own algorithm says. */
    if (!JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0]))
        event_target_add_listener(ctx, this_val, "change", argv[0], /*capture*/ false, /*once*/ false,
                                  /*passive*/ -1, /*signal*/ JS_UNDEFINED);
    return JS_UNDEFINED;
}

static JSValue js_mql_remove_listener(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    (void)magic;
    DCHECK(argc >= 1, "MediaQueryList.removeListener reached its body with no callback — §3.6 step 5's "
                      "TypeError is the declaration's");
    if (!mql_data(ctx, this_val)) return JS_EXCEPTION;
    if (!JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0]))
        event_target_remove_listener(ctx, this_val, "change", argv[0]);
    return JS_UNDEFINED;
}

/* ---- `MediaQueryListEvent` ----------------------------------------------------------------------------------
   `[Exposed=Window] interface MediaQueryListEvent : Event { constructor(CSSOMString type, optional
      MediaQueryListEventInit eventInitDict = {}); readonly attribute CSSOMString media;
      readonly attribute boolean matches; }` */

static JSValue ev_slot(JSContext *ctx, JSValueConst ev, const char *name)
{
    JSAtom k;
    JSValue slots, v;

    if (!JS_IsObject(ev)) return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_ev_key);
    if (k == JS_ATOM_NULL) return JS_UNDEFINED;
    /* AN OWN SLOT, never a property LOOKUP: a lookup walks the prototype chain into the solver's absent-state
       seam and would mint a concolic for a name nobody defined. */
    if (JS_GetOwnSlot(ctx, &slots, ev, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_UNDEFINED; }
    v = JS_GetPropertyStr(ctx, slots, name);
    JS_FreeValue(ctx, slots);
    return v;
}

static JSValue js_ev_media(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    return ev_slot(ctx, this_val, "media");
}

static JSValue js_ev_matches(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    return ev_slot(ctx, this_val, "matches");
}

/* Mint one over an Event this component then re-points at MediaQueryListEvent.prototype — a derived interface
   is a prototype chain, not a second kind of event object. `media` and `matches` are CONSUMED. */
static JSValue ev_new(JSContext *ctx, JSValue ev, JSValue media, JSValue matches)
{
    JSValue slots;

    if (JS_IsException(ev)) { JS_FreeValue(ctx, media); JS_FreeValue(ctx, matches); return ev; }
    {
        JSValue proto = JS_GetClassProto(ctx, g_ev_class);
        DCHECK(!JS_IsNull(proto),
               "MediaQueryListEvent.prototype was asked for in a realm that never ran its install");
        JS_SetPrototype(ctx, ev, proto);
        JS_FreeValue(ctx, proto);
    }
    slots = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(slots), "MediaQueryListEvent: OOM allocating its slots");
    JS_SetPropertyStr(ctx, slots, "media", media);
    JS_SetPropertyStr(ctx, slots, "matches", matches);
    {
        JSAtom k = JS_ValueToAtom(ctx, g_ev_key);
        CHECK(k != JS_ATOM_NULL, "the MediaQueryListEvent slot key could not be reached");
        JS_DefinePropertyValue(ctx, ev, k, slots, 0);   /* an internal slot: not enumerable, not writable */
        JS_FreeAtom(ctx, k);
    }
    return ev;
}

static const IdlArgType EV_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
/* `dictionary MediaQueryListEventInit : EventInit { CSSOMString media = ""; boolean matches = false; }`, in
   §3.2.17's READ order — which is NOT the IDL's declaration order, since step 4 sorts a dictionary's own
   members lexicographically and `matches` is written second. `media` and `matches` are declared on
   MediaQueryListEventInit and the three above are EventInit's, so they carry the level that says so. */
static const IdlDictMember EV_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "matches", IDL_BOOLEAN, false, NULL, 1 }, { "media", IDL_DOMSTRING, false, NULL, 1 },
};

static JSValue js_ev_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue ev, media;
    const char *type;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor MediaQueryListEvent requires 'new'");
    DCHECK(argc >= 1, "MediaQueryListEvent's constructor reached its body with no type — §3.6 step 5's "
                      "TypeError is the declaration's");
    type = JS_ToCString(ctx, argv[0]);   /* a real string by now: this cannot reach the page */
    if (!type) return JS_EXCEPTION;
    ev = event_new_untrusted(ctx, type, idl_dict_bool(ctx, init, "bubbles"),
                             idl_dict_bool(ctx, init, "cancelable"));   /* §2.2: constructed IS untrusted */
    JS_FreeCString(ctx, type);
    media = idl_dict_get(ctx, init, "media");
    /* `CSSOMString media = ""` — the dictionary's declared default, which an absent member takes. */
    if (!JS_IsString(media)) { JS_FreeValue(ctx, media); media = JS_NewString(ctx, ""); }
    return ev_new(ctx, ev, media, JS_NewBool(ctx, idl_dict_bool(ctx, init, "matches")));
}

/* ---- the document's collection — CSSOM VIEW §4.2 ------------------------------------------------------------- */

/* This document's list, OWNED. A per-realm baseline Array: its mutations are property writes the heap COW
   already captures, so a MediaQueryList created by one arm of a fork does not exist for its sibling — which is
   the same reason §8.12 Animation frames's map of animation frame callbacks is one. */
static JSValue mql_collection(JSContext *ctx)
{
    JSValue arr;

    DCHECK(g_rt != NULL, "a document's MediaQueryList collection was reached before §4.2 was declared, or "
                         "after media_query_list_free gave its slot back");
    arr = realm_value_get(ctx, g_slot);
    DCHECK(JS_IsArray(arr),
           "a realm answered §4.2's collection of MediaQueryList objects with no collection — every Document "
           "is given one at creation, so this realm never ran media_query_list_install_proto and its "
           "`matchMedia` would register into nothing");
    return arr;
}

static uint32_t arr_len(JSContext *ctx, JSValueConst arr)
{
    JSValue len = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

bool media_query_list_pending(JSContext *ctx)
{
    JSValue arr = mql_collection(ctx);
    uint32_t i, n = arr_len(ctx, arr);
    bool any = false;

    for (i = 0; i < n && !any; i++) {
        JSValue target = JS_GetPropertyUint32(ctx, arr, i);
        MqlData *d = mql_data(ctx, target);

        DCHECK(d != NULL, "§4.2's collection held something that is not a MediaQueryList");
        any = mql_matches_now(ctx, target) != d->last_matches;
        JS_FreeValue(ctx, target);
    }
    JS_FreeValue(ctx, arr);
    return any;
}

uint32_t media_query_list_count(JSContext *ctx)
{
    JSValue arr = mql_collection(ctx);
    uint32_t n = arr_len(ctx, arr);

    JS_FreeValue(ctx, arr);
    return n;
}

JSValue media_query_list_change(JSContext *ctx, uint32_t i, JSValue *ptarget)
{
    JSValue arr = mql_collection(ctx), target;
    MqlData *d;
    bool now;

    DCHECK(i < arr_len(ctx, arr),
           "update-the-rendering step 10 walked past the collection snapshot it took — the collection only "
           "ever GROWS during a frame, so an index inside the snapshot cannot fall off the end");
    target = JS_GetPropertyUint32(ctx, arr, i);
    JS_FreeValue(ctx, arr);
    d = mql_data(ctx, target);
    DCHECK(d != NULL, "§4.2's collection held something that is not a MediaQueryList");
    now = mql_matches_now(ctx, target);
    if (now == d->last_matches) {          /* "if target's matches state has NOT changed": nothing to report */
        JS_FreeValue(ctx, target);
        *ptarget = JS_UNDEFINED;
        return JS_UNDEFINED;
    }
    /* LATCHED BEFORE THE FIRE, because the fire runs the page's code and that code reads `.matches`. The latch
       is the state we are ABOUT to report, so a listener that queries the same list must not see the frame
       still owing it a change. */
    d->last_matches = now;
    *ptarget = target;
    /* CSSOM View §4.2: "fire an event named change at target using MediaQueryListEvent, with its isTrusted
       attribute initialized to true, its media attribute initialized to target's media, and its matches
       attribute initialized to target's matches state." It does not bubble and is not cancelable — nothing in
       the algorithm reads a canceled flag. */
    return ev_new(ctx, event_new(ctx, "change", /*bubbles*/ false, /*cancelable*/ false),
                  JS_NewString(ctx, d->media), JS_NewBool(ctx, now));
}

/* ---- `Window.matchMedia` ------------------------------------------------------------------------------------- */

/* CSSOM View §4 Extensions to the Window Interface: `[NewObject] MediaQueryList matchMedia(CSSOMString query)`.
   The number here read §7 until this diff, and §7 is Extensions to the HTMLElement Interface — a citation that
   reads as authoritative and sends the reader to a section saying nothing about this member. §4 is where the
   partial `Window` declares it and where its own steps are stated; §4.2 below is the interface it returns.
   1. Let parsed be the result of parsing query. 2. Return a new MediaQueryList object with its document set to
   this's associated Document and its media set to parsed. No page code runs: the declaration converted the
   argument, and everything after it is this component's own data. */
static JSValue js_match_media(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue obj, arr;
    MqlData *d;
    const char *q;

    (void)this_val; (void)magic;
    DCHECK(argc >= 1, "matchMedia reached its body with no query — §3.6 step 5's TypeError for a call short "
                      "of a member's required arguments is the declaration's");
    DCHECK(JS_IsString(argv[0]) || concolic_is(argv[0]),
           "matchMedia's query reached the body unconverted — §7 declares it CSSOMString and the args machine "
           "converts a declared member's arguments before the body runs");
    /* AN UNKNOWN QUERY IS STILL A QUERY. A concolic argument denotes its SHAPE — a real, stable string — which
       is what concolic_name_cstr is for at every other DOM member that takes a name; the media query language
       has no way to say "unknown", and parsing the shape gives `not all`, which is the honest answer for a
       string nobody has pinned. */
    q = concolic_name_cstr(ctx, argv[0]);
    if (!q) return JS_EXCEPTION;
    {
        JSValue proto = JS_GetClassProto(ctx, g_mql_class);
        DCHECK(!JS_IsNull(proto), "matchMedia was called in a realm with no MediaQueryList.prototype");
        obj = JS_NewObjectProtoClass(ctx, proto, g_mql_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj)) { JS_FreeCString(ctx, q); return obj; }
    d = calloc(1, sizeof *d);
    CHECK(d != NULL, "matchMedia: OOM building a MediaQueryList");
    d->set = media_query_parse(q);
    JS_FreeCString(ctx, q);
    d->media = media_query_serialize(d->set);
    CHECK(d->media != NULL, "matchMedia: OOM serializing a media query list");
    JS_SetOpaque(obj, d);
    /* THE REPORTED STATE STARTS AT THE CURRENT ONE. CSSOM View §4.2 fires when the state has changed
       "since the last time these steps were run", and a list created between two frames has not changed
       since it was created — a zero-initialised latch would make every `matchMedia("(min-width: 1px)")`
       fire a spurious `change` on the very next rendering opportunity. */
    d->last_matches = media_query_matches(ctx, d->set);
    /* CREATION ORDER is the order §4.2 walks, so the collection is appended to and never reordered. */
    arr = mql_collection(ctx);
    JS_SetPropertyUint32(ctx, arr, arr_len(ctx, arr), JS_DupValue(ctx, obj));
    JS_FreeValue(ctx, arr);
    return obj;
}

/* ---- install --------------------------------------------------------------------------------------------- */

void media_query_list_init(JSContext *ctx)
{
    static const IdlArgType MATCH_ARGS[1] = { IDL_DOMSTRING };
    static const IdlArgType LISTENER_ARGS[1] = { IDL_CALLBACK_INTERFACE_NULLABLE };
    JSRuntime *rt = JS_GetRuntime(ctx);

    /* THE LATCH, AND IT COMPILES OUT IN RELEASE — which is why neither class id may be carried past the
       release. There is no early return here, so under -DAPICLIENT_DEV=0 a second agent runs this whole body,
       and JS_NewClassID hands a NON-ZERO slot back UNCHANGED rather than allocating: the CHECKs below would
       then ask JS_NewClass for numbers the new runtime's own allocator never issued and is about to issue to
       whichever component asks next. */
    DCHECK(g_rt == NULL, "media_query_list_init ran twice — §4.2's interfaces are declared once per agent");
    g_rt = rt;
    {
        JSClassDef mql = { "MediaQueryList", .finalizer = mql_finalizer };
        JSClassDef ev = { "MediaQueryListEvent" };
        JS_NewClassID(rt, &g_mql_class);
        CHECK(JS_NewClass(rt, g_mql_class, &mql) == 0,
              "MediaQueryList: the per-realm prototype slot could not be declared");
        JS_NewClassID(rt, &g_ev_class);
        CHECK(JS_NewClass(rt, g_ev_class, &ev) == 0,
              "MediaQueryListEvent: the per-realm prototype slot could not be declared");
    }
    g_ev_key = JS_NewSymbol(ctx, "mediaQueryListEventSlots", false);
    CHECK(!JS_IsException(g_ev_key), "the MediaQueryListEvent slot key allocation failed");
    g_id_match = idl_method_id(ctx, MATCH_ARGS, 1, js_match_media, 0);
    g_id_add = idl_method_id(ctx, LISTENER_ARGS, 1, js_mql_add_listener, 0);
    g_id_remove = idl_method_id(ctx, LISTENER_ARGS, 1, js_mql_remove_listener, 0);
    g_id_ev_ctor = idl_method_id_dict(ctx, EV_CTOR_ARGS, 2, EV_INIT,
                                      (int)(sizeof(EV_INIT) / sizeof(EV_INIT[0])), js_ev_ctor, 0);
    idl_optional_from(1);   /* `optional MediaQueryListEventInit eventInitDict = {}` */
    g_slot = realm_value_declare(ctx, "CSSOM VIEW §4.2 the document's MediaQueryList objects");
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. It declared NOTHING while its
       row's release column was EMPTY, which is the pair of silences core/platform.c's list reads as agreement:
       a component that holds everything and gives none of it back produces character-for-character the report
       a component that holds nothing produces. It held eight slots and gave six of them back; the two it kept
       are the CLASS IDS, and one of those is the class the finalizer above looks its record up under. */
    agent_state_ptr("media_query_list", &g_rt,
                    "the runtime CSSOM View §4.2 The MediaQueryList Interface's two classes, its realm-value "
                    "slot and its four member declarations were registered in");
    agent_state_class("media_query_list", &g_mql_class,
                      "CSSOM View §4.2 The MediaQueryList Interface's per-realm prototype slot, the brand "
                      "every member of it checks, and the class its finalizer is dispatched through");
    agent_state_class("media_query_list", &g_ev_class,
                      "CSSOM View §4.2 The MediaQueryList Interface's MediaQueryListEvent per-realm prototype "
                      "slot");
    agent_state_value("media_query_list", &g_ev_key,
                      "§4.2's internal-slot key, the Symbol MediaQueryListEvent's `media` and `matches` live "
                      "under");
    agent_state_id("media_query_list", &g_slot,
                   "the per-realm slot CSSOM View §4.2's collection of MediaQueryList objects lives in");
    agent_state_id("media_query_list", &g_id_match,
                   "CSSOM View §4 Extensions to the Window Interface's `matchMedia(query)` declaration");
    agent_state_id("media_query_list", &g_id_add,
                   "CSSOM View §4.2 The MediaQueryList Interface's `addListener(callback)` declaration");
    agent_state_id("media_query_list", &g_id_remove,
                   "CSSOM View §4.2 The MediaQueryList Interface's `removeListener(callback)` declaration");
    agent_state_id("media_query_list", &g_id_ev_ctor,
                   "CSSOM View §4.2 The MediaQueryList Interface's MediaQueryListEvent constructor "
                   "declaration");
    realm_declare_intrinsic(media_query_list_install_proto);
}

void media_query_list_install_proto(JSContext *ctx)
{
    JSValue proto, prev, base, arr;

    DCHECK(g_rt != NULL, "a realm asked for MediaQueryList.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_mql_class);
    DCHECK(JS_IsNull(prev), "media_query_list_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = event_target_derived_proto(ctx);              /* §4.2: `MediaQueryList : EventTarget` */
    idl_interface_tag(ctx, proto, "MediaQueryList");
    event_target_install_handlers(ctx, proto, EH_MEDIA_QUERY_LIST);
    idl_install_accessor(ctx, proto, "media", js_mql_media, 0, -1);
    idl_install_accessor(ctx, proto, "matches", js_mql_matches, 0, -1);
    idl_install_method(ctx, proto, "addListener", g_id_add);
    idl_install_method(ctx, proto, "removeListener", g_id_remove);
    JS_SetClassProto(ctx, g_mql_class, proto);

    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "MediaQueryListEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "MediaQueryListEvent");
    idl_install_accessor(ctx, proto, "media", js_ev_media, 0, -1);
    idl_install_accessor(ctx, proto, "matches", js_ev_matches, 0, -1);
    JS_SetClassProto(ctx, g_ev_class, proto);

    /* THE DOCUMENT's COLLECTION, built with the realm so it belongs to the pre-boot BASELINE. Created inside a
       flow instead it would be that flow's private array and every sibling would walk a collection nobody
       wrote — the same reason §8.12 Animation frames's map is built here. */
    arr = JS_NewArray(ctx);
    CHECK(!JS_IsException(arr), "§4.2's collection of MediaQueryList objects could not be allocated");
    realm_value_set(ctx, g_slot, arr);
}

void media_query_list_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor, proto;

    DCHECK(g_rt != NULL, "CSSOM VIEW §4.2 was installed before it was declared");
    idl_install_method(ctx, (JSValue)global, "matchMedia", g_id_match);

    /* §3.7.1: an interface with NO constructor still has an interface object, and calling it is a TypeError. */
    proto = JS_GetClassProto(ctx, g_mql_class);
    DCHECK(!JS_IsNull(proto), "MediaQueryList was installed in a realm with no prototype for it");
    ctor = idl_interface_object(ctx, "MediaQueryList", proto);
    CHECK(!JS_IsException(ctor), "the MediaQueryList interface object could not be allocated");
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "MediaQueryList", ctor);

    ctor = idl_step_constructor(ctx, "MediaQueryListEvent", g_id_ev_ctor);
    CHECK(!JS_IsException(ctor), "the MediaQueryListEvent interface object could not be allocated");
    proto = JS_GetClassProto(ctx, g_ev_class);
    DCHECK(!JS_IsNull(proto), "MediaQueryListEvent was installed in a realm with no prototype for it");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "MediaQueryListEvent", ctor);
}

/* THE AGENT'S HALF, UNDONE — a row on core/platform.h's release column, and it takes the RUNTIME because that
 * is what an agent is. It took a JSContext until this diff and used it for nothing but JS_FreeValue, which is
 * JS_FreeValueRT(ctx->rt, v); that signature is the whole of what kept this component off the column and made
 * it a hand-written line in three hosts instead.
 *
 * AND ITS PLACE ON THE COLUMN IS NOT WHERE THE HOSTS PUT IT. All three wrote `page_reveal_free(ctx);
 * media_query_list_free(ctx);` adjacently, AFTER platform_agent_free — so this component was released after
 * CSSOM View §4's `viewport` and §12's `visual_viewport`, both of which are already rows. That is the
 * dependent-released-second order: §4.2's whole answer is a predicate over the viewport (media_query_matches
 * reads it per realm), which is exactly why `media_query_list` is DECLARED after `viewport`. Reverse
 * declaration order gives media_query_list, then visual_viewport, then viewport, then page_reveal — the hosts'
 * pair inverted, with the two viewport rows between them — and no author has to agree with any other about it.
 *
 * NOTHING READS THIS COMPONENT AFTER IT RUNS, and that is a claim about a list rather than a hope. The only
 * file outside this one that names any symbol of it is core/rendering/rendering.c — `media_query_list_pending`
 * in its step-4 test, `_count` and `_change` at step 10 — and `rendering` is the row AFTER this one, so
 * rendering_free has already run. core/css/media_query.c, which this file is built over, holds no agent state
 * at all: it is a parser, a serializer and an evaluator over `const` tables. No other release on the column and
 * no host teardown line names a static of this file. */
void media_query_list_free(JSRuntime *rt)
{
    /* NOT `if (!g_rt) return;`. This is a row on core/platform.c's list, whose declare pass is unconditional
       and which runs only where platform_agent_init ran, so a null runtime here is a host tearing down a
       browser it never built — and a silent return would make that indistinguishable from a release that
       worked. */
    DCHECK(g_rt != NULL,
           "CSSOM View §4.2's MediaQueryList was released in an agent that never declared it — "
           "media_query_list_init is a row on core/platform.c's declare column, so reaching here without it "
           "is a teardown of a browser that was never brought up");
    DCHECK(g_rt == rt,
           "CSSOM View §4.2's MediaQueryList was released against a RUNTIME other than the one it was declared "
           "in — its two classes, its realm-value slot and its four member declarations are registrations in "
           "that runtime, and zeroing them against another leaves every one of them standing in the runtime "
           "that issued them");
    /* The prototypes and the collections are the REALMS' — each goes with its context. */
    JS_FreeValueRT(rt, g_ev_key);
    g_ev_key = JS_UNDEFINED;
    g_slot = g_id_match = g_id_add = g_id_remove = g_id_ev_ctor = -1;
    /* AND THE TWO CLASS IDS, which this release kept. core/agent_state.h settles it: a class is registered in a
       RUNTIME, so a carried id names a class in a runtime that is gone — and because JS_NewClassID returns a
       non-zero slot UNCHANGED rather than allocating, a second agent's media_query_list_init would hand
       JS_NewClass numbers the new runtime's own allocator never issued and will issue to whichever component
       asks next. Giving g_mql_class back is what made mql_finalizer's `JS_GetOpaque(val, g_mql_class)` a leak,
       so that entry reaches its record through JS_GetAnyOpaque now — the obligation the zeroing creates,
       discharged in the same diff rather than named in a comment. */
    g_mql_class = 0;
    g_ev_class = 0;
    g_rt = NULL;
}
