/* The Element interface — Blink core/dom. One JS object per Lexbor element, per document. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void    element_init(JSContext *ctx);
void    element_free(JSContext *ctx);

/* The wrapper for `el`, or JS_NULL. The SAME Lexbor element always yields the SAME JS object: a page compares
   nodes by identity constantly, and a fresh wrapper per lookup makes every such comparison silently false. */
JSValue element_wrap(JSContext *ctx, lxb_dom_element_t *el);

#endif
