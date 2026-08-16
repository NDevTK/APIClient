/* HTML §7.1.2's agent cluster, as one fact three members read. See agent_cluster.h. */
#include "check.h"
#include "core/frame/agent_cluster.h"
#include "core/idl_args.h"

/* §7.1.3/§7.1.4's CROSS-ORIGIN ISOLATION MODE for this browsing context group. It is derived from the
   `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` response headers of the top-level Document,
   and §7.2.6's policy container is where a Document's headers land — this engine's carries a CSP and nothing
   else (core/frame/policy_container.c), so no Document it builds has ever been handed either header and the
   mode is the standard's initial value, "none". Evaluated at the step that asks it rather than assumed away,
   and the day the container carries a COOP/COEP pair this is the one line that reads it. */
static bool cross_origin_isolated(void)
{
    return false;
}

bool agent_cluster_is_origin_keyed(void)
{
    /* §8.1.2.2 step 3: "if group's cross-origin isolation mode is not `none`, then set key to origin" — the one
       input this engine can currently be non-default in. §7.1.2 states the same thing from the other end:
       "Documents whose agent cluster's cross-origin isolation mode is not `none` are automatically
       origin-keyed". */
    if (cross_origin_isolated()) return true;
    /* §8.1.2.2 steps 4-5: the historical agent cluster key map, and `requestsOAC` — which §7.11's create-and-
       initialize sets from the `Origin-Agent-Cluster` response header. Neither reaches a Document here (see
       cross_origin_isolated above for where a header would arrive), so the key stays the SITE and step 6's
       "if key is an origin ... set agentCluster's is origin-keyed to true" never runs. */
    return false;
}

/* §7.1.2: "The originAgentCluster getter steps are to return the surrounding agent's agent cluster's is
   origin-keyed." §8.1.7.1: "The crossOriginIsolated getter steps are to return the surrounding agent's agent
   cluster's cross-origin isolation mode is `logical` or `concrete`" — which is the negation of "none", and is
   why the two are one component and not two booleans that could disagree. */
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
        return JS_NewBool(ctx, cross_origin_isolated());
    }
}

void agent_cluster_install(JSContext *ctx, JSValueConst global)
{
    idl_install_accessor(ctx, global, "originAgentCluster", js_agent_cluster, AC_ORIGIN_KEYED, -1);
    idl_install_accessor(ctx, global, "crossOriginIsolated", js_agent_cluster, AC_CROSS_ORIGIN_ISOLATED, -1);
}
