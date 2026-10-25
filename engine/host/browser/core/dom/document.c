/* DOM §4.5 "Interface Document" (Blink core/dom): the per-document record, the creation facts every Document
 * carries, and the Document members that read the tree. The metadata members (`cookie`, `referrer` and the rest)
 * are core/dom/document_metadata.c's. */
#include <stddef.h>   /* offsetof — DocAddress's CowRecord layout names its owned value by it */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"
#include "solver/dom_cow.h"   /* dom_cow_note_created — a created node belongs to the flow's delta */
#include "solver/cow.h"       /* cow_capture_host_record: a Document's address is per-flow state that owns a
                                 value, so it goes through the record arm, not the byte one */
#include "quickjs.h"
#include "solver/concolic.h"
#include "solver/engine.h"
#include "core/events/event.h"
#include "core/events/create_event.h"
#include "core/events/event_target.h"
#include "core/events/page_transition_event.h"
#include "core/events/report_exception.h"
#include "core/fullscreen/fullscreen.h"
#include "core/html/html_element.h"
#include "core/svg/svg_element.h"   /* svg_element_install: SVGElement and SVGAnimatedString */
#include "core/html/html_form.h"
#include "core/html/custom_elements.h"
#include "core/html/element_internals.h"
#include "core/html/html_iframe.h"
#include "core/html/declarative_shadow.h"
#include "core/html/media_element.h"
#include "core/html/event_handler_attribute.h"
#include "core/html/html_image.h"
#include "core/html/html_link.h"
#include "core/html/html_dialog.h"
#include "core/html/html_style_element.h"
#include "core/html/dom_string_list.h"   /* §3.1.3's ancestor origins list is a DOMStringList */
#include "core/html/html_script.h"   /* §4.12.1.1's `force async`: the stamp every parser makes */
#include "core/html/html_base_element.h"   /* §4.2.3's frozen base URL, whose two facts this record holds */
#include "core/html/html_meta_csp.h"       /* §4.2.5.3's pragma: what one `<meta>` delivers to this container */
#include "core/html/html_meta_referrer.h"  /* §4.2.5.1's `referrer`: the referrer policy one `<meta>` sets */
#include "core/url/url.h"                  /* §2.4.3's fallback base URL asks whether an address matches about: */
#include "core/frame/csp_source_list.h"   /* CSP §6.4.2.1's per-ancestor frame-ancestors match */
#include "core/dom/dom_token_list.h"
#include "core/dom/collections.h"
#include "core/dom/mutation_observer.h"
#include "core/dom/attr.h"
#include "core/dom/attr_list.h"
#include "core/dom/selector_match.h"
#include "core/css/css_style_declaration.h"
#include "core/css/css_rule.h"
#include "core/css/css_font_feature_values.h"
#include "core/css/media_list.h"
#include "core/css/css_rule_list.h"
#include "core/css/css_style_sheet.h"
#include "core/css/style_sheet_list.h"
#include "core/encoding/encoding.h"   /* DOM §4.5's encoding is an id in Encoding §4.2's registry, asked of it by name */
#include "core/dom/document.h"
#include "core/dom/document_current_script.h"
#include "core/dom/document_domain.h"
#include "core/dom/document_metadata.h"
#include "core/html/document_write.h"   /* §8.4's three members drive the parser, not the tree; see its header */
#include "core/html/html_parse.h"       /* §13.2.7's premise: the lifecycle advances when the parse has ended */
#include "solver/world.h"
#include "core/frame/window_proxy.h"
#include "core/frame/navigable.h"
#include "core/frame/location.h"
#include "core/frame/session_history.h"
#include "core/dom/page_visibility.h"
#include "core/dom/render_blocking.h"
#include "core/html/autofocus.h"
#include "core/dom/unknown_markup.h"
#include "core/html/focus.h"
#include "core/dom/document_fragment.h"
#include "core/dom/shadow_root.h"
#include "core/dom/slot.h"
#include "core/dom/document_type.h"
#include "core/dom/dom_implementation.h"
#include "core/dom/names.h"      /* §1.4's validate-and-extract, createElementNS's step 1, one implementation */
#include "core/xml/xml_name.h"   /* XML 1.0 §2.3 [5] Name, which createProcessingInstruction's target fork asks */
#include "core/idl_args.h"
#include "core/realm.h"

/* Every member here takes DOMStrings; createElementNS takes two. Declared, not masked. */
static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
/* `Element createElementNS(DOMString? namespace, DOMString qualifiedName, …)`: the first argument is nullable,
   and `createElementNS(null, "div")` is how a page asks for the null namespace, so it must not be ToString'd to
   `null`. Not shared with IDL_2STR, because createProcessingInstruction's two arguments are both non-nullable. */
static const IdlArgType IDL_NSSTR_STR[2] = { IDL_DOMSTRING_NULLABLE, IDL_DOMSTRING };
#include "core/dom/node.h"
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>

#include "core/dom/element.h"
#include "core/dom/node_filter.h"
#include "core/dom/node_iterator.h"
#include "core/dom/tree_walker.h"
#include "core/dom/range.h"
#include "core/dom/selection.h"        /* Selection API §2's per-document selection, §4.1's getSelection */
#include "core/dom/node_interface.h"   /* the one place a Document is made; see that header */

/* The document's own state, held on the document, not the file. A similar-origin window agent's documents are
 * JSContexts of one runtime: HTML puts every same-origin document of one browsing-context group in one similar-origin window agent (one
 * heap) and gives each its own global, and the corpus relies on the shared heap
 * (`iframe.contentDocument.body.appendChild(subframe)` moves a node this document created into the other one, with
 * the same wrapper). What the whole agent shares (class ids, prototypes, interface objects, and the origin, since
 * an agent is origin-keyed) does not live here.
 *
 * A realm is the active document of one navigable, but not the only document in it: §4.5's createHTMLDocument,
 * createDocument and `new Document()` build a Document with no browsing context (no navigable, Window, WindowProxy
 * or scripts), a second tree in this heap whose nodes are this realm's objects (`foreignDoc.createElement("p")
 * instanceof Element` holds). So the record is per document, on the Lexbor document's embedder slot (the
 * document-to-realm answer §4.2.3 needs), and holds the address `baseURI` reads, the content type and §4.5's
 * `[SameObject] implementation`; the realm's context opaque names its active document, the answer to "the API base
 * URL" and "who fires load". Per-navigable fields are UNDEFINED on a document with no browsing context, which is
 * what §3.1.1's `location` returning null for one means. */

/* A document's address: DOM §4.5 "Interface Document"'s `URL`, not its base URL. Two fields because an address is a
 * value, not a string (core/dom/document.h's document_url_value): a router computes where it goes from a value the
 * run learned, so the address carries a concrete example that solver/route_seed.h's declaration takes verbatim and
 * an unconstrained domain a later `location.pathname` branch must still fork on.
 * `bytes` is the example, interned (doc_addr_intern), because "The URL and documentURI getter steps are to return
 * this's URL, serialized" and a serialization has no bound a fixed array could hold. Never NULL; the empty string
 * is "no address" (a realm whose install returned before it had one), which readers test for.
 * `value` is what the bytes came from: a plain JS string where the address is a concrete fact (a response was
 * fetched from it), the run's concolic otherwise. Never JS_UNDEFINED (both writers assert it); `concolic_is` tells
 * the two apart.
 * They are one struct because they are one capture, so a delta unapplies both or neither. It cannot use the byte
 * arm: solver/cow.h reserves that for a POD scalar latch, and a memcpy of a JSValue makes an uncounted reference. */
typedef struct DocAddress {
    const char *bytes;
    JSValue     value;
} DocAddress;

/* The layout saying which field is counted: solver/cow.h's CowRecord, the delta's counterpart of doc_rec_refs for the
   collector. One entry, for the one JSValue; the capture dups it and the delta frees it, and cow.c asserts the offset
   is in range, aligned and not repeated. */
static const uint16_t DOC_ADDR_VALS[] = { (uint16_t)offsetof(DocAddress, value) };
static const CowRecord DOC_ADDR_REC = { sizeof(DocAddress), DOC_ADDR_VALS, 1 };

typedef struct Document {
    JSContext           *realm;    /* the realm this document belongs to — every document has exactly one */
    uint32_t             doc;      /* this document's handle in the world registry; its name is what crosses */
    lxb_html_document_t *dom;
    int                  owned;    /* this record destroys `dom`: true for a document the page created */
    PolicyContainer     *policy;    /* owned; NULL for a document with no browsing context */
    /* Permissions Policy §9.5's permissions policy for the navigable this Document was created in: owned, NULL for
       a document with no browsing context (HTML §4.8.5's step 1). Separate from the container, which holds §7.1.7's
       CSP list, embedder policy, referrer policy and integrity policies; Permissions Policy §10.1 puts this on the
       Document, and §9.5 computes it from the navigable's container element. */
    PermissionsPolicy   *permissions_policy;
    /* Permissions Policy §10.1's report-only permissions policy: "Every Document has a report-only permissions
       policy, which is a permissions policy, which is initially empty", set by §9.6 with report-only true (the
       `Permissions-Policy-Report-Only` header). A second policy, not a flag: Permissions Policy §9.10 reads both
       answers ("if result is `Disabled`" reports an enforced violation, "else if report-only result is `Disabled`"
       a report-only one), and §9.6 re-runs §9.5 for it so a report-only header never narrows the enforced answer. */
    PermissionsPolicy   *report_only_permissions_policy;
    /* HTML §7.1.5's active sandboxing flag set: a field of the Document, not the policy container (§7.5.1's creation
       table lists them on separate rows). Written once, by the install below; no standard writes it again, so it is
       a plain word rather than a captured record. */
    SandboxFlags         sandbox_flags;
    JSValue              doc_obj;   /* the `document` object: held, released by document_free */
    JSValue              win_obj;   /* this document's Window: held, UNDEFINED with no browsing context */
    /* §7.2.3's one WindowProxy for this navigable: `window`, `self`, and the `source` of every message this document
       posts, so `e.source` compares equal across messages. It lives on the realm; a table keyed by document would be
       an immortal root holding one proxy per navigable the frontier ever created. Held, released with the realm,
       UNDEFINED with no browsing context. */
    JSValue              proxy;
    /* §4.5's `[SameObject] readonly attribute DOMImplementation implementation`: a page holds and compares it. */
    JSValue              impl;
    /* Selection API §2's unique selection: "Every document with a browsing context has a unique selection associated
       with it … This one selection must be shared by all the content of the document (though not by nested
       documents)". A field, since §3's state is on that object. UNDEFINED with no browsing context, which is §4.1's
       "must return null otherwise", so document_install builds it, not doc_rec_new. */
    JSValue              selection;
    /* HTML §3.1.3 "Ancestor origins": every Document has an ancestor origins list and an internal ancestor origin
       objects list. The internal one is origins, which a child reads to build its own; the public one is the
       DOMStringList §7.2.4's `ancestorOrigins` hands to script, [SameObject] for the Document's life
       (`location-ancestor-origins-new-object.html` asserts the identity). Both are a snapshot taken at §7.3.2.1
       "Creating browsing contexts", not a walk at the read, so removing the frame empties the answer (§7.2.4 finds no
       relevant Document) rather than the list. The internal list is held as serialized origins, which a child
       appends verbatim and a peer-instance child receives as text. An opaque origin serializes to `null`, so step
       10's masking compares the parent's origin record, and a cross-instance child's list is composed in the
       creator: two opaque origins serialize alike and are not same origin. */
    JSValue              ancestor_origins;          /* §7.2.4's DOMStringList — [SameObject], UNDEFINED with no browsing context */
    JSValue              ancestor_origin_strings;   /* the internal list, as an Array of serialized origins */
    /* The document's address (see DocAddress): bytes and value are one field and one capture. */
    DocAddress           addr;
    /* HTML §2.4.3's document base URL, which is not the address. Two algorithms: `frozen`/`frozen_el` are §4.2.3's
     * frozen base URL and its base element; `about` is §7.4's about base URL, which the fallback base URL answers
     * with for a Document a creator made at `about:blank` or `about:srcdoc` (without it a relative URL in a srcless
     * frame would resolve against `about:blank`). The address is the fallback's last step.
     * `frozen_el` is compared, never dereferenced: it answers whether the element now first in tree order is the
     * one this URL was frozen for, and §4.2.3's removing steps replace it when the element leaves the document.
     * Each field is a pointer, a POD scalar captured on its own (solver/cow.h's byte arm is for scalar latches);
     * `frozen_el` and `frozen` are one fact captured as two entries, which a wholly unapplied delta keeps together.
     * A flow that appends a `<base href>` changes where URLs resolve in its world only. */
    struct {
        lxb_dom_element_t *frozen_el;
        const char        *frozen;   /* NULL exactly when `frozen_el` is — the pair is one fact */
        const char        *about;    /* NULL is §7.4's null about base URL */
    } base;
    /* Every address this record has held (see doc_addr_intern). The fields above point into this list, and
       nothing in it is freed before the record. */
    char               **addrs;
    int                  n_addrs;
    int                  cap_addrs;
    char                 content_type[32];   /* §4.5 contentType: what this document was created as */
    /* §4.5's other creation fact: "A document is said to be an XML document if its type is `xml`; otherwise an HTML
       document." A field, not a compare against the bytes above, because HTML §7.5.4's text document is an HTML
       document whose content type is not "text/html" (see DocumentKind in document.h). */
    bool                 is_xml;
    /* DOM §4.5 "Interface Document"'s third creation fact: which interface this document implements, the argument of
       "create a document that implements an interface" (§4.5.1's createDocument answers XMLDocument, every other
       creating algorithm Document). A field, not a test on `is_xml`, because a DOMParser `text/xml` parse and a
       createDocument agree about `type` and disagree about the interface (document.h's DocumentInterface). node_wrap
       reads it for the wrapper's prototype. */
    DocumentInterface    iface;
    /* HTML §4.8.5 "The `iframe` element"'s class, carried because it is a fact about the creating operation that
     * nothing the document holds can recover: "Navigate an iframe or frame given element, about:srcdoc, the empty
     * string, and the value of element's srcdoc attribute. The resulting Document must be considered an iframe
     * srcdoc document." §2.4.3 "Document base URLs"' fallback base URL step 1 is its only reader. Settled by the
     * creating algorithm and never written again, so it takes no COW capture.
     * The address is not a substitute. A Document created by document_new takes its source's URL (HTML §8.5.1 for
     * DOMParser, DOM §4.4 for a clone) with no about base URL, so inside a srcdoc frame a parsed or cloned document
     * is addressed `about:srcdoc` without being one. And `<iframe src="about:srcdoc">` is not an iframe srcdoc
     * document (no srcdoc attribute, so §4.8.5's `Otherwise` arm), though §7.4.2.2 "Beginning navigation" gives it an
     * about base URL; the standard's answer for it is step 3, its own address. */
    bool                 is_iframe_srcdoc;
    /* DOM §4.5 "Interface Document"'s "Each document has an associated encoding (an encoding)": an id in the
       Encoding registry (core/encoding), which HTML §4.12.1.1 falls back to for a `<script>` with no `charset`
       ("let encoding be el's node document's the encoding"). Per-document state a navigation's response decides,
       written from HTML §13.2.3.2's encoding sniffing over the response's bytes and `Content-Type` charset. */
    int                  encoding;
    /* HTML §7.7's input (document_state_x_frame_options): the split `X-Frame-Options`, NULL when the response
       carried none; `xfo_stated` once the creating operation said so. Owned, set once. */
    char               **xfo;
    size_t               n_xfo;
    bool                 xfo_stated;
    /* HTML §4.12.3's two facts that make a Document's appropriate template contents owner document: its associated
       inert template document, and whether it is one ("a Document created by this algorithm"), which ends the
       recursion (an inert document is its own owner). A field, because nothing about an empty Document can be
       inspected to tell. The inert document is on the realm's `next_created` chain like any baseline creation;
       this names it so a second ask finds the first answer. */
    struct Document     *inert_template;
    int                  is_inert_template;
    /* The realm's chain of documents it created at baseline. A document a flow creates is owned by that flow's COW
       delta (dom_cow_note_created_document) and dies with it; one created with capture off is baseline, outlived by
       its realm. The head is on the active document's record, which the realm's teardown reaches. */
    struct Document     *next_created;
} Document;

/* The running realm's active document: the context opaque, because a JSContext is one navigable's document, so
 * there is no table to consult and no way to answer with another document. NULL before install, a state only the
 * accessors that tolerate it may see. */
static Document *doc_of(JSContext *ctx)
{
    return (Document *)JS_GetContextOpaque(ctx);
}

/* A dedicated worker realm's environment record, defined with its accessors below. */
typedef struct WorkerEnvironment {
    uint32_t doc;              /* the world-registry handle its work is filed under (its owner's) */
    PolicyContainer *policy;   /* owned */
    char *url;                 /* the worker global scope's url, its API base URL and creation URL (owned) */
    JSContext *owner;          /* the outside settings' realm, a reference this record holds */
} WorkerEnvironment;
static WorkerEnvironment *worker_env_of(JSContext *ctx);

/* The same pointer for the readers that dereference it, guarded by a CHECK because they dereference it in every
 * build. Two kinds of realm have no Document: a WorkerGlobalScope realm, which core/realm.h's per-realm column
 * builds without document_install and on which every `[Exposed]` member is placed (core/xhr/xml_http_request.c
 * reaches `document_doc` and `document_fully_active` from XHR §3.5.1 "The open() method" step 1, whose condition
 * "relevant global object is a Window object and its associated Document is not fully active" has its first
 * conjunct dropped here); and the first realm of an agent a host brought up with no navigable. The invariant is
 * stated once at the pointer every entry takes rather than per reader. It is a CHECK, not a DFAIL, because a
 * missing capability must abort in dev and `document_doc`'s answer is the handle a program is compiled under
 * (solver/engine.h), so a wild one compiles a page into another document's global. It adds no default: 0 is the
 * world registry's NONE, which would put a whole realm kind on one value. The macro passes the caller's site. */
/* Named residual: a realm that is not a Document cannot be named to the scheduler. Not covered: every route from
 * source text to a frontier flow is an `engine_queue_` entry taking a `uint32_t doc` handle (solver/flow.h refuses a
 * JSContext, since a handle survives a park), and the handle is a field of the Document record. Next diff builds the
 * decision of whether and how an environment has a world-registry name, before any field: the handle is an index
 * one step from HTML §8.1.3.1's environment id (`world_doc_name(handle)`), a Window realm may have a name or none
 * (the declaration-cycle fixture builds one with none), and minting a child-document name for a worker (§10.2.6.2
 * "Script settings for workers" sets up no Document) would misstate its creator. How its absence shows: a realm with
 * no Document reaches this line and aborts with `@E`, naming the asking component's file and line. */
static Document *doc_here_at(JSContext *ctx, const char *at_file, int at_line)
{
    Document *d = doc_of(ctx);

    CHECKF(d != NULL,
           "%s:%d asked a realm WHICH DOCUMENT it is in a realm that has none. TWO KINDS REACH HERE and this "
           "message used to name one. A WorkerGlobalScope realm is the first: core/realm.h's per-REALM column "
           "builds it and places every `[Exposed]` member on it. The second is the FIRST REALM OF AN AGENT A "
           "HOST BROUGHT UP WITH NO NAVIGABLE, which core/platform.c states is a `Window` realm and which has "
           "no Document either. Neither goes through document_install, so this realm has no Document and no "
           "world-registry handle — there is no answer to give and no default that would not answer for a "
           "different document. Either the asking member does not belong in a non-Window realm (Web IDL "
           "§3.3.7 \"[Exposed]\" decides that, from the corpus, at its own install), or the step it is running "
           "is conditioned on the global being a Window and this engine dropped the condition. If it belongs "
           "here, what must be built FIRST is not this handle but the decision the residual above names — what "
           "a non-Document realm's environment name IS and which table it holds it in — because no realm can "
           "state a name the world registry's DOCUMENT table has no kind for",
           at_file, at_line);
    return d;
}
#define doc_here(ctx_) doc_here_at((ctx_), IDL_SITE)

/* ---- The record's addresses: one producer, one release, nothing freed between ------------------------------
 *
 * A Document's URL, frozen base URL and about base URL are each a URL record's serialization, unbounded, so each is
 * an allocated string this record owns. An address is never freed on a change, which makes the pointer a legal COW
 * field: a flow that ran `history.pushState(s, "", "/b")` captured the old pointer as its baseline, and a context
 * switch writes it back, so the old string stays reachable from this list for the record's life (the capture's
 * target `&d->addr` is inside the record, so it is valid only that long too). The value beside it is counted, since
 * solver/cow.h's record arm dups it.
 * The record's life bounds the list: one string per distinct address, freed once each by doc_rec_release. Distinct
 * matters because §4.2.3 re-freezes a base URL on every situation that could change the first base element, so the
 * same serialization recurs, and an interned address is immutable, so returning the existing string is sound.
 * Refcounting strings by the delta entries naming them would need a new COW entry kind and would only reclaim the
 * addresses of a document that outlives its own navigations. */
static const char *doc_addr_intern(Document *d, const char *s)
{
    char *copy;
    int i;

    DCHECK(s != NULL, "a Document address was interned from nothing — the ABSENCE of one of these three URLs "
                      "is a NULL field, which is the fact §2.4.3's steps test, and never a string to store");
    for (i = 0; i < d->n_addrs; i++)
        if (strcmp(d->addrs[i], s) == 0) return d->addrs[i];
    if (d->n_addrs == d->cap_addrs) {
        int cap = d->cap_addrs ? d->cap_addrs * 2 : 4;
        char **g = realloc(d->addrs, (size_t)cap * sizeof *g);

        CHECK(g != NULL, "document: OOM recording a Document address — an address this record stops owning is "
                         "one a parked flow's delta still names, so the next context switch would write a "
                         "dangling pointer into the field every base-URL resolution reads");
        d->addrs = g;
        d->cap_addrs = cap;
    }
    copy = strdup(s);
    CHECK(copy != NULL, "document: OOM copying a Document address");
    d->addrs[d->n_addrs++] = copy;
    return copy;
}

/* Whether this pointer is one of this record's own addresses: the two-sided half of the intern, which lets the
   release free every slot without asking whether a field still points at it, and refuses a field holding a
   borrowed pointer. Linear, and it runs at address writes only. */
static int doc_addr_owned(const Document *d, const char *p)
{
    int i;

    for (i = 0; i < d->n_addrs; i++)
        if (d->addrs[i] == p) return 1;
    return 0;
}

/* Every address this record held, given back from doc_rec_release, the one place a record dies. The slots are
   distinct by construction (the intern returns an existing slot and adds only fresh allocations), so freeing the
   list frees each address once even when two fields name one string; the NULL-out makes a second pass visible. */
static void doc_addrs_free(Document *d)
{
    int i;

    DCHECK(d->n_addrs <= d->cap_addrs, "a Document's address list counts more entries than it has room for");
    DCHECK(d->addr.bytes == NULL || doc_addr_owned(d, d->addr.bytes),
           "a Document's ADDRESS was not one this record interned — the release is about to free the list and "
           "not this pointer, so the field named storage with a second owner or no owner at all");
    DCHECK(d->base.frozen == NULL || doc_addr_owned(d, d->base.frozen),
           "a Document's FROZEN BASE URL was not one this record interned");
    DCHECK(d->base.about == NULL || doc_addr_owned(d, d->base.about),
           "a Document's ABOUT BASE URL was not one this record interned");
    for (i = 0; i < d->n_addrs; i++) {
        DCHECK(d->addrs[i] != NULL, "a Document's address list holds an empty slot — every slot is written "
                                    "once by the intern and cleared only here, so an empty one has been freed "
                                    "already and this pass would free it twice");
        free(d->addrs[i]);
        d->addrs[i] = NULL;
    }
    free(d->addrs);
    d->addrs = NULL;
    d->n_addrs = d->cap_addrs = 0;
    d->addr.bytes = d->base.frozen = d->base.about = NULL;
}

/* ---- The record's counted references: one list, two consumers --------------------------------------------- */

/* A Document record is malloc'd C, so its references are invisible to the runtime's own walks, and two of them
   (`win_obj`, `proxy`) point back into the realm the record hangs off. Without telling the collector, gc_decref
   never subtracts them, the WindowProxy reads as externally rooted, the realm can never be collected, and since the
   realm's teardown hook is what releases the record, every child navigable's realm would live to JS_FreeRuntime.
   The list is written once because the release and the mark must agree exactly: a field the release frees and the
   mark omits leaks the realm, and one the mark reports and the release does not own over-subtracts a refcount. */
typedef void DocRefFn(JSContext *ctx, JSValue *pv, void *arg);

static void doc_rec_refs(Document *d, DocRefFn *fn, void *arg)
{
    fn(d->realm, &d->impl, arg);
    fn(d->realm, &d->selection, arg);
    /* §3.1.1's two ancestor-origins fields, both held and created together at §7.3.2.1. */
    fn(d->realm, &d->ancestor_origins, arg);
    fn(d->realm, &d->ancestor_origin_strings, arg);
    fn(d->realm, &d->proxy, arg);
    fn(d->realm, &d->win_obj, arg);
    fn(d->realm, &d->doc_obj, arg);
    /* The address's value, the counted half of DocAddress. The COW record arm dups it into a parked flow's delta, so
       a value this list forgot would be over-released at the record's death while a delta names it. DOC_ADDR_VALS
       is the same statement for the delta. */
    fn(d->realm, &d->addr.value, arg);
}

static void doc_ref_release(JSContext *ctx, JSValue *pv, void *arg)
{
    (void)arg;
    JS_FreeValue(ctx, *pv);
    *pv = JS_UNDEFINED;
}

/* A struct, not the function pointer itself through `void *`, because C does not define converting a function
   pointer to an object pointer. */
typedef struct { JSRuntime *rt; JS_MarkFunc *mark; } DocMarkArg;

static void doc_ref_mark(JSContext *ctx, JSValue *pv, void *arg)
{
    DocMarkArg *m = arg;

    (void)ctx;
    JS_MarkValue(m->rt, *pv, m->mark);
}

/* What this realm's records hold, answered to the collector: quickjs.h's JS_SetContextMarkHook, asked about every
   realm from inside a collection. It walks exactly what document_free walks: the realm's active record and the
   chain of documents it created at baseline. A record a flow created is owned by that flow's COW delta and reachable
   from no realm, so it is neither walked here nor released there; until the delta destroys it, its references
   correctly read as rooted from outside. */
static void document_realm_mark(JSRuntime *rt, JSContext *ctx, JS_MarkFunc *mark_func)
{
    DocMarkArg m;
    Document *c;

    m.rt = rt;
    m.mark = mark_func;
    for (c = doc_of(ctx); c; c = c->next_created) {
        DCHECK(c->realm == ctx,
               "a document record on a realm's baseline chain names a DIFFERENT realm — the chain is what that "
               "realm's teardown releases, so a foreign record on it would have its references reported by one "
               "realm's collection and freed by another's teardown");
        doc_rec_refs(c, doc_ref_mark, &m);
    }
}

/* The record for a DOM document, whatever realm is asking. NULL for a Lexbor document no record was built for, which
   is a solver scratch parse. */
static Document *doc_rec(const lxb_dom_document_t *dom)
{
    return dom ? (Document *)dom->user : NULL;
}

/* Whether this value implements `Document`: Web IDL §3.7 "Interfaces"' implementation check step 3 ("If object does
   not implement interface, then throw a TypeError."), as the predicate core/idl_args' idl_this_iface takes. §3.7.7
   "Operations"' create an operation function asks it before computing the effective overload set, so a member that
   states it at its declaration refuses a foreign receiver before §3.6 converts an argument (a conversion runs the
   page's own `toString`). doc_receiver's refusal is this same predicate. */
bool document_is(JSValueConst v)
{
    const lxb_dom_node_t *n = node_of(v);

    return n != NULL && n->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

/* The record for the document a member was called on. Every §4.5 member is `Document.prototype`'s, so its receiver
   names which document it is about: `foreignDoc.createElement` builds its element in foreignDoc, and reading the
   realm's active document instead would be wrong for every second Document. */
static Document *doc_receiver(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);
    Document *d;

    /* Web IDL §3.7.6 "Attributes" and §3.7.7 "Operations" state the same step for both member kinds this serves: a
       receiver that does not implement the interface is a TypeError at the read
       (`Object.getOwnPropertyDescriptor(Document.prototype, "URL").get.call(null)` is something the corpus does on
       purpose), so it is a throw, not a DCHECK. */
    if (!document_is(this_val)) {
        JS_ThrowTypeError(ctx, "this is not a Document");
        return NULL;
    }
    d = doc_rec(lxb_dom_interface_document(n));
    DCHECK(d != NULL, "a Document member ran on a Lexbor document with no record — document_install and "
                      "document_new are the two places one is built, and a tree that came from neither cannot "
                      "answer for its own address");
    return d;
}

/* §4.2.6 / §4.9 the selector members as one machine: querySelector, querySelectorAll, matches, closest. Each
 * compiles once and then walks, so the compiled list is what survives a suspension and the cursor is the resume
 * point; the four differ in where the cursor goes and what a match does, declared as a magic.
 * core/dom/selector_match.c makes the walk equivalent to lxb_selectors_find: a combinator is resolved by walking up
 * from the candidate through the whole document, so §4.2.6's scoped matching holds (`el.querySelectorAll('div p')`
 * finds a <p> under `el` whose <div> ancestor is outside `el`, since only the results are filtered to the subtree).
 * Across a rest point the machine holds the compiled selector and a cursor, nothing else, so it is forkable: the
 * parser finishes compiling before the first rest, the matching arena cleans itself after every match, and the
 * compiled list is shared by reference between forked arms. */
enum { QS_FIRST = 0, QS_ALL, QS_MATCHES, QS_CLOSEST };

/* Where this machine rests, as the standard numbers it: parse the selector, then match it (§1.3 for querySelector
 * and querySelectorAll through scope-match a selectors string, §4.9 for matches and closest). The match walks the
 * page's tree, so it rests at every node. The parse is one stage, because Lexbor's selector parser has no smaller
 * entry, so its length is the page's to choose. Splitting it means feeding the CSS tokenizer through
 * lxb_css_syntax_tokenizer_next_chunk and holding a live tokenizer across rests, which a fork could only shed and
 * re-feed, as core/html/fragment_parser.c does its HTML parser; until that is built this machine keeps the
 * forkable walk. */
#define QS_STAGES(X) \
    X(QS_PARSE, "DOM §1.3 steps 1-2 / §4.9 steps 1-2 (parse a selector; SyntaxError if it is failure)") \
    X(QS_MATCH, "DOM §1.3 step 3 (match a selector against a tree) / §4.9 matches step 3 / closest steps 3-5, " \
                "one node per step")
enum { IDL_STEP_STAGE_BASE(QS_STAGES) QS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const QS_STEPS[] = { QS_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* Which completion of a §6 predicate over an undetermined value this world is in. Two, and outcome 0 is "does not
   hold", by step_fork_run's numbering rule: a run with no forking policy (the @S candidate re-fire) takes 0, which
   is the arm the matcher's control flow already realises (an undetermined read behaves as no-match). */
enum { QS_PRED_FALSE = 0, QS_PRED_TRUE, QS_PRED_OUTCOMES };

typedef struct {
    SelectorList *compiled;   /* Selectors 4 §17.1's parsed selector, shared by reference with every forked arm */
    lxb_dom_node_t *root, *cursor;
    JSValue arr;      /* QS_ALL's collected matches (owned) */
    uint32_t n;
    /* The §6 predicate this flow has proved holds, on the state because the snapshot must carry it: a sibling
       minted at the fork resumes here and re-runs the match, whose answer depends on what is supplied, so an arm in
       a C local would never reach the sibling and the re-run would fork again forever. Its `over` is owned (qs_visit
       names it), since the value must outlive the park the arm was decided across. */
    SelectorUndet decided;
    /* The name of the fork this walk is asking: step_fork_run's `op`, half the constraint key. On the state because
       the driver reads `JSStepHdr::fork_op` after the machine returns JS_STEP_FORK; it is re-composed from the
       record on every entry, so what survives the park is `fork_ask_key`'s content hash. */
    char ask[SELECTOR_UNDET_KEY_MAX];
} QsState;

/* Everything this machine owns, which is what makes it forkable. The compiled selector is refcounted and read-only
   after selector_list_compile, so the sibling takes a reference and interior pointers stay valid in both arms; the
   cursor and root are tree pointers the COW delta isolates. The decided predicate's `over` is the third reference;
   the rest of that record, and `ask`, are inline bytes the fork's byte copy carries. */
static void qs_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    QsState *s = st;
    v->val(ctx, &s->arr);
    v->val(ctx, &s->decided.over);
    v->shared(ctx, (void **)&s->compiled, s->compiled ? &s->compiled->refs : NULL, selector_list_destroy);
}

static int js_document_qs(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    QsState *s = st;
    int magic = idl_step_magic(hdr);
    bool matched;

    (void)out_cb; (void)out_argc;
    /* This machine issues no request that delivers a value, so `cb_result` is always the driver's filler (on an
       ordinary re-entry and on both of a fork's entries), and releasing it unconditionally leaves no
       `step_fork_pending` question to get wrong. */
    JS_FreeValue(ctx, cb_result);

    if (hdr->stage == QS_PARSE) {
        lxb_dom_node_t *n = node_of(hdr->this_val);
        const char *sel;

        if (!n || argc < 1) {
            *presult = magic == QS_ALL ? collections_static(ctx, JS_NewArray(ctx))
                     : magic == QS_MATCHES ? JS_FALSE : JS_NULL;
            return JS_STEP_DONE;
        }
        sel = concolic_name_cstr(ctx, argv[0]);   /* an unknown argument passes as itself, so its name is its shape */
        if (!sel) return JS_STEP_ABRUPT;
        /* Selectors 4 §17.1, finished before the first rest point: the parser lives and dies inside
           selector_list_compile, and what comes back is the read-only list the walk reads. */
        s->compiled = selector_list_compile(sel);
        JS_FreeCString(ctx, sel);
        if (!s->compiled) {
            /* §4.2.6 and §4.9: an unparseable selector is a SyntaxError from all four members. */
            JS_ThrowDOMException(ctx, "SyntaxError", "not a valid selector");
            return JS_STEP_ABRUPT;
        }
        s->root = n;
        /* Where the cursor goes is all the four members differ in: a subtree for the two queries, the node for
           matches, the node and its ancestors for closest. */
        s->cursor = (magic == QS_FIRST || magic == QS_ALL) ? node_next_in(n, n) : n;
        if (magic == QS_ALL) {
            s->arr = JS_NewArray(ctx);
            CHECK(!JS_IsException(s->arr), "querySelectorAll could not allocate its result");
        }
        hdr->stage = QS_MATCH;
        return JS_STEP_YIELD;
    }

    DCHECK(hdr->stage == QS_MATCH, "a selector walk resumed into a stage its algorithm does not have");
    if (!s->cursor) {
        /* Ran out without a match. */
        switch (magic) {
        case QS_ALL:
            /* §4.2.6: a static NodeList, since the result does not track the tree, and a real one, so
               `instanceof NodeList` holds and `.map` is absent as in a browser. */
            *presult = collections_static(ctx, s->arr);
            s->arr = JS_UNDEFINED;
            break;
        case QS_MATCHES: *presult = JS_FALSE; break;
        default:         *presult = JS_NULL;  break;
        }
        return JS_STEP_DONE;
    }

    /* Selectors 4 §17.3 against this one node, and a fork where its answer is Kleene's third value. The loop
     * walks this node's questions, not the tree (the cursor does not move inside it): the matcher abandons a
     * compound at the first simple selector it cannot decide, so answers arrive one at a time (decide one
     * predicate, run the match again, and it concludes or declines on the next), as `step_ownkeys_chain` does,
     * and the flow ends up holding a constraint on every undetermined member. Re-running is sound because a match
     * runs no page code: it reads the tree, this flow's pins and its decided predicate. Every entry does it,
     * including the parent's fork delivery and the sibling's resume: neither carries a value, so an entry re-runs
     * with `decided` unchanged, declines on the same predicate, re-composes the same key and consumes its answer
     * at the same call site, which `JSStepHdr::fork_ask_key`'s content hash asserts. */
    for (;;) {
        SelectorUndet u;
        int arm, rc;

        matched = selector_match_node(s->cursor, s->compiled->list, NULL,
                                      s->decided.undetermined ? &s->decided : NULL, &u);
        /* A definite answer: every node of every document that put no unknown in an attribute. */
        if (!u.undetermined) break;
        /* An undetermined answer this fork cannot realise, which selector_match_node has already aborted on in dev
           (see SelectorUndet::forkable). This is that abort's release arm: `matched` is the answer under "the test
           does not hold". */
        if (!u.forkable) break;
        /* The same predicate declining again after this flow proved it holds would make this loop unbounded, and it
           cannot arise: host_attr_value_read grants the arm on `selector_pred_same`, the comparison asked here, and
           `SelectorUndet::forkable` refused every ask whose value has no identity and every operand that cannot
           satisfy its test. Its release arm breaks with `matched`, the "does not hold" answer, since a re-decline
           would be a sibling per re-match over one question that emits and explores nothing. */
        if (s->decided.undetermined && selector_pred_same(&s->decided, &u)) {
            DFAILF("DOM §1.3 step 3's walk proved `[%s\"%s\"]` HOLDS and the next match declined on that same "
                   "predicate — the supply at core/dom/selector_match.c's host_attr_value_read grants the arm "
                   "on selector_pred_same and on the value's own identity, and this asks the same comparison, "
                   "so a re-decline means the VALUE the match read is not the one the arm was decided about. "
                   "The usual cause is a flow whose attribute shadow moved under it between the fork and the "
                   "re-match, which a match cannot do because it runs none of the page's code",
                   u.attr, u.operand);
            break;
        }
        if (!selector_undet_key(s->ask, sizeof s->ask, &u)) {
            /* A truncated key files two questions under one name. SelectorUndet::forkable refuses a truncated field;
               this checks the composed key's width, so each bound is checked where it is. */
            DFAILF("DOM §1.3 step 3's walk could not spell the constraint key for `[%s\"%s\"]` — the key is what "
                   "a parked flow's recorded answers are filed under, and a truncated one lets one predicate's "
                   "arm decide another's. SELECTOR_UNDET_KEY_MAX is sized from SelectorUndet's own two buffers, "
                   "so reaching this means those two widths and this one have stopped agreeing",
                   u.attr, u.operand);
            break;
        }
        /* `real` is JS_OUTCOME_REAL_UNSTATED: which completion the predicate reaches on the operand's example is
           not computable here, because host_attr_value_read answers undetermined rather than handing over unproved
           bytes, so both arms run and neither is marked forced.
           Named residual. Not covered: a concolic with a concrete example has bytes the matcher could decide against,
           so `real` is knowable for that population. Next diff builds: a probe answer at the value seam supplying
           the example for one ask, so this machine can run the predicate on it. How its absence would show: a
           forced request whose provenance says no arm was forced, on a page whose attribute has an example. */
        rc = step_fork_run(ctx, hdr, u.over, s->ask, QS_PRED_OUTCOMES, JS_OUTCOME_REAL_UNSTATED, &arm);
        if (rc) return rc;   /* parked at the fork; the sibling resumes at this stage with the other completion */
        DCHECKF(arm == QS_PRED_FALSE || arm == QS_PRED_TRUE,
                "DOM §1.3 step 3's walk was answered with a completion it never declared (%d) — a §6 predicate "
                "has two and the numbering is `does not hold` first", arm);
        /* Outcome 0: the test does not hold, and the match just performed already answers under it. */
        if (arm == QS_PRED_FALSE) break;
        /* Outcome 1: it holds. Hand it to the next match, which realises it and concludes or declines on the next
           question.
           Named residual: one decided slot. Not covered: a compound with two undetermined members whose flow takes
           the holding arm of both, which needs both supplied to one match; overwriting the first would decide the
           second against the first's operand. Next diff builds: the slot as a set the machine's `visit` carries. How
           its absence shows: the DFAILF below, and in release that selector answering no-match. */
        if (s->decided.undetermined) {
            DFAILF("DOM §1.3 step 3's walk proved a SECOND §6 predicate holds in one match — `[%s\"%s\"]` "
                   "beside `[%s\"%s\"]` — and this machine supplies one at a time. A compound with two "
                   "undetermined members whose flow takes the holding arm of both needs both supplied to the "
                   "same match; build the decided slot into a SET the machine's `visit` carries. Until then the "
                   "second test is undetermined at the re-match and this selector answers no-match, which is "
                   "the answer every build gave before this fork existed",
                   s->decided.attr, s->decided.operand, u.attr, u.operand);
            break;
        }
        s->decided = u;
        s->decided.over = JS_DupValue(ctx, u.over);   /* owned from here: the park has to carry the value */
    }

    if (matched) {
        switch (magic) {
        case QS_ALL:
            JS_SetPropertyUint32(ctx, s->arr, s->n++, node_wrap(ctx, s->cursor));
            break;
        case QS_MATCHES:
            *presult = JS_TRUE;
            return JS_STEP_DONE;
        default:
            *presult = node_wrap(ctx, s->cursor);   /* the first in tree order, or the nearest ancestor */
            return JS_STEP_DONE;
        }
    }

    /* The decided predicate is spent when the cursor moves, and dropping it loses no fact: it is one match's supply,
       while the knowledge is in the flow's constraint record, so a later node with the same value under the same
       predicate has its answer replayed at the ask. Keeping it would make the one-slot limit apply to the whole
       walk, so two nodes holding two different unknowns under one selector would hit the abort above. */
    if (s->decided.undetermined) {
        JS_FreeValue(ctx, s->decided.over);
        memset(&s->decided, 0, sizeof s->decided);
    }
    switch (magic) {
    case QS_FIRST:
    case QS_ALL:     s->cursor = node_next_in(s->cursor, s->root); break;
    case QS_MATCHES: s->cursor = NULL;                             break;   /* this node alone */
    default:         s->cursor = s->cursor->parent;                break;   /* inclusive ancestors */
    }
    return JS_STEP_YIELD;
}

/* No `release`: everything this machine owns is a reference qs_visit names, so teardown discharges it. The parser
   dies in the compile and the matching arena is the agent's. */
static const IdlStepDecl QS_STEP = { js_document_qs, sizeof(QsState), qs_visit, NULL,
                                     "DOM §1.3 scope-match a selectors string / §4.9 Element.matches, closest",
                                     QS_STEPS };

const IdlStepDecl *document_qs_decl(void) { return &QS_STEP; }

/* §3.1.7 "DOM tree accessors"' element shortcuts: forms, images, scripts, embeds and links. Each is a live
   HTMLCollection of the elements of a type in the document, so each is the by-name collection with a tag baked in;
   `links` is a predicate instead (`a`/`area` with an href). Bundle scanners read `document.scripts` and
   `document.forms` in particular.
   magic 0 = forms, 1 = images, 2 = scripts, 3 = embeds, 4 = links. */
static JSValue js_doc_shortcut(JSContext *ctx, JSValueConst this_val, int magic)
{
    static const char *const TAGS[] = { "form", "img", "script", "embed" };
    if (magic == 4) return collections_links(ctx, this_val);
    DCHECK(magic >= 0 && magic < (int)(sizeof(TAGS) / sizeof(TAGS[0])),
           "a document element-shortcut ran with a magic it does not have");
    return collections_by_name(ctx, this_val, TAGS[magic], false);
}

/* HTML §3.1.7 "DOM tree accessors"' `NodeList getElementsByName(DOMString elementName)`: "return a live NodeList
   containing all the HTML elements in that document that have a name attribute whose value is identical to the
   elementName argument, in tree order". A Document member only (DOM §4.5 and §4.9 declare the other by-name walks
   on both interfaces), so it is not beside js_node_by_name. Every read of it in the committed site corpus is
   unguarded (Next.js's AppRouterAnnouncer on every app-router mount, and its hash-scroll path), so its absence
   would end the flow with a TypeError. The walk is the collection component's. */
static JSValue js_doc_by_element_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    const char *name;
    JSValue r;

