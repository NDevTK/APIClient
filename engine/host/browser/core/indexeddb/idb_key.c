/* INDEXED DATABASE §2.4's KEY — the value the whole standard is defined over, built as real state.
 *
 * WHY THIS IS THE FIRST FILE OF INDEXED DATABASE AND NOT THE DATABASE. §2.2 says an object store "has a list of
 * records ... The list is sorted according to key in ascending order", §2.9 says a key range is "a continuous
 * interval over some data type used for keys", §2.10's cursor walks records in that order and §2.6's index maps
 * a key to a key. Not one of those sentences can be written down before "compare two keys" exists — a store
 * built first would be a list with no order, which is not the thing §2.2 describes. So the key is subproblem
 * one, and the store is written against it rather than beside it.
 *
 * A KEY IS A JS VALUE, WHICH IS THE LOAD-BEARING DECISION AND IS THE SAME ONE core/file/file_system.c MADE.
 * Keys are stored: they are what a record is filed under, what a cursor's position is, what an index's own
 * records hold as their value. That state is written by a FLOW — one flow's `put` must not be visible to its
 * sibling — and it must PARK to the cold tier and resume byte-identical. §State-isolation names exactly one
 * primitive for that, the per-flow COW delta over PROPERTY WRITES, and names the failure mode of the
 * alternative: a malloc'd record captured as a pointer reverts the POINTER on a context switch and leaves what
 * it named reachable from nothing. So a key is an internal-slot record (core/idl_slots.h) carrying §2.4's own
 * two fields — "an associated type" and "an associated value" — and every write to one is a property write the
 * delta already captures, the snapshot machinery already carries and the cold tier already knows how to park.
 *
 * AND IT IS WHAT LETS A KEY BE ATTACKER INPUT. A page keys its records by things it read from the address bar
 * and from messages — `store.get(location.hash.slice(1))` is the ordinary shape — so a key's value is external
 * input in exactly the sense a fragment is. A concolic cannot ride a `double` or a `char *`; it can ride a
 * JSValue, so the value on the record IS the concolic and the taint reaches whatever the key is later compared
 * with, stored under, or handed back to the page as. Reading one back must not force a branch, which is why
 * the comparison below takes a concolic's EXAMPLE rather than testing what the concolic IS. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
/* JS_GetArrayBufferView — Web IDL's `BufferSource` as ONE read, which §7.4's buffer-source arm is stated over
   and which a DataView satisfies without being a typed array. It is declared here rather than in quickjs.h. */
#include "quickjs-step.h"
#include "cutils.h"
#include "core/idl_slots.h"
#include "core/indexeddb/idb_key.h"
#include "solver/concolic.h"

/* §2.4's TWO FIELDS, on the record a key IS. Spelled once so the writers and the readers cannot disagree. */
#define IDB_KEY_TYPE  "type"
#define IDB_KEY_VALUE "value"

/* §2.4's FIVE TYPES, and their ORDER. The standard states the ordering as a cascade in "compare two keys"
   ("If ta is array, then return 1. If tb is array, then return -1. If ta is binary ..."), and then states its
   own consequence as a note: "Number keys are less than date keys. Date keys are less than string keys. String
   keys are less than binary keys. Binary keys are less than array keys." The cascade and the note are one
   fact, so it is written once, as the rank — a cascade written out again here would be a second place for the
   five to get out of step, and the enumerators below ARE the note in order. */
enum { IDB_RANK_NUMBER = 0, IDB_RANK_DATE, IDB_RANK_STRING, IDB_RANK_BINARY, IDB_RANK_ARRAY };
static const char *const IDB_TYPE_NAME[] = { "number", "date", "string", "binary", "array" };
#define IDB_RANK_N ((int)(sizeof IDB_TYPE_NAME / sizeof IDB_TYPE_NAME[0]))

/* ---- the key record --------------------------------------------------------------------------------------- */

/* THE LENGTH OF ONE OF THIS FILE'S OWN LISTS — an array key's value, which is §2.4's "a list of other keys" and
   is a plain Array this component built. It runs none of the page's code. */
static uint32_t idb_key_list_len(JSContext *ctx, JSValueConst list)
{
    JSValue len = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0;
    int r;

    DCHECK(!JS_IsException(len), "reading the length of an array key's list of subkeys threw — it is a plain "
                                 "Array this component built and has no getters to run");
    r = JS_ToUint32(ctx, &n, len);
    DCHECK(r >= 0, "an array key's list of subkeys had a length that is not a number");
    (void)r;
    JS_FreeValue(ctx, len);
    return n;
}

