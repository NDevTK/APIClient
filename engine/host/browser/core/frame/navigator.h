/* THE NAVIGATOR INTERFACE — HTML §8.10.1, Blink core/frame.
 *
 * DECLARED ONCE PER AGENT, BUILT ONCE PER REALM. There is no install to call per document: the component
 * declares itself into core/realm.h's ONE per-realm list, so every realm — the agent's first and every child
 * navigable's — gets its own Navigator.prototype, its own `Navigator` interface object and its own Navigator,
 * with no host holding a hand-copied line that a new host can forget. §3.7 requires that, and here it decides
 * ANSWERS: a C member runs in the realm that DEFINED it. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATOR_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATOR_H
#include <stdbool.h>

#include "quickjs.h"

/* The class, the member declarations and the per-realm intrinsic. Called once per agent, before the first
   realm's intrinsics are installed. */
void navigator_init(JSContext *ctx);

/* THIS REALM'S Navigator — for a component that owns a member of a `partial interface Navigator` declared by
   ANOTHER standard (Storage §2's `storage`). The object is HTML's and the member is theirs, so the member's
   component installs it from its own per-realm intrinsic, declared AFTER this one so the object exists. It is
   the same object §7.2.5's `navigator` hands the page — read from the one realm slot, never a second
   reference. OWNED. */
JSValue navigator_object(JSContext *ctx);

/* THE WEB IDL BRAND, for a member of a `partial interface Navigator` another component owns. §3.7.6
   "Attributes" and §3.7.7 "Operations" both begin by refusing a receiver that "does not implement the
   interface", and the one object per realm WEARS this component's class — so the check is a class-id
   comparison a page cannot forge, and a partial's member asks it here rather than growing a second, weaker
   test of its own (a shape test, or an equality against `navigator_object`, which cannot tell "not a
   Navigator" from "another realm's"). */
bool navigator_is(JSValueConst v);

void navigator_free(void);

#endif
