/* The Document interface — Blink core/dom. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_H
#include <lexbor/html/html.h>
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/frame/policy_container.h"

/* Install `document` for `dom`, addressed at `url`. THE REALM IS THE DOCUMENT: `ctx` is what this document's
   state hangs off from here on, so a second same-origin document in the same agent is a second JSContext in the
   same JSRuntime and not a second instance. Only the members this engine can answer TRUTHFULLY are
   installed; the tree-walking half is absent until Element exists, because a querySelector that answers null
   for an element the document HAS is a lie, and a lie is worse than a ReferenceError that names the gap. */
void document_install(JSContext *ctx, JSValueConst global, lxb_html_document_t *dom, const char *url);
/* §4.4 baseURI's answer: the document's address. ONE component owns what this document's URL is — two answers
   to that question is how they drift apart. */
const char *document_base_url(JSContext *ctx);
/* The parsed document's root node, for a component that walks the whole tree. NULL in a realm that has no
   document yet, which `window.document` is entitled to see. */
lxb_dom_node_t *document_root_node(JSContext *ctx);

/* CSP §3.3's `<meta http-equiv="Content-Security-Policy">`: the policy container a parsed tree declares for
   itself. A REAL LEXBOR WALK rather than a regex over the source, for the reason the bundle id is a `<script>`
   scan — a `content` attribute is parsed markup by the time it is here, so entity decoding and quoting are the
   parser's answer rather than a second one. It lives on the DOM half because only that half may walk a tree,
   and it RETURNS the container rather than installing one so it is exercisable with a document of its own.
   OWNED by the caller. */
PolicyContainer *document_meta_policy(lxb_html_document_t *dom);

/* THIS DOCUMENT'S POLICY CONTAINER — HTML §7.2.6, built at install from the above. Built at install rather
   than on demand because §7.4 clones it for an about:blank child at the moment that child is created, which
   may be before anything else has asked. BORROWED. */
const PolicyContainer *document_policy(JSContext *ctx);
/* Release what the component HOLDS across the document's lifecycle — the window it fires `load` at. */
void document_free(JSContext *ctx);

/* §4.2.6/§4.9's FOUR selector members as ONE declaration — querySelector, querySelectorAll, matches, closest.
   They differ in where the cursor goes and what a match yields, which is the magic: 0 = querySelector,
   1 = querySelectorAll, 2 = matches, 3 = closest. Installed by whoever owns the interface the IDL puts them on;
   the compiled selector and the walk live here, where the selector engine already did. */
const IdlStepDecl *document_qs_decl(void);

#endif
