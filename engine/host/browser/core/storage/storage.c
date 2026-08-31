/* THE Storage INTERFACE — HTML §12.2.1 "The Storage interface". The IDL and what its absence cost are in
 * storage.h.
 *
 * IT IS A LEGACY PLATFORM OBJECT AND THAT IS THE HALF REAL BUNDLES USE. The IDL declares a named property
 * getter, setter and deleter (`getter`/`setter`/`deleter` on the three members), so `s.foo = 'bar'`,
 * `s.foo`, `delete s.foo`, `'foo' in s` and `Object.keys(s)` are the map — Web IDL §3.9 "Legacy platform
 * objects". Building only the six named members would leave every one of those spellings hitting an ordinary
 * property of the wrapper: the write would land on the object, the read would find it, and the two would agree
 * with each other while agreeing with nothing the map holds. That is worse than absence, because it looks
 * like it works.
 *
 * §3.9.7's NAMED PROPERTY VISIBILITY ALGORITHM IS WRITTEN OUT HERE, and it has to be. quickjs consults a
 * class's exotic get_own_property only after the ordinary own-property scan on THIS object misses, which is
 * the algorithm's step 2 — but its steps 4-5 walk the PROTOTYPE CHAIN and refuse a name any prototype has an
 * own property for, and no engine hook does that for us. Storage carries no [LegacyOverrideBuiltIns], so
 * `length`, `key`, `getItem`, `setItem`, `removeItem`, `clear`, `constructor`, @@toStringTag and everything on
 * Object.prototype are INVISIBLE as named properties: `localStorage.setItem('length','5')` stores the entry
 * and `localStorage.length` still answers the count. Skipping the walk would make it answer "5".
 *
 * KEYS AND VALUES ARE STRINGS BECAUSE THE IDL SAYS SO — `DOMString key`, `DOMString value` — so
 * `setItem('k', 1)` reads back the string "1". That conversion is the DECLARATION's (core/idl_args.h), never
 * this file's: a body that coerced its own argument would run the page's `toString` from a C activation with
 * no flow base under it, which is the one thing §3.2's conversions must not do here.
 *
 * …AND UNKNOWN EXTERNAL INPUT CROSSES AS ITSELF, which is the other half of that. idl_args.c passes a concolic
 * through every string conversion untouched, so `localStorage.setItem('t', location.hash)` stores the CONCOLIC
 * and `localStorage.getItem('t')` hands the same one back — taint intact, still forking control flow, still
 * solvable at a sink. A ToString here would de-taint it and the whole stored-XSS path through Web Storage
 * would go quiet. A concolic KEY has no bytes to be a property name, so its SHAPE is the name — the same
 * answer core/html/dom_string_map.c gives for a concolic written into a `data-*` attribute, and it round-trips
 * for the same reason: the read computes the same shape the write did.
 *
 * THE MAP TIME-TRAVELS BECAUSE IT IS A JS OBJECT (core/storage/storage_shed.c). Nothing in this file captures
 * anything: every mutation below is an ordinary property write or delete on a baseline null-prototype object,
 * which is exactly what the COW delta already records.
 *
 * THE PROTOTYPE AND THE INTERFACE OBJECT ARE PER-REALM INTRINSICS (core/realm.h). js_call_c_function takes its
 * `ctx` from the function object, so a prototype built once would answer every document's `length` out of the
 * realm that happened to build it first. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/events/event_target.h"
#include "core/events/storage_event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/storage/storage.h"
#include "core/storage/storage_shed.h"
#include "solver/concolic.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's slots hang off */
static JSClassID g_class;
static int       g_ready;
static int       g_id_key = -1, g_id_get_item = -1, g_id_set_item = -1, g_id_remove_item = -1, g_id_clear = -1;

static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
static const IdlArgType IDL_1ULONG[1] = { IDL_UNSIGNED_LONG };

/* ---- the object's own state ---------------------------------------------------------------------------- */

/* §12.2.1: "A Storage object has an associated map (a storage proxy map) and type ('local' or 'session')."
   Both live in one slot record under the private Symbol, so they are property writes the COW delta captures
   and a page cannot forge the brand. OWNED, or JS_UNDEFINED for an object that is not a Storage. */
