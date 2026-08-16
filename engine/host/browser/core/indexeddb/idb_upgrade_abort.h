/* INDEXED DATABASE §5.8's ABORT AN UPGRADE TRANSACTION. See idb_upgrade_abort.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_UPGRADE_ABORT_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_UPGRADE_ABORT_H

#include "quickjs.h"

/* §5.5 step 3: "If transaction is an upgrade transaction, run the steps to ABORT AN UPGRADE TRANSACTION with
 * transaction." §5.5 is the only caller and there can be no other: the standard states these steps in one
 * place and invokes them from one, and §5.5's own note says what they are for — "this reverts changes to all
 * connection, object store handle, and index handle instances associated with transaction". §5.5 step 2 puts
 * the DATABASE back; this puts back what the PAGE can still see of it.
 *
 * ITS POSITION IS PART OF ITS CONTRACT and is asserted rather than assumed: it runs AFTER step 2's revert
 * (whose result it reads) and BEFORE step 4's "set transaction's state to finished" (which empties the record
 * of what this transaction changed, and step 5 is a question only that record can answer). */
void idb_abort_upgrade_transaction(JSContext *ctx, JSValueConst tx);

#endif
