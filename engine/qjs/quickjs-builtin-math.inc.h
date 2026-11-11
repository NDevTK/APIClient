/* Math */

/* precondition: a and b are not NaN */
static double js_fmin(double a, double b)
{
    if (a == 0 && b == 0) {
        JSFloat64Union a1, b1;
        a1.d = a;
        b1.d = b;
        a1.u64 |= b1.u64;
        return a1.d;
    } else {
        return a < b ? a : b;
    }
}

/* precondition: a and b are not NaN */
static double js_fmax(double a, double b)
{
    if (a == 0 && b == 0) {
        JSFloat64Union a1, b1;
        a1.d = a;
        b1.d = b;
        a1.u64 &= b1.u64;
        return a1.d;
    } else {
        return a < b ? b : a;
    }
}

/* §21.3.2.25 Math.max ( ...args ) / §21.3.2.26 Math.min ( ...args ) RUN OVER THE OPERANDS' OWN EXAMPLES. Both
   are "For each element arg of args, do Let n be ? ToNumber(arg)" followed by a comparison walk, so the real
   function is re-entered with each unknown replaced by the concrete this engine has for it — the same shape
   22.1.2.1's example builder uses, and for the same reason: the value the page would have computed is worth
   more than a placeholder. An argument with NO example makes the whole result exampleless rather than letting
   a picked number decide a later `if`. */
static JSValue js_math_min_max(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic);
static JSValue js_math_min_max_example(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv, int magic)
{
    JSValue *ex = js_malloc(ctx, sizeof(JSValue) * (size_t)(argc > 0 ? argc : 1));
    JSValue r;
    int i, n = 0;

    /* ALWAYS FATAL rather than a dropped example: a derivation that silently loses its concrete is a value the
       @H surface then reports as shape-only, which is indistinguishable from an operand that never had one. */
    CHECK(ex != NULL, "out of memory building a derived unknown's example operands");
    for (i = 0; i < argc; i++) {
        JSValue e = js_concolic_operand(ctx, argv[i]);
        if (JS_IsUninitialized(e))
            e = js_dup(argv[i]);
        else if (JS_IsUndefined(e)) {          /* the unknown carries no concrete operand */
            r = JS_UNDEFINED;
            goto done;
        }
        DCHECK(!JS_IsObject(e) && !JS_IsSymbol(e),
               "an unknown's example is not an operable primitive — an example rides the value the interpreter "
               "actually produced, so a producer attached something this engine never computed");
        ex[n++] = e;
    }
    r = js_math_min_max(ctx, this_val, argc, (JSValueConst *)ex, magic);
done:
    for (i = 0; i < n; i++)
        JS_FreeValue(ctx, ex[i]);
    js_free(ctx, ex);
    return r;
}

static JSValue js_math_min_max(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    bool is_max = magic;
    double r, a;
    int i;
    uint32_t tag;

    /* §21.3.2.25 / §21.3.2.26 OVER UNKNOWN EXTERNAL INPUT. The coerce-then-compute declaration's coercion is
       §7.1.1 ToPrimitive, which over a concolic is the identity, so an unknown argument arrives here uncoerced
       and every JS_ToFloat64 below asked the conversion boundary for a real double and aborted the instance.
       `Math.max(0, Math.min(255, x))` is the clamp every bundle writes, which is how this was found.
       ONE unknown argument makes the WHOLE result unknown: step 4's `number > highest` is undecided against a
       value nobody has, so no argument can be shown to win, and a partial maximum over the KNOWN arguments
       would be a value the page never computes. The derivation keeps the operand's source and root, so a
       branch over the clamped value still forks and an @S candidate still injects at the source that fed it. */
    for (i = 0; i < argc; i++) {
        JSValue c;
        if (!js_value_is_concolic(argv[i]))
            continue;
        c = js_concolic_derive(ctx, argv[i], is_max ? "Math.max" : "Math.min",
                               js_math_min_max_example(ctx, this_val, argc, argv, magic));
        DCHECK(!JS_IsUninitialized(c),
               "the concolic derivation declined an operand this function had already recognised as unknown "
               "external input — the value semantics were installed without the builtin-derivation hook");
        return c;
    }

    if (unlikely(argc == 0)) {
        return js_float64(is_max ? -INFINITY : INFINITY);
    }

    tag = JS_VALUE_GET_TAG(argv[0]);
    if (tag == JS_TAG_INT) {
        int a1, r1 = JS_VALUE_GET_INT(argv[0]);
        for(i = 1; i < argc; i++) {
            tag = JS_VALUE_GET_TAG(argv[i]);
            if (tag != JS_TAG_INT) {
                r = r1;
                goto generic_case;
            }
            a1 = JS_VALUE_GET_INT(argv[i]);
            if (is_max)
                r1 = max_int(r1, a1);
            else
                r1 = min_int(r1, a1);

        }
        return js_int32(r1);
    } else {
        if (JS_ToFloat64(ctx, &r, argv[0]))
            return JS_EXCEPTION;
        i = 1;
    generic_case:
        while (i < argc) {
            if (JS_ToFloat64(ctx, &a, argv[i]))
                return JS_EXCEPTION;
            if (!isnan(r)) {
                if (isnan(a)) {
                    r = a;
                } else {
                    if (is_max)
                        r = js_fmax(r, a);
                    else
                        r = js_fmin(r, a);
                }
            }
            i++;
        }
        return js_number(r);
    }
}

