/* Global JS function bindings — see global_functions.h. Moved out of the scheduler (main.c): eval / new
 * Function / structuredClone are JS-global bindings, not scheduler logic. eval + Function FEED the solver
 * (solve_add records the code body as an @S sink); the scheduler decides the candidate replays. */
#include "bindings/global_functions.h"
#include "solve.h"     /* solve_add — an eval/Function code body is an @S sink */
#include "opaque.h"    /* js_concolic (the eval result), js_noop (a safe callable so new Function(x)() is safe) */

extern char *g_candidate;   /* @S replay: a concrete candidate body IS the sink code — record it, do not run it */

/* new Function(...args, BODY): the body compiles as code -> an eval-class @S sink. An attacker/candidate body
   is recorded + a harmless callable returned; anything else DELEGATES to the real Function (identity preserved,
   wrap.prototype = real Function.prototype so `x instanceof Function` holds). */
static JSValue g_real_function = JS_UNDEFINED;
static JSValue js_function_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    (void)new_target;
    if (argc >= 1) {
        JSValueConst body = argv[argc - 1];
        if (JS_IsOpaque(body) || (g_candidate && JS_IsString(body))) {
            solve_add(ctx, "Function", "js", body);   /* @S: body is code */
            return JS_NewCFunction(ctx, js_noop, "", 0);   /* callable no-op so new Function(x)() is safe */
        }
    }
    return JS_IsUndefined(g_real_function) ? JS_NewCFunction(ctx, js_noop, "", 0)
                                           : JS_CallConstructor(ctx, g_real_function, argc, argv);   /* real compile */
}

static JSValue js_eval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    if (JS_IsOpaque(argv[0])) { solve_add(ctx, "eval", "js", argv[0]); return js_concolic(ctx, "{evalResult}", JS_UNDEFINED); }   /* @S: opaque reaches eval -> detect + spawn candidate replays */
    /* On a candidate-REPLAY flow the payload arrives as a CONCRETE string (the real code transformed it). eval's
       arg IS the sink code, so RECORD it for the breakout check — do NOT JS_Eval it (that would run the X9
       payload against an undefined X9 and never verify the sink). */
    if (g_candidate && JS_IsString(argv[0])) { solve_add(ctx, "eval", "js", argv[0]); return js_concolic(ctx, "{evalResult}", JS_UNDEFINED); }
    if (JS_IsString(argv[0])) {
        size_t len = 0; const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
        JSValue r = s ? JS_Eval(ctx, s, len, "<eval>", JS_EVAL_TYPE_GLOBAL) : JS_UNDEFINED;
        if (s) JS_FreeCString(ctx, s);
        return r;
    }
    return JS_DupValue(ctx, argv[0]);   /* non-string: spec returns the arg unchanged */
}

/* structuredClone(x): a deep clone is identity for forced-exec (shape/opacity/taint carry through unchanged). */
static JSValue js_structured_clone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ (void)this_val; return argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED; }

void install_js_global_functions(JSContext *ctx, JSValueConst global) {
    JSValue g = (JSValue)global;
    JS_SetPropertyStr(ctx, g, "eval", JS_NewCFunction(ctx, js_eval, "eval", 1));   /* eval(concrete) -> forced-execute; eval(external) -> @S */
    JS_SetPropertyStr(ctx, g, "structuredClone", JS_NewCFunction(ctx, js_structured_clone, "structuredClone", 1));
    JSValue realFn = JS_GetPropertyStr(ctx, global, "Function");   /* wrap Function: new Function(attacker) is an eval-class @S sink */
    if (JS_IsFunction(ctx, realFn)) {
        JS_FreeValue(ctx, g_real_function); g_real_function = JS_DupValue(ctx, realFn);
        JSValue wrap = JS_NewCFunction2(ctx, js_function_ctor, "Function", 1, JS_CFUNC_constructor, 0);
        JSValue proto = JS_GetPropertyStr(ctx, realFn, "prototype");   /* preserve so `x instanceof Function` holds */
        if (!JS_IsUndefined(proto)) JS_SetPropertyStr(ctx, wrap, "prototype", proto); else JS_FreeValue(ctx, proto);
        JS_SetPropertyStr(ctx, g, "Function", wrap);
    }
    JS_FreeValue(ctx, realFn);
}

void js_global_functions_free(JSContext *ctx) { JS_FreeValue(ctx, g_real_function); g_real_function = JS_UNDEFINED; }
