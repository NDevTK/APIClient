/* CROSS-ORIGIN EMBEDDER POLICIES — HTML §7.1.4, and the half of cross-origin isolation a response states
 * about ITSELF.
 *
 * IT IS ONE OF §7.1.7'S FIVE POLICY-CONTAINER ITEMS, and that is the fact this file is written to serve. A
 * policy container holds "a CSP list, an embedder policy, a referrer policy, an integrity policy and a report
 * only integrity policy" — no sandboxing flag set and no opener policy, which are two SEPARATE rows of §7.5.1's
 * Document creation table — and §7.1.7's create-a-policy-container-from-a-fetch-response builds the embedder
 * policy item by "obtaining an embedder policy given response and environment". That is the algorithm below.
 *
 * WHY A THREE-VALUED ENUM AND NOT A BOOLEAN. §7.1.4 gives `unsafe-none`, `require-corp` and `credentialless`,
 * and TWO of them are "compatible with cross-origin isolation" while the three differ in what they do to a
 * cross-origin no-CORS fetch (`require-corp` demands CORP or CORS; `credentialless` omits credentials
 * instead). A boolean would answer the isolation question and lose the fetch question, which is the same
 * collapse core/frame/agent_cluster.c had to undo for the three-valued isolation MODE.
 *
 * THE PROCESSING MODEL FAILS OPEN AND THAT IS LOAD-BEARING. §7.1.4.1: a header that cannot be parsed as a
 * token — including the LIST that two identical headers combine into — leaves the value at `unsafe-none`. So
 * this file never rejects a response; it answers with the policy the standard says that response has, and the
 * only way a value becomes non-default is a well-formed token that IS compatible with cross-origin isolation.
 * That is why `obtain` has no failure return: every response has an embedder policy.
 *
 * THE REPORT-ONLY BRANCH WRITES `endpoint`, NOT `report only reporting endpoint`, AND THAT IS THE SPEC'S OWN
 * TEXT rather than a transcription slip here — §7.1.4's obtain reads, verbatim, "Set policy's report only
 * value to parsedItem[0]. If parsedItem[1]["report-to"] exists, then set policy's endpoint to
 * parsedItem[1]["report-to"]" in BOTH branches. Implementing what the sentence says rather than what it
 * evidently means is the discipline CLAUDE.md asks for — the spec is the source of truth and a divergence
 * invented here would be invisible the day Reporting is built. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_EMBEDDER_POLICY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_EMBEDDER_POLICY_H
#include <stdbool.h>

#include "core/fetch/headers.h"

/* §7.1.4's EMBEDDER POLICY VALUE — "one of three strings that controls the fetching of cross-origin resources
   without explicit permission from resource owners". */
typedef enum {
    EMBEDDER_POLICY_UNSAFE_NONE = 0,   /* §7.1.4's initial value, and what a fail-open parse leaves */
    EMBEDDER_POLICY_REQUIRE_CORP,
    EMBEDDER_POLICY_CREDENTIALLESS,
} EmbedderPolicyValue;

/* §7.1.4's EMBEDDER POLICY struct, all four items. The two endpoints are the empty string initially — §7.1.4
   says "a reporting endpoint STRING, initially the empty string", which is a different absence from the opener
   policy's null endpoint, so they are spelled differently here too. Owned. */
typedef struct {
    EmbedderPolicyValue value;
    char               *endpoint;
    EmbedderPolicyValue report_only_value;
    char               *report_only_endpoint;
} EmbedderPolicy;

/* §7.1.4's "a new embedder policy" — every field at its stated initial value. */
void embedder_policy_init(EmbedderPolicy *p);
void embedder_policy_free(EmbedderPolicy *p);

/* §7.1.4: "An embedder policy value is COMPATIBLE WITH CROSS-ORIGIN ISOLATION if it is `credentialless` or
   `require-corp`." Read by §7.1.3's obtain-an-opener-policy (which is how `same-origin` becomes
   `same-origin-plus-COEP`) and by §7.1.4.2's embedder policy checks. */
bool embedder_policy_compatible_with_cross_origin_isolation(EmbedderPolicyValue v);

/* §7.1.4's "obtain an embedder policy from a response `response` and an environment `environment`", over the
 * response's HEADER LIST — which is the whole of what the algorithm reads from the response.
 *
 * `secure_context` IS THE ENVIRONMENT, reduced to the one question step 2 asks of it: "if environment is a
 * NON-SECURE CONTEXT, then return policy". A non-secure page gets the default policy no matter what it sends,
 * which is why an embedder policy cannot be a pure function of a header list. HTML §8.1.3.5 answers it over
 * the environment's TOP-LEVEL CREATION URL (core/frame/secure_context.h), and the caller asks there because
 * this algorithm runs BEFORE the realm whose environment it is exists.
 *
 * `out` is written from step 1's "let policy be a NEW embedder policy" onward — it is filled, never merged
 * into, and the caller frees it. */
void embedder_policy_obtain(EmbedderPolicy *out, const HeaderList *headers, bool secure_context);

#endif