/* A NEW KEY. Every constructor goes through it so a key cannot exist with a type this file does not know, and
   `value` is CONSUMED. */
static JSValue idb_key_new(JSContext *ctx, int rank, JSValue value)
{
    JSValue k = idl_slots_new(ctx);

    DCHECK(rank >= 0 && rank < IDB_RANK_N, "an IndexedDB key was built with a type §2.4 does not have");
    CHECK(!JS_IsException(k), "IndexedDB: a key record could not be allocated");
    JS_SetPropertyStr(ctx, k, IDB_KEY_TYPE, JS_NewString(ctx, IDB_TYPE_NAME[rank]));
    JS_SetPropertyStr(ctx, k, IDB_KEY_VALUE, value);
    return k;
}

/* §2.4's "let ta be the TYPE of a", as the rank the ordering is stated in. This is where a value that is not a
   key is caught: every caller reaches it, so no caller needs an assertion of its own. */
static int idb_key_rank(JSContext *ctx, JSValueConst key)
{
    JSValue t;
    const char *s;
    int rank = -1, i;

    DCHECK(JS_IsObject(key), "an IndexedDB key's TYPE was read off something that is not a key record");
    t = JS_GetPropertyStr(ctx, key, IDB_KEY_TYPE);
    s = JS_ToCString(ctx, t);
    if (s)
        for (i = 0; i < IDB_RANK_N; i++)
            if (!strcmp(s, IDB_TYPE_NAME[i])) { rank = i; break; }
    DCHECK(rank >= 0, "an IndexedDB key's TYPE is not one of §2.4's five — every key in this engine is built by "
                      "idb_key_new, which takes the type as the rank the ordering is stated in, so a key "
                      "wearing anything else was not built by this file");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, t);
    return rank < 0 ? IDB_RANK_NUMBER : rank;
}

/* §2.4's "let va be the VALUE of a". OWNED. */
static JSValue idb_key_value(JSContext *ctx, JSValueConst key)
{
    JSValue v = JS_GetPropertyStr(ctx, key, IDB_KEY_VALUE);

    CHECK(!JS_IsException(v), "IndexedDB: a key's VALUE could not be read");
    return v;
}

/* THE CONCRETE VALUE BEHIND A KEY'S VALUE. A key whose value is external input carries the concolic itself, and
   every operation that needs an actual number, string or byte takes its EXAMPLE — the same seam
   core/file/file_system.c's `file_system_value_bytes` opens with, for the same reason: the domain says what the
   value can be and the example says what it concretely is, and the taint is not lost by reading the second
   half, because the taint rides the value that stays ON the record. OWNED. */
static JSValue idb_concrete(JSContext *ctx, JSValueConst v)
{
    return concolic_is(v) ? concolic_example(ctx, v) : JS_DupValue(ctx, v);
}

/* ---- §7.4's CONVERT A VALUE TO A KEY ----------------------------------------------------------------------- */


/* §7.4's BUFFER SOURCE ARM — "If input is detached then return 'invalid value'. Let bytes be the result of
   getting a copy of the bytes held by the buffer source input. Return a new key with type binary and value
   bytes." A view contributes its OWN WINDOW of the buffer and not the whole of it, which is why the offset is
   read rather than assumed, and the bytes are COPIED because §2.4 makes the key's value a byte sequence: a
   later write through the page's view must not change a key already filed under it.
   DETACHMENT IS REPORTED BY THE ENGINE AS A THROW, and §7.4 answers it with "invalid value" instead — so the
   throw is taken and discarded here, at the one place the algorithm defines a different answer for it. That is
   not a swallowed error: it is this step's stated result replacing the engine's way of reporting the state. */
static IdbKeyResult idb_key_from_buffer_source(JSContext *ctx, JSValueConst input, JSValue *pkey)
{
    size_t off = 0, len = 0, whole = 0;
    JSValue buf;
    uint8_t *base;

    if (JS_IsArrayBuffer(input)) {
        buf = JS_DupValue(ctx, input);
    } else {
        buf = JS_GetArrayBufferView(ctx, input, &off, &len);
        if (JS_IsException(buf)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return IDB_KEY_INVALID_VALUE;
        }
    }
    base = JS_GetArrayBuffer(ctx, &whole, buf);
    if (!base) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, buf);
        return IDB_KEY_INVALID_VALUE;
    }
    if (JS_IsArrayBuffer(input)) { off = 0; len = whole; }
    DCHECK(off + len <= whole, "an ArrayBufferView's window reaches past the buffer it views");
    *pkey = idb_key_new(ctx, IDB_RANK_BINARY, JS_NewArrayBufferCopy(ctx, base + off, len));
    JS_FreeValue(ctx, buf);
    return IDB_KEY_OK;
}

