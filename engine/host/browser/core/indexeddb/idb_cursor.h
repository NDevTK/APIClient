/* INDEXED DATABASE §2.10's CURSOR, §4.9's IDBCursor and IDBCursorWithValue, and §6.7's iterate a cursor.
   See idb_cursor.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_CURSOR_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_CURSOR_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared once per AGENT: §4.9's two classes, its members, §6.7's machine, and the per-realm install. The one
   agent-lifetime JS value is the private Symbol §2.10's fields hang off, which is what the release takes back. */
void idb_cursor_init(JSContext *ctx);
void idb_cursor_free(JSRuntime *rt);

/* §4.9's `enum IDBCursorDirection { "next", "nextunique", "prev", "prevunique" }` — Web IDL §3.2.18's
   enumeration, whose value list IS the type. It is exported because the FOUR members that open a cursor
   (§4.5's and §4.6's `openCursor` and `openKeyCursor`) each declare a position of this type, and four copies of
   the four identifiers would be four answers to what the enumeration admits. NULL-terminated, static. */
extern const char *const IDB_CURSOR_DIRECTIONS[];

/* THE SAME ENUMERATION AS THE BRANCH EVERY CONSUMER WRITES — the index of each value in the list above, which
   is what makes the two one declaration rather than two that could be reordered apart. §6.7's iterate-a-cursor
   and §6.2/§6.3's retrieve-multiple each switch on all four, and a second enum in either would silently
   disagree with this list the day a value moved. */
enum { IDB_CURSOR_DIR_NEXT = 0, IDB_CURSOR_DIR_NEXTUNIQUE, IDB_CURSOR_DIR_PREV, IDB_CURSOR_DIR_PREVUNIQUE };

/* §3.2.18's ENUMERATION, DECODED — the one place a direction string becomes a branch. `v` is the string the
   IDL's conversion has already checked against the list, so an unrecognised one is this engine disagreeing
   with its own declaration and is ALWAYS fatal rather than silently meaning "next". */
int idb_cursor_direction_of(JSContext *ctx, JSValueConst v);

/* §4.5's and §4.6's `openCursor`/`openKeyCursor` step "let cursor be A NEW CURSOR with its source handle set to
 * this, undefined position, direction set to direction, got value flag set to false, undefined key and value,
 * range set to range, and key only flag set to …" — every field that step names, and nothing derived.
 *
 * WHAT IS NOT A FIELD HERE IS §2.10's SOURCE, and that is the standard's own shape: "a cursor has a source,
 * which is an index or an object store FROM THE CURSOR'S SOURCE HANDLE". It is the handle's associated index or
 * associated object store, asked of the handle, so there is one answer rather than two that could disagree
 * after §4.5's `deleteIndex` — the same argument §2.6.1 makes for an index handle's transaction.
 *
 * THE TRANSACTION IS A FIELD, and §2.10 makes it one for the opposite reason: "a cursor has a transaction,
 * which is the transaction FROM the cursor's source handle" — taken at the moment the cursor is created, which
 * is what §scheduler's rule demands of anything an iteration later reads. The four members of §4.9 that refuse
 * an inactive transaction read THIS one, not whichever the handle names by then.
 *
 * `source_handle` is the IDBObjectStore or IDBIndex whose member is creating it; `direction` is §4.9's
 * IDBCursorDirection, already converted by the IDL; `range` is a §2.9 key range. All BORROWED. The cursor wears
 * IDBCursorWithValue exactly when `key_only` is false, which is §4.9's own last sentence. OWNED. */
JSValue idb_cursor_new(JSContext *ctx, JSValueConst source_handle, JSValueConst transaction,
                       const char *direction, JSValueConst range, bool key_only);

/* §4.5's and §4.6's `openCursor` last steps but one, "SET CURSOR'S REQUEST TO REQUEST". It is a write and not a
   creation operand because §5.6 needs the cursor to mint the operation and the cursor needs §5.6's answer — the
   standard states the cycle in that order and this is the half that closes it. `request` is BORROWED. */
void idb_cursor_set_request(JSContext *ctx, JSValueConst cursor, JSValueConst request);

/* §4.5's and §4.6's `openCursor` step "let operation be an algorithm to run ITERATE A CURSOR with the current
   Realm record and cursor" — §6.7 closed over the cursor alone, which is the shape every opening member states.
   §4.9's `advance`, `continue` and `continuePrimaryKey` mint the same operation with the further operands their
   own steps name, through the entry below. `cursor` is BORROWED; the callable is OWNED. */
JSValue idb_cursor_iterate_operation(JSContext *ctx, JSValueConst cursor);

/* Web IDL §3.8 Platform objects implementing interfaces' "value implements an interface interface", asked of a
   value that arrived from another component. TRUE for an IDBCursorWithValue
   too: §4.9 declares `interface IDBCursorWithValue : IDBCursor`, so a brand that answered false for one would
   make `cursor.key` a TypeError on every cursor a page actually holds. */
bool idb_cursor_is(JSValueConst v);

#endif