static JSValue st_slots(JSContext *ctx, JSValueConst obj)
{
    JSAtom k;
    JSValue slots;

    if (JS_GetClassID(obj) != g_class) return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "storage: the slot key could not be interned");
    if (JS_GetOwnSlot(ctx, &slots, obj, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    DCHECK(JS_IsObject(slots),
           "a Storage object has no slot record — storage_new places it before the object is reachable, so an "
           "object of this class without one was minted by something other than that constructor");
    return slots;
}

/* WEB IDL'S BRAND CHECK for a member reached through the prototype — §3.7.6 "Attributes" and §3.7.7
   "Operations", each of whose generated steps end in "throw a TypeError" for a `this` that does not implement
   the interface. `Storage.prototype.getItem.call({})` is that TypeError, and a page tells it apart from a
   `null` result. Returns the slot record, OWNED, or JS_UNDEFINED with the throw live. */
static JSValue st_brand(JSContext *ctx, JSValueConst this_val)
{
    JSValue slots;

    DCHECK(g_ready, "a Storage member ran before storage_init declared the interface");
    if (JS_GetClassID(this_val) != g_class) {
        JS_ThrowTypeError(ctx, "a Storage member was reached on something that is not a Storage");
        return JS_UNDEFINED;
    }
    slots = st_slots(ctx, this_val);
    return slots;
}

/* §4.7: every operation on a storage proxy map is performed on its BACKING MAP. OWNED. */
static JSValue st_map(JSContext *ctx, JSValueConst slots)
{
    JSValue pm = JS_GetPropertyStr(ctx, slots, "proxyMap");
    JSValue m;

    DCHECK(JS_IsObject(pm), "a Storage object's map is not a storage proxy map");
    m = storage_shed_backing_map(ctx, pm);
    JS_FreeValue(ctx, pm);
    return m;
}

bool storage_is(JSValueConst v) { return g_ready && JS_GetClassID(v) == g_class; }

JSClassID storage_class_id(void) { return g_ready ? g_class : 0; }

/* §12.2.1 step 1's "storage's relevant global object's associated Document", as the `document` object of the
   realm this Storage was minted in. It is a SLOT rather than a lookup because a Storage outlives the member
   call that made it and is reached from a broadcast running in ANOTHER realm: js_call_c_function takes `ctx`
   from the function object, so asking the running realm would name the broadcaster's document for every remote
   Storage in the set — which is both the wrong URL (step 2) and the wrong realm to fire in (step 4). OWNED. */
static JSValue st_document(JSContext *ctx, JSValueConst slots)
{
    JSValue doc = JS_GetPropertyStr(ctx, slots, "document");

    DCHECK(JS_IsObject(doc), "a Storage object carries no Document — storage_new places it before the object is "
                             "reachable, and §12.2.1's broadcast reads both its URL and its realm off it");
    return doc;
}

/* ---- keys, values and their sizes ------------------------------------------------------------------------ */

/* THE PROPERTY NAME A `DOMString key` ARGUMENT NAMES. The argument arrives already converted by the member's
   declaration, so this interns a string and runs no page code — except for unknown external input, which
   crosses conversions as itself and has no bytes at all; its SHAPE is the name (see this file's header).
   JS_ATOM_NULL with the throw live. */
static JSAtom st_key_atom(JSContext *ctx, JSValueConst key)
{
    if (concolic_is(key)) {
        const char *shape = concolic_shape_c(key);

        DCHECK(shape != NULL, "a concolic Storage key has no shape — concolic_is said it is one, and every "
                              "concolic carries a display shape");
        return JS_NewAtom(ctx, shape);
    }
    DCHECK(JS_IsString(key), "a Storage key reached the map without having been converted to a DOMString — the "
                             "member's IDL declaration is what converts it, and a body that did so itself "
                             "would run the page's toString from C");
    return JS_ValueToAtom(ctx, key);
}

/* HOW MANY BYTES ONE STORED STRING COSTS is the MODEL's measure and not this interface's — Storage §4.1's
   per-bottle quota and §6's storage usage are the same number counted for two purposes, so
   core/storage/storage_shed.h owns it and step 4 below reads it. It used to be a static here, and the second
   reader (§6's usage, which sums it over a shelf's bottles) would have been a second copy of "what a stored
   string costs" to disagree with the first the day either changed. */

/* §12.2.1's "get the keys on this's map", as the own enumerable string keys of the backing map. Storage §4.7's
   own note licenses whatever order this is: "To reorder a Storage object storage, reorder storage's map's
   entries in an implementation-defined manner … iteration order is not defined and can change upon most
   mutations." Returns <0 with the throw live. */
static int st_keys(JSContext *ctx, JSValueConst map, JSPropertyEnum **ptab, uint32_t *plen)
{
    return JS_GetOwnPropertyNames(ctx, ptab, plen, map, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);
}

static void st_keys_free(JSContext *ctx, JSPropertyEnum *tab, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++) JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
}

/* §12.2.1's REORDER A Storage OBJECT: "reorder storage's map's entries in an implementation-defined manner."
   The manner this engine implements is to leave them where they are — which is a manner, and the spec's own
   note ("Unfortunate as it is, iteration order is not defined and can change upon most mutations") is what
   makes that a complete implementation of the step rather than a skipped one. It is a named function because
   the step is named: setItem and removeItem each reach it, and the day an ordering becomes observable this is
   where it goes. */
static void st_reorder(JSContext *ctx, JSValueConst self) { (void)ctx; (void)self; }

/* ---- §12.2.1's BROADCAST ---------------------------------------------------------------------------------- */

/* "To broadcast a Storage object storage, given a key, oldValue, and newValue" — steps 1-2 name this
 * document's URL, step 3 collects the OTHER Storage objects of the same type whose environment is same origin,
 * and step 4 queues a global task on the DOM manipulation task source at each of their globals to fire a
 * `storage` event using StorageEvent.
 *
 * STEP 3 IS ANSWERED BY THE BOTTLE'S §4.6 PROXY MAP REFERENCE SET, which is the same set by construction, and
 * each of its three conditions is met by a different part of that construction rather than tested here. TYPE:
 * a bottle belongs to exactly one (type, identifier), so every proxy map in this set stands for a Storage of
 * this Storage's type — asserted below, because a set holding two types would silently deliver a localStorage
 * write into a sessionStorage listener. SAME ORIGIN: an instance is an ORIGIN-KEYED AGENT CLUSTER
 * (CLAUDE.md §Security), so every environment reaching this shed keys the one storage key
 * obtain_storage_key asserts. THE TRAVERSABLE, for "session": storage_shed.c builds the agent's ROOT
 * traversable's session shed and crashes at the obtain for any other, so a session bottle's set spans one
 * traversable by the same construction. "EXCLUDING STORAGE" is this object's own proxy map, which is the one
 * thing that IS tested — by identity, in the shed, where the set lives.
 *
 * STEP 4's TASK SOURCE IS event_target_fire — core/events/event_target.h's queued reach, which is for "a caller
 * whose spec says QUEUE a task … to fire an event named X" and for no other, and which is what
 * core/frame/session_history.c uses for §7.4.6.2's hashchange on this same DOM manipulation task source. It is
 * enqueued in the TARGET's realm, which is what §8.1.7.2 "Queuing tasks"' queue-a-global-task says
 * ("let document be global's associated Document … queue a task given source, event loop, document, and
 * steps"): the task belongs to the receiving document, so HTML §7.5.10's step 7 removal of a destroyed
 * document's tasks reaches it, which it would not if the broadcaster had queued it in its own.
 *
 * A TARGET THAT IS NOT FULLY ACTIVE IS NOT AN ERROR HERE, and §12.2.1 says so in its own words: "The Document
 * object associated with the resulting task is not necessarily fully active, but events fired on such objects
 * are ignored by the event loop until the Document becomes fully active again." So the queue is unconditional
 * and the waiting is the EVENT LOOP's rather than this algorithm's: §8.1.7.3 "Processing model" step 2 chooses
 * among the task queues "with at least one RUNNABLE task", and §8.1.7.1 "Definitions" makes a task runnable "if
 * its document is either null or fully active". */
