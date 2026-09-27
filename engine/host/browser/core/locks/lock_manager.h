/* THE WEB LOCKS API — Web Locks API §3.1 "Navigator Mixins", §3.2 "LockManager class", §3.3 "Lock class" and
 * the five algorithms of §4 "Algorithms". See lock_manager.c.
 *
 * WHY THIS MEMBER AND NOT ANOTHER. `navigator.locks` was absent, and an absent MEMBER does not throw —
 * ECMAScript §10.1.8.1 "OrdinaryGet ( obj, propertyKey, receiver )" step 2.b returns undefined — so what the
 * absence costs is decided by whatever consumes that undefined a statement later. Here it is a CALLBACK, and a
 * callback is a whole program region: `navigator.locks.request(name, cb)` runs `cb` or it runs nothing, and the
 * code inside `cb` is reached no other way. The absence therefore costs everything behind the lock, which is
 * the loss §NO STUBS is about, and it is the one loss no crash reports.
 * WHAT IS BEHIND IT WAS READ RATHER THAN GUESSED, over the programs a real-network drive saved: a desktop
 * session login whose POST and whose cookie refresh are composed ONLY inside two such callbacks, a session
 * token refresh poll that runs only inside a third, and a query-history migration that reads and writes a
 * store only inside a fourth. None of those addresses is composed anywhere else on its page.
 *
 * ALL OF §2, §3 AND §4 LAND TOGETHER, AND THAT IS THE UNIT RATHER THAN A CHOICE. §NO STUBS' rule is that the
 * landing is the smallest diff that makes the guard's TRUE branch SURVIVABLE, and the guard here is
 * `"locks" in navigator` or `navigator.locks &&`: the instant this member exists, every one of those branches
 * is taken. Three of the sites read pass `{signal}` or `{mode}`, so the THREE-argument overload is part of the
 * unit — §3.6's steps 3-4 remove the two-argument entry by ARGUMENT COUNT, so a build with only the shorter
 * entry converts the page's OPTIONS OBJECT as the callback and answers a TypeError where a browser grants a
 * lock, which is worse than the absence it replaced. `query()` is in the unit for the opposite reason: no site
 * read calls it, and §4.5 is nine steps over state this component already holds, so leaving it out would be an
 * absence nobody could justify rather than one somebody chose.
 *
 * THE STATE IS JS VALUES AND NOT A C LIST, which §PLATFORM-DATA-A-FLOW-QUEUES-IS-A-JS-VALUE decides rather
 * than taste. A lock manager's held set and its request queues are per-flow (two arms of a fork legitimately
 * hold different locks), they must PARK to the cold tier with the flow and RESUME, and every item in them is a
 * page value — a callback, a capability's resolving functions, an AbortSignal. A JS object gives all of that
 * for free: its mutations are ordinary property writes the COW delta already captures. A malloc'd queue
 * captured as head/tail POINTERS would revert the pointers on a context switch and leave the nodes reachable
 * from nothing, which is the leak that header names by construction.
 *
 * WHERE THE MANAGER LIVES IS §2.2 "Lock Managers"' OWN ANSWER: "Each storage bucket includes one lock manager
 * through an associated storage bottle for the Web Locks API", reached by "obtaining a local storage bottle map
 * given environment and" the identifier this file passes. So it hangs off that bottle's backing map and is
 * SHARED by every environment with the same storage key, which is what that section's Note requires — "Pages
 * and workers (agents) sharing a storage bucket opened in the same user agent share a lock manager even if
 * they are in unrelated browsing contexts."
 * §2.2's FAILURE IS A REAL ANSWER AND NOT AN ERROR: obtain-a-storage-key answers failure for an OPAQUE ORIGIN,
 * and §3.2.1 and §3.2.2 both turn that into a "SecurityError" DOMException. It is asked of the MODELLED
 * document's origin and never of the frame this engine runs in — SECURITY.md makes that frame's origin opaque
 * by design, so a component reading it would answer SecurityError for every page there has ever been.
 *
 * WHAT THIS COMPONENT OWNS AND WHAT IT DOES NOT. It owns §3.2's LockManager, §3.3's Lock, the manager state and
 * the five §4 algorithms. It does NOT own the member on Navigator: §3.1 declares `NavigatorLocks` as an
 * `interface mixin`, and Web IDL §3.7.3 "Interface prototype object" gives a mixin no prototype of its own, so
 * `Navigator includes NavigatorLocks;` makes Navigator's prototype the object that member goes on — which is
 * core/frame/navigator.c's, and is why the accessor is installed from there and the VALUE comes from here. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOCKS_LOCK_MANAGER_H
#define ENGINE_HOST_BROWSER_CORE_LOCKS_LOCK_MANAGER_H

#include "quickjs.h"

/* Declared from navigator_init and released from navigator_free, for core/permissions/permissions.h's reason:
   §3.1's mixin is included into Navigator, so a host that has a Navigator has `navigator.locks`, and a
   hand-copied row in each host's list is the list core/realm.h exists to abolish. */
void lock_manager_init(JSContext *ctx);
void lock_manager_free(void);

/* §3.1: "The locks getter's steps are to return this's relevant settings object's LockManager object." The
 * argument is the RECEIVER'S realm and never `ctx`, exactly as navigator.c's `permissions` and
 * `userActivation` getters take it: the object is kept per realm, so which one comes back depends on whose
 * realm is asking. OWNED.
 *
 * `[SameObject]` IS WHERE THE OBJECT IS KEPT AND NOT A CACHE IN THE GETTER — the realm mints one at its
 * install, so `navigator.locks === navigator.locks` holds by construction and no flow's first read can become
 * every sibling's baseline. */
JSValue lock_manager_object(JSContext *ctx);

#endif
