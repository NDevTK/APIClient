/* HTML §7.1.4's cross-origin embedder policy. See embedder_policy.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/fetch/structured_fields.h"
#include "core/frame/embedder_policy.h"

static char *ep_dup(const char *s)
{
    char *r = strdup(s ? s : "");
    CHECK(r != NULL, "embedder policy: OOM copying a reporting endpoint");
    return r;
}

void embedder_policy_init(EmbedderPolicy *p)
{
    DCHECK(p != NULL, "an embedder policy was initialized into nothing");
    p->value = EMBEDDER_POLICY_UNSAFE_NONE;
    p->report_only_value = EMBEDDER_POLICY_UNSAFE_NONE;
    /* §7.1.4: "a reporting endpoint string, INITIALLY THE EMPTY STRING" — not null. The difference is real:
       the opener policy's endpoint is initially NULL, so the two absences are spelled differently and a
       consumer that tested one for the other would read this one as present. */
    p->endpoint = ep_dup("");
    p->report_only_endpoint = ep_dup("");
}

void embedder_policy_free(EmbedderPolicy *p)
{
    if (!p) return;
    free(p->endpoint);
    free(p->report_only_endpoint);
    p->endpoint = NULL;
    p->report_only_endpoint = NULL;
}

bool embedder_policy_compatible_with_cross_origin_isolation(EmbedderPolicyValue v)
{
    /* §7.1.4, verbatim: "An embedder policy value is compatible with cross-origin isolation if it is
       `credentialless` or `require-corp`." */
    return v == EMBEDDER_POLICY_CREDENTIALLESS || v == EMBEDDER_POLICY_REQUIRE_CORP;
}

/* §7.1.4.1: "The valid token values are the embedder policy values." A token outside them is not a value, and
   the caller's fail-open then leaves the policy at its default. `unsafe-none` is a valid token and IS the
   default, which is why this can answer it without the caller needing a third outcome. */
bool embedder_policy_value_of_token(const char *token, EmbedderPolicyValue *out)
{
    if (!token) return false;
    if (!strcmp(token, "unsafe-none"))    { *out = EMBEDDER_POLICY_UNSAFE_NONE;    return true; }
    if (!strcmp(token, "require-corp"))   { *out = EMBEDDER_POLICY_REQUIRE_CORP;   return true; }
    if (!strcmp(token, "credentialless")) { *out = EMBEDDER_POLICY_CREDENTIALLESS; return true; }
    return false;
}

const char *embedder_policy_value_token(EmbedderPolicyValue v)
{
    switch (v) {
    case EMBEDDER_POLICY_UNSAFE_NONE:    return "unsafe-none";
    case EMBEDDER_POLICY_REQUIRE_CORP:   return "require-corp";
    case EMBEDDER_POLICY_CREDENTIALLESS: return "credentialless";
    }
    /* NOT A `default:` ARM, so the day §7.1.4 gains a fourth value the COMPILER names every switch that has
       to grow. This is reached only by a value that is not one of the three, which is a cast from an integer
       somebody read off a wire without asking whether it names a policy. */
    DFAIL("an embedder policy value that HTML §7.1.4 does not define was serialized — the section names three "
          "strings and embedder_policy_value_of_token is what turns bytes back into one, so a value outside "
          "them is an integer that crossed a seam pretending to be a policy");
    return "unsafe-none";
}

SerializedEmbedderPolicy serialized_embedder_policy(EmbedderPolicyValue value, const char *endpoint,
                                                    EmbedderPolicyValue report_only_value,
                                                    const char *report_only_endpoint)
{
    SerializedEmbedderPolicy out;

    /* §7.1.4 MAKES BOTH ENDPOINTS STRINGS WHOSE ABSENCE IS THE EMPTY ONE, so there is no NULL to spell here —
       and refusing it at the one constructor is what keeps every seam from having to decide what a NULL meant.
       An opener policy's endpoint IS null initially (core/frame/opener_policy.h), which is why the two are
       spelled differently and why a caller that copied the other one's shape crashes here rather than handing
       a reader an absence that is not this standard's. */
    DCHECK(endpoint != NULL && report_only_endpoint != NULL,
           "§7.1.4's embedder policy was built with a NULL reporting endpoint — the section says \"a reporting "
           "endpoint STRING, initially the empty string\", so the absence of one IS the empty string and a "
           "NULL is a producer that stopped writing the item rather than a policy that names no endpoint");
    out.value = value;
    out.endpoint = endpoint;
    out.report_only_value = report_only_value;
    out.report_only_endpoint = report_only_endpoint;
    return out;
}

