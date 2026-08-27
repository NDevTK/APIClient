/* The Document interface — Blink core/dom. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_H
#include <lexbor/html/html.h>
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "core/idl_args.h"
#include "core/frame/policy_container.h"
#include "core/permissions_policy/permissions_policy.h"   /* §9.5's policy is a field of this record */

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
/* `policy` is HTML §7.1.7's POLICY CONTAINER this Document is CREATED WITH — §7.5.1's own creation table row,
   decided by §7.1.7's determine-navigation-params-policy-container and handed over whole. ONE argument rather
   than one per item, because §7.1.7 makes it one struct and its clone moves every item at once; a seam that
   spelled the items separately would drop the item added next, silently.
   ITS CSP LIST'S SELF-ORIGIN is a separate fact from the Document's `url` and from its principal for the
   reason those two are separate from each other: §2.2.2 sets it from the RESPONSE's URL, §7.3.2.1's clone
   carries the CREATOR's into a document built from no response, and §2.2's own note exists to say they differ
   (a document with an opaque origin which inherited its policy still resolves `'self'` against the origin the
   policy came from). It travels as BYTES because that is how a policy container travels, and the identity a
   serialization drops decides nothing here: both bullets of §6.7.2.8's `'self'` arm read components an OPAQUE
   origin has none of, so an opaque self-origin matches nothing whether or not two documents' copies of it are
   the same record. */
/* `sandbox_flags` is §7.1.5's ACTIVE SANDBOXING FLAG SET this Document is CREATED with, and it is a SEPARATE
   argument from `csp` because §7.1.7's policy container has no such field — the two are separate items of the
   Document in §7.5.1's own creation table ("policy container … navigationParams's policy container; active
   sandboxing flag set … navigationParams's final sandboxing flag set"). The caller states the whole set,
   because only the caller knows which algorithm is running: §7.2's create-a-new-browsing-context-and-document
   gives the initial about:blank the navigable's CREATION sandboxing flags alone, while §7.5.1 gives a
   navigated Document §7.4.5's union of those and the response policy's CSP-derived flags. */
