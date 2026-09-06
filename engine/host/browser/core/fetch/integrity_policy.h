/* SUBRESOURCE INTEGRITY §3.8 "Integrity-Policy" — the policy a response's header states, and §3.8.2's
 * "Should request be blocked by Integrity Policy", which is Fetch §4.1 "Main fetch" step 7's FOURTH disjunct.
 * See integrity_policy.c.
 *
 * EVERY NUMBER AND QUOTATION THIS COMPONENT GIVES FOR THAT STANDARD IS COUNTED AND NEVER CHECKED.
 * engine/specindex holds no row for Subresource Integrity, so the citation auditor resolves nothing here,
 * compares no quotation of it, and reports zero — which is SILENCE ABOUT THIS STANDARD and not a clean bill.
 * Treat the numbers as this file's claim. Its two WebAppSec siblings (csp, securecontexts) are indexed at the
 * same editor's-draft base, so the row is one fetch away and would make these checked like any other.
 *
 * IT IS NOT A STANDARD OF ITS OWN, WHICH IS THE FIRST THING A READER GETS WRONG. There is no "Integrity
 * Policy" document — both plausible homes answer a real 404 — and Fetch's own cross-reference data resolves
 * this disjunct into Subresource Integrity. Fetch's step-7 sentence also renders the section's title with the
 * word Policy DOUBLED ("should request be blocked by Integrity Policy Policy"), while the section is
 * "Should request be blocked by Integrity Policy": FETCH'S RENDERING IS REDUNDANT AND THE STANDARD IS NOT
 * WRONG, so a reader searching that standard for Fetch's exact phrase finds nothing and should not conclude
 * either document is in error.
 *
 * THE POLICY IS AN ITEM OF THE POLICY CONTAINER (HTML §7.1.7 "Policy containers"), which is why the parsed
 * value lives there and this file only builds and reads it — see core/frame/policy_container.h, whose struct
 * now carries the pair `integrity_policy_text` + `integrity_policy` exactly as it carries `csp_text` + `csp`,
 * for the same reason: the TEXT is what a container travels as across an instance, and the parsed form is
 * what every reader wants. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_INTEGRITY_POLICY_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_INTEGRITY_POLICY_H
#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"
#include "core/fetch/fetch.h"
#include "core/url/url.h"

/* §3.8's INTEGRITY POLICY struct, reduced to the two of its three lists that DECIDE anything here.
 *
 * §3.8 gives it `sources`, `blocked destinations` and `endpoints`, and closes each domain by hand: "A source
 * is a string. The only possible value for it is `inline`." and "A destination is a destination type. The
 * possible values for it are `script` and `style`." Two closed domains of one and two members are BOOLEANS,
 * not lists — a list here would be a growable structure whose only legal contents the standard enumerates,
 * which is a second spelling of the enumeration and a free-list obligation for nothing.
 *
 * `endpoints` IS ABSENT AND IT IS A NAMED RESIDUAL RATHER THAN A THIRD BOOLEAN. WHAT IS NOT COVERED: the
 * reporting endpoints a violation would be delivered to. WHAT THE NEXT DIFF BUILDS: §3.8.3 "Report
 * violations", over the Reporting standard — which HAS a committed corpus row, so its citation will be
 * checked rather than counted. HOW ITS ABSENCE SHOWS: a page that registers a ReportingObserver for
 * `integrity-violation` is delivered nothing, on a document whose script this component correctly refused.
 * The BLOCK answer is unaffected: §3.8.2 computes `block` from the enforcing policy alone. */
/* NAMED TAG, so that core/frame/policy_container.h can FORWARD-DECLARE it for its accessor rather than
   including this file: fetch.h already includes policy_container.h (§2.2.5's metadata rides the request) and
   this file includes fetch.h for the MODE, so an include the other way is a cycle. The container's own struct
   holds one BY VALUE and its .c includes this header for that. */
typedef struct IntegrityPolicy {
    bool sources_inline;   /* §3.8's `sources` contains "inline" */
    bool blocks_script;    /* §3.8's `blocked destinations` contains "script" */
    bool blocks_style;     /* §3.8's `blocked destinations` contains "style" */
} IntegrityPolicy;

/* §3.8's "a new integrity policy" — every list empty, which §3.8.2's step 7 reads as a positive statement
   ("If both policy and reportPolicy are empty integrity policys, return Allowed") and not as an absence. */
IntegrityPolicy integrity_policy_new(void);

/* §3.8.2's "empty integrity policy" test over one of them. */
bool integrity_policy_is_empty(const IntegrityPolicy *p);

/* §3.8's "processing an integrity policy", over the header value a response stated. The standard takes a
   header LIST and a header NAME and runs Fetch §2.2.2's get-a-structured-field-value with type "dictionary";
   this takes the VALUE because that is the form the item travels in — a policy container carries its items as
   text so that a clone across an instance carries them too, and core/fetch/structured_fields.h parses a
   dictionary out of one string exactly as it does out of a header list.
   `text` may be NULL or empty, which is a response that stated no such header: the answer is then a new
   integrity policy, which is also what §3.8's own steps produce for a value that does not parse. */
IntegrityPolicy integrity_policy_parse(const char *text, size_t len);

/* §3.8.2's two answers, NAMED, for the reason core/frame/policy_container.h names CSP's two and
   core/fetch/port_blocking.h names its own: the caller is a disjunction of blocking checks in ONE `if`, and a
   bool among them read the wrong way round is a request silently made or silently refused. */
typedef enum {
    INTEGRITY_POLICY_ALLOWED = 0,
    INTEGRITY_POLICY_BLOCKED = 1,
} IntegrityPolicyVerdict;

/* §3.8.2 "Should request be blocked by Integrity Policy", over the request fields its steps read.
 *
 * `policy` is step 5's "policyContainer's integrity policy". Step 6's REPORT-ONLY policy is not passed
 * because this engine does not store one — see policy_container.h for why that is a field with no reader
 * rather than an omission — so steps 6, 13 and 14 are the residual named there and `block` is computed from
 * the enforcing policy alone, which is what step 15 returns.
 * `url` is the request's current URL, PARSED, for step 4's "request's url is local" (Fetch §2.1, whose
 * predicate core/url/url.h owns).
 * `destination` is Fetch §2.2.5's destination string, for step 12's membership test.
 * `integrity`/`integrity_len` are Fetch §2.2.5's integrity metadata, for step 2's parse.
 * `mode` is Fetch §2.2.5's mode, for step 3 — and step 3 is a CONJUNCTION of that parse being non-empty with
 * the mode being `cors` or `same-origin`, which is why the mode had to arrive before this could be written at
 * all: neither half of it can be assumed. `ctx` is the request's client, for steps 8 and 9. */
IntegrityPolicyVerdict integrity_policy_should_block_request(JSContext *ctx, const IntegrityPolicy *policy,
                                                            const UrlRecord *url, const char *destination,
                                                            const char *integrity, size_t integrity_len,
                                                            FetchMode mode);

#endif
