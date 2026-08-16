/* HTML §7.4 — creating a navigable, and where a same-origin about:blank child comes from.
 *
 * `window.open()` with no argument and an `<iframe>` with no `src` both produce a navigable whose initial
 * Document is `about:blank`. That Document has no response to take anything from, which is why HTML gives every
 * Document a §7.2.6 POLICY CONTAINER and has §7.4 CLONE THE CREATOR'S when there is a creator. The child
 * inherits its parent's CSP by the ordinary rule, not by an inheritance rule written for CSP — see
 * policy_container.h.
 *
 * AN INSTANCE IS AN ORIGIN-KEYED AGENT, so WHERE the child lives is decided by its ORIGIN. A SAME-ORIGIN child
 * is a second realm in THIS heap — HTML's similar-origin window agent — and its policy container is cloned by
 * an ordinary in-heap copy. A CROSS-ORIGIN child is another instance, and there the clone travels as the
 * creator's SERIALIZED policy on the create notice, which the container can do precisely because it is a flat
 * parse over one owned string: the clone that crosses an instance and the one that crosses a session are the
 * same operation.
 *
 * CREATION IS SYNCHRONOUS, AND THAT IS NOT A CONVENIENCE — IT IS THE SPEC. §4.8.5's iframe INSERTION STEPS
 * create the child navigable, so `frame.contentWindow` answers on the line after the append, and §7.4's
 * `open()` returns its WindowProxy at its own call site. Neither can round-trip to the host. This used to, and
 * the cost was not subtle: every host answered "not created" rather than host a second document, so
 * contentWindow was null and the whole of html/browsers read members of null.
 *
 * WHAT MAKES IT SYNCHRONOUS IS THE NAME. A document is named, and a document created by this one is named
 * "<my name>.<n>" — unique by induction with no allocator and no round trip (world.h). So the engine MINTS the
 * child and the host is TOLD, as a one-way notice. The host still owns routing, because only the trusted zone
 * knows which instance holds which document; what it no longer owns is the identity, which it never needed to.
 *
 * A HOST THAT WILL NOT HOST THE CHILD does not answer that by withholding a name — it simply never provisions
 * the instance, and every read through the proxy parks. That is a host gap, visible as a parked flow, rather
 * than a page-visible null that a page cannot tell from a popup blocker. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGABLE_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGABLE_H

#include <stdbool.h>

#include <stdint.h>

#include <lexbor/html/html.h>

#include "quickjs.h"
#include "core/frame/sandboxing.h"
#include "core/frame/window_features.h"
#include "core/url/origin.h"

/* THE TWO HALVES OF §7.4, AND THEY ARE DECLARED SEPARATELY BECAUSE THEY BELONG TO DIFFERENT OWNERS.
 *
 * Creating a navigable decides WHICH DOCUMENT it has; navigating it to an address FETCHES that address. The
 * first is semantics and belongs to the engine — a Lexbor parse, like every other document this engine holds.
 * The second is the NETWORK and belongs to the host, because SECURITY.md puts every byte of it behind the
 * trusted chokepoint and no engine-side code may reach it.
 *
 * THEY WERE ONE CALLBACK, AND THAT CONFLATION IS WHY ONE HOST DID BOTH AND ANOTHER SILENTLY DID NEITHER. The
 * builder took a `url` and was left to decide what to do with it: the conformance runner fetched, parsed and
 * installed; the PRODUCT host wrote `(void)url;` and returned a realm holding the empty about:blank Document.
 * So `window.open("/admin")` in the shipped engine produced a popup whose scripts never ran, with no assert, no
 * notice and nothing in the output to distinguish it from a page that genuinely had none — a surface reported
 * as explored that was never reached. A seam with two jobs lets a host implement one and look finished.
 *
 * A HOST THAT CANNOT FETCH IS NOT A HOST THAT NAVIGATES TO about:blank. It is a host missing a capability, and
 * §7.4 says so at the point the address arrives rather than handing back an empty document that reads as a
 * real one. */