void document_install(JSContext *ctx, JSValueConst global, lxb_html_document_t *dom, const char *url,
                      SerializedPolicyContainer policy, SandboxFlags sandbox_flags,
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
/* SELECTION API §4.1's `Selection? getSelection()` FOR ONE DOCUMENT — "the selection associated with this if
   this has an associated browsing context, and … null otherwise". The receiver names the document, because a
   realm holds several (a createHTMLDocument, a DOMParser parse, an XHR responseXML) and only its ACTIVE one
   has a browsing context. §2's selection is per DOCUMENT, so it is this record's field and not a realm value;
   the algorithm and the object are core/dom/selection.c's. `doc` must be a Document wrapper — a receiver that
   is not one is Web IDL §3.7.5's TypeError, thrown here. OWNED. */
JSValue document_selection(JSContext *ctx, JSValueConst doc);
/* HTML §8.1.5.1's API BASE URL of this realm — "return the current BASE URL of window's associated Document",
   which HTML §2.4.2's parse a URL resolves every relative reference against. It is §2.4.3's DOCUMENT BASE URL
   and NOT the document's address: this header used to say the address, the code used to return it, and both
   were wrong for every page shipping a `<base href>`. ONE component owns it, so two answers cannot drift. */
const char *document_base_url(JSContext *ctx);
/* THIS REALM'S ACTIVE DOCUMENT'S ADDRESS — §4.5's `document.URL`. A DIFFERENT question from the one above, and
   the caller has to say which it is asking: §7.2.4's Location url, §7.2.5's can-have-its-URL-rewritten,
   §7.4.4's "let newURL be document's URL" and §8.1's blob origin are all about the ADDRESS, and every one of
   them read `document_base_url` while the two happened to be the same string. BORROWED. */
const char *document_url(JSContext *ctx);
/* THE SAME §2.4.3 ANSWER FOR ONE DOCUMENT rather than for the running realm — what DOM §4.4's `baseURI` reads
   ("this's node document's document base URL") and what HTML §2.6.1's URL reflections resolve against
   ("relative to element's node document"). A second Document of this agent — a DOMParser parse, an XHR
   responseXML, a createHTMLDocument — has its own, and answering from the realm gives it the wrong one.
   BORROWED: the record owns the bytes. */
const char *document_base_url_of(const lxb_dom_document_t *dom);
/* §2.4.3's FALLBACK BASE URL — a SEPARATE answer, and the one §4.2.3 freezes a `<base href>` against ("thus,
   the base element isn't affected by itself"). §7.4's about base URL for an `about:blank`/`about:srcdoc`
   Document created by a creator, otherwise the document's address. BORROWED. */
const char *document_fallback_base_url_of(const lxb_dom_document_t *dom);
/* §4.2.3's FROZEN BASE URL, STORED — the pair (which base element, what it froze to). core/html/html_base_element.c
   owns the algorithm that decides when to write it and is its only writer; this is the storage, beside the
   address it is not. `el` NULL with `url` NULL says the document has no base element with an href. The element
   is COMPARED and never dereferenced — see the definition. */
void document_set_frozen_base_url(lxb_dom_document_t *dom, lxb_dom_element_t *el, const char *url);
lxb_dom_element_t *document_frozen_base_element(const lxb_dom_document_t *dom);
/* HTML §7.4's ABOUT BASE URL of THIS realm's Document — `creatorBaseURL` for the initial `about:blank` a
   navigable is created with, and §7.4.5's initiator base URL for an `about:` navigation. WRITE-ONCE, by the
   operation that CREATED the Document and before its tree is walked; a Document created from a response never
   receives one, which is §2.4.3's null. */
void document_set_about_base_url(JSContext *ctx, const char *url);
/* HTML §3.1.1's "the encoding" of this realm's active document, as an id in the Encoding registry
   (core/encoding/encoding.h). It is what HTML §4.12.1 falls back to when a `<script>` carries no `charset`
   attribute — "let encoding be el's node document's the encoding" — and therefore what HTML §8.1.4.2's fetch a
   classic script decodes a fetched body with when the response names no charset of its own. ONE component owns
   it, exactly as one owns the document's address. */
int document_encoding(JSContext *ctx);
/* The same fact asked of a DOCUMENT — HTML §4.10.22.5 "Selecting a form submission encoding" step 1's "the
   document's character encoding", where the document is the form's node document and not whichever one the
   realm currently calls active. See the definition. */
int document_encoding_of(const lxb_dom_document_t *dom);
/* HTML §13.2.3.2 "Determining the character encoding"'s ANSWER. DOM §4.5 Interface Document gives a document
   the utf-8 encoding "unless stated otherwise"; the encoding sniffing algorithm
   (core/html/html_encoding_sniff.h) is what states otherwise, and the LOADER runs it — the response bytes and
   its `Content-Type` are the navigation's, not the Document's — so the answer is written here. Per flow, like
   the address. */
void document_set_encoding(JSContext *ctx, int encoding);
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

/* §4.5's `document.URL` FOR ONE DOCUMENT: its ADDRESS. NOT what DOM §4.4's `baseURI` answers — that is the
   document BASE URL (document_base_url_of above), which this used to be mistaken for; the address is only
   §2.4.3's LAST step, and a document carrying a `<base href>` or an about base URL answers neither of the
   earlier two with it. This is what RFC 6265's request-uri, §4.4's clone-a-Document and §7.4.4's URL and
   history update steps are about. */
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
   §6.7.2.8 has no answer without it. OWNED by the caller.
   `embedder` IS §7.1.4'S ITEM OF THE CONTAINER THIS DOCUMENT IS CREATED WITH, passed straight through: the
   meta walk below has nothing to say about it (CSP §3.3's `<meta>` delivers CSP policies and nothing else, and
   `Cross-Origin-Embedder-Policy` has no `<meta>` form at all), so this function neither derives it nor defaults
   it — which is what keeps the two halves this function DOES merge from acquiring a silent third. */
PolicyContainer *document_policy_new(lxb_html_document_t *dom, const char *csp, const Origin *self_origin,
                                     SerializedEmbedderPolicy embedder);

/* THIS DOCUMENT'S POLICY CONTAINER — HTML §7.2.6, built at install from the above. Built at install rather
   than on demand because §7.4 clones it for an about:blank child at the moment that child is created, which
   may be before anything else has asked. BORROWED. */
const PolicyContainer *document_policy(JSContext *ctx);
/* THE SAME CONTAINER AS A FACT ABOUT ONE DOCUMENT, which is how CSP states every question over it ("let CSP
   list be DOCUMENT's global object's csp list"). §4.2.3's freeze runs for an element whose node document is
   routinely not the running realm's active one. BORROWED; NULL for a document with no browsing context. */
const PolicyContainer *document_policy_of(const lxb_dom_document_t *dom);

/* HTML §4.8.5 "The `iframe` element"'s ALLOWED TO USE, which is where the standard defines the phrase every
 * other section asks with: "To determine whether a Document object document is allowed to use the
 * policy-controlled-feature feature, run these steps: 1. If document's browsing context is null, then return
 * false. 2. If document is not fully active, then return false. 3. If the result of running is feature enabled
 * in document for origin on feature, document, and document's origin is `Enabled`, then return true.
 * 4. Return false."
 *
 * IT LIVES HERE AND NOT IN THE POLICY COMPONENT because steps 1 and 2 are DOCUMENT facts — a browsing context
 * and §7.3.1's fully active walk — and only step 3 is a question about the policy. The split is the standard's:
 * Permissions Policy §9 is written over a policy and two origins, and HTML is what turns a Document into them.
 *
 * ITS CALLERS ARE UNRELATED AND THAT IS THE POINT. HTML §7.2.2.6 "Script settings for Window objects" makes it
 * the second conjunct of the CROSS-ORIGIN ISOLATED CAPABILITY; §6.6.6's allow focus steps make it their first
 * clause. Each had written its own answer as "same origin with the top-level traversable's active document",
 * which §9.7 says nowhere and which is wrong for a same-origin document nested through a cross-origin frame. */
bool document_allowed_to_use(JSContext *ctx, PermissionsPolicyFeature feature);

/* THIS DOCUMENT'S PERMISSIONS POLICY — Permissions Policy §9.5's, created at install from the navigable's
   container exactly as §9.5 is invoked for a navigable. BORROWED; NULL for a Document with no browsing
   context, which is §4.8.5 step 1's refusal and is why that step needs no second field to read. It is exposed
   because §9.7's inheritance reads the CONTAINER DOCUMENT's policy, and the container document is a different
   realm's — so a child navigable's install asks the parent's Document for this. */
const PermissionsPolicy *document_permissions_policy(JSContext *ctx);

/* Permissions Policy §9.5 "Create a Permissions Policy for a navigable" — "given null or an element
 * (container) and an origin (origin) this algorithm returns a new Permissions Policy" — over exactly those two
 * arguments, which is the form its OTHER caller needs.
 *
 * IT IS PUBLIC BECAUSE §9.5 HAS TWO CALLERS AND ONE IMPLEMENTATION. The first is this file's own install, for
 * every Document a navigable of THIS agent is given. The second is HTML §7.3.1.3's create-a-new-child-navigable
 * when the child is CROSS-ORIGIN (core/frame/navigable.c): SECURITY.md makes that child the root of a PEER
 * instance, the peer holds no element and cannot run §9.5 at all, and both of §9.5's arguments are right here —
 * the container element is in this tree and `origin` is the child's, which that same create computed. So the
 * creator runs the algorithm and its ANSWER crosses on the provisioning record. A second evaluation in the
 * peer would be the one-question-two-places defect, and it would need §9.7's inputs rather than its result.
 *
 * ITS ARGUMENTS ARE §9.5'S OWN TWO AND THERE IS NO `ctx`, which is not a saving: the algorithm reads an
 * ELEMENT and an origin, and the element carries its own node document. A realm handed in beside it would be a
 * third thing the answer could be taken from, and §9.7's every step names the CONTAINER's document rather than
 * the asking one.
 *
 * `container` is §7.3.1.3's NAVIGABLE CONTAINER — the element wrapper, or JS_NULL/JS_UNDEFINED for §9.7 step
 * 1's "container is null" (a top-level traversable, an auxiliary navigable, a detached frame). `origin` is the
 * origin of the Document being created in the navigable. OWNED by the caller: permissions_policy_free. */
PermissionsPolicy *document_permissions_policy_for_container(JSValueConst container, const Origin *origin);

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
