/* THE POLICY CONTAINER — HTML §7.1.7 "Policy containers", over CSP's own policy/directive model.
 *
 * WHY IT EXISTS, AND WHY IT IS THE ANSWER TO "how does an about:blank child get a policy". A navigable created
 * with no URL — `window.open()` with no argument, `<iframe>` with no src — gets its initial `about:blank`
 * Document SYNCHRONOUSLY, and that Document has no response to take a policy from. HTML's answer is not a
 * special case: every Document has a POLICY CONTAINER, and §7.3.2.1 "Creating browsing contexts" says that when
 * creator is non-null, "set document's policy container to a clone of creator's policy container". So the child
 * inherits the parent's CSP by the ordinary rule, not by an inheritance rule written for CSP.
 *
 * AND WHICH CONTAINER A DOCUMENT IS CREATED WITH IS §7.1.7's OWN ALGORITHM, WHICH LIVES HERE ONCE. Two program
 * entries create a Document from a host's statement of a response and a creator, and a rule spelled at both is
 * two rules: determine-navigation-params-policy-container is one function at the bottom of this file, and its
 * predicate is the standard's `responseURL is local` rather than the approximation "the response carried no
 * policy" — which puts every third-party frame of a CSP-bearing page under its embedder's policy.
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
 * the two seams the clone crosses — window_proxy.c's `creator_csp`, for a lazily-materialized about:blank
 * child, and navigable.c's `navigable.create` notice, for a peer instance. BOTH NOW CARRY THE WHOLE CSP
 * LIST (its text AND CSP §2.2's self-origin) and neither carries anything else, so what is still owed is
 * the same step in a smaller form: a serialization of the CONTAINER in place of the two CSP fields, after
 * which this item has somewhere to sit at both ends.
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
 * core/frame/csp_directive_list.h (§2.2/§2.3) and core/frame/csp_source_list.h (§2.3.1/§6.7.3/§6.7.2), and
 * what is left here is HTML's container and the questions asked OF a container: §4.2.3's about inline content
 * of a type, §4.4.1's about string compilation, and CSP §4.1.2's about a URL — Fetch's main fetch step 7. All
 * three are here rather than in files of their own because they are the SAME walk over the same list with a
 * different question per policy, and because §2.2's "a second policy can only narrow" is then stated once. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/frame/csp_directive_list.h"
#include "core/frame/csp_source_list.h"
#include "core/frame/policy_container.h"
#include "core/frame/sandboxing.h"
/* NAMED RATHER THAN INHERITED THROUGH A NEIGHBOUR: §7.1.7's determine step reads Fetch §2.1's local-scheme
   answer and HTML §2.4.1's about:srcdoc match off a PARSED record, so this unit is a direct user of the URL
   parser and says so. */
#include "core/url/url.h"

struct PolicyContainer {
    /* The serialized CSP list, comma-delimited, or NULL for none (OWNED). It is kept because it is what the
       container TRAVELS as — a clone across an instance or a session is this string, re-parsed — and because a
       report is worth nothing without the policy text it names. */
    char    *csp_text;
    /* §2.2's CSP LIST, parsed over `csp_text`. Every name and value token is a SLICE of it, so the two fields
       are one value with one lifetime: free them together and never reallocate the text under the list. */
    CspList  csp;
    char    *referrer_policy;    /* §7.1.7's referrer policy (owned) */
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
    /* §7.1.7's "clone a policy container", and §7.3.2.1's answer to where an about:blank child's policy comes
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

SerializedCspList policy_container_determine_navigation_params(const char *response_url,
                                                               const char *response_csp,
                                                               const char *response_url_origin,
                                                               const char *inherited_csp,
                                                               const char *inherited_self_origin)
{
    SerializedCspList out;
    UrlRecord rec;
    bool parsed, local, srcdoc;
    /* THE SELF-ORIGIN IS WHAT SAYS THERE IS A CONTAINER TO INHERIT, NOT THE POLICY TEXT. §2.2 gives every CSP
       list a self-origin, so a creator that holds no policies still states one — and this build merges CSP
       §3.3's `<meta>` policies into the SAME list under the SAME self-origin, so a `data:` child with a `<meta>`
       CSP resolves `'self'` against its creator's origin when there was a creator and against its own OPAQUE
       origin when there was not. Reading the presence off `inherited_csp` collapses those two into one. */
    bool has_inherited = inherited_self_origin != NULL && *inherited_self_origin;

    DCHECK(response_url != NULL && *response_url,
           "§7.1.7's determine-navigation-params-policy-container was asked without a responseURL — its third "
           "step turns on whether that URL is LOCAL (Fetch §2.1), so without it there is no algorithm to run");
    DCHECK(response_url_origin != NULL && *response_url_origin,
           "a Document was created with no origin for its own response's CSP list — §2.2.2 makes that origin "
           "the list's self-origin, so without it a response-delivered `'self'` matches nothing this document "
           "could ever load");
    /* THE TWO HALVES OF ONE STRUCT ARRIVE TOGETHER OR THE STRUCT DID NOT ARRIVE. A policy with no self-origin
       beside it is the one shape §2.2 forbids, and it is exactly what the `navigable.create` notice carried
       before the self-origin had a field on it — so this is the assert that the seam was repaired, stated at
       the one place both entries reach rather than as a hole a `?:` fills with this document's own origin. */
    DCHECK(!(inherited_csp != NULL && *inherited_csp) || has_inherited,
           "a Document was created with an INHERITED CSP list carrying no CSP §2.2 self-origin — the trusted "
           "zone relays both halves of §7.1.7's clone off the `navigable.create` notice, so a policy without "
           "one is a relay that stopped writing the field, and `'self'` would resolve against this document's "
           "own address instead of the creator's origin");

