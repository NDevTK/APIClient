/* INDEXED DATABASE §2.4's KEY — the value everything else in that standard is defined over. See idb_key.c.
   A key path is the ADDRESS of one inside a value and is a different contract: core/indexeddb/idb_key_path.h. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_H

#include <stdbool.h>

#include "quickjs.h"

/* §7.4's CONVERT A VALUE TO A KEY — its own three answers, before any caller's next step. "The result of these
 * steps is a key, 'invalid value', or 'invalid type', or the steps may throw an exception."
 *
 * The two invalid answers are NOT collapsed into one: no algorithm in the standard tells them apart today
 * (§7.1 maps both to `invalid`, §7.4's own array arm maps both to "invalid value"), and the day one does, the
 * distinction is restored in the ANSWER rather than reconstructed by a caller from an exception name.
 *
 * IDB_KEY_ARRAY IS NOT ONE OF §7.4's ANSWERS — it is this engine's fourth, and it is not a refusal. §7.4's array
 * arm runs the PAGE'S OWN CODE (an own array index may be an accessor, and step 5.3 is a `? Get`), so it exists
 * in exactly one form: the parkable walk in core/indexeddb/idb_key_array.h. `idb_key_convert_here` therefore
 * answers every OTHER arm and hands the array BACK for that walk to perform. Only that function returns it; the
 * plain-C entry below crashes on it, naming the call site to convert.
 *
 * *pkey is OWNED on IDB_KEY_OK and JS_UNDEFINED otherwise. */
typedef enum { IDB_KEY_OK = 0, IDB_KEY_INVALID_VALUE, IDB_KEY_INVALID_TYPE, IDB_KEY_ARRAY } IdbKeyResult;

/* §7.4's ARMS THAT RUN NONE OF THE PAGE'S CODE — Number, Date, String, a buffer source, this engine's concolic,
   and "otherwise". Each is one O(1) engine action, which is why they are a call. On IDB_KEY_ARRAY `*parray` is
   the OWNED Array exotic object whose conversion is the walk's; it is JS_UNDEFINED on every other answer. */
IdbKeyResult idb_key_convert_here(JSContext *ctx, JSValueConst input, JSValue *pkey, JSValue *parray);

/* §7.4 STEP 6's "a new array key with value keys" — §2.4's value for type array being "a list of other keys",
   which this engine holds as a plain Array of key records. `keys` is CONSUMED. It is here rather than in the
   walk because a key record is this file's own shape and there is one constructor for one. */
JSValue idb_key_new_array(JSContext *ctx, JSValue keys);

/* §2.4's NUMBER KEY, minted from a number the ENGINE computed rather than converted from a page value. §2.11's
   "generate a key" is the one caller and is the whole reason this entry exists beside §7.4: that algorithm
   answers with its generator's current number, and §6.1 files a record under it — there is no page value for
   §7.4 to convert, so reaching this through it would mean minting one to convert back. */
JSValue idb_key_new_number(JSContext *ctx, double value);

/* §2.11 STEPS 1-2's ONE QUESTION ABOUT A KEY — "if the TYPE of key is not number, abort these steps. Let value
 * be the VALUE of key." They are one call because the value is only ever read on the arm the type answered for,
 * and a caller that read the value first would have to know what a date or binary key's value is.
 *
 * False for every other type, with *pvalue untouched. Where the key's value is a CONCOLIC standing in for the
 * number, the EXAMPLE comes back — the same seam idb_key_compare reads through and for the same reason: a key
 * generator's current number is engine state that needs an actual number, while the taint stays on the key that
 * is filed. */
bool idb_key_is_number(JSContext *ctx, JSValueConst key, double *pvalue);

/* §7.4 FOR A CALLER WITH NO FLOW UNDER IT — an in-C fixture, every §4 member having its own flow and driving the
   walk. It answers the arms above and CRASHES on an Array, naming the walk to route through: there is no second
   implementation of the array arm for it to fall back to. */
IdbKeyResult idb_key_convert(JSContext *ctx, JSValueConst input, JSValue *pkey);

/* THE SAME CONVERSION FOLLOWED BY THE STEP EVERY §4 MEMBER THAT TAKES A KEY PERFORMS. §4.3's `cmp`, §4.7's
 * `only`, `lowerBound`, `upperBound`, `bound` and `includes` each say "If it is 'invalid value' or 'invalid
 * type', throw a 'DataError' DOMException". Six copies of one sentence is six chances to write it once as
 * something else, so the sentence is here and the members state their own steps around it. §7.1's extract-a-key
 * is NOT one of those callers — it maps both invalid answers to its own `invalid` and its caller decides what
 * to report — which is why the conversion above is the entry and this is the wrapper.
 *
 * Returns 0 with `*pkey` an owned key, or -1 with the "DataError" DOMException live. */
int idb_key_from_value(JSContext *ctx, JSValueConst input, JSValue *pkey);

/* §7.3's CONVERT A KEY TO A VALUE — the other direction, which is what a member that hands a key BACK to the
   page returns (§4.7's `lower` and `upper`, §4.8's `key`, §6.2's retrieve-a-key). OWNED. */
JSValue idb_key_to_value(JSContext *ctx, JSValueConst key);

/* §2.4's "AN ARRAY KEY", and §6.1 steps 5.4 and 5.6's "THE SUBKEYS OF index key" — the two questions §6.1 step
   5's multiEntry arms are stated over, and the only place outside this file that has to know a key has a type
   at all. The subkey list is §2.4's "a list of other keys" and is OWNED; asking for one of a key that is not an
   array key is a should-never-happen, because the condition selecting that arm is the same question. */
bool    idb_key_is_array(JSContext *ctx, JSValueConst key);
JSValue idb_key_subkeys(JSContext *ctx, JSValueConst key);

/* §2.4's COMPARE TWO KEYS — -1, 0 or 1. It is THE ordering of this standard: §2.2's list of records is sorted
   by it, §2.9's key range is bounded by it, §2.10's cursor walks in it, and §4.3's `cmp` is it, exposed. */
int idb_key_compare(JSContext *ctx, JSValueConst a, JSValueConst b);

/* INFRA'S CODE-UNIT ORDERING OVER TWO STRINGS — -1, 0 or 1. §2.4's string arm is stated over it and so is
   §5.12's "sorted in ascending order with the code unit less than algorithm", which is why the one this file
   already had is exported rather than written again: the UTF-16 order it computes disagrees with the UTF-8 byte
   order of the same strings, and two implementations would disagree on which of the two a name list uses. */
int idb_code_unit_compare(JSContext *ctx, JSValueConst a, JSValueConst b);

#endif
