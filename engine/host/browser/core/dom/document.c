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
#include "core/dom/document.h"
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
    JS_SetPropertyStr(ctx, doc, "createElement",
                      JS_NewCFunction(ctx, js_doc_create_element, "createElement", 1));
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

    JS_SetPropertyStr(ctx, (JSValue)global, "document", doc);
}
