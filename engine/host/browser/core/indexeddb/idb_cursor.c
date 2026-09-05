/* INDEXED DATABASE §2.10's CURSOR, §4.9's IDBCursor and IDBCursorWithValue over it, and §6.7's ITERATE A
 * CURSOR.
 *
 * WHAT A CURSOR ACTUALLY IS IN THIS STANDARD, AND WHY IT IS NOT A NEW MECHANISM. §4.5's `openCursor` places an
 * ordinary request whose operation is §6.7, and §4.9's `advance`/`continue`/`continuePrimaryKey` place THE SAME
 * REQUEST AGAIN — "let request be this's request; set request's processed flag to false; set request's done
 * flag to false; ... run asynchronously execute a request with this's source handle, operation, AND REQUEST".
 * That optional third operand of §5.6 is the whole of the cursor's asynchrony: one IDBRequest object, re-fired,
 * whose `success` event carries the cursor when a record was found and `null` when the range is exhausted. So
 * there is nothing here that queues, nothing that polls and no second scheduler — a cursor is a request that
 * comes back, and §5.6's machine is the one that brings it.
 *
 * §6.7 IS A STEP MACHINE BECAUSE ITS STEP 4 NAMES A LIST OF THE PAGE'S SIZE. "Let records be the list of
 * records in source", and step 9.1 is "the FIRST record in records which satisfy all of the following
 * requirements" — a scan whose length is however many records the page put in the store. §scheduler forbids
 * driving that to completion at any depth, so the scan rests once per record examined, exactly as §4.5's index
 * population walk rests once per record it files (core/indexeddb/idb_index_populate.c). The `while count is
 * greater than 0` of step 9 is a second unbounded loop — `advance(4294967295)` is a legal call — and it is the
 * same rest point re-entered, not a C `for`.
 *
 * WHY THE SEARCH IS A SCAN AND NOT A SEEK. §2.2 keeps a store's list sorted by key and §2.6 keeps an index's
 * sorted primarily by key and secondarily by value, so every requirement step 9.1 lists is MONOTONE in that
 * order and a binary search would answer the same record. It is still a scan, because §6.7 states one and
 * because the requirement list is the part a reader has to be able to check against the standard: the seek is
 * an optimisation of an algorithm that must first exist, and its precondition (the list is sorted the way the
 * two sections say) is asserted by the components that keep the lists rather than assumed here.
 *
 * THE ORDER IS THE SPEC AND NOT A DETAIL. "next" and "nextunique" take the FIRST record satisfying the
 * requirements and "prev" and "prevunique" take the LAST, which is why this file scans forward for the first
 * two and BACKWARD for the second two — a backward scan's first hit IS the list's last hit, so there is one
 * loop and one predicate rather than two searches that could disagree. "prevunique" then needs a SECOND,
 * forward scan, because its found record is "the first record in records whose key is equal to temp record's
 * key" and not the temp record itself; that is a stage of its own for the same reason.
 *
 * WHAT TIME-TRAVELS. §2.10's eleven fields live in internal slots under a private Symbol, so every write —
 * the position, the got value flag, the key and the value — is an ordinary property write the per-flow COW
 * delta captures. Two flows iterating one store see two cursors at two positions over two documents, and a
 * flow parked mid-iteration resumes at the exact record it was examining. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/structured_clone.h"
#include "core/indexeddb/idb_cursor.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_index.h"
#include "core/indexeddb/idb_index_handle.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_key_array.h"
#include "core/indexeddb/idb_key_path.h"
#include "core/indexeddb/idb_key_range.h"
#include "core/indexeddb/idb_object_store.h"
#include "core/indexeddb/idb_request.h"
#include "core/indexeddb/idb_transaction.h"

/* §2.10's OWN FIELDS, spelled once. Every one of them is a sentence of that section. */
#define CU_SOURCE_HANDLE  "sourceHandle"    /* "the index handle or the object store handle that opened it" */
#define CU_TRANSACTION    "transaction"     /* "the transaction from the cursor's source handle" */
#define CU_RANGE          "range"           /* "a range of records in either an index or an object store" */
#define CU_DIRECTION      "direction"
#define CU_POSITION       "position"        /* a key record, or JS_UNDEFINED for §6.7's "position is defined" */
#define CU_OS_POSITION    "objectStorePosition"   /* §2.10's object store position, for an INDEX source only */
#define CU_KEY            "key"
#define CU_VALUE          "value"
#define CU_GOT_VALUE      "gotValue"
#define CU_REQUEST        "request"
#define CU_KEY_ONLY       "keyOnly"

/* §4.9's IDBCursorDirection, as the four things the engine actually branches on. The STRING is what
   `direction` answers with and what the IDL converted, and it is stored rather than the enum for that reason.
   The enum that indexes this list and the decode that produces it are in the header, because §6.2's and
   §6.3's retrieve-multiple branch on the same four and two enums over one list is one fact answered twice. */
IDL_ENUM_VALUES_EXTERN(IDB_CURSOR_DIRECTIONS, "next", "nextunique", "prev", "prevunique");

static JSValue g_key;
static int     g_ready;
/* TWO CLASSES BECAUSE §4.9 DECLARES TWO INTERFACES, and a class is what carries the per-realm prototype slot:
   one class for both would put `value` on a key-only cursor, which §4.9's last sentence is exactly about ("a
   cursor that has its key only flag set to FALSE implements the IDBCursorWithValue interface as well"). The
   RECORD is identical, which is why the two share every function below. */
static JSClassID g_cursor_class, g_cursor_wv_class;
static JSRuntime *g_cursor_rt;
static int g_iterate_stepid = -1;
static int g_id_advance = -1, g_id_continue = -1, g_id_continue_pk = -1;
static int g_id_update = -1, g_id_delete = -1;

/* ---- the record ------------------------------------------------------------------------------------------ */

static JSValue cu_slots(JSContext *ctx, JSValueConst c)
{
    JSAtom k;
    JSValue st;

    DCHECK(g_ready, "an IDBCursor slot was asked for before idb_cursor_init made the key");
    if (!JS_IsObject(c))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBCursor slot key could not be interned");
    if (JS_GetOwnSlot(ctx, &st, c, k) <= 0)
        st = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return st;
}

bool idb_cursor_is(JSValueConst v)
{
    JSClassID id = JS_GetClassID(v);

    return (g_cursor_class != 0 && id == g_cursor_class) ||
           (g_cursor_wv_class != 0 && id == g_cursor_wv_class);
}

static bool cu_brand(JSContext *ctx, JSValueConst this_val)
{
    if (idb_cursor_is(this_val)) return true;
    JS_ThrowTypeError(ctx, "an IDBCursor member was reached on something that is not an IDBCursor");
    return false;
}

static JSValue cu_get(JSContext *ctx, JSValueConst c, const char *field)
{
    JSValue slots = cu_slots(ctx, c), v;

    DCHECK(JS_IsObject(slots), "an IDBCursor field was read off a value carrying no slot record");
    v = JS_GetPropertyStr(ctx, slots, field);
    JS_FreeValue(ctx, slots);
    return v;
}

/* `v` is CONSUMED. */
static void cu_set(JSContext *ctx, JSValueConst c, const char *field, JSValue v)
{
    JSValue slots = cu_slots(ctx, c);

    DCHECK(JS_IsObject(slots), "an IDBCursor field was written on a value carrying no slot record");
    JS_SetPropertyStr(ctx, slots, field, v);
    JS_FreeValue(ctx, slots);
}

static bool cu_flag(JSContext *ctx, JSValueConst c, const char *field)
{
    JSValue v = cu_get(ctx, c, field);
    bool b = JS_ToBool(ctx, v);

    JS_FreeValue(ctx, v);
    return b;
}

/* §2.10's DIRECTION, decoded. The string is the field, because that is what §4.9's getter answers with and what
   the IDL's enumeration already checked; this is the one place it becomes a branch. */
int idb_cursor_direction_of(JSContext *ctx, JSValueConst v)
{
    const char *s;
    int i;

    DCHECK(JS_IsString(v), "§4.9's direction was decoded from something that is not a string — §3.2.18's "
                           "conversion and the enumeration's own default have both already run by the time "
                           "anything reads one");
    s = JS_ToCString(ctx, v);
    CHECK(s != NULL, "IndexedDB: a direction could not be read");
    for (i = 0; IDB_CURSOR_DIRECTIONS[i]; i++)
        if (!strcmp(s, IDB_CURSOR_DIRECTIONS[i])) { JS_FreeCString(ctx, s); return i; }
    JS_FreeCString(ctx, s);
    /* ALWAYS FATAL, and not a DCHECK with a `return IDB_CURSOR_DIR_NEXT` behind it: that `?:` past a broken
       invariant would make an unvalidated string silently mean "next" in a release build, so §6.7's iteration
       order AND §6.2's record order would both be decided by a value nothing checked. §3.2.18's enumeration
       check is what makes an unrecognised value impossible, so one arriving here means a member was declared
       without its value list — this engine's own IDL disagreeing with itself, not a page's input. */
    CHECK_FAIL("a direction that §4.9's IDBCursorDirection does not list reached the decode — the member or "
               "the field that produced it never went through §3.2.18's enumeration check");
}

static int cu_direction(JSContext *ctx, JSValueConst c)
{
    JSValue d = cu_get(ctx, c, CU_DIRECTION);
    int i = idb_cursor_direction_of(ctx, d);

    JS_FreeValue(ctx, d);
    return i;
}

/* §2.10's SOURCE — "an index or an object store from the cursor's SOURCE HANDLE. If the cursor's source handle
   is an index handle, then the cursor's source is the index handle's associated index. Otherwise, cursor's
   source is the object store handle's associated object store." Derived and never stored, so §4.5's
   `deleteIndex` cannot leave a cursor naming an index its handle no longer does. OWNED. */
static JSValue cu_source(JSContext *ctx, JSValueConst c)
{
    JSValue h = cu_get(ctx, c, CU_SOURCE_HANDLE), src;

    if (idb_index_handle_is(h))
        src = idb_index_handle_index(ctx, h);
    else {
        DCHECK(idb_object_store_is(h), "a cursor's source handle is neither an index handle nor an object "
                                       "store handle — §2.10 gives every cursor one of the two");
        src = idb_object_store_handle_store(ctx, h);
    }
    JS_FreeValue(ctx, h);
    return src;
}

static bool cu_source_is_index(JSContext *ctx, JSValueConst c)
{
    JSValue h = cu_get(ctx, c, CU_SOURCE_HANDLE);
    bool b = idb_index_handle_is(h);

    JS_FreeValue(ctx, h);
    return b;
}