SerializedEmbedderPolicy serialized_embedder_policy_new(void)
{
    return serialized_embedder_policy(EMBEDDER_POLICY_UNSAFE_NONE, "", EMBEDDER_POLICY_UNSAFE_NONE, "");
}

SerializedEmbedderPolicy serialized_embedder_policy_of(const EmbedderPolicy *p)
{
    DCHECK(p != NULL, "§7.1.4's embedder policy was serialized from nothing — every §7.1.7 policy container "
                      "holds one, initially a new one, so a NULL here is a caller holding half a container");
    return serialized_embedder_policy(p->value, p->endpoint, p->report_only_value, p->report_only_endpoint);
}

void embedder_policy_adopt(EmbedderPolicy *out, SerializedEmbedderPolicy s)
{
    DCHECK(out != NULL, "§7.1.7's clone was asked to copy an embedder policy into nothing");
    /* EVERY OWNED FIELD IS DUPLICATED, WHICH IS THE WHOLE OF THIS FUNCTION AND THE WHOLE OF THE OBLIGATION AN
       ITEM CREATES. A struct copied field-by-field must dup every owned field, so the day §7.1.4 gains a fifth
       item this is one of the two places that must grow — the other is embedder_policy_free, and they are read
       together for exactly that reason. `serialized_embedder_policy` has already refused a NULL endpoint, so
       ep_dup here is copying a string rather than deciding what an absence meant. */
    out->value = s.value;
    out->report_only_value = s.report_only_value;
    out->endpoint = ep_dup(s.endpoint);
    out->report_only_endpoint = ep_dup(s.report_only_endpoint);
}

/* One header's contribution: parse it as an ITEM, and answer only when the item is a TOKEN naming a value that
   is compatible with cross-origin isolation — which is exactly the condition BOTH branches of §7.1.4's obtain
   are written with ("if parsedItem is non-null and parsedItem[0] is compatible with cross-origin isolation").
   `*endpoint` is replaced when the item carries a `report-to` parameter that is a STRING. */
static void ep_read(const HeaderList *headers, const char *name, EmbedderPolicyValue *value, char **endpoint)
{
    SfItem it;
    EmbedderPolicyValue v;
    const SfBareItem *report_to;

    if (!sf_header_item(headers, name, &it))
        return;   /* absent, or not a structured field at all — §7.1.4.1's fail-open */
    if (it.item.kind != SF_TOKEN || !embedder_policy_value_of_token(it.item.text, &v) ||
        !embedder_policy_compatible_with_cross_origin_isolation(v)) {
        sf_item_free(&it);
        return;
    }
    *value = v;
    /* §7.1.4.1: "of these, the `report-to` parameter can have a valid URL string identifying an appropriate
       reporting endpoint". A `report-to` that is not a string is not one, and the parameter is then ignored
       exactly as an absent one is. */
    report_to = sf_item_param(&it, "report-to");
    if (report_to && report_to->kind == SF_STRING) {
        free(*endpoint);
        *endpoint = ep_dup(report_to->text);
    }
    sf_item_free(&it);
}