static void st_broadcast(JSContext *ctx, JSValueConst self, JSValueConst slots, JSValueConst key,
                         JSValueConst old_value, JSValueConst new_value)
{
    JSValue pm = JS_GetPropertyStr(ctx, slots, "proxyMap");
    JSValue doc = st_document(ctx, slots);
    JSValue others, type;
    const char *url;
    uint32_t n = 0, i;

    (void)self;   /* the exclusion below is an assert's, and an assert is compiled out of a release build */
    DCHECK(JS_IsObject(pm), "a Storage object's map is not a storage proxy map");
    /* STEPS 1-2, off THIS Storage's own Document rather than off the running realm's — see st_document. */
    url = document_url_of(lxb_dom_interface_document(node_of(doc)));
    DCHECK(url != NULL, "the Document a Storage was minted in has no address — every Document is installed at "
                        "one, and §12.2.1 step 2 is the serialization of it");
    JS_FreeValue(ctx, doc);
    type = JS_GetPropertyStr(ctx, slots, "type");

    others = storage_shed_other_storages(ctx, pm);          /* step 3 */
    JS_FreeValue(ctx, pm);
    CHECK(JS_IsArray(others), "storage: §12.2.1 broadcast step 3's remoteStorages could not be collected");
    {
        JSValue len = JS_GetPropertyStr(ctx, others, "length");
        CHECK(JS_ToUint32(ctx, &n, len) == 0, "storage: remoteStorages has no length");
        JS_FreeValue(ctx, len);
    }
    for (i = 0; i < n; i++) {                               /* step 4, for each remoteStorage */
        JSValue remote = JS_GetPropertyUint32(ctx, others, i);
        JSValue rslots, rtype, rdoc, win, ev;
        JSContext *rctx;

        DCHECK(storage_is(remote), "§12.2.1 broadcast step 3 collected something that is not a Storage");
        DCHECK(JS_VALUE_GET_PTR(remote) != JS_VALUE_GET_PTR(self),
               "§12.2.1 broadcast step 3 collected the broadcasting Storage itself — the step says \"all "
               "Storage objects EXCLUDING storage\", and the exclusion is the proxy map's identity in the "
               "bottle's reference set");
        rslots = st_slots(ctx, remote);
        rtype = JS_GetPropertyStr(ctx, rslots, "type");
        DCHECK(JS_IsStrictEqual(ctx, type, rtype),
               "§12.2.1 broadcast step 3 collected a Storage of the OTHER type — a §4.6 bottle belongs to one "
               "(type, identifier), so a reference set holding both means a bucket handed out a bottle of the "
               "type it was not built for and a localStorage write would fire at a sessionStorage listener");
        JS_FreeValue(ctx, rtype);
        rdoc = st_document(ctx, rslots);
        JS_FreeValue(ctx, rslots);
        /* §12.2.1 step 4's "remoteStorage's relevant global object" — the global of the realm that Document
           belongs to. The EVENT is built there too, because it is handed to that document's listeners. */
        rctx = document_realm_of(node_of(rdoc));
        JS_FreeValue(ctx, rdoc);
        DCHECK(rctx != NULL, "a remote Storage's Document belongs to no realm — every Storage is minted by a "
                             "realm's own localStorage getter, so its Document has a record");
        win = JS_GetGlobalObject(rctx);
        ev = storage_event_new_to_fire(rctx, key, old_value, new_value, url, remote);
        CHECK(!JS_IsException(ev), "storage: §12.2.1 step 4's StorageEvent could not be allocated — a dropped "
                                   "broadcast is a document that never learns another wrote its storage");
        event_target_fire(rctx, win, ev, JS_UNDEFINED);     /* CONSUMES ev */
        JS_FreeValue(rctx, win);
        JS_FreeValue(ctx, remote);
    }
    JS_FreeValue(ctx, others);
    JS_FreeValue(ctx, type);
}

/* ---- the members ------------------------------------------------------------------------------------------ */

/* "The length getter steps are to return this's map's size." */
static JSValue js_st_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = st_brand(ctx, this_val), map;
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0;

    (void)magic;
    if (JS_IsUndefined(slots)) return JS_EXCEPTION;
    map = st_map(ctx, slots);
    JS_FreeValue(ctx, slots);
    if (st_keys(ctx, map, &tab, &n) < 0) { JS_FreeValue(ctx, map); return JS_EXCEPTION; }
    st_keys_free(ctx, tab, n);
    JS_FreeValue(ctx, map);
    return JS_NewUint32(ctx, n);
}

