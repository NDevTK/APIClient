/* THE PER-REALM INTRINSICS, IN ONE PLACE EVERY REALM GOES THROUGH.
 *
 * §3.7 gives every realm its own interface prototype objects, and in this engine that decides ANSWERS and not
 * just identities: a C member runs in the realm that DEFINED it (js_call_c_function takes `ctx` from the
 * function object), so a prototype shared between documents answers every document's question out of whichever
 * realm happened to build it first. Each such component therefore has an install that builds THIS realm's copy.
 *
 * WHAT THIS FILE EXISTS FOR IS THAT THE LIST OF THEM MUST NOT BE HAND-COPIED. It was: `event_target_install`
 * was written into three hosts' child-realm builders, one line each, and a component added without touching
 * all three would silently share another realm's prototype with nothing to say so. Three components became
 * nine lines the moment a second interface needed the same treatment, which is the point at which a
 * hand-maintained list stops being maintained.
 *
 * SO A COMPONENT DECLARES ITSELF, at agent init, beside the declaration it already makes there — and every
 * realm runs the declared list. A component that declares nothing installs nothing, which is how a host that
 * does not build a given interface stays correct without anybody writing down which host builds what. The
 * ORDER is the declaration order, which is already the dependency order: Event declares before MessageEvent,
 * so a realm's Event.prototype exists before the prototype that chains to it. */
#ifndef ENGINE_HOST_BROWSER_CORE_REALM_H
#define ENGINE_HOST_BROWSER_CORE_REALM_H

#include "quickjs.h"

typedef void (*RealmIntrinsic)(JSContext *ctx);

/* Declared ONCE PER AGENT, by the component, from its own `_init`. */
void realm_declare_intrinsic(RealmIntrinsic install);
/* Run for EVERY realm — the agent's first one after its `_init`s, and each child navigable's realm as it is
   built. Exactly once per realm: each component's install asserts its own prototype is not already there. */
void realm_install_intrinsics(JSContext *ctx);
/* Agent teardown: the declarations are the agent's. */
void realm_intrinsics_free(void);

/* A PER-REALM VALUE THAT IS NOT A PROTOTYPE.
 *
 * §3.7's intrinsics are not all interface prototypes. `Response.json(data)` serialises with %JSON.stringify% —
 * the INTRINSIC, so a page reassigning `JSON.stringify` must not change it — and that intrinsic belongs to the
 * realm the member runs in. Held in a module static it is one realm's function object answering for every
 * document, which is the same defect as a shared prototype and harder to see because nothing about the value
 * says which realm it came from.
 *
 * THE STORE IS QUICKJS'S OWN PER-CONTEXT SLOT ARRAY, reached by declaring a class whose slot never holds a
 * prototype. That array is what the runtime already frees with the context, so a realm's value is released
 * exactly when the realm is and there is no second lifetime to get wrong. Declared ONCE PER AGENT (from a
 * component's `_init`), set from that component's per-realm install, read wherever the member runs. */
int     realm_value_declare(JSContext *ctx, const char *what);
void    realm_value_set(JSContext *ctx, int slot, JSValue v);   /* CONSUMES v */
JSValue realm_value_get(JSContext *ctx, int slot);              /* OWNED: the caller frees */

#endif
