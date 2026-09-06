/* Observable / Subscriber — the Observable standard (WICG, upstreaming into DOM). See observable.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_OBSERVABLE_H
#define ENGINE_HOST_BROWSER_CORE_DOM_OBSERVABLE_H

#include "quickjs.h"

/* The agent's declarations: the two class ids, every step definition, and the realm-registry entry. */
void observable_init(JSContext *ctx);
/* Observable §2.1 "The Subscriber interface"'s and §2.2 "The Observable interface"'s interface prototype
   objects AND their two Web IDL §3.7.1 interface objects, for ONE realm — declared into core/realm.h's list.
   Both interfaces are `[Exposed=*]`, and Web IDL §3.8 "Platform objects implementing interfaces" is given a
   REALM and names no Document, so the two names are owed by a realm that reaches no per-document install and
   are placed from here rather than from core/platform.c's third column. */
void observable_install_protos(JSContext *ctx);
/* Agent teardown: the step ids and class ids are the agent's; the prototypes are the realms'. */
void observable_free(JSContext *ctx);

#endif
