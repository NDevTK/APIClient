/* BROWSING CONTEXT GROUPS — HTML §7.3.2.3 "Groupings of browsing contexts".
 *
 * WHAT A GROUP IS, IN THE STANDARD'S OWN WORDS. "A browsing context group holds a browsing context set (a set
 * of top-level browsing contexts)"; "a browsing context group has an associated agent cluster map"; "a
 * browsing context group has an associated historical agent cluster key map"; and — the item this component
 * exists for — "a browsing context group has a CROSS-ORIGIN ISOLATION MODE, which is a cross-origin isolation
 * mode. It is INITIALLY `none`."
 *
 * THIS INSTANCE IS IN EXACTLY ONE GROUP, WHICH IS NOT A SIMPLIFICATION BUT SECURITY.md's OWN KEY. A WASM
 * instance is an ORIGIN-KEYED AGENT CLUSTER — `(browsing context group, origin)` — so the group is the first
 * half of the key that decided there is one instance here at all, and a module static is the right shape for
 * the same reason core/frame/agent_cluster.c's is: every realm of this instance is in this group. A navigable
 * LEAVING the group is therefore not a second group held here — it is a SECOND INSTANCE, and performing that
 * departure is the other half of this file (browsing_context_group_swap below).
 *
 * THE MODE HAS ONE WRITER IN THE WHOLE STANDARD AND IT IS §7.1.3.2. Its obtain-a-browsing-context-to-use-for-a-
 * navigation-response, on the swap arm, says: "Let navigationCOOP be navigationParams's cross-origin opener
 * policy. If navigationCOOP's value is `same-origin-plus-COEP`, then set newBrowsingContext's group's
 * cross-origin isolation mode to either `logical` or `concrete`. The choice of which is implementation-
 * defined." Nothing else in HTML assigns it. So a group is CREATED with the navigation COOP that produced it,
 * and there is no setter — a mode that could be raised later would be a fact about a group answered from a
 * second place, which is the defect CLAUDE.md's per-realm rule names one level up.
 *
 * WHY THE ROOT DOCUMENT'S OWN COOP DECIDES THIS INSTANCE'S MODE, AND WHY THAT IS A DERIVATION RATHER THAN A
 * GUESS. The engine is handed a document the real browser has already navigated to, so the swap (or its
 * absence) happened before `qjs_init`. Take a top-level Document whose opener policy is `same-origin-plus-COEP`
 * and ask which group it is in. Either the navigation to it SWAPPED — and §7.1.3.2's step above set the new
 * group's mode — or it did not, which by §7.1.3.2's check happens only when matching returned true, i.e. the
 * previous top-level Document had the SAME value and was SAME ORIGIN, so by induction that group's mode was
 * already set. Either way the mode is not `none`. §7.3.2.3's own note states the invariant from the other end:
 * `logical` and `concrete` "are both used for browsing context groups where every top-level Document has
 * `Cross-Origin-Opener-Policy: same-origin`, and every Document has a `Cross-Origin-Embedder-Policy` header
 * whose value is compatible with cross-origin isolation" — which is precisely what §7.1.3's
 * `same-origin-plus-COEP` value means (core/frame/opener_policy.h).
 *
 * THE `concrete` ARM IS UNREACHABLE IN THIS BUILD AND THAT IS CHECKABLE, NOT A HEDGE. `same-origin-plus-COEP`
 * is produced only when the SAME response carries a `Cross-Origin-Embedder-Policy` compatible with cross-origin
 * isolation (§7.1.3 step 4.1, core/frame/opener_policy.c), and core/frame/navigation_params.c already CRASHES
 * on exactly that predicate over exactly that header list — §7.1.7's policy container has nowhere to carry an
 * embedder policy yet. So every response this build can process reaches here with `none`, by construction and
 * not by luck, and the day the container travels as a container that crash goes and this arm becomes live.
 * WHAT MUST BE SETTLED BEFORE IT DOES: a CHILD instance's group is its PARENT's — a child navigable never
 * leaves its creator's browsing context group — so a cross-origin child provisioned as its own WASM instance
 * must be created with the GROUP's mode and not with the mode its own response would imply (§7.4.5 obtains an
 * opener policy only for a top-level traversable, and a child's is `unsafe-none` whatever it sent). The
 * trusted zone already routes on group identity (`eng.groupId` in extension/bridge.js), so the mode is a fact
 * it can state; nothing states it today, which is why this note is here and not a comment about a value.
 *
 * `concrete` IS THE CHOICE, AND THE STANDARD SAYS IT IS THE IMPLEMENTATION'S. §7.3.2.3: "On some platforms, it
 * is difficult to provide the security properties required to grant safe access to the APIs gated by the
 * cross-origin isolated capability. As a result, only `concrete` can grant access [to] that capability.
 * `logical` is used on platform[s] not supporting this capability." This engine models real Chrome, which
 * grants it — a page served `COOP: same-origin` beside a `COEP: require-corp` reads `crossOriginIsolated ===
 * true` there — so `logical` would be modelling a browser this is not, and CLAUDE.md's §Headless rule forbids
 * shrugging a modelable value down. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_BROWSING_CONTEXT_GROUP_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_BROWSING_CONTEXT_GROUP_H

#include "quickjs.h"
#include "core/frame/opener_policy.h"
#include "core/frame/sandboxing.h"
#include "core/url/origin.h"

/* §7.3.2.3's CROSS-ORIGIN ISOLATION MODE — "one of three possible values". It is three-valued and not a
   boolean because its readers split on different cuts of it: §8.1.2.2's agent cluster key allocation asks
   whether it is NOT `none` (so `logical` counts), while §7.2.2's cross-origin isolated capability asks whether
   it IS `concrete` (so `logical` does not). core/frame/agent_cluster.h states that pair. */
