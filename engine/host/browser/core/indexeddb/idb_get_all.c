/* INDEXED DATABASE §5.12's CREATING A REQUEST TO RETRIEVE MULTIPLE ITEMS, and §6.2's and §6.3's RETRIEVE
 * MULTIPLE ITEMS under it — the algorithm behind six members: `getAll`, `getAllKeys` and `getAllRecords` on
 * §4.5's IDBObjectStore and on §4.6's IDBIndex.
 *
 * WHY ONE FILE FOR SIX MEMBERS. §4.5 and §4.6 state all six as one sentence each — "return the result of
 * creating a request to retrieve multiple items with the current Realm record, this, KIND, queryOrOptions, and
 * count if given" — so the members differ in exactly two operands: which SOURCE they name, and which of the
 * three KINDs. Six bodies would be six chances for the direction, the count clamp or the order to drift, and
 * the order is the half a length assertion cannot see.
 *
 * ---- WHAT ORDER THE RECORDS COME BACK IN, AND WHY THIS FILE DOES NOT FOLLOW §6.2's PROSE LITERALLY ----------
 *
 * §6.2's retrieve-multiple says, for a "prev" direction, "set records to the LAST count of store's list of
 * records whose key is in range", and then its step 6 appends them in list order — which would answer in
 * ASCENDING key order. That is wrong, and three independent readings say so:
 *
 *   1. §2.10 Cursor is the authority on what a direction MEANS, and it is unambiguous: "prev — when iterated,
 *      the cursor should yield all records, including duplicates, in monotonically DECREASING order of keys".
 *      §5.12 takes its `direction` from `IDBCursorDirection`, which is that enumeration.
 *   2. §6.3's own "prevunique" arm PREPENDS each record it keeps, i.e. it reverses — so the standard does
 *      state the reversal, in the one arm that spells its loop out. §6.2's omission is editorial.
 *   3. §4.5's member description says "set the direction option to next to retrieve the first count records,
 *      or PREV TO RETURN THE LAST COUNT RECORDS" — the last ones, which is a different SET from the first
 *      ones, and a set that had to be re-sorted ascending to be returned would make "prev" a synonym for a
 *      trailing slice rather than a direction.
 *
 * So a descending direction answers in DECREASING key order. Anyone re-reading §6.2's sentence and "fixing"
 * the walk below back to its letter will make `getAllRecords({direction:'prev', count:3})` answer the three
 * SMALLEST keys where every browser answers the three largest.
 *
 * ---- AND WHY THE UNIQUE ARMS ARE NOT §6.3's LOOP LITERALLY EITHER ------------------------------------------
 *
 * §6.3's "nextunique" and "prevunique" arms are written as `let i be 0. While i is less than
 * rangeRecordsLength: increase i by 1; ... compare rangeRecords[i] and rangeRecords[i-1]`. The increment
 * precedes every use of `i`, so the loop never considers `rangeRecords[0]` — implemented literally it DROPS
 * THE FIRST RECORD of every unique query, and reads one past the end on its last turn. The intent is plain
 * from §2.10, which is again the authority: "nextunique/prevunique — the cursor should not yield records with
 * the same key ... FOR EVERY KEY WITH DUPLICATE VALUES, ONLY THE FIRST RECORD IS YIELDED". First in the
 * SOURCE's own order, which §2.6 sorts by (key, value) — so the record kept is the one with the lowest
 * referenced primary key, in both unique directions, and "prevunique" then reverses like "prev".
 *
 * THIS FILE THEREFORE ASKS ONE QUESTION PER RECORD, in both directions: a record is kept when it is the first
 * of its equal-key run — `i == from || compare(key[i], key[i-1]) != 0`. Walked ascending that yields the run
 * leaders in increasing order; walked descending it yields the same leaders in decreasing order, which is
 * exactly "dedupe keeping the first, then reverse". No list is built and reversed, and no record's value is
 * deserialized only to be discarded by the count.
 *
 * ---- IT IS A STEP MACHINE, ONE RECORD PER STAGE -------------------------------------------------------------
 *
 * The list it walks is the PAGE'S size — every record the store or index holds — so quickjs-step.h's rule
 * applies at its plainest: a span over a list is not a range one stage may name, it is a stage per record, and
 * the walking stage returns JS_STEP_YIELD at every turn so the scheduler is ASKED at each one. There is no C
 * entry: a second, non-suspending copy of this walk is the thing this shape exists to prevent.
 *
 * ---- WHAT IS NOT BUILT, NAMED AT THE SITE --------------------------------------------------------------------
 *
 * §5.12 step 9 reads `queryOrOptions["query"]`, `["count"]` and `["direction"]` — Web IDL dictionary-member
 * lookups on a value that, for `getAll` and `getAllKeys`, arrives as a bare `any`. So the conversion to
 * `IDBGetAllOptions` has to happen INSIDE the algorithm, after the branch, and this engine's §3.2.17
 * conversion is reachable only as a DECLARED ARGUMENT TYPE. That capability is named in the DFAIL at the
 * branch rather than hand-rolled here — a trio of step_getprop_run calls would be a second dictionary machine
 * beside the one that exists.
 *
 * WHAT IT ACTUALLY BLOCKS IS ONE SHAPE, because three of the four ways step 9 is reached need no machinery:
 * `getAllRecords` declares the dictionary, so the args machine converted it at the argument boundary;
 * undefined and null never reach step 9 (they are potentially valid key ranges — see
 * ga_potentially_valid_key_range); and a non-object PRIMITIVE is §3.2.17 step 2's TypeError, which reads
 * nothing. So what waits on the capability is exactly `getAll(obj)` / `getAllKeys(obj)` with a real options
 * object, which is what WPT's `*-options` files pass. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_get_all.h"
#include "core/indexeddb/idb_index.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_key_range.h"
#include "core/indexeddb/idb_record.h"
#include "core/indexeddb/idb_request.h"
#include "core/indexeddb/idb_transaction.h"
#include "core/structured_clone.h"

const char *const IDB_CURSOR_DIRECTION_VALUES[5] = { "next", "nextunique", "prev", "prevunique", NULL };

const IdlDictMember IDB_GET_ALL_OPTIONS[3] = {
    /* §3.2.17 step 4.1 reads a dictionary's own members LEXICOGRAPHICALLY, which idl_args.c asserts:
       "count" < "direction" < "query". Declared in any other order this aborts at every runtime init. */
    { "count", IDL_UNRESTRICTED_DOUBLE },
    { "direction", IDL_ENUM, false, IDB_CURSOR_DIRECTION_VALUES, 0, NULL, IDL_DEFAULT_STRING, "next" },
    { "query", IDL_ANY, false, NULL, 0, NULL, IDL_DEFAULT_NULL },
};

