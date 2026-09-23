/* CSP §2.4 "Violations", §5.2 "Obtain the blockedURI of a violation's resource", §5.4 "Strip URL for use in
 * reports" and §5.5 "Report a violation" — what a policy's REFUSAL is observable as.
 *
 * IT IS A SEPARATE COMPONENT FROM THE WALK THAT DECIDES, and that is the standard's own decomposition rather
 * than a preference: §4.1.2 step 3.3.1 reads "execute §5.5 Report a violation on the result of executing
 * §2.4.2 Create a violation object for request, and policy", which is a CALL out of the deciding algorithm
 * into two others. The split also puts each half where its inputs are. Deciding needs a POLICY and a URL and
 * nothing else — core/frame/policy_container.c answers it with no realm anywhere in sight, which is why
 * test_forced.c can exercise every source-list relation before a JSRuntime exists. Reporting needs a GLOBAL
 * OBJECT: §2.4.1 reads the global's url, its document's referrer and the running script's source position,
 * and §5.5 fires at "target's associated Document". One component holding both would make the matcher
 * untestable without a realm, for a step the matcher does not use.
 *
 * See csp_violation.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_CSP_VIOLATION_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_CSP_VIOLATION_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "core/frame/csp_directive_list.h"

/* CSP §2.4's RESOURCE — "either null, "inline", "eval", "wasm-eval", "trusted-types-policy",
   "trusted-types-sink", or a URL. It represents the resource which violated the policy."
   §2.4's own note rules out the null: "The value null for a violation's resource is only allowed while the
   violation is being populated. By the time the violation is reported … the violation's resource should be
   populated with a URL or one of the allowed strings." So this enum has no null member — a violation is
   created and reported in one act here, and there is no moment at which a caller holds a half-populated one.
   THE SPELLINGS ARE THE SPEC'S OWN and are what §5.2 returns verbatim for the string arms. */
typedef enum {
    CSP_RESOURCE_URL = 0,              /* the URL arm — §5.2 runs §5.4 over it */
    CSP_RESOURCE_INLINE,               /* "inline" */
    CSP_RESOURCE_EVAL,                 /* "eval" */
    CSP_RESOURCE_WASM_EVAL,            /* "wasm-eval" */
    CSP_RESOURCE_TRUSTED_TYPES_POLICY, /* "trusted-types-policy" */
    CSP_RESOURCE_TRUSTED_TYPES_SINK,   /* "trusted-types-sink" */
} CspResourceKind;

/* THE GLOBAL OBJECT §2.4.1 POPULATES A VIOLATION FROM AND §5.5 FIRES AT, CARRIED AS ONE VALUE.
 *
 * IT IS A STRUCT WITH A LATCH RATHER THAN A BARE `JSContext *`, AND THE LATCH IS THE WHOLE POINT. A bare
 * pointer has a zero value that every designated initializer and every memset produces for free, so a caller
 * that never thought about reporting would be silently exempted from it — which is the shape
 * core/frame/policy_container.h's own CspRequestMetadata was given two spellings to prevent, in this same
 * component and for this same reason. `stated` is false in a zero-filled struct and in nothing a constructor
 * below returns, so a reporter that was never built ABORTS at the first walk that is handed one rather than
 * quietly deciding a request and reporting nothing.
 *
 * THERE ARE EXACTLY TWO WAYS TO BUILD ONE and both are NAMED CLAIMS about the caller's algorithm. A caller
 * running §4.1.2 / §4.2.3 / §4.4.1 as a browser runs them has a global object and says so; a caller running
 * them for their ANSWER ALONE, outside any global object, says THAT, and the two read differently at the call
 * site. Forgetting is not one of the two. */
typedef struct {
    JSContext *ctx;    /* the realm whose global object is §2.4.1's `global`; NULL only from the second form */
    bool       stated; /* which of the two constructors built this — never true in a struct nobody built */
} CspReporter;

/* THE REALM §5.5 REPORTS INTO. `ctx` may not be NULL: a caller with no realm makes the OTHER claim. */
CspReporter csp_reporter(JSContext *ctx);
/* "THIS CALLER IS ASKING FOR THE DECISION AND NOT FOR ITS REPORTS" — §6.7.2.1's question asked outside any
   global object, which has no `target` for §5.5's fire to reach and no document for §2.4.1 to read a url off.
   It is spelled differently from `csp_reporter(NULL)` so that no caller reaches it by passing a null, and a
   caller that uses it NAMES the claim it is making — the same rule, in the same component, that
   csp_request_metadata_unstated exists for. test_forced.c's CSP fixtures are its callers: they run BEFORE
   any JSRuntime exists, deliberately, so that a source-list relation's failure is reported at the matcher's
   own row rather than inside whatever realm-owning algorithm first consults it. */
CspReporter csp_reporter_none(void);

/* CSP §4.1.2 step 3.3.1 — "execute §5.5 Report a violation on the result of executing §2.4.2 Create a
   violation object for request, and policy on request and policy", for ONE policy the walk has just found
   violated. §2.4.2 runs §6.8.1 over the request itself, so this takes the request's DESTINATION rather than
   an effective-directive name somebody else computed: two answers to that question could disagree, and the
   one §5.5 reports must be the one §6.8.1 gives.
   `request_url` is the request's URL SERIALIZED — §2.4.2 sets the violation's resource to "request's url and
   not its current url", and its own note says why: "the latter might contain information about redirect
   targets to which the page must not be given access". */
void csp_report_violation_for_request(CspReporter r, const CspPolicy *policy, const char *destination,
                                      const char *request_url);

/* CSP §4.2.3's / §4.4.1's / §6.3.1.1's "execute §5.5 Report a violation on violation", where the violation
   came from §2.4.1 "Create a violation object for global, policy, and directive" rather than from §2.4.2 —
   every violation this engine decides that is NOT about a request.
   `effective_directive` is §2.4.1's `directive` argument, already computed by the caller's own algorithm:
   §4.2.3 hands it §6.8.2's "get the effective directive for inline checks" over the inline type, §4.4.1 hands
   it the literal "script-src", and §6.3.1.1 hands it the literal "base-uri".
   `source` AND `source_len` ARE THE CONTENT, NOT THE SAMPLE. §4.2.3 and §4.4.1 both gate the sample on the
   directive's own value — "if directive's value contains the expression 'report-sample' then set violation's
   sample to the substring of source containing its first 40 characters" — so handing over a sample already
   cut would put that test at every caller, and the test needs the DIRECTIVE, which this component resolves
   from the policy and the effective directive it was given. A caller with no content at all (§6.3.1.1 has
   none) passes NULL and 0, which is §2.4's initial value for the field: "it is the empty string unless
   otherwise specified".
   `element` is §2.4's element and MAY be NULL — §4.2.4 runs the inline check "upon null" for a `javascript:`
   navigation, and §4.4.1 and §6.3.1.1 have no element at all. It decides §5.5 step 3.1's target and nothing
   else. */
void csp_report_violation_for_global(CspReporter r, const CspPolicy *policy, const char *effective_directive,
                                     CspResourceKind resource, const char *source, size_t source_len,
                                     const lxb_dom_element_t *element);

#endif
