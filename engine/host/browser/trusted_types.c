/* Trusted Types — see trusted_types.h. A real TrustedTypePolicyFactory + TrustedTypePolicy (internal-slot class
 * generated from its IDL), so the engine EXECUTES trustedTypes.createPolicy(...) the way a browser does and the
 * @S analysis observes the REAL policies (RUN, don't match). createHTML wraps the page's own function into a
 * TrustedHTML whose stringifier is the produced string — so a sink that assigns the TrustedHTML sees the page's
 * ACTUAL sanitized output (a weak/identity createHTML lets the payload through; a real sanitizer neutralises it),
 * decided by re-execution, never by reading source. A 'default' policy is recorded because it auto-applies to
 * every sink assignment — the strongest TT-abuse surface. */
#include <string.h>
#include <stdlib.h>
#include "trusted_types.h"
#include "idl.h"
#include "opaque.h"   /* js_noop */

extern int solve_broke(const char *sc, const char *res);   /* the real breakout detector — parse the createHTML OUTPUT */

static JSClassID g_ttpolicy_class_id;
typedef struct { JSValue rules; } TTPolicy;   /* internal slot: the page's {createHTML,createScript,…} options */

static int g_tt_default = 0, g_tt_any = 0, g_tt_default_weak = -1;   /* observed by RUNNING createPolicy (per document) */
void tt_reset(void) { g_tt_default = 0; g_tt_any = 0; g_tt_default_weak = -1; }
int tt_default_exists(void) { return g_tt_default; }
int tt_any_policy(void) { return g_tt_any; }
int tt_default_weak(void) { return g_tt_default_weak; }   /* 1 = createHTML lets an XSS payload through (RUN-verified), 0 = sanitizes, -1 = no default/unprobed */

/* RUN the default policy's createHTML on an XSS probe and PARSE the output with the real breakout detector:
   a weak/identity policy leaves the marker executable; a real sanitizer strips it. Run-verified, never matched. */
static void tt_probe_default(JSContext *ctx, JSValueConst rules) {
    JSValue ch = JS_GetPropertyStr(ctx, rules, "createHTML");
    if (JS_IsFunction(ctx, ch)) {
        JSValue probe = JS_NewString(ctx, "<img src=x onerror=X9>");
        JSValue out = JS_Call(ctx, ch, JS_UNDEFINED, 1, (JSValueConst *)&probe);
        JS_FreeValue(ctx, probe);
        if (JS_IsException(out)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); g_tt_default_weak = 0; }
        else { const char *s = JS_ToCString(ctx, out); if (s) { g_tt_default_weak = solve_broke("html", s); JS_FreeCString(ctx, s); } }
        JS_FreeValue(ctx, out);
    }
    JS_FreeValue(ctx, ch);
}

static void ttpolicy_finalizer(JSRuntime *rt, JSValue val) {
    TTPolicy *p = JS_GetOpaque(val, g_ttpolicy_class_id);
    if (p) { JS_FreeValueRT(rt, p->rules); free(p); }
}

static JSValue tt_tostring(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)c; (void)v; return JS_GetPropertyStr(ctx, t, "__value");   /* the produced trusted string */
}
/* A TrustedHTML/Script/ScriptURL: an object whose stringifier IS the produced string (the sink consumes it as
   the trusted value). Marked so trustedTypes.isHTML(x) recognises it. CONSUMES `str`. */
static JSValue tt_wrap(JSContext *ctx, JSValue str) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "__trustedType", JS_TRUE);
    JS_SetPropertyStr(ctx, o, "__value", str);   /* the produced trusted string (consumed) */
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, tt_tostring, "toString", 0));
    return o;
}

/* policy.createHTML(input, …args): RUN the page's rules.createHTML(input,…) and wrap the result. A policy whose
   options lack createHTML THROWS on createHTML (spec) — the flow explores that throw. */
