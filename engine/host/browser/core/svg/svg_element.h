/* SVG 2 §4.4.1 Interface SVGElement. See svg_element.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_SVG_SVG_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_SVG_SVG_ELEMENT_H
#include "quickjs.h"

/* Declared once per agent from element_init, after core/html/html_element.c (whose mixin installer the
   prototype build calls), and released from element_free before it. */
void svg_element_init(JSContext *ctx);
void svg_element_install_protos(JSContext *ctx);   /* SVGElement.prototype, for ONE realm */
void svg_element_install(JSContext *ctx, JSValueConst global);
void svg_element_free(JSRuntime *rt);

#endif
