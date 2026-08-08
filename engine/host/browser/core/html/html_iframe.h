/* HTMLIFrameElement's navigable — HTML §4.8.5. See html_iframe.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_IFRAME_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_IFRAME_H

#include "quickjs.h"

void iframe_init(JSContext *ctx);
/* Install §4.8.5's `contentWindow` on HTMLIFrameElement's prototype. */
void iframe_install(JSContext *ctx, JSValueConst proto);
/* Does this iframe have a navigable IN THE RUNNING FLOW? Kept on the wrapper, so the heap COW delta isolates
   it: a sibling that never inserted the frame has none. */
bool iframe_has_navigable(JSContext *ctx, JSValueConst wrapper);
/* §4.8.5's create-a-child-navigable, run from the insertion-steps walk exactly where the spec puts it. It does
   not suspend and cannot: the child's document name is minted locally and the host is notified, so there is
   nothing to wait for — which is what makes `frame.contentWindow` answer on the line after the append. Calling
   it for an element that already has one in this flow is a no-op. */
void iframe_create_navigable(JSContext *ctx, JSValueConst wrapper);
/* §4.8.5's removing steps: DESTROY the child navigable. The element loses it (contentWindow goes null) and the
   proxy a page is still holding reports `closed`. A no-op for an element this flow never gave one. */
void iframe_destroy_navigable(JSContext *ctx, JSValueConst wrapper);
/* §7.2.5's DOCUMENT-TREE CHILD NAVIGABLES, in tree order: what `window.length` counts and `window[i]` indexes.
   Walked from the document tree on every ask, because the set changes with every insertion, removal and
   reparent — and both the tree and the navigables are per-flow, so the answer is this flow's. The nth is
   JS_UNDEFINED when there is no nth. */
int     iframe_child_navigable_count(JSContext *ctx);
JSValue iframe_child_navigable(JSContext *ctx, int index);

void iframe_free(JSContext *ctx);

#endif
