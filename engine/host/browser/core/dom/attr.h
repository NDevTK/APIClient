/* The Attr interface (DOM §4.9.2) and the NamedNodeMap (§4.9.1) that holds them. See attr.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ATTR_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ATTR_H
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

#endif
