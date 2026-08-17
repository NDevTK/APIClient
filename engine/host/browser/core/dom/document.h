/* The Document interface — Blink core/dom. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_H
#include <lexbor/html/html.h>
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "core/idl_args.h"
#include "core/frame/policy_container.h"

/* Install `document` for `dom`, addressed at `url`. THE REALM IS THE DOCUMENT: `ctx` is what this document's
   state hangs off from here on, so a second same-origin document in the same agent is a second JSContext in the
   same JSRuntime and not a second instance. Only the members this engine can answer TRUTHFULLY are
   installed; the tree-walking half is absent until Element exists, because a querySelector that answers null
   for an element the document HAS is a lie, and a lie is worse than a ReferenceError that names the gap. */
/* THE AGENT'S HALF: Document.prototype, its members and its mixins. A member is declared once per agent; a
   second realm installs `document` from the same declarations. */
void document_init(JSContext *ctx);
/* §3.1.1's prototype for ONE realm — declared into core/realm.h's list. */
void document_install_proto(JSContext *ctx);

/* `nav_proxy` is §7.2.3's ONE WindowProxy for the navigable this realm is the active document of. The
   navigable exists before its realm — §7.4 created it and handed its proxy to the page — so the caller that
   owns it passes it in rather than a second one being minted here. */
/* `csp` is the policy text this document was CREATED with — the `Content-Security-Policy` the response carried,
   or §7.4's clone of the creator's for a document that came from no response (`about:blank`). NULL when there
   is neither. It is not an optional extra: CSP §3.3's `<meta>` is only HALF of §7.2.6's container, and a
   document whose created-with policy is dropped reports a sink as exploitable that the real page's CSP kills. */
/* `csp_self_origin` is CSP §2.2's SELF-ORIGIN for that policy list, SERIALIZED — §2.2.2's "response's URL's
   origin" for a document built from a response, and §7.4's clone of the CREATOR's for one built from none.
   It is a separate argument from `csp` for the reason `origin` is separate from `url`: a Document's own
   principal and the origin its policy resolves `'self'` against are two facts, and §2.2's own note exists to
   say they differ (a document with an opaque origin which inherited its policy still resolves `'self'`
   against the origin the policy came from). It travels as BYTES because that is how a policy container
   travels, and the identity a serialization drops decides nothing here: both bullets of §6.7.2.8's `'self'`
   arm read components an OPAQUE origin has none of, so an opaque self-origin matches nothing whether or not
   two documents' copies of it are the same record. Never NULL — every document's policy has an origin to be
   measured against, even when there is no policy. */
/* `sandbox_flags` is §7.1.5's ACTIVE SANDBOXING FLAG SET this Document is CREATED with, and it is a SEPARATE
   argument from `csp` because §7.1.7's policy container has no such field — the two are separate items of the
   Document in §7.5.1's own creation table ("policy container … navigationParams's policy container; active
   sandboxing flag set … navigationParams's final sandboxing flag set"). The caller states the whole set,
   because only the caller knows which algorithm is running: §7.2's create-a-new-browsing-context-and-document
   gives the initial about:blank the navigable's CREATION sandboxing flags alone, while §7.5.1 gives a
   navigated Document §7.4.5's union of those and the response policy's CSP-derived flags. */
void document_install(JSContext *ctx, JSValueConst global, lxb_html_document_t *dom, const char *url,
                      const char *csp, const char *csp_self_origin, SandboxFlags sandbox_flags,
                      uint32_t doc_id, JSValueConst nav_proxy);

/* WHICH DOCUMENT THIS REALM IS, in the world registry's naming. §7.4 mints a child's name from it, so a
   same-origin child of a child is named from the child and not from the instance root. */
uint32_t document_doc(JSContext *ctx);

/* THE LOAD LIFECYCLE, PER DOCUMENT — see the definition. Advances ONE document of this agent by one stage
   (DOMContentLoaded in tree order, then `load` innermost-first) and returns 1, or 0 when every document is
   complete. WHEN a document has finished loading is the SCHEDULER's answer — it is the flow that knows it has
   run everything the documents gave it — so this is registered as that hook; it is declared here as well
   because a host that drives its own pump has the same lifecycle to run, and a host that runs NONE of it has
   documents that are render-blocked for ever. */
int document_lifecycle_step(JSContext *ctx);

/* HTML §8.1.7.3 "update the rendering" step 3's render-blocked clause — see the definition. True while this
   document's parser has not finished, which is `readyState === "loading"`. */
bool document_render_blocked(JSContext *ctx);

