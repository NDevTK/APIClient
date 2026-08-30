/* THE DOCUMENT INTERFACE — Blink core/dom, the members that can be answered truthfully today.
 *
 * WHAT IS HERE splits the same way Location does. `URL`, `documentURI`, `domain` and `title` are facts about
 * the document this engine actually parsed, so they are CONCRETE and a bundle that routes on them gets the real
 * value. `cookie` and `referrer` are INPUT — a cookie jar this engine was not given and a referrer the visitor
 * arrived with — so they are concolic, example-free, and a branch on either FORKS. `document.cookie` in
 * particular is the source that carries a session into a request URL, and collapsing it to "" makes every
 * cookie-gated path unreachable.
 *
 * WHAT IS NOT HERE is the tree: querySelector, getElementById, createElement, body, head. They need the Element
 * interface, which does not exist yet, and the honest answer is ABSENCE. A querySelector that returns null for
 * an element the document HAS is a lie the page cannot detect and this engine would report a surface it never
 * reached; a ReferenceError names the component to write. The DOM is parsed and sitting in `dom` — this
 * component holds it precisely so Element can be grown against it. */
#include <stddef.h>   /* offsetof — DocAddress's CowRecord layout names its owned value by it */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"
#include "solver/dom_cow.h"   /* dom_cow_note_created — a created node belongs to the flow's delta */
#include "solver/cow.h"       /* cow_capture_host_record — a Document's ADDRESS is per-flow state, and
                                 it OWNS a value, so it goes through the record arm and not the byte one */
#include "quickjs.h"
#include "solver/concolic.h"
#include "solver/engine.h"
#include "core/events/event.h"
#include "core/events/create_event.h"
#include "core/events/event_target.h"
#include "core/events/page_transition_event.h"
#include "core/events/report_exception.h"
#include "core/html/html_element.h"
#include "core/html/html_form.h"
#include "core/html/custom_elements.h"
#include "core/html/element_internals.h"
#include "core/html/html_iframe.h"
#include "core/html/declarative_shadow.h"
#include "core/html/media_element.h"
#include "core/html/html_image.h"
#include "core/html/html_link.h"
#include "core/html/html_style_element.h"
#include "core/html/dom_string_list.h"   /* §3.1.3's ancestor origins list IS a DOMStringList */
#include "core/html/html_script.h"   /* §4.12.1.1's `force async`: the stamp every parser makes */
#include "core/html/html_base_element.h"   /* §4.2.3's frozen base URL, whose two facts this record holds */
#include "core/url/url.h"                  /* §2.4.3's fallback base URL asks whether an address matches about: */
#include "core/dom/dom_token_list.h"
#include "core/dom/collections.h"
#include "core/dom/mutation_observer.h"
#include "core/dom/attr.h"
#include "core/dom/attr_list.h"
#include "core/dom/selector_match.h"
#include "core/css/css_style_declaration.h"
#include "core/css/css_rule.h"
#include "core/css/media_list.h"
#include "core/css/css_rule_list.h"
#include "core/css/css_style_sheet.h"
#include "core/css/style_sheet_list.h"
#include "core/encoding/encoding.h"   /* §3.1.1's encoding is an id in §4.2's registry, asked of it by name */
#include "core/dom/document.h"
#include "core/dom/document_current_script.h"
#include "core/dom/document_domain.h"
#include "core/dom/document_metadata.h"
#include "core/html/document_write.h"   /* §8.4's three members drive the PARSER, not the tree — see its header */
#include "core/html/html_parse.h"       /* §13.2.7's premise: the lifecycle advances when the PARSE has ended */
#include "solver/world.h"
#include "core/frame/window_proxy.h"
#include "core/frame/navigable.h"
#include "core/frame/location.h"
#include "core/frame/session_history.h"
#include "core/dom/page_visibility.h"
#include "core/html/autofocus.h"
#include "core/html/focus.h"
#include "core/dom/document_fragment.h"
#include "core/dom/shadow_root.h"
#include "core/dom/slot.h"
#include "core/dom/document_type.h"
#include "core/dom/dom_implementation.h"
#include "core/xml/xml_name.h"   /* §4.13 step 1's `Name` production is XML's, referenced by the DOM */
#include "core/dom/names.h"      /* §1.4's validate-and-extract — createElementNS's step 1, ONE implementation */
#include "core/idl_args.h"
#include "core/realm.h"

/* Every member here takes DOMStrings; createElementNS takes two. Declared, not masked. */
static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
/* `Element createElementNS(DOMString? namespace, DOMString qualifiedName, …)` — the FIRST argument is nullable
   and `createElementNS(null, "div")` is the ordinary way a page asks for the null namespace. Declared as a
   plain DOMString it was ToString'd, so the body received the four characters `null` and built an element
   whose namespaceURI was that string. Not shared with IDL_2STR: createProcessingInstruction's two arguments
   are both non-nullable, and one array across both would say the wrong thing about one of them. */
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
#include "core/dom/node_interface.h"   /* the ONE place a Document is made — see that header */

/* THE DOCUMENT'S OWN STATE, HELD ON THE REALM THAT IS THIS DOCUMENT — not on the file.
 *
 * AN AGENT IS A JSRuntime AND A DOCUMENT IS A JSContext IN IT, because that is what the two words mean. HTML
 * puts every same-origin document of one browsing-context group in ONE similar-origin window agent — one heap —
 * and gives each its own global; a JSRuntime is the heap and a JSContext is the global. So the state a document
 * HAS (its tree, its address, its policy container, its `document` object, its Window) hangs off the context.
 *
 * It was file-scope, and file-scope IS the sentence "one instance is one document" — which is the sentence this
 * design stopped making. Same-origin documents share a heap and the corpus RELIES on it:
 * `iframe.contentDocument.body.appendChild(subframe)` inserts a node THIS document created, and afterwards
 * `subframe.parentNode` is a node of the OTHER document while the wrapper stays the same object. There is no
 * name to pass there — it is one object graph.
 *
 * WHAT DOES NOT LIVE HERE is anything the whole agent shares: a class id, a prototype, an interface object, and
 * the ORIGIN — an agent is origin-keyed, so every document in it has the same one, which is exactly why
 * SECURITY.md's one-principal-per-instance still holds word for word. */
/* A REALM IS THE ACTIVE DOCUMENT OF ONE NAVIGABLE — AND IT IS NOT THE ONLY DOCUMENT IN IT.
 *
 * §4.5's createHTMLDocument, createDocument and `new Document()` build a Document that has NO browsing context:
 * no navigable, no Window, no WindowProxy, and no scripts of its own. HTML's similar-origin window agent is one
 * heap, so such a document is neither a second realm nor a second instance — it is a second tree in this one,
 * and its nodes are ordinary objects of this realm (`foreignDoc.createElement("p") instanceof Element` holds,
 * and its wrapper's prototype is THIS realm's).
 *
 * SO THE RECORD IS PER DOCUMENT, NOT PER REALM, and it hangs off the Lexbor document's own embedder slot — the
 * same slot that used to hold the realm pointer, which was already the (document -> realm) answer §4.2.3 needs.
 * One indirection more buys every per-document fact: the address `baseURI` reads, the content type, and §4.5's
 * `[SameObject] implementation`. The realm's context opaque still names its ACTIVE document, because that is a
 * fact about the realm and the answer to "the API base URL" and "who fires load".
 *
 * WHAT IS PER NAVIGABLE stays on the active document's record and is UNDEFINED on every other: a document with
 * no browsing context has no Window and no WindowProxy, which is exactly what §3.1.1's `location` returning
 * null for one means. */

/* A DOCUMENT'S ADDRESS — DOM §4.5 Interface Document's `URL`, and NOT its base URL.
 *
 * TWO FIELDS BECAUSE AN ADDRESS IS A VALUE AND NOT A STRING, and core/dom/document.h's document_url_value
 * states the whole of why: a router COMPUTES where it is going out of a value the run learned, so the address
 * carries a concrete EXAMPLE that solver/route_seed.h's declaration must take verbatim AND an unconstrained
 * DOMAIN that a later `location.pathname` branch must still fork on. One without the other is a wrong report
 * in one direction or a deleted arm in the other.
 *
 * `bytes` IS THE EXAMPLE, INTERNED (doc_addr_intern), never a buffer: DOM §4.5 states "Each document has an
 * associated encoding …, content type …, URL (a URL) …" and "The URL and documentURI getter steps are to
 * return this's URL, serialized", and a URL record's serialization has no length a fixed array could be sized
 * against. It was `char[2048]`, so a longer address was snprintf'd to a PREFIX and every base-URL resolution
 * in the page then resolved against that prefix with nothing able to notice.
 * NEVER NULL, and the empty string is this engine's "no address": a realm whose install returned before it had
 * one, which several readers below test for by exactly that.
 *
 * `value` IS WHAT THOSE BYTES CAME OUT OF — a plain JS string where the address is a concrete fact (a response
 * was fetched from it), the CONCOLIC the run computed where it is not. NEVER JS_UNDEFINED: both writers assert
 * it, so an absent value is never a state a reader has to guess about, and `concolic_is` is the one test that
 * separates the two.
 *
 * THEY ARE ONE STRUCT BECAUSE THEY ARE ONE CAPTURE, which is the strongest form of "they cannot drift": a
 * delta unapplies WHOLLY, so a context switch puts both back or neither, and there is no window in which one
 * flow's bytes sit beside another flow's value. That is also why this cannot go through the byte arm the
 * encoding and the base URLs use — solver/cow.h reserves that for a POD SCALAR LATCH, and a memcpy of a
 * JSValue makes a reference nothing counts, so the next restore would free a value the blob still names. */
typedef struct DocAddress {
    const char *bytes;
    JSValue     value;
} DocAddress;

/* …AND THE LAYOUT THAT SAYS WHICH OF ITS FIELDS IS COUNTED — solver/cow.h's CowRecord, the same statement of
   what a record OWNS that doc_rec_refs below is for the collector. One entry, because one field is a JSValue;
   the capture DUPs it and the delta frees it, and cow.c asserts the offset is in range, aligned and not
   repeated so a field added by copying the line above it cannot be freed twice. */
static const uint16_t DOC_ADDR_VALS[] = { (uint16_t)offsetof(DocAddress, value) };
static const CowRecord DOC_ADDR_REC = { sizeof(DocAddress), DOC_ADDR_VALS, 1 };

typedef struct Document {
    JSContext           *realm;    /* the realm this document belongs to — every document has exactly one */
    uint32_t             doc;      /* this document's handle in the world registry — its NAME is what crosses */
    lxb_html_document_t *dom;
    int                  owned;    /* this record destroys `dom` — true for a document the page CREATED */
    PolicyContainer     *policy;    /* owned; NULL for a document with no browsing context */
    /* Permissions Policy §9.5's PERMISSIONS POLICY for the navigable this Document was created in — owned,
       and NULL for a document with no browsing context, which is HTML §4.8.5's step 1.
       A SEPARATE FIELD FROM THE CONTAINER BESIDE IT, because it is not one of §7.1.7's items: a policy
       container holds a CSP list, an embedder policy, a referrer policy and integrity policies, and Permissions
       Policy §10.1 puts the permissions policy on the DOCUMENT instead. The two are also created by different
       operations from different inputs — §7.1.7's determine step picks a container, §9.5 walks the navigable's
       container element — so a record that folded one into the other would have to invent the walk. */
    PermissionsPolicy   *permissions_policy;
    /* HTML §7.1.5's ACTIVE SANDBOXING FLAG SET — a field of the DOCUMENT and not of the policy container
       beside it, which §7.1.7 gives a CSP list, an embedder policy, a referrer policy and two integrity
       policies and no flag set at all. §7.5.1's creation table lists the two on separate rows for that
       reason. Written exactly once, by the install below, from the set the creating operation decided; no
       standard writes it again, which is why it is a plain word rather than a captured record. */
    SandboxFlags         sandbox_flags;
    JSValue              doc_obj;   /* the `document` object — HELD, released by document_free */
    JSValue              win_obj;   /* this document's Window — HELD, and UNDEFINED with no browsing context */
    /* §7.2.3's ONE WindowProxy FOR THIS NAVIGABLE — `window`, `self`, and the `source` of every message this
       document posts. It lives on the REALM because that is what it is one of: a page comparing `e.source`
       across two messages must find the same object, and a table keyed by document would be an immortal root
       holding one proxy per navigable a forced-execution frontier ever created — thousands, none collectable.
       HELD, released with the realm. UNDEFINED for a document with no browsing context. */
    JSValue              proxy;
    /* §4.5's `[SameObject] readonly attribute DOMImplementation implementation`. SameObject is what makes this
       a field rather than a fresh object per read: a page holds `document.implementation` and compares it. */
    JSValue              impl;
    /* SELECTION API §2's UNIQUE SELECTION: "Every document with a browsing context has a unique selection
       associated with it … This one selection must be shared by all the content of the document (though not by
       nested documents)". A field for the same reason `impl` is one, and a stronger one: §3's whole state is on
       that object, so a fresh one per read would forget every range the page set. UNDEFINED for a document
       with NO browsing context, which is §4.1's "must return null otherwise" — the one place the two states
       differ, and the reason this is built by document_install and not by doc_rec_new. */
    JSValue              selection;
    /* HTML §3.1.3 "Ancestor origins" gives every Document an ANCESTOR ORIGINS LIST and, beside it, an
       INTERNAL ANCESTOR ORIGIN OBJECTS LIST. Two fields because the standard has two and they answer different
       questions: the internal one is a list of ORIGINS and is what a CHILD reads to build its own; the public
       one is the DOMStringList §7.2.4's `ancestorOrigins` hands to script, and it is [SameObject] for the life
       of the Document — `location-ancestor-origins-new-object.html` asserts that identity directly.
       BOTH ARE A SNAPSHOT taken at §7.3.2.1 "Creating browsing contexts", never a walk performed at the read:
       the list records the ancestors the Document was CREATED under, so a walk done later would report whatever
       a subsequent navigation left in the tree. That is also why removing the frame does not empty this list —
       it empties the ANSWER, because §7.2.4's first step finds no relevant Document at all.
       THE INTERNAL LIST IS HELD AS SERIALIZED ORIGINS, which is what all three of its consumers need: a child
       composing its own list appends these verbatim, the public list is their concatenation, and a child in a
       PEER INSTANCE is handed the finished list as text on the record that provisions it. An OPAQUE origin is
       the three bytes `null`, and the one place that distinction still matters is the masking comparison
       below, which is why that comparison asks the parent's origin RECORD and not this text — and it is also
       why the composition runs in the CREATOR for a cross-instance child rather than the list crossing and the
       steps running at the far end. A peer handed this text and the parent's serialized origin could not run
       step 10: two opaque origins serialize to the same three bytes and are NOT same origin, so it would mask
       an entry that is not the parent's whenever the parent is itself opaque. */
    JSValue              ancestor_origins;          /* §7.2.4's DOMStringList — [SameObject], UNDEFINED with no browsing context */
    JSValue              ancestor_origin_strings;   /* the internal list, as an Array of serialized origins */
    /* THE DOCUMENT'S ADDRESS — DOM §4.5 Interface Document's `URL`, and NOT its base URL. See DocAddress
       above: the bytes and the value they are the example of are ONE field because they are one fact, and
       because one capture is what makes them unable to disagree across a context switch. */
    DocAddress           addr;
    /* HTML §2.4.3's DOCUMENT BASE URL IS NOT THE ADDRESS, and this record used to hold only the address and
     * answer both questions with it — so every relative URL in a page shipping `<base href="/app/v2/">`
     * resolved against the wrong path, silently, with nothing in the engine able to notice.
     *
     * TWO FIELDS BECAUSE §2.4.3 IS TWO ALGORITHMS. `frozen`/`frozen_el` are §4.2.3's FROZEN BASE URL and the
     * base element it belongs to; `about` is §7.4's ABOUT BASE URL, which is what the FALLBACK base URL
     * answers with for a Document created at `about:blank` or `about:srcdoc` BY A CREATOR — the creator's own
     * base URL, without which a relative URL in a srcless frame resolves against `about:blank` and fails to
     * parse at all. The ADDRESS is the third and is the fallback's last step.
     *
     * `frozen_el` IS COMPARED AND NEVER DEREFERENCED. It answers one question — "is the element that is NOW
     * first in tree order the one this URL was frozen for" — and §4.2.3's own situations keep it live: a
     * removal that takes the frozen element out of the document runs the removing steps, which recompute and
     * replace it. So the pointer can never outlive its element, and nothing here would notice if it did,
     * which is the design rather than an accident: reading through it is what would make this a lifetime
     * problem, and there is no read.
     *
     * ONE STRUCT BECAUSE IT IS §2.4.3's ONE ANSWER, NOT BECAUSE IT IS ONE CAPTURE — it used to be the latter
     * and the two byte arrays that made it so are gone. Each field is now a POINTER, so each is a POD SCALAR
     * captured on its own (solver/cow.h reserves the byte arm for a scalar latch, and a 4 KB struct through it
     * is the record-as-bytes that arm's assert names). `frozen_el` and `frozen` are ONE FACT and are captured
     * as two entries, which is not a weaker statement: a delta unapplies wholly, so both pointers go back
     * together or neither does. A flow that appends a `<base href>` changes where every subsequent URL in ITS
     * world resolves and in no sibling's. */
    struct {
        lxb_dom_element_t *frozen_el;
        const char        *frozen;   /* NULL exactly when `frozen_el` is — the pair is one fact */
        const char        *about;    /* NULL IS §7.4's null about base URL */
    } base;
    /* EVERY ADDRESS THIS RECORD HAS EVER HELD — see doc_addr_intern. The three fields above point INTO this
       list and nowhere else, and nothing in it is freed before the record is. */
    char               **addrs;
    int                  n_addrs;
    int                  cap_addrs;
    char                 content_type[32];   /* §4.5 contentType — what this document was created as */
    /* §4.5's OTHER creation fact: "A document is said to be an XML document if its type is `xml`; otherwise
       an HTML document." A FIELD AND NOT A COMPARE AGAINST THE BYTES ABOVE — see DocumentKind in document.h
       for HTML §7.5.4's text document, which is an HTML document whose content type is not "text/html" and
       which every such compare therefore answered backwards. */
    bool                 is_xml;
    /* HTML §3.1.1's "Each Document has an encoding (an encoding), used for the document's character encoding"
       — an id in the Encoding registry (core/encoding), and the fact HTML §4.12.1 falls back to when a
       `<script>` has no `charset` attribute: "let encoding be el's node document's the encoding". It is a
       FIELD and not a constant at each asker because it is per-document state that a navigation's response
       decides; what will WRITE it is HTML §13.2.3.2's encoding sniffing algorithm over the response's bytes
       and its `Content-Type` charset, which is where a document served as windows-1252 stops being UTF-8. */
    int                  encoding;
    /* HTML §4.12.3's two facts about a Document, which are what its "appropriate template contents owner
       document" is made of: the ASSOCIATED INERT TEMPLATE DOCUMENT, and whether this document IS one ("a
       Document created by this algorithm"). The second is what ends the recursion — an inert document is its
       own template contents owner — and it is a FIELD rather than a test on the tree because the algorithm
       states it as one: a document is created by that algorithm or it is not, and nothing about an empty
       Document can be inspected to tell. The inert document is on the realm's `next_created` chain like any
       other baseline creation; this names it so the second ask finds the first one's answer. */
    struct Document     *inert_template;
    int                  is_inert_template;
    /* THE REALM'S CHAIN OF DOCUMENTS IT CREATED AT BASELINE. A document a FLOW creates is owned by that flow's
       COW delta (dom_cow_note_created_document) and dies with it; one created while capture is off is baseline,
       exactly like a baseline node, and the realm that made it is what outlives it. Head on the active
       document's record, because that is the record the realm's teardown already reaches. */
    struct Document     *next_created;
} Document;

/* THE RUNNING REALM'S ACTIVE DOCUMENT. The context opaque, because a JSContext IS one navigable's document — so
   there is no table to look it up in and no way for the answer to be the wrong document's. NULL before install,
   which is a state only the accessors that tolerate it may see. */
static Document *doc_of(JSContext *ctx)
{
    return (Document *)JS_GetContextOpaque(ctx);
}

static Document *doc_here(JSContext *ctx)
{
    Document *d = doc_of(ctx);
    DCHECK(d != NULL, "a document member ran in a realm with no Document — document_install names which realm "
                      "a document is, and a realm that never had one cannot answer for a tree it has not got");
    return d;
}

/* ---- THE RECORD'S ADDRESSES: ONE PRODUCER, ONE RELEASE, AND NOTHING FREED IN BETWEEN ---------------------
 *
 * A Document's URL, its frozen base URL and its about base URL are each a URL record's SERIALIZATION, so none
 * of them has a length a fixed buffer could be sized against. Each is an allocated string this record owns.
 *
 * AND AN ADDRESS IS NEVER FREED ON A CHANGE, WHICH IS WHAT MAKES THE POINTER A LEGAL COW FIELD. The address is
 * per-flow state: a flow that ran `history.pushState(s, "", "/b")` captured the OLD pointer as its baseline,
 * and a context switch writes those eight bytes straight back. Freeing on the change would leave that entry
 * naming storage something else now occupies — so a change NEVER FREES, and the old string stays reachable
 * from this list for as long as the record does. That is also why the capture may hold the pointer at all: the
 * entry's target (`&d->addr`) already points INTO the record, so the entry is only valid while the record
 * lives, and every string this list holds lives exactly that long. The VALUE beside it in that same entry is
 * counted rather than borrowed — solver/cow.h's record arm dups it — which is the difference between a field
 * whose lifetime this list guarantees and one whose lifetime the runtime does.
 *
 * WHAT BOUNDS IT is the record's life, not a cap: a Document holds one string per DISTINCT address it has ever
 * had, and doc_rec_release frees all of them, once each, because this is the only producer and every slot it
 * adds is a fresh allocation. DISTINCT is what makes that a small number rather than a per-write one: §4.2.3
 * re-freezes a base URL on every situation that could have changed which element is first in tree order, so
 * the same serialization is re-stated constantly, and a string these bytes already name is the string to
 * return. That is sound because an address is IMMUTABLE once interned — two fields naming one string cannot
 * disagree, and a delta entry naming it is writing back a pointer that still means what it meant.
 * The alternative — refcounting a string by the delta entries naming it — is a new COW entry kind for owned
 * strings, and it buys back only the addresses of a document that outlives its own navigations, which the
 * record's life already bounds. */
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

/* IS THIS POINTER ONE OF THIS RECORD'S OWN ADDRESSES — the two-sided half of the intern above, and the reason
   the release can free every slot without asking whether some field still points at it. A field holding a
   BORROWED pointer is the bug this makes impossible to write: it would outlive its lender, and the record's
   release would either miss it or free storage it never owned. Linear, and it runs at address WRITES only. */
static int doc_addr_owned(const Document *d, const char *p)
{
    int i;

    for (i = 0; i < d->n_addrs; i++)
        if (d->addrs[i] == p) return 1;
    return 0;
}

/* EVERY ADDRESS THIS RECORD EVER HELD, GIVEN BACK — from doc_rec_release, the ONE place a record dies. The
   slots are distinct by construction: the intern is the only producer, it returns an EXISTING slot rather than
   re-adding one, and every slot it does add is a fresh allocation. So freeing the whole list frees each
   address exactly once even when two fields name the same string; the NULL-out is what makes a second pass
   over a slot visible instead of a double free. */
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

/* ---- THE RECORD'S COUNTED REFERENCES: ONE LIST, TWO CONSUMERS ------------------------------------------- */

/* A Document record is malloc'd C, so the four references it holds are invisible to every walk the runtime
   makes of itself — and two of them (`win_obj`, `proxy`) point straight back into the realm the record hangs
   off. That is a cycle with one edge the collector cannot see: gc_decref never subtracts these, the WindowProxy
   reads as externally rooted, gc_scan revives the Window and every function object behind it, and the realm can
   never be collected. Since the ONLY thing that releases the record is the realm's own teardown hook, the
   record was preventing the event that frees the record. Every child navigable's realm therefore lived to
   JS_FreeRuntime, whose gc_obj_list walk reported the whole page as a leak naming no owner — the largest single
   abort cause in web-platform-tests' html/browsers, and in the product a per-navigable heap that only grows for
   the life of the browsing session.
   THE LIST IS WRITTEN ONCE because the two consumers must agree EXACTLY. A field the release frees and the mark
   omits is the leak above coming back; a field the mark reports and the release does not own is worse — an
   over-subtracted refcount and a use-after-free. Two hand-kept lists is the drift CLAUDE.md's host-record rule
   names, so there is one list and neither consumer restates it. */
typedef void DocRefFn(JSContext *ctx, JSValue *pv, void *arg);

static void doc_rec_refs(Document *d, DocRefFn *fn, void *arg)
{
    fn(d->realm, &d->impl, arg);
    fn(d->realm, &d->selection, arg);
    /* §3.1.1's two ancestor-origins fields, both HELD and both here for the reason the list exists: they are
       created together at §7.3.2.1 and a field one consumer walks and the other does not is the exact pair of
       defects the paragraph above names. */
    fn(d->realm, &d->ancestor_origins, arg);
    fn(d->realm, &d->ancestor_origin_strings, arg);
    fn(d->realm, &d->proxy, arg);
    fn(d->realm, &d->win_obj, arg);
    fn(d->realm, &d->doc_obj, arg);
    /* THE ADDRESS'S VALUE — the half of DocAddress that is counted. It is here for the reason every other
       field is, and for one more: the COW record arm DUPS it into a parked flow's delta, so a value this list
       forgot would be over-released at the record's death while a delta still names it, and one this list
       reported without owning would be subtracted twice. DOC_ADDR_VALS is the SAME statement for the delta
       that this line is for the collector, and the two are read together for exactly that reason. */
    fn(d->realm, &d->addr.value, arg);
}

static void doc_ref_release(JSContext *ctx, JSValue *pv, void *arg)
{
    (void)arg;
    JS_FreeValue(ctx, *pv);
    *pv = JS_UNDEFINED;
}

/* A STRUCT AND NOT THE FUNCTION POINTER ITSELF THROUGH `void *`: converting a function pointer to an object
   pointer is not something C defines, and this file already carries the scar of a data pointer put where a
   function pointer belonged (§C-stack's JSCFunctionType rule). */
typedef struct { JSRuntime *rt; JS_MarkFunc *mark; } DocMarkArg;

static void doc_ref_mark(JSContext *ctx, JSValue *pv, void *arg)
{
    DocMarkArg *m = arg;

    (void)ctx;
    JS_MarkValue(m->rt, *pv, m->mark);
}

/* WHAT THIS REALM'S RECORDS HOLD, ANSWERED TO THE COLLECTOR — quickjs.h's JS_SetContextMarkHook, asked about
   every realm of the runtime from inside a collection.
   IT WALKS EXACTLY WHAT document_free WALKS: the realm's active record and the chain of documents that realm
   created at BASELINE, which is the whole set of records the realm's teardown releases. A record a FLOW created
   is owned by that flow's COW delta and is reachable from no realm, so it is neither walked here nor released
   there — the delta destroys it, and until it does its references correctly read as rooted from outside. */
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

/* THE RECORD FOR A DOM DOCUMENT — the answer to every per-document question, whatever realm is asking. NULL for
   a Lexbor document no record was ever built for, which is a solver scratch parse. */
static Document *doc_rec(const lxb_dom_document_t *dom)
{
    return dom ? (Document *)dom->user : NULL;
}

/* THE RECORD FOR THE DOCUMENT A MEMBER WAS CALLED ON. Every §4.5 member is `Document.prototype`'s, so its
   receiver names WHICH document it is about — `foreignDoc.createElement` must build its element in foreignDoc,
   and reading the realm's active document instead is the defect a second Document makes visible. */
static Document *doc_receiver(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);
    Document *d;

    /* WEB IDL §3.7.5: a member reached with a receiver that does not implement the interface is a TypeError,
       thrown at the read — `Object.getOwnPropertyDescriptor(Document.prototype, "URL").get.call(null)` is a
       thing the corpus does deliberately. It is NOT an engine invariant and so NOT a DCHECK: asserting it would
       turn a test that asks for the throw into an abort that takes the whole file with it. */
    if (!n || n->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
        JS_ThrowTypeError(ctx, "this is not a Document");
        return NULL;
    }
    d = doc_rec(lxb_dom_interface_document(n));
    DCHECK(d != NULL, "a Document member ran on a Lexbor document with no record — document_install and "
                      "document_new are the two places one is built, and a tree that came from neither cannot "
                      "answer for its own address");
    return d;
}

