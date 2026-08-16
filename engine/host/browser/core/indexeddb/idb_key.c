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
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
/* JS_GetArrayBufferView — Web IDL's `BufferSource` as ONE read, which §7.4's buffer-source arm is stated over
   and which a DataView satisfies without being a typed array. It is declared here rather than in quickjs.h. */
#include "quickjs-step.h"
#include "cutils.h"
#include "libunicode.h"
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

/* The three answers §7.4 gives that are not an exception. */
typedef enum { IDB_KEY_OK = 0, IDB_KEY_INVALID_VALUE, IDB_KEY_INVALID_TYPE } IdbKeyResult;

static IdbKeyResult idb_convert_to_key(JSContext *ctx, JSValueConst input, JSValue *pkey);

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
 * A DATE's value is its [[DateValue]] and a BINARY key's is a copied byte sequence, so neither is a place a
 * concolic can ride; those two take the concrete value out of the example, which is the honest limit of a value
 * model in which a Date is a Date. */
static IdbKeyResult idb_key_from_concolic(JSContext *ctx, JSValueConst input, JSValue *pkey)
{
    JSValue ex = concolic_example(ctx, input);
    IdbKeyResult r;

    if (JS_IsUndefined(ex)) {
        JS_FreeValue(ctx, ex);
        /* TWO STATES WEAR THIS SHAPE and neither is built. A concolic with NO example yet is a value whose
           §7.4 answer is not decidable at all — it is a key on one arm and a "DataError" on the other, so the
           step must FORK and explore both, which is the flow the solver owes here. A concolic whose example
           genuinely IS `undefined` has a decided answer ("invalid type"), and concolic_example reports the two
           identically, so the fork cannot be written without an accessor that tells them apart. */
        DFAIL("Indexed Database §7.4 reached a concolic with no EXAMPLE: whether it is a valid key is undecided, "
              "so the step must FORK a key arm and an invalid arm rather than answer one of them — and "
              "concolic_example cannot yet distinguish an absent example from an `undefined` one, which is the "
              "accessor that fork needs first");
        return IDB_KEY_INVALID_TYPE;
    }
    r = idb_convert_to_key(ctx, ex, pkey);
    JS_FreeValue(ctx, ex);
    if (r == IDB_KEY_OK) {
        int rank = idb_key_rank(ctx, *pkey);

        if (rank == IDB_RANK_NUMBER || rank == IDB_RANK_STRING)
            JS_SetPropertyStr(ctx, *pkey, IDB_KEY_VALUE, JS_DupValue(ctx, input));
    }
    return r;
}

static IdbKeyResult idb_convert_to_key(JSContext *ctx, JSValueConst input, JSValue *pkey)
{
    *pkey = JS_UNDEFINED;

    /* THE CONCOLIC ARM COMES FIRST because a concolic is an OBJECT: every test below would answer "no" for one
       and it would fall out of the bottom as "invalid type". */
    if (concolic_is(input))
        return idb_key_from_concolic(ctx, input, pkey);

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
       right one here and `Array.isArray`'s proxy-piercing behaviour would be the wrong one. */
    if (JS_IsArray(input)) {
        DFAIL("Indexed Database §7.4's ARRAY arm is not built. Its steps are ToLength(Get(input, \"length\")), "
              "then one HasOwnProperty and one Get per element, then a recursive conversion of each — every one "
              "of which is the PAGE'S code (an index accessor, a length getter), so it is a declared step "
              "machine carrying §7.4's own `seen` set as a STACK OF CURSORS the way IDL_SEQUENCE_STRING_OR_DICT "
              "does, and never a C walk. Build it before any member that takes a compound key, and give §7.3 "
              "and §2.4's array arms their bodies in the same diff");
        return IDB_KEY_INVALID_TYPE;
    }
    return IDB_KEY_INVALID_TYPE;   /* "Otherwise: return 'invalid type'." */
}

int idb_key_from_value(JSContext *ctx, JSValueConst input, JSValue *pkey)
{
    if (idb_convert_to_key(ctx, input, pkey) == IDB_KEY_OK) return 0;
    JS_ThrowDOMException(ctx, "DataError", "the value is not a valid IndexedDB key");
    return -1;
}

/* ---- §7.3's CONVERT A KEY TO A VALUE ----------------------------------------------------------------------- */

JSValue idb_key_to_value(JSContext *ctx, JSValueConst key)
{
    int rank = idb_key_rank(ctx, key);
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
        DFAIL("Indexed Database §7.3's ARRAY arm is not built, because §7.4's is not: no array key can exist "
              "for it to convert. Its steps are an Array construction and one recursive conversion plus one "
              "CreateDataProperty per subkey, and it lands with §7.4's step machine");
        return JS_UNDEFINED;
    }
}

/* ---- §2.4's COMPARE TWO KEYS -------------------------------------------------------------------------------- */

