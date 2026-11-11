/* A built-in's receiver check over an unknown carrier: ECMAScript §10.1.15 RequireInternalSlot ( obj,
   internalSlot ) and the This*Value operations (§20.3.3.3.1 ThisBooleanValue, §20.4.3.4.1 ThisSymbolValue,
   §21.1.3.7.1 ThisNumberValue, §21.2.3.4.1 ThisBigIntValue, §22.1.3.35.1 ThisStringValue).
   The carrier's types partition the check: a carrier that is the slot's primitive type alone holds it (step 1
   returns arg); one that may be an Object may or may not (`Object.create(Date.prototype)` holds no
   [[DateValue]]), so the check has two completions, the slot and the TypeError; any other carrier throws. */

typedef enum JSSlotKind {
    JS_SLOT_BOOLEAN_DATA, JS_SLOT_SYMBOL_DATA, JS_SLOT_NUMBER_DATA, JS_SLOT_BIGINT_DATA, JS_SLOT_STRING_DATA,
    JS_SLOT_DATE_VALUE,
} JSSlotKind;

/* The check's completions as a fork numbers them: 0 is the one a run with no forking policy takes. */
enum { JS_SLOT_ARM_HELD = 0, JS_SLOT_ARM_ABSENT = 1 };
/* The partition's answers over a carrier. */
enum { JS_SLOT_ABSENT, JS_SLOT_HELD, JS_SLOT_UNDECIDED };

static const struct {
    JSClassID class_id;     /* the class whose objects hold the slot */
    unsigned prim;          /* the primitive type step 1 returns as itself, 0 where none is */
    unsigned value_type;    /* the slot value's type */
    const char *op;         /* the check, which names its fork and its derivation */
    const char *refusal;    /* the TypeError's message */
} js_slot_rows[] = {
    [JS_SLOT_BOOLEAN_DATA] = { JS_CLASS_BOOLEAN, JS_LANG_TYPE_BOOLEAN, JS_LANG_TYPE_BOOLEAN,
                               "20.3.3.3.1 ThisBooleanValue ( arg )", "not a boolean" },
    [JS_SLOT_SYMBOL_DATA] = { JS_CLASS_SYMBOL, JS_LANG_TYPE_SYMBOL, JS_LANG_TYPE_SYMBOL,
                              "20.4.3.4.1 ThisSymbolValue ( arg )", "not a symbol" },
    [JS_SLOT_NUMBER_DATA] = { JS_CLASS_NUMBER, JS_LANG_TYPE_NUMBER, JS_LANG_TYPE_NUMBER,
                              "21.1.3.7.1 ThisNumberValue ( arg )", "not a number" },
    [JS_SLOT_BIGINT_DATA] = { JS_CLASS_BIG_INT, JS_LANG_TYPE_BIGINT, JS_LANG_TYPE_BIGINT,
                              "21.2.3.4.1 ThisBigIntValue ( arg )", "not a BigInt" },
    [JS_SLOT_STRING_DATA] = { JS_CLASS_STRING, JS_LANG_TYPE_STRING, JS_LANG_TYPE_STRING,
                              "22.1.3.35.1 ThisStringValue ( arg )", "not a string" },
    [JS_SLOT_DATE_VALUE] = { JS_CLASS_DATE, 0, JS_LANG_TYPE_NUMBER,
                             "10.1.15 RequireInternalSlot ( obj, [[DateValue]] )", "not a Date object" },
};

/* Whether the concrete `x` holds slot `k`: a primitive of its type, or an object of its class whose slot holds a
   value this engine computed. */
static bool js_slot_held_by(JSValueConst x, JSSlotKind k)
{
    bool prim = false;

    switch (k) {
    case JS_SLOT_BOOLEAN_DATA: prim = JS_IsBool(x); break;
    case JS_SLOT_SYMBOL_DATA: prim = JS_IsSymbol(x); break;
    case JS_SLOT_NUMBER_DATA: prim = JS_IsNumber(x); break;
    case JS_SLOT_BIGINT_DATA: prim = JS_IsBigInt(x); break;
    case JS_SLOT_STRING_DATA: prim = JS_IsString(x); break;
    case JS_SLOT_DATE_VALUE: break;
    }
    if (prim)
        return true;
    return JS_VALUE_GET_TAG(x) == JS_TAG_OBJECT && JS_VALUE_GET_OBJ(x)->class_id == js_slot_rows[k].class_id &&
           !js_value_is_concolic(JS_VALUE_GET_OBJ(x)->u.object_data);
}

