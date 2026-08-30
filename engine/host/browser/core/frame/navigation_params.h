/* NAVIGATION PARAMS — HTML §7.4.6, reduced to WHAT A RESPONSE'S HEADER LIST DECIDES ABOUT THE DOCUMENT IT
 * CREATES. This is the route: a response's headers arrive here, and what leaves is the set of facts §7.5.1's
 * create-and-initialize-a-Document reads off navigationParams.
 *
 * WHY THERE IS A STRUCT BETWEEN THE HEADERS AND THE DOCUMENT, AND WHY IT IS THE SPEC'S OWN. CLAUDE.md's
 * scheduler rule — an operation that becomes a work item takes its inputs WITH it, and never reads them back
 * off the object it acts on — is the whole reason §7.4.6 and §7.5.1 are two algorithms in the standard rather
 * than one. The response is DECIDED at fetch time and the Document is created LATER, so every fact the
 * response states about the Document has to travel as a value. §7.4 step 14's double load is what happens
 * when it does not. A Document therefore never inspects a response, and nothing here holds one: the header
 * list is read once, in one place, and what comes out is a value that parks, clones and crosses an instance
 * the way an integer does.
 *
 * WHOSE HEADERS THESE ARE IS THE CALLER'S ANSWER AND NEVER THIS FILE'S. §7.1.7's determine-navigation-params-
 * policy-container picks between the history entry's, the initiator's, the parent's and the RESPONSE's
 * container, and only the algorithm that is running knows which — a navigation clones the INITIATOR's, a
 * create clones the CREATOR's, and `about:srcdoc` takes the PARENT's. This function answers exactly one of
 * those cases, the one where there IS a response, and a caller with no response must not call it: an empty
 * header list here would answer "no policy at all" for a Document whose policy is somebody's clone.
 *
 * THE FINAL SANDBOXING FLAG SET IS A UNION AND BOTH HALVES ARRIVE FROM DIFFERENT PLACES. §7.4.5: "let
 * finalSandboxFlags be the union of targetSnapshotParams's sandboxing flags and policyContainer's CSP list's
 * CSP-derived sandboxing flags". The first half is the NAVIGABLE's — an `<iframe sandbox>` attribute read in
 * the navigating flow's own delta, plus the embedder document's own set — and cannot be recovered from a
 * response; the second half is this response's. So the caller states the first and this computes the second,
 * which is also why §7.3.2.1's create-a-new-browsing-context-and-document does not come through here at all: it
 * gives the initial about:blank the creation flags ALONE, with no CSP-derived half, and unioning one in would
 * re-sandbox exactly the popup `allow-popups-to-escape-sandbox` exists to free. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATION_PARAMS_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATION_PARAMS_H
#include <stdbool.h>

#include "core/fetch/headers.h"
#include "core/frame/embedder_policy.h"
#include "core/frame/opener_policy.h"
#include "core/frame/sandboxing.h"

typedef struct {
    /* §7.1.7 step 3's CSP LIST, SERIALIZED — CSP §2.2's comma-delimited form, which is how the header itself
       delivers several policies and how core/frame/policy_container.c parses them apart again. NULL when the
       response carried no `Content-Security-Policy`. Owned. */
    char        *csp;
    /* PERMISSIONS POLICY §9.1's TWO HEADER FIELD VALUES, as Fetch §2.2.2 step 2's get returned them — the
       enforced `Permissions-Policy` and the report-only `Permissions-Policy-Report-Only`. NULL for a response
       that carried neither, which is §9.1 step 3's empty ordered map and not a hole. Owned.
       THEY ARE THE RESPONSE AS FAR AS §9.1 CAN SEE IT, and they travel rather than being re-read because the
       algorithm that consumes them is HTML §7.5.1's "creating a permissions policy from a response", which
       needs the ORIGIN and therefore cannot run until the Document is created — by which time this engine has
       freed the header list on purpose. BOTH names are carried because §10.1 inserts a SECOND §9.6 call with
       report-only True beside §7.5.1's, so one response feeds two policies. */
    char        *permissions_policy;
    char        *permissions_policy_report_only;
    /* §7.4.5's FINAL SANDBOXING FLAG SET — the union described above. */
    SandboxFlags sandbox_flags;
    /* §7.1.7's EMBEDDER POLICY, the container item obtained from this response (§7.1.4). */
    EmbedderPolicy embedder;
    /* §7.5.1's CROSS-ORIGIN OPENER POLICY row — NOT a container item; see core/frame/opener_policy.h. */
    OpenerPolicy   opener;
    /* §7.5.1's `requestsOAC`: the `Origin-Agent-Cluster` response header is the structured-field boolean TRUE,
       and the reserved environment is a secure context. It is an input to §8.1.2.2's obtain-a-similar-origin-
       window-agent and to nothing else. */
    bool         requests_oac;
} NavigationParams;

/* Read a response's HEADER LIST into the facts §7.5.1 creates a Document from.
   `secure_context` is HTML §8.1.3.5's answer for the RESERVED ENVIRONMENT — §7.1.3's and §7.1.4's obtains
   both return the default policy for a non-secure one, and §7.5.1 clears `requestsOAC` for it — asked over
   the environment's TOP-LEVEL CREATION URL, because this runs before the realm whose environment it is
   exists. `out` is filled and is the caller's to navigation_params_free. */
void navigation_params_from_response(NavigationParams *out, const HeaderList *headers,
                                     SandboxFlags target_sandbox_flags, bool secure_context);
void navigation_params_free(NavigationParams *p);

#endif