static double js_math_sign(double a)
{
    if (isnan(a) || a == 0.0)
        return a;
    if (a < 0)
        return -1;
    else
        return 1;
}

static double js_math_round(double a)
{
    JSFloat64Union u;
    uint64_t frac_mask, one;
    unsigned int e, s;

    u.d = a;
    e = (u.u64 >> 52) & 0x7ff;
    if (e < 1023) {
        /* abs(a) < 1 */
        if (e == (1023 - 1) && u.u64 != 0xbfe0000000000000) {
            /* abs(a) > 0.5 or a = 0.5: return +/-1.0 */
            u.u64 = (u.u64 & ((uint64_t)1 << 63)) | ((uint64_t)1023 << 52);
        } else {
            /* return +/-0.0 */
            u.u64 &= (uint64_t)1 << 63;
        }
    } else if (e < (1023 + 52)) {
        s = u.u64 >> 63;
        one = (uint64_t)1 << (52 - (e - 1023));
        frac_mask = one - 1;
        u.u64 += (one >> 1) - s;
        u.u64 &= ~frac_mask; /* truncate to an integer */
    }
    /* otherwise: abs(a) >= 2^52, or NaN, +/-Infinity: no change */
    return u.d;
}

/* §21.3.2.11 Math.clz32 ( x ), §21.3.2.19 Math.hypot ( ...args ), §21.3.2.20 Math.imul ( x, y ) and §21.4.3.4
   Date.UTC — the coerce-then-compute GENERIC bodies whose steps are ToNumber over their arguments — RUN OVER
   THE OPERANDS' OWN EXAMPLES. §21.3.2.25 Math.max ( ...args ) / §21.3.2.26 Math.min ( ...args ) already answer
   this way one function down, and theirs is NOT this builder only because it is the `generic_magic` twin and
   carries a magic this family has none of.
   RE-ENTERING THE REAL BODY IS THE POINT rather than reimplementing each algorithm here: the derived value's
   concrete is then whatever this engine's own `Math.imul` computes, so an example cannot drift from the
   function it claims to be an example of. All three are a coercion followed by pure arithmetic with no user
   code in them, so the re-entry runs nothing a page can observe twice.
   THE VECTOR IS PADDED TO WHAT THE BODY READS AND NOT TO argc. js_primargs_step hands the body the call's
   REAL argc over a buffer padded to the declaration's arity, and a fixed-arity body reads that padding:
   `Math.imul(x)` is argc 1 and still reads argv[1]. An example vector sized argc would be read off its end.
   AN ARGUMENT WITH NO EXAMPLE MAKES THE WHOLE RESULT EXAMPLELESS rather than letting a picked number decide a
   later `if` — a value known only to satisfy a gate is INVENTED, which is the one thing @H may never emit. */
