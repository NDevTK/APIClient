/* THE POLICY CONTAINER — HTML §7.2.6, over CSP's own policy/directive model.
 *
 * WHY IT EXISTS, AND WHY IT IS THE ANSWER TO "how does an about:blank child get a policy". A navigable created
 * with no URL — `window.open()` with no argument, `<iframe>` with no src — gets its initial `about:blank`
 * Document SYNCHRONOUSLY, and that Document has no response to take a policy from. HTML's answer is not a
 * special case: every Document has a POLICY CONTAINER, and §7.4's create-a-new-browsing-context says that when
 * there is a creator, the new document's policy container is a CLONE OF THE CREATOR'S. So the child inherits
 * the parent's CSP by the ordinary rule, not by an inheritance rule written for CSP.
 *
 * WHAT IS IN ONE IS §7.1.7'S FIVE ITEMS AND NO OTHERS — a CSP list, an EMBEDDER POLICY, a referrer policy, an
 * integrity policy and a report-only integrity policy. This line said "and sandboxing flags", which is the
 * claim five predicates in this tree were once written against and which the standard does not make: a
 * Document's ACTIVE SANDBOXING FLAG SET is a row of §7.5.1's creation table BESIDE the container
 * (core/frame/sandboxing.h), and so is its OPENER POLICY. What the container contributes to the flag set is one
 * half of it, through the CSP `sandbox` directive alone — the function at the bottom of this file, and the only
 * connection between the two.
 *
 * THE EMBEDDER POLICY IS THE ITEM THIS FILE STILL DOES NOT HOLD, AND THE REASON IS TRAVEL, NOT THE ITEM.
 * §7.1.4's obtain is built (core/frame/embedder_policy.h) and a response's is obtained where a response is read
 * (core/frame/navigation_params.c), which CRASHES BY NAME at any response whose policy this container would
 * drop. Adding the field before a container can travel AS a container would put a value in it that vanishes at
 * the two seams §7.4's clone crosses as CSP TEXT — window_proxy.c's `creator_csp`, for a lazily-materialized
 * about:blank child, and navigable.c's `navigable.create` notice, for a peer instance.
 *
 * AND THE CLONE CROSSES INSTANCES. A cross-origin creator and the child it clones from are not in the same
 * heap, so the clone is serialized as CSP TEXT and parsed again on the other side — which is exactly what
 * policy_container_clone does in one heap, so the two are one operation and the transport needs no second
 * representation. Nothing here may be shaped to avoid that transport; see policy_container.h.
 *
 * IT IS NOT YET PER-FLOW, and the place that will make it so is asserted rather than described. A flow that
 * NAVIGATES a frame replaces its policy container while a sibling that did not still holds the old one, so the
 * binding is per-flow state for the same reason the WindowProxy binding is. Navigation is not built, so today
 * a document's container is installed once on the baseline before any flow runs — and document_install CRASHES
 * on a second install rather than carrying a comment promising to handle it.
 *
 * THERE IS NO CSP PARSER IN THIS FILE ANY MORE, AND THAT IS THE POINT. It used to carry one: a `Directive`
 * struct of six booleans, a source-list scan that set them, and a `DCHECK` that ABORTED on any source
 * expression it did not model — a nonce, a hash, `https:`, `*.example.com`. That crash read as an honest gap
 * and was not one; it was naming a question this file had invented. CSP's own §6.7.3.2 decides whether inline
 * content runs by classifying a source list with exactly three tests and IGNORING every other expression, so
 * the real algorithm is total over every policy the web sends. The model those questions are now asked of is
 * core/frame/csp_directive_list.h (§2.2/§2.3) and core/frame/csp_source_list.h (§2.3.1/§6.7.3.2/§6.7.2), and
 * what is left here is HTML's container and the questions asked OF a container: §S's four about inline content
 * and `eval`, and CSP §4.1.2's about a URL — Fetch's main fetch step 7. The last one is here rather than in a
 * file of its own because it is the SAME walk over the same list with a different question per policy, and
 * because §2.2's "a second policy can only narrow" is then stated once. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/frame/csp_directive_list.h"
#include "core/frame/csp_source_list.h"
#include "core/frame/policy_container.h"
#include "core/frame/sandboxing.h"

struct PolicyContainer {
    /* The serialized CSP list, comma-delimited, or NULL for none (OWNED). It is kept because it is what the
       container TRAVELS as — a clone across an instance or a session is this string, re-parsed — and because a
       report is worth nothing without the policy text it names. */
    char    *csp_text;
    /* §2.2's CSP LIST, parsed over `csp_text`. Every name and value token is a SLICE of it, so the two fields
       are one value with one lifetime: free them together and never reallocate the text under the list. */
    CspList  csp;
    char    *referrer_policy;    /* §7.2.6's referrer policy (owned) */
};

