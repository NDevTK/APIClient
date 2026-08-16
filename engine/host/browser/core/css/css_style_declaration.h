/* CSSOM — CSSStyleDeclaration, `element.style` and `getComputedStyle()`. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_STYLE_DECLARATION_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_STYLE_DECLARATION_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void cssom_init(JSContext *ctx);
/* THE CASCADE'S WINNER for `name` on `el` — inline, then this flow's author rules, then the UA sheet, then the
   property's initial value — as text. It is the SPECIFIED value: the declaration that won, before the
   property's own `Computed value:` line has been applied to it, which is core/css/css_computed_value.h's job
   and is who this exists for. OWNED: the caller frees. NULL only for a property no layer declares and that has
   no initial value in lexbor's registry (a custom property nobody set). */
char *cssom_cascaded_value(lxb_dom_element_t *el, const char *name);
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
