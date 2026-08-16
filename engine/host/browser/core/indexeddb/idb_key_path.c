/* INDEXED DATABASE §2.5's KEY PATH — the ADDRESS of a key inside a value — and §7.1's walk of one.
 *
 * WHY THIS IS NOT idb_key.c. A KEY is a value: §2.4 says which values are keys, how two of them order, and how
 * one crosses to and from the page. A KEY PATH is an ADDRESS: §2.5 says which addresses exist and §7.1 resolves
 * one against a value the page supplied. Those are two contracts, and they meet at exactly one sentence — the
 * walk splits the string into the same segments the validity accepts, so a validity that accepted a segment the
 * walk cannot address, or a walk that addressed one the validity refuses, would be a silent disagreement
 * between two files. In one file it is a disagreement between two functions that sit twelve lines apart. The
 * key path's whole surface is here: §2.5's validity, §7.1's evaluate and §7.1's extract, and §7.2's inject and
 * its injectability check when the key generator that is their only caller exists.
 *
 * WHAT §7.1 IS FOR. It is the algorithm behind IN-LINE KEYS: a store created with a key path files each record
 * under a key read OUT of the value itself, so `store.put({id: 7, ...})` is a `put` with no key argument at all.
 * §4.5's `add or put` runs it, §6.1's index step runs it per index, and §4.8's cursor `update` runs it to check
 * the page has not moved the key — one algorithm, three callers, and the two that do not exist yet are §2.6's
 * index and §5.12's cursor rather than anything about this.
 *
 * THE VALUE IT WALKS IS THE PAGE'S, WHICH IS WHERE THE SOLVER MEETS IT. A bundle stores what it received —
 * `store.put(await res.json())`, `store.put(JSON.parse(location.hash.slice(1)))` — so the value, or a field of
 * it, is routinely a CONCOLIC: unknown external input carrying a source identity. §7.1's ordinary arm asks two
 * questions of it, "does it have this own property" and "what is at it", and the honest answer to both for an
 * unknown is another unknown. Answering `failure` instead would be FORCING A BRANCH on unknown input — and
 * forcing it toward the arm that takes the record out of the store entirely, which loses the code path and the
 * taint together. So a concolic answers with the DERIVED concolic its own exotic [[Get]] mints ("{reply}.id"),
 * the record is filed under a key that carries the fact an attacker chose it, and every later comparison on
 * that key forks as it should. Nothing here is de-tainted and nothing here concretizes. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "cutils.h"       /* utf8_decode_len / utf8_decode_buf16 — the engine's own UTF-8 reader */
#include "libunicode.h"   /* lre_is_id_start / lre_is_id_continue — the table the JS lexer asks */
#include "core/file/blob.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_key_path.h"
#include "solver/concolic.h"

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
 * followed rather than trimmed: §2.5's key path is compared against real property names by §7.1 below, and an
 * escape would name a property whose characters are the escaped ones — which is a DIFFERENT string from the one
 * the page wrote. No browser accepts one either.
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

/* ONE STRING OF A KEY PATH, read back as bytes for the walk above. The string is the one §3.2.25's conversion
   produced, so it is engine-owned and cannot run anything — a failure here is an allocation failure. */
static bool idb_key_path_string_is_valid(JSContext *ctx, JSValueConst v)
{
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, v);
    bool ok;

    CHECK(s != NULL, "IndexedDB: §2.5's key path could not be read back as a string");
    ok = idb_key_path_is_valid(s, len);
    JS_FreeCString(ctx, s);
    return ok;
}

/* HOW MANY ENTRIES A KEY PATH LIST HAS. The list is the one §3.2.25's union built, so it is the engine's own
   Array of strings with no getters to run and no hole to skip — a malformed one is this engine disagreeing with
   itself and crashes rather than reporting. */
static uint32_t idb_key_path_list_len(JSContext *ctx, JSValueConst path)
{
    JSValue len = JS_GetPropertyStr(ctx, path, "length");
    uint32_t n = 0;
    int r;

    DCHECK(!JS_IsException(len), "reading the length of the sequence §3.2.25 built threw — it is the engine's "
                                 "own Array and has no getters to run");
    r = JS_ToUint32(ctx, &n, len);
    DCHECK(r >= 0, "the sequence §3.2.25 built had a length that is not a number");
    (void)r;
    JS_FreeValue(ctx, len);
    return n;
}