/* §4.2.6 / §4.9 THE SELECTOR MEMBERS, AS ONE MACHINE — querySelector, querySelectorAll, matches, closest.
 *
 * They were two implementations with two different defects, and the defects were the same shape twice: work
 * done per node that belongs to the query.
 *
 *   - qs_run reached lxb_selectors_find, which walks the whole subtree to completion inside one opcode. It is
 *     the most-called query in a modern page and it was the last drive-to-completion in this component.
 *   - document_sel_match CREATED AND DESTROYED a CSS parser, a selectors context and a compiled selector list
 *     on EVERY CALL — and `closest` calls it once per ancestor, so walking up ten levels compiled the same
 *     selector ten times.
 *
 * Compiling once and then walking is what both of them wanted, and it is also exactly what a machine needs: the
 * compiled list is the thing that survives the suspension, and the cursor is the resume point. So there is one
 * of these, and what the four members differ in is WHERE the cursor goes and WHAT is done with a match —
 * declared as a magic, not as four bodies.
 *
 * core/dom/selector_match.c is what makes the walk equivalent to lxb_selectors_find rather than an
 * approximation of it: a combinator is resolved by walking UP from the candidate, through the whole document,
 * so §4.2.6's scoped matching still holds — `el.querySelectorAll('div p')` finds a <p> under `el` whose <div>
 * ancestor is OUTSIDE `el`, because the selector is evaluated against the document and only the RESULTS are
 * filtered to the subtree. That is asserted rather than assumed; it is the case an implementation that walks a
 * subtree in isolation gets wrong.
 *
 * AND WHAT THIS MACHINE HOLDS ACROSS A REST POINT IS THE COMPILED SELECTOR AND A CURSOR — nothing else. It
 * held a live lxb_css_parser_t and an lxb_selectors_t as well, and DECLARED ITSELF UNFORKABLE because of them,
 * with a reason that was an argument about the PAGE: the walk runs no bytecode, so no branch can fork it. That
 * is not a property of the machine, and the scheduler's own reasons to take a snapshot of a parked flow — a
 * higher-value sibling, RAM pressure, a cold-tier eviction, a cross-session resume — never ask what the page
 * is doing. Neither object was state: the parser had finished compiling before the first rest, and the
 * matching arena cleans itself at the end of every match. Both moved to the component that owns them, the
 * compiled list is SHARED by reference between forked arms, and the declaration is gone. */
enum { QS_FIRST = 0, QS_ALL, QS_MATCHES, QS_CLOSEST };

/* WHERE THIS MACHINE RESTS, AS THE STANDARD NUMBERS IT. All four members are the same two steps: parse the
 * selector, then match it — §1.3 states them for querySelector and querySelectorAll (through scope-match a
 * selectors string) and §4.9 restates them for matches and closest, with the same wording and the same order.
 * The match is a walk of the page's tree, so it is a stage per NODE — it rests at every one, and the scheduler
 * is asked there rather than at the end of the walk.
 * THE PARSE IS ONE STAGE BECAUSE LEXBOR'S SELECTOR PARSER HAS NO SMALLER ENTRY, and that is a stretch of
 * engine execution proportional to the SELECTOR'S LENGTH, which is the page's to choose. It is not defended by
 * "no page code runs between steps 1 and 2" — that argument is the one this file's own machine had to unlearn
 * (see the header above), and it would justify a span of any size. Splitting it means feeding the CSS
 * tokenizer through lxb_css_syntax_tokenizer_next_chunk and holding it across the rests — which puts a live
 * lexbor tokenizer back into this machine's state and makes it unforkable again. So the two are ONE
 * subproblem, and it is the one frag_unforkable names: a lexbor tokenizer that can be COPIED. Until that
 * exists, a parkable compile and a forkable walk cannot both be had, and this machine keeps the forkable
 * walk. */
#define QS_STAGES(X) \
    X(QS_PARSE, "DOM §1.3 steps 1-2 / §4.9 steps 1-2 (parse a selector; SyntaxError if it is failure)") \
    X(QS_MATCH, "DOM §1.3 step 3 (match a selector against a tree) / §4.9 matches step 3 / closest steps 3-5, " \
                "one node per step")
enum { IDL_STEP_STAGE_BASE(QS_STAGES) QS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const QS_STEPS[] = { QS_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    SelectorList *compiled;   /* SELECTORS §5's parsed selector, SHARED by reference with every forked arm */
    lxb_dom_node_t *root, *cursor;
    JSValue arr;      /* QS_ALL's collected matches (owned) */
    uint32_t n;
} QsState;

/* EVERYTHING THIS MACHINE OWNS, and the whole of it — which is what makes it forkable. The compiled selector
   is refcounted and read-only, so the sibling arm takes a reference rather than a copy: nothing writes it
   after selector_list_compile built it, and the interior pointers a match takes into it therefore stay valid
   in both arms. The cursor and the root are borrowed tree pointers the COW delta already isolates. */
static void qs_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    QsState *s = st;
    v->val(ctx, &s->arr);
    v->shared(ctx, (void **)&s->compiled, s->compiled ? &s->compiled->refs : NULL, selector_list_destroy);
}

static int js_document_qs(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    QsState *s = st;
    int magic = idl_step_magic(hdr);

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    if (hdr->stage == QS_PARSE) {
        lxb_dom_node_t *n = node_of(hdr->this_val);
        const char *sel;

        if (!n || argc < 1) {
            *presult = magic == QS_ALL ? collections_static(ctx, JS_NewArray(ctx))
                     : magic == QS_MATCHES ? JS_FALSE : JS_NULL;
            return JS_STEP_DONE;
        }
        sel = concolic_name_cstr(ctx, argv[0]);   /* the declaration passes UNKNOWN input through as itself, so an unknown name denotes its SHAPE */
        if (!sel) return JS_STEP_ABRUPT;
        /* SELECTORS §5, AND IT IS OVER BEFORE THE FIRST REST POINT. The parser that compiles this lives and
           dies inside selector_list_compile; what comes back is the read-only list the walk below reads. */
        s->compiled = selector_list_compile(sel);
        JS_FreeCString(ctx, sel);
        if (!s->compiled) {
            /* §4.2.6 AND §4.9: an unparseable selector is a SyntaxError from ALL FOUR members. matches and
               closest already threw; querySelector and querySelectorAll answered null and an empty list, so a
               page with a typo in a selector got "no such element" instead of being told, and the branch behind
               that answer ran. */
            JS_ThrowDOMException(ctx, "SyntaxError", "not a valid selector");
            return JS_STEP_ABRUPT;
        }
        s->root = n;
        /* WHERE THE CURSOR GOES is the whole of what the four members differ in: a subtree for the two queries,
           the node itself for matches, and the node plus its ancestors for closest. */
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
            /* §4.2.6: a STATIC NodeList, because the spec says the result does not track the tree — and a real
               one, so `instanceof NodeList` holds and `.map` is honestly absent as it is in a browser. */
            *presult = collections_static(ctx, s->arr);
            s->arr = JS_UNDEFINED;
            break;
        case QS_MATCHES: *presult = JS_FALSE; break;
        default:         *presult = JS_NULL;  break;
        }
        return JS_STEP_DONE;
    }

    if (selector_match_node(s->cursor, s->compiled->list, NULL)) {
        switch (magic) {
        case QS_ALL:
            JS_SetPropertyUint32(ctx, s->arr, s->n++, node_wrap(ctx, s->cursor));
            break;
        case QS_MATCHES:
            *presult = JS_TRUE;
            return JS_STEP_DONE;
        default:
            *presult = node_wrap(ctx, s->cursor);   /* the FIRST in tree order, or the nearest ancestor */
            return JS_STEP_DONE;
        }
    }

    switch (magic) {
    case QS_FIRST:
    case QS_ALL:     s->cursor = node_next_in(s->cursor, s->root); break;
    case QS_MATCHES: s->cursor = NULL;                             break;   /* this node alone */
    default:         s->cursor = s->cursor->parent;                break;   /* INCLUSIVE ancestors */
    }
    return JS_STEP_YIELD;
}

/* NO `release`: everything this machine owns is a REFERENCE qs_visit names, so the teardown discharges the one
   declaration. The lexbor half a release used to own is gone — the parser died in the compile and the matching
   arena is the agent's — which is what left this machine with nothing outside its ownership list. */
static const IdlStepDecl QS_STEP = { js_document_qs, sizeof(QsState), qs_visit, NULL,
                                     "DOM §1.3 scope-match a selectors string / §4.9 Element.matches, closest",
                                     QS_STEPS };

const IdlStepDecl *document_qs_decl(void) { return &QS_STEP; }

/* §3.1.5 THE DOCUMENT'S ELEMENT SHORTCUTS — forms, images, scripts, embeds and links. Every one is a LIVE
   HTMLCollection the spec defines as "the elements of type X in the document", so each is the by-name
   collection over the document with a tag baked in, and `links` is the one that is a predicate instead
   (`a`/`area` WITH an href). A page uses these to find its own markup, and a bundle scanner uses
   `document.scripts` and `document.forms` in particular — with them absent the loop over them never ran and
   nothing said why.
   magic 0 = forms, 1 = images, 2 = scripts, 3 = embeds, 4 = links. */
static JSValue js_doc_shortcut(JSContext *ctx, JSValueConst this_val, int magic)
{
    static const char *const TAGS[] = { "form", "img", "script", "embed" };
    if (magic == 4) return collections_links(ctx, this_val);
    DCHECK(magic >= 0 && magic < (int)(sizeof(TAGS) / sizeof(TAGS[0])),
           "a document element-shortcut ran with a magic it does not have");
    return collections_by_name(ctx, this_val, TAGS[magic], false);
}

/* §4.5 createDocumentFragment(). A page batches inserts into one and attaches it once, which is the ordinary
   way to add many nodes — and it is the same object `new DocumentFragment()` builds, so this is the member
   name for a constructor that already exists rather than a second way to make one. */
static JSValue js_doc_create_fragment(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_document_fragment_t *frag;

    (void)argc; (void)argv; (void)magic;
    DCHECK(n != NULL, "createDocumentFragment ran on something that is not the document");
    frag = lxb_dom_document_fragment_interface_create(n->owner_document);
    CHECK(frag != NULL, "createDocumentFragment: the Lexbor fragment allocation failed");
    /* THIS FLOW MADE IT, exactly as its five siblings on this interface say of theirs. A fragment is detached
       and that is not the question the entry answers: the node came out of the DOCUMENT's Lexbor arena, so a
       flow that creates one and is then discarded — which is every flow, since a fragment is emptied by the
       insertion that consumes it and never becomes reachable from the tree — leaves it in that arena with no
       owner at all, invisible to the runtime's gc_obj_list walk because it is not a GC object. */
    dom_cow_note_created(lxb_dom_interface_node(frag));
    return node_wrap(ctx, lxb_dom_interface_node(frag));
}

/* 4.5.1 createElement. The element is created IN this document and returned DETACHED — a page builds a subtree
   and attaches it later, and creating it already-attached would put nodes in the tree the page never inserted.
   It is not a per-flow write for that reason: nothing observable changed until appendChild, which IS one. */
static JSValue js_doc_create_element(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    const char *tag;
    lxb_dom_element_t *el;
    Document *d;
    JSValue r;

    if (argc < 1) return JS_NULL;
    /* 4.5.1 OVER AN UNKNOWN TAG NAME. Lexbor needs real bytes to create an element and the coercion below owes
       them, so an unknown tag can only crash there — and `createElement(someParameter)` is a real pattern, and
       an XSS-relevant one (the tag decides whether the node executes). The answer is an unknown derived from
       the source rather than a node of a guessed name: a concrete tag would be an element the page never
       created, and every query and every sink after it would be about the wrong node. */
    if (concolic_is(argv[0])) {
        /* THE CREATION RUNS, on the tag this source concretely is. §solver allows no other way for an example
           to propagate — the engine performs the real operation on the concrete, never a rule that predicts
           what performing it would have produced — so this re-enters with the example in place of the operand
           and the element it really builds becomes the derived value's example. With no example there is
           nothing to run and the answer is honestly unknown, which is a non-answer rather than a guess. */
        JSValue ex = concolic_example(ctx, argv[0]), real = JS_UNDEFINED;
        if (JS_IsString(ex)) {
            JSValueConst a2[1];
            a2[0] = ex;
            real = js_doc_create_element(ctx, this_val, 1, a2, magic);
            if (JS_IsException(real)) { JS_FreeValue(ctx, JS_GetException(ctx)); real = JS_UNDEFINED; }
        }
        JS_FreeValue(ctx, ex);
        return concolic_builtin_hook(ctx, argv[0], "createElement", real);
    }
    tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return JS_EXCEPTION;
    /* §4.5 step 5's "create an element GIVEN THIS": the element's node document is the RECEIVER's, never the
       realm's active one — `foreignDoc.createElement("p")` builds a node of foreignDoc. */
    d = doc_receiver(ctx, this_val);
    if (!d) { JS_FreeCString(ctx, tag); return JS_EXCEPTION; }
    el = lxb_dom_document_create_element(lxb_dom_interface_document(d->dom),
                                         (const lxb_char_t *)tag, strlen(tag), NULL);
    JS_FreeCString(ctx, tag);
    dom_cow_note_created(el ? lxb_dom_interface_node(el) : NULL);   /* this flow made it: the delta owns it */
    DCHECK(el != NULL, "createElement produced no element — a page building its DOM would silently build "
                       "nothing and every query after it would answer null");
    r = element_wrap(ctx, el);
    return r;
}

/* DOM §4.5 createElement, AS THE ALGORITHM IT DELEGATES TO — §4.9 "create an element" with
 * `synchronousCustomElements` TRUE, which is the `true` that makes a custom element's constructor run INSIDE
 * document.createElement rather than at some later checkpoint.
 *
 * WHY IT IS A MACHINE. Step 5.1.4.1 is "constructing C with no arguments", and C is the page's class: its body
 * has loops, awaits and DOM mutations in it, so a JS_CallConstructor from here is the drive-to-completion the
 * engine aborts on and the class's own `super()` would reach a C entry with no flow base under it. A construct
 * is a request like every other, and the machine rests on it.
 *
 * AND STEP 5.1.4 IS "RUN THESE STEPS WHILE CATCHING ANY EXCEPTIONS", which is the whole of why the answer to a
 * throwing constructor is an element and not a propagating throw. The construct AND the checks after it are
 * inside that catch, so `class X extends HTMLElement { constructor(){ super(); this.attachInternals(); } }`
 * over an absent API — or one that returns the wrong node — REPORTS (fires `error` at the global, the page's
 * code again) and answers with an HTMLUnknownElement of the requested local name whose custom element state is
 * "failed". Propagating instead destroyed the whole document: eleven corpus files went from a real number to
 * no result at all on the diff that first made constructors run. The capability is DECLARED
 * (IdlStepDecl::catches_abrupt) rather than assumed, because the abrupt then arrives at this body's own
 * request site and every request this machine makes has to answer for it. */
#define DCE_STAGES(X) \
    X(DCE_LOOKUP,    "DOM §4.5 steps 1-5 into §4.9 steps 1-3 (the local name, and looking up a custom element " \
                     "definition for it) and §4.9 step 6 when there is none") \
    X(DCE_CONSTRUCT, "DOM §4.9 step 5.1.4.1 (constructing C with no arguments — the page's constructor)") \
    X(DCE_CHECKS,    "DOM §4.9 steps 5.1.4.2-11 (what the constructor returned, checked against what this " \
                     "operation asked for)") \
    X(DCE_REPORT,    "DOM §4.9 step 5.1.4's exception arm (report the exception the construct or the checks " \
                     "threw), which is HTML §8.1.4.6 report an exception")
enum { IDL_STEP_STAGE_BASE(DCE_STAGES) DCE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DCE_STEPS[] = { DCE_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* DOM §4.5's "FLATTEN ELEMENT CREATION OPTIONS", given `options` and the DOCUMENT the creation is for.
 *
 * It answers two things and step 3.2.1 is why they are one algorithm: naming BOTH an `is` and a
 * `customElementRegistry` is a NotSupportedError, because a customized built-in's definition can only come from
 * the registry its document already resolves in. `is` has no other reader in this engine — §4.13.4 rejects
 * `extends`, so no definition a lookup could find by one can be committed — and it is read here because the
 * throw is what the member does with it.
 *
 * STEP 1 IS PER DOCUMENT, NOT PER REALM. "Look up a custom element registry given document" answers NULL for a
 * document that is not a Window's — a createHTMLDocument, DOMParser or `new Document()` one — which is exactly
 * why an element parsed into one is never upgraded. The receiver is what names WHICH document, so the by-node
 * entry is what can say that; this realm's own registry would answer for the wrong document the moment
 * `otherDoc.createElement("x-y")` is written.
 *
 * Returns the registry — a CustomElementRegistry or JS_NULL, OWNED — or JS_EXCEPTION with the exception the
 * step names pending. */
static JSValue flatten_creation_options(JSContext *ctx, JSValueConst doc_wrap, JSValueConst options)
{
    JSValue registry = custom_elements_node_registry(ctx, doc_wrap);   /* STEP 1 */
    JSValue is_v, reg_v;                                               /* STEP 2: `is` is null */

    /* STEP 3, "if options is a dictionary". §3.2.25's union was resolved by the DECLARATION — null, undefined
       and every Object took the dictionary arm and everything else took the DOMString — so what arrived being
       the converted dictionary object IS the answer, and both members below were read as rest points rather
       than by a [[Get]] from this C activation.
       UNKNOWN EXTERNAL INPUT crosses the declaration as ITSELF rather than being read as a dictionary — that
       is the boundary rule the argument conversion states — so it has no members this algorithm can name, and
       the answer is the one an absent options gives rather than an invented member. */
    if (!JS_IsObject(options) || concolic_is(options)) return registry;
    is_v = idl_dict_get(ctx, options, "is");                           /* STEP 3.1 */
    reg_v = idl_dict_get(ctx, options, "customElementRegistry");
    /* WEB IDL §3.2.15 ON THE MEMBER'S DECLARED TYPE, which is `CustomElementRegistry?`. It runs BEFORE any of
       the algorithm's own steps because a conversion does, so `{is:"x-y", customElementRegistry:5}` is a
       TypeError and not step 3.2.1's NotSupportedError. It is here rather than in the declaration because the
       class is custom_elements.c's own and this component may not name it — the shape §4.8's attachShadow uses
       for the identical member. */
    if (!JS_IsUndefined(reg_v) && !JS_IsNull(reg_v) && !custom_elements_is_registry(reg_v)) {
        JS_FreeValue(ctx, is_v);
        JS_FreeValue(ctx, reg_v);
        JS_FreeValue(ctx, registry);
        return JS_ThrowTypeError(ctx, "ElementCreationOptions's customElementRegistry is not a "
                                      "CustomElementRegistry");
    }
    if (!JS_IsUndefined(reg_v)) {                                      /* STEP 3.2: the member EXISTS */
        if (!JS_IsUndefined(is_v)) {                                   /* STEP 3.2.1 */
            JS_FreeValue(ctx, is_v);
            JS_FreeValue(ctx, reg_v);
            JS_FreeValue(ctx, registry);
            return JS_ThrowDOMException(ctx, "NotSupportedError",
                                        "an element creation cannot name both `is` and "
                                        "`customElementRegistry`");
        }
        JS_FreeValue(ctx, registry);
        registry = reg_v;                                              /* STEP 3.2.2 (the reference moves) */
    } else {
        JS_FreeValue(ctx, reg_v);
    }
    JS_FreeValue(ctx, is_v);
    /* STEP 3.3. The two registries a creation may name are a SCOPED one and the document's own; anything else
       is a registry this document resolves nothing in, and handing it over would make every later lookup on
       the element answer out of a set the document has nothing to do with. */
    if (JS_IsObject(registry) && !custom_elements_registry_is_scoped(ctx, registry)) {
        JSValue doc_reg = custom_elements_node_registry(ctx, doc_wrap);
        bool same = JS_VALUE_GET_PTR(doc_reg) == JS_VALUE_GET_PTR(registry);

        JS_FreeValue(ctx, doc_reg);
        if (!same) {
            JS_FreeValue(ctx, registry);
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
    /* §4.9's "CREATE AN ELEMENT INTERNAL" result, made BEFORE step 3's lookup — see the LOOKUP stage. It is
       the element step 6 answers with and the element step 5.1.4's failure arm answers with; only a
       constructor that RETURNS discards it. Owned. */
    JSValue el;
    JSValue local;      /* the local name the operation was given (owned) — step 5.1.4.8 compares against it */
    JSValue result;     /* what the page's constructor answered (owned) */
    JSValue exc;        /* step 5.1.4's caught exception (owned), held across the report's own park */
    ReportExceptionWork rw;
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
    report_exception_work_visit(ctx, &s->rw, v);
}

/* §8.1.4.6 step 5's FLAG, and nothing else: every value this state holds — the report's own included — is named
   by doc_create_el_visit, which is the one list the teardown discharges. A flag on the global is not a
   reference, so no declaration can name it, and leaving it set would put the global in error reporting mode
   forever and swallow every later report on it. */
static void doc_create_el_release(JSContext *ctx, void *st)
{
    report_exception_work_unlock(ctx, &((DocCreateElState *)st)->rw);
}

static int js_doc_create_element_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                      JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    DocCreateElState *s = st;
    int r;

    if (hdr->stage == DCE_LOOKUP) {
        JSValue registry;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY owned field before the first thing that can throw — the failure path tears this state down
           through doc_create_el_release, which frees exactly what the state holds and nothing else. */
        s->cb[0] = s->def = s->el = s->local = s->result = s->exc = JS_UNDEFINED;
        report_exception_work_start(&s->rw);
        /* §4.5 createElement STEP 3, and it runs whatever the local name is: `{is, customElementRegistry}`
           refuses itself before any element is created. */
        registry = flatten_creation_options(ctx, hdr->this_val, argc > 1 ? argv[1] : JS_UNDEFINED);
        if (JS_IsException(registry)) return -1;
        /* §4.9 step 3 needs a NAME to look a definition up by, and a concolic tag has none — the creation is
           the one below, over whatever example the source carries. */
        if (argc < 1 || concolic_is(argv[0])) {
            JS_FreeValue(ctx, registry);
            *presult = js_doc_create_element(ctx, hdr->this_val, argc, argv, 0);
            return JS_IsException(*presult) ? -1 : 0;
        }
        /* §4.9's "CREATE AN ELEMENT INTERNAL", PERFORMED BEFORE STEP 3'S LOOKUP. The lookup is over
           (registry, namespace, local name, is), and the only form of it that can answer out of a SCOPED
           registry reads those four off an ELEMENT — so the element step 6 would create is created here and
           carries them. It is not spent work: step 6 answers with exactly it, and so does step 5.1.4's failure
           arm, whose element the standard also builds by creating an element internal. Only a constructor that
           RETURNS an element of its own leaves this one unused. */
        s->el = js_doc_create_element(ctx, hdr->this_val, argc, argv, 0);
        if (JS_IsException(s->el)) { s->el = JS_UNDEFINED; JS_FreeValue(ctx, registry); return -1; }
        /* "…custom element registry to registry", written through the component that owns the association —
           the once-only rule and the scoped-registry latch belong with the slot and not with each writer. */
        custom_elements_node_associate_registry(ctx, s->el, registry);
        s->def = custom_elements_definition_lookup_for_element(ctx, s->el);   /* §4.9 STEP 3 */
        /* §4.9 STEPS 5.1.2-5.1.3, WHICH DO NOT EXIST YET AND ARE NOW REACHABLE. They set the surrounding
           agent's ACTIVE CUSTOM ELEMENT CONSTRUCTOR MAP[C] to `registry` for the duration of the construct,
           and that map is the only thing that tells §4.13.2's `[HTMLConstructor]` which registry the element
           it mints belongs to — with it absent the constructor derives the DOCUMENT's, so an element created
           through a scoped registry would answer for a registry the page never named. Until this creation
           option existed no scoped registry could reach a construct at all; now one can, so the case that
           cannot be answered aborts here instead of answering wrongly. */
        DCHECK(!JS_IsObject(s->def) || !JS_IsObject(registry) ||
                   !custom_elements_registry_is_scoped(ctx, registry),
               "DOM §4.9 steps 5.1.2-5.1.3 are unbuilt: a custom element was created through a SCOPED "
               "CustomElementRegistry, and the agent's ACTIVE CUSTOM ELEMENT CONSTRUCTOR MAP is what carries "
               "that registry into HTML §4.13.2's constructor. core/html/custom_elements.c owns it — the map "
               "is per AGENT, keyed by the definition's constructor, set around the Construct and restored "
               "after it (steps 5.1.5-5.1.6) — and §4.13.2 step 7 must read it instead of deriving the "
               "current global's document's registry");
        JS_FreeValue(ctx, registry);
        if (!JS_IsObject(s->def)) {                  /* §4.9 step 6: not a custom element */
            *presult = s->el;
            s->el = JS_UNDEFINED;
            return 0;
        }
        /* Step 5.1.4.8 compares what the constructor made against the name this operation was GIVEN, which the
           declaration has already converted to a string. */
        s->local = JS_DupValue(ctx, argv[0]);
        STEP_GOTO(hdr->stage, DCE_CONSTRUCT, &s->phase, NULL);
    }
    if (hdr->stage == DCE_CONSTRUCT) {
        /* §4.9 STEP 5.1.4.1 — constructing C. Steps 5.1.2-5.1.3's active custom element constructor map is
           what a SCOPED registry would be carried into the constructor through; the LOOKUP stage asserts that
           no scoped registry reaches here, and names the map as the thing to build, rather than this stage
           constructing as though every registry were the document's. */
        JSValue ctor = custom_elements_definition_constructor(ctx, s->def);
        JSValue made = JS_UNDEFINED;

        r = step_construct_run(ctx, &s->phase, STEP_CB(s->cb), ctor, 0, NULL, cb_result, &made,
                               out_cb, out_argc);
        JS_FreeValue(ctx, ctor);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;                          /* parked ON the page's constructor */
        /* step 5.1.4's catch. The construct threw — synchronously (r < 0) or delivered as JS_EXCEPTION,
           which is what this member's declared catches_abrupt asks the driver for. */
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
        /* The checks are INSIDE step 5.1.4's catch too — a constructor that gave its element an attribute, a
           child, a parent or the wrong local name throws a NotSupportedError the spec REPORTS. */
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
       the HTML namespace, prefix, "failed", null, and registry." The INTERFACE is named by the step, so the
       element the page gets back is an HTMLUnknownElement carrying the local name it asked for — which is how
       a page distinguishes a component whose constructor threw from one that worked. */
    {
        /* THE ELEMENT THE LOOKUP STAGE ALREADY CREATED — §4.9's create an element internal, performed in the
           RECEIVER's document (§4.5 step 5's "given this", which a create in the current global's document
           would get wrong for `otherDoc.createElement(...)`) and carrying the registry step 3 flattened, which
           is what this step names too. */
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
    /* DOM §4.9 step 5.1.4 is "run these steps while catching any exceptions", so the construct's abrupt
       completion is this algorithm's VALUE — it reports it and answers with a failed HTMLUnknownElement. */
    .catches_abrupt = 1
};

/* DOM §4.9 "create an element internal" for THIS REALM'S document — see document.h. Separate from the member
   above because the two name different documents on purpose: `createElement` creates in its RECEIVER's
   document (§4.5 step 5's "given this"), and HTML §4.13.2 step 7.2 creates in the CURRENT GLOBAL's. */
JSValue document_create_element_internal(JSContext *ctx, const char *local, size_t len)
{
    lxb_dom_element_t *el = lxb_dom_document_create_element(lxb_dom_interface_document(doc_here(ctx)->dom),
                                                            (const lxb_char_t *)local, len, NULL);

    dom_cow_note_created(el ? lxb_dom_interface_node(el) : NULL);   /* this flow made it: the delta owns it */
    DCHECK(el != NULL, "HTML §4.13.2 step 7 produced no element for a definition's local name — the definition "
                       "was made from a name §4.13.1 already accepted, so Lexbor refusing it is a disagreement "
                       "about what a name is");
    return element_wrap(ctx, el);
}

/* 4.5.3 createElementNS(namespace, qualifiedName). Same element creation as createElement, plus the spec's
   "validate and extract": a qualified name may carry a prefix (`svg:rect`), and the element is created in the
   named NAMESPACE rather than the document's default. testharness.js reaches it on every completed document —
   `output_document.createElementNS(xhtml_ns, "style")` in Output.show_results — and a missing one threw
   "not a function" inside the completion callback, which aborted the callback list and so silenced the
   REPORTING of documents whose tests had all already run.
   Lexbor carries the namespace on the element, so this is its create with the namespace resolved, not a
   createElement in disguise: `el.namespaceURI` is what the page asked for. */
/* 4.5.1 createTextNode / createComment. The two non-element nodes a page builds by hand, and without them a
   page could not put TEXT into the tree at all: testharness.js's make_dom_single does
   `output_document.createTextNode(template[i])` for every string in a template. Detached, like createElement —
   nothing is observable until appendChild, which IS the per-flow write. */
static JSValue js_doc_create_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    const char *s;
    size_t len = 0;
    lxb_dom_text_t *t;
    Document *d = doc_receiver(ctx, this_val);

    if (!d) return JS_EXCEPTION;
    s = argc >= 1 ? JS_ToCStringLen(ctx, &len, argv[0]) : JS_ToCStringLen(ctx, &len, JS_UNDEFINED);
    if (!s) return JS_EXCEPTION;
    t = lxb_dom_document_create_text_node(lxb_dom_interface_document(d->dom), (const lxb_char_t *)s, len);
    dom_cow_note_created(t ? lxb_dom_interface_node(t) : NULL);   /* this flow made it */
    JS_FreeCString(ctx, s);
    DCHECK(t != NULL, "createTextNode produced no node — a page building its DOM would silently build nothing");
    return node_wrap(ctx, lxb_dom_interface_node(t));
}

/* §4.5 `new Document()` — "set this's origin to the origin of current global object's associated Document",
   and nothing else: the document it makes has NO tree, no doctype and no document element, its URL is
   `about:blank` and its content type is `application/xml`. It is the ONE way a page gets an XML document
   without going through DOMImplementation, and dom/common.js opens with it.
   The Document is built here rather than by the interface object's shared throw because §4.5's IDL declares a
   constructor; `node_install_interface` is for the interfaces that declare none. */
static JSValue js_doc_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic)
{
    lxb_html_document_t *dom;

    (void)new_target; (void)argc; (void)argv; (void)magic;
    dom = dom_document_create();
    CHECK(dom != NULL, "new Document(): OOM building a second Document");
    /* The ORIGIN is the constructing realm's document's, which document_new takes from the realm it runs in —
       the same rule createDocument's step 6 states, and the reason this is not `document_install`. */
    /* §4.5's OWN DEFAULTS, which is what `new Document()` is: "Unless stated otherwise, a document's …
       content type is `application/xml`, URL is `about:blank`, … type is `xml`". The constructor states
       nothing else, so both facts are the standard's initial values verbatim — and the `xml` half is what
       makes `new Document().createCDATASection(…)` succeed where an HTML document's must throw. */
    return document_new(ctx, dom, "about:blank", document_kind(/*is_xml*/true, "application/xml"));
}

/* §4.5.1 `createCDATASection(data)` and `createProcessingInstruction(target, data)` — the two node factories
   an XML document has and an HTML one does not, and the reason they are here rather than absent: dom/common.js
   builds `paras[5]` out of two CDATA sections and `xmlDoc` out of two processing instructions, so EVERY
   §5 and §6 test file that includes it threw inside its own setup and reported zero subtests. That is the
   excluded-test defect wearing a page's TypeError: fifteen files ERRORed at load, and the count looked like
   fifteen rather than like the hundreds of subtests they contain.
   magic 0 = createCDATASection, 1 = createProcessingInstruction. */
static JSValue js_doc_create_xml_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    Document *d = doc_receiver(ctx, this_val);
    lxb_dom_document_t *dom;
    const char *data, *target = NULL;
    size_t dlen = 0, tlen = 0;
    lxb_dom_node_t *made = NULL;
    JSValue r;

    if (!d) return JS_EXCEPTION;
    dom = lxb_dom_interface_document(d->dom);
    if (magic == 0) {
        /* STEP 1. `createCDATASection` on an HTML document is a NotSupportedError, and §4.5's own words are
           "if this is an HTML document" — which §4.5 defines over the document's TYPE alone ("A document is
           said to be an XML document if its type is `xml`; otherwise an HTML document"). This asked the
           CONTENT TYPE instead, which answers the same for §4.5.1's factories and differently for a Document
           a load created: HTML §7.5.4 "Loading text documents" creates an HTML document whose content type is
           the response's, so every `text/plain` document was treated as XML here and allowed a CDATA section
           that a browser refuses. */
        if (!d->is_xml)
            return JS_ThrowDOMException(ctx, "NotSupportedError",
                                        "an HTML document has no CDATA sections");
    } else {
        target = JS_ToCStringLen(ctx, &tlen, argv[0]);
        if (!target) return JS_EXCEPTION;
        /* STEP 1 of §4.13's "initialize a ProcessingInstruction node", which this algorithm reaches through
           "create a processing instruction". It is XML 1.0 §2.3's `Name` production — the DOM REFERENCES that
           production (its step links https://www.w3.org/TR/xml/#NT-Name) rather than restating it, and its own
           §1.4 predicates in core/dom/names.h are a different, deliberately looser set that would accept `0`
           and `\A` here. core/xml/xml_name.h owns the production for the same reason: every name an XML parser
           scans is it. `xml:fail` IS a legal target — §2.3 requires a processor to accept the colon as a name
           character, and narrowing it to an NCName is the namespace layer's job, not this one's.
           It runs BEFORE the "?>" test below because that is the order the two steps are written in, and the
           order decides which of the two DOMExceptions a page sees when both are wrong. */
        if (!xml_name_is_name(target, tlen)) {
            JS_FreeCString(ctx, target);
            return JS_ThrowDOMException(ctx, "InvalidCharacterError",
                                        "a processing instruction target must match the XML Name production");
        }
    }
    data = JS_ToCStringLen(ctx, &dlen, argv[magic == 0 ? 0 : 1]);
    if (!data) { if (target) JS_FreeCString(ctx, target); return JS_EXCEPTION; }
    /* createCDATASection's STEP 2, and "initialize a ProcessingInstruction node"'s: the one sequence the
       node's own serialization cannot survive. */
    if (magic == 0 ? (strstr(data, "]]>") != NULL) : (strstr(data, "?>") != NULL)) {
        JS_FreeCString(ctx, data);
        if (target) JS_FreeCString(ctx, target);
        return JS_ThrowDOMException(ctx, "InvalidCharacterError",
                                    magic == 0 ? "a CDATA section cannot contain \"]]>\""
                                               : "a processing instruction cannot contain \"?>\"");
    }
    if (magic == 0) {
        lxb_dom_cdata_section_t *c =
            lxb_dom_document_create_cdata_section(dom, (const lxb_char_t *)data, dlen);
        CHECK(c != NULL, "createCDATASection: the Lexbor node allocation failed");
        made = lxb_dom_interface_node(c);
    } else {
        lxb_dom_processing_instruction_t *pi =
            lxb_dom_document_create_processing_instruction(dom, (const lxb_char_t *)target, tlen,
                                                           (const lxb_char_t *)data, dlen);
        CHECK(pi != NULL, "createProcessingInstruction: the Lexbor node allocation failed");
        made = lxb_dom_interface_node(pi);
    }
    dom_cow_note_created(made);   /* this flow made it; detached until the page inserts it */
    JS_FreeCString(ctx, data);
    if (target) JS_FreeCString(ctx, target);
    r = node_wrap(ctx, made);
    return r;
}

static JSValue js_doc_create_comment(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    const char *s;
    size_t len = 0;
    lxb_dom_comment_t *c;
    Document *d = doc_receiver(ctx, this_val);

    if (!d) return JS_EXCEPTION;
    s = argc >= 1 ? JS_ToCStringLen(ctx, &len, argv[0]) : JS_ToCStringLen(ctx, &len, JS_UNDEFINED);
    if (!s) return JS_EXCEPTION;
    c = lxb_dom_document_create_comment(lxb_dom_interface_document(d->dom), (const lxb_char_t *)s, len);
    dom_cow_note_created(c ? lxb_dom_interface_node(c) : NULL);   /* this flow made it */
    JS_FreeCString(ctx, s);
    DCHECK(c != NULL, "createComment produced no node — a page building its DOM would silently build nothing");
    return node_wrap(ctx, lxb_dom_interface_node(c));
}

/* 4.5.3 createElementNS(namespace, qualifiedName). createElement's element creation over the validated triple —
   the element carries the NAMESPACE the page asked for, so `el.namespaceURI` and a namespaced selector answer
   what they should. testharness.js reaches it on every completed document (`createElementNS(xhtml_ns, "style")`
   in Output.show_results), and a missing one threw inside the completion-callback list, silencing documents
   whose tests had all already run.

   §1.4's "validate and extract" IS core/dom/names.c's, and there is no longer a second one here. The copy that
   stood in this file was written before that component existed and had drifted from it in three ways that a
   page can see, each one a consequence of being STRING-BASED where the algorithm is length-carrying:
     - it never validated the LOCAL NAME at all, so `createElementNS(SVG_NS, "a b")` and `createElementNS(null,
       "1")` built elements out of names §1.4 step 6 rejects, where the shared algorithm throws the
       InvalidCharacterError the step names;
     - `strchr`/`strcmp` stop at U+0000, which DOM §1.4 treats as an ordinary code point a page may write, so
       `createElementNS(ns, "a\0:b")` found no colon and `createElementNS("http://…/xmlns/\0x", "xmlns")`
       compared equal to the XMLNS namespace it is not;
     - it validated a prefix only by "non-empty and no second colon", where §1.4 step 4.3 is the valid-namespace-
       prefix predicate.
   The shared one also takes the CONTEXT — `createElementNS` is an ELEMENT context, and that is a real
   difference rather than a parameter for tidiness: `a=b` is a valid element local name and not a valid
   attribute one, and the private copy validated neither.

   The FIRST ARGUMENT IS `DOMString?` AND IS NOW DECLARED SO. It was declared IDL_DOMSTRING, which is ToString,
   so `document.createElementNS(null, "div")` reached this body holding the four characters `null` and built an
   element in a namespace of that name — and the JS_IsNull test below, which is the shape every other nullable
   member here uses, was unreachable code that made it look handled. The one remaining reader of that test is
   §4.5.1's createDocument, which calls the internal steps with a real JS_NULL. */
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
    /* LENGTH-CARRYING on both, because §1.4 decides about U+0000 and `strlen` decides about the prefix of the
       string in front of it. names.h says the same thing from the component's side. */
    qname = JS_ToCStringLen(ctx, &qname_len, argv[1]);
    if (!qname) { if (ns) JS_FreeCString(ctx, ns); return JS_EXCEPTION; }

