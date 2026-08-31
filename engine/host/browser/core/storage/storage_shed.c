/* THE STORAGE STANDARD'S MODEL — §4.2 "Storage keys" through §4.7 "Storage proxy maps".
 *
 * WHY THE WHOLE GRAPH IS JS OBJECTS AND NOT C RECORDS. Every byte a page puts in localStorage is SHARED
 * BASELINE STATE that flows mutate, so it has to time-travel: flow A writing `localStorage.token` must be
 * invisible to flow B, and a flow parked to the IDB cold tier last week must resume seeing exactly what it
 * wrote. CLAUDE.md §State-isolation gives the answer directly — "PLATFORM DATA A FLOW QUEUES IS A JS VALUE,
 * never malloc'd C" — and core/idl_slots.h gives the reason: an ordinary property write is ALREADY captured by
 * the COW delta, so a null-prototype object used as a map gets isolation, snapshotting, fork-visibility and
 * cold-tier parking with no new delta kind and no capture site to forget. A malloc'd hash table captured as a
 * head POINTER would revert the pointer on a context switch and leave the nodes reachable from nothing — a
 * leak the runtime's own GC walk cannot see. The alternative mechanism, cow_capture_host_record, is for a
 * component whose state is a C record behind a class opaque; this state is not one, and choosing it here would
 * be choosing the harder half of §State-isolation for no reason.
 *
 * THE GRAPH IS BUILT AT THE PRE-BOOT COW BASELINE, and that is load-bearing rather than tidy. §4.4 says "if
 * shed[key] does not exist, then set shed[key] to the result of running create a storage shelf" — built lazily,
 * that shelf would be created INSIDE whichever flow read localStorage first, so it would be flow-local, every
 * sibling would build its own, and each arm of a fork would get a private localStorage that shares nothing
 * with what boot wrote before the fork. Built here, at declaration time, every flow shares ONE map object and
 * each flow's writes to it are captured — which is both halves of what the spec means by shared storage.
 * core/file/storage_manager.c states the same argument for §2.1's root directory entries.
 *
 * §4.2's KEY IS THE AGENT'S. CLAUDE.md §Security makes an instance an ORIGIN-KEYED AGENT CLUSTER, so every
 * environment in this heap has the same origin and the user agent's shed holds exactly ONE shelf. That is not
 * assumed, it is ASSERTED at every obtain: an environment whose origin is not the agent's reached this heap
 * through a boundary that must have been an instance boundary.
 *
 * WHAT IS DELIBERATELY NOT HERE: §4.3's LEGACY-CLONE A TRAVERSABLE STORAGE SHED. A traversable navigable holds
 * the session shed, and an AUXILIARY browsing context is its own top-level traversable whose shed is a CLONE of
 * its opener's ("After creating a new auxiliary browsing context and document, the session storage is copied
 * over" — HTML §12.2.2). This build creates the shed of the agent's ROOT traversable and crashes at the obtain
 * for any other, naming that algorithm. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/dom/document.h"
#include "core/frame/window_proxy.h"
#include "core/idl_slots.h"
#include "core/storage/storage_shed.h"
#include "core/url/origin.h"
#include "solver/concolic.h"

/* §4.3: "A user agent holds a storage shed ... A user agent's storage shed holds all local storage data." */
static JSValue g_local_shed = JS_UNDEFINED;
/* §4.3: "A traversable navigable holds a storage shed ... holds all session storage data." THE AGENT'S ROOT
   traversable's, and only that one — see the header note on legacy-clone. */
static JSValue g_session_shed = JS_UNDEFINED;
static int     g_ready;

/* §4.1's REGISTERED STORAGE ENDPOINTS, as the table states them. `quota` is in bytes; -1 is the table's null,
   which §4.6 calls "the lack of a limit".
   File System §3 reaches `obtain a local storage bottle map` with the identifier "fileSystem", which that
   standard registers in ITS table rather than in this one — there is no row for it here because this component
   is not yet that standard's door (core/file/storage_manager.c reaches its root directly). The DFAIL below
   names it. */
typedef struct {
    const char *identifier;
    StorageType type;
    double      quota;
} StorageEndpoint;

static const StorageEndpoint ENDPOINTS[] = {
    { "caches",                     STORAGE_TYPE_LOCAL,   -1 },
    { "indexedDB",                  STORAGE_TYPE_LOCAL,   -1 },
    { "localStorage",               STORAGE_TYPE_LOCAL,   5.0 * 1024 * 1024 },
    { "serviceWorkerRegistrations", STORAGE_TYPE_LOCAL,   -1 },
    { "sessionStorage",             STORAGE_TYPE_SESSION, 5.0 * 1024 * 1024 },
};
#define ENDPOINTS_N ((int)(sizeof(ENDPOINTS) / sizeof(ENDPOINTS[0])))

