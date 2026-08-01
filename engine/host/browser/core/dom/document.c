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
    element_doc_set(dom);

    JS_SetPropertyStr(ctx, (JSValue)global, "document", doc);
}