/* THE NETWORK HALF IS A HOST-OWED ANSWER, not a callback — `document.fetch<TAB><url>`, issued by §7.4 step
   14's load job and parked on until the host answers with `{body, csp}` (navigable.c). There is nothing for a
   host to install here, which is the point: a synchronous fetcher could only ever be implemented by a host
   whose network happens to be synchronous, and the product host's is the trusted zone's — every request parks
   the asking flow. The one shape that serves both is the one every other host-owed answer in this engine
   already uses, so the load uses it and a host that cannot answer leaves the flow parked, visibly, rather than
   silently keeping an empty document. */

/* HOW THIS AGENT BUILDS A REALM AROUND A DOCUMENT — declared by the HOST, called by §7.4 when the child is
 * SAME-ORIGIN.
 *
 * A same-origin child navigable is a second REALM in this heap, and it must get the SAME platform surface its
 * creator has: a child whose `window` is smaller is a different browser, and every fidelity answer measured in
 * it would be measured in the wrong one. WHICH surface that is belongs to the HOST — it is the host that
 * decides what this build exposes — so the host declares the builder once and §7.4 calls it.
 *
 * `dom` is the child's Document, ALREADY DECIDED: the empty about:blank one §7.4 creates, or the parse of the
 * address's bytes when there was an address and the fetcher answered. `url` is that Document's ADDRESS — a
 * FACT ABOUT IT, which `document.URL`, §4.4's base URL and Location are all built from — and NOT an instruction
 * to fetch anything; the fetch already happened, above, and the parse is the engine's. That distinction is the
 * whole of what was wrong before: the same argument meant "here is the address, do whatever you think that
 * implies", and one host read it as "fetch and parse this" while the other read it as nothing at all. */
/* `csp` is the policy this document is CREATED with: the response's `Content-Security-Policy` above, or §7.4's
   clone of the CREATOR's for an `about:` child, which came from no response at all. NULL when there is neither.
   It travels with the parsed tree because §7.2.6's container is built from BOTH, and a builder handed only the
   tree would silently install a document judged against its `<meta>` policies alone. */
/* `top_level_url` is HTML §8.1.3.1's TOP-LEVEL CREATION URL for the environment this realm is built with, and
   it is a SEPARATE argument from `url` because the two answer different questions and only sometimes agree.
   `url` is THIS document's address; `top_level_url` is the address of the environment at the top of the
   navigable's chain, which §8.1.3.5 reads to decide whether this realm is a SECURE CONTEXT and therefore which
   of Web IDL §3.3.13's members exist in it at all. They differ for every nested navigable — an `https` iframe
   inside an `http` page is not a secure context and an `about:blank` iframe of one is not either — so a
   builder handed only `url` would install a platform surface belonging to a document it is not building. */
/* `sandbox_flags` is §7.1.5's ACTIVE SANDBOXING FLAG SET for the Document this realm is being built for, and
   it is a SEPARATE argument from `csp` because §7.1.7's policy container does not contain one — five sites in
   this tree used to say it did. What the container contributes is one HALF of it, through the CSP `sandbox`
   directive alone (§7.1.5's CSP-derived sandboxing flags); the other half is the navigable's CREATION
   sandboxing flags, which come from the `<iframe sandbox>` attribute and the embedder's own set. Which of the
   two algorithms produced the set depends on WHICH ALGORITHM IS CREATING THE DOCUMENT — §7.2's create hands
   the initial about:blank the creation flags alone, §7.5.1's create-and-initialize hands a navigated Document
   §7.4.5's union — so the caller states the whole set and the builder never re-derives it. */