JSValue idb_key_new_array(JSContext *ctx, JSValue keys)
{
    DCHECK(JS_IsArray(keys), "§7.4 step 6's \"a new array key with value keys\" was given something that is not "
                             "a list of keys — §2.4's value for type array is \"a list of other keys\", and "
                             "this engine's list is a plain Array");
    return idb_key_new(ctx, IDB_RANK_ARRAY, keys);
}

JSValue idb_key_new_number(JSContext *ctx, double value)
{
    DCHECK(!isnan(value), "§2.4's NUMBER key was minted carrying NaN. §7.4's number arm answers \"invalid "
                          "value\" for one, so no key of that type can hold it and §2.4's compare over it "
                          "would answer neither -1, 0 nor 1");
    return idb_key_new(ctx, IDB_RANK_NUMBER, JS_NewFloat64(ctx, value));
}

bool idb_key_is_number(JSContext *ctx, JSValueConst key, double *pvalue)
{
    JSValue v, c;
    int r;

    if (idb_key_rank(ctx, key) != IDB_RANK_NUMBER)   /* §2.11 STEP 1 */
        return false;
    v = idb_key_value(ctx, key);                     /* §2.11 STEP 2 */
    c = idb_concrete(ctx, v);
    r = JS_ToFloat64(ctx, pvalue, c);
    DCHECK(r >= 0, "a key of type NUMBER carries a value that is not a number — §7.4's number arm is the only "
                   "thing that builds one, and where it stores the concolic instead it does so only after the "
                   "EXAMPLE answered that same arm");
    (void)r;
    JS_FreeValue(ctx, c);
    JS_FreeValue(ctx, v);
    return true;
}

/* §7.4's ARMS THAT ARE NOT THE ARRAY ONE, over a value that is not a concolic. Each is decided by asking what
   the value IS and answers in one O(1) engine action, which is exactly why they are a call and the array arm is
   a walk. */
static IdbKeyResult idb_key_concrete_arm(JSContext *ctx, JSValueConst input, JSValue *pkey, JSValue *parray)
{
    /* "If Type(input) is Number: if input is NaN then return 'invalid value'; otherwise return a new key with
       type number and value input." Type(input) is Number and not "is a Number object", so `new Number(1)` is
       NOT a key — which is what a page's `store.get(new Number(1))` must fail on. */
    if (JS_IsNumber(input)) {
        double d = 0;

        JS_ToFloat64(ctx, &d, input);
        if (isnan(d)) return IDB_KEY_INVALID_VALUE;
        *pkey = idb_key_new(ctx, IDB_RANK_NUMBER, JS_NewFloat64(ctx, d));
        return IDB_KEY_OK;
    }
    /* "If input is a Date (has a [[DateValue]] internal slot): let ms be the value of input's [[DateValue]]
       internal slot; if ms is NaN then return 'invalid value'; otherwise return a new key with type date and
       value ms." The INTERNAL SLOT, never `getTime()` — that is the page's method to replace, and this engine's
       accessor for the slot exists precisely so no C entry calls one. */
    if (JS_IsDate(input)) {
        double ms = JS_GetDateValue(input);

        if (isnan(ms)) return IDB_KEY_INVALID_VALUE;
        *pkey = idb_key_new(ctx, IDB_RANK_DATE, JS_NewFloat64(ctx, ms));
        return IDB_KEY_OK;
    }
    /* "If Type(input) is String: return a new key with type string and value input." */
    if (JS_IsString(input)) {
        *pkey = idb_key_new(ctx, IDB_RANK_STRING, JS_DupValue(ctx, input));
        return IDB_KEY_OK;
    }
    /* "If input is a buffer source type" — Web IDL's `(ArrayBuffer or ArrayBufferView)`, and a DataView is one
       of those even though it is not a typed array. */
    if (JS_IsArrayBuffer(input) || JS_GetTypedArrayType(input) >= 0 || JS_IsDataView(input))
        return idb_key_from_buffer_source(ctx, input, pkey);
    /* "If input is an Array exotic object" — a Proxy is not one, which is why the engine's own test is the
       right one here and `Array.isArray`'s proxy-piercing behaviour would be the wrong one. The arm's own steps
       run the PAGE'S code, so they are not answered here: the array is handed back and core/indexeddb/
       idb_key_array.h's walk performs them inside whichever member is running. */
    if (JS_IsArray(input)) {
        *parray = JS_DupValue(ctx, input);
        return IDB_KEY_ARRAY;
    }
    return IDB_KEY_INVALID_TYPE;   /* "Otherwise: return 'invalid type'." */
}

