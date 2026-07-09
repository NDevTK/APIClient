/* The DOM Element JSClass + el_wrap — the foundation the element methods (js_el_*) hang off.  See
 * dom_element.c.  The element methods themselves still live in main.c and migrate into this TU incrementally;
 * they and dom_select.c / forms.c share the class id + wrapper declared here (all already un-static). */
#ifndef ENGINE_HOST_DOM_ELEMENT_H
#define ENGINE_HOST_DOM_ELEMENT_H
#include "quickjs.h"
#include <lexbor/dom/dom.h>

extern JSClassID g_el_class_id;                              /* the "Element" JSClass id (lexbor owns the nodes) */
JSValue el_wrap(JSContext *ctx, lxb_dom_element_t *el);      /* wrap a real Lexbor element as a JS Element (NULL -> null) */

#endif
