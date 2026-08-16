/* INDEXED DATABASE §2.4's KEY — the value everything else in that standard is defined over. See idb_key.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

/* §7.4's CONVERT A VALUE TO A KEY, FOLLOWED BY THE STEP EVERY MEMBER THAT TAKES ONE PERFORMS. §7.4 answers a
 * key, "invalid value" or "invalid type", and every caller in §4 spells the next step identically — §4.3's
 * `cmp`, §4.7's `only`, `lowerBound`, `upperBound`, `bound` and `includes` each say "If it is 'invalid value'
 * or 'invalid type', throw a 'DataError' DOMException". Six copies of one sentence is six chances to write it
 * once as something else, so the sentence is here and the members state their own steps around it.
 *
 * The two invalid answers are NOT collapsed into one by this: no algorithm in the standard tells them apart
 * (§7.1 maps both to `invalid`, §7.4's own array arm maps both to "invalid value"), and the day one does, the
 * distinction is restored in the ANSWER rather than reconstructed by a caller from an exception name.
 *
 * Returns 0 with `*pkey` an owned key, or -1 with the "DataError" DOMException live. */
int idb_key_from_value(JSContext *ctx, JSValueConst input, JSValue *pkey);

/* §7.3's CONVERT A KEY TO A VALUE — the other direction, which is what a member that hands a key BACK to the
   page returns (§4.7's `lower` and `upper`, §4.8's `key`, §6.2's retrieve-a-key). OWNED. */
JSValue idb_key_to_value(JSContext *ctx, JSValueConst key);

/* §2.5's VALID KEY PATH, over ONE string. "A valid key path is one of: an empty string; an identifier, which is
 * a string matching the IdentifierName production from the ECMAScript Language Specification; a string
 * consisting of two or more identifiers separated by periods (U+002E FULL STOP)" — and the standard's own note
 * is the whole of why this is a real test rather than a glance: "spaces are not allowed within a key path".
 *
 * §4.4's `createObjectStore` is the caller, and the answer is a "SyntaxError" DOMException there. It is asked
 * of THIS component because a key path is the address of a KEY inside a value, so the rule that says which
 * addresses exist belongs beside the rule that says which values are keys — and because §7.1's extract-a-key
 * will walk exactly the segments this accepts.
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

/* §2.4's COMPARE TWO KEYS — -1, 0 or 1. It is THE ordering of this standard: §2.2's list of records is sorted
   by it, §2.9's key range is bounded by it, §2.10's cursor walks in it, and §4.3's `cmp` is it, exposed. */
int idb_key_compare(JSContext *ctx, JSValueConst a, JSValueConst b);

#endif