    if (!dom_validate_and_extract(ctx, ns, ns_len, qname, qname_len, DOM_NAME_ELEMENT, &qn)) {
        if (ns) JS_FreeCString(ctx, ns);
        JS_FreeCString(ctx, qname);
        return JS_EXCEPTION;          /* the DOMException §1.4's own step names is already thrown */
    }
    /* §4.5's storage step, and NOT lxb_dom_element_create: that entry interns the namespace and the local
       name through lexbor's case-folding hashes, so this method's two string arguments came back out of
       `namespaceURI` and `localName` lowercased. element.c states which standard says each is a different
       name; here the only thing to say is that the element is created from what the page passed — and the
       three slices validate-and-extract hands back ARE what it passed. */
    el = element_create_ns(lxb_dom_interface_document(d->dom),
                           qn.ns, qn.ns_len, qn.local, qn.local_len, qn.prefix, qn.prefix_len);
    /* THIS FLOW MADE IT — the same statement every other creating member on this interface makes, and the one
       this one did not. It is not inside element_create_ns, and cannot be: what a creation entry declares is
       OWNERSHIP, and ownership is the CALLER's fact. §4.4's clone builds a whole subtree with the same creator
       and notes only its ROOT, because dom_release_created destroys deep and a second entry naming a node
       already freed under its parent is a double free. So the creator cannot answer it and each caller must. */
    dom_cow_note_created(lxb_dom_interface_node(el));
    if (ns) JS_FreeCString(ctx, ns);
    JS_FreeCString(ctx, qname);
    r = element_wrap(ctx, el);
    return r;
}

/* §4.5's "INTERNAL createElementNS STEPS", named as the spec names them because a second caller reaches them:
   §4.5.1's createDocument step 3 is "the internal createElementNS steps, GIVEN DOCUMENT" — the new document,
   not the one whose implementation was asked. One implementation of validate-and-extract and of the element
   creation, rather than a second one over there that could disagree about a prefix. */
JSValue document_create_element_ns(JSContext *ctx, JSValueConst doc, int argc, JSValueConst *argv)
{
    return js_doc_create_element_ns(ctx, doc, argc, argv, 0);
}

/* §4.5's `[CEReactions] Node adoptNode(Node node)` — four steps, and the algorithm they are stated over is DOM
 * §4.5's "adopt a node", which core/dom/node.c owns because it is a shadow-including tree walk that writes each
 * node's node document through the per-flow chokepoint.
 *
 * THE TWO REFUSALS ARE DIFFERENT EXCEPTIONS AND THAT IS THE MEMBER'S POINT: a Document is a NotSupportedError
 * and a shadow root is a HierarchyRequestError, which a page's `catch (e) { e.name }` tells apart. They are
 * refusals rather than asserts — a page calling `document.adoptNode(document)` is asking a question the spec
 * answers, not violating an engine invariant.
 *
 * THE REALM IS THE RECEIVER DOCUMENT'S, not the running one. Adopt's step 3 mints wrappers and reads per-flow
 * records off them (the shadow-root association, the registry re-derivation, the adoptedCallback reaction), and
 * those are facts about the document being adopted INTO — the same rule node.c's own insert-adopt follows, and
 * the one that answers a second same-origin document's adoption out of its own realm rather than out of
 * whichever one happened to call. */
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
    /* STEP 1. */
    if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT)
        return JS_ThrowDOMException(ctx, "NotSupportedError", "a Document cannot be adopted");
    /* STEP 2. It is a HierarchyRequestError and not the same refusal as step 1's: a shadow root is a node the
       tree could hold nowhere, where a Document is a node no document may own. */
    if (shadow_root_is(n))
        return JS_ThrowDOMException(ctx, "HierarchyRequestError", "a shadow root cannot be adopted");
    node_adopt(d->realm, n, lxb_dom_interface_document(d->dom));           /* STEP 3: adopt node into this */
    return JS_DupValue(ctx, argv[0]);                                      /* STEP 4 */
}

/* §4.5's `[SameObject] readonly attribute DOMImplementation implementation`. SameObject is the whole reason the
   object lives on the document's record: a page holds it and calls it later, and a fresh one per read would
   compare unequal to the one it kept. */
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

/* THE DOCUMENT'S LOAD LIFECYCLE. Stage 0 is DOMContentLoaded — fired at the DOCUMENT and bubbling to window,
   which is where a page registers it — and stage 1 is load, fired at window. `readyState` moves with them,
   because a page that missed the event reads it instead. Both are per-FLOW: the scheduler asks once per stage
   for each flow that has run everything the document gave it, so an arm that reached the end of the document
   fires its own listeners in its own world. */
/* The parsed document's ROOT node — what a whole-tree walk starts from. One component owns which document a
   realm parsed; a second copy of that pointer is how the two drift apart. NULL before the install, which is a
   real state a Window member reads (`window.document` before there is one). */
lxb_dom_node_t *document_root_node(JSContext *ctx)
{
    Document *d = doc_of(ctx);
    return d ? lxb_dom_interface_node(d->dom->dom_document.element) : NULL;
}

/* ---- HTML §2.4.3 "DOCUMENT BASE URLS" -------------------------------------------------------------------
 *
 * THREE ALGORITHMS, AND THE ENGINE ANSWERED ALL OF THEM WITH THE ADDRESS. §2.4.3 defines a Document's
 * FALLBACK BASE URL (the srcdoc case, the about:blank case, then the address) and its DOCUMENT BASE URL (the
 * first base element's frozen URL, else the fallback); §4.2.3 defines the FREEZE that fills the first. What
 * stood here was the address alone, which is only the fallback's LAST step — so `<base href>` did nothing at
 * all, and a Document at about:blank resolved relative URLs against a URL that cannot be a base.
 *
 * THE FREEZE ITSELF LIVES IN core/html/html_base_element.c, because it is an algorithm about an ELEMENT and
 * this file owns what a Document HAS. The split is the one core/dom/document.c already makes with §4.12.1's
 * scripts and §4.8.5's iframes: the DOM half stores, the element's own component decides. */

/* §2.4.3's FALLBACK BASE URL of one Document — the URL §4.2.3 freezes a `<base href>` AGAINST, which is why it
   is a separate answer from the document base URL and not a synonym for it ("thus, the base element isn't
   affected by itself"). BORROWED. */
const char *document_fallback_base_url_of(const lxb_dom_document_t *dom)
{
    const Document *d = doc_rec(dom);

    DCHECK(d != NULL, "§2.4.3's fallback base URL was asked of a document with no record — a tree that came "
                      "from neither document_install nor document_new has no address, no about base URL and "
                      "no way to answer");
    /* STEPS 1 AND 2 read the document's ABOUT BASE URL, and both require it to be non-null: step 1 ASSERTS it
       for an iframe srcdoc document, step 2 tests it for a document whose URL matches about:blank. A null one
       (a NULL pointer here) therefore cannot reach either, so the parse those steps need is performed only
       when there is an about base URL to return — which for a document created from a response is never. */
    if (d->base.about) {
        UrlRecord u;
        bool about;

        url_record_init(&u);
        CHECK(url_parse(&u, d->addr.bytes, strlen(d->addr.bytes), NULL),
              "a document's own address is not a URL — §2.4.3 asks whether it MATCHES about:blank, which is a "
              "question about a URL record, and this record's address never parsed");
        /* HTML §2.4.1's two match relations, asked in §2.4.3's order. A document created AT about:blank whose
           address later moved (§7.4.4's URL and history update steps — `history.pushState` from an inherited
           origin) stops matching, and step 3's address is then the answer even though the about base URL is
           still there: the two are separate facts and the standard tests both. */
        about = url_matches_about(&u, "srcdoc", /*query_must_be_null*/ true) ||
                url_matches_about(&u, "blank", /*query_must_be_null*/ false);
        url_record_free(&u);
        if (about) return d->base.about;
    } else {
        /* STEP 1's ASSERT, stated where the standard states it: "assert: document's about base URL is
           non-null" for an iframe srcdoc document. The cheap prefix test rather than a parse, because this is
           the branch taken by every document in the engine and the assert must not cost one. */
        DCHECK(strncmp(d->addr.bytes, "about:srcdoc", 12) != 0,
               "§2.4.3 step 1's assert failed: an iframe srcdoc Document has a NULL about base URL. A srcdoc "
               "document has no address of its own to fall back to — every relative URL in it, and every "
               "`<base href>` it carries, would resolve against `about:srcdoc` — so whoever created this "
               "Document must give it §7.4's about base URL (the srcdoc iframe's node document's base URL)");
    }
    return d->addr.bytes;   /* STEP 3: "return document's URL" */
}

/* §2.4.3's DOCUMENT BASE URL of one Document — the answer DOM §4.4's `baseURI`, HTML §2.4.2's parse-a-URL and
   §8.1.5.1's API base URL all name. A fact about THAT DOCUMENT and not about the running realm, which stopped
   being the same question the moment §4.5.1's factories could build a second Document. BORROWED. */
const char *document_base_url_of(const lxb_dom_document_t *dom)
{
    const Document *d = doc_rec(dom);

    DCHECK(d != NULL, "§2.4.3's document base URL was asked of a document with no record");
    /* STEP 1: "if document has no descendant base element that has an href attribute, then return document's
       fallback base URL." STEP 2: "otherwise, return the FROZEN BASE URL of the first base element in document
       that has an href attribute, in tree order" — which is the element this record names, kept in step by
       §4.2.3's own situations (core/html/html_base_element.c). */
    if (!d->base.frozen_el) return document_fallback_base_url_of(dom);
    DCHECK(d->base.frozen != NULL && d->base.frozen[0] != '\0',
           "a document names a base element whose FROZEN BASE URL is absent or empty — §4.2.3's set the "
           "frozen base URL ends in either a serialized URL record or the document's fallback base URL, and "
           "neither is empty, so this pair was written by something that is not that algorithm");
    return d->base.frozen;
}

const char *document_base_url(JSContext *ctx)
{
    Document *d = doc_here(ctx);
    DCHECK(d->addr.bytes[0] != '\0', "this realm's API base URL was read before its document was installed");
    return document_base_url_of(lxb_dom_interface_document(d->dom));
}

/* THIS REALM'S ACTIVE DOCUMENT'S ADDRESS — §4.5's `document.URL`, and a DIFFERENT question from the one above.
 * It is separated now because it was not before, and the conflation was invisible while the base URL WAS the
 * address: `document_base_url` answered both, so §7.2.4's "this Location object's relevant Document's URL",
 * §7.2.5's can-have-its-URL-rewritten, §7.4.4's "let newURL be document's URL" and §8.1's blob origin all read
 * a function whose name says base URL. The moment `<base href>` works, every one of those would have started
 * reporting the base instead — `location.href` would answer the markup's base URL, and a blob URL would carry
 * a CDN's origin. Which question a caller is asking is now something it has to say. */
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

/* §4.2.3's FROZEN BASE URL, STORED — see the field, and core/html/html_base_element.c for the algorithm that
   decides WHEN this is called. `el` NULL with `url` NULL is "this document has no base element with an href",
   which §2.4.3 step 1 answers with the fallback base URL. */
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
    /* TWO SCALAR CAPTURES FOR ONE FACT — see the field. Each is a POINTER-sized latch, which is what the byte
       arm is for; the pair reverts together because a delta unapplies wholly. */
    cow_capture_host_state(d->realm, d->doc_obj, &d->base.frozen_el, sizeof d->base.frozen_el);
    cow_capture_host_state(d->realm, d->doc_obj, &d->base.frozen, sizeof d->base.frozen);
    d->base.frozen_el = el;
    /* THE OLD FROZEN URL IS NOT FREED — a flow that captured it above has an entry naming it, and §4.2.3's own
       situations re-freeze on every tree change, so this is the field that changes most. It stays on the
       record's address list until the record dies. */
    d->base.frozen = url ? doc_addr_intern(d, url) : NULL;
    DCHECK((d->base.frozen_el == NULL) == (d->base.frozen == NULL),
           "§4.2.3's frozen base URL and the element it belongs to disagree about whether this document has "
           "one — they are captured as two entries and reverted as one fact, so a document holding half the "
           "pair has had one of them written outside this setter");
}

/* §7.4's ABOUT BASE URL, which "create a new browsing context and document" gives the initial `about:blank` as
 * `creatorBaseURL` and §7.4.5 gives an `about:` navigation from its initiator. WRITE-ONCE, at creation, by the
 * operation that created the Document — never by anything the page can reach.
 *
 * IT IS AN INPUT OF THE OPERATION AND NOT A READ OFF THE TARGET, which is why the caller states it: whose base
 * URL it is depends on WHICH operation is running (the CREATOR's for a create, the INITIATOR's for a
 * navigation) and never on the state of the navigable being filled — the same sentence §7.2.6's inherited
 * policy container is stated with, one field over. */
void document_set_about_base_url(JSContext *ctx, const char *url)
{
    Document *d = doc_here(ctx);

    DCHECK(url != NULL && url[0] != '\0',
           "§7.4's about base URL was set to nothing — a null one is the ABSENCE of this call, which is what "
           "every Document created from a response has, rather than a value to write");
    DCHECK(d->base.about == NULL,
           "§7.4's about base URL was written twice for one Document — it is a CREATION item, decided by the "
           "operation that made the Document and fixed for its life, so a second writer is a second answer to "
           "which document created this one");
    DCHECK(d->base.frozen_el == NULL,
           "§7.4's about base URL was set for a Document that has ALREADY FROZEN a base element's URL — §4.2.3 "
           "freezes AGAINST the fallback base URL, which this call changes, so that element is frozen to an "
           "answer this Document never had. Set the about base URL where the Document is CREATED, before its "
           "tree is walked, rather than after");
    DCHECK(JS_IsObject(d->doc_obj),
           "§7.4's about base URL was set before the document's `document` object existed");
    cow_capture_host_state(ctx, d->doc_obj, &d->base.about, sizeof d->base.about);
    d->base.about = doc_addr_intern(d, url);
}

/* HTML §3.1.1's "the encoding" of this realm's active document — see document.h. ONE component owns what a
   document's encoding is, for the same reason it owns what its URL is: two answers to that question is how
   they drift apart, and this one decides how a `<script src>`'s bytes become source text. */
int document_encoding(JSContext *ctx)
{
    Document *d = doc_here(ctx);

    DCHECK(d->encoding >= 0,
           "a document's encoding was read before the record carried one — §3.1.1 gives every Document an "
           "encoding and doc_rec_new writes it, so a negative id is a record some other path built");
    return d->encoding;
}

/* THE SAME FACT ASKED OF A DOCUMENT RATHER THAN OF A REALM, which is a different question and not a
   convenience: HTML §4.10.22.5 "Selecting a form submission encoding" step 1 is "let encoding be THE
   DOCUMENT's character encoding", and the document it means is the form's NODE DOCUMENT — a realm holds
   several (an inert template document, a `createHTMLDocument` result), so answering from the active one would
   report a different document's encoding for a form that is not in it. Same shape as document_base_url_of one
   screen up, and for the same reason. */
int document_encoding_of(const lxb_dom_document_t *dom)
{
    const Document *d = doc_rec(dom);

    DCHECK(d != NULL, "HTML §3.1.1's encoding was asked of a document with no record");
    DCHECK(d->encoding >= 0,
           "a document's encoding was read before the record carried one — §3.1.1 gives every Document an "
           "encoding and doc_rec_new writes it, so a negative id is a record some other path built");
    return d->encoding;
}

/* HTML §13.2.3.2 "Determining the character encoding"'s ANSWER, WRITTEN ONTO THE DOCUMENT IT IS ABOUT.
 *
 * DOM §4.5 Interface Document gives every document the utf-8 encoding "unless stated otherwise", which
 * doc_rec_new writes; this is the one algorithm that states otherwise, and it is a NAVIGATION's step rather
 * than a Document's own — the bytes and the response metadata it reads belong to the load, so the loader calls
 * it and hands the result here. That is why this is a setter and not a lazy read: nothing reachable from a
 * Document can re-derive it, because by the time anything asks, the bytes have been decoded and parsed.
 *
 * IT IS PER-FLOW, and the capture is the POD one for the same reason document_set_url's is: a document's
 * encoding is an `int` on this record, and the `document` object is what owns the record's storage for a flow
 * parked in the middle of a load. Two flows loading one navigable from two forked responses see their own. */
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

/* THE BYTES AN ADDRESS VALUE STANDS FOR — see core/dom/document.h.
 *
 * IT IS ONE FUNCTION AND NOT AN `if` AT EACH ARRIVAL, which is the rule CLAUDE.md states for every dispatch
 * that decides what a set of bytes IS: §7.2.5's `pushState`, §4.8.5's `<iframe src>` and §7.2.2.1's
 * `window.open` all turn a value the page computed into a Document address, and a question asked at some of
 * them and not the others does not report an absent capability — it reports an unrelated subsystem failing on
 * input it should never have been shown. The concolic's own ToString is exactly that failure: it names the
 * JavaScript coercion boundary for a value that is a perfectly good address and merely has a domain.
 *
 * THE MISSING HALF CRASHES HERE, ONCE. A concolic with NO example is a Document whose address this run does
 * not know, and there is nothing to take: taking its display SHAPE would seed `…/admin/%7Bstate%7D.id` — an
 * address no server has, fetched over the person's own session by solver/route_seed.h's declaration, whose
 * reply's fields would then be examples shaping the next endpoint. */
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
    /* FATAL AND NOT "THE ADDRESS IS ABSENT". Two things reach it — OOM, and a RELEASE build where the arm
       above is compiled out and a concolic's ToString throws — and the second is precisely the one that must
       not be swallowed into a Document silently left where it was. */
    CHECK(bytes != NULL,
          "a Document address value would not convert to bytes: either the allocation failed, or this is a "
          "release build and the address is unknown external input, whose ToString has no concolic semantics — "
          "a dev build names the capability to build at the arm above this line");
    return bytes;
}

/* THE TWO HALVES OF AN ADDRESS SAY THE SAME THING — asserted at EVERY writer, because it is the only thing
   standing between this record and the two-answers-to-one-question defect that made a Document answer its own
   base URL with somebody else's. `bytes` is what every byte consumer resolves against and what the trusted
   zone loads; `value` is what a branch forks on. They are written by one call each time, so a disagreement is
   not a race — it is a writer that computed one of them from something other than the other. */
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