/* §7.4's CONCOLIC ARM, which the standard does not have because a browser has no unknown values and this engine
 * does. A page keys its records by what it read from the address, from a message, from a reply — so the value
 * arriving here is routinely one whose DOMAIN is unknown and whose EXAMPLE is a real string or number. §7.4
 * decides the key's TYPE by asking what the value IS, and asking that of a concolic answers "an object", which
 * would make every attacker-derived key a "DataError" and take the whole surface out of reach.
 *
 * So the TYPE is decided from the example — that is what the example is for — and the VALUE the key carries is
 * the CONCOLIC ITSELF wherever §2.4's value for that type is a primitive the concolic can stand in for (number
 * and string). Nothing is de-tainted: the key hands that same value back through §7.3, compares through its
 * example, and reaches a sink carrying the fact that an attacker chose it.
 *
 * A DATE's value is its [[DateValue]], a BINARY key's is a copied byte sequence and an ARRAY key's is a list of
 * other keys, so none of the three is a place a concolic can ride; those take the concrete value out of the
 * example, which is the honest limit of a value model in which a Date is a Date. An array EXAMPLE still yields
 * a real array key whose SUBKEYS carry whatever concolics its elements were, which is where the taint stays.
 *
 * THE UNWRAP IS A LOOP AND NOT A RECURSIVE CALL. It was a call into the entry below, which is a C cycle, and
 * engine/check_recursion.mjs counts those over the whole program; asking the same question of the example in a
 * loop says the same thing with no frame. */
IdbKeyResult idb_key_convert_here(JSContext *ctx, JSValueConst input, JSValue *pkey, JSValue *parray)
{
    JSValueConst v = input;
    JSValue held = JS_UNDEFINED;      /* the example standing in for a concolic (owned) */
    IdbKeyResult res;

    *pkey = JS_UNDEFINED;
    *parray = JS_UNDEFINED;

    /* THE CONCOLIC ARM COMES FIRST because a concolic is an OBJECT: every test in the arm above would answer
       "no" for one and it would fall out of the bottom as "invalid type". */
    while (concolic_is(v)) {
        JSValue ex = concolic_example(ctx, v);

        if (JS_IsUndefined(ex)) {
            JS_FreeValue(ctx, ex);
            JS_FreeValue(ctx, held);
            /* TWO STATES WEAR THIS SHAPE and neither is built. A concolic with NO example yet is a value whose
               §7.4 answer is not decidable at all — it is a key on one arm and a "DataError" on the other, so
               the step must FORK and explore both, which is the flow the solver owes here. A concolic whose
               example genuinely IS `undefined` has a decided answer ("invalid type"), and concolic_example
               reports the two identically, so the fork cannot be written without an accessor that tells them
               apart. */
            DFAIL("Indexed Database §7.4 reached a concolic with no EXAMPLE: whether it is a valid key is "
                  "undecided, so the step must FORK a key arm and an invalid arm rather than answer one of "
                  "them — and concolic_example cannot yet distinguish an absent example from an `undefined` "
                  "one, which is the accessor that fork needs first");
            return IDB_KEY_INVALID_TYPE;
        }
        JS_FreeValue(ctx, held);
        held = ex;
        v = held;
    }
    res = idb_key_concrete_arm(ctx, v, pkey, parray);
    if (res == IDB_KEY_OK && concolic_is(input)) {
        int rank = idb_key_rank(ctx, *pkey);

        if (rank == IDB_RANK_NUMBER || rank == IDB_RANK_STRING)
            JS_SetPropertyStr(ctx, *pkey, IDB_KEY_VALUE, JS_DupValue(ctx, input));
    }
    JS_FreeValue(ctx, held);
    return res;
}

IdbKeyResult idb_key_convert(JSContext *ctx, JSValueConst input, JSValue *pkey)
{
    JSValue arr = JS_UNDEFINED;
    IdbKeyResult r = idb_key_convert_here(ctx, input, pkey, &arr);

    if (r != IDB_KEY_ARRAY)
        return r;
    JS_FreeValue(ctx, arr);
    /* THE C ENTRY REACHED §7.4's ARRAY ARM, whose steps are the page's own code and which therefore exists in
       exactly one form: the parkable walk in core/indexeddb/idb_key_array.h. There is nothing to fall back to
       and this is not a fallback — it is a caller with no flow base, named. */
    DFAIL("Indexed Database §7.4's ARRAY arm reached idb_key_convert's C entry, which has no flow under it to "
          "run the page's index accessors on. Every member of §4 drives the walk instead — §4.3's cmp, §4.7's "
          "five, and §4.5's add-or-put, get, getKey, delete and count through §2.9's convert-a-value-to-a-key-"
          "range and §7.1's extract-a-key, each of which declares the algorithm's stage block with "
          "IDB_KEY_ARRAY_ALGO_STAGES, embeds an IdbKeyWalk and chains idb_key_walk_visit into its visit. So what "
          "stands here is a caller with no flow base at all: an in-C fixture, whose only route to an Array key "
          "is to become a flow");
    return IDB_KEY_INVALID_TYPE;
}

