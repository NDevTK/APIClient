/* BigInt */

/* §21.2.1.1.1 NumberToBigInt ( number ): a RangeError for a Number that is not integral, else ℤ(ℝ(number)). */
static JSValue js_number_to_bigint(JSContext *ctx, double d)
{
    int res;
    JSBigInt *r = js_bigint_from_float64(ctx, &res, d);

    if (r)
        return JS_CompactBigInt(ctx, r);
    if (res == 0)
        return JS_EXCEPTION;
    return JS_ThrowRangeError(ctx, res == 1 ? "cannot convert to BigInt: not an integer"
                                            : "cannot convert NaN or Infinity to BigInt");
}

/* Steps 3 and 4 of §21.2.1.1 BigInt ( value ) over `prim`, step 2's primitive (consumed): a Number goes through
   NumberToBigInt, anything else through §7.1.15 ToBigInt, whose own ToPrimitive is the identity on it. */
static JSValue js_bigint_of_primitive(JSContext *ctx, JSValue prim)
{
    DCHECK(JS_VALUE_GET_TAG(prim) != JS_TAG_OBJECT,
           "BigInt's steps 3-4 were handed an Object — step 2's ToPrimitive answers a primitive or throws");
    switch (JS_VALUE_GET_NORM_TAG(prim)) {
    case JS_TAG_INT:
        return JS_NewBigInt64(ctx, JS_VALUE_GET_INT(prim));
    case JS_TAG_FLOAT64:
        return js_number_to_bigint(ctx, JS_VALUE_GET_FLOAT64(prim));
    default:
        return JS_ToBigIntFree(ctx, prim);
    }
}

/* §21.2.1.1 BigInt ( value ). Step 2's ToPrimitive is the declaration's (js_bigint_ctor_def's PRIMARGS), which
   over an unknown is the identity, so an unknown arrives here uncoerced and the result is derived from it,
   carrying what steps 3-4 produce from its example. */
static JSValue js_bigint_constructor(JSContext *ctx,
                                     JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    JSValue ex;

    if (!JS_IsUndefined(new_target))
        return JS_ThrowTypeErrorNotAConstructor(ctx, new_target);
    ex = js_concolic_operand(ctx, argv[0]);
    if (!JS_IsUninitialized(ex)) {
        JSValue real, c;

        if (js_concolic_codec_operand(argv[0], ex)) {
            real = js_bigint_of_primitive(ctx, ex);
        } else {
            JS_FreeValue(ctx, ex);
            real = JS_UNDEFINED;
        }
        c = js_concolic_derive(ctx, argv[0], "21.2.1.1 BigInt ( value )", real);
        DCHECK(!JS_IsUninitialized(c),
               "the concolic derivation declined an operand this function had already recognised as unknown "
               "external input — the value semantics were installed without the builtin-derivation hook");
        return c;
    }
    return js_bigint_of_primitive(ctx, js_dup(argv[0]));
}

