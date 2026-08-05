/* The Document interface — Blink core/dom. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_H
#include <lexbor/html/html.h>
#include "quickjs.h"
#include "core/idl_args.h"

/* Install `document` for `dom`, addressed at `url`. Only the members this engine can answer TRUTHFULLY are
   installed; the tree-walking half is absent until Element exists, because a querySelector that answers null
   for an element the document HAS is a lie, and a lie is worse than a ReferenceError that names the gap. */
void document_install(JSContext *ctx, JSValueConst global, lxb_html_document_t *dom, const char *url);
/* §4.4 baseURI's answer: the document's address. ONE component owns what this document's URL is — two answers
   to that question is how they drift apart. */
const char *document_base_url(void);
/* The parsed document's root node, for a component that walks the whole tree. */
lxb_dom_node_t *document_root_node(void);
/* Release what the component HOLDS across the document's lifecycle — the window it fires `load` at. */
void document_free(JSContext *ctx);

/* §4.2.6/§4.9's FOUR selector members as ONE declaration — querySelector, querySelectorAll, matches, closest.
   They differ in where the cursor goes and what a match yields, which is the magic: 0 = querySelector,
   1 = querySelectorAll, 2 = matches, 3 = closest. Installed by whoever owns the interface the IDL puts them on;
   the compiled selector and the walk live here, where the selector engine already did. */
const IdlStepDecl *document_qs_decl(void);

#endif