static JSValue js_primargs_body_example(JSContext *ctx, JSCFunction *body, JSValueConst this_val,
                                       int argc, int nread, JSValueConst *argv)
{
    int n = argc > nread ? argc : nread;
    JSValue *ex = js_malloc(ctx, sizeof(JSValue) * (size_t)(n > 0 ? n : 1));
    JSValue r;
    int i, built = 0;

    /* ALWAYS FATAL rather than a dropped example: a derivation that silently loses its concrete is a value the
       @H surface then reports as shape-only, which is indistinguishable from an operand that never had one. */
    CHECK(ex != NULL, "out of memory building a derived unknown's example operands");
    for (i = 0; i < n; i++) {
        JSValue e = js_concolic_operand(ctx, argv[i]);
        if (JS_IsUninitialized(e))
            e = js_dup(argv[i]);
        else if (JS_IsUndefined(e)) {          /* the unknown carries no concrete operand */
            r = JS_UNDEFINED;
            goto done;
        }
        DCHECK(!JS_IsObject(e) && !JS_IsSymbol(e),
               "an unknown's example is not an operable primitive — an example rides the value the interpreter "
               "actually produced, so a producer attached something this engine never computed");
        ex[built++] = e;
    }
    r = body(ctx, this_val, argc, (JSValueConst *)ex);
done:
    for (i = 0; i < built; i++)
        JS_FreeValue(ctx, ex[i]);
    js_free(ctx, ex);
    return r;
}

/* The answer the three owe AT THEIR OWN SITE, which is what the conversion boundary's abort asks for by name.
   `nread` is how many leading arguments the SPEC's steps coerce — two for §21.3.2.20 "Math.imul ( x, y )",
   whose steps name x and y; one for §21.3.2.11 "Math.clz32 ( x )", whose step 1 names x; and every one of
   them for §21.3.2.19 "Math.hypot ( ...args )", which coerces each element of args — so an unknown in a slot the
   algorithm never reads leaves the result KNOWN: `Math.imul(1, 2, {unknown})` is 2 and not an unknown, because
   step 1 and step 2 name x and y and no step reads a third argument.
   Returns JS_UNINITIALIZED when no argument the steps coerce is unknown, which is the caller's signal that its
   ordinary body is the right answer. */
static JSValue js_primargs_body_unknown(JSContext *ctx, JSCFunction *body, const char *op,
                                       JSValueConst this_val, int argc, int nread, JSValueConst *argv)
{
    int i, coerced = argc < nread ? argc : nread;

    for (i = 0; i < coerced; i++) {
        JSValue c;
        if (!js_value_is_concolic(argv[i]))
            continue;
        c = js_concolic_derive(ctx, argv[i], op,
                               js_primargs_body_example(ctx, body, this_val, argc, nread, argv));
        DCHECK(!JS_IsUninitialized(c),
               "the concolic derivation declined an operand this function had already recognised as unknown "
               "external input — the value semantics were installed without the builtin-derivation hook");
        return c;
    }
    return JS_UNINITIALIZED;
}

static JSValue js_math_hypot(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    double r, a;
    JSValue c;
    int i;

    /* §21.3.2.19 Math.hypot ( ...args ) OVER UNKNOWN EXTERNAL INPUT. Its steps are "For each element arg of
       args, do Let n be ? ToNumber(arg)" and then a walk over the coerced List, so the JS_ToFloat64 below IS
       that §7.1.4 ToNumber ( arg ). The coerce-then-compute declaration's own coercion is §7.1.1 ToPrimitive
       ( input [ , preferredType ] ), which over a concolic is the IDENTITY — the value is primitive in the page
       and wears an Object only as the solver's carrier — so an unknown argument arrives here UNCOERCED and that
       conversion boundary, which owes C a real double and can only abort, took the whole document's findings
       with it.
       ONE unknown argument makes the WHOLE result unknown. A known +∞𝔽 would settle the result on its own, so
       answering unknown there is broader than the algorithm strictly needs; it costs a FORK the search would
       otherwise not take, which is the direction to err in, and never a value the page did not compute. */
    c = js_primargs_body_unknown(ctx, js_math_hypot, "Math.hypot", this_val, argc, argc, argv);
    if (!JS_IsUninitialized(c))
        return c;

    r = 0;
    if (argc > 0) {
        if (JS_ToFloat64(ctx, &r, argv[0]))
            return JS_EXCEPTION;
        if (argc == 1) {
            r = fabs(r);
        } else {
            /* use the built-in function to minimize precision loss */
            for (i = 1; i < argc; i++) {
                if (JS_ToFloat64(ctx, &a, argv[i]))
                    return JS_EXCEPTION;
                r = hypot(r, a);
            }
        }
    }
    return js_float64(r);
}

