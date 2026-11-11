/* Boolean */
/* 20.3.1.1 Boolean(value). ToBoolean invokes nothing and neither does storing [[BooleanData]], so the ONLY
   page-visible operation is step 3's OrdinaryCreateFromConstructor — an ordinary `Get(newTarget, "prototype")`,
   which js_create_from_ctor ran from C, so `Reflect.construct(Boolean, [], new Proxy(f, {get(){for(;;){}}}))`
   aborted at the getter's back-edge. The create-from-ctor DECLARATION performs it on the tramp and hands the
   object here; `obj` is UNDEFINED for the call form (step 2) and OWNED otherwise. */
static JSValue js_boolean_ctor_body(JSContext *ctx, JSValueConst obj_, int argc, JSValueConst *argv)
{
    JSValue obj = unsafe_unconst(obj_);
    JSValue val;

    /* STEP 1'S ToBoolean OVER UNKNOWN INPUT — the SECOND caller of §7.1.2 at operator level, and the same
       answer OP_lnot's is: JS_ToBool below answers `true` for every JS_TAG_OBJECT, so `Boolean(unknown)` is
       the concrete `true` and `if (Boolean(x))` has one arm. `!!x` and `Boolean(x)` are the same predicate and
       reach the same hook with negate 0, so they compose to ONE constraint entry rather than two.
       THE CONSTRUCT FORM DERIVES THE WRAPPER WHOLE, exactly as `new String(unknown)` does and for the same
       reason: step 4's [[BooleanData]] is read back by js_thisBooleanValue off the slot's TAG, so there is no
       slot representation for an unknown, and storing one would make `new Boolean(x).valueOf()` throw. `obj`
       is ours on that arm, so it is given back before the derivation replaces it. */
    if (g_concolic.is(argv[0])) {
        JSValue c;
        if (JS_IsUndefined(obj)) {
            /* A CHECK AND NOT A DCHECK, AND THE PROMOTION IS THIS LINE'S. The hook set is one struct with one
               installer, so `is` answering yes while `to_bool` is absent is the engine's own logic being
               wrong — a DCHECK by category. But this line is the only dereference of that pointer in a
               RELEASE build, where a DCHECK is compiled out: the choice there is between a named abort and a
               segfault, and the state it would segfault on is one where every predicate behind this call has
               already been answered concretely. That is the data-integrity case, so it is fatal in both. */
            return g_concolic.to_bool(ctx, argv[0], 0);   /* step 2: the call form IS the primitive */
        }
        /* RESIDUAL — THE WRAPPER IS DERIVED, NOT BUILT, so `new Boolean(unknown)` is an unknown and not a
           Boolean exotic object. It is a residual and not an assert because this arm is CORRECT for what it
           does: the alternative is storing a concrete `true` in [[BooleanData]], which decides the value the
           whole diff above exists to keep undecided, and there is nothing to crash on — the operand is
           handled, just more coarsely than §20.3.1 describes.
             WHAT IS NOT COVERED: step 4's [[BooleanData]] holding an unknown. js_thisBooleanValue reads that
             slot by TAG, so an unknown in it has no representation the wrapper's own members can read back.
             WHAT THE NEXT DIFF BUILDS: the primitive-example wrapper surface — the same one the concolic
             class's [[OwnPropertyKeys]] already DFAILs for by name when an unknown whose example is a STRING
             is asked to enumerate itself. One surface answers both, because both are the same question: what
             a wrapper over an unknown primitive exposes.
             HOW ITS ABSENCE WOULD SHOW: `new Boolean(x) instanceof Boolean` is false and
             `Object.prototype.toString.call(new Boolean(x))` is not "[object Boolean]", for an `x` this
             engine minted as unknown — where the same two expressions over any concrete `x` answer normally. */
        c = js_concolic_derive(ctx, argv[0], "new Boolean", JS_UNDEFINED);
        DCHECK(!JS_IsUninitialized(c),
               "new Boolean(unknown external input) declined an operand this engine had already recognised as "
               "unknown — the value semantics were installed without their derivation hook");
        JS_FreeValue(ctx, obj);
        return c;
    }
    val = js_bool(JS_ToBool(ctx, argv[0]));
    if (JS_IsUndefined(obj))
        return val;                        /* step 2: the call form IS the primitive */
    JS_SetObjectData(ctx, obj, val);       /* step 4; consumes val, leaves obj ours */
    return obj;
}

static JSValue js_thisBooleanValue(JSContext *ctx, JSValueConst this_val)
{
    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_BOOL)
        return js_dup(this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_BOOLEAN) {
            if (JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_BOOL)
                return p->u.object_data;
        }
    }
    return JS_ThrowTypeError(ctx, "not a boolean");
}

static JSValue js_boolean_toString(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue val = js_thisBooleanValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    return JS_AtomToString(ctx, JS_VALUE_GET_BOOL(val) ?
                       JS_ATOM_true : JS_ATOM_false);
}

static JSValue js_boolean_valueOf(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    return js_thisBooleanValue(ctx, this_val);
}

static const JSCFunctionListEntry js_boolean_proto_funcs[] = {
    JS_CFUNC_DEF("toString", 0, js_boolean_toString ),
    JS_CFUNC_DEF("valueOf", 0, js_boolean_valueOf ),
};

