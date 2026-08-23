/* HTML §7.1.2's agent cluster, as one fact its four readers each take their own question to. See
   agent_cluster.h. */
#include "check.h"
#include "core/frame/agent_cluster.h"
#include "core/frame/browsing_context_group.h"
#include "core/idl_args.h"
#include "core/url/origin.h"

/* §7.3.2.3's CROSS-ORIGIN ISOLATION MODE for this browsing context group, and it is THREE-VALUED because its
   two readers read it differently. §7.3.2.3 gives `none`, `logical` and `concrete`; §8.1.2.2's key allocation asks
   whether it is NOT `none`, while §7.2.2's cross-origin isolated capability asks whether it IS `concrete`. A
   single boolean answered both, which is one fact collapsed into the WRONG shape rather than into one place:
   under `logical` a Document is origin-keyed and `crossOriginIsolated` is FALSE, and a boolean cannot say that.

   THE MODE IS NOT THIS COMPONENT'S FACT AND IS NO LONGER SPELLED HERE. §7.3.2.3 puts the cross-origin
   isolation mode on the BROWSING CONTEXT GROUP, and §7.1.3.2's obtain-a-browsing-context-to-use-for-a-
   navigation-response is the one step in the standard that ever assigns it ("if navigationCOOP's value is
   `same-origin-plus-COEP`, then set newBrowsingContext's group's cross-origin isolation mode to either
   `logical` or `concrete`"). Both live in core/frame/browsing_context_group.h now, which is where this file
   asks — a second enum here would be one fact with two spellings, drifting the day one of them learns a value
   the other does not. */

/* §8.1.2.2's AGENT CLUSTER, for the ONE cluster this instance is. SECURITY.md keys a WASM instance on
   `(browsing context group, origin)`, which is exactly an agent cluster key, so there is one of these per
   agent and a module static is the right shape rather than the per-realm answer CLAUDE.md's §3.7 rule asks
   for: `originAgentCluster` returns "the SURROUNDING AGENT's agent cluster's is origin-keyed", and every realm
   of this instance is in the same agent. */
static bool g_agent_obtained;
static bool g_is_origin_keyed;

void agent_cluster_obtain_window_agent(const Origin *origin, bool requests_oac)
{
    /* §8.1.2.2's "obtain a similar-origin window agent, given an origin, a browsing context group and a
       boolean requestsOAC", to the depth that decides the one observable it produces:

         1. Let site be the result of obtaining a site with origin.
         2. Let key be site.
         3. If group's cross-origin isolation mode is not "none", then set key to origin.
         4. Otherwise, if group's historical agent cluster key map[origin] exists, then set key to it.
         5. Otherwise: if requestsOAC is true, then set key to origin. Set the map entry to key.
         6. ... if key is an ORIGIN: assert key is origin; set agentCluster's is origin-keyed to true.

       WHAT IS COMPUTED IS "IS THE KEY AN ORIGIN", not the key itself, and that is the whole of step 6's test.
       A key is an origin in exactly two ways: step 3 or step 5 made it `origin`, or step 1's SITE already was
       one — §7.1.1.1's obtain-a-site returns the ORIGIN ITSELF for an opaque origin and a (scheme, host)
       tuple otherwise, which is the standard's own reason §7.1.2 says "Documents with an opaque origin can be
       considered unconditionally origin-keyed". The registrable-domain half of a site is never needed here:
       §8.1.2.2 keys its maps by ORIGIN, and this instance holds exactly one. */
    DCHECK(origin != NULL, "an agent was obtained for no origin — §8.1.2.2 takes one, and an agent cluster key "
                           "is a site or a tuple origin, both of which are derived from it");
    /* ONCE PER AGENT, because an instance IS one agent cluster. A second document of this cluster arriving —
       the `qjs_join` SECURITY.md names, a same-origin frame this engine did not model, a navigation replacing
       the root — must NOT recompute this: §8.1.2.2's historical agent cluster key map exists precisely so that
       a later same-origin Document in the same group gets the FIRST one's key even when it sends a different
       `Origin-Agent-Cluster` header, which is what §7.1.2 means by "the getter can return false, even if the
       header is set". The answer below IS this cluster's map entry; whoever builds that join reads it. */
    DCHECK(!g_agent_obtained,
           "a similar-origin window agent was obtained twice in one instance — one WASM instance is one "
           "`(browsing context group, origin)` agent cluster, so this runs once, and a second document of the "
           "cluster inherits the recorded key through §8.1.2.2's historical agent cluster key map rather than "
           "re-running the allocation with its own header");

    /* Step 1-2, and step 6's test over them: an OPAQUE origin's site is that origin. */
    g_is_origin_keyed = origin_is_opaque(origin);
    if (browsing_context_group_isolation_mode() != BROWSING_CONTEXT_GROUP_ISOLATION_NONE)
        g_is_origin_keyed = true;            /* step 3 */
    else if (requests_oac)
        g_is_origin_keyed = true;            /* step 5 */
    g_agent_obtained = true;
}