int idb_key_from_value(JSContext *ctx, JSValueConst input, JSValue *pkey)
{
    if (idb_key_convert(ctx, input, pkey) == IDB_KEY_OK) return 0;
    JS_ThrowDOMException(ctx, "DataError", "the value is not a valid IndexedDB key");
    return -1;
}

/* ---- §7.3's CONVERT A KEY TO A VALUE ----------------------------------------------------------------------- */

/* §7.3's FOUR ARMS THAT ARE NOT THE ARRAY ONE, over a key whose rank the caller has already read. Factored out
   so that neither the array walk nor the entry below is on a call CYCLE with the other — a leaf both of them
   reach, rather than an entry the walk calls back into, which engine/check_recursion.mjs would report over the
   whole program as C recursion whatever its actual depth. */
static JSValue idb_key_scalar_to_value(JSContext *ctx, int rank, JSValueConst key)
{
    JSValue v = idb_key_value(ctx, key), out;

    switch (rank) {
    /* "Return an ECMAScript Number value equal to value" / "a String value equal to value" — and where the
       value is a concolic standing in for that number or string, the page gets the concolic, which is what
       keeps `range.lower` an attacker-derived value rather than a laundered copy of one. */
    case IDB_RANK_NUMBER:
    case IDB_RANK_STRING:
        return v;
    /* "Let date be the result of executing the ECMAScript Date constructor with the single argument value.
       Assert: date is not an abrupt completion. Return date." */
    case IDB_RANK_DATE: {
        JSValue c = idb_concrete(ctx, v);
        double ms = 0;

        JS_ToFloat64(ctx, &ms, c);
        JS_FreeValue(ctx, c);
        JS_FreeValue(ctx, v);
        out = JS_NewDate(ctx, ms);
        CHECK(!JS_IsException(out), "IndexedDB: §7.3's Date could not be allocated");
        return out;
    }
    /* "Let buffer be the result of executing the ECMAScript ArrayBuffer constructor with len ... Set the
       entries in buffer's [[ArrayBufferData]] internal slot to the entries in value." A NEW buffer every time,
       which is the whole point of the step: a page that writes through `range.lower` must not reach the bytes
       the key is filed under. */
    case IDB_RANK_BINARY: {
        size_t len = 0;
        uint8_t *bytes = JS_GetArrayBuffer(ctx, &len, v);

        CHECK(bytes != NULL || len == 0,
              "IndexedDB: a binary key's byte sequence could not be read — the buffer is one this component "
              "allocated and never published, so nothing can have detached it");
        out = JS_NewArrayBufferCopy(ctx, bytes ? bytes : (const uint8_t *)"", len);
        JS_FreeValue(ctx, v);
        CHECK(!JS_IsException(out), "IndexedDB: §7.3's ArrayBuffer could not be allocated");
        return out;
    }
    default:
        JS_FreeValue(ctx, v);
        DFAIL("Indexed Database §7.3's scalar conversion was asked for the ARRAY rank, whose steps build an "
              "Array and are the walk below — the entry routes the two apart and is the only caller of either");
        return JS_UNDEFINED;
    }
}

/* §7.3's ARRAY ARM, AS A WALK OVER AN EXPLICIT STACK.
 *
 * "Let array be the result of executing the ECMAScript Array constructor with no arguments … let len be value's
 * size … while index is less than len: let entry be the result of converting a key to a value with value[index];
 * let status be CreateDataProperty(array, index, entry); assert: status is true … return array."
 *
 * NOT C RECURSION, and not as a matter of style: an array key's DEPTH is whatever the page nested, so a C frame
 * per level is a stack whose height the page picks, and engine/check_recursion.mjs reports one over the whole
 * program. A level holds exactly what the recursive call would have — the subkey list, the Array being filled,
 * and ONE cursor into both, because keys[index] becomes array[index].
 *
 * IT IS NOT A STEP MACHINE, WHICH IS A STATEMENT ABOUT THESE STEPS AND NOT A CONCESSION. Every one of them is an
 * engine action over data the ENGINE owns: §7.4 refused anything that was not a key, so an array key's subkeys
 * are key records this component built, and a fresh Array's CreateDataProperty is an ordinary define. There is
 * no page code here to give a flow base to, exactly as there is none in the string and byte-sequence
 * comparisons below. The Array it returns is unreachable until the caller hands it back, so nothing can observe
 * it half-built. */