    (void)magic;
    DCHECK(argc >= 1, "getElementsByName reached its body with no argument — its declaration states one "
                      "REQUIRED DOMString and no idl_optional_from, so Web IDL §3.6's argument-count "
                      "TypeError did not run for it");
    /* The argument may be unknown, and an unknown denotes its shape, as for getElementsByTagName:
       `getElementsByName(decodeURIComponent(location.hash))` is an ordinary spelling, and a plain ToString would
       spend its provenance on bytes no run computed. */
    name = concolic_name_cstr(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    r = collections_by_element_name(ctx, this_val, name);
    JS_FreeCString(ctx, name);
    return r;
}

/* §4.5 createDocumentFragment(): the same object `new DocumentFragment()` builds, under its member name. A page
   batches inserts into one and attaches it once. */
static JSValue js_doc_create_fragment(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_document_fragment_t *frag;

    (void)argc; (void)argv; (void)magic;
    DCHECK(n != NULL, "createDocumentFragment ran on a receiver that is not a Document — the declaration states "
                      "Web IDL §3.7 Interfaces' implementation check, so reaching the body means "
                      "idl_implementation_check did not run for it");
    frag = lxb_dom_document_fragment_interface_create(n->owner_document);
    CHECK(frag != NULL, "createDocumentFragment: the Lexbor fragment allocation failed");
    /* This flow made it, as its siblings on this interface say of theirs. A fragment is emptied by the insertion
       that consumes it and never becomes reachable from the tree, so without an owner its node would stay in the
       document's Lexbor arena, invisible to gc_obj_list. */
    dom_cow_note_created(lxb_dom_interface_node(frag));
    return node_wrap(ctx, lxb_dom_interface_node(frag));
}

/* The HTML namespace, spelled once for this file; core/html/trusted_types.c, core/html/sanitizer.c and
   core/xml/xml_serialize.c each keep their own, since it is a constant of the standards. */
#define DOC_NS_HTML "http://www.w3.org/1999/xhtml"

/* DOM §4.5 createElement. The element is created in this document and returned detached, since a page builds a
 * subtree and attaches it later; nothing observable changes until appendChild, which is a per-flow write.
 * Its namespace is §4.5's step 4, which is conditional, so this body states it rather than using
 * lxb_dom_document_create_element, which reads `lxb_dom_document_t::type`; §4.5's `type` is on this engine's record
 * and never mirrored there. Step 4: "Let namespace be the HTML namespace, if this is an HTML document or this's
 * content type is "application/xhtml+xml"; otherwise null." An XHTML document is an XML document that creates in the
 * HTML namespace, so neither term alone is the rule. Step 2 ("If this is an HTML document, then set localName to
 * localName in ASCII lowercase") is the same question, since `lxb_tag_append_lower` folds every name it interns;
 * both are asked once, of the record. The answer comes from `d`, §4.5's "this": `foreignDoc.createElement("p")` is
 * `foreignDoc`'s question. */
/* DOM §4.5 createElement step 1's predicate over a string value: whether it is a valid element local name. */
static bool doc_string_is_valid_local_name(JSContext *ctx, JSValueConst v)
{
    size_t len = 0;
    const char *b = JS_ToCStringLen(ctx, &len, v);
    bool ok;

    CHECK(b != NULL, "createElement could not read the bytes of a string it was handed");
    ok = dom_valid_element_local_name(b, len);
    JS_FreeCString(ctx, b);
    return ok;
}

static JSValue js_doc_create_element(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    const char *tag;
    size_t tag_len = 0;
    char *folded = NULL;
    const char *local, *ns;
    lxb_dom_element_t *el;
    lxb_dom_document_t *dom;
    Document *d;
    JSValue r;

    if (argc < 1) return JS_NULL;
    /* §4.5 createElement over an unknown tag name. Lexbor needs real bytes, and `createElement(someParameter)` is a
       real and XSS-relevant pattern (the tag decides whether the node executes), so the answer is an unknown derived
       from the source rather than a node of a guessed name, about which every later query and sink would be wrong. */
    if (concolic_is(argv[0])) {
        /* The arm in which the name is valid (js_doc_create_element_step forked step 1's two completions first).
           The creation runs on the tag this source concretely is: an example propagates only because the engine
           performs the real operation on the concrete, so this re-enters with the example and the element it builds
           becomes the derived value's example. An example that is not a valid name does not belong to this arm, and
           with none the answer is unknown, a non-answer, not a guess. */
        JSValue ex = concolic_example(ctx, argv[0]), real = JS_UNDEFINED;
        if (JS_IsString(ex) && doc_string_is_valid_local_name(ctx, ex)) {
            JSValueConst a2[1];
            a2[0] = ex;
            real = js_doc_create_element(ctx, this_val, 1, a2, magic);
            CHECK(!JS_IsException(real), "createElement refused a local name step 1 accepted");
        }
        JS_FreeValue(ctx, ex);
        return concolic_builtin_hook(ctx, argv[0], "createElement", real);
    }
    /* Length-carrying, as in createElementNS: §1.4 decides what a name may contain, and `strlen` would stop at a
       U+0000, which is a different name. */
    tag = JS_ToCStringLen(ctx, &tag_len, argv[0]);
    if (!tag) return JS_EXCEPTION;
    /* §4.5 step 5's "create an element given this": the node document is the receiver's, never the realm's active
       one (`foreignDoc.createElement("p")` builds a node of foreignDoc). */
    d = doc_receiver(ctx, this_val);
    if (!d) { JS_FreeCString(ctx, tag); return JS_EXCEPTION; }
    dom = lxb_dom_interface_document(d->dom);
    local = tag;
    if (!d->is_xml) {                                               /* step 2 */
        size_t i;
        folded = js_malloc(ctx, tag_len + 1);
        CHECK(folded != NULL,
              "createElement could not copy its own local name — §4.5 step 2 folds it and the argument's bytes "
              "belong to the JSString, which this must not write through");
        for (i = 0; i < tag_len; i++)
            folded[i] = (tag[i] >= 'A' && tag[i] <= 'Z') ? (char)(tag[i] - 'A' + 'a') : tag[i];
        folded[tag_len] = '\0';
        local = folded;
    }
    /* Step 4, both terms: §4.5's "A document is said to be an XML document if its type is `xml`; otherwise an HTML
       document", and a compare against the content type, a separate field because HTML §7.5.4 "Loading text
       documents" makes an HTML document whose content type is `text/plain` (see DocumentKind). */
    ns = (!d->is_xml || strcmp(d->content_type, "application/xhtml+xml") == 0) ? DOC_NS_HTML : NULL;
    el = element_create_ns(dom, ns, ns ? sizeof(DOC_NS_HTML) - 1 : 0, local, tag_len, NULL, 0);
    js_free(ctx, folded);
    JS_FreeCString(ctx, tag);
    /* Two-sided: element_create_ns interns the namespace and picks the element's C struct from the id it gets
       back, so a namespace other than the one step 4 chose would give a struct and a namespace from two different
       elements (core/dom/element.c records a case-folding probe answering LXB_NS_HTML for
       `HTTP://WWW.W3.ORG/1999/XHTML`). Asked here, where step 4's choice was made. */
    DCHECK(lxb_dom_interface_node(el)->ns == (ns != NULL ? LXB_NS_HTML : LXB_NS__UNDEF),
           "DOM §4.5 \"Interface Document\"'s createElement step 4 chose a namespace and the element came back "
           "in another one — the element's C struct is picked from the interned namespace id, so the two "
           "disagreeing means the node reports a namespace its own interface does not implement");
    dom_cow_note_created(lxb_dom_interface_node(el));   /* this flow made it: the delta owns it */
    r = element_wrap(ctx, el);
    return r;
}

/* DOM §4.5 createElement as the algorithm it delegates to: §4.9 "create an element" with
 * `synchronousCustomElements` true, so a custom element's constructor runs inside document.createElement.
 * It is a machine because step 5.1.4.1 is "constructing C with no arguments" and C is the page's class, whose body
 * can loop, await and mutate the DOM, so a construct is a request the machine rests on.
 * Step 5.1.4 is "run these steps while catching any exceptions", so a throwing constructor answers with an element,
 * not a propagating throw: the construct and the checks after it are inside the catch, so a constructor that
 * throws, or returns the wrong node, reports (fires `error` at the global) and answers with an HTMLUnknownElement of
 * the requested local name whose custom element state is "failed". The capability is declared
 * (IdlStepDecl::catches_abrupt), because the abrupt then arrives at this body's own request site. */
#define DCE_STAGES(X) \
    X(DCE_LOOKUP,    "DOM §4.5 steps 1-5 into §4.9 steps 1-3 (the local name, and looking up a custom element " \
                     "definition for it), §4.9 step 6 when there is none, and §4.9 steps 5.1.1-5.1.3 (the " \
                     "active custom element constructor map entry the Construct runs under) when there is") \
    X(DCE_UPGRADE,   "DOM §4.9 step 4.3's catching list (upgrade result using definition — HTML §4.13.5, " \
                     "which parks on the page's constructor at its own step 10.3)") \
    X(DCE_UP_REPORT, "DOM §4.9 step 4.3's threw-list (report the exception the upgrade threw), which is HTML " \
                     "§8.1.4.6 report an exception") \
    X(DCE_CONSTRUCT, "DOM §4.9 step 5.1.4.1 (constructing C with no arguments — the page's constructor)") \
    X(DCE_CHECKS,    "DOM §4.9 steps 5.1.4.2-11 (what the constructor returned, checked against what this " \
                     "operation asked for)") \
    X(DCE_REPORT,    "DOM §4.9 step 5.1.4's exception arm (report the exception the construct or the checks " \
                     "threw), which is HTML §8.1.4.6 report an exception")
enum { IDL_STEP_STAGE_BASE(DCE_STAGES) DCE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DCE_STEPS[] = { DCE_STAGES(JS_STEP_STAGE_LABEL) NULL };
/* The question §4.5 step 1 asks of an unknown local name; completion 0 is the valid name, 1 InvalidCharacterError. */
#define DCE_NAME_OP "DOM §4.5 createElement step 1 (localName is a valid element local name)"

/* DOM §4.5's "flatten element creation options", given `options` and the document the creation is for. It answers
 * two things ("Return registry and is"), one algorithm because step 3.2.1 makes naming both an `is` and a
 * `customElementRegistry` a NotSupportedError. `*pis` is JS_NULL for step 2's "let is be null" (an absent member)
 * or the string step 3.1 found, set on every exit including the throwing ones.
 * Step 1 is per document: "Let registry be the result of looking up a custom element registry given document",
 * which answers NULL for a document that is not a Window's (createHTMLDocument, DOMParser, `new Document()`), so the
 * receiver names the document; this realm's registry would answer for the wrong one under
 * `otherDoc.createElement("x-y")`. `is` drives HTML §4.13.3's lookup step 4 and DOM §4.9's customized-built-in arm.
 * Returns the registry (a CustomElementRegistry or JS_NULL, owned), or JS_EXCEPTION with the step's exception
 * pending, in which case `*pis` is JS_NULL and owns nothing. Otherwise `*pis` is the is value, owned. */
static JSValue flatten_creation_options(JSContext *ctx, JSValueConst doc_wrap, JSValueConst options,
                                        JSValue *pis)
{
    JSValue registry = custom_elements_node_registry(ctx, doc_wrap);   /* step 1 */
    JSValue is_v, reg_v;

    *pis = JS_NULL;                                                    /* step 2: "Let is be null" */

    /* Step 3, "if options is a dictionary". §3.2.25's union was resolved by the declaration (null, undefined and
       every Object take the dictionary arm), so a converted dictionary object is the answer, and both members
       below were read as rest points. Unknown external input crosses the declaration as itself, with no members
       this algorithm can name, so the answer is an absent options'. */
    if (!JS_IsObject(options) || concolic_is(options)) return registry;
    is_v = idl_dict_get(ctx, options, "is");                           /* step 3.1 */
    reg_v = idl_dict_get(ctx, options, "customElementRegistry");
    /* Web IDL §3.2.15 on the member's declared type `CustomElementRegistry?`, before the algorithm's steps as a
       conversion is, so `{is:"x-y", customElementRegistry:5}` is a TypeError, not step 3.2.1's NotSupportedError.
       Named residual: the check is in the body, not the declaration. Not covered: a browser throws inside
       §3.2.17's member walk, before `is` (which sorts after this member) is read. Next diff builds: the member
       declared with IdlDictMember's `iface_is`/`iface_name` and `custom_elements_is_registry`, the shared diff
       §4.9's attachShadow names for the identical member. How its absence shows: an `is` getter that runs before
       the TypeError. */
    if (!JS_IsUndefined(reg_v) && !JS_IsNull(reg_v) && !custom_elements_is_registry(reg_v)) {
        JS_FreeValue(ctx, is_v);
        JS_FreeValue(ctx, reg_v);
        JS_FreeValue(ctx, registry);
        return JS_ThrowTypeError(ctx, "ElementCreationOptions's customElementRegistry is not a "
                                      "CustomElementRegistry");
    }
    if (!JS_IsUndefined(reg_v)) {                                      /* step 3.2: the member exists */
        if (!JS_IsUndefined(is_v)) {                                   /* step 3.2.1 */
            JS_FreeValue(ctx, is_v);
            JS_FreeValue(ctx, reg_v);
            JS_FreeValue(ctx, registry);
            return JS_ThrowDOMException(ctx, "NotSupportedError",
                                        "an element creation cannot name both `is` and "
                                        "`customElementRegistry`");
        }
        JS_FreeValue(ctx, registry);
        registry = reg_v;                                              /* step 3.2.2 (the reference moves) */
    } else {
        JS_FreeValue(ctx, reg_v);
    }
    /* Step 3.1's answer, "If options["is"] exists, then set is to it". The member is declared DOMString, so the
       converted value is the string DOM §4.9 takes; an absent member is JS_UNDEFINED, step 2's null. */
    if (JS_IsUndefined(is_v)) JS_FreeValue(ctx, is_v);
    else *pis = is_v;
    /* Step 3.3. A creation may name a scoped registry or the document's own; any other is one this document resolves
       nothing in, and every later lookup on the element would answer from it. */
    if (JS_IsObject(registry) && !custom_elements_registry_is_scoped(ctx, registry)) {
        JSValue doc_reg = custom_elements_node_registry(ctx, doc_wrap);
        bool same = JS_VALUE_GET_PTR(doc_reg) == JS_VALUE_GET_PTR(registry);

        JS_FreeValue(ctx, doc_reg);
        if (!same) {
            JS_FreeValue(ctx, registry);
            JS_FreeValue(ctx, *pis);
            *pis = JS_NULL;
            return JS_ThrowDOMException(ctx, "NotSupportedError",
                                        "an element creation was given a custom element registry that is "
                                        "neither scoped nor this document's");
        }
    }
    return registry;
}

typedef struct {
    uint8_t phase;      /* the construct request's own cursor */
    JSValue cb[1];      /* [ctor] — §4.9 step 5.1.4.1 passes no arguments, so the buffer is one slot */
    JSValue def;        /* step 3's definition (owned) */
    /* §4.9's "create an element internal" result, made before step 3's lookup (see DCE_LOOKUP): the element step 6
       answers with and step 5.1.4's failure arm answers with; only a constructor that returns discards it. Owned. */
    JSValue el;
    JSValue local;      /* the local name the operation was given (owned); step 5.1.4.8 compares against it */
    JSValue result;     /* what the page's constructor answered (owned) */
    JSValue exc;        /* step 5.1.4's caught exception (owned), held across the report's own park */
    /* Step 5.1.1's `C`, held because step 5.1.4 constructs it across a park, and a `define` inside the page's
       constructor may have replaced the definition's entry, while the class step 5.1.4 resumes into must be the one
       step 5.1.1 named. Owned. The restore step 5.1.6 owes is the machine's and holds its own reference (see
       idl_active_ctor_owed). */
    JSValue ctor;
    /* Whether steps 5.1.2-5.1.3 ran. The paired restore is the machine's (idl_active_ctor_owed); step 5.1.4 asserts
       this before constructing, since an unentered map means HTML §3.2.3 "HTML element constructors" step 3 would
       answer from the document's registry for a creation that may have named a scoped one. A flag, not
       `JS_IsObject(s->ctor)`, because the two are different facts. */
    uint8_t entered;
    ReportExceptionWork rw;
    /* HTML §4.13.5's cursor, for DOM §4.9 step 4.3, the customized-built-in arm, which upgrades rather than
       constructs; the same record the reaction drain embeds (custom_elements.h says why it is a struct). */
    CeUpgrade up;
} DocCreateElState;

static void doc_create_el_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    DocCreateElState *s = st;
    v->val(ctx, &s->cb[0]);
    v->val(ctx, &s->def);
    v->val(ctx, &s->el);
    v->val(ctx, &s->local);
    v->val(ctx, &s->result);
    v->val(ctx, &s->exc);
    v->val(ctx, &s->ctor);
    report_exception_work_visit(ctx, &s->rw, v);
    custom_elements_upgrade_visit(ctx, &s->up, v);
}

/* What this algorithm took from outside itself and owes back on every exit: §8.1.4.6 step 5's error-reporting flag
   on the global. Values are named by doc_create_el_visit, the one list teardown discharges. DOM §4.9 steps
   5.1.5-5.1.6 also owe every exit (a flow discarded while parked on the constructor never resumes, and the
   agent-wide map entry would answer every later `new C()` in the agent from this creation's registry), but removing
   the entry drops a reference to `C`, which doc_create_el_visit names, and a `release` runs inside idl_args.c's
   fingerprint bracket, which compares holder counts and cannot tell that give-back from a discharged declaration.
   So the give-back is declared to the machine at the enter (idl_active_ctor_owed) and performed at its teardown,
   below the bracket, beside §4.13.5's half. */
static void doc_create_el_release(JSContext *ctx, void *st)
{
    DocCreateElState *s = st;

    report_exception_work_unlock(ctx, &s->rw);
    /* The same obligation for step 4.3's arm: HTML §4.13.5 step 10's regardless-list is owed by every exit from its
       step 10, including a flow discarded while parked on the upgrade's Construct (the map entry from steps 8-9 is
       agent-wide, and step 6's construction-stack entry is read by the next `super()`). A no-op when no upgrade is
       in flight. It is placed here because §4.13.5 holds its own `C` on the cursor, not a declared value. Steps 4
       and 5 are the two arms of one `if`, so only one of the two entries ever ran. */
    custom_elements_upgrade_unlock(ctx, &s->up);
}

static int js_doc_create_element_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                      JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    DocCreateElState *s = st;
    int r;

    if (hdr->stage == DCE_LOOKUP) {
        JSValue registry, is;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* Every owned field before the first thing that can throw: the failure path tears the state down through
           doc_create_el_release, which frees exactly what the state holds. */
        s->cb[0] = s->def = s->el = s->local = s->result = s->exc = s->ctor = JS_UNDEFINED;
        s->entered = 0;
        report_exception_work_start(&s->rw);
        custom_elements_upgrade_init(&s->up);
        /* §4.5 createElement step 3, whatever the local name: `{is, customElementRegistry}` refuses itself before
           any element is created. */
        registry = flatten_creation_options(ctx, hdr->this_val, argc > 1 ? argv[1] : JS_UNDEFINED, &is);
        if (JS_IsException(registry)) return -1;      /* `is` is JS_NULL on that exit and owns nothing */
        /* §4.9 step 3 needs a name to look a definition up by, and a concolic tag has none. §4.5 step 1 ("If
           localName is not a valid element local name, then throw an InvalidCharacterError") has two feasible
           completions over an unknown name, and the element arm is the creation below, over the source's
           example. The state is complete at the fork (every field set above), and a re-entry re-runs this stage
           to here with nothing done twice: the options were converted before the body ran. */
        if (concolic_is(argv[0])) {
            JSValue ex = concolic_example(ctx, argv[0]);
            int real = JS_IsString(ex) ? !doc_string_is_valid_local_name(ctx, ex) : JS_OUTCOME_REAL_UNSTATED;
            int rc, arm = 0;

            JS_FreeValue(ctx, ex);
            JS_FreeValue(ctx, registry);
            JS_FreeValue(ctx, is);
            rc = step_fork_run(ctx, hdr, argv[0], DCE_NAME_OP, 2, real, &arm);
            if (rc)
                return rc;
            DCHECKF(arm == 0 || arm == 1, "createElement's step 1 fork answered completion %d", arm);
            if (arm == 1) {
                JS_ThrowDOMException(ctx, "InvalidCharacterError", "not a valid element local name");
                return -1;
            }
            *presult = js_doc_create_element(ctx, hdr->this_val, argc, argv, 0);
            return JS_IsException(*presult) ? -1 : 0;
        }
        /* An `is` that is unknown external input is resolved, where its source carries an example, the way the
           local name is: the engine runs the real operation on the concrete, so
           `createElement("button", {is: location.hash.slice(1)})` looks up the definition the example names. Where
           there is none, the two differ: an unknown local name leaves nothing to create (§4.9 step 3 and step 6.1
           need it), while an unknown is value still creates the same element and leaves only the definition
           unknown. The member's declared type is DOMString, and §3.2.17's member loop hands a concolic member
           across uncoerced. */
        if (concolic_is(is)) {
            JSValue ex = concolic_example(ctx, is);

            if (JS_IsString(ex)) {
                JS_FreeValue(ctx, is);
                is = ex;
            } else {
                /* No example is not the null is value: a source with no example still holds a string whose bytes
                   this run has not learned, and DOM §4.9 passes whatever `is` holds to create an element internal
                   step 2. Null would claim no customized built-in (step 6.3 skipped, `<button>` "uncustomized",
                   `:defined` true, attachInternals allowed). The unknown rides through; readers of its bytes carry
                   their own residuals in custom_elements.c. */
                JS_FreeValue(ctx, ex);
            }
        }
        /* §4.9's "create an element internal", performed before step 3's lookup, because the lookup over
           (registry, namespace, local name, is) that can answer from a scoped registry reads them off an element.
           Step 6 answers with this element, as does step 5.1.4's failure arm; only a constructor returning its own
           element leaves it unused. */
        s->el = js_doc_create_element(ctx, hdr->this_val, argc, argv, 0);
        if (JS_IsException(s->el)) {
            s->el = JS_UNDEFINED;
            JS_FreeValue(ctx, registry);
            JS_FreeValue(ctx, is);
            return -1;
        }
        /* "…custom element registry to registry", through the component that owns the association (the once-only
           rule and the scoped-registry latch belong with the slot). */
        custom_elements_node_associate_registry(ctx, s->el, registry);
        /* "…and is value to is": create an element internal step 2, before step 3's lookup, whose step 4 reads it
           (`<button is="my-btn">` is found by name `my-btn` and local name `button`). The same entry HTML §13.2.6.1's
           create an element for the token step 5 writes through. The namespace is not asked here: create an element
           internal sets the is value unconditionally, and the HTML-namespace condition is DOM §4.9's step 6.3,
           which that entry applies, so `new Document()`'s null-namespace element keeps "uncustomized" (and HTML
           §4.13.3's lookup step 2 refuses a non-HTML namespace). */
        if (JS_IsString(is) || concolic_is(is)) {
            lxb_dom_node_t *made = node_of(s->el);
            size_t ilen = 0;
            const char *iv;

            DCHECK(made != NULL && made->type == LXB_DOM_NODE_TYPE_ELEMENT,
                   "DOM §4.9 create an element internal answered with something that is not an element, and "
                   "the is value about to be written is a fact about an ELEMENT — the cast below would "
                   "otherwise read another node kind's struct as one");
            /* Two spellings of one write: a known is value goes as bytes, so the entry can own the realm resolution
               the markup producer also needs; an unknown one goes as itself, uncoerced, since ToString would store
               its shape as a plausible string. */
            if (concolic_is(is)) {
                custom_elements_created_with_is_value(lxb_dom_interface_element(made), NULL, 0, is);
            } else {
                iv = JS_ToCStringLen(ctx, &ilen, is);
                if (!iv) { JS_FreeValue(ctx, registry); JS_FreeValue(ctx, is); return -1; }
                custom_elements_created_with_is_value(lxb_dom_interface_element(made), iv, ilen, JS_UNDEFINED);
                JS_FreeCString(ctx, iv);
            }
        }
        JS_FreeValue(ctx, is);
        s->def = custom_elements_definition_lookup_for_element(ctx, s->el);   /* §4.9 step 3 */
        if (!JS_IsObject(s->def)) {                  /* §4.9 step 6: not a custom element */
            JS_FreeValue(ctx, registry);
            *presult = s->el;
            s->el = JS_UNDEFINED;
            return 0;
        }
        /* §4.9 step 4: "If definition is non-null, and definition's name is not equal to its local name (i.e.,
           definition represents a customized built-in element)". Steps 4.1-4.2 are the element this stage made
           (the interface for localName and the HTML namespace; state "undefined" from the is-value write). This arm
           upgrades rather than constructs: step 4.3 runs "Upgrade result using definition" while catching
           exceptions, and HTML §4.13.5 steps 8-9 push their own map entry keyed on the element's registry and give it
           back in step 10, so steps 5.1.1-5.1.3's bracket is not entered here. */
        if (custom_elements_definition_is_customized_builtin(ctx, s->def)) {
            JS_FreeValue(ctx, registry);
            STEP_GOTO(hdr->stage, DCE_UPGRADE, &s->phase, NULL);
            goto upgrade;
        }
        /* §4.9 step 5.1.1: `C` is the definition's constructor, held on the state because step 5.1.6 names it from
           an exit this stage cannot see. */
        s->ctor = custom_elements_definition_constructor(ctx, s->def);
        /* §4.9 steps 5.1.2-5.1.3, which bracket step 5.1.4's construct: `previousRegistry` is read and map[C] set to
           `registry`, the only thing telling HTML §3.2.3 "HTML element constructors" step 3 which registry the
           element its `super()` mints belongs to (without it a class defined in a scoped registry cannot find its
           own definition). */
        custom_elements_active_ctor_enter(ctx, s->ctor, registry);
        /* The restore is owed to the machine, not written after the construct or in `release`: step 5.1.4 parks on
           page code, so its three exits (checks done, report done, flow discarded) share only the machine's
           teardown, and the give-back drops a reference to `C` (see doc_create_el_release). */
        idl_active_ctor_owed(ctx, hdr, s->ctor);
        s->entered = 1;
        JS_FreeValue(ctx, registry);
        /* Step 5.1.4.8 compares what the constructor made against the name this operation was given, already
           converted to a string by the declaration. */
        s->local = JS_DupValue(ctx, argv[0]);
        STEP_GOTO(hdr->stage, DCE_CONSTRUCT, &s->phase, NULL);
    }
upgrade:
    /* DOM §4.9 step 4.3's catching list, "Upgrade result using definition": HTML §4.13.5 "Upgrades", which parks on
       the page's constructor at its own step 10.3. It is the implementation the §4.13.6 reaction drain runs, called
       now rather than enqueued, which is what `synchronousCustomElements` true means here (step 4.4's "Otherwise"
       arm is unreachable). The cursor is a CeUpgrade this state embeds and the park is an ordinary
       JS_STEP_CONSTRUCT. */
    if (hdr->stage == DCE_UPGRADE) {
        JSValue thrown = JS_UNDEFINED;

        r = custom_elements_upgrade_run(ctx, &s->up, s->el, s->def, cb_result, out_cb, out_argc, &thrown);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;                          /* parked inside §4.13.5 step 10.3's Construct */
        if (!JS_IsUndefined(thrown)) {
            s->exc = thrown;
            STEP_GOTO(hdr->stage, DCE_UP_REPORT, &s->phase, NULL);
        } else {
            /* §4.9 step 7, "Return result": the element this stage created, which the upgrade ran the page's class
               over, the same node the markup form would give. */
            *presult = s->el;
            s->el = JS_UNDEFINED;
            return 0;
        }
    }
    if (hdr->stage == DCE_UP_REPORT) {
        /* DOM §4.9 step 4.3's threw-list, both entries. The report is HTML §8.1.4.6, which fires `error` at the
           global (page code, so it parks); and the element returned is the one being upgraded, with state
           "failed", not a fresh HTMLUnknownElement: a customized built-in whose class threw is still a `button`. */
        r = report_exception_run(ctx, &s->rw, s->exc, cb_result, out_cb, out_argc);
        if (r > 0) return r;                          /* parked inside the `error` event's own dispatch */
        JS_FreeValue(ctx, s->exc);
        s->exc = JS_UNDEFINED;
        DCHECK(JS_IsObject(s->el),
               "DOM §4.9 step 4.3's threw-list has no element to set a custom element state on — step 4.2's "
               "element is created before step 3's lookup and held for exactly these two exits, so a report "
               "reached without one came from a stage that never made it");
        custom_elements_mark_failed(ctx, s->el);
        *presult = s->el;
        s->el = JS_UNDEFINED;
        return 0;
    }
    if (hdr->stage == DCE_CONSTRUCT) {
        /* §4.9 step 5.1.4.1: constructing C, the value step 5.1.1 named and steps 5.1.2-5.1.3 entered, constructed
           from the state so it matches what step 5.1.6 restores even if a `define` inside the constructor replaced
           the definition's entry. */
        JSValue made = JS_UNDEFINED;

        DCHECK(s->entered, "DOM §4.9 step 5.1.4 was reached without steps 5.1.2-5.1.3 — the Construct below is "
                           "what that pair brackets, so an unentered map means HTML §3.2.3 \"HTML element "
                           "constructors\" step 3 is about to answer out of the document's registry for a "
                           "creation that may have named a scoped one");
        r = step_construct_run(ctx, &s->phase, STEP_CB(s->cb), s->ctor, 0, NULL, cb_result, &made,
                               out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;                          /* parked on the page's constructor */
        /* Step 5.1.4's catch: the construct threw, synchronously (r < 0) or delivered as JS_EXCEPTION, which this
           member's catches_abrupt asks the driver for. */
        if (r < 0 || JS_IsException(made)) {
            JS_FreeValue(ctx, made);
            s->exc = JS_GetException(ctx);
            STEP_GOTO(hdr->stage, DCE_REPORT, &s->phase, NULL);
            goto report;
        }
        s->result = made;
        STEP_GOTO(hdr->stage, DCE_CHECKS, &s->phase, NULL);
    }
    if (hdr->stage == DCE_CHECKS) {
        size_t len = 0;
        const char *local = JS_ToCStringLen(ctx, &len, s->local);
        Document *d = doc_receiver(ctx, hdr->this_val);

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (!local || !d) { if (local) JS_FreeCString(ctx, local); return -1; }
        r = custom_elements_created_check(ctx, s->result,
                                          lxb_dom_interface_document(d->dom), local, len);
        JS_FreeCString(ctx, local);
        /* The checks are inside step 5.1.4's catch too: a constructor that gave its element an attribute, a child,
           a parent or the wrong local name throws a NotSupportedError that is reported. */
        if (r < 0) {
            s->exc = JS_GetException(ctx);
            STEP_GOTO(hdr->stage, DCE_REPORT, &s->phase, NULL);
            goto report;
        }
        *presult = s->result;
        s->result = JS_UNDEFINED;
        return 0;
    }
report:
    DCHECK(hdr->stage == DCE_REPORT, "document.createElement resumed into a stage DOM §4.9 does not have");
    r = report_exception_run(ctx, &s->rw, s->exc, cb_result, out_cb, out_argc);
    if (r > 0) return r;                              /* parked inside the `error` event's own dispatch */
    JS_FreeValue(ctx, s->exc);
    s->exc = JS_UNDEFINED;
    /* "Set result to the result of creating an element internal given document, HTMLUnknownElement, localName,
       the HTML namespace, prefix, "failed", null, and registry." The interface is named by the step, so the page
       gets an HTMLUnknownElement with the local name it asked for. */
    {
        /* The element the lookup stage created: §4.9's create an element internal in the receiver's document (§4.5
           step 5's "given this") with the registry step 3 flattened, which this step names too. */
        JSValue el = s->el;
        JSValue proto;

        s->el = JS_UNDEFINED;
        DCHECK(JS_IsObject(el), "DOM §4.9 step 5.1.4's failure arm has no element to answer with — the element "
                                "is created before step 3's lookup precisely so this arm and step 6 share one, "
                                "and a report reached without one came from a stage that never made it");
        proto = html_unknown_element_proto(ctx);
        JS_SetPrototype(ctx, el, proto);
        JS_FreeValue(ctx, proto);
        custom_elements_mark_failed(ctx, el);
        *presult = el;
    }
    return 0;
}

static const IdlStepDecl DOC_CREATE_EL_STEP = {
    js_doc_create_element_step, sizeof(DocCreateElState), doc_create_el_visit, doc_create_el_release,
    "DOM §4.5 Document.createElement, over §4.9 create an element", DCE_STEPS,
    /* DOM §4.9 step 5.1.4 is "run these steps while catching any exceptions", so the construct's abrupt completion
       is this algorithm's value: it reports it and answers with a failed HTMLUnknownElement. */
    .catches_abrupt = 1
};

/* DOM §4.9 "create an element internal" for this realm's document (see document.h). Separate from the member above
   because they name different documents: `createElement` creates in its receiver's (§4.5 step 5's "given this"),
   HTML §3.2.3 step 9.2 in the current global's. */
JSValue document_create_element_internal(JSContext *ctx, const char *local, size_t len)
{
    /* The HTML namespace, named: §4.13.3 "Core concepts" admits a custom element name only in the HTML namespace
       (see document.h's document_create_element_html). §4.5 step 2's ASCII-lowercase fold is absent, which is a
       claim about the callers and is asserted: §4.13.3's production is lowercase, §4.13.4 step 7.3 admits an
       `extends` only when HTML §3.2.2 gives it an element interface (every such tag is lowercase ASCII, and step
       7.4 makes it the local name), and literals from this engine's C (§4.8.3's `new Image()` passes "img") are
       written lowercase. A caller breaking it would create an element no selector, parser or interface lookup
       matches. */
    lxb_dom_element_t *el;
    size_t i;

    for (i = 0; i < len; i++)
        DCHECK(!(local[i] >= 'A' && local[i] <= 'Z'),
               "DOM §4.5 step 2's ASCII-lowercase fold is not performed here, and a local name carrying an "
               "ASCII upper alpha reached it — every caller is required to pass a name that is already folded");
    el = document_create_element_html(lxb_dom_interface_document(doc_here(ctx)->dom), local, len);

    dom_cow_note_created(lxb_dom_interface_node(el));   /* this flow made it: the delta owns it */
    return element_wrap(ctx, el);
}

/* §4.5 createElementNS(namespace, qualifiedName): createElement plus "validate and extract", so a qualified name may
   carry a prefix (`svg:rect`) and the element is created in the named namespace. Lexbor carries the namespace on the
   element, so `el.namespaceURI` is what the page asked for. testharness.js reaches it on every completed document
   (`output_document.createElementNS(xhtml_ns, "style")` in Output.show_results). */
/* Bytes for a DOMString a factory is about to put in the tree, when the value may be unknown external input.
   Lexbor stores bytes, and quickjs's `js_force_tostring` asserts by design that a concolic never becomes a
   `const char *` ("the fix is never here"), so the edge that asked answers. The answer is the shape, as
   core/dom/node.c's `js_cd_set_data` gives for DOM §4.10's `CharacterData.data` (node.c: unknown external input has
   no bytes, and its shape is what the node carries), so a source parked in the tree as text reads back as that
   source. It invents nothing: a shape displays a value the run has, and solver/attr_shadow.c keeps the taint on the
   slot. `*owned` says who frees it: a shape is borrowed from the concolic, a converted string is this function's.
   Named residual. Not covered: §4.5's `createProcessingInstruction(target, data)` and `createCDATASection(data)`
   take bytes from a byte consumer, so an unknown reaching either dies at that assertion; each runs a step over the
   bytes (§4.5 step 2's `]]>` refusal, the target's match against `xml`) that a shape would decide about a string the
   page never wrote. Next diff builds: those steps asked of the value (refusing only what they can see, admitting an
   unknown as unknown), then both route here. How its absence would show: a run building an XML document from
   forced input aborts at the byte consumer on document.c's processing-instruction or CDATA line. */
static const char *doc_domstring_bytes(JSContext *ctx, JSValueConst v, size_t *len, int *owned)
{
    const char *s;

    *owned = 0;
    if (concolic_is(v)) {
        s = concolic_shape_c(v);
        DCHECK(s != NULL, "a concolic value answered no display shape to a node factory — concolic_shape_c is "
                          "declared to answer for every concolic, so a NULL here is a value that passed "
                          "concolic_is and cannot say what it is");
        *len = s ? strlen(s) : 0;
        return s ? s : "";
    }
    *owned = 1;
    return JS_ToCStringLen(ctx, len, v);
}

/* §4.5 createTextNode / createComment, the two non-element nodes a page builds by hand
   (testharness.js's make_dom_single does `output_document.createTextNode(template[i])`). Detached, like
   createElement: nothing is observable until appendChild, the per-flow write. */
static JSValue js_doc_create_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    const char *s;
    size_t len = 0;
    int owned = 0;
    lxb_dom_text_t *t;
    Document *d = doc_receiver(ctx, this_val);

