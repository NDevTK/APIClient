/* INDEXED DATABASE §2.8's REQUEST and §4.1's IDBRequest. See idb_request.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_REQUEST_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_REQUEST_H

#include <stdbool.h>

#include "quickjs.h"

void idb_request_init(JSContext *ctx);
void idb_request_free(JSRuntime *rt);

/* §5.6's ASYNCHRONOUSLY EXECUTE A REQUEST — "with the source object and an operation to perform on a database".
 *
 * `source` is §2.8's source object and is BORROWED: an object store handle, an index or a cursor once §4.6-§4.8
 * exist, and JS_NULL for an open request, whose source §2.8.1 says "is always null".
 *
 * `transaction` is what §5.6 step 1 derives from the source ("let transaction be the transaction associated
 * with source"). It is passed rather than derived because the thing that knows a handle's transaction is
 * §4.6's IDBObjectStore, which does not exist — and because §scheduler's rule is that an operation which
 * becomes a work item takes its inputs WITH it: a transaction read back off the source at task time would be
 * whichever one that handle belonged to then, not the one this request was placed against.
 *
 * `operation` is the DATABASE OPERATION, as a callable this engine built (a step closure over its own
 * operands, so it parks and time-travels like anything else). Its CONTRACT is §5.6's own: it either answers
 * with the operation's result, or it THROWS the DOMException the algorithm names ("this operation failed with
 * a ConstraintError DOMException"), which arrives here as a value because this machine catches an abrupt
 * request result. It must be ATOMIC — an operation that fails must have changed nothing — because §5.6 step
 * 5.4's "revert all changes made by operation" has nothing to undo in this engine and is not written; §6.1
 * satisfies that structurally, reporting every failure before it copies the value it was given.
 *
 * Returns the IDBRequest (§5.6's last step, "Return request"), OWNED. */
JSValue idb_request_execute(JSContext *ctx, JSValueConst source, JSValueConst transaction,
                            JSValueConst operation);

/* §5.5's abort-a-transaction step 6, for ONE request of the aborted transaction's request list: "abort the
   steps to asynchronously execute a request for request, set request's processed flag to true, and queue a
   database task to [set done, set result to undefined, set error to a newly created AbortError DOMException,
   and fire an event named error at request with its bubbles and cancelable attributes initialized to true]".
   It is asked of the request rather than reached into from the transaction because every field it writes is
   this component's. */
void idb_request_abort(JSContext *ctx, JSValueConst request);

/* Web IDL §3.7.5's brand, asked of a value that arrived from another component. */
bool idb_request_is(JSValueConst v);

#endif
