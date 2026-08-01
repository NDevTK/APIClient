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
#include "quickjs.h"
#include "solver/concolic.h"
#include "solver/engine.h"
#include "core/events/event_target.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>

#include "core/dom/element.h"

/* The document this component installed, for the tree walks below. One instance is one document. */
static lxb_html_document_t *g_doc;
static void element_doc_set(lxb_html_document_t *d) { g_doc = d; }

/* 4.5.3 getElementById: the first element in tree order whose id attribute matches. A pure Lexbor walk. */
static JSValue js_doc_get_element_by_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_collection_t *col;
    const char *id;
    JSValue r = JS_NULL;

    (void)this_val;
    DCHECK(g_doc != NULL, "getElementById ran before the document was installed");
    if (argc < 1) return JS_NULL;
    id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_EXCEPTION;
    col = lxb_dom_collection_make(&g_doc->dom_document, 8);
    if (col) {
        lxb_dom_element_t *root = lxb_dom_interface_element(g_doc->dom_document.element);
        if (root &&
            lxb_dom_elements_by_attr(root, col, (const lxb_char_t *)"id", 2,
                                     (const lxb_char_t *)id, strlen(id), true) == LXB_STATUS_OK &&
            lxb_dom_collection_length(col) > 0)
            r = element_wrap(ctx, lxb_dom_collection_element(col, 0));
        lxb_dom_collection_destroy(col, true);
    }
    JS_FreeCString(ctx, id);
    return r;
}

/* 4.2.6 querySelector / querySelectorAll over Lexbor's CSS selector engine — the real one, which ships with
   the DOM library this engine already links. This was ABSENT with a note saying it "needs a CSS selector
   engine"; the engine was sitting in the same source tree, and the note was a workaround dressed as a gap.
   A partial matcher would be worse than absence — it returns the WRONG element silently — but the complete one
   is right here. */
typedef struct { lxb_dom_element_t *first; JSContext *ctx; JSValue arr; uint32_t n; } QsCtx;

static lxb_status_t qs_found(lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *vctx)
{
    QsCtx *q = vctx;
    (void)spec;
    if (!q->first)
        q->first = lxb_dom_interface_element(node);
    if (!JS_IsUndefined(q->arr))
        JS_SetPropertyUint32(q->ctx, q->arr, q->n++, element_wrap(q->ctx, lxb_dom_interface_element(node)));
    return LXB_STATUS_OK;
}

/* Run `sel` over the document. `all` collects every match into an array; otherwise the first in tree order. */
static JSValue qs_run(JSContext *ctx, const char *sel, bool all)
{
    lxb_css_parser_t *parser;
    lxb_selectors_t *selectors;
    lxb_css_selector_list_t *list;
    QsCtx q = { NULL, ctx, JS_UNDEFINED, 0 };
    JSValue r = all ? JS_NewArray(ctx) : JS_NULL;

    if (all) q.arr = r;
    parser = lxb_css_parser_create();
    if (!parser || lxb_css_parser_init(parser, NULL) != LXB_STATUS_OK)
        return all ? r : JS_NULL;
    selectors = lxb_selectors_create();
    if (!selectors || lxb_selectors_init(selectors) != LXB_STATUS_OK) {
        lxb_css_parser_destroy(parser, true);
        return all ? r : JS_NULL;
    }
    list = lxb_css_selectors_parse(parser, (const lxb_char_t *)sel, strlen(sel));
    if (list) {
        lxb_selectors_find(selectors, lxb_dom_interface_node(g_doc->dom_document.element),
                           list, qs_found, &q);
        lxb_css_selector_list_destroy_memory(list);
    }
    lxb_selectors_destroy(selectors, true);
    lxb_css_parser_destroy(parser, true);
    if (!all)
        r = element_wrap(ctx, q.first);
    return r;
}

static JSValue js_doc_query_selector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *sel;
    JSValue r;
    (void)this_val;
    DCHECK(g_doc != NULL, "querySelector ran before the document was installed");
    if (argc < 1) return JS_NULL;
    sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return JS_EXCEPTION;
    r = qs_run(ctx, sel, false);
    JS_FreeCString(ctx, sel);
    return r;
}