/* §4.5's TWO MODES, in the StorageBucketMode enum's own order — the one place either string is spelled. The
   value is stored as the STRING the standard names rather than as the integer, for the reason every other
   record in this file holds spec vocabulary: the graph is JS objects so that it time-travels, and an integer
   would be a second encoding to agree with at the cold tier. */
static const char *const BUCKET_MODE[] = { "best-effort", "persistent" };
#define BUCKET_MODE_N ((int)(sizeof(BUCKET_MODE) / sizeof(BUCKET_MODE[0])))

/* §6 Usage and quota's STORAGE QUOTA of a storage shelf — "an implementation-defined conservative estimate of
 * the total amount of bytes it can hold."
 *
 * IT IS A CONSTANT, AND THAT IS THE SPEC'S OWN ANSWER RATHER THAN A SHRUG. §6 states two requirements and both
 * point away from measuring anything: "This amount SHOULD be LESS THAN the total storage space on the device"
 * and "It MUST NOT be a function of the available storage space on the device" — the second because, as §6's
 * own note says, "Directly or indirectly revealing available storage space can lead to fingerprinting and
 * leaking information outside the scope of the origin involved". So a user agent that derived this number from
 * a disk would be violating the standard in the direction of a fingerprinting surface, and a headless one has
 * no disk to derive it from anyway. What is left is a choice, which is what "implementation-defined" means.
 *
 * ONE GIBIBYTE. It is comfortably less than the storage space of any device this runs on (§6's SHOULD), it is
 * far above the 5 MiB §4.1 gives the two Web Storage endpoints, and it is a number a bundle's own arithmetic
 * can act on: `quota - usage` is what an offline cache checks before it writes, and an engine answering 0 there
 * would route every such bundle into its out-of-space branch and lose the branch that stores. */
#define SHELF_QUOTA_BYTES (1024.0 * 1024.0 * 1024.0)

/* A field of one of the records below. They are all null-prototype objects (core/idl_slots.h), so a read
   cannot reach the page and a write cannot be swallowed by an inherited accessor. */
static JSValue rec_get(JSContext *ctx, JSValueConst rec, const char *field)
{
    JSValue v;
    JSAtom a = JS_NewAtom(ctx, field);

    CHECK(a != JS_ATOM_NULL, "storage: a record field name could not be interned");
    if (JS_GetOwnSlot(ctx, &v, rec, a) <= 0) v = JS_UNDEFINED;
    JS_FreeAtom(ctx, a);
    return v;
}

static void rec_set(JSContext *ctx, JSValueConst rec, const char *field, JSValue v)
{
    CHECK(JS_SetPropertyStr(ctx, rec, field, v) >= 0,
          "storage: a record field could not be written");
}

/* §4.6: "A storage bottle has a map, which is initially an empty map ... a proxy map reference set, which is
   initially an empty set ... a quota." */
static JSValue bottle_new(JSContext *ctx, double quota)
{
    JSValue bottle = idl_slots_new(ctx);
    JSValue map = idl_slots_new(ctx);
    JSValue refs = JS_NewArray(ctx);

    CHECK(!JS_IsException(bottle) && !JS_IsException(map) && !JS_IsException(refs),
          "storage: a §4.6 storage bottle could not be allocated");
    rec_set(ctx, bottle, "map", map);
    rec_set(ctx, bottle, "proxyMaps", refs);
    rec_set(ctx, bottle, "quota", JS_NewFloat64(ctx, quota));
    return bottle;
}

/* §4.5's CREATE A STORAGE BUCKET, given a storage type: a bottle map holding one bottle per registered endpoint
   whose types contain that type, and — for "local" — §4.5's mode, initially "best-effort". */
static JSValue bucket_new(JSContext *ctx, StorageType type)
{
    JSValue bucket = idl_slots_new(ctx);
    JSValue bottles = idl_slots_new(ctx);
    int i;

    CHECK(!JS_IsException(bucket) && !JS_IsException(bottles),
          "storage: a §4.5 storage bucket could not be allocated");
    for (i = 0; i < ENDPOINTS_N; i++)
        if (ENDPOINTS[i].type == type)
            rec_set(ctx, bottles, ENDPOINTS[i].identifier, bottle_new(ctx, ENDPOINTS[i].quota));
    rec_set(ctx, bucket, "bottleMap", bottles);
    if (type == STORAGE_TYPE_LOCAL)
        rec_set(ctx, bucket, "mode", JS_NewString(ctx, BUCKET_MODE[STORAGE_BUCKET_BEST_EFFORT]));
    return bucket;
}