/* "The key(index) method steps are: 1. If index is greater than or equal to this's map's size, then return
    null. 2. Let keys be the result of running get the keys on this's map. 3. Return keys[index]." */
static JSValue js_st_key(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue slots = st_brand(ctx, this_val), map, r;
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, index = 0;

    (void)magic;
    if (JS_IsUndefined(slots)) return JS_EXCEPTION;
    DCHECK(argc >= 1, "Storage.key() ran with no index — `key(unsigned long index)` has one required argument "
                      "and the declaration is what enforces arity");
    if (JS_ToUint32(ctx, &index, argv[0]) < 0) { JS_FreeValue(ctx, slots); return JS_EXCEPTION; }
    map = st_map(ctx, slots);
    JS_FreeValue(ctx, slots);
    if (st_keys(ctx, map, &tab, &n) < 0) { JS_FreeValue(ctx, map); return JS_EXCEPTION; }
    JS_FreeValue(ctx, map);
    r = (index >= n) ? JS_NULL : JS_AtomToValue(ctx, tab[index].atom);   /* steps 1 and 3 */
    st_keys_free(ctx, tab, n);
    return r;
}

/* "The getItem(key) method steps are: 1. If this's map[key] does not exist, then return null. 2. Return
    this's map[key]." */
static JSValue js_st_get_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue slots = st_brand(ctx, this_val), map, v;
    JSAtom a;

    (void)magic;
    if (JS_IsUndefined(slots)) return JS_EXCEPTION;
    DCHECK(argc >= 1, "Storage.getItem() ran with no key — its one argument is required");
    a = st_key_atom(ctx, argv[0]);
    map = st_map(ctx, slots);
    JS_FreeValue(ctx, slots);
    if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, map); return JS_EXCEPTION; }
    if (JS_GetOwnSlot(ctx, &v, map, a) <= 0) v = JS_NULL;               /* step 1 */
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, map);
    return v;                                                            /* step 2 */
}

/* "The setItem(key, value) method steps are: 1. Let oldValue be null. 2. Let reorder be true. 3. If this's
    map[key] exists: 1. Set oldValue to this's map[key]. 2. If oldValue is value, then return. 3. Set reorder
    to false. 4. If value cannot be stored, then throw a QuotaExceededError. 5. Set this's map[key] to value.
    6. If reorder is true, then reorder this. 7. Broadcast this with key, oldValue, and value." */
static JSValue js_st_set_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue slots = st_brand(ctx, this_val), map, pm, old = JS_NULL;
    JSAtom a;
    bool exists, reorder = true;
    double quota, total = 0, after;

    (void)magic;
    if (JS_IsUndefined(slots)) return JS_EXCEPTION;
    DCHECK(argc >= 2, "Storage.setItem() ran without both of its required arguments");
    /* WHAT ENTERS THE MAP IS A STRING OR UNKNOWN EXTERNAL INPUT, asserted at the ORIGIN of that invariant —
       here, where the two values arrive — rather than at the several places that later MEASURE them. §12.2.1
       declares `DOMString key, DOMString value`, so the member's own declaration converts both, and unknown
       external input crosses that conversion as itself; a value of any other kind means the declaration this
       member was installed with does not list the two DOMStrings its IDL does. */
    DCHECK(JS_IsString(argv[0]) || concolic_is(argv[0]),
           "Storage.setItem()'s `key` reached its body as neither a string nor unknown external input — its IDL "
           "declares `DOMString key`, so core/idl_args.h's declaration for this member is what converts it");
    DCHECK(JS_IsString(argv[1]) || concolic_is(argv[1]),
           "Storage.setItem()'s `value` reached its body as neither a string nor unknown external input — its "
           "IDL declares `DOMString value`, so core/idl_args.h's declaration for this member is what converts "
           "it");
    a = st_key_atom(ctx, argv[0]);
    map = st_map(ctx, slots);
    if (a == JS_ATOM_NULL) goto fail;

    exists = JS_GetOwnSlot(ctx, &old, map, a) > 0;                       /* step 3 */
    if (!exists) old = JS_NULL;
    if (exists && JS_IsStrictEqual(ctx, old, argv[1])) {                 /* step 3.2 */
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, old);
        JS_FreeValue(ctx, map);
        JS_FreeValue(ctx, slots);
        return JS_UNDEFINED;
    }
    if (exists) reorder = false;                                         /* step 3.3 */

    /* STEP 4, AS A COMPUTED FACT. Storage §4.1 gives the "localStorage" and "sessionStorage" endpoints a quota
       of 5 × 2^20 bytes, and §4.6 calls a null one "the lack of a limit" — so "value cannot be stored" is
       whether this write takes the bottle past that number, not a shrug. */
    quota = storage_shed_quota(ctx, (pm = JS_GetPropertyStr(ctx, slots, "proxyMap")));
    JS_FreeValue(ctx, pm);
    if (quota >= 0) {
        /* WHAT THE BOTTLE ALREADY HOLDS is the model's own sum (Storage §6 reads the identical one over every
           bottle of a shelf), so the walk is asked for rather than repeated here. */
        total = storage_shed_map_usage(ctx, map);
        after = total + (double)storage_shed_string_bytes(ctx, argv[1]);
        if (exists) after -= (double)storage_shed_string_bytes(ctx, old);
        else        after += (double)storage_shed_string_bytes(ctx, argv[0]);
        if (after > quota) {
            JS_ThrowDOMException(ctx, "QuotaExceededError",
                                 "storing that value would take this origin's storage past its quota");
            goto fail_atom;
        }
    }

    if (JS_SetProperty(ctx, map, a, JS_DupValue(ctx, argv[1])) < 0) goto fail_atom;   /* step 5 */
    JS_FreeAtom(ctx, a);
    if (reorder) st_reorder(ctx, this_val);                              /* step 6 */
    /* STEP 7's THREE ARGUMENTS ARE THIS MEMBER'S OWN, not the map's: `key` is the argument the page passed —
       which for unknown external input is the concolic itself, while the MAP is keyed by its shape — and
       `newValue` is the value now stored. Reading them back off the map instead would hand a listener the
       shape where the writer wrote a taint. */
    st_broadcast(ctx, this_val, slots, argv[0], old, argv[1]);           /* step 7 */
    JS_FreeValue(ctx, old);
    JS_FreeValue(ctx, map);
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;

