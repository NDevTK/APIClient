/* CSSOM — CSSStyleDeclaration, `element.style` and `getComputedStyle()`. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_STYLE_DECLARATION_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_STYLE_DECLARATION_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void cssom_init(JSContext *ctx);
/* CSSOM §6.7's prototype for ONE realm — declared into core/realm.h's list, run once per realm. */
void cssom_install_proto(JSContext *ctx);
/* PER REALM. OWNED: the caller frees. */
JSValue cssom_proto(JSContext *ctx);
void cssom_free(JSContext *ctx);
/* `CSSStyleDeclaration` as a global, and `getComputedStyle` on the one the Window IDL puts it on. */
void cssom_install(JSContext *ctx, JSValueConst global);
/* HTMLElement's `[SameObject] attribute CSSStyleDeclaration style` — installed by the html layer, because the
   attribute is HTMLElement's and the object is this component's. */
void cssom_install_style_attribute(JSContext *ctx, JSValueConst proto);

#endif
