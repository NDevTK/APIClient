/* INDEXED DATABASE §5.12's CREATING A REQUEST TO RETRIEVE MULTIPLE ITEMS, with §6.2's and §6.3's
   retrieve-multiple operations under it. See idb_get_all.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_GET_ALL_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_GET_ALL_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/indexeddb/idb_cursor.h"
#include "core/indexeddb/idb_key_range.h"

/* Declared ONCE PER AGENT: the retrieve-multiple step machine §5.12's operation is. */
void idb_get_all_init(JSContext *ctx);
void idb_get_all_free(JSRuntime *rt);

/* §5.12's `kind`, which is the ONE operand that tells `getAll` from `getAllKeys` from `getAllRecords` — the
   standard states the three members as one algorithm differing in this and nothing else, so it is a magic the
   member declares rather than three bodies that could drift. §6.2's and §6.3's switch names the three. */
enum { IDB_GET_ALL_VALUE = 0, IDB_GET_ALL_KEY, IDB_GET_ALL_RECORD };

/* §2.10's CURSOR DIRECTION is core/indexeddb/idb_cursor.h's — its value list, the enum that indexes it and
   the one decode. §4.9 is where the enumeration is DECLARED, so §5.12 borrows it rather than restating four
   identifiers that would silently disagree the day a value moved. */

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
 * IT IS DECLARED IDL_UNSIGNED_LONG_ENFORCE, WHICH IS §3.3.6 [EnforceRange]'s WHOLE ARM of §3.2.4.9 Abstract
 * operations' ConvertToInt and not a ToNumber with a test after it. That arm is four steps: "If x is NaN, +∞,
 * or −∞, then throw a TypeError"; "Set x to IntegerPart(x)"; "If x < lowerBound or x > upperBound, then throw
 * a TypeError"; "Return x" — never the "Set x to x modulo 2^bitLength" the SAME algorithm performs for a
 * §3.2.4.6 "unsigned long" WITHOUT the attribute, which is what would make `getAll(q, -1)` a request for
 * 4294967295 records. The attribute and the steps are two sections on purpose: §3.3.6 says which types carry
 * it, §3.2.4.9 says what it does, and a citation naming only one of them cannot be checked against the other.
 *
 * THE REFUSAL BELONGS TO THE TYPE AND NOT TO A CONSUMER, and that is a statement about WHEN it runs rather than
 * about where the code sits. Web IDL performs it inside §3.2.17 Dictionary types (ES-to-IDL list) step 4.1.4.1's
 * "converting jsMemberValue to an IDL value whose type is the type member is declared to be of" — so it lands
 * BEFORE the members §2.7 orders after `count` are read at all. It was a consumer's, split as "the DECLARATION
 * performs the ToNumber and the CONSUMER performs the range", and the split cost exactly what §3.2.17's read
 * order exists to make observable: `getAllRecords({count: -1, direction: {toString(){ … }}})` READ `direction`
 * and RAN the page's `toString` — and would report ITS throw — where a browser throws the TypeError `count`
 * already owed and never reaches `direction`. An extra conversion is an extra place for the page's code to run
 * inside an operation the standard has already ended.
 *
 * SO NOTHING HERE PERFORMS A RANGE TEST. A member reads the converted value with idl_number_of, which is the
 * one door a body has to the number a §3.2 conversion produced, and which answers for unknown external input
 * from that value's own example rather than owing C a number it does not have.
 * `open`'s `[EnforceRange] unsigned long long version` (core/indexeddb/indexed_db.c) and
 * `autoAllocateChunkSize` (core/streams/readable_stream.c) still compose, and it is the WIDTH that keeps them
 * there: core/idl_args.h has a row for the attribute over `unsigned long` and none over `unsigned long long`. */
extern const IdlDictMember IDB_GET_ALL_OPTIONS[3];

/* §5.12 AS A DELEGATABLE ALGORITHM — the shape core/indexeddb/idb_key_range.h is, and for its reason: step 8's
 * and step 9's "converting a value to a key range" is §7.4 underneath, whose ARRAY ARM runs the page's own
 * code, so §5.12 has that algorithm's rest points inside its own and cannot be a call. A member declares this
 * block in its stage list, embeds an IdbGetAllWalk, chains idb_get_all_walk_visit into its `visit`, hands `run`
 * the base of the block, and performs steps 10-13 with `take` at its own stage.
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

/* WHICH OF STEP 9's TWO CONVERSIONS A RESUME COMES BACK INTO. Step 9 is two conversions in sequence — Web IDL
   §3.2.17 Dictionary types over `queryOrOptions` itself, and then §2.9's convert-a-value-to-a-key-range over
   the `query` member it produced — and BOTH park inside the ONE stage block above, because neither has a
   stage of its own to park in: §3.2.17's rest points are all REQUESTS, which park and resume at their own call
   site with the hosting machine's stage unmoved, and §2.9's are the block's own. So a stage cannot tell the
   two apart on a resume and this byte is what does.
   THE KEY RANGE IS ZERO because a step state is js_mallocz'd and every other way into this walk — step 8's
   branch, and step 9's for `getAllRecords`, whose dictionary Web IDL converted at the argument boundary —
   begins at the range with no dictionary conversion ever started.

   NAMED RESIDUAL — THE MACHINERY IS RIGHT AND THE LABEL A PARKED FLOW SAYS IS NOT. A flow parked inside a
   member's [[Get]] during step 9's §3.2.17 conversion holds this block's FIRST stage, whose label is §7.4's
   Array arm step 1 (`len is ? ToLength(? Get(input, "length"))`) — a conversion that has not started and may
   never start. The resume itself is correct: the label resolves to that stage, and this byte is what the run
   reads there, so the dictionary conversion continues exactly where it stopped. What is narrower is the
   REPORT. The next diff gives step 9's dictionary a stage of its own in IDB_GET_ALL_ALGO_STAGES below, naming
   Web IDL §3.2.17 Dictionary types, and adds its STEP_ARM to the two members that expand the block
   (core/indexeddb/idb_object_store.c and core/indexeddb/idb_index_handle.c) — which is why it is a separate
   diff and not this one: the block is expanded in files this change does not touch. HOW ITS ABSENCE SHOWS: an
   abort or a park report taken while `store.getAll({get count(){…}})` is running names §7.4's Array arm and
   sends the reader hunting an array conversion that never happened. */
