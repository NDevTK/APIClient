/* HTML §7.3.2.3's browsing context group, for the one group this instance is in, and §7.1.3.2's swap out of
   it. See browsing_context_group.h. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "core/frame/browsing_context_group.h"
#include "core/frame/window_proxy.h"
#include "solver/engine.h"
#include "solver/world.h"

static bool                              g_created;
static BrowsingContextGroupIsolationMode g_mode;

void browsing_context_group_create(OpenerPolicyValue navigation_coop)
{
    DCHECK(!g_created,
           "a second browsing context group was created in one WASM instance — SECURITY.md keys an instance on "
           "`(browsing context group, origin)`, so a second group here is a second agent cluster sharing one "
           "heap. What creates a second group is §7.1.3.2's browsing context group SWITCH, and a navigable that "
           "switches leaves this instance for one the host provisions");
    /* §7.3.2.3: "A browsing context group has a cross-origin isolation mode … It is initially `none`." */
    g_mode = BROWSING_CONTEXT_GROUP_ISOLATION_NONE;
    /* §7.1.3.2's obtain-a-browsing-context-to-use-for-a-navigation-response, on the swap arm: "If
       navigationCOOP's value is `same-origin-plus-COEP`, then set newBrowsingContext's group's cross-origin
       isolation mode to either `logical` or `concrete`. The choice of which is implementation-defined." This is
       the ONLY assignment of the mode in the whole standard, which is why it lives inside the create and there
       is no setter. `concrete` is the choice — see the header for why `logical` would be a browser this is
       not. */
    if (navigation_coop == OPENER_POLICY_SAME_ORIGIN_PLUS_COEP)
        g_mode = BROWSING_CONTEXT_GROUP_ISOLATION_CONCRETE;
    g_created = true;
}

BrowsingContextGroupIsolationMode browsing_context_group_isolation_mode(void)
{
    DCHECK(g_created,
           "this instance's browsing context group was asked for its cross-origin isolation mode before "
           "§7.3.2.3's create-a-new-browsing-context-group ran — every host reaches that through "
           "platform_agent_init, so a group without one is an instance that was built past the one list every "
           "host goes through");
    return g_mode;
}

void browsing_context_group_release(void)
{
    g_created = false;
    g_mode = BROWSING_CONTEXT_GROUP_ISOLATION_NONE;
}