/* HTML §3.1.5's CURRENT DOCUMENT READINESS of ONE DOCUMENT, as the string `readyState` answers with —
   "loading", "interactive" or "complete". Asked of this component because it owns the load lifecycle that
   moves it; the member itself is core/dom/document_metadata.c's, and there is exactly one statement of the
   fact so the two cannot disagree. A Document that is no realm's ACTIVE document — a createHTMLDocument, a
   DOMParser parse, an XHR responseXML — is "complete", which is §3.1.5's own initial value. BORROWED. */
const char *document_readiness_of(const lxb_dom_node_t *doc);

/* HTML §7.5.9's PAGE SHOWING for THIS realm's Document — "initially false", set true by §13.2.7's "the end"
   when it fires `pageshow`, and read-then-cleared by §7.5.9 step 9, which fires `pagehide` only if it is true.
   The pair is what stops a page seeing two pagehides with no pageshow between them, so the flag is the
   CONDITION on both fires rather than bookkeeping beside them. Per-realm and per-flow — see the definition. */
bool document_page_showing(JSContext *ctx);
void document_page_showing_set(JSContext *ctx, bool showing);

/* §7.2.3's ONE WindowProxy for THIS realm's navigable — what `window.closed` reads the navigable's state
   through and what every message this document posts carries as `source`. BORROWED. It lives on the realm
   because it is one PER realm; a registry keyed by document would be an immortal root holding a proxy for
   every navigable a forced-execution frontier ever created. */
JSValueConst document_window_proxy(JSContext *ctx);

/* HTML §7.3.1 "fully active" for THIS realm's Document — the guard a family of algorithms opens with, and the
   reason a detached iframe's Observable pushes nothing. See the definition for why it is a walk and not a
   flag. */
bool document_fully_active(JSContext *ctx);

/* THIS REALM'S `document` OBJECT. BORROWED. §4.8.5's `contentDocument` for a SAME-ORIGIN child answers with
   exactly this object out of the child's realm — the two documents are one agent, so it is a pointer and not
   a message. */
JSValueConst document_object(JSContext *ctx);
/* §4.4 baseURI's answer: the document's address. ONE component owns what this document's URL is — two answers
   to that question is how they drift apart. */
const char *document_base_url(JSContext *ctx);
/* HTML §3.1.1's "the encoding" of this realm's active document, as an id in the Encoding registry
   (core/encoding/encoding.h). It is what HTML §4.12.1 falls back to when a `<script>` carries no `charset`
   attribute — "let encoding be el's node document's the encoding" — and therefore what HTML §8.1.4.2's fetch a
   classic script decodes a fetched body with when the response names no charset of its own. ONE component owns
   it, exactly as one owns the document's address. */
int document_encoding(JSContext *ctx);
/* HTML's SET THE URL — what HTML §7.4.4's URL and history update steps step 8 performs, and the only way a
   Document's address changes without a new Document being installed. PER FLOW: the address rides the running
   flow's COW delta, so the arm that called `history.pushState(s, "", "/b")` is the only one whose
   `location.pathname` is `/b`. See the definition for why the capture is the POD one. */
void document_set_url(JSContext *ctx, const char *url);

/* §4.2.3's STEPS RUN IN THE NODE'S DOCUMENT'S REALM — see document.c. `document_realm_of` answers NULL for a
   document no record was ever built for (a solver scratch parse), which is a caller's business to assert. */
JSContext *document_realm_of(const lxb_dom_node_t *n);
/* AND THE REALM WHOSE **ACTIVE** DOCUMENT `doc` IS — a different question, and the one every algorithm stated
   over "the active document of a navigable" is asking. A realm can hold SEVERAL Documents: `createHTMLDocument`,
   a DOMParser parse and XHR's `responseXML` each build one, each gets a record, and each answers
   document_realm_of with the realm that created it — but none of them is that realm's active document, so none
   of them has a §6.6.2 focused area, appears in §6.6.7's autofocus candidates, or is §7.3.1 fully active. NULL
   is the real answer for those, and it is what makes `implementation.createHTMLDocument("").hasFocus()` false.
   `doc` must be a DOCUMENT node; anything else answers NULL. */
JSContext *document_active_realm_of(const lxb_dom_node_t *doc);
/* HTML's "the document's relevant global object" — what DOM §2.9's get the parent puts above a Document in the
   event path, and JS_NULL for a document with no browsing context. BORROWED: a realm owns its global. It is a
   fact about THAT DOCUMENT and not about whoever is running, which a caller reading the realm's global instead
   gets wrong exactly when two same-origin documents are one agent. */
JSValueConst document_window_of(const lxb_dom_node_t *n);
/* DOM §2.7's DEFAULT PASSIVE VALUE, the three parts of its target test that are §3.1.1/§4.5 lookups: is this
   node the document, its document element, or its body. */
