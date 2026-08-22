/* DOMTokenList — DOM §7.1, and `Element.classList`, the reflection that made it necessary. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOM_TOKEN_LIST_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOM_TOKEN_LIST_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void dom_token_list_init(JSContext *ctx);
/* §7.1's prototype for ONE realm — declared into core/realm.h's list, run once per realm. */
void dom_token_list_install_proto(JSContext *ctx);
/* PER REALM. OWNED: the caller frees. */
JSValue dom_token_list_proto(JSContext *ctx);
void dom_token_list_free(JSRuntime *rt);
/* `DOMTokenList` as a global — the interface object and its prototype. */
void dom_token_list_install(JSContext *ctx, JSValueConst global);
/* Install `classList` on Element.prototype — §4.9 puts it there and nowhere else. */
void dom_token_list_install_element(JSContext *ctx, JSValueConst element_proto);
/* Install ONE `[SameObject] readonly attribute DOMTokenList` reflection on the interface prototype that
   DECLARES it — `relList` on HTMLAnchorElement/HTMLAreaElement/HTMLLinkElement/HTMLFormElement, `sizes` on
   HTMLLinkElement, `sandbox` on HTMLIFrameElement. The member NAME is what a caller names, because the content
   attribute it views is this component's to know and a caller that passed one could pass the wrong one; a name
   §7.1 does not define is a DFAIL rather than a list over an attribute nothing writes. */
void dom_token_list_install_reflection(JSContext *ctx, JSValueConst proto, const char *member);

#endif