PolicyContainer *policy_container_new(const char *csp_text, const Origin *self_origin,
                                      const char *referrer_policy)
{
    PolicyContainer *p = calloc(1, sizeof *p);

    CHECK(p != NULL, "policy container: OOM");
    if (csp_text && *csp_text) {
        p->csp_text = strdup(csp_text);
        CHECK(p->csp_text != NULL, "policy container: OOM copying a policy");
    }
    /* THE PARSE RUNS WHETHER OR NOT THERE IS TEXT, because §2.2.2's last step does: a container with no
       policy still holds a CSP LIST, and every list has a self-origin. Running it unconditionally is what
       keeps ONE writer for that field — a container built with no text and one built with text are the same
       shape, so no reader has to ask which of the two it is holding. */
    csp_list_parse(&p->csp, p->csp_text, p->csp_text ? strlen(p->csp_text) : 0, self_origin);
    if (referrer_policy) {
        p->referrer_policy = strdup(referrer_policy);
        CHECK(p->referrer_policy != NULL, "policy container: OOM copying a referrer policy");
    }
    return p;
}

PolicyContainer *policy_container_clone(const PolicyContainer *src)
{
    /* §7.2.6's "clone a policy container", and §7.4's answer to where an about:blank child's policy comes
       from. It is a DEEP copy: the child's policy is its own from the moment it exists, so a later navigation
       of the parent does not reach back and change what the child is allowed to do. Re-parsing rather than
       copying the parsed model is not laziness — the model's tokens point INTO the parent's text, so a
       structural copy would have to rebase every one of them, and re-parsing is the same operation the
       cross-instance and cross-session clones must perform anyway. */
    DCHECK(src != NULL, "a policy container was cloned from nothing — every document has one, including the "
                        "initial about:blank, which is the whole reason this operation exists");
    /* THE CLONE KEEPS THE SOURCE'S SELF-ORIGIN, WHICH IS THE POINT OF THE FIELD. §2.2's own note says it
       exists so that a document with an OPAQUE origin which inherited its policy still resolves `'self'`
       against the origin the policy came FROM — so re-deriving one here from the child would be the
       inheritance failing in exactly the case the field was added for. The RECORD travels, so the two
       containers share an identity as well as a tuple; where this clone crosses an instance instead, the
       origin crosses as its serialization like every other cross-boundary fact. */
    return policy_container_new(src->csp_text, src->csp.self_origin, src->referrer_policy);
}

void policy_container_free(PolicyContainer *p)
{
    if (!p) return;
    csp_list_free(&p->csp);   /* frees the parse's arrays; the text it points into is freed next */
    free(p->csp_text);
    free(p->referrer_policy);
    free(p);
}

const char *policy_container_csp(const PolicyContainer *p) { return p ? p->csp_text : NULL; }

const CspList *policy_container_csp_list(const PolicyContainer *p) { return p ? &p->csp : NULL; }