bool document_is_passive_default_node(const lxb_dom_node_t *n);

/* §4.5's "DOCUMENT ELEMENT" and §3.1.1's `body`, AS FACTS ABOUT ONE DOCUMENT rather than about the running
   realm — the two lookups HTML §6.6.6's `activeElement` getter ends in (steps 5-6), asked of the component
   that already owns them so the focus model does not grow a second walk that disagrees at the edges (`body` is
   the first BODY *or FRAMESET* child of the document element, which is the edge a second walk gets wrong).
   NULL is a real answer for both: a frameset document has no body, and a `createDocument` with no qualified
   name has no document element. */
lxb_dom_node_t *document_document_element_of(const lxb_dom_node_t *doc);
lxb_dom_node_t *document_body_of(const lxb_dom_node_t *doc);

/* A SECOND DOCUMENT IN THIS REALM — what DOM §4.5.1's createDocument and createHTMLDocument return, and what
   HTML's `new Document()` builds. It has NO BROWSING CONTEXT: no navigable, no Window, no WindowProxy, and no
   scripts, which is why §3.1.1's `location` is null on it. It is NOT a second realm and NOT a second instance —
   HTML's similar-origin window agent is ONE HEAP, so the nodes it makes are ordinary objects of `ctx` with
   `ctx`'s prototypes, and none of navigable.c's or world.c's machinery is involved (both are about NAVIGABLES,
   which this has none of).
   `dom` is CONSUMED: whoever ends up owning the document destroys it. That is the running flow's COW delta when
   capture is on — a document a flow created dies with the flow, exactly like a node it created — and the REALM
   when it is off, since a creation made at baseline is baseline. Returns the document's wrapper, OWNED. */
JSValue document_new(JSContext *ctx, lxb_html_document_t *dom, const char *url, const char *content_type);

/* HTML §4.12.3's APPROPRIATE TEMPLATE CONTENTS OWNER DOCUMENT for `doc` — the inert Document a `<template>`'s
   contents belong to, so that a template's markup is NOT live: the owner has no browsing context, which is what
   the standard means by inert (its scripts do not run, its custom elements do not upgrade). `doc` itself when
   `doc` is a Document this algorithm created; otherwise `doc`'s associated inert template document, created on
   the first ask in `doc`'s RELEVANT REALM and remembered on it. Never NULL. The inert document is the REALM's —
   see the definition for why the flow's delta may not own it. */
lxb_html_document_t *document_template_contents_owner(JSContext *ctx, lxb_dom_document_t *doc);

/* §4.4 baseURI's answer FOR ONE NODE: its NODE DOCUMENT's address. Not the same as document_base_url the moment
   a second Document exists — that one is the REALM's active document, which is HTML's "API base URL" and is
   what a fetch, a hyperlink and a navigation resolve against. */
const char *document_url_of(const lxb_dom_document_t *dom);
/* §4.5's CONTENT TYPE — the string `document.contentType` answers, as a fact about ONE document rather than
   about the running realm. It is the field §4.4's "clone a single node" copies onto a Document's copy ("set
   copy's encoding, content type, URL, origin, type, mode, and allow declarative shadow roots to those of
   node"), and it is also what §4.5 makes an HTML document an HTML document rather than an XML one, so a copy
   that took the creator's default instead would answer for a document it is not a copy of. BORROWED: the
   record owns the bytes and outlives the tree they describe. */
const char *document_content_type_of(const lxb_dom_document_t *dom);

/* THE DOCUMENT IS ABOUT TO BE DESTROYED — release the record that names it, and everything the record holds
   (its wrapper, its DOMImplementation, its policy container). Called from the ONE place a document's lifetime
   ends, which is dom_cow's destroy, so a record cannot outlive its tree and a flow that created a document
   cannot leave one behind. A no-op for a document whose record is already gone. */
void document_record_release(lxb_html_document_t *dom);

/* §4.5's "INTERNAL createElementNS STEPS" — named as the spec names them because §4.5.1's createDocument step 3
   reaches them on a DIFFERENT document from the one whose implementation was asked. `doc` is that document's
   wrapper; `argv` is (namespace, qualifiedName). */
JSValue document_create_element_ns(JSContext *ctx, JSValueConst doc, int argc, JSValueConst *argv);
/* The parsed document's root node, for a component that walks the whole tree. NULL in a realm that has no
   document yet, which `window.document` is entitled to see. */
lxb_dom_node_t *document_root_node(JSContext *ctx);