    if (!d) return JS_EXCEPTION;
    s = doc_domstring_bytes(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &len, &owned);
    if (!s) return JS_EXCEPTION;
    t = lxb_dom_document_create_text_node(lxb_dom_interface_document(d->dom), (const lxb_char_t *)s, len);
    dom_cow_note_created(t ? lxb_dom_interface_node(t) : NULL);   /* this flow made it */
    if (owned) JS_FreeCString(ctx, s);
    DCHECK(t != NULL, "createTextNode produced no node — a page building its DOM would silently build nothing");
    return node_wrap(ctx, lxb_dom_interface_node(t));
}

/* §4.5 `new Document()`: "set this's origin to the origin of current global object's associated Document", and
   nothing else, so the document has no tree, doctype or document element, URL `about:blank` and content type
   `application/xml`. It is the one way a page gets an XML document without DOMImplementation (dom/common.js opens
   with it). Built here because §4.5's IDL declares a constructor; `node_install_interface` is for interfaces that
   declare none. */
static JSValue js_doc_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic)
{
    lxb_html_document_t *dom;

    (void)new_target; (void)argc; (void)argv; (void)magic;
    dom = dom_document_create();
    CHECK(dom != NULL, "new Document(): OOM building a second Document");
    /* The origin is the constructing realm's document's, which document_new takes from the realm it runs in, as
       createDocument's step 6 states. The other facts are §4.5's defaults verbatim ("Unless stated otherwise, a
       document's … content type is `application/xml`, URL is `about:blank`, … type is `xml`"), and `xml` is what
       lets `new Document().createCDATASection(…)` succeed. The interface is `Document`, as §4.5 says: "Unlike
       createDocument(), this constructor does not return an XMLDocument object, but a document (Document
       object)", an `xml` document that is not an XMLDocument, which is why DocumentInterface is an argument. */
    return document_new(ctx, dom, "about:blank", DOCUMENT_IFACE_DOCUMENT,
                        document_kind(/*is_xml*/true, "application/xml"));
}

/* DOM §4.5 "Interface Document"'s `createCDATASection(data)` and `createProcessingInstruction(target, data)`: the
   node factories an XML document has and an HTML one does not (dom/common.js builds `paras[5]` from two CDATA
   sections and `xmlDoc` from two processing instructions, so every DOM §5 and §6 test file that includes it needs
   them). The processing-instruction arm is one call: "return the result of creating a processing instruction node
   given this, target, and data", the §4.13 algorithm core/dom/node.c owns (node_pi_create), shared with
   `new ProcessingInstruction(target, data)`. createCDATASection has its own HTML-document refusal and `]]>` test
   and is defined nowhere else. One body, so a parked flow can say which member it is in.
   magic 0 = createCDATASection, 1 = createProcessingInstruction. */
static JSValue doc_create_xml_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic, bool target_decided, bool data_decided)
{
    Document *d = doc_receiver(ctx, this_val);
    lxb_dom_document_t *dom;
    const char *data, *target = NULL;
    size_t dlen = 0, tlen = 0;
    lxb_dom_cdata_section_t *c;
    lxb_dom_node_t *made = NULL;
    JSValue r;

    if (!d) return JS_EXCEPTION;
    dom = lxb_dom_interface_document(d->dom);
    DCHECK(argc >= (magic == 0 ? 1 : 2),
           "a §4.5 XML node factory reached its body short of its declared arguments — both declare every "
           "position REQUIRED, so Web IDL §3.6 Overload resolution algorithm's arity check has already thrown "
           "for a call that passed fewer");
    if (magic == 1) {
        /* "Return the result of creating a processing instruction node given this, target, and data". `this` is
           the receiver's document, the one thing separating this from §4.13's constructor; §4.13 carries no
           HTML-document refusal, so a processing instruction in an HTML page is a node, as in a browser. */
        target = JS_ToCStringLen(ctx, &tlen, argv[0]);
        if (!target) return JS_EXCEPTION;
        data = JS_ToCStringLen(ctx, &dlen, argv[1]);
        if (!data) { JS_FreeCString(ctx, target); return JS_EXCEPTION; }
        r = node_pi_create_decided(ctx, dom, target, tlen, data, dlen, target_decided, data_decided);
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, target);
        return r;
    }
    /* Step 1: `createCDATASection` on an HTML document is a NotSupportedError, "if this is an HTML document", which
       §4.5 defines over the type alone ("A document is said to be an XML document if its type is `xml`; otherwise an
       HTML document"), not the content type: HTML §7.5.4 "Loading text documents" creates an HTML document whose
       content type is the response's. */
    if (!d->is_xml)
        return JS_ThrowDOMException(ctx, "NotSupportedError", "an HTML document has no CDATA sections");
    data = JS_ToCStringLen(ctx, &dlen, argv[0]);
    if (!data) return JS_EXCEPTION;
    /* Step 2: "If data contains the string "]]>", then throw an "InvalidCharacterError" DOMException", the one
       sequence the node's serialization cannot survive. Over `dlen` bytes, since a DOMString may contain U+0000,
       which would end a C-string search early; `memmem` is this tree's spelling (core/html/form_data.c,
       core/html/html_encoding_sniff.c). */
    if (!data_decided && dlen >= 3 && memmem(data, dlen, "]]>", 3) != NULL) {
        JS_FreeCString(ctx, data);
        return JS_ThrowDOMException(ctx, "InvalidCharacterError",
                                    "a CDATA section cannot contain \"]]>\"");
    }
    c = lxb_dom_document_create_cdata_section(dom, (const lxb_char_t *)data, dlen);
    CHECK(c != NULL, "createCDATASection: the Lexbor node allocation failed");
    made = lxb_dom_interface_node(c);
    dom_cow_note_created(made);   /* this flow made it; detached until the page inserts it */
    JS_FreeCString(ctx, data);
    r = node_wrap(ctx, made);
    return r;
}

/* The two factories as the page calls them: step machines, because a string they test may be unknown external
 * input, and each test then has both completions — createProcessingInstruction's §4.13 step 1 (the target matches
 * XML's Name) and step 2 (the data contains `?>`), createCDATASection's step 2 (the data contains `]]>`) — so each
 * forks, one stage per test in the algorithm's order. A passing arm creates the node over the unknown's shape, as
 * createTextNode does; known strings run the factory directly. createCDATASection's step 1, the HTML-document
 * refusal, reads no string and runs before its fork. magic 0 = createCDATASection, 1 = createProcessingInstruction. */
#define XN_ALGORITHM "DOM §4.5 Interface Document createCDATASection / createProcessingInstruction"
#define XN_STAGES(X)                                                                                         \
    X(XN_TARGET, "DOM §4.13 create a processing instruction node step 1 (the target matches XML's Name)")     \
    X(XN_DATA,   "DOM §4.13 step 2 / §4.5 createCDATASection step 2 (the data contains the closing sequence)") \
    X(XN_CREATE, XN_ALGORITHM " (create the node)")
enum { IDL_STEP_STAGE_BASE(XN_STAGES) XN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const XN_STEPS[] = { XN_STAGES(JS_STEP_STAGE_LABEL) NULL };
#define XN_TARGET_OP "DOM §4.13 create a processing instruction node step 1 (target matches the XML Name production)"
static const char *const XN_DATA_OP[2] = {
    "DOM §4.5 createCDATASection step 2 (data contains ]]>)",
    "DOM §4.13 create a processing instruction node step 2 (data contains ?>)",
};

typedef struct { bool target_decided, data_decided; } XnState;   /* the passing arms taken; nothing owned */
static void xn_visit(JSContext *ctx, void *state, JSStepVisit *v) { (void)ctx; (void)state; (void)v; }

/* Which completion a test reaches on the unknown's example: 0 passes, 1 throws. */
static int xn_real(JSContext *ctx, JSValueConst v, bool target, int magic)
{
    JSValue ex = concolic_example(ctx, v);
    int real = JS_OUTCOME_REAL_UNSTATED;

    if (JS_IsString(ex)) {
        size_t len = 0;
        const char *b = JS_ToCStringLen(ctx, &len, ex);

        CHECK(b != NULL, "an XML node factory could not read the bytes of a string it was handed");
        if (target)
            real = xml_name_is_name(b, len) ? 0 : 1;
        else if (magic == 0)
            real = (len >= 3 && memmem(b, len, "]]>", 3) != NULL) ? 1 : 0;
        else
            real = (len >= 2 && memmem(b, len, "?>", 2) != NULL) ? 1 : 0;
        JS_FreeCString(ctx, b);
    }
    JS_FreeValue(ctx, ex);
    return real;
}

static int js_doc_create_xml_node_step(JSContext *ctx, JSStepHdr *hdr, void *state, int argc,
                                       JSValueConst *argv, JSValue cb_result, JSValue *presult,
                                       JSValue **out_cb, int *out_argc)
{
    XnState *s = state;
    int magic = idl_step_magic(hdr), di = magic == 0 ? 0 : 1, rc, arm = 0;
    JSValueConst a2[2];
    JSValue shapes[2] = { JS_UNDEFINED, JS_UNDEFINED }, r;
    int i;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);   /* this machine makes no request that delivers a value */
    *presult = JS_UNDEFINED;
    DCHECK(magic == 0 || magic == 1, "an XML node factory was declared with a magic this file does not name");
    DCHECK(argc == di + 1, "an XML node factory reached its body with an argument count its declaration does not produce");
    if (hdr->stage == XN_TARGET) {
        Document *d = doc_receiver(ctx, hdr->this_val);

        if (!d) return JS_STEP_ABRUPT;
        if (magic == 0 && !d->is_xml) {   /* createCDATASection step 1, before the string it tests */
            JS_ThrowDOMException(ctx, "NotSupportedError", "an HTML document has no CDATA sections");
            return JS_STEP_ABRUPT;
        }
        if (magic == 1 && concolic_is(argv[0])) {
            rc = step_fork_run(ctx, hdr, argv[0], XN_TARGET_OP, 2, xn_real(ctx, argv[0], true, magic), &arm);
            if (rc)
                return rc;
            if (arm != 0) {
                JS_ThrowDOMException(ctx, "InvalidCharacterError",
                                     "a processing instruction target must match the XML Name production");
                return JS_STEP_ABRUPT;
            }
            s->target_decided = true;
        }
        hdr->stage = XN_DATA;
    }
    if (hdr->stage == XN_DATA) {
        if (concolic_is(argv[di])) {
            rc = step_fork_run(ctx, hdr, argv[di], XN_DATA_OP[magic], 2, xn_real(ctx, argv[di], false, magic), &arm);
            if (rc)
                return rc;
            if (arm != 0) {
                JS_ThrowDOMException(ctx, "InvalidCharacterError", "the data contains the node's closing sequence");
                return JS_STEP_ABRUPT;
            }
            s->data_decided = true;
        }
        hdr->stage = XN_CREATE;
    }
    DCHECK(hdr->stage == XN_CREATE, "an XML node factory resumed into a stage its declaration does not have");
    /* An unknown string the arms decided stands as its shape, a real string. */
    for (i = 0; i <= di; i++) {
        if (concolic_is(argv[i])) {
            const char *sh = concolic_shape_c(argv[i]);

            DCHECK(sh != NULL, "an unknown string answered no display shape");
            shapes[i] = JS_NewString(ctx, sh);
            CHECK(!JS_IsException(shapes[i]), "an XML node factory could not make a string of a shape");
            a2[i] = shapes[i];
        } else {
            a2[i] = argv[i];
        }
    }
    r = doc_create_xml_node(ctx, hdr->this_val, argc, a2, magic, s->target_decided, s->data_decided);
    JS_FreeValue(ctx, shapes[0]);
    JS_FreeValue(ctx, shapes[1]);
    if (JS_IsException(r))
        return JS_STEP_ABRUPT;
    *presult = r;
    return JS_STEP_DONE;
}

static const IdlStepDecl XN_DECL = {
    js_doc_create_xml_node_step, sizeof(XnState), xn_visit, NULL, XN_ALGORITHM, XN_STEPS, 0, NULL
};

static JSValue js_doc_create_comment(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    const char *s;
    size_t len = 0;
    int owned = 0;
    lxb_dom_comment_t *c;
    Document *d = doc_receiver(ctx, this_val);

    if (!d) return JS_EXCEPTION;
    s = doc_domstring_bytes(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &len, &owned);
    if (!s) return JS_EXCEPTION;
    c = lxb_dom_document_create_comment(lxb_dom_interface_document(d->dom), (const lxb_char_t *)s, len);
    dom_cow_note_created(c ? lxb_dom_interface_node(c) : NULL);   /* this flow made it */
    if (owned) JS_FreeCString(ctx, s);
    DCHECK(c != NULL, "createComment produced no node — a page building its DOM would silently build nothing");
    return node_wrap(ctx, lxb_dom_interface_node(c));
}

/* §4.5 createElementNS(namespace, qualifiedName): createElement's creation over the validated triple, so the element
   carries the namespace the page asked for. testharness.js reaches it on every completed document
   (`createElementNS(xhtml_ns, "style")` in Output.show_results). §1.4's "validate and extract" is core/dom/names.c's,
   length-carrying, and given the element context (`a=b` is a valid element local name and not a valid attribute
   one): it validates the local name (§1.4 step 6), treats U+0000 as an ordinary code point, and applies §1.4 step
   4.3's valid-namespace-prefix predicate. The first argument is declared `DOMString?`, so `createElementNS(null,
   "div")` arrives as JS_NULL; §4.5.1's createDocument calls the internal steps with a real JS_NULL too. */
static JSValue js_doc_create_element_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    const char *ns = NULL, *qname;
    size_t ns_len = 0, qname_len = 0;
    DomQName qn;
    lxb_dom_element_t *el;
    Document *d = doc_receiver(ctx, this_val);
    JSValue r;

    if (!d) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "createElementNS requires a namespace and a qualified name");
    if (!JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
        ns = JS_ToCStringLen(ctx, &ns_len, argv[0]);
        if (!ns) return JS_EXCEPTION;
    }
    /* Length-carrying on both, because §1.4 decides about U+0000 and `strlen` would stop at it (names.h). */
    qname = JS_ToCStringLen(ctx, &qname_len, argv[1]);
    if (!qname) { if (ns) JS_FreeCString(ctx, ns); return JS_EXCEPTION; }

    if (!dom_validate_and_extract(ctx, ns, ns_len, qname, qname_len, DOM_NAME_ELEMENT, &qn)) {
        if (ns) JS_FreeCString(ctx, ns);
        JS_FreeCString(ctx, qname);
        return JS_EXCEPTION;          /* the DOMException §1.4's step names is already thrown */
    }
    /* §4.5's storage step, not lxb_dom_element_create, which interns the namespace and local name through lexbor's
       case-folding hashes; the element is created from the three slices validate-and-extract returned, which are
       what the page passed (element.c says which standard makes case significant for each). */
    el = element_create_ns(lxb_dom_interface_document(d->dom),
                           qn.ns, qn.ns_len, qn.local, qn.local_len, qn.prefix, qn.prefix_len);
    /* This flow made it, as every creating member on this interface says. Ownership is the caller's fact, not
       element_create_ns's: §4.4's clone builds a subtree with the same creator and notes only its root, because
       dom_release_created destroys deep and a second entry under a freed parent would be a double free. */
    dom_cow_note_created(lxb_dom_interface_node(el));
    if (ns) JS_FreeCString(ctx, ns);
    JS_FreeCString(ctx, qname);
    r = element_wrap(ctx, el);
    return r;
}

/* DOM §4.5 createElementNS as the page calls it: a step machine, because its qualified name may be unknown
 * external input and §1.4's validate-and-extract then has three feasible completions — an element,
 * InvalidCharacterError and NamespaceError — which a plain C body could only collapse into one. Each is
 * feasible for every namespace (`xmlns` fails step 10 or step 11 whichever namespace is given, and `` fails step
 * 6), so the fork always declares three. The element arm is a value derived from the name, whose example is the
 * element the example name creates when that name is valid. A known name runs the internal steps directly.
 *
 * Named residual. Not covered: an unknown namespace beside the name, §4.5.1 createDocument's qualified name and
 * createDocumentType's name, each a byte consumer of its name. Next diff builds: the same fork
 * at each, asking dom_qname_outcome or the entry's own name predicate. Absence shows as: a @WHY naming ToString
 * over unknown external input at one of those entries' conversions. */
#define CEN_ALGORITHM "DOM §4.5 Interface Document createElementNS(namespace, qualifiedName)"
#define CEN_OP "DOM §1.4 validate and extract a namespace and qualifiedName"
#define CEN_STAGES(X) X(CEN_CREATE, CEN_ALGORITHM " (the internal createElementNS steps)")
enum { IDL_STEP_STAGE_BASE(CEN_STAGES) CEN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CEN_STEPS[] = { CEN_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { int unused; } CenState;   /* the fork's request lives on the header; nothing is owned here */
static void cen_visit(JSContext *ctx, void *state, JSStepVisit *v) { (void)ctx; (void)state; (void)v; }

/* Which completion the example name reaches under the given namespace, for step_fork_run's `real`. */
static int cen_real(JSContext *ctx, JSValueConst ns_v, JSValueConst name)
{
    JSValue ex = concolic_example(ctx, name);
    const char *ns = NULL, *q, *why = NULL;
    size_t ns_len = 0, q_len = 0;
    DomQName qn;
    int real = JS_OUTCOME_REAL_UNSTATED;

    if (JS_IsString(ex)) {
        if (!JS_IsNull(ns_v) && !JS_IsUndefined(ns_v)) ns = JS_ToCStringLen(ctx, &ns_len, ns_v);
        q = JS_ToCStringLen(ctx, &q_len, ex);
        CHECK(q != NULL && (ns != NULL || JS_IsNull(ns_v) || JS_IsUndefined(ns_v)),
              "createElementNS could not read the bytes of a string it was handed");
        real = (int)dom_qname_outcome(ns, ns_len, q, q_len, DOM_NAME_ELEMENT, &qn, &why);
        JS_FreeCString(ctx, q);
        if (ns) JS_FreeCString(ctx, ns);
    }
    JS_FreeValue(ctx, ex);
    return real;
}

static JSValue js_doc_create_element_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                        int magic);

static int js_doc_create_element_ns_step(JSContext *ctx, JSStepHdr *hdr, void *state, int argc,
                                         JSValueConst *argv, JSValue cb_result, JSValue *presult,
                                         JSValue **out_cb, int *out_argc)
{
    JSValue r;

    (void)state; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);   /* this machine makes no request that delivers a value */
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == CEN_CREATE, "createElementNS resumed into a stage its declaration does not have");
    DCHECK(argc == 2, "createElementNS reached its body with an argument count its declaration does not produce");
    if (concolic_is(argv[1])) {
        int rc, arm = 0;
        JSValue ex, made = JS_UNDEFINED;

        DCHECK(!concolic_is(argv[0]),
               "createElementNS was handed an unknown namespace beside an unknown qualified name — the "
               "completion then depends on both, so build the fork over the pair (concolic_new_rel) here");
        rc = step_fork_run(ctx, hdr, argv[1], CEN_OP, DOM_QNAME_OUTCOMES, cen_real(ctx, argv[0], argv[1]), &arm);
        if (rc)
            return rc;
        DCHECKF(arm >= 0 && arm < DOM_QNAME_OUTCOMES, "createElementNS's fork answered completion %d", arm);
        if (arm != DOM_QNAME_VALID) {
            JS_ThrowDOMException(ctx, dom_qname_outcome_exception((DomQNameOutcome)arm),
                                 "the qualified name is not one validate and extract accepts");
            return JS_STEP_ABRUPT;
        }
        ex = concolic_example(ctx, argv[1]);
        if (JS_IsString(ex) && cen_real(ctx, argv[0], argv[1]) == DOM_QNAME_VALID) {
            JSValueConst a2[2];

            a2[0] = argv[0];
            a2[1] = ex;
            made = js_doc_create_element_ns(ctx, hdr->this_val, 2, a2, 0);
            CHECK(!JS_IsException(made), "createElementNS refused a qualified name validate and extract accepted");
        }
        JS_FreeValue(ctx, ex);
        *presult = concolic_builtin_hook(ctx, argv[1], "createElementNS", made);
        return JS_STEP_DONE;
    }
    r = js_doc_create_element_ns(ctx, hdr->this_val, argc, argv, 0);
    if (JS_IsException(r))
        return JS_STEP_ABRUPT;
    *presult = r;
    return JS_STEP_DONE;
}

static const IdlStepDecl CEN_DECL = {
    js_doc_create_element_ns_step, sizeof(CenState), cen_visit, NULL, CEN_ALGORITHM, CEN_STEPS, 0, NULL
};

/* §4.5's "internal createElementNS steps", named as the spec names them because §4.5.1's createDocument step 3 calls
   them "given document" (the new document, not the one whose implementation was asked); one implementation of
   validate-and-extract and the creation. */
JSValue document_create_element_ns(JSContext *ctx, JSValueConst doc, int argc, JSValueConst *argv)
{
    return js_doc_create_element_ns(ctx, doc, argc, argv, 0);
}

/* §4.5's `[CEReactions] Node adoptNode(Node node)`: four steps over DOM §4.5's "adopt a node", which core/dom/node.c
 * owns (a shadow-including walk writing each node document through the per-flow chokepoint). The two refusals
 * are different exceptions a page's `catch (e) { e.name }` tells apart (a Document is a NotSupportedError, a
 * shadow root a HierarchyRequestError), and they are throws: `document.adoptNode(document)` is a spec question.
 * The realm is the receiver document's: adopt's step 3 mints wrappers and reads per-flow records (the shadow-root
 * association, registry re-derivation, adoptedCallback reaction) about the document adopted into, as node.c's
 * insert-adopt does. */
static JSValue js_doc_adopt_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    Document *d = doc_receiver(ctx, this_val);
    lxb_dom_node_t *n;

    (void)magic;
    if (!d) return JS_EXCEPTION;
    DCHECK(argc >= 1, "adoptNode ran with no argument — `Node node` is required, so the declaration's own "
                      "arity check answers that before this body is entered");
    n = node_of(argv[0]);
    DCHECK(n != NULL, "adoptNode was handed something that is not a Node — the argument is an INTERFACE type "
                      "and the declaration brands it, so a non-Node is a TypeError before this body runs");
    /* Step 1. */
    if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT)
        return JS_ThrowDOMException(ctx, "NotSupportedError", "a Document cannot be adopted");
    /* Step 2, a HierarchyRequestError rather than step 1's refusal: a shadow root is a node the tree could hold
       nowhere, a Document a node no document may own. */
    if (shadow_root_is(n))
        return JS_ThrowDOMException(ctx, "HierarchyRequestError", "a shadow root cannot be adopted");
    node_adopt(d->realm, n, lxb_dom_interface_document(d->dom));           /* step 3: adopt node into this */
    return JS_DupValue(ctx, argv[0]);                                      /* step 4 */
}

/* §4.5's `[CEReactions, NewObject] Node importNode(Node node, optional (boolean or ImportNodeOptions) options =
 * false)`: seven steps over DOM §4.4's `clone a node`, distinguished from cloneNode by step 7: "Return the result of
 * cloning a node given node with document set to this, subtree set to subtree, and fallbackRegistry set to
 * registry." A clone defaults to node's own document; an import names the receiver.
 * It is a machine because `clone a node` is one: it rests at every node (core/dom/node.h's NODE_CLONE_ALGO_STAGES),
 * so this member declares that stage block, embeds NodeCloneState and hands node_clone_run its base, as cloneNode
 * and §5.5's extract do, keeping §4.4 step 3's cloning steps and step 6's clonable shadow root.
 * Two refusals, both NotSupportedError: step 1 refuses a document or a shadow root (cloneNode refuses only the
 * shadow root, since §4.4 defines a document's copy), and step 5.3 refuses a registry that is neither scoped nor
 * this document's. Every refusal is the spec's throw: the node, options and registry are page-supplied, and only
 * what this codebase computed (the receiver's record, the argument machine's output) is asserted. */
#define DIN_STAGES(X) \
    X(DIN_CHECK, "DOM §4.5 importNode steps 1-6 (refuse a document or a shadow root, then resolve `subtree` " \
                 "and `registry` from the `(boolean or ImportNodeOptions)` union)") \
    NODE_CLONE_ALGO_STAGES(X, DIN, "DOM §4.5 importNode step 7") \
    X(DIN_RETURN, "DOM §4.5 importNode step 7 (return the `clone a node` result)")
enum { IDL_STEP_STAGE_BASE(DIN_STAGES) DIN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DIN_STEPS[] = { DIN_STAGES(JS_STEP_STAGE_LABEL) NULL };

static void doc_import_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    node_clone_visit_state(ctx, st, v);
}

/* §4.5's importNode steps 3-5 as one answer: "Let registry be null", the boolean arm's `subtree`, and the
   dictionary arm's three sub-steps. It answers `subtree` through `*psubtree` and the refusal through its return,
   as flatten_creation_options does, because both come from one reading of one argument. The registry is computed
   only to be refused (a residual, named at step 7). Returns true to proceed, false with the exception pending. */
static bool doc_import_options(JSContext *ctx, JSValueConst this_val, JSValueConst options, bool *psubtree)
{
    JSValue reg_v, registry, doc_reg;
    bool same;

    *psubtree = false;                                                 /* step 2: "Let subtree be false" */
    /* Step 4, "if options is a boolean, then set subtree to options". §3.2.25 "Union types" converted to exactly
       one member type and IDL_BOOL_OR_DICT places it, so `JS_IsBool` is the union's output. The omitted call is
       this arm, which is why the declaration states `= false`: §3.6 step 16.1 places a declared default, and this
       member's default is the boolean `false` where other union-with-dictionary positions default to `{}`, so
       `document.importNode(n)` is shallow (an all-defaults ImportNodeOptions would be deep, since step 5.1 negates
       `selfOnly`). An explicit `null` takes the dictionary arm (§3.2.25 step 4) and is deep. */
    if (JS_IsBool(options)) { *psubtree = JS_ToBool(ctx, options) != 0; return true; }
    if (concolic_is(options)) {
        DFAIL("DOM §4.5's importNode reached step 4 with UNKNOWN EXTERNAL INPUT that Web IDL §3.2.25 \"Union "
              "types\" placed on the BOOLEAN arm of `(boolean or ImportNodeOptions)`, and step 4 reads that "
              "boolean's VALUE: `true` is a deep import and `false` is one node, which are two different trees "
              "and therefore two worlds. The union's ARM was already forked by the conversion "
              "(IDL_BOOL_OR_DICT is IDL_CONCOLIC_FORKS); what is missing is the fork over the boolean's own "
              "TRUTH, and it is missing for every member that declares this row — CSSOM VIEW §6's "
              "scrollIntoView and HTML §4.12.4's togglePopover carry the same gap and say so. BUILD IT AT THE "
              "CONVERSION, not here: idl_args.c's plain IDL_BOOLEAN position already asks step_tobool_run at "
              "the BRANCH seam, keyed by the value's own branch identity, so `if (cfg.deep)` and "
              "`importNode(n, cfg.deep)` are ONE gate and one constraint entry — a fork asked in this body "
              "would file a second independent entry over one predicate. The arm that places the boolean is "
              "the IDL_BOOL_OR_DICT branch of idl_dict_walk_run's position loop");
        return false;
    }
    /* Step 5, the dictionary arm. Its sub-steps are siblings (5.3 is not under 5.2), so a registry still null from
       step 3 reaches 5.3 and passes, since the condition is about a non-null one. */
    DCHECK(JS_IsObject(options),
           "§3.2.25's union placed something at importNode's `options` that is neither the boolean it converted "
           "nor the dictionary it built — the two member types are all the algorithm has, and a third value "
           "means the position is no longer declared IDL_BOOL_OR_DICT");
    /* Step 5.1, "set subtree to the negation of options[\"selfOnly\"]". `boolean selfOnly = false`, so the dictionary
       arm defaults to a deep import. §3.2.17's member loop asks step_tobool_run for a declared boolean, so an unknown
       `selfOnly` is forked at the conversion and arrives here as a real truth value. */
    *psubtree = !idl_dict_bool(ctx, options, "selfOnly");
    /* Step 5.2, "if options[\"customElementRegistry\"] exists, then set registry to it": existence, since §3.2.17
       makes `undefined` the absence (the member declares no default). */
    reg_v = idl_dict_get(ctx, options, "customElementRegistry");
    /* Web IDL §3.2.15 "Interface types" on the member's declared type `CustomElementRegistry`, before the algorithm's
       steps, so `{customElementRegistry: 5}` is a TypeError, not step 5.3's NotSupportedError. It is in the body for
       the reason §4.5's ElementCreationOptions and §4.9's ShadowRootInit give (the member is IDL_ANY until an
       IdlDictMember has `iface_is`/`iface_name` for it); core/dom/shadow_root.c names that residual once for all
       three. Until then a browser throws inside §3.2.17's walk before `selfOnly` is read, and this after. */
    if (!JS_IsUndefined(reg_v) && !custom_elements_is_registry(reg_v)) {
        JS_FreeValue(ctx, reg_v);
        JS_ThrowTypeError(ctx, "ImportNodeOptions's customElementRegistry is not a CustomElementRegistry");
        return false;
    }
    registry = reg_v;                                                  /* step 5.2 (the reference moves) */
    /* Step 5.3: an import may name a scoped registry or this document's; any other is one the receiver resolves
       nothing in. It is the receiver's, as in flatten_creation_options step 1: `otherDoc.importNode(n)` asks about
       otherDoc's registry. */
    if (JS_IsObject(registry) && !custom_elements_registry_is_scoped(ctx, registry)) {
        doc_reg = custom_elements_node_registry(ctx, this_val);
        same = JS_VALUE_GET_PTR(doc_reg) == JS_VALUE_GET_PTR(registry);
        JS_FreeValue(ctx, doc_reg);
        if (!same) {
            JS_FreeValue(ctx, registry);
            JS_ThrowDOMException(ctx, "NotSupportedError",
                                 "importNode was given a custom element registry that is neither scoped nor "
                                 "this document's");
            return false;
        }
    }
    JS_FreeValue(ctx, registry);
    return true;
}

static int js_doc_import_node(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                              JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    NodeCloneState *s = st;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    STEP_DISPATCH(DIN_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(DIN_CHECK);
    {
        Document *d = doc_receiver(ctx, hdr->this_val);
        lxb_dom_node_t *n;
        bool subtree;

        /* The declaration's `idl_this_iface` has already refused a foreign receiver at Web IDL §3.7.7 "Operations"'
           position, before the argument conversions; doc_receiver here answers which Document, the record holding
           the tree this import copies into. */
        if (!d) return JS_STEP_ABRUPT;
        DCHECK(argc == 2, "§4.5 declares `importNode(Node node, optional (boolean or ImportNodeOptions) "
                          "options = false)` — two positions, with the default materialized by the argument "
                          "machine — so a body seeing any other count is a declaration that grew without this");
        n = node_of(argv[0]);
        DCHECK(n != NULL, "importNode was handed something that is not a Node — the argument is an INTERFACE "
                          "type and the declaration brands it, so a non-Node is a TypeError before this body "
                          "runs");
        /* Step 1, both kinds, one exception. A document is refused because an import puts the copy in this document
           and §4.4's document arm puts it in one of its own; a shadow root because only `attach a shadow root`
           creates one, so a copy would be a second root for a host that has one. */
        if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT || shadow_root_is(n)) {
            JS_ThrowDOMException(ctx, "NotSupportedError",
                                 n->type == LXB_DOM_NODE_TYPE_DOCUMENT
                                     ? "a Document cannot be imported"
                                     : "a shadow root cannot be imported");
            return JS_STEP_ABRUPT;
        }
        if (!doc_import_options(ctx, hdr->this_val, argv[1], &subtree))   /* steps 2-5 */
            return JS_STEP_ABRUPT;
        /* Step 7, `clone a node` given `document` set to this, which is the member.
           Named residual. Not covered: step 6 ("if registry is null, then set registry to the result of looking up
           a custom element registry given this") and step 7's `fallbackRegistry`: node.c's clone threads no
           registry, so a copy's registry is derived as an ordinary insertion derives it. The copy is created
           uncustomized and §4.13's upgrade reaction runs on insertion, as for cloneNode. Next diff builds:
           `registry` as a third argument of node_clone_start, carried on NodeCloneState beside `doc`, read by
           clone_element_into and handed to the element creation. How its absence would show: an element imported
           with a scoped registry resolves its definition from the document's set, so the wrong constructor runs on
           an inserted copy. */
        node_clone_start(hdr, s, n, lxb_dom_interface_document(d->dom), subtree, DIN_ROOT, DIN_RETURN);
        return JS_STEP_YIELD;
    }

    /* Step 7's `clone a node`, whose six rest points are this member's six stages, named individually as cloneNode
       names them, so a stage added to NODE_CLONE_ALGO_STAGES does not compile without an arm here. */
    STEP_ARM(DIN_ROOT);
    STEP_ARM(DIN_COPY);
    STEP_ARM(DIN_TEMPLATE);
    STEP_ARM(DIN_CHILDREN);
    STEP_ARM(DIN_SHADOW);
    STEP_ARM(DIN_LEAVE);
    return node_clone_run(ctx, hdr, s, DIN_ROOT);

    STEP_ARM(DIN_RETURN);
    DCHECK(s->copy != NULL, "`clone a node` finished without a copy — its step 7 is what sets the answer, and "
                            "the caller is only ever resumed from there");
    *presult = node_wrap(ctx, s->copy);
    return JS_STEP_DONE;
}

static const IdlStepDecl DOC_IMPORT_NODE_STEP = {
    /* No release: the level stack is node_clone_visit_state's, and teardown discharges that one list. */
    js_doc_import_node, sizeof(NodeCloneState), doc_import_visit, NULL,
    "DOM §4.5 Document.importNode (over §4.4's `clone a node` concept)", DIN_STEPS
};

/* §4.5's `[SameObject] readonly attribute DOMImplementation implementation`, held on the record so a page that
   kept it compares equal later. */
static JSValue js_doc_implementation(JSContext *ctx, JSValueConst this_val, int magic)
{
    Document *d = doc_receiver(ctx, this_val);

    (void)magic;
    if (!d) return JS_EXCEPTION;
    DCHECK(JS_IsObject(d->impl),
           "a document answered for its `implementation` with no object — it is built WITH the document, so a "
           "document that has one and cannot say so came from neither document_install nor document_new");
    return JS_DupValue(ctx, d->impl);
}

/* The document's load lifecycle. Stage 0 is DOMContentLoaded, fired at the document and bubbling to window; stage 1
   is load, fired at window. `readyState` moves with them, for a page that missed the event. Both are per flow: the
   scheduler asks once per stage for each flow that ran everything the document gave it, so each arm fires its own
   listeners in its own world. */