static JSValue idb_array_to_value(JSContext *ctx, JSValueConst key)
{
    typedef struct { JSValue keys, out; uint32_t n, i; } Level;
    Level *st = NULL;
    int sp = 0, cap = 0;
    /* THE LIST A LEVEL IS ABOUT TO BE PUSHED FOR — the whole of the descent, and JS_UNDEFINED when there is
       none, which an array key's value can never be. */
    JSValue pending = idb_key_value(ctx, key);

    for (;;) {
        Level *f;

        if (!JS_IsUndefined(pending)) {
            if (sp == cap) {
                int want = cap ? cap * 2 : 4;
                Level *a = realloc(st, sizeof(Level) * (size_t)want);

                CHECK(a != NULL, "IndexedDB: §7.3's array arm could not grow its level stack — the nesting is "
                                 "the key's own, and a dropped level would hand the page an array with a "
                                 "subkey missing from it");
                st = a;
                cap = want;
            }
            f = &st[sp++];
            f->keys = pending;
            pending = JS_UNDEFINED;
            f->out = JS_NewArray(ctx);
            CHECK(!JS_IsException(f->out), "IndexedDB: §7.3's Array could not be allocated");
            f->n = idb_key_list_len(ctx, f->keys);
            f->i = 0;
            continue;
        }
        f = &st[sp - 1];
        if (f->i < f->n) {
            JSValue sub = JS_GetPropertyUint32(ctx, f->keys, f->i);
            int srank = idb_key_rank(ctx, sub);

            /* A SUBKEY THAT IS ITSELF AN ARRAY KEY is the recursive call, which is a descent on the next turn. */
            if (srank == IDB_RANK_ARRAY) {
                pending = idb_key_value(ctx, sub);
                JS_FreeValue(ctx, sub);
                continue;
            }
            {
                JSValue entry = idb_key_scalar_to_value(ctx, srank, sub);

                JS_FreeValue(ctx, sub);
                CHECK(JS_DefinePropertyValueUint32(ctx, f->out, f->i, entry, JS_PROP_C_W_E) >= 0,
                      "IndexedDB: §7.3's CreateDataProperty on its own fresh Array failed");
            }
            f->i++;
            continue;
        }
        /* THE LEVEL IS FULL: its Array is this algorithm's answer, or the parent's next entry. */
        {
            JSValue done = f->out;

            JS_FreeValue(ctx, f->keys);
            sp--;
            if (sp == 0) {
                free(st);
                return done;
            }
            f = &st[sp - 1];
            CHECK(JS_DefinePropertyValueUint32(ctx, f->out, f->i, done, JS_PROP_C_W_E) >= 0,
                  "IndexedDB: §7.3's CreateDataProperty on its own fresh Array failed");
            f->i++;
        }
    }
}

JSValue idb_key_to_value(JSContext *ctx, JSValueConst key)
{
    int rank = idb_key_rank(ctx, key);

    if (rank == IDB_RANK_ARRAY)
        return idb_array_to_value(ctx, key);
    return idb_key_scalar_to_value(ctx, rank, key);
}

/* §2.4's ARRAY KEY, asked and unpacked. It is the ONE type question this file answers for another component:
   §6.1 step 5's two multiEntry arms turn on "index key is an array key" and then walk "the subkeys of index
   key", which is that key's value. The unpack asserts the type rather than answering for a scalar, because the
   condition that selects the arm is the same question and a caller holding "the subkeys of a number" has
   nothing it could correctly do with them. */
bool idb_key_is_array(JSContext *ctx, JSValueConst key)
{
    return idb_key_rank(ctx, key) == IDB_RANK_ARRAY;
}

JSValue idb_key_subkeys(JSContext *ctx, JSValueConst key)
{
    JSValue subkeys;

    DCHECK(idb_key_rank(ctx, key) == IDB_RANK_ARRAY,
           "the SUBKEYS of a key that is not an array key were asked for — §2.4 gives a list of other keys to "
           "the array type alone, and idb_key_is_array is how that is asked");
    subkeys = idb_key_value(ctx, key);
    DCHECK(JS_IsArray(subkeys), "an array key's value is not a list of subkeys");
    return subkeys;
}

/* ---- §2.4's COMPARE TWO KEYS -------------------------------------------------------------------------------- */