/* HTML §2.4.3 "Document base URLs" — SET THE URL, step 1: "Set document's URL to url". It is the only way a
 * Document's address changes without a new Document, and HTML §7.4.4 "Non-fragment synchronous
 * \"navigations\""'s URL and history update steps step 8 performs it.
 *
 * IT TAKES A VALUE, WHICH IS WHERE THE ADDRESS PRIMITIVE ACTUALLY LIVES. core/dom/document.h's
 * document_url_value states why an address is a triple and not a string; this is the ONE door through which a
 * Document acquires one, so the bytes and the value can never be written by two callers that disagree — the
 * bytes are DERIVED from the value here rather than passed beside it.
 *
 * IT IS PER-FLOW, AND THE CAPTURE IS THE RECORD ARM. A flow that called `history.pushState(s, "", "/b")` is
 * the only flow whose `location.pathname` is `/b`; a sibling arm that never pushed still reads the address it
 * forked at, and a parked flow resumes onto its own. It is the record arm and not the byte one because the
 * pair now holds a JSValue: solver/cow.h reserves the byte arm for a POD SCALAR LATCH, and a memcpy of a
 * JSValue makes a reference nothing counts, so the next restore would free a value the blob still names.
 * CAPTURING THE PAIR IN ONE ENTRY IS THE POINT, not an economy: a delta unapplies wholly, so a context switch
 * puts the bytes and the value back together or puts neither back, and there is no window in which a
 * `location.pathname` derived from one flow's value serializes another flow's address.
 *
 * THE UNAPPLY IS SOUND BECAUSE NOTHING FREES THE OLD STRING: the entry writes the old pointer back and the
 * string it names is still on this record's address list. The old VALUE is not borrowed that way — the capture
 * dup'd it, so this releases the record's own reference here and the delta keeps its. THE OWNER IS THE
 * `document` OBJECT, which is what holds the record's storage alive for a parked flow that still names it. */
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
    /* PUBLISHED BEFORE THE OLD ONE IS RELEASED, and the order is the whole of it. doc_rec_refs walks this
       slot for the collector, and releasing a concolic address runs that class's finalizer, which is platform
       code that may allocate — and an allocation IS a collection. Freeing first would leave the slot naming
       storage already on the free list for the whole of that call, and the walk would decref a JSObject that
       no longer exists. */
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

/* THIS REALM'S ACTIVE DOCUMENT'S ADDRESS AS A VALUE — see core/dom/document.h. It is the accessor
   core/frame/location.c derives §7.2.4's members through and the reason `location.pathname` on a COMPUTED
   address still forks. */
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

/* HTML §3.1.5's CURRENT DOCUMENT READINESS, which is the internal slot and NOT the member. `readyState` is a
   READONLY accessor on Document.prototype (core/dom/document_metadata.c) that RETURNS this, so there is one
   statement of the fact. It was a data property on the `document` object, RE-WRITTEN here every time the
   readiness moved — two statements with a window between them, and one a page could assign: the load lifecycle
   and §8.1.7.3 step 3's render-blocked test both read the readiness, so `document.readyState = "complete"` let
   a page skip its own DOMContentLoaded and unblock its rendering.
 *
 * IT LIVES IN THIS REALM'S OWN BASELINE RECORD, the same shape §8.12 Animation frames's map of animation frame callbacks and
 * §7.4.6.3's has-been-revealed use, and for the same two reasons: the record is unreachable from the page, so
 * nothing can write the readiness but this component; and its `stage` is an ordinary property write, so the
 * heap COW captures it and one arm of a fork advances its lifecycle without touching its sibling's.
 * A private Symbol on the Document would have done the first job and not the second cleanly — and it would
 * have been an AGENT-wide value freed by a PER-REALM teardown (document_free runs once per realm, from
 * navigable.c's realm sweep), which is one document dropping the key the others still read through. */
static int g_ready_slot = -1;

/* HTML §3.1.5's THREE READINESS VALUES, in the order the enum declares them — the strings `readyState` answers
   with and the only place they are written down. */
static const char *const DOC_READINESS[3] = { "loading", "interactive", "complete" };

static int document_readiness(JSContext *ctx);

/* HTML §3.1.5's UPDATE THE CURRENT DOCUMENT READINESS, all four of its steps.
   STEP 1 IS NOT BOOKKEEPING: "if document's current document readiness equals readinessValue, then return" is
   what stops a `readystatechange` firing for a readiness that did not change, and it is also what makes step 4
   below reachable only where there IS a document — a realm's record is built at "loading" before its Document
   exists, and a re-statement of "loading" for it is exactly the no-change case.
   STEP 4 FIRES `readystatechange` AT THE DOCUMENT, which nothing did: §3.1.1 declares `onreadystatechange` and
   the corpus registers it, so a page waiting for "interactive" through the event rather than through the
   property waited for ever. Step 3's load timing info is HONESTLY ABSENT — its two fields belong to
   §Navigation-Timing's `PerformanceNavigationTiming`, which this build does not have, so there is nothing here
   to write them onto and no member that could read them back.
   STEP 4 IS A BARE FIRE — "fire an event named readystatechange at document", with no queue around it — and
   the queue is at the CALLER: §13.2.7 "The end" step 9 runs "update the current document readiness to
   'complete'" (step 9.1) and the `load` fire (step 9.5) inside ONE queued global task. That fire is event_target_fire_run's shape
   and not this one, and it stays this one only because document_done_stage below is a plain C body the
   scheduler drives rather than a step machine — which is the same statement as "the readiness transition
   cannot suspend on a `readystatechange` listener", and it is what has to change. */
static void document_set_ready(JSContext *ctx, int stage)
{
    Document *d = doc_here(ctx);
    JSValue rec;

    DCHECK(stage >= 0 && stage <= 2, "a document readiness HTML does not define");
    if (document_readiness(ctx) == stage) return;                                 /* STEP 1 */
    rec = realm_value_get(ctx, g_ready_slot);
    DCHECK(JS_IsObject(rec), "a realm answered for its document readiness with no record");
    JS_SetPropertyStr(ctx, rec, "stage", JS_NewInt32(ctx, stage));                /* STEP 2 */
    JS_FreeValue(ctx, rec);
    DCHECK(JS_IsObject(d->doc_obj),
           "a document's readiness moved before its `document` object existed — §3.1.5 step 4 fires "
           "`readystatechange` AT the document, so there would be no target for the event the page listens for");
    event_target_fire(ctx, d->doc_obj,                                            /* STEP 4 */
                      event_new(ctx, "readystatechange", /*bubbles*/ false, /*cancelable*/ false), JS_UNDEFINED);
}

/* HTML §8.4.1 "Opening the input stream" step 18 — "Update the current document readiness of document to
   'loading'" — as the ONE readiness transition another algorithm may ask for.
 *
 * IT IS A NAMED ENTRY AND NOT AN EXPORTED SETTER. Every other move of the readiness belongs to §13.2.7 "The
 * end" and is driven by the lifecycle below, so a general `document_set_readiness(ctx, stage)` would be a way
 * for any component to tell a page its document had finished loading; §8.4.1's step is the single case where a
 * Document that HAS finished goes back to loading, and it is the algorithm that re-opened the input stream
 * that says so. The `readystatechange` this fires is unobservable at its one call site — §8.4.1's own note
 * says why: step 10 erased every listener and handler that could have heard it. */
void document_set_readiness_loading(JSContext *ctx)
{
    document_set_ready(ctx, 0);
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

/* §3.1.5's readyState getter steps — "return this's current document readiness" — as a fact about ONE DOCUMENT,
   which is what the member's RECEIVER names.
   §3.1.5 states the default in the same paragraph: "Each Document has a current document readiness, a string,
   initially 'complete'", and only a Document that the create-and-initialize algorithm produced is reset to
   "loading" before any script can see it. In this engine that algorithm is document_install, and the document
   it installs is the realm's ACTIVE one — so a createHTMLDocument, a DOMParser parse or an XHR responseXML
   answers "complete", which is both the standard's default and the truth about a tree that has no parser. */
const char *document_readiness_of(const lxb_dom_node_t *doc)
{
    JSContext *realm = document_active_realm_of(doc);

    return realm ? DOC_READINESS[document_readiness(realm)] : DOC_READINESS[2];
}

/* HTML §7.5.9's PAGE SHOWING, which is a Document's and lives where its readiness does.
 *
 * It is the boolean "used to ensure that scripts receive pageshow and pagehide events in a consistent manner
 * (e.g., that they never receive two pagehide events in a row without an intervening pageshow, or vice
 * versa)" — so it is not bookkeeping beside those two fires, it is the CONDITION on both of them: §13.2.7's
 * "the end" asserts it is false and sets it true before firing `pageshow`, and §7.5.9 step 9 fires `pagehide`
 * only if it is true, setting it false first.
 *
 * IT LIVES IN THIS REALM'S OWN BASELINE RECORD, the shape the readiness above and §6.2's visibility state
 * already use, and for the same two reasons: the record is unreachable from the page, so nothing but this
 * component can write the flag; and the write is an ordinary property write, so the heap COW captures it and
 * one arm of a fork can unload its document without touching its sibling's. */
static int g_showing_slot = -1;

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

/* HTML §8.1.7.3 "update the rendering" step 3's RENDER-BLOCKED clause, answered by the component that owns the
   document's load lifecycle rather than guessed by the one that runs the steps.
 *
 * A Document is render-blocked while it has render-blocking elements, and the parser is what removes the last
 * of them: a browser does not present a document, and does not reveal it, until its parse has finished. In
 * this engine the tree is one Lexbor parse and the parser's completion IS stage 0 — the moment the readiness
 * leaves "loading" and DOMContentLoaded fires. It is what puts the first rendering opportunity (and therefore
 * `pagereveal` and the first animation frame) AFTER DOMContentLoaded, which is where a browser puts it. */
bool document_render_blocked(JSContext *ctx)
{
    return document_readiness(ctx) == 0;
}

/* HTML §7.5.8 "Finishing the loading process" — "to COMPLETELY FINISH LOADING a Document document":
 *
 *   1. Assert: document's browsing context is non-null.
 *   2. Set document's completely loaded time to the current time.
 *   3. Let container be document's node navigable's container.
 *   4. If container is an `iframe` element, then queue an element task on the DOM manipulation task source
 *      given container to run the iframe load event steps given container.
 *   5. Otherwise, if container is non-null, then queue an element task on the DOM manipulation task source
 *      given container to fire an event named `load` at container.
 *
 * THIS IS THE ONLY PLACE AN IFRAME'S `load` COMES FROM, and until it existed there was no place at all: the
 * engine fired `load` at the child's WINDOW (step 9.5 above) and nothing whatever at the CONTAINER, so
 * `frame.onload` was a handler that could not run and `frame.addEventListener('load', …)` was a listener
 * nothing dispatched to. Every corpus test that appends a frame and waits for it — which is how a test gets a
 * second document to ask questions of — waited for ever and reported a timeout, in an area whose failures are
 * then about a frame tree nobody ever finished building.
 *
 * STEP 2 IS NOT HERE BECAUSE NOTHING IN THIS TREE ASKS IT. A "completely loaded time" is read by exactly one
 * algorithm, §7.2.4's location-object navigate ("if location's relevant Document is not yet completely
 * loaded … set historyHandling to 'replace'"), and core/frame/location.c answers that from the session
 * history's own initial-about:blank test rather than from a clock. Writing a timestamp here that no reader
 * consults is the mirror of a defaulted read: a field with a producer and no consumer, indistinguishable at a
 * glance from a measurement.
 *
 * THIS IS THE LOAD EVENT STEPS' CALLER FOR A DOCUMENT A NAVIGATION PRODUCED, AND ONLY FOR ONE. §4.8.5's other
 * caller — "process the iframe attributes" with initialInsertion true, over a url that matches about:blank —
 * is built, in core/html/html_iframe.c, and the two do not overlap: the arm inside this function reads the
 * navigable's §7.4.4 "is initial about:blank" and returns, so a frame gets its `load` from exactly one of
 * them. A note here used to argue the opposite, that this path COVERED the initial-insertion case and building
 * that one would fire `load` twice. The premise was that the initial about:blank "reaches this algorithm
 * later, through document_lifecycle_step" — and it does not, because that walk lists only MATERIALIZED
 * navigables and nothing materializes a srcless frame until a read reaches through it, which is what the
 * missing `load` was carrying. The argument was self-sealing rather than wrong-in-detail: the case it claimed
 * to cover was the one case it could never see.
 *
 * `ctx` IS THE CHILD'S REALM — the document whose loading finished — and the fire must be enqueued in the
 * CONTAINER'S. §7.5.8's "element task" has the element's node document's relevant global for its global, which
 * is the parent's, and the parent is where the listener was registered. */
static void completely_finish_loading(JSContext *ctx)
{
    JSValueConst proxy = doc_here(ctx)->proxy;
    JSValue container;
    lxb_dom_node_t *cn;
    JSContext *cctx;

    /* STEP 1. Step 9.2 above already returned for a destroyed navigable, so what is left is a Document with no
       navigable at all — §4.5's createDocument, a DOMParser's — and one of those never reaches §13.2.7's end
       because document_lifecycle_step walks navigables. */
    DCHECK(window_proxy_is(proxy),
           "§7.5.8's completely-finish-loading was reached by a Document with no navigable — §13.2.7 \"The "
           "end\" runs for the documents of this agent's navigables, so a Document that has none has no "
           "loading process for this algorithm to finish");
    container = window_proxy_container(ctx, proxy);                                          /* step 3 */
    cn = node_of(container);
    if (!cn) { JS_FreeValue(ctx, container); return; }   /* a top-level traversable: step 5's "non-null" fails */
    /* STEP 3's OTHER ANSWER, WHICH THIS ENGINE HAS TO STATE BECAUSE ITS CONTAINER LINK IS NEVER NULL. §7.5.8
       "Finishing the loading process" reads the container at step 3 and notes that it "will be null in the case
       where document is the initial about:blank Document in a frame or iframe, since at the point of browsing
       context creation which calls this algorithm, the container relationship has not yet been established …
       The consequence of this is that the following steps do nothing, i.e., we do not fire an asynchronous
       load event on the container element for such cases. Instead, a synchronous load event is fired in a
       special initial-insertion case when processing the iframe attributes."
       §7.3.1.3's create is handed the container element in this engine and writes the link as one of its own
       steps, so the null the standard relies on never arrives and the SAME FACT has to be asked directly:
       window_proxy_ever_navigated is §7.4.4's "document's IS INITIAL about:blank" read off the navigable, and
       false here is exactly the case the note is about. The comment that stood in this block claimed the
       opposite — that because the container is found early, this general path COVERS the initial-insertion
       case and building it would fire `load` twice. It does not cover it and could not: navigable_tree_order
       lists only MATERIALIZED navigables, and a navigable holding its initial about:blank is materialized by
       the first read that reaches THROUGH it — which for a srcless `<iframe>` is the `contentWindow` a page
       touches inside the very `onload` this walk was supposed to deliver. That is a DEADLOCK and not a delay,
       and it is why the claim could stand: the walk's silence and a frame that is merely slow look the same.
       §4.8.5's branch is built (core/html/html_iframe.c) and this is its other half: it fires
       once, at the insertion, and this arm is what keeps a later materialization from firing a second one. */
    if (!window_proxy_ever_navigated(proxy)) { JS_FreeValue(ctx, container); return; }
    /* STEP 4's BRANCH IS THE ONLY ONE THIS ENGINE CAN REACH, and step 5's is an ASSERT rather than three
       written-but-unreachable lines. A content navigable is created for `<iframe>` and for nothing else here
       (html_iframe.c is the only component that creates one from an element), so a container that is not an
       `<iframe>` cannot exist — and the day §4.8.6's `<embed>` or §4.8.7's `<object>` gets one, this is the
       site that must learn step 5, which is what makes the crash the instruction rather than a silent widening
       of step 4 onto an element whose load event is not the iframe load event steps. */
    DCHECK(iframe_element_is(cn),
           "§7.5.8 step 5 — a navigable's container that is not an `<iframe>` finished loading. Content "
           "navigables belong to `<iframe>` alone in this engine, so building §4.8.6's `<embed>` or §4.8.7's "
           "`<object>` as navigable containers means teaching this site step 5: queue an element task on the "
           "DOM manipulation task source given container to FIRE `load` AT IT, which is not the same algorithm "
           "as §4.8.5's iframe load event steps");
    /* THE CONTAINER'S REALM, asked of the container's own document. Two same-origin documents are one agent
       and one heap, so this is a pointer and not a message; a CROSS-ORIGIN container is a peer's element and
       is not in this heap to be found, which is the same boundary window_proxy_container is read across. */
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
        document_set_ready(ctx, 1);
        /* §3.1.1: DOMContentLoaded is fired AT THE DOCUMENT and BUBBLES, which is how a `window.onload`-style
           listener registered on window hears it — the propagation path derives that from the document's
           ancestors now rather than the caller naming the window. It is not cancelable. */
        event_target_fire(ctx, doc_here(ctx)->doc_obj,
                          event_new(ctx, "DOMContentLoaded", /*bubbles*/ true, /*cancelable*/ false), JS_UNDEFINED);
        return 1;
    }
    DCHECK(stage == 1, "the document lifecycle was asked for a stage it does not have");
    /* §13.2.7 "The end", STEP 9 — the second of its two queued global tasks. Its sub-steps are in this order
       and each of them is observable.
       THE NUMBER WAS 7 HERE AND IN THREE OTHER PLACES, AND 7 IS A DIFFERENT STEP: step 7 is "Spin the event
       loop until the set of scripts that will execute as soon as possible and the list of scripts that will
       execute in order as soon as possible are empty", which is a GATE on this task and not this task. Step 8
       is the other gate ("Spin the event loop until there is nothing that delays the load event in the
       Document") — so a reader sent to "step 7" to find the `load` fire arrived at the ASAP-script wait, which
       core/loader/script_type.h already cites correctly, and the two halves of §13.2.7 read as one. */
    document_set_ready(ctx, 2);                                                  /* step 9.1 */
    /* STEP 9.2: "If the Document object's browsing context is null, then abort these steps." A document whose
       navigable was destroyed under it — a frame removed, a traversable closed — must not fire `load` or
       `pageshow` at a Window whose browsing context is gone, and §7.5.9 destroys documents while this walk is
       still running. It is the destruction that is asked for and not `closed`: a traversable that is merely
       CLOSING still has its browsing context, and its documents are still finishing. */
    {
        JSValueConst proxy = doc_here(ctx)->proxy;
        if (window_proxy_is(proxy) && window_proxy_destroyed(proxy))
            return 1;
    }
    /* STEP 9.5: `load` at the WINDOW — it does not bubble (there is nothing above it to bubble to) and it is
       fired WITH THE LEGACY TARGET OVERRIDE FLAG SET, so a listener's `e.target` is the DOCUMENT. */
    event_target_fire(ctx, doc_here(ctx)->win_obj,
                      event_new(ctx, "load", /*bubbles*/ false, /*cancelable*/ false),
                      doc_here(ctx)->doc_obj);
    /* STEPS 9.9-9.11: the document is now SHOWING, and `pageshow` says so with `persisted` false — this
       document was loaded rather than restored from a session history entry. §7.5.9's `pagehide` is the other
       end of that pair and fires only because this ran. */
    /* STEPS 9.9-9.11 ARE A TRANSITION, AND §8.4.1 IS WHY THAT SENTENCE HAD TO BE WRITTEN DOWN. What stood here
       was §13.2.7 step 9.9's assert, taken literally — "Assert: Document's page showing is false" — with the
       reasoning that a document reaching the end twice would fire two pageshows with no pagehide between them.
       THE INVARIANT IS RIGHT AND THE ASSERT IS NOT, because reaching the end twice is a state the standard
       itself creates: §8.4.1 "Opening the input stream" replaces a Document IN PLACE, mints a new parser and
       sets the readiness back to "loading", so `document.open(); document.write(…); document.close()` runs
       §13.2.7 again on a Document that is already showing — and §8.4.1 never unloads it, so nothing has fired
       `pagehide`. §7.5.9 "Unloading documents" is the only algorithm that clears the flag, and it is the one
       the pair is defined against. So the flag stays the CONDITION on 9.10 and 9.11 (which is what §7.5.9's
       own `pagehide` does with it) rather than an assertion about how many times an end can be reached. The
       `load` fire above is deliberately NOT under this test: a browser fires `load` again after a
       document.close(), which is what an `iframe`'s `onload` after a written document depends on. */
    if (!document_page_showing(ctx)) {
        document_page_showing_set(ctx, true);
        event_target_fire(ctx, doc_here(ctx)->win_obj,
                          page_transition_event_new(ctx, "pageshow", /*persisted*/ false),
                          doc_here(ctx)->doc_obj);
    }
    completely_finish_loading(ctx);                                              /* step 9.12 */
    return 1;
}

/* HTML §13.2.7 "The end" STEP 8 — "Spin the event loop until there is nothing that DELAYS THE LOAD EVENT in
 * the Document" — as a PREDICATE over this agent's navigable tree, asked of one document.
 *
 * IT IS A GATE THE ENGINE DID NOT HAVE, AND ITS ABSENCE WAS BEING COVERED BY THE SCHEDULER'S QUIESCENCE. The
 * `load` stage below used to run whenever the walk found a document at "interactive", and the only thing that
 * had ever kept it in order was that solver/engine.c would not ask for a lifecycle stage until the running
 * flow had NOTHING left — no program, no job, no reply the host still owed. That is not step 8: quiescence is
 * both too strong and too weak, and it is wrong in the two directions separately.
 *   TOO STRONG, which is the defect a page feels: HTML makes NOTHING about a `fetch()` or a dynamic `import()`
 *     delay the load event, and a flow parked on one is not quiescent — so a bundle with one request still in
 *     the air at the end of its parse never reached `load` at all, and every listener behind it (an SPA
 *     bootstrap, a router, a lazy-loader, an analytics beacon) never ran.
 *   TOO WEAK, which is what this predicate is for: quiescence is a fact about ONE FLOW and the load event is a
 *     fact about ONE DOCUMENT, so nothing in it says a parent may not finish ahead of its own frames.
 *
 * THE SOURCE BUILT HERE IS THE NAVIGABLE CONTAINER'S, which §4.8.5 "The iframe element" states for every element
 * type that POTENTIALLY DELAYS THE LOAD EVENT (`iframe`, `embed`, `object`): "the user agent must delay the
 * load event of element's node document if element's content navigable is non-null and any of the following
 * are true: element's content navigable's active document is not ready for post-load tasks; element's content
 * navigable's is delaying load events is true; or anything is delaying the load event of element's content
 * navigable's active document."
 * ALL THREE BULLETS ARE ONE QUESTION HERE, and that is the tree's doing rather than a simplification. "Ready
 * for post-load tasks" is §13.2.7's own step 11, which follows step 9.1's readiness "complete" with nothing
 * observable between them — so readiness < 2 IS the first bullet. And the walk below is over the WHOLE
 * subtree, not over one generation, so a grandchild that is still loading answers the third bullet by being in
 * the same list. Asked of the CONTAINER relation (window_proxy_parent_navigable) rather than of the element,
 * because that relation is what §7.3.1.3 writes and what §7.3.1.6 severs — an `<iframe>` the running flow has
 * detached is not this document's delay any more, and the navigable's own parent link is where that shows.
 *
 * WHAT IT DOES NOT ANSWER, AND WHERE THAT IS ANSWERED INSTEAD. navigable_tree_order lists only the navigables
 * this instance has MATERIALIZED and only the LOCAL ones, so two kinds of child are invisible to this walk:
 *   - A CHILD WHOSE DOCUMENT IS STILL BEING FETCHED has no realm yet. It is covered, but by the SCHEDULER and
 *     not here: §7.4 step 14's load is a job whose fetch parks as a synchronous host request
 *     (solver/engine.c's engine_host_request pushes FLOW_PENDING_HOSTREQ), which makes flow_blocked true, and
 *     the arm that asks for a lifecycle stage will not ask while the flow is blocked. So the interlock exists
 *     — it is just one layer up, and it is stated in both places rather than assumed in either.
 *   - A CROSS-ORIGIN CHILD IS IN ANOTHER WASM INSTANCE and is filtered out as REMOTE. Its delay is a fact this
 *     instance cannot compute alone: §4.8.5 "The iframe element"'s three bullets are all questions about the child's
 *     ACTIVE DOCUMENT, which lives in the peer. It is the cross-instance read §Security describes, and until
 *     that read exists a parent whose only unfinished frame is cross-origin fires `load` EARLY. That is a hole
 *     with a name and an owner, not a defaulted answer.
 * The remaining sources of step 8 are per-ELEMENT flags this engine keeps no record of — see the call site. */