/* The document element of the realm's parsed document, read from the tree (DOM §4.2.1 "Document tree"). NULL before
   the install, which `window.document` may see, and NULL once the page removes its root. */
lxb_dom_node_t *document_root_node(JSContext *ctx)
{
    Document *d = doc_of(ctx);
    return d ? document_document_element_of(lxb_dom_interface_node(d->dom)) : NULL;
}

/* This realm's associated document, not reached through the root node above: `document_root_node` answers with the
   document element, which is null once the page removes it (`document.documentElement.remove()`) while the realm,
   its global and its Document remain. A caller that wants the Document asks for it. NULL means the realm never had
   one, an engine invariant rather than a page state. */
lxb_dom_document_t *document_associated(JSContext *ctx)
{
    Document *d = doc_of(ctx);

    return d ? lxb_dom_interface_document(d->dom) : NULL;
}

/* ---- HTML §2.4.3 "Document base URLs" -------------------------------------------------------------------
 *
 * Three algorithms: §2.4.3's fallback base URL (the srcdoc case, the about:blank case, then the address) and document
 * base URL (the first base element's frozen URL, else the fallback), and HTML §4.2.3 "The base element"'s freeze,
 * which lives in core/html/html_base_element.c because it is about an element; this file stores what a Document has,
 * as it does for §4.12.1's scripts and §4.8.5's iframes. */

/* §2.4.3's fallback base URL of one Document: the URL HTML §4.2.3 freezes a `<base href>` against, a separate answer
   from the document base URL ("Thus, the base element isn't affected by itself"). Borrowed. */
const char *document_fallback_base_url_of(const lxb_dom_document_t *dom)
{
    const Document *d = doc_rec(dom);

    DCHECK(d != NULL, "§2.4.3's fallback base URL was asked of a document with no record — a tree that came "
                      "from neither document_install nor document_new has no address, no about base URL and "
                      "no way to answer");
    /* Step 1: "if document is an iframe srcdoc document: assert document's about base URL is non-null; return
       document's about base URL." The condition is a class, so it reads a creation fact, not the address. HTML
       §4.8.5 "The `iframe` element" makes the class a membership rule ("the resulting Document must be considered
       an iframe srcdoc document"), and its URL matching about:srcdoc is a consequence (HTML §2.4.1's note: "the set
       of all URLs that match about:srcdoc only vary in their fragment"), so the class is contained in the match set,
       not equal to it; §2.4.3 names the class here and spells a URL match in step 2. See the record's field. */
    if (d->is_iframe_srcdoc) {
        DCHECK(d->base.about != NULL,
               "§2.4.3 step 1's assert failed: an iframe srcdoc Document has a NULL about base URL. A srcdoc "
               "document has no address of its own to fall back to — every relative URL in it, and every "
               "`<base href>` it carries, would resolve against `about:srcdoc`, whose opaque path cannot be a "
               "base — so whoever created this Document must give it §7.4's about base URL (the srcdoc "
               "iframe's node document's base URL). core/frame/navigable.c asserts the same pairing at the "
               "one call every load converges on, where §4.8.5's resource and that base URL are both in hand");
        return d->base.about;
    }
    /* Step 2: "if document's URL matches about:blank and document's about base URL is non-null, then return
       document's about base URL." A URL test, asked only when there is an about base URL, which a document created
       from a response never has, so ordinary documents do not pay for the parse. */
    if (d->base.about) {
        UrlRecord u;
        bool about;

        url_record_init(&u);
        CHECK(url_parse(&u, d->addr.bytes, strlen(d->addr.bytes), NULL),
              "a document's own address is not a URL — §2.4.3 asks whether it MATCHES about:blank, which is a "
              "question about a URL record, and this record's address never parsed");
        /* HTML §2.4.1's match relation, the about:blank one only. A document created at about:blank whose address
           later moved (§7.4.4's URL and history update steps, `history.pushState` from an inherited origin) stops
           matching, and step 3's address answers though the about base URL remains: the standard tests both. */
        about = url_matches_about(&u, "blank", /*query_must_be_null*/ false);
        url_record_free(&u);
        if (about) return d->base.about;
    }
    return d->addr.bytes;   /* step 3: "return document's URL" */
}

/* §2.4.3's document base URL of one Document: what DOM §4.4's `baseURI`, HTML §2.4.2's parse-a-URL and §8.1.5.1's API
   base URL name. A fact about that document, not the running realm, since §4.5.1's factories build other Documents.
   Borrowed. */
const char *document_base_url_of(const lxb_dom_document_t *dom)
{
    const Document *d = doc_rec(dom);

    DCHECK(d != NULL, "§2.4.3's document base URL was asked of a document with no record");
    /* Step 1: "if document has no descendant base element that has an href attribute, then return document's
       fallback base URL." Step 2: "otherwise, return the frozen base URL of the first base element in document that
       has an href attribute, in tree order", the element this record names, kept current by §4.2.3's situations
       (core/html/html_base_element.c). */
    if (!d->base.frozen_el) return document_fallback_base_url_of(dom);
    DCHECK(d->base.frozen != NULL && d->base.frozen[0] != '\0',
           "a document names a base element whose FROZEN BASE URL is absent or empty — §4.2.3's set the "
           "frozen base URL ends in either a serialized URL record or the document's fallback base URL, and "
           "neither is empty, so this pair was written by something that is not that algorithm");
    return d->base.frozen;
}

const char *document_base_url(JSContext *ctx)
{
    Document *d;

    /* HTML §10.2.6.2 Script settings for workers: a worker's API base URL is its global scope's url. */
    if (!doc_of(ctx)) return worker_env_of(ctx)->url;
    d = doc_here(ctx);
    DCHECK(d->addr.bytes[0] != '\0', "this realm's API base URL was read before its document was installed");
    return document_base_url_of(lxb_dom_interface_document(d->dom));
}

/* This realm's active document's address, §4.5's `document.URL`, a different question from the base URL. §7.2.4's
 * "this Location object's relevant Document's URL", §7.2.5's can-have-its-URL-rewritten, §7.4.4's "let newURL be
 * document's URL" and §8.1's blob origin read this; with `<base href>` in effect, the base URL would make
 * `location.href` answer the markup's base and a blob URL carry a CDN's origin. A caller says which it asks. */
const char *document_url(JSContext *ctx)
{
    Document *d = doc_here(ctx);

    DCHECK(d->addr.bytes[0] != '\0', "this realm's document address was read before its document was installed");
    return d->addr.bytes;
}

lxb_dom_element_t *document_frozen_base_element(const lxb_dom_document_t *dom)
{
    const Document *d = doc_rec(dom);

    DCHECK(d != NULL, "§4.2.3's frozen base element was asked of a document with no record");
    return d->base.frozen_el;
}

/* §4.2.3's frozen base URL, stored (see the field; core/html/html_base_element.c decides when). `el` and `url` both
   NULL means no base element with an href, which §2.4.3 step 1 answers with the fallback base URL. */
void document_set_frozen_base_url(lxb_dom_document_t *dom, lxb_dom_element_t *el, const char *url)
{
    Document *d = doc_rec(dom);

    DCHECK(d != NULL, "§4.2.3's frozen base URL was set on a document with no record");
    DCHECK((el == NULL) == (url == NULL),
           "§4.2.3's frozen base URL was set with an element and no URL, or a URL and no element — the pair is "
           "one fact ('a base element … HAS a frozen base URL'), and a document that held half of it would "
           "either answer §2.4.3 step 2 with nothing or hold a base URL belonging to no element");
    DCHECK(url == NULL || url[0] != '\0',
           "§4.2.3's frozen base URL was set to the empty string — the algorithm ends in either a serialized "
           "URL record or the document's fallback base URL, and neither is ever empty");
    DCHECK(JS_IsObject(d->doc_obj),
           "§4.2.3's frozen base URL was set before the document's `document` object existed — the object is "
           "what owns the storage the flow's delta captures, so there would be nothing to keep the address "
           "alive for a parked flow");
    /* Two scalar captures for one fact (see the field): each is a pointer-sized latch, the byte arm's purpose, and
       the pair reverts together because a delta unapplies wholly. */
    cow_capture_host_state(d->realm, d->doc_obj, &d->base.frozen_el, sizeof d->base.frozen_el);
    cow_capture_host_state(d->realm, d->doc_obj, &d->base.frozen, sizeof d->base.frozen);
    d->base.frozen_el = el;
    /* The old frozen URL is not freed: a flow that captured it above names it, and §4.2.3 re-freezes on every tree
       change. It stays on the record's address list until the record dies. */
    d->base.frozen = url ? doc_addr_intern(d, url) : NULL;
    DCHECK((d->base.frozen_el == NULL) == (d->base.frozen == NULL),
           "§4.2.3's frozen base URL and the element it belongs to disagree about whether this document has "
           "one — they are captured as two entries and reverted as one fact, so a document holding half the "
           "pair has had one of them written outside this setter");
}

/* §7.4's about base URL has no setter: it is an argument of doc_rec_new, so the record is born with it, before
 * document_install's parsed walks run. Two of them read §2.4.3 "Document base URLs": §4.2.3's freeze (the fallback
 * base URL) and §4.8.5's iframe walk (the document base URL for a nested `src`), and for an iframe srcdoc document a
 * null about base URL would fire §2.4.3 step 1's assert on ordinary markup such as `<iframe srcdoc="<base
 * href=/x/>">`, or in release resolve every relative URL against `about:srcdoc`, whose parse fails. */

/* DOM §4.5's encoding of this realm's active document (see document.h). One component owns what a document's
   encoding is, as for its URL; it decides how a `<script src>`'s bytes become source text. */
int document_encoding(JSContext *ctx)
{
    Document *d = doc_here(ctx);

    DCHECK(d->encoding >= 0,
           "a document's encoding was read before the record carried one — DOM §4.5 gives every Document an "
           "encoding and doc_rec_new writes it, so a negative id is a record some other path built");
    return d->encoding;
}

/* The same fact asked of a Document rather than a realm: HTML §4.10.22.5 "Selecting a form submission encoding"
   step 1, "let encoding be the document's character encoding", means the form's node document, and a realm holds
   several (an inert template document, a `createHTMLDocument` result). Same shape as document_base_url_of. */
int document_encoding_of(const lxb_dom_document_t *dom)
{
    const Document *d = doc_rec(dom);

    DCHECK(d != NULL, "DOM §4.5's encoding was asked of a document with no record");
    DCHECK(d->encoding >= 0,
           "a document's encoding was read before the record carried one — DOM §4.5 gives every Document an "
           "encoding and doc_rec_new writes it, so a negative id is a record some other path built");
    return d->encoding;
}

/* HTML §13.2.3.2 "Determining the character encoding"'s answer, written onto the document it is about. DOM §4.5
 * "Interface Document" gives every document utf-8 "unless stated otherwise", which doc_rec_new writes; this is the
 * one algorithm that states otherwise, and it is a navigation's step (the bytes and response metadata belong to the
 * load), so the loader calls it and nothing reachable from a Document can re-derive it later.
 * It is per flow, through the byte capture, since the encoding is a POD `int` on this
 * record, and the `document` object owns the record's storage for a flow parked in a load. */
void document_state_x_frame_options(JSContext *ctx, char *const *values, size_t n)
{
    Document *d = doc_here(ctx);
    size_t i;

    DCHECK(!d->xfo_stated, "a Document's X-Frame-Options was stated twice — it is a fact about the one response "
                           "the Document was created from, stated by the operation that created it");
    DCHECK((values == NULL) == (n == 0), "a Document's X-Frame-Options was stated with a count that disagrees "
                                         "with its values — get, decode, and split answers null or a list");
    if (values) {
        d->xfo = malloc(n * sizeof *d->xfo);
        CHECK(d->xfo != NULL, "document: OOM holding X-Frame-Options");
        for (i = 0; i < n; i++) {
            d->xfo[i] = strdup(values[i]);
            CHECK(d->xfo[i] != NULL, "document: OOM holding X-Frame-Options");
        }
    }
    d->n_xfo = n;
    d->xfo_stated = true;
}

bool document_embeddable_cross_origin(JSContext *ctx, const UrlRecord *ancestor)
{
    Document *d = doc_here(ctx);
    const CspList *list = policy_container_csp_list(d->policy);
    bool stated = false, set_deny = false, set_allowall = false, set_sameorigin = false;
    size_t i, j, distinct = 0;

    DCHECK(d->xfo_stated, "a Document's embeddability was asked before its creating operation stated the "
                          "response's X-Frame-Options (document_state_x_frame_options)");
    /* HTML §7.7 steps 2.1-2.2: an enforced frame-ancestors decides, by CSP §6.4.2.1's per-ancestor match, whose
       step 1 allows a response at a local URL outright. */
    {
        UrlRecord own;
        const char *u = document_url(ctx);
        bool local;

        url_record_init(&own);
        local = url_parse(&own, u, strlen(u), NULL) && url_scheme_is_local(own.scheme);
        url_record_free(&own);
        if (local) return true;
    }
    for (i = 0; i < list->n_policies; i++) {
        const CspDirective *dir;

        if (!csp_policy_is_enforced(&list->policies[i])) continue;
        if (!(dir = csp_policy_directive(&list->policies[i], "frame-ancestors"))) continue;
        stated = true;
        if (csp_source_list_match_url(dir, ancestor, list->self_origin, 0) != CSP_MATCHES) return false;
    }
    if (stated) return true;
    if (!d->xfo) return true;
    /* Steps 4-5: the values as a set, ASCII lowercase. */
    for (i = 0; i < d->n_xfo; i++) {
        for (j = 0; j < i && strcasecmp(d->xfo[j], d->xfo[i]) != 0; j++) {}
        if (j < i) continue;
        distinct++;
        set_deny |= !strcasecmp(d->xfo[i], "deny");
        set_allowall |= !strcasecmp(d->xfo[i], "allowall");
        set_sameorigin |= !strcasecmp(d->xfo[i], "sameorigin");
    }
    if (distinct > 1) return !(set_deny || set_allowall || set_sameorigin);   /* steps 6-7 */
    if (!strcasecmp(d->xfo[0], "deny")) return false;                           /* step 8 */
    /* Step 9: `sameorigin` against a container document this Document is not same origin with. */
    if (!strcasecmp(d->xfo[0], "sameorigin")) return false;
    return true;
}

/* Named residual: a nested navigable's Document has no obtained opener policy (§7.4.5 obtains it only for a
   top-level traversable), so `opener` is not stated for it. The next diff obtains every response's opener
   policy, so a top-level load of its address is judged. Its absence shows as a framed document's message
   offering `frame` alone. */
void document_message_routes(JSContext *ctx, bool *frame, bool *opener)
{
    JSValueConst proxy = document_window_proxy(ctx);
    UrlRecord attacker;

    url_record_init(&attacker);
    CHECK(url_parse(&attacker, ATTACKER_PAGE, strlen(ATTACKER_PAGE), NULL),
          "document: the attacker page's own address did not parse");
    *frame = document_embeddable_cross_origin(ctx, &attacker);
    url_record_free(&attacker);
    *opener = window_proxy_is_top_level(proxy) && window_proxy_opener_policy(proxy) == OPENER_POLICY_UNSAFE_NONE;
}

void document_set_encoding(JSContext *ctx, int encoding)
{
    Document *d = doc_here(ctx);

    DCHECK(encoding >= 0,
           "a Document's encoding was set to a failure — §13.2.3.2's last step is a DEFAULT and therefore "
           "always answers, so a negative id here is a caller that skipped the algorithm and passed on a raw "
           "`get an encoding` result instead");
    DCHECK(JS_IsObject(d->doc_obj),
           "a Document's encoding was set before its `document` object existed — the object is what owns the "
           "storage the flow's delta captures, so there would be nothing to keep the value alive for a flow "
           "that parks inside this load");
    cow_capture_host_state(ctx, d->doc_obj, &d->encoding, sizeof d->encoding);
    d->encoding = encoding;
}

/* The bytes an address value stands for (see core/dom/document.h). One function, not an `if` at each arrival
 * (§7.2.5's `pushState`, §4.8.5's `<iframe src>`, §7.2.2.1's `window.open`), since a dispatch that decides what
 * bytes are is one component every entry calls; a concolic's own ToString would name the JavaScript coercion
 * boundary for a value that is a good address with a domain.
 * A concolic with no example is a Document whose address this run does not know, and it crashes here once: its
 * display shape would seed `…/admin/%7Bstate%7D.id`, an address no server has, which solver/route_seed.h would
 * declare and the zone would fetch over the person's session. */
const char *document_address_example(JSContext *ctx, JSValueConst address)
{
    const char *bytes;

    DCHECK(!JS_IsUndefined(address) && !JS_IsNull(address),
           "a Document address value was asked for its bytes and there is no value — an address is a VALUE in "
           "this engine and every producer of one states it, so an absent value is a caller that built an "
           "address out of a slot it never wrote rather than a Document that has no address (which is the "
           "EMPTY STRING here, and a string is a value)");
    if (concolic_is(address)) {
        JSValue ex = concolic_example(ctx, address);
        int has_example = !JS_IsUndefined(ex);

        if (has_example) {
            bytes = JS_ToCString(ctx, ex);
            JS_FreeValue(ctx, ex);
            CHECK(bytes != NULL, "document: an address value's example could not be read as bytes");
            return bytes;
        }
        JS_FreeValue(ctx, ex);
        DFAILF("a Document's address is UNKNOWN EXTERNAL INPUT `%s` CARRYING NO EXAMPLE, so this run does not "
               "know where this Document went. Every arrival that makes a computed value a Document's address "
               "reaches this one line — HTML §7.2.5 \"The History interface\"'s shared history push/replace "
               "state steps step 5, HTML §4.8.5 \"The `iframe` element\"'s `src`, HTML §7.2.2.1 \"Opening and "
               "closing windows\"'s window open steps — and none of them may answer it by coercion or by "
               "shape: the bytes become the Document's address at HTML §7.4.4 \"Non-fragment synchronous "
               "\\\"navigations\\\"\" step 8 and are declared a page of this application at the line before it "
               "(solver/route_seed.h), and the trusted zone LOADS a declared page over the person's own "
               "session. BUILD: the Document whose ADDRESS IS UNKNOWN — a Document state whose address has a "
               "DOMAIN and no example, so §7.4.4 step 8 can install it, `location.href` can answer with it, "
               "and route_seed declares NOTHING because there is no address to load. Until that exists there "
               "is no newURL for §7.2.5 step 5 to produce, and inventing one from the shape is the false page "
               "this crash refuses",
               concolic_shape_c(address) ? concolic_shape_c(address) : "{}");
    }
    bytes = JS_ToCString(ctx, address);
    /* Fatal, not "the address is absent": the two causes are OOM and a release build, where the arm above is
       compiled out and a concolic's ToString throws, which must not silently leave a Document where it was. */
    CHECK(bytes != NULL,
          "a Document address value would not convert to bytes: either the allocation failed, or this is a "
          "release build and the address is unknown external input, whose ToString has no concolic semantics — "
          "a dev build names the capability to build at the arm above this line");
    return bytes;
}

/* The two halves of an address agree, asserted at every writer: `bytes` is what every byte consumer resolves
   against and the trusted zone loads, `value` what a branch forks on. One call writes both, so a disagreement is a
   writer that computed one from something other than the other. */
static void doc_addr_assert_agrees(JSContext *ctx, const Document *d)
{
#if APICLIENT_DEV
    const char *ex = document_address_example(ctx, d->addr.value);
    int same = strcmp(ex, d->addr.bytes) == 0;

    JS_FreeCString(ctx, ex);
    DCHECK(same,
           "a Document's ADDRESS and the VALUE it was computed out of disagree — the bytes are what "
           "solver/route_seed.h declares and what every base-URL resolution in the page resolves against, and "
           "the value is what a `location.pathname` branch forks on, so a record holding two different "
           "addresses reports one page to the trusted zone and explores another");
#else
    (void)ctx; (void)d;
#endif
}

/* HTML §2.4.3 "Document base URLs"' set the URL, step 1: "Set document's URL to url", the only way a Document's
 * address changes without a new Document; HTML §7.4.4 "Non-fragment synchronous \"navigations\""'s URL and history
 * update steps step 8 performs it.
 * It takes a value (core/dom/document.h's document_url_value): this is the one door through which a Document gets an
 * address, and the bytes are derived from the value here rather than passed beside it.
 * It is per flow, through the record arm: a flow that called `history.pushState(s, "", "/b")` is the only one whose
 * `location.pathname` is `/b`. The pair holds a JSValue, which the byte arm (solver/cow.h, POD scalar latches only)
 * would copy uncounted, and one entry for the pair means a context switch restores both or neither. The unapply is
 * sound because the old string stays on this record's address list; the old value was dup'd by the capture, so the
 * record's own reference is released here and the delta keeps its. The owner is the `document` object, which holds
 * the record's storage alive for a parked flow. */
void document_set_url(JSContext *ctx, JSValue address)
{
    Document *d = doc_here(ctx);
    const char *url;

    CHECK(!JS_IsException(address), "document: a Document's address could not be built");
    url = document_address_example(ctx, address);
    DCHECK(url[0] != '\0',
           "a Document's URL was set to nothing — every caller of this has already parsed and serialized a URL "
           "record, and an empty address is what a document with no browsing context has rather than something "
           "an algorithm assigns");
    DCHECK(JS_IsObject(d->doc_obj),
           "a Document's URL was set before its `document` object existed — the object is what owns the "
           "storage the flow's delta captures, so there would be nothing to keep the address alive for a "
           "parked flow");
    cow_capture_host_record(d->doc_obj, &d->addr, &DOC_ADDR_REC);
    d->addr.bytes = doc_addr_intern(d, url);
    /* Published before the old value is released: doc_rec_refs walks this slot for the collector, and releasing a
       concolic address runs that class's finalizer, which may allocate (and an allocation may collect), so freeing
       first would let the walk decref a freed object. */
    {
        JSValue prev = d->addr.value;

        d->addr.value = address;                   /* consumed */
        JS_FreeValue(ctx, prev);
    }
    JS_FreeCString(ctx, url);
    DCHECK(doc_addr_owned(d, d->addr.bytes),
           "a Document's address is not one this record interned — the delta captured the POINTER, so the "
           "field may only ever name storage this record owns and releases");
    doc_addr_assert_agrees(ctx, d);
}

/* This realm's active document's address as a value (see core/dom/document.h): what core/frame/location.c derives
   §7.2.4's members through, so `location.pathname` on a computed address still forks. */
JSValue document_url_value(JSContext *ctx)
{
    Document *d = doc_here(ctx);

    DCHECK(!JS_IsUndefined(d->addr.value),
           "a Document's address was read as a value and the record holds none — doc_rec_new writes a string "
           "at the record's birth and document_set_url writes one at every change, so an absent value is a "
           "record some other path built, and a reader defaulting past it would report a COMPUTED address as "
           "a concrete fact");
    return JS_DupValue(ctx, d->addr.value);
}

/* HTML §3.1.5's current document readiness, the internal slot, not the member: `readyState` is a readonly accessor
 * on Document.prototype (core/dom/document_metadata.c) that returns this, so a page cannot assign it (the load
 * lifecycle and §8.1.7.3 step 3's render-blocked test both read it). It lives in this realm's own baseline record,
 * the shape §8.12 "Animation frames"'s map and §7.4.6.3's has-been-revealed use: unreachable from the page, and its
 * `stage` is an ordinary property write the heap COW captures, so one arm of a fork advances its lifecycle alone. A
 * private Symbol would be agent-wide and freed by a per-realm teardown (document_free runs per realm). */
static JSClassID g_ready_slot = JS_INVALID_CLASS_ID;

/* HTML §3.1.5's three readiness values, in the enum's order: the strings `readyState` answers with. */
static const char *const DOC_READINESS[3] = { "loading", "interactive", "complete" };

static int document_readiness(JSContext *ctx);
static void document_set_dcl_queued(JSContext *ctx, bool on);

/* HTML §3.1.5's update the current document readiness, all four steps. Step 1 ("if document's current document
   readiness equals readinessValue, then return") stops a `readystatechange` for an unchanged readiness, and makes
   step 4 reachable only where there is a document (a realm's record starts at "loading" before its Document
   exists). Step 4 fires `readystatechange` at the document (§3.1.1 declares `onreadystatechange`). Step 3's load
   timing info is absent: its fields belong to Navigation Timing's `PerformanceNavigationTiming`, which this build
   lacks.
   Named residual. Not covered: step 4 is a bare fire, and §13.2.7 "The end" step 9 runs step 9.1's readiness update
   and step 9.5's `load` fire in one queued task, so a `readystatechange` listener cannot suspend this transition.
   Next diff builds: document_done_stage as a step machine, so the fire is event_target_fire_run's shape. How its
   absence would show: a `readystatechange` listener that awaits or loops past a quantum. */
static void document_set_ready(JSContext *ctx, int stage)
{
    Document *d = doc_here(ctx);
    JSValue rec;

    DCHECK(stage >= 0 && stage <= 2, "a document readiness HTML does not define");
    if (document_readiness(ctx) == stage) return;                                 /* step 1 */
    rec = realm_value_get(ctx, g_ready_slot);
    DCHECK(JS_IsObject(rec), "a realm answered for its document readiness with no record");
    JS_SetPropertyStr(ctx, rec, "stage", JS_NewInt32(ctx, stage));                /* step 2 */
    JS_FreeValue(ctx, rec);
    DCHECK(JS_IsObject(d->doc_obj),
           "a document's readiness moved before its `document` object existed — §3.1.5 step 4 fires "
           "`readystatechange` AT the document, so there would be no target for the event the page listens for");
    /* No task source: §3.1.5's step 4 is a bare "fire an event named readystatechange at document", a synchronous
       step of whichever algorithm moved the readiness, so it states the positive no-source answer. */
    event_target_fire(ctx, d->doc_obj,                                            /* step 4 */
                      event_new(ctx, "readystatechange", /*bubbles*/ false, /*cancelable*/ false),
                      JS_UNDEFINED, TASK_SOURCE_NOT_A_TASK);
}

/* HTML §8.4.1 "Opening the input stream" step 18, "Update the current document readiness of document to 'loading'",
 * the one readiness transition another algorithm may ask for. A named entry, not a general setter: every other move
 * belongs to §13.2.7 "The end" and the lifecycle below, and §8.4.1 is the single case where a finished Document goes
 * back to loading. The `readystatechange` it fires is unobservable here, since §8.4.1 step 10 erased every listener
 * and handler. */
void document_set_readiness_loading(JSContext *ctx)
{
    document_set_ready(ctx, 0);
    document_set_dcl_queued(ctx, false);   /* the reopened stream reaches §13.2.7 again */
}

/* See document.h. It is its own fact because step 3 makes the readiness "interactive" before step 5 runs the
   deferred scripts, so the readiness cannot say it. */
bool document_dcl_queued(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_ready_slot), v;
    bool on;

    DCHECK(JS_IsObject(rec), "a realm answered for its DOMContentLoaded latch with no record");
    v = JS_GetPropertyStr(ctx, rec, "dcl");
    DCHECK(JS_IsBool(v), "a document's readiness record holds no DOMContentLoaded latch");
    on = JS_ToBool(ctx, v) != 0;
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, rec);
    return on;
}

static void document_set_dcl_queued(JSContext *ctx, bool on)
{
    JSValue rec = realm_value_get(ctx, g_ready_slot);

    DCHECK(JS_IsObject(rec), "a realm was asked to set a DOMContentLoaded latch it has no record for");
    JS_SetPropertyStr(ctx, rec, "dcl", JS_NewBool(ctx, on));
    JS_FreeValue(ctx, rec);
}

/* This document's readiness: 0 loading, 1 interactive, 2 complete. */
static int document_readiness(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_ready_slot), v;
    int32_t r = 0;

    DCHECK(JS_IsObject(rec), "a realm answered for its document readiness with no record");
    v = JS_GetPropertyStr(ctx, rec, "stage");
    JS_ToInt32(ctx, &r, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, rec);
    DCHECK(r >= 0 && r <= 2, "a document's readiness record holds a stage HTML does not define");
    return r;
}

/* §3.1.5's readyState getter steps, "return this's current document readiness", for the receiver's document.
   §3.1.5: "Each Document has a current document readiness, a string, initially 'complete'"; only a Document the
   create-and-initialize algorithm produced (document_install, the realm's active one) is reset to "loading", so a
   createHTMLDocument, a DOMParser parse or an XHR responseXML answers "complete". */
const char *document_readiness_of(const lxb_dom_node_t *doc)
{
    JSContext *realm = document_active_realm_of(doc);

    return realm ? DOC_READINESS[document_readiness(realm)] : DOC_READINESS[2];
}

/* HTML §7.5.9's page showing, a Document's, kept where its readiness is. It is the boolean "used to ensure that
 * scripts receive pageshow and pagehide events in a consistent manner (e.g., that they never receive two pagehide
 * events in a row without an intervening pageshow, or vice versa)", the condition on both fires: §13.2.7 sets it
 * true before `pageshow`, and §7.5.9 step 9 fires `pagehide` only if it is true, clearing it first. It lives in this
 * realm's baseline record, as the readiness and §6.2's visibility state do: unreachable from the page, and an
 * ordinary property write the heap COW captures, so one arm of a fork can unload its document alone. */
static JSClassID g_showing_slot = JS_INVALID_CLASS_ID;

bool document_page_showing(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_showing_slot), v;
    int32_t on = 0;

    DCHECK(JS_IsObject(rec), "a realm answered for its document's page showing with no record");
    v = JS_GetPropertyStr(ctx, rec, "showing");
    JS_ToInt32(ctx, &on, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, rec);
    return on != 0;
}

void document_page_showing_set(JSContext *ctx, bool showing)
{
    JSValue rec = realm_value_get(ctx, g_showing_slot);

    DCHECK(JS_IsObject(rec), "a realm was asked to set a page showing it has no record for");
    JS_SetPropertyStr(ctx, rec, "showing", JS_NewInt32(ctx, showing ? 1 : 0));
    JS_FreeValue(ctx, rec);
}

/* HTML §7.5.8 "Finishing the loading process": "to completely finish loading a Document document":
 *
 *   1. Assert: document's browsing context is non-null.
 *   2. Set document's completely loaded time to the current time.
 *   3. Let container be document's node navigable's container.
 *   4. If container is an `iframe` element, then queue an element task on the DOM manipulation task source
 *      given container to run the iframe load event steps given container.
 *   5. Otherwise, if container is non-null, then queue an element task on the DOM manipulation task source
 *      given container to fire an event named `load` at container.
 *
 * This is where an iframe's `load` comes from (`frame.onload`, `frame.addEventListener('load', …)`). */
/* Step 2 is not here, because nothing reads it: a completely loaded time is read only by §7.2.4's location-object
 * navigate ("if location's relevant Document is not yet completely loaded … set historyHandling to 'replace'"),
 * which core/frame/location.c answers from the session history's initial-about:blank test.
 * This is the load event steps' caller for a document a navigation produced, and only that. §4.8.5's other caller,
 * "process the iframe attributes" with initialInsertion true over an about:blank url, is core/html/html_iframe.c's;
 * the arm below reads §7.4.4's "is initial about:blank" and returns, so a frame gets its `load` from exactly one.
 * `ctx` is the child's realm, and the fire is enqueued in the container's: §7.5.8's element task has the element's
 * node document's relevant global, the parent's, where the listener was registered. */
static void completely_finish_loading(JSContext *ctx)
{
    JSValueConst proxy = doc_here(ctx)->proxy;
    JSValue container;
    lxb_dom_node_t *cn;
    JSContext *cctx;

    /* Step 1. Step 9.2 already returned for a destroyed navigable, so a Document here with no navigable would be
       §4.5's createDocument or a DOMParser's, which never reach §13.2.7's end because document_lifecycle_step walks
       navigables. */
    DCHECK(window_proxy_is(proxy),
           "§7.5.8's completely-finish-loading was reached by a Document with no navigable — §13.2.7 \"The "
           "end\" runs for the documents of this agent's navigables, so a Document that has none has no "
           "loading process for this algorithm to finish");
    container = window_proxy_container(ctx, proxy);                                          /* step 3 */
    cn = node_of(container);
    if (!cn) { JS_FreeValue(ctx, container); return; }   /* a top-level traversable: step 5's "non-null" fails */
    /* Step 3's other answer, stated because this engine's container link is never null. §7.5.8 notes the container
       "will be null in the case where document is the initial about:blank Document in a frame or iframe, since at
       the point of browsing context creation which calls this algorithm, the container relationship has not yet
       been established … Instead, a synchronous load event is fired in a special initial-insertion case when
       processing the iframe attributes." §7.3.1.3's create here writes the link as one of its own steps, so the fact
       is asked directly: window_proxy_ever_navigated is §7.4.4's "is initial about:blank", and false is that case.
       §4.8.5's branch (core/html/html_iframe.c) fires once at the insertion, and this arm keeps a later
       materialization (navigable_tree_order lists only materialized navigables) from firing a second. */
    if (!window_proxy_ever_navigated(proxy)) { JS_FreeValue(ctx, container); return; }
    /* Step 4's branch is the only one reachable here, and step 5 is an assert: only html_iframe.c creates a content
       navigable from an element, so a non-`<iframe>` container cannot exist. When §4.8.6's `<embed>` or §4.8.7's
       `<object>` gets one, this site must learn step 5 (fire `load` at the container), which is not the iframe load
       event steps. */
    DCHECK(iframe_element_is(cn),
           "§7.5.8 step 5 — a navigable's container that is not an `<iframe>` finished loading. Content "
           "navigables belong to `<iframe>` alone in this engine, so building §4.8.6's `<embed>` or §4.8.7's "
           "`<object>` as navigable containers means teaching this site step 5: queue an element task on the "
           "DOM manipulation task source given container to FIRE `load` AT IT, which is not the same algorithm "
           "as §4.8.5's iframe load event steps");
    /* The container's realm, asked of the container's own document: two same-origin documents are one agent and one
       heap, so this is a pointer; a cross-origin container is a peer's element and not in this heap. */
    cctx = document_realm_of(cn);
    DCHECK(cctx != NULL,
           "§7.5.8 reached a container element whose document has no realm — an element that presents a "
           "navigable is an element in a document a flow ran the insertion steps in, so its realm is the one "
           "that ran them");
    iframe_run_load_event_steps(cctx, container);                                            /* step 4 */
    JS_FreeValue(ctx, container);
}

static int document_done_stage(JSContext *ctx, int stage)
{
    if (stage == 0) {
        DCHECK(document_readiness(ctx) == 1,
               "§13.2.7 step 6's DOMContentLoaded task ran before step 3 made the readiness interactive");
        document_set_dcl_queued(ctx, true);
        /* §3.1.1: DOMContentLoaded is fired at the document and bubbles, so a listener on window hears it through
           the document's ancestors. It is not cancelable. */
        event_target_fire(ctx, doc_here(ctx)->doc_obj,
                          event_new(ctx, "DOMContentLoaded", /*bubbles*/ true, /*cancelable*/ false),
                          JS_UNDEFINED, TASK_SOURCE_DOM_MANIPULATION);   /* §13.2.7 */
        return 1;
    }
    DCHECK(stage == 1, "the document lifecycle was asked for a stage it does not have");
    /* §13.2.7 "The end", step 9: the second of its two queued global tasks, whose sub-steps are each observable and
       in this order. Steps 7 (the ASAP-script wait, cited by core/loader/script_type.h) and 8 (the load-delay wait)
       are gates on this task. */
    document_set_ready(ctx, 2);                                                  /* step 9.1 */
    /* Step 9.2: "If the Document object's browsing context is null, then abort these steps." A document whose
       navigable was destroyed (a frame removed, a traversable closed) fires no `load` or `pageshow`, and §7.5.9
       destroys documents while this walk runs. Destruction is asked, not `closed`: a closing traversable still has
       its browsing context. */
    {
        JSValueConst proxy = doc_here(ctx)->proxy;
        if (window_proxy_is(proxy) && window_proxy_destroyed(proxy))
            return 1;
    }
    /* Step 9.5: `load` at the window. It does not bubble, and it is fired with the legacy target override flag set,
       so a listener's `e.target` is the document. */
    event_target_fire(ctx, doc_here(ctx)->win_obj,
                      event_new(ctx, "load", /*bubbles*/ false, /*cancelable*/ false),
                      doc_here(ctx)->doc_obj, TASK_SOURCE_DOM_MANIPULATION);   /* §13.2.7 */
    /* Steps 9.9-9.11: the document is showing, and `pageshow` says so with `persisted` false (loaded, not restored);
       §7.5.9's `pagehide` is the other end of the pair. They are a transition conditioned on the flag rather than
       step 9.9's literal "Assert: Document's page showing is false", because the standard reaches the end twice:
       §8.4.1 "Opening the input stream" replaces a Document in place and resets the readiness to "loading", so
       `document.open(); document.write(…); document.close()` runs §13.2.7 again on a showing Document that nothing
       unloaded (§7.5.9 is the only algorithm that clears the flag). The `load` fire above is not under the test: a
       browser fires `load` again after document.close(), which an `iframe`'s `onload` after a written document
       relies on. */
    if (!document_page_showing(ctx)) {
        document_page_showing_set(ctx, true);
        event_target_fire(ctx, doc_here(ctx)->win_obj,
                          page_transition_event_new(ctx, "pageshow", /*persisted*/ false),
                          doc_here(ctx)->doc_obj, TASK_SOURCE_DOM_MANIPULATION);   /* §13.2.7 */
    }
    completely_finish_loading(ctx);                                              /* step 9.12 */
    return 1;
}

/* HTML §13.2.7 "The end" step 8, "Spin the event loop until there is nothing that delays the load event in the
 * Document", as a predicate over this agent's navigable tree, asked of one document. The scheduler's quiescence is
 * not this gate: it is a fact about one flow (so a parent could finish ahead of its frames), and HTML makes nothing
 * about a `fetch()` or dynamic `import()` delay the load event.
 * The source built here is the navigable container's: §4.8.5 "The iframe element" says "the user agent must delay the
 * load event of element's node document if element's content navigable is non-null and any of the following are
 * true: element's content navigable's active document is not ready for post-load tasks; element's content
 * navigable's is delaying load events is true; or anything is delaying the load event of element's content
 * navigable's active document." Readiness < 2 is the first bullet (§13.2.7 step 11 follows step 9.1 with nothing
 * observable between), and the walk covers the whole subtree, so a loading grandchild answers the third. It asks the
 * container relation (window_proxy_parent_navigable), which §7.3.1.6 severs when a flow detaches an `<iframe>`. */
