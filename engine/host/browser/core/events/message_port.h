/* MessagePort and MessageChannel — HTML §9.4.2 and §9.4.3. See message_port.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_MESSAGE_PORT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_MESSAGE_PORT_H
#include <stdbool.h>

#include "quickjs.h"

void message_port_init(JSContext *ctx);
/* §9.4.2/§9.4.3's two interface prototype objects AND their two §3.8 global property references, for ONE realm
   — declared into core/realm.h's list. ONE entry because Web IDL §3.8 `define the global property references`
   is given "target" and "a realm realm" and its step 1 population is "every interface that is exposed in
   realm": no Document appears in it. Both identifiers are `[Exposed=(Window,Worker)]`, so a WORKER realm owes
   both — and while the interface objects were installed from core/platform.c's per-document column, a worker
   realm, which reaches no platform_document_install, received neither. */
void message_port_install_realm(JSContext *ctx);
/* Agent teardown — core/platform.h's release column. It takes the RUNTIME because everything it gives back is
   the agent's: two class ids, three pool entries, the delivery callee, the live-port table, and HTML §8.1.7.2's
   handler-set hook, which this component claimed in core/events/event_target.c and must release before that
   component does. The per-realm prototypes go with their contexts. */
void message_port_free(JSRuntime *rt);

/* IS THIS A MessagePort? MessageEvent's `source` union and its `sequence<MessagePort> ports` both brand-test,
   and a transfer list has to recognise one before it can refuse or move it. */
bool message_port_is(JSValueConst v);

/* HOW MANY LIVE MessagePorts HAVE THIS REALM AS THEIR RELEVANT REALM — AND IT NO LONGER STANDS ON A STEP OF
   §7.5.10. It was written for that section's steps 4 and 5, which the 3 October 2026 edition DELETED: the
   enumeration and the disentangle both went, `disentangle` occurs nowhere in §7.5.10, and the obligation was
   RETIRED upstream rather than renumbered, so there is no step here to re-key to. §9.4.6 "Ports and garbage
   collection" states liveness as a strong reference and mentions disentangling only as author advice.
   THE RETIRED ARGUMENT IS KEPT BECAUSE A READER RE-DERIVES IT: the SET was enumerable (message_port.c keeps
   the live ports and each records its realm) and the DISENTANGLE was not, a disentangle being a per-flow WRITE
   that a borrowed-pointer list cannot attribute — so document_lifecycle.c used the count to STOP rather than to
   reach into a timeline it cannot see.
   WHETHER THIS COUNT STILL HAS A CONSUMER IS NOT DECIDED HERE. Its one caller is the DCHECK in
   core/frame/document_lifecycle.c's destroy_a_document, and that assert carries the NAMED RESIDUAL deciding
   it: either the guard goes with the deleted steps, or it is incidentally protecting THIS ENGINE's own realm
   ownership, and retiring it would leave this producer with no reader. Read that residual before touching
   either site. */
int message_port_count_in_realm(JSContext *realm);

/* §9.4.3's two entangled ports, for a caller that is not the MessageChannel constructor — a transferred port
   pair, and every specification that hands a page one end of a channel it owns the other of. Answers port1
   (owned) and writes port2 (owned) through the out-parameter. */
JSValue message_port_pair(JSContext *ctx, JSValue *port2);

#endif