static double js_math_f16round(double a)
{
    return fromfp16(tofp16(a));
}

static double js_math_fround(double a)
{
    return (float)a;
}

static JSValue js_math_imul(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    uint32_t a, b, c;
    int32_t d;
    JSValue u;

    /* §21.3.2.20 Math.imul ( x, y ) OVER UNKNOWN EXTERNAL INPUT. Steps 1 and 2 are "Let a be ℝ(? ToUint32(x))"
       and "Let b be ℝ(? ToUint32(y))", and §7.1.9 ToUint32 ( arg )'s own step 1 is §7.1.4 ToNumber ( arg ) — so
       the two JS_ToUint32 calls below ARE that coercion. The declaration's coercion is §7.1.1 ToPrimitive
       ( input [ , preferredType ] ), which over a concolic is the IDENTITY, so an unknown arrives here
       uncoerced and asked the conversion boundary for a real integer, which owes C one and can only abort.
       Every hashing helper a bundler ships is `Math.imul` over bytes it was handed, which is how large this is.
       EXACTLY TWO ARGUMENTS ARE COERCED, which is why the scan is bounded rather than over argc: no step reads
       a third, so `Math.imul(1, 2, {unknown})` is 2 and answering unknown there would be a value the page does
       not compute. */
    u = js_primargs_body_unknown(ctx, js_math_imul, "Math.imul", this_val, argc, 2, argv);
    if (!JS_IsUninitialized(u))
        return u;

    if (JS_ToUint32(ctx, &a, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &b, argv[1]))
        return JS_EXCEPTION;
    c = a * b;
    memcpy(&d, &c, sizeof(d));
    return js_int32(d);
}

static JSValue js_math_clz32(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    uint32_t a, r;
    JSValue u;

    /* §21.3.2.11 Math.clz32 ( x ) OVER UNKNOWN EXTERNAL INPUT. Step 1 is "Let n be ? ToUint32(x)", whose own
       step 1 is §7.1.4 ToNumber ( arg ), so the JS_ToUint32 below IS that coercion and an unknown reaching it
       aborted the instance for the reason its two siblings did: §7.1.1 ToPrimitive over a concolic is the
       IDENTITY, so the declaration hands one straight through uncoerced.
       ONE argument is coerced and no step reads a second, so the scan is bounded at one: the count of leading
       zero bits of a value nobody has is unknown, and nothing else about the call can make it known. */
    u = js_primargs_body_unknown(ctx, js_math_clz32, "Math.clz32", this_val, argc, 1, argv);
    if (!JS_IsUninitialized(u))
        return u;

    if (JS_ToUint32(ctx, &a, argv[0]))
        return JS_EXCEPTION;
    if (a == 0)
        r = 32;
    else
        r = clz32(a);
    return js_int32(r);
}


static void sum_precise_init(SumPreciseState *s)
{
    s->state = SUM_PRECISE_STATE_MINUS_ZERO;
    s->acc[0] = 0;
    s->n_limbs = 1;
}

#define ADDC64(res, carry_out, op1, op2, carry_in)      \
do {                                                    \
    uint64_t __v, __a, __k, __k1;                       \
    __v = (op1);                                        \
    __a = __v + (op2);                                  \
    __k1 = __a < __v;                                   \
    __k = (carry_in);                                   \
    __a = __a + __k;                                    \
    carry_out = (__a < __k) | __k1;                     \
    res = __a;                                          \
} while (0)