bool agent_cluster_is_origin_keyed(void)
{
    /* §7.1.2: "The originAgentCluster getter steps are to return the surrounding agent's agent cluster's is
       origin-keyed." A read before the agent exists is not a `false` to default to — it is a question asked of
       a cluster that was never allocated, and answering it would be the plausible datum CLAUDE.md's rule about
       defaults is written against. */
    DCHECK(g_agent_obtained,
           "this agent's cluster was asked whether it is origin-keyed before §8.1.2.2's obtain-a-similar-"
           "origin-window-agent allocated one — every host reaches that through platform_agent_init, so an "
           "agent without it is one that was built past the one list every host goes through");
    return g_is_origin_keyed;
}

bool agent_cluster_cross_origin_isolated(JSContext *ctx)
{
    /* HTML §7.2.2's set up a window environment settings object defines this environment field: "The
       cross-origin isolated capability — return true if both of the following hold, and false otherwise:
       realm's agent cluster's cross-origin-isolation mode is `concrete`, and window's associated Document is
       allowed to use the `cross-origin-isolated` feature." FIRST CONJUNCT, and note it is `concrete` alone —
       §7.3.2.3's `logical` mode is the one where a page IS origin-keyed and this capability is still false. */
    (void)ctx;   /* the SECOND conjunct is the Document's, and reads this environment — see the DFAIL below */
    if (browsing_context_group_isolation_mode() != BROWSING_CONTEXT_GROUP_ISOLATION_CONCRETE)
        return false;
    /* SECOND CONJUNCT, AND IT IS REACHABLE NOW — a response carrying `Cross-Origin-Opener-Policy: same-origin`
       beside a `Cross-Origin-Embedder-Policy` compatible with cross-origin isolation gives this instance's
       group the `concrete` mode (core/frame/browsing_context_group.h), which is the line above. It is a crash
       rather than a `true` because assuming it would hand every cross-origin-isolated environment a capability
       the Document's permissions policy may deny — and the first thing that reads the answer is HR-TIME §4's
       clock resolution, which a page measures directly. */
    DFAIL("HTML §7.2.2's CROSS-ORIGIN ISOLATED CAPABILITY has its first conjunct — this agent cluster's "
          "cross-origin isolation mode is now `concrete` — and its second conjunct is the PERMISSIONS POLICY "
          "question: is the Document allowed to use the `cross-origin-isolated` feature. This build has no "
          "permissions-policy component to ask, so build one and read it here");
    return false;
}

/* §7.1.2: "The originAgentCluster getter steps are to return the surrounding agent's agent cluster's is
   origin-keyed." §8.1.7.1: "The crossOriginIsolated getter steps are to return this's relevant settings
   object's cross-origin isolated capability" — the §7.2.2 field above, which is stated OVER this cluster's
   mode and is why the two members are one component and not two booleans that could disagree. */
enum { AC_ORIGIN_KEYED, AC_CROSS_ORIGIN_ISOLATED };

static JSValue js_agent_cluster(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val;
    switch (magic) {
    case AC_ORIGIN_KEYED:
        return JS_NewBool(ctx, agent_cluster_is_origin_keyed());
    default:
        DCHECK(magic == AC_CROSS_ORIGIN_ISOLATED,
               "an agent-cluster getter was installed with a magic that names no member of this file");
        return JS_NewBool(ctx, agent_cluster_cross_origin_isolated(ctx));
    }
}

void agent_cluster_install(JSContext *ctx, JSValueConst global)
{
    idl_install_accessor(ctx, global, "originAgentCluster", js_agent_cluster, AC_ORIGIN_KEYED, -1);
    idl_install_accessor(ctx, global, "crossOriginIsolated", js_agent_cluster, AC_CROSS_ORIGIN_ISOLATED, -1);
}

void agent_cluster_release(void)
{
    /* The cluster goes with the AGENT — platform_agent_free. Nothing here is allocated; what is released is
       the STATEMENT that an agent exists, so a second instance in one process (the native host re-executing
       itself as a peer) allocates its own rather than reading the previous one's answer. */
    g_agent_obtained = false;
    g_is_origin_keyed = false;
}
