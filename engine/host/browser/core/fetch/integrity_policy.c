/* SUBRESOURCE INTEGRITY §3.8 and §3.8.2 — see integrity_policy.h, including why every number here is
   counted and never checked. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/fetch/integrity_policy.h"
#include "core/fetch/structured_fields.h"
#include "core/fetch/subresource_integrity.h"
#include "core/realm.h"
#include "core/url/url.h"

IntegrityPolicy integrity_policy_new(void)
{
    IntegrityPolicy p;

    memset(&p, 0, sizeof p);
    return p;
}

bool integrity_policy_is_empty(const IntegrityPolicy *p)
{
    DCHECK(p != NULL, "§3.8.2's empty-integrity-policy test was asked about no policy — every policy container "
                      "has one (a new integrity policy when no header stated it), so a NULL here is a "
                      "container built somewhere that does not go through policy_container_new");
    return !p->sources_inline && !p->blocks_script && !p->blocks_style;
}

/* §3.8's inner-list membership test: does this member's value contain the token `want`?
   THE MEMBER MAY BE EITHER ARM AND ONLY ONE OF THEM CAN CONTAIN ANYTHING. §3.8 says "every member-value being
   an inner list of tokens", so `blocked-destinations=(script)` is the inner-list arm; a sender that wrote
   `blocked-destinations=script` produced the ITEM arm, which is a bare token and not a list, and §3.8's own
   steps ask whether the VALUE CONTAINS the token — a question a bare item answers `false` for every token
   including itself. That is the standard read literally rather than a leniency this file invents: a receiver
   that treated the item arm as a one-member list would be accepting a field the sender did not write. */
static bool member_contains_token(const SfMember *m, const char *want)
{
    int i;

    if (!m->inner_list) return false;
    for (i = 0; i < m->n_items; i++) {
        const SfBareItem *b = &m->items[i].item;
        if (b->kind == SF_TOKEN && b->text && !strcmp(b->text, want)) return true;
    }
    return false;
}

static const SfMember *dict_get(const SfDictionary *d, const char *key)
{
    int i;

    for (i = 0; i < d->n_members; i++)
        if (d->members[i].key && !strcmp(d->members[i].key, key)) return &d->members[i].value;
    return NULL;
}

IntegrityPolicy integrity_policy_parse(const char *text, size_t len)
{
    /* §3.8 step 1: "Let integrityPolicy be a new integrity policy". */
    IntegrityPolicy p = integrity_policy_new();
    SfDictionary d;
    const SfMember *m;

    /* A RESPONSE THAT STATED NO SUCH HEADER, which §3.8.1 spells as its `If headers contains …` guard rather
       than as an input to this algorithm. The answer is the new policy above — never a parse of nothing. */
    if (!text || !len) return p;

    /* §3.8 step 2: "Let dictionary be the result of getting a structured field value from headers given
       headerName and `dictionary`". A value that does not parse leaves `p` as the new policy, which is the
       same answer Fetch §2.2.2's get gives for a malformed field and is why there is no error arm here: THIS
       IS FOREIGN INPUT. The bytes are a stranger's server's, so a malformed one is INPUT and not a violated
       invariant, and the answer is a refusal that yields the record's declared absence rather than an assert
       (CLAUDE.md: assert only what this codebase computed). */
    if (!sf_parse_dictionary(text, len, &d)) return p;

    /* §3.8 step 3: "If dictionary["sources"] does not exist or if its value contains "inline", append
       "inline" to integrityPolicy's sources." THE ABSENT KEY IS THE PERMISSIVE ARM and that is the standard's
       own order — a header naming only `blocked-destinations` still has `inline` sources, which is what makes
       the ordinary one-directive header do anything at all. */
    m = dict_get(&d, "sources");
    if (!m || member_contains_token(m, "inline")) p.sources_inline = true;

    /* §3.8 steps 4.1 and 4.2, under step 4's "If dictionary["blocked-destinations"] exists". §3.8 closes this
       domain by hand — "A destination is a destination type. The possible values for it are "script" and
       "style"" — so a token outside those two is not a destination and is skipped by having no arm, exactly
       as the standard skips it by naming only two. */
    m = dict_get(&d, "blocked-destinations");
    if (m) {
        if (member_contains_token(m, "script")) p.blocks_script = true;
        if (member_contains_token(m, "style"))  p.blocks_style  = true;
    }
    /* §3.8 step 5 is `endpoints` and is the named residual in integrity_policy.h: its only reader is §3.8.3
       "Report violations", which needs Reporting. Nothing is stored, so nothing is written that no one reads. */
    sf_dictionary_free(&d);
    return p;   /* §3.8 step 6 */
}