static void sum_precise_add(SumPreciseState *s, double d)
{
    uint64_t a, m, a0, carry, acc_sign, a_sign;
    int sgn, e, p, n, i;
    unsigned shift;

    a = float64_as_uint64(d);
    sgn = a >> 63;
    e = (a >> 52) & ((1 << 11) - 1);
    m = a & (((uint64_t)1 << 52) - 1);
    if (unlikely(e == 2047)) {
        if (m == 0) {
            /* +/- infinity */
            if (s->state == SUM_PRECISE_STATE_NAN ||
                (s->state == SUM_PRECISE_STATE_MINUS_INFINITY && !sgn) ||
                (s->state == SUM_PRECISE_STATE_INFINITY && sgn)) {
                s->state = SUM_PRECISE_STATE_NAN;
            } else {
                s->state = SUM_PRECISE_STATE_INFINITY + sgn;
            }
        } else {
            /* NaN */
            s->state = SUM_PRECISE_STATE_NAN;
        }
    } else if (e == 0) {
        if (likely(m == 0)) {
            /* zero */
            if (s->state == SUM_PRECISE_STATE_MINUS_ZERO && !sgn)
                s->state = SUM_PRECISE_STATE_FINITE;
        } else {
            /* subnormal */
            p = 0;
            shift = 0;
            goto add;
        }
    } else {
        m |= (uint64_t)1 << 52;
        shift = e - 1;
        p = shift / 64;
        /* 'p' is the position of a0 in acc */
        shift %= 64;
    add:
        if (s->state >= SUM_PRECISE_STATE_INFINITY)
            return;
        s->state = SUM_PRECISE_STATE_FINITE;
        n = s->n_limbs;

        acc_sign = (int64_t)s->acc[n - 1] >> 63;

        /* sign extend acc */
        for(i = n; i <= p; i++)
            s->acc[i] = acc_sign;

        carry = sgn;
        a_sign = -sgn;
        a0 = m << shift;
        ADDC64(s->acc[p], carry, s->acc[p], a0 ^ a_sign, carry);
        if (shift >= 12) {
            p++;
            if (p >= n)
                s->acc[p] = acc_sign;
            a0 = m >> (64 - shift);
            ADDC64(s->acc[p], carry, s->acc[p], a0 ^ a_sign, carry);
        }
        p++;
        if (p >= n) {
            n = p;
        } else {
            /* carry */
            for(i = p; i < n; i++) {
                /* if 'a' positive: stop condition: carry = 0.
                   if 'a' negative: stop condition: carry = 1. */
                if (carry == sgn)
                    goto done;
                ADDC64(s->acc[i], carry, s->acc[i], a_sign, carry);
            }
        }

        /* extend the accumulator if needed */
        a0 = carry + acc_sign + a_sign;
        /* -1 <= a0 <= 1 (if both acc and a are negative, carry is set) */
        if (a0 != ((int64_t)s->acc[n - 1] >> 63)) {
            s->acc[n++] = a0;
        }
    done:
        s->n_limbs = n;
    }
}

static double sum_precise_get_result(SumPreciseState *s)
{
    int n, shift, e, p, is_neg, i;
    uint64_t m, addend, carry;

    if (s->state != SUM_PRECISE_STATE_FINITE) {
        switch(s->state) {
        default:
        case SUM_PRECISE_STATE_MINUS_ZERO:
            return -0.0;
        case SUM_PRECISE_STATE_INFINITY:
            return INFINITY;
        case SUM_PRECISE_STATE_MINUS_INFINITY:
            return -INFINITY;
        case SUM_PRECISE_STATE_NAN:
            return NAN;
        }
    }

    /* extract the sign and absolute value */
    n = s->n_limbs;
    is_neg = s->acc[n - 1] >> 63;
    if (is_neg) {
        /* acc = -acc */
        carry = 1;
        for(i = 0; i < n; i++) {
            ADDC64(s->acc[i], carry, ~s->acc[i], 0, carry);
        }
    }
    /* normalize */
    while (n > 0 && s->acc[n - 1] == 0)
        n--;
    /* zero result. The spec tells it is always positive in the finite case */
    if (n == 0)
        return 0.0;
    /* subnormal case */
    if (n == 1 && s->acc[0] < ((uint64_t)1 << 52))
        return uint64_as_float64(((uint64_t)is_neg << 63) | s->acc[0]);
    /* normal case */
    e = n * 64;
    p = n - 1;
    m = s->acc[p];
    shift = clz64(m);
    e = e - shift - 52;
    if (shift != 0) {
        m <<= shift;
        if (p > 0) {
            int shift1;
            uint64_t nz;
            p--;
            shift1 = 64 - shift;
            nz = s->acc[p] & (((uint64_t)1 << shift1) - 1);
            m = m | (s->acc[p] >> shift1) | (nz != 0);
        }
    }
    if ((m & ((1 << 10) - 1)) == 0) {
        /* see if the LSB part is non zero for the final rounding  */
        while (p > 0) {
            p--;
            if (s->acc[p] != 0) {
                m |= 1;
                break;
            }
        }
    }
    /* rounding to nearest with ties to even */
    addend = (1 << 10) - 1 + ((m >> 11) & 1);
    m = (m + addend) >> 11;
    /* handle overflow in the rounding */
    if (m == 0)
        e++;
    if (unlikely(e >= 2047)) {
        /* infinity */
        return uint64_as_float64(((uint64_t)is_neg << 63) | ((uint64_t)2047 << 52));
    } else {
        m &= (((uint64_t)1 << 52) - 1);
        return uint64_as_float64(((uint64_t)is_neg << 63) | ((uint64_t)e << 52) | m);
    }
}

