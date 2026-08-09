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
#include <string.h>

#include "check.h"
#include "solver/dom_cow.h"   /* dom_cow_note_created — a created node belongs to the flow's delta */
#include "quickjs.h"
#include "solver/concolic.h"
#include "solver/engine.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/html/html_element.h"
#include "core/html/html_form.h"
#include "core/html/custom_elements.h"
#include "core/html/html_iframe.h"
#include "core/dom/dom_token_list.h"
#include "core/dom/collections.h"
#include "core/dom/attr.h"
#include "core/css/css_style_declaration.h"
#include "core/dom/document.h"
#include "solver/world.h"
#include "core/frame/window_proxy.h"
#include "core/dom/document_fragment.h"
#include "core/idl_args.h"
#include "core/realm.h"

/* Every member here takes DOMStrings; createElementNS takes two. Declared, not masked. */
static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
#include "core/dom/node.h"
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>

#include "core/dom/element.h"

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
typedef struct Document {
    uint32_t             doc;      /* this document's handle in the world registry — its NAME is what crosses */
    lxb_html_document_t *dom;
    PolicyContainer     *policy;    /* owned */
    JSValue              doc_obj;   /* the `document` object — HELD, released by document_free */
    JSValue              win_obj;   /* this document's Window — HELD */
    /* §7.2.5.1's ONE WindowProxy FOR THIS NAVIGABLE — `window`, `self`, and the `source` of every message this
       document posts. It lives on the REALM because that is what it is one of: a page comparing `e.source`
       across two messages must find the same object, and a table keyed by document would be an immortal root
       holding one proxy per navigable a forced-execution frontier ever created — thousands, none collectable.
       HELD, released with the realm. */
    JSValue              proxy;
    char                 url[2048]; /* the document's address, which §4.4 baseURI reads */
} Document;

/* THE RUNNING REALM'S DOCUMENT. The context opaque, because a JSContext IS the document — so there is no table
   to look it up in and no way for the answer to be the wrong document's. NULL before install, which is a state
   only the accessors that tolerate it may see. */
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
 * lxb_selectors_match_node is what makes the walk equivalent to lxb_selectors_find rather than an approximation
 * of it: a combinator is resolved by walking UP from the candidate, through the whole document, so
 * §4.2.6's scoped matching still holds — `el.querySelectorAll('div p')` finds a <p> under `el` whose <div>
 * ancestor is OUTSIDE `el`, because the selector is evaluated against the document and only the RESULTS are
 * filtered to the subtree. That is asserted rather than assumed; it is the case an implementation that walks a
 * subtree in isolation gets wrong. */
enum { QS_FIRST = 0, QS_ALL, QS_MATCHES, QS_CLOSEST };

typedef struct {
    uint8_t stage;
    lxb_css_parser_t       *parser;
    lxb_selectors_t        *selectors;
    lxb_css_selector_list_t *list;
    lxb_dom_node_t *root, *cursor;
    JSValue arr;      /* QS_ALL's collected matches (owned) */
    uint32_t n;
} QsState;

static lxb_status_t qs_hit_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *vctx)
{
    (void)node; (void)spec;
    *(bool *)vctx = true;
    return LXB_STATUS_OK;
}

static void qs_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    QsState *s = st;
    /* A FORK CANNOT REACH A SELECTOR WALK. It runs none of the page's code, so no concolic branch can happen
       under it — which is what lets the compiled list be held as a bare pointer: there is no such thing as half
       a selector context to hand a second flow. */
    DCHECK(s->parser == NULL || s->stage == 0, "a selector walk was forked mid-walk");
    v->val(ctx, &s->arr);
}

static void qs_release(JSContext *ctx, void *st)
{
    QsState *s = st;
    (void)ctx;
    /* The throw path owns these too — a flow dropped mid-walk would otherwise leak a compiled selector list and
       the two contexts behind it. */
    if (s->list) lxb_css_selector_list_destroy_memory(s->list);
    if (s->selectors) lxb_selectors_destroy(s->selectors, true);
    if (s->parser) lxb_css_parser_destroy(s->parser, true);
    s->list = NULL; s->selectors = NULL; s->parser = NULL;
}