/* "If va is CODE UNIT LESS THAN vb, then return -1" — Infra's code-unit ordering over UTF-16, which is NOT the
 * ordering of the UTF-8 the engine stores a string as. They disagree for exactly the strings a page is most
 * likely to have got out of an attacker: a supplementary character encodes as a lead surrogate in D800..DBFF
 * and therefore sorts BELOW U+E000..U+FFFF in code units, while its code point sorts above them, and UTF-8 byte
 * order is code POINT order. Getting that wrong puts a record in the wrong place in §2.2's list and a cursor
 * then walks past it, so the units are materialised and compared as units.
 * The decode is the engine's own (cutils.h's utf8_decode_buf16, the same one JS_NewStringUTF16 is the inverse
 * of), asked twice: once for the length, once to fill.
 * IT IS EXPORTED because §2's create-a-sorted-name-list is stated over the SAME Infra ordering — "sorted in
 * ascending order with the code unit less than algorithm" — and a second implementation of it would be a second
 * answer to one question, disagreeing on exactly the strings above. */
int idb_code_unit_compare(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    size_t alen = 0, blen = 0, an, bn, n, i;
    const char *au = JS_ToCStringLen(ctx, &alen, a);
    const char *bu = JS_ToCStringLen(ctx, &blen, b);
    uint16_t *ac, *bc;
    int r = 0;

    CHECK(au != NULL && bu != NULL, "IndexedDB: a string key's value could not be read");
    an = utf8_decode_buf16(NULL, 0, au, alen);
    bn = utf8_decode_buf16(NULL, 0, bu, blen);
    ac = malloc((an + 1) * sizeof *ac);
    bc = malloc((bn + 1) * sizeof *bc);
    CHECK(ac != NULL && bc != NULL, "IndexedDB: OOM comparing two string keys");
    utf8_decode_buf16(ac, an, au, alen);
    utf8_decode_buf16(bc, bn, bu, blen);
    n = an < bn ? an : bn;
    for (i = 0; i < n; i++)
        if (ac[i] != bc[i]) { r = ac[i] < bc[i] ? -1 : 1; break; }
    if (r == 0 && an != bn) r = an < bn ? -1 : 1;
    free(ac);
    free(bc);
    JS_FreeCString(ctx, au);
    JS_FreeCString(ctx, bu);
    return r;
}

/* "If va is BYTE LESS THAN vb, then return -1" — Infra's byte-sequence ordering, whose own note the standard
   repeats: "Members of binary keys are compared as unsigned byte values ... rather than signed byte values."
   memcmp is unsigned, which is what makes it the right primitive and not merely a fast one. */
static int idb_binary_compare(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    size_t alen = 0, blen = 0, n;
    uint8_t *ab = JS_GetArrayBuffer(ctx, &alen, a);
    uint8_t *bb = JS_GetArrayBuffer(ctx, &blen, b);
    int c;

    CHECK((ab != NULL || alen == 0) && (bb != NULL || blen == 0),
          "IndexedDB: a binary key's byte sequence could not be read — §7.4 copies the bytes into a buffer this "
          "component allocated and never published, so nothing can have detached one");
    n = alen < blen ? alen : blen;
    c = (n && ab && bb) ? memcmp(ab, bb, n) : 0;
    if (c) return c < 0 ? -1 : 1;
    if (alen == blen) return 0;
    return alen < blen ? -1 : 1;
}

/* §2.4's FOUR ARMS THAT ARE NOT THE ARRAY ONE, over two keys of the same rank. Factored out for the reason
   §7.3's scalar arms are: it is the leaf both the array walk and the entry reach, so neither is on a call cycle
   with the other. */
static int idb_key_scalar_compare(JSContext *ctx, int rank, JSValueConst a, JSValueConst b)
{
    JSValue ha = idb_key_value(ctx, a), hb = idb_key_value(ctx, b);
    JSValue va = idb_concrete(ctx, ha), vb = idb_concrete(ctx, hb);
    int r;

    switch (rank) {
    /* "number / date: if va is greater than vb, then return 1; if va is less than vb, then return -1; return
       0." One arm for both, which is the standard's own — a date key's value IS an unrestricted double. */
    case IDB_RANK_NUMBER:
    case IDB_RANK_DATE: {
        double da = 0, db = 0;

        JS_ToFloat64(ctx, &da, va);
        JS_ToFloat64(ctx, &db, vb);
        r = da > db ? 1 : (da < db ? -1 : 0);
        break;
    }
    case IDB_RANK_STRING:
        r = idb_code_unit_compare(ctx, va, vb);
        break;
    case IDB_RANK_BINARY:
        r = idb_binary_compare(ctx, va, vb);
        break;
    default:
        r = 0;
        DFAIL("Indexed Database §2.4's scalar compare was asked for the ARRAY rank, whose steps walk two lists "
              "of subkeys and are the walk below — the entry routes the two apart and is the only caller of "
              "either");
        break;
    }
    JS_FreeValue(ctx, va);
    JS_FreeValue(ctx, vb);
    JS_FreeValue(ctx, ha);
    JS_FreeValue(ctx, hb);
    return r;
}