/* `csp_self_origin` is CSP §2.2's SELF-ORIGIN of that policy list, serialized — it rides beside `csp` because
   it is the other half of one CSP list (§2.2 makes the list a struct of policies AND an origin) and because it
   is the half the bytes do not contain: §2.2.2 states it from the RESPONSE's URL, and §7.4's clone hands the
   CREATOR's to a document that came from no response. A builder that answered it from `origin` would be right
   for an unsandboxed document loaded from its own address and wrong for both of those. */
typedef JSContext *(*RealmBuilder)(JSRuntime *rt, lxb_html_document_t *dom, const char *url,
                                   const char *top_level_url, const char *origin,
                                   const char *csp, const char *csp_self_origin, SandboxFlags sandbox_flags,
                                   uint32_t doc_id, JSValueConst nav_proxy);
void navigable_set_realm_builder(RealmBuilder b);

/* BUILD THE REALM OF A SAME-ORIGIN NAVIGABLE THIS AGENT HOLDS. The answer is BORROWED, and that is a statement
 * about who owns a realm rather than a convention for this call: a realm is kept alive by its own function
 * objects (each holds a counted reference to the realm that defined it), those hang off the Window, and the
 * Window hangs off the navigable's WindowProxy — so a realm lives exactly as long as its NAVIGABLE is
 * reachable, and dies as a garbage cycle when it is not. Anything that took an owning reference would be an
 * external ROOT making its realm, and everything the realm reaches, permanently reachable. See navigable.c.
 *
 * WHEN IT IS BUILT IS DECIDED BY WHAT THE NAVIGABLE DOES, and the two answers are one spec sentence read from
 * both ends.
 *
 * A NAVIGABLE WITH AN ADDRESS IS MATERIALIZED AT CREATION, because §7.4 step 14 NAVIGATES it and navigating
 * RUNS THE DOCUMENT'S SCRIPTS. Those scripts owe nothing to the creator — a popup posts back to its opener
 * without the opener ever touching the proxy — so a realm built on first touch is a popup that never ran.
 * Twelve files in html/browsers reported nothing whatsoever for exactly that reason. This is not a cost
 * question: not running a document changes what the page DOES.
 *
 * A NAVIGABLE WITH NO ADDRESS IS MATERIALIZED ON FIRST TOUCH, and there the deferral genuinely has no
 * observable. `open()` and an `<iframe>` with no `src` hold the INITIAL about:blank Document §7.4 creates them
 * with; that Document has no scripts by construction, so nothing in it can run and the only way to observe it
 * at all is a read through its WindowProxy — which is where it is built. The navigable itself is fully there
 * meanwhile: named, counted by `parent.length`, nested, destroyable.
 *
 * THE DEFERRED HALF IS LOAD-BEARING, and it was measured rather than assumed. A forced-execution frontier holds
 * thousands of flows and each one's boot runs the same `open()` in its own world, so materializing every
 * never-touched about:blank is a platform per flow: the heap ran out at ~2030 flows, with the initial
 * about:blank parse itself failing to allocate. Building them all and paging the low-value tail to the cold
 * tier is the design that would remove even this line — a JSContext is not a snapshot, so that is a real
 * mechanism to build and not a rewording of the deferral. */
/* THE RESPONSE IS AN ARGUMENT, because fetching it is a SUSPEND and this function cannot suspend — it is
   called from `proxy_realm`, which answers a property read in the turn that made it. So whoever HAS a response
   passes it and whoever has none passes NULL, and the two are the same call: `body`/`body_len` are the bytes
   the address served (NULL for `about:`, and NULL for a fetch that failed — a browser showing an error page
   still has a navigable), and `csp` is the policy the Document is CREATED with, which is the response's
   `Content-Security-Policy` when there was a response and §7.2.6's inherited clone when there was not. WHOSE
   clone that is depends on the operation and only the caller knows: creating a navigable clones the CREATOR's
   (kept on the navigable, because a srcless child's realm is built later and by whichever document reads
   through it first), navigating one clones the INITIATOR's, the document whose script ran. */
