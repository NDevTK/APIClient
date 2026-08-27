/* HTMLIFrameElement's navigable — HTML §4.8.5. See html_iframe.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_IFRAME_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_IFRAME_H

#include <lexbor/dom/dom.h>
#include <stdbool.h>

#include "quickjs.h"

void iframe_init(JSContext *ctx);
/* IS THIS NODE AN `<iframe>` — the question §4.8.5's insertion steps, its removing steps, §7.2.2.2's child
   navigables and §7.5.8's container branch each have to ask before they may act on an element. ASCII
   case-insensitive over the qualified name, because a parsed `<iframe>` and a `createElement('IFRAME')` are
   the same element and must not be told apart by how their name happened to be stored.
   IT IS ONE PREDICATE BECAUSE IT IS ONE QUESTION. It was four copies of the same `qn == 6 && !strncasecmp`,
   in two files, and four copies of a comparison are four places for the next element type that gets a content
   navigable — §4.8.6's `<embed>` and §4.8.7's `<object>` — to be added to three of. */
bool iframe_element_is(const lxb_dom_node_t *n);
/* Install §4.8.5's `contentWindow` on HTMLIFrameElement's prototype. */
/* Declared once per AGENT; iframe_install then names the cached ids for each realm's prototype. */
void iframe_declare(JSContext *ctx);
void iframe_install(JSContext *ctx, JSValueConst proto);
/* Does this iframe have a navigable IN THE RUNNING FLOW? Kept on the wrapper, so the heap COW delta isolates
   it: a sibling that never inserted the frame has none. */
bool iframe_has_navigable(JSContext *ctx, JSValueConst wrapper);
/* THIS FLOW'S CHILD NAVIGABLE for that element — its WindowProxy, or JS_UNDEFINED. Owned.
   IT IS NOT `contentWindow`. §4.8.5's attribute is an IDL ACCESSOR, and an engine walk that read it would be
   running a getter from a C activation — which has no flow base under it, so a body that loops drives to
   completion. §7.3.3's named-access walk did exactly that and aborted three spec files. The navigable is a
   slot on the wrapper; asking the component that owns the slot runs no page code by construction. */
JSValue iframe_navigable(JSContext *ctx, JSValueConst wrapper);
/* §4.8.5's create-a-child-navigable, run from the insertion-steps walk exactly where the spec puts it. It does
   not suspend and cannot: the child's document name is minted locally and the host is notified, so there is
   nothing to wait for — which is what makes `frame.contentWindow` answer on the line after the append. Calling
   it for an element that already has one in this flow is a no-op. */
void iframe_create_navigable(JSContext *ctx, JSValueConst wrapper);
/* §4.8.5's removing steps: DESTROY the child navigable. The element loses it (contentWindow goes null) and the
   proxy a page is still holding reports `closed`. A no-op for an element this flow never gave one. */
void iframe_destroy_navigable(JSContext *ctx, JSValueConst wrapper);
/* HTML §4.8.5's "RUN THE IFRAME LOAD EVENT STEPS, given an iframe element element" — the algorithm that makes
   `frame.onload` a thing that happens. Its one caller is §7.5.8's completely-finish-loading, which is §13.2.7
   "The end" step 9.12 of the CHILD's own document (core/dom/document.c).
   `ctx` IS THE CONTAINER'S REALM AND NOT THE CHILD'S. §7.5.8 queues an ELEMENT task, whose global is the
   element's node document's relevant global — the parent's — and the child's document is the one whose loading
   just finished. Handing the child's realm here would enqueue the parent's listeners onto the child's queue. */
void iframe_run_load_event_steps(JSContext *ctx, JSValueConst wrapper);
/* §4.8.5 for the iframes the PARSER inserted: a browser runs the insertion steps during tree construction, and
   this engine's tree comes from a parse that does not pass through the DOM chokepoint. Run once, when the
   document is installed; anything a script appends afterwards goes through the chokepoint instead. */
void iframe_document_parsed(JSContext *ctx);

/* §7.2.2.2's DOCUMENT-TREE CHILD NAVIGABLES, in tree order: what `window.length` counts and `window[i]` indexes.
   Walked from the document tree on every ask, because the set changes with every insertion, removal and
   reparent — and both the tree and the navigables are per-flow, so the answer is this flow's. The nth is
   JS_UNDEFINED when there is no nth. */
int     iframe_child_navigable_count(JSContext *ctx);
JSValue iframe_child_navigable(JSContext *ctx, int index);

void iframe_free(JSRuntime *rt);

#endif
