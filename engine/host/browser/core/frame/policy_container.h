/* THE POLICY CONTAINER — HTML §7.1.7 "Policy containers". See policy_container.c.
 *
 * IT CROSSES INSTANCES, and the initial about:blank is what forces that. A child created with no URL —
 * `window.open()`, an `<iframe>` with no src — gets its Document synchronously and has no response to take a
 * policy from; §7.3.2.1 "Creating browsing contexts" says its container is "a clone of creator's policy
 * container". When the child is CROSS-ORIGIN it lives in
 * another instance, so that clone is a CROSS-INSTANCE operation: the creator's container is serialized to the
 * child's instance, and the requesting flow SUSPENDS across the boundary the same way it suspends at an await.
 * WHICH SIDE OF THAT A CHILD FALLS ON IS ITS ORIGIN'S ANSWER, NOT A COST DECISION. This said that one WASM
 * instance is one DOCUMENT regardless of origin, so same-origin was not an exemption, and that premise is the model
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
#include "core/frame/embedder_policy.h"
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
   built from a response takes its own, and §7.3.2.1's clone takes the CREATOR's along with the text. BORROWED —
   an origin lives for the agent.
   `embedder` IS §7.1.4'S ITEM AND IS STATED BY EVERY CALLER, never defaulted here. §7.1.7 makes it "initially
   a NEW embedder policy" and its create-a-policy-container-from-a-fetch-response makes it "the result of
   obtaining an embedder policy given response and environment" — two different answers to one item, and which
   of them applies is a fact about the OPERATION building the container. A container that filled it in itself
   would answer `unsafe-none` for a response that opted into cross-origin isolation, which is the silence this
   item exists to end; a caller with no response says so by passing serialized_embedder_policy_new(), which is
   the standard's own words rather than an omission. It is COPIED — both endpoint strings — because a container
   owns its items and outlives the operation that built it. */
PolicyContainer *policy_container_new(const char *csp_text, const Origin *self_origin,
                                      const char *referrer_policy, SerializedEmbedderPolicy embedder);

/* §7.1.7's "clone a policy container" — §7.3.2.1 performs this for a navigable created with a creator, which is
   how an initial about:blank inherits its CSP. A DEEP copy: the child's policy is its own from the moment it
   exists, so a later navigation of the parent cannot reach back and change what the child may do.
   AND IT KEEPS THE SOURCE'S CSP §2.2 SELF-ORIGIN, which §7.1.7's clone algorithm does not restate: its steps
   are "let clone be a new policy container" and "for each policy in policyContainer's CSP list, append a copy
   of policy into clone's CSP list", which move the POLICIES and say nothing about the list's other half. That
   is an editorial hole in the clone and not a licence to re-derive one, because CSP §2.2's note states the
   intent directly — the self-origin exists "to facilitate the 'self' checks of local scheme documents/workers
   that have inherited their policy but have an opaque origin", and a clone that dropped it would fail in
   exactly the case the field was added for. */
PolicyContainer *policy_container_clone(const PolicyContainer *src);
void policy_container_free(PolicyContainer *p);