/* §2.4's ARRAY ARM, AS A WALK OVER AN EXPLICIT STACK.
 *
 * "Let length be the lesser of va's size and vb's size. Let i be 0. While i is less than length: let c be the
 * result of recursively comparing two keys with va[i] and vb[i]; if c is not 0, return c; increase i by 1. If
 * va's size is greater than vb's size, then return 1. If va's size is less than vb's size, then return -1.
 * Return 0."
 *
 * A LEVEL IS THE RECURSIVE CALL, for §7.3's reason: the depth is the page's, so a C frame per level is a stack
 * whose height the page picks. The propagation is what the shape has to get right — a non-zero c returns from
 * EVERY level at once ("if c is not 0, return c"), so a decided answer abandons the whole stack, while a level
 * that runs out of subkeys with everything equal falls through to the SIZES and only a 0 there pops. */
static int idb_array_compare(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    typedef struct { JSValue la, lb; uint32_t na, nb, n, i; } Level;
    Level *st = NULL;
    int sp = 0, cap = 0, r = 0;
    JSValue pa = idb_key_value(ctx, a), pb = idb_key_value(ctx, b);   /* the pair a level is pushed for */
    bool decided = false;

    while (!decided) {
        Level *f;

        if (!JS_IsUndefined(pa)) {
            if (sp == cap) {
                int want = cap ? cap * 2 : 4;
                Level *g = realloc(st, sizeof(Level) * (size_t)want);

                CHECK(g != NULL, "IndexedDB: §2.4's array arm could not grow its level stack — the nesting is "
                                 "the keys' own, and a dropped level would order two records by a prefix");
                st = g;
                cap = want;
            }
            f = &st[sp++];
            f->la = pa;
            f->lb = pb;
            pa = pb = JS_UNDEFINED;
            f->na = idb_key_list_len(ctx, f->la);
            f->nb = idb_key_list_len(ctx, f->lb);
            f->n = f->na < f->nb ? f->na : f->nb;
            f->i = 0;
            continue;
        }
        f = &st[sp - 1];
        if (f->i < f->n) {
            JSValue ka = JS_GetPropertyUint32(ctx, f->la, f->i), kb = JS_GetPropertyUint32(ctx, f->lb, f->i);
            int rka = idb_key_rank(ctx, ka), rkb = idb_key_rank(ctx, kb);

            f->i++;
            if (rka != rkb) {
                r = rka > rkb ? 1 : -1;      /* §2.4's cascade, at the subkey */
                decided = true;
            } else if (rka == IDB_RANK_ARRAY) {
                pa = idb_key_value(ctx, ka);
                pb = idb_key_value(ctx, kb);
            } else if ((r = idb_key_scalar_compare(ctx, rka, ka, kb)) != 0) {
                decided = true;
            }
            JS_FreeValue(ctx, ka);
            JS_FreeValue(ctx, kb);
            continue;
        }
        /* Every subkey up to the shorter list's size compared equal, so the SIZES decide this level. */
        if (f->na != f->nb) {
            r = f->na > f->nb ? 1 : -1;
            decided = true;
            continue;
        }
        JS_FreeValue(ctx, f->la);
        JS_FreeValue(ctx, f->lb);
        sp--;
        if (sp == 0)
            break;                            /* "return 0" — the two keys are equal */
    }
    /* A DECIDED ANSWER ABANDONS EVERY LEVEL, which is what "return c" from inside the recursion means. */
    while (sp > 0) {
        JS_FreeValue(ctx, st[sp - 1].la);
        JS_FreeValue(ctx, st[sp - 1].lb);
        sp--;
    }
    JS_FreeValue(ctx, pa);
    JS_FreeValue(ctx, pb);
    free(st);
    return r;
}

int idb_key_compare(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    int ra = idb_key_rank(ctx, a), rb = idb_key_rank(ctx, b);

    /* "If ta does not equal tb" — the cascade, as the rank the five types are declared in. */
    if (ra != rb) return ra > rb ? 1 : -1;
    if (ra == IDB_RANK_ARRAY) return idb_array_compare(ctx, a, b);
    return idb_key_scalar_compare(ctx, ra, a, b);
}
