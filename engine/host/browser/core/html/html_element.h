/* HTMLElement and the PER-TAG interfaces — HTML §3.2.2 and §4's element-interface table. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_ELEMENT_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

/* Build HTMLElement.prototype on Element.prototype, then every per-tag interface on top of it. Called by
   element_init, because the HTML layer is built ON the DOM layer and there is no order in which it is not. */
void html_element_init(JSContext *ctx);
/* §3.2.2 and §4's prototypes for ONE realm — declared into core/realm.h's list. */
void html_element_install_protos(JSContext *ctx);
void html_element_free(JSContext *ctx);
/* §3.2.2's HTMLElement.prototype FOR THIS REALM. OWNED: the caller frees. HTML §4.13.2 step 9 needs it — a
   custom element constructor whose NewTarget carries a non-object `prototype` gets the interface prototype
   object of that constructor's realm, so the answer is a realm's and never a static. */
JSValue html_element_proto(JSContext *ctx);
/* §4's HTMLUnknownElement.prototype FOR THIS REALM. OWNED. DOM §4.9 step 5.1.4's failure arm names the
   interface by name — "create an element internal given document, HTMLUnknownElement, localName, …" — so a
   custom element whose constructor threw is an HTMLUnknownElement with that local name, which is what makes
   `el instanceof HTMLUnknownElement` the page's way to see that the upgrade failed. */
JSValue html_unknown_element_proto(JSContext *ctx);
/* The interface OBJECTS as globals — `HTMLElement`, `HTMLAnchorElement`, … Separate from the prototypes because
   they need a global to hang off, which the document install has and this does not. */
void html_element_install(JSContext *ctx, JSValueConst global);

#endif
