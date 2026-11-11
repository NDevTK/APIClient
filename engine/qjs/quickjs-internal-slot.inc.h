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

/* The built-ins whose whole algorithm is the receiver check and one step over its value, as one machine, so the
   check over an unknown carrier can fork: §20.3.3.2 Boolean.prototype.toString, §20.3.3.3 valueOf, §20.4.3.3
   Symbol.prototype.toString, §20.4.3.4 valueOf, §20.4.3.5 [ %Symbol.toPrimitive% ], §21.1.3.7
   Number.prototype.valueOf, §21.2.3.4 BigInt.prototype.valueOf, §22.1.3.29 String.prototype.toString and
   §22.1.3.35 valueOf. The def's `arg` names the row. */
typedef enum JSThisValueRow {
    JS_THISV_BOOLEAN_TOSTRING, JS_THISV_BOOLEAN_VALUEOF, JS_THISV_SYMBOL_TOSTRING, JS_THISV_SYMBOL_VALUEOF,
    JS_THISV_NUMBER_VALUEOF, JS_THISV_BIGINT_VALUEOF, JS_THISV_STRING_VALUEOF,
} JSThisValueRow;

static JSValue js_thisBooleanValue(JSContext *ctx, JSValueConst this_val);
static JSValue js_thisSymbolValue(JSContext *ctx, JSValueConst this_val);
static JSValue js_thisNumberValue(JSContext *ctx, JSValueConst this_val);
static JSValue js_thisBigIntValue(JSContext *ctx, JSValueConst this_val);
static JSValue js_thisStringValue(JSContext *ctx, JSValueConst this_val);
/* The step over the slot's value `v` (taken), which may be unknown. */
static JSValue js_boolean_string(JSContext *ctx, JSValue v);
static JSValue js_symbol_descriptive_string(JSContext *ctx, JSValue v);

static const struct { JSSlotKind kind; JSValue (*known)(JSContext *, JSValueConst); } js_thisv_rows[] = {
    [JS_THISV_BOOLEAN_TOSTRING] = { JS_SLOT_BOOLEAN_DATA, js_thisBooleanValue },
    [JS_THISV_BOOLEAN_VALUEOF] = { JS_SLOT_BOOLEAN_DATA, js_thisBooleanValue },
    [JS_THISV_SYMBOL_TOSTRING] = { JS_SLOT_SYMBOL_DATA, js_thisSymbolValue },
    [JS_THISV_SYMBOL_VALUEOF] = { JS_SLOT_SYMBOL_DATA, js_thisSymbolValue },
    [JS_THISV_NUMBER_VALUEOF] = { JS_SLOT_NUMBER_DATA, js_thisNumberValue },
    [JS_THISV_BIGINT_VALUEOF] = { JS_SLOT_BIGINT_DATA, js_thisBigIntValue },
    [JS_THISV_STRING_VALUEOF] = { JS_SLOT_STRING_DATA, js_thisStringValue },
};

typedef struct JSThisValue {
    JSStepHdr hdr;        /* MUST be first: the generic step driver casts the state to JSStepHdr * */
    JSValue result;       /* DONE (owned) */
} JSThisValue;

#define THISV_STAGES(X) \
    X(THISV_ENTRY, "the receiver check (a This*Value operation) and the step over the value it answers")
enum { THISV_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_thisv_steps[] = { THISV_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_thisv_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSThisValue *s = st;
    JSThisValueRow row = (JSThisValueRow)s->hdr.arg;
    JSValue v;
    int r;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    s->result = JS_UNDEFINED;
    if (js_value_is_concolic(s->hdr.this_val)) {
        r = js_step_slot_check(ctx, &s->hdr, s->hdr.this_val, js_thisv_rows[row].kind, &v);
        if (r) return r;
    } else {
        v = js_thisv_rows[row].known(ctx, s->hdr.this_val);
        if (JS_IsException(v)) return -1;
    }
    switch (row) {
    case JS_THISV_BOOLEAN_TOSTRING: s->result = js_boolean_string(ctx, v); break;
    case JS_THISV_SYMBOL_TOSTRING: s->result = js_symbol_descriptive_string(ctx, v); break;
    case JS_THISV_BOOLEAN_VALUEOF: case JS_THISV_SYMBOL_VALUEOF: case JS_THISV_NUMBER_VALUEOF:
    case JS_THISV_BIGINT_VALUEOF: case JS_THISV_STRING_VALUEOF: s->result = v; break;
    }
    if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; return -1; }
    return 0;
}

static void js_thisv_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSThisValue *s = st;
    v->val(ctx, &s->result);
}

static JSValue js_thisv_fini(JSContext *ctx, void *st, bool take_result)
{
    JSThisValue *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

#define THISV_DEF(ROW, ALG, TYPES) { sizeof(JSThisValue), js_thisv_step, js_thisv_fini, ROW, \
    .visit = js_thisv_visit, .algorithm = ALG, .steps = js_thisv_steps, .result_types = TYPES }
static const JSTrampStepDef js_boolean_tostring_def =
    THISV_DEF(JS_THISV_BOOLEAN_TOSTRING, "20.3.3.2 Boolean.prototype.toString ( )", JS_LANG_TYPE_STRING);
static const JSTrampStepDef js_boolean_valueof_def =
    THISV_DEF(JS_THISV_BOOLEAN_VALUEOF, "20.3.3.3 Boolean.prototype.valueOf ( )", JS_LANG_TYPE_BOOLEAN);
static const JSTrampStepDef js_symbol_tostring_def =
    THISV_DEF(JS_THISV_SYMBOL_TOSTRING, "20.4.3.3 Symbol.prototype.toString ( )", JS_LANG_TYPE_STRING);
static const JSTrampStepDef js_symbol_valueof_def =
    THISV_DEF(JS_THISV_SYMBOL_VALUEOF, "20.4.3.4 Symbol.prototype.valueOf ( )", JS_LANG_TYPE_SYMBOL);
static const JSTrampStepDef js_symbol_toprim_def =
    THISV_DEF(JS_THISV_SYMBOL_VALUEOF, "20.4.3.5 Symbol.prototype [ %Symbol.toPrimitive% ] ( hint )",
              JS_LANG_TYPE_SYMBOL);
static const JSTrampStepDef js_number_valueof_def =
    THISV_DEF(JS_THISV_NUMBER_VALUEOF, "21.1.3.7 Number.prototype.valueOf ( )", JS_LANG_TYPE_NUMBER);
static const JSTrampStepDef js_bigint_valueof_def =
    THISV_DEF(JS_THISV_BIGINT_VALUEOF, "21.2.3.4 BigInt.prototype.valueOf ( )", JS_LANG_TYPE_BIGINT);
static const JSTrampStepDef js_string_tostring_def =
    THISV_DEF(JS_THISV_STRING_VALUEOF, "22.1.3.29 String.prototype.toString ( )", JS_LANG_TYPE_STRING);
static const JSTrampStepDef js_string_valueof_def =
    THISV_DEF(JS_THISV_STRING_VALUEOF, "22.1.3.35 String.prototype.valueOf ( )", JS_LANG_TYPE_STRING);
#undef THISV_DEF
