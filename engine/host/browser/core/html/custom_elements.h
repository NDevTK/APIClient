/* CUSTOM ELEMENTS — HTML §4.13: the registry, the upgrade, and the lifecycle reactions. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_CUSTOM_ELEMENTS_H
#define ENGINE_HOST_BROWSER_CORE_HTML_CUSTOM_ELEMENTS_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void custom_elements_init(JSContext *ctx);
void custom_elements_free(JSContext *ctx);
/* `window.customElements` — §4.13.4's CustomElementRegistry. */
void custom_elements_install(JSContext *ctx, JSValueConst global);

/* §4.13.3 "try to upgrade": called when an element enters the tree. A no-op unless its local name is defined
   and it has not been upgraded already. */
void custom_elements_try_upgrade(JSContext *ctx, lxb_dom_element_t *el);
/* §4.13.3's disconnected reaction — the twin of the upgrade, for an element LEAVING a document. A no-op unless
   the element was upgraded, because only an upgraded element has a lifecycle to react with. */
void custom_elements_disconnected(JSContext *ctx, lxb_dom_element_t *el);

#endif
