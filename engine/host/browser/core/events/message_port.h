/* MessagePort and MessageChannel — HTML §9.4.2 and §9.4.3. See message_port.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_MESSAGE_PORT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_MESSAGE_PORT_H
#include <stdbool.h>

#include "quickjs.h"

void message_port_init(JSContext *ctx);
void message_port_install(JSContext *ctx, JSValueConst global);
void message_port_free(JSContext *ctx);

/* IS THIS A MessagePort? MessageEvent's `source` union and its `sequence<MessagePort> ports` both brand-test,
   and a transfer list has to recognise one before it can refuse or move it. */
bool message_port_is(JSValueConst v);

/* §9.4.3's two entangled ports, for a caller that is not the MessageChannel constructor — a transferred port
   pair, and every specification that hands a page one end of a channel it owns the other of. Answers port1
   (owned) and writes port2 (owned) through the out-parameter. */
JSValue message_port_pair(JSContext *ctx, JSValue *port2);

#endif
