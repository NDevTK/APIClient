/* CountQueuingStrategy and ByteLengthQueuingStrategy — the Streams Standard §7. See queuing_strategy.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_STREAMS_QUEUING_STRATEGY_H
#define ENGINE_HOST_BROWSER_CORE_STREAMS_QUEUING_STRATEGY_H
#include "quickjs.h"

void queuing_strategy_init(JSContext *ctx);
/* §7's prototypes, size functions and INTERFACE OBJECTS, per realm — Web IDL §3.8 "Platform objects
   implementing interfaces" is given a realm, so there is no per-document half to declare here. */
void queuing_strategy_install_protos(JSContext *ctx);
void queuing_strategy_free(JSContext *ctx);

#endif
