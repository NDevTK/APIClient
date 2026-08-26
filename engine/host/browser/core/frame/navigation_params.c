/* HTML §7.4.6/§7.5.1 — what a response's header list decides about the Document it creates. See
   navigation_params.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/fetch/structured_fields.h"
#include "core/frame/navigation_params.h"
#include "core/frame/policy_container.h"

/* §7.5.1's requestsOAC, verbatim:
     "Let oacHeader be the result of getting a structured field value given `Origin-Agent-Cluster` and `item`
      from navigationParams's response's header list.
      Let requestsOAC be true if oacHeader is not null and oacHeader[0] is the boolean true; otherwise false.
      If navigationParams's reserved environment is a non-secure context, then set requestsOAC to false."
   §7.1.2 states the same rule from the other end — "a Document delivered over a SECURE CONTEXT can request
   that it be placed in an origin-keyed agent cluster" — and adds that "values that are not the structured
   header boolean true value (i.e. `?1`) will be ignored", which is why the kind is tested and not just the
   presence: `Origin-Agent-Cluster: 1` is a valid structured field carrying an INTEGER, and it requests
   nothing. */
static bool np_requests_oac(const HeaderList *headers, bool secure_context)
{
    SfItem it;
    bool requests = false;

    if (sf_header_item(headers, "origin-agent-cluster", &it)) {
        requests = it.item.kind == SF_BOOLEAN && it.item.boolean;
        sf_item_free(&it);
    }
    return secure_context && requests;
}

void navigation_params_from_response(NavigationParams *out, const HeaderList *headers,
                                     SandboxFlags target_sandbox_flags, bool secure_context)
{
    DCHECK(out != NULL && headers != NULL,
           "navigation params were built from no response — the caller that has no response performs §7.1.7's "
           "determine-navigation-params-policy-container instead, which clones somebody's container, and an "
           "empty header list here would answer 'no policy' for a Document that inherits one");
    memset(out, 0, sizeof *out);

    /* §7.1.7 step 3: "set result's CSP list to the result of parsing a response's Content Security Policies
       given response". Fetch's `get` joins repeated headers with ", ", which is CSP §2.2's own serialization
       of a policy LIST — so several `Content-Security-Policy` headers arrive here as the list they are, and
       policy_container.c splits them apart again on the comma. Nothing is dropped and nothing is merged.
       `Content-Security-Policy-Report-Only` is deliberately not read: every policy this engine parses has
       disposition ENFORCE (core/frame/policy_container.h), and a report-only policy blocks nothing. */
    out->csp = header_list_get(headers, "content-security-policy");

    /* §7.4.5's FINAL SANDBOXING FLAG SET. The caller's half plus this response's, and the union is HERE rather
       than at the call site because the second half is a fact about the response and this is the one place a
       response is read. */
    out->sandbox_flags = target_sandbox_flags |
                         policy_csp_derived_sandboxing_flags(out->csp, out->csp ? strlen(out->csp) : 0);

    out->requests_oac = np_requests_oac(headers, secure_context);

    /* §7.1.4's obtain, which §7.1.7's create-a-policy-container-from-a-fetch-response step 4 makes the policy
       container's EMBEDDER POLICY item: "if environment is non-null, then set result's embedder policy to the
       result of obtaining an embedder policy given response and environment".
       IT IS THE CONTAINER'S ITEM AND THIS STRUCT ONLY CARRIES IT AS FAR AS THE CREATION. Every caller that
       builds a Document out of these params hands it to serialized_policy_container as the response's own
       container's item — which is the ONLY thing this value is for, and the reason it is not read anywhere
       else here: which container a Document is created with is §7.1.7's determine step's answer, and the
       obtained policy is the candidate that step may or may not pick. */
    embedder_policy_obtain(&out->embedder, headers, secure_context);

    /* §7.1.3's obtain, which §7.5.1's creation table gives the Document as its OPENER POLICY row.
       THE ROW HAS A HOME AND THE SWAP HAS A DECISION NOW, which is what used to be missing here — this line
       stood beside a crash saying so, and six of the thirty real signed-out product surfaces in this project's
       corpus (vercel, stripe, posthog, discourse, dropbox, fly.io) send a `Cross-Origin-Opener-Policy` and
       therefore aborted at it before a single flow ran. The row is the NAVIGABLE's, because §7.1.3.2 and
       §7.3.2.1 both read it off a browsing context's ACTIVE DOCUMENT (core/frame/window_proxy.h); the group
       and its cross-origin isolation mode are core/frame/browsing_context_group.h's; and §7.1.3.2's four
       decisions are in core/frame/opener_policy.h, asked by §7.4.5's own arm in core/frame/navigable.c and by
       the hosts that root an agent at a response. */
    opener_policy_obtain(&out->opener, headers, secure_context);
}

void navigation_params_free(NavigationParams *p)
{
    if (!p) return;
    free(p->csp);
    p->csp = NULL;
    embedder_policy_free(&p->embedder);
    opener_policy_free(&p->opener);
}