enum { IDB_GET_ALL_CONV_RANGE = 0, IDB_GET_ALL_CONV_OPTIONS };

/* §5.12 IN FLIGHT. `opts` and `rw` are the conversions, which are the whole of the algorithm's suspendable
   part; everything else is what steps 8 and 9 decided BEFORE them and what step 10 mints the operation over. */
typedef struct IdbGetAllWalk {
    IdbRangeWalk rw;
    /* STEP 9's §3.2.17 conversion, for the two members whose first argument is a bare `any`. It carries NO
       nested-conversion frames, and that is a STATEMENT about the declaration rather than a shortcut:
       idl_args.c's `idl_members_depth` over IDBGetAllOptions is 0, because a frame is pushed only for an
       IDL_SEQUENCE_STRING_OR_DICT member and this dictionary declares none. idl_dict_walk_start asserts that
       number against what it is handed, so the day such a member is added the START crashes naming the host
       block that has to be sized for it, rather than the conversion failing at the depth. */
    IdlDictWalk opts;
    JSValue  source;      /* step 1's source — a §2.2 object store or a §2.6 index record (owned) */
    JSValue  tx;          /* step 4's transaction (owned) */
    /* THE CALLER'S OWN STAGE, carried because step 9's two conversions are SEQUENTIAL and only the first of
       them is begun where the caller named it: `run` is handed the block's `base` and nothing else, so the
       point at which the dictionary answers and the key range conversion begins has no other way to know where
       to hand control back. */
    int      after;
    uint32_t count;       /* step 8's or step 9's count, meaningless unless `has_count` */
    uint8_t  has_count;   /* "count IF GIVEN" — §6.2 step 1's own distinction */
    uint8_t  kind;
    uint8_t  is_index;    /* which of step 10's two operations, and which list the walk reads */
    uint8_t  direction;
    uint8_t  conv;        /* IDB_GET_ALL_CONV_* */
} IdbGetAllWalk;

/* BEGIN §5.12 AT STEP 6, the member having performed steps 1-5. Steps 6, 7 and step 8's OWN TEST are one O(1)
 * engine action each, and "is a potentially valid key range" is O(1) BY CONSTRUCTION: §7.4's arms that decide
 * it all answer without reading anything of the page's, and its Array arm can only ever produce a key or
 * "invalid value", never the "invalid type" that is the only false answer. So the branch is taken without a
 * rest point, and the conversion the chosen arm names is BEGUN here and driven from the caller's block.
 *
 * `options_converted` is TRUE for `getAllRecords`, whose IDL declares `optional IDBGetAllOptions options = {}`
 * — so what arrives is a dictionary Web IDL already converted at the argument boundary, and step 9's three
 * lookups are reads of an engine-built object. It is FALSE for `getAll` and `getAllKeys`, whose first argument
 * is `any`: step 9's own §3.2.17 conversion is then run HERE, inside the algorithm, because step 8's "is a
 * potentially valid key range" is what decided the value is a dictionary at all and the argument boundary
 * could not have known. Either way step 9's three lookups read ONE shape, and they are performed in one place.
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

/* §5.12 STEPS 10-13 AT THE CALLER'S OWN STAGE: the conversion's "DataError" is reported, the operation is
   minted over what steps 6-9 decided (step 10), and the request is step 13's "return the result (an
   IDBRequest) of running asynchronously execute a request with sourceHandle and operation".

   STEPS 11 AND 12 ARE INSIDE THE MINT, not beside it: the standard picks the operation per source kind — an
   index takes §6.3's retrieve multiple items from an index, an object store §6.2's from an object store — and
   this hands the closure `is_index` instead, so the one step machine answers for both. That is why ONE mint
   covers three steps, and it is the reason the source kind is a data slot rather than a branch at the mint.
   `source_handle` is §2.8's source object — the IDBObjectStore or IDBIndex whose member ran, which is what
   `request.source` answers with. Returns the IDBRequest OWNED, or JS_EXCEPTION with the DataError live. */
JSValue idb_get_all_walk_take(JSContext *ctx, IdbGetAllWalk *w, JSValueConst source_handle);

#endif