/* CSP §2.2.1 "Parse a serialized CSP"'s RESULT, APPENDED TO THIS CONTAINER'S CSP LIST — HTML §4.2.5.3 "Pragma
 * directives"' content security policy state step 5, "Enforce the policy policy".
 *
 * THE LIST IS A LIST AND EVERY DELIVERY APPENDS. CSP §2.2 "Policies" makes a CSP list "a struct consisting of
 * policies (a list of policies) and a self-origin", §2.2.2 "Parse response's Content Security Policies"
 * APPENDS each header-delivered policy to it, and §8.1 "The effect of multiple policies" states what that
 * buys: "adding additional policies to the list of policies to enforce can only further restrict the
 * capabilities of the protected resource". So a second delivery neither replaces the first nor merges into it
 * — it stands beside it, is enforced independently, and the content runs only if EVERY policy permits it.
 * Both directions of getting that wrong are wrong in this engine specifically: a REPLACE reports a breakout
 * the first policy kills, and a DROP reports one the second policy kills, and §@S measures every breakout
 * against exactly this list.
 *
 * WHY THIS EXISTS AT ALL, WHICH IS THE PART THAT IS NOT PEDANTIC. A container had no way to GROW: the only
 * road into its list was the constructor, so the two deliveries this engine sees had to be composed into one
 * string before a container existed. That is fine for the two the DOCUMENT CREATION knows about (the
 * response's `Content-Security-Policy` and the `<meta>` elements the parser already put in the tree) and it
 * has no answer at all for the third, which HTML names in its own words: "At the time of inserting the meta
 * element to the document, it is possible that some resources have already been fetched", and its worked
 * example is "dynamically inserting a meta element with an http-equiv attribute in the Content security
 * policy state". A policy a script inserts is enforced by every browser and had nowhere to go here.
 *
 * IT GROWS THE CONTAINER IN PLACE rather than building a replacement, and that is an ownership statement:
 * everything a container holds is reachable only through the pointer its Document keeps, so a replacement
 * would leave that Document naming one container while the flow that made the second one named the other.
 * The text is re-composed and the §2.2 list is RE-PARSED WHOLE, because every name and value token of the old
 * parse is a SLICE of the text this append moves.
 *
 * `serialized_policy` is ONE POLICY in §2.2 "Policies"' serialization — the string §2.2.1 "Parse a serialized
 * CSP" takes, not the comma-delimited list §2.2.2 splits — and is BORROWED. It must carry no U+002C
 * (§2.3's `directive-value` grammar excludes it and §2.2 makes it the LIST delimiter, so a comma inside would
 * append TWO policies) and no `sandbox` directive (§4.2.5.3 step 4 removes it, and §7.1.5's CSP-derived
 * sandboxing flags were unioned into the Document's active sandboxing flag set at its CREATION, where nothing
 * can recompute them). Both are asserted here, at the one place a policy can now arrive after that point.
 *
 * NAMED RESIDUAL — the grown list is the DOCUMENT's and not the appending FLOW's. policy_container.c already
 * states that a container is installed once on the pre-boot baseline and is not yet per-flow; an append made
 * by a flow is therefore visible to that flow's siblings, which is the same gap document_install's own DCHECK
 * names ("build it as a COW record, like ProxyData's PROXY_REC, captured in its accessor"). The next diff is
 * that record, and the append is what makes it load-bearing rather than hypothetical. Its absence shows as a
 * finding emitted by a flow that never ran the inserting script carrying a `cspBlocks` naming a policy that
 * flow's own path never delivered — a SUPPRESSED real finding, which is the safe half of the two directions
 * above and is why this lands ahead of the record rather than behind it. */
void policy_container_enforce_policy(PolicyContainer *p, const char *serialized_policy);

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
   ask it a question: §7.3.2.1's create, which clones the CREATOR's container into a navigable it is making.
   NULL only for a container that does not exist. */
const Origin *policy_container_self_origin(const PolicyContainer *p);
/* §7.1.4'S EMBEDDER POLICY ITEM OF THIS CONTAINER — what §7.1.4.2's embedder policy checks reads off a child
   navigable's CONTAINER DOCUMENT ("let parentPolicy be navigable's container document's policy container's
   embedder policy"), and what §7.1.3's obtain-an-opener-policy asks to turn `same-origin` into
   `same-origin-plus-COEP`. BORROWED, and NULL only for a container that does not exist — which is a different
   answer from `unsafe-none` and must stay so: the first is "there is no document here", the second is a real
   policy a real response stated. */
const EmbedderPolicy *policy_container_embedder(const PolicyContainer *p);

