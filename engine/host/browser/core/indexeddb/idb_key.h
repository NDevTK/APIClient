/* INDEXED DATABASE §2.4's KEY — the value everything else in that standard is defined over. See idb_key.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_H

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

/* §2.4's COMPARE TWO KEYS — -1, 0 or 1. It is THE ordering of this standard: §2.2's list of records is sorted
   by it, §2.9's key range is bounded by it, §2.10's cursor walks in it, and §4.3's `cmp` is it, exposed. */
int idb_key_compare(JSContext *ctx, JSValueConst a, JSValueConst b);

#endif