/* "If va is CODE UNIT LESS THAN vb, then return -1" — Infra's code-unit ordering over UTF-16, which is NOT the
 * ordering of the UTF-8 the engine stores a string as. They disagree for exactly the strings a page is most
 * likely to have got out of an attacker: a supplementary character encodes as a lead surrogate in D800..DBFF
 * and therefore sorts BELOW U+E000..U+FFFF in code units, while its code point sorts above them, and UTF-8 byte
 * order is code POINT order. Getting that wrong puts a record in the wrong place in §2.2's list and a cursor
 * then walks past it, so the units are materialised and compared as units.
 * The decode is the engine's own (cutils.h's utf8_decode_buf16, the same one JS_NewStringUTF16 is the inverse
 * of), asked twice: once for the length, once to fill. */
static int idb_string_compare(JSContext *ctx, JSValueConst a, JSValueConst b)
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

int idb_key_compare(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    int ra = idb_key_rank(ctx, a), rb = idb_key_rank(ctx, b);
    JSValue ha, hb, va, vb;
    int r;

    /* "If ta does not equal tb" — the cascade, as the rank the five types are declared in. */
    if (ra != rb) return ra > rb ? 1 : -1;

    ha = idb_key_value(ctx, a);
    hb = idb_key_value(ctx, b);
    va = idb_concrete(ctx, ha);
    vb = idb_concrete(ctx, hb);
    switch (ra) {
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
        r = idb_string_compare(ctx, va, vb);
        break;
    case IDB_RANK_BINARY:
        r = idb_binary_compare(ctx, va, vb);
        break;
    default:
        r = 0;
        DFAIL("Indexed Database §2.4's ARRAY arm is not built, because §7.4's is not: no array key can exist "
              "for it to order. Its steps are a recursive compare of each subkey up to the shorter list's "
              "size, then the sizes, and it lands with §7.4's step machine");
        break;
    }
    JS_FreeValue(ctx, va);
    JS_FreeValue(ctx, vb);
    JS_FreeValue(ctx, ha);
    JS_FreeValue(ctx, hb);
    return r;
}

/* ---- §2.5's VALID KEY PATH ----------------------------------------------------------------------------------
 *
 * "An empty string. An identifier, which is a string matching the IdentifierName production from the ECMAScript
 * Language Specification. A string consisting of two or more identifiers separated by periods."
 *
 * THE IDENTIFIER IS ECMASCRIPT'S OWN AND SO IS THE TEST. IdentifierName is IdentifierStart IdentifierPart*, and
 * both are UNICODE properties (ID_Start / ID_Continue, plus `$`, `_`, and ZWNJ/ZWJ in the continue set) — so the
 * question is asked of the same table the JS lexer asks, `lre_is_id_start`/`lre_is_id_continue`, rather than of
 * an ASCII range that would refuse `store.createObjectStore("x",{keyPath:"café"})` a browser accepts. The
 * standard's own note — "spaces are not allowed within a key path" — is not a separate rule here: a space is
 * neither an IdentifierStart nor an IdentifierPart, so it is refused by the production itself.
 *
 * The `\uXXXX` UnicodeEscapeSequence arm of IdentifierName is NOT accepted, and that is the production being
 * followed rather than trimmed: §2.5's key path is compared against real property names by §7.1, and an escape
 * would name a property whose characters are the escaped ones — which is a DIFFERENT string from the one the
 * page wrote. No browser accepts one either.
 *
 * A LEADING OR TRAILING PERIOD, AND AN EMPTY SEGMENT, are refused because "two or more identifiers separated by
 * periods" means every segment is an identifier and an empty string is not one — while the WHOLE path being
 * empty IS valid, which is the standard's first arm and is why the empty case is answered before the walk. */
bool idb_key_path_is_valid(const char *path, size_t len)
{
    const uint8_t *p = (const uint8_t *)path, *end = p + len;
    bool at_segment_start = true;

    DCHECK(path != NULL, "§2.5's valid key path was asked of no string — a key path is a string, and the empty "
                         "string is one while a null pointer is not");
    if (len == 0)
        return true;   /* "An empty string." */
    while (p < end) {
        uint32_t c = utf8_decode_len(p, (size_t)(end - p), &p);

        if (c == '.') {
            /* A separator only ever follows a COMPLETE identifier, so a period at a segment's start is either
               a leading one or an empty segment between two periods. */
            if (at_segment_start)
                return false;
            at_segment_start = true;
            continue;
        }
        if (at_segment_start) {
            if (!(c == '$' || c == '_' || lre_is_id_start(c)))
                return false;
            at_segment_start = false;
            continue;
        }
        /* IdentifierPart, which the lexer's own table answers including the two zero-width joiners. */
        if (!(c == '$' || c == '_' || c == 0x200C || c == 0x200D || lre_is_id_continue(c)))
            return false;
    }
    /* A trailing period leaves the walk expecting an identifier that never arrived. */
    return !at_segment_start;
}