void browsing_context_group_swap(JSContext *ctx, JSValueConst proxy, const char *url, const Origin *origin,
                                 SandboxFlags final_sandbox_flags, const char *provenance)
{
    uint32_t swapped;
    char *op;

    DCHECK(url != NULL && *url && origin != NULL,
           "§7.1.3.2's browsing context group swap was asked to create a top-level browsing context with no "
           "destination — the address and the principal are the navigation's, and the new instance is created "
           "from exactly the two of them");
    /* AND THE THIRD FACT THE NAVIGATION CARRIES, asserted here because the zone's decision to LOAD this
       address at all is made from it alone — see the header. An empty one is not "no opinion": it is a caller
       that stopped stating a field whose absence §Attacker-sources makes a crash at the decision. */
    DCHECK(provenance != NULL && *provenance,
           "§7.1.3.2's group swap was asked to announce an instance with NO PROVENANCE for the navigation that "
           "caused it — the load job carries the word (core/frame/navigable.c) and the trusted zone re-fetches "
           "this address to build the Document, so without it that zone is deciding whether to spend the "
           "network, and whose session, on a navigation nothing has told it who named");
    /* §7.1.3.2 STEP 2 — "if browsingContext is not a top-level browsing context, then return browsingContext".
       Reaching the swap for a child navigable means the predicate was asked where the standard never asks it;
       §7.4.5 obtains an opener policy only for a top-level traversable, which is the same fact from the other
       end and is where core/frame/navigable.c gates the call. */
    DCHECK(window_proxy_is_top_level(proxy),
           "§7.1.3.2's group swap was reached for a navigable that is not a TOP-LEVEL browsing context — its "
           "step 2 returns before the predicate is even evaluated for a child, and §7.4.5 obtains no opener "
           "policy for one, so a child that got here was decided by a policy it never had");
    /* §7.1.3.2 STEPS 13-14 — "let sandboxFlags be a clone of navigationParams's final sandboxing flag set. If
       sandboxFlags is not empty: … Set newBrowsingContext's popup sandboxing flag set to sandboxFlags."
       §7.1.5's set does not cross the record, so a non-empty one would be a sandbox that exists in the markup
       and nowhere in the model — the identical gap core/frame/navigable.c names on its own create notice, and
       it is asserted here rather than dropped for the same reason. (Step 14's own first assertion —
       "navigationCOOP's value is `unsafe-none`" — is §7.4.5's network-error arm one call up, which
       core/frame/navigable.c asserts before this is reached.) */
    DCHECK(final_sandbox_flags == 0,
           "§7.1.3.2 step 14 must put this navigation's FINAL SANDBOXING FLAG SET on the swapped-to browsing "
           "context's POPUP sandboxing flag set, and the `navigable.swap` record carries no flag set — so the "
           "peer instance would build this Document unsandboxed, running scripts and submitting forms the "
           "`sandbox` directive forbids. Add the set to the record (it is one word, and it crosses as text like "
           "every other field) and hand it to that instance's document_install as its active sandboxing flag "
           "set — the same field core/frame/navigable.c's create notice owes");
    /* §7.1.3.2 STEP 10 — "let newBrowsingContext be the first return value of creating a new top-level browsing
       context and document". The NAME is minted here because a name is all this instance may mint (SECURITY.md)
       and because minting is synchronous and unbounded (solver/world.h); it is deliberately NOT adopted — this
       agent does not host the document, the host provisions an instance that does, and world_doc_adopt is the
       statement that would make every read through it resolve into this heap instead of suspending. Minted from
       the navigable's CURRENT document, which is the one the swap supersedes, so the name is unique by the same
       induction every other document name here rests on. */
    swapped = world_mint_doc(window_proxy_doc(proxy));
    /* NO CREATOR FIELD AND NO POLICY FIELD, because §7.3.2.3 creates this browsing context "with null, null,
       and group": there is no creator to clone a policy container from and none to inherit an opener policy
       from. A field that is always empty is a field no reader can validate, so the record does not carry one.
       NO TOP-LEVEL CREATION URL EITHER: the swapped-to navigable IS a top-level traversable, so HTML §8.1.3.1's
       top-level creation URL for its environments is its own address — one fact, stated once, derived by the
       zone from the address it is already given rather than sent twice and able to disagree.
       A FOURTH FIELD THAT IS THE NAVIGATION'S AND NOT THE RECORD'S: what this load is evidence of. It is the
       one field that is NOT derivable by the zone — every other fact here it can compute from the address or
       from §7.3.2.3's own "null, null, and group", and this one is a statement about the PATH the engine ran,
       which nothing outside the engine can see. TAB-SAFE by its vocabulary: solver/engine.h's three tokens are
       ASCII lowercase letters.
       SIZED FROM THIS ARRAY AND NOT BY HAND — the same primitive the create arm uses, and it is here for a
       reason that had not yet cost anything: this record's slack constant was written when it carried three
       fields, the provenance field made it four, and the constant was generous enough to absorb the extra TAB.
       The create arm's was EXACT and the identical edit truncated it. A producer that is correct only because
       its hand-computed slack happened to be loose is one field away from the same defect, so the arithmetic
       goes rather than being corrected. */
    const char *const fields[] = {
        world_doc_name(swapped),
        url,
        origin_serialized(origin),
        provenance,
    };

    op = engine_notice_build("navigable.swap", fields, sizeof fields / sizeof fields[0]);
    engine_host_notify(ctx, op);
    free(op);
    /* AND THE OLD BROWSING CONTEXT IS DISCARDED — step 10's note, "browsingContext will not be used by the new
       Document that we are about to create", which is what §7.2.2.1's `closed` getter reads as "this's browsing
       context is null". PER FLOW, through the WindowProxy record's own COW capture: a sibling arm whose
       response carried no such policy still holds the window open.
       IT IS NOT §7.5.10's DESTROY, AND THE TWO ARE DIFFERENT WRITES for that reason. §7.5.10 hands the Window
       back and nulls the realm; step 10's note says only that a user agent "might destroy" this browsing
       context, and this engine keeps a superseded Document alive on EVERY navigation because a flow parked
       inside it resumes there. So the Document behind this handle stays exactly where it was — which is also
       what makes a read through the handle answer about THAT document rather than about the one the navigation
       went on to create somewhere else. */
    window_proxy_discard_browsing_context(ctx, proxy);
}
