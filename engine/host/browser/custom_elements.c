/* Custom elements — see custom_elements.h. Extracted from main.c (Blink core/html/custom). */
#include "custom_elements.h"
#include "dom_element.h"   /* g_el_class_id, el_wrap */

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
