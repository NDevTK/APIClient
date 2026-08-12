/* DOMStringMap — HTML §3.2.2's `dataset`. See dom_string_map.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_DOM_STRING_MAP_H
#define ENGINE_HOST_BROWSER_CORE_HTML_DOM_STRING_MAP_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void dom_string_map_init(JSContext *ctx);
void dom_string_map_install_proto(JSContext *ctx);   /* §3.2.9's prototype, for ONE realm */
void dom_string_map_install(JSContext *ctx, JSValueConst global);
void dom_string_map_free(JSContext *ctx);
/* A new map over `el`'s data-* attributes. The caller caches it on the element's wrapper — `dataset` is
   [SameObject], so `el.dataset === el.dataset` is what the IDL states. */
JSValue dom_string_map_new(JSContext *ctx, lxb_dom_element_t *el);

#endif
