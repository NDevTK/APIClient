/* Trusted Types — see trusted_types.h. A real TrustedTypePolicyFactory + TrustedTypePolicy (internal-slot class
 * generated from its IDL), so the engine EXECUTES trustedTypes.createPolicy(...) the way a browser does and the
 * @S analysis observes the REAL policies (RUN, don't match). createHTML wraps the page's own function into a
 * TrustedHTML whose stringifier is the produced string — so a sink that assigns the TrustedHTML sees the page's
 * ACTUAL sanitized output (a weak/identity createHTML lets the payload through; a real sanitizer neutralises it),
 * decided by re-execution, never by reading source. A 'default' policy is recorded because it auto-applies to
 * every sink assignment — the strongest TT-abuse surface. */
#include <string.h>
#include <stdlib.h>
#include "core/trustedtypes/trusted_types.h"
#include "bindings/idl.h"
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

/* TrustedHTML / TrustedScript / TrustedScriptURL — REAL platform objects with an internal [[Data]] slot (the
   produced string) + a kind, NOT a page-visible {__trustedType,__value} bag. So (a) the value is not readable
   or enumerable off the object (no fingerprint), (b) isHTML/isScript/isScriptURL are UNCOUNTERFEITABLE brand
   checks on the slot+kind (a page cannot spoof one with {__trustedType:true}), (c) each is type-discriminated.
   Only toString/toJSON on the prototype expose the string. Mirrors blob.c's internal-slot pattern. */
static JSClassID g_ttval_class_id;
enum { TT_HTML = 0, TT_SCRIPT = 1, TT_SCRIPTURL = 2 };
typedef struct { JSValue value; int kind; } TTVal;
static void ttval_finalizer(JSRuntime *rt, JSValue val) {
    TTVal *v = JS_GetOpaque(val, g_ttval_class_id);
    if (v) { JS_FreeValueRT(rt, v->value); free(v); }
}
static JSValue ttval_tostring(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)c; (void)v; TTVal *d = JS_GetOpaque(t, g_ttval_class_id);
    return d ? JS_DupValue(ctx, d->value) : JS_NewString(ctx, "");
}
/* Wrap the produced string as a Trusted* of `kind` (CONSUMES `str`). */
static JSValue tt_wrap(JSContext *ctx, JSValue str, int kind) {
    JSValue o = JS_NewObjectClass(ctx, g_ttval_class_id);
    TTVal *d = malloc(sizeof *d);
    if (!d) { JS_FreeValue(ctx, str); JS_FreeValue(ctx, o); return JS_ThrowOutOfMemory(ctx); }
    d->value = str; d->kind = kind;
    JS_SetOpaque(o, d);
    return o;
}

/* policy.createHTML(input, …args): RUN the page's rules.createHTML(input,…) and wrap the result. A policy whose
   options lack the rule THROWS (spec) — the flow explores that throw. */
static JSValue tt_create(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, const char *rule, int kind) {
    TTPolicy *p = JS_GetOpaque(this_val, g_ttpolicy_class_id);
    if (!p) return JS_ThrowTypeError(ctx, "%s called on non-TrustedTypePolicy", rule);
    JSValue fn = JS_GetPropertyStr(ctx, p->rules, rule);
    if (!JS_IsFunction(ctx, fn)) { JS_FreeValue(ctx, fn); return JS_ThrowTypeError(ctx, "policy has no %s", rule); }
    JSValue res = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);   /* RUN the page's sanitizer on the real input */
    JS_FreeValue(ctx, fn);
    if (JS_IsException(res)) return res;
    JSValue str = JS_ToString(ctx, res); JS_FreeValue(ctx, res);
    return tt_wrap(ctx, str, kind);   /* the page's REAL sanitized output, as an internal-slot Trusted* */
}
static JSValue m_createHTML(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return tt_create(ctx, t, c, v, "createHTML", TT_HTML); }
static JSValue m_createScript(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return tt_create(ctx, t, c, v, "createScript", TT_SCRIPT); }
static JSValue m_createScriptURL(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return tt_create(ctx, t, c, v, "createScriptURL", TT_SCRIPTURL); }

static const IDLMember TTPOLICY_MEMBERS[] = {
    { "createHTML",      IDL_METHOD, m_createHTML,      1 },
    { "createScript",    IDL_METHOD, m_createScript,    1 },
    { "createScriptURL", IDL_METHOD, m_createScriptURL, 1 },
};
void trusted_types_init(JSContext *ctx) {
    static const IDLInterface iface = { "TrustedTypePolicy", TTPOLICY_MEMBERS, 3, ttpolicy_finalizer };
    g_ttpolicy_class_id = idl_define_class(ctx, &iface);
    JSRuntime *rt = JS_GetRuntime(ctx);   /* the Trusted* value class: internal slot + a prototype with only the stringifier */
    JS_NewClassID(rt, &g_ttval_class_id);
    JSClassDef def = { "TrustedHTML", .finalizer = ttval_finalizer };
    JS_NewClass(rt, g_ttval_class_id, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "toString", JS_NewCFunction(ctx, ttval_tostring, "toString", 0));
    JS_SetPropertyStr(ctx, proto, "toJSON", JS_NewCFunction(ctx, ttval_tostring, "toJSON", 0));
    JS_SetClassProto(ctx, g_ttval_class_id, proto);
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
/* isHTML/isScript/isScriptURL: an UNCOUNTERFEITABLE brand check — the arg must be a real Trusted* of the
   matching kind (magic), not any object carrying a property. */
static JSValue f_isType(JSContext *ctx, JSValueConst t, int c, JSValueConst *v, int magic) {
    (void)t; (void)ctx; if (c < 1) return JS_FALSE;
    TTVal *d = JS_GetOpaque(v[0], g_ttval_class_id);
    return JS_NewBool(ctx, d && d->kind == magic);
}

JSValue js_trusted_types_make(JSContext *ctx) {
    JSValue f = JS_NewObject(ctx);   /* TrustedTypePolicyFactory (singleton) */
    JS_SetPropertyStr(ctx, f, "createPolicy", JS_NewCFunction(ctx, f_createPolicy, "createPolicy", 2));
    JS_SetPropertyStr(ctx, f, "isHTML", JS_NewCFunctionMagic(ctx, f_isType, "isHTML", 1, JS_CFUNC_generic_magic, TT_HTML));
    JS_SetPropertyStr(ctx, f, "isScript", JS_NewCFunctionMagic(ctx, f_isType, "isScript", 1, JS_CFUNC_generic_magic, TT_SCRIPT));
    JS_SetPropertyStr(ctx, f, "isScriptURL", JS_NewCFunctionMagic(ctx, f_isType, "isScriptURL", 1, JS_CFUNC_generic_magic, TT_SCRIPTURL));
    JS_SetPropertyStr(ctx, f, "emptyHTML", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, f, "emptyScript", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, f, "defaultPolicy", JS_NULL);   /* set to the created 'default' policy by real browsers; null until created */
    return f;
}