static bool document_load_event_delayed(JSContext *ctx, JSValueConst docs, uint32_t n, JSValueConst self)
{
    uint32_t j;

    for (j = 0; j < n; j++) {
        JSValue other = JS_GetPropertyUint32(ctx, docs, j);
        JSValue up;
        bool below = false;

        if (JS_IsSameValue(ctx, other, self)) { JS_FreeValue(ctx, other); continue; }
        /* Is `self` an ANCESTOR of `other`? Walk the navigable's containing-navigable chain, which ends at
           JS_UNDEFINED for a top-level traversable — so a document in another tree of this agent (an auxiliary
           navigable a `window.open()` created) terminates without ever matching, which is right: a popup is
           nobody's navigable container and delays nobody's load event. */
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

/* THE LOAD LIFECYCLE IS PER DOCUMENT, and it was per FLOW — one counter, for one document, driven with the
 * ROOT realm's ctx. That is not a small mismatch: HTML gives every Document its own readiness and its own
 * DOMContentLoaded, so a CHILD navigable's document could never leave "loading". It never fired
 * DOMContentLoaded, never fired `load`, and — the moment §8.1.7.3 step 3's render-blocked clause existed — was
 * removed from every rendering opportunity for ever. A child document simply never ran the half of its code
 * that is behind those events, and nothing anywhere said so; wpt's
 * animation-frames/callback-cross-realm-report-exception is one test that says it out loud.
 *
 * SO THE STAGE IS READ FROM THE DOCUMENT, and the flow's counter is DELETED. The readiness is already
 * per-document and per-flow (an own slot on each realm's Document, isolated by the COW delta), which is
 * exactly what the counter was trying to be and could not be, because one integer cannot hold N documents.
 *
 * THE ORDER IS THE SPEC'S, and it is two passes rather than one. Every document's DOMContentLoaded comes
 * before any document's `load` — a parent's parse finishes while its frames are still loading — and a CHILD's
 * `load` fires before its PARENT's, because a parent's load waits for its subframes. Tree order gives the
 * first; the REVERSE of tree order gives the second, since a container precedes what it contains.
 * THE REVERSE ORDER IS ONLY THE ORDER, NOT THE WAIT, and reading it as both was the gap document_load_event_
 * delayed above now fills: reversing the walk decides which of two documents that are BOTH ready goes first,
 * and says nothing whatever about a parent that is ready while its child is not. §13.2.7 step 8 is the wait,
 * and it is asked per document in pass two.
 *
 * ONE DOCUMENT PER CALL, then return, because this is a work item on the one frontier like everything else the
 * scheduler asks for — each fire queues listener tasks the loop picks up before the next document's stage.
 * Returns 1 when it advanced one, 0 when every document of this agent is complete. */
/* §13.2.7 "The end"'s OWN PREMISE, asked of one document: "Once the user agent stops parsing the document, the
 * user agent must run the following steps." The readiness alone used to stand for it, and it stopped standing
 * for it the moment §8.4.1 "Opening the input stream" existed: those steps set the readiness back to "loading"
 * and open an input stream that only §8.4.2's `close()` ends, so a `document.open()` with no close would have
 * had this walk fire DOMContentLoaded and `load` on a document whose parse had not begun to finish — which a
 * browser never does, and which would then have fired them a second time when the close finally came.
 * §13.2.3.5's insertion point IS "the parse has not ended" (§13.2.7's step 2 is what makes it undefined), so
 * this asks the parser rather than inferring from the readiness. A realm with no Document record cannot be
 * mid-parse: there is no tree for a parser to be building. */
static bool document_parse_ended(JSContext *realm)
{
    const Document *d = doc_of(realm);

    return d == NULL || !html_parse_insertion_point_defined(lxb_dom_interface_document(d->dom));
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
        if (document_readiness(realm) == 0 && document_parse_ended(realm)) {
            document_done_stage(realm, 0);
            DCHECK(document_readiness(realm) == 1,
                   "a document's DOMContentLoaded stage ran and left its readiness where it was — this walk "
                   "would then pick the same document for ever, queueing its listeners again every turn, which "
                   "is a live-lock the scheduler cannot tell from progress");
            did = 1;
        }
        JS_FreeValue(ctx, proxy);
    }
    for (i = n; i > 0 && !did; i--) {          /* pass two: `load`, innermost first */
        JSValue proxy = JS_GetPropertyUint32(ctx, docs, i - 1);
        JSContext *realm = window_proxy_realm(ctx, proxy);
        /* §13.2.7 STEP 8 — the spin. The reverse walk is what puts a CHILD's `load` before its parent's when
           both are ready at once; this is what holds the parent when the child is not ready YET, which the
           order alone cannot express and which quiescence used to hide.
           THE SOURCES THIS PREDICATE DOES NOT YET CARRY are `img` (§4.8.4.3.5 "Updating the image data": "When
           delay load event is true, fetching the image must delay the load event of the element's node
           document"), the media elements' §4.8.11.5 "Loading the media resource" delaying-the-load-event flag, and
           §4.12.1.1 "Processing model"'s per-`script` delaying the load event. Each is a per-ELEMENT flag with
           no record in this engine, so each is a document that may reach `load` EARLY — the same shape this
           whole gate exists to end, one element type down. They are not defaulted-to-false here and then
           forgotten: the flag belongs to the element's own component, and the honest place for its absence to
           crash is where that component would raise it, not in a `||` chain here that would have to guess at
           the state of an element it does not own. */
        if (document_readiness(realm) == 1 && !document_load_event_delayed(ctx, docs, n, proxy)) {
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

/* §3.1.1's `location` — the LOCATION OBJECT OF THIS DOCUMENT'S RELEVANT GLOBAL. It was absent, and absent is
   not a small gap here: `document.location.pathname` is how WPT's own /common/PrefixedPostMessage.js names a
   message channel, so 63 subtests across html/browsers failed on a property of undefined without ever reaching
   what they were testing. The IDL audit had it listed among Document's absent members the whole time.
   IT IS THE GLOBAL'S, not a second Location: a document and its window are one browsing context and §3.1.1 says
   "the Location object of this's relevant global object", so this reads the one location.c built for this realm
   rather than building another that would compare unequal to it.
   ASKED OF THE COMPONENT, not read off the global. `window.location` is §7.2.2's `[LegacyUnforgeable] readonly
   attribute Location`, so it is an ACCESSOR, and a JS_GetPropertyStr that reaches a getter aborts — there is no
   flow base under a C activation. The JS_IsUndefined arm that stood here went with it: it was the "a host
   installed no Location at all" case, and a Location is a per-realm intrinsic now, so a realm without one is a
   realm that declined the component and realm_value_get says so at the read instead of this answering null. */
static JSValue js_doc_location(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    /* §3.1.1: null when this document is not FULLY ACTIVE — and a document §4.5 created has no browsing context
       at all, so it can never be. It was the realm's `location` unconditionally, which handed a page an address
       belonging to a different document the moment a second one existed. */
    Document *d = doc_receiver(ctx, this_val);

    if (!d) return JS_EXCEPTION;
    if (JS_IsUndefined(d->proxy))
        return JS_NULL;
    return location_object(ctx);
}


/* §3.1.1's TREE ENTRY POINTS, COMPUTED FROM THE RECEIVER'S TREE — documentElement, body, head and doctype.
 *
 * They were four DATA PROPERTIES latched onto the `document` object at install, and that is wrong twice. It is
 * wrong in TIME: each is defined as a lookup in the tree AS IT IS, so a page that replaces `<body>` (which
 * `document.body = el` and a `replaceChild` both do) went on being handed the node the parse produced. And it
 * is wrong in SUBJECT: a value stored on one object cannot answer for a second document, so
 * `implementation.createHTMLDocument("").body` was undefined — the whole reason this had to be looked at.
 * magic 0 = documentElement, 1 = body, 2 = head, 3 = doctype. */
static lxb_dom_node_t *doc_child_named(lxb_dom_node_t *parent, const char *a, const char *b)
{
    lxb_dom_node_t *n;

    for (n = parent ? parent->first_child : NULL; n; n = n->next) {
        size_t qn = 0;
        const lxb_char_t *q;
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        q = lxb_dom_element_qualified_name(lxb_dom_interface_element(n), &qn);
        if (!q) continue;
        if ((qn == strlen(a) && !memcmp(q, a, qn)) || (b && qn == strlen(b) && !memcmp(q, b, qn)))
            return n;
    }
    return NULL;
}

/* §4.5 "document element": the ELEMENT child of the document. There is at most one, and NULL is a real answer —
   a document §4.5's createDocument built with no qualified name has none. */
lxb_dom_node_t *document_document_element_of(const lxb_dom_node_t *doc)
{
    lxb_dom_node_t *n;

    if (!doc || doc->type != LXB_DOM_NODE_TYPE_DOCUMENT)
        return NULL;
    for (n = doc->first_child; n; n = n->next)
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT)
            return n;
    return NULL;
}

/* §3.1.1's `body`, AS A LOOKUP ANOTHER COMPONENT CAN MAKE. HTML §6.6.6's `activeElement` ends in exactly these
   two steps ("if candidate has a body element, return that body element; if candidate's document element is
   non-null, return that document element"), and a second walk written there would be a second answer to a
   question this file already answers — which is what made `document.body` a latched data property once. */
lxb_dom_node_t *document_body_of(const lxb_dom_node_t *doc)
{
    return doc_child_named(document_document_element_of(doc), "body", "frameset");
}

/* DOM §2.7's DEFAULT PASSIVE VALUE names four targets, and three of them are this file's definitions: the node
   DOCUMENT itself, its document element, and its body. The fourth is the Window, which is not a node and is
   answered where the registration is. It lives here so the two §3.1.1 lookups have one implementation. */
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

/* HTML's "the document's relevant global object", which §2.9's get the parent puts above a Document in the
   event path. BORROWED, and JS_NULL for a document with no browsing context — one `createHTMLDocument` built,
   whose events therefore stop at the document exactly as the spec says. */
JSValueConst document_window_of(const lxb_dom_node_t *n)
{
    Document *d = n ? doc_rec(n->owner_document) : NULL;

    if (!d || !JS_IsObject(d->win_obj))
        return JS_NULL;
    return d->win_obj;
}

static JSValue js_doc_tree(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *doc = node_of(this_val), *root, *n;

    /* WEB IDL §3.7.5's brand check — a TypeError, not an assert; see doc_receiver. */
    if (!doc || doc->type != LXB_DOM_NODE_TYPE_DOCUMENT)
        return JS_ThrowTypeError(ctx, "this is not a Document");
    root = document_document_element_of(doc);
    switch (magic) {
    case 0:
        return node_wrap(ctx, root);
    case 1:
        /* §3.1.1: "the first of the html element's children that is either a BODY or a FRAMESET element, or
           null" — a frameset document has no body at all, which is the parser following the spec. */
        return node_wrap(ctx, document_body_of(doc));
    case 2:
        /* §3.1.1: "the first head element that is a child of the html element". */
        return node_wrap(ctx, doc_child_named(root, "head", NULL));
    default:
        DCHECK(magic == 3, "a Document tree accessor was declared with a magic this table does not name");
        /* §4.5 doctype: "the first DocumentType node child, in tree order, or null". */
        for (n = doc->first_child; n; n = n->next)
            if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE) return node_wrap(ctx, n);
        return JS_NULL;
    }
}

/* §4.5 / §3.1.1's PER-DOCUMENT STRINGS. Every one is a fact about the receiver rather than about the realm, and
   every one was a data property latched at install and therefore absent on any other document.
   magic 0 = URL and documentURI (§4.5 defines the second as an alias of the first), 1 = contentType,
   2 = compatMode, 3 = characterSet / charset / inputEncoding. */
static JSValue js_doc_strings(JSContext *ctx, JSValueConst this_val, int magic)
{
    Document *d = doc_receiver(ctx, this_val);

    if (!d) return JS_EXCEPTION;
    switch (magic) {
    /* §4.5: "The URL and documentURI getter steps are to return this's URL, SERIALIZED" — so this member IS
       the address, and it answers with the address VALUE rather than with a fresh string of its bytes. That is
       not a derivation and needs no operation name: the value's example IS that serialization, which
       doc_addr_assert_agrees checks at both writers. A page that computed its own route and then branches on
       `document.URL.includes("/admin")` therefore forks, exactly as the same branch through
       `location.href` does — one address, one answer, and no member of it that quietly reads as concrete. */
    case 0:
        DCHECK(!JS_IsUndefined(d->addr.value),
               "§4.5's `URL` was read off a record holding no address value — doc_rec_new writes one at the "
               "record's birth, so this is a record some other path built and the member would report a "
               "COMPUTED address as `undefined` rather than as the address it is");
        return JS_DupValue(ctx, d->addr.value);
    case 1:
        return JS_NewString(ctx, d->content_type);
    case 2:
        /* §4.5: "BackCompat" if the document is in quirks mode, "CSS1Compat" otherwise — the PARSER's answer,
           read off the tree it built rather than assumed. */
        return JS_NewString(ctx, d->dom->dom_document.compat_mode == LXB_DOM_DOCUMENT_CMODE_QUIRKS
                                     ? "BackCompat" : "CSS1Compat");
    default:
        DCHECK(magic == 3, "a Document string accessor was declared with a magic this table does not name");
        /* DOM §4.5 Interface Document: "The characterSet, charset, and inputEncoding getter steps are to
           return this's encoding's NAME." `charset` and `inputEncoding` are the spec's own historical aliases
           of `characterSet` and are the SAME getter rather than three that could disagree.
           THE NAME IS ENCODING §4.2 Names and labels' `Name` COLUMN, WHICH IS MIXED CASE ("UTF-8",
           "Shift_JIS", "ISO-8859-8-I") — a DIFFERENT column from §7.1 Interface mixin TextDecoderCommon's
           `encoding`, which is "this's encoding's name, ASCII lowercased". Asking for the wrong one of the two
           is a defect that reads as a spelling choice: it reports `shift_jis` for a member every browser
           answers `Shift_JIS` for. `encoding_name` is the one this line owes and `encoding_name_ascii_
           lowercased` is the other; the registry keeps both because the standard has both.
           THE VALUE IS THE RECORD'S, which is §3.1.1's per-Document state and not a constant at this line:
           DOM §4.5 gives a document the utf-8 encoding "unless stated otherwise", and HTML §13.2.3.2
           "Determining the character encoding" is what states otherwise (core/html/html_encoding_sniff.h),
           writing it through document_set_encoding when a navigation's response is decoded. */
        return JS_NewString(ctx, encoding_name(d->encoding));
    }
}

/* §3.1.5's `title` AND §3.2.6.4's `dir`, AND WHY NEITHER IS A STORED STRING.
 *
 * `title` was one: install read `lxb_html_document_title` once and wrote the answer onto the `document` object.
 * That is wrong in all three of the ways §3.1.5's algorithm is an algorithm. It is wrong in TIME — the getter is
 * defined as a walk of the tree AS IT IS, so `document.querySelector("title").textContent = "x"` did not change
 * `document.title`, and neither did inserting a `<title>` into a document that had none. It is wrong in SUBJECT
 * — a value on one object cannot answer for a second Document. And it has no SETTER at all, so
 * `document.title = "x"` stored a string where the standard CREATES A TITLE ELEMENT AND APPENDS IT TO THE HEAD:
 * a page that sets its title and then reads its own `<head>` markup back saw markup it never got.
 *
 * `dir` is §3.2.6.4's reflection of the html element's `dir` content attribute LIMITED TO ONLY KNOWN VALUES,
 * which is what makes the getter a filter rather than a read: `<html dir=sideways>` answers the empty string,
 * and so does a document with no html element. */

/* The first ELEMENT of `root`'s subtree, in TREE ORDER, whose namespace is `ns` and whose qualified name is
   `name` — the lookup "the title element of a document is the first title element in the document" is. */
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

/* The same in ONE generation — §3.1.5's SVG arm says "the first SVG title element that is a CHILD OF the
   document element", which is a different lookup and not a narrowing of the one above. */
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

/* §3.1.5's `title` GETTER, whose two remaining steps are DOM §4.4's CHILD TEXT CONTENT ("the concatenation of
   the data of all the Text node CHILDREN of node, in tree order" — children, never descendants) and
   Infra's STRIP AND COLLAPSE ASCII WHITESPACE (every run of TAB/LF/FF/CR/SPACE becomes one U+0020, and the
   leading and trailing ones go). Both are done into one buffer because a collapse is a copy either way.
   Returns an owned JS string; `el` may be NULL, which is the "or the empty string if the title element is null"
   arm rather than a guard. */
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
    /* §7.2.2's defaultView getter steps: "If this's browsing context is null, then return null. Return this's
       browsing context's WindowProxy object." A §4.5 Document has no browsing context and so no view — which is
       the same `proxy` absence `location` answers null for, asked of the same field. */
    if (magic == DOC_DEFAULT_VIEW)
        return JS_IsUndefined(d->proxy) ? JS_NULL : JS_DupValue(ctx, d->proxy);

    doc = lxb_dom_interface_node(d->dom);
    de = document_document_element_of(doc);
    if (magic == DOC_DIR) {
        /* §3.2.6.4: the html element's `dir`, LIMITED TO ONLY KNOWN VALUES — an unknown or absent one is the
           empty string, and so is a document with no html element. The comparison is ASCII case-insensitive
           because `dir` is an enumerated attribute, whose keywords are matched that way. */
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
    /* §3.1.5 STEP 1: an SVG document element answers from the first SVG `title` that is a CHILD of it. */
    if (de && de->ns == LXB_NS_SVG)
        return doc_child_text_stripped(ctx, doc_first_child_ns(de, LXB_NS_SVG, "title"));
    /* STEP 2: otherwise the TITLE ELEMENT, which is the first `title` in the document in tree order. */
    return doc_child_text_stripped(ctx, doc_first_in_tree(doc, LXB_NS_HTML, "title"));
}

/* DOM §4.4's STRING REPLACE ALL, which is where both of §3.1.5's setter branches end: remove every child
   through the per-flow chokepoint, then append ONE Text node — and only if the string is not empty, because
   "string replace all" with the empty string adds no node and a page reading `firstChild` can tell. */
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

/* §3.1.5's `title` SETTER and §3.2.6.4's `dir` SETTER — "the steps corresponding to the FIRST MATCHING
   CONDITION", in the standard's own order, with its own "Otherwise: do nothing" as the last arm rather than as
   a guard in front. magic 0 = title, 1 = dir. */
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
    /* UNKNOWN EXTERNAL INPUT has no bytes: the SHAPE is what the tree carries, exactly as it is for
       `textContent =`, so a title set from attacker input reads back as the source it came from. */
    if (concolic_is(val)) {
        s = concolic_shape_c(val);
        if (!s) s = "";
        len = strlen(s);
    } else {
        s = JS_ToCStringLen(ctx, &len, val);
        if (!s) return JS_EXCEPTION;
    }

    if (magic == DOC_DIR) {
        /* §3.2.6.4: "If there is no such element, then the attribute must return the empty string and DO
           NOTHING ON SETTING." A reflection limited to known values does not validate what it is given — the
           filter is the getter's — so the content attribute takes the string as written. */
        if (de && de->ns == LXB_NS_HTML)
            dom_cow_set_attribute(lxb_dom_interface_element(de), "dir", s, len, JS_UNDEFINED);
        goto done;
    }
    DCHECK(magic == DOC_TITLE, "a Document string setter was declared with a magic this table does not name");
    if (de && de->ns == LXB_NS_SVG) {
        /* The SVG arm: reuse the first SVG `title` child, or CREATE one and insert it as the document
           element's FIRST child — the position is the standard's, and an append would put the title after the
           graphics that a renderer reads it in front of. */
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
        lxb_dom_node_t *head = doc_child_named(de, "head", NULL);
        el = doc_first_in_tree(doc, LXB_NS_HTML, "title");
        /* "If the title element is null AND the head element is null, then return" — a `<frameset>` document
           is exactly that, and a title it is given is dropped rather than given a head to live in. */
        if (!el && !head) goto done;
        if (!el) {
            lxb_dom_element_t *made = lxb_dom_document_create_element(lxb_dom_interface_document(d->dom),
                                                                      (const lxb_char_t *)"title", 5, NULL);
            CHECK(made != NULL, "document.title=: the title element could not be created");
            el = lxb_dom_interface_node(made);
            dom_cow_note_created(el);
            dom_cow_append_child(head, el);
        }
        doc_string_replace_all(ctx, el, s, len);
    }
    /* "Otherwise: do nothing" — a document element in neither namespace, or none at all. */
done:
    if (!concolic_is(val)) JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* §4.5's TWO TRAVERSER FACTORIES. `createNodeIterator(root, whatToShow, filter)` and `createTreeWalker(...)`
   are the same five-line construction with a different object at the end, so they are one body with a magic —
   and neither runs a line of the page's code once its arguments are converted, which is why they are plain C
   and their MEMBERS are machines. The IDL is what does the work: `Node root` is an interface type (a non-Node
   is a TypeError before step 1), `optional unsigned long whatToShow = 0xFFFFFFFF` is ToNumber and a modulo, and
   `optional NodeFilter? filter = null` accepts a function OR any object and rejects a primitive.
   magic 0 = createNodeIterator, 1 = createTreeWalker. */
static JSValue js_doc_create_traverser(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                       int magic)
{
    JSValueConst root = argc > 0 ? argv[0] : JS_UNDEFINED, filter = JS_NULL;
    uint32_t what = 0xFFFFFFFFu;

    DCHECK(node_of(this_val) != NULL, "a traverser factory ran on something that is not a document");
    if (!node_of(root))
        return JS_ThrowTypeError(ctx, "createNodeIterator/createTreeWalker requires a Node root");
    /* The IDL's defaults. An absent optional argument arrives as undefined, which is what §3.6 means by
       absent — so the default is applied here and nowhere else. */
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToUint32(ctx, &what, argv[1]) < 0)
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) filter = argv[2];
    return magic == 0 ? node_iterator_new(ctx, root, what, filter)
                      : tree_walker_new(ctx, root, what, filter);
}

/* §4.5 `[NewObject] Range createRange()` — "a new live range with (this, 0) as its start and end". It is the
   same construction `new Range()` performs, and it is the Document's rather than the current global's: a page
   that calls `otherDoc.createRange()` gets a range rooted in THAT document. */
static JSValue js_doc_create_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    (void)argc; (void)argv; (void)magic;
    DCHECK(node_of(this_val) != NULL, "createRange ran on something that is not a document");
    return range_new_at(ctx, this_val);
}

/* §4.5 `[NewObject] Event createEvent(DOMString interface)` — the legacy factory. The TABLE and the
   construction belong to the events component; what is Document's is the member and the realm whose interfaces
   the exposure check asks about, which is this document's global. */
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

/* The Document METHODS — on Document.prototype, so there is one of each rather than one per install, and so
   `Document.prototype.querySelector` is a thing that exists. */
/* THE DECLARATIONS ARE THE AGENT'S, THE INSTALLS ARE THE REALM'S — the IDL pool is sealed after agent init, so
   a declaration minted from a per-realm install trips idl_declared_before_seal on the SECOND realm. */
static JSClassID g_document_class;   /* §3.1.1's prototype slot, per realm */
/* THE RUNTIME THIS COMPONENT DECLARED INTO, AND THE DECLARATION LATCH — one static answering both, the shape
   core/dom/element.c's `g_element_rt` states for the group this one is declared beside. `document_init` had no
   double-declaration assert at all, which is the defect core/agent_state.h records for window_message: a second
   declaration in one process would have re-minted the class every existing wrapper is branded with, re-declared
   fifteen members into a sealed pool, and left the first agent's sub-components holding handles nobody freed.
   It is also what the release below is checked against — every atom and every value the cascade gives back is
   freed against a runtime, and JS_FreeValueRT reports nothing at all when it is the wrong one. */
static JSRuntime *g_document_rt;
static int g_id_create_element = -1, g_id_create_text = -1, g_id_create_comment = -1,
           g_id_create_fragment = -1, g_id_create_element_ns = -1, g_id_create_iterator = -1,
           g_id_create_walker = -1, g_id_create_range = -1, g_id_create_event = -1,
           g_id_create_cdata = -1, g_id_create_pi = -1, g_id_doc_ctor = -1, g_id_adopt_node = -1,
           g_id_title_set = -1, g_id_dir_set = -1, g_id_location_set = -1;
/* THE SAME FOUR TOUCH HANDLERS core/html/html_element.c excludes, and for the same reason — this interface
   includes the same `GlobalEventHandlers`, so §Touch Events Level 2's "this mixin must not be implemented"
   reaches it too. The list is stated HERE rather than shared from there because idl_members_excluded reads the
   interface name as a literal at the call and resolves its table per file; one list named from three sites is
   not expressible, and html_element.c says so where it declares its own. */
static const char *const TOUCH_EXCLUDED[] = { "ontouchstart", "ontouchend", "ontouchmove", "ontouchcancel" };


