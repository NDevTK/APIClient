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

/* DOM §4.5 "Interface Document"'s TWO CREATION FACTS, AS ONE VALUE — the pair HTML §7.5.1 "Shared document
 * creation infrastructure" is given ("create and initialize a Document object, given a type `type`, content
 * type `contentType`, and navigation params navigationParams") and writes onto the Document it makes ("Let
 * document be a new Document, with type `type`, content type `contentType`, …").
 *
 * THEY ARE TWO FACTS AND NEITHER DERIVES THE OTHER, which is the whole reason this is a type. §4.5 states them
 * separately — "Unless stated otherwise, a document's … content type is `application/xml` … type is `xml`" —
 * and then defines the predicate over only one of them: "A document is said to be an XML document if its type
 * is `xml`; otherwise an HTML document." HTML §7.5.4 "Loading text documents" is the case that makes the
 * distinction load-bearing rather than pedantic: it creates its Document given type `"html"` and contentType
 * `type`, so a `text/plain` response is an HTML DOCUMENT whose `contentType` is not "text/html". Answering
 * "is this an HTML document" with a string compare against the content type therefore reports every text
 * document as an XML one — and this engine asked it that way, so `createCDATASection` was permitted on a
 * document that must refuse it and §4.12.3's template contents owner was minted with the wrong type.
 *
 * ONE VALUE AND NOT ONE ARGUMENT PER ITEM, for core/platform.h's reason and with its force: it is built
 * through the constructors below, each of which names every item, so a creation that stops stating one stops
 * compiling rather than silently keeping a neighbour's answer. */
typedef struct {
    bool is_xml;             /* §4.5's `type`: true is "xml", false is "html" — the predicate above, stored */
    char content_type[32];   /* §4.5's content type — the string `document.contentType` answers */
} DocumentKind;

/* THE PAIR A STANDARD STATES OUTRIGHT — DOM §4.5.1's three factories, HTML §8.5.1's `parseFromString` and
   XMLHttpRequest §3.6.6's "set a document response", each of which is given both facts by its own algorithm
   and has no response to dispatch on. `content_type` is copied; it must be non-empty and must fit, and both
   are asserted rather than truncated, because a content type that lost its tail is a type no algorithm
   produced and every later compare against it silently answers for a document that does not exist. */
DocumentKind document_kind(bool is_xml, const char *content_type);

/* HTML §7.3.2.1 "Creating browsing contexts"' INITIAL `about:blank` — "Let document be a new Document, with:
   type `html`, content type `text/html`, …". The Document with no response, so there is no computed type to
   dispatch on and nothing for core/loader/document_load_type.h to answer; it is a constant of the standard,
   stated in ONE place because a constant spelled at each of the entries that creates one is that many rules. */
DocumentKind document_kind_initial_about_blank(void);