const Origin *policy_container_self_origin(const PolicyContainer *p) { return p ? p->csp.self_origin : NULL; }

SandboxFlags policy_csp_derived_sandboxing_flags(const char *serialized_csp_list, size_t len)
{
    /* §7.1.5's "Every CSP list cspList has CSP-derived sandboxing flags", verbatim: collect the `sandbox`
       directive of every ENFORCE policy, and if there are any, parse a sandboxing directive over the LAST
       one. Not the union of them — the last one WINS, which is the one place in CSP where a later policy does
       something other than narrow, and is why this cannot be answered from a per-container flag.
       IT TAKES TEXT rather than a container because the caller that needs it is the NAVIGATION and the
       container it would read does not exist yet: §7.5.1 hands the new Document its policy container and its
       final sandboxing flag set in the same breath. The parse BORROWS these bytes for the length of the call.
       WHAT THE `sandbox` DIRECTIVE'S VALUE IS: not a source list. §7.1.5's sandboxing-directive parse reads
       the whole value as a token set, which is why the model keeps the raw remainder beside the split — and
       why an ABSENT `sandbox` and a `sandbox` with an EMPTY value are not the same thing. The empty value is
       the most restrictive form there is; absence is an empty flag set. */
    CspList list;
    SandboxFlags out = 0;
    size_t i;

    memset(&list, 0, sizeof list);
    /* NO SELF-ORIGIN, and that is a statement rather than an omission: this list is read for ONE directive's
       raw value and is never asked to match a URL, so there is no `'self'` here to resolve. §6.7.2.7 asserts
       the origin is present, which is what makes this the only shape of list allowed to lack one. */
    csp_list_parse(&list, serialized_csp_list, len, NULL);
    for (i = 0; i < list.n_policies; i++) {
        const CspDirective *d = csp_policy_directive(&list.policies[i], "sandbox");
        if (d)
            out = sandbox_parse_directive(d->value_text.p, d->value_text.n);
    }
    csp_list_free(&list);
    return out;
}

/* §4.2.3's `type` for one of §S's three INLINE questions. `eval` is not an inline check at all — §4.4.1 asks a
   source list a different question entirely — so it is not in this mapping and reaching here with it is a bug
   in the caller rather than a type the standard omits. */
static CspInlineType inline_type_of(PolicyScriptKind kind)
{
    switch (kind) {
    case POLICY_INLINE_SCRIPT:  return CSP_INLINE_SCRIPT;
    case POLICY_INLINE_HANDLER: return CSP_INLINE_SCRIPT_ATTRIBUTE;
    case POLICY_JAVASCRIPT_URL: return CSP_INLINE_NAVIGATION;
    case POLICY_EVAL:           break;
    }
    DFAIL("a script kind with no §4.2.3 inline type was routed to the inline check — `eval` is §4.4.1's "
          "question about a source list and has no element, no type and no §6.8.2 mapping");
    return CSP_INLINE_SCRIPT;
}

/* §4.4.1's EnsureCSPDoesNotBlockStringCompilation, for ONE policy. Its directive lookup is written out in the
   algorithm itself and is NOT §6.8's fallback machinery: "if policy contains a directive whose name is
   script-src, set source-list to that directive's value; otherwise if policy contains default-src, that one."
   §6.1.10 says why in as many words — 'unsafe-eval' acts as a global page flag, so the granular script-src-elem
   and script-src-attr forms are never consulted for it.
   'trusted-types-eval' is the other expression that can permit compilation, and it cannot be reached here: it
   applies only when the code string arrived as a TrustedScript, and §2's three types do not exist in this
   engine, so no value can be one. That is the same step-is-decided-not-skipped reading core/html/
   trusted_types.c already relies on for §3.4 step 1, and it is why an eval sink under
   `require-trusted-types-for 'script'` is reported through the trusted-types answer rather than this one. */
