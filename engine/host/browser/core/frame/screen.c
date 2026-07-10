/* Screen + window viewport dimensions — see screen.h. A real browser has a screen even with no display; the
 * dimensions are CONCOLIC (the default 1920x1080 desktop as example, forkable so a responsive breakpoint
 * `if (screen.width < 768) …` / `if (innerWidth < 768) …` explores BOTH the mobile and desktop shipped code —
 * more logic, you don't know which arm ships an endpoint). colorDepth/pixelDepth are concrete 24. */
#include "core/frame/screen.h"
#include "opaque.h"   /* js_concolic — concolic constant (fork + example) */

JSValue js_screen_make(JSContext *ctx) {
    JSValue s = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, s, "width", js_concolic(ctx, "{screenW}", JS_NewInt32(ctx, 1920)));
    JS_SetPropertyStr(ctx, s, "height", js_concolic(ctx, "{screenH}", JS_NewInt32(ctx, 1080)));
    JS_SetPropertyStr(ctx, s, "availWidth", js_concolic(ctx, "{screenW}", JS_NewInt32(ctx, 1920)));
    JS_SetPropertyStr(ctx, s, "availHeight", js_concolic(ctx, "{screenH}", JS_NewInt32(ctx, 1040)));
    JS_SetPropertyStr(ctx, s, "colorDepth", JS_NewInt32(ctx, 24));
    JS_SetPropertyStr(ctx, s, "pixelDepth", JS_NewInt32(ctx, 24));
    JS_SetPropertyStr(ctx, s, "orientation", js_concolic(ctx, "{orientation}", JS_NewString(ctx, "landscape-primary")));
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
