/* INDEXED DATABASE §7.4's "convert a value to a key" OVER AN ARRAY EXOTIC OBJECT — the one arm of that
 * algorithm that runs the PAGE'S OWN CODE, so the one that is a walk rather than a call. See idb_key_array.c.
 *
 * WHY IT IS A SEPARATE CONTRACT FROM idb_key.h. Every other arm of §7.4 is decided by asking what the value IS
 * (`Type(input) is Number`, a `[[DateValue]]` slot, a buffer source) and answers in one O(1) engine action;
 * idb_key.h's `idb_key_convert_here` is exactly those arms. The array arm is a different kind of thing: its
 * steps are `? ToLength(? Get(input, "length"))`, then one `? HasOwnProperty(input, index)` and one
 * `? Get(input, index)` per element, then a conversion of each element that may itself be an array. An array
 * index CAN be an accessor — `Object.defineProperty(a, 0, {get(){…}})` — so step 5.3 is the page's code, and
 * `? ` on every one of those steps means the page's throw propagates out of §7.4 entirely. A C loop over any of
 * it would be the drive-to-completion this engine aborts on, and C recursion over the element that is itself an
 * array would be a stack whose depth the page picks.
 *
 * SO IT IS A DELEGATABLE ALGORITHM, in the shape core/dom/node.h's `clone a node` established: the CALLER
 * declares this algorithm's stage block inside its own stage list with IDB_KEY_ARRAY_ALGO_STAGES, embeds an
 * IdbKeyWalk, chains idb_key_walk_visit into its own `visit`, and hands idb_key_walk_run the base of that
 * block. One implementation, performed inside whichever member is running, parkable at every element of a
 * structure the page chose the size and the depth of — which is what §scheduler requires of a walk that is not
 * O(1), and the reason there is no second, non-suspending copy of these steps anywhere.
 *
 * A MEMBER THAT CANNOT DRIVE ONE CRASHES AT idb_key_convert's C ENTRY, naming itself. That is not a fallback:
 * there is no other implementation of the array arm to fall back TO. §4.5's `add or put`, §4.5's `get`,
 * `delete` and `count` (through idb_key_range_from_value) and §7.1's extract-a-key are the call sites still
 * standing there, and each is a member to convert rather than a limitation to record.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_ARRAY_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_ARRAY_H

#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/indexeddb/idb_key.h"

/* THE ALGORITHM'S SIX REST POINTS, expanded into the CALLER's stage list with the caller's own prefix and its
   own leading text — stage identity is the LABEL (quickjs-step.h's JSTrampStepDef::steps), so the list is
   written once here and the labels a parked flow holds name a step of §7.4 rather than a private counter.
   EVERY STEP THAT READS SOMETHING OFF THE PAGE'S OBJECT IS ITS OWN STAGE. The loop is over the page's own
   `length`, so the span from step 5 to step 6 is not a range this may name in one label; steps 2-4 and steps
   5.4-5.8 are each one O(1) engine action and are labelled as the ranges they are. */
#define IDB_KEY_ARRAY_ALGO_STAGES(X, P, W) \
    X(P##_LENGTH, W " → Indexed Database §7.4 convert a value to a key, the Array arm step 1 (len is " \
                    "? ToLength(? Get(input, \"length\")))") \
    X(P##_BEGIN,  W " → Indexed Database §7.4 convert a value to a key, the Array arm steps 2-4 (append input " \
                    "to seen; keys is a new empty list; index is 0)") \
    X(P##_HOP,    W " → Indexed Database §7.4 convert a value to a key, the Array arm steps 5-5.2 (hop is " \
                    "? HasOwnProperty(input, index); a false hop returns \"invalid value\")") \
    X(P##_ENTRY,  W " → Indexed Database §7.4 convert a value to a key, the Array arm step 5.3 (entry is " \
                    "? Get(input, index))") \
    X(P##_SUBKEY, W " → Indexed Database §7.4 convert a value to a key, the Array arm steps 5.4-5.8 (convert " \
                    "entry to a key with seen; an invalid subkey returns \"invalid value\"; append it to keys " \
                    "and increase index by 1)") \
    X(P##_LEAVE,  W " → Indexed Database §7.4 convert a value to a key, the Array arm step 6 (return a new " \
                    "array key with value keys)")