/* navigable_tree_order lists only materialized local navigables. A child whose document is still being fetched is
 * held by the scheduler instead: §7.4 step 14's fetch parks as a synchronous host request (FLOW_PENDING_HOSTREQ),
 * so flow_blocked is true and no lifecycle stage is asked. A cross-origin child is in another instance and is
 * skipped, so a parent whose only unfinished frame is cross-origin fires `load` early until the cross-instance read
 * §Security describes exists. Per-element sources are named at the call site. */
static bool document_load_event_delayed(JSContext *ctx, JSValueConst docs, uint32_t n, JSValueConst self)
{
    uint32_t j;

    for (j = 0; j < n; j++) {
        JSValue other = JS_GetPropertyUint32(ctx, docs, j);
        JSValue up;
        bool below = false;

        if (JS_IsSameValue(ctx, other, self)) { JS_FreeValue(ctx, other); continue; }
        /* Is `self` an ancestor of `other`? The containing-navigable chain ends at JS_UNDEFINED for a top-level
           traversable, so a document in another tree of this agent (an auxiliary navigable from `window.open()`)
           never matches: a popup delays nobody's load event. */
        up = window_proxy_parent_navigable(ctx, other);
        while (!JS_IsUndefined(up) && !JS_IsNull(up)) {
            JSValue next;
            if (JS_IsSameValue(ctx, up, self)) { below = true; break; }
            next = window_proxy_parent_navigable(ctx, up);
            JS_FreeValue(ctx, up);
            up = next;
        }
        JS_FreeValue(ctx, up);
        if (below && document_readiness(window_proxy_realm(ctx, other)) < 2) {
            JS_FreeValue(ctx, other);
            return true;
        }
        JS_FreeValue(ctx, other);
    }
    return false;
}

/* The load lifecycle is per document: the stage is read from each realm's Document (its readiness is an own slot,
 * isolated by the COW delta), so a child navigable's document gets its own DOMContentLoaded and `load` and leaves
 * §8.1.7.3 step 3's render-blocked state. The order is the spec's, in two passes: every document's DOMContentLoaded
 * precedes any `load` (a parent's parse finishes while its frames load), in tree order, and a child's `load`
 * precedes its parent's, in reverse tree order. The reverse order decides which of two ready documents goes first;
 * §13.2.7 step 8 (document_load_event_delayed) is the wait, asked per document in pass two. One document per call,
 * because this is a work item on the one frontier and each fire queues listener tasks the loop runs before the next
 * stage. Returns 1 when it advanced one, 0 when every document of this agent is complete. */
/* §13.2.7 "The end"'s premise, asked of one document: "Once the user agent stops parsing the document, the user
 * agent must run the following steps." The readiness does not answer it: §8.4.1 "Opening the input stream" sets the
 * readiness back to "loading" and opens an input stream only §8.4.2's `close()` ends. §13.2.3.5's insertion point
 * is "the parse has not ended" (§13.2.7 step 2 makes it undefined), so this asks the parser. A realm with no Document
 * record cannot be mid-parse. */
static bool document_parse_ended(JSContext *realm)
{
    const Document *d = doc_of(realm);

    return d == NULL || !html_parse_insertion_point_defined(lxb_dom_interface_document(d->dom));
}

/* See document.h. Steps 1, 2 and 4 have nothing to do here: there is no speculative parser, and lexbor's close
   already set the insertion point to undefined and popped the stack of open elements. */
int document_parse_end_step(JSContext *ctx)
{
    JSValue docs = navigable_tree_order(ctx), len;
    uint32_t n = 0, i;
    int did = 0;

    len = JS_GetPropertyStr(ctx, docs, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n && !did; i++) {
        JSValue proxy = JS_GetPropertyUint32(ctx, docs, i);
        JSContext *realm = window_proxy_realm(ctx, proxy);

        if (document_readiness(realm) == 0 && document_parse_ended(realm) &&
            engine_document_parse_position(document_doc(realm)) == NULL) {
            document_set_ready(realm, 1);   /* step 3, which fires `readystatechange` (§3.1.5 step 4) */
            did = 1;
        }
        JS_FreeValue(ctx, proxy);
    }
    JS_FreeValue(ctx, docs);
    return did;
}

int document_lifecycle_step(JSContext *ctx)
{
    JSValue docs = navigable_tree_order(ctx), len;
    uint32_t n = 0, i;
    int did = 0;

    len = JS_GetPropertyStr(ctx, docs, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n && !did; i++) {          /* pass one: DOMContentLoaded, in tree order */
        JSValue proxy = JS_GetPropertyUint32(ctx, docs, i);
        JSContext *realm = window_proxy_realm(ctx, proxy);
        /* §13.2.7 steps 5-6: DOMContentLoaded is queued once the list of scripts that will execute when the
           document has finished parsing is empty, which is a question about this document's scripts alone.
           Step 3 (document_parse_end_step) has already made the readiness interactive. */
        if (document_readiness(realm) == 1 && !document_dcl_queued(realm) &&
            !engine_document_dcl_scripts_held(document_doc(realm))) {
            document_done_stage(realm, 0);
            DCHECK(document_dcl_queued(realm),
                   "a document's DOMContentLoaded stage ran and left its latch where it was — this walk "
                   "would then pick the same document for ever, queueing its listeners again every turn, which "
                   "is a live-lock the scheduler cannot tell from progress");
            did = 1;
        }
        JS_FreeValue(ctx, proxy);
    }
    for (i = n; i > 0 && !did; i--) {          /* pass two: `load`, innermost first */
        JSValue proxy = JS_GetPropertyUint32(ctx, docs, i - 1);
        JSContext *realm = window_proxy_realm(ctx, proxy);
        /* §13.2.7 step 8, the spin: it holds a parent whose child is not ready yet, which the reverse order alone
           cannot express. A `script` element's own delaying-the-load-event flag is not a source here: it is set
           only while the element is fetching, and a fetching script is on one of the lists step 5 or step 7
           waits for until it has run.
           Named residual. Not covered: `img` (§4.8.4.3.5 "Updating the image data": "When delay load event is true,
           fetching the image must delay the load event of element's node document") and the media elements'
           §4.8.11.5 "Loading the media resource" delaying-the-load-event flag. Next diff builds: each as a flag its
           own element component raises and this predicate reads. How its absence would show: a document reaching
           `load` before such an element finished. */
        /* Step 7, the spin over the two as-soon-as-possible lists, asked before step 8's. */
        if (document_readiness(realm) == 1 && document_dcl_queued(realm) &&
            !engine_document_load_scripts_held(document_doc(realm)) &&
            !document_load_event_delayed(ctx, docs, n, proxy)) {
            document_done_stage(realm, 1);
            DCHECK(document_readiness(realm) == 2,
                   "a document's `load` stage ran and left its readiness where it was — as above, the walk "
                   "would re-fire it every turn");
            did = 1;
        }
        JS_FreeValue(ctx, proxy);
    }
    JS_FreeValue(ctx, docs);
    return did;
}

/* §3.1.1's `location`: "the Location object of this's relevant global object", the one location.c built for this
   realm, not a second Location that would compare unequal to it (WPT's /common/PrefixedPostMessage.js names a
   message channel from `document.location.pathname`). It is asked of the component, not read off the global:
   `window.location` is §7.2.2's `[LegacyUnforgeable] readonly attribute Location`, an accessor, and a
   JS_GetPropertyStr reaching a getter aborts with no flow base under a C activation. A Location is a per-realm
   intrinsic, so a realm without one declined the component, which realm_value_get reports at the read. */
static JSValue js_doc_location(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    /* §3.1.1: null when this document is not fully active, and a document §4.5 created has no browsing context, so
       it never is. */
    Document *d = doc_receiver(ctx, this_val);

    if (!d) return JS_EXCEPTION;
    if (JS_IsUndefined(d->proxy))
        return JS_NULL;
    return location_object(ctx);
}


/* DOM §4.2.1 "Document tree"'s document element, through lexbor's one reading of the tree. NULL is a real
   answer: a document §4.5's createDocument built with no qualified name has none, and a page may remove it. */
lxb_dom_node_t *document_document_element_of(const lxb_dom_node_t *doc)
{
    if (!doc || doc->type != LXB_DOM_NODE_TYPE_DOCUMENT)
        return NULL;
    return lxb_dom_interface_node(lxb_dom_document_element(lxb_dom_interface_document((lxb_dom_node_t *)doc)));
}

/* §3.1.7's `body` as a lookup other components can make: HTML §6.6.6's `activeElement` ends in these two steps ("if
   candidate has a body element then return that body element; if candidate's document element is non-null then
   return that document element"), and a second walk there would be a second answer. */
lxb_dom_node_t *document_body_of(const lxb_dom_node_t *doc)
{
    if (!doc || doc->type != LXB_DOM_NODE_TYPE_DOCUMENT)
        return NULL;
    return lxb_dom_interface_node(lxb_html_document_body_element(lxb_html_interface_document((lxb_dom_node_t *)doc)));
}

/* HTML §7.4.6.4 "Scrolling to a fragment"'s target element (see document.h for why NULL here is the standard's own
   initial value rather than a stand-in, and why the name is not asked on the global). */
lxb_dom_node_t *document_target_element(JSContext *ctx)
{
    DCHECK(ctx != NULL,
           "HTML §7.4.6.4's TARGET ELEMENT was asked for with no realm — it is a field of ONE Document, and the "
           "realm is which document is being asked about; a caller with no realm has no document either");
    (void)ctx;
    return NULL;
}

/* DOM §2.7's default passive value names four targets, three of them this file's: the node document, its document
   element and its body. The fourth, the Window, is not a node and is answered where the registration is. */
bool document_is_passive_default_node(const lxb_dom_node_t *n)
{
    lxb_dom_node_t *doc;

    if (!n)
        return false;
    if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT)
        return true;
    doc = n->owner_document ? lxb_dom_interface_node(n->owner_document) : NULL;
    return n == document_document_element_of(doc) || n == document_body_of(doc);
}

/* HTML's "the document's relevant global object", which §2.9's get the parent puts above a Document in the event
   path. Borrowed, and JS_NULL for a document with no browsing context (one `createHTMLDocument` built), whose
   events stop at the document as the spec says. */
JSValueConst document_window_of(const lxb_dom_node_t *n)
{
    Document *d = n ? doc_rec(n->owner_document) : NULL;

    if (!d || !JS_IsObject(d->win_obj))
        return JS_NULL;
    return d->win_obj;
}

/* The tree entry points, each read from the receiver's tree as it is now through lexbor's one reading of it
   (DOM §4.5 "Interface Document", HTML §3.1.7 "DOM tree accessors"). Every Document in this engine is an
   lxb_html_document_t (node.c asserts it), so the HTML accessors apply to all of them.
   magic 0 = documentElement, 1 = body, 2 = head, 3 = doctype. */
static JSValue js_doc_tree(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *doc = node_of(this_val), *root;

    /* Web IDL §3.7.6 "Attributes"' brand check: a TypeError, not an assert (see doc_receiver). */
    if (!doc || doc->type != LXB_DOM_NODE_TYPE_DOCUMENT)
        return JS_ThrowTypeError(ctx, "this is not a Document");
    root = document_document_element_of(doc);
    switch (magic) {
    case 0:
        return node_wrap(ctx, root);
    case 1:
        /* §3.1.7: "the first of the html element's children that is either a body element or a frameset element, or
           null"; a frameset document has no body. */
        return node_wrap(ctx, document_body_of(doc));
    case 2:
        /* §3.1.7: "the first head element that is a child of the html element". */
        return node_wrap(ctx, lxb_dom_interface_node(lxb_html_document_head_element(lxb_html_interface_document(doc))));
    default:
        DCHECK(magic == 3, "a Document tree accessor was declared with a magic this table does not name");
        /* DOM §4.5's doctype: "the child of this that is a doctype; otherwise null". */
        return node_wrap(ctx, lxb_dom_interface_node(lxb_dom_document_doctype(lxb_dom_interface_document(doc))));
    }
}

/* §4.5 / §3.1.1's per-document strings, each a fact about the receiver, not the realm.
   magic 0 = URL and documentURI (§4.5 defines the second as an alias of the first), 1 = contentType,
   2 = compatMode, 3 = characterSet / charset / inputEncoding. */
static JSValue js_doc_strings(JSContext *ctx, JSValueConst this_val, int magic)
{
    Document *d = doc_receiver(ctx, this_val);

    if (!d) return JS_EXCEPTION;
    switch (magic) {
    /* §4.5: "The URL and documentURI getter steps are to return this's URL, serialized", so this member is the
       address and answers with the address value, whose example is that serialization (doc_addr_assert_agrees
       checks it at both writers). A page branching on `document.URL.includes("/admin")` over a computed route forks,
       as the same branch through `location.href` does. */
    case 0:
        DCHECK(!JS_IsUndefined(d->addr.value),
               "§4.5's `URL` was read off a record holding no address value — doc_rec_new writes one at the "
               "record's birth, so this is a record some other path built and the member would report a "
               "COMPUTED address as `undefined` rather than as the address it is");
        return JS_DupValue(ctx, d->addr.value);
    case 1:
        return JS_NewString(ctx, d->content_type);
    case 2:
        /* §4.5: "BackCompat" if the document is in quirks mode, "CSS1Compat" otherwise: the parser's answer, read off
           the tree it built. */
        return JS_NewString(ctx, d->dom->dom_document.compat_mode == LXB_DOM_DOCUMENT_CMODE_QUIRKS
                                     ? "BackCompat" : "CSS1Compat");
    default:
        DCHECK(magic == 3, "a Document string accessor was declared with a magic this table does not name");
        /* DOM §4.5 "Interface Document": "The characterSet, charset, and inputEncoding getter steps are to return
           this's encoding's name." The aliases are the same getter. The name is Encoding §4.2 "Names and labels"'
           `Name` column, mixed case ("UTF-8", "Shift_JIS", "ISO-8859-8-I"), not §7.1 "Interface mixin
           TextDecoderCommon"'s `encoding`, which is "this's encoding's name, ASCII lowercased"; `encoding_name` is
           this one and `encoding_name_ascii_lowercased` the other. The value is the record's, utf-8 "unless stated
           otherwise", which HTML §13.2.3.2 "Determining the character encoding" states through
           document_set_encoding (core/html/html_encoding_sniff.h). */
        return JS_NewString(ctx, encoding_name(d->encoding));
    }
}

/* §3.1.7 "DOM tree accessors"' `title` and §3.2.6.4 "The dir attribute"'s `dir`, neither a stored string. §3.1.7's
 * title getter is a walk of the tree as it is now (so `document.querySelector("title").textContent = "x"` and an
 * inserted `<title>` change it), per document, and its setter creates a title element and appends it to the head.
 * `dir` is §3.2.6.4's reflection of the html element's `dir` attribute limited to only known values, so the getter
 * filters: `<html dir=sideways>`, and a document with no html element, answer the empty string. */

/* The first element of `root`'s subtree in tree order whose namespace is `ns` and qualified name `name`: the lookup
   "the title element of a document is the first title element in the document" is. */
static lxb_dom_node_t *doc_first_in_tree(lxb_dom_node_t *root, lxb_ns_id_t ns, const char *name)
{
    lxb_dom_node_t *n;
    size_t nlen = strlen(name);

    for (n = root ? node_next_in(root, root) : NULL; n; n = node_next_in(n, root)) {
        size_t qn = 0;
        const lxb_char_t *q;
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || n->ns != ns) continue;
        q = lxb_dom_element_qualified_name(lxb_dom_interface_element(n), &qn);
        if (q && qn == nlen && !memcmp(q, name, qn)) return n;
    }
    return NULL;
}

/* The same in one generation: §3.1.7's SVG arm is "the first SVG title element that is a child of the document
   element", a different lookup, not a narrowing of the one above. */
static lxb_dom_node_t *doc_first_child_ns(lxb_dom_node_t *parent, lxb_ns_id_t ns, const char *name)
{
    lxb_dom_node_t *n;
    size_t nlen = strlen(name);

    for (n = parent ? parent->first_child : NULL; n; n = n->next) {
        size_t qn = 0;
        const lxb_char_t *q;
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || n->ns != ns) continue;
        q = lxb_dom_element_qualified_name(lxb_dom_interface_element(n), &qn);
        if (q && qn == nlen && !memcmp(q, name, qn)) return n;
    }
    return NULL;
}

/* §3.1.7's `title` getter's remaining steps: DOM §4.11 "Interface Text"'s child text content ("the concatenation of
   the data of all the Text node children of node, in tree order", children, not descendants) and Infra's strip and
   collapse ASCII whitespace (each run of TAB/LF/FF/CR/SPACE becomes one U+0020, leading and trailing ones dropped),
   into one buffer. Returns an owned JS string; `el` NULL is "or the empty string if the title element is null". */
static JSValue doc_child_text_stripped(JSContext *ctx, lxb_dom_node_t *el)
{
    lxb_dom_node_t *n;
    char *acc = NULL;
    size_t len = 0;
    bool pending_space = false, any = false;
    JSValue out;

    for (n = el ? el->first_child : NULL; n; n = n->next) {
        const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)n;
        size_t dn, i;
        const lxb_char_t *d;
        if (n->type != LXB_DOM_NODE_TYPE_TEXT) continue;
        d = cd->data.data;
        dn = cd->data.length;
        if (!d) continue;
        for (i = 0; i < dn; i++) {
            char c = (char)d[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r') {
                pending_space = any;      /* leading whitespace is dropped, not collapsed to a space */
                continue;
            }
            {
                char *grown = realloc(acc, len + 2);
                CHECK(grown != NULL, "document.title: OOM collapsing the title's text");
                acc = grown;
            }
            if (pending_space) { acc[len++] = ' '; pending_space = false; }
            acc[len++] = c;
            any = true;
        }
    }
    out = JS_NewStringLen(ctx, acc ? acc : "", len);
    free(acc);
    CHECK(!JS_IsException(out), "document.title: the title could not be allocated");
    return out;
}

/* magic 0 = title, 1 = dir, 2 = defaultView. */
enum { DOC_TITLE = 0, DOC_DIR, DOC_DEFAULT_VIEW };

static JSValue js_doc_html_members(JSContext *ctx, JSValueConst this_val, int magic)
{
    Document *d = doc_receiver(ctx, this_val);
    lxb_dom_node_t *doc, *de;

    if (!d) return JS_EXCEPTION;
    /* HTML §7.2.2's defaultView getter steps: "If this's browsing context is null, then return null. Return this's
       browsing context's WindowProxy object." A §4.5 Document has no browsing context, the same `proxy` absence
       `location` answers null for. */
    if (magic == DOC_DEFAULT_VIEW)
        return JS_IsUndefined(d->proxy) ? JS_NULL : JS_DupValue(ctx, d->proxy);

    doc = lxb_dom_interface_node(d->dom);
    de = document_document_element_of(doc);
    if (magic == DOC_DIR) {
        /* §3.2.6.4: the html element's `dir` limited to only known values: unknown or absent is the empty string,
           as is a document with no html element. Matched ASCII case-insensitively, as an enumerated attribute. */
        static const char *const KNOWN[3] = { "ltr", "rtl", "auto" };
        size_t vl = 0;
        const lxb_char_t *v;
        int i;

        if (!de || de->ns != LXB_NS_HTML) return JS_NewStringLen(ctx, "", 0);
        v = lxb_dom_element_get_attribute(lxb_dom_interface_element(de), (const lxb_char_t *)"dir", 3, &vl);
        for (i = 0; v && i < 3; i++) {
            size_t k, kl = strlen(KNOWN[i]);
            if (vl != kl) continue;
            for (k = 0; k < kl; k++) {
                char c = (char)v[k];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                if (c != KNOWN[i][k]) break;
            }
            if (k == kl) return JS_NewString(ctx, KNOWN[i]);
        }
        return JS_NewStringLen(ctx, "", 0);
    }
    DCHECK(magic == DOC_TITLE, "a Document HTML-member getter was declared with a magic this table does not name");
    /* §3.1.7 step 1: an SVG document element answers from the first SVG `title` that is a child of it. */
    if (de && de->ns == LXB_NS_SVG)
        return doc_child_text_stripped(ctx, doc_first_child_ns(de, LXB_NS_SVG, "title"));
    /* Step 2: otherwise the title element, the first `title` in the document in tree order. */
    return doc_child_text_stripped(ctx, doc_first_in_tree(doc, LXB_NS_HTML, "title"));
}

/* DOM §4.4's string replace all, where both §3.1.7 setter branches end: remove every child through the per-flow
   chokepoint, then append one Text node only if the string is not empty (a page reading `firstChild` can tell). */
static void doc_string_replace_all(JSContext *ctx, lxb_dom_node_t *el, const char *s, size_t len)
{
    lxb_dom_node_t *n, *next;

    (void)ctx;
    for (n = el->first_child; n; n = next) {
        next = n->next;
        dom_cow_remove_child(n);
    }
    if (len) {
        lxb_dom_text_t *t = lxb_dom_document_create_text_node(el->owner_document,
                                                              (const lxb_char_t *)s, len);
        CHECK(t != NULL, "document.title=: the title's Text node could not be allocated");
        dom_cow_note_created(lxb_dom_interface_node(t));
        dom_cow_append_child(el, lxb_dom_interface_node(t));
    }
}

/* §3.1.7's `title` setter and §3.2.6.4's `dir` setter: "the steps corresponding to the first matching condition", in
   the standard's order, ending with its "Otherwise: do nothing". magic 0 = title, 1 = dir. */
static JSValue js_doc_set_html_member(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    Document *d = doc_receiver(ctx, this_val);
    lxb_dom_node_t *doc, *de, *el;
    const char *s;
    size_t len = 0;

    if (!d) return JS_EXCEPTION;
    DCHECK(JS_IsString(val) || concolic_is(val),
           "a Document string setter reached its body unconverted — the IDL declaration is what converts it, "
           "and running the page's toString from here is the drive-to-completion the flow machinery avoids");
    doc = lxb_dom_interface_node(d->dom);
    de = document_document_element_of(doc);
    /* Unknown external input has no bytes: the shape is what the tree carries, as for `textContent =`, so a title
       set from attacker input reads back as its source. */
    if (concolic_is(val)) {
        s = concolic_shape_c(val);
        if (!s) s = "";
        len = strlen(s);
    } else {
        s = JS_ToCStringLen(ctx, &len, val);
        if (!s) return JS_EXCEPTION;
    }

    if (magic == DOC_DIR) {
        /* §3.2.6.4: "If there is no such element, then the attribute must return the empty string and do nothing on
           setting." The filter is the getter's, so the content attribute takes the string as written. */
        if (de && de->ns == LXB_NS_HTML)
            dom_cow_set_attribute(lxb_dom_interface_element(de), "dir", s, len, JS_UNDEFINED);
        goto done;
    }
    DCHECK(magic == DOC_TITLE, "a Document string setter was declared with a magic this table does not name");
    if (de && de->ns == LXB_NS_SVG) {
        /* The SVG arm: reuse the first SVG `title` child, or create one as the document element's first child, the
           standard's position (before the graphics a renderer reads it in front of). */
        el = doc_first_child_ns(de, LXB_NS_SVG, "title");
        if (!el) {
            lxb_dom_element_t *made = element_create_ns(lxb_dom_interface_document(d->dom),
                                                        "http://www.w3.org/2000/svg", 26, "title", 5, NULL, 0);
            el = lxb_dom_interface_node(made);
            dom_cow_note_created(el);
            if (de->first_child) dom_cow_insert_before(de->first_child, el);
            else dom_cow_append_child(de, el);
        }
        doc_string_replace_all(ctx, el, s, len);
        goto done;
    }
    if (de && de->ns == LXB_NS_HTML) {
        lxb_dom_node_t *head = lxb_dom_interface_node(lxb_html_document_head_element(lxb_html_interface_document(doc)));
        el = doc_first_in_tree(doc, LXB_NS_HTML, "title");
        /* "If the title element is null and the head element is null, then return": a `<frameset>` document, whose
           title is dropped. */
        if (!el && !head) goto done;
        if (!el) {
            /* The HTML namespace, named: this arm is guarded by `de->ns == LXB_NS_HTML`, so the created `title`
               belongs in that namespace. */
            lxb_dom_element_t *made = document_create_element_html(lxb_dom_interface_document(d->dom),
                                                                   "title", 5);
            el = lxb_dom_interface_node(made);
            dom_cow_note_created(el);
            dom_cow_append_child(head, el);
        }
        doc_string_replace_all(ctx, el, s, len);
    }
    /* "Otherwise: do nothing": a document element in neither namespace, or none. */
done:
    if (!concolic_is(val)) JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* §4.5's two traverser factories, `createNodeIterator(root, whatToShow, filter)` and `createTreeWalker(...)`,
   one body with a magic. Neither runs page code once its arguments are converted, so they are plain C and
   their members are machines. The IDL does the work: `Node root` is an interface type, `optional unsigned long
   whatToShow = 0xFFFFFFFF` is ToNumber and a modulo, and `optional NodeFilter? filter = null` accepts a function
   or any object.
   magic 0 = createNodeIterator, 1 = createTreeWalker. */
static JSValue js_doc_create_traverser(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                       int magic)
{
    JSValueConst root = argc > 0 ? argv[0] : JS_UNDEFINED, filter = JS_NULL;
    uint32_t what = 0xFFFFFFFFu;

    DCHECK(document_is(this_val), "a traverser factory ran on a receiver that is not a Document — the "
                                  "declaration states Web IDL §3.7 Interfaces' implementation check, so "
                                  "reaching the body means idl_implementation_check did not run for it");
    if (!node_of(root))
        return JS_ThrowTypeError(ctx, "createNodeIterator/createTreeWalker requires a Node root");
    /* The IDL's defaults: an absent optional argument arrives as undefined, which is §3.6's absence, so the default
       is applied here. */
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToUint32(ctx, &what, argv[1]) < 0)
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) filter = argv[2];
    return magic == 0 ? node_iterator_new(ctx, root, what, filter)
                      : tree_walker_new(ctx, root, what, filter);
}

/* §4.5 `[NewObject] Range createRange()`: "a new live range with (this, 0) as its start an end" (the standard's
   spelling). The construction `new Range()` performs, rooted in the receiver: `otherDoc.createRange()` gets a range
   in that document. */
static JSValue js_doc_create_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    (void)argc; (void)argv; (void)magic;
    DCHECK(document_is(this_val), "createRange ran on a receiver that is not a Document — the declaration "
                                  "states Web IDL §3.7 Interfaces' implementation check, so reaching the body "
                                  "means idl_implementation_check did not run for it");
    return range_new_at(ctx, this_val);
}

/* §4.5 `[NewObject] Event createEvent(DOMString interface)`, the legacy factory. The table and construction belong to
   the events component; the Document supplies the member and the realm whose interfaces the exposure check asks
   about, this document's global. */
static JSValue js_doc_create_event(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    JSValue global, r;
    const char *iface;

    (void)this_val; (void)argc; (void)magic;
    iface = JS_ToCString(ctx, argv[0]);   /* a real string by now: the declaration converted it */
    if (!iface) return JS_EXCEPTION;
    global = JS_GetGlobalObject(ctx);
    r = create_event(ctx, global, iface);
    JS_FreeValue(ctx, global);
    JS_FreeCString(ctx, iface);
    return r;
}

/* The Document methods, on Document.prototype, so there is one of each and `Document.prototype.querySelector`
   exists. The declarations are the agent's and the installs the realm's: the IDL pool is sealed after agent init,
   so a declaration minted from a per-realm install trips idl_declared_before_seal on the second realm. */
static JSClassID g_document_class;   /* §3.1.1's prototype slot, per realm */
/* DOM §4.5 "Interface Document"'s other interface: `[Exposed=Window] interface XMLDocument : Document {};`. A second
   prototype slot, not a second component: its whole content is which prototype a Document's wrapper wears (§4.5's
   "create a document that implements an interface"). A class id used purely as a per-realm prototype slot
   (JS_SetClassProto/JS_GetClassProto, as core/html/html_element.c does per HTML element interface): every node
   wrapper is core/dom/node.c's one node class, and only the prototype differs ("Return a new node that implements
   interface, in realm"). Per realm, since a member runs in the realm that defined it. */
static JSClassID g_xml_document_class;
/* The runtime this component declared into, which is also the declaration latch (as core/dom/element.c's
   `g_element_rt`): a second declaration in one process would re-mint the class every wrapper is branded with and
   re-declare members into a sealed pool. The release is checked against it, since JS_FreeValueRT reports nothing
   against the wrong runtime. */
static JSRuntime *g_document_rt;
static int g_id_create_element = -1, g_id_create_text = -1, g_id_create_comment = -1,
           g_id_create_fragment = -1, g_id_create_element_ns = -1, g_id_create_iterator = -1,
           g_id_create_walker = -1, g_id_create_range = -1, g_id_create_event = -1,
           g_id_create_cdata = -1, g_id_create_pi = -1, g_id_doc_ctor = -1, g_id_adopt_node = -1,
           g_id_title_set = -1, g_id_dir_set = -1, g_id_location_set = -1, g_id_by_element_name = -1, g_id_import_node = -1;
/* The four touch handlers core/html/html_element.c excludes, for the same reason: this interface includes the same
   `GlobalEventHandlers`, and Touch Events Level 2 says "this mixin must not be implemented". Stated here because
   idl_members_excluded reads the interface name as a literal and resolves its table per file. */
static const char *const TOUCH_EXCLUDED[] = { "ontouchstart", "ontouchend", "ontouchmove", "ontouchcancel" };


static void document_declare_members(JSContext *ctx)
{
    /* §4.5's `[CEReactions, NewObject] Element createElement(DOMString localName, optional (DOMString or
       ElementCreationOptions) options = {})`, a step because §4.9 step 5.1.4.1 constructs the page's custom element
       class synchronously inside it. The options are declared, not read by the body: §3.2.25's union and the
       dictionary's members are conversions, each a rest point, since a page accessor on `customElementRegistry` is
       its code. */
    {
        static const IdlArgType CREATE_EL[2] = { IDL_DOMSTRING, IDL_STRING_OR_DICT };
        /* §4.5's `dictionary ElementCreationOptions { CustomElementRegistry? customElementRegistry; DOMString is;
           }`, in the IDL's order, which is the order Web IDL reads them. The registry crosses unconverted (IDL_ANY)
           and is brand-tested by flatten_creation_options; the named residual there and core/dom/shadow_root.c's
           state the declaration that replaces it. */
        static const IdlDictMember ELEMENT_CREATION_OPTIONS[] = {
            { "customElementRegistry", IDL_ANY,       false, NULL, 0 },
            { "is",                    IDL_DOMSTRING, false, NULL, 0 },
        };
        g_id_create_element = idl_method_id_step(ctx, CREATE_EL, 2, ELEMENT_CREATION_OPTIONS,
                                                 (int)(sizeof ELEMENT_CREATION_OPTIONS /
                                                       sizeof ELEMENT_CREATION_OPTIONS[0]),
                                                 &DOC_CREATE_EL_STEP, 0);
        idl_optional_from(1);
    }
    g_id_create_text = idl_method_id(ctx, IDL_1STR, 1, js_doc_create_text, 0);
    g_id_create_comment = idl_method_id(ctx, IDL_1STR, 1, js_doc_create_comment, 0);
    g_id_create_cdata = idl_method_id_step(ctx, IDL_1STR, 1, NULL, 0, &XN_DECL, 0);
    g_id_create_pi = idl_method_id_step(ctx, IDL_2STR, 2, NULL, 0, &XN_DECL, 1);
    /* §4.5's `[Exposed=Window] interface Document { constructor(); }`: the interface object is constructible, and
       `new Document()` is how a page gets an XML document without DOMImplementation. */
    g_id_doc_ctor = idl_method_id(ctx, NULL, 0, js_doc_ctor, 0);
    g_id_create_fragment = idl_method_id(ctx, NULL, 0, js_doc_create_fragment, 0);
    idl_this_iface(document_is, "Document");
    /* HTML §3.1.7's `NodeList getElementsByName(DOMString elementName)`. The receiver's brand is declared, because
       Web IDL §3.7.7 "Operations" throws for a foreign receiver before converting the DOMString argument, and a body
       test would run the page's `toString` first. */
    g_id_by_element_name = idl_method_id(ctx, IDL_1STR, 1, js_doc_by_element_name, 0);
    idl_this_iface(document_is, "Document");
    g_id_create_element_ns = idl_method_id_step(ctx, IDL_NSSTR_STR, 2, NULL, 0, &CEN_DECL, 0);
    {
        /* §4.5: `(Node root, optional unsigned long whatToShow = 0xFFFFFFFF, optional NodeFilter? filter = null)`,
           twice. */
        static const IdlArgType TRAVERSER[3] = { IDL_INTERFACE, IDL_UNSIGNED_LONG,
                                                 IDL_CALLBACK_INTERFACE_NULLABLE };
        g_id_create_iterator = idl_method_id(ctx, TRAVERSER, 3, js_doc_create_traverser, 0);
        idl_iface_brand(node_class_id());
        idl_optional_from(1);
        idl_this_iface(document_is, "Document");
        g_id_create_walker = idl_method_id(ctx, TRAVERSER, 3, js_doc_create_traverser, 1);
        idl_iface_brand(node_class_id());
        idl_optional_from(1);
        idl_this_iface(document_is, "Document");
    }
    g_id_create_range = idl_method_id(ctx, NULL, 0, js_doc_create_range, 0);
    idl_this_iface(document_is, "Document");
    g_id_create_event = idl_method_id(ctx, IDL_1STR, 1, js_doc_create_event, 0);
    {
        /* §4.5's `[CEReactions, NewObject] Node importNode(Node node, optional (boolean or ImportNodeOptions) options
           = false)`, a step because §4.4's `clone a node` rests at every node. The union is the declaration's, and
           `= false` is stated with idl_arg_default because the default is the boolean arm: every other
           union-with-dictionary position in the platform's IDL defaults to `{}`, and without it
           `document.importNode(n)` would take the dictionary arm and copy deep (step 5.1 negates `selfOnly`). */
        static const IdlArgType IMPORT_NODE[2] = { IDL_INTERFACE, IDL_BOOL_OR_DICT };
        /* §4.5's `dictionary ImportNodeOptions { CustomElementRegistry customElementRegistry; boolean selfOnly =
           false; }`, in the IDL's order. The registry crosses unconverted and is brand-tested by doc_import_options,
           as for ElementCreationOptions; `selfOnly` is a declared boolean, so §3.2.17's member loop forks an unknown
           one and the body reads a real truth value. */
        static const IdlDictMember IMPORT_NODE_OPTIONS[] = {
            { "customElementRegistry", IDL_ANY,     false, NULL, 0 },
            { "selfOnly",              IDL_BOOLEAN, false, NULL, 0, NULL, IDL_DEFAULT_FALSE, NULL },
        };
        g_id_import_node = idl_method_id_step(ctx, IMPORT_NODE, 2, IMPORT_NODE_OPTIONS,
                                              (int)(sizeof IMPORT_NODE_OPTIONS /
                                                    sizeof IMPORT_NODE_OPTIONS[0]),
                                              &DOC_IMPORT_NODE_STEP, 0);
        idl_iface_brand(node_class_id());
        idl_optional_from(1);
        idl_arg_default(1, IDL_DEFAULT_FALSE, NULL);
        idl_this_iface(document_is, "Document");
    }
    {
        /* §4.5's `[CEReactions] Node adoptNode(Node node)`. The argument is an interface type, so a non-Node is a
           TypeError before step 1; `[CEReactions]` is the machine's epilogue, which drains the adoptedCallback
           reactions adopt enqueued before the value returns. */
        static const IdlArgType ONE_NODE[1] = { IDL_INTERFACE };
        g_id_adopt_node = idl_method_id(ctx, ONE_NODE, 1, js_doc_adopt_node, 0);
        idl_iface_brand(node_class_id());
    }
    /* §3.1.1's `[CEReactions] attribute DOMString title` and `[CEReactions] attribute DOMString dir`; the args
       machine's epilogue drains the reactions the element creation and insertions enqueue. */
    g_id_title_set = idl_setter_id(ctx, IDL_DOMSTRING, false, js_doc_set_html_member, DOC_TITLE);
    g_id_dir_set = idl_setter_id(ctx, IDL_DOMSTRING, false, js_doc_set_html_member, DOC_DIR);
    /* HTML §3.1.1 "The Document object"'s `[PutForwards=href, LegacyUnforgeable] readonly attribute Location?
       location`: the forwarding is Web IDL's, not location.c's, so `document.location = url` and `window.location
       = url` are one binding rule read off two IDL lines, the pair (`location`, `href`) as §7.2.2's. */
    g_id_location_set = idl_setter_id_put_forwards(ctx, "location", "href");
    /* Each pool entry is agent-lifetime state, declared beside the line that mints it (core/agent_state.h), so a
       release that gave the pool back and kept these numbers would be caught: the next agent's `_init` would read
       one as a latch. */
    agent_state_id("document", &g_id_create_element, "§4.5's createElement machine");
    agent_state_id("document", &g_id_create_text, "§4.5's createTextNode");
    agent_state_id("document", &g_id_create_comment, "§4.5's createComment");
    agent_state_id("document", &g_id_create_cdata, "§4.5's createCDATASection");
    agent_state_id("document", &g_id_create_pi, "§4.5's createProcessingInstruction");
    agent_state_id("document", &g_id_doc_ctor, "§4.5's `new Document()`");
    agent_state_id("document", &g_id_create_fragment, "§4.5's createDocumentFragment");
    agent_state_id("document", &g_id_by_element_name, "HTML §3.1.7's getElementsByName");
    agent_state_id("document", &g_id_create_element_ns, "§4.5's createElementNS");
    agent_state_id("document", &g_id_create_iterator, "§4.5's createNodeIterator");
    agent_state_id("document", &g_id_create_walker, "§4.5's createTreeWalker");
    agent_state_id("document", &g_id_create_range, "§4.5's createRange");
    agent_state_id("document", &g_id_create_event, "§4.5's createEvent");
    agent_state_id("document", &g_id_import_node, "§4.5's importNode machine");
    agent_state_id("document", &g_id_adopt_node, "§4.5's adoptNode");
    agent_state_id("document", &g_id_title_set, "§3.1.7's `title` setter");
    agent_state_id("document", &g_id_dir_set, "§3.2.6.4's `dir` setter");
    agent_state_id("document", &g_id_location_set, "§3.1.1's `location` [PutForwards=href] setter");
}

