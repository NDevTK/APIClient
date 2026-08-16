/* HTML §7.1.2 ORIGIN-KEYED AGENT CLUSTERS, and §8.1.2.2's allocation rule that decides one.
 *
 * ONE FACT, ONE PLACE. Four algorithms ask about this agent's cluster — `window.originAgentCluster` (§7.1.2),
 * `window.crossOriginIsolated` (§8.1.7.1), §7.1.1.2's `document.domain` setter step 5, which RETURNS WITHOUT
 * SETTING when the cluster is origin-keyed, and HR-TIME §4's coarsen time, whose CLOCK RESOLUTION is 5
 * microseconds instead of 100 for an environment with the cross-origin isolated capability. A constant written
 * into each of them is the defect CLAUDE.md names: one fact answered from four places, drifting the day one of
 * them learns something the others do not. So the cluster is a component and the four read it.
 *
 * ONE FACT IS NOT ONE BOOLEAN. §7.1.4's cross-origin isolation mode has THREE values and its readers split on
 * different cuts of them: origin-keying asks "not `none`" (so `logical` counts) and the capability asks "is
 * `concrete`" (so `logical` does not). This file held one bool for both, which was invisible only because the
 * mode is `none` in every build so far. The mode is the fact; each reader takes its own question to it.
 *
 * WHAT THIS AGENT'S ANSWER ACTUALLY IS, AND WHY IT IS COMPUTED RATHER THAN ASSUMED. §8.1.2.2's allocation is
 * "let site be the result of obtaining a site with origin; let key be site; if group's cross-origin isolation
 * mode is not `none`, then set key to origin; otherwise, if group's historical agent cluster key map[origin]
 * exists ...; otherwise: if requestsOAC is true, then set key to origin", and `is origin-keyed` is true only
 * when the key ended up being an ORIGIN. The three inputs are the browsing context group's cross-origin
 * isolation mode, the historical map, and the `Origin-Agent-Cluster` response header — and the only header this
 * engine hands a Document at its creation is `Content-Security-Policy`, so that one has never reached a
 * Document it built and the mode is "none". The key is therefore the SITE and the cluster is NOT origin-keyed.
 * Each of those is evaluated at the step that asks it, so the day a response's headers reach a Document this
 * is the one place they are read.
 * §7.1.5's SANDBOXING FLAG SET IS NOT THE MISSING PIECE, and it is named because the two used to be described
 * as one absence. That set is carried now (core/frame/sandboxing.h) and it is a field of the DOCUMENT rather
 * than of the policy container, which is where §7.5.1's creation table puts it — and it says nothing about
 * agent-cluster keying. What is still absent is the `Origin-Agent-Cluster`, `Cross-Origin-Opener-Policy` and
 * `Cross-Origin-Embedder-Policy` RESPONSE HEADERS: the only header a Document is created with in this engine
 * is its `Content-Security-Policy` (core/dom/document.h's install), so §7.1.7's EMBEDDER POLICY has no writer
 * and neither does the OPENER POLICY §7.5.1 gives a Document beside its container.
 *
 * ONE INSTANCE PER ORIGIN IS NOT ORIGIN-KEYING, and conflating the two would give the wrong answer here.
 * SECURITY.md partitions this engine by `(browsing context group, origin)` — a heap boundary — and an agent
 * cluster key is a different thing: Blink partitions cross-origin frames into separate PROCESSES and still
 * reports `originAgentCluster === false`, because site isolation is an implementation's process model and
 * origin-keying is an observable the page opts into. What the instance boundary really decides is that a
 * relaxed domain cannot travel: §7.1.1's serialization has no domain component, so an origin that crossed a
 * boundary as bytes always arrives with a null domain, and §7.1.1's same origin-domain step 2.1 needs both
 * sides non-null. That is asserted where a domain is written (core/url/origin.c), not argued here. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_AGENT_CLUSTER_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_AGENT_CLUSTER_H
#include <stdbool.h>

#include "quickjs.h"

/* §7.1.2's IS ORIGIN-KEYED, which `originAgentCluster` returns and which §7.1.1.2's setter step 5 stops on. */
bool agent_cluster_is_origin_keyed(void);

/* HTML §7.2.2's environment settings object field CROSS-ORIGIN ISOLATED CAPABILITY, for the environment `ctx`
 * IS: "true if both of the following hold — realm's agent cluster's cross-origin-isolation mode is `concrete`,
 * and window's associated Document is allowed to use the `cross-origin-isolated` feature". §8.1.7.1's
 * `crossOriginIsolated` getter returns exactly this field, and HR-TIME §4's coarsen time decides its resolution
 * from it (core/timing/hr_time.h).
 *
 * IT TAKES THE REALM because the capability is a fact about an ENVIRONMENT and not about the cluster alone: the
 * mode half is the browsing context group's, and the second half is the DOCUMENT's permissions policy. Asking
 * the cluster without a realm would be the module-static answer CLAUDE.md's per-realm rule names — one answer
 * for every document — the moment either half stops being uniform across this agent.
 *
 * IT HAS NO WRITER, which is what lets hr_time.c stamp a time origin on the resolution grid this decides and
 * trust that every later moment in that realm is coarsened on the same grid. */
bool agent_cluster_cross_origin_isolated(JSContext *ctx);

/* §7.1.2's `originAgentCluster` and §8.1.7.1's `crossOriginIsolated` — the two Window members stated over this
   cluster, installed by the component that owns it rather than by the Window they hang off, for the reason
   core/frame/navigation.c's row gives. */
void agent_cluster_install(JSContext *ctx, JSValueConst global);

#endif
