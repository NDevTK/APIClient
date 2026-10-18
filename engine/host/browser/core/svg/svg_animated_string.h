/* SVG 2 §4.6.7 Interface SVGAnimatedString. See svg_animated_string.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_SVG_SVG_ANIMATED_STRING_H
#define ENGINE_HOST_BROWSER_CORE_SVG_SVG_ANIMATED_STRING_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void svg_animated_string_init(JSContext *ctx);
void svg_animated_string_install_proto(JSContext *ctx);   /* the interface prototype object, for ONE realm */
void svg_animated_string_install(JSContext *ctx, JSValueConst global);
void svg_animated_string_free(void);
/* A new SVGAnimatedString reflecting `el`'s content attribute `attr` (a name the engine owns, static storage).
   The caller caches it on the element's wrapper, since every attribute of this type is [SameObject]. */
JSValue svg_animated_string_new(JSContext *ctx, lxb_dom_element_t *el, const char *attr);

#endif