/* ONE LEVEL of the algorithm's own recursion — the array being walked and the subkeys collected off it. There
   is one per nested array the page wrote, so the stack these live in is grown rather than sized: a level count
   is the page's number and a ceiling on it would refuse a key the standard answers. */
typedef struct IdbKeyLevel {
    JSValue src;      /* this level's `input` — an Array exotic object (owned) */
    JSValue keys;     /* step 3's list, as a plain Array of key records (owned) */
    int64_t len;      /* step 1's len */
    int64_t index;    /* step 4's index */
} IdbKeyLevel;

/* THE WHOLE OF §7.4 IN FLIGHT, as the record a member embeds. `res` and `key` are the algorithm's answer and
   are read at the member's own `after` stage; every other field is the walk's. */
typedef struct IdbKeyWalk {
    IdbKeyLevel *lv;      /* `sp` live levels in a `cap`-level allocation (owned) */
    int          sp, cap;
    /* §7.4 step 1's `seen` SET, as a plain Array of the arrays step 2 has appended to it. It is a JS value and
       not a malloc'd list for §State-isolation's reason: it must park to the cold tier and resume with the
       flow, and an Array's appends are property writes the machinery already carries. It is NOT the level
       stack — the set is never popped from, so `[a, a]` for one array `a` is "invalid value" while `[[1],[1]]`
       is a valid two-subkey key, and a stack of ancestors would answer the first of those wrong. */
    JSValue      seen;
    JSValue      entry;   /* step 5.3's value, between the read and step 5.4's conversion (owned) */
    /* STEP 5.1's PROPERTY KEY, OWNED ACROSS THE REQUEST — and this record has to own it, unlike step 1's and
       step 5.3's. quickjs-step.h's keyed READ dups the atom onto the header (`get_atom`), so a caller may
       release its own reference the moment it asks; the [[GetOwnProperty]] request BORROWS the atom instead
       (it rides `*out_argc`), so the machine that asked is the only thing keeping it alive while the driver
       resolves a trap. JS_ATOM_NULL is 0, so a zeroed state already reads as "nothing asked". */
    JSAtom       hop_atom;
    JSValue      key;     /* THE ANSWER on IDB_KEY_OK (owned) */
    IdbKeyResult res;
    int          after;   /* the CALLER's stage this algorithm hands control back at */
} IdbKeyWalk;

/* BEGIN §7.4 over `input` — the WHOLE algorithm and not only its array arm, so a member has one path and never
 * a test for which arm it got. `base` is where the caller declared the stage block and `after` is the caller's
 * own stage, which this points `hdr->stage` at: immediately when the arms that run none of the page's code
 * decide the answer, and at step 6 otherwise. The caller returns JS_STEP_YIELD and reads `w->res` at `after`.
 * A walk already used is released first, because §4.7's `bound` converts two values through one record. */
void idb_key_walk_start(JSContext *ctx, JSStepHdr *hdr, IdbKeyWalk *w, JSValueConst input, int base, int after);

/* ONE STAGE of it. Returns a step code the caller must return (a request's, or JS_STEP_YIELD to rest), or
   JS_STEP_ABRUPT having let the page's throw stand — §7.4's `?` on every one of its reads. `in` is CONSUMED. */
int  idb_key_walk_run(JSContext *ctx, JSStepHdr *hdr, IdbKeyWalk *w, JSValue in, int base,
                      JSValue **out_cb, int *out_argc);

/* THE CALLER'S OWN `visit` CHAINS INTO THIS — the level stack is this record's allocation and the three values
   are its references, so a caller has no second list to write and no `release` half to get wrong. */
void idb_key_walk_visit(JSContext *ctx, IdbKeyWalk *w, JSStepVisit *v);

/* THE ANSWER, AS §4 SPELLS IT: "Rethrow any exceptions. If key is 'invalid value' or 'invalid type', throw a
   'DataError' DOMException." Returns 0 with `*pkey` an owned key record, or -1 with the "DataError" live —
   the same sentence idb_key_from_value writes for a caller that has no walk, so the two cannot drift. */
int  idb_key_walk_take(JSContext *ctx, IdbKeyWalk *w, JSValue *pkey);

#endif
