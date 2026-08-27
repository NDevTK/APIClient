/* HTML §7.1.2 ORIGIN-KEYED AGENT CLUSTERS, and §8.1.2.2's allocation rule that decides one.
 *
 * ONE FACT, ONE PLACE. Four algorithms ask about this agent's cluster — `window.originAgentCluster` (§7.1.2),
 * `window.crossOriginIsolated` (§8.1.7.1), §7.1.1.2's `document.domain` setter step 5, which RETURNS WITHOUT
 * SETTING when the cluster is origin-keyed, and HR-TIME §4's coarsen time, whose CLOCK RESOLUTION is 5
 * microseconds instead of 100 for an environment with the cross-origin isolated capability. A constant written
 * into each of them is the defect CLAUDE.md names: one fact answered from four places, drifting the day one of
 * them learns something the others do not. So the cluster is a component and the four read it.
 *
 * ONE FACT IS NOT ONE BOOLEAN. §7.3.2.3's cross-origin isolation mode has THREE values and its readers split on
 * different cuts of them: origin-keying asks "not `none`" (so `logical` counts) and the capability asks "is
 * `concrete`" (so `logical` does not). This file held one bool for both, which was invisible only because the
 * mode is `none` in every build so far. The mode is the fact; each reader takes its own question to it.
 *
 * WHAT THIS AGENT'S ANSWER ACTUALLY IS, AND WHY IT IS COMPUTED RATHER THAN ASSUMED. §8.1.2.2's allocation is
 * "let site be the result of obtaining a site with origin; let key be site; if group's cross-origin isolation
 * mode is not `none`, then set key to origin; otherwise, if group's historical agent cluster key map[origin]
 * exists ...; otherwise: if requestsOAC is true, then set key to origin", and `is origin-keyed` is true only
 * when the key ended up being an ORIGIN. The three inputs are the browsing context group's cross-origin
 * isolation mode, the historical map, and the `Origin-Agent-Cluster` response header.
 *
 * THE HEADER NOW ARRIVES, so this is a real allocation and not a constant. A navigation response's HEADER LIST
 * crosses the ABI (core/frame/navigation_params.c reads §7.5.1's `requestsOAC` out of it, boolean-true only and
 * cleared for a non-secure context), and every host reaches the allocation below through the one call it goes
 * through — so `originAgentCluster` and §7.1.1.2's `document.domain` setter step 5 answer for the header the
 * server actually sent. An OPAQUE origin is unconditionally origin-keyed and always was: §7.1.1.1's
 * obtain-a-site returns the origin itself for one, so the key is an origin with no header involved.
 *
 * THE MODE IS THE GROUP'S AND THIS FILE ASKS FOR IT. §7.3.2.3 puts the cross-origin isolation mode on the
 * BROWSING CONTEXT GROUP and §7.1.3.2 is the one algorithm that ever assigns it, from §7.1.3's opener policy
 * obtained off the navigation response — so the fact lives in core/frame/browsing_context_group.h, created
 * once per agent beside this cluster and read here. A page served `Cross-Origin-Opener-Policy: same-origin`
 * beside a `Cross-Origin-Embedder-Policy` compatible with cross-origin isolation reaches `concrete`, and the
 * SECOND conjunct — HTML §4.8.5's allowed-to-use over Permissions Policy §9.10 — is asked of the Document
 * (core/permissions_policy/permissions_policy.h). The two do not agree on an ordinary page and that is the
 * point of asking both: the mode is the whole GROUP's, so a cross-origin `<iframe>` of an isolated page has
 * `concrete` and is still not allowed a 'self' feature.
 * §7.1.5's SANDBOXING FLAG SET IS NOT THE MISSING PIECE, and it is named because the two used to be described
 * as one absence. That set is carried now (core/frame/sandboxing.h) and it is a field of the DOCUMENT rather
 * than of the policy container, which is where §7.5.1's creation table puts it — and it says nothing about
 * agent-cluster keying.
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
#include "core/url/origin.h"

/* §8.1.2.2's OBTAIN A SIMILAR-ORIGIN WINDOW AGENT, for the one agent cluster this instance is — SECURITY.md
 * keys an instance on `(browsing context group, origin)`, which IS an agent cluster key, so this runs exactly
 * once per agent and every realm of the instance is in the cluster it allocates.
 *
 * `requests_oac` is §7.5.1's `requestsOAC`: the response's `Origin-Agent-Cluster` header parsed as a
 * structured-field ITEM whose bare item is the boolean TRUE, and false for a non-secure context. It is stated
 * by the caller rather than read here, because a response is read in exactly one place (§7.4.6's navigation
 * params) and an agent is not a thing that holds one. */
void agent_cluster_obtain_window_agent(const Origin *origin, bool requests_oac);

/* §7.1.2's IS ORIGIN-KEYED, which `originAgentCluster` returns and which §7.1.1.2's setter step 5 stops on.
   Asked of a cluster that was never allocated, this CRASHES rather than answering false. */
bool agent_cluster_is_origin_keyed(void);

/* HTML §7.2.2.6 "Script settings for Window objects"' environment settings object field CROSS-ORIGIN ISOLATED
 * CAPABILITY, for the environment `ctx` IS: "Return true if both of the following hold, and false otherwise:
 * realm's agent cluster's cross-origin-isolation mode is `concrete`, and window's associated Document is
 * allowed to use the "cross-origin-isolated" feature". §8.1.7.1's
 * `crossOriginIsolated` getter returns exactly this field, and HR-TIME §4's coarsen time decides its resolution
 * from it (core/timing/hr_time.h).
 *
 * IT TAKES THE REALM because the capability is a fact about an ENVIRONMENT and not about the cluster alone: the
 * mode half is the browsing context group's, and the second half is the DOCUMENT's permissions policy — which
 * is per-document state and DIFFERS across the realms of one agent, so the realm is what decides the answer
 * rather than decorating it. A cluster asked without a realm would be the module-static answer CLAUDE.md's
 * per-realm rule names: one document's capability reported as every document's.
 *
 * IT HAS NO WRITER, and that is what lets HR-TIME §4's grid be a pure function of the realm: every moment
 * core/timing/hr_time.c coarsens — including the realm's own time origin, which it coarsens at the READ
 * precisely because this question cannot be asked before the Document exists — lands on one resolution for the
 * life of the realm, so no two timestamps of one environment can disagree about which grid they are on. */
bool agent_cluster_cross_origin_isolated(JSContext *ctx);

/* §7.1.2's `originAgentCluster` and §8.1.7.1's `crossOriginIsolated` — the two Window members stated over this
   cluster, installed by the component that owns it rather than by the Window they hang off, for the reason
   core/frame/navigation.c's row gives. */
void agent_cluster_install(JSContext *ctx, JSValueConst global);

/* The cluster goes with the AGENT. Nothing here is allocated — what is given up is the STATEMENT that an agent
   was obtained, so a process that brings a second agent up (a native host re-executing itself as a peer) runs
   §8.1.2.2 again rather than reading the previous one's key. */
void agent_cluster_release(void);

#endif