/* DELETED: js_math_sumPrecise. Its last case was a non-iterable argument, whose TypeError the acquire raises on
   the tramp like any other — so with the sink declared at the definition there is nothing for a C entry to do. */

/* ECMAScript §21.3.2.28 Math.random ( ): the value is chosen by "an implementation-defined algorithm or
   strategy", and the host is the implementation that knows which flow is asking, so it answers. A generator
   state on the context would be shared by every flow interleaved on it, making each value depend on the
   schedule. The answer is a Number in [+0, 1) or an unknown whose example is one. */
static JSValue js_math_random(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValue v;

    CHECK(g_concolic.random != NULL,
          "Math.random was called under a host that installed no JSConcolicHooks.random, a member of "
          "JS_CONCOLIC_REQUIRED: the engine holds no generator of its own");
    v = g_concolic.random(ctx);
    DCHECK(JS_IsNumber(v) || (g_concolic.is(v)),
           "JSConcolicHooks.random answered neither a Number nor an unknown");
    return v;
}

/* use local wrappers for math functions to
   - avoid initializing data with dynamic library entry points.
   - avoid some overhead if the call can be inlined at compile or link time.
 */
static double js_math_fabs(double d) { return fabs(d); }
static double js_math_floor(double d) { return floor(d); }
static double js_math_ceil(double d) { return ceil(d); }
static double js_math_sqrt(double d) { return sqrt(d); }
static double js_math_acos(double d) { return acos(d); }
static double js_math_asin(double d) { return asin(d); }
static double js_math_atan(double d) { return atan(d); }
static double js_math_atan2(double a, double b) { return atan2(a, b); }
static double js_math_cos(double d) { return cos(d); }
static double js_math_exp(double d) { return exp(d); }
static double js_math_log(double d) { return log(d); }
static double js_math_sin(double d) { return sin(d); }
static double js_math_tan(double d) { return tan(d); }
static double js_math_trunc(double d) { return trunc(d); }
static double js_math_cosh(double d) { return cosh(d); }
static double js_math_sinh(double d) { return sinh(d); }
static double js_math_tanh(double d) { return tanh(d); }
static double js_math_acosh(double d) { return acosh(d); }
static double js_math_asinh(double d) { return asinh(d); }
static double js_math_atanh(double d) { return atanh(d); }
static double js_math_expm1(double d) { return expm1(d); }
static double js_math_log1p(double d) { return log1p(d); }
static double js_math_log2(double d) { return log2(d); }
static double js_math_log10(double d) { return log10(d); }
static double js_math_cbrt(double d) { return cbrt(d); }