static void document_declare_members(JSContext *ctx)
{
    /* §4.5's `[CEReactions, NewObject] Element createElement(DOMString localName,
       optional (DOMString or ElementCreationOptions) options = {})` — a STEP because §4.9 step 5.1.4.1
       constructs the page's custom element class synchronously inside it.
       THE OPTIONS ARGUMENT IS DECLARED, not read by the body: §3.2.25's union and the dictionary's two members
       are conversions, and every one of them is a rest point — a page accessor on `customElementRegistry` is
       its code, and a body reaching for it with JS_GetPropertyStr would run that inside a C activation. */
    {
        static const IdlArgType CREATE_EL[2] = { IDL_DOMSTRING, IDL_STRING_OR_DICT };
        /* §4.5's `dictionary ElementCreationOptions { CustomElementRegistry? customElementRegistry;
           DOMString is; }`, in the IDL's own order because that is the order Web IDL reads them in. The
           registry crosses UNCONVERTED and is brand-tested by flatten_creation_options, because the class is
           custom_elements.c's own and this component may not name it. */
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
    g_id_create_cdata = idl_method_id(ctx, IDL_1STR, 1, js_doc_create_xml_node, 0);
    g_id_create_pi = idl_method_id(ctx, IDL_2STR, 2, js_doc_create_xml_node, 1);
    /* §4.5's `[Exposed=Window] interface Document { constructor(); }` — the interface object is CONSTRUCTIBLE,
       which it was not, and `new Document()` is how a page gets an XML document without DOMImplementation. */
    g_id_doc_ctor = idl_method_id(ctx, NULL, 0, js_doc_ctor, 0);
    g_id_create_fragment = idl_method_id(ctx, NULL, 0, js_doc_create_fragment, 0);
    g_id_create_element_ns = idl_method_id(ctx, IDL_NSSTR_STR, 2, js_doc_create_element_ns, 0);
    {
        /* §4.5: `(Node root, optional unsigned long whatToShow = 0xFFFFFFFF, optional NodeFilter? filter =
           null)`, twice. */
        static const IdlArgType TRAVERSER[3] = { IDL_INTERFACE, IDL_UNSIGNED_LONG,
                                                 IDL_CALLBACK_INTERFACE_NULLABLE };
        g_id_create_iterator = idl_method_id(ctx, TRAVERSER, 3, js_doc_create_traverser, 0);
        idl_iface_brand(node_class_id());
        idl_optional_from(1);
        g_id_create_walker = idl_method_id(ctx, TRAVERSER, 3, js_doc_create_traverser, 1);
        idl_iface_brand(node_class_id());
        idl_optional_from(1);
    }
    g_id_create_range = idl_method_id(ctx, NULL, 0, js_doc_create_range, 0);
    g_id_create_event = idl_method_id(ctx, IDL_1STR, 1, js_doc_create_event, 0);
    {
        /* §4.5's `[CEReactions] Node adoptNode(Node node)`. The argument is an INTERFACE type, so the
           declaration brands it and a non-Node is a TypeError before the member's step 1; the `[CEReactions]`
           half is the machine's own epilogue, which drains the adoptedCallback reactions adopt enqueued before
           the member's value is returned. */
        static const IdlArgType ONE_NODE[1] = { IDL_INTERFACE };
        g_id_adopt_node = idl_method_id(ctx, ONE_NODE, 1, js_doc_adopt_node, 0);
        idl_iface_brand(node_class_id());
    }
    /* §3.1.1's `[CEReactions] attribute DOMString title` and `[CEReactions] attribute DOMString dir`. The
       `[CEReactions]` half is the args machine's own epilogue, which drains the reactions the element creation
       and the insertions below enqueue before the setter returns. */
    g_id_title_set = idl_setter_id(ctx, IDL_DOMSTRING, false, js_doc_set_html_member, DOC_TITLE);
    g_id_dir_set = idl_setter_id(ctx, IDL_DOMSTRING, false, js_doc_set_html_member, DOC_DIR);
    /* HTML §3.1.1 The Document object's `[PutForwards=href, LegacyUnforgeable] readonly attribute Location?
       location` — the SECOND carrier of the same extended attribute, and the reason the forwarding is Web IDL's
       and not location.c's: `document.location = url` and `window.location = url` are ONE binding rule read off
       two IDL lines, and a component-local copy would make one spelling of the same assignment answer
       differently from the other. The pair is (`location`, `href`) exactly as §7.2.2's is. */
    g_id_location_set = idl_setter_id_put_forwards(ctx, "location", "href");
    /* EACH POOL ENTRY IS AGENT-LIFETIME STATE, DECLARED BESIDE THE LINE THAT MINTS IT (core/agent_state.h). A
       release that gives the pool back and keeps these numbers is exactly what fetch_free did — the next agent's
       `_init` reads one to decide it need not run, and every member then answers out of a pool a dead runtime
       issued. They are declared here rather than at the end of document_init because here is where they are set. */
    agent_state_id("document", &g_id_create_element, "§4.5's createElement machine");
    agent_state_id("document", &g_id_create_text, "§4.5's createTextNode");
    agent_state_id("document", &g_id_create_comment, "§4.5's createComment");
    agent_state_id("document", &g_id_create_cdata, "§4.5's createCDATASection");
    agent_state_id("document", &g_id_create_pi, "§4.5's createProcessingInstruction");
    agent_state_id("document", &g_id_doc_ctor, "§4.5's `new Document()`");
    agent_state_id("document", &g_id_create_fragment, "§4.5's createDocumentFragment");
    agent_state_id("document", &g_id_create_element_ns, "§4.5's createElementNS");
    agent_state_id("document", &g_id_create_iterator, "§4.5's createNodeIterator");
    agent_state_id("document", &g_id_create_walker, "§4.5's createTreeWalker");
    agent_state_id("document", &g_id_create_range, "§4.5's createRange");
    agent_state_id("document", &g_id_create_event, "§4.5's createEvent");
    agent_state_id("document", &g_id_adopt_node, "§4.5's adoptNode");
    agent_state_id("document", &g_id_title_set, "§3.1.5's `title` setter");
    agent_state_id("document", &g_id_dir_set, "§3.1.5's `dir` setter");
    agent_state_id("document", &g_id_location_set, "§3.1.1's `location` [PutForwards=href] setter");
}

static void document_install_members(JSContext *ctx, JSValueConst proto)
{
    idl_install_method(ctx, proto, "createElement", 1, g_id_create_element);
    idl_install_method(ctx, proto, "createTextNode", 1, g_id_create_text);
    idl_install_method(ctx, proto, "createComment", 1, g_id_create_comment);
    idl_install_method(ctx, proto, "createCDATASection", 1, g_id_create_cdata);
    idl_install_method(ctx, proto, "createProcessingInstruction", 2, g_id_create_pi);
    idl_install_method(ctx, proto, "createDocumentFragment", 0, g_id_create_fragment);
    {
        /* §3.1.5's five element shortcuts, each a LIVE HTMLCollection over the document. */
        static const char *const NAMES[] = { "forms", "images", "scripts", "embeds", "links" };
        unsigned k;
        for (k = 0; k < sizeof(NAMES) / sizeof(NAMES[0]); k++)
            idl_install_accessor(ctx, proto, NAMES[k], js_doc_shortcut, (int)k, -1);
    }
    idl_install_method(ctx, proto, "createElementNS", 2, g_id_create_element_ns);
    /* §4.5's adoptNode. `importNode` is NOT beside it and is honestly ABSENT: it is stated over "clone a node"
       with `document` set to the receiver and a `fallbackRegistry`, and node.c's clone machine
       (node_clone_start) takes neither — so a page's own TypeError names the gap rather than a member that
       clones into the wrong document. */
    idl_install_method(ctx, proto, "adoptNode", 1, g_id_adopt_node);
    /* §4.5's two ATTRIBUTE factories, declared beside the interface they build (attr.c) — "create an attribute"
       is §4.9.2's algorithm and belongs to the attribute component, not to a second copy of it here. */
    attr_install_document_members(ctx, proto);
    idl_install_method(ctx, proto, "createNodeIterator", 1, g_id_create_iterator);
    idl_install_method(ctx, proto, "createTreeWalker", 1, g_id_create_walker);
    idl_install_method(ctx, proto, "createRange", 0, g_id_create_range);
    idl_install_method(ctx, proto, "createEvent", 1, g_id_create_event);
    /* §3.1.1 The Document object's `[PutForwards=href, LegacyUnforgeable] readonly attribute Location?
       location`, with the FORWARDING half — Web IDL §3.7.6's, the same declaration §7.2.2's `location` carries,
       so `document.location = url` and `window.location = url` are the one algorithm ending in the `href`
       setter's navigation.
       WHAT THIS INSTALL IS STILL NARROWER THAN is the OTHER extended attribute on that line. §3.4.10
       [LegacyUnforgeable] puts an attribute on EVERY OBJECT THAT IMPLEMENTS THE INTERFACE, non-configurable,
       and this is on the interface prototype object and configurable — which is what every other member of
       this prototype is and what none of them should be if the IDL says otherwise. The next diff is the
       per-instance unforgeable install for a Document, which is what idl_install_accessor_unforgeable already
       does for §7.2.2's `location` on the global; it needs a hook at document creation, since a Document is
       built per navigation rather than once per realm. ITS ABSENCE SHOWS as
       `Object.getOwnPropertyDescriptor(document, "location")` answering undefined where a browser answers a
       non-configurable accessor, and as `delete Document.prototype.location` succeeding. */
    idl_install_accessor(ctx, proto, "location", js_doc_location, 0, g_id_location_set);
    /* §3.1.1's TREE ENTRY POINTS and §4.5's per-document strings. They were data properties latched onto ONE
       document object at install — wrong in time (each is a lookup in the tree as it IS) and wrong in subject
       (a value stored on one object cannot answer for a second document). */
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
    /* §3.1.5's `title`, §3.2.6.4's `dir` and §7.2.2's `defaultView` — the first two READ-WRITE, which is what
       makes them accessors rather than a value: `title` was a string latched at install with no setter at all,
       so `document.title = "x"` stored a property where the standard creates a `<title>` element. */
    idl_install_accessor(ctx, proto, "title",       js_doc_html_members, DOC_TITLE, g_id_title_set);
    idl_install_accessor(ctx, proto, "dir",         js_doc_html_members, DOC_DIR,   g_id_dir_set);
    idl_install_accessor(ctx, proto, "defaultView", js_doc_html_members, DOC_DEFAULT_VIEW, -1);
    /* §3.1.4's resource metadata members and §3.1.5's `readyState` — their own component, because `cookie` and
       `referrer` are ATTACKER SOURCES with their own browser delivery and a cookie store behind them. */
    document_metadata_install(ctx, proto);
    /* §7.1.1.2's `domain`, the fifth member of that partial interface — see document_domain.h for why the
       origin-mutating one is not in the component that reads the resource's metadata. */
    document_domain_install(ctx, proto);
    /* §8.4 Dynamic markup insertion's `write`, `writeln` and `close`. A THIRD problem again: every member
       above answers a question about this tree, and these three drive the parser that builds it — and
       `document.write` is a top-three DOM-XSS sink whose absence meant no @S search was ever opened for it. */
    document_write_install(ctx, proto);
    /* CSSOM §6.2.3's `styleSheets`, one of the two members its `partial interface mixin DocumentOrShadowRoot`
       adds. ShadowRoot gets the same call from its own component, because a `<style>` in a shadow tree is in
       THAT tree's collection and not in this one. `adoptedStyleSheets`, the mixin's other member, is an
       `ObservableArray<CSSStyleSheet>` of CONSTRUCTED sheets and is absent with the constructor. */
    style_sheet_list_install_mixin(ctx, proto);
}

/* HTML §7.2.6's container for THIS document, from BOTH halves of the policy list.
   `csp` IS WHAT THE DOCUMENT WAS CREATED WITH — the response's `Content-Security-Policy` header, or §7.4's
   clone of the creator's for a document that came from no response — and it used to arrive nowhere: the
   trusted zone captured the header, handed it to the engine, and the engine's entry point cast it to `(void)`.
   So every document was judged against its `<meta>` policies alone, and a sink that the page's real policy
   kills was reported as a working exploit — the exact false PoC §@S exists to never emit.
   The meta half is a REAL LEXBOR WALK, not a regex over the source: a `content` attribute is parsed markup by
   the time it is here, so entity decoding and quoting are the parser's answer rather than a second one — the
   same reason the bundle id is a `<script>` scan. */
PolicyContainer *document_policy_new(lxb_html_document_t *dom, const char *csp, const Origin *self_origin,
                                     SerializedEmbedderPolicy embedder)
{
    lxb_dom_node_t *cur;
    char *acc = NULL;
    size_t acc_len = 0;

    /* THE CREATED-WITH POLICIES COME FIRST, because they were delivered first; every policy in a list is
       enforced so the order changes no verdict, but a container that reports its own text should report it in
       the order the document received it. */
    if (csp && *csp) {
        acc_len = strlen(csp);
        acc = malloc(acc_len + 1);
        CHECK(acc != NULL, "document: OOM holding the policy this document was created with");
        memcpy(acc, csp, acc_len + 1);
    }

    /* No guard for a missing tree: document_install has already asserted there is one, and a second, softer
       answer here would be the defensive branch that hides the case the assert exists to catch. */
    for (cur = lxb_dom_interface_node(dom)->first_child; cur; ) {
        if (cur->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t qn = 0;
            const lxb_char_t *q = lxb_dom_element_qualified_name((lxb_dom_element_t *)cur, &qn);
            if (q && qn == 4 && !memcmp(q, "meta", 4)) {
                size_t hl = 0, cl = 0;
                const lxb_char_t *he = lxb_dom_element_get_attribute((lxb_dom_element_t *)cur,
                                                                     (const lxb_char_t *)"http-equiv", 10, &hl);
                /* The equivalence is ASCII case-insensitive, which is how every real page spells it. */
                if (he && hl == 23 && !strncasecmp((const char *)he, "content-security-policy", 23)) {
                    const lxb_char_t *cv = lxb_dom_element_get_attribute((lxb_dom_element_t *)cur,
                                                                         (const lxb_char_t *)"content", 7, &cl);
                    if (cv && cl) {
                        /* CSP §3.3 MAKES `sandbox` MEANINGLESS IN A `<meta>`, and this container carries no
                           per-policy SOURCE that could tell a meta policy from a header one — so a meta
                           `sandbox` merged here would be read by §7.1.5's CSP-derived sandboxing flags as if
                           the server had sent it, and would sandbox a document the server did not sandbox.
                           Asked with the same function that would go on to misread it, so the question and
                           the answer cannot drift: give a Policy its CSP §2.2 SOURCE (`header`/`meta`) and
                           have the derivation skip the meta ones, then this assert is the line to delete. */
                        DCHECK(policy_csp_derived_sandboxing_flags((const char *)cv, cl) == 0,
                               "a `<meta http-equiv=Content-Security-Policy>` declares a `sandbox` directive, "
                               "which CSP §3.3 IGNORES for a meta-delivered policy — this container has no "
                               "per-policy SOURCE, so merging it would let §7.1.5's CSP-derived sandboxing "
                               "flags read it as a header policy and sandbox a Document the response did not");
                        /* SEVERAL META POLICIES ALL APPLY, and they are joined with a COMMA because that is
                           CSP §2.2's serialization of a policy LIST. A ';' join would have made them one
                           policy, where a repeated directive is ignored and `script-src` overrides
                           `default-src` — so the narrowing second policy would have silently vanished. */
                        size_t add = cl + (acc_len ? 1 : 0);
                        char *g = realloc(acc, acc_len + add + 1);
                        CHECK(g != NULL, "document: OOM collecting a policy");
                        acc = g;
                        if (acc_len) acc[acc_len++] = ',';
                        memcpy(acc + acc_len, cv, cl);
                        acc_len += cl;
                        acc[acc_len] = 0;
                    }
                }
            }
        }
        if (cur->first_child) { cur = cur->first_child; continue; }
        while (cur && !cur->next) cur = cur->parent;
        if (cur) cur = cur->next;
    }
    {
        /* THE MERGED LIST TAKES ONE SELF-ORIGIN, and it is the created-with policy's rather than a second one
           derived for the `<meta>` half. CSP §3.3 delivers a meta policy INSIDE the response this document
           came from, so §2.2.2's "response's URL's origin" is the same origin for both halves; the two are one
           list precisely because they belong to one response.
           AND §7.1.4'S ITEM PASSES THROUGH UNTOUCHED, which is a statement about CSP §3.3 rather than a
           shortcut: a `<meta http-equiv>` can deliver a CSP policy and nothing else, so there is no second
           half of the embedder policy for this walk to find and no merge for it to perform. */
        PolicyContainer *p = policy_container_new(acc, self_origin, NULL, embedder);
        free(acc);
        return p;
    }
}

const PolicyContainer *document_policy(JSContext *ctx) { return doc_here(ctx)->policy; }

/* THE SAME CONTAINER AS A FACT ABOUT ONE DOCUMENT — CSP §6.3.1.1 step 1 is stated that way ("let CSP list be
   DOCUMENT's global object's csp list"), and §4.2.3's freeze runs for an element whose node document is
   routinely not the running realm's active one (a DOMParser tree, a second Document of this agent). NULL for a
   document with no browsing context, which is the same answer as a list of zero policies. */
const PolicyContainer *document_policy_of(const lxb_dom_document_t *dom)
{
    const Document *d = doc_rec(dom);

    DCHECK(d != NULL, "a document's policy container was asked of a tree with no record");
    return d->policy;
}

SandboxFlags document_active_sandbox_flags(JSContext *ctx) { return doc_here(ctx)->sandbox_flags; }

const PermissionsPolicy *document_permissions_policy(JSContext *ctx) { return doc_here(ctx)->permissions_policy; }

/* HTML §3.1.3 "Ancestor origins" — the INTERNAL ANCESTOR ORIGIN OBJECTS LIST CREATION STEPS, "given a
 * `Document` object document and a referrer policy referrerPolicy". §7.3.2.1 "Creating browsing contexts" is
 * the one caller and it runs them together with the ANCESTOR ORIGINS LIST CREATION STEPS beside them ("set
 * document's internal ancestor origin objects list to the result of running the internal ancestor origin
 * objects list creation steps given document and iframeReferrerPolicy" then "set document's ancestor origins
 * list to the result of running the ancestor origins list creation steps given document").
 *
 * IT IS CALLED AT THE CREATION AND NOWHERE ELSE, which is the whole content of the member: §7.2.4's
 * `ancestorOrigins` is a SNAPSHOT of the tree this Document was created in. Reading it by walking parents at
 * the getter would answer with the tree as it is NOW, so a page that removed and re-inserted a frame would be
 * told about ancestors this document never had.
 *
 * THE THREE INPUTS ARE PASSED RATHER THAN DERIVED FROM THE CHILD'S NAVIGABLE, and that is what lets the ONE
 * implementation serve both sides of an instance boundary. §7.3.1.3's create-a-new-child-navigable holds all
 * three in the CREATOR's heap — the parent navigable is "element's node navigable", the container is the
 * element, the child's origin is what that create just computed — so for a child whose Document will be built
 * in a PEER INSTANCE the creator runs these steps and the RESULT crosses, exactly as Permissions Policy §9.5's
 * result does on the same record. Deriving the inputs from the child's own proxy instead would tie the
 * composition to a navigable whose Document is in another heap, which is the one arrangement it cannot have.
 *
 * `masked` IS THE REFERRER POLICY'S DOING and the two arms are step 8's: "no-referrer" masks unconditionally,
 * and "same-origin" masks only when the parent is cross-origin with this document. Masking replaces an origin
 * with A NEW OPAQUE ORIGIN rather than dropping it, so the list's LENGTH still reports the depth of the frame
 * tree — which is why the corpus asserts `['null']` and not `[]`.
 *
 * THE POLICY IS READ OFF THE CONTAINER ELEMENT AT THIS MOMENT, AND FOR A NAVIGATION THAT IS THE WRONG MOMENT.
 * This paragraph used to claim the read WAS the standard's snapshot and cite "§7.4.2's beginning-navigation
 * step 16" for it. All three parts were wrong: the section is §7.4.2.2 "Beginning navigation", its step 16 is
 * the is-delaying-load-events one, and the snapshot is its STEP 17 — "let targetSnapshotParams be the result of
 * snapshotting target snapshot params given navigable". §7.4.2.1 "Supporting concepts" defines that struct as
 * exactly two items, "sandboxing flags" and "IFRAME ELEMENT REFERRER POLICY", and says it "is snapshotted at
 * the beginning of a navigation and used throughout the navigation's lifetime"; the second item is "the result
 * of determining the iframe element referrer policy given targetNavigable's container", which is §7.1.6 — the
 * function core/html/html_iframe.c now owns.
 *
 * SO THE VALUE IS CAPTURED WHEN THE NAVIGATION STARTS AND CONSUMED WHEN THE DOCUMENT IS CREATED, AND THOSE ARE
 * DIFFERENT INSTANTS. Reading the element here is correct for the INITIAL about:blank, whose creation is not
 * preceded by a navigation at all — there is no earlier moment for it to have been snapshotted at. It is wrong
 * for every Document a navigation produces: a page that writes `referrerpolicy` after starting the navigation
 * and before the load completes is answered with the NEW value, where the standard answers with the one that
 * was live when the navigation began. That is observable and the corpus observes it — an ancestor origin comes
 * back unmasked because the attribute was cleared while the response was still in flight.
 *
 * WHAT CLOSES IT IS THE STRUCT, AND THE STRUCT DOES NOT EXIST — an earlier version of this paragraph said this
 * engine "already carries target snapshot params for the sandboxing flags" and that the referrer policy was
 * simply "the struct's OTHER field". That was false about this tree and it was written without grepping for the
 * type: every occurrence of `targetSnapshotParams` here is PROSE inside a comment, naming the spec's struct to
 * describe a computation done without it. A reader who trusted it would have gone looking for a record to add a
 * field to and found nothing, which is the stale-DFAIL failure mode with the coordinates pointing at a struct
 * rather than at a file.
 *
 * WHAT IS ACTUALLY THERE is a different mechanism for the OTHER half. §7.4.5's final sandboxing flag set is
 * built as `window_proxy_creation_sandbox_flags(proxy) | csp_flags`, so the sandboxing half is a LATCH ON THE
 * WINDOWPROXY, written when the navigable is created and read at §7.4.5. That is a snapshot — just not the
 * standard's struct, and not at the standard's instant, since §7.4.2.1 takes it at the start of the NAVIGATION
 * and the latch is taken at the creation of the NAVIGABLE. Whether those two instants are observably different
 * for the `sandbox` attribute is NOT established here and must not be assumed in either direction; the referrer
 * policy's are observably different, which is why only this half has a corpus failure behind it.
 *
 * SO THE WORK IS TO BUILD §7.4.2.1's TARGET SNAPSHOT PARAMS as a real record, snapshotted at §7.4.2.2 step 17,
 * carrying both of the two items that section lists — this one, whose value is §7.1.6's answer, and the
 * sandboxing flags, whose existing latch then moves onto it and gets taken at the right instant by
 * construction. Once the navigation carries the record, this function takes the policy as an argument the way
 * §7.3.2.1 already passes `iframeReferrerPolicy`, and stops touching the container element for a navigated
 * Document. Reading the attribute earlier and stashing it on the element would be the same defect moved: the
 * operation must carry its inputs, not re-derive them from the object it acts on (CLAUDE.md, §scheduler).
 *
 * THE POLICY IS §7.1.6's ANSWER AND NOT THE RAW ATTRIBUTE, which is what §7.3.2.1 "Creating browsing contexts"
 * passes as `iframeReferrerPolicy` and is a different function from reading the element. HTML §7.1.6 "iframe
 * element referrer policy" is "if embedder is an `iframe` element, then return embedder's `referrerpolicy`
 * attribute's STATE's corresponding keyword; return the empty string" — a STATE, so §2.5.5's enumerated
 * attribute resolved by §2.3.3's ASCII case-insensitive match with an invalid value default of the empty string
 * state, and the empty string for a container that is not an `iframe` at all. This site used to compare the raw
 * bytes with `strcmp`, which failed to mask for `referrerpolicy="No-Referrer"` and would have honoured the
 * attribute on an `<object>`; core/html/html_iframe.c owns §7.1.6 now and this asks it, so the two questions
 * ("which state" and "does it mask") are answered in the two places the standard answers them.
 *
 * THE MASKED SAME-ORIGIN COMPARISON ASKS THE PARENT'S ORIGIN RECORD, NOT ITS TEXT. Step 12.1's condition is
 * "ancestorOrigin is same origin with parentDoc's origin", and §7.1.1's same origin is FALSE for two opaque
 * origins — while both serialize to the same three bytes `null`. Comparing the stored text alone would
 * therefore mask an entry that is not the parent's whenever the parent is itself opaque, so the text
 * comparison is admitted only once the parent's origin is known to be a tuple. */
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
    const char *rp;   /* §7.1.6's keyword — borrowed from its own table, so there is nothing to free */
    JSValue len;

    CHECK(!JS_IsException(out), "§3.1.3's ancestor origins list could not be allocated");
    DCHECK(window_proxy_is(parent_nav) && !window_proxy_is_remote(parent_nav),
           "§3.1.3's internal ancestor origin objects list creation steps were run against a parent navigable "
           "this instance does not hold — step 5 reads the parent Document's own list and steps 9, 11 and 12.1 read "
           "its ORIGIN RECORD, and neither exists in this heap for a peer's navigable. The composition belongs in "
           "the instance that holds the ancestors and its RESULT crosses as text");
    /* STEP 5: "let ancestorOrigins be parentDoc's internal ancestor origin objects list." */
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
    /* STEPS 6-9: the container element, `masked`, and the referrer policy's two masking arms. The keyword §7.1.6 returns is already
       canonical, so the two comparisons below are against the standard's own spellings and are exact by
       construction — the case-insensitivity lives once, inside §2.3.3's match, and not at every consumer. */
    rp = iframe_element_referrer_policy(container);
    DCHECK(rp != NULL, "§7.1.6 answered with no keyword — its two arms both return one (a state's corresponding "
                       "keyword, or the empty string), so a NULL is an arm that returned nothing at all and "
                       "steps 8-9 would read it as 'no policy' rather than crashing");
    if (!strcmp(rp, "no-referrer")) {
        masked = true;                                                              /* STEP 8 */
    } else if (!strcmp(rp, "same-origin")) {                                        /* STEP 9 */
        DCHECK(child_origin != NULL,
               "§3.1.3's step 9 compares this Document's origin against its parent's and the navigable being "
               "created has none — every navigable is created with the origin its Document will have, so an "
               "absent one is a create that had not computed it yet rather than a Document without one");
        masked = !origin_same(porigin, child_origin);
    }
    /* STEPS 10-11: "if masked is true, then append a new opaque origin to output"; "otherwise, append
       parentDoc's origin to output." Two numbered steps in the standard and one statement here, because
       exactly one of them runs. */
    JS_SetPropertyUint32(ctx, out, nout++,
                         JS_NewString(ctx, masked ? "null" : (pser ? pser : "null")));
    /* STEP 12: the parent's own ancestors, each masked when it is the parent's origin again (its step 12.1). */
    len = JS_GetPropertyStr(ctx, pd->ancestor_origin_strings, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++) {
        JSValue a = JS_GetPropertyUint32(ctx, pd->ancestor_origin_strings, i);
        const char *s = JS_ToCString(ctx, a);

        if (masked && !popaque && s && pser && !strcmp(s, pser)) {
            JS_SetPropertyUint32(ctx, out, nout++, JS_NewString(ctx, "null"));
        } else {
            /* "Append ancestorOrigin to output AND SET MASKED TO FALSE." Masking is not a mode that runs to
               the end of the list: it stops at the first ancestor that is not the parent's own origin, and the
               standard attaches a note saying why that is not a leak — those further ancestors "might have
               been previously masked when an ancestor document ran these steps", and what this list holds is
               whatever the PARENT's list already recorded. Reading the loop without this line gives an answer
               that is wrong only for a frame three deep, which is exactly the shape nothing shallower tests. */
            JS_SetPropertyUint32(ctx, out, nout++, JS_DupValue(ctx, a));
            masked = false;
        }
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, a);
    }
    /* STEPS 10-11 APPENDED ONE BEFORE THE LOOP COULD APPEND NONE, so a list composed for a navigable that HAS a
       parent is never empty. That is the invariant the record crossing an instance boundary is checked
       against at both ends: the empty list means "no container document", which means "no parent". */
    DCHECK(nout >= 1, "§3.1.3's steps returned an EMPTY list for a navigable that has a parent — steps 10-11 "
                      "append unconditionally (exactly one of the two runs), so an empty output is these steps having skipped it and the "
                      "child would report itself top-level");
    return out;
}

/* §3.1.3's TWO LISTS FOR THE DOCUMENT BEING INSTALLED, from whichever of the three states its navigable is in.
 *
 * THE THIRD STATE IS THE ONE THAT MAKES THIS A DISPATCH RATHER THAN A CALL, and it is the same third state
 * §9.5's own creator has: a navigable whose PARENT lives in another WASM instance. §Security makes a
 * cross-instance read a SUSPEND POINT and a Document's install has no flow base under it to take one, so the
 * answer is one the creating instance already composed and put on the record that provisioned this instance.
 * It is read back here rather than recomputed, for the reason document_create_permissions_policy states one
 * function along: shipping the INPUTS would be a second site running one algorithm, and here it would be a
 * second site running it WRONG — step 12.1's same-origin comparison needs the parent's origin RECORD, which is
 * exactly the thing a serialization cannot carry. */
static void doc_create_ancestor_origins(JSContext *ctx, Document *d, JSValueConst nav_proxy)
{
    JSValue parent_nav, container, out;

    /* A DOCUMENT WITH NO BROWSING CONTEXT HAS NEITHER LIST, and that is a state §7.2.4 already answers: its
       first step finds no relevant Document and returns the LOCATION's own empty list. Leaving both fields
       UNDEFINED is what says so; an empty list here would be the positive claim that this Document sits at the
       top of a tree, which is a different fact from having no tree at all. */
    if (!window_proxy_is(nav_proxy)) return;
    /* STEPS 2-3: "let parentDoc be document's container document; if parentDoc is null, then return output." A
       top-level traversable has no container, and its list is empty for ever. */
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
    /* THE ANCESTOR ORIGINS LIST CREATION STEPS, whose whole content is "append the SERIALIZATION of origin to
       output" over the list above — which is what that list already holds, for the reason its field says. */
    d->ancestor_origins = dom_string_list_new(ctx, JS_DupValue(ctx, out));
}

/* §7.2.4's `ancestorOrigins` answers with THIS — the list built at this Document's creation, [SameObject]. */
JSValue document_ancestor_origins(JSContext *ctx)
{
    Document *d = doc_here(ctx);

    DCHECK(!JS_IsUndefined(d->ancestor_origins),
           "§7.2.4's ancestorOrigins was asked of a Document with no ANCESTOR ORIGINS LIST — §7.3.2.1 gives "
           "one to every Document it creates, so a Document without one reached its realm by some other route "
           "and its ancestry was never recorded");
    return JS_DupValue(ctx, d->ancestor_origins);
}

/* §3.1.3's INTERNAL LIST IN THE FORM IT CROSSES AN INSTANCE BOUNDARY — see document.h for the grammar and for
 * why a SPACE is the separator rather than a character somebody liked the look of. */
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
    /* THE EMPTY LIST IS A WORD AND NOT AN EMPTY FIELD, which is the same rule §9.5's answer beside it follows
       and for the same reason: a host that STOPPED WRITING the field and a navigable with no ancestors are two
       different facts, and only one of them is a bug. An empty field would make them one, and the bug is the
       silent kind — a cross-origin frame reporting itself top-level. */
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
        /* THE SEPARATOR CANNOT OCCUR IN AN ENTRY AND THAT IS A THEOREM, NOT AN OBSERVATION OF TODAY'S DATA.
           §7.1.1's serialization of an origin is either the three bytes `null` or scheme, "://", host and an
           optional ":" and port; a scheme is ASCII alphanumeric with "+", "-" and ".", a port is digits, and
           URL §3.2 "Host miscellaneous" makes U+0020 SPACE a FORBIDDEN HOST CODE POINT — so a host containing
           one fails to parse and never reaches an origin record. It is asserted anyway, because the value that
           would break this record is exactly the value nothing else would notice: an entry carrying a space
           would arrive at the peer as two ancestors. */
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
    /* A `CHECK` FOR THE REASON THE DESERIALIZER'S ARE, and it is the same judgement permissions_policy.c makes
       one field along: this is a field's presence read off a record that crossed an INSTANCE boundary, and
       SECURITY.md makes the instance on the other side of it UNTRUSTED. It is also the only guard between a
       host that stated nothing and a `strcmp` on NULL, which a DCHECK would leave standing in release. */
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

    /* EVERY REFUSAL HERE IS A `CHECK`, for permissions_policy_deserialize's reason: the bytes come from
       another instance, so a malformed record is check.h's security-boundary case rather than this engine's
       own logic being wrong, and a DCHECK would be compiled out of the build that faces it. */
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
    /* THE SAME INVARIANT THE COMPOSER ASSERTS, READ FROM THE OTHER END. A record that says there are ancestors
       and yields none is a grammar whose two ends have come apart, and the failure it would produce — a frame
       silently reporting itself top-level — is the one this whole field exists to prevent. */
    CHECK(nout >= 1,
          "§3.1.3's list arrived from a peer instance stating that there ARE ancestors and parsed to none — "
          "the two ends of this record disagree about its grammar, and the answer it would install is the "
          "one an absent field would have given");
    return out;
}

/* §3.1.3's LIST FOR A CHILD NAVIGABLE WHOSE DOCUMENT WILL BE BUILT IN A PEER INSTANCE — see document.h. */
char *document_ancestor_origins_for_child(JSContext *ctx, JSValueConst container, const Origin *child_origin)
{
    /* BORROWED, like every read of it — see document.h. §7.3.1.3's "let parentNavigable be element's node
       navigable" runs in the EMBEDDER's realm, which is this one, so this is that navigable. */
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

/* WHAT PERMISSIONS POLICY §9.7 READS OFF A NAVIGABLE CONTAINER, gathered for the navigable this Document is
 * being created in. §9.5 takes "null or an element (container)" and §9.7 reads three things off it: "container's
 * node document"'s permissions policy (its steps 2 and 3), that document's ORIGIN (its step 7), and the `allow`
 * attribute §9.4 "Process permissions policy attributes" turns into a container policy (its steps 4-5).
 *
 * THE CONTAINER IS HTML §7.3.1.3 "Child navigables"' CONTAINER OF A NAVIGABLE — "the navigable container whose
 * content navigable is navigable, or null if there is no such element" — asked of the navigable rather than
 * derived from the parent, because that definition is what §9.5's argument IS and because the two differ in
 * cases the standard has answers for. Everything §9.7 needs then comes from the ELEMENT's node document, which
 * is the document that holds the `<iframe>`, and never from a separate walk that could disagree with it.
 *
 * A NULL CONTAINER HAS THREE CAUSES AND ONLY TWO OF THEM ARE §9.7 STEP 1. A top-level traversable (and every
 * auxiliary browsing context `window.open` creates) has no container at all, and §7.3.1.6's
 * destroy-a-child-navigable severs the relation so §7.3.1.3 answers null for a detached frame — for both of
 * those "container is null, return `Enabled`" is literally the standard's answer. The third is a container that
 * exists in ANOTHER WASM INSTANCE, which is not a null container and must not be read as one: taking step 1 for
 * it would grant a cross-origin child every feature its embedder holds. It is told apart by the PARENT being a
 * remote WindowProxy, and it is answered from the record that provisioned this instance — see below. */
PermissionsPolicy *document_permissions_policy_for_container(JSValueConst container, const Origin *origin)
{
    const PermissionsPolicy *container_doc_policy = NULL;
    const Origin *container_doc_origin = NULL;
    const lxb_char_t *allow = NULL;
    size_t allow_len = 0;

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
        /* §9.4 step 2 reads "the value of element's allow attribute". An element carrying none yields NULL,
           which §9.4 turns into an empty policy directive — a positive statement that this container declared
           nothing, and not an input the walk failed to find. */
        allow = lxb_dom_element_get_attribute((lxb_dom_element_t *)el, (const lxb_char_t *)"allow", 5,
                                              &allow_len);
    }
    return permissions_policy_create(container_doc_policy, container_doc_origin, (const char *)allow,
                                     allow_len, origin);
}