static JSValue js_thisBigIntValue(JSContext *ctx, JSValueConst this_val)
{
    if (JS_IsBigInt(this_val))
        return js_dup(this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_BIG_INT) {
            if (JS_IsBigInt(p->u.object_data))
                return js_dup(p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a BigInt");
}

static JSValue js_bigint_toString(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue val;
    int base;
    JSValue ret;

    val = js_thisBigIntValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        base = 10;
    } else {
        base = js_get_radix(ctx, argv[0]);
        if (base < 0)
            goto fail;
    }
    ret = js_bigint_to_string1(ctx, val, base);
    JS_FreeValue(ctx, val);
    return ret;
 fail:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_bigint_valueOf(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return js_thisBigIntValue(ctx, this_val);
}

static JSValue js_bigint_asUintN(JSContext *ctx,
                                  JSValueConst this_val,
                                  int argc, JSValueConst *argv, int asIntN)
{
    uint64_t bits;
    JSValue res, a;

    if (JS_ToIndex(ctx, &bits, argv[0]))
        return JS_EXCEPTION;
    a = JS_ToBigInt(ctx, argv[1]);
    if (JS_IsException(a))
        return JS_EXCEPTION;
    if (bits == 0) {
        JS_FreeValue(ctx, a);
        res = __JS_NewShortBigInt(ctx, 0);
    } else if (JS_VALUE_GET_TAG(a) == JS_TAG_SHORT_BIG_INT) {
        /* fast case */
        js_slimb_t sv = JS_VALUE_GET_SHORT_BIG_INT(a);
        if (bits >= JS_SHORT_BIG_INT_BITS && (asIntN || sv >= 0)) {
            res = a;
        } else if (bits < JS_SHORT_BIG_INT_BITS) {
            uint64_t v;
            int shift;
            shift = 64 - bits;
            v = sv;
            v = v << shift;
            if (asIntN)
                v = (int64_t)v >> shift;
            else
                v = v >> shift;
            res = __JS_NewShortBigInt(ctx, v);
        } else {
            JSBigInt *r;
            uint64_t len64 = (bits + JS_LIMB_BITS - 1) / JS_LIMB_BITS;
            int len, shift, i;

            if (len64 > JS_BIGINT_MAX_SIZE) {
                JS_FreeValue(ctx, a);
                return JS_ThrowRangeError(ctx, "BigInt is too large to allocate");
            }
            len = len64;
            r = js_bigint_new(ctx, len);
            if (!r) {
                JS_FreeValue(ctx, a);
                return JS_EXCEPTION;
            }
            r->len = len;
            r->tab[0] = sv;
            for(i = 1; i < len; i++)
                r->tab[i] = -1;
            shift = (-bits) & (JS_LIMB_BITS - 1);
            r->tab[len - 1] = (r->tab[len - 1] << shift) >> shift;
            r = js_bigint_extend(ctx, r, 0);
            JS_FreeValue(ctx, a);
            if (!r)
                return JS_EXCEPTION;
            res = JS_CompactBigInt(ctx, r);
        }
    } else {
        JSBigInt *r, *p = JS_VALUE_GET_PTR(a);
        int is_neg = js_bigint_sign(p);
        if (bits >= (uint64_t)p->len * JS_LIMB_BITS &&
            (asIntN || !is_neg)) {
            res = a;
        } else {
            uint64_t len64 = (bits + JS_LIMB_BITS - 1) / JS_LIMB_BITS;
            int len, shift, i;
            js_limb_t v;
            if (len64 > JS_BIGINT_MAX_SIZE) {
                JS_FreeValue(ctx, a);
                return JS_ThrowRangeError(ctx, "BigInt is too large to allocate");
            }
            len = len64;
            r = js_bigint_new(ctx, len);
            if (!r) {
                JS_FreeValue(ctx, a);
                return JS_EXCEPTION;
            }
            r->len = len;
            for(i = 0; i < len - 1; i++)
                r->tab[i] = i < p->len ? p->tab[i] : -is_neg;
            shift = (-bits) & (JS_LIMB_BITS - 1);
            /* 0 <= shift <= JS_LIMB_BITS - 1 */
            v = (len - 1 < p->len ? p->tab[len - 1] : -is_neg) << shift;
            if (asIntN)
                v = (js_slimb_t)v >> shift;
            else
                v = v >> shift;
            r->tab[len - 1] = v;
            if (!asIntN)
                r = js_bigint_extend(ctx, r, 0);
            if (!r) {
                JS_FreeValue(ctx, a);
                return JS_EXCEPTION;
            }
            r = js_bigint_normalize(ctx, r);
            JS_FreeValue(ctx, a);
            res = JS_CompactBigInt(ctx, r);
        }
    }
    return res;
}

static const JSCFunctionListEntry js_bigint_funcs[] = {
    JS_CFUNC_STEP_DEF("asUintN", 2, STEPDEF_BIGINT_ASUINTN ),
    JS_CFUNC_STEP_DEF("asIntN", 2, STEPDEF_BIGINT_ASINTN ),
};

static const JSCFunctionListEntry js_bigint_proto_funcs[] = {
    JS_CFUNC_STEP_DEF("toString", 0, STEPDEF_BIGINT_TOSTRING ),
    JS_CFUNC_DEF("valueOf", 0, js_bigint_valueOf ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "BigInt", JS_PROP_CONFIGURABLE ),
};

int JS_AddIntrinsicBigInt(JSContext *ctx)
{
    JSValue obj1;

    obj1 = JS_NewCConstructor(ctx, JS_CLASS_BIG_INT, "BigInt",
                              js_bigint_constructor, 1, JS_CFUNC_step_ctor, STEPDEF_BIGINT_CTOR,
                              JS_UNDEFINED,
                              js_bigint_funcs, countof(js_bigint_funcs),
                              js_bigint_proto_funcs, countof(js_bigint_proto_funcs),
                              0);
    if (JS_IsException(obj1))
        return -1;
    JS_FreeValue(ctx, obj1);
    return 0;
}