/* ---- HTML §7.1.7 "Policy containers" — THE CONTAINER IN THE FORM IT CROSSES A SEAM -------------------------
 *
 * ONE VALUE, BECAUSE §7.1.7 MAKES IT ONE STRUCT. "A policy container is a struct containing policies that apply
 * to a Document", and §7.1.7's clone-a-policy-container moves EVERY item of it in one operation. A seam that
 * carries the items as separate arguments is not carrying a container; it is carrying whichever of them its
 * author remembered, and the item added NEXT is the one nobody adds to the seam. That is not hypothetical —
 * §7.1.4's embedder policy WAS obtained where a response is read and had nowhere to sit, because the two seams
 * a clone crosses (a lazily-materialized about:blank child's navigable, and the `navigable.create` notice a
 * peer instance is provisioned from) each carried the CSP list's two halves and nothing else. A field written
 * in one place and absent in the next is the defect this build makes greppable, and this type is the fix for
 * it.
 *
 * IT IS BUILT THROUGH A FUNCTION AND NEVER BY AN INITIALIZER, WHICH IS THE WHOLE FORCING MECHANISM. A
 * designated initializer zero-fills the member it does not name, so a struct that grows silently hands every
 * producer a container whose new item is whatever zero happens to mean, with the compiler saying nothing —
 * exactly the shape of a consumer defaulting a producer's field, one layer earlier. `serialized_policy_container`
 * below names EVERY item §7.1.7 gives a container that this build holds, so the day another item lands here
 * every producer STOPS COMPILING until it states one. Nothing may construct this struct any other way. THAT
 * MECHANISM HAS NOW BEEN EXERCISED ONCE, WHICH IS THE ONLY WAY TO KNOW IT WORKS: §7.1.4's embedder policy was
 * the item it was built for, and adding it stopped every producer in this tree — the two host ABI entries, the
 * WPT runner, the fixture, the WindowProxy that holds a creator's clone and the `navigable.create` notice that
 * carries one to a peer — until each of them stated where its answer comes from.
 *
 * EVERYTHING IN IT IS TEXT AND BORROWED. A live value crosses neither an instance, nor a session, nor a park,
 * so the clone that crosses an instance and the clone that crosses a session are the same operation as the
 * in-heap one (policy_container_clone re-parses this same text). Nothing here outlives the call it was built
 * for unless the receiver copies it — window_proxy.c's navigable does, because a navigable outlives its
 * creator's stack frame.
 *
 * THE CSP LIST IS TWO OF THE ITEMS AND NOT ONE, because CSP §2.2 "Policies" makes a list "a struct consisting
 * of policies (a list of policies) and a self-origin (an origin which is used when matching the 'self'
 * keyword)" and §2.2.2 states the self-origin from OUTSIDE the bytes ("self-origin is response's URL's
 * origin"). The policies travel as their §2.2 serialization and the self-origin as §7.1.1's. */
typedef struct {
    const char *csp;           /* CSP §2.2's policies, serialized. NULL or "" is a list holding none. */
    /* CSP §2.2's SELF-ORIGIN of that list, serialized — and ALSO WHAT SAYS THERE IS A CONTAINER AT ALL. §2.2
       gives every CSP list a self-origin whether or not it holds policies, so an absent one means there is no
       container here, while an empty POLICY with a self-origin beside it is a real container that holds no
       policies. Those two are not interchangeable — this build merges CSP §3.3's `<meta>` policies into the
       SAME list under the SAME self-origin (core/dom/document.c's document_policy_new) — and reading the
       presence off the POLICY collapses them, which is §7.1.7's `initiatorPolicyContainer is not null`
       answered from the wrong field. Ask serialized_policy_container_exists rather than testing it here. */
    const char *self_origin;
    /* §7.1.4's EMBEDDER POLICY, the item §7.1.7 lists between the CSP list and the referrer policy. It is ONE
       VALUE rather than four members for the same reason this whole struct is one value rather than three:
       §7.1.4 makes a policy a struct of four items and §7.1.7's clone moves it as one ("set clone's embedder
       policy to a COPY of policyContainer's embedder policy"), so a seam that spelled the items separately
       would drop the one added next. It is ALWAYS present — a container that exists has an embedder policy,
       initially a new one — which is why nothing here spells its absence. */
    SerializedEmbedderPolicy embedder;
} SerializedPolicyContainer;

/* A CONTAINER, stating every item. `self_origin` is REQUIRED and non-empty: a container that exists has a CSP
   list and every CSP list has one. The ABSENCE of a container is serialized_policy_container_none below, which
   is a different thing and is spelled differently so that no caller can reach it by passing NULL here.
   `embedder` is REQUIRED for a different reason and one that admits no "none": §7.1.7 gives every container an
   embedder policy whether or not a response stated one, so the caller with nothing to state says so with
   serialized_embedder_policy_new() — §7.1.4's own "a new embedder policy" — rather than by omitting an
   argument this function would then have to invent. */
SerializedPolicyContainer serialized_policy_container(const char *csp, const char *self_origin,
                                                      SerializedEmbedderPolicy embedder);

/* NO CONTAINER — §7.1.7's `null` for the initiator/parent/history containers its determine step tests against,
   which is a real state (a Document with no creator) and not a caller that forgot an argument. */