/* DOM §4.5 "Interface Document"'s THIRD CREATION FACT, WHICH IS NOT ONE OF THE PAIR ABOVE — WHICH INTERFACE THE
 * DOCUMENT IMPLEMENTS. §4.5 declares two of them in one section: `interface Document : Node` and, verbatim,
 * `[Exposed=Window] interface XMLDocument : Document {};`. It also states the operation that chooses between
 * them, verbatim: "To create a document that implements an interface interface, given a realm realm: Assert:
 * interface is Document or an interface that inherits from Document. Return a new node that implements
 * interface, in realm." So the interface is an ARGUMENT of the creation, exactly as `type` and `content type`
 * are — and it is what decides the wrapper's prototype, `doc.constructor` and `doc instanceof XMLDocument`.
 *
 * IT IS A SEPARATE FACT FROM §4.5's `type` AND NEITHER DERIVES THE OTHER, which is the whole reason it is
 * stated rather than computed from `is_xml`. §4.5.1's createDocument creates "a document that implements
 * XMLDocument" and §4.5's default type `xml` is what it leaves in place — so it is both. HTML §8.5.1 "The
 * DOMParser interface"'s parseFromString says "Let document be a NEW DOCUMENT, whose content type is type and
 * URL is this's relevant global object's associated Document's URL" and then switches on the type to pick a
 * parser — so an `application/xml` parse is an XML document that implements Document and NOT XMLDocument. Those
 * two are the same `type` and different interfaces, so a `document_is_xml_of` test in place of this field
 * answers `doc instanceof XMLDocument` TRUE for every DOMParser XML parse, which is the answer
 * domparsing/DOMParser-parseFromString-xml.html asserts against eight times. §4.5's own note on the `Document()`
 * constructor states the same split from the other side, verbatim: "Unlike createDocument(), this constructor
 * does not return an XMLDocument object, but a document (Document object)."
 *
 * WHO PASSES WHICH, AND IT IS A CLOSED SET BECAUSE THE STANDARDS ARE. Exactly one algorithm in the platform
 * names XMLDocument: DOM §4.5.1's createDocument. HTML §7.5.1 "Shared document creation infrastructure" (and so
 * §7.5.2, §7.5.3 "Loading XML documents" and §7.5.4), HTML §8.5.1's parseFromString, §4.5's `Document()`
 * constructor, §4.5.1's createHTMLDocument and XMLHttpRequest §3.6.6's "set a document response" all say "a new
 * Document" or "a document that implements Document"; XHR's text does not contain the word XMLDocument at all.
 * The one other way a Document acquires the fact is by COPY: DOM §4.4 "Interface Node"'s clone a single node
 * says "if node is a document, set copy to the result of creating a document that implements THE SAME
 * INTERFACES AS NODE, given document's relevant realm" — which is why document_interface_of exists and why
 * core/dom/node.c's clone reads it rather than re-deriving one. */
typedef enum {
    DOCUMENT_IFACE_DOCUMENT = 0,   /* §4.5's `interface Document : Node` */
    DOCUMENT_IFACE_XML_DOCUMENT    /* §4.5's `[Exposed=Window] interface XMLDocument : Document {};` */
} DocumentInterface;

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
/* `kind` is DOM §4.5's TYPE AND CONTENT TYPE for this Document, as HTML §7.4.5 "Populating a session history
   entry"'s load-a-document arm decided them — core/loader/document_load_type.h's `document_load_kind` for a
   response, §7.3.2.1's constant for the initial `about:blank`. IT IS CARRIED AND NEVER RE-DERIVED HERE: the
   arm is a fact about the RESPONSE, which this function does not have, and the one component that classifies
   a response is the one every loader already dispatches through. This used to be the literal "text/html",
   written into every Document this engine installed whatever had been fetched — so an XHTML document reported
   `text/html`, §4.5's "is this an HTML document" answered yes for it, and the HTML parse-boundary correction
   ran over a tree an XML parser built. */
