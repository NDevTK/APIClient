/* Observable / Subscriber — the Observable standard (WICG, upstreaming into DOM). See observable.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_OBSERVABLE_H
#define ENGINE_HOST_BROWSER_CORE_DOM_OBSERVABLE_H

#include "quickjs.h"

/* The agent's declarations: the two class ids, every step definition, and the realm-registry entry. */
void observable_init(JSContext *ctx);
/* §2.1's and §2.2's interface prototype objects for ONE realm — declared into core/realm.h's list. */
void observable_install_protos(JSContext *ctx);
/* The two interface objects on the global. */
void observable_install(JSContext *ctx, JSValueConst global);
/* Agent teardown: the step ids and class ids are the agent's; the prototypes are the realms'. */
void observable_free(JSContext *ctx);

#endif