SerializedPolicyContainer serialized_policy_container_none(void);

/* §7.1.7's "policy container-OR-NULL" AS IT ARRIVES FROM OUTSIDE THIS ENGINE — an ABI entry, or the
   `navigable.create` notice the trusted zone relays from the instance that created this document. Such a
   boundary carries FIELDS and cannot carry a null struct, so the absence arrives as an absent SELF-ORIGIN, and
   this is the ONE place that spelling is read: a host entry that tested it inline would be a second reading of
   §7.1.7's `is not null`, in a different file, for each entry that has one.
   `embedder` IS STILL STATED FOR AN ABSENT CONTAINER, and that is not a contradiction: an entry reading a
   record reads its fields before it knows whether they describe a container, and refusing to take the item
   until the answer is known would put the §7.1.7 test in the caller — which is the second reading this
   function exists to prevent. An absent container discards it, exactly as it discards the policy text. */
SerializedPolicyContainer serialized_policy_container_or_none(const char *csp, const char *self_origin,
                                                              SerializedEmbedderPolicy embedder);

/* THE SERIALIZATION OF A LIVE CONTAINER — §7.1.7's clone, in the form it crosses a seam. It reads EVERY item
   off `p`, so a container that grows grows here once and no caller has to be told: this is the one place a
   live container becomes bytes. `p` must exist; the absence is the function above. The bytes are BORROWED from
   `p` and from the agent's origin records, so they live as long as the container does. */
SerializedPolicyContainer serialized_policy_container_of(const PolicyContainer *p);

/* IS THERE A CONTAINER — §7.1.7's `is not null`, asked in ONE place so that every seam answers it from the
   same field. See `self_origin` above for why it is not the policy text. */
bool serialized_policy_container_exists(SerializedPolicyContainer c);

/* HTML §7.1.7's "determine navigation params policy container", over the containers that reach a seam.
 *
 * THE ALGORITHM IS THE SPEC'S AND ITS PREDICATE IS `responseURL is local` — NOT "the response carried no
 * policy". The standard's steps, in order, are: a history container when the URL requires storing one; the
 * PARENT's for `about:srcdoc`; "if responseURL is local and initiatorPolicyContainer is not null, then return a
 * clone of initiatorPolicyContainer"; then "if responsePolicyContainer is not null, then return
 * responsePolicyContainer"; then a new one. So a document FETCHED from a non-local URL is judged under its own
 * response's container even when that response carried no policy at all — an empty list is a container, and
 * inheriting instead would put a cross-origin child under its EMBEDDER's policy, which is the common shape on
 * any CSP-bearing page that frames a third party. Reading the ordering off the presence of a policy is the
 * approximation that produces exactly that, silently, for every such frame.
 *
 * WHICH CONTAINER IS "INHERITED" IS THE OPERATION'S ANSWER AND NOT THIS FUNCTION'S. The initiator's and the
 * parent's are one parameter here because the operation that reaches this seam — §7.3.2.1's create, announced
 * as a `navigable.create` — has one creator that is both. A caller with a different operation states a
 * different container; it does not get a different rule.
 *
 * `response_url` IS THE ADDRESS THE DOCUMENT IS BEING CREATED AT, and it is parsed here rather than tested with
 * a `strncmp` because Fetch §2.1's local set is `about`, `blob` and `data` — the two beyond `about` being
 * precisely the schemes whose Document has an OPAQUE origin, which is the case CSP §2.2's own note says the
 * self-origin exists for.
 * `response` is §7.1.7's create-a-policy-container-from-a-fetch-response for THIS document's own response — its
 * CSP list is §2.2.2's pair, "a CSP list whose policies is policies and self-origin is response's URL's origin".
 * IT IS ALWAYS STATED, including for a Document created from NO response: that one gets step 5's "new policy
 * container", whose list holds no policies, and the self-origin it still needs is this document's own address's
 * origin — which is the same answer §2.2.2 would have given had there been a response. So the absence a caller
 * has is an absence of POLICIES, which this type spells with a NULL `csp`, and never an absent container.
 * `inherited` is §7.1.7's CLONE of the creator's container, or none. */
SerializedPolicyContainer policy_container_determine_navigation_params(const char *response_url,
                                                                      SerializedPolicyContainer response,
                                                                      SerializedPolicyContainer inherited);

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
