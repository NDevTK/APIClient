/* HTML §7.1.2 ORIGIN-KEYED AGENT CLUSTERS, and §8.1.2.2's allocation rule that decides one.
 *
 * ONE FACT, ONE PLACE. Three members and one algorithm ask the same question about this agent's cluster —
 * `window.originAgentCluster` (§7.1.2), `window.crossOriginIsolated` (§8.1.7.1), and §7.1.1.2's `document.domain`
 * setter step 5, which RETURNS WITHOUT SETTING when the cluster is origin-keyed. A constant written into each
 * of them is the defect CLAUDE.md names: one fact answered from three places, drifting the day one of them
 * learns something the others do not. So the cluster is a component and the three read it.
 *
 * WHAT THIS AGENT'S ANSWER ACTUALLY IS, AND WHY IT IS COMPUTED RATHER THAN ASSUMED. §8.1.2.2's allocation is
 * "let site be the result of obtaining a site with origin; let key be site; if group's cross-origin isolation
 * mode is not `none`, then set key to origin; otherwise, if group's historical agent cluster key map[origin]
 * exists ...; otherwise: if requestsOAC is true, then set key to origin", and `is origin-keyed` is true only
 * when the key ended up being an ORIGIN. The three inputs are the browsing context group's cross-origin
 * isolation mode, the historical map, and the `Origin-Agent-Cluster` response header — and this engine's §7.2.6
 * policy container carries a CSP and nothing else, so the header has never reached a Document it built and the
 * mode is "none". The key is therefore the SITE and the cluster is NOT origin-keyed. Each of those is evaluated
 * at the step that asks it, the same shape core/frame/navigable.c evaluates the sandboxed-origin flag with, so
 * the day a policy container carries the header this is the one place it is read.
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

/* §7.1.2's `originAgentCluster` and §8.1.7.1's `crossOriginIsolated` — the two Window members stated over this
   cluster, installed by the component that owns it rather than by the Window they hang off, for the reason
   core/frame/navigation.c's row gives. */
void agent_cluster_install(JSContext *ctx, JSValueConst global);

#endif
