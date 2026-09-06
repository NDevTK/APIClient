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

    /* PERMISSIONS POLICY §9.1 "Process response policy" — "let header name be `Permissions-Policy-Report-Only`
     * if report-only is True, or `Permissions-Policy` otherwise. Let parsed header be the result of executing
     * get a structured field value given header name and `dictionary` from response's header list. If parsed
     * header is null, return an empty ordered map."
     *
     * THE ABSENT HEADER IS THE ANSWER, NOT A HOLE, and this is where that is stated. Its step 3 gives an absent
     * header a MEANING — the empty declared policy — which is exactly what §9.5's create hands every Document
     * this build makes, so a response with neither header is fully modelled and nothing here fires.
     *
     * WHAT IS PERFORMED HERE IS §9.1'S STEP 1 AND FETCH'S HALF OF ITS STEP 2, AND NOT THE PARSE. Step 1 picks
     * the header NAME from `report-only`, and step 2's get-a-structured-field-value begins with Fetch §2.2.2
     * step 2's JOIN of every header of that name with 0x2C 0x20. Both are questions about a header LIST, and a
     * header list exists only while the response does — this engine frees it at the parse, deliberately, so
     * that nothing the bundle runs can ask for a header again. The rest of §9.1 needs the ORIGIN, which is not
     * decided until HTML §7.5.1 creates the Document, so it runs there
     * (core/permissions_policy/permissions_policy.h's permissions_policy_apply_response).
     *
     * BOTH NAMES ARE GOT UNCONDITIONALLY BECAUSE `report-only` IS NOT KNOWN HERE — §10.1 "Changes to the HTML
     * specification" inserts a SECOND §9.6 call beside §7.5.1's with report-only True, so ONE response feeds
     * two policies and this struct carries what both of them will ask for. Getting one and not the other would
     * be this function deciding which of §7.5.1's two calls exists.
     *
     * AN ABSENT HEADER IS NULL AND THAT IS A POSITIVE STATEMENT — §9.1 step 3's empty ordered map, which §9.6's
     * merge then has nothing to copy from, leaving §9.5's policy. It is the same answer a response carrying a
     * MALFORMED value gets, which is Fetch's own note ("get a structured field value intentionally does not
     * distinguish between a header not being present and its value failing to parse") and is decided at the
     * parse rather than here.
     *
     * IT IS READ HERE BECAUSE THIS IS WHERE A RESPONSE IS READ — the same seam §7.1.3's opener policy and
     * §7.1.4's embedder policy are obtained at. A ROOT document's headers DO reach it: an entry that roots an
     * agent at a response is handed the response's header block, parses it into a Fetch header list and calls
     * this function over that list.
     *
     * WHAT THIS DOES NOT COVER IS THE RESPONSE THAT DOES NOT COME THROUGH THIS FUNCTION — a CHILD NAVIGABLE's.
     * Its loader parses its own header list and then performs §7.4.6 PIECEWISE off it, calling §7.1.4's and
     * §7.1.3's obtains directly and asking `header_list_get` for the CSP list and the computed type; the piece
     * it does not perform is this one, so a framed document's `Permissions-Policy` is dropped in silence while
     * the identical header on the framing document is applied. §7.5.1's `requestsOAC` is NOT the same omission
     * and must not be read as one: §8.1.2.2 allocates a cluster once per agent and §7.1.2's note says a later
     * same-origin Document inherits that allocation, so a child navigable of this agent has no such question to
     * ask.
     * ONE ALGORITHM ASKED AT ONE ENTRY AND NOT AT ANOTHER IS THE DEFECT, AND THE FIX IS AT THE ENTRY THAT DOES
     * NOT ASK — the child loader builds its navigation params through this function like every other entry.
     * It is not a second reader here, and it is not a field on the host record: a fact this engine already
     * decides from a response it already holds does not need a second door in from the trusted zone. */
    out->permissions_policy             = header_list_get(headers, "permissions-policy");
    out->permissions_policy_report_only = header_list_get(headers, "permissions-policy-report-only");
    /* SRI §3.8.1 step 2's header, by the name that standard gives it. The report-only one its step 3 reads is
       deliberately not taken — see the field's own note. */
    out->integrity_policy               = header_list_get(headers, "integrity-policy");

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
    free(p->permissions_policy);
    p->permissions_policy = NULL;
    free(p->permissions_policy_report_only);
    p->permissions_policy_report_only = NULL;
    free(p->integrity_policy);
    p->integrity_policy = NULL;
    embedder_policy_free(&p->embedder);
    opener_policy_free(&p->opener);
}