/* §9.5 FOR THE NAVIGABLE A DOCUMENT IS BEING CREATED IN — the same algorithm above, asked of the navigable
 * rather than of an element, which is what routes its three container cases.
 *
 * THE CROSS-INSTANCE ARM IS ANSWERED FROM A CARRIED RESULT, NOT FROM CARRIED INPUTS, and the two are not
 * interchangeable. §9.5's arguments are the container ELEMENT and the origin of the Document being created,
 * and BOTH of them belong to the instance that created this navigable: the element is in its tree and the
 * child's origin is what its create computed. So the creator runs §9.5 — the function directly above, its one
 * implementation — and its ANSWER crosses on the `navigable.create` notice; this side reads it back.
 * The alternative, shipping §9.7's inputs and re-running it here, is a SECOND site evaluating one algorithm,
 * and it would have to ship the container document's whole permissions policy (its §9.8 steps read the
 * DECLARED allowlists too, at two different origins), its origin and the `allow` attribute's bytes.
 *
 * IT IS ASKED OF THE PARENT, AND THAT IS THE ASSERT. A navigable whose parent is a REMOTE WindowProxy is a
 * child navigable by §7.3.1.3's own definition, so it HAS a container; the record that provisioned this
 * instance either stated what that container answered or it did not, and the second is a host that owes a
 * call rather than a navigable with no container. */
static PermissionsPolicy *document_create_permissions_policy(JSContext *ctx, JSValueConst nav_proxy)
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
            return permissions_policy_deserialize(carried);
        }
    }
    policy = document_permissions_policy_for_container(container, window_proxy_origin(nav_proxy));
    JS_FreeValue(ctx, container);
    return policy;
}

/* HTML §4.8.5 "The `iframe` element"'s ALLOWED TO USE — see document.h for the steps and for why they are here
   rather than in the policy component. */
bool document_allowed_to_use(JSContext *ctx, PermissionsPolicyFeature feature)
{
    Document *d = doc_here(ctx);

    /* Step 1: "If document's browsing context is null, then return false." A Document with no browsing context
       is one §9.5 never ran for, which is exactly the absence of a policy on this record. */
    if (d->permissions_policy == NULL)
        return false;
    /* Step 2: "If document is not fully active, then return false." §7.3.1's walk, asked and not remembered —
       removing an ancestor `<iframe>` stops this document being fully active with nothing done to its own
       navigable. */
    if (!document_fully_active(ctx))
        return false;
    /* Step 3: §9.10 "on feature, document, and document's origin" — so §9.10's `origin` argument IS this
       document's own origin, and its `report` takes the default True. Step 4 is the other answer. */
    {
        const Origin *origin = window_proxy_origin(d->proxy);

        return permissions_policy_is_feature_enabled_in_document(d->permissions_policy,
                                                                 permissions_policy_empty(), feature,
                                                                 origin, origin, true) == PP_ENABLED;
    }
}

JSValueConst document_window_proxy(JSContext *ctx)
{
    Document *d = doc_here(ctx);
    DCHECK(!JS_IsUndefined(d->proxy), "this realm's WindowProxy was read before its Document was installed — "
                                      "§7.2.3 gives a navigable ONE, and it is minted with the realm");
    return d->proxy;
}

/* HTML §7.3.1 "FULLY ACTIVE": a Document is fully active when it is the active document of a navigable, and
 * that navigable is either a top-level traversable or its container document is itself fully active.
 *
 * WHY IT IS ITS OWN QUESTION AND NOT `!closed`. The two differ exactly where it matters: removing an
 * `<iframe>` destroys THAT navigable, and every document nested inside it stops being fully active without
 * anything having been done to its own navigable. So the answer is the WALK the definition states — this
 * navigable and every one containing it — and it is asked, not remembered, because the tree it walks is
 * per-flow: one arm of a fork removed the frame and its sibling did not.
 *
 * WHO ASKS. Every algorithm the standards guard with it, and there is a family of them: the Observable
 * standard opens §2.1's next/error/complete/addTeardown, §2.2.1's subscribe and §3's when() with this exact
 * sentence, and §2.1's close-a-subscription RE-asks it before every teardown because "each teardown could
 * result in the above Document becoming inactive". A detached document must silently do nothing rather than
 * push values into a realm the user agent has discarded. */
bool document_fully_active(JSContext *ctx)
{
    Document *d = doc_here(ctx);
    JSValue cur;
    bool ok = true;

    /* A Document with no navigable at all — `new Document()`, a DOMParser result — has no browsing context, so
       the guard's own premise ("the relevant global object is a Window") is false and the algorithm proceeds.
       The realm this runs in is a Window's, and this realm's proxy is minted with it. */
    if (!window_proxy_is(d->proxy))
        return true;
    cur = JS_DupValue(ctx, d->proxy);
    for (;;) {
        JSValue parent;
        if (window_proxy_closed(ctx, cur)) { ok = false; break; }
        parent = window_proxy_parent(ctx, cur);
        /* §7.3.1's base case is a TOP-LEVEL traversable, and §7.2.2.4's `parent` of one is the navigable ITSELF —
           so the walk ends when the answer stops moving, or when it is not a navigable's proxy at all (a
           cross-instance parent this agent cannot walk into, which is answered by its own instance). */
        if (!window_proxy_is(parent) ||
            JS_VALUE_GET_PTR(parent) == JS_VALUE_GET_PTR(cur)) {
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

JSValueConst document_object(JSContext *ctx) { return doc_here(ctx)->doc_obj; }

/* SELECTION API §4.1's getSelection — see document.h. The RECEIVER decides which document, because a realm can
   hold several and only the active one has a browsing context. */
JSValue document_selection(JSContext *ctx, JSValueConst doc)
{
    Document *d = doc_receiver(ctx, doc);

    if (!d) return JS_EXCEPTION;
    /* "…if this has an associated browsing context, and it must return null otherwise." The record states the
       browsing context as the presence of the Window it was installed with, and the selection is built by the
       same line — so the two can never disagree, which this asserts rather than testing one and trusting the
       other. */
    DCHECK(JS_IsUndefined(d->selection) == JS_IsUndefined(d->win_obj),
           "a Document has a browsing context and no selection, or a selection and no browsing context — "
           "Selection API §2 gives one to exactly the documents that have one, and document_install is the "
           "single line that builds both");
    if (JS_IsUndefined(d->selection)) return JS_NULL;
    return JS_DupValue(ctx, d->selection);
}

/* DOCUMENT.PROTOTYPE, and the Document as a real NODE. §4.4 `interface Document : Node`, and it was neither —
   a plain JS_NewObject with the members copied onto it. So `document.nodeType` was undefined,
   `document.appendChild` was not a function, `document.contains(el)` (which is how a page asks whether a node
   is still in the tree) was absent, and `document.body.parentNode.parentNode === document` compared a node
   wrapper against something that was not one. It is a node_wrap of the document node now, so it is in the ONE
   identity table with everything else and its members come from a prototype chained to Node.prototype rather
   than being installed per object.
   IT IS THE AGENT'S HALF, with every other prototype: a member is DECLARED once and a declaration builds one
   pool entry, so building this inside the per-document install declared the whole of Document a second time
   for a second realm — which is the shape the pool's seal exists to catch. Web IDL wants the PROTOTYPE per
   realm too; that is the gap this split makes visible, and it is the same one every DOM component has. */
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
    /* HOW THIS COMPONENT'S RECORD REACHES THE COLLECTOR. Declared with the AGENT and for the RUNTIME, beside the
       class it belongs to and before any record can exist — doc_rec_new asserts that order from the other side,
       at the birth of each record. It is the mirror of the realm-teardown hook navigable.c declares: that one
       says when a record dies, this one says what it holds while it lives, and a record with only the first is
       a record that never gets to use it. */
    JS_SetContextMarkHook(JS_GetRuntime(ctx), document_realm_mark);
    g_ready_slot = realm_value_declare(ctx, "HTML current document readiness");
    /* §7.5.9's page showing is the readiness's twin — one Document's state, written by the same two algorithms
       that move the readiness ("the end" sets it, unloading clears it) — so it is declared beside it. */
    g_showing_slot = realm_value_declare(ctx, "HTML §7.5.9 page showing");
    document_declare_members(ctx);
    /* §3.1.4's resource metadata members declare their own setter and their own per-realm cookie store, and
       document_metadata_install runs from document_install_proto below — so the declaration is paired with it
       HERE for the reason page_visibility_init's is, rather than copied into each host's own init list. */
    document_metadata_init(ctx);
    /* §7.1.1.2's `domain` is the fifth member of that same partial interface and a different PROBLEM — the one
       operation in the platform that mutates an origin — so it is its own component, declared here beside the
       other four for the reason document_metadata_init's line gives. */
    document_domain_init(ctx);
    /* §8.4's `write`, `writeln` and `close` — declared here beside the others for the reason
       document_metadata_init's line gives, and because `write`/`writeln` are VARIADIC DOMString members whose
       declaration is what converts every argument the page passed (and suspends on the page's own toString)
       before the algorithm sees one. */
    document_write_init(ctx);
    /* §3.1.7's `currentScript` is a sixth member of that partial interface and a THIRD problem: its getter is
       one line and everything about it is the §4.12.1.1 bracket that WRITES it, which is a per-flow slot rather
       than a fact about this tree. Declared here for the reason the two above are — its install runs from
       document_install_proto below, so a host cannot be the place that remembers it. */
    document_current_script_init(ctx);
    document_fragment_init(ctx);   /* §4.7, before any fragment is wrapped as a bare Node */
    shadow_root_init(ctx);         /* §4.8, whose prototype chains to §4.7's — declared after it */
    slot_init(ctx);                /* §4.2.2's slots, which only exist inside a §4.8 tree */
    document_type_init(ctx);       /* §4.6, before the parser's doctype is wrapped as a bare Node */
    dom_implementation_init(ctx);  /* §4.5.1, which every document's record builds one of */
    /* SELECTION API §3, whose §2 state every document's record builds one of, and whose §4.1 `getSelection` is
       installed from document_install_proto below — so its declaration is paired with it HERE for the reason
       page_visibility_init's and focus_init's are. AFTER element_init's row of core/platform.c's list, which
       is what declares §5.5's class this brands `addRange` against; selection_init asserts that from its own
       side rather than trusting the row order. */
    selection_init(ctx);
    /* §6.2's visibility state is a DOCUMENT's, and page_visibility_install already runs from
       document_install_proto below — so its declaration is paired with it HERE rather than copied into each
       host's own init list, which is the hand-picked list CLAUDE.md warns about: three hosts each declaring
       their own is three places for the next component to be missing from one of. */
    page_visibility_init(ctx);
    /* HTML §6.6's FOCUSED AREA is a Document's state too, and focus_install_document_members runs from
       document_install_proto below for the same reason page_visibility_install does — so its declaration is
       paired with it here rather than copied into each host's own init list. */
    focus_init(ctx);
    /* HTML §6.6.7's AUTOFOCUS CANDIDATES and its processed flag are a Document's state too, filled by the
       insertion steps and drained by §8.1.7.3's step 7 — declared here with the focused area they end up
       designating, and for the same reason. */
    autofocus_init(ctx);
    realm_declare_intrinsic(document_install_proto);
    /* §13.2.7 "THE END" IS AN AGENT FACT AND WAS BEING STATED PER DOCUMENT. This line was the last statement of
       document_install — the per-DOCUMENT half — so a page with one <iframe> claimed the ONE frontier's single
       document-lifecycle slot twice, and nothing ever gave it back. That is §per-realm-fact read backwards, the
       identical shape core/timing/timer.c's §8.1.7 timer step was in until its own line moved here: one agent
       fact answered from as many places as there are documents. There is one such slot per agent because
       document_lifecycle_step walks navigable_tree_order and answers for EVERY document of this agent in one
       call — it never needed a per-document claim to reach a per-document answer. Claimed once, here, and given
       back once, at document_agent_free; the setter asserts both halves now that it can. */
    engine_set_document_done_hook(document_lifecycle_step);
    agent_state_ptr("document", &g_document_rt,
                    "the runtime §4.5's interface and its ten sub-components declared into, and the latch");
    agent_state_class("document", &g_document_class, "§4.5's interface prototype slot and brand");
    agent_state_id("document", &g_ready_slot, "the per-realm slot §3.1.5's current document readiness lives in");
    agent_state_id("document", &g_showing_slot, "the per-realm slot §7.5.9's page showing lives in");
}

/* §3.1.1's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM. */
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
    /* §4.5: `Document includes ParentNode` — not ChildNode, because a document has no parent to be removed
       from — and `NonElementParentNode`, the same getElementById DocumentFragment includes. */
    node_install_parent_mixin(ctx, proto);
    node_install_nonelement_parent_mixin(ctx, proto);
    /* HTML §6.2's `visibilityState` and `hidden` — one source and the comparison the spec defines over it. */
    page_visibility_install(ctx, proto);
    /* HTML §6.6.6's `activeElement` (DocumentOrShadowRoot) and `hasFocus()`, and this realm's INITIAL FOCUSED
       AREA — the viewport, built with the realm so it is baseline rather than whichever flow read first. */
    focus_install_document_members(ctx, proto);
    /* SELECTION API §4.1's `Selection? getSelection()`. Installed by the component that owns the algorithm and
       the state, exactly as §6.6's two members above are. */
    selection_install_document_members(ctx, proto);
    /* HTML §3.1.7's `currentScript`, and this realm's BASELINE record holding §3.1.7's initial null — built
       with the realm for the reason the readiness record below is, and installed by the component that owns
       §4.12.1.1's bracket rather than reflected into a member beside it. */
    document_current_script_install(ctx, proto);
    /* HTML §6.6.7's per-document autofocus candidates and processed flag, built with the realm so an element
       inserted by the FIRST flow to run does not find a list that flow created. */
    autofocus_install_document(ctx);
    JS_SetClassProto(ctx, g_document_class, proto);
    /* THIS REALM'S DOCUMENT READINESS, built with the realm so it belongs to the pre-boot BASELINE — the same
       reason §8.12 Animation frames's map and §7.4.6.3's flag are built here. It exists before this realm has a Document at all,
       which is right: "loading" is what a Document that has not been installed yet would answer anyway, and
       the lifecycle walk only ever reaches a realm through its materialized navigable. */
    {
        JSValue rec = JS_NewObjectProto(ctx, JS_NULL);
        CHECK(!JS_IsException(rec), "this realm's document-readiness record could not be allocated");
        JS_SetPropertyStr(ctx, rec, "stage", JS_NewInt32(ctx, 0));
        realm_value_set(ctx, g_ready_slot, rec);
    }
    /* §7.5.9: page showing is "initially false", and it is built HERE for the reason the readiness is — a
       record made on first touch would be made inside whichever flow happened to read first, and would then be
       that flow's document rather than the baseline every flow forks from. */
    {
        JSValue rec = JS_NewObjectProto(ctx, JS_NULL);
        CHECK(!JS_IsException(rec), "this realm's §7.5.9 page-showing record could not be allocated");
        JS_SetPropertyStr(ctx, rec, "showing", JS_NewInt32(ctx, 0));
        realm_value_set(ctx, g_showing_slot, rec);
    }
}

/* DOM §4.5's TWO CREATION FACTS, BUILT — see document.h for why they are one value and why neither derives
   the other. The assertions are here rather than at the copy into the record because THIS is where a caller's
   answer arrives: a type that is empty or does not fit is a caller that has not run an algorithm producing
   one, and letting it through would put bytes in the record that no standard emitted and that every later
   read reports as this document's own. */
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

/* HTML §7.3.2.1 "Creating browsing contexts": "Let document be a new Document, with: type `html`, content
   type `text/html`, …" — the Document created with no response at all. */
DocumentKind document_kind_initial_about_blank(void)
{
    return document_kind(/*is_xml*/false, "text/html");
}

/* A DOCUMENT'S RECORD, AND THE ONE PLACE THE (document -> record) ANSWER IS ESTABLISHED.
 *
 * It lives on the Lexbor document's own `user` slot, which lexbor keeps for its embedder and never reads. That
 * slot used to hold the REALM pointer, which was already the (document -> realm) answer §4.2.3 needs; holding
 * the record instead answers that question and every other per-document one through one indirection, with no
 * registry to keep in step — a registry is a second list of documents whose failure mode is a stale row. */
static Document *doc_rec_new(JSContext *ctx, lxb_html_document_t *dom, const char *url, DocumentKind kind)
{
    lxb_dom_document_t *dd = lxb_dom_interface_document(dom);
    Document *d;
    const char *type = kind.content_type;

    DCHECK(dom != NULL, "a document record was built for no document");
    /* §4.5 GIVES EVERY DOCUMENT A CONTENT TYPE, so this is a required argument and not a defaulted one — the
       address may legitimately be absent (a document with no browsing context has none to speak of), the type
       may not, and §4.4's clone reads it back off the record it is copying. */
    DCHECK(type != NULL && type[0] != '\0',
           "a document record was built with no content type — §4.5's `contentType` is state every document "
           "has, and a record that never received one would answer for its document with an empty string that "
           "no parse, no createDocument and no clone could have produced");
    DCHECK(dd->user == NULL, "a second record was built for one Lexbor document — the first would be leaked and "
                             "every node of the tree would answer through whichever won");
    /* THE ORIGIN OF THE UNCOLLECTABLE REALM, ASKED AT THE BIRTH OF THE RECORD THAT CAUSES IT. Four counted
       references are about to hang off malloc'd C, and two of them point back INTO this realm — so from this
       line on, a collector that has not been told how to read this record can never collect the realm, and
       nothing says so until JS_FreeRuntime's gc_obj_list walk reports a whole page with no owner named. That is
       the worst possible place to learn it, so it is asked here, where it is still a fact about ONE record.
       It is asked of every record and not only of a realm's active one, because a second Document (§4.5.1's
       three factories, DOMParser, XHR's responseXML) holds the same references through the same list. */
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
    /* Selection API §2 gives a selection to a document WITH A BROWSING CONTEXT, so it is document_install that
       builds one and every other document keeps this — which §4.1's getSelection reads as its "must return
       null otherwise" rather than as a hole a first read would fill. */
    d->selection = JS_UNDEFINED;
    /* §3.1.1's ancestor-origins pair. UNDEFINED is the state of a Document with NO browsing context — a
       §4.5 createDocument, a DOMParser's — which is exactly the case §7.2.4's first step answers with the
       Location's own empty list, so an absent list here is never read as an empty ancestry. */
    d->ancestor_origins = JS_UNDEFINED;
    d->ancestor_origin_strings = JS_UNDEFINED;
    /* THE ADDRESS'S VALUE IS UNDEFINED BEFORE IT IS ANYTHING ELSE. A zeroed JSValue is the INTEGER 0 (JS_TAG_INT
       is 0), so a field doc_rec_refs walks before the line below has written it would hand the collector — and
       document_url_value's own assert — a real value nothing produced. */
    d->addr.value = JS_UNDEFINED;
    /* THE RECORD'S FIRST ADDRESS. A NULL one is a Document install that has no address to give — the empty
       string is what this engine's readers test for, so it is stored as one rather than left NULL.
       AND ITS VALUE IS A PLAIN STRING, WHICH IS A POSITIVE STATEMENT AND NOT AN INITIAL VALUE WAITING TO BE
       OVERWRITTEN: a Document is created FROM a response, so the address it is created with is a concrete fact
       — bytes a server answered at — and `concolic_is` on it is false for exactly that reason. A Document
       whose address the run COMPUTED acquires it through document_set_url, which is §7.4.4 step 8. */
    d->addr.bytes = doc_addr_intern(d, url ? url : "");
    d->addr.value = JS_NewString(ctx, url ? url : "");
    CHECK(!JS_IsException(d->addr.value), "document: OOM naming a document's address");
    doc_addr_assert_agrees(ctx, d);
    /* BOTH OF §4.5's CREATION FACTS, FROM THE ONE VALUE THAT CARRIES THEM. `document_kind` has already
       asserted the content type is non-empty and fits, so this copy cannot be the one that truncates. */
    snprintf(d->content_type, sizeof d->content_type, "%s", type);
    d->is_xml = kind.is_xml;
    /* §3.1.1's encoding. Every document this engine parses is decoded as UTF-8 — there is one source of bytes
       and one decode of them — so this is the real answer rather than an initial value waiting to be
       overwritten, and it is asked of the REGISTRY rather than written as a table index so that the set of
       encodings has exactly one authority. */
    d->encoding = encoding_utf8();
    dd->user = d;
    return d;
}

/* THE RECORD'S HELD REFERENCES GO BACK, and the tree goes with them when this record OWNS it. Every node of a
   document the page created has a wrapper the identity map holds a reference to, so a document freed without
   handing those back leaves the map naming freed memory and the runtime's own leak walk counting its whole
   tree. */
static void doc_rec_release(Document *d)
{
    JSContext *ctx = d->realm;

    /* BEFORE the references go, and it is not one of them: §4.5.1's object holds a raw pointer to the Lexbor
       document, and a DOMImplementation the page kept outlives this record. */
    dom_implementation_detach(ctx, d->impl);
    doc_rec_refs(d, doc_ref_release, NULL);   /* the ONE list — see doc_rec_refs */
    doc_addrs_free(d);   /* every address this record ever held — see doc_addr_intern */
    policy_container_free(d->policy);   /* malloc'd, so the GC walk would never have named it */
    /* Permissions Policy §9.5's policy, malloc'd for the same reason and released beside the container it sits
       beside on the record. NULL for a Document with no browsing context, which never had one. */
    permissions_policy_free(d->permissions_policy);
    if (d->dom)
        lxb_dom_interface_document(d->dom)->user = NULL;
    free(d);
}

/* THE TREE IS ABOUT TO GO, SO THE RECORD THAT NAMES IT GOES FIRST — called from the ONE place a document's
   lifetime ends. A delta-owned document is destroyed by the flow that made it, and its record is reachable from
   nothing else, so without this the struct and the two references it holds (the wrapper and the
   DOMImplementation) survive every exploration arm that ever created a document. */
void document_record_release(lxb_html_document_t *dom)
{
    Document *d = doc_rec(lxb_dom_interface_document(dom));

    if (!d) return;   /* already released — the realm's own path clears the record before it destroys the tree */
    DCHECK(!d->owned, "a document the REALM owns was destroyed through the delta's path — two owners for one "
                      "tree is one free too many");
    doc_rec_release(d);
}

/* The realm's own path: clear the record FIRST, so the destroy below finds nothing to release and the two
   owners cannot both run. */
static void doc_rec_free(JSContext *ctx, Document *d)
{
    lxb_html_document_t *dom = d->owned ? d->dom : NULL;

    (void)ctx;
    doc_rec_release(d);
    if (dom)
        dom_cow_destroy_document(dom);
}

/* THE REALM OWNS THIS DOCUMENT — one of the two arms every Document created in this realm takes, and the one a
   creation takes when it is BASELINE: it goes on the chain the realm's teardown walks and outlives every flow.
   The other arm is the running flow's COW delta (dom_cow_note_created_document), which destroys the document
   when the delta is discarded. Exactly one of the two, which is what `owned` records. */
static void doc_realm_owns(JSContext *ctx, Document *d)
{
    Document *active = doc_of(ctx);

    DCHECK(active != NULL, "a document was created in a realm that has none — the chain that owns a baseline "
                           "creation hangs off the realm's ACTIVE document, and there is none to hang it on");
    d->owned = 1;
    d->next_created = active->next_created;
    active->next_created = d;
}

/* THE REST OF WHAT EVERY DOCUMENT IN THIS REALM HAS, whoever ends up owning its tree: the wrapper the identity
   map holds, and §4.5's `[SameObject] implementation`. Returns the wrapper, OWNED. */
static JSValue doc_finish(JSContext *ctx, Document *d)
{
    JSValue doc = node_wrap(ctx, lxb_dom_interface_node(d->dom));

    CHECK(JS_IsObject(doc), "a created Document's wrapper allocation failed");
    d->doc_obj = JS_DupValue(ctx, doc);
    d->impl = dom_implementation_new(ctx, doc);
    return doc;
}

/* A SECOND DOCUMENT IN THIS REALM — see document.h. */
JSValue document_new(JSContext *ctx, lxb_html_document_t *dom, const char *url, DocumentKind kind)
{
    Document *d = doc_rec_new(ctx, dom, url, kind);

    /* WHO DESTROYS IT. A document a FLOW created is that flow's, exactly like a node it created: the COW delta
       owns it and destroys it when the delta is discarded, so the frontier does not accumulate one document per
       exploration arm. A creation made while capture is OFF is BASELINE — the boot flow's creations are the
       baseline by definition — and the realm that made it is what outlives it. */
    if (!dom_cow_note_created_document(dom)) doc_realm_owns(ctx, d);
    {
        /* HTML §4.2.3 FOR THE TREE THIS DOCUMENT WAS HANDED, for the reason document_install runs the same
           walk: a `DOMParser.parseFromString` and an XHR `responseXML` are PARSED markup, so their
           `<base href>` is already in the tree and no chokepoint ever saw it — and DOM §4.4's `baseURI` on
           their nodes is that document's document base URL, not its address. AFTER the record's wrapper
           exists (doc_finish returns it), because the freeze captures into the flow's delta against the
           `document` object; the walk is therefore between the two rather than folded into either. */
        JSValue w = doc_finish(ctx, d);

        html_base_element_parsed(ctx, lxb_dom_interface_node(dom));
        return w;
    }
}

/* HTML §4.12.3's APPROPRIATE TEMPLATE CONTENTS OWNER DOCUMENT — the Document that owns the template contents of
 * every `<template>` in `doc`, and the reason those contents are INERT: the owner has no browsing context, so
 * its scripts do not run and its custom elements do not upgrade. DOM §4.5 adopt step 3.4 reaches it through
 * HTML's adopting steps for a template element, which adopt the template's contents into the owner of the
 * document the template was just adopted INTO.
 *
 * THE ALGORITHM IS TWO FACTS ABOUT A DOCUMENT and the record holds both. "If document is not a Document created
 * by this algorithm" is the flag: an inert document IS its own template contents owner, which is what makes the
 * standard's "template elements inside Document objects that are created by this algorithm just reuse the same
 * Document owner" true rather than a second inert document per level.
 *
 * WHO OWNS THE INERT DOCUMENT: THE REALM, never the running flow's delta — the other of the two owners
 * document_new chooses between, and the choice is not free here. The inert document is named by a Document
 * RECORD, and a record is malloc'd C the COW delta has no entry kind for: a delta-owned inert document would be
 * destroyed with the flow that first touched a template while the record every sibling flow reads went on
 * naming it, which is a use-after-free with nothing to say so. The other direction costs nothing observable —
 * a template's contents are a DocumentFragment, and a fragment is not a CHILD of the document that owns it, so
 * the inert document's tree stays EMPTY and no flow can walk into a sibling's markup through it. Which is what
 * the standard describes: "a single Document to act as its proxy for owning the template contents of all its
 * template elements".
 */
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
           Document an HTML document is §4.5's TYPE — the same fact §4.5.1's createCDATASection refuses on —
           so the owner takes `document`'s type and NOT a compare against its content type, which HTML §7.5.4's
           text documents answer backwards. A document that is not an HTML document gets what "creating a
           document that implements Document" produces: §4.5's defaults, `application/xml` at `about:blank`
           with no browsing context. The CONTENT TYPE is the standard's default either way and never the
           creator's — §4.12.3 marks the owner an HTML document, it does not copy the response's type. */
        inert = doc_rec_new(d->realm, dom, "about:blank",
                            document_kind(d->is_xml, d->is_xml ? "application/xml" : "text/html"));
        inert->is_inert_template = 1;
        doc_realm_owns(d->realm, inert);
        /* The wrapper the record itself holds is the one that outlives this: nothing here has a use for a
           second reference to a document the page cannot name. */
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

void document_install(JSContext *ctx, JSValueConst global, lxb_html_document_t *dom, const char *url,
                      DocumentKind kind, SerializedPolicyContainer policy, SandboxFlags sandbox_flags,
                      uint32_t doc_id, JSValueConst nav_proxy)
{
    Document *d;
    JSValue doc;

    DCHECK(dom != NULL, "the Document install was handed no parsed document");
    /* BEFORE ANYTHING ELSE, and before the no-address return below: the policy is a property of the parsed
       TREE, not of the address, and §7.4 clones it for an about:blank child at the moment that child is
       created — which can be the first thing a boot script does.
       ONCE PER REALM, ON THE BASELINE. A realm IS a document, so a second install into the same one means a
       NAVIGATION, and a navigation's container is per-flow: the flow that navigated sees the new policy and its
       siblings still see the old one. Replacing it here would answer for whichever world ran last, so the
       second install crashes naming the COW record to build instead. A SECOND DOCUMENT is a second realm and
       does not come through here twice. */
    DCHECK(doc_of(ctx) == NULL,
           "a document was installed twice into one realm — that is a NAVIGATION, and its container is "
           "per-flow state: build it as a COW record (like ProxyData's PROXY_REC) captured in its accessor, so "
           "the flow that navigated and the sibling that did not each read their own");
    d = doc_rec_new(ctx, dom, url, kind);
    d->doc = doc_id;
    /* CSP §2.2's SELF-ORIGIN BECOMES A RECORD HERE, at the one point a document's facts stop being the bytes a
       host stated and start being the types the algorithms are written over. origin_parse is the transport
       core/url/origin.h defines for exactly this — "null" states an OPAQUE origin and mints one, which is what
       a sandboxed document's `'self'` must be measured against and is same origin with nothing. */
    DCHECK(serialized_policy_container_exists(policy),
           "a Document was installed with NO §7.1.7 POLICY CONTAINER — every Document has one, and CSP §2.2 "
           "gives its CSP list a self-origin which §2.2.2 states from the response's URL; a document without "
           "one cannot resolve `'self'` and would report every one of its own scripts as blocked by its own "
           "policy. §7.1.7's determine step answers this for every creating operation and always with a "
           "container, so an absence here is a caller that did not run it");
    d->policy = document_policy_new(dom, policy.csp, origin_parse(policy.self_origin), policy.embedder);
    /* §7.1.5's ACTIVE SANDBOXING FLAG SET, as the creating operation decided it — §7.2's creation flags for
       the initial about:blank, §7.4.5's final flag set for a navigated Document. Beside the policy container
       because §7.5.1 hands the Document both in one breath, and NOT derived from it: the container's only
       contribution is the CSP `sandbox` directive, and which of the two algorithms unions that in is a fact
       about the operation that no inspection of the container could recover. */
    d->sandbox_flags = sandbox_flags;
    /* THE REALM'S ACTIVE DOCUMENT FROM HERE ON — set before the early return below, because the policy was
       already built and §7.4 clones it for an about:blank child whether or not this document got an address. */
    JS_SetContextOpaque(ctx, d);
    /* AND THE SAME SENTENCE FROM THE DOCUMENT'S SIDE: this realm is the realm OF `doc_id`, which is the only
       direction a name arriving from ANOTHER INSTANCE can be read in. It is stated here rather than derived by
       a walk for the reason document_realm_of's slot is: this is the one moment the pair exists, so a row
       written here cannot disagree with anything. Before the no-address return, because a document with no
       address still has a navigable a peer can hold a reference into. */
    world_doc_realm_set(doc_id, ctx);
    /* §7.2.3's ONE WindowProxy FOR THIS NAVIGABLE, minted WITH the realm because that is what it is one of.
       Before the early return below: a document with no address still has a navigable, and `window.closed`
       reads the navigable's state through this object. */
    /* §7.2.3: A NAVIGABLE HAS ONE WindowProxy, AND THE NAVIGABLE COMES FIRST. A realm is built for a
       navigable that already exists — §7.4 created it, named it and handed its proxy to the page — so minting
       one here made a SECOND proxy for a navigable that had one. The consequence is not academic: the second
       carries no parent and no opener, so a child's `parent` answered ITSELF instead of its creator and a
       popup's `opener` was null, which is the whole of what a popup is for.
       The caller supplies it, because the caller is whoever owns the navigable: the host for the ROOT one it
       named, and §7.4 for every child it created. */
    DCHECK(window_proxy_is(nav_proxy),
           "a Document was installed for a realm with no navigable — §7.2.3's proxy belongs to the navigable "
           "and the navigable exists before its realm, so the caller that owns it passes it in");
    d->proxy = JS_DupValue(ctx, nav_proxy);
    /* PERMISSIONS POLICY §9.5's CREATE A PERMISSIONS POLICY FOR A NAVIGABLE, here because §9.5 is invoked
       exactly once per Document a navigable is given and the navigable is the argument beside it. Before the
       no-address return below for the same reason the policy container is: a Document with no address still has
       a navigable, still has a container, and a script in it still asks whether it is allowed to use a feature.
       ITS INPUTS ARE THE CONTAINER'S, so they are read here rather than derived later — §7.3.1.3 severs a
       navigable's container when the element is removed, and a policy re-derived after that would silently
       become a top-level document's. */
    d->permissions_policy = document_create_permissions_policy(ctx, nav_proxy);
    /* §7.3.2.1 "Creating browsing contexts" runs the two ancestor-origins step lists here, beside §9.5's
       permissions policy and for the same reason: both are SNAPSHOTS of the container relation, both are
       read off `nav_proxy` while it still names the tree this Document was created in, and both become
       wrong the moment that relation is asked for again later. */
    doc_create_ancestor_origins(ctx, d, nav_proxy);
    if (!url || !*url)
        return;   /* no address, no Document — the page's own throw is the honest answer */

    /* DOCUMENT.PROTOTYPE, and the Document as a real NODE. §4.4 `interface Document : Node`, and it was neither
       — a plain JS_NewObject with the members copied onto it. So `document.nodeType` was undefined,
       `document.appendChild` was not a function, `document.contains(el)` (which is how a page asks whether a
       node is still in the tree) was absent, and `document.body.parentNode.parentNode === document` compared a
       node wrapper against something that was not one. It is a node_wrap of the document node now, so it is in
       the ONE identity table with everything else and its members come from a prototype chained to
       Node.prototype rather than being installed per object. */
    doc = node_wrap(ctx, lxb_dom_interface_node(dom));
    CHECK(JS_IsObject(doc), "the Document wrapper allocation failed");
    d->impl = dom_implementation_new(ctx, doc);   /* §4.5's [SameObject], built WITH the document */
    /* SELECTION API §2's UNIQUE SELECTION, built WITH the document for the reason §6.6's focused-area record
       is built with the realm: a selection minted on the first `getSelection()` would be minted INSIDE
       whichever flow happened to read first, and would be that flow's object rather than the baseline every
       flow forks from. This line is the pre-boot baseline. Only THIS install builds one — document_new's
       documents have no browsing context, which is §4.1's null. */
    d->selection = selection_new(ctx, doc);

    /* `URL`, `documentURI`, `documentElement`, `body` and `head` were SET HERE, as data properties on this one
       object. Every one of them is now an accessor on Document.prototype computed from the receiver's tree —
       see js_doc_tree and js_doc_strings — because a value latched onto one object is wrong in TIME (each is
       defined as a lookup in the tree AS IT IS) and wrong in SUBJECT (it cannot answer for a second document). */

    /* `title`, `cookie`, `referrer` and `readyState` were SET HERE TOO, as own data properties — and the first
       three of them are worse than the tree members above, because each is an ALGORITHM and not a lookup:
       §3.1.5's title getter walks the tree and its setter creates an element, and `cookie` is an ATTACKER SOURCE
       whose whole point is that it is minted PER READ so a candidate run can substitute it. They are accessors
       on Document.prototype now — `title` and `dir` beside the other tree members below, the three resource
       metadata members in core/dom/document_metadata.c. */

    /* §4.4 a Document is an EventTarget through Node, so addEventListener comes down the prototype chain now
       rather than being installed here. */

    /* §3.7.6 makes an attribute an ACCESSOR and §3.4.10's [LegacyUnforgeable] makes this one non-configurable;
       JS_SetPropertyStr wrote a data property at its default flags, so `window.document` had no getter and was
       WRITABLE — `window.document = x` replaced the binding a page cannot shadow in any browser. */
    idl_install_value_attribute(ctx, (JSValue)global, "document", JS_DupValue(ctx, doc), IDL_ATTR_UNFORGEABLE);
    /* HELD, not borrowed: `doc` is this function's own reference and the global got a DUP of it, so the
       component owns one of the two and document_free is what releases it. The comment here used to say
       "borrowed", and a reference nobody released kept the Document — and through it the wrapped tree and the
       window — alive: JS_FreeRuntime's gc_obj_list walk counted 751 surviving objects, one per object in the
       page, from these two lines. */
    d->doc_obj = doc;
    /* HTML §3.1.5: a Document created by "create and initialize a Document object" is at "loading" before any
       script can observe it — which the REALM's record already says, because document_install_proto builds it
       there. This was a document_set_ready(ctx, 0) call, and with §3.1.5 step 1 in place it is provably a
       no-op; the fact it was stating is stated here as the two-sided assertion instead. */
    DCHECK(document_readiness(ctx) == 0,
           "a Document was installed into a realm whose readiness had already moved — the readiness record is "
           "built at \"loading\" with the realm and only §3.1.5's update writes it, so a realm that is past "
           "\"loading\" before it has a Document has run a lifecycle for a document it did not have");
    d->win_obj = JS_DupValue(ctx, global);
    /* HTML §7.4.1's FIRST SESSION HISTORY ENTRY for this navigable — the entry §7.4.5 populates and §7.4.6
       activates for a document a load produced, reached directly because the load is what built this document
       rather than something this engine can be inside of. It is here rather than in the per-realm install for
       the reason core/frame/location.c reads the address at the call: a realm's intrinsics are built before its
       navigable has a document, and an entry holds that document's URL, id and origin. All three exist by this
       line, and this line is still the pre-boot BASELINE, so the entry belongs to every flow rather than to
       whichever one happened to push first. */
    session_history_install_document(ctx);
    /* The interface OBJECTS, now that every prototype exists. Node's goes first because the derived ones
       inherit from it; each component names the one it owns rather than node.c enumerating them. */
    node_install_interfaces(ctx, global);
    {
        JSValue ep = element_proto(ctx);
        node_install_interface(ctx, global, "Element", ep);
        JS_FreeValue(ctx, ep);
    }
    html_element_install(ctx, global);   /* HTMLElement and every per-tag interface object */
    cssom_install(ctx, global);          /* CSSStyleDeclaration, and getComputedStyle on the Window */
    css_style_sheet_install(ctx, global); /* CSSOM §6.1.1 StyleSheet and §6.1.2 CSSStyleSheet */
    style_sheet_list_install(ctx, global); /* CSSOM §6.2.2 StyleSheetList */
    media_list_install(ctx, global);     /* CSSOM §4.4 MediaList, which §7.3's `media` hands back */
    css_rule_install(ctx, global);       /* CSSOM §6.4's five rule interfaces, §7.2 and §7.3 included */
    css_rule_list_install(ctx, global);  /* CSSOM §6.4.1 CSSRuleList */
    custom_elements_install(ctx, global);   /* §4.13.4 window.customElements */
    element_internals_install(ctx, global);  /* §4.13.7 ElementInternals, CustomStateSet, ValidityState */
    dom_token_list_install(ctx, global);    /* §7.1 DOMTokenList */
    node_filter_install(ctx, global);       /* §6.3 NodeFilter — the constants every traverser is read with */
    node_iterator_install(ctx, global);     /* §6.1 NodeIterator */
    tree_walker_install(ctx, global);       /* §6.2 TreeWalker */
    range_install(ctx, global);             /* §5.3 AbstractRange, §5.4 StaticRange, §5.5 Range */
    selection_install(ctx, global);         /* Selection API §3 Selection — its interface object */
    collections_install(ctx, global);       /* §4.2.10 NodeList, §4.2.11 HTMLCollection */
    mutation_observer_install(ctx, global); /* §4.3.1 MutationObserver, §4.3.3 MutationRecord */
    attr_install(ctx, global);              /* §4.9.1/§4.9.2 NamedNodeMap and Attr */
    document_fragment_install(ctx, global); /* §4.7 DocumentFragment, which IS constructible */
    shadow_root_install(ctx, global);      /* §4.8 ShadowRoot */
    document_type_install(ctx, global);     /* §4.6 DocumentType */
    dom_implementation_install(ctx, global);/* §4.5.1 DOMImplementation */
    {
        /* §4.5 declares a CONSTRUCTOR, so `Document` is not one of the interface objects whose call is the
           shared "Illegal constructor" throw. */
        JSValue dp = node_type_proto(ctx, LXB_DOM_NODE_TYPE_DOCUMENT);
        node_install_interface_ctor(ctx, global, "Document", dp,
                                    idl_step_constructor(ctx, "Document", 0, g_id_doc_ctor));
        JS_FreeValue(ctx, dp);
    }
    /* §4.8.5 FOR THE TREE THE PARSER BUILT. Insertion steps run during tree construction in a browser, so an
       <iframe> the page's own markup contains has a child navigable before the first script runs — this
       engine's tree comes from a Lexbor parse that does not pass through the DOM chokepoint, so the parsed
       tree's iframes get their step here. It is LAST, after every wrapper and prototype exists, because
       creating a navigable wraps the element and stores a WindowProxy on it. */
    /* HTML tree construction produces attributes in the NULL namespace; lexbor stamps them with the element's
       namespace instead, and only here — on the tree the parse just built — are the two distinguishable.
       ONLY FOR A TREE AN HTML PARSER BUILT, and the arm is the Document's recorded §4.5 TYPE rather than a
       classification made here. HTML §7.5.2 "Loading HTML documents" and §7.5.4 "Loading text documents" both
       create an HTML PARSER, so their trees carry §13.2.6.1's boundary; §7.5.3 "Loading XML documents"
       creates an XML parser, whose attribute namespaces are Namespaces in XML §6.2's EXPANSION — already
       computed from the bindings in scope, with nothing to correct and names the standard just decided.
       core/html/domparser.c makes the identical split for §8.5.1's two arms and states each seam's own
       reason; the closure is the assertion inside the walk, so a THIRD route into it fires rather than
       silently widening this one. Running it over an XML tree is not a no-op: an `id` or `class` attribute
       reaches the walk's own DCHECK, because those two are the element's cached attribute slots and the walk
       is about to re-intern the name they are keyed on. */
    if (!document_is_xml_of(lxb_dom_interface_document(dom)))
        dom_attr_normalize_parsed(lxb_dom_interface_node(dom));
    /* HTML §4.2.3 FOR THE TREE THE PARSER BUILT, AND FIRST AMONG THESE WALKS. A browser sets the frozen base
       URL as tree construction inserts the element, so a `<base href>` in the page's own markup is in force
       before anything else in the document resolves a URL — and every walk below resolves one: §4.8.5's
       iframe walk resolves `src` into a child navigable's address, §4.8.11.2's media walk resolves `src`, and
       the script inventory resolves every `<script src>`. It is after dom_attr_normalize_parsed because the
       `href` this reads is an attribute in the NULL namespace, which is what that call makes true. */
    html_base_element_parsed(ctx, lxb_dom_interface_node(dom));
    /* HTML §4.12.1.1's `force async` — "set to false by the HTML parser and the XML parser on script elements
       they insert". A lexbor parse has no per-token hook, so the parser's stamp happens on the tree it built,
       here, before the document's first script can read `async`. NOT inert: a document parse's scripts run. It
       is BEFORE the declarative-shadow conversion because that moves a `<template>`'s contents into a shadow
       root, and a `<script>` among them belongs to the tree it ends up in. */
    html_script_parsed(ctx, lxb_dom_interface_node(dom), /*inert*/false);
    /* HTML §13.2.6.4.4's template start tag, for the SAME tree and the same reason: `<template
       shadowrootmode>` attaches a shadow root to its parent DURING tree construction, and a lexbor parse has
       no such step — so the parsed tree's declarative shadow roots are attached here, before the document's
       first script can read one. It runs BEFORE the iframe walk because a `<template shadowrootmode>` moves
       its contents into a shadow root, and an `<iframe>` among them belongs to the tree it ends up in.
       "Allow declarative shadow roots" is the DOCUMENT'S, and this is the one function that installs a
       document a NAVIGATION produced — HTML "read html" creates that parser with the flag TRUE. The documents
       whose flag is false (`createHTMLDocument`, `DOMParser`, XHR's `responseXML`) are built by document_new
       and never reach here, which is why they keep their `<template>` elements. */
    declarative_shadow_parsed(ctx, lxb_dom_interface_node(dom),
                              lxb_dom_interface_node(dom->dom_document.element), /*allow*/ true);
    /* HTML §4.8.11.2 FOR THE TREE THE PARSER BUILT — "if a media element is created with a src attribute, the
       user agent must immediately invoke the media element's resource selection algorithm". §13.2.6.1's
       "create an element for a token" ends by appending the token's attributes to the element, so a parsed
       `<video src=x>` IS created with one; a lexbor parse has no per-token hook, so the invocation happens
       here, before the document's first script can read a networkState. It is NOT the insertion steps, which
       HTML uses one element over for `<source>` and deliberately not for this — see media_element.c.
       AFTER the declarative-shadow conversion, because the walk is shadow-including and a `<video src>` in a
       `<template shadowrootmode>` is in the shadow tree by now. */
    media_element_parsed(ctx, lxb_dom_interface_node(dom));
    /* HTML §4.8.4.3.2 FOR THE SAME TREE AND FOR THE SAME REASON — the list of relevant mutations counts an
       `img`'s `src` being SET and the img element insertion steps, and a parsed element reaches neither: its
       attributes arrive with §13.2.6.1's "create an element for a token" and its insertion is the parser's.
       Without this a document's own `<img src=…>` markup issued no request at all — every image address the
       page ships was invisible to the endpoint surface, and every markup `onerror` had nothing to fire it. */
    html_image_parsed(ctx, lxb_dom_interface_node(dom));
    /* HTML §4.2.4 FOR THE SAME TREE AND FOR THE SAME REASON — §4.6.8.20's appropriate times are stated over an
       element that "becomes browsing-context connected", and a parser-inserted `<link>` becomes connected
       through the parser rather than through the mutation chokepoint, so the markup's own
       `<link rel=preload>` elements are triggered here. AFTER the declarative-shadow conversion, for the
       reason the media and image walks are. */
    html_link_parsed(ctx, lxb_dom_interface_node(dom));
    /* HTML §4.2.6 FOR THE TREE THE PARSER BUILT — "the element is popped off the stack of open elements of an
       HTML parser" is update a style block's first trigger, and a Lexbor parse has no per-token seam, so the
       markup's own `<style>` elements get their CSS style sheets here. AFTER the declarative-shadow conversion,
       for the reason the media walk is: a `<style>` inside a `<template shadowrootmode>` is by now a style
       element in a shadow tree, and its sheet is the shadow root's. */
    html_style_element_parsed(ctx, lxb_dom_interface_node(dom));
    iframe_document_parsed(ctx);
    /* HTML §6.6.7 FOR THE TREE THE PARSER BUILT, for the reason the line above it exists: a browser runs the
       insertion steps during tree construction, so `<input autofocus>` in the page's own markup is a candidate
       before the first script runs. AFTER the iframe walk, because an `<iframe autofocus>` is a candidate whose
       focusable area is its content navigable's active document. */
    autofocus_document_parsed(ctx);
    /* §13.2.7's load lifecycle is NOT claimed here any more — it is one slot on the ONE frontier and this is the
       per-DOCUMENT half, so every document of this agent was re-claiming it. It is declared once, in
       document_init, and given back once, in document_agent_free. */
}

/* THE DOCUMENT'S LIFECYCLE REFERENCES. Both are HELD across the lifecycle — `DOMContentLoaded` fires at the
   Document and `load` at the window long after install returns — and a held reference to either keeps the whole
   object graph alive. With no release, JS_FreeRuntime's gc_obj_list walk reported 751 surviving objects, which
   is the entire page counted one object at a time. A component that holds a reference owns releasing it. */
/* THE (DOCUMENT -> REALM) ANSWER, WHICH §4.2.3 NEEDS AND §3.7 EXPLAINS.
 *
 * The insertion and removing steps belong to the node's NODE DOCUMENT, not to whoever performed the mutation:
 * two same-origin documents are one agent (SECURITY.md's origin-keyed cluster), so `parentDoc.body` and
 * `frame.contentDocument.body` are both writable from one flow, and an <iframe> appended into the CHILD's tree
 * must create its navigable, prepare its scripts and upgrade its custom elements in the CHILD's realm. The
 * mutating member's ctx is the wrong answer for exactly the case the cluster exists to allow.
 *
 * IT LIVES ON THE DOM DOCUMENT'S OWN `user` SLOT, which lexbor keeps for its embedder and never reads. That is
 * O(1) with no registry to keep in step with the realms — a registry is a second list of documents, and the
 * failure mode of one is a stale row answering for a realm that is gone.
 *
 * IT IS ONE INDIRECTION FURTHER NOW, through the document's own record — the slot holds the RECORD and the
 * record names the realm. That is what makes a second Document expressible at all: the slot used to be the
 * whole answer, and "which document" and "which realm" were then the same question, which is exactly the
 * question §4.5.1's three factories have to ask twice. */
JSContext *document_realm_of(const lxb_dom_node_t *n)
{
    Document *d = n ? doc_rec(n->owner_document) : NULL;
    return d ? d->realm : NULL;
}

/* THE REALM WHOSE ACTIVE DOCUMENT THIS DOCUMENT IS, or NULL — see document.h for who asks and why it is not
   document_realm_of. */
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

    if (!d) return;   /* a realm that never had a document — the runner builds one per component test */
    /* THE REALM STOPS NAMING ITS RECORDS BEFORE THE FIRST ONE IS RELEASED, and it is the chain walk below that
       makes the order matter. Every line under this one drops a counted reference, and dropping one can end a
       refcount cascade that runs finalizers — so between the first free and the last there must be no chain for
       document_realm_mark to walk, or a collection landing mid-teardown reads records that are already freed.
       Clearing the opaque here makes that window EMPTY rather than merely short. `doc_of(ctx)` answering NULL
       is the honest state for a realm whose document is going: it no longer has one. */
    JS_SetContextOpaque(ctx, NULL);
    /* THE DOCUMENTS THIS REALM CREATED AT BASELINE go first: each holds a wrapper of the ACTIVE document's realm
       and each owns a whole Lexbor tree, and the active record is what the chain hangs off. */
    for (c = d->next_created; c; c = next) { next = c->next_created; doc_rec_free(ctx, c); }
    /* THIS REALM STOPS BEING THE REALM OF ITS DOCUMENT HERE, and the row goes with it. A stale one would hand
       a peer's operation a JSContext the collector has freed — the failure mode this file names when it
       refuses a registry, closed by writing the clear at the same site as the set rather than by not keeping
       the row. The documents above never had a name in the world registry: they have no browsing context, so
       no navigable names them and no peer can reach them. */
    world_doc_realm_set(d->doc, NULL);
    doc_rec_free(ctx, d);
}

