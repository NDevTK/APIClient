/* INDEXED DATABASE §2.5's KEY PATH and §7.1 over one. See idb_key_path.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_PATH_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_PATH_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/indexeddb/idb_key_array.h"

/* §2.5's VALID KEY PATH, over ONE string. "A valid key path is one of: an empty string; an identifier, which is
 * a string matching the IdentifierName production from the ECMAScript Language Specification; a string
 * consisting of two or more identifiers separated by periods (U+002E FULL STOP)" — and the standard's own note
 * is the whole of why this is a real test rather than a glance: "spaces are not allowed within a key path".
 *
 * §4.4's `createObjectStore` is the caller, and the answer is a "SyntaxError" DOMException there.
 *
 * `path` is UTF-8, `len` its length in bytes. */
bool idb_key_path_is_valid(const char *path, size_t len);

/* §2.5's VALIDITY OVER THE WHOLE OF WHAT A KEY PATH CAN BE, which is the question a member actually has: its
 * last bullet is "a NON-EMPTY LIST containing only strings conforming to the above requirements", and the
 * value a member is handed is whatever §3.2.25's `(DOMString or sequence<DOMString>)` answered with — a string,
 * or the ENGINE'S OWN Array of strings. Asking it here rather than in each caller is what stops the list arm
 * being re-derived per member: §4.4's createObjectStore and §4.5's createIndex both report the same
 * "SyntaxError" for the same reason, and the second of them is not written yet.
 *
 * THE EMPTINESS IS THE LIST'S OWN AND IS NOT THE STRING RULE APPLIED N TIMES: the empty STRING is a valid key
 * path (it names the value itself), so a loop over an empty list would answer true for a list §2.5 refuses.
 * Every entry has already been ToString'd by the sequence conversion, which is why `['a', ['b','c']]` arrives
 * as `['a', 'b,c']` and is refused for the comma rather than for being nested. */
bool idb_key_path_value_is_valid(JSContext *ctx, JSValueConst path);

/* §7.1's THREE ANSWERS THAT ARE NOT AN EXCEPTION: "the result of these steps is a key, invalid, or failure".
 * The three are DISTINCT at §4.5's `add or put`, which spells a different step for each — a key becomes the
 * record's key, `invalid` is a "DataError" DOMException, and `failure` is a "DataError" ONLY for a store with
 * no key generator (with one it is the value the generated key is injected into). Collapsing any two of them
 * would make one of those three sentences unwritable. */
typedef enum { IDB_KEY_PATH_KEY = 0, IDB_KEY_PATH_INVALID, IDB_KEY_PATH_FAILURE } IdbKeyPathResult;

/* §7.1's EXTRACT A KEY FROM A VALUE USING A KEY PATH — "let r be the result of evaluating a key path on a value
 * with value and keyPath; if r is failure, return failure; let key be the result of converting a value to a key
 * with r; if key is 'invalid value' or 'invalid type', return invalid; return key".
 *
 * `value` MUST BE THE OUTPUT OF StructuredDeserialize, which is the standard's own precondition ("assertions
 * can be made in the above steps because this algorithm is only applied to values that are the output of
 * StructuredDeserialize and only access 'own' properties") and is what lets every property read below be a
 * SLOT read rather than an operation. §4.5's `put` satisfies it by extracting from the CLONE and never from the
 * page's own object. It is asserted at the read (JS_GetOwnSlot refuses a Proxy and an accessor), not restated
 * as a test here.
 *
 * `key_path` MUST BE §2.5-VALID — the two functions above are what makes that true at §4.4, and this asserts
 * it, because the walk splits exactly the segments they accept.
 *
 * THERE IS NO multiEntry FLAG, and that is a fact about this engine rather than a step skipped. §7.1's optional
 * flag has exactly one producer in the standard — §6.1's "for each index which references store" step, passing
 * the index's own flag — and §2.6's index does not exist, so nothing can pass one; the conversion the flag
 * selects ("convert a value to a multiEntry key") does not exist either. The parameter arrives with the index,
 * with its producer.
 *
 * *pkey is OWNED and is a key record (core/indexeddb/idb_key.h) on IDB_KEY_PATH_KEY, JS_UNDEFINED otherwise.
 *
 * THIS IS THE C ENTRY, for a caller with no flow under it — an in-C fixture. Its step 3 is §7.4, whose ARRAY arm
 * runs the page's own code, so a key path that resolves to an Array (which every LIST key path does, its step 1
 * assembling one) crashes here exactly where §7.4's own C entry does. Every member drives the walk below. */