bool idb_key_path_value_is_valid(JSContext *ctx, JSValueConst path)
{
    uint32_t i, n;

    if (JS_IsString(path))
        return idb_key_path_string_is_valid(ctx, path);
    DCHECK(JS_IsArray(path),
           "§2.5's validity was asked of a key path that is neither a string nor a sequence. Its declared type "
           "is §3.2.25's `(DOMString or sequence<DOMString>)`, which answers with exactly those two — and the "
           "IDL null a member may also be handed means the store has NO key path, which is a question its "
           "caller answers before it reaches this");
    n = idb_key_path_list_len(ctx, path);
    /* "A NON-EMPTY list" — see the header for why this is the list's own rule and not the loop's. */
    if (n == 0)
        return false;
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, path, i);
        bool ok;

        DCHECK(JS_IsString(e), "the sequence §3.2.25 built held something that is not a string — §3.2.20's "
                               "create-a-sequence-from-an-iterable converts every entry to its element type, "
                               "so `['a', ['b','c']]` arrives here as `['a', 'b,c']` and is refused for the "
                               "comma rather than crossing this line as a nested list");
        ok = idb_key_path_string_is_valid(ctx, e);
        JS_FreeValue(ctx, e);
        if (!ok)
            return false;
    }
    return true;
}

/* ---- §7.1's EVALUATE A KEY PATH ON A VALUE ------------------------------------------------------------------ */

/* ONE SEGMENT OF THE PATH against `value`. `ident` is a byte range of the key path string and NOT a C string —
   it is a slice of one, so it has no terminator and every comparison below is against its length too. */
static bool idb_segment_is(const char *ident, size_t len, const char *lit)
{
    return strlen(lit) == len && memcmp(ident, lit, len) == 0;
}

/* §7.1's "for each identifier of identifiers, jump to the appropriate step below" — ONE identifier, whose arms
 * are a CASCADE in the standard's own order. False is the algorithm's `failure`; on true `*pnext` is OWNED and
 * is the value the next identifier is resolved against.
 *
 * THE ARMS BEFORE "OTHERWISE" ARE WHY THIS IS NOT A PROPERTY READ. A String's `length`, an Array's `length`, a
 * Blob's `size` and `type` and a File's `name` and `lastModified` are read as the INTERNAL VALUES they are and
 * never through [[Get]], so a page that shadows `Blob.prototype.size` cannot change the key a record is filed
 * under, and the walk works on a primitive string, which has no [[Get]] of its own to make. The four Blob and
 * File arms are absent for the reason stated where the DCHECK that guards their absence stands. */