int idb_get_all_count_enforce_range(JSContext *ctx, JSValueConst v, uint32_t *pcount)
{
    double x = 0;
    int r = JS_ToFloat64(ctx, &x, v);

    DCHECK(r >= 0, "§5.12's `count` reached its range check as something its own declaration did not convert "
                   "to a number — the declaration is what performs §3.2.7's ToNumber, because that is the "
                   "page's `valueOf` and has to be a request");
    (void)r;
    /* [EnforceRange] REPLACES §3.2.4.7's modulo with a refusal. `getAll(q, -1)` is a TypeError, where the
       plain `unsigned long` conversion would ask for 4294967295 records. */
    if (!isfinite(x) || x != trunc(x) || x < 0 || x > 4294967295.0) {
        JS_ThrowTypeError(ctx, "the count is outside the range of an unsigned long");
        return -1;
    }
    *pcount = (uint32_t)x;
    return 0;
}

/* ---- §6.2's and §6.3's RETRIEVE MULTIPLE ITEMS, as §5.12 step 10's operation ------------------------------- */

/* WHAT THE OPERATION WAS MINTED OVER, captured by the closure because §scheduler's rule is that an operation
   which becomes a work item takes its inputs WITH it: a direction or a range read back off the handle at task
   time would be whatever that handle named THEN, and the handle is not even an operand of these algorithms. */
#define OP_GA_SOURCE   0   /* the §2.2 object store or §2.6 index */
#define OP_GA_RANGE    1   /* §5.12 step 8's or step 9's key range */
#define OP_GA_KIND     2
#define OP_GA_DIR      3
#define OP_GA_COUNT    4   /* the count if given, JS_UNDEFINED when it was not — §6.2 step 1's distinction */
#define OP_GA_IS_INDEX 5

typedef struct {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    JSValue   list;     /* §6.2 step 5's "let list be an empty list" (owned) */
    double    count;    /* step 1's count, INFINITY when it was not given or was 0 */
    uint32_t  n;        /* how many records the source held when the selection started */
    uint32_t  pos;      /* the selection cursor, then the emission cursor */
    uint32_t  from;     /* the first in-range position */
    uint32_t  len;      /* how many in-range records there are */
    uint32_t  taken;    /* how many have been appended */
    uint8_t   found;    /* whether the selection has reached the run yet */
} JSIdbGetAllState;

static void js_idb_get_all_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSIdbGetAllState *s = st;

    v->val(ctx, &s->list);
}

static JSValue js_idb_get_all_fini(JSContext *ctx, void *st, bool take_result)
{
    JSIdbGetAllState *s = st;
    JSValue r = take_result ? s->list : JS_UNDEFINED;

    (void)ctx;
    if (take_result) s->list = JS_UNDEFINED;
    return r;
}

