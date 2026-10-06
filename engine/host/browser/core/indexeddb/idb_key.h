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
 * IDB_KEY_UNKNOWN IS THE FIFTH AND IS THE SAME KIND OF THING — not an answer, a HAND-BACK. §7.4 decides a key's
 * type by asking what the value IS, and a value this flow has no bytes for is not any of them and is not none
 * of them either: the answer is UNDECIDED, so it is a FORK and a fork needs a flow to snapshot. The arms that
 * run none of the page's code are still answered in C, and this one is handed back to the same walk the array
 * arm is handed back to, for the same reason and through the same door.
 *
 * *pkey is OWNED on IDB_KEY_OK and JS_UNDEFINED otherwise. */
typedef enum { IDB_KEY_OK = 0, IDB_KEY_INVALID_VALUE, IDB_KEY_INVALID_TYPE, IDB_KEY_ARRAY,
               IDB_KEY_UNKNOWN } IdbKeyResult;

/* §7.4's COMPLETIONS OVER A VALUE THIS FLOW HAS NO KEY BYTES FOR — the worlds the fork explores, declared
 * HERE because two files read them: core/indexeddb/idb_key_array.c asks `step_fork_run` with this many
 * completions and switches on the arm, and idb_key.c mints the key each arm names. A second copy of the
 * numbering is a second place for an arm to mean a different world than the one it was recorded as.
 *
 * WHY THREE AND NOT §7.4's SIX ARMS, AND NOT THE TWO THE CRASH THIS REPLACES ASKED FOR. A page does not
 * observe "which arm"; it observes the ANSWER, and §2.4's compare reads a key's TYPE before it reads any
 * value ("if ta does not equal tb" — number < date < string < binary < array), so the type of the key §7.4
 * returns is observable with no further unknown in it. That is why a single "a key" arm would be wrong. What
 * bounds it at three is what the engine can MINT WITHOUT INVENTING: §2.4's value for a number and for a
 * string is a primitive the concolic can stand in for, and idb_key.c already rides it there for an unknown
 * whose example answered. A DATE's value is a double, a BINARY key's is a byte sequence and an ARRAY key's is
 * a list of other keys — none of the three is a place a concolic can ride, so entering those arms means
 * minting bytes nothing observed, which is §RUN-DON'T-MATCH's invention, and an array arm additionally needs
 * a length only a bound could supply. See idb_key_array.c's `walk_unknown` for the residual that names them.
 *
 * ARM 0 IS THE REFUSAL AND THAT IS THE NUMBERING RULE RATHER THAN A PREFERENCE: quickjs-step.h's
 * `step_fork_run` requires that outcome 0 be the completion a run with NO forking policy takes, and a
 * concolic rides an Object, so every type test in idb_key_concrete_arm answers no for one and the value falls
 * out of §7.4's own "Otherwise: return 'invalid type'". §7.4's TWO refusals are ONE arm because nothing in
 * this standard tells them apart (see above), so the worlds in which the unknown is a NaN number, a NaN-dated
 * Date or a detached buffer source are inside this arm too.
 *
 * A NEW COMPLETION GOES LAST. solver/decide.h: the frontier is never reset, so a parked flow holds arms keyed
 * by the numbers declared on the day it parked — appending appends a QUESTION and every existing boolean
 * replays unchanged, while INSERTING shifts every number above it and answers questions it was never about. */
typedef enum {
    IDB_KEY_UNKNOWN_INVALID = 0,
    IDB_KEY_UNKNOWN_NUMBER,
    IDB_KEY_UNKNOWN_STRING,
    IDB_KEY_UNKNOWN_ARMS
} IdbKeyUnknownArm;

/* THE OPERATION HALF OF THAT FORK'S CONSTRAINT KEY, SPELLED ONCE. A constraint key is what a parked flow's
   recorded answers are filed under, in this session and out of the cold tier in the next, so two spellings of
   one question file it under two names and a flow reaching it through the other door finds nothing recorded.
   The walk asks at two sites — the top-level input and a subkey inside an Array — and they share this string
   because the OPERAND half differs and the two are mutually exclusive within one conversion: the top level
   asks only where the input is not an Array, and only an Array has subkeys. */
#define IDB_KEY_UNKNOWN_ASK "Indexed Database §7.4 convert a value to a key"

/* THE KEY ONE OF THOSE ARMS NAMES, over the unknown the arm is about. `value` is the CONCOLIC ITSELF and is
   DUP'd onto the record: §2.4's value for a number key and for a string key is a primitive, so the unknown
   rides it exactly as it does for an unknown whose example answered §7.4, and nothing is de-tainted — the key
   hands that same value back through §7.3 and reaches a sink carrying the fact that an attacker chose it.
   IDB_KEY_UNKNOWN_INVALID names no key and is not a value this entry accepts. */
JSValue idb_key_new_unknown(JSContext *ctx, IdbKeyUnknownArm arm, JSValueConst value);

/* §7.4's ARMS THAT RUN NONE OF THE PAGE'S CODE — Number, Date, String, a buffer source, this engine's concolic,
   and "otherwise". Each is one O(1) engine action, which is why they are a call. On IDB_KEY_ARRAY `*parray` is
   the OWNED Array exotic object whose conversion is the walk's; it is JS_UNDEFINED on every other answer.
   On IDB_KEY_UNKNOWN `*punknown` is the OWNED concolic the fork is about, which is NOT always `input`: the
   concolic arm unwraps an example that is itself a concolic, so the operand with no bytes can be one this
   function uncovered. It is handed back rather than re-derived because the constraint key is keyed on the
   VALUE, and a fork filed under `input`'s identity would be a different question than the one being asked. */
IdbKeyResult idb_key_convert_here(JSContext *ctx, JSValueConst input, JSValue *pkey, JSValue *parray,
                                  JSValue *punknown);

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
   §2's "sorted in ascending order with the code unit less than algorithm", which is why the one this file
   already had is exported rather than written again: the UTF-16 order it computes disagrees with the UTF-8 byte
   order of the same strings, and two implementations would disagree on which of the two a name list uses. */
int idb_code_unit_compare(JSContext *ctx, JSValueConst a, JSValueConst b);

#endif