/* Does the compiled selector match this node? The one place lexbor is asked, so the four members cannot
   disagree about what a selector means. */
static bool qs_matches(QsState *s, lxb_dom_node_t *node)
{
    bool hit = false;
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    lxb_selectors_match_node(s->selectors, node, s->list, qs_hit_cb, &hit);
    return hit;
}

static int js_document_qs(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    QsState *s = st;
    int magic = idl_step_magic(hdr);

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    if (s->stage == 0) {
        lxb_dom_node_t *n = node_of(hdr->this_val);
        const char *sel;

        if (!n || argc < 1) {
            *presult = magic == QS_ALL ? collections_static(ctx, JS_NewArray(ctx))
                     : magic == QS_MATCHES ? JS_FALSE : JS_NULL;
            return JS_STEP_DONE;
        }
        sel = JS_ToCString(ctx, argv[0]);   /* a real string by now: the declaration converted it */
        if (!sel) return JS_STEP_ABRUPT;
        s->parser = lxb_css_parser_create();
        s->selectors = lxb_selectors_create();
        if (!s->parser || lxb_css_parser_init(s->parser, NULL) != LXB_STATUS_OK ||
            !s->selectors || lxb_selectors_init(s->selectors) != LXB_STATUS_OK) {
            JS_FreeCString(ctx, sel);
            CHECK_FAIL("the CSS selector engine could not be initialised");
        }
        s->list = lxb_css_selectors_parse(s->parser, (const lxb_char_t *)sel, strlen(sel));
        JS_FreeCString(ctx, sel);
        if (!s->list) {
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
        s->stage = 1;
        return JS_STEP_YIELD;
    }

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

    if (qs_matches(s, s->cursor)) {
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

static const IdlStepDecl QS_STEP = { js_document_qs, sizeof(QsState), qs_visit, qs_release };

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
    JSValue r;

    (void)this_val;
    if (argc < 1) return JS_NULL;
    tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return JS_EXCEPTION;
    el = lxb_dom_document_create_element(lxb_dom_interface_document(doc_here(ctx)->dom),
                                         (const lxb_char_t *)tag, strlen(tag), NULL);
    JS_FreeCString(ctx, tag);
    dom_cow_note_created(el ? lxb_dom_interface_node(el) : NULL);   /* this flow made it: the delta owns it */
    DCHECK(el != NULL, "createElement produced no element — a page building its DOM would silently build "
                       "nothing and every query after it would answer null");
    r = element_wrap(ctx, el);
    return r;
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

    (void)this_val;
    s = argc >= 1 ? JS_ToCStringLen(ctx, &len, argv[0]) : JS_ToCStringLen(ctx, &len, JS_UNDEFINED);
    if (!s) return JS_EXCEPTION;
    t = lxb_dom_document_create_text_node(lxb_dom_interface_document(doc_here(ctx)->dom), (const lxb_char_t *)s, len);
    dom_cow_note_created(t ? lxb_dom_interface_node(t) : NULL);   /* this flow made it */
    JS_FreeCString(ctx, s);
    DCHECK(t != NULL, "createTextNode produced no node — a page building its DOM would silently build nothing");
    return node_wrap(ctx, lxb_dom_interface_node(t));
}

static JSValue js_doc_create_comment(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    const char *s;
    size_t len = 0;
    lxb_dom_comment_t *c;

    (void)this_val;
    s = argc >= 1 ? JS_ToCStringLen(ctx, &len, argv[0]) : JS_ToCStringLen(ctx, &len, JS_UNDEFINED);
    if (!s) return JS_EXCEPTION;
    c = lxb_dom_document_create_comment(lxb_dom_interface_document(doc_here(ctx)->dom), (const lxb_char_t *)s, len);
    dom_cow_note_created(c ? lxb_dom_interface_node(c) : NULL);   /* this flow made it */
    JS_FreeCString(ctx, s);
    DCHECK(c != NULL, "createComment produced no node — a page building its DOM would silently build nothing");
    return node_wrap(ctx, lxb_dom_interface_node(c));
}

/* DOM 4.5.3 "validate and extract" — the whole of it, because every one of its failures is a DOMException the
   spec names and a page catches. `""` MEANS NULL: a namespace of the empty string is set to null before
   anything else, and skipping that step handed Lexbor a zero-length namespace it refuses, which is what made
   `document.createElementNS("", "div")` — a shape five WPT documents open with — produce no element at all.
   Returns 0 and leaves an exception pending on failure; on success *local points into qname. */
static int validate_and_extract(JSContext *ctx, const char **ns, const char *qname,
                                const char **local, size_t *prefix_len)
{
    static const char XML_NS[]   = "http://www.w3.org/XML/1998/namespace";
    static const char XMLNS_NS[] = "http://www.w3.org/2000/xmlns/";
    const char *colon;

    if (*ns && **ns == 0)
        *ns = NULL;                       /* 1. "If namespace is the empty string, set it to null." */
    if (!*qname) {
        JS_ThrowDOMException(ctx, "InvalidCharacterError", "the qualified name is empty");
        return 0;
    }
    colon = strchr(qname, ':');
    *local = colon ? colon + 1 : qname;
    *prefix_len = colon ? (size_t)(colon - qname) : 0;
    if (colon && (*prefix_len == 0 || **local == 0 || strchr(*local, ':'))) {
        JS_ThrowDOMException(ctx, "InvalidCharacterError", "'%s' is not a valid qualified name", qname);
        return 0;
    }
    if (colon && !*ns) {
        JS_ThrowDOMException(ctx, "NamespaceError", "a prefixed name needs a namespace");
        return 0;
    }
    if (colon && *prefix_len == 3 && memcmp(qname, "xml", 3) == 0 && strcmp(*ns, XML_NS) != 0) {
        JS_ThrowDOMException(ctx, "NamespaceError", "the xml prefix is bound to the XML namespace");
        return 0;
    }
    {
        int q_is_xmlns = strcmp(qname, "xmlns") == 0;
        int p_is_xmlns = colon && *prefix_len == 5 && memcmp(qname, "xmlns", 5) == 0;
        if ((q_is_xmlns || p_is_xmlns) && (!*ns || strcmp(*ns, XMLNS_NS) != 0)) {
            JS_ThrowDOMException(ctx, "NamespaceError", "xmlns is bound to the XMLNS namespace");
            return 0;
        }
        if (*ns && strcmp(*ns, XMLNS_NS) == 0 && !q_is_xmlns && !p_is_xmlns) {
            JS_ThrowDOMException(ctx, "NamespaceError", "the XMLNS namespace only binds xmlns");
            return 0;
        }
    }
    return 1;
}

/* 4.5.3 createElementNS(namespace, qualifiedName). createElement's element creation over the validated triple —
   the element carries the NAMESPACE the page asked for, so `el.namespaceURI` and a namespaced selector answer
   what they should. testharness.js reaches it on every completed document (`createElementNS(xhtml_ns, "style")`
   in Output.show_results), and a missing one threw inside the completion-callback list, silencing documents
   whose tests had all already run. */
static JSValue js_doc_create_element_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    const char *ns = NULL, *qname, *local;
    size_t prefix_len = 0;
    lxb_dom_element_t *el;
    JSValue r;

    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "createElementNS requires a namespace and a qualified name");
    if (!JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
        ns = JS_ToCString(ctx, argv[0]);
        if (!ns) return JS_EXCEPTION;
    }
    qname = JS_ToCString(ctx, argv[1]);
    if (!qname) { if (ns) JS_FreeCString(ctx, ns); return JS_EXCEPTION; }

    {
        const char *ns_in = ns;   /* validate_and_extract may null it; the ORIGINAL is what must be freed */
        if (!validate_and_extract(ctx, &ns, qname, &local, &prefix_len)) {
            if (ns_in) JS_FreeCString(ctx, ns_in);
            JS_FreeCString(ctx, qname);
            return JS_EXCEPTION;
        }
        el = lxb_dom_element_create(lxb_dom_interface_document(doc_here(ctx)->dom),
                                    (const lxb_char_t *)local, strlen(local),
                                    (const lxb_char_t *)ns, ns ? strlen(ns) : 0,
                                    prefix_len ? (const lxb_char_t *)qname : NULL, prefix_len,
                                    NULL, 0, false);
        DCHECK(el != NULL, "createElementNS produced no element for a name the spec accepts — a page building "
                           "its DOM would silently build nothing and every query after it would answer null");
        if (ns_in) JS_FreeCString(ctx, ns_in);
    }
    JS_FreeCString(ctx, qname);
    r = element_wrap(ctx, el);
    return r;
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

const char *document_base_url(JSContext *ctx)
{
    Document *d = doc_here(ctx);
    DCHECK(d->url[0] != '\0', "a node's baseURI was read before the document was installed");
    return d->url;
}

static void document_set_ready(JSContext *ctx, const char *state)
{
    Document *d = doc_here(ctx);
    if (JS_IsObject(d->doc_obj))
        JS_SetPropertyStr(ctx, d->doc_obj, "readyState", JS_NewString(ctx, state));
}

static int document_done_stage(JSContext *ctx, int stage)
{
    if (stage == 0) {
        document_set_ready(ctx, "interactive");
        /* §3.1.1: DOMContentLoaded is fired AT THE DOCUMENT and BUBBLES, which is how a `window.onload`-style
           listener registered on window hears it — the propagation path derives that from the document's
           ancestors now rather than the caller naming the window. It is not cancelable. */
        event_target_fire(ctx, doc_here(ctx)->doc_obj,
                          event_new(ctx, "DOMContentLoaded", /*bubbles*/ true, /*cancelable*/ false));
        return 1;
    }
    DCHECK(stage == 1, "the document lifecycle was asked for a stage it does not have");
    document_set_ready(ctx, "complete");
    /* HTML: `load` is fired at the WINDOW and does not bubble — there is nothing above it to bubble to. */
    event_target_fire(ctx, doc_here(ctx)->win_obj,
                      event_new(ctx, "load", /*bubbles*/ false, /*cancelable*/ false));
    return 1;
}

/* §3.1.1's `location` — the LOCATION OBJECT OF THIS DOCUMENT'S RELEVANT GLOBAL. It was absent, and absent is
   not a small gap here: `document.location.pathname` is how WPT's own /common/PrefixedPostMessage.js names a
   message channel, so 63 subtests across html/browsers failed on a property of undefined without ever reaching
   what they were testing. The IDL audit had it listed among Document's absent members the whole time.
   IT IS THE GLOBAL'S, not a second Location: a document and its window are one browsing context and §3.1.1 says
   "the Location object of this's relevant global object", so this reads the one location.c installed rather
   than building another that would compare unequal to it.
   NULL WHEN THE DOCUMENT IS NOT FULLY ACTIVE, which is what §3.1.1's `Location?` is for — and it is also the
   honest answer for a host that installed no Location at all (one whose document has no address), where
   inventing an object would claim an address the engine was never given. */
static JSValue js_doc_location(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue g = JS_GetGlobalObject(ctx), loc;

    (void)this_val; (void)magic;
    loc = JS_GetPropertyStr(ctx, g, "location");
    JS_FreeValue(ctx, g);
    if (JS_IsUndefined(loc)) { JS_FreeValue(ctx, loc); return JS_NULL; }
    return loc;
}

/* The Document METHODS — on Document.prototype, so there is one of each rather than one per install, and so
   `Document.prototype.querySelector` is a thing that exists. */
/* THE DECLARATIONS ARE THE AGENT'S, THE INSTALLS ARE THE REALM'S — the IDL pool is sealed after agent init, so
   a declaration minted from a per-realm install trips idl_declared_before_seal on the SECOND realm. */
static JSClassID g_document_class;   /* §3.1.1's prototype slot, per realm */
static int g_id_create_element = -1, g_id_create_text = -1, g_id_create_comment = -1,
           g_id_create_fragment = -1, g_id_create_element_ns = -1;

static void document_declare_members(JSContext *ctx)
{
    g_id_create_element = idl_method_id(ctx, IDL_1STR, 1, js_doc_create_element, 0);
    g_id_create_text = idl_method_id(ctx, IDL_1STR, 1, js_doc_create_text, 0);
    g_id_create_comment = idl_method_id(ctx, IDL_1STR, 1, js_doc_create_comment, 0);
    g_id_create_fragment = idl_method_id(ctx, NULL, 0, js_doc_create_fragment, 0);
    g_id_create_element_ns = idl_method_id(ctx, IDL_2STR, 2, js_doc_create_element_ns, 0);
}

static void document_install_members(JSContext *ctx, JSValueConst proto)
{
    idl_install_method(ctx, proto, "createElement", 1, g_id_create_element);
    idl_install_method(ctx, proto, "createTextNode", 1, g_id_create_text);
    idl_install_method(ctx, proto, "createComment", 1, g_id_create_comment);
    idl_install_method(ctx, proto, "createDocumentFragment", 0, g_id_create_fragment);
    {
        /* §3.1.5's five element shortcuts, each a LIVE HTMLCollection over the document. */
        static const char *const NAMES[] = { "forms", "images", "scripts", "embeds", "links" };
        unsigned k;
        for (k = 0; k < sizeof(NAMES) / sizeof(NAMES[0]); k++)
            idl_install_accessor(ctx, proto, NAMES[k], js_doc_shortcut, (int)k, -1);
    }
    idl_install_method(ctx, proto, "createElementNS", 2, g_id_create_element_ns);
    /* §3.1.1: `[PutForwards=href] readonly attribute Location? location`. The forwarding half of the extended
       attribute — `document.location = url` navigating — is NOT built, and it is absent rather than silently
       dropped: a setter that stored a string would make a page believe it had navigated. */
    idl_install_accessor(ctx, proto, "location", js_doc_location, 0, -1);
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
PolicyContainer *document_policy_new(lxb_html_document_t *dom, const char *csp)
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
        PolicyContainer *p = policy_container_new(acc, NULL);
        free(acc);
        return p;
    }
}

