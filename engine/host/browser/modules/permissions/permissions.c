/* Permissions API — Blink modules/permissions. A VIRTUAL permission system, not a bare-opaque shrug: there is
 * no real prompt headless, but the spec still DEFINES the behavior, so we model it. query(descriptor) returns a
 * real PermissionStatus whose `state` defaults to the spec default 'prompt' (an un-prompted origin) as the
 * concolic EXAMPLE but FORKS, so `if (status.state === 'granted')` explores the permission-gated code (the
 * logged-out→granted surface the moat wants); `name` echoes the queried permission; onchange/addEventListener
 * register the change handler as a driven flow (a permission-change handler often ships an endpoint). Opaque is
 * RESERVED for genuinely-unknowable attacker input — a permission default is modelable virtual land, not opaque. */
#include "modules/permissions/permissions.h"
#include "solver/opaque.h"   /* js_concolic — the granted/denied outcome is genuinely unknown (forks), example 'prompt' */

extern JSValue js_resolved(JSContext *ctx, JSValue val);                                        /* wrap in a resolved promise */
extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* register handler -> driven flow */

typedef JSValue (*GetFn)(JSContext *, JSValueConst);
typedef JSValue (*SetFn)(JSContext *, JSValueConst, JSValueConst);
static JSValue ps_get_undef(JSContext *ctx, JSValueConst t) { (void)ctx; (void)t; return JS_UNDEFINED; }
/* onchange = fn : register for the 'change' event (permission state changed) so the handler is driven. */
static JSValue ps_set_onchange(JSContext *ctx, JSValueConst t, JSValueConst val) {
    if (JS_IsFunction(ctx, val)) { JSValue ty = JS_NewString(ctx, "change"); JSValueConst a[2] = { ty, val };
        JSValue r = js_add_listener(ctx, t, 2, a); JS_FreeValue(ctx, r); JS_FreeValue(ctx, ty); }
    return JS_UNDEFINED;
}
/* removeEventListener kept a dedicated no-effect so every registered change handler stays reachable for driving. */
static JSValue ps_remove(JSContext *ctx, JSValueConst t, int ac, JSValueConst *av) { (void)ctx; (void)t; (void)ac; (void)av; return JS_UNDEFINED; }

/* query(descriptor) -> Promise<PermissionStatus>. The PermissionStatus is a REAL object (spec shape), not opaque. */
static JSValue perm_query(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    JSValue st = JS_NewObject(ctx);
    JSValue name = (argc >= 1 && JS_IsObject(argv[0])) ? JS_GetPropertyStr(ctx, argv[0], "name") : JS_UNDEFINED;
    JS_SetPropertyStr(ctx, st, "name", JS_IsString(name) ? name : (JS_FreeValue(ctx, name), JS_NewString(ctx, "")));
    /* state: 'prompt' is the default for an un-prompted origin (the example); concolic so the granted/denied gate forks. */
    JS_SetPropertyStr(ctx, st, "state", js_concolic(ctx, "{permissionState}", JS_NewString(ctx, "prompt")));
    { JSAtom a = JS_NewAtom(ctx, "onchange");
      JS_DefinePropertyGetSet(ctx, st, a,
          JS_NewCFunction2(ctx, (JSCFunction *)(GetFn)ps_get_undef, "onchange", 0, JS_CFUNC_getter, 0),
          JS_NewCFunction2(ctx, (JSCFunction *)(SetFn)ps_set_onchange, "onchange", 1, JS_CFUNC_setter, 0),
          JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
      JS_FreeAtom(ctx, a); }
    JS_SetPropertyStr(ctx, st, "addEventListener",    JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, st, "removeEventListener", JS_NewCFunction(ctx, ps_remove, "removeEventListener", 2));
    return js_resolved(ctx, st);
}

JSValue js_permissions_make(JSContext *ctx) {
    JSValue p = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, p, "query", JS_NewCFunction(ctx, perm_query, "query", 1));
    return p;
}
