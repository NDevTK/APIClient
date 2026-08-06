/* CountQueuingStrategy and ByteLengthQueuingStrategy — the Streams Standard §7. See queuing_strategy.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_STREAMS_QUEUING_STRATEGY_H
#define ENGINE_HOST_BROWSER_CORE_STREAMS_QUEUING_STRATEGY_H
#include "quickjs.h"

void queuing_strategy_init(JSContext *ctx);
void queuing_strategy_install(JSContext *ctx, JSValueConst global);
void queuing_strategy_free(JSContext *ctx);

#endif
