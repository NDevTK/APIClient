/* `MediaQueryList`, `MediaQueryListEvent`, `Window.matchMedia` — CSSOM VIEW §4.2. See media_query_list.h for why
   `matches` is concolic with a real example, and why the reported-state latch time-travels. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/media_query.h"
#include "core/css/media_query_list.h"
#include "core/dom/document.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include "solver/decide.h"

static JSClassID g_mql_class, g_ev_class;
static int  g_slot = -1;                      /* this document's collection, in creation order */
static int  g_id_match = -1, g_id_add = -1, g_id_remove = -1, g_id_ev_ctor = -1;
static JSValue g_ev_key = JS_UNDEFINED;       /* the private Symbol MediaQueryListEvent's two slots live under */
static int  g_ready;

/* ---- the record --------------------------------------------------------------------------------------------- */

typedef struct {
    MediaQuerySet *set;
    char          *media;         /* the serialization — computed once, immutable, so it never time-travels */
    /* §4.2's "the last time these steps were run" state. THE one mutable field, and a POD latch, which is
       exactly what cow_capture_host_state is for (cow.h: a byte copy of a JSValue would make an uncounted
       reference, and there is no JSValue in this record). */
    bool           last_matches;
} MqlData;

static void mql_finalizer(JSRuntime *rt, JSValue val)
{
    MqlData *d = JS_GetOpaque(val, g_mql_class);

    (void)rt;
    if (!d) return;
    media_query_free(d->set);
    free(d->media);
    free(d);
}

/* THE RECORD, captured where the flow REACHES it (CLAUDE.md §COW). A flow that has reached this record is one
   that may write its latch, the delta dedups to one entry, and there is then no write site left to miss —
   which is the only way this goes wrong. */
static MqlData *mql_data(JSContext *ctx, JSValueConst obj)
{
    MqlData *d = JS_GetOpaque2(ctx, obj, g_mql_class);

    if (!d) return NULL;
    cow_capture_host_state(ctx, obj, &d->last_matches, sizeof d->last_matches);
    return d;
}

/* ---- `matches` ---------------------------------------------------------------------------------------------- */

/* THE SOURCE IDENTITY of one document's answer to one query. The DOCUMENT is part of it and that is not
   decoration: a child navigable's viewport is 300 CSS pixels wide and the top-level traversable's is 1280, so
   `(min-width: 600px)` is genuinely a different question in each — one key would let a branch taken in the
   parent decide the iframe's. The SHAPE is the human-readable half a finding carries, so it names the query
   and not the document id. `out` must hold both; the caller sizes it. */
static void mql_source(JSContext *ctx, const MqlData *d, char *shape, size_t nshape, char *src, size_t nsrc)
{
    JSValueConst self = document_window_proxy(ctx);

    DCHECK(window_proxy_is(self), "a MediaQueryList was reached in a realm whose document has no WindowProxy");
    snprintf(shape, nshape, "{media:%s}", d->media);
    snprintf(src, nsrc, "{media#%u}%s", (unsigned)window_proxy_doc(self), d->media);
}

/* The modelled answer — the real predicate run against the real modelled viewport. */
static bool mql_computed(JSContext *ctx, const MqlData *d)
{
    return media_query_matches(ctx, d->set);
}

/* §4.2's `matches`, as the page sees it: the computed answer carried as the EXAMPLE of a concolic keyed on
   this document's answer to this query. concolic_source_wrap hands back the plain boolean where no source
   overlay is installed (a conformance host), which is what keeps this component testable against the standard. */
static JSValue mql_matches_value(JSContext *ctx, JSValueConst this_val)
{
    MqlData *d = mql_data(ctx, this_val);
    char shape[256], src[256];

    if (!d) return JS_EXCEPTION;
    mql_source(ctx, d, shape, sizeof shape, src, sizeof src);
    return concolic_source_wrap(ctx, shape, src, JS_NewBool(ctx, mql_computed(ctx, d)));
}

static JSValue js_mql_matches(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    return mql_matches_value(ctx, this_val);
}

/* THE ENGINE's OWN READ. Step 10 is C and cannot fork, so it takes the arm this flow already committed to —
   asked BY VALUE so decide.c stays the only speller of the constraint key — and falls back to the modelled
   example when the flow has committed to neither. page_visibility.c does the identical thing for `hidden`,
   because it is the identical problem. */