void document_install(JSContext *ctx, JSValueConst global, lxb_html_document_t *dom, const char *url,
                      DocumentKind kind, SerializedPolicyContainer policy,
                      SerializedResponsePermissionsPolicy permissions_policy, SandboxFlags sandbox_flags,
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

/* HTML §8.4.1 "Opening the input stream" step 18's readiness transition, and ONLY that one — see the body for
   why the entry is named after its algorithm rather than being a general setter. Its caller is
   core/html/document_open.c. */
void document_set_readiness_loading(JSContext *ctx);

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
   is not one is Web IDL §3.7.7 Operations' TypeError, thrown here — `getSelection()` is an operation, so the
   step is "If jsValue does not implement the interface target, throw a TypeError". OWNED. */
JSValue document_selection(JSContext *ctx, JSValueConst doc);
/* HTML §8.1.3.2 Environment settings objects' API BASE URL of this realm, which §7.2.2.6 Script settings for
   Window objects answers — "Return the current base URL of window's associated Document." —
   which HTML §2.4.2's parse a URL resolves every relative reference against. It is §2.4.3's DOCUMENT BASE URL
   and NOT the document's address: this header used to say the address, the code used to return it, and both
   were wrong for every page shipping a `<base href>`. ONE component owns it, so two answers cannot drift. */
/* HTML §7.2.4's `ancestorOrigins` answers with this Document's ANCESTOR ORIGINS LIST — the DOMStringList
   §7.3.2.1 "Creating browsing contexts" gave it at its creation. [SameObject]: the same object for the life of
   the Document, which the corpus asserts by identity. Owned. Crashes for a Document that has none, because a
   Document with no browsing context is answered by §7.2.4's FIRST step (the Location's own empty list) and
   never reaches this. */
JSValue document_ancestor_origins(JSContext *ctx);

/* HTML §3.1.3 "Ancestor origins"' INTERNAL ANCESTOR ORIGIN OBJECTS LIST IN THE FORM IT CROSSES AN INSTANCE
 * BOUNDARY, which is the only form it can cross one in: §Security makes what crosses TEXT, and this list is
 * already held as §7.1.1 serializations of origins for its two in-heap consumers.
 *
 * WHAT CROSSES IS THE CHILD'S FINISHED LIST, COMPOSED IN THE CREATOR — never the creator's own list with the
 * steps left to run at the far end. §3.1.3's step 12.1 asks whether an ancestor "is same origin with
 * parentDoc's origin", and §7.1.1's same origin compares an opaque origin by IDENTITY while every opaque
 * origin serializes to the same three bytes `null`. A peer given the text alone would therefore mask an entry that is not the
 * parent's whenever the parent is itself opaque — a wrong list, silently, in exactly the `data:`-iframe case
 * that made this crossing necessary. Steps 6-9 are no better placed there: the container element is in the
 * CREATOR's tree and an element does not cross at all. So the creator runs §3.1.3 ONCE, with all three of its
 * inputs in one heap, and its RESULT travels — the identical division Permissions Policy §9.5's answer makes
 * on the same record, for the identical reason.
 *
 * THE GRAMMAR IS SPACE-SEPARATED SERIALIZED ORIGINS, and the separator is a THEOREM rather than a taste: URL
 * §3.2 "Host miscellaneous" makes U+0020 SPACE a FORBIDDEN HOST CODE POINT, and §7.1.1's serialization of an
 * origin is `null` or a scheme, "://", a host and an optional port — none of which admits one. (A `;` would
 * NOT do: it is not forbidden in a host, and `permissions_policy.h`'s record beside this one gets away with it
 * only because its entries are §4.1's feature tokens.) The EMPTY LIST IS A WORD because an empty field and a
 * host that stopped writing the field are two different facts and only one is a bug — the same distinction
 * PERMISSIONS_POLICY_SERIALIZED_NO_CONTAINER draws one field along, and here the bug it catches is a
 * cross-origin frame silently reporting itself top-level to `location.ancestorOrigins`.
 *
 * `document_ancestor_origins_for_child` is what §7.3.1.3's create calls in the CREATING instance, for a child
 * whose Document a peer will build; the other two are the peer's side. OWNED — the caller frees. */
#define DOCUMENT_ANCESTOR_ORIGINS_SERIALIZED_NONE "none"
char *document_ancestor_origins_for_child(JSContext *ctx, JSValueConst container, const Origin *child_origin);
char *document_ancestor_origins_serialize(JSContext *ctx, JSValueConst list);
/* Whether that record states any ancestors at all, which is §3.1.3's step 3 answered from the record: an
   empty list means no container document, which by §7.3.1.3 means no parent. CHECKs an ABSENT field, because
   the bytes come from another instance and a DCHECK would be compiled out of the build that faces them. */
bool document_ancestor_origins_serialized_has_ancestors(const char *text);
JSValue document_ancestor_origins_deserialize(JSContext *ctx, const char *text);

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
/* HTML's SET THE URL — what HTML §7.4.4 "Non-fragment synchronous \"navigations\""'s URL and history update
   steps step 8 performs ("set the URL given document to newURL"), and the only way a Document's address
   changes without a new Document being installed. PER FLOW: the address rides the running flow's COW delta, so
   the arm that called `history.pushState(s, "", "/b")` is the only one whose `location.pathname` is `/b`.
   IT TAKES A VALUE AND NOT BYTES — see document_url_value below, which is the whole of why. CONSUMES
   `address`; a caller holding only bytes says so by handing over a JS string, which is a POSITIVE statement
   that this address is a concrete fact and not a shorthand for one. */
void document_set_url(JSContext *ctx, JSValue address);

/* THIS REALM'S ACTIVE DOCUMENT'S ADDRESS AS A VALUE — the SAME address `document_url` answers in bytes, as the
 * thing the run computed those bytes out of. OWNED: the caller frees.
 *
 * WHY AN ADDRESS IS NOT A STRING. A client-side router computes where it is going — `"/routes/" + cfg.region +
 * "/admin"`, where `cfg` is a JSON config this run FETCHED — and HTML §7.2.5 "The History interface"'s
 * `pushState` then makes that computed string this Document's address. CLAUDE.md §Solver-half's rule about
 * EVERY value applies to it exactly as it applies to every other: it is a triple, and two of the three are
 * wanted by DIFFERENT consumers, which is why neither may stand in for the other.
 *   The EXAMPLE is what solver/route_seed.h declares a page of this application and what the trusted zone
 *   LOADS over the person's own session, so it must be the real `/routes/us-east-1/admin` and never a display
 *   shape — a shape seeds an address no server has, and the reply's fields are then examples that shape the
 *   next endpoint, which is §@H's never-invent one network hop out.
 *   The DOMAIN is what a later `location.pathname.startsWith("/admin")` branches on, and the domain of a
 *   computed address is unconstrained even where its example is known — so BOTH arms must still run. Writing
 *   the bare example into the address instead DECIDES that branch and deletes the arm this engine exists to
 *   reach, with nothing to say so. That is the shortcut, and it is the one this primitive refuses.
 * So `document_url` answers the example (every consumer that wants BYTES is asking for exactly that and is
 * unchanged by this) and this answers the value the example came out of.
 *
 * A PLAIN STRING IS A POSITIVE STATEMENT AND NEVER A HOLE — it says this address is a CONCRETE FACT, which is
 * what a Document loaded from a response has, and `concolic_is` on the result is the ONE test that tells the
 * two apart. There is no third state: the record ALWAYS holds a value, asserted at both of its writers. */
JSValue document_url_value(JSContext *ctx);

/* THE BYTES AN ADDRESS VALUE STANDS FOR — its EXAMPLE where the run computed it from unknown input, and the
   string itself where it is a concrete fact. It is the ONE place that conversion is spelled, because it is the
   step at which an address stops carrying its domain and every caller performing it is SPENDING that fact:
   §7.4.4's session history entry, solver/route_seed.h's declaration and §7.2.5 step 5's own URL parse all take
   these bytes. One place is also what makes the missing half of the capability crash ONCE for every arrival
   site rather than being asked at some of them and not others.
   OWNED — free with JS_FreeCString. Never NULL: an address value carrying no example is a Document whose
   address is UNKNOWN, which is a capability this engine does not have, and the crash naming it is here. */
const char *document_address_example(JSContext *ctx, JSValueConst address);

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

/* HTML §7.4.6.4 "Scrolling to a fragment"'s TARGET ELEMENT of the document this realm is presenting — "there is
 * also a target element for each Document, which is used in defining the :target pseudo-class and is updated by
 * the above algorithm. It is initially null."
 *
 * NULL IS THE STANDARD'S OWN ANSWER FOR A DOCUMENT §7.4.6.4 HAS NOT RUN ON, so it is a derivation and not a
 * stand-in: scroll to the fragment is the ONLY algorithm in any standard that writes a target element ("set
 * document's target element to target" on its element arm, "set document's target element to null" on the other
 * two), this build runs it nowhere, and the one site that would run it — HTML §7.4.2.3.3's step 15, in
 * core/frame/session_history.c — asserts against THIS function rather than writing the claim down as a comment.
 * The initial value is therefore the current value for every Document there has ever been here.
 *
 * IT IS THE FACT ITSELF AND NOT A PROXY FOR ONE, which is the whole reason it exists. HTML §6.6.7 "The
 * `autofocus` attribute"'s flush autofocus candidates reads a target element twice — step 4's second disjunct
 * ("or topDocument has non-null target element") and step 5.8's climb over the inclusive ancestor navigables —
 * and both of those asked `realm_awaits(docctx, "scrollTo", …)` instead, a NAME ON THE GLOBAL standing in for a
 * Document field. That proxy was wrong in BOTH directions at once. Installing CSSOM VIEW §4's `scrollTo` on the
 * Window fires it while no Document can hold a target element — a probe announcing a capability that has not
 * arrived, which is worse than no probe because the next reader builds on it. And building §7.4.6.4, whose
 * four observable effects are setting the target element, running the ancestor revealing algorithm, running
 * §6.6.4 "Processing model"'s focusing steps and moving the sequential focus navigation starting point — only
 * ONE of which is a scroll — would have left it SILENT at the moment its readers came alive. A capability is asked
 * of the component that owns the capability; core/timing/hr_time.c records what asking the global cost. */
lxb_dom_node_t *document_target_element(JSContext *ctx);

/* A SECOND DOCUMENT IN THIS REALM — what DOM §4.5.1's createDocument and createHTMLDocument return, and what
   HTML's `new Document()` builds. It has NO BROWSING CONTEXT: no navigable, no Window, no WindowProxy, and no
   scripts, which is why §3.1.1's `location` is null on it. It is NOT a second realm and NOT a second instance —
   HTML's similar-origin window agent is ONE HEAP, so the nodes it makes are ordinary objects of `ctx` with
   `ctx`'s prototypes, and none of navigable.c's or world.c's machinery is involved (both are about NAVIGABLES,
   which this has none of).
   `dom` is CONSUMED: whoever ends up owning the document destroys it. That is the running flow's COW delta when
   capture is on — a document a flow created dies with the flow, exactly like a node it created — and the REALM
   when it is off, since a creation made at baseline is baseline. Returns the document's wrapper, OWNED.
   `iface` IS DOM §4.5's "create a document that implements an interface"' OWN ARGUMENT and it is stated here
   rather than derived from `kind`, for the reason DocumentInterface gives: createDocument and a DOMParser XML
   parse agree about `type` and disagree about the interface. It is on THIS entry and not on document_install
   because HTML §7.5.1 "Shared document creation infrastructure" — every navigation's creation — says "Let
   document be a new Document" and has no arm that says otherwise, so an argument there would be a question with
   one answer. */
JSValue document_new(JSContext *ctx, lxb_html_document_t *dom, const char *url, DocumentInterface iface,
                     DocumentKind kind);

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

/* DOM §4.5 Interface Document's OTHER creation fact for ONE document: "A document is said to be an XML
   document if its type is `xml`; otherwise an HTML document." A SEPARATE READ from the content type beside
   it, never a compare against those bytes — see DocumentKind above for the text document that makes the two
   disagree. §4.4's "clone a single node" copies both ("set copy's encoding, content type, URL, origin,
   type, mode, and allow declarative shadow roots to those of node"), which is the other reason the record
   has to hold both. */
bool document_is_xml_of(const lxb_dom_document_t *dom);

/* WEB IDL §3.7 Interfaces' implementation-check an object, step 3 — "If object does not implement interface,
   then throw a TypeError." — as the PREDICATE core/idl_args' idl_this_iface and its [LegacyLenientSetter]
   installer take. It was file-static here and is exported for one reason: §3.4.2's no-op setter runs §3.7.6's
   validThis test before it does nothing, and the component installing that setter on `Document.prototype` has
   to name the brand. A second copy in that component would be a second answer to a question this file already
   answers — core/fullscreen/fullscreen.c had one (`fs_document_receiver`'s own node-type test) and it is gone. */
bool document_is(JSValueConst v);

/* …AND DOM §4.5's THIRD creation fact for ONE document: WHICH INTERFACE IT IMPLEMENTS — see DocumentInterface
   above. Its ONE consumer beyond this file is DOM §4.4 "Interface Node"'s clone a single node, whose document
   arm is "creating a document that implements THE SAME INTERFACES AS NODE": the copy is not re-classified, it
   is given what the source has. */
DocumentInterface document_interface_of(const lxb_dom_document_t *dom);

/* …AND THE INTERFACE PROTOTYPE OBJECT THAT FACT NAMES, in `dom`'s OWN REALM — Web IDL §3.7.3 "Interface
   prototype object"'s object for §4.5's `Document` or for its `XMLDocument`, whichever this document was
   created implementing. It is asked at the ONE place a node wrapper is built (core/dom/node.c's node_wrap),
   for the same reason an ELEMENT's interface is asked there rather than keyed by node type: a node TYPE table
   cannot answer a question whose answer differs between two nodes of that type. OWNED, like every per-realm
   prototype read. */
JSValue document_interface_proto(JSContext *ctx, const lxb_dom_document_t *dom);

/* DOM §4.9 "Interface Element"'s "CREATE AN ELEMENT" WITH THE HTML NAMESPACE NAMED — the shape four standards
   sentences spell out in full rather than deriving: HTML §8.5.5 "The outerHTML property" setter step 5 ("set
   parent to the result of creating an element given this's node document, "body", and the HTML namespace"),
   HTML §8.5.6 "The insertAdjacentHTML() method" step 4 and HTML §8.5.7 "The createContextualFragment() method"
   step 6 (both "then set context to the result of creating an element given this's node document, "body", and
   the HTML namespace"), and DOM §4.5.1's createHTMLDocument.

   IT EXISTS BECAUSE THE NAMESPACE MUST BE STATED AND NOT ASKED. `lxb_dom_document_create_element` decides it
   from `lxb_dom_document_t::type`, and DOM §4.5's `type` is kept on THIS engine's Document record — nothing
   here ever writes lexbor's field, so it reads HTML for every document this engine builds and the namespace a
   creation got was one nobody decided. For the four sites above the answer is a constant of the standard, so
   naming it is not a workaround: an algorithm that says "and the HTML namespace" has no question to ask.
   §4.5's createElement is the one creation whose namespace is genuinely CONDITIONAL, and its own step 4 is a
   DISJUNCTION lexbor's entry cannot express at all ("the HTML namespace, if this is an HTML document or this's
   content type is "application/xhtml+xml"; otherwise null") — so it states its answer at its own site and
   reaches core/dom/element.h's element_create_ns directly.

   `local` is BORROWED and need not be NUL-terminated. Never returns NULL — element_create_ns says why. */
lxb_dom_element_t *document_create_element_html(lxb_dom_document_t *dom, const char *local, size_t local_len);

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
   DETACHED, with an empty attribute list. HTML §3.2.3 "HTML element constructors" step 9 is its second caller
   — a custom element constructor reached with an EMPTY construction stack makes the element itself, and
   step 9.2 names the
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
                                     SerializedEmbedderPolicy embedder, const char *integrity_policy);