static JSValue js_doc_query_selector_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *sel;
    JSValue r;
    (void)this_val;
    DCHECK(g_doc != NULL, "querySelectorAll ran before the document was installed");
    if (argc < 1) return JS_NewArray(ctx);
    sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return JS_EXCEPTION;
    r = qs_run(ctx, sel, true);
    JS_FreeCString(ctx, sel);
    return r;
}

/* 4.5 getElementsByTagName. A pure Lexbor walk, no page code — and the one method testharness.js needs before it
   can decide its own timeout, which is why every WPT document stopped at it.
   IT RETURNS A STATIC ARRAY, not a live HTMLCollection. The spec's collection re-walks the tree on every read,
   so a page that inserts a matching element and re-reads `.length` sees the change; this does not. That is a
   fidelity gap named here rather than papered over — it is the same shape querySelectorAll already returns, and
   the live collection is its own component when a page needs one. */
static JSValue js_doc_get_elements_by_tag_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_collection_t *col;
    const char *name;
    JSValue arr;
    uint32_t k = 0;

    (void)this_val;
    DCHECK(g_doc != NULL, "getElementsByTagName ran before the document was installed");
    if (argc < 1) return JS_NewArray(ctx);
    name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    col = lxb_dom_collection_make(&g_doc->dom_document, 16);
    if (col) {
        lxb_dom_element_t *root = lxb_dom_interface_element(g_doc->dom_document.element);
        if (root && lxb_dom_elements_by_tag_name(root, col, (const lxb_char_t *)name, strlen(name)) == LXB_STATUS_OK)
            for (size_t i = 0; i < lxb_dom_collection_length(col); i++)
                JS_SetPropertyUint32(ctx, arr, k++, element_wrap(ctx, lxb_dom_collection_element(col, i)));
        lxb_dom_collection_destroy(col, true);
    }
    JS_FreeCString(ctx, name);
    return arr;
}

/* 4.5.1 createElement. The element is created IN this document and returned DETACHED — a page builds a subtree
   and attaches it later, and creating it already-attached would put nodes in the tree the page never inserted.
   It is not a per-flow write for that reason: nothing observable changed until appendChild, which IS one. */
static JSValue js_doc_create_element(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *tag;
    lxb_dom_element_t *el;
    JSValue r;

    (void)this_val;
    if (argc < 1) return JS_NULL;
    tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return JS_EXCEPTION;
    el = lxb_dom_document_create_element(lxb_dom_interface_document(g_doc),
                                         (const lxb_char_t *)tag, strlen(tag), NULL);
    JS_FreeCString(ctx, tag);
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
static JSValue js_doc_create_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *s;
    size_t len = 0;
    lxb_dom_text_t *t;

    (void)this_val;
    s = argc >= 1 ? JS_ToCStringLen(ctx, &len, argv[0]) : JS_ToCStringLen(ctx, &len, JS_UNDEFINED);
    if (!s) return JS_EXCEPTION;
    t = lxb_dom_document_create_text_node(lxb_dom_interface_document(g_doc), (const lxb_char_t *)s, len);
    JS_FreeCString(ctx, s);
    DCHECK(t != NULL, "createTextNode produced no node — a page building its DOM would silently build nothing");
    return node_wrap(ctx, lxb_dom_interface_node(t));
}

