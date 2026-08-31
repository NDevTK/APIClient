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
 * ITS ONE READER IS §7.1.3.2, AND IT IS IN THIS FILE. An opener policy decides whether a navigation SWAPS
 * browsing context groups, and a swap is the one step in the whole standard that sets a group's cross-origin
 * isolation mode to anything but `none` ("if navigationCOOP's value is `same-origin-plus-COEP`, then set
 * newBrowsingContext's group's cross-origin isolation mode to either `logical` or `concrete`"). Everything
 * downstream of that — `window.crossOriginIsolated`, HR-TIME §4's 5µs clock resolution, whether
 * `document.domain` does anything — is decided by the mode and not by this value. The mode is
 * core/frame/browsing_context_group.h's; the VALUE a navigable's active document holds is
 * core/frame/window_proxy.h's, which is where §7.5.1's Document row lives for the reason that header gives.
 * This file holds no state at all: it PARSES a response into a policy and DECIDES, from two policies and two
 * origins, whether a navigation crosses a group boundary.
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
#include "core/url/origin.h"

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

/* HTML §7.1.3.1 "The headers"'s "obtain an opener policy given a response `response` and an environment
 * `reservedEnvironment`", over the response's HEADER LIST — which, with the environment's secure-context
 * answer, is the whole of what the algorithm reads.
 *
 * `secure_context` is step 2's "if reservedEnvironment is a NON-SECURE CONTEXT, then return policy", the same
 * question §7.1.4's obtain asks and answered from the same place (core/frame/secure_context.h over the
 * environment's top-level creation URL). `out` is filled from step 1's "let policy be a NEW opener policy"
 * onward and is the caller's to free. */
void opener_policy_obtain(OpenerPolicy *out, const HeaderList *headers, bool secure_context);

/* §7.1.3's MATCH OPENER POLICY VALUES, verbatim:
     "1. If documentCOOP is `unsafe-none` and responseCOOP is `unsafe-none`, then return true.
      2. If documentCOOP is `unsafe-none` or responseCOOP is `unsafe-none`, then return false.
      3. If documentCOOP is responseCOOP and documentOrigin is same origin with responseOrigin, then return
         true.
      4. Return false."
   Step 1 before step 2 is the whole shape of it: two `unsafe-none`s match WITHOUT any origin comparison, which
   is why an ordinary cross-origin navigation between two pages that send no header does not swap groups. */
bool opener_policy_values_match(OpenerPolicyValue document_coop, const Origin *document_origin,
                                OpenerPolicyValue response_coop, const Origin *response_origin);

/* §7.1.3.2's CHECK IF POPUP COOP VALUES REQUIRE A BROWSING CONTEXT GROUP SWITCH:
     "1. If responseCOOPValue is `noopener-allow-popups`, then return true.
      2. If all of the following are true: activeDocumentCOOPValue's value is `same-origin-allow-popups` or
         `noopener-allow-popups`; and responseCOOPValue is `unsafe-none`, then return false.
      3. If the result of matching activeDocumentCOOPValue, activeDocumentNavigationOrigin, responseCOOPValue,
         and responseOrigin is true, then return false.
      4. Return true."
   Step 2 IS what `same-origin-allow-popups` is for and is the reason this variant exists at all: a page that
   sends it keeps its popups in its own group when they carry no policy of their own, where a plain
   `same-origin` page's popups would be severed. The standard writes "activeDocumentCOOPValue's value" in step
   2 although the parameter is already a VALUE; the item it names is the value itself. */
bool opener_policy_popup_switch_required(const Origin *response_origin,
                                         const Origin *active_document_navigation_origin,
                                         OpenerPolicyValue response_coop, OpenerPolicyValue active_coop);

/* §7.1.3.2's CHECK IF COOP VALUES REQUIRE A BROWSING CONTEXT GROUP SWITCH:
     "1. If isInitialAboutBlank is true, then return the result of checking if popup COOP values requires a
         browsing context group switch with responseOrigin, activeDocumentNavigationOrigin, responseCOOPValue,
         and activeDocumentCOOPValue.
      2. [Here we are dealing with a non-popup navigation.] If the result of matching activeDocumentCOOPValue,
         activeDocumentNavigationOrigin, responseCOOPValue, and responseOrigin is true, then return false.
      3. Return true."
   `is_initial_about_blank` is the navigable's active document's IS INITIAL about:blank — a navigable §7.4 has
   created and not yet navigated, which core/frame/window_proxy.h answers with `ever_navigated` from the
   navigable's own side. */
bool opener_policy_switch_required(bool is_initial_about_blank, const Origin *response_origin,
                                   const Origin *active_document_navigation_origin,
                                   OpenerPolicyValue response_coop, OpenerPolicyValue active_coop);

/* §7.1.3.2's THIRD CHECK — "check if enforcing report-only COOP would require a browsing context group
   switch" — IS DELIBERATELY ABSENT, and this sentence is here so the next reader does not write it again
   before its reader exists. Its answer is §7.1.3.2's enforcement-result item "would need a browsing context
   group switch due to report-only", whose only two consumers in the standard are §7.1.3.3's violation reports
   and setting "browsingContext's VIRTUAL BROWSING CONTEXT GROUP ID to a new unique identifier". This engine
   has neither, so the predicate would compute a value nothing asks for — and a spec-perfect algorithm with no
   caller is indistinguishable from an unexecuted one, which reads as done and is worse than absent. Write it
   in the same diff as the reporting that reads it; the report-only halves it needs are already parsed onto the
   policy (§7.1.3.1 is implemented whole, because truncating the standard's own parse is a different mistake). */

#endif