static void document_install_members(JSContext *ctx, JSValueConst proto)
{
    idl_install_method(ctx, proto, "createElement", g_id_create_element);
    idl_install_method(ctx, proto, "createTextNode", g_id_create_text);
    idl_install_method(ctx, proto, "createComment", g_id_create_comment);
    idl_install_method(ctx, proto, "createCDATASection", g_id_create_cdata);
    idl_install_method(ctx, proto, "createProcessingInstruction", g_id_create_pi);
    idl_install_method(ctx, proto, "createDocumentFragment", g_id_create_fragment);
    idl_install_method(ctx, proto, "getElementsByName", g_id_by_element_name);
    {
        /* §3.1.7's five element shortcuts, each a live HTMLCollection over the document. */
        static const char *const NAMES[] = { "forms", "images", "scripts", "embeds", "links" };
        unsigned k;
        for (k = 0; k < sizeof(NAMES) / sizeof(NAMES[0]); k++)
            idl_install_accessor(ctx, proto, NAMES[k], js_doc_shortcut, (int)k, -1);
    }
    idl_install_method(ctx, proto, "createElementNS", g_id_create_element_ns);
    /* §4.5's importNode and adoptNode, the two members stated over another document's node: an import copies
       (§4.4's `clone a node`, the source keeps its tree) and an adopt moves (§4.5's `adopt a node` rewrites the
       node's node document). importNode's registry is the named residual at its step 7. */
    idl_install_method(ctx, proto, "importNode", g_id_import_node);
    idl_install_method(ctx, proto, "adoptNode", g_id_adopt_node);
    /* §4.5's two attribute factories, declared beside the interface they build (attr.c): "create an attribute" is
       §4.9.2's algorithm and belongs to the attribute component. */
    attr_install_document_members(ctx, proto);
    idl_install_method(ctx, proto, "createNodeIterator", g_id_create_iterator);
    idl_install_method(ctx, proto, "createTreeWalker", g_id_create_walker);
    idl_install_method(ctx, proto, "createRange", g_id_create_range);
    idl_install_method(ctx, proto, "createEvent", g_id_create_event);
    /* §3.1.1 "The Document object"'s `[PutForwards=href, LegacyUnforgeable] readonly attribute Location? location`,
       with the forwarding half (Web IDL §3.7.6, as on §7.2.2's `location`), so `document.location = url` ends in the
       `href` setter's navigation.
       Named residual. Not covered: §3.4.10 [LegacyUnforgeable] puts the attribute on every implementing object,
       non-configurable; this is on the interface prototype object and configurable. Next diff builds: the
       per-instance unforgeable install for a Document, as idl_install_accessor_unforgeable does for §7.2.2's
       `location` on the global, with a hook at document creation (a Document is built per navigation). How its
       absence shows: `Object.getOwnPropertyDescriptor(document, "location")` answers undefined where a browser
       answers a non-configurable accessor, and `delete Document.prototype.location` succeeds. */
    idl_install_accessor(ctx, proto, "location", js_doc_location, 0, g_id_location_set);
    /* §3.1.1's tree entry points and §4.5's per-document strings, accessors because each is a lookup in the tree as
       it is and a fact about the receiver. */
    idl_install_accessor(ctx, proto, "documentElement", js_doc_tree, 0, -1);
    idl_install_accessor(ctx, proto, "body",           js_doc_tree, 1, -1);
    idl_install_accessor(ctx, proto, "head",           js_doc_tree, 2, -1);
    idl_install_accessor(ctx, proto, "doctype",        js_doc_tree, 3, -1);
    idl_install_accessor(ctx, proto, "URL",            js_doc_strings, 0, -1);
    idl_install_accessor(ctx, proto, "documentURI",    js_doc_strings, 0, -1);
    idl_install_accessor(ctx, proto, "contentType",    js_doc_strings, 1, -1);
    idl_install_accessor(ctx, proto, "compatMode",     js_doc_strings, 2, -1);
    idl_install_accessor(ctx, proto, "characterSet",   js_doc_strings, 3, -1);
    idl_install_accessor(ctx, proto, "charset",        js_doc_strings, 3, -1);
    idl_install_accessor(ctx, proto, "inputEncoding",  js_doc_strings, 3, -1);
    idl_install_accessor(ctx, proto, "implementation", js_doc_implementation, 0, -1);
    /* §3.1.7's `title`, §3.2.6.4's `dir` and §7.2.2's `defaultView`; the first two are read-write, and `title`'s
       setter creates a `<title>` element. */
    idl_install_accessor(ctx, proto, "title",       js_doc_html_members, DOC_TITLE, g_id_title_set);
    idl_install_accessor(ctx, proto, "dir",         js_doc_html_members, DOC_DIR,   g_id_dir_set);
    idl_install_accessor(ctx, proto, "defaultView", js_doc_html_members, DOC_DEFAULT_VIEW, -1);
    /* §3.1.4's resource metadata members and §3.1.5's `readyState`, their own component, because `cookie` and
       `referrer` are attacker sources with their own browser delivery and a cookie store behind them. */
    document_metadata_install(ctx, proto);
    /* §7.1.1.2's `domain`, the fifth member of that partial interface (see document_domain.h for why the
       origin-mutating one is separate from the metadata reader). */
    document_domain_install(ctx, proto);
    /* §8.4 "Dynamic markup insertion"'s `write`, `writeln` and `close`, which drive the parser rather than read the
       tree; `document.write` is a top-three DOM-XSS sink. */
    document_write_install(ctx, proto);
    /* CSSOM §6.2.3's `styleSheets`, one of the two members its `partial interface mixin DocumentOrShadowRoot` adds;
       ShadowRoot gets the same call from its own component, since a shadow tree's `<style>` is in that tree's
       collection.
       Named residual. Not covered: `adoptedStyleSheets`, an `ObservableArray<CSSStyleSheet>` of constructed sheets
       (CSSOM §6.1's constructed CSSStyleSheet is built in core/css/css_style_sheet.c). Next diff builds: Web IDL's
       observable array type (a backing list with set-an-indexed-value and delete-an-indexed-value algorithms) and
       the steps refusing a sheet whose constructor document is not this one. How its absence shows:
       `document.adoptedStyleSheets` is undefined. */
    style_sheet_list_install_mixin(ctx, proto);
    /* DOM §4.2.5 "Mixin DocumentOrShadowRoot"'s own member, `readonly attribute CustomElementRegistry?
       customElementRegistry;`, whose steps reach the same field for either host ("If this is a document, then return
       this's custom element registry. Assert: this is a ShadowRoot node. Return this's custom element registry."),
       so one getter in core/html/custom_elements.c serves both. */
    custom_elements_install_document_or_shadow_root_member(ctx, proto);
}

/* HTML §7.1.7 "Policy containers"' container for this document, from both halves of the policy list. `csp` is what
   the document was created with: the response's `Content-Security-Policy` header, or §7.4's clone of the creator's
   for a document from no response. The `<meta>` half is a Lexbor walk asking core/html/html_meta_csp.h, per node,
   HTML §4.2.5.3 "Pragma directives"' content security policy state, which refuses a `meta` outside `head` (step 1)
   and removes `report-uri`, `frame-ancestors` and `sandbox` (step 4), since §@S measures every breakout against the
   policy a browser would apply.
   This batch is an inversion: §7.5.1 "Shared document creation infrastructure" creates the Document and §7.5.2
   "Loading HTML documents" feeds a parser into it, so step 5's "enforce the policy" appends to an existing list;
   here the record is built after the parse, so the parser's `<meta>`s are appended as one batch. A `<meta>` a
   script inserts later goes through DOM §4.2.3's insertion steps to document_meta_csp_inserted below, which performs
   step 5 as CSP §2.2's append. `csp_report_only` is the response's `Content-Security-Policy-Report-Only`
   (or the creator's clone of it), which no `<meta>` adds to. */
PolicyContainer *document_policy_new(lxb_html_document_t *dom, const char *csp, const char *csp_report_only,
                                     const Origin *self_origin, SerializedEmbedderPolicy embedder,
                                     const char *integrity_policy)
{
    lxb_dom_node_t *cur;
    char *acc = NULL;
    size_t acc_len = 0;

    /* The created-with policies first, since they were delivered first; every policy in a list is enforced, so the
       order changes no verdict, but the container reports its text in the order received. */
    if (csp && *csp) {
        acc_len = strlen(csp);
        acc = malloc(acc_len + 1);
        CHECK(acc != NULL, "document: OOM holding the policy this document was created with");
        memcpy(acc, csp, acc_len + 1);
    }

    /* No guard for a missing tree: document_install asserted there is one. Every node is asked and none is filtered
       here, because §4.2.5.3's selection (a `meta`, in the content security policy state, a child of `head`) is the
       algorithm's own first acts. */
    for (cur = lxb_dom_interface_node(dom)->first_child; cur; ) {
        char *delivered = html_meta_csp_policy(cur);

        if (delivered) {
            /* §4.2.5.3 step 5, "enforce the policy policy", an append to this serialized list. Several meta policies
               all apply, joined with a comma, CSP §2.2's serialization of a policy list; a `;` join would make them
               one policy, in which a repeated directive is ignored. */
            size_t dl = strlen(delivered);
            size_t add = dl + (acc_len ? 1 : 0);
            char *g = realloc(acc, acc_len + add + 1);
            CHECK(g != NULL, "document: OOM collecting a policy");
            acc = g;
            if (acc_len) acc[acc_len++] = ',';
            memcpy(acc + acc_len, delivered, dl);
            acc_len += dl;
            acc[acc_len] = 0;
            free(delivered);
        }
        if (cur->first_child) { cur = cur->first_child; continue; }
        while (cur && !cur->next) cur = cur->parent;
        if (cur) cur = cur->next;
    }
    /* The `sandbox` invariant §7.1.5 rests on: CSP §3.3 "The <meta> element" makes `sandbox` meaningless there, and
       §4.2.5.3 step 4 removes it, so merging the meta policies cannot change §7.1.5's CSP-derived sandboxing flags;
       this asserts it over the whole merged list. */
    DCHECK(!acc || policy_csp_derived_sandboxing_flags(acc, acc_len) ==
                   (csp && *csp ? policy_csp_derived_sandboxing_flags(csp, strlen(csp)) : 0),
           "merging this document's `<meta>` policies changed §7.1.5's CSP-derived sandboxing flags — only a "
           "`sandbox` directive can do that and HTML §4.2.5.3 \"Pragma directives\" step 4 removes it from "
           "every meta-delivered policy, so a difference here is a policy that reached this list without "
           "going through that step");
    {
        /* The merged list takes one self-origin, the created-with policy's: CSP §3.3 delivers a meta policy inside
           the response this document came from, so §2.2.2's "response's URL's origin" is the same for both halves.
           §7.1.4's item passes through untouched, since a `<meta http-equiv>` delivers only a CSP policy. */
        PolicyContainer *p = policy_container_new(acc, csp_report_only, self_origin, NULL, embedder,
                                                  integrity_policy);
        free(acc);
        return p;
    }
}


const PolicyContainer *document_policy(JSContext *ctx)
{
    Document *d = doc_of(ctx);

    return d ? d->policy : worker_env_of(ctx)->policy;   /* a worker realm's is its environment record's */
}

/* The same container as a fact about one document: CSP §6.3.1.1 step 1 states it that way ("let CSP list be
   document's global object's csp list"), and §4.2.3's freeze runs for elements whose node document is often not the
   realm's active one (a DOMParser tree). NULL for a document with no browsing context, the same answer as a list of
   zero policies. */
const PolicyContainer *document_policy_of(const lxb_dom_document_t *dom)
{
    const Document *d = doc_rec(dom);

    DCHECK(d != NULL, "a document's policy container was asked of a tree with no record");
    return d->policy;
}

/* HTML §4.2.5.3 "Pragma directives"' content security policy state for an element a flow inserted: the second caller
 * of html_meta_csp, beside document_policy_new's parse-time batch. §4.2.5.3 runs at insertion ("At the time of
 * inserting the meta element to the document, it is possible that some resources have already been fetched. For
 * example, images might be stored in the list of available images prior to dynamically inserting a meta element with
 * an http-equiv attribute in the Content security policy state."), so a policy a script inserts is enforced, and
 * the Document is not judged under a more permissive list than the real page has.
 * The element's own node document is the one enforced on ("upon the current document", which DOM §4.4 makes the
 * node document), never the running realm's active one: a script inserting a CSP `<meta>` into a
 * `createHTMLDocument`, DOMParser or `responseXML` tree says nothing about the page. `el` is every inserted element,
 * since §4.2.5.3's pragma selection is core/html/html_meta_csp.h's. */
void document_meta_csp_inserted(lxb_dom_element_t *el)
{
    const lxb_dom_node_t *n;
    Document *d;
    char *delivered;

    DCHECK(el != NULL, "§4.2.5.3's content security policy state ran for no element — DOM §4.2.3's insertion "
                       "steps are given a node, so an absent one is a walk that reached past its own tree");
    n = lxb_dom_interface_node(el);
    d = doc_rec(n->owner_document);
    DCHECK(d != NULL,
           "§4.2.5.3's content security policy state ran in a document with no record — the insertion seam has "
           "already asserted a realm for this node's document, and a realm is a record, so an absence here is "
           "a node inserted into a tree nothing owns");
    /* Steps 1-4 and the pragma selection. NULL for every node that delivers no policy, and for a `<meta>` whose whole
       content step 4 removed. */
    delivered = html_meta_csp_policy(n);
    if (!delivered)
        return;
    if (!d->policy) {
        /* A document with no §7.1.7 container, a positive statement: document_install builds one for every Document
           with a browsing context, while document_new's family (DOMParser, createHTMLDocument, responseXML) gets
           none and document_policy_of answers NULL ("allowed") for it, so there is no list for step 5 to append to.
           The DCHECK separates that family from a realm's active document created without document_install. */
        DCHECK(document_active_realm_of((const lxb_dom_node_t *)n->owner_document) == NULL,
               "a `<meta>` delivered a policy to a Document that is a realm's ACTIVE document and holds no "
               "§7.1.7 policy container — every such Document gets one at document_install, so this is a "
               "creation path that did not run it, and the policy the page just installed would be enforced "
               "nowhere while every breakout was measured against a list that does not exist");
        free(delivered);
        return;
    }
    /* Step 5, "Enforce the policy policy": CSP §2.2's list grows by one, which the container asserts at its origin
       (core/frame/policy_container.h). */
    policy_container_enforce_policy(d->policy, delivered);
    free(delivered);
}

void document_meta_referrer_changed(lxb_dom_element_t *el)
{
    const lxb_dom_node_t *n;
    Document *d;
    const char *policy;

    DCHECK(el != NULL, "§4.2.5.1's referrer steps ran for no element");
    n = lxb_dom_interface_node(el);
    policy = html_meta_referrer_policy(n);
    if (!policy) return;
    d = doc_rec(n->owner_document);
    DCHECK(d != NULL, "§4.2.5.1's referrer steps ran in a document with no record");
    /* A Document of the second family (DOMParser, createHTMLDocument) has no container to set, as for
       document_meta_csp_inserted; a realm's active document always has one. */
    if (!d->policy) {
        DCHECK(document_active_realm_of((const lxb_dom_node_t *)n->owner_document) == NULL,
               "a `<meta name=referrer>` reached a Document that is a realm's ACTIVE document and holds no "
               "§7.1.7 policy container — document_install builds one for every such Document");
        return;
    }
    /* Step 6: "set element's node document's policy container's referrer policy to policy". */
    policy_container_set_referrer_policy(d->policy, policy);
}

SandboxFlags document_active_sandbox_flags(JSContext *ctx) { return doc_here(ctx)->sandbox_flags; }

const PermissionsPolicy *document_permissions_policy(JSContext *ctx) { return doc_here(ctx)->permissions_policy; }

/* HTML §3.1.3 "Ancestor origins"' internal ancestor origin objects list creation steps, "given a `Document` object
 * document and a referrer policy referrerPolicy". §7.3.2.1 "Creating browsing contexts" is the one caller, and runs
 * them with the ancestor origins list creation steps. They run at creation only: §7.2.4's `ancestorOrigins` is a
 * snapshot of the tree the Document was created in. The three inputs are passed, not derived from the child's
 * navigable: §7.3.1.3's create holds all three in the creator's heap (the parent navigable, the container element,
 * the child's origin), so for a child built in a peer instance the creator runs these steps and the result crosses,
 * as Permissions Policy §9.5's does.
 * `masked` follows step 8: "no-referrer" masks unconditionally, "same-origin" when the parent is cross-origin with
 * this document. Masking appends a new opaque origin rather than dropping one, so the length reports the frame
 * depth (the corpus asserts `['null']`, not `[]`). The policy is HTML §7.1.6 "iframe element referrer policy"'s
 * answer, the `referrerpolicy` attribute's state keyword (an enumerated attribute, ASCII case-insensitive, invalid
 * value default the empty string) for an `iframe` and the empty string otherwise, owned by core/html/html_iframe.c.
 * Step 12.1's "ancestorOrigin is same origin with parentDoc's origin" is false for two opaque origins though both
 * serialize to `null`, so the text comparison is admitted only once the parent's origin is a tuple. */
/* Named residual: the referrer policy is read off the container element at this moment. Not covered: a Document a
 * navigation produces should use §7.4.2.1 "Supporting concepts"' target snapshot params, "snapshotted at the
 * beginning of a navigation and used throughout the navigation's lifetime", whose two items are the sandboxing flags
 * and the iframe element referrer policy, taken at §7.4.2.2 "Beginning navigation" step 17; so a page that changes
 * `referrerpolicy` while the response is in flight is answered with the new value (the corpus observes an ancestor
 * origin coming back unmasked). The initial about:blank, which no navigation precedes, is correct. The sandboxing
 * half is a latch on the WindowProxy taken at navigable creation (`window_proxy_creation_sandbox_flags`), a
 * different instant whose observability for `sandbox` is not established. Next diff builds: target snapshot params
 * as a record taken at step 17 with both items, the sandboxing latch moved onto it, and this function taking
 * the policy as an argument, as §7.3.2.1 passes `iframeReferrerPolicy`. How its absence shows: the corpus failure
 * above. */
static JSValue doc_compose_ancestor_origins(JSContext *ctx, JSValueConst parent_nav, JSValueConst container,
                                            const Origin *child_origin)
{
    JSValue out = JS_NewArray(ctx);
    uint32_t n = 0, i, nout = 0;
    JSContext *prealm;
    Document *pd;
    const Origin *porigin;
    const char *pser;
    bool masked = false, popaque;
    const char *rp;   /* §7.1.6's keyword, borrowed from its own table */
    JSValue len;

    CHECK(!JS_IsException(out), "§3.1.3's ancestor origins list could not be allocated");
    DCHECK(window_proxy_is(parent_nav) && !window_proxy_is_remote(parent_nav),
           "§3.1.3's internal ancestor origin objects list creation steps were run against a parent navigable "
           "this instance does not hold — step 5 reads the parent Document's own list and steps 9, 11 and 12.1 read "
           "its ORIGIN RECORD, and neither exists in this heap for a peer's navigable. The composition belongs in "
           "the instance that holds the ancestors and its RESULT crosses as text");
    /* Step 5: "let ancestorOrigins be parentDoc's internal ancestor origin objects list." */
    prealm = window_proxy_realm(ctx, parent_nav);
    pd = doc_here(prealm);
    DCHECK(pd != NULL && JS_IsArray(pd->ancestor_origin_strings),
           "§3.1.3's step 5 read a parent Document that has no INTERNAL ancestor origin objects list — every "
           "Document of this agent is given one by this same function at its creation, so a parent without one "
           "was installed past it and this child's list would silently start from nothing");
    porigin = window_proxy_origin(pd->proxy);
    DCHECK(porigin != NULL, "§3.1.3's step 11 needs the parent Document's origin and its navigable has none");
    popaque = origin_is_opaque(porigin);
    pser = origin_serialized(porigin);
    /* Steps 6-9: the container element, `masked`, and the referrer policy's two masking arms. §7.1.6's keyword is
       canonical, so the comparisons below are exact; the case-insensitivity lives once, inside §2.3.3's match. */
    rp = iframe_element_referrer_policy(container);
    DCHECK(rp != NULL, "§7.1.6 answered with no keyword — its two arms both return one (a state's corresponding "
                       "keyword, or the empty string), so a NULL is an arm that returned nothing at all and "
                       "steps 8-9 would read it as 'no policy' rather than crashing");
    if (!strcmp(rp, "no-referrer")) {
        masked = true;                                                              /* step 8 */
    } else if (!strcmp(rp, "same-origin")) {                                        /* step 9 */
        DCHECK(child_origin != NULL,
               "§3.1.3's step 9 compares this Document's origin against its parent's and the navigable being "
               "created has none — every navigable is created with the origin its Document will have, so an "
               "absent one is a create that had not computed it yet rather than a Document without one");
        masked = !origin_same(porigin, child_origin);
    }
    /* Steps 10-11: "if masked is true, then append a new opaque origin to output"; "otherwise, append parentDoc's
       origin to output." Exactly one runs, so one statement. */
    JS_SetPropertyUint32(ctx, out, nout++,
                         JS_NewString(ctx, masked ? "null" : (pser ? pser : "null")));
    /* Step 12: the parent's own ancestors, each masked when it is the parent's origin again (step 12.1). */
    len = JS_GetPropertyStr(ctx, pd->ancestor_origin_strings, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++) {
        JSValue a = JS_GetPropertyUint32(ctx, pd->ancestor_origin_strings, i);
        const char *s = JS_ToCString(ctx, a);

        if (masked && !popaque && s && pser && !strcmp(s, pser)) {
            JS_SetPropertyUint32(ctx, out, nout++, JS_NewString(ctx, "null"));
        } else {
            /* "Append ancestorOrigin to output and set masked to false." Masking stops at the first ancestor that is
               not the parent's origin; the standard's note says the further ones "might have been previously masked
               when an ancestor document ran these steps", and this list holds what the parent's list recorded. Only
               a frame three deep shows the difference. */
            JS_SetPropertyUint32(ctx, out, nout++, JS_DupValue(ctx, a));
            masked = false;
        }
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, a);
    }
    /* Steps 10-11 appended one before the loop, so a list for a navigable with a parent is never empty, the invariant
       a record crossing an instance boundary is checked against at both ends (empty means no parent). */
    DCHECK(nout >= 1, "§3.1.3's steps returned an EMPTY list for a navigable that has a parent — steps 10-11 "
                      "append unconditionally (exactly one of the two runs), so an empty output is these steps having skipped it and the "
                      "child would report itself top-level");
    return out;
}

/* §3.1.3's two lists for the document being installed, from whichever of three states its navigable is in. The third
 * is a navigable whose parent is in another WASM instance: §Security makes a cross-instance read a suspend point, and
 * an install has no flow base to take one, so the answer is the one the creating instance composed and put on the
 * provisioning record. It is read back rather than recomputed, as for document_create_permissions_policy; step
 * 12.1's comparison needs the parent's origin record, which a serialization cannot carry. */
static void doc_create_ancestor_origins(JSContext *ctx, Document *d, JSValueConst nav_proxy)
{
    JSValue parent_nav, container, out;

    /* A document with no browsing context has neither list: §7.2.4's first step finds no relevant Document and
       returns the Location's own empty list. UNDEFINED says so; an empty list would claim this Document is at the
       top of a tree. */
    if (!window_proxy_is(nav_proxy)) return;
    /* Steps 2-3: "let parentDoc be document's container document; if parentDoc is null, then return output." A
       top-level traversable has no container, and its list is empty. */
    parent_nav = window_proxy_parent_navigable(ctx, nav_proxy);
    if (!window_proxy_is(parent_nav)) {
        out = JS_NewArray(ctx);
        CHECK(!JS_IsException(out), "§3.1.3's ancestor origins list could not be allocated");
    } else if (window_proxy_is_remote(parent_nav)) {
        const char *carried = window_proxy_remote_ancestor_origins(nav_proxy);

        DCHECK(carried != NULL,
               "§3.1.3's internal ancestor origin objects list is being created for a navigable whose PARENT "
               "is in another WASM instance, and the record that provisioned this instance stated NOTHING "
               "about it — the ancestors live in that peer, so its create ran these steps and the finished "
               "list is what crosses. Reading the silence as an empty list would make this Document report "
               "itself TOP-LEVEL to every `location.ancestorOrigins` read, which is a different fact and one "
               "no page can tell from the truth. The statement exists (core/frame/navigable.h's "
               "navigable_root_ancestor_origins, beside navigable_root_container) and this host did not make "
               "it: carry the notice's ancestor-origins field to it");
        DCHECK(document_ancestor_origins_serialized_has_ancestors(carried),
               "a navigable whose §7.3.1.3 PARENT is in another WASM instance was provisioned with §3.1.3's "
               "EMPTY list — the section's step 3 returns an empty output only when there is no container "
               "document, and §7.3.1.3 makes a navigable with a parent a CHILD navigable that has one. The "
               "two statements on that record contradict each other and one of them is about a different "
               "navigable");
        out = document_ancestor_origins_deserialize(ctx, carried);
    } else {
        container = window_proxy_container(ctx, nav_proxy);
        out = doc_compose_ancestor_origins(ctx, parent_nav, container, window_proxy_origin(nav_proxy));
        JS_FreeValue(ctx, container);
    }
    JS_FreeValue(ctx, parent_nav);
    d->ancestor_origin_strings = out;
    /* The ancestor origins list creation steps, "append the serialization of origin to output" over the list above,
       which already holds serializations (see its field). */
    d->ancestor_origins = dom_string_list_new(ctx, JS_DupValue(ctx, out));
}

/* §7.2.4's `ancestorOrigins` answers with this: the list built at this Document's creation, [SameObject]. */
JSValue document_ancestor_origins(JSContext *ctx)
{
    Document *d = doc_here(ctx);

    DCHECK(!JS_IsUndefined(d->ancestor_origins),
           "§7.2.4's ancestorOrigins was asked of a Document with no ANCESTOR ORIGINS LIST — §7.3.2.1 gives "
           "one to every Document it creates, so a Document without one reached its realm by some other route "
           "and its ancestry was never recorded");
    return JS_DupValue(ctx, d->ancestor_origins);
}

/* §3.1.3's internal list in the form it crosses an instance boundary (see document.h for the grammar and why a space
 * is the separator). */
char *document_ancestor_origins_serialize(JSContext *ctx, JSValueConst list)
{
    uint32_t n = 0, i;
    size_t cap = 1;
    char *out;
    JSValue len;

    DCHECK(JS_IsArray(list),
           "§3.1.3's internal ancestor origin objects list was serialized for a peer instance from something "
           "that is not the list — every Document of this agent holds one as an Array of serialized origins "
           "and there is nothing else this record can be made of");
    len = JS_GetPropertyStr(ctx, list, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    /* The empty list is a word, not an empty field, as for §9.5's answer beside it: a host that stopped writing the
       field and a navigable with no ancestors are different facts, and only the first is a bug. */
    if (n == 0) {
        out = strdup(DOCUMENT_ANCESTOR_ORIGINS_SERIALIZED_NONE);
        CHECK(out != NULL, "document: OOM stating §3.1.3's empty ancestor list to a peer instance");
        return out;
    }
    for (i = 0; i < n; i++) {
        JSValue a = JS_GetPropertyUint32(ctx, list, i);
        const char *s = JS_ToCString(ctx, a);

        CHECK(s != NULL, "document: OOM reading §3.1.3's list to serialize it for a peer instance");
        cap += strlen(s) + 1;
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, a);
    }
    out = malloc(cap);
    CHECK(out != NULL, "document: OOM serializing §3.1.3's ancestor list for a peer instance");
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        JSValue a = JS_GetPropertyUint32(ctx, list, i);
        const char *s = JS_ToCString(ctx, a);

        CHECK(s != NULL, "document: OOM reading §3.1.3's list to serialize it for a peer instance");
        /* The separator cannot occur in an entry: §7.1.1's serialization of an origin is `null` or scheme, "://",
           host and an optional ":" and port, a scheme is ASCII alphanumeric with "+", "-" and ".", a port is
           digits, and URL §3.2 "Host miscellaneous" makes U+0020 a forbidden host code point. Asserted anyway,
           because an entry carrying a space would reach the peer as two ancestors and nothing else would notice. */
        DCHECK(strchr(s, ' ') == NULL,
               "§7.1.1's serialization of an origin contains a SPACE and this record is space-separated — URL "
               "§3.2 \"Host miscellaneous\" makes SPACE a forbidden host code point, so a byte that reached "
               "here came from an origin record built somewhere that did not parse its host, and the peer "
               "would read one ancestor as two");
        DCHECK(*s != '\0' && strcmp(s, DOCUMENT_ANCESTOR_ORIGINS_SERIALIZED_NONE) != 0,
               "§3.1.3's list holds an entry that is empty or is this grammar's word for the EMPTY LIST — "
               "every entry is §7.1.1's serialization of an origin, which is `null` or a scheme-and-host, so "
               "an entry spelling the absence would make a one-ancestor list unreadable from a no-ancestor one");
        if (i != 0) strcat(out, " ");
        strcat(out, s);
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, a);
    }
    return out;
}

bool document_ancestor_origins_serialized_has_ancestors(const char *text)
{
    /* A CHECK, as for the deserializer, and as permissions_policy.c judges the field beside it: this is a field's
       presence on a record that crossed an instance boundary from an untrusted instance (SECURITY.md), and the only
       guard before a `strcmp` on NULL in release. */
    CHECK(text != NULL && *text != '\0',
          "§3.1.3's internal ancestor origin objects list was read off a record that states NOTHING for it — "
          "an empty list and a list of ancestors are both answers a provisioning record makes, so an absent "
          "field is a host that stopped writing one, and reading it as the empty list tells a cross-origin "
          "frame it is the top of its own tree");
    return strcmp(text, DOCUMENT_ANCESTOR_ORIGINS_SERIALIZED_NONE) != 0;
}

JSValue document_ancestor_origins_deserialize(JSContext *ctx, const char *text)
{
    JSValue out;
    uint32_t nout = 0;
    const char *p;

    /* Every refusal here is a CHECK, as in permissions_policy_deserialize: the bytes come from another instance, so a
       malformed record is check.h's security-boundary case, and a DCHECK would be compiled out where it matters. */
    CHECK(document_ancestor_origins_serialized_has_ancestors(text),
          "§3.1.3's list was rebuilt from a record that says there are NO ancestors — the empty list is a "
          "decision the READER of this record takes (a navigable with no parent has no container document, so "
          "step 3 returns an empty output), not a list to build, and a caller that reached here has not asked "
          "whether its navigable has a parent at all");
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "§3.1.3's ancestor origins list could not be allocated");
    for (p = text; *p != '\0'; ) {
        const char *end = strchr(p, ' ');
        size_t n = end ? (size_t)(end - p) : strlen(p);

        CHECK(n > 0,
              "§3.1.3's list arrived from a peer instance with an EMPTY entry — every entry is §7.1.1's "
              "serialization of an origin, which is never the empty string, so a record holding one has a "
              "separator where an origin should be and every ancestor after it is at the wrong depth");
        JS_SetPropertyUint32(ctx, out, nout++, JS_NewStringLen(ctx, p, n));
        p = end ? end + 1 : p + n;
    }
    /* The composer's invariant read from the other end: a record saying there are ancestors that yields none is a
       grammar whose ends came apart, and would make a frame silently report itself top-level. */
    CHECK(nout >= 1,
          "§3.1.3's list arrived from a peer instance stating that there ARE ancestors and parsed to none — "
          "the two ends of this record disagree about its grammar, and the answer it would install is the "
          "one an absent field would have given");
    return out;
}

/* §3.1.3's list for a child navigable whose document will be built in a peer instance (see document.h). */
char *document_ancestor_origins_for_child(JSContext *ctx, JSValueConst container, const Origin *child_origin)
{
    /* Borrowed (see document.h). §7.3.1.3's "let parentNavigable be element's node navigable" runs in the embedder's
       realm, this one, so this is that navigable. */
    JSValueConst parent_nav = document_window_proxy(ctx);
    JSValue list;
    char *text;

    DCHECK(JS_IsObject(container),
           "§3.1.3's list was composed for a cross-instance child with no CONTAINER ELEMENT — §7.3.1.3's "
           "create-a-new-child-navigable is handed one and step 6 reads it, and an AUXILIARY navigable has no "
           "container document at all so its list is the EMPTY one and never this call's answer");
    list = doc_compose_ancestor_origins(ctx, parent_nav, container, child_origin);
    text = document_ancestor_origins_serialize(ctx, list);
    JS_FreeValue(ctx, list);
    return text;
}

/* Permissions Policy §7.2 "The permissionsPolicy object"'s declared origin of an element: "the origin of the document
 * which the embedding page intends to load into a frame", §9.4 step 2's third argument, which the `allow`
 * attribute's `'src'` keyword resolves to. An intention, computed from the element: §4.7 step 3 checks the created
 * Document's own origin against it, so a frame that redirected cross-origin does not get the delegation. Each opaque
 * arm mints a fresh origin (§7.1.1: same origin-domain only with itself, and §7.3.2.1 gives the sandboxed Document
 * its own), so a `'src'` allowlist on a sandboxed frame matches nothing.
 *   1. "If node's node document's sandboxed origin browsing context flag is set, then return a new opaque
 *      origin."  §7.1.5's flag on the container document's active sandboxing flag set.
 *   2. "If node's `sandbox` attribute is set, and does not contain the `allow-same-origin` keyword, then
 *      return a new opaque origin."  Asked through §7.1.5's own parse: `sandbox_parse_directive` sets the
 *      sandboxed origin flag for exactly the token lists lacking that keyword.
 *   3. "If node's `srcdoc` attribute is set, then return node's node document's origin."
 *   4. "If node's `src` attribute is set: let url be the result of parsing node's `src` attribute, relative to
 *      node's node document. If url is not failure, return url's origin."  An unparseable `src` falls to step 5.
 *   5. "Return node's node document's origin." */
static const Origin *doc_declared_origin(lxb_dom_node_t *el, SandboxFlags document_flags,
                                         const Origin *document_origin)
{
    lxb_dom_element_t *e = (lxb_dom_element_t *)el;
    const lxb_char_t  *v;
    size_t             n = 0;
    UrlRecord          url, base;
    const char        *base_url;
    bool               have_base;
    const Origin      *o;

    /* "Is set" is a presence test at all three attributes: Lexbor's get_attribute answers NULL for a present empty
       value, which would read `<iframe sandbox>` (the most restrictive spelling) as unsandboxed and `<iframe
       srcdoc>` as loading its `src`, as core/html/html_iframe.c also distinguishes. */
    if (document_flags & SANDBOX_ORIGIN)                                    /* step 1 */
        return origin_opaque_new();
    if (lxb_dom_element_has_attribute(e, (const lxb_char_t *)"sandbox", 7)) {
        v = lxb_dom_element_get_attribute(e, (const lxb_char_t *)"sandbox", 7, &n);
        if (sandbox_parse_directive(v ? (const char *)v : "", v ? n : 0) & SANDBOX_ORIGIN)   /* step 2 */
            return origin_opaque_new();
    }
    if (lxb_dom_element_has_attribute(e, (const lxb_char_t *)"srcdoc", 6))
        return document_origin;                                             /* step 3 */
    if (!lxb_dom_element_has_attribute(e, (const lxb_char_t *)"src", 3))
        return document_origin;                                             /* step 5 */
    n = 0;
    v = lxb_dom_element_get_attribute(e, (const lxb_char_t *)"src", 3, &n);
    /* `<iframe src="">` is parsed: the empty string resolves to the base URL, which a `<base href>` can move off the
       document's origin, so steps 4 and 5 can differ here. */
    base_url = document_base_url_of(el->owner_document);
    url_record_init(&base);
    have_base = base_url && *base_url && url_parse(&base, base_url, strlen(base_url), NULL);
    o = url_parse(&url, v ? (const char *)v : "", v ? n : 0, have_base ? &base : NULL)   /* step 4 */
        ? origin_of_url(&url) : document_origin;                            /* or step 5 on a failure */
    url_record_free(&url);
    url_record_free(&base);
    return o;
}

/* What Permissions Policy §9.7 reads off a navigable container, for the navigable this Document is created in. §9.5
 * takes "null or an element (container)", and §9.7 reads its node document's permissions policy (steps 2 and 3),
 * that document's origin (step 7, and §9.4 step 2's second argument, which `'self'` resolves to), the `allow`
 * attribute, and the element's §7.2 declared origin (§9.4 step 2's target origin).
 * §9.4 step 1, "if element is not an `iframe` element, then return an empty policy directive", is performed here,
 * since what an element is is this file's knowledge; `<object>`, `<embed>` and `<frame>` reach §9.7 with no container
 * policy, so an `allow` HTML does not define on them grants nothing. The container is HTML §7.3.1.3 "Child
 * navigables"' container of a navigable ("the navigable container whose content navigable is navigable, or null if
 * there is no such element"), and everything else comes from its node document.
 * A null container is §9.7 step 1's "return `Enabled`" for a top-level traversable or auxiliary browsing context and
 * for a frame §7.3.1.6 detached. A container in another WASM instance is not null, and reading it as null would grant
 * a cross-origin child every feature its embedder holds; it is told apart by a remote parent WindowProxy and answered
 * from the provisioning record (see below). */
