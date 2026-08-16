/* INDEXED DATABASE §2.5's KEY PATH and §7.1 over one. See idb_key_path.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_PATH_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_PATH_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

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
 * *pkey is OWNED and is a key record (core/indexeddb/idb_key.h) on IDB_KEY_PATH_KEY, JS_UNDEFINED otherwise. */
IdbKeyPathResult idb_key_path_extract(JSContext *ctx, JSValueConst value, JSValueConst key_path, JSValue *pkey);

#endif
