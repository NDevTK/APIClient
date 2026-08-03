/* HTMLElement and the PER-TAG interfaces — HTML §3.2.2 and §4's element-interface table. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_ELEMENT_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

/* Build HTMLElement.prototype on Element.prototype, then every per-tag interface on top of it. Called by
   element_init, because the HTML layer is built ON the DOM layer and there is no order in which it is not. */
void html_element_init(JSContext *ctx);
void html_element_free(JSContext *ctx);
/* The interface OBJECTS as globals — `HTMLElement`, `HTMLAnchorElement`, … Separate from the prototypes because
   they need a global to hang off, which the document install has and this does not. */
void html_element_install(JSContext *ctx, JSValueConst global);

#endif
