/* HTML §10.2.1.1 "The WorkerGlobalScope common interface" and §10.2.1.2 "Dedicated workers and the
   DedicatedWorkerGlobalScope interface" — the two interfaces a worker realm's global object implements. See
   worker_global_scope.c for which of their members this engine can compute and which are named residuals. */
#ifndef ENGINE_HOST_BROWSER_CORE_WORKERS_WORKER_GLOBAL_SCOPE_H
#define ENGINE_HOST_BROWSER_CORE_WORKERS_WORKER_GLOBAL_SCOPE_H
#include <stdbool.h>

#include "quickjs.h"

/* Declared ONCE PER AGENT, from core/platform.c's `worker_global_scope` row. */
void worker_global_scope_init(JSContext *ctx);
void worker_global_scope_free(JSRuntime *rt);

/* DOES `v` IMPLEMENT §10.2.1.1's WorkerGlobalScope? — Web IDL §3.8 Platform objects implementing interfaces'
 * "A JavaScript value value implements an interface interface if value is a platform object and the inclusive
 * inherited interfaces of value.[[PrimaryInterface]] contains interface", which is the test §3.7.6 Attributes'
 * create an attribute getter step 1.1.2.3 makes before it runs any getter steps.
 *
 * IT IS A FUNCTION AND NOT A CLASS-ID COMPARISON AT EACH SITE BECAUSE THE SET GROWS. Today the only interface
 * in this build whose inclusive inherited interfaces contain WorkerGlobalScope is DedicatedWorkerGlobalScope,
 * so one class id answers it; §10.2.1.3's SharedWorkerGlobalScope and Service Workers' ServiceWorkerGlobalScope
 * are two more, and each is a second id this predicate must accept. One place to change is what keeps a member
 * of the BASE interface from silently answering "not a WorkerGlobalScope" to a shared worker. */
bool worker_global_scope_implements(JSValueConst v);

#endif