PermissionsPolicy *document_permissions_policy_for_container(JSValueConst container, const Origin *origin)
{
    const PermissionsPolicy *container_doc_policy = NULL;
    const Origin *container_doc_origin = NULL;
    const Origin *container_declared_origin = NULL;
    const lxb_char_t *allow = NULL;
    size_t allow_len = 0;
    bool allowfullscreen = false;

    if (JS_IsObject(container)) {
        lxb_dom_node_t *el = node_of(container);
        Document *cd;

        DCHECK(el != NULL && el->type == LXB_DOM_NODE_TYPE_ELEMENT,
               "§7.3.1.3's container of this navigable is not an ELEMENT — §9.4's process-permissions-policy-"
               "attributes reads an `allow` attribute off it and §9.7 reads its node document, and a container "
               "that is not an element is a navigable whose creating operation recorded something else");
        cd = doc_rec(el->owner_document);
        DCHECK(cd != NULL && !JS_IsUndefined(cd->proxy),
               "§9.7's \"container's node document\" has no record with a navigable — the container element is "
               "in this heap, so the document holding it is a Document of this agent and every such Document is "
               "installed with its §7.2.3 WindowProxy; without one there is no origin for step 7 to compare");
        container_doc_policy = cd->permissions_policy;
        container_doc_origin = window_proxy_origin(cd->proxy);
        DCHECK(container_doc_policy != NULL,
               "§9.7's \"container's node document\" holds no permissions policy while holding a navigable — "
               "§9.5 runs for every Document a navigable is given, so a container document without one was "
               "installed past it and its own inheritance is unknown rather than empty");
        /* §9.4 step 1, then step 2's "the value of element's allow attribute" and "element's declared origin". No
           `allow` attribute yields NULL, which §9.4 turns into an empty policy directive, a positive statement. The
           declared origin is computed either way, since §9.3 step 2.9.1 reads it for a directive with an empty
           target list. */
        if (lxb_html_tree_node_is(el, LXB_TAG_IFRAME)) {
            allow = lxb_dom_element_get_attribute((lxb_dom_element_t *)el, (const lxb_char_t *)"allow", 5,
                                                  &allow_len);
            /* §9.4 step 3's first conjunct: "if element's `allowfullscreen` attribute is specified". A boolean
               content attribute (HTML §2.3.2 "Boolean attributes": "The presence of a boolean attribute on an
               element represents the true value, and the absence of the attribute represents the false value"), so
               presence is what counts (`allowfullscreen="false"` specifies it), asked with `has_attribute` since a
               present empty value reads as NULL. Inside the `iframe` guard because §9.4 step 1 returns early
               otherwise and HTML declares `allowfullscreen` on `iframe` alone. */
            allowfullscreen = lxb_dom_element_has_attribute((lxb_dom_element_t *)el,
                                                           (const lxb_char_t *)"allowfullscreen", 15);
            container_declared_origin = doc_declared_origin(el, cd->sandbox_flags, container_doc_origin);
        }
    }
    return permissions_policy_create(container_doc_policy, container_doc_origin, (const char *)allow,
                                     allow_len, allowfullscreen, container_declared_origin, origin);
}

/* §9.6 "Create a Permissions Policy for a navigable from response" for the navigable a Document is created in: §9.5
 * asked of the navigable (which routes its three container cases), then the response's declared policy over it.
 * Step 1 has two implementations here and steps 2-4 one, as core/permissions_policy/permissions_policy.h argues: step
 * 1 is about the container (an element in this heap, or a peer's computed answer read off the provisioning record),
 * steps 2-4 about this document's response, so both arms rejoin at permissions_policy_apply_response.
 * `header_value` is the caller's pick (§9.1 step 1): HTML §7.5.1 passes `Permissions-Policy` and §10.1 "Changes to
 * the HTML specification" passes `Permissions-Policy-Report-Only` for the Document's report-only policy, two separate
 * policies that §9.10 compares.
 * The cross-instance arm reads a carried result, not carried inputs: §9.5's arguments (the container element and
 * the new Document's origin) belong to the creating instance, so the creator runs §9.5 (the function above) and its
 * answer crosses on the `navigable.create` notice. A navigable whose parent is a remote WindowProxy is a child
 * navigable by §7.3.1.3's definition, so a record stating nothing about its container is a host that owes a call. */
static PermissionsPolicy *document_create_permissions_policy(JSContext *ctx, JSValueConst nav_proxy,
                                                             const char *header_value)
{
    JSValue container = window_proxy_container(ctx, nav_proxy);
    PermissionsPolicy *policy;

    if (!JS_IsObject(container)) {
        JSValue parent = window_proxy_parent_navigable(ctx, nav_proxy);
        bool remote_parent = window_proxy_is(parent) && window_proxy_is_remote(parent);

        JS_FreeValue(ctx, parent);
        if (remote_parent) {
            const char *carried = window_proxy_remote_container(nav_proxy);

            DCHECK(carried != NULL,
                   "Permissions Policy §9.5 is creating a policy for a navigable whose §7.3.1.3 CONTAINER is "
                   "in another WASM instance, and the record that provisioned this instance stated NOTHING "
                   "about it — this navigable has a parent, so §7.3.1.3 makes it a CHILD NAVIGABLE and a child "
                   "navigable has a container. Reading that silence as a null container takes §9.7 step 1 and "
                   "returns `Enabled` for every supported feature, which hands a cross-origin child the "
                   "`cross-origin-isolated` capability its embedder was never asked about. The statement "
                   "exists (core/frame/navigable.h's navigable_root_container, beside navigable_root) and this "
                   "host did not make it: carry the notice's container field to it");
            DCHECK(permissions_policy_serialized_has_container(carried),
                   "a navigable whose §7.3.1.3 PARENT is in another WASM instance was provisioned with §9.5's "
                   "\"container is null\" — the section defines a CHILD NAVIGABLE as one whose parent is "
                   "non-null and gives it a container element, so the two statements on that record "
                   "contradict each other and one of them is about a different navigable");
            JS_FreeValue(ctx, container);
            /* §9.6 step 1, cross-instance arm: §9.5 ran in the instance holding the container element and its answer
               crossed. Steps 2-4 run below over this document's response, which is why the record carries §9.5's
               result and not §9.6's. */
            policy = permissions_policy_deserialize(carried);
            permissions_policy_apply_response(policy, header_value, window_proxy_origin(nav_proxy));
            return policy;
        }
    }
    /* §9.6 step 1, local arm. */
    policy = document_permissions_policy_for_container(container, window_proxy_origin(nav_proxy));
    JS_FreeValue(ctx, container);
    /* §9.6 steps 2-4: "let d be the result of running Process response policy given response, origin and
       report-only", then copy each declaration the inherited policy still permits. The origin is §9.2's, the new
       Document's, the same one §9.5 was given above (both take navigationParams's origin). */
    permissions_policy_apply_response(policy, header_value, window_proxy_origin(nav_proxy));
    return policy;
}

/* HTML §4.8.5 "The `iframe` element"'s allowed to use (see document.h for the steps and why they are here). */
bool document_allowed_to_use(JSContext *ctx, PermissionsPolicyFeature feature)
{
    Document *d = doc_here(ctx);

    /* Step 1: "If document's browsing context is null, then return false." §9.5 never ran for such a Document, so it
       has no policy on this record. */
    if (d->permissions_policy == NULL)
        return false;
    /* Step 2: "If document is not fully active, then return false." §7.3.3's walk, asked rather than remembered:
       removing an ancestor `<iframe>` stops this document being fully active with nothing done to its navigable. */
    if (!document_fully_active(ctx))
        return false;
    /* Step 3, HTML §4.8.5's "on feature, document, and document's origin": Permissions Policy §9.10's `origin` is
       this document's own, and `report` takes the default true. Step 4 is the other answer. */
    {
        const Origin *origin = window_proxy_origin(d->proxy);

        return permissions_policy_is_feature_enabled_in_document(d->permissions_policy,
                                                                 d->report_only_permissions_policy, feature,
                                                                 origin, origin, true) == PP_ENABLED;
    }
}

JSValueConst document_window_proxy(JSContext *ctx)
{
    Document *d;

    /* A dedicated worker answers for its owner's navigable, whose descendant it is for Mixed Content's parent
       question (HTML §10.2.4: the owner set). */
    if (!doc_of(ctx)) return document_window_proxy(worker_env_of(ctx)->owner);
    d = doc_here(ctx);
    DCHECK(!JS_IsUndefined(d->proxy), "this realm's WindowProxy was read before its Document was installed — "
                                      "§7.2.3 gives a navigable ONE, and it is minted with the realm");
    return d->proxy;
}

/* HTML §7.3.3 "Fully active documents": a Document is fully active when it is the active document of a navigable
 * that is a top-level traversable or whose container document is itself fully active. It is not `!closed`: removing
 * an `<iframe>` destroys that navigable, and every document nested in it stops being fully active with nothing done
 * to its own navigable, so the answer is the definition's walk, asked rather than remembered because the tree is per
 * flow. The Observable standard asks it to open §2.1's next/error/complete/addTeardown, §2.2.1's subscribe and §3's
 * when(), and re-asks it in §2.1's close-a-subscription before every teardown, since "each teardown could result in
 * the above Document becoming inactive". */
bool document_fully_active(JSContext *ctx)
{
    Document *d = doc_here(ctx);
    JSValue cur;
    bool ok = true;

    /* A Document with no navigable (`new Document()`, a DOMParser result) has no browsing context, so the guard's
       premise ("the relevant global object is a Window") is false and the algorithm proceeds. */
    if (!window_proxy_is(d->proxy))
        return true;
    cur = JS_DupValue(ctx, d->proxy);
    for (;;) {
        JSValue parent;
        if (window_proxy_closed(ctx, cur)) { ok = false; break; }
        /* The navigable tree, not §7.2.2.4's `parent` getter: the container document is the parent navigable's
           active document, and the getter answers through that document's realm, which this question must never
           materialize. JS_UNDEFINED is §7.3.3's base case, a top-level traversable.
           Named residual: a parent held by another instance (a peer's navigable, whose container document is
           the peer's) ends the walk as if this navigable were top-level. The next diff asks that instance
           whether its document is fully active, a cross-instance read and so a suspend point. Its absence shows
           as a frame rooted in a peer answering fully active after the peer's own ancestor was detached. */
        parent = window_proxy_parent_navigable(ctx, cur);
        if (!window_proxy_is(parent) || window_proxy_is_remote(parent)) {
            JS_FreeValue(ctx, parent);
            break;
        }
        JS_FreeValue(ctx, cur);
        cur = parent;
    }
    JS_FreeValue(ctx, cur);
    return ok;
}

uint32_t document_doc(JSContext *ctx) { return doc_here(ctx)->doc; }

/* A dedicated worker realm's environment record, which stands where a Window realm's Document does for the two
   facts the engine asks of an environment: the world-registry handle its work is filed under (its owner's), and
   its policy container. A realm value holding an object of an internal class whose opaque is the record. */

static JSClassID g_env_slot = JS_INVALID_CLASS_ID, g_env_class;

static void worker_env_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id;
    WorkerEnvironment *e = JS_GetAnyOpaque(val, &id);

    (void)rt;
    if (!e) return;
    policy_container_free(e->policy);
    free(e->url);
    JS_FreeContext(e->owner);
    free(e);
}

static WorkerEnvironment *worker_env_of(JSContext *ctx)
{
    JSValue v = realm_value_get(ctx, g_env_slot);
    WorkerEnvironment *e = JS_GetOpaque(v, g_env_class);

    JS_FreeValue(ctx, v);   /* the realm's slot keeps the object */
    DCHECK(e != NULL, "a worker realm's environment slot holds no environment record");
    return e;
}

uint32_t document_environment_doc(JSContext *ctx)
{
    Document *d = doc_of(ctx);

    return d ? d->doc : worker_env_of(ctx)->doc;
}

void document_set_worker_environment(JSContext *ctx, JSContext *owner, const char *url)
{
    WorkerEnvironment *e;
    JSValue obj;

    DCHECK(doc_of(ctx) == NULL, "a realm with its own Document was given a worker's environment");
    e = calloc(1, sizeof *e);
    CHECK(e != NULL, "document: OOM building a worker's environment record");
    e->doc = document_environment_doc(owner);
    e->url = strdup(url);
    CHECK(e->url != NULL, "document: OOM copying a worker's url");
    e->owner = JS_DupContext(owner);
    /* HTML §7.1.7 Policy containers' create a policy container from a fetch response: "If response's URL's
       scheme is "blob", then return a clone of response's URL's blob URL entry's environment's policy container"
       — the owner's, for a blob the owner created.
       Named residual. Not covered: a worker fetched over HTTP(S), whose CSP list is parsed from its own response.
       Next diff builds: the worker's policy container set from the script's response at run a worker's delivery.
       Absence shows as: an HTTP(S) worker enforcing its owner's CSP rather than the one its response carries. */
    e->policy = policy_container_clone(document_policy(owner));
    obj = JS_NewObjectProtoClass(ctx, JS_NULL, g_env_class);
    CHECK(!JS_IsException(obj), "document: OOM holding a worker's environment record");
    JS_SetOpaque(obj, e);
    realm_value_set(ctx, g_env_slot, obj);
}

JSValueConst document_object(JSContext *ctx) { return doc_here(ctx)->doc_obj; }

/* Selection API §4.1's getSelection (see document.h). The receiver decides which document: a realm can hold several
   and only the active one has a browsing context. */
JSValue document_selection(JSContext *ctx, JSValueConst doc)
{
    Document *d = doc_receiver(ctx, doc);

    if (!d) return JS_EXCEPTION;
    /* "…if this has an associated browsing context, and it must return null otherwise." The record states the
       browsing context as the Window it was installed with, and document_install builds the selection with it, so
       the two cannot disagree, which this asserts. */
    DCHECK(JS_IsUndefined(d->selection) == JS_IsUndefined(d->win_obj),
           "a Document has a browsing context and no selection, or a selection and no browsing context — "
           "Selection API §2 gives one to exactly the documents that have one, and document_install is the "
           "single line that builds both");
    if (JS_IsUndefined(d->selection)) return JS_NULL;
    return JS_DupValue(ctx, d->selection);
}

/* Selection API §2's replacement: "A document's selection is a singleton object associated with that document, so it
   gets replaced with a new object when Document.open() is called." Here, because `selection` is this record's field
   (core/dom/selection.c mints the object and core/html/document_open.c runs the algorithm). With no browsing context
   it returns rather than minting: `implementation.createHTMLDocument("")` has no selection and §8.4.1 is reachable on
   it, and its `getSelection()` must stay null. The old object is freed: the engine holds the only reference, and a
   page that kept its own keeps an object that is no longer the document's, which "replaced" means. */
void document_replace_selection(JSContext *ctx, JSValueConst doc)
{
    Document *d = doc_receiver(ctx, doc);
    JSValue fresh;

    if (!d) return;
    if (JS_IsUndefined(d->selection)) return;   /* no browsing context: §4.1 answers null, and still will */
    fresh = selection_new(ctx, doc);
    DCHECK(!JS_IsException(fresh),
           "Selection API §2's replacement could not mint the new selection — §8.4.1's steps have already "
           "emptied the tree by the time this runs, so leaving the old one would hold a Selection over a tree "
           "that no longer exists, which is the state this replacement exists to end");
    JS_FreeValue(ctx, d->selection);
    d->selection = fresh;
}

/* Document.prototype, and the Document as a real node: §4.4 `interface Document : Node`, so the document object is a
   node_wrap of the document node, in the one identity table, with members on a prototype chained to Node.prototype
   (`document.nodeType`, `document.contains(el)`, `document.body.parentNode.parentNode === document`). This is the
   agent's half: a member is declared once and builds one pool entry, so declaring per document would declare
   Document again for a second realm, which the pool's seal catches. */
void document_init(JSContext *ctx)
{
    JSClassDef d = { "Document" };

    DCHECK(g_document_rt == NULL,
           "document_init ran twice — §4.5's interface, its fifteen member declarations and the ten "
           "sub-components below are declared once per AGENT, and a second declaration re-mints the class every "
           "wrapper of the first agent is already branded with");
    g_document_rt = JS_GetRuntime(ctx);
    JS_NewClassID(JS_GetRuntime(ctx), &g_document_class);
    JS_NewClass(JS_GetRuntime(ctx), g_document_class, &d);
    node_claim_type(LXB_DOM_NODE_TYPE_DOCUMENT, g_document_class);
    {
        /* DOM §4.5's `[Exposed=Window] interface XMLDocument : Document {};`, its per-realm prototype slot. It does
           not claim the document node type (node_claim_type asserts one claimant per type), since the record, not
           the type, decides between the two, at document_interface_proto. */
        JSClassDef xd = { "XMLDocument" };
        JS_NewClassID(JS_GetRuntime(ctx), &g_xml_document_class);
        JS_NewClass(JS_GetRuntime(ctx), g_xml_document_class, &xd);
    }
    /* How this component's records reach the collector, declared with the agent before any record exists
       (doc_rec_new asserts that order at each record's birth). It mirrors the realm-teardown hook navigable.c
       declares: that one says when a record dies, this one what it holds while it lives. */
    JS_SetContextMarkHook(JS_GetRuntime(ctx), document_realm_mark);
    g_ready_slot = realm_value_declare(ctx, "HTML current document readiness");
    /* §7.5.9's page showing, the readiness's twin (written by "the end" and by unloading), declared beside it. */
    g_showing_slot = realm_value_declare(ctx, "HTML §7.5.9 page showing");
    g_env_slot = realm_value_declare(ctx, "a dedicated worker realm's environment record");
    {
        JSClassDef ed = { "WorkerEnvironment", .finalizer = worker_env_finalizer };
        JS_NewClassID(JS_GetRuntime(ctx), &g_env_class);
        CHECK(JS_NewClass(JS_GetRuntime(ctx), g_env_class, &ed) == 0,
              "the worker environment record's class could not be registered");
    }
    document_declare_members(ctx);
    /* §3.1.4's resource metadata members declare their own setter and per-realm cookie store;
       document_metadata_install runs from document_install_proto below, so the declaration is paired here, not in
       each host's init list. */
    document_metadata_init(ctx);
    /* §7.1.1.2's `domain`, the fifth member of that partial interface, is its own component because it is the one
       operation in the platform that mutates an origin; declared here for the same pairing. */
    document_domain_init(ctx);
    /* §8.4's `write`, `writeln` and `close`, declared here for the same pairing; `write`/`writeln` are variadic
       DOMString members whose declaration converts every argument (suspending on the page's own toString) before the
       algorithm sees one. */
    document_write_init(ctx);
    /* §3.1.7's `currentScript`, a sixth member of that partial interface: its getter is one line, and its substance
       is the §4.12.1.1 bracket that writes a per-flow slot. Its install runs from document_install_proto below. */
    document_current_script_init(ctx);
    document_fragment_init(ctx);   /* §4.7, before any fragment is wrapped as a bare Node */
    shadow_root_init(ctx);         /* §4.8, whose prototype chains to §4.7's — declared after it */
    slot_init(ctx);                /* §4.2.2's slots, which only exist inside a §4.8 tree */
    document_type_init(ctx);       /* §4.6, before the parser's doctype is wrapped as a bare Node */
    dom_implementation_init(ctx);  /* §4.5.1, which every document's record builds one of */
    /* Selection API §3, whose §2 state every document's record builds and whose §4.1 `getSelection` is installed from
       document_install_proto below, paired here. It follows element_init's row of core/platform.c's list, which
       declares §5.5's class `addRange` brands against; selection_init asserts that. */
    selection_init(ctx);
    /* §6.2's visibility state is a document's, and page_visibility_install runs from document_install_proto below,
       so its declaration is paired here rather than in each host's init list. */
    page_visibility_init(ctx);
    /* HTML §3.1.6's render-blocking element set is a Document's, built by document_install_proto below. */
    render_blocking_init(ctx);
    /* HTML §6.6's focused area is a Document's state too, and focus_install_document_members runs from
       document_install_proto below, so its declaration is paired here. */
    focus_init(ctx);
    /* HTML §6.6.7's autofocus candidates and processed flag, filled by the insertion steps and drained by §8.1.7.3's
       step 7, declared with the focused area they designate. */
    autofocus_init(ctx);
    /* The regions whose children are the parse of unknown markup, which §7.2.2.3's named access asks. */
    unknown_markup_init(ctx);
    /* Fullscreen §2 "Model"'s per-element fullscreen flag is a slot key, and both members stated over it
       (`fullscreen` on this prototype, `fullscreenElement` on this one and on ShadowRoot's) install from this
       component's list, so the declaration is paired here. */
    fullscreen_init(ctx);
    realm_declare_intrinsic(document_install_proto);
    /* §13.2.7 "The end" is an agent fact: document_lifecycle_step walks navigable_tree_order and answers for every
       document of this agent in one call, so the frontier's single document-lifecycle slot is claimed once, here,
       and given back once at document_agent_free (the setter asserts both). */
    engine_set_document_done_hook(document_lifecycle_step);
    engine_set_parse_end_hook(document_parse_end_step);   /* §13.2.7 step 3, asked above the sequence */
    agent_state_ptr("document", &g_document_rt,
                    "the runtime §4.5's interface and its ten sub-components declared into, and the latch");
    agent_state_class("document", &g_document_class, "§4.5's interface prototype slot and brand");
    agent_state_class("document", &g_xml_document_class, "DOM §4.5's XMLDocument interface prototype slot");
    agent_state_realm_slot("document", &g_env_slot, "a dedicated worker realm's environment record");
    agent_state_class("document", &g_env_class, "a dedicated worker realm's environment record class");
    agent_state_realm_slot("document", &g_ready_slot,
                           "the per-realm slot §3.1.5's current document readiness lives in");
    agent_state_realm_slot("document", &g_showing_slot, "the per-realm slot §7.5.9's page showing lives in");
}

/* §3.1.1's interface prototype object, for one realm. */
void document_install_proto(JSContext *ctx)
{
    JSValue proto, base, prev;

    prev = JS_GetClassProto(ctx, g_document_class);
    DCHECK(JS_IsNull(prev), "document_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = node_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "Document.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Document");
    document_install_members(ctx, proto);
    /* §3.1.1's IDL includes GlobalEventHandlers and adds onreadystatechange / onvisibilitychange. */
    event_target_install_handlers(ctx, proto, EH_GLOBAL | EH_DOCUMENT);
    idl_members_excluded(ctx, proto, "Document", TOUCH_EXCLUDED,
                         (int)(sizeof(TOUCH_EXCLUDED) / sizeof(TOUCH_EXCLUDED[0])),
                         "Touch Events Level 2, `Extensions to the GlobalEventHandlers mixin`: \"For user "
                         "agents where expose legacy touch event APIs is false, this mixin must not be "
                         "implemented.\" This agent's `expose legacy touch event APIs` is false — TouchEvent, "
                         "Touch and TouchList are absent, so a touch handler would have nothing to be handed");
    /* §4.5: `Document includes ParentNode` (not ChildNode, since a document has no parent) and
       `NonElementParentNode`, the same getElementById DocumentFragment includes. */
    node_install_parent_mixin(ctx, proto);
    node_install_nonelement_parent_mixin(ctx, proto);
    /* HTML §6.2's `visibilityState` and `hidden`: one source and the comparison the spec defines over it. */
    page_visibility_install(ctx, proto);
    render_blocking_install(ctx);
    /* HTML §6.6.6's `activeElement` (DocumentOrShadowRoot) and `hasFocus()`, and this realm's initial focused area,
       the viewport, built with the realm so it is baseline. */
    focus_install_document_members(ctx, proto);
    /* Selection API §4.1's `Selection? getSelection()`, installed by the component that owns the algorithm and
       state. */
    selection_install_document_members(ctx, proto);
    /* Fullscreen §3 "API"'s `fullscreenEnabled` (§7's `fullscreen` permissions-policy feature conjoined with §2's
       "fullscreen is supported"); a readonly accessor declares nothing at the agent, so it has no init line above
       (core/fullscreen/fullscreen.h). */
    fullscreen_install_document_members(ctx, proto);
    /* HTML §3.1.7's `currentScript` and this realm's baseline record holding its initial null, installed by the
       component that owns §4.12.1.1's bracket. */
    document_current_script_install(ctx, proto);
    /* HTML §6.6.7's per-document autofocus candidates and processed flag, built with the realm so an element the
       first flow inserts does not find a list that flow created. */
    autofocus_install_document(ctx);
    unknown_markup_install_document(ctx);
    {
        /* DOM §4.5's `[Exposed=Window] interface XMLDocument : Document {};`: Web IDL §3.7.3 "Interface prototype
           object" for an interface with no members, so it is empty and everything comes through Document.prototype.
           Created over it, §3.7.3's order ("Let proto be … the interface prototype object in realm of that inherited
           interface", then OrdinaryObjectCreate(proto)), which idl_interface_tag's inheritance assert reads. Built
           before the JS_SetClassProto below, which consumes `proto`. */
        JSValue xproto = JS_NewObjectProto(ctx, proto);

        CHECK(!JS_IsException(xproto), "XMLDocument.prototype could not be allocated");
        idl_interface_tag(ctx, xproto, "XMLDocument");
        JS_SetClassProto(ctx, g_xml_document_class, xproto);
    }
    JS_SetClassProto(ctx, g_document_class, proto);
    /* This realm's document readiness, built with the realm so it is baseline, as §8.12 "Animation frames"'s map and
       §7.4.6.3's flag are. It exists before the realm has a Document; "loading" is what an uninstalled Document
       would answer, and the lifecycle walk reaches a realm only through its materialized navigable. */
    {
        JSValue rec = JS_NewObjectProto(ctx, JS_NULL);
        CHECK(!JS_IsException(rec), "this realm's document-readiness record could not be allocated");
        JS_SetPropertyStr(ctx, rec, "stage", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, rec, "dcl", JS_FALSE);
        realm_value_set(ctx, g_ready_slot, rec);
    }
    /* §7.5.9: page showing is "initially false", built here so it is baseline rather than created inside whichever
       flow read first. */
    {
        JSValue rec = JS_NewObjectProto(ctx, JS_NULL);
        CHECK(!JS_IsException(rec), "this realm's §7.5.9 page-showing record could not be allocated");
        JS_SetPropertyStr(ctx, rec, "showing", JS_NewInt32(ctx, 0));
        realm_value_set(ctx, g_showing_slot, rec);
    }
}

/* DOM §4.5's two creation facts, built (see document.h for why they are one value and neither derives the other).
   Asserted here, where a caller's answer arrives: an empty or oversized type is a caller that ran no algorithm
   producing one. */
DocumentKind document_kind(bool is_xml, const char *content_type)
{
    DocumentKind k;

    DCHECK(content_type != NULL && content_type[0] != '\0',
           "a Document was created with no content type — §4.5 gives every document one and every algorithm "
           "that creates a Document states it (HTML §7.5.2's literal \"text/html\", §7.5.3's and §7.5.4's "
           "computed type, §4.5.1's own arguments), so an absent one is a creation that skipped its own step");
    DCHECK(strlen(content_type) < sizeof k.content_type,
           "a Document's content type does not fit the record — §4.2 \"MIME type\"'s essence is what a "
           "creation states here and this buffer was sized for one, so a longer string is either a full "
           "serialization (parameters and all, which §7.5.1 does not pass) or a type this build should size "
           "for; truncating it would store bytes no MIME type parse produced");
    k.is_xml = is_xml;
    snprintf(k.content_type, sizeof k.content_type, "%s", content_type);
    return k;
}

/* HTML §7.3.2.1 "Creating browsing contexts": "Let document be a new Document, with: type `html`, content type
   `text/html`, …", the Document created with no response. */
DocumentKind document_kind_initial_about_blank(void)
{
    return document_kind(/*is_xml*/false, "text/html");
}

/* A document's record, and the one place the document-to-record answer is established: the Lexbor document's
 * `user` slot, which lexbor keeps for its embedder and never reads. One indirection answers every per-document
 * question with no registry to keep in step (a registry's failure mode is a stale row). */
static Document *doc_rec_new(JSContext *ctx, lxb_html_document_t *dom, const char *url, DocumentInterface iface,
                             DocumentKind kind, const char *about_base_url, bool is_iframe_srcdoc)
{
    lxb_dom_document_t *dd = lxb_dom_interface_document(dom);
    Document *d;
    const char *type = kind.content_type;

    DCHECK(dom != NULL, "a document record was built for no document");
    /* §4.5 gives every document a content type, so it is required: the address may be absent (a document with no
       browsing context has none), the type may not, and §4.4's clone reads it back off the record. */
    DCHECK(type != NULL && type[0] != '\0',
           "a document record was built with no content type — §4.5's `contentType` is state every document "
           "has, and a record that never received one would answer for its document with an empty string that "
           "no parse, no createDocument and no clone could have produced");
    DCHECK(dd->user == NULL, "a second record was built for one Lexbor document — the first would be leaked and "
                             "every node of the tree would answer through whichever won");
    /* The cause of an uncollectable realm, asked at the birth of the record: its counted references hang off malloc'd
       C, two of them pointing back into this realm, so a collector not told how to read the record could never
       collect the realm, which JS_FreeRuntime's gc_obj_list walk would report with no owner named. Asked of every
       record, since second Documents (§4.5.1's factories, DOMParser, responseXML) hold the same references. */
    DCHECK(JS_GetContextMarkHook(JS_GetRuntime(ctx)) == document_realm_mark,
           "a Document record was built in a runtime that does not report this component's records to the "
           "collector — document_init declares document_realm_mark, and without it every reference the record "
           "holds is one gc_decref cannot subtract, so the realm the record points back into is a cycle with an "
           "invisible edge and is uncollectable for the life of the runtime");
    d = calloc(1, sizeof *d);
    CHECK(d != NULL, "document: OOM naming a document");
    d->realm = ctx;
    d->dom = dom;
    d->doc_obj = JS_UNDEFINED;
    d->win_obj = JS_UNDEFINED;
    d->proxy = JS_UNDEFINED;
    d->impl = JS_UNDEFINED;
    /* Selection API §2 gives a selection to a document with a browsing context, so document_install builds it; every
       other document keeps UNDEFINED, which §4.1's getSelection reads as its "must return null otherwise". */
    d->selection = JS_UNDEFINED;
    /* §3.1.1's ancestor-origins pair. UNDEFINED is a Document with no browsing context (a §4.5 createDocument, a
       DOMParser's), which §7.2.4's first step answers with the Location's own empty list. */
    d->ancestor_origins = JS_UNDEFINED;
    d->ancestor_origin_strings = JS_UNDEFINED;
    /* The address's value is UNDEFINED first: a zeroed JSValue is the integer 0 (JS_TAG_INT is 0), which
       doc_rec_refs or document_url_value's assert would read as a real value. */
    d->addr.value = JS_UNDEFINED;
    /* The record's first address. A NULL one is stored as the empty string, which readers test for. Its value is a
       plain string: a Document is created from a response, so its address is a concrete fact and `concolic_is` is
       false for it. A Document whose address the run computed acquires it through document_set_url (HTML §7.4.4 step
       8). */
    d->addr.bytes = doc_addr_intern(d, url ? url : "");
    d->addr.value = JS_NewString(ctx, url ? url : "");
    CHECK(!JS_IsException(d->addr.value), "document: OOM naming a document's address");
    doc_addr_assert_agrees(ctx, d);
    /* HTML §7.4's about base URL, written at the record's birth: §7.5.1 "Shared document creation infrastructure"'s
       create-and-initialize-a-Document-object gives it as one row of the table that gives the address, beside `URL`,
       `policy container` and `final sandboxing flag set`. The value is a fact about §7.4's operation, which
       core/frame/navigable.c owns and the host builder carries, as it carries the address, §7.1.7's container and
       §7.1.5's flags. It must exist before document_install's parsed walks run inside the builder (§4.2.3's freeze
       and §4.8.5's iframe walk ask §2.4.3 for a fallback base URL), or an iframe srcdoc document fires §2.4.3 step
       1's assert. No COW capture: no flow can yet read the field and there is no `document` object for a delta. */
    DCHECK(about_base_url == NULL || about_base_url[0] != '\0',
           "§7.4's about base URL was stated as the EMPTY STRING at a Document's creation — a null one is the "
           "ABSENCE of the fact, which is what every Document created from a response has, and §2.4.3 "
           "\"Document base URLs\" tests the stored value for null rather than for emptiness, so an empty one "
           "would be returned as this Document's fallback base URL and every relative URL in it would resolve "
           "against nothing");
    if (about_base_url) d->base.about = doc_addr_intern(d, about_base_url);
    /* HTML §4.8.5's class, and its pairing with the field above, asserted at the birth of the record holding both:
       §2.4.3 step 1 asserts "document's about base URL is non-null" for this class, at a read arbitrarily far away
       (the first relative URL anything resolves), while here both operands are in hand and the message can name the
       caller. core/frame/navigable.c asserts the same invariant over §7.4.2.2's document resource at its enqueue,
       one hop earlier, with the realm builders between. */
    DCHECK(!is_iframe_srcdoc || about_base_url != NULL,
           "an IFRAME SRCDOC DOCUMENT was created with no about base URL. HTML §4.8.5 \"The `iframe` "
           "element\"'s process-the-iframe-attributes step 1 confers this class on the Document its srcdoc "
           "navigation produces, and §2.4.3 \"Document base URLs\"' fallback base URL step 1 asserts a "
           "non-null about base URL for exactly it — a srcdoc document has no address of its own to fall back "
           "to, so without one every relative URL in it resolves against `about:srcdoc`, whose opaque path "
           "cannot be a base. Pass §7.4.2.2 \"Beginning navigation\"'s initiatorBaseURLSnapshot beside the "
           "resource: the two are rows of ONE constructor (§7.4.5's create navigation params from a srcdoc "
           "resource reads the body AND the about base URL off the same entry), and a creation carrying one "
           "of them is half of it");
    d->is_iframe_srcdoc = is_iframe_srcdoc;
    /* Both of §4.5's creation facts from the one value carrying them; `document_kind` asserted the content type is
       non-empty and fits, so this copy cannot truncate. */
    snprintf(d->content_type, sizeof d->content_type, "%s", type);
    d->is_xml = kind.is_xml;
    /* And DOM §4.5's third, "create a document that implements an interface interface", stored and never derived
       from `is_xml` (see document.h). */
    d->iface = iface;
    /* DOM §4.5's default encoding, utf-8 ("unless stated otherwise"); HTML §13.2.3.2's answer is written later by
       document_set_encoding. Asked of the registry, so the set of encodings has one authority. */
    d->encoding = encoding_utf8();
    dd->user = d;
    return d;
}

/* The record's held references go back, and the tree with them when this record owns it: every node of a
   page-created document has a wrapper the identity map references, so a document freed without handing them back
   would leave the map naming freed memory. */
static void doc_rec_release(Document *d)
{
    JSContext *ctx = d->realm;

    /* Before the references go, and not one of them: §4.5.1's object holds a raw pointer to the Lexbor document, and
       a DOMImplementation the page kept outlives this record. */
    dom_implementation_detach(ctx, d->impl);
    doc_rec_refs(d, doc_ref_release, NULL);   /* the one list; see doc_rec_refs */
    doc_addrs_free(d);   /* every address this record held; see doc_addr_intern */
    policy_container_free(d->policy);   /* malloc'd, so the GC walk would never have named it */
    /* Permissions Policy §9.5's policies, malloc'd too and released beside the container. NULL for a Document with
       no browsing context. */
    permissions_policy_free(d->permissions_policy);
    permissions_policy_free(d->report_only_permissions_policy);
    header_values_free(d->xfo, d->n_xfo);
    if (d->dom)
        lxb_dom_interface_document(d->dom)->user = NULL;
    free(d);
}

/* The tree is about to go, so the record naming it goes first, from the one place a document's lifetime ends. A
   delta-owned document is destroyed by the flow that made it and its record is reachable from nothing else. */
void document_record_release(lxb_html_document_t *dom)
{
    Document *d = doc_rec(lxb_dom_interface_document(dom));

    if (!d) return;   /* already released: the realm's path clears the record before destroying the tree */
    DCHECK(!d->owned, "a document the REALM owns was destroyed through the delta's path — two owners for one "
                      "tree is one free too many");
    doc_rec_release(d);
}

/* And whether there is still one to release (see document.h for why the destroy may not read `user` itself). It
   decides nothing, as a DCHECK condition must. */
bool document_record_names(lxb_html_document_t *dom)
{
    return dom && doc_rec(lxb_dom_interface_document(dom)) != NULL;
}

/* The realm's own path: clear the record first, so the destroy below finds nothing to release and only one owner
   runs. */
static void doc_rec_free(JSContext *ctx, Document *d)
{
    lxb_html_document_t *dom = d->owned ? d->dom : NULL;

    (void)ctx;
    doc_rec_release(d);
    if (dom)
        dom_cow_destroy_document(dom);
}

/* The realm owns this document: the arm a baseline creation takes, onto the chain the realm's teardown walks. The
   other arm is the running flow's COW delta (dom_cow_note_created_document), which destroys the document when the
   delta is discarded. Exactly one of the two, which `owned` records. */
static void doc_realm_owns(JSContext *ctx, Document *d)
{
    Document *active = doc_of(ctx);

    DCHECK(active != NULL, "a document was created in a realm that has none — the chain that owns a baseline "
                           "creation hangs off the realm's ACTIVE document, and there is none to hang it on");
    d->owned = 1;
    d->next_created = active->next_created;
    active->next_created = d;
}

/* The rest of what every document in this realm has, whoever owns its tree: the wrapper the identity map holds,
   and §4.5's `[SameObject] implementation`. Returns the wrapper, owned. */
static JSValue doc_finish(JSContext *ctx, Document *d)
{
    JSValue doc = node_wrap(ctx, lxb_dom_interface_node(d->dom));

    CHECK(JS_IsObject(doc), "a created Document's wrapper allocation failed");
    d->doc_obj = JS_DupValue(ctx, doc);
    d->impl = dom_implementation_new(ctx, doc);
    return doc;
}

/* A second document in this realm (see document.h). */
JSValue document_new(JSContext *ctx, lxb_html_document_t *dom, const char *url, DocumentInterface iface,
                     DocumentKind kind)
{
    /* No about base URL and not HTML §4.8.5's class. §7.4 gives an about base URL to a Document whose creating
       operation addressed it `about:blank` or `about:srcdoc`, and §4.8.5's "the resulting Document must be
       considered an iframe srcdoc document" is said of the Document a srcdoc navigation produces. A second Document
       in this realm (§4.5.1's factories, DOMParser, XHR's responseXML) is created by neither, even when the address
       it copies off its source is `about:srcdoc` (a DOMParser or a clone inside a srcdoc frame), so §2.4.3
       "Document base URLs" answers for it with step 3, its own address. */
    Document *d = doc_rec_new(ctx, dom, url, iface, kind, /*about_base_url*/ NULL, /*is_iframe_srcdoc*/ false);

    /* Who destroys it: a document a flow created belongs to that flow's COW delta, as a node it created does, and
       is destroyed when the delta is discarded, so the frontier does not keep one document per exploration arm. A
       creation made while capture is off is baseline (the boot flow's creations are), and the realm that made it
       outlives it. */
    if (!dom_cow_note_created_document(dom)) doc_realm_owns(ctx, d);
    {
        /* HTML §4.2.3 for the tree this document was handed, as document_install runs it: a
           `DOMParser.parseFromString` result and an XHR `responseXML` are parsed markup, so their `<base href>` is
           in the tree without having passed a chokepoint, and DOM §4.4's `baseURI` on their nodes is the document
           base URL, not the address. After doc_finish rather than inside it, because the freeze captures into the
           flow's delta against the `document` object doc_finish returns. */
        JSValue w = doc_finish(ctx, d);

        html_base_element_parsed(ctx, lxb_dom_interface_node(dom));
        return w;
    }
}

/* HTML §4.12.3's appropriate template contents owner document: the Document that owns the template contents of
 * every `<template>` in `doc`, and why those contents are inert (the owner has no browsing context, so its scripts
 * do not run and its custom elements do not upgrade). DOM §4.5 adopt step 3.4 reaches it through HTML's adopting
 * steps for a template element, which adopt the contents into the owner of the document the template was adopted
 * into.
 * "If document is not a Document created by this algorithm" is the record's flag: an inert document is its own
 * owner, which makes "template elements inside Document objects that are created by this algorithm just reuse the
 * same Document owner" true rather than one inert document per level.
 * The realm owns the inert document, never the running flow's delta. The record naming it is malloc'd C with no
 * COW entry kind, so a delta-owned inert document would be destroyed with the first flow that touched a template
 * while the record sibling flows read still named it. Realm ownership costs nothing observable: template contents
 * are a DocumentFragment, not a child of the owner, so the owner's tree stays empty and no flow reaches a sibling's
 * markup through it ("a single Document to act as its proxy for owning the template contents of all its template
 * elements"). */
lxb_html_document_t *document_template_contents_owner(JSContext *ctx, lxb_dom_document_t *doc)
{
    Document *d = doc_rec(doc), *inert;

    DCHECK(d != NULL, "HTML §4.12.3 was asked for the template contents owner of a document with no record — "
                      "document_install and document_new are the two places one is built, and a tree that came "
                      "from neither names no realm to create the inert document in");
    DCHECK(ctx == d->realm, "HTML §4.12.3 creates the inert template document in `document`'s RELEVANT REALM, "
                            "which is the record's — a caller asking out of another realm of this agent would "
                            "build it with another document's prototypes and hand a template contents whose "
                            "nodes answer for the wrong global");
    if (d->is_inert_template)      /* "a Document created by this algorithm": it is its own owner */
        return d->dom;
    if (!d->inert_template) {
        lxb_html_document_t *dom = dom_document_create();

        CHECK(dom != NULL, "HTML §4.12.3: OOM creating an inert template document");
        /* "If document is an HTML document, then mark newDocument as an HTML document also." What makes a
           Document an HTML document is §4.5's type (the fact §4.5's createCDATASection refuses on), so the owner
           takes `document`'s type rather than comparing its content type, which HTML §7.5.4's text documents would
           answer backwards. A document that is not an HTML document gets §4.5's defaults, `application/xml` at
           `about:blank` with no browsing context. The content type is the standard's default either way, never the
           creator's: §4.12.3 marks the owner an HTML document and does not copy the response's type. */
        /* The interface is named by §4.12.3 and not inherited from `document`: "Let newDocument be the result of
           creating a document that implements Document, given document's relevant realm". So an XMLDocument's
           template contents owner is a Document, and the interface is stated here rather than read off `d`. */
        inert = doc_rec_new(d->realm, dom, "about:blank", DOCUMENT_IFACE_DOCUMENT,
                            document_kind(d->is_xml, d->is_xml ? "application/xml" : "text/html"),
                            /*about_base_url*/ NULL, /*is_iframe_srcdoc*/ false);
        inert->is_inert_template = 1;
        doc_realm_owns(d->realm, inert);
        /* The record holds the wrapper that outlives this; nothing here needs a second reference to a document the
           page cannot name. */
        JS_FreeValue(d->realm, doc_finish(d->realm, inert));
        d->inert_template = inert;
    }
    return d->inert_template->dom;
}

const char *document_url_of(const lxb_dom_document_t *dom)
{
    Document *d = doc_rec(dom);

    DCHECK(d != NULL, "a document's ADDRESS was read in a document with no record — a tree that came from "
                      "neither document_install nor document_new has none");
    return d->addr.bytes;
}

char *document_referrer_source(JSContext *ctx)
{
    const lxb_dom_document_t *dom;
    Document *d;

    /* Referrer Policy §8.3: an environment whose global is not a Window has its creation URL as the referrer
       source, which for a worker is its global scope's url. */
    if (!doc_of(ctx)) {
        char *src = strdup(worker_env_of(ctx)->url);
        CHECK(src != NULL, "document: OOM copying a worker's referrer source");
        return src;
    }
    dom = lxb_dom_interface_document(doc_here(ctx)->dom);
    d = doc_rec(dom);

    /* "If document's origin is an opaque origin, return no referrer." This instance is one origin-keyed agent
       cluster, so the agent's origin is the document's. */
    if (origin_is_opaque(origin_agent())) return NULL;
    /* "While document is an iframe srcdoc document, let document be document's browsing context's browsing
       context container's node document." A srcdoc document has its parent's origin, so that document is in
       this instance. */
    while (d->is_iframe_srcdoc) {
        JSValue container = window_proxy_container(ctx, d->proxy);
        lxb_dom_node_t *cn = node_of(container);

        JS_FreeValue(ctx, container);
        CHECK(cn != NULL && cn->owner_document != NULL,
              "an iframe srcdoc document has no navigable container — HTML §4.8.5 \"The iframe element\" makes "
              "it the document of an iframe's content navigable, so its container's node document is the "
              "referrer source Referrer Policy §8.3 names");
        dom = cn->owner_document;
        d = doc_rec(dom);
        CHECK(d != NULL, "an iframe srcdoc document's container is in a document with no record");
    }
    {
        char *src = strdup(document_url_of(dom));
        CHECK(src != NULL, "document: OOM copying a referrer source");
        return src;
    }
}

const char *document_content_type_of(const lxb_dom_document_t *dom)
{
    Document *d = doc_rec(dom);

    DCHECK(d != NULL, "a document's CONTENT TYPE was read from a document with no record — the string is set "
                      "once, by whichever of document_install and document_new created the document, and a "
                      "tree that came from neither never had one to answer with");
    return d->content_type;
}

bool document_is_xml_of(const lxb_dom_document_t *dom)
{
    Document *d = doc_rec(dom);

    DCHECK(d != NULL, "a document's §4.5 TYPE was read from a document with no record — the type is set once, "
                      "by whichever of document_install and document_new created the document, and a tree "
                      "that came from neither never had one to answer with. It is asked BEFORE any HTML "
                      "parse-boundary correction runs over a tree, so a caller reaching here with no record "
                      "is one whose correction would run over a tree nothing has classified");
    return d->is_xml;
}

DocumentInterface document_interface_of(const lxb_dom_document_t *dom)
{
    Document *d = doc_rec(dom);

    DCHECK(d != NULL, "DOM §4.5's INTERFACE was read from a document with no record — \"create a document that "
                      "implements an interface\" is what states it, and the two entries that run it are "
                      "document_install and document_new, so a tree that came from neither never received one. "
                      "It is read at the wrapper and by §4.4's clone, so a document reaching here with no "
                      "record is one whose wrapper would have no prototype to wear");
    return d->iface;
}

/* DOM §4.5 "Interface Document"'s `XMLDocument` interface prototype object, for this realm. Owned. */
static JSValue xml_document_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_xml_document_class);

    DCHECK(!JS_IsNull(proto),
           "XMLDocument.prototype was asked for in a realm that never ran document_install_proto — DOM §4.5 "
           "declares XMLDocument beside Document and the two prototypes are built in one breath, so a realm "
           "with one and not the other cannot exist; a document created implementing XMLDocument in such a "
           "realm would wear no prototype at all");
    return proto;
}