/* THIS DOCUMENT'S POLICY CONTAINER — HTML §7.2.6, built at install from the above. Built at install rather
   than on demand because §7.4 clones it for an about:blank child at the moment that child is created, which
   may be before anything else has asked. BORROWED. */
const PolicyContainer *document_policy(JSContext *ctx);
/* THE SAME CONTAINER AS A FACT ABOUT ONE DOCUMENT, which is how CSP states every question over it ("let CSP
   list be DOCUMENT's global object's csp list"). §4.2.3's freeze runs for an element whose node document is
   routinely not the running realm's active one. BORROWED; NULL for a document with no browsing context. */
const PolicyContainer *document_policy_of(const lxb_dom_document_t *dom);

/* HTML §4.2.5.3 "Pragma directives"' content security policy state FOR AN INSERTED ELEMENT — DOM §4.2.3's
   insertion steps' half of the algorithm document_policy_new above runs over a PARSED tree.
   TWO CALLERS BECAUSE THERE ARE TWO WAYS A `<meta>` REACHES A DOCUMENT, and the standard treats them alike:
   §4.2.5.3 runs at the insertion, and HTML's own note beside the steps is about the scripted one ("prior to
   dynamically inserting a meta element with an http-equiv attribute in the Content security policy state").
   The batch walk cannot see that one, because it ran before the script did — so its policy used to be dropped
   and the Document judged under a MORE PERMISSIVE list than the real page has, which is a breakout the real
   policy kills reported as a working exploit.
   `el` IS EVERY INSERTED ELEMENT: §4.2.5.3's own first act is the pragma selection, so a caller that filtered
   first would be a second reading of it (core/html/html_meta_csp.h states this from the component's side). The
   policy is enforced upon the ELEMENT'S NODE DOCUMENT, never the running realm's active one — a `<meta>`
   inserted into a DOMParser tree says nothing whatever about the page. */
void document_meta_csp_inserted(lxb_dom_element_t *el);

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