/* §4.4's CREATE A STORAGE SHELF: "Set shelf's bucket map['default'] to the result of running create a storage
   bucket with type." §4.4's own note — "For now 'default' is the only key that exists in a bucket map". */
static JSValue shelf_new(JSContext *ctx, StorageType type)
{
    JSValue shelf = idl_slots_new(ctx);
    JSValue buckets = idl_slots_new(ctx);

    CHECK(!JS_IsException(shelf) && !JS_IsException(buckets),
          "storage: a §4.4 storage shelf could not be allocated");
    rec_set(ctx, buckets, "default", bucket_new(ctx, type));
    rec_set(ctx, shelf, "bucketMap", buckets);
    return shelf;
}

/* §4.2's OBTAIN A STORAGE KEY for THIS realm's environment settings object, serialized — NULL is §4.2 step 2's
   FAILURE ("if key's origin is an opaque origin, then return failure"), which is a real answer and not an
   error. BORROWED: an origin's serialization lives for the agent (core/url/origin.h). */
static const char *obtain_storage_key(JSContext *ctx)
{
    const Origin *o = window_proxy_origin(document_window_proxy(ctx));

    DCHECK(o != NULL, "a storage key was obtained for a realm whose Document has no origin record — every "
                      "environment settings object has one, and §4.2's key is a tuple consisting of it");
    if (origin_is_opaque(o)) return NULL;
    /* §Security: an instance is an ORIGIN-KEYED AGENT CLUSTER, so every environment in this heap keys the same
       shelf. A second key here is a document that reached this heap across an instance boundary. */
    DCHECK(origin_same(o, origin_agent()),
           "a second STORAGE KEY appeared inside one instance — §Security makes an instance an origin-keyed "
           "agent cluster, so the user agent's shed here holds exactly one shelf; a document with another "
           "origin belongs to another instance and reaches its storage through that instance");
    return origin_serialized(o);
}

/* §4.3's SESSION SHED, which a TRAVERSABLE NAVIGABLE holds. This build creates the agent's ROOT traversable's;
   an auxiliary browsing context is its own top-level traversable and §4.3's legacy-clone is what fills its
   shed, which is the algorithm the crash names. */
static JSValue session_shed(JSContext *ctx)
{
    JSValueConst self = document_window_proxy(ctx);
    JSValue top = window_proxy_top_navigable(ctx, self);
    JSValue opener = window_proxy_opener_navigable(ctx, top);
    bool auxiliary = !JS_IsNull(opener) && !JS_IsUndefined(opener);

    JS_FreeValue(ctx, opener);
    JS_FreeValue(ctx, top);
    if (auxiliary) {
        DFAIL("sessionStorage was reached from an AUXILIARY browsing context, whose top-level traversable is "
              "its own and therefore holds its own §4.3 storage shed — build Storage §4.3's `legacy-clone a "
              "traversable storage shed` (HTML §12.2.2: \"After creating a new auxiliary browsing context and "
              "document, the session storage is copied over\") and key the sheds by traversable here");
    }
    return JS_DupValue(ctx, g_session_shed);
}

/* §4.6 Storage bottles' obtain-a-storage-bottle-map STEPS 8-9: "Let proxyMap be a new storage proxy map whose
   backing map is bottle's map" and "Append proxyMap to bottle's proxy map reference set".
   THE NUMBERS IN THIS ALGORITHM'S CITATIONS WERE EACH ONE LOW FROM STEP 6 ONWARD, and the drift begins where
   the shed is chosen: selecting it is steps 1-3 (`Let shed be null`, the `local` arm, and the `Otherwise:`
   whose own two sub-items are 3.1 and 3.2), not steps 1-2. Everything at or before that read correctly, which
   is why sampling the top of the list would not have caught it — the count is verified from the LAST step
   backwards. Ten top-level steps, ending at "Return proxyMap". */
