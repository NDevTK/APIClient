/* Custom elements — see custom_elements.h. Extracted from main.c (Blink core/html/custom). */
#include "core/dom/custom_elements.h"
#include "core/dom/dom_element.h"   /* g_el_class_id, el_wrap */
#include "solver/concolic.h"   /* js_concolic_stub / js_noop — customElements.get/whenDefined/upgrade */

JSValue g_ce_registry = JS_UNDEFINED;   /* {tagName -> ctor}; createElement upgrades a defined tag */
JSValue g_ce_instances = JS_UNDEFINED;  /* retained upgraded instances (findable receivers) */

/* customElements.define(name, ctor): record the class so createElement(name) UPGRADES to a real instance
   (ctor.prototype chain + el-backed) — the browser's upgrade, which is what makes `this.attachShadow`/
   `this.getAttribute` work inside a lifecycle callback. */
JSValue js_ce_define(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 2 && JS_IsString(argv[0]) && JS_IsFunction(ctx, argv[1]) && JS_IsObject(g_ce_registry)) {
        const char *nm = JS_ToCString(ctx, argv[0]);
        if (nm) { JS_SetPropertyStr(ctx, g_ce_registry, nm, JS_DupValue(ctx, argv[1])); JS_FreeCString(ctx, nm); }
    }
    return JS_UNDEFINED;
}

/* Upgrade a DEFINED custom element: the instance's proto = ctor.prototype (so connectedCallback etc. are reached
   with this=the element) AND it is el-backed (g_el_class_id + opaque el, so this.getAttribute/attachShadow work
   against the real element). Retain it so JS_FindReceiver drives connectedCallback with THIS instance. Not
   defined -> el_wrap(el). The caller owns `tag` (not freed here). */
JSValue ce_upgrade(JSContext *ctx, lxb_dom_element_t *el, const char *tag) {
    JSValue ctor = JS_IsObject(g_ce_registry) ? JS_GetPropertyStr(ctx, g_ce_registry, tag) : JS_UNDEFINED;
    if (el && JS_IsFunction(ctx, ctor)) {
        JSValue cproto = JS_GetPropertyStr(ctx, ctor, "prototype");
        if (JS_IsObject(cproto)) {
            JSValue o = JS_NewObjectProtoClass(ctx, cproto, g_el_class_id);
            JS_FreeValue(ctx, cproto); JS_FreeValue(ctx, ctor);
            if (JS_IsObject(o)) {
                JS_SetOpaque(o, el);
                if (JS_IsArray(g_ce_instances)) {   /* retain so JS_FindReceiver drives connectedCallback with THIS instance */
                    uint32_t n = 0; JSValue lv = JS_GetPropertyStr(ctx, g_ce_instances, "length"); JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
                    JS_SetPropertyUint32(ctx, g_ce_instances, n, JS_DupValue(ctx, o));
                }
                return o;
            }
            JS_FreeValue(ctx, o); return el_wrap(ctx, el);
        }
        JS_FreeValue(ctx, cproto);
    }
    JS_FreeValue(ctx, ctor);
    return el_wrap(ctx, el);
}

void ce_init(JSContext *ctx) { g_ce_registry = JS_NewObject(ctx); g_ce_instances = JS_NewArray(ctx); }
void ce_free(JSContext *ctx) {
    JS_FreeValue(ctx, g_ce_registry); g_ce_registry = JS_UNDEFINED;
    JS_FreeValue(ctx, g_ce_instances); g_ce_instances = JS_UNDEFINED;
}

/* A CONSTRUCTABLE DOM interface base so `class X extends HTMLElement {}` DEFINES (else it throws and the whole
   Web Component — with its connectedCallback endpoints/sinks — is lost). super() returns the default derived
   `this`; a custom element IS an HTMLElement, so the base prototype chains to the element method proto
   (el_proto), giving `this.attachShadow`/`this.getAttribute` inside a lifecycle callback. */
static JSValue js_ctor_stub(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) { (void)ctx; (void)nt; (void)argc; (void)argv; return JS_UNDEFINED; }
static void def_ctor(JSContext *ctx, JSValueConst g, const char *name, JSValueConst el_proto) {
    JSValue c = JS_NewCFunction2(ctx, js_ctor_stub, name, 0, JS_CFUNC_constructor, 0);
    JSValue proto = JS_NewObject(ctx);
    if (!JS_IsUndefined(el_proto)) JS_SetPrototype(ctx, proto, el_proto);
    JS_SetConstructor(ctx, c, proto);   /* c.prototype = proto (an OBJECT) so `class X extends <c>` is valid */
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)g, name, c);
}

/* Install the DOM interface base constructors (EventTarget..SVGElement) + the customElements registry onto the
   global. Called by main.c after the element proto exists. customElements.define is effectively a no-op — the
   ctor's methods are already reachable + orphan-driven; get/whenDefined are opaque, upgrade a no-op. */
void install_dom_interface_ctors(JSContext *ctx, JSValueConst g, JSValueConst el_proto) {
    /* EventTarget + Node are owned by event_target.c / node.c (the DOM inheritance spine), installed before this
       — do NOT redefine them here or the flat el_proto-based ctor overwrites the real window.EventTarget/Node and
       breaks `x instanceof EventTarget/Node` for everything that chains to the real spine prototypes. */
    /* Element/HTMLElement ARE the interface every el_wrap'd node uses (all our elements share el_proto), so their
       ctor.prototype IS el_proto — then `node instanceof Element` / `instanceof HTMLElement` hold (the common DOM
       feature-detection). The specific tags (HTMLDivElement..) keep a fresh proto inheriting el_proto: our engine
       has one shared element proto, not per-tag prototypes, so per-tag instanceof is a known simplification. */
    { JSValue ec = JS_NewCFunction2(ctx, js_ctor_stub, "Element", 0, JS_CFUNC_constructor, 0);
      JS_SetConstructor(ctx, ec, el_proto); JS_SetPropertyStr(ctx, (JSValue)g, "Element", ec); }
    { JSValue hc = JS_NewCFunction2(ctx, js_ctor_stub, "HTMLElement", 0, JS_CFUNC_constructor, 0);
      JS_SetConstructor(ctx, hc, el_proto); JS_SetPropertyStr(ctx, (JSValue)g, "HTMLElement", hc); }
    static const char *N[] = { "HTMLDivElement",
        "HTMLInputElement", "HTMLButtonElement", "HTMLFormElement", "HTMLAnchorElement", "HTMLSpanElement",
        "HTMLImageElement", "SVGElement" };
    for (int i = 0; i < (int)(sizeof N / sizeof N[0]); i++) def_ctor(ctx, g, N[i], el_proto);
    ce_init(ctx);
    JSValue ce = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ce, "define", JS_NewCFunction(ctx, js_ce_define, "define", 2));
    JS_SetPropertyStr(ctx, ce, "get", JS_NewCFunction(ctx, js_concolic_stub, "get", 1));
    JS_SetPropertyStr(ctx, ce, "whenDefined", JS_NewCFunction(ctx, js_concolic_stub, "whenDefined", 1));
    JS_SetPropertyStr(ctx, ce, "upgrade", JS_NewCFunction(ctx, js_noop, "upgrade", 1));
    JS_SetPropertyStr(ctx, (JSValue)g, "customElements", ce);
}