IntegrityPolicyVerdict integrity_policy_should_block_request(JSContext *ctx, const IntegrityPolicy *policy,
                                                            const UrlRecord *url, const char *destination,
                                                            const char *integrity, size_t integrity_len,
                                                            FetchMode mode)
{
    SriMetadataParse it;
    SriHashExpression e;
    bool parsed_metadata_non_empty;

    /* §3.8.2 step 1 is "Let policyContainer be request's policy container" and the caller has already read
       the item off it — see integrity_policy.h for why the POLICY rather than the container arrives here. */
    DCHECK(policy != NULL,
           "§3.8.2 was asked about a request whose policy container states no integrity policy — every "
           "container has one (core/frame/policy_container.h), so a NULL is a container that did not come "
           "through policy_container_new rather than a document whose response carried no header");
    DCHECK(destination != NULL,
           "§3.8.2 was asked about a request stating no DESTINATION — Fetch §2.2.5 \"Requests\" gives every "
           "request one and step 12 tests this policy's blocked destinations for membership of it, so a "
           "request without one would be compared against a pointer nobody wrote");
    DCHECK(integrity != NULL,
           "§3.8.2 was asked about a request whose integrity metadata is a NULL rather than the EMPTY STRING "
           "— Fetch §2.2.5 makes the empty string that field's initial value, which step 2's parse reads as "
           "the empty set, so a NULL is a producer that dropped the field and not a request without one");
    /* THE MODE IS ASSERTED HERE AS WELL AS AT THE TWO SEAMS, and this is the site where getting it wrong is
       ACTUALLY WRONG rather than merely unstated: step 3 is a CONJUNCTION of the parse being non-empty with
       the mode being `cors` or `same-origin`, so an unplaced zero silently takes the arm that does NOT
       early-allow and refuses a `<script src integrity crossorigin>` a browser loads. Nothing downstream
       would crash; the element would fire `error`, and this engine would explore a path the real page never
       takes. That is why the domain has a zero and why it is refused rather than defaulted. */
    DCHECK(mode != FETCH_MODE_UNPLACED,
           "§3.8.2 step 3 was asked of a request stating NO MODE — Fetch §2.2.5 \"Requests\" gives every "
           "request one, and this step's early-allow arm is a conjunction with it, so an unstated mode does "
           "not fail open here: it silently REFUSES a request carrying integrity metadata that a browser "
           "loads. State the mode the algorithm creating this request names (HTML §2.5.1's create a "
           "potential-CORS request answers it from the element's `crossorigin` state — "
           "core/html/cors_settings_attribute.h)");

    /* STEP 2: "Let parsedMetadata be the result of calling parse metadata with request's integrity metadata."
       Only its EMPTINESS is read, and core/fetch/subresource_integrity.h's cursor answers that in one turn —
       its own header says the set-versus-stream difference is unobservable to exactly this question. */
    sri_parse_metadata(&it, integrity, integrity_len);
    parsed_metadata_non_empty = sri_parse_metadata_next(&it, &e);

    /* STEP 3: "If parsedMetadata is not the empty set and request's mode is either `cors` or `same-origin`,
       return Allowed." */
    if (parsed_metadata_non_empty && (mode == FETCH_MODE_CORS || mode == FETCH_MODE_SAME_ORIGIN))
        return INTEGRITY_POLICY_ALLOWED;

    /* STEP 4: "If request's url is local, return Allowed." Fetch §2.1's predicate, over the PARSED scheme —
       core/url/url.h says why a `strncmp` over the address answers `about` and misses `data:` and `blob:`. */
    if (url && url->scheme && url_scheme_is_local(url->scheme)) return INTEGRITY_POLICY_ALLOWED;

    /* STEPS 5 THROUGH 7: the enforcing policy is the argument; the REPORT-ONLY one this engine does not store
       is an empty integrity policy at every document, so step 7's "both … are empty" reduces to this one. */
    if (integrity_policy_is_empty(policy)) return INTEGRITY_POLICY_ALLOWED;

    /* STEPS 8 AND 9: "Let global be request's client's global object" … "If global is not a Window nor a
       WorkerGlobalScope, return Allowed." The realm IS the environment in this engine (core/realm.h), and the
       one global kind that is neither is a WorkletGlobalScope. */
    if (realm_global_is_worklet(ctx)) return INTEGRITY_POLICY_ALLOWED;

    /* STEPS 10 THROUGH 13: `block` is set when this policy's sources contain `inline` AND its blocked
       destinations contain the request's destination. `reportBlock` and step 14's report are the residual. */
    if (policy->sources_inline &&
        ((policy->blocks_script && !strcmp(destination, "script")) ||
         (policy->blocks_style  && !strcmp(destination, "style"))))
        return INTEGRITY_POLICY_BLOCKED;   /* STEP 15 */
    return INTEGRITY_POLICY_ALLOWED;
}