const PolicyContainer *document_policy(JSContext *ctx) { return doc_here(ctx)->policy; }

JSValueConst document_window_proxy(JSContext *ctx)
{
    Document *d = doc_here(ctx);
    DCHECK(!JS_IsUndefined(d->proxy), "this realm's WindowProxy was read before its Document was installed — "
                                      "§7.2.5.1 gives a navigable ONE, and it is minted with the realm");
    return d->proxy;
}

uint32_t document_doc(JSContext *ctx) { return doc_here(ctx)->doc; }

JSValueConst document_object(JSContext *ctx) { return doc_here(ctx)->doc_obj; }

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

    JS_NewClassID(JS_GetRuntime(ctx), &g_document_class);
    JS_NewClass(JS_GetRuntime(ctx), g_document_class, &d);
    node_claim_type(LXB_DOM_NODE_TYPE_DOCUMENT, g_document_class);
    document_declare_members(ctx);
    document_fragment_init(ctx);   /* §4.7, before any fragment is wrapped as a bare Node */
    realm_declare_intrinsic(document_install_proto);
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
    /* §4.5: `Document includes ParentNode` — not ChildNode, because a document has no parent to be removed
       from — and `NonElementParentNode`, the same getElementById DocumentFragment includes. */
    node_install_parent_mixin(ctx, proto);
    node_install_nonelement_parent_mixin(ctx, proto);
    JS_SetClassProto(ctx, g_document_class, proto);
}

