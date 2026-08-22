/* NodeList and HTMLCollection — DOM §4.2.10 and §4.2.11. See collections.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_COLLECTIONS_H
#define ENGINE_HOST_BROWSER_CORE_DOM_COLLECTIONS_H
#include <lexbor/dom/dom.h>
#include <stdbool.h>
#include "quickjs.h"

void collections_init(JSContext *ctx);
/* §4.2.10's two prototypes for ONE realm — declared into core/realm.h's list, run once per realm. */
void collections_install_protos(JSContext *ctx);
/* PER REALM. OWNED: the caller frees. */
JSValue nodelist_proto(JSContext *ctx);
JSValue htmlcollection_proto(JSContext *ctx);
void collections_free(JSRuntime *rt);
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

/* §4.5/§4.9's two by-name collections, LIVE over `owner`'s subtree. A new one per call — the spec does not
   mark these [SameObject], because the query is part of what the collection is. */
JSValue collections_by_name(JSContext *ctx, JSValueConst owner, const char *name, bool by_class);

/* §4.5's OTHER by-name algorithm: "list of elements with namespace `ns` and local name `local`", LIVE over
   `owner`'s subtree. `*` means any in EITHER position independently, and `ns` NULL is the null namespace —
   a real query matching an element in no namespace, which is why it is not spelled as an empty string. */
JSValue collections_by_tag_ns(JSContext *ctx, JSValueConst owner, const char *ns, const char *local);

/* HTML §7.3.3's NAMED ELEMENTS, live over `owner`'s subtree: any HTML element whose `id` is `name`, plus
   embed/form/img/object/iframe whose `name` attribute is. It is what named access on the Window answers with
   when more than one element carries the name — the spec returns an HTMLCollection there, not the first match,
   because a page reads `.length` off it. */
JSValue collections_named(JSContext *ctx, JSValueConst owner, const char *name);

/* §3.1.5 `document.links` — `a`/`area` elements that HAVE an href. A predicate, not a name. */
JSValue collections_links(JSContext *ctx, JSValueConst owner);

#endif
