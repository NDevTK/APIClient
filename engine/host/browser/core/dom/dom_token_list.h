/* DOMTokenList — DOM §7.1, and `Element.classList`, the reflection that made it necessary. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOM_TOKEN_LIST_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOM_TOKEN_LIST_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void dom_token_list_init(JSContext *ctx);
void dom_token_list_free(JSContext *ctx);
/* `DOMTokenList` as a global — the interface object and its prototype. */
void dom_token_list_install(JSContext *ctx, JSValueConst global);
/* Install `classList` (and the other [SameObject] token-list reflections) on Element.prototype. */
void dom_token_list_install_element(JSContext *ctx, JSValueConst element_proto);

#endif
