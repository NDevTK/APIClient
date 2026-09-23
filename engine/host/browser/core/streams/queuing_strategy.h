/* CountQueuingStrategy and ByteLengthQueuingStrategy — the Streams Standard §7. See queuing_strategy.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_STREAMS_QUEUING_STRATEGY_H
#define ENGINE_HOST_BROWSER_CORE_STREAMS_QUEUING_STRATEGY_H
#include "quickjs.h"

void queuing_strategy_init(JSContext *ctx);
/* §7's prototypes, size functions and INTERFACE OBJECTS, per realm — Web IDL §3.8 "Platform objects
   implementing interfaces" is given a realm, so there is no per-document half to declare here. */
void queuing_strategy_install_protos(JSContext *ctx);
/* THE AGENT'S HALF, RUN ONCE FROM core/platform.c's RELEASE COLUMN. It takes NOTHING because the state it
   gives back is the AGENT'S and not a realm's: the two prototypes and the two size functions belong to the
   realms that hold them and go with their contexts, so the JSContext this used to take was unread. That
   argument is core/platform.c's own -- a row that wanted a JSContext would be a per-realm component in the
   wrong column -- and the parameter was the last thing making this look like one. */
void queuing_strategy_free(void);

#endif
