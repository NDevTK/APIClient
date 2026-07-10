/* Screen + window viewport dimensions — see screen.h. A real browser has a screen even with no display; the
 * dimensions are CONCOLIC (the default 1920x1080 desktop as example, forkable so a responsive breakpoint
 * `if (screen.width < 768) …` / `if (innerWidth < 768) …` explores BOTH the mobile and desktop shipped code —
 * more logic, you don't know which arm ships an endpoint). colorDepth/pixelDepth are concrete 24. */
#include "core/frame/screen.h"
#include "solver/opaque.h"   /* js_concolic — concolic constant (fork + example) */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);

/* onchange = fn : the multi-screen `change` event handler, registered as a driven flow (a page listening for a
   display change ships code behind it). */
static JSValue scr_get_undef(JSContext *ctx, JSValueConst t) { (void)ctx; (void)t; return JS_UNDEFINED; }
static JSValue scr_set_onchange(JSContext *ctx, JSValueConst t, JSValueConst val) {
    if (JS_IsFunction(ctx, val)) { JSValue ty = JS_NewString(ctx, "change"); JSValueConst a[2] = { ty, val };
        JSValue r = js_add_listener(ctx, t, 2, a); JS_FreeValue(ctx, r); JS_FreeValue(ctx, ty); }
    return JS_UNDEFINED;
}

JSValue js_screen_make(JSContext *ctx) {
    JSValue s = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, s, "width", js_concolic(ctx, "{screenW}", JS_NewInt32(ctx, 1920)));
    JS_SetPropertyStr(ctx, s, "height", js_concolic(ctx, "{screenH}", JS_NewInt32(ctx, 1080)));
    JS_SetPropertyStr(ctx, s, "availWidth", js_concolic(ctx, "{screenW}", JS_NewInt32(ctx, 1920)));
    JS_SetPropertyStr(ctx, s, "availHeight", js_concolic(ctx, "{screenH}", JS_NewInt32(ctx, 1040)));
    JS_SetPropertyStr(ctx, s, "colorDepth", JS_NewInt32(ctx, 24));
    JS_SetPropertyStr(ctx, s, "pixelDepth", JS_NewInt32(ctx, 24));
    JS_SetPropertyStr(ctx, s, "orientation", js_concolic(ctx, "{orientation}", JS_NewString(ctx, "landscape-primary")));
    /* isExtended: whether the display extends across multiple monitors — genuinely unknown headless, so a
       concolic bool (example: false, single display) that FORKS a `if (screen.isExtended)` multi-screen branch. */
    JS_SetPropertyStr(ctx, s, "isExtended", js_concolic(ctx, "{screenIsExtended}", JS_FALSE));
    { JSAtom a = JS_NewAtom(ctx, "onchange");
      JS_DefinePropertyGetSet(ctx, s, a,
          JS_NewCFunction2(ctx, (JSCFunction *)scr_get_undef, "onchange", 0, JS_CFUNC_getter, 0),
          JS_NewCFunction2(ctx, (JSCFunction *)scr_set_onchange, "onchange", 1, JS_CFUNC_setter, 0),
          JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
      JS_FreeAtom(ctx, a); }
    return s;
}

void screen_install_viewport(JSContext *ctx, JSValueConst g) {
    JS_SetPropertyStr(ctx, (JSValue)g, "screen", js_screen_make(ctx));
    JS_SetPropertyStr(ctx, (JSValue)g, "innerWidth", js_concolic(ctx, "{innerW}", JS_NewInt32(ctx, 1280)));
    JS_SetPropertyStr(ctx, (JSValue)g, "innerHeight", js_concolic(ctx, "{innerH}", JS_NewInt32(ctx, 720)));
    JS_SetPropertyStr(ctx, (JSValue)g, "outerWidth", js_concolic(ctx, "{innerW}", JS_NewInt32(ctx, 1280)));
    JS_SetPropertyStr(ctx, (JSValue)g, "outerHeight", js_concolic(ctx, "{innerH}", JS_NewInt32(ctx, 720)));
    JS_SetPropertyStr(ctx, (JSValue)g, "devicePixelRatio", js_concolic(ctx, "{dpr}", JS_NewFloat64(ctx, 1.0)));
}
