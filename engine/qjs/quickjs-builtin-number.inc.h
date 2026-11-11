/* Number */

/* 21.1.1.1. The receiver slot IS new_target on a constructor step; UNDEFINED means a plain `Number(x)` call.
   The ToNumeric is a TOPRIMITIVE step, so a valueOf with a loop in it parks like any other callee. */
#define NUMCTOR_STAGES(X) \
    X(NUMCTOR_VALUE,  "21.1.1.1 steps 1-3 (prim is ToNumeric(value); n)") \
    X(NUMCTOR_CREATE, "21.1.1.1 step 4 (O is OrdinaryCreateFromConstructor(NewTarget, \"%Number.prototype%\"))")
enum { NUMCTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_num_ctor_steps[] = { NUMCTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_num_ctor_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSNumCtor *s = st;
    JSValueConst nt = s->hdr.this_val;
    JSValueConst arg = step_arg(&s->hdr, 0);
    JSValue val, obj;
    int r;

    /* the value is computed and parked on the state; only the wrapper's prototype read is left */
    if (s->hdr.stage == NUMCTOR_CREATE)
        goto made_value;
    /* 21.1.1.1 over UNKNOWN INPUT: Number(x) of unknown external input is unknown, source kept — the operator
       answers, because the ToNumber boundary owes C a real number. `new Number(x)` is a wrapper and is not this
       case. The derived value IS the number-hint coercion, so .arith's identity operator is what names it. */
    if (!s->coerced && s->hdr.argc > 0 && JS_IsUndefined(nt) &&
        g_concolic.is(arg)) {
        JSValue slot[1];
        slot[0] = JS_DupValue(ctx, arg);
        if (g_concolic.arith(ctx, slot + 1, JS_CARITH_PLUS, 1)) {
            JS_FreeValue(ctx, cb_result);
            s->result = slot[0];
            return 0;
        }
        JS_FreeValue(ctx, slot[0]);
    }
    if (!s->coerced && s->hdr.argc > 0 && JS_VALUE_GET_TAG(arg) == JS_TAG_OBJECT) {
        s->coerced = 1;
        s->cb[0] = (JSValue)arg;   /* borrowed: hdr.argv holds the reference */
        *out_cb = s->cb; *out_argc = HINT_NUMBER;
        JS_FreeValue(ctx, cb_result);
        return 5;
    }
    if (s->coerced) {
        val = JS_ToNumericFree(ctx, cb_result);   /* a primitive now: this runs nothing */
    } else {
        JS_FreeValue(ctx, cb_result);
        val = (s->hdr.argc == 0) ? js_int32(0) : JS_ToNumeric(ctx, arg);
    }
    cb_result = JS_UNDEFINED;   /* consumed above; the prototype read below is handed a fresh delivery slot */
    if (JS_IsException(val)) return -1;
    switch(JS_VALUE_GET_TAG(val)) {
    case JS_TAG_SHORT_BIG_INT:
        val = js_int64(JS_VALUE_GET_SHORT_BIG_INT(val));
        if (JS_IsException(val)) return -1;
        break;
    case JS_TAG_BIG_INT:
        {
            JSBigInt *p = JS_VALUE_GET_PTR(val);
            double d = js_bigint_to_float64(ctx, p);
            JS_FreeValue(ctx, val);
            val = js_float64(d);
        }
        break;
    default:
        break;
    }
    if (JS_IsUndefined(nt)) {
        s->result = val;
        return 0;
    }
    /* the wrapper's prototype read can suspend, so the computed value moves onto the state first */
    s->val = val;
    s->hdr.stage = NUMCTOR_CREATE;
made_value:
    r = step_create_from_ctor_run(ctx, &s->hdr, nt, JS_CLASS_NUMBER, cb_result, &obj, out_cb, out_argc);
    if (r) return r < 0 ? -1 : r;
    JS_SetObjectData(ctx, obj, s->val);
    s->val = JS_UNDEFINED;
    s->result = obj;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_num_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSNumCtor *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->val);
}

