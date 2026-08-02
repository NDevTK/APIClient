/* The Document interface — Blink core/dom. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_H
#include <lexbor/html/html.h>
#include "quickjs.h"

/* Install `document` for `dom`, addressed at `url`. Only the members this engine can answer TRUTHFULLY are
   installed; the tree-walking half is absent until Element exists, because a querySelector that answers null
   for an element the document HAS is a lie, and a lie is worse than a ReferenceError that names the gap. */
void document_install(JSContext *ctx, JSValueConst global, lxb_html_document_t *dom, const char *url);

/* The ParentNode mixin's ONE selector engine (§4.2.6), scoped to any root — Document, Element and
   DocumentFragment differ only in what they scope to. `all` != 0 returns an array, else the first match or null. */
JSValue document_qs_run(JSContext *ctx, lxb_dom_node_t *root, const char *sel, int all);

#endif
