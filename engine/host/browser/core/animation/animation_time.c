/* Web Animations Level 1 §4.2 "Time values". See animation_time.h. */
#include <math.h>
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/animation/animation_time.h"
#include "solver/concolic.h"
#include "solver/decide.h"

/* The relation names a predicate's key is composed from, one per relation this file reads. */
static const char AT_LT[] = "Web Animations Level 1 §4.2 Time values: time value <";
static const char AT_EQ[] = "Web Animations Level 1 §4.2 Time values: time value ==";

bool animation_time_resolved(JSValueConst t)
{
    DCHECK(JS_IsNull(t) || JS_IsNumber(t) || concolic_is(t),
           "a Web Animations time value is neither unresolved (null), a number nor unknown external input — "
           "every producer in core/animation/ writes one of the three");
    return !JS_IsNull(t);
}

static void at_operand(JSValueConst v)
{
    DCHECK(JS_IsNumber(v) || concolic_is(v),
           "Web Animations time arithmetic was given an unresolved or non-numeric operand — every algorithm "
           "tests resolution before it evaluates an expression over a time value");
}

static JSValue at_arith(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    JSValue sp[2];

    at_operand(a);
    at_operand(b);
    if (!concolic_is(a) && !concolic_is(b)) {
        double x = 0, y = 0, r = 0;

        JS_ToFloat64(ctx, &x, a);
        JS_ToFloat64(ctx, &y, b);
        switch (op) {
        case JS_CARITH_SUB: r = x - y; break;
        case JS_CARITH_MUL: r = x * y; break;
        case JS_CARITH_DIV: r = x / y; break;
        default: DFAIL("Web Animations time arithmetic was asked an operator it does not evaluate");
        }
        return JS_NewFloat64(ctx, r);
    }
    sp[0] = JS_DupValue(ctx, a);
    sp[1] = JS_DupValue(ctx, b);
    if (!concolic_arith_hook(ctx, sp + 2, op, 2))
        DFAIL("ECMAScript §13.15.3 ApplyStringOrNumericBinaryOperator declined an operand this file has already "
              "established is unknown — the concolic value semantics are not installed in this host");
    return sp[0];
}

JSValue animation_time_sub(JSContext *ctx, JSValueConst a, JSValueConst b) { return at_arith(ctx, a, b, JS_CARITH_SUB); }
JSValue animation_time_mul(JSContext *ctx, JSValueConst a, JSValueConst b) { return at_arith(ctx, a, b, JS_CARITH_MUL); }
JSValue animation_time_div(JSContext *ctx, JSValueConst a, JSValueConst b) { return at_arith(ctx, a, b, JS_CARITH_DIV); }

JSValue animation_time_add(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValue sp[2];

    at_operand(a);
    at_operand(b);
    if (!concolic_is(a) && !concolic_is(b)) {
        double x = 0, y = 0;

        JS_ToFloat64(ctx, &x, a);
        JS_ToFloat64(ctx, &y, b);
        return JS_NewFloat64(ctx, x + y);
    }
    sp[0] = JS_DupValue(ctx, a);
    sp[1] = JS_DupValue(ctx, b);
    if (!concolic_add_hook(ctx, sp + 2, JS_CONCOLIC_ADD_PLUS))
        DFAIL("ECMAScript §13.15.3 ApplyStringOrNumericBinaryOperator declined an operand this file has already "
              "established is unknown — the concolic value semantics are not installed in this host");
    return sp[0];
}

JSValue animation_time_neg(JSContext *ctx, JSValueConst a)
{
    JSValue zero = JS_NewFloat64(ctx, 0), r = animation_time_sub(ctx, zero, a);

    return r;
}

/* The relation this flow has decided, or the numbers' own answer. An unknown nobody ordered is a fork, and
   these members are plain C with no machine state to clone, so it is the declaration that is missing. */
static bool at_rel(JSContext *ctx, JSValueConst a, JSValueConst b, const char *op, const char *site)
{
    JSValue pred;
    int d;

    at_operand(a);
    at_operand(b);
    if (!concolic_is(a) && !concolic_is(b)) {
        double x = 0, y = 0;

        JS_ToFloat64(ctx, &x, a);
        JS_ToFloat64(ctx, &y, b);
        return op == AT_LT ? x < y : x == y;
    }
    pred = concolic_new_rel(ctx, op, a, b);
    d = decide_value_arm(pred);
    JS_FreeValue(ctx, pred);
    if (d < 0)
        DFAILF("%s: a Web Animations timing algorithm compared a time value this flow has not ordered (%s) — "
               "both orders are real programs, and this algorithm runs in a plain C member. Declare the member a "
               "step machine (JS_CFUNC_STEP_DEF) and ask the relation through its step_fork_run",
               site, op);
    return d == 1;
}

bool animation_time_lt_at(JSContext *ctx, JSValueConst a, JSValueConst b, const char *site)
{
    return at_rel(ctx, a, b, AT_LT, site);
}

bool animation_time_eq_at(JSContext *ctx, JSValueConst a, JSValueConst b, const char *site)
{
    return at_rel(ctx, a, b, AT_EQ, site);
}

JSValue animation_time_max(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    return JS_DupValue(ctx, animation_time_lt(ctx, a, b) ? b : a);
}

JSValue animation_time_min(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    return JS_DupValue(ctx, animation_time_lt(ctx, b, a) ? b : a);
}