static bool idb_key_path_step(JSContext *ctx, JSValueConst value, const char *ident, size_t len, JSValue *pnext)
{
    JSAtom atom;
    int has;

    *pnext = JS_UNDEFINED;
    /* "If Type(value) is String, and identifier is 'length': let value be a Number equal to the NUMBER OF
       ELEMENTS in value." The elements of a JS string are UTF-16 CODE UNITS, which is not the number of bytes
       the engine holds it as — they disagree for every string with a non-ASCII character in it, and a key that
       counted bytes would file a record in the wrong place in §2.2's sorted list. So the units are counted with
       the engine's own decoder, the same one idb_key.c's code-unit comparison materialises with. */
    if (JS_IsString(value) && idb_segment_is(ident, len, "length")) {
        size_t blen = 0;
        const char *s = JS_ToCStringLen(ctx, &blen, value);

        CHECK(s != NULL, "IndexedDB: §7.1 could not read a string value's bytes");
        *pnext = JS_NewInt64(ctx, (int64_t)utf8_decode_buf16(NULL, 0, s, blen));
        JS_FreeCString(ctx, s);
        return true;
    }
    /* "If value is an Array and identifier is 'length': let value be ! ToLength(! Get(value, 'length'))." An
       Array exotic object's `length` is its own non-configurable data property, so the Get is a slot read and
       ToLength is the identity on the uint32 it holds — which is what the assertion below states. A Proxy is
       not an Array exotic object and JS_IsArray is the test that says so. */
    if (JS_IsArray(value) && idb_segment_is(ident, len, "length")) {
        JSValue n = JS_GetPropertyStr(ctx, value, "length");

        DCHECK(JS_IsNumber(n), "an Array's own `length` is not a number — §7.1's ToLength is the identity on "
                               "what an Array exotic object holds there, and this value is the output of "
                               "StructuredDeserialize, so nothing can have redefined it");
        *pnext = n;
        return true;
    }
    /* THE CONCOLIC ARM, first among what the standard calls "Otherwise" because a concolic IS an Object and
       every test in that arm would answer for the wrapper instead of for the unknown it stands for. The two
       steps that arm performs are HasOwnProperty and Get, and an unknown answers both the way it answers them
       everywhere else in this engine: it has any key (solver/concolic.c's exotic [[HasProperty]] states that
       rule for membership gates) and reading one yields a DERIVED concolic carrying the field path. See the
       file header for why `failure` would be the wrong answer rather than the conservative one. */
    if (concolic_is(value)) {
        atom = JS_NewAtomLen(ctx, ident, len);
        *pnext = JS_GetProperty(ctx, value, atom);
        JS_FreeAtom(ctx, atom);
        CHECK(!JS_IsException(*pnext), "IndexedDB: §7.1's read of a concolic field threw — a concolic's exotic "
                                       "[[Get]] mints a derived value and has nothing to throw with");
        return true;
    }
    /* "Otherwise: if Type(value) is not Object, return failure." */
    if (!JS_IsObject(value))
        return false;
    /* §7.1's FOUR PLATFORM-OBJECT ARMS HAVE NOTHING TO READ FROM, and that is a fact about this engine rather
       than a step skipped: a Blob cannot reach a value this walks. Every caller of §7.1 hands it the output of
       StructuredDeserialize, and this engine's serializer is quickjs's own writer, which refuses an object of a
       host class outright ("unsupported object class", re-reported by core/structured_clone.c as §2.7's
       DataCloneError) — so `store.put(new Blob([...]))` throws at the CLONE and never arrives. The arms are
       "value is a Blob and identifier is size / type" and "value is a File and identifier is name /
       lastModified", each an internal value core/file/blob.h already exposes, and they land in this cascade
       with HTML §2.7's Blob serialization steps. This asserts that rather than answering `failure` for a Blob,
       which is what the arm below would silently do — `size` is an accessor on the prototype, so the own-slot
       read misses it and the record would go missing instead of being filed under its length. */
    DCHECK(!blob_is(value), "Indexed Database §7.1's Blob and File arms are not built, and a Blob has reached "
                            "the walk — so HTML §2.7's Blob serialization steps now exist and the four arms "
                            "they were waiting for are owed: `size` and `type` for a Blob, `name` and "
                            "`lastModified` for a File, each read as the internal value blob.h exposes and "
                            "never through [[Get]]");
    /* "Let hop be ! HasOwnProperty(value, identifier). If hop is false, return failure. Let value be
       ! Get(value, identifier)." ONE slot read answers both, and it is a SLOT read and not an operation because
       of the standard's own note: this algorithm only ever runs on the output of StructuredDeserialize and only
       accesses own properties, so neither a Proxy nor an accessor is reachable — which is exactly the pair
       JS_GetOwnSlot asserts, so the note is enforced at the read rather than restated as a comment. */
    atom = JS_NewAtomLen(ctx, ident, len);
    has = JS_GetOwnSlot(ctx, pnext, value, atom);
    JS_FreeAtom(ctx, atom);
    CHECK(has >= 0, "IndexedDB: §7.1's own-property read threw — the value is the output of "
                    "StructuredDeserialize and holds nothing that can run");
    if (has == 0)
        return false;
    /* "If value is undefined, return failure." An own property holding `undefined` is not a key and is not an
       address to keep walking from, which is the same answer as an absent one and a DIFFERENT statement. */
    if (JS_IsUndefined(*pnext))
        return false;
    return true;
}

static bool idb_key_path_evaluate(JSContext *ctx, JSValueConst value, JSValueConst key_path, JSValue *pout);

/* §7.1's STRING ARM: "if keyPath is the empty string, return value ... let identifiers be the result of
   STRICTLY SPLITTING keyPath on U+002E FULL STOP", then one step per identifier.
   The split is performed IN PLACE. Infra's strictly split produces the tokens between the separators including
   empty ones, and §2.5's validity has already refused every path that would produce an empty one — a leading
   period, a trailing period, and two periods in a row — which is the assertion in the loop. */
static bool idb_key_path_evaluate_string(JSContext *ctx, JSValueConst value, JSValueConst key_path, JSValue *pout)
{
    size_t len = 0, i = 0;
    const char *path = JS_ToCStringLen(ctx, &len, key_path);
    JSValue cur;

    CHECK(path != NULL, "IndexedDB: §7.1 could not read a key path back as a string");
    /* "If keyPath is the empty string, return value and skip the remaining steps." It is stated BEFORE the
       split because the split of "" is one EMPTY identifier and not none, so a walk that reached it would look
       for a property named "" instead of answering with the value itself. WPT's keypath.any.js is the case:
       "[''] uses value as [key]". */
    if (len == 0) {
        JS_FreeCString(ctx, path);
        *pout = JS_DupValue(ctx, value);
        return true;
    }
    cur = JS_DupValue(ctx, value);
    while (i < len) {
        const char *seg = path + i;
        size_t n = 0;
        JSValue next;

        while (i + n < len && seg[n] != '.')
            n++;
        i += n + 1;   /* past the separator; on the last segment this passes `len` and ends the walk */
        DCHECK(n > 0, "§7.1 split a key path into an EMPTY identifier — §2.5's validity refuses a leading "
                      "period, a trailing period and an empty segment, and idb_key_path_extract asserts that "
                      "every path reaching this walk passed it");
        if (!idb_key_path_step(ctx, cur, seg, n, &next)) {
            JS_FreeValue(ctx, cur);
            JS_FreeCString(ctx, path);
            return false;
        }
        JS_FreeValue(ctx, cur);
        cur = next;
    }
    JS_FreeCString(ctx, path);
    *pout = cur;
    return true;
}

