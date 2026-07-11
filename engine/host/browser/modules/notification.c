/* Notification — see notification.h. A REAL object reflecting its constructor arguments, not an arg-dropping
 * stub: `new Notification(title, options)` sets title from arg0 and body/tag/icon/data/... from `options` (with
 * the spec defaults), so `new Notification('x',{data:cfg}); n.onclick=()=>fetch(n.data.url)` reads the REAL
 * stashed data and the handler is driven. onclick/onshow/onerror/onclose register their handler as a driven
 * scheduler flow; close()/dispatchEvent are dedicated documented no-effect. requestPermission() resolves to a
 * permission the user grants — genuinely unknown headless, so it reads as the opaque concolic value (a gate
 * `if (perm === 'granted')` FORKS, reaching the permission-gated code). */
#include "modules/notification.h"
#include "solver/concolic.h"   /* js_concolic — permission/timestamp are genuinely unknown headless */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);

/* options[key] or the spec default `dflt` (consumed). */
static JSValue opt_or(JSContext *ctx, JSValueConst o, const char *k, JSValue dflt) {
    if (JS_IsObject(o)) { JSValue v = JS_GetPropertyStr(ctx, o, k);
        if (!JS_IsUndefined(v)) { JS_FreeValue(ctx, dflt); return v; } JS_FreeValue(ctx, v); }
    return dflt;
}
/* register `val` for event `type` so the handler is driven by exploration (onX = fn <=> addEventListener). */
static JSValue set_on(JSContext *ctx, JSValueConst t, JSValueConst val, const char *type) {
    if (JS_IsFunction(ctx, val)) { JSValue ty = JS_NewString(ctx, type); JSValueConst a[2] = { ty, val };
        JSValue r = js_add_listener(ctx, t, 2, a); JS_FreeValue(ctx, r); JS_FreeValue(ctx, ty); }
    return JS_UNDEFINED;
}
static JSValue set_onclick(JSContext *c, JSValueConst t, JSValueConst v) { return set_on(c, t, v, "click"); }
static JSValue set_onshow (JSContext *c, JSValueConst t, JSValueConst v) { return set_on(c, t, v, "show"); }
static JSValue set_onerror(JSContext *c, JSValueConst t, JSValueConst v) { return set_on(c, t, v, "error"); }
static JSValue set_onclose(JSContext *c, JSValueConst t, JSValueConst v) { return set_on(c, t, v, "close"); }
static JSValue get_undef  (JSContext *c, JSValueConst t) { (void)t; return JS_UNDEFINED; }

typedef JSValue (*GetFn)(JSContext *, JSValueConst);
typedef JSValue (*SetFn)(JSContext *, JSValueConst, JSValueConst);
static void def_getset(JSContext *ctx, JSValue o, const char *name, GetFn get, SetFn set) {
    JSAtom a = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, o, a,
        JS_NewCFunction2(ctx, (JSCFunction *)get, name, 0, JS_CFUNC_getter, 0),
        JS_NewCFunction2(ctx, (JSCFunction *)set, name, 1, JS_CFUNC_setter, 0),
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

/* close()/dispatchEvent are DEDICATED documented no-effect: no rendered notification to close headless, and
   handlers are driven by exploration rather than a synthetic dispatch (removeEventListener kept a no-effect so
   every registered handler stays reachable for orphan-driving). */
static JSValue n_close   (JSContext *c, JSValueConst t, int ac, JSValueConst *av) { (void)c; (void)t; (void)ac; (void)av; return JS_UNDEFINED; }
static JSValue n_remove  (JSContext *c, JSValueConst t, int ac, JSValueConst *av) { (void)c; (void)t; (void)ac; (void)av; return JS_UNDEFINED; }
static JSValue n_dispatch(JSContext *c, JSValueConst t, int ac, JSValueConst *av) { (void)c; (void)t; (void)ac; (void)av; return JS_FALSE; }

JSValue js_notification_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt;
    JSValueConst opts = argc >= 2 ? argv[1] : JS_UNDEFINED;
    JSValue o = JS_NewObject(ctx);
    /* title = arg0; the rest = options with spec defaults — reflected so a handler reading n.data/n.body/n.tag
       gets the REAL app value the page passed, never a dropped arg. */
    JS_SetPropertyStr(ctx, o, "title", argc >= 1 ? JS_ToString(ctx, argv[0]) : JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, o, "dir",   opt_or(ctx, opts, "dir",  JS_NewString(ctx, "auto")));
    JS_SetPropertyStr(ctx, o, "lang",  opt_or(ctx, opts, "lang", JS_NewString(ctx, "")));
    JS_SetPropertyStr(ctx, o, "body",  opt_or(ctx, opts, "body", JS_NewString(ctx, "")));
    JS_SetPropertyStr(ctx, o, "tag",   opt_or(ctx, opts, "tag",  JS_NewString(ctx, "")));
    JS_SetPropertyStr(ctx, o, "image", opt_or(ctx, opts, "image", JS_NewString(ctx, "")));
    JS_SetPropertyStr(ctx, o, "icon",  opt_or(ctx, opts, "icon", JS_NewString(ctx, "")));
    JS_SetPropertyStr(ctx, o, "badge", opt_or(ctx, opts, "badge", JS_NewString(ctx, "")));
    JS_SetPropertyStr(ctx, o, "vibrate", opt_or(ctx, opts, "vibrate", JS_NewArray(ctx)));
    JS_SetPropertyStr(ctx, o, "renotify", opt_or(ctx, opts, "renotify", JS_FALSE));
    JS_SetPropertyStr(ctx, o, "silent", opt_or(ctx, opts, "silent", JS_NULL));
    JS_SetPropertyStr(ctx, o, "requireInteraction", opt_or(ctx, opts, "requireInteraction", JS_FALSE));
    JS_SetPropertyStr(ctx, o, "data",  opt_or(ctx, opts, "data",  JS_NULL));
    JS_SetPropertyStr(ctx, o, "actions", opt_or(ctx, opts, "actions", JS_NewArray(ctx)));
    /* timestamp defaults to the construction time — genuinely unknown headless, so a concolic (a branch on it forks). */
    JS_SetPropertyStr(ctx, o, "timestamp", js_concolic(ctx, "{notificationTimestamp}", JS_UNDEFINED));
    def_getset(ctx, o, "onclick", get_undef, set_onclick);
    def_getset(ctx, o, "onshow",  get_undef, set_onshow);
    def_getset(ctx, o, "onerror", get_undef, set_onerror);
    def_getset(ctx, o, "onclose", get_undef, set_onclose);
    JS_SetPropertyStr(ctx, o, "addEventListener",    JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, o, "removeEventListener", JS_NewCFunction(ctx, n_remove, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, o, "dispatchEvent",       JS_NewCFunction(ctx, n_dispatch, "dispatchEvent", 1));
    JS_SetPropertyStr(ctx, o, "close",               JS_NewCFunction(ctx, n_close, "close", 0));
    return o;
}
JSValue js_notif_request_perm(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v;
    return js_concolic(ctx, "{notificationPermission}", JS_UNDEFINED);   /* Promise-awaited permission is unknown -> forks the granted/denied gate */
}
