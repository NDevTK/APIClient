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
 * request result.
 *
 * IT NEED NOT BE ATOMIC, AND THIS PARAGRAPH USED TO SAY IT WAS. The claim was that §5.6 step 5.4's "revert all
 * changes made by operation" had nothing to undo because §6.1 "reports every failure before it copies the
 * value". That was true while step 2 was §6.1's last refusal and is FALSE now that §6.1 step 5 is built: its
 * unique-index "ConstraintError" is raised after step 3 removed the displaced record, step 4 wrote the new one
 * and earlier indexes took their records. So step 5.4 is a REAL step, performed by this machine against the
 * watermark it takes before it performs the operation (idb_database_revert_operation).
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

/* §2.8.1's OPEN REQUEST — "a special type of request used when opening a connection or deleting a database.
 * In addition to success and error events, blocked and upgradeneeded events may be fired at an open request to
 * indicate progress." Its interface is §4.1's IDBOpenDBRequest, which adds exactly those two event handler IDL
 * attributes and nothing else.
 *
 * IT IS NOT PLACED AGAINST A TRANSACTION, which is the whole reason §5.1 and §4.3 write its four §2.8 fields
 * themselves rather than reaching §5.6: "the source of an open request is always null", "the transaction of an
 * open request is null unless an upgradeneeded event has been fired", and its get-the-parent returns null — so
 * there is no transaction to activate around a dispatch and §4.3's own note says that is why §5.9 and §5.10 are
 * not used for it. OWNED. */
JSValue idb_request_new_open(JSContext *ctx);
bool    idb_request_is_open(JSValueConst v);

/* §2.8's FOUR FIELDS, written by the algorithms that own them. §5.1's open, §5.7's upgrade, §5.5's abort of an
   upgrade transaction and §4.3's completion task each write these directly — an open request's progress is
   THEIR steps and not §5.6's — so the writes are asked of this component rather than reached into from four
   files, and each one asserts what §2.8 says the field is. Every value is CONSUMED. */
void idb_request_set_result(JSContext *ctx, JSValueConst req, JSValue result);
void idb_request_set_error(JSContext *ctx, JSValueConst req, JSValue error);
void idb_request_set_transaction(JSContext *ctx, JSValueConst req, JSValue tx);
void idb_request_set_done(JSContext *ctx, JSValueConst req, bool done);
void idb_request_set_processed(JSContext *ctx, JSValueConst req, bool processed);
/* §5.1 step 10.9's "If request's error is set" — the value, OWNED, JS_NULL when no error occurred. */
JSValue idb_request_error(JSContext *ctx, JSValueConst req);

/* §2.8's SOURCE OBJECT, as §4.5's `createIndex` reads it: that member may only create an index into a store
   whose content is SETTLED, and "settled" is a question about which PENDING REQUESTS of this transaction are
   placed against THIS store — so the pending requests have to be able to say what they are against. OWNED,
   JS_NULL for an open request ("the source of an open request is always null"). */
JSValue idb_request_source(JSContext *ctx, JSValueConst req);

/* §2.8's TRANSACTION, as the algorithms outside this component read it: §5.4 step 2.5.4 and §5.5 step 7.3 each
   reach the open request FROM the upgrade transaction and back. OWNED, JS_NULL when there is none. */
JSValue idb_request_transaction(JSContext *ctx, JSValueConst req);

/* Web IDL §3.7.5's brand, asked of a value that arrived from another component. TRUE for an open request too:
   §2.8.1's open request IS a request, and IDBOpenDBRequest inherits IDBRequest, so a brand that answered false
   for one would make `openRequest.readyState` a TypeError. */
bool idb_request_is(JSValueConst v);

#endif