static JSValue proxy_map_new(JSContext *ctx, JSValueConst bottle)
{
    JSValue pm = idl_slots_new(ctx);
    JSValue refs, len;
    uint32_t n = 0;

    CHECK(!JS_IsException(pm), "storage: a §4.7 storage proxy map could not be allocated");
    rec_set(ctx, pm, "backing", rec_get(ctx, bottle, "map"));
    /* THE BOTTLE ITSELF, which §4.7 does not put on a proxy map because the spec's own callers still hold it as
       an algorithm local. Here the proxy map is what crosses into HTML §12.2.1, and two of that section's steps
       are about the BOTTLE rather than the map — its §4.1 quota (setItem step 4) and its §4.6 proxy map
       reference set (broadcast step 3) — so the link travels with the map that stands in for it. */
    rec_set(ctx, pm, "bottle", JS_DupValue(ctx, bottle));
    /* THE Storage THIS MAP STANDS FOR IS NOT SET HERE, and cannot be: §4.6 step 9 appends the map to the
       reference set and HTML §12.2.2/§12.2.3 step 4 mints the Storage AFTER the obtain returns. The mint binds
       it (storage_shed_proxy_map_bind) with nothing between the two that can fail, and the walk over the
       reference set asserts every member is bound. */

    refs = rec_get(ctx, bottle, "proxyMaps");
    DCHECK(JS_IsArray(refs), "a §4.6 storage bottle has no proxy map reference set — it is created with an "
                             "empty one and nothing removes it");
    len = JS_GetPropertyStr(ctx, refs, "length");
    CHECK(JS_ToUint32(ctx, &n, len) == 0, "storage: a proxy map reference set has no length");
    JS_FreeValue(ctx, len);
    CHECK(JS_SetPropertyUint32(ctx, refs, n, JS_DupValue(ctx, pm)) >= 0,
          "storage: a proxy map could not be appended to its bottle's reference set");
    JS_FreeValue(ctx, refs);
    return pm;
}

/* §4.4 Storage shelves' OBTAIN A STORAGE SHELF, given a shed (which `type` selects, exactly as §4.6's steps
   1-3 do), THIS realm's environment settings object, and that type. It has TWO doors — §4.6's
   obtain-a-storage-bottle-map reaches it at that algorithm's step 4, and §4.4's own obtain-a-LOCAL-storage-
   shelf is it with the type fixed at "local" — so the four steps are written here once rather than at each.
   §4.4 STATES THREE ALGORITHMS (obtain a storage shelf, obtain a local storage shelf, create a storage shelf)
   and each numbers its steps from 1, so a bare "§4.4 step N" would name three different steps; every citation
   below therefore names the algorithm it counts. JS_UNDEFINED is obtain-a-storage-shelf step 2's FAILURE.
   OWNED. */
static JSValue obtain_shelf(JSContext *ctx, StorageType type)
{
    const char *key = obtain_storage_key(ctx);   /* obtain-a-storage-shelf step 1 */
    JSValue shed, shelf;

    DCHECK(g_ready, "a storage shelf was obtained before storage_shed_init built the shed");
    if (!key) return JS_UNDEFINED;               /* obtain-a-storage-shelf step 2 */

    shed = (type == STORAGE_TYPE_LOCAL) ? JS_DupValue(ctx, g_local_shed) : session_shed(ctx);
    shelf = rec_get(ctx, shed, key);
    JS_FreeValue(ctx, shed);
    /* OBTAIN-A-STORAGE-SHELF STEP 3 creates a missing shelf. §4.4 holds THREE algorithms and each numbers its
       own steps from 1, so every citation of this section names which one it is counting. It CANNOT be
       missing: the one storage key this instance can have is the agent's, the shelf for it is built at the
       baseline, and obtain_storage_key above asserts no second key reaches here — so a miss means the key
       changed under a shelf that was built for another. */
    DCHECK(JS_IsObject(shelf),
           "the storage shelf for this instance's storage key does not exist — it is created at the pre-boot "
           "baseline for the agent's origin, so an absent one means the environment's key is not the agent's");
    return shelf;                                /* obtain-a-storage-shelf step 4 */
}

JSValue storage_shed_obtain_local_shelf(JSContext *ctx)
{
    return obtain_shelf(ctx, STORAGE_TYPE_LOCAL);
}

JSValue storage_shed_obtain_bottle_map(JSContext *ctx, StorageType type, const char *identifier)
{
    JSValue shelf, buckets, bucket, bottles, bottle, pm;
    int i, known = 0;

    DCHECK(g_ready, "a storage bottle map was obtained before storage_shed_init built the shed");
    DCHECK(identifier != NULL && *identifier, "a storage bottle map was obtained for no §4.1 storage identifier");
    for (i = 0; i < ENDPOINTS_N; i++)
        if (ENDPOINTS[i].type == type && strcmp(ENDPOINTS[i].identifier, identifier) == 0) known = 1;
    if (!known)
        DFAILF("no registered storage endpoint of this type has the identifier `%s` — Storage §4.1's table "
               "is the five rows this component builds, and a standard that registers its own (File System "
               "§3's \"fileSystem\") adds its row to ENDPOINTS here rather than obtaining a bottle nothing "
               "created", identifier);

    /* §4.6 steps 1-3 choose the shed and step 4 obtains the shelf; obtain_shelf performs both, because which
       shed a type names is the same fact §4.4's own algorithm needs. */
    shelf = obtain_shelf(ctx, type);                      /* §4.6 steps 1-4 */
    if (JS_IsUndefined(shelf)) return JS_UNDEFINED;       /* §4.6 step 5: "If shelf is failure, return failure" */

    buckets = rec_get(ctx, shelf, "bucketMap");
    bucket = rec_get(ctx, buckets, "default");            /* §4.6 step 6 */
    bottles = rec_get(ctx, bucket, "bottleMap");
    bottle = rec_get(ctx, bottles, identifier);           /* §4.6 step 7 */
    DCHECK(JS_IsObject(bottle),
           "a §4.5 storage bucket has no bottle for a registered endpoint of its own type — create-a-storage-"
           "bucket makes one per endpoint, so an absent bottle means the bucket was built for the other type");
    pm = proxy_map_new(ctx, bottle);                      /* §4.6 steps 8-9 */

    JS_FreeValue(ctx, bottle);
    JS_FreeValue(ctx, bottles);
    JS_FreeValue(ctx, bucket);
    JS_FreeValue(ctx, buckets);
    JS_FreeValue(ctx, shelf);
    return pm;
}