/* THE AGENT'S HALF, UNDONE — core/platform.h's third column, and `document` is the one component that has BOTH
 * halves rather than one. document_free above is the PER-REALM half: it reads `doc_of(ctx)`, clears that
 * realm's own opaque and releases that realm's records, so it answers for ONE realm and runs once per realm.
 * This one answers for the AGENT — the class, the fifteen member declarations, the two realm-value slot ids,
 * the claim this component made on the ONE frontier, and the ten sub-components document_init declares.
 *
 * THE TWO HALVES RUN AT DIFFERENT PHASES, AND THE ORDER BETWEEN THEM IS NOT THIS COLUMN'S TO DECIDE. A CHILD
 * navigable's document is released from quickjs's realm-teardown hook (core/frame/navigable.c), which fires
 * when the JSContext dies — inside JS_RunGC or inside JS_FreeRuntime, and BOTH of those are after
 * platform_agent_free. So there is no position on the release column, and no reordering of any host's teardown,
 * under which every Document record is already gone when this runs: a page with one <iframe> has records that
 * outlive the whole platform by construction. An assert demanding otherwise would fire on every such page,
 * which is why there is not one.
 *
 * WHAT MAKES THAT SAFE IS A FACT ABOUT THE RECORDS, NOT ABOUT THE ORDER, and it is the reason this row can
 * exist at all: document_free reads NO static of this file. It walks the record, releases the four references
 * doc_rec_refs names, frees the policy container and destroys the trees the record owns — every one of those
 * reached through the record itself. So a document released after the agent's half finds nothing missing, and
 * this half may clear every handle it holds without waiting for a realm it cannot wait for.
 *
 * WHICH IS ALSO WHY §gc's MARK HOOK IS NOT GIVEN BACK HERE, and that is ASSERTED below rather than left to a
 * comment. Its storage is `rt->ctx_mark` — the RUNTIME's, released by JS_FreeRuntime — so it is not a claim in
 * the sense core/platform.h's fourth paragraph states (a slot in another COMPONENT, whose static outlives this
 * release and would go on naming freed state), and no static of this file holds it to go stale for a second
 * agent. Clearing it here would be actively WRONG: the collection a host runs after platform_agent_free is
 * exactly what tears down the child realms whose records this hook reports, and a record whose references
 * gc_decref cannot subtract is the uncollectable realm cycle doc_rec_new's own DCHECK exists to prevent,
 * arriving from the other end. */
void document_agent_free(JSRuntime *rt)
{
    /* NOT `if (!g_document_rt) return;`. The release is the inverse of the DECLARATION and rides the same row
       of core/platform.c's one list, whose declare pass is unconditional and whose table asserts that a release
       row has a declare. */
    DCHECK(rt == g_document_rt,
           "the Document component was released against a runtime that is not the one it declared in — every "
           "atom and every value the sub-components below give back would have its reference subtracted from a "
           "runtime that never took it, and JS_FreeValueRT reports nothing at all when it is the wrong one");
    /* THE ONE CLAIM THIS COMPONENT MADE IN ANOTHER, GIVEN BACK FIRST. §13.2.7's load lifecycle is a slot on the
       ONE frontier (solver/engine.c), which solver_agent_free releases AFTER the whole platform — so a release
       that kept it would leave the scheduler holding a callback into a component whose class and member
       declarations are gone. That is the defect core/agent_state.h records for Indexed Database §2.7.1, and it
       is first for the reason element_free's two DOM-write hooks are: the very next call can run a finalizer. */
    engine_set_document_done_hook(NULL);
    /* THE TEN SUB-COMPONENTS document_init DECLARES, IN THE REVERSE OF THAT ORDER — the same rule
       platform_agent_free runs its own list by, and for the same reason: a component declared after another may
       hold what that one minted.
       THREE OF THESE WERE BEING FREED FROM element_free's CASCADE, which is a release undoing somebody else's
       work — the shape platform_check_table forbids one row above. Nothing declares document_fragment,
       shadow_root or slot but document_init, so nothing may release them but this. They come back to their
       declarer here, and element_free names the move at the lines they left. */
    autofocus_free();
    focus_free();
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
    /* THE PROTOTYPES ARE THE REALMS' — each is in its own class-proto slot and released with its context. What
       this component itself holds is the class, the pool entries and the two realm-value slot ids. */
    g_document_class = 0;
    g_id_create_element = g_id_create_text = g_id_create_comment = g_id_create_fragment =
        g_id_create_element_ns = g_id_create_iterator = g_id_create_walker = g_id_create_range =
        g_id_create_event = g_id_create_cdata = g_id_create_pi = g_id_doc_ctor = g_id_adopt_node =
        g_id_title_set = g_id_dir_set = g_id_location_set = -1;
    g_ready_slot = g_showing_slot = -1;
    DCHECK(JS_GetContextMarkHook(rt) == document_realm_mark,
           "§gc's realm-mark hook was given back at the Document component's agent release — it must NOT be, "
           "and this is where that is checked. The records it reports outlive this call (a child navigable's is "
           "released from quickjs's realm-teardown hook, which fires inside JS_RunGC or JS_FreeRuntime), and "
           "each of them holds four counted references back INTO its own realm. With the hook gone gc_decref "
           "cannot subtract them, the realm reads as externally rooted, and the collection that would have "
           "freed the record is the one the record prevents — which is exactly the cycle doc_rec_new asserts "
           "the hook's presence to make impossible. Its storage is rt->ctx_mark and JS_FreeRuntime releases it");
    g_document_rt = NULL;
}
