/* INDEXED DATABASE §5.12's CREATING A REQUEST TO RETRIEVE MULTIPLE ITEMS, with §6.2's and §6.3's
   retrieve-multiple operations under it. See idb_get_all.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_GET_ALL_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_GET_ALL_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/indexeddb/idb_key_range.h"

/* Declared ONCE PER AGENT: the retrieve-multiple step machine §5.12's operation is. */
void idb_get_all_init(JSContext *ctx);
void idb_get_all_free(JSRuntime *rt);

/* §5.12's `kind`, which is the ONE operand that tells `getAll` from `getAllKeys` from `getAllRecords` — the
   standard states the three members as one algorithm differing in this and nothing else, so it is a magic the
   member declares rather than three bodies that could drift. §6.2's and §6.3's switch names the three. */
enum { IDB_GET_ALL_VALUE = 0, IDB_GET_ALL_KEY, IDB_GET_ALL_RECORD };

/* §2.10's CURSOR DIRECTION, as the four values `IDBCursorDirection` lists. It is here rather than beside §2.10
   because §5.12 is the first algorithm to take one; a cursor takes the same four and must take THESE, or two
   components disagree about what "prevunique" means. The strings are the enumeration's own, in IDL order. */
enum { IDB_DIR_NEXT = 0, IDB_DIR_NEXTUNIQUE, IDB_DIR_PREV, IDB_DIR_PREVUNIQUE };
extern const char *const IDB_CURSOR_DIRECTION_VALUES[5];   /* NULL-terminated, for an IDL_ENUM declaration */

/* §4.5's `IDBGetAllOptions`, DECLARED ONCE. Six members across two files take one, and a dictionary written
 * twice is two answers to one IDL — the members are in §3.2.17's read order, which is LEXICOGRAPHIC (count,
 * direction, query) and which idl_args.c asserts.
 *
 *     dictionary IDBGetAllOptions {
 *       any query = null;
 *       [EnforceRange] unsigned long count;
 *       IDBCursorDirection direction = "next";
 *     };
 *
 * `count` CARRIES NO DEFAULT and that is load-bearing: §6.2 step 1 distinguishes "not given" from a value, and
 * §5.12's own note ("if count is specified and there are more than count records in range, only the first
 * count will be retrieved") is stated about a member that may be absent. IDL_DEFAULT_NONE is how §3.2.17 says
 * a member does not exist, which is a different state from existing and being 0.
 * It is declared IDL_UNRESTRICTED_DOUBLE and not IDL_UNSIGNED_LONG because `[EnforceRange]` REPLACES §3.2.4.7's
 * modulo with a refusal — the same composition core/indexeddb/indexed_db.c states for `open`'s version and
 * core/streams/readable_stream.c for `autoAllocateChunkSize`: the DECLARATION performs ToNumber (the page's
 * `valueOf`, which has to be a request) and the CONSUMER performs the range test, through
 * idb_get_all_count_enforce_range below so there is one copy of it. */
extern const IdlDictMember IDB_GET_ALL_OPTIONS[3];

/* WEB IDL §3.2.4.7's `[EnforceRange] unsigned long` RANGE, over the double a declaration's ToNumber already
 * produced. A fractional, negative, non-finite or too-large value is a TypeError, never a clamp and never a
 * modulo — `store.getAll(query, -1)` throws where an unsigned-long conversion would ask for 4294967295 records.
 *
 * BOTH PLACES `count` ARRIVES SHARE IT: §4.5's positional `optional [EnforceRange] unsigned long count`, and
 * `IDBGetAllOptions`'s member of the same type. Returns 0 with *pcount set, or -1 with the TypeError live.
 *
 * THE POSITIONAL ONE IS TESTED BEFORE THE MEMBER'S OWN STEPS, which is Web IDL's order and observable: an
 * argument is converted before the operation runs, so `deletedStore.getAll(q, -1)` is that TypeError and not
 * §5.12 step 2's "InvalidStateError". The member calls this ahead of its refusals for that reason. */
int idb_get_all_count_enforce_range(JSContext *ctx, JSValueConst v, uint32_t *pcount);