static bool policy_allows_eval(const CspPolicy *policy)
{
    const CspDirective *d = csp_policy_directive(policy, "script-src");

    if (!d) d = csp_policy_directive(policy, "default-src");
    /* A policy with neither says nothing about compilation, which is not the same as forbidding it. */
    if (!d) return true;
    return csp_source_list_contains(d, "'unsafe-eval'");
}

/* §4.2.3 (and §4.2.4's javascript: half) for ONE policy: find the directive §6.8.4 says executes, then ask
   §6.7.3.2 whether its source list allows all inline behavior of this type.
   THE REST OF §6.7.3.3 CANNOT CHANGE THIS ANSWER for content an attacker supplies, and each of its remaining
   arms is decided rather than skipped: its nonce arm needs the injected element to carry a `nonce` attribute
   whose value the page's own policy lists, its hash arms need the injected source to hash to a listed digest,
   and its 'strict-dynamic' arm needs an element that is NOT parser-inserted — which injected markup never is,
   and which a javascript: navigation has no element for at all. */
static bool policy_allows_inline(const CspPolicy *policy, CspInlineType type)
{
    const CspDirective *d =
        csp_policy_governing_directive(policy, csp_effective_directive_for_inline_checks(type));

    /* A policy that carries no directive governing this check says NOTHING about it: `img-src 'none'` alone
       blocks no handler. §4.2.3's loop leaves its result at "Allowed". */
    if (!d) return true;
    return csp_source_list_allows_all_inline(d, type);
}

bool policy_allows(const PolicyContainer *p, PolicyScriptKind kind)
{
    size_t i;

    /* A document with no policy allows everything, which is the overwhelmingly common case and is what "no
       Content-Security-Policy header" means. */
    if (!p)
        return true;
    /* CSP §2.2: the policies in a list are enforced INDEPENDENTLY, so content runs only if EVERY one permits
       it — the opposite quantifier from core/html/trusted_types.c's question over the same list, and for the
       same reason: a second policy can only narrow. */
    for (i = 0; i < p->csp.n_policies; i++) {
        const CspPolicy *pol = &p->csp.policies[i];
        bool allowed = kind == POLICY_EVAL ? policy_allows_eval(pol)
                                           : policy_allows_inline(pol, inline_type_of(kind));
        if (!allowed)
            return false;
    }
    return true;
}

/* §6.7.2.1 "does request violate policy?" for ONE policy, over the FETCH DIRECTIVES — which is every directive
 * whose pre-request check can answer "Blocked".
 *
 * WHY THE SPEC'S LOOP OVER EVERY DIRECTIVE COLLAPSES TO ONE LOOKUP, and it is an identity rather than a
 * shortcut. §6.7.2.1 runs each directive's pre-request check; the fourteen directives that HAVE one (§6.1.1-
 * §6.1.15's fetch directives, plus §6.2.2's worker-src) each open with the SAME two lines — take §6.8.1's
 * effective directive name for the request, and return "Allowed" immediately unless §6.8.4 says THIS directive
 * is the one that executes for that name. §6.8.4 answers Yes for at most one directive of a policy, and
 * csp_policy_governing_directive is that walk. Every other directive of a policy — base-uri, form-action,
 * frame-ancestors, sandbox, webrtc, report-to, and the two *-attr forms — defines no pre-request check at all,
 * so §6.7.2.1 leaves `violates` untouched for them.
 *
 * WHAT IS NOT COLLAPSED, and crashes instead of being approximated: §6.7.1.1's SCRIPT-DIRECTIVE pre-request
 * check. script-src, script-src-elem, style-src and style-src-elem do not end in §6.7.2.5 directly — for a
 * SCRIPT-LIKE destination they first honour the request's nonce, its integrity metadata and 'strict-dynamic',
 * any of which allows a URL the source list refuses. Reaching one of them here would mean a request whose
 * destination is script-like, and this engine makes none: `fetch()` and XMLHttpRequest both carry §6.8.1's
 * empty destination. The assert names the three arms rather than the file. */
