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
 * of the BASE interface from silently answering "not a WorkerGlobalScope" to a shared worker.
 * IT LEAVES THIS DIRECTORY AS DATA. core/events/ cannot name this header — a host that installs events would
 * then link the worker interfaces — so worker_global_scope_init hands the events layer a forwarder over this
 * predicate, which is how HTML §8.1.8.1 Event handlers' event handler processing algorithm step 4 gets the
 * second implementer of HTML §8.2 The WindowOrWorkerGlobalScope mixin. A third worker global interface therefore
 * reaches that step through this one function too. */
bool worker_global_scope_implements(JSValueConst v);

/* WEB IDL §3.7.3 "Interface prototype object" — `WorkerGlobalScope.prototype` in THIS realm, or JS_UNDEFINED
 * where this realm's §3.3.8 [Global] names are not a worker's. OWNED by the caller.
 *
 * IT IS ASKED BY THE COMPONENT THAT OWNS A MEMBER, NOT BY A REALM TEST. Web IDL §2.3 "Interface mixins" makes
 * a mixin's members the including interface's own, so every member HTML §8.2 "The WindowOrWorkerGlobalScope
 * mixin" brings is a `Window` member in a Window realm and a `WorkerGlobalScope` member in a worker one — and
 * §3.7.3's [Global] conditional then places it on the global in the first and on THIS object in the second.
 * A component owning one of those members asks for this object and installs onto whichever it gets; the
 * choice is §3.7.3's arm and browser/idl_exposure.h's IDL_GLOBALS band is what asserts it on both sides.
 *
 * IT IS THIS DIRECTORY'S TO ANSWER AND THE OTHER DIRECTION WOULD BE WRONG: core/idl_args.c may not include
 * this header — core/ depending on workers/ inverts the dependency and would make every host that installs
 * an attribute link the worker layer — which is why §3.7.6's `target` travels to that file as DATA (this
 * header's predicate above) rather than as a name it could look up.
 * WHAT WOULD RETIRE THE CALL ALTOGETHER is a PRODUCTION Web IDL §3.7.3 census on core/realm.h: that file
 * already records every interface prototype object a realm builds, by identifier and BY OBJECT, and
 * realm_interface_prototype_object hands it back — but the whole census is `#if APICLIENT_DEV`, so no release
 * build can reach an interface's prototype by name. The day it is not, a component asks the realm for
 * `WorkerGlobalScope`'s prototype and names no component at all. */
JSValue worker_global_scope_proto(JSContext *ctx);

#endif