void embedder_policy_obtain(EmbedderPolicy *out, const HeaderList *headers, bool secure_context)
{
    DCHECK(out != NULL && headers != NULL, "an embedder policy was obtained from nothing");
    embedder_policy_init(out);   /* step 1: "let policy be a new embedder policy" */
    /* Step 2: "if environment is a NON-SECURE CONTEXT, then return policy." A page served over http gets the
       default whatever it sends — which is why cross-origin isolation is unreachable off https and why the
       caller has to state the environment rather than this reading the header list alone. */
    if (!secure_context)
        return;
    ep_read(headers, "cross-origin-embedder-policy", &out->value, &out->endpoint);
    /* EACH ARM WRITES ITS OWN ENDPOINT, WHICH IS THE ITEM HTML §7.1.4.1 "The headers" NAMES AT EACH ARM. The
       two sentences are "Set policy's reporting endpoint to parsedItem[1]["report-to"]" and "Set policy's
       report-only reporting endpoint to parsedItem[1]["report-to"]" — two of §7.1.4's four items, never one
       item written twice.
       THIS CALL USED TO PASS `&out->endpoint` TOO, under a comment quoting a sentence that set `policy's
       endpoint` in BOTH arms and deferring to embedder_policy.h for why the collapse was faithful. That header
       had ALREADY retired the quotation as one occurring nowhere in HTML — and the code written to it outlived
       the retraction, so the collapse went on standing under a header that said in its own words that each arm
       writes its own. A FIX THAT RETIRES AN ARGUMENT FALSIFIES EVERY SITE THAT ARGUED IT, and this is the site
       that was missed; it is recorded rather than quietly corrected because the next reader who re-derives the
       collapse will re-derive it from the same absent sentence.
       THE COST WAS NOT THE UNREAD ITEM. A response that sends BOTH headers had the enforce arm's `report-to`
       OVERWRITTEN by the report-only arm's, because the second call wrote the field the first had just filled —
       so the endpoint §7.1.4.2 step 5 reports an ENFORCED violation to was whichever of the two headers the
       response happened to send second, and a report-only header could silently re-address an enforced
       violation's report. */
    ep_read(headers, "cross-origin-embedder-policy-report-only", &out->report_only_value,
            &out->report_only_endpoint);
}

/* §7.1.4.2's CHECK A NAVIGATION RESPONSE'S ADHERENCE TO ITS EMBEDDER POLICY, steps 3-6. Steps 1 and 2 belong to
 * the caller — see embedder_policy.h for why, and for why parentPolicy is read live rather than carried.
 *
 * NEITHER QUEUE STEP ASSERTS, AND THE OPERAND IS THE WHOLE REASON: BOTH POLICIES ARE A STRANGER'S BYTES.
 * `parentPolicy` is whatever the container document's server put in its `Cross-Origin-Embedder-Policy`, and
 * `responsePolicy` is whatever the framed response's server put in its own. A DCHECK asserts that THIS
 * codebase's logic is correct, and no byte of either policy is this codebase's — so an assert on them hands
 * every origin an abort switch for the dev engine, on the one input this product exists to run.
 * THIS PARAGRAPH USED TO SAY THE TWO QUEUE STEPS CRASH UNCONDITIONALLY, AND THAT THIS WAS THE STRICTER READING
 * OF THE CSP VIOLATION REFUSALS ONE FILE OVER — which it described as refusing only where the violated policy
 * DECLARES a reporting endpoint. Both halves are retired and neither was retired by disagreement. The TREE half
 * went stale: core/frame/policy_container.c's policy_blocks_request and core/html/html_base_element.c no longer
 * refuse at all, having removed those asserts on exactly the ground stated above, so there is no endpoint gate
 * left here to be stricter than. The ARGUMENT half was backwards: an endpoint gate is not a weaker crash, it is
 * a SMALLER abort switch, so the unconditional form was the larger defect rather than the more rigorous one.
 * What that paragraph got right is its SPEC half, which is kept below because it is why the residuals are
 * written for every violation rather than for the endpoint-bearing ones.
 * RETIREMENT: this record goes when no assert in this component stands on a value a response stated.
 *
 * A REPORT NOBODY NAMED AN ENDPOINT FOR STILL REACHES SOMEBODY, WHICH IS WHY NEITHER RESIDUAL IS GATED ON ONE.
 * Reporting §3.4.1 "Generate report of type with data" defines generate-and-queue-a-report as: let report be
 * the result of generating one, notify the reporting observers on the global with it, and "Append report to
 * context's reports". No endpoint is consulted anywhere in it. It is §3.5.1 "Send reports" — a later and
 * separate algorithm — that walks "context's endpoints list" and drops a report whose destination matches no
 * endpoint in it. HTML §7.1.4 states the consequence in as many words: the "coep" report type "is visible to
 * ReportingObservers". So the observable a container document is owed does not depend on that document having
 * sent `report-to`, and a residual written only for the endpoint-bearing half would name the smaller gap. */
