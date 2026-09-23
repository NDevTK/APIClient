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
/* THE AGENT'S HALF, RUN ONCE FROM core/platform.c's RELEASE COLUMN. It takes the RUNTIME and not a JSContext
   because that is what an AGENT is: the step ids, the two class ids, the slot key and its interned name are
   held for the whole agent and are given back against the runtime they were minted in, which is that column's
   entry condition — core/platform.c's own note says a row that wanted a JSContext would be a per-realm
   component in the wrong column. The prototypes, the two interface objects and §2.1's four per-realm member
   functions ARE per-realm and go with their contexts, so the context this used to take was read for exactly
   two things, a JS_FreeAtom and a JS_FreeValue, whose runtime-scoped spellings are JS_FreeAtomRT and
   JS_FreeValueRT. */
void observable_free(JSRuntime *rt);

#endif