static const JSCFunctionListEntry js_math_funcs[] = {
    JS_CFUNC_STEP_DEF("min", 2, STEPDEF_MATH_MIN ),
    JS_CFUNC_STEP_DEF("max", 2, STEPDEF_MATH_MAX ),
    JS_CFUNC_STEP_DEF("abs", 1, STEPDEF_MATH_ABS ),
    JS_CFUNC_STEP_DEF("floor", 1, STEPDEF_MATH_FLOOR ),
    JS_CFUNC_STEP_DEF("ceil", 1, STEPDEF_MATH_CEIL ),
    JS_CFUNC_STEP_DEF("round", 1, STEPDEF_MATH_ROUND ),
    JS_CFUNC_STEP_DEF("sqrt", 1, STEPDEF_MATH_SQRT ),

    JS_CFUNC_STEP_DEF("acos", 1, STEPDEF_MATH_ACOS ),
    JS_CFUNC_STEP_DEF("asin", 1, STEPDEF_MATH_ASIN ),
    JS_CFUNC_STEP_DEF("atan", 1, STEPDEF_MATH_ATAN ),
    JS_CFUNC_STEP_DEF("atan2", 2, STEPDEF_MATH_ATAN2 ),
    JS_CFUNC_STEP_DEF("cos", 1, STEPDEF_MATH_COS ),
    JS_CFUNC_STEP_DEF("exp", 1, STEPDEF_MATH_EXP ),
    JS_CFUNC_STEP_DEF("log", 1, STEPDEF_MATH_LOG ),
    JS_CFUNC_STEP_DEF("pow", 2, STEPDEF_MATH_POW ),
    JS_CFUNC_STEP_DEF("sin", 1, STEPDEF_MATH_SIN ),
    JS_CFUNC_STEP_DEF("tan", 1, STEPDEF_MATH_TAN ),
    /* ES6 */
    JS_CFUNC_STEP_DEF("trunc", 1, STEPDEF_MATH_TRUNC ),
    JS_CFUNC_STEP_DEF("sign", 1, STEPDEF_MATH_SIGN ),
    JS_CFUNC_STEP_DEF("cosh", 1, STEPDEF_MATH_COSH ),
    JS_CFUNC_STEP_DEF("sinh", 1, STEPDEF_MATH_SINH ),
    JS_CFUNC_STEP_DEF("tanh", 1, STEPDEF_MATH_TANH ),
    JS_CFUNC_STEP_DEF("acosh", 1, STEPDEF_MATH_ACOSH ),
    JS_CFUNC_STEP_DEF("asinh", 1, STEPDEF_MATH_ASINH ),
    JS_CFUNC_STEP_DEF("atanh", 1, STEPDEF_MATH_ATANH ),
    JS_CFUNC_STEP_DEF("expm1", 1, STEPDEF_MATH_EXPM1 ),
    JS_CFUNC_STEP_DEF("log1p", 1, STEPDEF_MATH_LOG1P ),
    JS_CFUNC_STEP_DEF("log2", 1, STEPDEF_MATH_LOG2 ),
    JS_CFUNC_STEP_DEF("log10", 1, STEPDEF_MATH_LOG10 ),
    JS_CFUNC_STEP_DEF("cbrt", 1, STEPDEF_MATH_CBRT ),
    JS_CFUNC_STEP_DEF("hypot", 2, STEPDEF_MATH_HYPOT ),
    JS_CFUNC_DEF("random", 0, js_math_random ),
    JS_CFUNC_STEP_DEF("f16round", 1, STEPDEF_MATH_F16ROUND ),
    JS_CFUNC_STEP_DEF("fround", 1, STEPDEF_MATH_FROUND ),
    JS_CFUNC_STEP_DEF("imul", 2, STEPDEF_MATH_IMUL ),
    JS_CFUNC_STEP_DEF("clz32", 1, STEPDEF_MATH_CLZ32 ),
    JS_CFUNC_CONSUME_DEF("sumPrecise", 1, ITERCONS_SUMPRECISE ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Math", JS_PROP_CONFIGURABLE ),
    JS_PROP_DOUBLE_DEF("E", 2.718281828459045, 0 ),
    JS_PROP_DOUBLE_DEF("LN10", 2.302585092994046, 0 ),
    JS_PROP_DOUBLE_DEF("LN2", 0.6931471805599453, 0 ),
    JS_PROP_DOUBLE_DEF("LOG2E", 1.4426950408889634, 0 ),
    JS_PROP_DOUBLE_DEF("LOG10E", 0.4342944819032518, 0 ),
    JS_PROP_DOUBLE_DEF("PI", 3.141592653589793, 0 ),
    JS_PROP_DOUBLE_DEF("SQRT1_2", 0.7071067811865476, 0 ),
    JS_PROP_DOUBLE_DEF("SQRT2", 1.4142135623730951, 0 ),
};

static const JSCFunctionListEntry js_math_obj[] = {
    JS_OBJECT_DEF("Math", js_math_funcs, countof(js_math_funcs), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),
};

