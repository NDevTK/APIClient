/* INDEXED DATABASE §2.8.2's CONNECTION QUEUE, §5.1's open and §5.7's upgrade. See idb_open.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_OPEN_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_OPEN_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared once per AGENT: the three step machines §5.1, §5.7 and §4.3's completion task are, §2.8.2's
   connection queues, and the two rendezvous this component registers with (a connection becoming closed, an
   upgrade transaction finishing). The queues and the blocked list are agent-lifetime JS values, so there is a
   release. */
void idb_open_init(JSContext *ctx);
void idb_open_free(JSRuntime *rt);

/* §4.3's `open(name, version)` — everything after its own two synchronous steps.
 *
 * The member has ALREADY reported the TypeError for a zero version and obtained the storage key; what is left
 * is "let request be a new open request", "run these steps in parallel: let result be the result of opening a
 * database connection ... queue a database task to [settle the request and fire]", and "return a new
 * IDBOpenDBRequest object for request". This entry is all of that: it mints the open request, files it in
 * §2.8.2's connection queue for `name`, starts §5.1 if the queue's head is now this request, and hands the
 * request back.
 *
 * `has_version` is the difference between `open(name)` and `open(name, v)` — §5.1 step 5 is stated over
 * "version is undefined", and collapsing the two would make `open(name)` on a database at version 7 an upgrade
 * to 1 and then a VersionError. OWNED. */
JSValue idb_open_request(JSContext *ctx, const char *name, double version, bool has_version);

#endif