/* The completion the unknown `v`'s example reaches, or JS_OUTCOME_REAL_UNSTATED where it holds none. */
static int js_slot_real(JSContext *ctx, JSValueConst v, JSSlotKind k)
{
    JSValue ex;
    int real;

    if (!js_concolic_held(v)) return JS_OUTCOME_REAL_UNSTATED;
    ex = js_concolic_operand(ctx, v);
    real = js_slot_held_by(ex, k) ? JS_SLOT_ARM_HELD : JS_SLOT_ARM_ABSENT;
    JS_FreeValue(ctx, ex);
    return real;
}

/* The check over the unknown carrier `v` on the running flow, from its types and, where an Object is among them,
   from the completion an earlier ask of the same check settled. With no forking policy installed the check takes
   its completion 0, as the step driver does. */
static int js_slot_partition(JSValueConst v, JSSlotKind k)
{
    unsigned ty = g_concolic.lang_types(v), prim = js_slot_rows[k].prim;
    int c;

    DCHECK(js_value_is_concolic(v), "a slot check was partitioned over a value that is not unknown");
    if (!(ty & (prim | JS_LANG_TYPE_OBJECT)))
        return JS_SLOT_ABSENT;
    if (!(ty & ~prim))
        return JS_SLOT_HELD;
    c = g_flow_control.outcome_settled ? g_flow_control.outcome_settled(v, js_slot_rows[k].op, 2)
        : g_flow_control.outcome ? -1 : JS_SLOT_ARM_HELD;
    return c == JS_SLOT_ARM_HELD ? JS_SLOT_HELD : c == JS_SLOT_ARM_ABSENT ? JS_SLOT_ABSENT : JS_SLOT_UNDECIDED;
}

/* The slot's value of the unknown carrier `v` the check found holding it: `v` itself where its types are the
   primitive's alone (step 1), else derived from `v` under the check's name and typed the slot's type, with the
   example's slot value as its example. Owned. */
static JSValue js_slot_value(JSContext *ctx, JSValueConst v, JSSlotKind k)
{
    JSValue ex = JS_UNDEFINED, r;

    if (!(g_concolic.lang_types(v) & ~js_slot_rows[k].prim))
        return js_dup(v);
    if (js_concolic_held(v)) {
        JSValue e = js_concolic_operand(ctx, v);

        if (js_slot_held_by(e, k))
            ex = JS_IsObject(e) ? js_dup(JS_VALUE_GET_OBJ(e)->u.object_data) : js_dup(e);
        JS_FreeValue(ctx, e);
    }
    r = g_concolic.derived(ctx, js_slot_rows[k].op, &v, 1, ex);
    DCHECK(js_value_is_concolic(r), "a slot's value derived from an unknown carrier is not unknown");
    g_concolic.type_fix(ctx, r, js_slot_rows[k].value_type);
    return r;
}

/* The check from a C body, which has no resume point to fork at: the slot's value (owned), or the TypeError. */
static JSValue js_slot_check_unknown(JSContext *ctx, JSValueConst v, JSSlotKind k)
{
    int p = js_slot_partition(v, k);

    if (p == JS_SLOT_UNDECIDED) {
        char why[320];

        snprintf(why, sizeof why, "%s over an unknown carrier that may be an Object, from a C body: its two "
                 "completions need a fork. Declare the built-in a step machine and ask js_step_slot_check",
                 js_slot_rows[k].op);
        DFAIL(why);
        p = JS_SLOT_ABSENT;
    }
    if (p == JS_SLOT_ABSENT)
        return JS_ThrowTypeError(ctx, "%s", js_slot_rows[k].refusal);
    return js_slot_value(ctx, v, k);
}

/* The check from a step machine over the unknown carrier `v`, which must stay reachable from the machine's visit
   while a fork is outstanding: 0 with the slot's value in `*out` (owned), -1 with the TypeError thrown, or the
   fork's request, after which the driver re-enters the machine at the same stage. */
static int js_step_slot_check(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSSlotKind k, JSValue *out)
{
    int p, arm, r;

    *out = JS_UNDEFINED;
    p = step_fork_pending(h) ? JS_SLOT_UNDECIDED : js_slot_partition(v, k);
    if (p == JS_SLOT_UNDECIDED) {
        r = step_fork_run(ctx, h, v, js_slot_rows[k].op, 2, js_slot_real(ctx, v, k), &arm);
        if (r) return r;
        p = arm == JS_SLOT_ARM_HELD ? JS_SLOT_HELD : JS_SLOT_ABSENT;
    }
    if (p == JS_SLOT_ABSENT) {
        JS_ThrowTypeError(ctx, "%s", js_slot_rows[k].refusal);
        return -1;
    }
    *out = js_slot_value(ctx, v, k);
    return 0;
}