    url_record_init(&rec);
    parsed = url_parse(&rec, response_url, strlen(response_url), NULL);
    /* A `CHECK` AND NOT A `DCHECK`: every caller of this has already parsed this same address to derive the
       Document's principal, so a failure here is not an unbuilt capability — it is the two parses disagreeing,
       and continuing would judge the Document under whichever container the `else` arm happens to name. */
    CHECK(parsed, "policy container: the address a Document is being created at does not parse, so §7.1.7's "
                  "local-URL step has nothing to ask");
    local  = url_scheme_is_local(rec.scheme);
    /* §7.1.7's SECOND STEP IS SEPARATE FROM ITS THIRD and asserts rather than falls through: "If responseURL is
       about:srcdoc: Assert: parentPolicyContainer is not null. Return a clone of parentPolicyContainer." A
       srcdoc Document has no response and no creator-less spelling — it exists only inside a parent — so a
       srcdoc address arriving here with nothing to inherit is a caller that did not carry the parent's
       container, and the `else` arm below would silently judge it under an empty list of its own. */
    srcdoc = url_matches_about(&rec, "srcdoc", true);
    url_record_free(&rec);

    DCHECK(!srcdoc || has_inherited,
           "§7.1.7 step 2 reached an `about:srcdoc` Document with no parent policy container — the standard "
           "ASSERTS one is there, because a srcdoc Document is created by its parent and has no response of "
           "its own; the caller that named this address did not carry the container that goes with it");

    /* §7.1.7 STEP 3 — "If responseURL is local and initiatorPolicyContainer is not null, then return a clone of
       initiatorPolicyContainer" — which step 2's srcdoc case reaches the same answer through here, since the
       parent IS the creator this seam carries. Everything else is step 4's responsePolicyContainer, whose CSP
       list is §2.2.2's pair; a document with no response at all has an empty one, which is step 5's new
       container and needs no arm of its own. */
    if (local && has_inherited) {
        out.csp = inherited_csp;
        out.self_origin = inherited_self_origin;
    } else {
        out.csp = response_csp;
        out.self_origin = response_url_origin;
    }
    return out;
}

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
static bool policy_permits_compilation(const CspPolicy *policy)
{
    const CspDirective *d = csp_policy_directive(policy, "script-src");

    if (!d) d = csp_policy_directive(policy, "default-src");
    /* A policy with neither says nothing about compilation, which is not the same as forbidding it. */
    if (!d) return true;
    return csp_source_list_contains(d, "'unsafe-eval'");
}

/* §4.2.3's INNER LOOP FOR ONE POLICY, and the collapse of it is an identity rather than a shortcut — the same
   one policy_blocks_request makes below, read on the inline side. §4.2.3 runs every directive's INLINE CHECK
   in turn; the SEVEN directives that HAVE one — default-src (§6.1.3.3), script-src (§6.1.10.3),
   script-src-elem (§6.1.11.3), script-src-attr (§6.1.12.1), style-src (§6.1.13.3), style-src-elem (§6.1.14.3)
   and style-src-attr (§6.1.15.1) — each open with the SAME two lines: take §6.8.2's effective directive name
   for the type, and return "Allowed" unless §6.8.4 says THIS directive is the one that executes for that
   name. The six granular ones then end in the SAME §6.7.3.3, and default-src's delegates to whichever of them
   §6.8.2 named "using this directive's value", which is the same thing done from the other side. §6.8.4
   answers Yes for at most one directive of a policy, and csp_policy_governing_directive is that walk. Every
   other directive defines no inline check, so §4.2.3 leaves its result untouched for them. */
static bool policy_permits_inline(const CspPolicy *policy, CspInlineType type,
                                  const lxb_dom_element_t *element, const char *source, size_t source_len)
{
    const CspDirective *d =
        csp_policy_governing_directive(policy, csp_effective_directive_for_inline_checks(type));

    /* A policy that carries no directive governing this check says NOTHING about it: `img-src 'none'` alone
       blocks no handler. §4.2.3's loop leaves its result at "Allowed". */
    if (!d) return true;
    return csp_element_match_source_list(d, element, type, source, source_len) == CSP_MATCHES;
}

/* THE ONE WALK, ASKED ONE OF TWO QUESTIONS. §2.2: the policies in a list are enforced INDEPENDENTLY, so
   content runs only if EVERY one permits it — the opposite quantifier from core/html/trusted_types.c's
   question over the same list, and for the same reason: a second policy can only narrow. A document with no
   policy allows everything, which is the overwhelmingly common case and is what "no Content-Security-Policy
   header" means. */
bool policy_allows_inline(const PolicyContainer *p, CspInlineType type, const lxb_dom_element_t *element,
                          const char *source, size_t source_len)
{
    size_t i;

    if (!p) return true;
    for (i = 0; i < p->csp.n_policies; i++)
        if (!policy_permits_inline(&p->csp.policies[i], type, element, source, source_len)) return false;
    return true;
}

bool policy_allows_string_compilation(const PolicyContainer *p)
{
    size_t i;

    if (!p) return true;
    for (i = 0; i < p->csp.n_policies; i++)
        if (!policy_permits_compilation(&p->csp.policies[i])) return false;
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
       THE QUANTIFIER IS THE SAME ONE `policy_allows_inline` RUNS, from the other end: §4.1.2 sets result to Blocked
       if ANY policy is violated, which is "allowed only if EVERY policy permits it". */
    for (i = 0; i < p->csp.n_policies; i++)
        if (policy_blocks_request(&p->csp.policies[i], url, effective, p->csp.self_origin, redirect_count))
            return CSP_REQUEST_BLOCKED;
    return CSP_REQUEST_ALLOWED;
}