static bool policy_blocks_request(const CspPolicy *policy, const UrlRecord *url, const char *effective,
                                  const Origin *self_origin, int redirect_count)
{
    const CspDirective *d = csp_policy_governing_directive(policy, effective);

    /* A policy carrying none of §6.8.3's fallback chain says NOTHING about this request — §6.8.4 answers No
       for every directive of it, so §6.7.2.1 returns "Does Not Violate" and §4.1.2 leaves its result
       "Allowed". `img-src 'none'` blocks no `fetch()`. */
    if (!d)
        return false;
    DCHECK(!csp_token_is(d->name, "script-src") && !csp_token_is(d->name, "script-src-elem") &&
           !csp_token_is(d->name, "style-src") && !csp_token_is(d->name, "style-src-elem"),
           "§6.7.1.1's SCRIPT-DIRECTIVE pre-request check governs this request and it is not implemented — "
           "before the source list is consulted it must return Allowed when §6.7.2.3 matches the request's "
           "cryptographic nonce metadata, when §6.7.2.4 matches its integrity metadata, and (unless the "
           "request is parser-inserted) when the list carries 'strict-dynamic'. Build those three against "
           "Fetch's request record; matching the source list alone reports a script a browser LOADS as blocked");
    if (csp_source_list_match_url(d, url, self_origin, redirect_count) == CSP_MATCHES)
        return false;
    /* §4.1.2 STEP 3.3.1 — "execute §5.5 Report a violation on the result of executing §2.4.2 Create a
       violation object for request, and policy" — WHICH THIS ENGINE DOES NOT PERFORM, and here is where that
       becomes visible rather than merely absent. A violation has exactly two observables: a
       `securitypolicyviolation` event at the Document, and a report POSTed to the endpoints a policy names.
       The second is DECLARED IN THE POLICY ITSELF, so it is a gap this function can see coming and refuse. */
    DCHECK(!csp_policy_directive(policy, "report-uri") && !csp_policy_directive(policy, "report-to"),
           "a request was BLOCKED by a policy that declares a reporting endpoint, and §4.1.2 step 3.3.1's "
           "report is not built — the page's server is owed a report it will never receive, and a test that "
           "waits for one waits forever. Build CSP §2.4.2's violation object and §5.5's report a violation, "
           "whose two observables are the `securitypolicyviolation` event fired at the Document and the POST "
           "to the endpoints named by `report-to`/`report-uri`");
    return true;
}

CspRequestVerdict policy_should_block_request(const PolicyContainer *p, const UrlRecord *url,
                                              const char *destination, int redirect_count)
{
    const char *effective;
    size_t i;

    /* §4.1.2 step 1 reads the request's policy container's CSP list, and a document with no container has no
       policies — the overwhelmingly common case, and §4.1.2's own "let result be Allowed" for it. */
    if (!p)
        return CSP_REQUEST_ALLOWED;
    effective = csp_effective_directive_for_request(destination);
    /* §6.8.1's "report" row: a violation report upload is governed by no fetch directive, so every policy's
       pre-request check returns Allowed and there is nothing to walk. */
    if (!effective)
        return CSP_REQUEST_ALLOWED;
    /* §4.1.2 step 3: for each policy. This build parses only ENFORCE policies (see csp_directive_list.h), so
       the step's "if policy's disposition is report, skip" is vacuous rather than skipped.
       THE QUANTIFIER IS THE SAME ONE `policy_allows` RUNS, from the other end: §4.1.2 sets result to Blocked
       if ANY policy is violated, which is "allowed only if EVERY policy permits it". */
    for (i = 0; i < p->csp.n_policies; i++)
        if (policy_blocks_request(&p->csp.policies[i], url, effective, p->csp.self_origin, redirect_count))
            return CSP_REQUEST_BLOCKED;
    return CSP_REQUEST_ALLOWED;
}
