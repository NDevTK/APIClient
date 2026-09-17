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

/* THIS REALM'S Navigator — for a component that owns a member ANOTHER standard declares on this interface and
   needs to say WHICH Navigator the call arrived on. It is the same object HTML §7.2.2 "The Window object"'s
   `navigator` hands the page — read from the one realm slot, never a second reference. OWNED.
   IT IS NOT HOW A MEMBER IS INSTALLED, AND THIS COMMENT SAID IT WAS. It read `the member's component
   installs it from its own per-realm intrinsic, declared AFTER this one so the object exists` — shown in
   backticks and not in quotation marks, because a run of THIS TREE'S prose inside quotation marks is read by
   engine/citegen.mjs as a claim about the nearest preceding standard and reported against it, which is what
   it did to this very sentence. Storage §8's `storage` was the one member that did exactly that — onto the
   INSTANCE, which Web IDL §3.7.6 "Attributes" makes a wrong answer: "Regular attributes are exposed on the
   interface prototype object, unless the attribute is unforgeable or if the interface was declared with the
   [Global] extended attribute". A member
   another standard declares here is installed FROM navigator's own per-realm intrinsic with the §3.7.3
   prototype PASSED IN, which is what core/frame/navigator_beacon.h states and what Beacon §2.1 and Storage §8
   both now do. The citation was wrong too and is corrected rather than dropped, because a reader who
   re-derives it writes it again: Storage §2 is "Terminology" and declares nothing; the member is Storage §8
   "API"'s, and §8 declares no partial interface at all — it declares `interface mixin NavigatorStorage` with
   two `includes` statements, which §3.7.3 treats differently from a partial in the one way that matters, a
   mixin having no prototype object of its own.
   WHAT IT IS FOR IS THE REALM QUESTION, and both of its callers ask exactly that: a member reached through
   ONE realm's Navigator.prototype on ANOTHER realm's Navigator answers out of the member's own realm, because
   `js_call_c_function` takes `ctx` from the function object — so a getter compares what arrived against what
   this realm holds and asserts they are the same object. Pair it with `navigator_is` below, which answers the
   §3.7.6/§3.7.7 BRAND: the two are different questions and a member that needs the realm needs both. */
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
