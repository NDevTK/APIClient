/* INDEXED DATABASE §2.11's KEY GENERATOR — "used to generate keys for records inserted into an object store if
 * not otherwise specified".
 *
 * WHY IT IS ITS OWN FILE AND NOT A FIELD OF §2.2's STORE. The generator is one number, and every sentence §2.11
 * writes about it is an INVARIANT over that number rather than a use of it: it is a positive integer at most
 * 2^53+1; it never decreases except when a database operation is reverted; the same key is never generated twice
 * for one store unless a transaction is rolled back; and every object store has its OWN, so "interacting with
 * one object store never affects the key generator of any other". Those are exactly the things a component
 * asserts at its own accessor — and a wrong current number is not a visible failure but a wrong KEY, which
 * §2.2's sorted list, §2.4's ordering and every retrieval built on them then quietly agree with.
 *
 * WHY THE CURRENT NUMBER IS NOT AN ECMASCRIPT NUMBER, which is the one decision this file is built around.
 * §2.11's range is "less than or equal to 2^53 (9007199254740992) + 1" and its own note explains that ceiling:
 * "integers greater than 9007199254740992 cannot be uniquely represented as ECMAScript Numbers. As an example,
 * 9007199254740992 + 1 === 9007199254740992 in ECMAScript." So the ONE state the standard reserves for
 * "exhausted" — the current number sitting at 2^53+1, from which "generate a key" answers failure forever — is
 * the one state a double cannot tell from 2^53, the last number the generator legitimately hands out. Holding it
 * as a double would round the exhausted state back onto a usable one and generate 2^53 TWICE, breaking §2.11's
 * "the same key is never generated twice for the same object store". WPT's keygenerator.any.js reaches it in two
 * lines (`store.put(v, Number.MAX_SAFE_INTEGER + 1)` then `store.put(v)`), and its own comment is the rule:
 * "large positive values will max out the key generator, so it can no longer produce keys".
 *
 * So the number is an int64 in this file and a BIGINT on the record — a JS value, because §State-isolation makes
 * that the only shape state can have here (the write is a property write the per-flow COW delta captures, so one
 * flow's `put` is invisible to its sibling and a parked flow resumes on its own generator), and a BigInt because
 * it is ECMAScript's exact integer. A double on the record would be the same bug one layer down from the one it
 * is chosen to avoid. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_key_generator.h"

/* §2.11's one field, spelled once. */
#define IDB_GENERATOR_NUMBER "currentNumber"

/* THE ONE WRITE, and the range every other line of this file rests on. Both entries below funnel through it so
   that §2.11's "always a positive integer less than or equal to 2^53 + 1" is asserted once, at the moment the
   number becomes what it is, rather than at each of the readers that would then be describing it. */
static void idb_key_generator_write(JSContext *ctx, JSValueConst gen, int64_t number)
{
    DCHECK(number >= 1 && number <= IDB_KEY_GENERATOR_CEILING + 1,
           "§2.11's current number left its range: \"the current number is always a positive integer less than "
           "or equal to 2^53 (9007199254740992) + 1\". Every number written here is either 1 (the initial "
           "value), a current number plus 1, or a floored key clamped to the ceiling plus 1, so a number "
           "outside it is one of those three arriving unclamped");
    JS_SetPropertyStr(ctx, gen, IDB_GENERATOR_NUMBER, JS_NewBigInt64(ctx, number));
}

JSValue idb_key_generator_new(JSContext *ctx)
{
    JSValue gen = idl_slots_new(ctx);

    CHECK(!JS_IsException(gen), "IndexedDB: §2.11's key generator could not be allocated");
    /* "The initial value of a key generator's current number is 1, set when the associated object store is
       created." Which is what makes §2.11's own worked example true: "the first key generated for an object
       store is always 1 (unless a higher numeric key is inserted first)". */
    idb_key_generator_write(ctx, gen, 1);
    return gen;
}

int64_t idb_key_generator_number(JSContext *ctx, JSValueConst gen)
{
    JSValue v = JS_GetPropertyStr(ctx, gen, IDB_GENERATOR_NUMBER);
    int64_t n = 0;
    int r;

    DCHECK(JS_IsBigInt(v), "§2.11's current number is not a BigInt. It is held as one BECAUSE it is not an "
                           "ECMAScript Number — 2^53+1, the exhausted state, is not representable as one — so "
                           "a number here is the exactness this component exists to keep, already lost");
    r = JS_ToBigInt64(ctx, &n, v);
    DCHECK(r >= 0, "§2.11's current number could not be read back as an integer");
    (void)r;
    JS_FreeValue(ctx, v);
    DCHECK(n >= 1 && n <= IDB_KEY_GENERATOR_CEILING + 1,
           "§2.11's current number is outside \"a positive integer less than or equal to 2^53 + 1\" — the write "
           "asserts the same range, so this is the record having been written by something other than this "
           "component");
    return n;
}

/* §2.11's "the current number is INCREMENTED as keys are generated, and may be updated to a specific value by
   using explicit keys" — the two writes the algorithms below make, which are the two that may only RAISE it.
   "The current number for a key generator never decreases, other than as a result of database operations being
   reverted", and that exception is the other entry. */