/* DOM §4.9 "CREATE AN ELEMENT INTERNAL" — the half of element creation that runs NO page code: a node
   implementing the interface for `local`, in THIS REALM'S associated Document, in the HTML namespace,
   DETACHED, with an empty attribute list. HTML §4.13.2 step 7 is its second caller — a custom element
   constructor reached with an EMPTY construction stack makes the element itself, and step 7.2 names the
   current global's document rather than any receiver's, which is why this is the realm's and `createElement`'s
   own creation is the receiver's. Returns the wrapper, OWNED. */
JSValue document_create_element_internal(JSContext *ctx, const char *local, size_t len);

/* HTML §7.2.6's container for one document, from BOTH of its sources: the policy it was created with (above)
   and CSP §3.3's `<meta http-equiv="Content-Security-Policy">` the parsed tree carries. They are ONE POLICY
   LIST — every policy in the list is enforced and CSP §2.2 serializes a list with commas — so joining them
   cannot lose the narrower one, which a `;` join would (a repeated directive in one policy is ignored).
   The meta half is a REAL LEXBOR WALK rather than a regex over the source, for the reason the bundle id is a
   `<script>` scan: a `content` attribute is parsed markup by the time it is here, so entity decoding and
   quoting are the parser's answer rather than a second one. It lives on the DOM half because only that half
   may walk a tree, and it RETURNS the container rather than installing one so it is exercisable with a
   document of its own. `csp` may be NULL; `self_origin` may not, because CSP §2.2 gives every list one and
   §6.7.2.8 has no answer without it. OWNED by the caller. */
PolicyContainer *document_policy_new(lxb_html_document_t *dom, const char *csp, const Origin *self_origin);

/* THIS DOCUMENT'S POLICY CONTAINER — HTML §7.2.6, built at install from the above. Built at install rather
   than on demand because §7.4 clones it for an about:blank child at the moment that child is created, which
   may be before anything else has asked. BORROWED. */
const PolicyContainer *document_policy(JSContext *ctx);

/* HTML §7.1.5's ACTIVE SANDBOXING FLAG SET for this realm's Document — the set §7.2's create or §7.4.5's
 * navigation handed it, unchanged since.
 *
 * IT DOES NOT NEED A COW CAPTURE, AND THAT IS THE SPEC RATHER THAN AN OMISSION. §7.1.5 says a Document's
 * active sandboxing flag set is empty when the Document is created and is populated by the navigation
 * algorithm — no standard writes it again — so the field has exactly ONE write and a delta entry for it could
 * never hold two values. What makes the ANSWER per-flow is its INPUTS: an `<iframe sandbox>` attribute is a
 * DOM read in the creating flow's own delta, and the child navigable that read produced belongs to that flow,
 * so two flows that disagree about the attribute end up with two Documents holding two sets. The one thing
 * that could give a single navigable two sets is a NAVIGATION, which replaces the whole Document — and
 * document_install already crashes on a second install into one realm, naming the per-flow record to build;
 * whoever builds that record carries this field in it with everything else.
 *
 * ZERO IS A REAL ANSWER, never "not known yet": an unsandboxed Document has an EMPTY active sandboxing flag
 * set, which is what every top-level document without a CSP `sandbox` directive gets. */
SandboxFlags document_active_sandbox_flags(JSContext *ctx);
/* THE PER-REALM HALF OF THIS COMPONENT'S RELEASE — one realm's Document records: the wrapper, the Window it
   fires `load` at, the WindowProxy, §4.5's implementation, and every Lexbor tree that realm created at
   baseline. Reached once per realm, from quickjs's realm-teardown hook (core/frame/navigable.c) for a child
   navigable and by hand for the root one, and it reads NOTHING this file holds for the agent — see
   document_agent_free, which is why the two halves may run in either order. */
void document_free(JSContext *ctx);

/* THE AGENT HALF — core/platform.h's third column. `document` is the ONE component with both halves, and the
   paragraph in that header about a JSContext in a signature is about the OTHER one. This releases what a C
   static holds for the whole agent: §4.5's class, its fifteen member declarations, the two realm-value slot
   ids, the §13.2.7 lifecycle claim this component makes on the ONE frontier, and the ten sub-components
   document_init declares — three of which element_free's cascade was releasing on its behalf. */
void document_agent_free(JSRuntime *rt);

/* §4.2.6/§4.9's FOUR selector members as ONE declaration — querySelector, querySelectorAll, matches, closest.
   They differ in where the cursor goes and what a match yields, which is the magic: 0 = querySelector,
   1 = querySelectorAll, 2 = matches, 3 = closest. Installed by whoever owns the interface the IDL puts them on;
   the compiled selector and the walk live here, where the selector engine already did. */
const IdlStepDecl *document_qs_decl(void);

#endif