void document_install(JSContext *ctx, JSValueConst global, lxb_html_document_t *dom, const char *url,
                      const char *csp, uint32_t doc_id, JSValueConst nav_proxy)
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
    d = calloc(1, sizeof *d);
    CHECK(d != NULL, "document: OOM naming this realm's document");
    d->dom = dom;
    d->doc = doc_id;
    d->doc_obj = JS_UNDEFINED;
    d->proxy = JS_UNDEFINED;
    d->win_obj = JS_UNDEFINED;
    d->policy = document_policy_new(dom, csp);
    /* THE REALM IS THE DOCUMENT FROM HERE ON — set before the early return below, because the policy was
       already built and §7.4 clones it for an about:blank child whether or not this document got an address. */
    JS_SetContextOpaque(ctx, d);
    document_realm_set(lxb_dom_interface_document(dom), ctx);   /* and the answer the other way round */
    /* §7.2.5.1's ONE WindowProxy FOR THIS NAVIGABLE, minted WITH the realm because that is what it is one of.
       Before the early return below: a document with no address still has a navigable, and `window.closed`
       reads the navigable's state through this object. */
    /* §7.2.5.1: A NAVIGABLE HAS ONE WindowProxy, AND THE NAVIGABLE COMES FIRST. A realm is built for a
       navigable that already exists — §7.4 created it, named it and handed its proxy to the page — so minting
       one here made a SECOND proxy for a navigable that had one. The consequence is not academic: the second
       carries no parent and no opener, so a child's `parent` answered ITSELF instead of its creator and a
       popup's `opener` was null, which is the whole of what a popup is for.
       The caller supplies it, because the caller is whoever owns the navigable: the host for the ROOT one it
       named, and §7.4 for every child it created. */
    DCHECK(window_proxy_is(nav_proxy),
           "a Document was installed for a realm with no navigable — §7.2.5.1's proxy belongs to the navigable "
           "and the navigable exists before its realm, so the caller that owns it passes it in");
    d->proxy = JS_DupValue(ctx, nav_proxy);
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

    snprintf(d->url, sizeof(d->url), "%s", url);

    /* Facts about the document this engine parsed. */
    JS_SetPropertyStr(ctx, doc, "URL",         JS_NewString(ctx, url));
    JS_SetPropertyStr(ctx, doc, "documentURI", JS_NewString(ctx, url));

    /* 3.1.1 title: the document's title, which Lexbor already computed from the tree — a pure read, no page
       code, and the real answer rather than a placeholder. */
    {
        size_t n = 0;
        const lxb_char_t *t = lxb_html_document_title(dom, &n);
        JS_SetPropertyStr(ctx, doc, "title",
                          t ? JS_NewStringLen(ctx, (const char *)t, n) : JS_NewString(ctx, ""));
    }

    /* INPUT. A cookie jar this engine was not handed and a referrer the visitor arrived with: unknown, not
       empty. `document.cookie` is how a session reaches a request URL, so "" would make every cookie-gated
       path unreachable — the same mistake as a concrete `undefined` for absent app state. */
    JS_SetPropertyStr(ctx, doc, "cookie",   concolic_new(ctx, "{document.cookie}",   "document.cookie",   JS_UNDEFINED));
    JS_SetPropertyStr(ctx, doc, "referrer", concolic_new(ctx, "{document.referrer}", "document.referrer", JS_UNDEFINED));

    /* getElementById is a pure tree walk over the id attribute — no page code, and the REAL element, wrapped
       once so identity holds. querySelector is still absent: it needs a CSS selector engine, and answering it
       with a partial matcher would return the wrong element silently, which is worse than the throw that names
       what to build. */

    /* §3.1.1 documentElement / body / head — the three tree entry points every page starts from. Lexbor has
       already parsed them, so these are the REAL elements wrapped once (identity holds: `el.parentNode ===
       document.body` is a comparison pages make).
       §3.1.1'S `body` IS "the first of the html element's children that is either a BODY or a FRAMESET
       element, or null". It read Lexbor's body-element accessor and asserted the result was never null,
       explaining that tree construction always creates one — and that is FALSE. A FRAMESET document is
       `<html><head></head><frameset>...</frameset></html>` with no body at all, which is not a malformed parse
       but the parser following the spec: the `in body` insertion mode gives way to `in frameset` and the body
       element is never inserted. The corpus has such documents and loads them in frames, so the assert fired on
       a correct parse the moment child navigables started loading their addresses — a false invariant standing
       exactly where the real rule belongs. */
    {
        lxb_dom_element_t *root = lxb_dom_interface_element(dom->dom_document.element);
        lxb_dom_node_t *body = NULL, *n;
        lxb_html_head_element_t *head = lxb_html_document_head_element(dom);

        for (n = lxb_dom_interface_node(root)->first_child; n; n = n->next) {
            size_t qn = 0;
            const lxb_char_t *q;
            if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            q = lxb_dom_element_qualified_name(lxb_dom_interface_element(n), &qn);
            if (!q) continue;
            if ((qn == 4 && !memcmp(q, "body", 4)) || (qn == 8 && !memcmp(q, "frameset", 8))) { body = n; break; }
        }
        JS_SetPropertyStr(ctx, doc, "documentElement", element_wrap(ctx, root));
        /* NULL IS A REAL ANSWER HERE, and the only one for a document whose html element has neither — which
           §3.1.1 states outright. */
        JS_SetPropertyStr(ctx, doc, "body",
                          body ? element_wrap(ctx, lxb_dom_interface_element(body)) : JS_NULL);
        JS_SetPropertyStr(ctx, doc, "head", element_wrap(ctx, lxb_dom_interface_element(head)));
    }

    /* §4.4 a Document is an EventTarget through Node, so addEventListener comes down the prototype chain now
       rather than being installed here. "loading" until its scripts have run. */
    JS_SetPropertyStr(ctx, doc, "readyState", JS_NewString(ctx, "loading"));

    JS_SetPropertyStr(ctx, (JSValue)global, "document", JS_DupValue(ctx, doc));
    /* HELD, not borrowed: `doc` is this function's own reference and the global got a DUP of it, so the
       component owns one of the two and document_free is what releases it. The comment here used to say
       "borrowed", and a reference nobody released kept the Document — and through it the wrapped tree and the
       window — alive: JS_FreeRuntime's gc_obj_list walk counted 751 surviving objects, one per object in the
       page, from these two lines. */
    d->doc_obj = doc;
    d->win_obj = JS_DupValue(ctx, global);
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
    custom_elements_install(ctx, global);   /* §4.13.4 window.customElements */
    dom_token_list_install(ctx, global);    /* §7.1 DOMTokenList */
    collections_install(ctx, global);       /* §4.2.10 NodeList, §4.2.11 HTMLCollection */
    attr_install(ctx, global);              /* §4.9.1/§4.9.2 NamedNodeMap and Attr */
    document_fragment_install(ctx, global); /* §4.7 DocumentFragment, which IS constructible */
    {
        JSValue dp = node_type_proto(ctx, LXB_DOM_NODE_TYPE_DOCUMENT);
        node_install_interface(ctx, global, "Document", dp);
        JS_FreeValue(ctx, dp);
    }
    /* §4.8.5 FOR THE TREE THE PARSER BUILT. Insertion steps run during tree construction in a browser, so an
       <iframe> the page's own markup contains has a child navigable before the first script runs — this
       engine's tree comes from a Lexbor parse that does not pass through the DOM chokepoint, so the parsed
       tree's iframes get their step here. It is LAST, after every wrapper and prototype exists, because
       creating a navigable wraps the element and stores a WindowProxy on it. */
    iframe_document_parsed(ctx);
    engine_set_document_done_hook(document_done_stage);
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
 * failure mode of one is a stale row answering for a realm that is gone. */
void document_realm_set(lxb_dom_document_t *dom, JSContext *ctx)
{
    DCHECK(dom != NULL, "a realm was named for no document");
    dom->user = ctx;
}

JSContext *document_realm_of(const lxb_dom_node_t *n)
{
    if (!n || !n->owner_document)
        return NULL;
    return (JSContext *)n->owner_document->user;
}

void document_free(JSContext *ctx)
{
    Document *d = doc_of(ctx);

    if (!d) return;   /* a realm that never had a document — the runner builds one per component test */
    JS_FreeValue(ctx, d->proxy);
    JS_FreeValue(ctx, d->win_obj);
    JS_FreeValue(ctx, d->doc_obj);
    policy_container_free(d->policy);   /* malloc'd, so the GC walk would never have named it */
    if (d->dom)
        document_realm_set(lxb_dom_interface_document(d->dom), NULL);
    free(d);
    JS_SetContextOpaque(ctx, NULL);
}