JSValue document_interface_proto(JSContext *ctx, const lxb_dom_document_t *dom)
{
    /* Which of DOM §4.5's two interfaces, asked of the record. The Document arm reads the node type table, which is
       where Document's prototype lives; a slot here would be a second answer to one question. */
    if (document_interface_of(dom) == DOCUMENT_IFACE_XML_DOCUMENT)
        return xml_document_proto(ctx);
    return node_type_proto(ctx, LXB_DOM_NODE_TYPE_DOCUMENT);
}

/* DOM §4.9 "Interface Element"'s create an element with the HTML namespace named (see document.h for the four
   sentences that name it, and why a creation states its namespace rather than reading a document field this
   engine does not keep). It spells the namespace URL once, so the four call sites cannot drift about it. */
lxb_dom_element_t *document_create_element_html(lxb_dom_document_t *dom, const char *local, size_t local_len)
{
    DCHECK(dom != NULL && local != NULL && local_len != 0,
           "§4.9's create-an-element was asked for an element with no document or no local name — every one of "
           "the four algorithms that reach this passes a literal name and the document its own step names");
    /* element_create_ns never returns NULL (core/dom/element.h) and interns the three names as given, so a `body`
       created here is not folded through a hash that already answers LXB_NS_HTML for a namespace that is not the
       HTML namespace. */
    return element_create_ns(dom, DOC_NS_HTML, sizeof(DOC_NS_HTML) - 1, local, local_len, NULL, 0);
}

void document_install(JSContext *ctx, JSValueConst global, lxb_html_document_t *dom, const char *url,
                      DocumentKind kind, SerializedPolicyContainer policy,
                      SerializedResponsePermissionsPolicy permissions_policy, SandboxFlags sandbox_flags,
                      uint32_t doc_id, JSValueConst nav_proxy, const char *about_base_url,
                      bool is_iframe_srcdoc)
{
    Document *d;
    JSValue doc;

    DCHECK(dom != NULL, "the Document install was handed no parsed document");
    /* First, and before the no-address return below: the policy is a property of the parsed tree, not of the
       address, and §7.4 clones it for an about:blank child when that child is created, which can be a boot
       script's first act.
       Once per realm, on the baseline. A realm is a document, so a second install into one is a navigation, whose
       container is per flow: the navigating flow sees the new policy and its siblings the old one. Replacing it
       here would answer for whichever flow ran last, so the assert names the COW record to build. A second
       Document is a second realm and does not come through here twice. */
    DCHECK(doc_of(ctx) == NULL,
           "a document was installed twice into one realm — that is a NAVIGATION, and its container is "
           "per-flow state: build it as a COW record (like ProxyData's PROXY_REC) captured in its accessor, so "
           "the flow that navigated and the sibling that did not each read their own");
    /* The interface is Document for every document this entry builds, so it is a constant, not an argument. HTML
       §7.5.1 "Shared document creation infrastructure", which every navigation's Document comes through (§7.5.2
       "Loading HTML documents", §7.5.3 "Loading XML documents" and §7.5.4 "Loading text documents" included), says
       "Let document be a new Document, with: type type, content type contentType, …", and §7.3.2.1 "Creating
       browsing contexts"' initial `about:blank` says the same. §7.5.3 is where a reader expects XMLDocument, but its
       text is "create and initialize a Document object document, given \"xml\", type, and navigationParams": the
       `xml` there is DOM §4.5's type. */
    d = doc_rec_new(ctx, dom, url, DOCUMENT_IFACE_DOCUMENT, kind, about_base_url, is_iframe_srcdoc);
    d->doc = doc_id;
    /* CSP §2.2's self-origin becomes a record here, where a document's facts stop being bytes a host stated and
       become the types the algorithms are written over. origin_parse is core/url/origin.h's transport for this:
       "null" states an opaque origin and mints one, which is what a sandboxed document's `'self'` is measured
       against and is same origin with nothing. */
    DCHECK(serialized_policy_container_exists(policy),
           "a Document was installed with NO §7.1.7 POLICY CONTAINER — every Document has one, and CSP §2.2 "
           "gives its CSP list a self-origin which §2.2.2 states from the response's URL; a document without "
           "one cannot resolve `'self'` and would report every one of its own scripts as blocked by its own "
           "policy. §7.1.7's determine step answers this for every creating operation and always with a "
           "container, so an absence here is a caller that did not run it");
    d->policy = document_policy_new(dom, policy.csp, policy.csp_report_only, origin_parse(policy.self_origin),
                                    policy.embedder, policy.integrity_policy);
    /* §7.1.5's active sandboxing flag set, as the creating operation decided it: §7.2's creation flags for the
       initial about:blank, §7.4.5's final flag set for a navigated Document. Beside the policy container because
       §7.5.1 hands the Document both together, and not derived from it: the container contributes only the CSP
       `sandbox` directive, and which algorithm unions it in is a fact about the operation that no inspection of the
       container recovers. */
    d->sandbox_flags = sandbox_flags;
    /* §7.1.7's referrer policy item as the creating operation stated it (NULL keeps the initial default), then
       HTML §4.2.5.1's meta `referrer` for each `<meta>` the parser inserted, in tree order, which is their
       insertion order; "only the most-recently-inserted or most-recently-modified meta element in this state
       has an effect". A meta a script inserts or edits later reaches document_meta_referrer_changed. */
    if (policy.referrer_policy) policy_container_set_referrer_policy(d->policy, policy.referrer_policy);
    {
        lxb_dom_node_t *cur;
        for (cur = lxb_dom_interface_node(dom)->first_child; cur; ) {
            if (cur->type == LXB_DOM_NODE_TYPE_ELEMENT) document_meta_referrer_changed(lxb_dom_interface_element(cur));
            if (cur->first_child) { cur = cur->first_child; continue; }
            while (cur && !cur->next) cur = cur->parent;
            if (cur) cur = cur->next;
        }
    }
    /* The realm's active document from here on, set before the early return below because the policy is already
       built and §7.4 clones it for an about:blank child whether or not this document has an address. */
    JS_SetContextOpaque(ctx, d);
    /* The document-to-realm direction is not recorded here. A realm is per-flow state, so two timelines that each
       materialize one srcless navigable's initial about:blank reach this line for one document, and a row with one
       slot per document would answer for one of them. core/frame/window_proxy.h's window_proxy_realm_of_document
       answers it from the navigable, for the asking timeline; solver/world.c states the argument. */
    /* §7.2.3: a navigable has one WindowProxy, and the navigable exists before its realm (§7.4 created it, named it
       and handed its proxy to the page), so the caller that owns the navigable supplies it: the host for the root
       one it named, §7.4 for every child. A proxy minted here would carry no parent and no opener, so a child's
       `parent` would answer itself and a popup's `opener` null. Before the early return below: a document with no
       address still has a navigable, and `window.closed` reads the navigable's state through this object. */
    DCHECK(window_proxy_is(nav_proxy),
           "a Document was installed for a realm with no navigable — §7.2.3's proxy belongs to the navigable "
           "and the navigable exists before its realm, so the caller that owns it passes it in");
    d->proxy = JS_DupValue(ctx, nav_proxy);
    /* HTML §8.1.3.1's "create a new realm" gives a Window realm its WindowProxy as the global this value, which
       ECMAScript §9.6 InitializeHostDefinedRealm step 7 takes from the host: this realm's top-level `this`,
       `globalThis` and §7.2.2's `window` are this object. Before any of the Document's code can run. */
    JS_SetGlobalThisValue(ctx, JS_DupValue(ctx, nav_proxy));
    /* Permissions Policy §9.5 "Create a Permissions Policy for a navigable", run here because §9.5 runs once per
       Document a navigable is given, and the navigable is the argument beside it. Before the no-address return for
       the policy container's reason: a Document with no address still has a navigable and a container, and a
       script in it still asks whether it is allowed to use a feature. Its inputs are the container's and are read
       now: §7.3.1.3 severs a navigable's container when the element is removed, and a policy derived after that
       would be a top-level document's. */
    d->permissions_policy = document_create_permissions_policy(ctx, nav_proxy, permissions_policy.enforced);
    /* §10.1's insertion into §7.5.1, the same algorithm with its report-only argument true: "let
       reportOnlyPermissionsPolicy be the result of calling Create a Permissions Policy for a navigable from
       response given navigationParams's navigable's container, navigationParams's origin, navigationParams's
       response, and True", and in step 10 "set the new Document's report-only permissions policy to
       reportOnlyPermissionsPolicy". Beside the enforced one because §9.10 reads both for one question. */
    d->report_only_permissions_policy =
        document_create_permissions_policy(ctx, nav_proxy, permissions_policy.report_only);
    /* §7.3.2.1 "Creating browsing contexts" runs the two ancestor-origins step lists here, beside §9.5 and for the
       same reason: both are snapshots of the container relation, read off `nav_proxy` while it still names the tree
       this Document was created in. */
    doc_create_ancestor_origins(ctx, d, nav_proxy);
    if (!url || !*url)
        return;   /* no address, no Document — the page's own throw is the honest answer */

    /* §4.4 `interface Document : Node`: the document object is the document node's wrapper, in the one identity
       table, with its members on Document.prototype (see document_install_proto). */
    doc = node_wrap(ctx, lxb_dom_interface_node(dom));
    CHECK(JS_IsObject(doc), "the Document wrapper allocation failed");
    d->impl = dom_implementation_new(ctx, doc);   /* §4.5's [SameObject], built with the document */
    /* Selection API §2's unique selection, built with the document for the reason §6.6's focused-area record is
       built with the realm: a selection minted on the first `getSelection()` would be minted inside whichever flow
       read first, not in the baseline every flow forks from. Only this install builds one; document_new's documents
       have no browsing context, which is §4.1's null. */
    d->selection = selection_new(ctx, doc);

    /* Web IDL §3.7.6 makes an attribute an accessor and §3.4.10's [LegacyUnforgeable] makes this one
       non-configurable, so `window.document` has a getter and a page cannot replace the binding. */
    idl_install_value_attribute(ctx, (JSValue)global, "document", JS_DupValue(ctx, doc), IDL_ATTR_UNFORGEABLE);
    /* Held, not borrowed: `doc` is this function's reference and the global got a dup, so the record owns one of
       the two and document_free releases it. It and `win_obj` are held across the lifecycle (`DOMContentLoaded`
       fires at the Document and `load` at the window long after install returns), and an unreleased one keeps the
       whole page's object graph alive. */
    d->doc_obj = doc;
    /* HTML §3.1.5: a Document created by "create and initialize a Document object" is at "loading" before any
       script can observe it, which the realm's record already says (document_install_proto builds it there), so
       this asserts it rather than writing it. */
    DCHECK(document_readiness(ctx) == 0,
           "a Document was installed into a realm whose readiness had already moved — the readiness record is "
           "built at \"loading\" with the realm and only §3.1.5's update writes it, so a realm that is past "
           "\"loading\" before it has a Document has run a lifecycle for a document it did not have");
    d->win_obj = JS_DupValue(ctx, global);
    /* HTML §7.4.1's first session history entry for this navigable, which §7.4.5 populates and §7.4.6 activates for
       a document a load produced, reached directly because the load is what built this document. Here rather than
       in the per-realm install, as core/frame/location.c reads the address at the call: a realm's intrinsics are
       built before its navigable has a document, and an entry holds that document's URL, id and origin. All three
       exist by this line, which is still the pre-boot baseline, so the entry belongs to every flow. */
    session_history_install_document(ctx);
    /* The interface objects, now that every prototype exists. Node's goes first because the derived ones inherit
       from it; each component names the one it owns rather than node.c enumerating them. */
    node_install_interfaces(ctx, global);
    {
        JSValue ep = element_proto(ctx);
        node_install_interface(ctx, global, "Element", ep);
        JS_FreeValue(ctx, ep);
    }
    html_element_install(ctx, global);   /* HTMLElement and every per-tag interface object */
    svg_element_install(ctx, global);    /* SVG 2's SVGElement and SVGAnimatedString */
    cssom_install(ctx, global);          /* CSSStyleDeclaration, and getComputedStyle on the Window */
    css_style_sheet_install(ctx, global); /* CSSOM §6.1.1 StyleSheet and §6.1.2 CSSStyleSheet */
    style_sheet_list_install(ctx, global); /* CSSOM §6.2.2 StyleSheetList */
    media_list_install(ctx, global);     /* CSSOM §4.4 MediaList, which §7.3's `media` hands back */
    /* CSS Fonts 4 §12.2's CSSFontFeatureValuesMap, whose seven attributes on a `@font-feature-values` rule hand
       one back. It has no constructor, so the interface object is here only for the brand a page reads with
       `instanceof`. */
    css_font_feature_values_install(ctx, global);
    css_rule_install(ctx, global);       /* CSSOM §6.4's five rule interfaces, §7.2 and §7.3 included */
    css_rule_list_install(ctx, global);  /* CSSOM §6.4.1 CSSRuleList */
    custom_elements_install(ctx, global);   /* §4.13.4 window.customElements */
    element_internals_install(ctx, global);  /* §4.13.7 ElementInternals, CustomStateSet, ValidityState */
    dom_token_list_install(ctx, global);    /* §7.1 DOMTokenList */
    node_filter_install(ctx, global);       /* §6.3 NodeFilter, the constants every traverser is read with */
    node_iterator_install(ctx, global);     /* §6.1 NodeIterator */
    tree_walker_install(ctx, global);       /* §6.2 TreeWalker */
    range_install(ctx, global);             /* §5.3 AbstractRange, §5.4 StaticRange, §5.5 Range */
    selection_install(ctx, global);         /* Selection API §3 Selection, its interface object */
    collections_install(ctx, global);       /* §4.2.10.1 Interface NodeList, §4.2.10.2 Interface HTMLCollection */
    mutation_observer_install(ctx, global); /* §4.3.1 MutationObserver, §4.3.3 MutationRecord */
    attr_install(ctx, global);              /* §4.9.1/§4.9.2 NamedNodeMap and Attr */
    document_fragment_install(ctx, global); /* §4.7 DocumentFragment, which is constructible */
    shadow_root_install(ctx, global);      /* §4.8 ShadowRoot */
    document_type_install(ctx, global);     /* §4.6 DocumentType */
    dom_implementation_install(ctx, global);/* §4.5.1 DOMImplementation */
    {
        /* §4.5 declares a constructor, so `Document` is not one of the interface objects whose call is the shared
           "Illegal constructor" throw. */
        JSValue dp = node_type_proto(ctx, LXB_DOM_NODE_TYPE_DOCUMENT);
        node_install_interface_ctor(ctx, global, "Document", dp,
                                    idl_step_constructor(ctx, "Document", g_id_doc_ctor));
        JS_FreeValue(ctx, dp);
    }
    {
        /* DOM §4.5's other interface object, `[Exposed=Window] interface XMLDocument : Document {};`. It declares no
           constructor, so `new XMLDocument()` is Web IDL §3.7.1 "Interface object"'s "Interface objects whose
           interfaces are not declared with a constructor operation will throw when called, both as a function and
           as a constructor", which is idl_interface_object's shared throw.
           Its [[Prototype]] is the `Document` interface object, written here because node_install_interface_ctor
           answers `Node` for every base. §3.7.1: "Let constructorProto be
           realm.[[Intrinsics]].[[%Function.prototype%]]. If I inherits from some other interface P, then set
           constructorProto to the interface object of P in realm." It is read back off the global, so the object
           linked to is the one the page reaches and `XMLDocument.__proto__ === Document` holds. Installed after
           `Document`, which the rule needs: the declaration-order dependency core/realm.h states for prototypes, one
           level up. */
        JSValue xdp = xml_document_proto(ctx);
        JSValue xctor = idl_interface_object(ctx, "XMLDocument", xdp);
        JSValue base = JS_GetPropertyStr(ctx, global, "Document");

        DCHECK(JS_IsFunction(ctx, base),
               "Web IDL §3.7.1's constructorProto for XMLDocument is \"the interface object of P in realm\" and "
               "this realm's global has no `Document` function to be it — the Document interface object is "
               "installed by the block directly above, so a failure here means that install stopped happening "
               "or this one moved in front of it");
        JS_SetPrototype(ctx, xctor, base);
        JS_FreeValue(ctx, base);
        JS_FreeValue(ctx, xdp);
        idl_define_global_property_reference(ctx, global, "XMLDocument", xctor);
    }
    /* HTML tree construction produces attributes in the null namespace; lexbor stamps them with the element's
       namespace instead, and only here, on the tree the parse just built, are the two distinguishable. Only for a
       tree an HTML parser built, decided by the Document's recorded §4.5 type: HTML §7.5.2 "Loading HTML documents"
       and §7.5.4 "Loading text documents" create an HTML parser, so their trees carry §13.2.6.1's boundary; §7.5.3
       "Loading XML documents" creates an XML parser, whose attribute namespaces are Namespaces in XML §6.2's
       expansion, already computed from the bindings in scope. core/html/domparser.c makes the same split for
       §8.5.1's two arms; the assertion inside the walk closes it, so a third route into it fires. Over an XML tree
       it is not a no-op: an `id` or `class` attribute reaches the walk's DCHECK, because those are the element's
       cached attribute slots and the walk re-interns the name they are keyed on. */
    if (!document_is_xml_of(lxb_dom_interface_document(dom)))
        dom_attr_normalize_parsed(lxb_dom_interface_node(dom));
    /* HTML §4.2.3 for the tree the parser built, first among these walks. A browser sets the frozen base URL as
       tree construction inserts the element, so a markup `<base href>` is in force before anything resolves a URL,
       and every walk below resolves one (§4.8.5's iframe `src`, §4.8.11.2's media `src`, the script inventory's
       `<script src>`). After dom_attr_normalize_parsed, because the `href` this reads is an attribute in the null
       namespace, which that call makes true. */
    html_base_element_parsed(ctx, lxb_dom_interface_node(dom));
    /* HTML §4.12.1.1's `force async`, "set to false by the HTML parser and the XML parser on script elements they
       insert". A lexbor parse has no per-token hook, so the stamp happens on the built tree, before the document's
       first script can read `async`. Not inert: a document parse's scripts run. Before the declarative-shadow
       conversion, which moves a `<template>`'s contents into a shadow root, because a `<script>` among them belongs
       to the tree it ends up in. */
    html_script_parsed(ctx, lxb_dom_interface_node(dom), /*inert*/false);
    /* HTML §13.2.6.4.4's template start tag, for the same tree and reason: `<template shadowrootmode>` attaches a
       shadow root to its parent during tree construction, a step a lexbor parse lacks, so the parsed tree's
       declarative shadow roots are attached here, before the first script can read one. Before the iframe walk,
       because an `<iframe>` among the moved contents belongs to the tree it ends up in.
       "Allow declarative shadow roots" is the document's, and this function installs only documents a navigation
       produced, whose "read html" parser has the flag true. The documents whose flag is false
       (`createHTMLDocument`, `DOMParser`, XHR's `responseXML`) are built by document_new and never reach here, so
       they keep their `<template>` elements. */
    declarative_shadow_parsed(ctx, lxb_dom_interface_node(dom),
                              document_document_element_of(lxb_dom_interface_node(dom)), /*allow*/ true);
    /* HTML §4.8.11.2 for the tree the parser built: "if a media element is created with a src attribute, the user
       agent must immediately invoke the media element's resource selection algorithm". §13.2.6.1's "create an
       element for a token" ends by appending the token's attributes, so a parsed `<video src=x>` is created with
       one; a lexbor parse has no per-token hook, so the invocation happens here, before the first script can read a
       networkState. It is not the insertion steps, which HTML uses for `<source>` and deliberately not for this
       (see media_element.c). After the declarative-shadow conversion: the walk is shadow-including, and a
       `<video src>` in a `<template shadowrootmode>` is in the shadow tree by now. */
    media_element_parsed(ctx, lxb_dom_interface_node(dom));
    /* HTML §4.8.4.3.2 for the same tree and reason: the list of relevant mutations counts an `img`'s `src` being
       set and the img element insertion steps, and a parsed element reaches neither (its attributes arrive with
       §13.2.6.1's "create an element for a token" and its insertion is the parser's). This walk is what makes a
       document's own `<img src=…>` markup issue its request and gives its markup `onerror` something to fire it. */
    html_image_parsed(ctx, lxb_dom_interface_node(dom));
    /* HTML §4.2.4 for the same tree and reason: §4.6.8.20's appropriate times are stated over an element that
       "becomes browsing-context connected", and a parser-inserted `<link>` becomes connected through the parser,
       not the mutation chokepoint, so the markup's `<link rel=preload>` elements are found here. After the
       declarative-shadow conversion, as the media and image walks are.
       It inventories and does not trigger. §4.2.4.3 "Fetching and processing a resource from a link element" ends
       in a fetch with no task or microtask before it, a fetch parks on a flow's own pending register, and this line
       is the pre-boot baseline, where there is none. The image walk reaches its fetch through §4.8.4.3.5 "Updating
       the image data"'s microtask, which the scheduler makes a flow; §4.2.4.3 has no such step, and inventing one
       would reorder the request against the rest of the parse. So the elements are recorded and this document's
       first flow serves them before it runs a program (core/html/html_link.h states the pair). */
    html_link_parsed(ctx, lxb_dom_interface_node(dom));
    /* HTML §4.2.6 for the tree the parser built: "the element is popped off the stack of open elements of an HTML
       parser" is update a style block's first trigger, and a Lexbor parse has no per-token seam, so the markup's
       own `<style>` elements get their CSS style sheets here. After the declarative-shadow conversion, for the
       media walk's reason: a `<style>` inside a `<template shadowrootmode>` is by now a style element in a shadow
       tree, and its sheet is the shadow root's. */
    html_style_element_parsed(ctx, lxb_dom_interface_node(dom));
    /* HTML §4.11.4 for the tree the parser built: the dialog HTML element insertion steps, which establish a
       `<dialog open>`'s §6.10.2 close watcher. A parser-inserted `dialog` becomes connected through the parser
       rather than the mutation chokepoint, and its `open` attribute arrives with §13.2.6.1's "create an element for
       a token", so neither of §4.11.4's two script-side hooks sees it. This walk gives the markup's own
       `<dialog open>` the watcher an Esc and `requestClose()` need (an open, connected, fully active dialog asserts
       it has one). After the declarative-shadow conversion, as the media and style walks are: the walk is
       shadow-including. */
    html_dialog_parsed(ctx, lxb_dom_interface_node(dom));
    /* HTML §8.1.8.1 "Event handlers" for the tree the parser built: its attribute change steps turn a markup
       `<body onload="init()">` or `<img onerror=…>` into a handler, and a lexbor parse writes its attributes through
       `lxb_dom_attr_set_value_wo_copy`, which reaches none of DOM §4.9's mutation chokepoint. This walk makes the
       markup the page shipped agree with `el.onclick = f`.
       After the declarative-shadow conversion, for the media and style walks' reason: an `<svg onload=…>` that was
       inside a `<template shadowrootmode>` is in a shadow tree by now. Before the iframe walk, because a child
       navigable's creation is the first thing in this function that can fire a `load` at something in this tree,
       and a handler installed after that dispatch would miss it. */
    event_handler_attribute_parsed(ctx, lxb_dom_interface_node(dom));
    /* HTML §4.8.5 for the tree the parser built. A browser runs the insertion steps during tree construction, so an
       <iframe> in the page's own markup has a child navigable before the first script runs. No document load here
       reaches those steps (§7.5.2's Lexbor parse never reaches the DOM chokepoint; §7.5.3's XML parse reaches it
       and is refused at the record by core/dom/element.c's tree_steps_can_run, since the realm this install builds
       does not exist until after the parse), so the parsed tree's iframes get their step here. After every wrapper
       and prototype exists, because creating a navigable wraps the element and stores a WindowProxy on it. */
    iframe_document_parsed(ctx);
    /* HTML §6.6.7 for the tree the parser built, for the iframe walk's reason: a browser runs the insertion steps
       during tree construction, so `<input autofocus>` in the page's own markup is a candidate before the first
       script runs. After the iframe walk, because an `<iframe autofocus>` is a candidate whose focusable area is its
       content navigable's active document. */
    autofocus_document_parsed(ctx);
    /* §13.2.7's load lifecycle is one slot on the frontier and an agent fact, so document_init claims it once and
       document_agent_free gives it back; no install claims it. */
}

/* The document-to-realm answer, which §4.2.3 needs and §3.7 explains. The insertion and removing steps belong to
 * the node's node document, not to whoever performed the mutation: two same-origin documents are one agent
 * (SECURITY.md's origin-keyed cluster), so `parentDoc.body` and `frame.contentDocument.body` are both writable from
 * one flow, and an <iframe> appended into the child's tree must create its navigable, prepare its scripts and
 * upgrade its custom elements in the child's realm. The mutating member's ctx is wrong for exactly the case the
 * cluster exists to allow.
 * It reads the Lexbor document's `user` slot, which holds the document's record, and the record names the realm
 * (see doc_rec): no registry to keep in step, and "which document" and "which realm" stay two questions, which
 * §4.5.1's three factories ask separately. */
JSContext *document_realm_of(const lxb_dom_node_t *n)
{
    Document *d = n ? doc_rec(n->owner_document) : NULL;
    return d ? d->realm : NULL;
}

/* The realm whose active document this document is, or NULL (see document.h for who asks and why it is not
   document_realm_of). */
JSContext *document_active_realm_of(const lxb_dom_node_t *doc)
{
    JSContext *realm;

    if (!doc || doc->type != LXB_DOM_NODE_TYPE_DOCUMENT) return NULL;
    realm = document_realm_of(doc);
    if (!realm) return NULL;
    return node_of(document_object(realm)) == (lxb_dom_node_t *)doc ? realm : NULL;
}

void document_free(JSContext *ctx)
{
    Document *d = doc_of(ctx), *c, *next;

    if (!d) return;   /* a realm that never had a document: the runner builds one per component test */
    /* The realm stops naming its records before the first one is released. Each line below drops a counted
       reference, which can end a refcount cascade that runs finalizers, so between the first free and the last
       there must be no chain for document_realm_mark to walk, or a collection landing mid-teardown reads freed
       records. Clearing the opaque here makes that window empty; `doc_of(ctx)` answers NULL for a realm whose
       document is going. */
    JS_SetContextOpaque(ctx, NULL);
    /* The documents this realm created at baseline go first: each holds a wrapper of the active document's realm
       and owns a whole Lexbor tree, and the chain hangs off the active record. */
    for (c = d->next_created; c; c = next) { next = c->next_created; doc_rec_free(ctx, c); }
    /* No world registry row names this realm. core/frame/window_proxy.c answers document-to-realm from the
       navigable, whose `realm` field is borrowed from the proxy's own `window`, so the realm torn down here is alive
       exactly while a navigable names it and no freed JSContext is reachable through a stale row. The documents
       released above have no browsing context, so no navigable names them and no peer can reach them. */
    doc_rec_free(ctx, d);
}

/* The agent's half, undone: core/platform.h's third column, and `document` is the one component with both halves.
 * document_free above is the per-realm half (it reads `doc_of(ctx)`, clears that realm's opaque and releases that
 * realm's records, once per realm); this one answers for the agent: the classes, the member declarations, the
 * realm-value slot ids, this component's claim on the frontier, and the sub-components document_init declares.
 * The order between the halves is not this column's to decide. A child navigable's document is released from
 * quickjs's realm-teardown hook (core/frame/navigable.c), which fires when the JSContext dies, inside JS_RunGC or
 * JS_FreeRuntime, both after platform_agent_free. So a page with one <iframe> has records that outlive the
 * platform, and no assert demands they are gone. That is safe because document_free reads no static of this
 * file: it walks the record, releases the references doc_rec_refs names, frees the policy container and destroys
 * the trees the record owns, all reached through the record itself. */
/* §gc's mark hook is not given back here, which is asserted below. Its storage is `rt->ctx_mark`, the runtime's,
 * released by JS_FreeRuntime, so it is not a claim in the sense of core/platform.h's fourth paragraph (a slot in
 * another component, whose static outlives this release), and no static of this file holds it. Clearing it here
 * would be wrong: the collection a host runs after platform_agent_free tears down the child realms whose records
 * this hook reports, and a record whose references gc_decref cannot subtract is the uncollectable realm cycle
 * doc_rec_new's DCHECK prevents. */
void document_agent_free(JSRuntime *rt)
{
    /* Not `if (!g_document_rt) return;`: the release is the inverse of the declaration and rides the same row of
       core/platform.c's one list, whose declare pass is unconditional and whose table asserts that a release row
       has a declare. */
    DCHECK(rt == g_document_rt,
           "the Document component was released against a runtime that is not the one it declared in — every "
           "atom and every value the sub-components below give back would have its reference subtracted from a "
           "runtime that never took it, and JS_FreeValueRT reports nothing at all when it is the wrong one");
    /* The one claim this component made in another, given back first. §13.2.7's load lifecycle is a slot on the
       frontier (solver/engine.c), which solver_agent_free releases after the whole platform, so keeping it would
       leave the scheduler holding a callback into a component whose class and member declarations are gone (the
       defect core/agent_state.h records for Indexed Database §2.7.1). First, as element_free's two DOM-write hooks
       are, because the very next call can run a finalizer. */
    engine_set_document_done_hook(NULL);
    engine_set_parse_end_hook(NULL);
    /* The sub-components document_init declares, in the reverse of that order, by platform_agent_free's rule: a
       component declared after another may hold what that one minted. document_fragment, shadow_root and slot are
       declared only by document_init, so only this releases them. */
    fullscreen_free(rt);
    unknown_markup_free();
    autofocus_free();
    focus_free();
    render_blocking_free();
    page_visibility_free();
    selection_free();
    dom_implementation_free();
    document_type_free();
    slot_free(rt);
    shadow_root_free(rt);
    document_fragment_free();
    document_current_script_free();
    document_domain_free();
    document_write_free(rt);
    document_metadata_free();
    DCHECK(JS_GetContextMarkHook(rt) == document_realm_mark,
           "§gc's realm-mark hook was given back at the Document component's agent release — it must NOT be, "
           "and this is where that is checked. The records it reports outlive this call (a child navigable's is "
           "released from quickjs's realm-teardown hook, which fires inside JS_RunGC or JS_FreeRuntime), and "
           "each of them holds four counted references back INTO its own realm. With the hook gone gc_decref "
           "cannot subtract them, the realm reads as externally rooted, and the collection that would have "
           "freed the record is the one the record prevents — which is exactly the cycle doc_rec_new asserts "
           "the hook's presence to make impossible. Its storage is rt->ctx_mark and JS_FreeRuntime releases it");
    /* Every handle this row declared, given back from the one list that already names them: this component's
       recorded runtime, §4.5's two classes, its member declarations and the realm-value slot ids §3.1.5's readiness
       and §7.5.9's page showing live in, with everything the sub-components above declare under this row's name
       (§3.1.7's currentScript slot, Selection API §3's class and entries, Fullscreen §2's flag key and Symbol, §6.6's
       focused-area slot and members). core/agent_state.h's agent_state_undo is why this is the inverse of the
       declarations rather than a second copy: `document` is declared from several files, and a hand-kept list
       misses a declaration added to one `_init`. The prototypes are the realms', each released with its context.
       References stay above, with the reset that guards each free: the undo resets handles, never references, so
       fullscreen_free keeps its JS_FreeAtomRT/JS_FreeValueRT and its own reset, since the sub-releases after it
       would otherwise read a slot naming a freed atom or a dropped value; the undo re-nulls each idempotently.
       Last, and that order is the contract (core/agent_state.h): the assert at the top reads g_document_rt and the
       one above asserts the realm-mark hook was not handed back, so undoing first would answer its own check. It is
       called by this component rather than from core/platform.c's release column, so agent_state_check_released
       has something to catch. */
    agent_state_undo("document");
}