fail_atom:
    JS_FreeAtom(ctx, a);
fail:
    JS_FreeValue(ctx, old);
    JS_FreeValue(ctx, map);
    JS_FreeValue(ctx, slots);
    return JS_EXCEPTION;
}

/* "The removeItem(key) method steps are: 1. If this's map[key] does not exist, then return. 2. Set oldValue to
    this's map[key]. 3. Remove this's map[key]. 4. Reorder this. 5. Broadcast this with key, oldValue, and
    null." */
static JSValue js_st_remove_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue slots = st_brand(ctx, this_val), map, old;
    JSAtom a;
    int r;

    (void)magic;
    if (JS_IsUndefined(slots)) return JS_EXCEPTION;
    DCHECK(argc >= 1, "Storage.removeItem() ran with no key — its one argument is required");
    a = st_key_atom(ctx, argv[0]);
    map = st_map(ctx, slots);
    if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, map); JS_FreeValue(ctx, slots); return JS_EXCEPTION; }
    if (JS_GetOwnSlot(ctx, &old, map, a) <= 0) {                         /* step 1 */
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, map);
        JS_FreeValue(ctx, slots);
        return JS_UNDEFINED;
    }
    r = JS_DeleteProperty(ctx, map, a, 0);                               /* step 3 — the COW delta's capture */
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, map);
    if (r < 0) { JS_FreeValue(ctx, old); JS_FreeValue(ctx, slots); return JS_EXCEPTION; }
    DCHECK(r == 1, "a Storage entry refused to be removed — every entry is created by this component as a "
                   "configurable data property of a null-prototype map, so a refusal means something else "
                   "defined it");
    st_reorder(ctx, this_val);                                           /* step 4 */
    /* STEP 5's `oldValue` is step 2's — the value this call removed — so it is held across the delete rather
       than freed at it. `newValue` is null, which §12.2.4 types the attribute for. */
    st_broadcast(ctx, this_val, slots, argv[0], old, JS_NULL);           /* step 5 */
    JS_FreeValue(ctx, old);
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;
}

/* "The clear() method steps are: 1. If this's map is empty, then return. 2. Clear this's map. 3. Broadcast
    this with null, null, and null." */
static JSValue js_st_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue slots = st_brand(ctx, this_val), map;
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;

    (void)argc; (void)argv; (void)magic;
    if (JS_IsUndefined(slots)) return JS_EXCEPTION;
    map = st_map(ctx, slots);
    if (st_keys(ctx, map, &tab, &n) < 0) { JS_FreeValue(ctx, map); JS_FreeValue(ctx, slots); return JS_EXCEPTION; }
    if (n == 0) {                                                        /* step 1 */
        st_keys_free(ctx, tab, n);
        JS_FreeValue(ctx, map);
        JS_FreeValue(ctx, slots);
        return JS_UNDEFINED;
    }
    for (i = 0; i < n; i++) {
        int r = JS_DeleteProperty(ctx, map, tab[i].atom, 0);             /* step 2 */
        DCHECK(r == 1, "a Storage entry refused to be removed by clear()");
        if (r < 0) { st_keys_free(ctx, tab, n); JS_FreeValue(ctx, map); JS_FreeValue(ctx, slots); return JS_EXCEPTION; }
    }
    st_keys_free(ctx, tab, n);
    JS_FreeValue(ctx, map);
    /* "Broadcast this with null, null, and null" — all three are the IDL null, which is what tells a listener
       that the whole area was cleared rather than one entry changed. */
    st_broadcast(ctx, this_val, slots, JS_NULL, JS_NULL, JS_NULL);       /* step 3 */
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;
}

/* ---- Web IDL §3.9's legacy platform object -------------------------------------------------------------- */

/* Is this atom a STRING key rather than a Symbol? §3.9's named-property arms all begin "P is a String", and a
   Symbol-keyed define or delete on a Storage is the ordinary operation. */
static bool st_atom_is_string(JSContext *ctx, JSAtom a)
{
    JSValue v = JS_AtomToValue(ctx, a);
    bool s = JS_IsString(v);

    JS_FreeValue(ctx, v);
    return s;
}

/* §3.9.7's NAMED PROPERTY VISIBILITY ALGORITHM, with property name P and object O. Storage carries no
 * [LegacyOverrideBuiltIns], so step 3 never fires and the prototype walk of steps 4-5 decides.
 *
 * Step 2 ("if O has an own property named P, then return false") is the engine's: quickjs reaches an exotic
 * get_own_property only after the ordinary own-property scan on this object has missed. Everything else is
 * here. `*pvalue`, when non-NULL and the answer is true, receives the map's value, OWNED. */