#define GA_STAGES(X) \
    X(GA_COUNT, "Indexed Database §6.2 / §6.3 retrieve multiple items steps 1-2 — ONE O(1) engine action: a " \
                "count that is not given or is 0 (zero) is infinity, and the list of records starts empty") \
    X(GA_SELECT, "Indexed Database §6.2 steps 3-4 / §6.3 step 3 — ONE RECORD of the source's list of records " \
                 "per stage: whether its key is in range, which is what decides the run the direction reads " \
                 "the first or the last of") \
    X(GA_LIST, "Indexed Database §6.2 step 5 / §6.3 step 4 (let list be an empty list)") \
    X(GA_ITEM, "Indexed Database §6.2 step 6 / §6.3 step 5 — ONE RECORD of the selected run per stage: the " \
               "switch on kind, and the append; or step 7's \"return list\" once the run or the count is " \
               "exhausted")
enum { GA_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const GA_STEPS[] = { GA_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* THE SOURCE IS AN OBJECT STORE OR AN INDEX, and §6.2 and §6.3 are the SAME algorithm over two lists — which
 * is why §6.5's counting operation is stated once over a "source" too. Both lists are sorted, both hold
 * records with a key, and the two differ in what the record's SECOND field is: an object store record's is its
 * §2.3 VALUE, and an index record's is a KEY of the referenced object store (§2.6's "the index will contain a
 * record with key Y and value X"). Every arm below that reads the second field says which one it is reading. */
static uint32_t ga_source_count(JSContext *ctx, JSValueConst source, bool is_index)
{
    return is_index ? idb_index_record_count(ctx, source) : idb_store_record_count(ctx, source);
}

static void ga_source_at(JSContext *ctx, JSValueConst source, bool is_index, uint32_t i,
                         JSValue *key, JSValue *second)
{
    if (is_index)
        idb_index_record_at(ctx, source, i, key, second);
    else
        idb_store_record_at(ctx, source, i, key, second);
}

/* §6.3's "let serialized be record's REFERENCED VALUE" — §2.6's "the value of the record in the index's
   referenced object store which has a key equal to the index's record's value". That is §6.2's retrieve-a-value
   over a key range containing only the primary key, which is how §6.3 reaches it and why this is not a second
   walk of the store's list. `primary` is BORROWED. */
static JSValue ga_referenced_value(JSContext *ctx, JSValueConst index, JSValueConst primary)
{
    JSValue store = idb_index_store(ctx, index), only = idb_key_range_only_key(ctx, primary), out;

    DCHECK(idb_count_records(ctx, store, only) == 1,
           "an index record's REFERENCED VALUE is not in the referenced object store. §2.6: \"each record in "
           "an index references one and only one record in the index's referenced object store\", and §6.4 "
           "step 2 is what keeps that true when a store record is removed — a missing one means that step did "
           "not run for this index");
    out = idb_retrieve_value(ctx, store, only);
    JS_FreeValue(ctx, only);
    JS_FreeValue(ctx, store);
    return out;
}

/* §6.2 step 6's / §6.3 step 5's SWITCH ON KIND, for ONE record. Every arm is the standard's own sentence, and
   the two halves differ exactly where §2.12's note says a record snapshot's fields differ. */
static JSValue ga_item(JSContext *ctx, JSValueConst source, bool is_index, int kind, uint32_t i)
{
    JSValue key, second, out;

    ga_source_at(ctx, source, is_index, i, &key, &second);
    if (kind == IDB_GET_ALL_KEY) {
        /* §6.2: "let key be the result of converting a key to a value with record's KEY". §6.3: "... with
           record's VALUE" — the referenced object store's key, which is what an index's getAllKeys answers
           with rather than the index key it was filed under. */
        out = idb_key_to_value(ctx, is_index ? second : key);
    } else {
        /* "Let serialized be record's value" / "record's referenced value", then
           "! StructuredDeserialize(serialized, targetRealm)". A FRESH copy every time, which is what makes a
           page that mutates what it got unable to reach the record — §2.3's by-value-not-by-reference. */
        JSValue value;

        if (is_index) {
            value = ga_referenced_value(ctx, source, second);
        } else {
            value = structured_clone(ctx, second);
            DCHECK(!JS_IsException(value),
                   "§6.2's deserialization refused a value §6.1 had already copied — the two halves of one "
                   "clone disagree, which no page input can cause");
        }
        if (kind == IDB_GET_ALL_VALUE) {
            out = value;
        } else {
            /* "Let recordSnapshot be a new record snapshot with its key set to key, value set to value, and
               primary key set to key." §6.2's three are the store record's own key twice and the value;
               §6.3's are the INDEX key, the record's value (the primary key) and the referenced value. */
            DCHECK(kind == IDB_GET_ALL_RECORD, "§6.2's switch on kind was entered with a kind the standard "
                                               "does not list — §5.12's three members name \"value\", "
                                               "\"key\" and \"record\" and nothing else");
            out = idb_record_new(ctx, JS_DupValue(ctx, key),
                                 JS_DupValue(ctx, is_index ? second : key), value);
        }
    }
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, second);
    return out;
}

/* §2.4's COMPARE over the keys at two positions of the source's list — the one question both the sortedness
   assert and the unique arms ask. */
static int ga_key_compare_at(JSContext *ctx, JSValueConst source, bool is_index, uint32_t a, uint32_t b)
{
    JSValue ka, va, kb, vb;
    int c;

    ga_source_at(ctx, source, is_index, a, &ka, &va);
    ga_source_at(ctx, source, is_index, b, &kb, &vb);
    c = idb_key_compare(ctx, ka, kb);
    JS_FreeValue(ctx, ka); JS_FreeValue(ctx, va);
    JS_FreeValue(ctx, kb); JS_FreeValue(ctx, vb);
    return c;
}

static int js_idb_get_all_operation(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb,
                                    int *out_argc)
{
    JSIdbGetAllState *s = st;
    JSValueConst source = JS_StepClosureData(&s->hdr, OP_GA_SOURCE);
    JSValueConst range = JS_StepClosureData(&s->hdr, OP_GA_RANGE);
    bool is_index = JS_ToBool(ctx, JS_StepClosureData(&s->hdr, OP_GA_IS_INDEX)) != 0;
    int kind = 0, dir = 0;

    (void)out_cb; (void)out_argc;
    JS_ToInt32(ctx, &kind, JS_StepClosureData(&s->hdr, OP_GA_KIND));
    JS_ToInt32(ctx, &dir, JS_StepClosureData(&s->hdr, OP_GA_DIR));

    STEP_DISPATCH(GA_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(GA_COUNT);
    {
        JSValueConst cv = JS_StepClosureData(&s->hdr, OP_GA_COUNT);

        JS_FreeValue(ctx, cb_result);
        /* A STEP STATE IS js_mallocz'd AND A ZEROED JSValue IS THE INTEGER 0, so every slot this machine owns
           is PLACED before anything can read one. */
        s->list = JS_UNDEFINED;
        DCHECK(JS_IsObject(source), "§5.12 step 10's operation was minted over something that is not an "
                                    "object store or an index");
        /* "If count is not given or is 0 (zero), let count be infinity." The two are ONE arm in the standard
           and the engine spells the answer as the actual IEEE infinity, so the clamp below is the comparison
           the standard writes rather than a flag a reader has to decode. */
        s->count = INFINITY;
        if (!JS_IsUndefined(cv)) {
            double c = 0;

            JS_ToFloat64(ctx, &c, cv);
            DCHECK(c >= 0 && c == trunc(c) && isfinite(c),
                   "§5.12 handed its operation a `count` that is not an unsigned long — "
                   "idb_get_all_count_enforce_range is what refuses one, and it runs before the member's own "
                   "steps because Web IDL converts an argument before the operation it belongs to");
            if (c != 0) s->count = c;
        }
        s->n = ga_source_count(ctx, source, is_index);   /* STEP 2's list, as it stands */
        STEP_GOTO(s->hdr.stage, GA_SELECT, &s->hdr.get_phase, &s->hdr.desc_phase, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(GA_SELECT);
    {
        JSValue key, second;
        bool in;

        JS_FreeValue(ctx, cb_result);
        DCHECK(s->n == ga_source_count(ctx, source, is_index),
               "a source's list of records CHANGED while §6.2's retrieve-multiple walked it. An operation is "
               "one task and §2.7.2 gives one object store to one read/write transaction at a time, so "
               "nothing may add to or remove from this list across one — and a record inserted behind the "
               "cursor is a record this answer would silently skip");
        if (s->pos >= s->n) {
            STEP_GOTO(s->hdr.stage, GA_LIST, &s->hdr.get_phase, &s->hdr.desc_phase, NULL);
            return JS_STEP_YIELD;
        }
        ga_source_at(ctx, source, is_index, s->pos, &key, &second);
        in = idb_key_range_contains(ctx, range, key);
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, second);
        /* §2.2's and §2.6's SORT, asserted where it is relied on. Both lists are read here as ordered by
           §2.4's compare, and the two differ in whether EQUAL keys may be adjacent: §2.2 says "there can never
           be multiple records in a given object store with the same key", while §2.6 sorts "primarily on the
           records keys, and secondarily on the records values" and admits duplicates. A store that had gone
           out of order — or had grown a duplicate — would make the run below select the wrong records and the
           unique arms skip the wrong ones, silently. */
        if (s->pos > 0) {
            int c = ga_key_compare_at(ctx, source, is_index, s->pos - 1, s->pos);

            DCHECK(c <= 0, "a source's list of records is not in §2.4's key order — §2.2 and §2.6 both sort "
                           "it, and every walk over a range in this file reads it as an interval of that "
                           "order");
            DCHECK(is_index || c < 0,
                   "an OBJECT STORE's list of records holds two records with the same key — §2.2: \"there "
                   "can never be multiple records in a given object store with the same key\", which is what "
                   "makes \"nextunique\" and \"prevunique\" identical to \"next\" and \"prev\" over one");
        }
        if (in) {
            /* §2.9's key range is an INTERVAL in §2.4's order and both lists are kept in that order, so the
               records a range selects are a CONTIGUOUS RUN — which is what makes `from` and `len` the whole
               of the selection rather than a set of positions. The same assertion idb_range_span makes. */
            DCHECK(!s->found || s->pos == s->from + s->len,
                   "a source's list of records holds an in-range record separated from the run that same "
                   "range selected — §2.9's key range is an interval in §2.4's key order and §2.2/§2.6 keep "
                   "the list in that order, so a range selects a contiguous run or the list is no longer "
                   "sorted");
            if (!s->found) {
                s->from = s->pos;
                s->found = 1;
                /* THE RUN'S FIRST RECORD IS ALSO ITS EQUAL-KEY RUN'S LEADER, which is what lets the unique
                   arms below test `i == from` instead of looking outside the run: a predecessor with an EQUAL
                   key would be in the same interval and therefore in range too, so it would have been the
                   first. */
                DCHECK(s->pos == 0 || ga_key_compare_at(ctx, source, is_index, s->pos - 1, s->pos) != 0,
                       "the first in-range record of a source's list shares its key with the record before "
                       "it, which is OUT of range — §2.9's range is an interval in §2.4's order, so two "
                       "equal keys are either both in it or both out");
            }
            s->len = s->pos + 1 - s->from;
        }
        s->pos++;
        return JS_STEP_YIELD;
    }

    STEP_ARM(GA_LIST);
        JS_FreeValue(ctx, cb_result);
        s->list = JS_NewArray(ctx);
        CHECK(!JS_IsException(s->list), "IndexedDB: §6.2 step 5's list could not be allocated");
        s->pos = 0;
        STEP_GOTO(s->hdr.stage, GA_ITEM, &s->hdr.get_phase, &s->hdr.desc_phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(GA_ITEM);
    {
        bool descending = dir == IDB_DIR_PREV || dir == IDB_DIR_PREVUNIQUE;
        bool unique = dir == IDB_DIR_NEXTUNIQUE || dir == IDB_DIR_PREVUNIQUE;
        uint32_t i;

        JS_FreeValue(ctx, cb_result);
        DCHECK(dir >= IDB_DIR_NEXT && dir <= IDB_DIR_PREVUNIQUE,
               "§5.12's operation was minted with a direction that is not one of §2.10's four");
        /* "If count is specified and there are more than count records in range, only the first count will be
           retrieved" — over the records this direction yields, which is why the clamp is on what has been
           TAKEN and not on how far the cursor has moved: a skipped duplicate is not one of the count. */
        if (s->taken >= s->count || s->pos >= s->len)
            return JS_STEP_DONE;                                              /* STEP 7: "Return list" */
        /* "next"/"nextunique" read the FIRST of the run and "prev"/"prevunique" the LAST — §2.10's
           monotonically increasing and decreasing orders. See this file's header for why §6.2's prose is not
           followed literally here. */
        i = descending ? s->from + s->len - 1 - s->pos : s->from + s->pos;
        s->pos++;
        /* §2.10: "for every key with duplicate values, only the FIRST record is yielded" — first in the
           SOURCE's order, which is the leader of each equal-key run. Asked identically in both directions;
           walked descending it yields those same leaders in decreasing order, which is what §6.3's
           "prevunique" arm spells as a prepend. */
        if (unique && i > s->from && ga_key_compare_at(ctx, source, is_index, i - 1, i) == 0)
            return JS_STEP_YIELD;
        {
            JSValue item = ga_item(ctx, source, is_index, kind, i);

            if (JS_IsException(item)) return JS_STEP_ABRUPT;
            CHECK(JS_SetPropertyUint32(ctx, s->list, s->taken, item) >= 0,
                  "IndexedDB: §6.2 step 6's append to the result list failed");
            s->taken++;
        }
        return JS_STEP_YIELD;
    }
}

static const JSTrampStepDef js_idb_get_all_def = {
    sizeof(JSIdbGetAllState), js_idb_get_all_operation, js_idb_get_all_fini, 0,
    .visit = js_idb_get_all_visit,
    .algorithm = "Indexed Database §6.2 / §6.3 retrieve multiple items from an object store or an index, as "
                 "§5.12 step 10's operation",
    .steps = GA_STEPS
};
static int g_get_all_stepid = -1;

/* ---- §5.12, from step 6 -------------------------------------------------------------------------------- */

/* §2.9's "IS A POTENTIALLY VALID KEY RANGE with ECMAScript value": "if value is a key range, return true. Let
 * key be the result of converting a value to a key with value. If key is 'invalid type' return false. Else
 * return true."
 *
 * IT RUNS NONE OF THE PAGE'S CODE AND THEREFORE NEEDS NO REST POINT, which is the whole reason §5.12's branch
 * is one O(1) engine action. §7.4's arms that could decide it — Number, Date, String, a buffer source,
 * "otherwise" — each answer without reading anything the page controls (the Date arm reads the [[DateValue]]
 * slot and never `getTime`), and its ARRAY arm is handed back unread, which is sound here because that arm can
 * only ever produce a key or "invalid value" and never the "invalid type" this question is about.
 *
 * The spec's own note is what the four answers are for: "a DETACHED BufferSource is a potentially valid key
 * range that will throw an exception when used with convert a value to a key range" — so `new Date(NaN)`,
 * `[{}]` and a detached buffer all take the TRUE branch and are then a "DataError" from the conversion.
 *
 * ---- undefined AND null ARE POTENTIALLY VALID KEY RANGES, AND THE STEPS ABOVE SAY THEY ARE NOT -------------
 *
 * The four steps ask CONVERT A VALUE TO A KEY, which has no undefined/null arm at all — both fall out of its
 * "Otherwise" as "invalid type", so read literally `getAll()` and `getAll(undefined, 10)` would be handled as
 * IDBGetAllOptions dictionaries. That is wrong, and the standard contradicts it in the sentence immediately
 * above those steps: "A potentially valid key range is an ECMAScript value that has a TYPE THAT IS CONVERTIBLE
 * TO A KEY RANGE" — and §2.9's convert-a-value-to-a-key-RANGE step 2 converts undefined and null to an
 * UNBOUNDED KEY RANGE. Their type is convertible; the steps just ask the wrong one of the two conversions.
 *
 * WHAT IT COSTS TO FOLLOW THE LETTER IS OBSERVABLE AND IT IS NOT A CORNER. On the dictionary branch `count`
 * comes from `queryOrOptions["count"]`, so the POSITIONAL count is discarded — `store.getAll(undefined, 10)`
 * would answer with every record in the store instead of ten, silently changing the two-argument overload
 * that has shipped since this standard's first edition. The standard's own note names the values the
 * dictionary branch exists to catch — "getAll() and getAllKeys() throw exceptions for Date, Array, and
 * ArrayBuffer first arguments that return 'invalid value'" — and undefined is not among them.
 *
 * So undefined and null answer TRUE here and reach §2.9's step 2, which is where the standard states what
 * they mean. Anyone re-deriving this function from the four steps alone will make `getAll(undefined, n)`
 * ignore its count. */
static bool ga_potentially_valid_key_range(JSContext *ctx, JSValueConst v)
{
    JSValue key = JS_UNDEFINED, array = JS_UNDEFINED;
    IdbKeyResult r;

    if (idb_key_range_is(v) || JS_IsUndefined(v) || JS_IsNull(v))
        return true;
    r = idb_key_convert_here(ctx, v, &key, &array);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, array);
    return r != IDB_KEY_INVALID_TYPE;
}

/* §5.12 step 9's `queryOrOptions["direction"]`, as the enumerator §2.10 names. The member's declaration has
   already run §3.2.18 — ToString and then membership in `IDBCursorDirection` — so what arrives is one of the
   four strings and anything else is this engine disagreeing with its own declaration. */
static int ga_direction_of(JSContext *ctx, JSValueConst v)
{
    const char *s;
    int i;

    DCHECK(JS_IsString(v), "§5.12 step 9 read a `direction` that is not a string — the member declares it as "
                           "an IDBCursorDirection, so §3.2.18's conversion and its default have both already "
                           "run by the time this reads it");
    s = JS_ToCString(ctx, v);
    CHECK(s != NULL, "IndexedDB: §5.12's direction could not be read");
    for (i = 0; IDB_CURSOR_DIRECTION_VALUES[i]; i++)
        if (!strcmp(s, IDB_CURSOR_DIRECTION_VALUES[i])) {
            JS_FreeCString(ctx, s);
            return i;
        }
    JS_FreeCString(ctx, s);
    /* ALWAYS FATAL, and not a DCHECK with a "next" behind it: a `?:` past this would make an unvalidated
       string silently mean "next" in a release build, so every direction-dependent answer below — the order
       the records come back in — would be decided by a value nothing checked. §3.2.18 makes an unrecognised
       value a TypeError at the DECLARATION, so one arriving here means the member was declared without its
       enumeration, which is a data-integrity failure of this engine's own IDL and not a page's input. */
    CHECK_FAIL("§5.12 step 9 read a `direction` that IDBCursorDirection does not list — the member's "
               "declaration is missing its IDL_ENUM values, so §3.2.18's membership test never ran");
}

int idb_get_all_walk_start(JSContext *ctx, JSStepHdr *hdr, IdbGetAllWalk *w,
                           JSValueConst source, JSValueConst tx, bool is_index, int kind,
                           JSValueConst query_or_options, bool options_converted,
                           uint32_t count_arg, bool has_count_arg, int base, int after)
{
    JSValue query;
    int r;

    DCHECK(JS_IsObject(source), "§5.12 step 1 produced something that is not an object store or an index");
    DCHECK(idb_transaction_is(tx), "§5.12 step 4 produced something that is not a transaction");
    DCHECK(idb_transaction_state(ctx, tx) == IDB_TX_ACTIVE,
           "§5.12 reached step 6 with a transaction that is not ACTIVE — step 5 is the refusal that makes "
           "that impossible, and it is the member's own step, run immediately before this");
    w->source = JS_DupValue(ctx, source);
    w->tx = JS_DupValue(ctx, tx);
    w->is_index = is_index ? 1 : 0;
    w->kind = (uint8_t)kind;
    w->direction = IDB_DIR_NEXT;                                              /* STEP 7 */
    w->count = count_arg;
    w->has_count = has_count_arg ? 1 : 0;

    if (ga_potentially_valid_key_range(ctx, query_or_options)) {              /* STEP 8 */
        DCHECK(!options_converted,
               "a CONVERTED IDBGetAllOptions dictionary answered \"is a potentially valid key range\" true — "
               "§3.2.17 builds a plain object and §7.4 answers \"invalid type\" for one, so the value this "
               "member's declaration converted is not what reached the algorithm");
        /* "Set range to the result of converting a value to a key range with queryOrOptions. Rethrow any
           exceptions. Set direction to next." No null-disallowed flag — §5.12 states none, and null cannot
           reach this branch anyway (§7.4 answers "invalid type" for it). */
        return idb_key_range_walk_start(ctx, hdr, &w->rw, query_or_options, /*null_disallowed*/ false,
                                        base, after);
    }

    /* STEP 9. `queryOrOptions["query"]`, `["count"]` and `["direction"]` are Web IDL dictionary-member
       lookups, so what they are stated over is an IDBGetAllOptions the value has been CONVERTED to. */
    if (!options_converted) {
        /* WEB IDL §3.2.17 STEP 2 IS O(1) AND IS THE WHOLE ANSWER FOR A PRIMITIVE: "if Type(V) is not
           Undefined, Null, or Object, throw a TypeError". Nothing is read off a boolean, a number that got
           here, a symbol or a bigint, so this arm needs none of the machinery the object arm below waits on —
           and undefined and null never reach step 9 at all (see ga_potentially_valid_key_range). */
        if (!JS_IsObject(query_or_options)) {
            JS_ThrowTypeError(ctx, "the first argument is neither a key nor an IDBGetAllOptions dictionary");
            return JS_STEP_ABRUPT;
        }
        DFAIL("Indexed Database §5.12 step 9 was reached with an UNCONVERTED `queryOrOptions`. `getAll` and "
              "`getAllKeys` declare their first argument as `any`, so the branch that reads it as an "
              "IDBGetAllOptions has to convert one HERE — after the potentially-valid-key-range test, which "
              "is what chose this branch. THE CAPABILITY TO BUILD IS: a step-driven Web IDL §3.2.17 "
              "dictionary conversion, embeddable in an algorithm (an idl_dict_walk_start/_run/_take beside "
              "core/idl_args.h's declared-argument path), SHARING the args machine's per-member conversion "
              "rather than a second copy of it — every member read is one [[Get]] the page may have put an "
              "accessor on, and each member's own conversion is the page's code again, so the algorithm must "
              "rest on the member it is on. `getAllRecords` does not need it: its IDL declares the "
              "dictionary, so the args machine converted it at the argument boundary");
        JS_ThrowTypeError(ctx, "an IDBGetAllOptions dictionary cannot yet be converted inside an algorithm: "
                               "this engine's Web IDL dictionary conversion is reachable only as a declared "
                               "argument type");
        return JS_STEP_ABRUPT;
    }
    DCHECK(JS_IsObject(query_or_options),
           "§5.12 step 9's options are not an object — `optional IDBGetAllOptions options = {}` makes the "
           "args machine build one on every path, including the one where the member was called with nothing");
    {
        JSValue cv = idl_dict_get(ctx, query_or_options, "count");
        JSValue dv = idl_dict_get(ctx, query_or_options, "direction");

        /* "Set count to queryOrOptions["count"]" — and the member carries NO default, so an absent one leaves
           §6.2 step 1's "not given" arm in place rather than becoming a 0 this consumer invented. The
           positional `count` an overload may also have passed is DISCARDED here, which is what makes
           `store.getAll({count: 10}, 17)` a request for ten. */
        w->has_count = 0;
        if (!JS_IsUndefined(cv)) {
            if (idb_get_all_count_enforce_range(ctx, cv, &w->count) < 0) {
                JS_FreeValue(ctx, cv);
                JS_FreeValue(ctx, dv);
                return JS_STEP_ABRUPT;
            }
            w->has_count = 1;
        }
        w->direction = (uint8_t)ga_direction_of(ctx, dv);      /* "Set direction to …["direction"]" */
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, dv);
    }
    /* "Set range to the result of converting a value to a key range with queryOrOptions["query"]. Rethrow any
       exceptions." The member declares `any query = null`, so an absent one is the IDL null, which §2.9 makes
       the unbounded key range — the answer `store.getAllRecords({})` must give. */
    query = idl_dict_get(ctx, query_or_options, "query");
    r = idb_key_range_walk_start(ctx, hdr, &w->rw, query, /*null_disallowed*/ false, base, after);
    JS_FreeValue(ctx, query);
    return r;
}

int idb_get_all_walk_run(JSContext *ctx, JSStepHdr *hdr, IdbGetAllWalk *w, JSValue in, int base,
                         JSValue **out_cb, int *out_argc)
{
    return idb_key_range_walk_run(ctx, hdr, &w->rw, in, base, out_cb, out_argc);
}

void idb_get_all_walk_visit(JSContext *ctx, IdbGetAllWalk *w, JSStepVisit *v)
{
    idb_key_range_walk_visit(ctx, &w->rw, v);
    v->val(ctx, &w->source);
    v->val(ctx, &w->tx);
}

JSValue idb_get_all_walk_take(JSContext *ctx, IdbGetAllWalk *w, JSValueConst source_handle)
{
    JSValueConst data[6];
    JSValue range, op, req;

    DCHECK(g_get_all_stepid >= 0, "§5.12's operation was minted before idb_get_all_init declared its machine");
    if (idb_key_range_walk_take(ctx, &w->rw, &range) < 0)                     /* step 8/9's DataError */
        return JS_EXCEPTION;
    /* §2.7.1's cleanup is a checkpoint of the event loop, so nothing may deactivate a transaction across a
       suspension INSIDE one member — and this member suspends, at §7.4's array arm. */
    DCHECK(idb_transaction_state(ctx, w->tx) == IDB_TX_ACTIVE,
           "§5.12's member had its transaction deactivated between its own step 5 and the step that places "
           "the request — the member suspended in between (§7.4's array arm), and §2.7.1's cleanup is a "
           "checkpoint of the event loop");
    data[OP_GA_SOURCE] = w->source;
    data[OP_GA_RANGE] = range;
    data[OP_GA_KIND] = JS_NewInt32(ctx, w->kind);
    data[OP_GA_DIR] = JS_NewInt32(ctx, w->direction);
    data[OP_GA_COUNT] = w->has_count ? JS_NewUint32(ctx, w->count) : JS_UNDEFINED;
    data[OP_GA_IS_INDEX] = JS_NewBool(ctx, w->is_index);
    op = JS_NewStepClosure(ctx, g_get_all_stepid, 0, 6, data);                /* STEP 10 */
    JS_FreeValue(ctx, range);
    CHECK(!JS_IsException(op), "IndexedDB: §5.12 step 10's operation could not be minted");
    /* "Return the result (an IDBRequest) of running asynchronously execute a request with SOURCEHANDLE and
       operation." The handle and not the source: `request.source` is the IDBObjectStore or IDBIndex the page
       called the member on. */
    req = idb_request_execute(ctx, source_handle, w->tx, op);                 /* STEP 11 */
    JS_FreeValue(ctx, op);
    return req;
}

void idb_get_all_init(JSContext *ctx)
{
    DCHECK(g_get_all_stepid < 0, "idb_get_all_init ran twice — one instance is one agent");
    g_get_all_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_idb_get_all_def);
    agent_state_id("idb_get_all", &g_get_all_stepid,
                   "§6.2's and §6.3's retrieve-multiple machine, and the declaration latch");
}

void idb_get_all_free(JSRuntime *rt)
{
    (void)rt;
    DCHECK(g_get_all_stepid >= 0, "§5.12's machinery was released in an agent that never declared it");
    g_get_all_stepid = -1;
}