static void idb_key_generator_advance(JSContext *ctx, JSValueConst gen, int64_t number)
{
    DCHECK(number > idb_key_generator_number(ctx, gen),
           "§2.11's current number was not RAISED by an algorithm that only raises it: \"the current number for "
           "a key generator never decreases, other than as a result of database operations being reverted\". A "
           "generate that did not move it hands the same key out twice, and an update that lowered it hands out "
           "a key some record already holds");
    idb_key_generator_write(ctx, gen, number);
}

void idb_key_generator_revert(JSContext *ctx, JSValueConst gen, int64_t number)
{
    DCHECK(number <= idb_key_generator_number(ctx, gen),
           "§5.5 step 2 put a key generator's current number UP. The revert writes back the number the generator "
           "held BEFORE a change this transaction made, and every change this transaction made raised it — so a "
           "higher number means the changes are being replayed rather than undone, or one was recorded after "
           "the write it was supposed to precede");
    idb_key_generator_write(ctx, gen, number);
}

bool idb_key_generate(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValue *pkey)
{
    JSValue gen = idb_object_store_key_generator(ctx, store);   /* STEP 1 */
    int64_t key = idb_key_generator_number(ctx, gen);           /* STEP 2 */

    *pkey = JS_UNDEFINED;
    /* "If key is greater than 2^53 (9007199254740992), then return failure." The generator is spent, and §2.11
       says it stays spent: "it is still possible to insert records into the object store by specifying an
       explicit key, however the only way to use a key generator again for such records is to delete the object
       store and create a new one." Nothing is written on this arm, so nothing is recorded either. */
    if (key > IDB_KEY_GENERATOR_CEILING) {                      /* STEP 3 */
        JS_FreeValue(ctx, gen);
        return false;
    }
    /* "Increase generator's current number by 1", recorded against the transaction FIRST — §2.11: "modifying a
       key generator's current number is considered part of a database operation ... likewise, if a transaction
       is aborted, the current number of the key generator for each object store in the transaction's scope is
       reverted to the value it had before the transaction was started." */
    idb_object_store_record_generator_change(ctx, tx, store, key);
    idb_key_generator_advance(ctx, gen, key + 1);               /* STEP 4 */
    JS_FreeValue(ctx, gen);
    /* "Return key." As §2.4's number key, which is what §6.1 files the record under. The double is EXACT here
       and only here: the ceiling test above is what leaves `key` at most 2^53, and 2^53 is the last integer a
       double holds alone. */
    DCHECK((int64_t)(double)key == key,
           "§2.11's generated key lost precision on its way into §2.4's number key — the ceiling test above is "
           "what bounds it to 2^53, so a key that does not round-trip is one that skipped it");
    *pkey = idb_key_new_number(ctx, (double)key);               /* STEP 5 */
    return true;
}

bool idb_key_generator_possibly_update(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst key)
{
    JSValue gen;
    double value;
    int64_t current, n;

    /* "If the type of key is not number, abort these steps. Let value be the value of key." */
    if (!idb_key_is_number(ctx, key, &value))                   /* STEPS 1-2 */
        return false;
    DCHECK(!isnan(value), "a NUMBER key carries NaN — §7.4's number arm answers \"invalid value\" for one, so "
                          "no such key reaches a store, and the clamp below would answer for it silently");
    /* "Set value to the minimum of value and 2^53 (9007199254740992)." Which is what makes every one of WPT's
       big_key_test cases (2^60, 2^63, 2^64, 2^70, Infinity) end at the same place: an explicit key above the
       ceiling maxes the generator out rather than being ignored. */
    if (value > (double)IDB_KEY_GENERATOR_CEILING)              /* STEP 3 */
        value = (double)IDB_KEY_GENERATOR_CEILING;
    value = floor(value);                                       /* STEP 4 — "the largest integer not greater" */
    gen = idb_object_store_key_generator(ctx, store);           /* STEP 5 */
    current = idb_key_generator_number(ctx, gen);
    /* STEP 6's test, in TWO halves because one of its operands is a double and the other must not be. A current
       number of 2^53+1 has no double, so comparing there would answer 2^53 >= 2^53 and raise a generator the
       standard has already spent; and a value of -Infinity (WPT's negative big keys) has no int64. So the
       values below the smallest possible current number are answered as doubles — §2.11 makes the current
       number at least 1, so anything under 1 is under it — and what survives is an integer in [1, 2^53], which
       crosses to int64 exactly. §2.11's own note is this arm: "keys of type number with value less than 1 do
       not affect the current number since they are always lower than the current number."
       WRITTEN AS A NEGATED >=, so that the one value the assertion above forbids cannot become undefined
       behaviour if that assertion is ever compiled out: NaN fails every comparison, and the cast below has no
       answer for it. */
    if (!(value >= 1.0)) {
        JS_FreeValue(ctx, gen);
        return false;
    }
    n = (int64_t)value;
    /* "If value is greater than or equal to generator's current number, then set generator's current number to
       value + 1." */
    if (n < current) {                                          /* STEP 6 */
        JS_FreeValue(ctx, gen);
        return false;
    }
    idb_object_store_record_generator_change(ctx, tx, store, current);
    idb_key_generator_advance(ctx, gen, n + 1);
    JS_FreeValue(ctx, gen);
    return true;
}