/* `origin` is §7.3.1's answer for THIS document — the RECORD, because the identity is what a same-origin check
   compares and the host boundary below takes only its serialization (a host builds a realm; it does not decide
   a principal). It is asserted to be same origin with the agent's, which is what makes that split sound. */
/* `sandbox_flags` is §7.1.5's ACTIVE SANDBOXING FLAG SET for the Document being built, stated by the caller
   for the same reason `csp` is and answering a question the container cannot: §7.2's create gives the initial
   about:blank the navigable's CREATION sandboxing flags, and §7.4.5's navigation gives its Document the UNION
   of those and the response policy's CSP-derived flags. */
JSContext *navigable_realm(JSContext *ctx, uint32_t doc, const char *url, const char *top_level_url,
                           const Origin *origin, JSValueConst nav_proxy, const char *body, size_t body_len,
                           const char *csp, const char *csp_self_origin, SandboxFlags sandbox_flags);

/* THE AGENT'S HALF: §7.4's `open` member, declared once. */
void navigable_init(JSContext *ctx);

/* Install §7.4's scriptable entry point — `window.open`. The origin an initial about:blank child inherits is
   THE AGENT'S (origin_agent), so it is not passed per document: an instance is an origin-keyed agent cluster,
   and a per-document principal would be a second answer to a question the agent already answers. `origin` is
   the serialization the host stated for this document, and it is here to be CHECKED against that one answer. */
void navigable_install(JSContext *ctx, JSValueConst global, const char *origin);

/* §7.4 STEP 14's NAVIGATE over a navigable that already has an active document — fetch the new document, build
   the realm its scripts run in, and hand both to §7.2.5.1's replace. Answers the SAME proxy (owned), because a
   navigation does not make a new one — that is the whole reason WindowProxy exists — or JS_UNDEFINED when the
   address does not parse, which §7.4 turns into a SyntaxError at the call site. */
/* EVERY NAVIGABLE OF THIS AGENT, IN TREE ORDER — a container before what it contains, siblings in the tree
   order of their navigable containers. As an Array of WindowProxy (owned).
 *
 * IT IS HERE BECAUSE THE TREE IS. Two components need this walk for two different reasons — HTML §8.1.7.3 step
 * 2's document list and the per-document LOAD LIFECYCLE — and a second copy of a tree walk is the second answer
 * that is always subtly wrong: nav_find_in_tree below is a third, breadth-first, because §7.1 wants the NEAREST
 * match rather than the outermost. Each caller applies its OWN filters to this list; what they share is the
 * ORDER, which is the spec's and not theirs.
 *
 * THE WALK IS ITERATIVE over an explicit worklist, for the reason nav_find_in_tree's is: a self-call would be
 * C-to-C recursion whose depth is the PAGE's iframe nesting, which is the page's to choose.
 *
 * A NAVIGABLE THIS INSTANCE HAS NOT MATERIALIZED IS NOT IN THE LIST, and neither is a peer's or a destroyed
 * one. An unmaterialized navigable holds the initial about:blank Document §7.4 created it with; that Document
 * has no scripts, so it has no lifecycle to run and nothing below it to walk — "not materialized" and "no
 * children" are the same answer (window_proxy.h), and materializing every navigable a forced-execution
 * frontier ever created in order to ask is the heap exhaustion navigable.c's deferral exists to avoid. */
JSValue navigable_tree_order(JSContext *ctx);

JSValue navigable_navigate(JSContext *ctx, JSValueConst proxy, const char *url);

