/* HTML §7.1.2's agent cluster, as one fact its four readers each take their own question to. See
   agent_cluster.h. */
#include "check.h"
#include "core/frame/agent_cluster.h"
#include "core/idl_args.h"

/* §7.1.4's CROSS-ORIGIN ISOLATION MODE for this browsing context group, and it is THREE-VALUED because its two
   readers read it differently. §7.1.4 gives `none`, `logical` and `concrete`; §8.1.2.2's key allocation asks
   whether it is NOT `none`, while §7.2.2's cross-origin isolated capability asks whether it IS `concrete`. A
   single boolean answered both, which is one fact collapsed into the WRONG shape rather than into one place:
   under `logical` a Document is origin-keyed and `crossOriginIsolated` is FALSE, and a boolean cannot say that.
   The mode is set by ONE step of ONE algorithm: §7.1.3.2's obtain-a-browsing-context-to-use-for-a-navigation-
   response, on a browsing context group SWAP — "if navigationCOOP's value is `same-origin-plus-COEP`, then set
   newBrowsingContext's group's cross-origin isolation mode to either `logical` or `concrete`". So it takes a
   COOP value on the navigation params, which takes a `Cross-Origin-Opener-Policy` response header, and no
   route carries one into this engine: the only header a Document is created with is its
   `Content-Security-Policy` (core/dom/document.h's install), §7.1.7's EMBEDDER POLICY has no writer, and the
   OPENER POLICY §7.5.1 gives a Document beside its policy container is not a field this build has. The mode is
   therefore the standard's initial value, `none`. Evaluated at the step that asks it rather than assumed away,
   and the day a response's headers reach a Document this is the one line that reads them. */
typedef enum {
    AC_ISOLATION_NONE = 0,
    AC_ISOLATION_LOGICAL,
    AC_ISOLATION_CONCRETE,
} AcIsolationMode;

static AcIsolationMode cross_origin_isolation_mode(void)
{
    return AC_ISOLATION_NONE;
}

bool agent_cluster_is_origin_keyed(void)
{
    /* §8.1.2.2 step 3: "if group's cross-origin isolation mode is not `none`, then set key to origin" — the one
       input this engine can currently be non-default in. §7.1.2 states the same thing from the other end:
       "Documents whose agent cluster's cross-origin isolation mode is not `none` are automatically
       origin-keyed", which is `logical` AS WELL AS `concrete` and is the half of the mode this reader wants. */
    if (cross_origin_isolation_mode() != AC_ISOLATION_NONE) return true;
    /* §8.1.2.2 steps 4-5: the historical agent cluster key map, and `requestsOAC` — which §7.5.1's create-and-
       initialize sets from the `Origin-Agent-Cluster` response header ("let oacHeader be the result of getting
       a structured field value given `Origin-Agent-Cluster` and `item` from navigationParams's response's
       header list"). Neither reaches a Document here (see cross_origin_isolation_mode above for where a header
       would arrive), so the key stays the SITE and step 6's "if key is an origin ... set agentCluster's is
       origin-keyed to true" never runs. */
    return false;
}

bool agent_cluster_cross_origin_isolated(JSContext *ctx)
{
    /* HTML §7.2.2's set up a window environment settings object defines this environment field: "The
       cross-origin isolated capability — return true if both of the following hold, and false otherwise:
       realm's agent cluster's cross-origin-isolation mode is `concrete`, and window's associated Document is
       allowed to use the `cross-origin-isolated` feature." FIRST CONJUNCT, and note it is `concrete` alone —
       §7.1.4's `logical` mode is the one where a page IS origin-keyed and this capability is still false. */
    (void)ctx;   /* the SECOND conjunct is the Document's, and reads this environment — see the DFAIL below */
    if (cross_origin_isolation_mode() != AC_ISOLATION_CONCRETE) return false;
    /* SECOND CONJUNCT. It is unreachable while no COOP/COEP header reaches a policy container, and it is a
       crash rather than a `true` because assuming it would hand every cross-origin-isolated environment a
       capability the Document's permissions policy may deny — and the first thing that reads the answer is
       HR-TIME §4's clock resolution, which a page measures directly. */
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
