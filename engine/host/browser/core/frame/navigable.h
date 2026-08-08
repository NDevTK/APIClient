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

/* THE NETWORK HALF — declared by the HOST. Returns the address's bytes (malloc'd, caller frees) and their
   length, or NULL when the fetch fails; a child whose address does not load keeps the initial about:blank
   Document, which is what a browser showing an error page still has.
   SYNCHRONOUS, WHICH IS WHY THE PRODUCT HOST HAS NONE. Its network is the trusted zone's and every request
   parks the running flow on a host-owed answer, so it cannot answer here at all — §7.4's navigate has to become
   a scheduled work item that resumes when the response lands, replacing the navigable's active Document. That
   is the mechanism this signature is the placeholder for, and the DFAIL below names it. */
typedef char *(*DocumentFetcher)(JSContext *ctx, const char *url, size_t *plen);
void navigable_set_document_fetcher(DocumentFetcher f);

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
typedef JSContext *(*RealmBuilder)(JSRuntime *rt, lxb_html_document_t *dom, const char *url, const char *origin,
                                   uint32_t doc_id, JSValueConst nav_proxy);
void navigable_set_realm_builder(RealmBuilder b);

/* BUILD THE REALM OF A SAME-ORIGIN NAVIGABLE THIS AGENT HOLDS, and hand the caller the reference.
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
JSContext *navigable_realm(JSContext *ctx, uint32_t doc, const char *url, const char *origin,
                           JSValueConst nav_proxy);

/* Install §7.4's scriptable entry point — `window.open`. `origin` is this document's, which the initial
   about:blank child inherits along with the policy container. */
/* THE AGENT'S HALF: §7.4's `open` member, declared once. */
void navigable_init(JSContext *ctx);

void navigable_install(JSContext *ctx, JSValueConst global, const char *origin);
void navigable_free(JSContext *ctx);

/* §7.4's CREATE A NEW NAVIGABLE. `url` is the child's initial address; NULL, "" or "about:blank" all mean the
   initial about:blank Document, which inherits this document's origin and policy container. Returns the child's
   WindowProxy, or JS_UNDEFINED when `url` does not parse — the caller decides what that means, because §7.4
   throws a SyntaxError for it where §4.8.5 does not. */
/* `name` is the browsing context name to give it (an iframe's `name` attribute, §7.4's target), or NULL.
   `is_child` distinguishes §4.8.5's CHILD navigable — nested in this one, so its `parent` is this Window — from
   §7.4's AUXILIARY one, which is its own top and links back through `opener`. */
JSValue navigable_create(JSContext *ctx, const char *url, const char *name, bool is_child);

#endif