bool embedder_policy_check_navigation_response(SerializedEmbedderPolicy parent_policy,
                                               SerializedEmbedderPolicy response_policy)
{
    /* Step 3: "If parentPolicy's report-only value is compatible with cross-origin isolation and
       responsePolicy's value is not, then queue a cross-origin embedder policy inheritance violation with
       response, "navigation", parentPolicy's REPORT ONLY REPORTING ENDPOINT, "reporting", and navigable's
       container document's relevant settings object." It does not decide the return value — step 4 is asked
       afterwards either way, and a parent that sends the report-only header alone still loads its frame.
       THE ENDPOINT THIS STEP NAMES IS §7.1.4's REPORT-ONLY REPORTING ENDPOINT, AND OBTAIN WRITES IT NOW. This
       paragraph used to say the opposite — that obtain never writes it, because both of its arms set `policy's
       endpoint` — and it was the second site of the fabricated quotation embedder_policy.h retired: the arm
       above writes the item §7.1.4.1 names at it, so a parent that sent `report-to` on its report-only header
       is reported to the endpoint that header named.

       NAMED RESIDUAL — WHAT IS NOT COVERED: step 3's queue, for every navigation this step describes. Because
       the step decides nothing, what this function answers is already the standard's answer and there is no
       arm here to get wrong; what is absent is the OBSERVABLE alone. The step is not written at all rather
       than written and gated, because the condition's only consumer was the report.
       WHAT THE NEXT DIFF BUILDS: §7.1.4.2's QUEUE A CROSS-ORIGIN EMBEDDER POLICY INHERITANCE VIOLATION, whose
       own steps are a `serialized` from Fetch §2.2.5 "Requests"' serialize-a-response-URL-for-reporting (which
       takes "a copy of response's URL list[0]" and says why in its own note — "This is not response's URL, in
       order to avoid leaking information about redirect targets"), a body of `type`, `blockedURL` and
       `disposition`, and queueing that body as the "coep" report type for the endpoint on the container
       document's relevant settings object. It needs Reporting §3.4.1 "Generate report of type with data" and
       §4's ReportingObserver underneath it, and those two land TOGETHER with this: `ReportingObserver` is named
       in this engine's exposure tables and installed by nothing, so an interface installed ahead of the queue
       would flip a page's own `if (window.ReportingObserver)` true over a buffer nothing ever fills.
       HOW ITS ABSENCE WOULD SHOW: a container document observing the "coep" report type receives nothing when
       a frame it embeds is refused, where a browser delivers one report per refusal. It is observable from no
       page TODAY, because the observer that would read it is itself absent — which is the reason the two are
       one landing and the reason neither half can be scored alone. */
    /* Step 4: "If parentPolicy's value is not compatible with cross-origin isolation or responsePolicy's value
       is compatible with cross-origin isolation, then return true." */
    if (!embedder_policy_compatible_with_cross_origin_isolation(parent_policy.value) ||
        embedder_policy_compatible_with_cross_origin_isolation(response_policy.value))
        return true;
    /* Step 5: the same queue with parentPolicy's REPORTING ENDPOINT and disposition "enforce". Two things this
       navigation is owed are unbuilt and they are NOT the same thing, which is why only one of them crashes and
       the crash is not here. THIS step owes a REPORT, and step 6's `return false` below is the standard's own
       answer — so this function is right and narrower, exactly as step 3 is. The BLOCKED NAVIGATION is the
       other, and it is a wrong ANSWER rather than a missing observable: §7.4.5 must give the navigable §7.5.7
       "Loading a document for inline content that doesn't have a DOM"'s Document instead of the response's, and
       nothing builds one, so a release build loads a frame a browser refuses. That is the unbuilt CAPABILITY,
       it is asserted at both of this function's callers (core/frame/navigable.c's js_nav_load_step and
       navigable_root), and those asserts are deliberately left standing: a crash whose absence makes an answer
       WRONG is the forcing function, where a crash whose absence costs only an observable is an abort switch a
       stranger holds. The two callers' asserts used to be unreachable in dev because THIS one fired first, so
       removing it does not weaken the check — it points the dev crash at the half that is actually wrong.

       NAMED RESIDUAL — WHAT IS NOT COVERED: step 5's queue, for every navigation this function refuses.
       WHAT THE NEXT DIFF BUILDS: the same queue step 3's residual above names, which serves both dispositions —
       one algorithm, called twice with "reporting" and "enforce" and with the two different endpoint items.
       HOW ITS ABSENCE WOULD SHOW: a container document observing the "coep" report type receives nothing for a
       frame this function refuses, where a browser delivers one; and the refusal itself remains visible only as
       the callers' abort rather than as the error Document a page would see. */
    /* Step 6. */
    return false;
}