/* §5.12 AS A DELEGATABLE ALGORITHM — the shape core/indexeddb/idb_key_range.h is, and for its reason: step 8's
 * and step 9's "converting a value to a key range" is §7.4 underneath, whose ARRAY ARM runs the page's own
 * code, so §5.12 has that algorithm's rest points inside its own and cannot be a call. A member declares this
 * block in its stage list, embeds an IdbGetAllWalk, chains idb_get_all_walk_visit into its `visit`, hands `run`
 * the base of the block, and performs steps 10-11 with `take` at its own stage.
 *
 * §5.12 STEPS 1-5 ARE THE MEMBER'S, not this block's, and that is not a shortcut: they are "let source be an
 * index or an object store FROM sourceHandle", the two "has been deleted" refusals and the transaction's state
 * — every one of which is asked of a HANDLE, and §4.5's and §4.6's handles are two different records with two
 * different refusal sets (§4.6's also refuses when the index's referenced object store was deleted). Each file
 * already states its own as one function (`os_check`, `ix_check`) that every member of that interface shares,
 * and a copy of them here would be a third. So the member performs steps 1-5 and hands this the SOURCE and the
 * TRANSACTION they produced. */
#define IDB_GET_ALL_ALGO_STAGES(X, P, W) \
    IDB_KEY_RANGE_ALGO_STAGES(X, P, W " → Indexed Database §5.12 step 8's first bullet or step 9's first " \
                                      "bullet (set range to the result of converting a value to a key range " \
                                      "with queryOrOptions, or with queryOrOptions[\"query\"])")

/* §5.12 IN FLIGHT. `rw` is the conversion, which is the whole of the algorithm's suspendable part; everything
   else is what steps 8 and 9 decided BEFORE it and what step 10 mints the operation over. */
typedef struct IdbGetAllWalk {
    IdbRangeWalk rw;
    JSValue  source;      /* step 1's source — a §2.2 object store or a §2.6 index record (owned) */
    JSValue  tx;          /* step 4's transaction (owned) */
    uint32_t count;       /* step 8's or step 9's count, meaningless unless `has_count` */
    uint8_t  has_count;   /* "count IF GIVEN" — §6.2 step 1's own distinction */
    uint8_t  kind;
    uint8_t  is_index;    /* which of step 10's two operations, and which list the walk reads */
    uint8_t  direction;
} IdbGetAllWalk;

/* BEGIN §5.12 AT STEP 6, the member having performed steps 1-5. Performs steps 6-9 here — they are one O(1)
 * engine action each, and "is a potentially valid key range" is O(1) BY CONSTRUCTION: §7.4's arms that decide
 * it all answer without reading anything of the page's, and its Array arm can only ever produce a key or
 * "invalid value", never the "invalid type" that is the only false answer. So the branch is taken without a
 * rest point, and step 8's or step 9's conversion is begun.
 *
 * `options_converted` is TRUE for `getAllRecords`, whose IDL declares `optional IDBGetAllOptions options = {}`
 * — so what arrives is a dictionary Web IDL already converted at the argument boundary, and step 9's three
 * lookups are reads of an engine-built object. It is FALSE for `getAll` and `getAllKeys`, whose first argument
 * is `any`: step 9 must then convert one HERE, which is a capability this engine does not have, and the false
 * branch says so at the site rather than answering with a dictionary nobody built.
 *
 * `count_arg` is the positional argument's already-range-checked value; `has_count_arg` is whether the member
 * was given one. Step 9 OVERWRITES both from the dictionary, which is what makes
 * `store.getAll({count: 10}, 17)` a request for ten records — the positional 17 belongs to an overload the
 * dictionary branch is not.
 *
 * Every operand is BORROWED. Returns the step code the caller must return. */
int  idb_get_all_walk_start(JSContext *ctx, JSStepHdr *hdr, IdbGetAllWalk *w,
                            JSValueConst source, JSValueConst tx, bool is_index, int kind,
                            JSValueConst query_or_options, bool options_converted,
                            uint32_t count_arg, bool has_count_arg, int base, int after);

/* ONE STAGE of step 8's or step 9's conversion. Same contract as idb_key_range_walk_run; `in` is CONSUMED. */
int  idb_get_all_walk_run(JSContext *ctx, JSStepHdr *hdr, IdbGetAllWalk *w, JSValue in, int base,
                          JSValue **out_cb, int *out_argc);

void idb_get_all_walk_visit(JSContext *ctx, IdbGetAllWalk *w, JSStepVisit *v);

/* §5.12 STEPS 10-11 AT THE CALLER'S OWN STAGE: the conversion's "DataError" is reported, the operation is
   minted over what steps 6-9 decided, and the request is the result of asynchronously executing it.
   `source_handle` is §2.8's source object — the IDBObjectStore or IDBIndex whose member ran, which is what
   `request.source` answers with. Returns the IDBRequest OWNED, or JS_EXCEPTION with the DataError live. */
JSValue idb_get_all_walk_take(JSContext *ctx, IdbGetAllWalk *w, JSValueConst source_handle);

#endif
