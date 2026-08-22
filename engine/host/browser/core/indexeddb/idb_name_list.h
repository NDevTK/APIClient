/* INDEXED DATABASE §2 Constructs — CREATE A SORTED NAME LIST. See idb_name_list.c.
 *
 * THE NUMBER IS §2 AND NOT §5.12, which is "Creating a request to retrieve multiple items" — a different
 * algorithm in the same standard that this component's siblings genuinely call. The sort is defined among §2's
 * definitions of `name`, beside the sentence that makes one "an arbitrary sequence of 16-bit code units",
 * which is exactly why its ordering is the code-unit one.
 *
 * IT IS ONE ALGORITHM WITH THREE CALLERS — §4.4's `objectStoreNames`, §4.5's `indexNames` and §4.10's
 * `objectStoreNames` — and each of those members is one sentence plus this: "let names be a list of the names
 * of the [object stores in this's object store set | indexes in this's index set | object stores in this's
 * scope]. Return the result of creating a sorted name list with names." The ORDER is the part a caller cannot
 * be trusted to restate, because a wrong one is a wrong answer that passes every length and containment check
 * a test writes; so the sort lives here and the members hand it their own set. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_NAME_LIST_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_NAME_LIST_H

#include "quickjs.h"

/* "To create a sorted name list from a list names: let sorted be names sorted in ascending order with the code
 * unit less than algorithm. Return a new DOMStringList associated with sorted."
 *
 * `names` is an Array of DOMString in whatever order its set is held in and is CONSUMED; the answer is an OWNED
 * DOMStringList (core/html/dom_string_list.h). The standard's own note is that this ordering "matches the
 * sort() method on an Array of strings" — ECMAScript's default comparator, which is code UNITS and not code
 * points, so the comparison is core/indexeddb/idb_key.h's rather than a byte compare. */
JSValue idb_sorted_name_list(JSContext *ctx, JSValue names);

#endif