IdbKeyPathResult idb_key_path_extract(JSContext *ctx, JSValueConst value, JSValueConst key_path, JSValue *pkey);

/* §7.1 AS A DELEGATABLE ALGORITHM, in core/indexeddb/idb_key_array.h's shape and for its reason: its step 3 is
 * §7.4, so a member performing §7.1 has that algorithm's rest points inside its own. Steps 1-2 (evaluate, and
 * `failure`) and steps 4-5 (`invalid`, and the key) are each one O(1) engine action over data this engine owns
 * and rest nowhere — the walk of the key path is over §2.5's own segments, whose count is the store's key path
 * and not the page's value — so every stage of this block is §7.4's, which is why it expands that one.
 *
 * THE EXTRA STATE IS AN ENUM AND THEREFORE THE CALLER'S OWN FIELD, unlike §2.9's, which is a reference and needs
 * a record with a `visit`. `*pres` is written by `start` and read back by `take`: it carries WHETHER step 2
 * already answered `failure`, which is the one thing the walk cannot say (it was never begun). It is POD, so it
 * rides the state's byte copy into a forked arm and into a parked snapshot with nothing to declare.
 *
 * SO THIS ALGORITHM HAS NO `run` AND NO `visit` OF ITS OWN, and that is the whole of the asymmetry with §2.9: the
 * record the stages drive IS the caller's own IdbKeyWalk, so the caller's block calls idb_key_walk_run over it
 * and its `visit` already names it. There is nothing here for a forwarding pair to forward. */
#define IDB_KEY_PATH_EXTRACT_ALGO_STAGES(X, P, W) \
    IDB_KEY_ARRAY_ALGO_STAGES(X, P, W " → Indexed Database §7.1 extract a key from a value using a key path " \
                                      "step 3 (let key be the result of converting a value to a key with r)")

void idb_key_path_walk_start(JSContext *ctx, JSStepHdr *hdr, IdbKeyWalk *w, IdbKeyPathResult *pres,
                             JSValueConst value, JSValueConst key_path, int base, int after);

/* STEPS 4-5 AT THE CALLER'S OWN STAGE, over the answer `start` recorded in `res`. Returns §7.1's own three
   answers, with *pkey OWNED on IDB_KEY_PATH_KEY — the same three the C entry returns, from the same two lines. */
IdbKeyPathResult idb_key_path_walk_take(JSContext *ctx, IdbKeyWalk *w, IdbKeyPathResult res, JSValue *pkey);

/* §7.2's CHECK THAT A KEY COULD BE INJECTED INTO A VALUE — "the result of these steps is either true or false".
 *
 * §4.5's `add or put` step 11.4.2 is the caller and a false is its "DataError": a store with a key generator and
 * the key path "id" cannot store `123`, because a number has nowhere to put an `id`. It runs on the arm where
 * §7.1's extract answered FAILURE, which is the arm §6.1 then generates and INJECTS on — so this is the
 * question "will the injection below have somewhere to write", asked before anything is written.
 *
 * `value` must be the output of StructuredDeserialize (§4.5 step 10's clone), for §7.1's reason and asserted the
 * same way: every read is an own-slot read. `key_path` is a §2.5-valid NON-EMPTY STRING — §7.2's own note is why
 * a sequence cannot arrive, and §4.4's createObjectStore is what makes the note true. */
bool idb_key_path_can_inject(JSContext *ctx, JSValueConst value, JSValueConst key_path);

/* §7.2's INJECT A KEY INTO A VALUE USING A KEY PATH — §6.1's step 1.1.3, "if store also uses in-line keys, then
 * run inject a key into a value using a key path with value, key and store's key path".
 *
 * IT MUTATES `value`, which is the whole of what the algorithm is: a store with in-line keys and a key generator
 * files the generated key INTO the record's value, which is how `store.put({name:'n'})` comes back out of §6.2
 * carrying `id: 1`. The value is §4.5 step 10's CLONE and never the page's own object, so the write is on
 * something no page holds a reference to and there is nothing for §5.5 step 2 to revert — what the abort undoes
 * is the record, and the record's value is a copy of this one.
 *
 * `key` is a key record (core/indexeddb/idb_key.h); §7.3 converts it on the way in. There is no answer: every
 * step of these is an assertion, because §4.5 step 11.4.2 ran the check above over this same pair first. */
void idb_key_path_inject(JSContext *ctx, JSValueConst value, JSValueConst key, JSValueConst key_path);

#endif