/* §2.10's EFFECTIVE OBJECT STORE — "if the source of a cursor is an object store, the effective object store of
   the cursor is THAT object store ... if the source of a cursor is an index, the effective object store of the
   cursor is that index's REFERENCED OBJECT STORE". OWNED. */
static JSValue cu_effective_store(JSContext *ctx, JSValueConst c)
{
    JSValue src = cu_source(ctx, c), store;

    if (!cu_source_is_index(ctx, c))
        return src;
    store = idb_index_store(ctx, src);
    JS_FreeValue(ctx, src);
    return store;
}

/* §2.10's EFFECTIVE KEY — "the effective key of the cursor is the cursor's POSITION ... [or, for an index
   source] the cursor's OBJECT STORE POSITION". OWNED, JS_UNDEFINED before the first iteration. */
static JSValue cu_effective_key(JSContext *ctx, JSValueConst c)
{
    return cu_get(ctx, c, cu_source_is_index(ctx, c) ? CU_OS_POSITION : CU_POSITION);
}

/* §6.7 step 4's `records`, as the two questions a scan asks of it. §2.2 and §2.6 each keep their own list and
   each keeps it sorted by its own ordering, which is why this is a pair of calls into those components rather
   than a list handed out: a caller holding the Array would be a second writer of invariants neither component
   could then assert. */
static uint32_t cu_record_count(JSContext *ctx, JSValueConst c, JSValueConst source)
{
    return cu_source_is_index(ctx, c) ? idb_index_record_count(ctx, source)
                                      : idb_store_record_count(ctx, source);
}

static void cu_record_at(JSContext *ctx, JSValueConst c, JSValueConst source, uint32_t i,
                         JSValue *key, JSValue *value)
{
    if (cu_source_is_index(ctx, c))
        idb_index_record_at(ctx, source, i, key, value);
    else
        idb_store_record_at(ctx, source, i, key, value);
}

JSValue idb_cursor_new(JSContext *ctx, JSValueConst source_handle, JSValueConst transaction,
                       const char *direction, JSValueConst range, bool key_only)
{
    JSClassID cls = key_only ? g_cursor_class : g_cursor_wv_class;
    JSValue c, st, proto;
    JSAtom k;

    DCHECK(g_ready, "a cursor was created before idb_cursor_init declared the interface");
    DCHECK(idb_object_store_is(source_handle) || idb_index_handle_is(source_handle),
           "§2.10's source handle is an object store handle or an index handle, and a cursor was created over "
           "neither — every opening member passes its own `this`");
    DCHECK(idb_transaction_is(transaction),
           "§2.10's transaction is \"the transaction from the cursor's source handle\", and a cursor was "
           "created with something that is not a transaction");
    proto = JS_GetClassProto(ctx, cls);
    DCHECK(!JS_IsNull(proto), "an IDBCursor prototype was asked for in a realm that never ran its install");
    c = JS_NewObjectProtoClass(ctx, proto, cls);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(c), "IndexedDB: the IDBCursor allocation failed");
    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "IndexedDB: the IDBCursor slot record allocation failed");
    JS_SetPropertyStr(ctx, st, CU_SOURCE_HANDLE, JS_DupValue(ctx, source_handle));
    JS_SetPropertyStr(ctx, st, CU_TRANSACTION, JS_DupValue(ctx, transaction));
    JS_SetPropertyStr(ctx, st, CU_RANGE, JS_DupValue(ctx, range));
    JS_SetPropertyStr(ctx, st, CU_DIRECTION, JS_NewString(ctx, direction));
    /* "undefined position ... undefined key and value" — §6.7's "if position is DEFINED" is a question about
       exactly this, so the absence is undefined and never a null a comparison would have to tell apart. */
    JS_SetPropertyStr(ctx, st, CU_POSITION, JS_UNDEFINED);
    JS_SetPropertyStr(ctx, st, CU_OS_POSITION, JS_UNDEFINED);
    JS_SetPropertyStr(ctx, st, CU_KEY, JS_UNDEFINED);
    JS_SetPropertyStr(ctx, st, CU_VALUE, JS_UNDEFINED);
    JS_SetPropertyStr(ctx, st, CU_GOT_VALUE, JS_FALSE);
    /* "Set cursor's request to request" is the opening member's LAST step but one, so there is none yet. */
    JS_SetPropertyStr(ctx, st, CU_REQUEST, JS_UNDEFINED);
    JS_SetPropertyStr(ctx, st, CU_KEY_ONLY, JS_NewBool(ctx, key_only));
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBCursor slot key could not be interned");
    JS_SetProperty(ctx, c, k, st);
    JS_FreeAtom(ctx, k);
    return c;
}

void idb_cursor_set_request(JSContext *ctx, JSValueConst cursor, JSValueConst request)
{
    DCHECK(idb_cursor_is(cursor), "a cursor's request was set on something that is not a cursor");
    DCHECK(idb_request_is(request), "§4.5's openCursor set a cursor's request to something that is not a "
                                    "request — §5.6's answer is what that step names");
    cu_set(ctx, cursor, CU_REQUEST, JS_DupValue(ctx, request));
}

/* ---- §6.7's ITERATE A CURSOR ------------------------------------------------------------------------------ */

/* What the operation was minted over — §6.7's own parameters. Captured by the closure, because §scheduler's
   rule is that an operation which becomes a work item takes its inputs WITH it: `continue(key)` names the key
   at the moment the member ran, and reading it back off anything at task time would be reading it at the wrong
   TIME. `key` and `primaryKey` are JS_UNDEFINED for "not given", which is the state §6.7 asks about with "if
   key is defined"; `count` is JS_UNDEFINED for the same, and step 8 is what turns that into 1. */
#define IT_CD_CURSOR       0
#define IT_CD_KEY          1
#define IT_CD_PRIMARY_KEY  2
#define IT_CD_COUNT        3

#define ITC_STAGES(X) \
    X(ITC_ENTER,  "Indexed Database §6.7 steps 1-8 — ONE O(1) engine action: source, direction, range, " \
                  "position and object store position are read off the cursor, and an absent count is 1") \
    X(ITC_SCAN,   "Indexed Database §6.7 step 9.1 (let found record be the first — or, for a backward " \
                  "direction, the last — record in records which satisfy all of the requirements): ONE " \
                  "record of the source's list is examined, out of a list of the page's size") \
    X(ITC_UNIQUE, "Indexed Database §6.7 step 9.1's \"prevunique\" tail (if temp record is defined, let found " \
                  "record be the FIRST record in records whose key is equal to temp record's key): ONE record " \
                  "of the source's list is examined") \
    X(ITC_ROUND,  "Indexed Database §6.7 steps 9.2-9.5 — ONE O(1) engine action: an undefined found record " \
                  "clears the cursor and answers null, and otherwise position, object store position and " \
                  "count take their new values and step 9's loop is re-tested") \
    X(ITC_FINISH, "Indexed Database §6.7 steps 10-15 — ONE O(1) engine action: the cursor's position, object " \
                  "store position and key are written, its value is the found record's deserialized value or " \
                  "referenced value, its got value flag is set, and the cursor is the answer")
enum { ITC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const ITC_STEPS[] = { ITC_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;          /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   started;
    JSValue   source;       /* step 1's, held across the scan so it is read once (owned) */
    JSValue   position;     /* step 6's local, which steps 9.3 and 10 make the cursor's (owned) */
    JSValue   os_position;  /* step 7's local, likewise (owned) */
    JSValue   found_key;    /* step 9.1's found record, split in two (owned) */
    JSValue   found_value;  /* owned */
    JSValue   temp_key;     /* "prevunique"'s temp record's key, between the two scans (owned) */
    JSValue   result;       /* step 9.2's null, or step 15's cursor (owned) */
    double    count;        /* step 8's count, which step 9.5 decreases */
    uint32_t  i;            /* how many records this scan has examined */
    uint32_t  n;            /* how long step 4's list was when the scan began */
    uint8_t   dir;
    uint8_t   is_index;
} JSIdbIterState;

static void js_idb_iter_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSIdbIterState *s = st;

    v->val(ctx, &s->source);
    v->val(ctx, &s->position);
    v->val(ctx, &s->os_position);
    v->val(ctx, &s->found_key);
    v->val(ctx, &s->found_value);
    v->val(ctx, &s->temp_key);
    v->val(ctx, &s->result);
}

