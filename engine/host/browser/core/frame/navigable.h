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
#include "quickjs-step.h"                 /* §7.4.3's reload is a machine: its first step runs the page's code */
#include "core/frame/navigate_event_fire.h" /* §7.2.6.10.4's fire, which §7.4.3 step 1 performs */
/* §7.1.7's POLICY CONTAINER in the form it crosses a seam — a Document is created with one and a navigable is
   created with the clone of its creator's, so both entries below take one. */
#include "core/dom/document.h"           /* DocumentKind — DOM §4.5's type and content type for a Document */
#include "core/frame/policy_container.h"
#include "core/frame/embedder_policy.h"   /* §7.1.4's item, which §7.1.4.2 checks a rooted navigable against */
#include "core/frame/opener_policy.h"     /* §7.5.1's opener policy ROW, which a rooted navigable carries */
#include "core/frame/sandboxing.h"
#include "core/frame/window_features.h"
#include "core/url/origin.h"
#include "core/mime/mime_type.h"   /* §7.4.5's computed type is a MIME type RECORD, not a header value */

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
   14's load job and parked on until the host answers with `{url, body, headers}` (navigable.c). The `url` is
   Fetch §2.2.6 "Responses"' RESPONSE URL and is the field only a host can state: HTML §7.4.5 "Populating a
   session history entry" determines the loaded Document's origin over it, and a host is the only party that
   saw the redirect chain — a host that follows none states the address it was asked for, which is a statement
   about its own network rather than an absence. There is nothing for a
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
/* `policy` is HTML §7.1.7's POLICY CONTAINER this document is CREATED with — §7.1.7's determine step has
   already chosen between the response's own and §7.3.2.1's clone of the CREATOR's, so the builder is handed
   the answer and never the choice. It travels with the parsed tree because a Document's container is built
   from BOTH (this build merges CSP §3.3's `<meta>` policies into the same CSP list), and a builder handed only
   the tree would silently install a document judged against its `<meta>` policies alone.
   ONE ARGUMENT AND NOT ONE PER ITEM: §7.1.7 makes a container one struct whose clone moves every item at once,
   and a seam that spelled the items separately is a seam the next item never reaches. */
/* `top_level_url` is HTML §8.1.3.1's TOP-LEVEL CREATION URL for the environment this realm is built with, and
   it is a SEPARATE argument from `url` because the two answer different questions and only sometimes agree.
   `url` is THIS document's address; `top_level_url` is the address of the environment at the top of the
   navigable's chain, which §8.1.3.5 reads to decide whether this realm is a SECURE CONTEXT and therefore which
   of Web IDL §3.3.13's members exist in it at all. They differ for every nested navigable — an `https` iframe
   inside an `http` page is not a secure context and an `about:blank` iframe of one is not either — so a
   builder handed only `url` would install a platform surface belonging to a document it is not building. */
/* `sandbox_flags` is §7.1.5's ACTIVE SANDBOXING FLAG SET for the Document this realm is being built for, and
   it is a SEPARATE argument from `policy` because §7.1.7's policy container does not contain one — five sites
   in this tree used to say it did. What the container contributes is one HALF of it, through the CSP `sandbox`
   directive alone (§7.1.5's CSP-derived sandboxing flags); the other half is the navigable's CREATION
   sandboxing flags, which come from the `<iframe sandbox>` attribute and the embedder's own set. Which of the
   two algorithms produced the set depends on WHICH ALGORITHM IS CREATING THE DOCUMENT — §7.3.2.1's create
   hands
   the initial about:blank the creation flags alone, §7.5.1's create-and-initialize hands a navigated Document
   §7.4.5's union — so the caller states the whole set and the builder never re-derives it. */