typedef enum {
    BROWSING_CONTEXT_GROUP_ISOLATION_NONE = 0,   /* §7.3.2.3's initial value */
    BROWSING_CONTEXT_GROUP_ISOLATION_LOGICAL,
    BROWSING_CONTEXT_GROUP_ISOLATION_CONCRETE,
} BrowsingContextGroupIsolationMode;

/* §7.3.2.3's CREATE A NEW BROWSING CONTEXT GROUP, followed immediately by §7.1.3.2's one mode-setting step —
 * the two are one operation because §7.1.3.2 is the only algorithm that ever creates a group whose mode is not
 * `none`, and it sets it on the group it has just made.
 *
 * `navigation_coop` is §7.1.3.2's `navigationCOOP`'s VALUE: the opener policy of the response whose navigation
 * produced this group. For the group this instance starts in, that is the ROOT document's response COOP
 * (core/frame/navigation_params.h), which is the derivation the header comment above states. Every value other
 * than `same-origin-plus-COEP` leaves §7.3.2.3's initial `none`, which is the answer for every page that sends
 * no header AND for every page that sends `same-origin` without a compatible embedder policy.
 *
 * ONCE PER AGENT. A second call is a second group in one instance, which the `(browsing context group, origin)`
 * key says cannot exist — a navigable that leaves this group leaves this instance. */
void browsing_context_group_create(OpenerPolicyValue navigation_coop);

/* §7.3.2.3's cross-origin isolation mode of THIS instance's group. Asked before the group exists this CRASHES
   rather than answering `none`: a group that was never created is a question about an instance that was never
   keyed, and answering it would be the plausible datum CLAUDE.md's rule about defaults is written against. */
BrowsingContextGroupIsolationMode browsing_context_group_isolation_mode(void);

/* The group goes with the AGENT — platform_agent_free. Nothing here is allocated; what is released is the
   STATEMENT that a group exists, so a second instance in one process (the native host re-executing itself as a
   peer) creates its own rather than reading the previous one's mode. */
void browsing_context_group_release(void);

