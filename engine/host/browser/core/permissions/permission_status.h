/* PERMISSIONS §6.3's PermissionStatus. See permission_status.c.
 *
 *   [Exposed=(Window,Worker)]
 *   interface PermissionStatus : EventTarget {
 *     readonly attribute PermissionState state;
 *     readonly attribute DOMString name;
 *     attribute EventHandler onchange;
 *   };
 *
 * IT IS AN EVENT TARGET AND THAT IS THE HALF THAT MATTERS. `state` alone is a value a bundle reads once; the
 * `change` handler is a body it SHIPS and never enters — and §6.3.5 says a PermissionStatus "MUST NOT be
 * garbage collected if it has an event listener whose type is change", which is the standard saying that a
 * page registers one and expects it to fire long after everything else has forgotten the object. That handler
 * is where the granted path of a real bundle lives: the notification subscribe, the location watch, the retry
 * that runs the first request the page could not make while it was prompting.
 *
 * WHICH IS WHY §6.3.4's TRIGGER IS A FORK AND NOT A FLAG. "Whenever the user agent is AWARE that the state of
 * a PermissionStatus instance status has changed, it asynchronously runs the PermissionStatus update steps."
 * The awareness is the user agent's, arriving from a UI this engine does not host, at a moment it cannot
 * observe — so "has it changed" is the same kind of unknown as the state itself, and answering it `no` forever
 * is a fact about this engine's inputs written down as a fact about the world. The status therefore ASKS,
 * through the same fork seam every other unknown is asked through: one arm is the world in which nothing
 * happened and the other is the world in which the user decided, and the second arm runs the update steps for
 * real — it learns WHICH state (a second chained question), writes it into §3.2's store, re-runs the query
 * algorithm and fires `change`.
 *   AND THE ANSWER IS THEN KNOWN, WHICH IS WHY THE STORE IS THE THING IT WRITES TO. A user agent that has
 * become aware of a decision is not ignorant of it any more: §5.2 step 7 writes exactly this ("set a
 * permission store entry with descriptor, key, and current state") and §5.1 step 7 reads it back ahead of the
 * unknown. So every later read in that flow — this status's, another status's over the same feature, a
 * powerful feature's own C-side ask — answers the decision rather than forking again over it. One store, one
 * timeline per flow, because the store is a JS object and the COW delta captures the write.
 *   A HOST WITH NO SOURCE OVERLAY ASKS NOTHING. The state is then the plain default-state string, the ask is
 * skipped because there is no unknown to ask about, and no `change` event is ever fired — which is exactly
 * what a conformance run must see, since nothing has simulated a user. */
#ifndef ENGINE_HOST_BROWSER_CORE_PERMISSIONS_PERMISSION_STATUS_H
#define ENGINE_HOST_BROWSER_CORE_PERMISSIONS_PERMISSION_STATUS_H

#include "quickjs.h"
#include "core/permissions/permission_store.h"

/* Declared ONCE PER AGENT — the class, the step definition its awareness chain runs on, and the per-realm
   install that builds §3.7's prototype and interface object. */
void permission_status_init(JSContext *ctx);
void permission_status_free(void);

/* §6.3.1 "create a PermissionStatus for a given PermissionDescriptor permissionDesc": initialize its [[query]]
   internal slot to permissionDesc and its name to permissionDesc's name. The DEFAULT PERMISSION QUERY
   ALGORITHM then "sets status's state to permissionDesc's permission state", which is §6.2.1's step 8.3 and is
   performed here rather than by the caller, because §6.3.4's update steps perform the identical step and two
   copies of one algorithm is one of them being wrong later.
     OWNED. Minting the status also seeds §6.3.4's awareness chain — see permission_status.c. */
JSValue permission_status_new(JSContext *ctx, const PermissionDescriptor *d);

/* §6.3's INTERFACE PROTOTYPE OBJECT for this realm — Web IDL §3.7, and here it decides ANSWERS: a C member
   runs in the realm that DEFINED it. OWNED: the caller frees. */
JSValue permission_status_proto(JSContext *ctx);

#endif
