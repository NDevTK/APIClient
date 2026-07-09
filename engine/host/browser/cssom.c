/* CSSOM environment reads — see cssom.h. Extracted from main.c. getComputedStyle needs the layout tree and
 * matchMedia needs the viewport/media state, NEITHER of which exists headless — so both return the OPAQUE
 * concolic value. Because opaque is infectious (a property read stays opaque, a method call returns opaque and
 * DRIVES its callback arg as a flow), the page reads a uniform honest-unknown for ANY access — `style.width`,
 * `style.getPropertyValue('x')`, `mq.matches`, `mq.addEventListener('change',cb)` all behave, and a gate on
 * any of them FORKS. This replaces the old fixed-shape stub objects, whose undefined-property reads the page's
 * JS never expects. Generic g_opaque (not a sourced/tainted source) — these are environment-unknown, not
 * attacker-controlled. */
#include "cssom.h"
#include "opaque.h"   /* g_opaque — the OPAQUE concolic value */

JSValue js_get_computed_style(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v;
    return JS_DupValue(ctx, g_opaque);   /* CSSStyleDeclaration: no layout headless -> every property is the honest unknown */
}

JSValue js_match_media(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v;
    return JS_DupValue(ctx, g_opaque);   /* MediaQueryList: no viewport headless -> .matches forks, .addEventListener drives its cb */
}