/* HTML §7.4.2.3.2's EVALUATE A JAVASCRIPT: URL, over a navigable whose active document is THIS one — which is
 * every `javascript:` navigation whose chosen navigable is the initiator's own, the `_self` case and the one a
 * `javascript:` href or form action takes when nothing names another target.
 *
 * IT IS NOT A FETCH AND IT IS NOT §7.4 STEP 14'S LOAD. Navigating to a `javascript:` URL has its own section
 * precisely because nothing is fetched: the URL's own bytes ARE the program. So this does what that section
 * says and nothing else — serialize, remove the leading `javascript:`, percent-decode, UTF-8 decode, and RUN
 * the classic script — and it runs it as a PROGRAM OF THE RUNNING FLOW rather than through a C `JS_Eval`,
 * because the source is the page's code and may hold a loop, an `await` or a recursion that has to park.
 *
 * `url` is the SERIALIZED URL, whose scheme must be `javascript`; the caller has already parsed it. */
void navigable_evaluate_javascript_url(JSContext *ctx, const char *url);

/* §7.4 STEPS 6 AND 14 AS ONE OPERATION — choose a navigable for `target` and navigate it to `url`, creating one
   and giving it the name when nothing answers to it. TWO callers, and they are not variants of each other:
   `window.open()` reaches it after parsing a features string, and §4.6.3's FOLLOWING A HYPERLINK reaches it
   from an `<a>`'s activation behaviour with `noopener` read off `rel`. The rules for choosing a navigable are
   ONE algorithm; a second copy in the hyperlink path would be the second answer that is always subtly wrong.
   Answers the chosen navigable's WindowProxy (owned), or JS_UNDEFINED when the url does not parse — which
   §7.4 turns into a SyntaxError and §4.6.3 discards, because a click is not a place a page can catch one. */
JSValue navigable_open(JSContext *ctx, const char *url, const char *target, const WindowFeatures *feat);
void navigable_free(JSContext *ctx);

/* HOW MANY CHILD REALMS ARE LIVE — the working set child_document's OOM `CHECK` names, asked of the only
   component that knows it. LIVE, not built: a realm leaves this count when it is torn down, which is when its
   navigable stops being reachable, so the number tracks reachable navigables rather than flows that ever made
   one. It exists because the alternative was inferring it from
   JS_ComputeMemoryUsage's `memory_used_*`, which is the runtime's MISCELLANEOUS bucket (every property array
   and every fast-array element vector lands in it) and answers a different question with a similar-looking
   number — see the @HEAP line in solver/engine.c. */
int navigable_realm_count(void);

/* §7.4's CREATE A NEW NAVIGABLE. `url` is the child's initial address; NULL, "" or "about:blank" all mean the
   initial about:blank Document, which inherits this document's origin and policy container. Returns the child's
   WindowProxy, or JS_UNDEFINED when `url` does not parse — the caller decides what that means, because §7.4
   throws a SyntaxError for it where §4.8.5 does not. */
/* `name` is the browsing context name to give it (an iframe's `name` attribute, §7.4's target), or NULL.
   `is_child` distinguishes §4.8.5's CHILD navigable — nested in this one, so its `parent` is this Window — from
   §7.4's AUXILIARY one, which is its own top and links back through `opener`. */
/* `feat` is §7.4's parsed features argument, or NULL for §4.8.5's iframe — which has no features to
   parse, is never a popup, and never has an opener. */
/* `iframe_sandbox_flags` is §7.1.5's IFRAME SANDBOXING FLAG SET of the container element — the parse of its
   `sandbox` content attribute, or an EMPTY set when the element carries no such attribute. It is the embedder's
   half of determine-the-creation-sandboxing-flags and only §4.8.5's caller has it: the attribute is read
   through the DOM chokepoint in the CREATING FLOW's delta, which is what makes a child navigable's sandboxing
   per-flow without anything here having to capture it. Must be empty for an AUXILIARY navigable (`is_child`
   false), which has no embedder element at all — §7.1.5 answers that case from the POPUP sandboxing flag set,
   which this function derives from the creator's own active set. */
JSValue navigable_create(JSContext *ctx, const char *url, const char *name, bool is_child,
                         const WindowFeatures *feat, SandboxFlags iframe_sandbox_flags);

#endif
