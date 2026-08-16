/* MessagePort and MessageChannel — HTML §9.4.2 and §9.4.3. See message_port.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_MESSAGE_PORT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_MESSAGE_PORT_H
#include <stdbool.h>

#include "quickjs.h"

void message_port_init(JSContext *ctx);
/* §9.4.2/§9.4.3's two interface prototype objects for ONE realm — declared into core/realm.h's list. */
void message_port_install_protos(JSContext *ctx);
void message_port_install(JSContext *ctx, JSValueConst global);
/* Agent teardown — core/platform.h's release column. It takes the RUNTIME because everything it gives back is
   the agent's: two class ids, three pool entries, the delivery callee, the live-port table, and HTML §8.1.7.2's
   handler-set hook, which this component claimed in core/events/event_target.c and must release before that
   component does. The per-realm prototypes go with their contexts. */
void message_port_free(JSRuntime *rt);

/* IS THIS A MessagePort? MessageEvent's `source` union and its `sequence<MessagePort> ports` both brand-test,
   and a transfer list has to recognise one before it can refuse or move it. */
bool message_port_is(JSValueConst v);

/* HOW MANY LIVE MessagePorts HAVE THIS REALM AS THEIR RELEVANT REALM — §7.5.10 step 4's set, counted.
   Destroying a Document must disentangle exactly those ports, and this is the enumeration the step needs; the
   disentangle itself is a per-flow WRITE and a borrowed-pointer list cannot say whose flow a port belongs to,
   so document_lifecycle.c uses the count to STOP rather than to reach into a timeline it cannot see. */
int message_port_count_in_realm(JSContext *realm);

/* §9.4.3's two entangled ports, for a caller that is not the MessageChannel constructor — a transferred port
   pair, and every specification that hands a page one end of a channel it owns the other of. Answers port1
   (owned) and writes port2 (owned) through the out-parameter. */
JSValue message_port_pair(JSContext *ctx, JSValue *port2);

#endif
