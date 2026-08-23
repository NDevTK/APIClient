/* THE POLICY CONTAINER — HTML §7.2.6. See policy_container.c.
 *
 * IT CROSSES INSTANCES, and the initial about:blank is what forces that. A child created with no URL —
 * `window.open()`, an `<iframe>` with no src — gets its Document synchronously and has no response to take a
 * policy from; §7.4 says its container is a CLONE OF THE CREATOR'S. When the child is CROSS-ORIGIN it lives in
 * another instance, so that clone is a CROSS-INSTANCE operation: the creator's container is serialized to the
 * child's instance, and the requesting flow SUSPENDS across the boundary the same way it suspends at an await.
 * WHICH SIDE OF THAT A CHILD FALLS ON IS ITS ORIGIN'S ANSWER, NOT A COST DECISION. This said "one WASM instance
 * is one DOCUMENT regardless of origin, so … same-origin is not an exemption", and that premise is the model
 * SECURITY.md rejects: an instance is an `(browsing-context group, origin)` AGENT CLUSTER, a same-origin child
 * is a second REALM in the creator's own heap (navigable.c's child_in_this_agent), and its clone is therefore
 * an ordinary in-heap copy — not because a memcpy is cheaper but because there is no boundary between them to
 * cross. Splitting a same-origin pair to make one is the design this file must not encourage, because HTML's
 * similar-origin window agent is one heap and DOM adoption and cross-frame closures rely on it.
 *
 * WHICH IS WHY IT IS A VALUE, NOT A POINTER GRAPH. Everything here is a flat parse over one owned string, so a
 * container serializes to its `csp_text` and reconstitutes by parsing it again — the clone that crosses an
 * instance and the clone that crosses a session are then the same operation, and neither needs a live heap. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_POLICY_CONTAINER_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_POLICY_CONTAINER_H
#include <stdbool.h>
#include <stddef.h>

#include "core/frame/csp_directive_list.h"
#include "core/frame/csp_source_list.h"
#include "core/frame/sandboxing.h"

typedef struct PolicyContainer PolicyContainer;

/* From a document's own headers/meta. `csp_text` is a SERIALIZED CSP LIST — CSP §2.2, comma-delimited, which
   is how several policies travel in one header and how several `<meta>` elements compose. It is a list rather
   than a single policy because the policies are enforced INDEPENDENTLY: a resource must be allowed by every
   one of them, so a second policy can only ever narrow. `csp_text` and `referrer_policy` are copied; either
   may be NULL.
   `self_origin` IS THE SECOND HALF OF THE CSP LIST AND NOT AN EXTRA. CSP §2.2 makes a list a struct of
   policies AND a self-origin, and §2.2.2 states that origin from OUTSIDE the bytes — "self-origin is
   response's URL's origin" — so it cannot be recovered from `csp_text` at either end of a clone and has to be
   handed over with it. Every caller states it, because WHOSE origin it is belongs to the operation: a Document
   built from a response takes its own, and §7.4's clone takes the CREATOR's along with the text. BORROWED —
   an origin lives for the agent. */
PolicyContainer *policy_container_new(const char *csp_text, const Origin *self_origin,
                                      const char *referrer_policy);

/* §7.2.6's "clone a policy container" — §7.4 performs this for a navigable created with a creator, which is
   how an initial about:blank inherits its CSP. A DEEP copy: the child's policy is its own from the moment it
   exists, so a later navigation of the parent cannot reach back and change what the child may do. */
PolicyContainer *policy_container_clone(const PolicyContainer *src);
void policy_container_free(PolicyContainer *p);

/* THE CONTAINER EXPORTS ITS CSP TWICE, and the two are for two different things.
   The TEXT is what the container TRAVELS as and what a report has to quote — §7.4's clone across an instance
   or a session is this string, re-parsed. BORROWED; NULL for no policy. */
const char *policy_container_csp(const PolicyContainer *p);
/* The PARSED §2.2 list is what a component ASKS. Every directive question in the engine goes through it, so
   the parse happens once per container rather than once per sink — and so that there is exactly one reading of
   CSP's grammar in the tree (core/frame/csp_directive_list.h says why that matters). BORROWED, and NULL for a
   container that does not exist, which is the same answer as a list of zero policies and is the reason no
   caller needs a second branch for it. */
const CspList *policy_container_csp_list(const PolicyContainer *p);
/* §2.2's SELF-ORIGIN of that list — for the ONE caller that has to pass a container's own along rather than
   ask it a question: §7.4's create, which clones the CREATOR's container into a navigable it is making. NULL
   only for a container that does not exist. */
const Origin *policy_container_self_origin(const PolicyContainer *p);

/* WOULD THIS RUN? TWO QUESTIONS, AND THEY ARE TWO FUNCTIONS BECAUSE THEY ARE TWO ALGORITHMS.
 *
 * §S asks the first of a BREAKOUT: a firing breakout in the model is not an exploit until it survives the
 * page's actual policy — an inline `onerror` is dead under `script-src 'self'`, and the honest report is then
 * "sink REAL, CSP blocks: needs X" rather than a bare XSS. HTML's own components ask the SAME question of the
 * page's OWN content: §4.2.6's update a style block runs it upon a `<style>` element before it may create a
 * sheet, and the two callers differ in their arguments rather than in their algorithm.
 *
 * THERE WAS A `PolicyScriptKind` HERE AND IT IS DELETED, NOT EXTENDED. It spelled four members — inline
 * script, inline handler, javascript: URL, eval — of which the first three were a SECOND SPELLING of
 * `CspInlineType` (core/frame/csp_directive_list.h), the vocabulary §4.2.3's `type` parameter is written in
 * and §6.8.2 switches on, and the fourth was not an inline check at all. A translation function existed for
 * no purpose but crossing between the two spellings, and its `DFAIL` existed for no purpose but catching the
 * member that did not belong in the enum. Adding "style" and "style attribute" to it would have fixed the
 * NAME — which by then asserted something false — and kept both defects. Naming the shared vocabulary fixes
 * the type instead, and the translation and its DFAIL go with it.
 *
 * The javascript: URL row of that enum carried the fix for a real bug and the fact survives its deletion:
 * §6.8.2 maps the inline type "navigation" to `script-src-elem`, NOT to `script-src-attr`, so
 * `script-src 'unsafe-inline'; script-src-attr 'none'` must NOT block a javascript: URL. It is stated where
 * the mapping is (csp_directive_list.c's §6.8.2) and asserted by the fixture, which is where it belongs. */