/* THE CONTAINER'S CSP LIST CARRIES CSP §2.2's SELF-ORIGIN, which is why a builder is never asked to derive
   one: §2.2 makes a list a struct of policies AND an origin, and the origin is the half the bytes do not
   contain (§2.2.2 states it from the RESPONSE's URL, while §7.3.2.1's clone hands the CREATOR's to a document
   that came from no response). A builder that answered it from `origin` would be right for an unsandboxed
   document loaded from its own address and wrong for both of those. */
/* `kind` is DOM §4.5 "Interface Document"'s TYPE AND CONTENT TYPE for the Document being built — HTML
   §7.5.1's two creation arguments, chosen by §7.4.5's load-a-document arm for a response and by §7.3.2.1
   "Creating browsing contexts" for the initial `about:blank`. A builder NEVER derives it: the arm is a fact
   about the response, which the builder does not have, and a builder that answered "text/html" would be
   stating the one thing every Document in this engine used to say whatever had been fetched. */
/* `permissions_policy` is PERMISSIONS POLICY §9.1's TWO RESPONSE HEADER FIELD VALUES, which HTML §7.5.1 runs
   "creating a permissions policy from a response" over. A builder NEVER derives them and there is nothing it
   could derive them FROM: they are a fact about the response, which the builder does not have, and §9.5's
   «[], []» — what a builder would fall back to — is the MOST PERMISSIVE declared policy there is, so a builder
   guessing here would answer `Enabled` for every feature the framed server withheld. */