/* §7.1.3.2's BROWSING CONTEXT GROUP SWAP — steps 10 to 15 of "obtain a browsing context to use for a navigation
 * response", performed once its predicate (opener_policy_switch_required, one file over) has said `swapGroup`
 * is true. `proxy` is step 1's `browsingContext`, the navigable being navigated; `url` and `origin` are the
 * destination the navigation carries; `final_sandbox_flags` is step 13's clone of navigationParams's final
 * sandboxing flag set.
 *
 * WHAT THE STANDARD DOES, VERBATIM, AND IT IS NOT A SEVERING. Step 10: "Let newBrowsingContext be the first
 * return value of creating a new top-level browsing context and document", whose note reads "In this case we
 * are going to perform a browsing context group swap. browsingContext will not be used by the new Document that
 * we are about to create. If it is not used by other Documents either (such as ones in the back/forward cache),
 * then the user agent might destroy it at this point." §7.3.2.1's create-a-new-top-level-browsing-context-and-
 * document is "Let group and document be the result of creating a new browsing context group and document",
 * and §7.3.2.3's create-a-new-browsing-context-group-and-document creates its browsing context "with null,
 * null, and group" — a NULL CREATOR, which is what makes every one of §7.3.2.1's creator-dependent steps (the
 * referrer, the policy container clone, the inherited opener policy) not run, and what leaves the new browsing
 * context with no opener browsing context at all: only §7.3.2.1's create-a-new-AUXILIARY-browsing-context-and-
 * document sets one, and this is not that algorithm.
 *
 * SO THE TWO OBSERVABLES SIT ON OPPOSITE SIDES OF THE BOUNDARY AND BOTH FALL OUT OF THE SAME SENTENCE.
 *   `window.opener` IS NULL IN THE NEW DOCUMENT — nothing gave it one (above).
 *   THE OPENER'S OWN HANDLE ANSWERS `closed === true`. §7.2.2.1's getter is "return true if this's browsing
 *   context is null or its is closing is true", and browsing-the-web's MAKE ACTIVE is "Set document's browsing
 *   context's WindowProxy's [[Window]] internal slot value to window" — through the DOCUMENT's browsing
 *   context. On an ordinary navigation that is the same browsing context the opener's handle names, so the
 *   handle follows the navigable to the new Document and `closed` stays false. On a swap the new Document's
 *   browsing context is `newBrowsingContext`, which has its OWN WindowProxy, so the handle is never updated: it
 *   keeps the Window it had, and that Window's Document's browsing context is the one this swap discarded.
 * SEVERING THE OPENER ALONE WOULD GET THE SECOND ONE WRONG, in the direction that matters here — the opener
 * would keep reading `closed === false` and keep reaching the new Document, so the engine would model a page
 * real Chrome has already cut off and §@S would emit a breakout the browser forbids.
 *
 * THIS INSTANCE HOSTS NEITHER HALF OF THE RESULT, WHICH IS WHY THIS IS AN EMISSION. SECURITY.md keys a WASM
 * instance on `(browsing context group, origin)`, so a second group here is a second agent cluster in one
 * JSRuntime — the opener and the swapped Document sharing a heap, which is exactly the boundary COOP exists to
 * draw, and browsing_context_group_create's own DCHECK refuses it. §7.5.1's step 1 says the same thing from the
 * other end: on a swap "the created Window, Document, and agent will not end up being used; because the created
 * Document's origin is opaque, we will end up creating a new agent and Window later in this algorithm" — a NEW
 * AGENT, which in this engine is a new instance. So the new top-level browsing context is announced to the host
 * as `navigable.swap<TAB><new document name><TAB><url><TAB><origin>` and provisioned there, the way
 * core/frame/navigable.c already announces a cross-origin child.
 *
 * AND THE HOST FETCHES THE RESPONSE AGAIN, WHICH IS THE PRICE OF THE TRUST BOUNDARY AND NOT AN OVERSIGHT. This
 * instance has the response in hand — it had to, or it could not have obtained the opener policy that decided
 * the swap — and it may not hand the bytes over: SECURITY.md makes the engine UNTRUSTED and lets it mint only
 * NAMES, so an engine that could hand a zone bytes plus a principal could name any origin's document into
 * existence. The address crosses and the trusted zone loads it, exactly as it does for `navigable.create`. That
 * is a SECOND request for one navigation where a real browser commits the response it already has, and the
 * fidelity gap it leaves has a name and a shape: a server that answers the second request differently gives the
 * new instance a document the first response's policy did not describe. THE NEXT SUBPROBLEM IS THE ZONE
 * REMEMBERING ITS OWN ANSWER — safe-fetch's reply for the `document.fetch` this navigation already made, keyed
 * by (instance, address) and handed to the instance this record provisions — which keeps every byte on the
 * trusted side and makes one navigation one request.
 *
 * §7.1.3.2 STEP 12'S MODE DOES NOT CROSS, AND THAT IS A DERIVATION RATHER THAN AN OMISSION. "If navigationCOOP's
 * value is `same-origin-plus-COEP`, then set newBrowsingContext's group's cross-origin isolation mode…" — the
 * navigation COOP is the one obtained from the response the new instance is about to be created from, so that
 * instance's own platform_agent_init reaches browsing_context_group_create with the identical value and
 * computes the identical mode. Carrying it would be a second answer to a question the peer already answers,
 * which is the defect the header comment above names one level up. (A CHILD instance is the case where the
 * derivation does NOT hold — a child navigable never leaves its creator's group, so it must be created with the
 * GROUP's mode rather than its own response's — and that is stated where it belongs, in the header comment
 * above, and is not this operation.)
 *
 * ONE EMISSION PER FLOW, and the ceiling that implies is named rather than capped. Forced execution runs a
 * `window.open()` once per flow, so a page whose popup swaps groups announces one instance per flow — the same
 * shape core/frame/navigable.c's cross-origin child already has, one level more expensive. The answer is
 * reclamation at the host's Level-1 admission, never a seen-set here.
 *
 * `provenance` IS THE NAVIGATION'S, NOT THIS RECORD'S — CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's
 * `observed`/`derived`/`forced` (solver/engine.h), taken from the load job that reached this arm. The zone
 * fetches the address a SECOND time, as the paragraph above says, so it makes the same firing decision the
 * create arm makes and needs the same field to make it from; a record that named an address and said nothing
 * about who named it left the zone with §Attacker-sources' unestablished provenance, which that section makes
 * a crash at the decision rather than a load. It is a PARAMETER and never a read of the running flow, for the
 * reason the address beside it is one: this is reached from inside a queued task, by whichever flow adopted
 * it, long after the operation that decided what this navigation is evidence of. */
void browsing_context_group_swap(JSContext *ctx, JSValueConst proxy, const char *url, const Origin *origin,
                                 SandboxFlags final_sandbox_flags, const char *provenance);

#endif
