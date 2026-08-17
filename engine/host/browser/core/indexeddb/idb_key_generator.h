/* INDEXED DATABASE §2.11's KEY GENERATOR — the number a store files a record under when the page gives none.
   See idb_key_generator.c. Its state lives on §2.2's object store (core/indexeddb/idb_database.h). */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_GENERATOR_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_GENERATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

/* §2.11's CEILING, which is the STANDARD's number and not a machine one: "if key is greater than 2^53
   (9007199254740992), then return failure", and the current number "is always a positive integer less than or
   equal to 2^53 (9007199254740992) + 1". Its note says where the number comes from — "integers greater than
   9007199254740992 cannot be uniquely represented as ECMAScript Numbers ... 9007199254740992 + 1 ===
   9007199254740992 in ECMAScript" — which is also why the current number is NOT held as one. */
#define IDB_KEY_GENERATOR_CEILING INT64_C(9007199254740992)

/* §2.11's generator, as the record §2.2 hangs on a store that "can be specified to use a key generator". "The
   initial value of a key generator's current number is 1, set when the associated object store is created", so
   idb_object_store_create is the one caller. OWNED. */
JSValue idb_key_generator_new(JSContext *ctx);

/* §2.11's CURRENT NUMBER, read. Every read asserts the standard's own range on it, because everything else in
   this component is stated over that range: the ceiling test, the increase, and the exactness of the double the
   generated key carries. */
int64_t idb_key_generator_number(JSContext *ctx, JSValueConst gen);

/* §5.5 step 2's HALF OF THE WRITE — "if a transaction is aborted, the current number of the key generator for
 * each object store in the transaction's scope is reverted to the value it had before the transaction was
 * started". It is a separate entry from the two algorithms below and not a shared setter, because the invariant
 * differs and each half asserts its own: the algorithms only ever RAISE the current number ("the current number
 * for a key generator never decreases, other than as a result of database operations being reverted"), and this
 * is that exception, which only ever lowers it. One setter would have had to assert neither. */
void idb_key_generator_revert(JSContext *ctx, JSValueConst gen, int64_t number);

/* §2.11's GENERATE A KEY FOR AN OBJECT STORE — "let generator be store's key generator; let key be generator's
 * current number; if key is greater than 2^53, then return FAILURE; increase generator's current number by 1;
 * return key".
 *
 * §6.1's step 1.1.1 is the caller and false is its `failure`, which that algorithm reports as a
 * "ConstraintError" — this component throws nothing, because the exception §6.1 names is §6.1's sentence.
 * *pkey is an OWNED key record (core/indexeddb/idb_key.h) of type number on true, JS_UNDEFINED on false.
 *
 * THE TRANSACTION IS AN OPERAND for the same reason every mutation in idb_database.h takes one: the increase is
 * a change to the database, and §5.5 step 2 can only undo a change that was recorded against the transaction
 * that made it. */
bool idb_key_generate(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValue *pkey);

/* §2.11's POSSIBLY UPDATE THE KEY GENERATOR FOR AN OBJECT STORE WITH KEY — "if the type of key is not number,
 * abort these steps; let value be the value of key; set value to the minimum of value and 2^53; set value to the
 * largest integer not greater than value; let generator be store's key generator; if value is greater than or
 * equal to generator's current number, then set generator's current number to value + 1".
 *
 * §6.1's step 1.2 is the caller — the arm where the page DID specify a key. The standard's note is the whole of
 * what the type test buys: "keys of type date, array (regardless of the other keys they contain), binary, or
 * string (regardless of whether they could be parsed as numbers) have no effect on the current number".
 *
 * IT ANSWERS WHETHER IT RAISED THE CURRENT NUMBER, which is not decoration: §6.1 asserts §2.11's "if an
 * insertion fails due to constraint violations or IO error, the key generator is not updated" against it, and
 * that assertion is the reason this engine needs no operation-level revert yet. */
bool idb_key_generator_possibly_update(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst key);

#endif