static bool st_named_visible(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValue *pvalue)
{
    JSValue slots, map, v = JS_UNDEFINED, proto;
    bool found;

    if (!st_atom_is_string(ctx, prop)) return false;
    slots = st_slots(ctx, obj);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return false; }
    map = st_map(ctx, slots);
    JS_FreeValue(ctx, slots);
    found = JS_GetOwnSlot(ctx, &v, map, prop) > 0;                       /* step 1: a supported property name */
    JS_FreeValue(ctx, map);
    if (!found) return false;

    /* Steps 4-5: "Let prototype be O.[[GetPrototypeOf]](). While prototype is not null: if prototype is not a
       named properties object, and prototype has an own property named P, then return false." Storage.prototype
       is not a named properties object and neither is Object.prototype, so every level counts. The query runs
       NO page code — an interface prototype's members are data properties and accessors this engine installed
       — which is what lets it happen inside an exotic hook with no flow base under it. */
    proto = JS_GetPrototype(ctx, obj);
    while (JS_IsObject(proto)) {
        JSValue next;
        int has = JS_GetOwnPropertyNoUserCode(ctx, NULL, proto, prop);

        if (has != 0) {
            JS_FreeValue(ctx, proto);
            JS_FreeValue(ctx, v);
            return false;
        }
        next = JS_GetPrototype(ctx, proto);
        JS_FreeValue(ctx, proto);
        proto = next;
    }
    JS_FreeValue(ctx, proto);
    if (pvalue) *pvalue = v;
    else JS_FreeValue(ctx, v);
    return true;                                                          /* step 6 */
}

/* §3.9.1 [[GetOwnProperty]] via LegacyPlatformObjectGetOwnProperty: a visible named property is a data
   property whose value is the named getter's result, [[Writable]] true because the interface has a named
   property setter, and [[Enumerable]]/[[Configurable]] true (Storage declares no
   [LegacyUnenumerableNamedProperties]). */
static int st_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    JSValue v = JS_UNDEFINED;

    if (!st_named_visible(ctx, obj, prop, desc ? &v : NULL)) return 0;
    if (!desc) return 1;
    desc->flags = JS_PROP_WRITABLE | JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
    desc->value = v;
    desc->getter = desc->setter = JS_UNDEFINED;
    return 1;
}

static int st_has(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    return st_named_visible(ctx, obj, prop, NULL) ? 1 : 0;
}

/* §3.9.6 [[OwnPropertyKeys]] step 3: "for each P of O's supported property names that is visible according to
   the named property visibility algorithm, append P to keys." Storage supports no indexed properties, and this
   object carries no own properties of its own besides the private-Symbol slot record — which is a Symbol and
   therefore belongs to step 5 rather than step 3. */
static int st_own_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
    JSValue slots = st_slots(ctx, obj), map;
    JSPropertyEnum *all = NULL, *out = NULL;
    uint32_t n = 0, i, k = 0;

    *ptab = NULL; *plen = 0;
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return 0; }
    map = st_map(ctx, slots);
    JS_FreeValue(ctx, slots);
    if (st_keys(ctx, map, &all, &n) < 0) { JS_FreeValue(ctx, map); return -1; }
    JS_FreeValue(ctx, map);
    if (n == 0) { st_keys_free(ctx, all, n); return 0; }
    out = js_malloc(ctx, sizeof(*out) * n);
    if (!out) { st_keys_free(ctx, all, n); return -1; }
    for (i = 0; i < n; i++)
        if (st_named_visible(ctx, obj, all[i].atom, NULL)) {
            out[k].is_enumerable = 1;
            out[k].atom = JS_DupAtom(ctx, all[i].atom);
            k++;
        }
    st_keys_free(ctx, all, n);
    *ptab = out;
    *plen = k;
    return 0;
}

/* §3.9.3 [[DefineOwnProperty]] step 2: Storage supports named properties, does not carry [Global], and has no
 * unforgeable property names — so for a String P, and because it has no own property named P (step 2.2's
 * condition, which is why `localStorage.getItem = 1` stores an entry in a browser rather than shadowing the
 * method), the NAMED PROPERTY SETTER is invoked with P and Desc.[[Value]]. §3.9.7's invoke-a-named-property-
 * setter converts that value to the setter's second argument type — DOMString — and performs setItem's steps.
 *
 * An ACCESSOR is Web IDL §3.9.3 [[DefineOwnProperty]] step 2.2.2.1's `false` — "If the result of calling
 * IsDataDescriptor(Desc) is false, then return false." — and the number is restated here rather than carried
 * over from the paragraph above, because the nearest citation to these words was §3.9.7 and they are not
 * §3.9.7's. A Symbol P falls through to OrdinaryDefineOwnProperty, which is the ordinary path this hook
 * re-enters with the exotic step suppressed.
 *
 * THAT `false` WAS RETURNED AS A BARE 0, WHICH IS A DIFFERENT ANSWER FROM THE SPEC'S. An internal method's
 * "return false" is not an observable — the CALLER decides what it looks like, and the two callers that can
 * reach this arm disagree. An accessor descriptor comes only from the defineProperty family (an assignment
 * cannot carry one), so this arm is reached by Object.defineProperty, which is ECMAScript §7.3.8
 * DefinePropertyOrThrow step 2 "If success is false, throw a TypeError exception", and by
 * Reflect.defineProperty, which is §28.1.3 step 4 "Return ? target.[[DefineOwnProperty]](propertyKey,
 * propertyDesc)" and reports the boolean. A bare 0 gave BOTH of them the second answer, so
 * `Object.defineProperty(localStorage, "k", { get() {} })` returned the object instead of throwing — the
 * refusal happened and nothing said so. quickjs carries which form the caller wants in `flags` and resolves
 * JS_PROP_THROW_STRICT against the running frame, both internal to it, so JS_RefuseOrThrowTypeError is the
 * one spelling of "return false" a hook compiled outside that file can make.
 *
 * A CONCOLIC VALUE CROSSES AS ITSELF, exactly as it does through a declared DOMString argument: `s.x =
 * location.hash` must keep its taint or the whole stored-source path goes quiet. */