/* CSP §4.2.3 "Should element's inline type behavior be blocked by Content Security Policy?" — true for
 * "Allowed". Every policy of the list is asked and the content runs only if EVERY one permits it (§2.2: a
 * second policy can only narrow), the opposite quantifier from core/html/trusted_types.c over the same list.
 *
 * `element` AND `source` ARE §6.7.3.3's, AND THEY ARE WHAT MAKES THIS ANSWER RIGHT FOR THE PAGE'S OWN
 * CONTENT. `<style nonce=abc>` under `style-src 'nonce-abc'` is ALLOWED; an answer computed from §6.7.3.2
 * alone refuses a sheet every browser applies, and every value the cascade then resolves is wrong. `element`
 * may be NULL — §4.2.4 runs this check "upon null" for a javascript: navigation, and an injected breakout has
 * been inserted nowhere; core/frame/csp_source_list.h says why that needs no branch of its own. `source` is
 * BORROWED, is not NUL-terminated, and is the bytes §6.7.3.3's hash arm digests. */
bool policy_allows_inline(const PolicyContainer *p, CspInlineType type, const lxb_dom_element_t *element,
                          const char *source, size_t source_len);

/* CSP §4.4.1 "EnsureCSPDoesNotBlockStringCompilation" — `eval`, `new Function`, `setTimeout(string)`. NOT an
   inline check: no element, no type, no §6.8.2 mapping, and §6.1.10 states that its directive lookup is
   deliberately not §6.8's fallback machinery — 'unsafe-eval' acts as a page-wide flag, so the granular
   `script-src-elem`/`script-src-attr` forms are never consulted for it. That difference is the whole reason
   this is its own entry point rather than one more member of a shared enum. */
bool policy_allows_string_compilation(const PolicyContainer *p);

/* CSP §4.1.2 "should request be blocked by Content Security Policy?" — Fetch's MAIN FETCH STEP 7, and the one
 * question this container answers about a URL rather than about inline content.
 *
 * IT LIVES HERE BECAUSE THE WALK IS THE SAME WALK. §4.1.2 runs §6.7.2.1 over every policy of this container's
 * list and blocks if ANY of them is violated, which is `policy_allows_inline`'s quantifier with the two answers
 * renamed — one file holding both is one statement of "a second policy can only narrow", and two files would
 * hold two copies of it. What differs is only the question each policy is asked.
 *
 * TWO ANSWERS, NAMED AS §4.1.2 NAMES THEM, for the reason core/fetch/port_blocking.h names its two: the
 * caller is a disjunction of blocking checks in ONE `if`, and a bool among them read the wrong way round is a
 * request silently made or silently refused.
 *
 * `destination` IS FETCH §2.2.5's DESTINATION STRING, passed through to §6.8.1 — see
 * csp_effective_directive_for_request for why it is that string and not an enum. `redirect_count` is the
 * request's; a request that has not been redirected has 0 and is the only kind this engine makes.
 * `url` is the request's CURRENT URL, parsed, because every relation §6.7.2 states reads a component of it. */
typedef enum {
    CSP_REQUEST_ALLOWED = 0,
    CSP_REQUEST_BLOCKED = 1,
} CspRequestVerdict;
CspRequestVerdict policy_should_block_request(const PolicyContainer *p, const UrlRecord *url,
                                              const char *destination, int redirect_count);

/* §7.1.5's CSP-DERIVED SANDBOXING FLAGS for a CSP list, which is the ONE thing a policy container contributes
 * to a Document's active sandboxing flag set. §7.4.5 builds navigationParams's final sandboxing flag set as
 * "the union of targetSnapshotParams's sandboxing flags and policyContainer's CSP list's CSP-derived
 * sandboxing flags", and this is the second half. The algorithm is: take every ENFORCE policy's `sandbox`
 * directive, keep the LAST one, and parse a sandboxing directive over its value; no such directive anywhere
 * in the list is an EMPTY flag set.
 *
 * IT TAKES THE TEXT RATHER THAN A CONTAINER, because the caller that needs it is the NAVIGATION and the
 * container it would read does not exist yet — §7.5.1 hands the new Document its policy container and its
 * final sandboxing flag set in the same breath, so the flag set is computed from the response's policy before
 * anything is installed. Every policy this engine parses has disposition ENFORCE: a container is built from
 * `Content-Security-Policy` and from CSP §3.3's `<meta>`, never from `Content-Security-Policy-Report-Only`,
 * which this build is never handed.
 *
 * IT IS NEVER GIVEN `<meta>` TEXT, and that is enforced rather than assumed: CSP §3.3 makes `sandbox`
 * meaningless in a `<meta>` element, and this parser carries no per-policy SOURCE that could tell one from
 * the other — so core/dom/document.c asserts, at the site that collects a `<meta>` policy, that the policy it
 * is about to merge declares no `sandbox` directive, using this same function as the question. */
SandboxFlags policy_csp_derived_sandboxing_flags(const char *serialized_csp_list, size_t len);

#endif
