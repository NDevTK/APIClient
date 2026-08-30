/* INDEXED DATABASE §2.9's KEY RANGE and §4.7's IDBKeyRange interface. See idb_key_range.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_RANGE_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_RANGE_H

#include <stdbool.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/indexeddb/idb_key_array.h"

/* Declared ONCE PER AGENT: the class §4.7's brand check asks, the five member declarations, and the per-realm
   install that builds this realm's IDBKeyRange.prototype and interface object. The component holds no
   agent-lifetime JS value, so there is nothing to release — a range's own state is on the range. */
void idb_key_range_init(JSContext *ctx);

/* §2.9's CONVERT A VALUE TO A KEY RANGE, which is what every member that takes a `query` runs before it does
 * anything else — §4.5's `get`, `getKey`, `delete` and `count`, and §5.12's retrieve-multiple. A value that IS
 * a key range is itself; undefined or null is an unbounded key range, or a "DataError" where the null
 * disallowed flag is set; anything else is §7.4's key, in a range containing only it.
 *
 * IT IS A DELEGATABLE ALGORITHM FOR ITS STEP 3's SAKE, in the same shape core/indexeddb/idb_key_array.h is and
 * for the same reason: "let key be the result of converting a value to a key with value" over an Array exotic
 * object runs the PAGE'S OWN CODE, so this algorithm cannot be a call either. Its own steps 1, 2, 4 and 5 are
 * each one O(1) engine action and rest nowhere; every rest point it has is §7.4's, which is why the block below
 * expands that one rather than adding stages of its own. A member declares IDB_KEY_RANGE_ALGO_STAGES in its
 * stage list, embeds an IdbRangeWalk, chains idb_key_range_walk_visit into its `visit`, and hands `run` the base
 * of that block; §4.5's `get`, `getKey`, `delete` and `count` are the four callers.
 *
 * Returns 0 with *prange an OWNED key range, or -1 with the "DataError" DOMException live. This is the C entry,
 * for a caller with no flow under it — an in-C fixture — and it crashes on an Array exactly where §7.4's own C
 * entry does. */
int idb_key_range_from_value(JSContext *ctx, JSValueConst value, bool null_disallowed, JSValue *prange);

#define IDB_KEY_RANGE_ALGO_STAGES(X, P, W) \
    IDB_KEY_ARRAY_ALGO_STAGES(X, P, W " → Indexed Database §2.9 convert a value to a key range step 3 (let key " \
                                      "be the result of converting a value to a key with value)")

/* §2.9 IN FLIGHT. The key walk is the whole of the algorithm's suspendable part; `range` is the answer steps 1
   and 2 decide BEFORE any conversion, and it is a record of its own rather than a field the caller declares
   because it is a REFERENCE and therefore something a `visit` has to name — unlike §7.1's extra state, which is
   an enum the caller can hold itself. */
typedef struct IdbRangeWalk {
    IdbKeyWalk w;
    JSValue    range;   /* step 1's or step 2's answer (owned), JS_UNDEFINED while step 3 decides */
} IdbRangeWalk;

/* BEGIN §2.9 over `value`, performing steps 1-2 here: the caller's `after` stage is reached immediately when
   either answers, and step 3's conversion is begun otherwise. Returns the step code the caller must return —
   JS_STEP_ABRUPT with step 2's "DataError" live, or JS_STEP_YIELD. */
int  idb_key_range_walk_start(JSContext *ctx, JSStepHdr *hdr, IdbRangeWalk *rw, JSValueConst value,
                              bool null_disallowed, int base, int after);

/* ONE STAGE of step 3's conversion. Same contract as idb_key_walk_run: return what it returns, `in` CONSUMED. */
int  idb_key_range_walk_run(JSContext *ctx, JSStepHdr *hdr, IdbRangeWalk *rw, JSValue in, int base,
                            JSValue **out_cb, int *out_argc);

void idb_key_range_walk_visit(JSContext *ctx, IdbRangeWalk *rw, JSStepVisit *v);

/* STEPS 4-5 AT THE CALLER'S OWN STAGE: an "invalid value" or "invalid type" key is a "DataError", and otherwise
   the answer is a key range containing only key — or the range steps 1-2 already decided. Returns 0 with
   *prange an OWNED key range, or -1 with the "DataError" live. */
int  idb_key_range_walk_take(JSContext *ctx, IdbRangeWalk *rw, JSValue *prange);

/* §2.9's "A KEY RANGE CONTAINING ONLY KEY", minted from a key record the engine ALREADY HOLDS rather than from
   a page value there is nothing to convert. Two algorithms ask for one: §6.1 step 3 removes the record a put
   displaces "using DELETE RECORDS FROM AN OBJECT STORE", which takes a range and not a key, and §6.3's first
   retrieval reaches the referenced value through §6.2 over the index record's primary key. Reaching this
   through the conversion above instead would mean minting a page value to convert back. `key` is BORROWED; the
   range is OWNED. */
JSValue idb_key_range_only_key(JSContext *ctx, JSValueConst key);

/* §2.9's "a key is IN a key range" — the membership test §6.2's retrievals, §6.4's delete, §6.5's count and
   §6.7's cursor iteration are each stated over. `range` is one of this component's own ranges, which is what
   the conversion above guarantees, so a value that is not one crashes rather than answering. */
bool idb_key_range_contains(JSContext *ctx, JSValueConst range, JSValueConst key);

/* WEB IDL §3.7.5's BRAND, asked of a value that arrived from another component. §2.9's convert-a-value-to-a-key
   -range asks it as its own step 1 and answers with the range, which is all a member that CONVERTS needs; §5.12's
   "is a potentially valid key range" asks it as a QUESTION and takes a different branch on the answer, so the
   question exists on its own. */
bool idb_key_range_is(JSValueConst v);

/* The AGENT's half, undone — core/platform.h's release column. */
void idb_key_range_free(void);

#endif