JSValue storage_shed_backing_map(JSContext *ctx, JSValueConst proxy_map)
{
    JSValue m = rec_get(ctx, proxy_map, "backing");

    DCHECK(JS_IsObject(m), "a storage proxy map has no backing map — §4.7 says every operation is performed on "
                           "it, so a proxy map without one stands in for nothing");
    return m;
}

double storage_shed_quota(JSContext *ctx, JSValueConst proxy_map)
{
    JSValue bottle = rec_get(ctx, proxy_map, "bottle");
    JSValue q = rec_get(ctx, bottle, "quota");
    double d = 0;

    DCHECK(JS_IsNumber(q), "a §4.6 storage bottle has no quota — §4.1's table gives every endpoint one, with "
                           "null (here -1) meaning the lack of a limit");
    CHECK(JS_ToFloat64(ctx, &d, q) == 0, "storage: a bottle quota is not a number");
    JS_FreeValue(ctx, q);
    JS_FreeValue(ctx, bottle);
    return d;
}

/* ---- §4.5's MODE, and §6's USAGE AND QUOTA — the facts a SHELF answers ------------------------------------
 *
 * They are here and not in the interface over them for the reason this component exists at all: a shelf's mode
 * and a shelf's usage are the MODEL's facts, and the STANDARD gives them more than one reader — Storage §8
 * API's three members, Storage §5 Persistence permission's permission revocation algorithm, and HTML §12.2.1
 * The Storage interface's setItem step 4, which charges the same byte measure against §4.1's per-bottle quota.
 * A fact with several readers belongs to the model rather than to whichever interface asked first. */

/* §4.4 Storage shelves: "A storage shelf ... holds a bucket map ... For now "default" is the only key that
   exists in a bucket map." OWNED. */
static JSValue bucket_default(JSContext *ctx, JSValueConst shelf)
{
    JSValue buckets = rec_get(ctx, shelf, "bucketMap");
    JSValue bucket;

    DCHECK(JS_IsObject(buckets), "a §4.4 storage shelf has no bucket map — create-a-storage-shelf gives every "
                                 "shelf one before it returns, so a shelf without one was not created by it");
    bucket = rec_get(ctx, buckets, "default");
    JS_FreeValue(ctx, buckets);
    DCHECK(JS_IsObject(bucket),
           "a §4.4 storage shelf's bucket map has no \"default\" — §4.4's CREATE-A-STORAGE-SHELF step 2 sets it "
           "and §4.4's own note says that is the only key a bucket map has, so an absent one means this object "
           "is not a storage shelf");
    return bucket;
}

StorageBucketMode storage_shed_bucket_mode(JSContext *ctx, JSValueConst shelf)
{
    JSValue bucket = bucket_default(ctx, shelf);
    JSValue m = rec_get(ctx, bucket, "mode");
    const char *s;
    int i, found = -1;

    /* §4.5 GIVES THE MODE TO A LOCAL STORAGE BUCKET AND NOT TO A SESSION ONE, so an absent field here is a
       SESSION shelf reaching a question only a local shelf can answer — and answering "best-effort" for it
       would be reporting the absence of the member as one of its two values. */
    DCHECK(JS_IsString(m),
           "a §4.5 storage bucket has no mode — only a LOCAL storage bucket has one, and Storage §8's "
           "persisted() and persist() both reach this through obtain-a-LOCAL-storage-shelf, so a bucket "
           "without one is a SESSION bucket that arrived through a caller that did not obtain a local shelf");
    s = JS_ToCString(ctx, m);
    CHECK(s != NULL, "storage: a §4.5 bucket mode could not be read");
    for (i = 0; i < BUCKET_MODE_N; i++)
        if (strcmp(s, BUCKET_MODE[i]) == 0) found = i;
    DCHECKF(found >= 0, "a §4.5 storage bucket's mode is `%s`, which is neither of the two values §4.5 gives it "
                        "(\"best-effort\" and \"persistent\") — the two spellings live in this file alone, so a "
                        "third one was written by something that does not go through storage_shed_bucket_set_mode",
            s);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, m);
    JS_FreeValue(ctx, bucket);
    return (StorageBucketMode)found;
}

