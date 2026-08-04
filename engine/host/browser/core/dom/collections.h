/* NodeList and HTMLCollection — DOM §4.2.10 and §4.2.11. See collections.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_COLLECTIONS_H
#define ENGINE_HOST_BROWSER_CORE_DOM_COLLECTIONS_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void collections_init(JSContext *ctx);
void collections_free(JSContext *ctx);
/* `NodeList` and `HTMLCollection` as globals. */
void collections_install(JSContext *ctx, JSValueConst global);

/* §4.4 `childNodes` — a LIVE NodeList over every child, cached on the owner's wrapper because the IDL says
   [SameObject]. `owner` is the node's WRAPPER, which is what the collection holds on to. */
JSValue collections_child_nodes(JSContext *ctx, JSValueConst owner);
/* §4.2.6 `children` — a LIVE HTMLCollection over the ELEMENT children, [SameObject] the same way. */
JSValue collections_children(JSContext *ctx, JSValueConst owner);
/* §4.2.6 `querySelectorAll` — a STATIC NodeList. Not [SameObject]: the spec returns a new one each call, and
   it does not track the tree afterwards, which is the whole difference from the two above. `nodes` is an array
   of wrappers this takes ownership of. */
JSValue collections_static(JSContext *ctx, JSValue nodes);

#endif
