/* The Attr interface (DOM §4.9.2) and the NamedNodeMap (§4.9.1) that holds them. See attr.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ATTR_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ATTR_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void attr_init(JSContext *ctx);
/* §4.9's two prototypes for ONE realm — declared into core/realm.h's list. */
void attr_install_protos(JSContext *ctx);
/* PER REALM. OWNED: the caller frees. */
JSValue named_node_map_proto(JSContext *ctx);
void attr_install(JSContext *ctx, JSValueConst global);
void attr_free(JSContext *ctx);
/* A NamedNodeMap over `owner`'s attributes. The caller caches it on the element's wrapper — §4.9's
   `[SameObject] readonly attribute NamedNodeMap attributes`. */
JSValue attr_named_node_map_new(JSContext *ctx, JSValueConst owner);

/* §4.9's "GET AN ATTRIBUTE BY NAME", as one implementation. §4.9.1's `getNamedItem` and §4.9's
   `getAttributeNode` are the SAME algorithm reached through two objects, and written twice they are two rules
   about what a qualified name matches. JS_UNDEFINED when there is none; the callers turn that into their own
   interface's null. */
JSValue attr_by_name(JSContext *ctx, lxb_dom_element_t *el, const char *name);

#endif