static JSValue js_num_ctor_fini(JSContext *ctx, void *st, bool take_result)
{
    JSNumCtor *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* Number.isNaN / Number.isFinite do NOT coerce (21.1.2.2 / 21.1.2.4 step 1 rejects a non-Number outright), so
   they run none of the page's code and read the number directly rather than borrowing the coercing global's
   body — which is now a step machine and cannot be called from C at all. */
static JSValue js_number_isNaN(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    double d;
    if (!JS_IsNumber(argv[0]))
        return JS_FALSE;
    if (unlikely(JS_ToFloat64(ctx, &d, argv[0])))   /* a Number: reads the value, runs nothing */
        return JS_EXCEPTION;
    return js_bool(isnan(d));
}

static JSValue js_number_isFinite(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    double d;
    if (!JS_IsNumber(argv[0]))
        return JS_FALSE;
    if (unlikely(JS_ToFloat64(ctx, &d, argv[0])))
        return JS_EXCEPTION;
    return js_bool(isfinite(d));
}

static JSValue js_number_isInteger(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    int ret;
    ret = JS_NumberIsInteger(ctx, argv[0]);
    if (ret < 0)
        return JS_EXCEPTION;
    else
        return js_bool(ret);
}

static JSValue js_number_isSafeInteger(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    double d;
    if (!JS_IsNumber(argv[0]))
        return JS_FALSE;
    if (unlikely(JS_ToFloat64(ctx, &d, argv[0])))
        return JS_EXCEPTION;
    return js_bool(is_safe_integer(d));
}

static const JSCFunctionListEntry js_number_funcs[] = {
    /* global ParseInt and parseFloat should be defined already or delayed */
    JS_ALIAS_BASE_DEF("parseInt", "parseInt", 0 ),
    JS_ALIAS_BASE_DEF("parseFloat", "parseFloat", 0 ),
    JS_CFUNC_DEF("isNaN", 1, js_number_isNaN ),
    JS_CFUNC_DEF("isFinite", 1, js_number_isFinite ),
    JS_CFUNC_DEF("isInteger", 1, js_number_isInteger ),
    JS_CFUNC_DEF("isSafeInteger", 1, js_number_isSafeInteger ),
    JS_PROP_DOUBLE_DEF("MAX_VALUE", 1.7976931348623157e+308, 0 ),
    JS_PROP_DOUBLE_DEF("MIN_VALUE", 5e-324, 0 ),
    JS_PROP_DOUBLE_DEF("EPSILON", 2.220446049250313e-16, 0 ), /* ES6 */
    JS_PROP_DOUBLE_DEF("MAX_SAFE_INTEGER", 9007199254740991.0, 0 ), /* ES6 */
    JS_PROP_DOUBLE_DEF("MIN_SAFE_INTEGER", -9007199254740991.0, 0 ), /* ES6 */
    // workarounds for msvc & djgpp where NAN and INFINITY
    // are not compile-time expressions
    JS_PROP_U2D_DEF("NaN",               0x7FF8ull<<48, 0 ),
    JS_PROP_U2D_DEF("NEGATIVE_INFINITY", 0xFFF0ull<<48, 0 ),
    JS_PROP_U2D_DEF("POSITIVE_INFINITY", 0x7FF0ull<<48, 0 ),
};

/* §21.1.3.7.1 ThisNumberValue ( arg ). Over an unknown carrier the Number is unknown too. */
static JSValue js_thisNumberValue(JSContext *ctx, JSValueConst this_val)
{
    if (js_value_is_concolic(this_val))
        return js_slot_check_unknown(ctx, this_val, JS_SLOT_NUMBER_DATA);
    if (JS_IsNumber(this_val))
        return js_dup(this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_NUMBER) {
            if (JS_IsNumber(p->u.object_data))
                return js_dup(p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a number");
}

static JSValue js_number_valueOf(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return js_thisNumberValue(ctx, this_val);
}

static int js_get_radix(JSContext *ctx, JSValueConst val)
{
    int radix;
    if (JS_ToInt32Sat(ctx, &radix, val))
        return -1;
    if (radix < 2 || radix > 36) {
        JS_ThrowRangeError(ctx, "toString() radix argument must be between 2 and 36");
        return -1;
    }
    return radix;
}

/* Number.prototype.toString / toLocaleString / toFixed / toExponential / toPrecision. thisNumberValue runs
   nothing (it unwraps the receiver or throws), but the DIGITS argument is ToInt32Sat'd — the page's code, from
   a C body, so `(1).toFixed({valueOf(){while(x){}}})` preempted with no flow base and `(1).toFixed([2])`
   reaches Array.prototype.toString, which is a machine no C body can drive.
   Stage 0 unwraps the receiver, 1 coerces the argument where the mode has one, 2 formats.
   FIVE algorithms, one machine; ONE stage list expanded once per algorithm, see AFIND_STAGES. toLocaleString
   coerces nothing, so its middle label says that rather than naming a step it does not have. */
#define NUMFMT_STAGES(X, RECV, ARG, FORMAT) \
    X(NUMFMT_RECV,   RECV) \
    X(NUMFMT_ARG,    ARG)  \
    X(NUMFMT_FORMAT, FORMAT)
enum { NUMFMT_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0) };
static const char *const js_num_tostring_steps[] = {
    NUMFMT_STAGES(JS_STEP_STAGE_LABEL,
        "21.1.3.6 step 1 (x is ThisNumberValue(this value))",
        "21.1.3.6 steps 2-3 (radixMV is 10, or ToIntegerOrInfinity(radix))",
        "21.1.3.6 steps 4-5 (radixMV is between 2 and 36; Number::toString(x, radixMV))")
    NULL };
static const char *const js_num_tolocale_steps[] = {
    NUMFMT_STAGES(JS_STEP_STAGE_LABEL,
        "21.1.3.4 step 1 (x is ThisNumberValue(this value))",
        "21.1.3.4 coerces no argument: reserved1 and reserved2 are not read",
        "21.1.3.4 step 2 (Number::toString(x, 10))")
    NULL };
static const char *const js_num_tofixed_steps[] = {
    NUMFMT_STAGES(JS_STEP_STAGE_LABEL,
        "21.1.3.3 step 1 (x is ThisNumberValue(this value))",
        "21.1.3.3 step 2 (f is ToIntegerOrInfinity(fractionDigits))",
        "21.1.3.3 steps 4-10 (f is between 0 and 100; the fixed-notation String)")
    NULL };
static const char *const js_num_toexp_steps[] = {
    NUMFMT_STAGES(JS_STEP_STAGE_LABEL,
        "21.1.3.2 step 1 (x is ThisNumberValue(this value))",
        "21.1.3.2 step 2 (f is ToIntegerOrInfinity(fractionDigits))",
        "21.1.3.2 steps 3-15 (f is between 0 and 100; the exponential-notation String)")
    NULL };
static const char *const js_num_toprec_steps[] = {
    NUMFMT_STAGES(JS_STEP_STAGE_LABEL,
        "21.1.3.5 step 1 (x is ThisNumberValue(this value))",
        "21.1.3.5 steps 2-3 (an absent precision is ToString(x); p is ToIntegerOrInfinity(precision))",
        "21.1.3.5 steps 4-13 (p is between 1 and 100; the String of p significant digits)")
    NULL };

/* The formatting stage of the five algorithms, over the Number in s->val and the argument in s->n. 0 with
   s->result set, or -1. */
static int js_number_fmt_format(JSContext *ctx, JSNumberFmt *s)
{
    int mode = s->hdr.arg;
    JSValueConst arg0 = step_arg(&s->hdr, 0);
    int f, flags;

    /* JS_ToInt32Sat's saturation, applied to the int64-saturated result the sub-sequence delivers */
    f = (int)(s->n < INT32_MIN ? INT32_MIN : s->n > INT32_MAX ? INT32_MAX : s->n);

    if (mode == NUMFMT_TOSTRING || mode == NUMFMT_TOLOCALESTRING) {
        int base = 10;
        if (mode == NUMFMT_TOSTRING && !JS_IsUndefined(arg0)) {
            base = f;
            if (base < 2 || base > 36) {
                JS_ThrowRangeError(ctx, "toString() radix argument must be between 2 and 36");
                return -1;
            }
        }
        if (JS_VALUE_GET_TAG(s->val) == JS_TAG_INT) {
            char buf1[70];
            int len = i64toa_radix(buf1, JS_VALUE_GET_INT(s->val), base);
            s->result = js_new_string8_len(ctx, buf1, len);
            return JS_IsException(s->result) ? -1 : 0;
        }
        if (JS_ToFloat64(ctx, &s->d, s->val))
            return -1;
        flags = JS_DTOA_FORMAT_FREE;
        if (base != 10)
            flags |= JS_DTOA_EXP_DISABLED;
        s->result = js_dtoa2(ctx, s->d, base, 0, flags);
        return JS_IsException(s->result) ? -1 : 0;
    }

    if (JS_ToFloat64(ctx, &s->d, s->val))
        return -1;

    if (mode == NUMFMT_TOFIXED) {
        if (f < 0 || f > 100) {
            JS_ThrowRangeError(ctx, "invalid number of digits");
            return -1;
        }
        flags = (fabs(s->d) >= 1e21) ? JS_DTOA_FORMAT_FREE : JS_DTOA_FORMAT_FRAC;
        s->result = js_dtoa2(ctx, s->d, 10, f, flags);
        return JS_IsException(s->result) ? -1 : 0;
    }
    if (mode == NUMFMT_TOEXPONENTIAL) {
        if (!isfinite(s->d)) {
            s->result = JS_ToStringFree(ctx, __JS_NewFloat64(s->d));
            return JS_IsException(s->result) ? -1 : 0;
        }
        if (JS_IsUndefined(arg0)) {
            flags = JS_DTOA_FORMAT_FREE;
            f = 0;
        } else {
            if (f < 0 || f > 100) {
                JS_ThrowRangeError(ctx, "invalid number of digits");
                return -1;
            }
            f++;
            flags = JS_DTOA_FORMAT_FIXED;
        }
        s->result = js_dtoa2(ctx, s->d, 10, f, flags | JS_DTOA_EXP_ENABLED);
        return JS_IsException(s->result) ? -1 : 0;
    }
    DCHECK(mode == NUMFMT_TOPRECISION, "Number formatting machine: unknown mode");
    if (JS_IsUndefined(arg0) || !isfinite(s->d)) {
        s->result = JS_ToStringFree(ctx, __JS_NewFloat64(s->d));
        return JS_IsException(s->result) ? -1 : 0;
    }
    if (f < 1 || f > 100) {
        JS_ThrowRangeError(ctx, "invalid number of digits");
        return -1;
    }
    s->result = js_dtoa2(ctx, s->d, 10, f, JS_DTOA_FORMAT_FIXED);
    return JS_IsException(s->result) ? -1 : 0;
}

/* The five formatters where ThisNumberValue (step 1) answered an unknown Number:
   the String is an unknown derived from the receiver and the coerced argument, whose example is the real
   formatter run on the receiver's example where that is a Number. A range error over the argument holds in
   every world, so it is thrown as the stated receiver's is. */
static int js_number_fmt_unknown(JSContext *ctx, JSNumberFmt *s)
{
    JSValue recv = s->val, ex, real = JS_UNDEFINED, ops[2];

    ex = js_concolic_held(recv) ? js_concolic_operand(ctx, recv) : JS_UNDEFINED;
    if (JS_IsNumber(ex)) {
        s->val = ex;
        if (js_number_fmt_format(ctx, s) < 0) {
            JS_FreeValue(ctx, s->val);
            s->val = recv;
            return -1;
        }
        real = s->result;
        s->result = JS_UNDEFINED;
        JS_FreeValue(ctx, s->val);
        s->val = recv;
    } else {
        JS_FreeValue(ctx, ex);
    }
    ops[0] = recv;
    ops[1] = JS_IsUndefined(step_arg(&s->hdr, 0)) ? JS_UNDEFINED : JS_NewInt64(ctx, s->n);
    s->result = g_concolic.derived(ctx, s->hdr.def->algorithm, ops, 2, real);
    DCHECK(!JS_IsUninitialized(s->result), "a Number formatter over an unknown receiver derived no unknown");
    return 0;
}

static int js_number_fmt_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSNumberFmt *s = st;
    int mode = s->hdr.arg;
    JSValueConst arg0 = step_arg(&s->hdr, 0);
    int r;

    if (s->hdr.stage == NUMFMT_RECV) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        s->result = JS_UNDEFINED;
        /* over an unknown carrier the Number is unknown, and its formatting derives (js_number_fmt_unknown) */
        if (js_value_is_concolic(s->hdr.this_val)) {
            r = js_step_slot_check(ctx, &s->hdr, s->hdr.this_val, JS_SLOT_NUMBER_DATA, &s->val);
            if (r) return r;
        } else {
            s->val = js_thisNumberValue(ctx, s->hdr.this_val);
            if (JS_IsException(s->val)) { s->val = JS_UNDEFINED; return -1; }
        }
        s->hdr.stage = NUMFMT_ARG;
    }
    if (s->hdr.stage == NUMFMT_ARG) {
        /* WHICH modes coerce, and when, is observable: toString with no radix and toPrecision with no
           precision never touch the argument, while toExponential and toFixed coerce even an undefined one. */
        bool coerces = (mode == NUMFMT_TOFIXED || mode == NUMFMT_TOEXPONENTIAL)
                    || (mode == NUMFMT_TOSTRING && !JS_IsUndefined(arg0))
                    || (mode == NUMFMT_TOPRECISION && !JS_IsUndefined(arg0));
        s->n = 0;
        if (coerces) {
            r = step_toint64_run(ctx, &s->hdr, arg0, cb_result, &s->n, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
        }
        s->hdr.stage = NUMFMT_FORMAT;
    }
    DCHECK(s->hdr.stage == NUMFMT_FORMAT, "a Number formatter resumed in no stage");
    JS_FreeValue(ctx, cb_result);

    if (js_value_is_concolic(s->val))
        return js_number_fmt_unknown(ctx, s);
    return js_number_fmt_format(ctx, s);
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_number_fmt_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSNumberFmt *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->val);
}

static JSValue js_number_fmt_fini(JSContext *ctx, void *st, bool take_result)
{
    JSNumberFmt *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static const JSCFunctionListEntry js_number_proto_funcs[] = {
    JS_CFUNC_STEP_DEF("toExponential", 1, STEPDEF_NUM_TOEXPONENTIAL ),
    JS_CFUNC_STEP_DEF("toFixed", 1, STEPDEF_NUM_TOFIXED ),
    JS_CFUNC_STEP_DEF("toPrecision", 1, STEPDEF_NUM_TOPRECISION ),
    JS_CFUNC_STEP_DEF("toString", 1, STEPDEF_NUM_TOSTRING ),
    JS_CFUNC_STEP_DEF("toLocaleString", 0, STEPDEF_NUM_TOLOCALESTRING ),
    JS_CFUNC_DEF("valueOf", 0, js_number_valueOf ),
};