static bool mql_matches_now(JSContext *ctx, JSValueConst obj)
{
    JSValue v = mql_matches_value(ctx, obj);
    int arm;
    bool r;

    DCHECK(!JS_IsException(v), "update-the-rendering step 10 read a MediaQueryList that is not one");
    if (!concolic_is(v)) {
        r = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        return r;
    }
    arm = decide_value_arm(v);
    if (arm >= 0) {
        JS_FreeValue(ctx, v);
        return arm == 1;
    }
    {
        JSValue ex = concolic_example(ctx, v);
        r = JS_ToBool(ctx, ex);
        JS_FreeValue(ctx, ex);
    }
    JS_FreeValue(ctx, v);
    return r;
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
    DCHECK(argc >= 1, "MediaQueryList.addListener reached its body with no callback — §3.6.2 step 1's TypeError "
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
    DCHECK(argc >= 1, "MediaQueryList.removeListener reached its body with no callback — §3.6.2 step 1's "
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
static const IdlDictMember EV_INIT[] = {   /* MediaQueryListEventInit, in IDL declaration order */
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "matches", IDL_BOOLEAN }, { "media", IDL_DOMSTRING },
};

static JSValue js_ev_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue ev, media;
    const char *type;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor MediaQueryListEvent requires 'new'");
    DCHECK(argc >= 1, "MediaQueryListEvent's constructor reached its body with no type — §3.6.2 step 1's "
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
   the same reason §8.9's map of animation frame callbacks is one. */
static JSValue mql_collection(JSContext *ctx)
{
    JSValue arr;

    DCHECK(g_ready, "a document's MediaQueryList collection was reached before §4.2 was declared");
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
    /* §4.2: "fire an event named change at target, using MediaQueryListEvent, with its matches attribute
       initialized to target's matches state and its media attribute initialized to target's media". It does
       not bubble and is not cancelable — nothing in the algorithm reads a canceled flag. */
    return ev_new(ctx, event_new(ctx, "change", /*bubbles*/ false, /*cancelable*/ false),
                  JS_NewString(ctx, d->media), JS_NewBool(ctx, now));
}

/* ---- `Window.matchMedia` ------------------------------------------------------------------------------------- */

/* CSSOM VIEW §7: `[NewObject] MediaQueryList matchMedia(CSSOMString query)`.
   1. Let parsed be the result of parsing query. 2. Return a new MediaQueryList object with its document set to
   this's associated Document and its media set to parsed. No page code runs: the declaration converted the
   argument, and everything after it is this component's own data. */
static JSValue js_match_media(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue obj, arr;
    MqlData *d;
    const char *q;

    (void)this_val; (void)magic;
    DCHECK(argc >= 1, "matchMedia reached its body with no query — §3.6.2 step 1's TypeError for a call short "
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
    /* THE REPORTED STATE STARTS AT THE CURRENT ONE. §4.2 fires when the state has changed "since the last time
       these steps were run", and a list created between two frames has not changed since it was created — a
       zero-initialised latch would make every `matchMedia("(min-width: 1px)")` fire a spurious `change` on the
       very next rendering opportunity. */
    d->last_matches = mql_computed(ctx, d);
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

    DCHECK(!g_ready, "media_query_list_init ran twice — §4.2's interfaces are declared once per agent");
    {
        JSClassDef mql = { "MediaQueryList", .finalizer = mql_finalizer };
        JSClassDef ev = { "MediaQueryListEvent" };
        JS_NewClassID(rt, &g_mql_class);
        JS_NewClass(rt, g_mql_class, &mql);
        JS_NewClassID(rt, &g_ev_class);
        JS_NewClass(rt, g_ev_class, &ev);
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
    g_ready = 1;
    realm_declare_intrinsic(media_query_list_install_proto);
}

void media_query_list_install_proto(JSContext *ctx)
{
    JSValue proto, prev, base, arr;

    DCHECK(g_ready, "a realm asked for MediaQueryList.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_mql_class);
    DCHECK(JS_IsNull(prev), "media_query_list_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "MediaQueryList.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "MediaQueryList");
    event_target_chain(ctx, proto);                      /* §4.2: `MediaQueryList : EventTarget` */
    event_target_install_handlers(ctx, proto, EH_MEDIA_QUERY_LIST);
    idl_install_accessor(ctx, proto, "media", js_mql_media, 0, -1);
    idl_install_accessor(ctx, proto, "matches", js_mql_matches, 0, -1);
    idl_install_method(ctx, proto, "addListener", 1, g_id_add);
    idl_install_method(ctx, proto, "removeListener", 1, g_id_remove);
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
       wrote — the same reason §8.9's map is built here. */
    arr = JS_NewArray(ctx);
    CHECK(!JS_IsException(arr), "§4.2's collection of MediaQueryList objects could not be allocated");
    realm_value_set(ctx, g_slot, arr);
}

void media_query_list_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor, proto;

    DCHECK(g_ready, "CSSOM VIEW §4.2 was installed before it was declared");
    idl_install_method(ctx, (JSValue)global, "matchMedia", 1, g_id_match);

    /* §3.7.1: an interface with NO constructor still has an interface object, and calling it is a TypeError. */
    proto = JS_GetClassProto(ctx, g_mql_class);
    DCHECK(!JS_IsNull(proto), "MediaQueryList was installed in a realm with no prototype for it");
    ctor = idl_interface_object(ctx, "MediaQueryList", proto);
    CHECK(!JS_IsException(ctor), "the MediaQueryList interface object could not be allocated");
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "MediaQueryList", ctor);

    ctor = idl_step_constructor(ctx, "MediaQueryListEvent", 1, g_id_ev_ctor);
    CHECK(!JS_IsException(ctor), "the MediaQueryListEvent interface object could not be allocated");
    proto = JS_GetClassProto(ctx, g_ev_class);
    DCHECK(!JS_IsNull(proto), "MediaQueryListEvent was installed in a realm with no prototype for it");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "MediaQueryListEvent", ctor);
}

void media_query_list_free(JSContext *ctx)
{
    if (!g_ready) return;
    g_ready = 0;
    /* The prototypes and the collections are the REALMS' — each goes with its context. */
    JS_FreeValue(ctx, g_ev_key);
    g_ev_key = JS_UNDEFINED;
    g_slot = g_id_match = g_id_add = g_id_remove = g_id_ev_ctor = -1;
}