/* §7.1's LIST ARM: "let result be a new Array object created as if by the expression []; let i be 0; for each
   item of keyPath: let key be the result of RECURSIVELY evaluating a key path on a value with item and value;
   if key is failure, ABORT THE OVERALL ALGORITHM and return failure; let p be ! ToString(i); let status be
   CreateDataProperty(result, p, key); assert: status is true; increase i by 1."
   The whole point of the arm is the abort: a compound key is the tuple of ALL its parts, so a value missing one
   of them has no key at all — not a shorter one, and not one with a hole. */
static bool idb_key_path_evaluate_list(JSContext *ctx, JSValueConst value, JSValueConst key_path, JSValue *pout)
{
    JSValue result = JS_NewArray(ctx);
    uint32_t i, n = idb_key_path_list_len(ctx, key_path);

    CHECK(!JS_IsException(result), "IndexedDB: §7.1's list arm could not allocate its Array");
    DCHECK(n > 0, "§7.1 was asked to evaluate an EMPTY list key path — §2.5's last bullet is a NON-EMPTY list "
                  "and §4.4's createObjectStore reports the empty one as a SyntaxError before a store exists");
    for (i = 0; i < n; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, key_path, i), key;
        bool ok;

        /* "This will only ever 'recurse' one level since key path sequences can't ever be nested" — the
           standard's own note, asserted rather than repeated: §3.2.20's sequence conversion has already
           ToString'd every entry, so a nested list arrives as a string with a comma in it and §2.5 refused it
           at createObjectStore. The recursion below therefore always takes the string arm. */
        DCHECK(JS_IsString(item), "§7.1's list arm holds something that is not a string — a key path sequence "
                                  "cannot nest, which is what makes this recursion exactly one level deep");
        ok = idb_key_path_evaluate(ctx, value, item, &key);
        JS_FreeValue(ctx, item);
        if (!ok) {
            JS_FreeValue(ctx, result);
            return false;
        }
        /* CreateDataProperty and not [[Set]]: an index accessor a page put on Array.prototype would otherwise
           swallow the entry, and the assembled key would be missing a part nothing reported. */
        JS_DefinePropertyValueUint32(ctx, result, i, key, JS_PROP_C_W_E);
    }
    *pout = result;
    return true;
}

static bool idb_key_path_evaluate(JSContext *ctx, JSValueConst value, JSValueConst key_path, JSValue *pout)
{
    *pout = JS_UNDEFINED;
    if (JS_IsArray(key_path))
        return idb_key_path_evaluate_list(ctx, value, key_path, pout);
    DCHECK(JS_IsString(key_path),
           "§7.1 was asked to evaluate a key path that is neither a string nor a list of strings — §2.2 stores "
           "the value §3.2.25's union answered with, and a store with NO key path is a question §4.5's `put` "
           "answers before it reaches this");
    return idb_key_path_evaluate_string(ctx, value, key_path, pout);
}

/* ---- §7.1's EXTRACT A KEY FROM A VALUE USING A KEY PATH ----------------------------------------------------- */

IdbKeyPathResult idb_key_path_extract(JSContext *ctx, JSValueConst value, JSValueConst key_path, JSValue *pkey)
{
    JSValue r;
    IdbKeyResult k;

    *pkey = JS_UNDEFINED;
    DCHECK(idb_key_path_value_is_valid(ctx, key_path),
           "§7.1 was asked to walk a key path §2.5 does not accept. Every key path in this engine reaches a "
           "store through §4.4's createObjectStore, which reports an invalid one as a SyntaxError, and the walk "
           "below splits exactly the segments that validity accepts — so a path arriving here that would not "
           "pass it is a member that stored one without asking");
    /* "Let r be the result of evaluating a key path on a value with value and keyPath. Rethrow any exceptions.
       If r is failure, return failure." */
    if (!idb_key_path_evaluate(ctx, value, key_path, &r))
        return IDB_KEY_PATH_FAILURE;
    /* "Let key be the result of converting a value to a key with r ... If key is 'invalid value' or 'invalid
       type', return invalid." The two invalid answers are exactly where this algorithm collapses them, which is
       why idb_key.h keeps them apart up to here and not past it. */
    k = idb_key_convert(ctx, r, pkey);
    JS_FreeValue(ctx, r);
    if (k != IDB_KEY_OK) {
        DCHECK(JS_IsUndefined(*pkey), "§7.4 answered `invalid` and left a key behind");
        return IDB_KEY_PATH_INVALID;
    }
    return IDB_KEY_PATH_KEY;   /* "Return key." */
}