static JSValue js_doc_create_comment(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *s;
    size_t len = 0;
    lxb_dom_comment_t *c;

    (void)this_val;
    s = argc >= 1 ? JS_ToCStringLen(ctx, &len, argv[0]) : JS_ToCStringLen(ctx, &len, JS_UNDEFINED);
    if (!s) return JS_EXCEPTION;
    c = lxb_dom_document_create_comment(lxb_dom_interface_document(g_doc), (const lxb_char_t *)s, len);
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
static JSValue js_doc_create_element_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
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
        el = lxb_dom_element_create(lxb_dom_interface_document(g_doc),
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
static JSValue g_doc_obj = JS_UNDEFINED, g_win_obj = JS_UNDEFINED;

static void document_set_ready(JSContext *ctx, const char *state)
{
    if (JS_IsObject(g_doc_obj))
        JS_SetPropertyStr(ctx, g_doc_obj, "readyState", JS_NewString(ctx, state));
}

static int document_done_stage(JSContext *ctx, int stage)
{
    if (stage == 0) {
        document_set_ready(ctx, "interactive");
        return event_target_fire(ctx, g_doc_obj, "DOMContentLoaded", g_win_obj);
    }
    DCHECK(stage == 1, "the document lifecycle was asked for a stage it does not have");
    document_set_ready(ctx, "complete");
    return event_target_fire(ctx, g_win_obj, "load", JS_UNDEFINED);
}

void document_install(JSContext *ctx, JSValueConst global, lxb_html_document_t *dom, const char *url)
{
    JSValue doc;

    DCHECK(dom != NULL, "the Document install was handed no parsed document");
    if (!url || !*url)
        return;   /* no address, no Document — the page's own throw is the honest answer */

    doc = JS_NewObject(ctx);
    CHECK(!JS_IsException(doc), "the Document allocation failed");

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
    JS_SetPropertyStr(ctx, doc, "getElementById",
                      JS_NewCFunction(ctx, js_doc_get_element_by_id, "getElementById", 1));
    JS_SetPropertyStr(ctx, doc, "querySelector",
                      JS_NewCFunction(ctx, js_doc_query_selector, "querySelector", 1));
    JS_SetPropertyStr(ctx, doc, "querySelectorAll",
                      JS_NewCFunction(ctx, js_doc_query_selector_all, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, doc, "getElementsByTagName",
                      JS_NewCFunction(ctx, js_doc_get_elements_by_tag_name, "getElementsByTagName", 1));
    JS_SetPropertyStr(ctx, doc, "createElement",
                      JS_NewCFunction(ctx, js_doc_create_element, "createElement", 1));
    JS_SetPropertyStr(ctx, doc, "createTextNode",
                      JS_NewCFunction(ctx, js_doc_create_text, "createTextNode", 1));
    JS_SetPropertyStr(ctx, doc, "createComment",
                      JS_NewCFunction(ctx, js_doc_create_comment, "createComment", 1));
    JS_SetPropertyStr(ctx, doc, "createElementNS",
                      JS_NewCFunction(ctx, js_doc_create_element_ns, "createElementNS", 2));
    element_doc_set(dom);

    /* 3.1.1 documentElement / body / head — the three tree entry points every page starts from. Lexbor has
       already parsed them, so these are the REAL elements wrapped once (identity holds: `el.parentNode ===
       document.body` is a comparison pages make). A document with no body is a document this engine parsed
       without one, which the HTML parser does not produce — so it is an invariant, not a case to answer null for. */
    {
        lxb_dom_element_t *root = lxb_dom_interface_element(dom->dom_document.element);
        lxb_html_body_element_t *body = lxb_html_document_body_element(dom);
        lxb_html_head_element_t *head = lxb_html_document_head_element(dom);
        DCHECK(body != NULL, "the parsed document has no BODY element — HTML tree construction always creates "
                             "one, so a document without it is a parse this engine should not have accepted");
        JS_SetPropertyStr(ctx, doc, "documentElement", element_wrap(ctx, root));
        JS_SetPropertyStr(ctx, doc, "body", element_wrap(ctx, lxb_dom_interface_element(body)));
        JS_SetPropertyStr(ctx, doc, "head", element_wrap(ctx, lxb_dom_interface_element(head)));
    }

    /* A Document is an EventTarget, and "loading" until its scripts have run. */
    event_target_install(ctx, doc);
    JS_SetPropertyStr(ctx, doc, "readyState", JS_NewString(ctx, "loading"));

    JS_SetPropertyStr(ctx, (JSValue)global, "document", JS_DupValue(ctx, doc));
    g_doc_obj = doc;                                  /* owned by the global; borrowed here for the lifecycle */
    g_win_obj = JS_DupValue(ctx, global);
    engine_set_document_done_hook(document_done_stage);
}