static int st_define_own(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val,
                         JSValueConst getter, JSValueConst setter, int flags)
{
    JSValue slots, map, old = JS_NULL, str;
    bool exists;
    int r;

    if (!st_atom_is_string(ctx, prop))
        return JS_DefineProperty(ctx, obj, prop, val, getter, setter, flags | JS_PROP_NO_EXOTIC);
    if (!JS_IsUndefined(getter) || !JS_IsUndefined(setter))
        return JS_RefuseOrThrowTypeError(ctx, flags,                     /* step 2.2.2.1 */
                                         "a named property of a Storage object is set through its named "
                                         "property setter, so it cannot be defined from an accessor "
                                         "descriptor");

    slots = st_slots(ctx, obj);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return 0; }
    map = st_map(ctx, slots);

    /* §3.9.7's "let value be the result of converting V to an IDL value of type T", T = DOMString. Unknown
       external input crosses as itself (core/idl_args.c's rule for every string conversion); anything else is
       ToString, which is safe here because a named-property define carries a value the page has ALREADY
       produced — the operand of an assignment, or a descriptor's [[Value]] — so no getter of the page's runs. */
    if (concolic_is(val)) {
        str = JS_DupValue(ctx, val);
    } else {
        str = JS_ToString(ctx, val);
        if (JS_IsException(str)) { JS_FreeValue(ctx, map); JS_FreeValue(ctx, slots); return -1; }
    }

    exists = JS_GetOwnSlot(ctx, &old, map, prop) > 0;                    /* setItem step 3 */
    if (!exists) old = JS_NULL;
    if (exists && JS_IsStrictEqual(ctx, old, str)) {                     /* setItem step 3.2 */
        JS_FreeValue(ctx, old); JS_FreeValue(ctx, str);
        JS_FreeValue(ctx, map); JS_FreeValue(ctx, slots);
        return 1;
    }
    r = JS_SetProperty(ctx, map, prop, JS_DupValue(ctx, str));           /* setItem step 5 */
    JS_FreeValue(ctx, map);
    if (r < 0) { JS_FreeValue(ctx, old); JS_FreeValue(ctx, str); JS_FreeValue(ctx, slots); return -1; }
    if (!exists) st_reorder(ctx, obj);                                   /* setItem step 6 */
    /* setItem step 7's THREE ARGUMENTS, through the named-property setter's door. `key` is §3.9.7's property
       name P as a value, `oldValue` is what the map held (null when it held nothing) and `newValue` is the
       converted value this define stored — the same three the method form passes, because §3.9.3 step 2's
       invoke-a-named-property-setter performs setItem's steps and not a shorter algorithm. */
    {
        JSValue kv = JS_AtomToValue(ctx, prop);

        CHECK(!JS_IsException(kv), "storage: a named property's key could not be read back for §12.2.1's "
                                   "broadcast");
        st_broadcast(ctx, obj, slots, kv, old, str);                     /* setItem step 7 */
        JS_FreeValue(ctx, kv);
    }
    JS_FreeValue(ctx, old);
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, slots);
    return 1;
}

/* §3.9.4 [[Delete]] step 2: a visible named property with a named property deleter runs removeItem's steps
   with P as the name. A name the map does not hold — or one the visibility algorithm hides — falls to step 3,
   which for this object (no own properties but the Symbol slot record) is step 4's `true`. */
static int st_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    JSValue slots, map, old;
    int r;

    /* §3.9.4 step 4's `true`. An own property of this object — the private-Symbol slot record is the only one
       — never reaches here at all: quickjs's delete_property walks the object's own shape first and consults
       an exotic handler only after that misses, which is step 3 answered by the engine. */
    if (!st_named_visible(ctx, obj, prop, NULL)) return 1;
    slots = st_slots(ctx, obj);
    map = st_map(ctx, slots);
    if (JS_GetOwnSlot(ctx, &old, map, prop) <= 0) {                      /* removeItem step 1 */
        JS_FreeValue(ctx, map); JS_FreeValue(ctx, slots);
        return 1;
    }
    r = JS_DeleteProperty(ctx, map, prop, 0);                            /* removeItem step 3 */
    JS_FreeValue(ctx, map);
    if (r < 0) { JS_FreeValue(ctx, old); JS_FreeValue(ctx, slots); return -1; }
    st_reorder(ctx, obj);                                                /* removeItem step 4 */
    {
        JSValue kv = JS_AtomToValue(ctx, prop);

        CHECK(!JS_IsException(kv), "storage: a named property's key could not be read back for §12.2.1's "
                                   "broadcast");
        st_broadcast(ctx, obj, slots, kv, old, JS_NULL);                 /* removeItem step 5 */
        JS_FreeValue(ctx, kv);
    }
    JS_FreeValue(ctx, old);
    JS_FreeValue(ctx, slots);
    return 1;
}

static const JSClassExoticMethods STORAGE_EXOTIC = {
    .get_own_property = st_get_own,
    .get_own_property_names = st_own_names,
    .delete_property = st_delete,
    .define_own_property = st_define_own,
    .has_property = st_has,
    /* The lookup is a map read plus §3.9.7's prototype walk, and neither can reach the page: the map is a
       null-prototype object this component owns, and every prototype on the chain carries only members this
       engine installed. That is what lets the engine's own accessor walks run these hooks from C. */
    .get_own_property_no_user_code = true,
};

