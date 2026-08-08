/* The Document interface — Blink core/dom. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_H
#include <lexbor/html/html.h>
#include <stdint.h>

#include "quickjs.h"
#include "core/idl_args.h"
#include "core/frame/policy_container.h"

/* Install `document` for `dom`, addressed at `url`. THE REALM IS THE DOCUMENT: `ctx` is what this document's
   state hangs off from here on, so a second same-origin document in the same agent is a second JSContext in the
   same JSRuntime and not a second instance. Only the members this engine can answer TRUTHFULLY are
   installed; the tree-walking half is absent until Element exists, because a querySelector that answers null
   for an element the document HAS is a lie, and a lie is worse than a ReferenceError that names the gap. */
/* THE AGENT'S HALF: Document.prototype, its members and its mixins. A member is declared once per agent; a
   second realm installs `document` from the same declarations. */
void document_init(JSContext *ctx);

/* `nav_proxy` is §7.2.5.1's ONE WindowProxy for the navigable this realm is the active document of. The
   navigable exists before its realm — §7.4 created it and handed its proxy to the page — so the caller that
   owns it passes it in rather than a second one being minted here. */
/* `csp` is the policy text this document was CREATED with — the `Content-Security-Policy` the response carried,
   or §7.4's clone of the creator's for a document that came from no response (`about:blank`). NULL when there
   is neither. It is not an optional extra: CSP §3.3's `<meta>` is only HALF of §7.2.6's container, and a
   document whose created-with policy is dropped reports a sink as exploitable that the real page's CSP kills. */
void document_install(JSContext *ctx, JSValueConst global, lxb_html_document_t *dom, const char *url,
                      const char *csp, uint32_t doc_id, JSValueConst nav_proxy);

/* WHICH DOCUMENT THIS REALM IS, in the world registry's naming. §7.4 mints a child's name from it, so a
   same-origin child of a child is named from the child and not from the instance root. */
uint32_t document_doc(JSContext *ctx);

/* §7.2.5.1's ONE WindowProxy for THIS realm's navigable — what `window.closed` reads the navigable's state
   through and what every message this document posts carries as `source`. BORROWED. It lives on the realm
   because it is one PER realm; a registry keyed by document would be an immortal root holding a proxy for
   every navigable a forced-execution frontier ever created. */
JSValueConst document_window_proxy(JSContext *ctx);

/* THIS REALM'S `document` OBJECT. BORROWED. §4.8.5's `contentDocument` for a SAME-ORIGIN child answers with
   exactly this object out of the child's realm — the two documents are one agent, so it is a pointer and not
   a message. */
JSValueConst document_object(JSContext *ctx);
/* §4.4 baseURI's answer: the document's address. ONE component owns what this document's URL is — two answers
   to that question is how they drift apart. */
const char *document_base_url(JSContext *ctx);
/* The parsed document's root node, for a component that walks the whole tree. NULL in a realm that has no
   document yet, which `window.document` is entitled to see. */
lxb_dom_node_t *document_root_node(JSContext *ctx);

/* HTML §7.2.6's container for one document, from BOTH of its sources: the policy it was created with (above)
   and CSP §3.3's `<meta http-equiv="Content-Security-Policy">` the parsed tree carries. They are ONE POLICY
   LIST — every policy in the list is enforced and CSP §2.2 serializes a list with commas — so joining them
   cannot lose the narrower one, which a `;` join would (a repeated directive in one policy is ignored).
   The meta half is a REAL LEXBOR WALK rather than a regex over the source, for the reason the bundle id is a
   `<script>` scan: a `content` attribute is parsed markup by the time it is here, so entity decoding and
   quoting are the parser's answer rather than a second one. It lives on the DOM half because only that half
   may walk a tree, and it RETURNS the container rather than installing one so it is exercisable with a
   document of its own. `csp` may be NULL. OWNED by the caller. */
PolicyContainer *document_policy_new(lxb_html_document_t *dom, const char *csp);

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
