/* CROSS-ORIGIN OPENER POLICIES — HTML §7.1.3.
 *
 * IT IS NOT A POLICY-CONTAINER ITEM, AND THAT IS THE FIRST THING TO GET RIGHT ABOUT IT. §7.1.7's container
 * holds a CSP list, an embedder policy, a referrer policy and two integrity policies; the opener policy is a
 * row of its own in §7.5.1's create-and-initialize-a-Document table, beside "policy container" and "active
 * sandboxing flag set", which are three separate items of one creation. A design that folded it into the
 * container would inherit it through §7.4's clone-a-policy-container, and an inherited opener policy is
 * exactly the thing §7.1.3 exists to make impossible: it is a fact about the RESPONSE that created a
 * top-level Document, and about nothing else.
 *
 * ITS ONE READER IS §7.1.3.2, AND THAT IS THE SUBPROBLEM AFTER THIS ONE. An opener policy decides whether a
 * navigation SWAPS browsing context groups, and a swap is the one step in the whole standard that sets a
 * group's cross-origin isolation mode to anything but `none` ("if navigationCOOP's value is
 * `same-origin-plus-COEP`, then set newBrowsingContext's group's cross-origin isolation mode to either
 * `logical` or `concrete`"). Everything downstream of that — `window.crossOriginIsolated`, HR-TIME §4's 5µs
 * clock resolution, whether `document.domain` does anything — is decided by the mode and not by this value.
 * So this file COMPUTES the policy and holds no state: the Document row that would keep it, and the group
 * switch that would read it, are the next thing to build, and core/frame/navigation_params.c crashes by name
 * at the exact response that would need them.
 *
 * `same-origin-plus-COEP` IS COMPUTED, NEVER SENT. §7.1.3: it "cannot be directly set via the
 * `Cross-Origin-Opener-Policy` header, but results from a combination of setting both
 * `Cross-Origin-Opener-Policy: same-origin` and a `Cross-Origin-Embedder-Policy` header whose value is
 * compatible with cross-origin isolation together". That is why obtaining an opener policy obtains an
 * EMBEDDER policy on the way — the two headers are one decision, and reading either alone gets it wrong. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_OPENER_POLICY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_OPENER_POLICY_H
#include <stdbool.h>

#include "core/fetch/headers.h"

/* §7.1.3's OPENER POLICY VALUES, in the standard's own order. */
typedef enum {
    OPENER_POLICY_UNSAFE_NONE = 0,          /* §7.1.3's initial value */
    OPENER_POLICY_SAME_ORIGIN_ALLOW_POPUPS,
    OPENER_POLICY_SAME_ORIGIN,
    OPENER_POLICY_SAME_ORIGIN_PLUS_COEP,    /* computed from COOP `same-origin` + a COI-compatible COEP */
    OPENER_POLICY_NOOPENER_ALLOW_POPUPS,
} OpenerPolicyValue;

/* §7.1.3's OPENER POLICY struct, all four items. Both endpoints are "a string or NULL, initially null" —
   which is a different absence from the embedder policy's initially-EMPTY-STRING endpoints, and they are
   spelled differently here because the standard spells them differently. Owned. */
typedef struct {
    OpenerPolicyValue value;
    char             *reporting_endpoint;
    OpenerPolicyValue report_only_value;
    char             *report_only_reporting_endpoint;
} OpenerPolicy;

void opener_policy_init(OpenerPolicy *p);
void opener_policy_free(OpenerPolicy *p);

/* §7.1.3's "obtain an opener policy given a response `response` and an environment `reservedEnvironment`",
 * over the response's HEADER LIST — which, with the environment's secure-context answer, is the whole of what
 * the algorithm reads.
 *
 * `secure_context` is step 2's "if reservedEnvironment is a NON-SECURE CONTEXT, then return policy", the same
 * question §7.1.4's obtain asks and answered from the same place (core/frame/secure_context.h over the
 * environment's top-level creation URL). `out` is filled from step 1's "let policy be a NEW opener policy"
 * onward and is the caller's to free. */
void opener_policy_obtain(OpenerPolicy *out, const HeaderList *headers, bool secure_context);

#endif