/* ---- construction and install ---------------------------------------------------------------------------- */

/* §12.2.2 step 4 and §12.2.3 step 4's "let storage be a new Storage object whose map is map".
 *
 * IT IS INFALLIBLE, and that is what makes the BINDING below impossible to miss. Storage §4.6 step 7 has
 * already appended `proxy_map` to its bottle's proxy map reference set by the time this runs — that is the
 * spec's own order — so between the obtain and the bind there is a proxy map in §12.2.1 broadcast step 3's set
 * that stands for no Storage object. An early return on an allocation failure would leave one there for the
 * life of the agent; an OOM here is a CHECK (CLAUDE.md §Offensive: allocation is always fatal), so the two
 * steps cannot come apart. */
JSValue storage_new(JSContext *ctx, JSValue proxy_map, StorageType type)
{
    JSValue obj, slots, proto;
    JSValueConst doc = document_object(ctx);
    JSAtom k;

    DCHECK(g_ready, "a Storage was minted before storage_init declared the interface");
    DCHECK(JS_IsObject(proxy_map), "a Storage was minted with no storage proxy map — §12.2.1's object IS its "
                                   "map, and Storage §4.6 is what obtains one");
    /* §12.2.2's and §12.2.3's holder is the DOCUMENT's, and §12.2.1 step 1 reads the Document back off the
       Storage — so a realm with none has nowhere to put this object and nothing for the broadcast to name. */
    DCHECK(JS_IsObject(doc), "a Storage was minted in a realm with no `document` — both getters hold their "
                             "Storage on this's associated Document, and §12.2.1 step 1 is that Document");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "a Storage was minted in a realm that never ran its per-realm install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a Storage object could not be allocated");

    slots = idl_slots_new(ctx);
    CHECK(!JS_IsException(slots), "a Storage object's slot record could not be allocated");
    JS_SetPropertyStr(ctx, slots, "proxyMap", JS_DupValue(ctx, proxy_map));
    JS_SetPropertyStr(ctx, slots, "type",
                      JS_NewString(ctx, type == STORAGE_TYPE_LOCAL ? "local" : "session"));
    JS_SetPropertyStr(ctx, slots, "document", JS_DupValue(ctx, doc));
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "storage: the slot key could not be interned");
    JS_SetProperty(ctx, obj, k, slots);
    JS_FreeAtom(ctx, k);
    /* §12.2.1 broadcast step 3's set is Storage §4.6's proxy map reference set, so the map has to know which
       Storage it stands for — see storage_shed.h. */
    storage_shed_proxy_map_bind(ctx, proxy_map, obj);
    JS_FreeValue(ctx, proxy_map);
    return obj;
}

static void storage_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global;

    DCHECK(g_ready, "a realm asked for Storage before storage_init declared the interface");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "storage_install_realm ran twice in one realm — everything already holding the "
                            "first Storage.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Storage.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Storage");
    idl_install_accessor(ctx, proto, "length", js_st_length, 0, -1);
    idl_install_method(ctx, proto, "key", g_id_key);
    idl_install_method(ctx, proto, "getItem", g_id_get_item);
    idl_install_method(ctx, proto, "setItem", g_id_set_item);
    idl_install_method(ctx, proto, "removeItem", g_id_remove_item);
    idl_install_method(ctx, proto, "clear", g_id_clear);
    JS_SetClassProto(ctx, g_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT on THIS realm's global. Storage declares no constructor, so `new Storage()` is
       a TypeError, and its PRESENCE is what `localStorage instanceof Storage` and every prototype-patching
       shim needs — which is the spelling a bundle uses to feature-detect the whole API. */
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "Storage", idl_interface_object(ctx, "Storage", proto));
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, proto);
}

void storage_init(JSContext *ctx)
{
    JSClassDef d = { "Storage", NULL, NULL, NULL, &STORAGE_EXOTIC };

    DCHECK(!g_ready, "storage_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "storageSlots", false);
    CHECK(!JS_IsException(g_key), "the Storage slot key could not be allocated");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_class, &d) == 0, "the Storage class could not be registered");
    g_id_key = idl_method_id(ctx, IDL_1ULONG, 1, js_st_key, 0);
    g_id_get_item = idl_method_id(ctx, IDL_1STR, 1, js_st_get_item, 0);
    g_id_set_item = idl_method_id(ctx, IDL_2STR, 2, js_st_set_item, 0);
    g_id_remove_item = idl_method_id(ctx, IDL_1STR, 1, js_st_remove_item, 0);
    g_id_clear = idl_method_id(ctx, NULL, 0, js_st_clear, 0);
    g_ready = 1;
    agent_state_value("storage", &g_key, "§12.2.1's private slot Symbol");
    agent_state_class("storage", &g_class, "the Storage class and its per-realm prototype slot");
    agent_state_flag("storage", &g_ready, "the declaration latch");
    agent_state_id("storage", &g_id_key, "§12.2.1's key() declaration");
    agent_state_id("storage", &g_id_get_item, "§12.2.1's getItem() declaration");
    agent_state_id("storage", &g_id_set_item, "§12.2.1's setItem() declaration");
    agent_state_id("storage", &g_id_remove_item, "§12.2.1's removeItem() declaration");
    agent_state_id("storage", &g_id_clear, "§12.2.1's clear() declaration");
    realm_declare_intrinsic(storage_install_realm);
}

void storage_free(JSRuntime *rt)
{
    if (!g_ready) return;
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_class = 0;
    g_ready = 0;
    g_id_key = g_id_get_item = g_id_set_item = g_id_remove_item = g_id_clear = -1;
}
