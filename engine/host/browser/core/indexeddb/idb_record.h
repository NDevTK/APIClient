/* INDEXED DATABASE §2.12's RECORD SNAPSHOT and §4.8's IDBRecord over it. See idb_record.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_RECORD_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_RECORD_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared ONCE PER AGENT: the class §4.8's brand check asks, and the per-realm install that builds this
   realm's IDBRecord.prototype and interface object. The component holds no agent-lifetime JS value — a
   snapshot's three fields live on the snapshot — so there is nothing to release. */
void idb_record_init(JSContext *ctx);

/* §2.12's "A RECORD SNAPSHOT contains keys and values copied from an object store record or an index record",
 * as the one constructor for one shape. §6.2's and §6.3's "record" arm is the only caller: each ends in "let
 * recordSnapshot be a new record snapshot with its key set to key, value set to value, and primary key set to
 * key", and the two arms differ in WHICH key each of the three is — §6.2 sets key and primary key to the
 * store record's own key, §6.3 sets key to the index key and primary key to the index record's value. That
 * difference is the CALLER'S, which is why this takes all three rather than a store-or-index flag.
 *
 * `key` and `primary_key` are §2.4 KEY RECORDS, not the page values §7.3 makes of them: §4.8's two key
 * getters each state "the result of converting a key to a value", so the conversion is the READ's and a
 * snapshot built out of already-converted values would run §7.3 once at retrieval time for a key the page may
 * never look at — and would answer `record.key === record.key` true for an array key, where two conversions
 * mint two Arrays.
 *
 * `value` IS ALREADY DESERIALIZED, and that is §6.2's own order: its "record" arm runs
 * `! StructuredDeserialize(serialized, targetRealm)` and puts the RESULT on the snapshot, so `record.value`
 * answers with the same object every time it is read. Deserializing in the getter instead would mint a fresh
 * copy per read, which no step of §4.8 states.
 *
 * All three are CONSUMED. The snapshot is OWNED. */
JSValue idb_record_new(JSContext *ctx, JSValue key, JSValue primary_key, JSValue value);

/* Web IDL §3.7.5's brand, asked of a value that arrived from another component. */
bool idb_record_is(JSValueConst v);

/* The AGENT's half, undone — core/platform.h's release column. */
void idb_record_free(void);

#endif