static JSValue js_idb_iter_fini(JSContext *ctx, void *st, bool take_result)
{
    JSIdbIterState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;

    (void)ctx;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* A STEP STATE IS js_mallocz'd AND A ZEROED JSValue IS THE INTEGER 0, so every value slot is placed before
   anything can read one and `started` is the byte that says whether it has been — the question a machine can
   never answer from its stage. It runs ABOVE the dispatch, where code that legitimately runs on every entry
   belongs. */
static void iter_state_start(JSIdbIterState *s)
{
    if (s->started)
        return;
    s->started = 1;
    s->source = JS_UNDEFINED;
    s->position = JS_UNDEFINED;
    s->os_position = JS_UNDEFINED;
    s->found_key = JS_UNDEFINED;
    s->found_value = JS_UNDEFINED;
    s->temp_key = JS_UNDEFINED;
    s->result = JS_UNDEFINED;
}

/* §6.7 STEP 9.1'S REQUIREMENT LIST, for ONE record and ONE direction — the whole of the switch, written once so
 * the four arms cannot disagree about the three requirements they share.
 *
 * The arms differ in exactly two places and the standard states both: whether the KEY and POSITION comparisons
 * are ">"-shaped or "<"-shaped (which is `fwd`, the same bit that decides which end the scan starts from), and
 * whether the INDEX's secondary ordering on the record's value participates. It does for "next" and "prev" —
 * where a record with the same key but a further-along value is still ahead of the cursor — and it does NOT for
 * "nextunique" and "prevunique", whose whole purpose is to skip the rest of a key's records. That is also why
 * §6.7 step 3 asserts primaryKey is only ever given with "next" or "prev".
 *
 * `unique` selects the second pair, `fwd` the direction. Every operand is BORROWED. */
static bool iter_satisfies(JSContext *ctx, bool fwd, bool unique, bool is_index, JSValueConst range,
                           JSValueConst key, JSValueConst primary_key, JSValueConst position,
                           JSValueConst os_position, JSValueConst rkey, JSValueConst rvalue)
{
    int sign = fwd ? 1 : -1, c;

    if (!JS_IsUndefined(key)) {
        c = sign * idb_key_compare(ctx, rkey, key);
        if (!unique && !JS_IsUndefined(primary_key)) {
            /* "The record's key is equal to key and the record's value is greater than or equal to primaryKey,
               [or] the record's key is greater than key." */
            if (c < 0) return false;
            if (c == 0 && sign * idb_key_compare(ctx, rvalue, primary_key) < 0) return false;
        } else if (c < 0) {
            /* "The record's key is greater than or equal to key." */
            return false;
        }
    }
    if (!JS_IsUndefined(position)) {
        c = sign * idb_key_compare(ctx, rkey, position);
        if (!unique && is_index) {
            /* "The record's key is equal to position and the record's value is greater than object store
               position, [or] the record's key is greater than position." */
            if (c < 0) return false;
            if (c == 0 && sign * idb_key_compare(ctx, rvalue, os_position) <= 0) return false;
        } else if (c <= 0) {
            /* "The record's key is greater than position." — and for "nextunique"/"prevunique" this is the
               requirement even when the source is an index, which is what makes them skip a key's other
               records. */
            return false;
        }
    }
    /* "The record's key is in range." */
    return idb_key_range_contains(ctx, range, rkey);
}

static int js_idb_iterate_operation(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb,
                                    int *out_argc)
{
    JSIdbIterState *s = st;
    JSValueConst cursor = JS_StepClosureData(&s->hdr, IT_CD_CURSOR);
    JSValueConst key = JS_StepClosureData(&s->hdr, IT_CD_KEY);
    JSValueConst primary_key = JS_StepClosureData(&s->hdr, IT_CD_PRIMARY_KEY);
    bool fwd, unique;

    DCHECK(idb_cursor_is(cursor), "§6.7's iterate a cursor was minted over something that is not a cursor");
    iter_state_start(s);
    JS_FreeValue(ctx, cb_result);
    STEP_DISPATCH(ITC_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(ITC_ENTER);
    {
        JSValueConst count = JS_StepClosureData(&s->hdr, IT_CD_COUNT);

        s->source = cu_source(ctx, cursor);                                    /* STEP 1 */
        s->dir = (uint8_t)cu_direction(ctx, cursor);                           /* STEP 2 */
        s->is_index = cu_source_is_index(ctx, cursor) ? 1 : 0;
        /* STEP 3: "Assert: if primaryKey is given, source is an index and direction is next or prev." §4.9's
           `continuePrimaryKey` reports both halves itself — an "InvalidAccessError" for a non-index source and
           for a unique direction — so an operation that reached here violating either is that member not
           having run its own steps. */
        DCHECK(JS_IsUndefined(primary_key) ||
               (s->is_index && (s->dir == IDB_CURSOR_DIR_NEXT || s->dir == IDB_CURSOR_DIR_PREV)),
               "§6.7 step 3: a cursor iteration was given a primaryKey while its source is not an index, or "
               "while its direction is one of the two unique ones. §4.9's continuePrimaryKey steps 4 and 5 "
               "are the two InvalidAccessErrors that make that unreachable");
        s->position = cu_get(ctx, cursor, CU_POSITION);                        /* STEP 6 */
        s->os_position = cu_get(ctx, cursor, CU_OS_POSITION);                  /* STEP 7 */
        DCHECK(s->is_index || JS_IsUndefined(s->os_position),
               "a cursor over an OBJECT STORE holds an object store position — §2.10 gives that field to an "
               "index source alone, and §6.7 step 9.1's object-store arm never reads it");
        /* THE TWO POSITIONS OF AN INDEX CURSOR ARE WRITTEN TOGETHER (steps 10 and 11) and there is exactly one
           state in which they disagree: step 9.2 clears the object store position and leaves the position, for
           a cursor that has just run off the end of its range. That cursor's got value flag is false, and
           §4.9's three iterating members each refuse on it — so an iteration entered in that state is one of
           those refusals not having run, and step 9.1's index requirement would then compare against nothing. */
        DCHECK(!s->is_index || JS_IsUndefined(s->position) == JS_IsUndefined(s->os_position),
               "§6.7 was entered on an index cursor holding a position but no object store position — that is "
               "the state step 9.2 leaves an EXHAUSTED cursor in, and §4.9's got-value refusal is what makes "
               "it unreachable");
        /* STEP 8: "If count is not given, let count be 1."
           THE COUNT IS ALWAYS A NUMBER HERE, AND THAT IS A FACT ABOUT ITS ONE PRODUCER rather than a hope:
           §4.9's advance is the only member that mints this operation with a count, and it resolves the
           page's operand through idl_number_of before doing so — `continue` and `continuePrimaryKey` both
           pass undefined. Asserting it is what keeps the coercion below off the page's own value: a producer
           that ever hands this a raw argument would run ToPrimitive from an activation with no flow base,
           and the abort would land in the coercion instead of at the member that passed it. */
        s->count = 1;
        if (!JS_IsUndefined(count)) {
            double d;

            DCHECK(JS_IsNumber(count),
                   "§6.7 was minted with a count that is not a Number — §4.9's advance is its only source and "
                   "resolves it through idl_number_of, so a value here means a second producer appeared that "
                   "hands this algorithm the page's own operand");
            CHECK(JS_ToFloat64(ctx, &d, count) == 0, "IndexedDB: §6.7's count could not be read");
            DCHECK(d >= 1 && d == floor(d),
                   "§6.7 was given a count that is not a positive integer. §4.9's advance step 1 throws a "
                   "TypeError for 0 and its [EnforceRange] unsigned long is what refuses the rest, so this "
                   "operation was minted by something that did not run those steps");
            s->count = d;
        }
        s->i = 0;
        s->n = cu_record_count(ctx, cursor, s->source);                        /* STEP 4 */
        STEP_GOTO(s->hdr.stage, ITC_SCAN, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(ITC_SCAN);
    {
        JSValue rkey, rvalue, range;
        uint32_t at;
        bool hit;

        fwd = (s->dir == IDB_CURSOR_DIR_NEXT || s->dir == IDB_CURSOR_DIR_NEXTUNIQUE);
        unique = (s->dir == IDB_CURSOR_DIR_NEXTUNIQUE || s->dir == IDB_CURSOR_DIR_PREVUNIQUE);
        /* §2.7.2 gives one object store to one read/write transaction at a time and an operation is ONE task,
           so nothing may add to or remove from this list across the scan — a record appearing behind the
           cursor is a record this iteration would step straight past. */
        DCHECK(s->n == cu_record_count(ctx, cursor, s->source),
               "the source's list of records CHANGED while §6.7's step 9.1 scanned it — step 4 names the list "
               "once, before step 9's loop, and §2.7.2 is what makes that name stable");
        if (s->i >= s->n) {
            /* Nothing satisfied the requirements. For "prevunique" that is also the end of the FIRST scan with
               no temp record, which is the same answer. */
            STEP_GOTO(s->hdr.stage, ITC_ROUND, NULL);
            return JS_STEP_YIELD;
        }
        /* "the FIRST record ... which satisfy" for the two forward directions, and "the LAST record" for the
           two backward ones — so a backward scan starts at the end and its first hit IS the list's last. */
        at = fwd ? s->i : s->n - 1 - s->i;
        s->i++;
        cu_record_at(ctx, cursor, s->source, at, &rkey, &rvalue);
        range = cu_get(ctx, cursor, CU_RANGE);
        hit = iter_satisfies(ctx, fwd, unique, s->is_index != 0, range, key, primary_key, s->position,
                             s->os_position, rkey, rvalue);
        JS_FreeValue(ctx, range);
        if (!hit) {
            JS_FreeValue(ctx, rkey);
            JS_FreeValue(ctx, rvalue);
            return JS_STEP_YIELD;   /* the same stage, one record further along */
        }
        if (s->dir == IDB_CURSOR_DIR_PREVUNIQUE) {
            /* "If TEMP RECORD is defined, let found record be the FIRST record in records whose key is equal
               to temp record's key." The note beside it is what this second scan buys: "iterating with
               prevunique visits the same records that nextunique visits, but in reverse order" — so the
               record yielded for a duplicated key is the one nextunique would have yielded, not the last. */
            JS_FreeValue(ctx, s->temp_key);
            s->temp_key = rkey;
            JS_FreeValue(ctx, rvalue);
            s->i = 0;
            STEP_GOTO(s->hdr.stage, ITC_UNIQUE, NULL);
            return JS_STEP_YIELD;
        }
        JS_FreeValue(ctx, s->found_key);
        JS_FreeValue(ctx, s->found_value);
        s->found_key = rkey;
        s->found_value = rvalue;
        STEP_GOTO(s->hdr.stage, ITC_ROUND, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(ITC_UNIQUE);
    {
        JSValue rkey, rvalue;

        DCHECK(s->dir == IDB_CURSOR_DIR_PREVUNIQUE,
               "§6.7's prevunique tail was entered for another direction — it is the only arm of step 9.1 "
               "that searches twice");
        DCHECK(!JS_IsUndefined(s->temp_key), "§6.7's prevunique tail was entered with no temp record — the "
                                             "step is conditional on one having been found");
        if (s->i >= s->n) {
            DFAIL("§6.7's prevunique tail scanned the whole list without finding a record whose key equals "
                  "the TEMP RECORD's key — the temp record is itself such a record, so this can only mean "
                  "the list changed under the scan or that idb_key_compare is not reflexive for this key");
            return JS_STEP_ABRUPT;
        }
        cu_record_at(ctx, cursor, s->source, s->i, &rkey, &rvalue);
        s->i++;
        if (idb_key_compare(ctx, rkey, s->temp_key) != 0) {
            JS_FreeValue(ctx, rkey);
            JS_FreeValue(ctx, rvalue);
            return JS_STEP_YIELD;
        }
        JS_FreeValue(ctx, s->found_key);
        JS_FreeValue(ctx, s->found_value);
        s->found_key = rkey;
        s->found_value = rvalue;
        STEP_GOTO(s->hdr.stage, ITC_ROUND, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(ITC_ROUND);
        if (JS_IsUndefined(s->found_key)) {
            /* STEP 9.2. The cursor's POSITION IS NOT WRITTEN — the standard's steps 10 and 11 are outside the
               loop and this arm returns from inside it — so a cursor that ran off the end of its range keeps
               the position of the last record it did yield. That is what makes a `continue(k)` after an
               exhausted iteration compare against the right key rather than against nothing, and §4.9's own
               got-value refusal is what stops the page reaching it anyway. */
            cu_set(ctx, cursor, CU_KEY, JS_UNDEFINED);                            /* STEP 9.2.1 */
            if (s->is_index)
                cu_set(ctx, cursor, CU_OS_POSITION, JS_UNDEFINED);                /* STEP 9.2.2 */
            if (!cu_flag(ctx, cursor, CU_KEY_ONLY))
                cu_set(ctx, cursor, CU_VALUE, JS_UNDEFINED);                      /* STEP 9.2.3 */
            s->result = JS_NULL;                                                  /* STEP 9.2.4 */
            return JS_STEP_DONE;
        }
        JS_FreeValue(ctx, s->position);
        s->position = JS_DupValue(ctx, s->found_key);                             /* STEP 9.3 */
        if (s->is_index) {
            JS_FreeValue(ctx, s->os_position);
            s->os_position = JS_DupValue(ctx, s->found_value);                    /* STEP 9.4 */
        }
        s->count -= 1;                                                            /* STEP 9.5 */
        if (s->count > 0) {
            /* Step 9's loop, re-tested. `key` and `primaryKey` STAY IN EFFECT for every turn of it, which is
               the standard's own shape: they are read at step 9.1 and nothing clears them. */
            JS_FreeValue(ctx, s->found_key);
            JS_FreeValue(ctx, s->found_value);
            JS_FreeValue(ctx, s->temp_key);
            s->found_key = JS_UNDEFINED;
            s->found_value = JS_UNDEFINED;
            s->temp_key = JS_UNDEFINED;
            s->i = 0;
            STEP_GOTO(s->hdr.stage, ITC_SCAN, NULL);
            return JS_STEP_YIELD;
        }
        STEP_GOTO(s->hdr.stage, ITC_FINISH, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(ITC_FINISH);
        cu_set(ctx, cursor, CU_POSITION, JS_DupValue(ctx, s->position));           /* STEP 10 */
        if (s->is_index)
            cu_set(ctx, cursor, CU_OS_POSITION, JS_DupValue(ctx, s->os_position)); /* STEP 11 */
        cu_set(ctx, cursor, CU_KEY, JS_DupValue(ctx, s->found_key));               /* STEP 12 */
        if (!cu_flag(ctx, cursor, CU_KEY_ONLY)) {                                  /* STEP 13 */
            JSValue v;

            if (s->is_index) {
                /* "or found record's REFERENCED VALUE otherwise" — §2.6's referenced value is the value of the
                   record in the index's referenced object store whose key is this record's value, and §6.2's
                   retrieval over a range containing only that key IS that lookup. It deserializes on the way
                   out, which is step 13.2's StructuredDeserialize. */
                JSValue store = cu_effective_store(ctx, cursor);
                JSValue range = idb_key_range_only_key(ctx, s->found_value);

                v = idb_retrieve_value(ctx, store, range);
                JS_FreeValue(ctx, range);
                JS_FreeValue(ctx, store);
                DCHECK(!JS_IsUndefined(v),
                       "an index record names a primary key its referenced object store holds no record for. "
                       "§6.4's removal takes an index's records with the store's, so the two lists agree by "
                       "construction and this is that invariant broken");
            } else {
                /* STEP 13.2 over the record's own value. A FRESH copy every time, for §2.3's reason: the page
                   may mutate what `cursor.value` handed it and must not reach the record. */
                v = structured_clone(ctx, s->found_value);
                DCHECK(!JS_IsException(v), "§6.7's deserialization refused a value §6.1 had already copied — "
                                           "the two are the same operation over the same data");
            }
            cu_set(ctx, cursor, CU_VALUE, v);
        }
        cu_set(ctx, cursor, CU_GOT_VALUE, JS_TRUE);                                /* STEP 14 */
        s->result = JS_DupValue(ctx, cursor);                                      /* STEP 15 */
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_iterate_def = {
    sizeof(JSIdbIterState), js_idb_iterate_operation, js_idb_iter_fini, 0, .visit = js_idb_iter_visit,
    .algorithm = "Indexed Database §6.7's iterate a cursor",
    .steps = ITC_STEPS
};

/* §6.7 CLOSED OVER ITS FOUR PARAMETERS. `key`, `primary_key` and `count` are JS_UNDEFINED for the ones the
   caller's step does not name, which is the state §6.7's own "if … is given/defined" tests are written over. */
static JSValue iter_operation(JSContext *ctx, JSValueConst cursor, JSValueConst key, JSValueConst primary_key,
                              JSValueConst count)
{
    JSValueConst data[4];
    JSValue op;

    DCHECK(g_iterate_stepid >= 0, "§6.7's operation was minted before idb_cursor_init declared its machine");
    data[IT_CD_CURSOR] = cursor;
    data[IT_CD_KEY] = key;
    data[IT_CD_PRIMARY_KEY] = primary_key;
    data[IT_CD_COUNT] = count;
    op = JS_NewStepClosure(ctx, g_iterate_stepid, 0, 4, data);
    CHECK(!JS_IsException(op), "IndexedDB: §6.7's cursor iteration operation could not be minted");
    return op;
}

JSValue idb_cursor_iterate_operation(JSContext *ctx, JSValueConst cursor)
{
    return iter_operation(ctx, cursor, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED);
}

/* ---- the refusals §4.9's iterating members share ---------------------------------------------------------
 *
 * §4.9 states them once per member and states them IDENTICALLY, in this order: "if transaction's state is not
 * active, then throw a TransactionInactiveError"; [for `update` and `delete`, a read-only transaction is a
 * ReadOnlyError]; "if this's source or effective object store has been DELETED, throw an InvalidStateError";
 * "if this's GOT VALUE FLAG is false, indicating that the cursor is being iterated or has iterated past its
 * end, throw an InvalidStateError".
 *
 * THE GOT-VALUE REFUSAL IS THE ONE THE PAGE ACTUALLY MEETS, and §4.9's own note says why: calling `continue()`
 * twice from one `onsuccess` handler is an "InvalidStateError" on the second call, because the first cleared
 * the flag and only the request's next success sets it again. Returns -1 with the throw live; `*ptx` is OWNED. */
static int cu_check(JSContext *ctx, JSValueConst c, bool writes, JSValue *ptx)
{
    JSValue tx = cu_get(ctx, c, CU_TRANSACTION), src, store;
    bool deleted;

    DCHECK(idb_transaction_is(tx), "a cursor carried no transaction — §2.10 gives every cursor the one its "
                                   "source handle had when it was created");
    if (idb_transaction_state(ctx, tx) != IDB_TX_ACTIVE) {
        JS_FreeValue(ctx, tx);
        JS_ThrowDOMException(ctx, "TransactionInactiveError",
                             "the transaction the cursor belongs to is not active");
        return -1;
    }
    /* §4.9's `update` step 3 and `delete` step 3, which the three ITERATING members do not have — §4.9 puts
       the two groups under different headings for exactly that reason ("the following methods throw a
       ReadOnlyError if called within a read-only transaction"). It sits between the active check and the
       deletion check because that is where the standard states it, and a page can tell: `cursor.update(v)` on
       a read-only transaction whose store was also deleted reports the ReadOnlyError. */
    if (writes && idb_transaction_mode(ctx, tx) == IDB_TX_READONLY) {
        JS_FreeValue(ctx, tx);
        JS_ThrowDOMException(ctx, "ReadOnlyError", "the transaction is a read-only transaction");
        return -1;
    }
    src = cu_source(ctx, c);
    store = cu_effective_store(ctx, c);
    /* "this's SOURCE or EFFECTIVE OBJECT STORE" — two questions, because §4.4's deleteObjectStore destroys the
       store and leaves the indexes referencing it alone, so an index cursor can outlive its store's deletion
       with its own source's deleted flag still false. */
    deleted = (cu_source_is_index(ctx, c) ? idb_index_is_deleted(ctx, src)
                                          : idb_object_store_is_deleted(ctx, src)) ||
              idb_object_store_is_deleted(ctx, store);
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, src);
    if (deleted) {
        JS_FreeValue(ctx, tx);
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "the cursor's source, or the object store it reads, has been deleted");
        return -1;
    }
    if (!cu_flag(ctx, c, CU_GOT_VALUE)) {
        JS_FreeValue(ctx, tx);
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "the cursor is being iterated or has iterated past its end");
        return -1;
    }
    *ptx = tx;
    return 0;
}

/* THE SIX LINES EVERY ITERATING MEMBER ENDS IN — "set this's got value flag to false; let request be this's
   request; set request's processed flag to false; set request's done flag to false; let operation be an
   algorithm to run iterate a cursor …; run asynchronously execute a request with this's source handle,
   operation, AND REQUEST". The last operand is what makes a cursor a request that comes back rather than a
   second request per turn: §4.9's `request` attribute answers the SAME IDBRequest for the whole iteration, and
   a page compares it. Every operand is BORROWED; the request is not returned, because none of these three
   members returns one. */
static void cu_place(JSContext *ctx, JSValueConst c, JSValueConst tx, JSValueConst key,
                     JSValueConst primary_key, JSValueConst count)
{
    JSValue req = cu_get(ctx, c, CU_REQUEST), handle, op, again;

    DCHECK(idb_request_is(req), "a cursor was iterated with no request — §4.5's openCursor sets one before it "
                                "returns, and §4.9's three iterating members re-fire exactly that one");
    cu_set(ctx, c, CU_GOT_VALUE, JS_FALSE);
    idb_request_set_processed(ctx, req, false);
    idb_request_set_done(ctx, req, false);
    op = iter_operation(ctx, c, key, primary_key, count);
    handle = cu_get(ctx, c, CU_SOURCE_HANDLE);
    again = idb_request_execute_into(ctx, handle, tx, op, req);
    DCHECK(JS_VALUE_GET_PTR(again) == JS_VALUE_GET_PTR(req),
           "§5.6 answered with a request other than the one it was given — its step 3 takes an existing "
           "request precisely so a cursor's stays the object the page is holding");
    JS_FreeValue(ctx, again);
    JS_FreeValue(ctx, handle);
    JS_FreeValue(ctx, op);
    JS_FreeValue(ctx, req);
}

/* ---- §4.9's ADVANCE ---------------------------------------------------------------------------------------
 *
 * It runs NONE of the page's code — its one argument is an `[EnforceRange] unsigned long` the IDL has already
 * converted (and a page `valueOf` runs inside THAT conversion, which is the args machine's own request) — so
 * the body is an ordinary C function and not a machine, unlike `continue`, whose key reaches §7.4's array arm.
 *
 * WHAT IT MAY NOT DO IS READ THAT ARGUMENT BACK WITH A COERCION, and this body did. `JS_ToUint32(ctx, &count,
 * argv[0])` is the read core/idl_args.h bans by name: idl_concolic_rule answers IDL_CONCOLIC_CROSSES for
 * IDL_UNSIGNED_LONG_ENFORCE, so unknown external input reaches here UNCONVERTED — that crossing is what makes
 * opacity survive §3.2's conversion and is the whole reason a later branch on the value still forks. A concolic
 * is an Object, so ToNumber on it reaches ECMAScript §7.1.1 ToPrimitive and runs the page's own valueOf from a
 * plain C activation with no flow base under it, which this engine aborts on INSIDE THE COERCION rather than
 * here at the member. `cursor.advance(new URLSearchParams(location.search).get("page") * 10)` is an ordinary
 * page's pagination and it ended the document.
 * AND THE ASSERT AROUND IT WAS SPEC-WRONG IN THE WORST DIRECTION — it was a release-fatal CHECK saying the
 * count "its [EnforceRange] unsigned long did not convert", which instructs the next reader to make the
 * declaration convert an unknown. Following that instruction deletes the crossing, and with it every fork the
 * value would have driven.
 * `idl_number_of` is the one answer: the converted Number for a real value, and for an unknown the SAME
 * §3.2.4.9 Abstract operations conversion run on that value's own EXAMPLE — so a count a fetched config
 * computed keeps this flow running on the number the app actually produced, while the value itself stays
 * unknown at its source. It is what §4.5's three sibling counts already read (core/indexeddb/idb_get_all.c,
 * idb_object_store.c, idb_index_handle.c), and this was the one member of that family still coercing. */
static JSValue js_cu_advance(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue tx, count_v;
    double count = 0;

    (void)argc; (void)magic;
    if (!cu_brand(ctx, this_val)) return JS_EXCEPTION;
    /* NO EXAMPLE MEANS NO NUMBER, and §4.9's `count` has no "not given" arm for one to fall back to — §4.5's
       counts do (§6.2 step 1's absent count is infinity, the superset a later example can only narrow), and
       that is exactly why the same absence is answered differently here. Advancing by any invented number is
       the fabrication §RUN-DON'T-MATCH forbids and returning early is the dropped flow §Time-travel's razor
       calls a cap, so there is nothing correct left to do and this crashes at the member that has to fork. */
    if (!idl_number_of(ctx, IDL_UNSIGNED_LONG_ENFORCE, argv[0], &count))
        DFAIL("IndexedDB §4.9's advance was given an UNKNOWN count carrying no example. Its step 1 "
              "(\"If count is 0 (zero), throw a TypeError\") has both completions feasible over an unconstrained "
              "[EnforceRange] unsigned long, and §6.7's step 9 then has one distinct outcome per skip — so "
              "this member must become a step machine (idl_method_id_step) and ask step_fork_run over step 1, "
              "exactly as core/timing/timer.c asks it over HTML §8.7 Timers' step 4, then fork §6.7 step 9 "
              "over the FINITE arm set {1 … n, at-or-past-n} that the cursor's own record count bounds");
    /* IN RELEASE THAT DFAIL IS GONE AND `count` IS STILL THE 0 IT WAS INITIALISED TO, which falls into step 1's
       TypeError two lines down. That is a DECISION and not a fall-through: release cannot build the fork, so
       the member refuses a count it cannot read, through the standard's own refusal for this argument, rather
       than advancing by a number nobody computed. idl_number_of leaves `*out` untouched when it answers 0, so
       there is nothing here that a compiled-out assert made undefined. */
    /* §3.3.6 [EnforceRange]'s arm of §3.2.4.9 Abstract operations' ConvertToInt REFUSES a non-finite value and
       one outside the type's range, at the declaration, before this body is entered — so a value here it would
       have refused means this position lost its declared type. */
    DCHECK(count >= 0 && count <= 4294967295.0 && count == (double)(uint32_t)count,
           "§4.9's advance reached its body with a count outside an unsigned long's range — ADVANCE_ARGS "
           "declares IDL_UNSIGNED_LONG_ENFORCE, whose §3.3.6 [EnforceRange] conversion refuses such a value "
           "before any body runs");
    /* "If count is 0 (zero), throw a TypeError." Step 1, ahead of every other refusal — so `advance(0)` on a
       cursor whose transaction has finished is a TypeError and not a TransactionInactiveError.
       RESIDUAL — this step is DECIDED and not FORKED for an unknown that carries an example, which is correct
       for the world that example names and narrower than the standard.
         NOT COVERED: an unknown count whose example is 5 explores only the advance-by-5 world; step 1's
           TypeError arm, and §6.7 step 9's other skip distances, are never entered for that value.
         THE NEXT DIFF: the machine the DFAIL above names — this member declared idl_method_id_step, step 1
           asked through step_fork_run with the example marking the primary arm (core/timing/timer.c's shape),
           and §6.7 step 9 forked over the record count's finite arm set.
         HOW ITS ABSENCE SHOWS: one `advance(x)` on an unknown produces exactly ONE request completion, so a
           store whose later records are reachable only past a different skip is never read and its cursor's
           `success` handler runs once per call rather than once per feasible count. */
    if (count == 0)
        return JS_ThrowTypeError(ctx, "the cursor cannot be advanced by 0 records");
    if (cu_check(ctx, this_val, /*writes*/ false, &tx) < 0) return JS_EXCEPTION;               /* STEPS 2-5 */
    /* THE NUMBER AND NOT THE VALUE crosses into §6.7, which is what makes that algorithm's own step 8 read a
       Number rather than repeating this coercion on the page's operand. This member is §6.7's ONLY source of a
       count — `continue` and `continuePrimaryKey` both mint the operation with an undefined one — so resolving
       it here is what makes step 8's read total. */
    count_v = JS_NewFloat64(ctx, count);
    cu_place(ctx, this_val, tx, JS_UNDEFINED, JS_UNDEFINED, count_v);       /* STEPS 6-11 */
    JS_FreeValue(ctx, count_v);
    JS_FreeValue(ctx, tx);
    return JS_UNDEFINED;
}

/* ---- §4.9's CONTINUE and CONTINUEPRIMARYKEY ---------------------------------------------------------------
 *
 * BOTH ARE MACHINES, AND §7.4 IS WHY. §4.9's "Let r be the result of converting a value to a key with key" over
 * an Array exotic object runs the PAGE'S OWN CODE — one `? HasOwnProperty` and one `? Get` per element, over a
 * structure the page sized and nested — so the conversion exists in exactly one form, the parkable walk
 * (core/indexeddb/idb_key_array.h), and these members drive it exactly as §4.5's `get` drives it.
 *
 * `continuePrimaryKey` drives it TWICE, over two arguments, and the two conversions are sequential rather than
 * nested: §7.4 releases a walk record already used at its own start, which is what §4.7's `bound` relies on, so
 * one record serves both in turn. */
typedef struct {
    IdbKeyWalk w;
    JSValue    tx;
    JSValue    key;   /* the first conversion's answer, held across the second (owned) */
} IdbCursorContinueState;

static void js_cu_continue_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    IdbCursorContinueState *s = st;

    idb_key_walk_visit(ctx, &s->w, v);
    v->val(ctx, &s->tx);
    v->val(ctx, &s->key);
}

#define CUC_STAGES(X) \
    X(CUC_BEGIN, "Indexed Database §4.9 continue steps 1-4 — ONE O(1) engine action: an inactive transaction " \
                 "is a TransactionInactiveError, a deleted source or effective object store is an " \
                 "InvalidStateError, and a false got value flag is an InvalidStateError — and step 5.1's " \
                 "conversion of key begins, or steps 6-11 run directly when no key was given") \
    IDB_KEY_ARRAY_ALGO_STAGES(X, CUC_K, "Indexed Database §4.9 continue step 5.1 (let r be the result of " \
                                        "converting a value to a key with key)") \
    X(CUC_PLACE, "Indexed Database §4.9 continue steps 5.2-11 — ONE O(1) engine action: an invalid r is a " \
                 "DataError, a key on the wrong side of this's position for this's direction is a DataError, " \
                 "the got value flag clears, the request's two flags clear, and the iteration is placed " \
                 "against the SAME request")
enum { IDL_STEP_STAGE_BASE(CUC_STAGES) CUC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CUC_STEPS[] = { CUC_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_cu_continue(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbCursorContinueState *s = st;

    STEP_DISPATCH(CUC_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(CUC_BEGIN);
        JS_FreeValue(ctx, cb_result);
        s->tx = JS_UNDEFINED;
        s->key = JS_UNDEFINED;
        if (!cu_brand(ctx, hdr->this_val)) return JS_STEP_ABRUPT;
        if (cu_check(ctx, hdr->this_val, /*writes*/ false, &s->tx) < 0) return JS_STEP_ABRUPT;   /* STEPS 1-4 */
        /* "If key is GIVEN, then:" — an absent argument is not a key to convert, and §6.7's "if key is
           defined" is the same absence read from the other side. */
        if (argc < 1 || JS_IsUndefined(argv[0])) {
            STEP_GOTO(hdr->stage, CUC_PLACE, &hdr->get_phase, &hdr->desc_phase, NULL);
            return JS_STEP_YIELD;
        }
        idb_key_walk_start(ctx, hdr, &s->w, argv[0], CUC_K_LENGTH, CUC_PLACE);   /* STEP 5.1 */
        return JS_STEP_YIELD;

    /* STEP 5.1's conversion, whose six rest points are this member's. Named individually for the reason
       core/dom/node.c names `clone a node`'s: a stage added to the algorithm does not compile until it has an
       arm here, where a partial list would silently route it into a neighbour. */
    STEP_ARM(CUC_K_LENGTH);
    STEP_ARM(CUC_K_BEGIN);
    STEP_ARM(CUC_K_HOP);
    STEP_ARM(CUC_K_ENTRY);
    STEP_ARM(CUC_K_SUBKEY);
    STEP_ARM(CUC_K_LEAVE);
        return idb_key_walk_run(ctx, hdr, &s->w, cb_result, CUC_K_LENGTH, out_cb, out_argc);

    STEP_ARM(CUC_PLACE);
    {
        JSValue position;
        int dir, c;

        JS_FreeValue(ctx, cb_result);
        /* THE SAME QUESTION STEP 5's CONDITION ASKED, asked of the same argument — the member's arguments are
           on the header for the length of the call, so "was a key given" needs no byte of state and cannot
           come to disagree with the branch that started the conversion. */
        if (argc >= 1 && !JS_IsUndefined(argv[0])) {
            if (idb_key_walk_take(ctx, &s->w, &s->key) < 0) return JS_STEP_ABRUPT;   /* STEPS 5.2-5.3 */
            /* "If key is less than or equal to this's POSITION and this's direction is next or nextunique,
               then throw a DataError" — and the mirror for the two backward directions. It is the refusal
               that makes `continue(k)` monotone, so a page cannot walk a cursor backwards through a forward
               iteration and see records twice. */
            dir = cu_direction(ctx, hdr->this_val);
            position = cu_get(ctx, hdr->this_val, CU_POSITION);
            if (!JS_IsUndefined(position)) {
                c = idb_key_compare(ctx, s->key, position);
                if ((dir == IDB_CURSOR_DIR_NEXT || dir == IDB_CURSOR_DIR_NEXTUNIQUE) ? (c <= 0) : (c >= 0)) {
                    JS_FreeValue(ctx, position);
                    JS_ThrowDOMException(ctx, "DataError",
                                         "the key is not ahead of the cursor's position in its direction");
                    return JS_STEP_ABRUPT;
                }
            }
            JS_FreeValue(ctx, position);
        }
        cu_place(ctx, hdr->this_val, s->tx, s->key, JS_UNDEFINED, JS_UNDEFINED);   /* STEPS 6-11 */
        *presult = JS_UNDEFINED;
        return JS_STEP_DONE;
    }
}

static const IdlStepDecl CONTINUE_STEP = {
    js_cu_continue, sizeof(IdbCursorContinueState), js_cu_continue_visit, NULL,
    "Indexed Database §4.9 IDBCursor.continue", CUC_STEPS
};

#define CUP_STAGES(X) \
    X(CUP_BEGIN, "Indexed Database §4.9 continuePrimaryKey steps 1-6 — ONE O(1) engine action: an inactive " \
                 "transaction is a TransactionInactiveError, a deleted source or effective object store is " \
                 "an InvalidStateError, a non-index source is an InvalidAccessError, a unique direction is " \
                 "an InvalidAccessError, and a false got value flag is an InvalidStateError — and step 7's " \
                 "conversion of key begins") \
    IDB_KEY_ARRAY_ALGO_STAGES(X, CUP_K, "Indexed Database §4.9 continuePrimaryKey step 7 (let r be the " \
                                        "result of converting a value to a key with key)") \
    X(CUP_TOOK_KEY, "Indexed Database §4.9 continuePrimaryKey steps 8-9 — ONE O(1) engine action: an invalid " \
                    "r is a DataError and otherwise key is r — and step 10's conversion of primaryKey begins") \
    IDB_KEY_ARRAY_ALGO_STAGES(X, CUP_P, "Indexed Database §4.9 continuePrimaryKey step 10 (let r be the " \
                                        "result of converting a value to a key with primaryKey)") \
    X(CUP_PLACE, "Indexed Database §4.9 continuePrimaryKey steps 11-19 — ONE O(1) engine action: an invalid " \
                 "r is a DataError, the four comparisons against this's position and object store position " \
                 "are each a DataError, the got value flag clears, the request's two flags clear, and the " \
                 "iteration is placed against the SAME request")
enum { IDL_STEP_STAGE_BASE(CUP_STAGES) CUP_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CUP_STEPS[] = { CUP_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_cu_continue_pk(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbCursorContinueState *s = st;

    (void)argc;
    STEP_DISPATCH(CUP_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(CUP_BEGIN);
        JS_FreeValue(ctx, cb_result);
        s->tx = JS_UNDEFINED;
        s->key = JS_UNDEFINED;
        if (!cu_brand(ctx, hdr->this_val)) return JS_STEP_ABRUPT;
        /* §4.9 ORDERS THESE and the order is observable: steps 1-3 are the shared refusals, steps 4 and 5 are
           this member's own two "InvalidAccessError"s, and step 6 is the got value flag — so a
           continuePrimaryKey on an object store cursor that has also run past its end reports the
           InvalidAccessError. cu_check runs 1-3 AND 6 together, so the two in between are performed here and
           the got-value question is asked again after them, in its own place. */
        {
            JSValue tx = cu_get(ctx, hdr->this_val, CU_TRANSACTION), src, store;
            bool deleted;

            if (idb_transaction_state(ctx, tx) != IDB_TX_ACTIVE) {               /* STEP 2 */
                JS_FreeValue(ctx, tx);
                JS_ThrowDOMException(ctx, "TransactionInactiveError",
                                     "the transaction the cursor belongs to is not active");
                return JS_STEP_ABRUPT;
            }
            src = cu_source(ctx, hdr->this_val);
            store = cu_effective_store(ctx, hdr->this_val);
            deleted = (cu_source_is_index(ctx, hdr->this_val) ? idb_index_is_deleted(ctx, src)
                                                              : idb_object_store_is_deleted(ctx, src)) ||
                      idb_object_store_is_deleted(ctx, store);
            JS_FreeValue(ctx, store);
            JS_FreeValue(ctx, src);
            if (deleted) {                                                       /* STEP 3 */
                JS_FreeValue(ctx, tx);
                JS_ThrowDOMException(ctx, "InvalidStateError",
                                     "the cursor's source, or the object store it reads, has been deleted");
                return JS_STEP_ABRUPT;
            }
            if (!cu_source_is_index(ctx, hdr->this_val)) {                        /* STEP 4 */
                JS_FreeValue(ctx, tx);
                JS_ThrowDOMException(ctx, "InvalidAccessError",
                                     "continuePrimaryKey is only defined for a cursor over an index");
                return JS_STEP_ABRUPT;
            }
            {
                int dir = cu_direction(ctx, hdr->this_val);

                if (dir != IDB_CURSOR_DIR_NEXT && dir != IDB_CURSOR_DIR_PREV) {                   /* STEP 5 */
                    JS_FreeValue(ctx, tx);
                    JS_ThrowDOMException(ctx, "InvalidAccessError",
                                         "continuePrimaryKey is only defined for a next or prev cursor");
                    return JS_STEP_ABRUPT;
                }
            }
            if (!cu_flag(ctx, hdr->this_val, CU_GOT_VALUE)) {                     /* STEP 6 */
                JS_FreeValue(ctx, tx);
                JS_ThrowDOMException(ctx, "InvalidStateError",
                                     "the cursor is being iterated or has iterated past its end");
                return JS_STEP_ABRUPT;
            }
            s->tx = tx;
        }
        idb_key_walk_start(ctx, hdr, &s->w, argv[0], CUP_K_LENGTH, CUP_TOOK_KEY);   /* STEP 7 */
        return JS_STEP_YIELD;

    STEP_ARM(CUP_K_LENGTH);
    STEP_ARM(CUP_K_BEGIN);
    STEP_ARM(CUP_K_HOP);
    STEP_ARM(CUP_K_ENTRY);
    STEP_ARM(CUP_K_SUBKEY);
    STEP_ARM(CUP_K_LEAVE);
        return idb_key_walk_run(ctx, hdr, &s->w, cb_result, CUP_K_LENGTH, out_cb, out_argc);

    STEP_ARM(CUP_TOOK_KEY);
        JS_FreeValue(ctx, cb_result);
        if (idb_key_walk_take(ctx, &s->w, &s->key) < 0) return JS_STEP_ABRUPT;      /* STEPS 8-9 */
        idb_key_walk_start(ctx, hdr, &s->w, argv[1], CUP_P_LENGTH, CUP_PLACE);      /* STEP 10 */
        return JS_STEP_YIELD;

    STEP_ARM(CUP_P_LENGTH);
    STEP_ARM(CUP_P_BEGIN);
    STEP_ARM(CUP_P_HOP);
    STEP_ARM(CUP_P_ENTRY);
    STEP_ARM(CUP_P_SUBKEY);
    STEP_ARM(CUP_P_LEAVE);
        return idb_key_walk_run(ctx, hdr, &s->w, cb_result, CUP_P_LENGTH, out_cb, out_argc);

    STEP_ARM(CUP_PLACE);
    {
        JSValue primary_key = JS_UNDEFINED, position, os_position;
        int dir = cu_direction(ctx, hdr->this_val), c;
        bool bad = false;

        JS_FreeValue(ctx, cb_result);
        if (idb_key_walk_take(ctx, &s->w, &primary_key) < 0) return JS_STEP_ABRUPT;  /* STEPS 11-12 */
        /* STEPS 13-16, as the standard's four sentences: the key must be strictly ahead of this's position, or
           equal to it with the primary key strictly ahead of this's object store position — which is exactly
           §6.7 step 9.1's index requirement, refused here so the iteration cannot be asked to go backwards. */
        position = cu_get(ctx, hdr->this_val, CU_POSITION);
        os_position = cu_get(ctx, hdr->this_val, CU_OS_POSITION);
        if (!JS_IsUndefined(position)) {
            c = idb_key_compare(ctx, s->key, position);
            if (dir == IDB_CURSOR_DIR_NEXT ? (c < 0) : (c > 0))                              /* STEPS 13-14 */
                bad = true;
            else if (c == 0 && !JS_IsUndefined(os_position)) {                       /* STEPS 15-16 */
                int p = idb_key_compare(ctx, primary_key, os_position);

                if (dir == IDB_CURSOR_DIR_NEXT ? (p <= 0) : (p >= 0))
                    bad = true;
            }
        }
        JS_FreeValue(ctx, os_position);
        JS_FreeValue(ctx, position);
        if (bad) {
            JS_FreeValue(ctx, primary_key);
            JS_ThrowDOMException(ctx, "DataError",
                                 "the key and primary key are not ahead of the cursor in its direction");
            return JS_STEP_ABRUPT;
        }
        cu_place(ctx, hdr->this_val, s->tx, s->key, primary_key, JS_UNDEFINED);      /* STEPS 17-19 */
        JS_FreeValue(ctx, primary_key);
        *presult = JS_UNDEFINED;
        return JS_STEP_DONE;
    }
}

static const IdlStepDecl CONTINUE_PK_STEP = {
    js_cu_continue_pk, sizeof(IdbCursorContinueState), js_cu_continue_visit, NULL,
    "Indexed Database §4.9 IDBCursor.continuePrimaryKey", CUP_STEPS
};

/* ---- §4.9's UPDATE and DELETE ------------------------------------------------------------------------------
 *
 * THE TWO WRITING MEMBERS, and the two things that set them apart from the three above: they refuse a READ-ONLY
 * transaction (cu_check's `writes`) and a KEY ONLY cursor, and they place a NEW request each rather than
 * re-firing the cursor's. That last difference is the standard's own — "return the result (an IDBRequest) of
 * running asynchronously execute a request with THIS and operation" — and both halves of it are observable:
 * the request is returned to the page, and its `source` is the CURSOR rather than the cursor's source handle.
 *
 * THE EFFECTIVE OBJECT STORE AND THE EFFECTIVE KEY ARE WHAT BOTH OPERATE ON, never the cursor's own key: for a
 * cursor over an INDEX the key is the index key and the record to write or remove lives in the referenced
 * object store under the OBJECT STORE POSITION. §2.10 defines both terms for exactly this pair of members.
 *
 * `delete` RUNS NONE OF THE PAGE'S CODE — its operands are the cursor's own already-converted key and the
 * store — so it is an ordinary C function. `update` IS A MACHINE, and §7.1 is why: an in-line-key store makes
 * it extract a key from the clone at the store's key path, whose step 3 is §7.4 and therefore §7.4's array arm,
 * which runs the page's own accessors for a LIST key path or an Array-valued one. */

static JSValue js_cu_delete(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue tx, store, key, range, op, req;

    (void)argc; (void)argv; (void)magic;
    if (!cu_brand(ctx, this_val)) return JS_EXCEPTION;
    if (cu_check(ctx, this_val, /*writes*/ true, &tx) < 0) return JS_EXCEPTION;    /* STEPS 1-5 */
    /* "If this's KEY ONLY FLAG is true, throw an InvalidStateError." An `openKeyCursor` cursor never read the
       record's value, so the standard refuses to let it write one or remove it through this member. */
    if (cu_flag(ctx, this_val, CU_KEY_ONLY)) {                                     /* STEP 6 */
        JS_FreeValue(ctx, tx);
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "a key-only cursor cannot delete the record it is positioned at");
    }
    store = cu_effective_store(ctx, this_val);
    key = cu_effective_key(ctx, this_val);
    DCHECK(!JS_IsUndefined(key), "§4.9's `delete` reached its operation with no effective key — the got value "
                                 "flag is what says the cursor is positioned at a record, and step 5 refuses "
                                 "when it is false");
    /* "an algorithm to run DELETE RECORDS FROM AN OBJECT STORE with this's effective object store and this's
       effective key" — §6.4 takes a RANGE and not a key, so the key becomes §2.9's range containing only it,
       exactly as §6.1 step 3's own removal does. */
    range = idb_key_range_only_key(ctx, key);
    op = idb_object_store_delete_operation(ctx, tx, store, range);                 /* STEP 7 */
    req = idb_request_execute(ctx, this_val, tx, op);                              /* STEP 8 */
    JS_FreeValue(ctx, op);
    JS_FreeValue(ctx, range);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, tx);
    return req;
}

typedef struct {
    IdbKeyWalk       w;
    IdbKeyPathResult kpres;   /* §7.1's extra state — an enum, so the caller holds it */
    JSValue tx, store, key;   /* the transaction, the effective object store and the effective key (owned) */
    JSValue clone;            /* step 8's clone, held across step 9's extraction (owned) */
    JSValue key_path;         /* the effective object store's, JS_NULL for out-of-line keys (owned) */
} IdbCursorUpdateState;

static void js_cu_update_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    IdbCursorUpdateState *s = st;

    idb_key_walk_visit(ctx, &s->w, v);
    v->val(ctx, &s->tx);
    v->val(ctx, &s->store);
    v->val(ctx, &s->key);
    v->val(ctx, &s->clone);
    v->val(ctx, &s->key_path);
}

#define CUU_STAGES(X) \
    X(CUU_BEGIN, "Indexed Database §4.9 update steps 1-8 — ONE O(1) engine action plus §5.11's clone: an " \
                 "inactive transaction is a TransactionInactiveError, a read-only transaction is a " \
                 "ReadOnlyError, a deleted source or effective object store is an InvalidStateError, a false " \
                 "got value flag is an InvalidStateError, a true key only flag is an InvalidStateError, and " \
                 "clone is a clone of value in this member's own Realm during transaction") \
    IDB_KEY_PATH_EXTRACT_ALGO_STAGES(X, CUU_KP, "Indexed Database §4.9 update step 9.1 (let kpk be the " \
                                                "result of extracting a key from a value using a key path " \
                                                "with clone and the key path of this's effective object " \
                                                "store)") \
    X(CUU_OPERATION, "Indexed Database §4.9 update steps 9.2-11 — ONE O(1) engine action: a kpk that is " \
                     "failure, invalid, or not equal to this's effective key is a DataError, the operation " \
                     "is an algorithm to run store a record into an object store with the effective object " \
                     "store, clone, the effective key and false, and the request is the result of " \
                     "asynchronously executing it")
enum { IDL_STEP_STAGE_BASE(CUU_STAGES) CUU_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CUU_STEPS[] = { CUU_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_cu_update(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbCursorUpdateState *s = st;

    (void)argc;
    STEP_DISPATCH(CUU_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(CUU_BEGIN);
        JS_FreeValue(ctx, cb_result);
        s->tx = JS_UNDEFINED;
        s->store = JS_UNDEFINED;
        s->key = JS_UNDEFINED;
        s->clone = JS_UNDEFINED;
        s->key_path = JS_UNDEFINED;
        if (!cu_brand(ctx, hdr->this_val)) return JS_STEP_ABRUPT;
        if (cu_check(ctx, hdr->this_val, /*writes*/ true, &s->tx) < 0)              /* STEPS 1-5 */
            return JS_STEP_ABRUPT;
        if (cu_flag(ctx, hdr->this_val, CU_KEY_ONLY)) {                             /* STEP 6 */
            JS_ThrowDOMException(ctx, "InvalidStateError",
                                 "a key-only cursor cannot update the record it is positioned at");
            return JS_STEP_ABRUPT;
        }
        s->store = cu_effective_store(ctx, hdr->this_val);
        s->key = cu_effective_key(ctx, hdr->this_val);
        DCHECK(!JS_IsUndefined(s->key), "§4.9's `update` reached step 8 with no effective key — step 5's got "
                                        "value refusal is what makes a positioned cursor the only one here");
        /* "Let clone be a clone of value in targetRealm DURING TRANSACTION. Rethrow any exceptions." The
           targetRealm of step 7 is this member's own; the clone is the MEMBER's and not the operation's, so an
           unclonable value is a "DataCloneError" thrown by `update` itself where the page's try/catch sees it
           rather than a request that later fires an `error` event. */
        s->clone = structured_clone(ctx, argv[0]);                                  /* STEP 8 */
        if (JS_IsException(s->clone)) {
            s->clone = JS_UNDEFINED;
            return JS_STEP_ABRUPT;
        }
        /* "If this's effective object store uses IN-LINE KEYS, then:" — §2.2's "with a key path it uses
           in-line keys", so the condition IS whether that field is null. */
        s->key_path = idb_object_store_key_path(ctx, s->store);                     /* STEP 9's condition */
        if (JS_IsNull(s->key_path)) {
            STEP_GOTO(hdr->stage, CUU_OPERATION, &hdr->get_phase, &hdr->desc_phase, NULL);
            return JS_STEP_YIELD;
        }
        /* No multiEntry flag: that flag is an INDEX's, and this is the effective object store's key path. */
        idb_key_path_walk_start(ctx, hdr, &s->w, &s->kpres, s->clone, s->key_path, /*multi_entry*/ false,
                                CUU_KP_LENGTH, CUU_OPERATION);                      /* STEP 9.1 */
        return JS_STEP_YIELD;

    /* STEP 9.1's extraction, whose rest points are §7.4's. Named individually for the reason
       core/dom/node.c names `clone a node`'s: a stage added to the algorithm does not compile until it has an
       arm here, where a partial list would silently route it into a neighbour. */
    STEP_ARM(CUU_KP_LENGTH);
    STEP_ARM(CUU_KP_BEGIN);
    STEP_ARM(CUU_KP_HOP);
    STEP_ARM(CUU_KP_ENTRY);
    STEP_ARM(CUU_KP_SUBKEY);
    STEP_ARM(CUU_KP_LEAVE);
        return idb_key_walk_run(ctx, hdr, &s->w, cb_result, CUU_KP_LENGTH, out_cb, out_argc);

    STEP_ARM(CUU_OPERATION);
    {
        JSValue op;

        JS_FreeValue(ctx, cb_result);
        if (!JS_IsNull(s->key_path)) {
            JSValue kpk = JS_UNDEFINED;
            IdbKeyPathResult r = idb_key_path_walk_take(ctx, &s->w, s->kpres, &kpk);
            /* "If kpk is FAILURE, INVALID, or NOT EQUAL TO this's effective key, throw a DataError." All three
               are one refusal in the standard and one here: the record stays filed under the key the cursor is
               positioned at, so a value whose in-line key would move it is not an update of that record. */
            bool bad = r != IDB_KEY_PATH_KEY || idb_key_compare(ctx, kpk, s->key) != 0;

            JS_FreeValue(ctx, kpk);
            if (bad) {                                                              /* STEP 9.2 */
                JS_ThrowDOMException(ctx, "DataError",
                                     "the value's key at the object store's key path is missing, is not a "
                                     "valid key, or is not the key the cursor is positioned at");
                return JS_STEP_ABRUPT;
            }
        }
        DCHECK(idb_transaction_state(ctx, s->tx) == IDB_TX_ACTIVE,
               "§4.9's `update` had its transaction deactivated between its own state check and the step that "
               "places the request — it suspends in between (§7.4's array arm is the page's own accessors), "
               "and nothing may deactivate a transaction across that");
        /* "... with this's effective object store, clone, this's effective key, AND FALSE" — the no-overwrite
           flag is false, which is what §4.9's own note describes: "if the record has been deleted since the
           cursor moved to it, a NEW RECORD WILL BE CREATED". */
        op = idb_object_store_record_operation(ctx, s->tx, s->store, s->clone, s->key,
                                               /*no_overwrite*/ false);             /* STEP 10 */
        *presult = idb_request_execute(ctx, hdr->this_val, s->tx, op);              /* STEP 11 */
        JS_FreeValue(ctx, op);
        return JS_STEP_DONE;
    }
}

static const IdlStepDecl UPDATE_STEP = {
    js_cu_update, sizeof(IdbCursorUpdateState), js_cu_update_visit, NULL,
    "Indexed Database §4.9 IDBCursor.update", CUU_STEPS
};

/* ---- §4.9's attributes ------------------------------------------------------------------------------------ */

/* "The source getter steps are to return this's SOURCE HANDLE." The standard's own note is the whole of why
   this asks nothing else: "the source attribute never returns null or throws an exception, even if the cursor
   is currently being iterated, has iterated past its end, or its transaction is not active." */
static JSValue js_cu_get_source(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!cu_brand(ctx, this_val)) return JS_EXCEPTION;
    return cu_get(ctx, this_val, CU_SOURCE_HANDLE);
}

static JSValue js_cu_get_direction(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!cu_brand(ctx, this_val)) return JS_EXCEPTION;
    return cu_get(ctx, this_val, CU_DIRECTION);
}

/* "The key getter steps are to return the result of CONVERTING A KEY TO A VALUE with the cursor's current key",
 * and "the primaryKey getter steps are to return the result of converting a key to a value with the cursor's
 * current EFFECTIVE KEY". `magic` picks which.
 *
 * THE NON-NORMATIVE PROSE BESIDE THEM SAYS THESE THROW ("throws an InvalidStateError DOMException if the cursor
 * is advancing or is finished") AND THE NORMATIVE STEPS DO NOT. The steps win: they are the member's
 * definition, they contain no refusal at all, and every shipping engine answers `undefined` for a cursor whose
 * key is undefined rather than throwing. The prose is a leftover from an edition in which the getters had
 * refusal steps of their own; following it would make `cursor.key` throw inside the very `success` handler that
 * §5.9 fires with `request.result` null, where a page reads it to decide the iteration is over.
 *
 * §7.3 IS NOT REACHED FOR AN ABSENT KEY, because §7.3 is stated over a key and "undefined" is the absence of
 * one — §6.7's own wording, "Set cursor's key to undefined." */
static JSValue js_cu_get_key(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue k, v;

    if (!cu_brand(ctx, this_val)) return JS_EXCEPTION;
    k = magic ? cu_effective_key(ctx, this_val) : cu_get(ctx, this_val, CU_KEY);
    if (JS_IsUndefined(k))
        return JS_UNDEFINED;
    v = idb_key_to_value(ctx, k);
    JS_FreeValue(ctx, k);
    return v;
}

/* "The request getter steps are to return this's request." `[SameObject]`, which is the whole point of §5.6
   step 3's optional request operand: every turn of the iteration fires the one this answers with. */
static JSValue js_cu_get_request(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue r;

    (void)magic;
    if (!cu_brand(ctx, this_val)) return JS_EXCEPTION;
    r = cu_get(ctx, this_val, CU_REQUEST);
    DCHECK(idb_request_is(r), "§4.9's `request` was read off a cursor that has none — §4.5's openCursor sets "
                              "it before the request it returns can reach the page");
    return r;
}

/* §4.9's IDBCursorWithValue: "the value getter steps are to return this's CURRENT VALUE." It is installed on
 * that interface's prototype alone, so a key-only cursor does not have it — which is §4.9's own last sentence
 * ("a cursor that has its key only flag set to FALSE implements the IDBCursorWithValue interface as well").
 *
 * ITS BRAND IS THE NARROWER CLASS AND NOT `idb_cursor_is`, which is Web IDL §3.7.6 Attributes and not
 * pedantry: a page
 * reaches this getter off IDBCursorWithValue.prototype with any receiver it likes
 * (`Object.getOwnPropertyDescriptor(IDBCursorWithValue.prototype, "value").get.call(keyOnlyCursor)`), and
 * §3.7.6 makes that a TypeError because the receiver does not IMPLEMENT the interface the member is declared
 * on. The shared brand would have accepted it and answered `undefined` — a key-only cursor reporting a value
 * attribute it does not have — so the check is the one the interface declares. */
static JSValue js_cu_get_value(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (JS_GetClassID(this_val) != g_cursor_wv_class)
        return JS_ThrowTypeError(ctx, "an IDBCursorWithValue member was reached on something that is not an "
                                      "IDBCursorWithValue");
    DCHECK(!cu_flag(ctx, this_val, CU_KEY_ONLY),
           "a cursor wearing IDBCursorWithValue has its KEY ONLY flag set — §4.9's last sentence makes those "
           "the same fact, and idb_cursor_new picks the class from that flag");
    return cu_get(ctx, this_val, CU_VALUE);
}

/* ---- install ---------------------------------------------------------------------------------------------- */

static void idb_cursor_install_realm(JSContext *ctx)
{
    JSValue proto, wv_proto, prev, ctor, global;

    DCHECK(g_cursor_class != 0, "a realm asked for IDBCursor.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_cursor_class);
    DCHECK(JS_IsNull(prev), "idb_cursor_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "IDBCursor.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "IDBCursor");
    /* IN §4.9'S OWN ORDER. The `length` of each method is Web IDL §3.7.7's — the number of REQUIRED arguments
       — so `advance` is 1, `continue` is 0 and `continuePrimaryKey` is 2. */
    idl_install_accessor(ctx, proto, "source", js_cu_get_source, 0, -1);
    idl_install_accessor(ctx, proto, "direction", js_cu_get_direction, 0, -1);
    idl_install_accessor(ctx, proto, "key", js_cu_get_key, 0, -1);
    idl_install_accessor(ctx, proto, "primaryKey", js_cu_get_key, 1, -1);
    idl_install_accessor(ctx, proto, "request", js_cu_get_request, 0, -1);
    idl_install_method(ctx, proto, "advance", g_id_advance);
    idl_install_method(ctx, proto, "continue", g_id_continue);
    idl_install_method(ctx, proto, "continuePrimaryKey", g_id_continue_pk);
    idl_install_method(ctx, proto, "update", g_id_update);
    idl_install_method(ctx, proto, "delete", g_id_delete);
    JS_SetClassProto(ctx, g_cursor_class, JS_DupValue(ctx, proto));

    ctor = idl_interface_object(ctx, "IDBCursor", proto);
    CHECK(!JS_IsException(ctor), "the IDBCursor interface object could not be allocated");
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "IDBCursor", ctor);

    /* §4.9's SECOND INTERFACE: `interface IDBCursorWithValue : IDBCursor`. Its prototype chains to the one just
       built — so `key`, `advance` and the rest are reached THROUGH it rather than copied — and it adds exactly
       the one attribute the standard declares on it. */
    wv_proto = JS_NewObjectProto(ctx, proto);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(wv_proto), "IDBCursorWithValue.prototype could not be allocated");
    idl_interface_tag(ctx, wv_proto, "IDBCursorWithValue");
    idl_install_accessor(ctx, wv_proto, "value", js_cu_get_value, 0, -1);
    JS_SetClassProto(ctx, g_cursor_wv_class, JS_DupValue(ctx, wv_proto));
    ctor = idl_interface_object(ctx, "IDBCursorWithValue", wv_proto);
    CHECK(!JS_IsException(ctor), "the IDBCursorWithValue interface object could not be allocated");
    JS_FreeValue(ctx, wv_proto);
    idl_define_global_property_reference(ctx, global, "IDBCursorWithValue", ctor);
    JS_FreeValue(ctx, global);
}

void idb_cursor_init(JSContext *ctx)
{
    JSClassDef d = { "IDBCursor" }, wd = { "IDBCursorWithValue" };
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* `advance([EnforceRange] unsigned long count)`, `continue(optional any key)` and
       `continuePrimaryKey(any key, any primaryKey)`. The two key positions are `any` because §7.4's
       convert-a-value-to-a-key is the MEMBER's own step and the IDL hands the value through unconverted. */
    static const IdlArgType ADVANCE_ARGS[1] = { IDL_UNSIGNED_LONG_ENFORCE };
    static const IdlArgType KEY_ARGS[1] = { IDL_ANY };
    static const IdlArgType KEY_PK_ARGS[2] = { IDL_ANY, IDL_ANY };
    /* `update(any value)` — `any`, because §5.11's clone is the member's own step 8 and the IDL converts
       nothing on the way. `delete()` takes no argument at all. */
    static const IdlArgType VALUE_ARGS[1] = { IDL_ANY };

    DCHECK(!g_ready, "idb_cursor_init ran twice — one instance is one document is one agent");
    g_key = JS_NewSymbol(ctx, "idbCursorState", false);
    CHECK(!JS_IsException(g_key), "the IDBCursor slot key allocation failed");
    g_cursor_rt = rt;
    JS_NewClassID(rt, &g_cursor_class);
    CHECK(JS_NewClass(rt, g_cursor_class, &d) == 0,
          "IDBCursor: the per-realm prototype slot could not be declared");
    JS_NewClassID(rt, &g_cursor_wv_class);
    CHECK(JS_NewClass(rt, g_cursor_wv_class, &wd) == 0,
          "IDBCursorWithValue: the per-realm prototype slot could not be declared");
    g_id_advance = idl_method_id(ctx, ADVANCE_ARGS, 1, js_cu_advance, 0);
    g_id_continue = idl_method_id_step(ctx, KEY_ARGS, 1, NULL, 0, &CONTINUE_STEP, 0);
    idl_optional_from(0);                        /* `continue(optional any key)` */
    g_id_continue_pk = idl_method_id_step(ctx, KEY_PK_ARGS, 2, NULL, 0, &CONTINUE_PK_STEP, 0);
    g_id_update = idl_method_id_step(ctx, VALUE_ARGS, 1, NULL, 0, &UPDATE_STEP, 0);
    g_id_delete = idl_method_id(ctx, NULL, 0, js_cu_delete, 0);
    g_iterate_stepid = JS_RegisterStepDef(rt, &js_idb_iterate_def);
    g_ready = 1;
    agent_state_flag("idb_cursor", &g_ready, "the declaration latch");
    agent_state_ptr("idb_cursor", &g_cursor_rt, "the runtime §2.10's slot key was minted in");
    agent_state_value("idb_cursor", &g_key, "§2.10's internal-slot key");
    agent_state_id("idb_cursor", &g_iterate_stepid, "§6.7's iterate-a-cursor machine");
    realm_declare_intrinsic(idb_cursor_install_realm);
}

void idb_cursor_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;` — see idb_transaction_free: the declare pass is unconditional, so an
       undeclared release is a teardown by something that is not core/platform.c's one list. */
    DCHECK(g_ready, "§2.10's cursor machinery was released in an agent that never declared it");
    DCHECK(rt == g_cursor_rt, "idb_cursor_free was given a runtime that is not the one it declared into");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    g_cursor_rt = NULL;
    g_iterate_stepid = -1;
    g_ready = 0;
}