static JSValue tt_create(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, const char *rule) {
    TTPolicy *p = JS_GetOpaque(this_val, g_ttpolicy_class_id);
    if (!p) return JS_ThrowTypeError(ctx, "createHTML called on non-TrustedTypePolicy");
    JSValue fn = JS_GetPropertyStr(ctx, p->rules, rule);
    if (!JS_IsFunction(ctx, fn)) { JS_FreeValue(ctx, fn); return JS_ThrowTypeError(ctx, "policy has no %s", rule); }
    JSValue res = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);   /* RUN the page's sanitizer on the real input */
    JS_FreeValue(ctx, fn);
    if (JS_IsException(res)) return res;
    JSValue str = JS_ToString(ctx, res); JS_FreeValue(ctx, res);
    return tt_wrap(ctx, str);   /* wrap the page's REAL sanitized output as the TrustedHTML */
}
static JSValue m_createHTML(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return tt_create(ctx, t, c, v, "createHTML"); }
static JSValue m_createScript(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return tt_create(ctx, t, c, v, "createScript"); }
static JSValue m_createScriptURL(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return tt_create(ctx, t, c, v, "createScriptURL"); }

static const IDLMember TTPOLICY_MEMBERS[] = {
    { "createHTML",      IDL_METHOD, m_createHTML,      1 },
    { "createScript",    IDL_METHOD, m_createScript,    1 },
    { "createScriptURL", IDL_METHOD, m_createScriptURL, 1 },
};
void trusted_types_init(JSContext *ctx) {
    static const IDLInterface iface = { "TrustedTypePolicy", TTPOLICY_MEMBERS, 3, ttpolicy_finalizer };
    g_ttpolicy_class_id = idl_define_class(ctx, &iface);
}

/* trustedTypes.createPolicy(policyName, policyOptions): create + RECORD the policy (observed by execution). */
static JSValue f_createPolicy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *name = argc >= 1 ? JS_ToCString(ctx, argv[0]) : NULL;
    JSValue o = JS_NewObjectClass(ctx, g_ttpolicy_class_id);
    TTPolicy *p = malloc(sizeof *p);
    if (!p) { if (name) JS_FreeCString(ctx, name); JS_FreeValue(ctx, o); return JS_ThrowOutOfMemory(ctx); }
    p->rules = (argc >= 2 && JS_IsObject(argv[1])) ? JS_DupValue(ctx, argv[1]) : JS_NewObject(ctx);
    JS_SetOpaque(o, p);
    JS_SetPropertyStr(ctx, o, "name", name ? JS_NewString(ctx, name) : JS_NewString(ctx, ""));
    g_tt_any = 1;
    if (name && strcmp(name, "default") == 0) { g_tt_default = 1; tt_probe_default(ctx, p->rules); }   /* auto-applies to every sink — RUN its createHTML to see if it actually sanitizes */
    if (name) JS_FreeCString(ctx, name);
    return o;
}
static JSValue f_isType(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; if (c < 1 || !JS_IsObject(v[0])) return JS_FALSE;
    JSValue m = JS_GetPropertyStr(ctx, v[0], "__trustedType"); int is = JS_ToBool(ctx, m); JS_FreeValue(ctx, m);
    return JS_NewBool(ctx, is);
}

JSValue js_trusted_types_make(JSContext *ctx) {
    JSValue f = JS_NewObject(ctx);   /* TrustedTypePolicyFactory (singleton) */
    JS_SetPropertyStr(ctx, f, "createPolicy", JS_NewCFunction(ctx, f_createPolicy, "createPolicy", 2));
    JS_SetPropertyStr(ctx, f, "isHTML", JS_NewCFunction(ctx, f_isType, "isHTML", 1));
    JS_SetPropertyStr(ctx, f, "isScript", JS_NewCFunction(ctx, f_isType, "isScript", 1));
    JS_SetPropertyStr(ctx, f, "isScriptURL", JS_NewCFunction(ctx, f_isType, "isScriptURL", 1));
    JS_SetPropertyStr(ctx, f, "emptyHTML", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, f, "emptyScript", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, f, "defaultPolicy", JS_NULL);   /* set to the created 'default' policy by real browsers; null until created */
    return f;
}
