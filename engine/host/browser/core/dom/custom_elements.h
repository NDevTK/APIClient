/* Custom elements — Blink core/html/custom (CustomElementRegistry). `customElements.define(name,ctor)` records
 * the class; `createElement(name)` (document.c/main.c) calls ce_upgrade to UPGRADE a defined tag to a real
 * ctor-prototype instance so its connectedCallback etc. drive with `this`=the element. Upgraded instances are
 * RETAINED (g_ce_instances) so the scheduler's JS_FindReceiver can drive their lifecycle callbacks even though
 * the Lexbor DOM holds only the node. See custom_elements.c. */
#ifndef ENGINE_HOST_BROWSER_CUSTOM_ELEMENTS_H
#define ENGINE_HOST_BROWSER_CUSTOM_ELEMENTS_H
#include "quickjs.h"
#include <lexbor/dom/dom.h>
extern JSValue g_ce_registry;    /* {tagName -> ctor} */
extern JSValue g_ce_instances;   /* retained upgraded instances (JS_FindReceiver scans them) */
JSValue js_ce_define(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);   /* customElements.define */
JSValue ce_upgrade(JSContext *ctx, lxb_dom_element_t *el, const char *tag);   /* upgrade a defined tag, else el_wrap(el) */
void ce_init(JSContext *ctx);    /* create the registry + instance list */
void ce_free(JSContext *ctx);    /* teardown */
/* Install the DOM interface base constructors (EventTarget..SVGElement, for `class X extends HTMLElement`) +
   the customElements registry onto the global; the bases chain to el_proto (the Element method prototype). */
void install_dom_interface_ctors(JSContext *ctx, JSValueConst g, JSValueConst el_proto);
#endif