typedef JSContext *(*RealmBuilder)(JSRuntime *rt, lxb_html_document_t *dom, const char *url,
                                   const char *top_level_url, const char *origin, DocumentKind kind,
                                   SerializedPolicyContainer policy,
                                   SerializedResponsePermissionsPolicy permissions_policy,
                                   SandboxFlags sandbox_flags,
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
   for the same reason `policy` is and answering a question the container cannot: §7.3.2.1's create gives
   the initial
   about:blank the navigable's CREATION sandboxing flags, and §7.4.5's navigation gives its Document the UNION
   of those and the response policy's CSP-derived flags. */
/* `about_base_url` is HTML §7.4's ABOUT BASE URL for the Document this builds — `creatorBaseURL` for §7.2's
   initial `about:blank`, and §7.4.5's INITIATOR base URL for a navigation whose destination is an `about:`
   URL. NULL for every Document that comes from a response, which is §2.4.3's null and the ordinary case. It is
   a parameter and not a read off anything, for the reason `policy` beside it is: whose base URL it is belongs
   to the OPERATION (the creator's for a create, the initiator's for a navigation) and never to the navigable
   being filled. */
/* `content_type` is the RESPONSE's `Content-Type` value as Fetch §2.2.2's `get` joined it, and the ONE thing
   read off it here is HTML §13.2.3.2 "Determining the character encoding"'s transport-layer charset (through
   Fetch §3.5's legacy extract an encoding). It is NOT what decides which parse this Document gets — that is
   `computed_type` below, and the two are read by different standards from the same header. It rides beside
   `body`/`body_len` because it is a fact about the same response and about nothing else: the navigable cannot
   answer it, and a builder that guessed it would decide a document's encoding from nothing. NULL means THERE
   WAS NO RESPONSE: §7.2's initial `about:blank`, or an address whose fetch failed. That is not "charset
   unknown" — those Documents get DOM §4.5's utf-8 default — and it is a different fact from a response that
   carried no `Content-Type` header, which has bytes and reaches §13.2.3.2's other steps. */
/* `computed_type` is §7.4.5's "the COMPUTED TYPE of navigationParams's response" — MIME Sniffing §7's answer
   for this response, computed by the caller because that is where the response is. It is a SEPARATE parameter
   from `content_type` beside it because the two are different facts read by different algorithms: the header
   VALUE is what HTML §13.2.3.2 asks for a charset (through Fetch §3.5's legacy extract an encoding), and the
   COMPUTED type is what decides which parse the Document gets. Deriving the second from the first at the
   builder is what stood here, and it made a response with no `Content-Type` — the ordinary case for a server
   handler that sets no header — an untyped one that crashed instead of being sniffed.
   NULL means THERE WAS NO RESPONSE, in exact step with `body`: §7.2's initial `about:blank` and an address
   whose fetch failed have nothing to compute a type from and are HTML by §7.4. A response that carried no
   `Content-Type` HAS bytes and reaches this with a computed type like any other, because §7 always produces
   one. The pairing is asserted rather than trusted. */
JSContext *navigable_realm(JSContext *ctx, uint32_t doc, const char *url, const char *top_level_url,
                           const Origin *origin, JSValueConst nav_proxy, const char *body, size_t body_len,
                           const char *content_type, const MimeType *computed_type,
                           SerializedPolicyContainer policy,
                           const char *about_base_url, SandboxFlags sandbox_flags);

/* THE AGENT'S HALF: §7.4's `open` member, declared once. */
void navigable_init(JSContext *ctx);

/* Install §7.4's scriptable entry point — `window.open`. The origin an initial about:blank child inherits is
   THE AGENT'S (origin_agent), so it is not passed per document: an instance is an origin-keyed agent cluster,
   and a per-document principal would be a second answer to a question the agent already answers. `origin` is
   the serialization the host stated for this document, and it is here to be CHECKED against that one answer. */
void navigable_install(JSContext *ctx, JSValueConst global, const char *origin);

/* §7.4 STEP 14's NAVIGATE over a navigable that already has an active document — fetch the new document, build
   the realm its scripts run in, and hand both to §7.2.3's replace. Answers the SAME proxy (owned), because a
   navigation does not make a new one — that is the whole reason WindowProxy exists — or JS_UNDEFINED when the
   address does not parse, which §7.4 turns into a SyntaxError at the call site. */
/* EVERY NAVIGABLE OF THIS AGENT, IN TREE ORDER — a container before what it contains, siblings in the tree
   order of their navigable containers. As an Array of WindowProxy (owned).
 *
 * "OF THIS AGENT" IS THE BROWSING CONTEXT GROUP'S TOP-LEVEL TRAVERSABLES AND THEIR TREES, NOT ONE TREE. HTML
 * §8.1.1 gives a similar-origin window agent ONE event loop, and all three consumers of this list are facts
 * about that loop rather than about a document's frames — §8.1.7.3 step 2's document list, §8.7's timer task
 * source, §13.2.7's per-document load lifecycle. An AUXILIARY navigable (§7.3.1.7 step 8's, what
 * `window.open()` makes) is its OWN top-level traversable, reachable from the group and from no tree, so a
 * single-tree answer left every popup out of all three at once: its document stayed at "loading" for ever, with
 * no DOMContentLoaded, no `load`, no `pageshow`, no timers and no rendering opportunities.
 *
 * IT IS HERE BECAUSE THE TREE IS, and a second copy of this walk is the second answer that is always subtly
 * wrong: nav_find_in_tree below is a third, breadth-first, because §7.1 wants the NEAREST match rather than the
 * outermost. Each caller applies its OWN filters to this list; what they share is the ORDER, which is the
 * spec's and not theirs.
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

/* HTML §7.4.3 "Reloading and traversing"'s RELOAD, over the navigable whose ACTIVE DOCUMENT is this realm's.
 *
 * IT IS NOT "NAVIGATE TO MY OWN ADDRESS", and the difference is not academic — it is why this is its own
 * algorithm and not a call to navigable_navigate above with `document_url`. Three of its steps say so:
 *   — ITS DESTINATION IS THE ACTIVE SESSION HISTORY ENTRY'S URL, not the Document's. Those are two fields with
 *     two writers (core/frame/session_history.h), and §7.4.4's URL and history update steps move one without
 *     the other in the general case; reading the wrong one is how a reload would fetch an address the page
 *     never asked for.
 *   — §7.4.2.2 STEP 11'S FRAGMENT ARM MUST NOT RUN. `https://x/y#a` reloaded satisfies every conjunct of
 *     that test — the destination equals the active entry's URL with exclude fragments set to true and its
 *     fragment is non-null — so routing a reload through navigate would answer `location.reload()` on any
 *     page with a fragment by firing `popstate` and fetching nothing at all. §7.4.3 has no same-document arm.
 *   — ITS NAVIGATE EVENT'S NavigationType IS "reload", which a router's own `navigate` listener branches on,
 *     and its destinationNavigationAPIState is the ENTRY's rather than the wrapper's default.
 *
 * IT IS A STEP MACHINE BECAUSE ITS FIRST STEP RUNS THE PAGE'S CODE. Step 1 fires a push/replace/reload
 * navigate event at the Navigation, which any `navigate` listener may cancel with `preventDefault()`, and a
 * cancelled reload fetches nothing. So `location.reload()` suspends inside its own call, siblings run, and it
 * resumes — the same conversion core/frame/location.c's setters and its assign/replace already took.
 *
 * WHOSE NAVIGABLE IT IS, ANSWERED BY THE REALM AND NEVER BY A PARAMETER. §7.4.6.1 says a reload "is always
 * treated as if it were done by the navigable itself, EVEN IN CASES LIKE parent.location.reload()", which is
 * also what makes reading the initiator's own facts here correct rather than the §scheduler violation it would
 * be for a navigation: for a reload the operation's initiator IS the target. And a Location's members run in
 * the realm that DEFINED them (core/frame/location.c's loc_assert_this_realm), so `parent.location.reload()`
 * already arrives in the parent's realm — `ctx` is the target navigable's active document either way.
 *
 * WHAT ITS LAST STEP COLLAPSES TO, and the one thing it costs, is stated at the enqueue in navigable.c and
 * asserted there rather than described here. */
typedef struct {
    uint8_t stage;                /* NAV_RELOAD_STAGES in navigable.c */
    /* §7.4.3 step 1.4's destinationURL — the ACTIVE SESSION HISTORY ENTRY's URL, taken WITH the operation as a
       string that parks. Between step 1's dispatch and step 4's job every `navigate` listener the page has
       runs, and any of them may push an entry or navigate; a machine that re-read the entry on resume would
       reload whatever a listener left behind. */
    JSValue url;
    NavigateEventFireWork fire;   /* step 1 */
} NavigableReloadWork;

void navigable_reload_start(NavigableReloadWork *w);
void navigable_reload_visit(JSContext *ctx, NavigableReloadWork *w, JSStepVisit *v);
/* §7.4.3 step 1's values, read at the moment the operation is CREATED. `userInvolvement` is "none" and
   `navigationAPIState` and `apiMethodTracker` are null — the defaults §7.2.4's `reload()` calls this with, and
   its only caller — so step 1's "if userInvolvement is not `browser UI`" is taken and step 1.3 leaves
   destinationNavigationAPIState at the active entry's state. Each becomes a parameter with the caller that
   passes something else, which is §7.2.6.7's `navigation.reload()`. */
void navigable_reload_begin(JSContext *ctx, NavigableReloadWork *w);
/* THE ALGORITHM, DRIVEN. JS_STEP_CALL/JS_STEP_YIELD = return it, 0 = §7.4.3 has finished (its load is
   enqueued, or a `navigate` listener cancelled it — the caller cannot tell those apart and must not, because
   §7.2.4's `reload()` has no return value and a cancellation is not an error), JS_STEP_ABRUPT = it threw. */
int navigable_reload_run(JSContext *ctx, NavigableReloadWork *w, JSValue in, JSValue **out_cb, int *out_argc);

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

/* HTML §7.3.1.7 "Navigable target names" — THE RULES FOR CHOOSING A NAVIGABLE'S **SECOND** RETURN VALUE.
 *
 * "Let targetNavigable and windowType be the result of applying the rules for choosing a navigable" is
 * §7.2.2.1's step 12, and §4.6.5 step 6 says the other half out loud — "let targetNavigable be THE FIRST
 * RETURN VALUE of applying the rules". Two values, and which of the two callers reads the second is the whole
 * shape of what happens next, so it is not an implementation detail this file may drop.
 *
 * WHAT DROPPING IT COST, because it was dropped and the price was three defects wearing three names.
 * §7.2.2.1 splits on this value: step 15 is the "new*" arm and step 16 the "existing or none" arm, and step 16
 * is where BOTH of the things an existing navigable needs live — 16.1's navigate, which happens only "if
 * urlRecord is not null", and 16.2's opener link, which has no counterpart in step 15 at all because a created
 * navigable got its opener from its create. With one unconditional navigate standing in for both arms:
 * `open("", name)` navigated where 16.1 does not; `open(url, "<existing name>")` never linked the opener; and
 * step 17's return, which asks this value FIRST ("if windowType is 'new with no opener', then return null")
 * before its noopener clause — a clause that EXCLUDES `_self`, `_parent` and `_top` — returned null for
 * `open(url, "_self", "noopener")`, a call that must answer with the WindowProxy of the navigable it just
 * navigated. One missing return value, three wrong answers, none of which looks like the others.
 *
 * THIS BUILD'S RULES PRODUCE TWO OF THE THREE. `new with no opener` is set by §7.3.1.7's opener-policy clause
 * — "if currentDocument's opener policy's value is 'same-origin' or 'same-origin-plus-COEP', and
 * currentDocument's origin is not same origin with its relevant settings object's top-level origin" — which
 * this build does not evaluate. That is a NAMED subproblem and not a silent omission: navigable_open crashes
 * where the clause belongs rather than quietly answering `new and unrestricted` for a COOP document, so the
 * value cannot be wrong without saying so. Step 17 handles all three regardless, because the arm is the
 * spec's and reading it off a value that cannot yet arrive costs nothing. */
typedef enum {
    /* §7.3.1.7 step 2's INITIAL VALUE — the rules answered with a navigable that ALREADY EXISTED (steps 4-7:
       the empty name, `_self`, `_parent`, `_top`, or a find-by-target-name hit). §7.2.2.1 step 16 is its arm. */
    WINDOW_TYPE_EXISTING_OR_NONE = 0,
    /* §7.3.1.7 step 8's third option — a new top-level traversable was created. §7.2.2.1 step 15 is its arm,
       and in this engine step 15's navigate is already done by the create (see navigable_open). */
    WINDOW_TYPE_NEW_AND_UNRESTRICTED,
    /* §7.3.1.7 step 8's opener-policy clause. NOT PRODUCED BY THIS BUILD — see above. */
    WINDOW_TYPE_NEW_WITH_NO_OPENER,
} WindowType;

/* HTML §7.3.1.7 "Navigable target names" — THE RULES FOR CHOOSING A NAVIGABLE, and NOTHING AFTER THEM.
   It used to be the rules PLUS an unconditional navigate, which is §4.6.5 step 9's tail and is the WRONG tail
   for §7.2.2.1 — see WindowType above for what that cost. Each caller now runs its own steps over the two
   values this answers with. TWO callers, and they are not variants of each other: §7.2.2.1's window open steps
   reach it after parsing a features string, and §4.6.5's FOLLOWING A HYPERLINK reaches it from an `<a>`'s
   activation behaviour with `noopener` read off `rel`. The rules for choosing a navigable are ONE algorithm; a
   second copy in the hyperlink path would be the second answer that is always subtly wrong.
   `target` IS THE TARGET ITS OWN ALGORITHM PRODUCED — the string the page passed to `window.open`, or the
   result of §4.2.3's GET AN ELEMENT'S TARGET — and NULL or the empty string means "no target", which §7.3.1.7
   step 4 answers with the CURRENT navigable. A caller wanting the other meaning states it: §7.2.2.1 step 5
   maps an empty target to "_blank" before applying these rules, and that mapping lives at that caller because
   it is that algorithm's step and not this one's.
   `source_element` IS §4.6.5 step 11's sourceElement — the `a`, `area` or `form` this navigation came from,
   and NULL for a script-initiated one. IT IS NOT DECORATION AND IT IS NOT OPTIONAL. §7.3.1.7's rules take any
   string, so the ONE difference between the two callers is invisible in the name they hand over: an element's
   target has passed §4.2.3 step 2's dangling-markup reset and `window.open`'s deliberately has not
   (`dangling-markup-window-name.html` asserts both halves). Without the element there is nothing here to assert
   against, and a call site that reads a `target` attribute raw joins the algorithm silently — which is how a
   window came to be named after smuggled markup. With it, that call site CRASHES.
   `out_window_type` receives §7.3.1.7's SECOND return value and is NOT optional — a caller that does not read
   it is a caller running one arm of a two-arm algorithm, which is the state this parameter exists to end.
   AND THE CONTRACT ON `url` IS ASYMMETRIC, WHICH THE CALLER MUST KNOW RATHER THAN GUESS. An EXISTING navigable
   is chosen and NOT navigated: §7.2.2.1 step 16.1 and §4.6.5 step 9 are the caller's own steps and they do not
   agree (16.1 skips an empty url; step 9 does not), so doing it here would be one of them imposed on both — the
   defect this split repaired. A NEW navigable HAS already been navigated to `url`, by §7.4 step 14 inside
   navigable_create, which is where this engine's create puts it; a caller that navigates it again loads the
   address twice into two documents of one navigable. The asymmetry is stated because it is real, and it is the
   next thing to remove: §7.3.1.7 step 8's create takes no url at all, and step 15's navigate belongs to
   §7.2.2.1 exactly as 16.1's does.
   Answers the chosen navigable's WindowProxy (owned), or JS_UNDEFINED when the url does not parse — which
   §7.2.2.1 turns into a SyntaxError and §4.6.5 discards, because a click is not a place a page can catch one. */
JSValue navigable_open(JSContext *ctx, const char *url, const char *target, const WindowFeatures *feat,
                       lxb_dom_node_t *source_element, WindowType *out_window_type);
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
/* `container` is HTML §7.3.1.3 "Child navigables"' NAVIGABLE CONTAINER — the ELEMENT wrapper this navigable is
   presented by, which create-a-new-child-navigable is handed by name ("To create a new child navigable, given
   an element element") and links in one of its own steps. JS_NULL for §7.3.1.7 step 8's AUXILIARY navigable,
   which no element presents.
   IT IS AN ARGUMENT AND NEVER A LATER SETTER, for the reason every other creation fact on this seam is one:
   §7.3.1.3 defines the container as the element whose content navigable is this navigable, so the only place
   the answer exists without a search down a document tree is the algorithm that was given the element. A create
   that recorded nothing would leave §7.2.2.4's `frameElement` to hunt for its own answer, and a navigable
   materialized in a flow that never walked that tree would not have one to find.
   THE PAIRING WITH `is_child` IS ASSERTED, so a child navigable cannot be created with no element to present
   it and an auxiliary one cannot be created with one. */
JSValue navigable_create(JSContext *ctx, const char *url, const char *name, bool is_child,
                         const WindowFeatures *feat, SandboxFlags iframe_sandbox_flags,
                         JSValueConst container);

/* THE NAVIGABLE AN INSTANCE IS ROOTED IN — the one navigable_create above did NOT make, because it was made in
 * another instance or by the browser itself — together with HTML §7.1.4.2 "Embedder policy checks" for the
 * response it is being rooted with. It is here rather than in a host because there are THREE hosts and this is
 * one rule: a copy per host is three rules waiting to disagree, which is the same reason §7.1.7's
 * determine-navigation-params-policy-container is called by each of them and restated by none.
 *
 * `parent_navigable` IS HTML §7.3.1.3 "Child navigables"' PARENT, AS TEXT. The section defines the term over
 * the link and not over a property — a navigable "is a child navigable", "which means that its parent is
 * non-null" — so a host with nothing to say here is not omitting a field, it is declaring a TOP-LEVEL
 * TRAVERSABLE. SECURITY.md makes a cross-origin document a separate INSTANCE, so the commonest child navigable
 * there is (a cross-origin `<iframe>`) is the ROOT of a peer instance, and a peer that declared itself
 * top-level answered §7.1.4.2 step 1, §7.2.2.4's `parent`/`top` and §7.5.9/§7.5.10's subtree walks with four
 * plausible facts about a page that does not exist — never a crash.
 * IT CROSSES AS core/frame/remote_object.h's NAVIGABLE IDENTITY and never as a document name, because the
 * receiving instance holds no proxy for that navigable and a name is not enough to MINT one (remote_object.c:
 * a minted parent needs its own origin, name, parent and opener). `u` is that grammar's undefined and is the
 * POSITIVE statement that this navigable has no parent.
 *
 * `inherited` IS §7.1.7's CLONE THE HOST WAS HANDED and `response_embedder` IS §7.1.4's ITEM OBTAINED FROM THIS
 * DOCUMENT'S OWN RESPONSE — the two operands of §7.1.4.2's steps 3-6. Its steps 1 and 2 are the caller's
 * (core/frame/embedder_policy.h), and here step 1 is the parent above while step 2 — "navigable's CONTAINER
 * DOCUMENT's policy container's embedder policy" — is `inherited`. THAT SUBSTITUTION IS PROVABLE RATHER THAN
 * CONVENIENT: §7.3.1.3's create-a-new-child-navigable creates the browsing context and document "given
 * element's node document, element, and group", so the CREATOR whose container was cloned onto the record that
 * provisioned this instance IS "navigable's container's node document" — one Document, named two ways. What
 * embedder_policy.h warns against is a LOAD JOB's INITIATOR container, which is a third document the moment
 * `frames[0].location = …` runs, and no record provisions an instance for that navigation. A live read is not
 * an alternative to prefer: the container document is in another instance and §Security makes a cross-instance
 * read a SUSPEND POINT, which a rooting entry has no flow under it to suspend.
 *
 * AND `creation_sandbox_flags` IS HTML §7.1.5 "Sandboxing"'s SET FOR THIS SAME NAVIGABLE — a FOURTH statement
 * the host makes about it, in the class of the parent above rather than of the container beside it. §7.1.7's
 * policy container holds a CSP list, an embedder policy, a referrer policy and two integrity policies and no
 * sandboxing flag set, and §7.3.2.1 sets the two from different algorithms in different steps. §7.1.5's own
 * inputs are the embedder ELEMENT's iframe sandboxing flag set and that element's node document's active set,
 * both in the instance that CREATED the navigable, so this instance can derive it no more than it can derive
 * its parent — which is why the two travel together on the `navigable.create` notice. It is what §7.4.2.1's
 * snapshot of this navigable's target snapshot params answers with at every navigation the instance performs
 * from its own root. */
JSValue navigable_root(JSContext *ctx, uint32_t doc, const char *name, OpenerPolicyValue opener_policy,
                       const char *parent_navigable, SerializedPolicyContainer inherited,
                       const EmbedderPolicy *response_embedder, SandboxFlags creation_sandbox_flags);

/* AND HTML §7.3.1.3 "Child navigables"' OTHER LINK FOR THAT NAVIGABLE — its CONTAINER, which is the element
 * whose content navigable it is. For the navigable an instance is ROOTED in, that element is in the CREATING
 * instance's tree, so what crosses is not the element but what it ANSWERED: Permissions Policy §9.5's result,
 * as core/permissions_policy/permissions_policy.h serializes an inherited policy, or that grammar's
 * PERMISSIONS_POLICY_SERIALIZED_NO_CONTAINER for a navigable that has no container at all.
 *
 * IT IS A SECOND STATEMENT AND NOT AN ARGUMENT TO THE ROOTING ABOVE, WHICH IS §7.3.1.3'S OWN ORDER. The
 * section creates a navigable and links it afterwards — "Let navigable be a new navigable … Set element's
 * content navigable to navigable" — which is exactly the order core/frame/navigable.c's create performs for a
 * navigable of THIS agent (window_proxy_set_container runs after the mint). A host rooting an instance stands
 * at the same point, one boundary away, and states the same link. Both statements come from the zone that
 * ROUTED the create, because it is the only party that can see above this instance at all.
 *
 * IT MUST BE MADE BEFORE THE FIRST DOCUMENT OF THAT NAVIGABLE IS INSTALLED, because §9.5 runs once per
 * Document a navigable is given (core/dom/document.c) and reads it there. A host that skips it does not get a
 * permissive default: that install CRASHES, naming this call.
 *
 * WHY THE ANSWER RATHER THAN §9.7's INPUTS, why a live read is not the alternative, and what the two links
 * assert about each other: core/frame/window_proxy.h's window_proxy_set_remote_container and the definition. */
void navigable_root_container(JSContext *ctx, JSValueConst proxy, const char *container_policy);

/* AND HTML §3.1.3 "Ancestor origins"' INTERNAL ANCESTOR ORIGIN OBJECTS LIST FOR THAT NAVIGABLE'S DOCUMENT,
 * composed by the instance that holds the ancestors and carried as text in the form core/dom/document.h
 * serializes one, or that grammar's DOCUMENT_ANCESTOR_ORIGINS_SERIALIZED_NONE for a Document with no
 * container document at all.
 *
 * IT IS A THIRD STATEMENT AND NOT A DERIVATION OF THE FIRST TWO, which is the whole reason it is its own call.
 * §3.1.3's steps read the PARENT DOCUMENT's own recorded list, that Document's ORIGIN RECORD, and the
 * CONTAINER ELEMENT. The parent statement above carries a navigable IDENTITY, which names a navigable and says
 * nothing about a Document's recorded ancestry; the container statement carries Permissions Policy §9.5's
 * answer, which is a different algorithm over two of the same inputs. Neither yields this one.
 *
 * WHY THE ANSWER RATHER THAN §3.1.3's INPUTS. Two of the three cannot cross at all — an element is an object,
 * and an ORIGIN RECORD is precisely what a serialization drops, since §7.1.1 decides an opaque origin by
 * IDENTITY and every opaque origin is the three bytes `null`. Step 10 compares an ancestor against "parentDoc's
 * origin", so a peer running the steps over carried text would mask an ancestor that is not the parent's
 * whenever the parent is opaque — and that is the ORDINARY case here, not a corner: a `data:` iframe is in a
 * peer instance BECAUSE its origin is opaque. A LIVE read is not the alternative either, for the reason the
 * container's is not: §Security makes a cross-instance read a suspend point and a Document's install has no
 * flow under it to suspend.
 *
 * IT MUST BE MADE BEFORE THE FIRST DOCUMENT OF THAT NAVIGABLE IS INSTALLED, because §7.3.2.1 runs §3.1.3's
 * steps once per Document a navigable is given and reads it there. A host that skips it does not get an empty
 * list — which would be the positive claim that this Document is top-level, invisible to any page: that
 * install CRASHES, naming this call.
 *
 * WHERE THE PAIRING WITH §7.3.1.3's PARENT IS ASSERTED, and what each direction of a disagreement would mean:
 * the definition. */
void navigable_root_ancestor_origins(JSContext *ctx, JSValueConst proxy, const char *ancestor_origins);

#endif