void storage_shed_bucket_set_mode(JSContext *ctx, JSValueConst shelf, StorageBucketMode mode)
{
    JSValue bucket = bucket_default(ctx, shelf);
    JSValue prev = rec_get(ctx, bucket, "mode");

    DCHECK(JS_IsString(prev),
           "a §4.5 storage bucket that has no mode was given one — only a LOCAL storage bucket has the member "
           "at all, so this write would CREATE it on a session bucket rather than change it");
    JS_FreeValue(ctx, prev);
    DCHECK(mode >= 0 && (int)mode < BUCKET_MODE_N,
           "a §4.5 storage bucket was set to a mode that is neither of the two values §4.5 gives it");
    /* AN ORDINARY PROPERTY WRITE ON A BASELINE OBJECT, which is what makes the mode time-travel: the flow that
       calls §8's persist() has its own persisted world and a sibling that did not still sees "best-effort". */
    rec_set(ctx, bucket, "mode", JS_NewString(ctx, BUCKET_MODE[mode]));
    JS_FreeValue(ctx, bucket);
}

size_t storage_shed_string_bytes(JSContext *ctx, JSValueConst v)
{
    size_t n = 0;
    const char *s;

    if (concolic_is(v)) {
        const char *shape = concolic_shape_c(v);

        DCHECK(shape != NULL, "unknown external input stored in a storage bottle has no shape — concolic_is "
                              "said it is one, and every concolic carries the expression the run built");
        return shape ? strlen(shape) : 0;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    CHECK(s != NULL, "storage: a stored value could not be measured — the value is already a string, so the "
                     "only way this fails is an allocation, and a usage computed without it would silently "
                     "admit a write the spec refuses and under-report §6's usage");
    JS_FreeCString(ctx, s);
    return n;
}

double storage_shed_map_usage(JSContext *ctx, JSValueConst map)
{
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    double total = 0;

    DCHECK(JS_IsObject(map), "the bytes of a §4.6 storage bottle's map were asked for something that is not a "
                             "map — every bottle is created with one and nothing removes it");
    /* §4.7's own note licenses whatever order this walk is in; a SUM does not depend on one either way. */
    CHECK(JS_GetOwnPropertyNames(ctx, &tab, &n, map, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0,
          "storage: a storage bottle's map could not be enumerated to measure what it holds");
    for (i = 0; i < n; i++) {
        JSValue kv = JS_AtomToValue(ctx, tab[i].atom), vv = JS_UNDEFINED;
        int got;

        CHECK(!JS_IsException(kv), "storage: a bottle map key could not be read back to measure it");
        total += (double)storage_shed_string_bytes(ctx, kv);
        JS_FreeValue(ctx, kv);
        /* EVERY ENUMERATED NAME HAS A VALUE. The map is a null-prototype record this component created and only
           an endpoint's own store writes it, so a name with no own DATA slot means something defined an
           ACCESSOR on a bottle's map — which JS_GetOwnSlot refuses by design and which no endpoint does. The
           read is performed ONCE into `got`, because a DCHECK's condition must be side-effect-free and this one
           takes a reference. */
        got = JS_GetOwnSlot(ctx, &vv, map, tab[i].atom);
        DCHECK(got > 0,
               "a §4.6 storage bottle's map enumerated a name that is not an own data property of it — the map "
               "is written only by its endpoint's store, so an accessor or a hole there was put on it by "
               "something that is not one");
        if (got > 0) {
            /* §12.2.1's keys and values are DOMStrings by the IDL, and unknown external input crosses that
               conversion as itself — anything else in the map came from a writer that did not convert. */
            DCHECK(JS_IsString(vv) || concolic_is(vv),
                   "a §4.6 storage bottle's map holds a value that is neither a string nor unknown external "
                   "input — HTML §12.2.1 declares its keys and values `DOMString`, so a value of any other "
                   "kind was written by a path that skipped the member's own declared conversion");
            total += (double)storage_shed_string_bytes(ctx, vv);
            JS_FreeValue(ctx, vv);
        }
    }
    for (i = 0; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    return total;
}

/* NAMED RESIDUAL — the BUCKET FILE SYSTEM's bytes are not in this sum, and the code is right rather than
 * unfinished, so there is nothing here to crash on.
 *   NOT COVERED: File System §3 Accessing the Bucket File System's data. That standard registers its own
 *     storage endpoint under the identifier "fileSystem", and this component has no ENDPOINTS row for it — so
 *     the entries live in core/file/file_system.c behind FS_ROOT_BUCKET, reached directly by
 *     core/file/storage_manager.c's getDirectory(), and no bottle of this shelf holds them. The sum below is
 *     therefore complete over every endpoint the shelf HAS, which is what makes it a narrowing rather than a
 *     wrong answer.
 *   WHAT THE NEXT DIFF BUILDS: a "fileSystem" row in this file's ENDPOINTS table with the entries stored in
 *     that bottle's map — which is exactly what obtain-a-storage-bottle-map's own DFAILF above already
 *     demands of any standard registering an endpoint ("adds its row to ENDPOINTS here rather than obtaining a
 *     bottle nothing created"). The sum then reaches it with no change to this walk, because the walk asks the
 *     bucket what bottles it has.
 *   HOW ITS ABSENCE WOULD SHOW: a page that writes N bytes through a FileSystemWritableFileStream and then
 *     reads `navigator.storage.estimate()` sees `usage` UNCHANGED by the write. */
double storage_shed_shelf_usage(JSContext *ctx, JSValueConst shelf)
{
    JSValue bucket = bucket_default(ctx, shelf);
    JSValue bottles = rec_get(ctx, bucket, "bottleMap");
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    double total = 0;

    DCHECK(JS_IsObject(bottles), "a §4.5 storage bucket has no bottle map — create-a-storage-bucket gives every "
                                 "bucket one before it returns");
    /* THE BUCKET IS ASKED WHAT IT HOLDS rather than the ENDPOINTS table being re-read: §4.5's create-a-storage-
       bucket is what filled this map, so enumerating it is reading the model's own statement of its bottles,
       and a table read here would be a second copy of that statement to disagree with. */
    CHECK(JS_GetOwnPropertyNames(ctx, &tab, &n, bottles, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0,
          "storage: a §4.5 bucket's bottle map could not be enumerated to measure §6's usage");
    for (i = 0; i < n; i++) {
        JSValue bottle = JS_UNDEFINED, map;
        int got = JS_GetOwnSlot(ctx, &bottle, bottles, tab[i].atom);

        DCHECK(got > 0, "a §4.5 storage bucket's bottle map enumerated a name it holds no bottle for — "
                        "create-a-storage-bucket writes one bottle per endpoint and nothing removes one");
        if (got > 0) {
            map = rec_get(ctx, bottle, "map");
            DCHECK(JS_IsObject(map), "a §4.6 storage bottle has no map — it is created with an empty one");
            total += storage_shed_map_usage(ctx, map);
            JS_FreeValue(ctx, map);
            JS_FreeValue(ctx, bottle);
        }
    }
    for (i = 0; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    JS_FreeValue(ctx, bottles);
    JS_FreeValue(ctx, bucket);
    return total;
}

double storage_shed_shelf_quota(JSContext *ctx, JSValueConst shelf)
{
    DCHECK(JS_IsObject(shelf), "§6's storage quota was asked of something that is not a §4.4 storage shelf");
    (void)ctx; (void)shelf;
    return SHELF_QUOTA_BYTES;
}

void storage_shed_proxy_map_bind(JSContext *ctx, JSValueConst proxy_map, JSValueConst storage)
{
    JSValue prev = rec_get(ctx, proxy_map, "storage");

    DCHECK(JS_IsUndefined(prev),
           "a §4.7 storage proxy map was bound to a SECOND Storage object — HTML §12.2.2 and §12.2.3 obtain a "
           "fresh map per Storage, so two over one map would put one document in §12.2.1 broadcast step 3's "
           "set twice and fire the event at it twice");
    JS_FreeValue(ctx, prev);
    DCHECK(JS_IsObject(storage), "a storage proxy map was bound to something that is not a Storage object");
    rec_set(ctx, proxy_map, "storage", JS_DupValue(ctx, storage));
}

JSValue storage_shed_other_storages(JSContext *ctx, JSValueConst proxy_map)
{
    JSValue bottle = rec_get(ctx, proxy_map, "bottle");
    JSValue refs = rec_get(ctx, bottle, "proxyMaps");
    JSValue out = JS_NewArray(ctx), len;
    uint32_t n = 0, i, k = 0;
    bool self_seen = false;

    CHECK(!JS_IsException(out), "storage: §12.2.1 broadcast step 3's remoteStorages could not be allocated");
    DCHECK(JS_IsArray(refs), "a §4.6 storage bottle has no proxy map reference set");
    len = JS_GetPropertyStr(ctx, refs, "length");
    CHECK(JS_ToUint32(ctx, &n, len) == 0, "storage: a proxy map reference set has no length");
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++) {
        JSValue pm = JS_GetPropertyUint32(ctx, refs, i), s;

        DCHECK(JS_IsObject(pm), "a §4.6 proxy map reference set holds something that is not a proxy map");
        if (JS_VALUE_GET_PTR(pm) == JS_VALUE_GET_PTR(proxy_map)) {   /* step 3's "excluding storage" */
            self_seen = true;
            JS_FreeValue(ctx, pm);
            continue;
        }
        s = rec_get(ctx, pm, "storage");
        JS_FreeValue(ctx, pm);
        /* EVERY MAP IN THE SET IS BOUND. §4.6 step 7 appends the map and storage_new binds it with nothing
           between them that can fail, so an unbound one means a map reached the set by another route — and
           §12.2.1 step 4 would then have a target it cannot name. */
        DCHECK(JS_IsObject(s), "a proxy map in a §4.6 reference set stands for no Storage object — the obtain "
                               "appends it and HTML §12.2.2/§12.2.3's mint binds it, so an unbound one was put "
                               "there by something that is not storage_shed_obtain_bottle_map");
        CHECK(JS_SetPropertyUint32(ctx, out, k++, s) >= 0,
              "storage: a remote Storage could not be collected into §12.2.1 broadcast step 3's list");
    }
    JS_FreeValue(ctx, refs);
    JS_FreeValue(ctx, bottle);
    (void)self_seen;   /* the witness is the assert's, and an assert is compiled out of a release build */
    DCHECK(self_seen, "a proxy map is not in its own bottle's reference set — §4.6 step 7 appends every one it "
                      "mints, so a set that does not contain this map was built by something else");
    return out;
}

void storage_shed_init(JSContext *ctx)
{
    const char *key;

    DCHECK(!g_ready, "storage_shed_init ran twice — the sheds are the AGENT's, and a second set would give the "
                     "same origin two localStorages");
    DCHECK(origin_agent() != NULL,
           "the storage shed was declared before the agent adopted its principal — §4.2's key is that origin, "
           "and core/platform.c adopts it before the declaration pass for exactly this reason");
    key = origin_serialized(origin_agent());

    g_local_shed = idl_slots_new(ctx);
    g_session_shed = idl_slots_new(ctx);
    CHECK(!JS_IsException(g_local_shed) && !JS_IsException(g_session_shed),
          "storage: a §4.3 storage shed could not be allocated");
    /* §4.4's shelf for THIS instance's one storage key, in each shed. An OPAQUE principal has no storage key at
       all (§4.2 step 2), so it gets no shelf and every obtain answers failure — which is the "SecurityError"
       HTML §12.2.2 and §12.2.3 throw, reached by the spec's own route rather than by a special case. */
    if (!origin_is_opaque(origin_agent())) {
        rec_set(ctx, g_local_shed, key, shelf_new(ctx, STORAGE_TYPE_LOCAL));
        rec_set(ctx, g_session_shed, key, shelf_new(ctx, STORAGE_TYPE_SESSION));
    }
    /* THE WHOLE GRAPH MUST BE BASELINE. A record created inside a flow is flow-local, so a sibling would never
       see it and each arm of a fork would get a private storage area — see this file's own header.
       IT IS THE COMPONENT'S HALF OF A PRECONDITION core/platform.c NOW STATES FOR THE WHOLE DECLARATION PASS,
       and both are kept: that one says the browser's baseline is built at the baseline stamp and names the host
       that broke it, this one says the same of the two objects whose being shared is the point. This assert is
       what found it — solver/cow.c's fork raised the global stamp in HOST TIME, so every object the platform
       declared was stamped above 0 and capturable by no delta, silently, for every component. */
    DCHECK(JS_ObjFlowGen(g_local_shed) == 0 && JS_ObjFlowGen(g_session_shed) == 0,
           "the storage sheds were created inside a FLOW — every flow would then build its own and localStorage "
           "would share nothing across a fork; the declaration pass must run at the pre-boot COW baseline");
    g_ready = 1;
    agent_state_value("storage_shed", &g_local_shed, "Storage §4.3's user agent storage shed");
    agent_state_value("storage_shed", &g_session_shed, "Storage §4.3's root traversable's storage shed");
    agent_state_flag("storage_shed", &g_ready, "the declaration latch");
}

void storage_shed_free(JSRuntime *rt)
{
    if (!g_ready) return;
    JS_FreeValueRT(rt, g_local_shed);
    JS_FreeValueRT(rt, g_session_shed);
    g_local_shed = JS_UNDEFINED;
    g_session_shed = JS_UNDEFINED;
    g_ready = 0;
}
