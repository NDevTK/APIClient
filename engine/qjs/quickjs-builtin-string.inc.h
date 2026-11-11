/* String */

static int js_string_get_own_property(JSContext *ctx,
                                      JSPropertyDescriptor *desc,
                                      JSValueConst obj, JSAtom prop)
{
    JSObject *p;
    JSString *p1;
    uint32_t idx, ch;

    /* This is a class exotic method: obj class_id is JS_CLASS_STRING */
    if (__JS_AtomIsTaggedInt(prop)) {
        p = JS_VALUE_GET_OBJ(obj);
        if (JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_STRING) {
            p1 = JS_VALUE_GET_STRING(p->u.object_data);
            idx = __JS_AtomToUInt32(prop);
            if (idx < p1->len) {
                if (desc) {
                    ch = string_get(p1, idx);
                    desc->flags = JS_PROP_ENUMERABLE;
                    desc->value = js_new_string_char(ctx, ch);
                    desc->getter = JS_UNDEFINED;
                    desc->setter = JS_UNDEFINED;
                }
                return true;
            }
        }
    }
    return false;
}

static int js_string_define_own_property(JSContext *ctx,
                                         JSValueConst this_obj,
                                         JSAtom prop, JSValueConst val,
                                         JSValueConst getter,
                                         JSValueConst setter, int flags)
{
    uint32_t idx;
    JSObject *p;
    JSString *p1, *p2;

    if (__JS_AtomIsTaggedInt(prop)) {
        idx = __JS_AtomToUInt32(prop);
        p = JS_VALUE_GET_OBJ(this_obj);
        if (JS_VALUE_GET_TAG(p->u.object_data) != JS_TAG_STRING)
            goto def;
        p1 = JS_VALUE_GET_STRING(p->u.object_data);
        if (idx >= p1->len)
            goto def;
        if (!check_define_prop_flags(JS_PROP_ENUMERABLE, flags))
            goto fail;
        /* check that the same value is configured */
        if (flags & JS_PROP_HAS_VALUE) {
            if (JS_VALUE_GET_TAG(val) != JS_TAG_STRING)
                goto fail;
            p2 = JS_VALUE_GET_STRING(val);
            if (p2->len != 1)
                goto fail;
            if (string_get(p1, idx) != string_get(p2, 0)) {
            fail:
                return JS_ThrowTypeErrorOrFalse(ctx, flags, "property is not configurable");
            }
        }
        return true;
    } else {
    def:
        return JS_DefineProperty(ctx, this_obj, prop, val, getter, setter,
                                 flags | JS_PROP_NO_EXOTIC);
    }
}

static int js_string_delete_property(JSContext *ctx,
                                     JSValueConst obj, JSAtom prop)
{
    uint32_t idx;

    if (__JS_AtomIsTaggedInt(prop)) {
        idx = __JS_AtomToUInt32(prop);
        if (idx < js_string_obj_get_length(ctx, obj)) {
            return false;
        }
    }
    return true;
}

static const JSClassExoticMethods js_string_exotic_methods = {
    .get_own_property = js_string_get_own_property,
    .define_own_property = js_string_define_own_property,
    .delete_property = js_string_delete_property,
    /* 10.4.3.1 [[GetOwnProperty]] ( propertyKey ) step 3 is what this hook is — the engine has already done
       step 1's OrdinaryGetOwnProperty by missing the shape — and step 3 is 10.4.3.5 StringGetOwnProperty
       ( string, propertyKey ), which reads the code unit at the index out of [[StringData]] and builds a
       descriptor from it. No accessor, no page code, so the engine's own C-side own-property reads
       (JS_GetOwnPropertyNoUserCode, and the COW capture's slot read) may run it with no flow base under them.
       Every host class in this engine declared this; the one class quickjs itself owns did not, which is only
       invisible for as long as nobody asserts it. */
    .get_own_property_no_user_code = true,
};


/* No prologue: the machine's whole job is the coercion, so there is no stage 0 to write. */
/* ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). */
#define STRCTOR_STAGES(X) \
    X(STRCTOR_VALUE,   "22.1.1.1 steps 1-3 (s is the empty String, SymbolDescriptiveString(value) for a plain " \
                       "call on a Symbol, or ToString(value); NewTarget undefined returns s)") \
    X(STRCTOR_WRAPPER, "22.1.1.1 step 4 (StringCreate(s, GetPrototypeFromConstructor(NewTarget, " \
                       "\"%String.prototype%\")))")
enum { STRCTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_str_ctor_steps[] = { STRCTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* §22.1.1.1 String ( value ) OVER UNKNOWN EXTERNAL INPUT — the answer for BOTH of its arms, at whichever of
   the two values step 3 is about to coerce: the argument itself, or what step 1's ToPrimitive request
   delivered (`new String({toString(){return location.hash}})` arrives the second way, and §7.1.19 step 11's
   assertion holds about the PAGE's value while the carrier is still an Object).
   Step 3's ToString of an unknown IS the plain call's whole result, which is why that arm has a dedicated hook
   whose shape is `String(x)` and not `x.<op>()`. Step 4's StringCreate over an unknown is a different
   operation and is named as one: there is no real JSString to give StringCreate — made_value below reads the
   wrapper's length straight off it — and the conversion boundary underneath owes C one. The wrapper's example
   is ABSENT rather than the operand's: `new String(v)` is an OBJECT, and handing back the string would claim a
   concrete this engine never built. JS_UNINITIALIZED = an ordinary operand, carry on. */
static JSValue js_str_ctor_unknown(JSContext *ctx, JSValueConst nt, JSValueConst v)
{
    JSValue c;

    if (!(g_concolic.is(v)))
        return JS_UNINITIALIZED;
    if (JS_IsUndefined(nt)) {
        c = g_concolic.to_str(ctx, v);
    } else {
        c = js_concolic_derive(ctx, v, "new String", JS_UNDEFINED);
    }
    DCHECK(!JS_IsUninitialized(c),
           "String(unknown external input) declined an operand this engine had already recognised as unknown — "
           "the value semantics were installed without their derivation hook");
    return c;
}

static int js_str_ctor_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSStrCtor *s = st;
    JSValueConst nt = s->hdr.this_val;   /* the receiver slot IS new_target here; UNDEFINED = a plain call */
    JSValueConst arg = step_arg(&s->hdr, 0);
    JSValue val, obj;
    int r;

    /* the wrapper stage = the string is computed and parked on the state; only its prototype read is left */
    if (s->hdr.stage == STRCTOR_WRAPPER)
        goto made_value;
    /* 22.1.1.1 over UNKNOWN INPUT, asked BEFORE the coercion request below — see js_str_ctor_unknown. The
       RESULT is the derived concolic itself on both arms: there is no wrapper left to build, and made_value
       below reads a real string's length off s->val. */
    if (!s->coerced) {
        JSValue c = js_str_ctor_unknown(ctx, nt, arg);
        if (!JS_IsUninitialized(c)) {
            JS_FreeValue(ctx, cb_result);
            s->result = c;
            return 0;
        }
    }
    if (!s->coerced && JS_VALUE_GET_TAG(arg) == JS_TAG_OBJECT) {
        s->coerced = 1;
        s->cb[0] = (JSValue)arg;   /* borrowed: the header holds the reference */
        *out_cb = s->cb; *out_argc = HINT_STRING;
        JS_FreeValue(ctx, cb_result);
        return 5;
    }
    if (s->coerced) {
        /* the coercion settled: a primitive, so ToString finishes without user code — unless the page's own
           toString ANSWERED with unknown external input, which is the same operator question one hop later. */
        JSValue c = js_str_ctor_unknown(ctx, nt, cb_result);
        if (!JS_IsUninitialized(c)) {
            JS_FreeValue(ctx, cb_result);
            s->result = c;
            return 0;
        }
        val = JS_ToString(ctx, cb_result);
        JS_FreeValue(ctx, cb_result);
    } else {
        JS_FreeValue(ctx, cb_result);
        if (s->hdr.argc == 0) {
            val = js_empty_string(ctx->rt);
        } else if (JS_IsUndefined(nt) && JS_IsSymbol(arg)) {
            /* 22.1.1.1 step 2.b: String(sym) describes the symbol; new String(sym) throws through ToString. */
            JSAtomStruct *p = JS_VALUE_GET_PTR(arg);
            val = JS_ConcatString3(ctx, "Symbol(", JS_AtomToString(ctx, js_get_atom_index(ctx->rt, p)), ")");
        } else {
            val = JS_ToString(ctx, arg);
        }
    }
    cb_result = JS_UNDEFINED;   /* consumed above; the prototype read below is handed a fresh delivery slot */
    if (JS_IsException(val)) return -1;
    if (JS_IsUndefined(nt)) {
        s->result = val;
        return 0;
    }
    /* the wrapper's prototype read can suspend, so the computed string moves onto the state first */
    s->val = val;
    s->hdr.stage = STRCTOR_WRAPPER;
made_value:
    r = step_create_from_ctor_run(ctx, &s->hdr, nt, JS_CLASS_STRING, cb_result, &obj, out_cb, out_argc);
    if (r) return r < 0 ? -1 : r;
    { int len = JS_VALUE_GET_STRING(s->val)->len;
      JS_SetObjectData(ctx, obj, s->val);
      s->val = JS_UNDEFINED;
      JS_DefinePropertyValue(ctx, obj, JS_ATOM_length, js_int32(len), 0);
      s->result = obj; }
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_str_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSStrCtor *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->val);
}

static JSValue js_str_ctor_fini(JSContext *ctx, void *st, bool take_result)
{
    JSStrCtor *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* §22.1.3.35.1 ThisStringValue ( arg ). Over an unknown carrier the String is unknown too. */
static JSValue js_thisStringValue(JSContext *ctx, JSValueConst this_val)
{
    if (js_value_is_concolic(this_val))
        return js_slot_check_unknown(ctx, this_val, JS_SLOT_STRING_DATA);
    if (tag_is_string(JS_VALUE_GET_TAG(this_val)))
        return js_dup(this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_STRING) {
            if (JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_STRING)
                return js_dup(p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a string");
}

/* §22.1.2.1 String.fromCharCode ( ...codeUnits ) RUN OVER THE OPERANDS' OWN EXAMPLES — the concrete this
   engine really has for each code unit, so a derived unknown carries what the function really produces rather
   than a placeholder. §solver models a spec codec faithfully and never enumerates or predicts it, which is why
   this RE-ENTERS the same walk a page with known values takes instead of restating it.
   A code unit with NO example makes the whole result exampleless, and that is the honest answer rather than a
   thin one: §@H only lets a value the code DETERMINED become concrete, and defaulting a missing example to 0
   would put a NUL the page never computed into a string a later sink reads back. */
static JSValue js_str_fromcharcode_example(JSContext *ctx, int argc, JSValueConst *argv)
{
    StringBuffer b_s, *b = &b_s;
    int i;

    string_buffer_init(ctx, b, argc);
    for (i = 0; i < argc; i++) {
        JSValue ex = js_concolic_operand(ctx, argv[i]);   /* UNINITIALIZED when argv[i] is a known primitive */
        bool owned = !JS_IsUninitialized(ex);
        JSValueConst v = owned ? (JSValueConst)ex : argv[i];
        int32_t c;
        bool fail = owned && JS_IsUndefined(ex);          /* the unknown carries no concrete code unit */

        DCHECK(!owned || !(JS_IsObject(ex) || JS_IsSymbol(ex)),
               "an unknown's example is not an operable primitive — an example rides the value the interpreter "
               "actually produced, so a producer attached something this engine never computed");
        if (!fail)
            fail = JS_ToInt32(ctx, &c, v) < 0 || string_buffer_putc16(b, c & 0xffff) < 0;
        if (owned)
            JS_FreeValue(ctx, ex);
        if (fail) {
            /* The marker is returned rather than the throw swallowed here: js_concolic_derive takes a model
               run's exception out of the context at the ONE point every derivation converges on, and a value
               returned with one still pending is a throw from an expression that never threw. */
            string_buffer_free(b);
            return JS_HasException(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
        }
    }
    return string_buffer_end(b);
}

static JSValue js_string_fromCharCode(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    int i;
    StringBuffer b_s, *b = &b_s;

    /* §22.1.2.1 String.fromCharCode ( ...codeUnits ) OVER UNKNOWN EXTERNAL INPUT. Step 2.a is "Let nextCU be
       the code unit whose numeric value is ℝ(? ToUint16(next))", and §7.1.11 ToUint16 ( argument ) step 1 is
       §7.1.4 ToNumber ( argument ) — so this loop IS that coercion. The declaration's own coercion is §7.1.1
       ToPrimitive, which over a concolic is the identity (the value is primitive in the page and wears an
       Object only as the solver's carrier), so unknown input arrives here uncoerced and JS_ToInt32 below asked
       the conversion boundary for a real double, which owes C one and can only abort — taking the whole
       document's findings with it. A real bundle's own decoder is `String.fromCharCode(x.charCode)` over an
       argument it was handed, which is how this was found.
       ONE unknown code unit makes the WHOLE result unknown, which is the rule §23.1.3.18
       Array.prototype.join ( separator )'s site already states and for the same reason: a string built out of a code unit nobody has is a string
       nobody has, and keeping the known prefix would be a value the page never computed. The derivation keeps
       the operand's source and root, so a branch over the result still forks and an @S candidate still injects
       at the source that fed it. §7.1.11 cannot throw on a Number, and the declaration's prim_arg_check has
       already rejected a Symbol, so there is no throw arm here to fork. */
    for (i = 0; i < argc; i++) {
        JSValue c;
        if (!js_value_is_concolic(argv[i]))
            continue;
        c = js_concolic_derive(ctx, argv[i], "String.fromCharCode",
                               js_str_fromcharcode_example(ctx, argc, argv));
        DCHECK(!JS_IsUninitialized(c),
               "the concolic derivation declined an operand this function had already recognised as unknown "
               "external input — the value semantics were installed without the builtin-derivation hook");
        return c;
    }

    // shortcut for single argument common case
    if (argc == 1 && JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT) {
        uint16_t c16 = JS_VALUE_GET_INT(argv[0]);
        return js_new_string_char(ctx, c16);
    }

    string_buffer_init(ctx, b, argc);

    for(i = 0; i < argc; i++) {
        int32_t c;
        if (JS_ToInt32(ctx, &c, argv[i]) || string_buffer_putc16(b, c & 0xffff)) {
            string_buffer_free(b);
            return JS_EXCEPTION;
        }
    }
    return string_buffer_end(b);
}

static JSValue js_string_fromCodePoint(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    double d;
    int i, c;
    StringBuffer b_s, *b = NULL;

    // shortcut for single argument common case
    if (argc == 1 && JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT) {
        c = JS_VALUE_GET_INT(argv[0]);
        if (c < 0 || c > 0x10ffff)
            goto range_error;
        if (c <= 0xffff) {
            return js_new_string_char(ctx, c);
        } else {
            uint16_t c16[2];
            c16[0] = get_hi_surrogate(c);
            c16[1] = get_lo_surrogate(c);
            return js_new_string16_len(ctx, c16, 2);
        }
    }

    /* XXX: could pre-compute string length if all arguments are JS_TAG_INT */
    b = &b_s;
    if (string_buffer_init(ctx, b, argc))
        goto fail;
    for(i = 0; i < argc; i++) {
        if (JS_VALUE_GET_TAG(argv[i]) == JS_TAG_INT) {
            c = JS_VALUE_GET_INT(argv[i]);
            if (c < 0 || c > 0x10ffff)
                goto range_error;
        } else {
            if (JS_ToFloat64(ctx, &d, argv[i]))
                goto fail;
            if (!(d >= 0 && d <= 0x10ffff) || (c = (int)d) != d)
                goto range_error;
        }
        if (string_buffer_putc(b, c))
            goto fail;
    }
    return string_buffer_end(b);

 range_error:
    JS_ThrowRangeError(ctx, "invalid code point");
 fail:
    if (b) string_buffer_free(b);
    return JS_EXCEPTION;
}

/* 22.1.2.4 String.raw ( template, ...substitutions ) — the ONLY thing the engine contributes is the
   concatenation. `template` and its `raw` are ordinary objects the page hands over: step 2's ToObject, step 3's
   `? Get(cooked, "raw")`, step 4's LengthOfArrayLike, step 7.a's `? Get(literals, Pk)` and the ToString of every
   literal (7.b) and every substitution (7.e.ii) are all its code, on accessors or Proxy traps. js_string_raw
   ran the whole loop from C, so a loop in any of them had no flow base and aborted at its back-edge.
   ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). The Repeat is step
   8; the prose here called it step 7 throughout, which is `nextIndex is 0`. The loop HEAD had a stage of its
   own and rested at no step - it ran no request, so it was entered and left inside one step() call - and it is
   the literal read's own bounds test now. */
#define SRAW_STAGES(X) \
    X(SRAW_TOOBJECT, "22.1.2.4 steps 1-2 (substitutionCount; cooked is ToObject(template))") \
    X(SRAW_RAW,      "22.1.2.4 step 3 (literals is ToObject(Get(cooked, \"raw\")))") \
    X(SRAW_LENGTH,   "22.1.2.4 step 4 (literalCount is LengthOfArrayLike(literals))") \
    X(SRAW_LITERAL,  "22.1.2.4 steps 5-8.a (a literalCount of 0 or less is the empty String; R; nextIndex; " \
                     "nextLiteralVal is Get(literals, ToString(nextIndex)))") \
    X(SRAW_LITSTR,   "22.1.2.4 steps 8.b-8.d (nextLiteral is ToString(nextLiteralVal); R is R and it; the last " \
                     "literal returns R)") \
    X(SRAW_SUBST,    "22.1.2.4 steps 8.e-8.f (nextSub is ToString(substitutions[nextIndex]); R is R and it)")
enum { SRAW_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_string_raw_steps[] = { SRAW_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_string_raw_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSStringRaw *s = st;
    JSValue str;
    int r;

    if (s->hdr.stage == SRAW_TOOBJECT) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds, and it ends
           the buffer — one initialised late is freed with a NULL ctx. */
        string_buffer_init(ctx, &s->b, 0);
        s->cooked = JS_UNDEFINED; s->literals = JS_UNDEFINED; s->el = JS_UNDEFINED;
        s->len = 0; s->i = 0;
        s->cooked = JS_ToObject(ctx, step_arg(&s->hdr, 0));
        if (JS_IsException(s->cooked)) { s->cooked = JS_UNDEFINED; return -1; }
        s->hdr.stage = SRAW_RAW;
    }
    if (s->hdr.stage == SRAW_RAW) {
        r = step_getprop_run(ctx, &s->hdr, s->cooked, JS_ATOM_raw, cb_result, &s->el, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        JS_FreeValue(ctx, s->cooked); s->cooked = JS_UNDEFINED;
        s->literals = JS_ToObject(ctx, s->el);
        JS_FreeValue(ctx, s->el); s->el = JS_UNDEFINED;
        if (JS_IsException(s->literals)) { s->literals = JS_UNDEFINED; return -1; }
        s->hdr.stage = SRAW_LENGTH;
    }
    if (s->hdr.stage == SRAW_LENGTH) {
        r = step_length_run(ctx, &s->hdr, s->literals, cb_result, &s->len, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = SRAW_LITERAL;
    }
    for (;;) {
        if (s->hdr.stage == SRAW_LITERAL) {
            /* step 5 folds in here: literalCount <= 0 leaves the buffer empty, which IS the empty String. */
            if (s->i >= s->len) { JS_FreeValue(ctx, cb_result); break; }
            r = step_getidx_run(ctx, &s->hdr, s->literals, s->i, cb_result, &s->el, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->hdr.stage = SRAW_LITSTR;
        }
        if (s->hdr.stage == SRAW_LITSTR) {
            r = step_tostring_run(ctx, &s->hdr, s->el, cb_result, &str, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            JS_FreeValue(ctx, s->el); s->el = JS_UNDEFINED;
            if (string_buffer_concat_value_free(&s->b, str)) return -1;
            /* step 8.d: the LAST literal ends the string — the trailing substitution is never appended. */
            if (s->i + 1 >= s->len) break;
            s->hdr.stage = SRAW_SUBST;
        }
        /* step 8.e: the substitution, present only while there are any left */
        if (s->i + 1 < s->hdr.argc) {
            r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, (int)(s->i + 1)), cb_result, &str,
                                  out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            if (string_buffer_concat_value_free(&s->b, str)) return -1;
        } else {
            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        }
        s->i++;
        s->hdr.stage = SRAW_LITERAL;
    }
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The template and its `raw`, the literal held between its read
   and its coercion, and the accumulator — which needs no take_result guard here either: string_buffer_end NULLs
   the buffer on every path that consumed it, so the release below is right whether the result was produced or
   not. This machine is the one whose result IS the buffer, so the completion builds it before the release. */
static void js_string_raw_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSStringRaw *s = st;
    v->val(ctx, &s->cooked);
    v->val(ctx, &s->literals);
    v->val(ctx, &s->el);
    v->strbuf(ctx, &s->b);
}

static JSValue js_string_raw_fini(JSContext *ctx, void *st, bool take_result)
{
    JSStringRaw *s = st;
    JSValue r = take_result ? string_buffer_end(&s->b) : JS_UNDEFINED;
    return r;
}

/* only used in test262 */
JSValue js_string_codePointRange(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    uint32_t start, end, i, n;
    StringBuffer b_s, *b = &b_s;

    if (JS_ToUint32(ctx, &start, argv[0]) ||
        JS_ToUint32(ctx, &end, argv[1]))
        return JS_EXCEPTION;
    end = min_uint32(end, 0x10ffff + 1);

    if (start > end) {
        start = end;
    }
    n = end - start;
    if (end > 0x10000) {
        n += end - max_uint32(start, 0x10000);
    }
    if (string_buffer_init2(ctx, b, n, end >= 0x100))
        return JS_EXCEPTION;
    for(i = start; i < end; i++) {
        string_buffer_putc(b, i);
    }
    return string_buffer_end(b);
}


static int string_cmp(JSString *p1, JSString *p2, int x1, int x2, int len);
static int JS_ToUTF32String(JSContext *ctx, uint32_t **pbuf, JSValue val1);
static JSValue JS_NewUTF32String(JSContext *ctx, const uint32_t *buf, int len);

/* JS_ToInt32Clamp(v, 0, len, 0)'s arithmetic once its ToInt32Sat half is a step. */
/* JS_ToInt32Clamp's shape: a negative index is relative to `neg_off` (the string length for slice/substr's start,
   zero for a plain length), then the result is clamped into [0, hi]. */
static int str_clamp_off(int64_t v, int hi, int neg_off)
{
    if (v < 0) v += neg_off;
    if (v < 0) v = 0;
    else if (v > hi) v = hi;
    return (int)v;
}

static int str_clamp(int64_t v, int len)
{
    if (v < 0) v = 0;
    else if (v > len) v = len;
    return (int)v;
}

static int to_utf32_buf(JSContext *ctx, JSString *p, uint32_t **pbuf);
static JSValue js_string_case_body(JSContext *ctx, JSValueConst strv, int to_lower);
static JSValue js_string_CreateHTML(JSContext *ctx, JSValueConst strv, JSValueConst valuev, int magic);
/* which CreateHTML tags take an attribute, i.e. which perform step 4.a's ToString. It is a bitmask
   rather than a second copy of the defs table: the machine needs only that one bit, and two tables
   that must agree are two chances to disagree. Bits: a/name, font/color, font/size, a/href. */
#define STRRECV_HTML_HAS_ATTR ((1u << 0) | (1u << 5) | (1u << 6) | (1u << 8))

/* THIRTY-ONE ALGORITHMS, ONE MACHINE — so ONE stage list expanded once per algorithm with that algorithm's own
   step text (see AFIND_STAGES for the shape). The four stages are the machine's four rest points: the receiver's
   ToString, each argument's coercion in the order the spec performs them, and the computation after them, which
   runs none of the page's code and so names its whole range.
   AN ALGORITHM THAT COERCES NOTHING STILL DECLARES THE ARGUMENT STAGES, because the machine PASSES THROUGH them
   on its way to the body — it never rests there, and the label is what says so. Sharing the two sentences is not
   the drift the one-list rule exists to stop: they are not steps of anything, and a stage that names a step has
   its own text in every one of the thirty-one arrays. */
#define STRRECV_STAGES(X, THIS, ARG0, ARG1, BODY) \
    X(SRV_THIS, THIS) \
    X(SRV_ARG0, ARG0) \
    X(SRV_ARG1, ARG1) \
    X(SRV_BODY, BODY)
#define SRV_NO_ARG0 "no step: this algorithm has no argument to coerce, so it only passes through this stage"
#define SRV_NO_ARG1 "no step: this algorithm has no second argument to coerce, so it only passes through this stage"
enum { STRRECV_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0, 0) };

static const char *const js_str_trim_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.32 steps 1-2, TrimString steps 1-2 (S is ToString(RequireObjectCoercible(this)))",
        SRV_NO_ARG0,
        SRV_NO_ARG1,
        "22.1.3.32 TrimString steps 5-6 (T is S with leading AND trailing white space removed)")
    NULL };
static const char *const js_str_trimStart_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.34 steps 1-2, TrimString steps 1-2 (S is ToString(RequireObjectCoercible(this)))",
        SRV_NO_ARG0,
        SRV_NO_ARG1,
        "22.1.3.34 TrimString steps 3, 6 (T is S with leading white space removed)")
    NULL };
static const char *const js_str_trimEnd_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.33 steps 1-2, TrimString steps 1-2 (S is ToString(RequireObjectCoercible(this)))",
        SRV_NO_ARG0,
        SRV_NO_ARG1,
        "22.1.3.33 TrimString steps 4, 6 (T is S with trailing white space removed)")
    NULL };
static const char *const js_str_at_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.1 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue))",
        "22.1.3.1 step 5 (k is ToAbsoluteIndex(index, length)) - only its ToIntegerOrInfinity half runs here",
        SRV_NO_ARG1,
        "22.1.3.1 steps 4, 6-7 (length; undefined when out of range, else the substring) - and step 5's "
        "negative-relative half, which resolves with them")
    NULL };
static const char *const js_str_codePointAt_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.4 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue))",
        "22.1.3.4 step 4 (position is ToIntegerOrInfinity(position))",
        SRV_NO_ARG1,
        "22.1.3.4 steps 5-8 (size; undefined when out of range, else CodePointAt(string, position))")
    NULL };
static const char *const js_str_substring_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.25 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue))",
        "22.1.3.25 step 5 (finalStart clamps ToIntegerOrInfinity(start) between 0 and len)",
        "22.1.3.25 step 7 (finalEnd is len when end is undefined, else the same clamp of end)",
        "22.1.3.25 steps 4, 6 and 8-10 (len; the assert; from is the smaller and to the larger; the "
        "substring)")
    NULL };
static const char *const js_str_indexOf_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.9 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue))",
        "22.1.3.9 step 4 (searchString is set to ToString(searchString))",
        "22.1.3.9 step 6 (start clamps ToIntegerOrInfinity(position) between 0 and length) - only its "
        "ToIntegerOrInfinity half runs here",
        "22.1.3.9 steps 5, 7-10 (length; the assert; StringIndexOf(string, searchString, start); -1 when "
        "not-found) - and step 6's clamping half, which resolves with them")
    NULL };
static const char *const js_str_lastIndexOf_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.11 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue))",
        "22.1.3.11 step 4 (searchString is set to ToString(searchString))",
        "22.1.3.11 step 5 (numberPosition is ToNumber(position))",
        "22.1.3.11 steps 6-14 (length; searchLength; maxStart, and -1 when it is negative; start is "
        "maxStart when numberPosition is NaN, else the clamp into [0, maxStart]; the assert; "
        "StringLastIndexOf(string, searchString, start))")
    NULL };
static const char *const js_str_normalize_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.15 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue))",
        "22.1.3.15 steps 4-5 (form is set to \"NFC\" when it is undefined, else to ToString(form))",
        SRV_NO_ARG1,
        "22.1.3.15 steps 6-8 (the RangeError for an unknown form; normal is string normalized into form)")
    NULL };
static const char *const js_str_charAt_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.2 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue))",
        "22.1.3.2 step 4 (position is ToIntegerOrInfinity(position))",
        SRV_NO_ARG1,
        "22.1.3.2 steps 5-7 (size; the empty String when out of range, else the one-code-unit substring)")
    NULL };
static const char *const js_str_charCodeAt_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.3 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue))",
        "22.1.3.3 step 4 (position is ToIntegerOrInfinity(position))",
        SRV_NO_ARG1,
        "22.1.3.3 steps 5-7 (size; NaN when out of range, else the numeric value of the code unit)")
    NULL };
static const char *const js_str_slice_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.22 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue))",
        "22.1.3.22 step 5 (from is ToClampedIndex(start, len))",
        "22.1.3.22 step 6 (to is len when end is undefined, else ToClampedIndex(end, len))",
        "22.1.3.22 steps 4, 7-8 (len; an empty result when from is not below to; the substring)")
    NULL };
static const char *const js_str_substr_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "B.2.2.1 steps 1-3 (obj is RequireObjectCoercible'd; string is ToString(obj))",
        "B.2.2.1 step 5 (intStart is ToClampedIndex(start, size)) - only its ToIntegerOrInfinity half runs here",
        "B.2.2.1 step 6 (intLength is size when length is undefined, else the clamp of "
        "ToIntegerOrInfinity(length) into [0, size]) - only its ToIntegerOrInfinity half runs here",
        "B.2.2.1 steps 4, 7-8 (size; intEnd is min(intStart + intLength, size); the substring) - and step "
        "5's relative-index half, which resolves with them")
    NULL };
static const char *const js_str_repeat_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.18 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue))",
        "22.1.3.18 step 4 (n is ToIntegerOrInfinity(count))",
        SRV_NO_ARG1,
        "22.1.3.18 steps 5-7 (the RangeError for a negative or infinite n; the empty String when n is 0; "
        "n copies of string)")
    NULL };
static const char *const js_str_padStart_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.17 steps 1-2 (thisValue is RequireObjectCoercible'd), then step 3's 22.1.3.17.1 "
        "StringPaddingBuiltinsImpl step 1 (string is ToString(thisValue))",
        "22.1.3.17.1 StringPaddingBuiltinsImpl step 2 (intMaxLength is ToLength(maxLength))",
        "22.1.3.17.1 StringPaddingBuiltinsImpl steps 3-6 (only when intMaxLength exceeds stringLength: "
        "fillString is set to ToString(fillString), or to the code unit 0x0020 when it is undefined)",
        "22.1.3.17.1 StringPaddingBuiltinsImpl step 7, 22.1.3.17.2 StringPad (the fill repeated BEFORE "
        "string)")
    NULL };
static const char *const js_str_padEnd_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.16 steps 1-2 (thisValue is RequireObjectCoercible'd), then step 3's 22.1.3.17.1 "
        "StringPaddingBuiltinsImpl step 1 (string is ToString(thisValue))",
        "22.1.3.17.1 StringPaddingBuiltinsImpl step 2 (intMaxLength is ToLength(maxLength))",
        "22.1.3.17.1 StringPaddingBuiltinsImpl steps 3-6 (only when intMaxLength exceeds stringLength: "
        "fillString is set to ToString(fillString), or to the code unit 0x0020 when it is undefined)",
        "22.1.3.17.1 StringPaddingBuiltinsImpl step 7, 22.1.3.17.2 StringPad (the fill repeated AFTER "
        "string)")
    NULL };
static const char *const js_str_localeCmp_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.12 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue))",
        "22.1.3.12 step 4 (thatValue is ToString(that))",
        SRV_NO_ARG1,
        "22.1.3.12 states NO step for the comparison - its four steps only PREPARE the Strings, and the "
        "implementation-defined locale-sensitive comparison of string and thatValue is the section's prose")
    NULL };
static const char *const js_str_toLower_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.28 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue))",
        SRV_NO_ARG0,
        SRV_NO_ARG1,
        "22.1.3.28 steps 4-7 (sText is StringToCodePoints(string); lowerText is toLowercase(sText); "
        "lowercaseString is CodePointsToString(lowerText))")
    NULL };
static const char *const js_str_toUpper_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.30 states no algorithm, so these are 22.1.3.28 steps 1-3 (thisValue is "
        "RequireObjectCoercible'd; string is ToString(thisValue))",
        SRV_NO_ARG0,
        SRV_NO_ARG1,
        "22.1.3.30 states no algorithm - 22.1.3.28 steps 4-7 with toUppercase in place of toLowercase")
    NULL };
/* ECMA-402 §20.1.2 and ECMA-402 §20.1.3 STATE THEIR OWN THREE STEPS, which is why these cite them directly rather than
   deferring the way js_str_toUpper_steps has to defer to 22.1.3.28. Stage SRV_ARG0 is the one that matters: it
   is TransformCase step 1, the algorithm's FIRST act after the receiver's ToString, and every rest point the
   locale layer will ever need is inside it — ECMA-402 §9.2.1's loop runs ToObject, LengthOfArrayLike, HasProperty, Get
   and ToString, all of which run the page's code. A label is a parked machine's identity across builds, so
   these say what the stage rests at and not what it currently does. */
static const char *const js_str_toLocaleLower_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "ECMA-402 §20.1.2 steps 1-2 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue))",
        "ECMA-402 §20.1.2 step 3 into ECMA-402 §20.1.2.1 TransformCase step 1 (requestedLocales is "
        "CanonicalizeLocaleList(locales), whose ECMA-402 §9.2.1 loop is where the page's code runs)",
        SRV_NO_ARG1,
        "ECMA-402 §20.1.2.1 TransformCase steps 2-10 (requestedLocale, the Available Locales List, "
        "LookupMatchingLocaleByPrefix, locale, and the lowercase transformation of codePoints)")
    NULL };
static const char *const js_str_toLocaleUpper_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "ECMA-402 §20.1.3 steps 1-2 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue))",
        "ECMA-402 §20.1.3 step 3 into ECMA-402 §20.1.2.1 TransformCase step 1 (requestedLocales is "
        "CanonicalizeLocaleList(locales), whose ECMA-402 §9.2.1 loop is where the page's code runs)",
        SRV_NO_ARG1,
        "ECMA-402 §20.1.2.1 TransformCase steps 2-10 (requestedLocale, the Available Locales List, "
        "LookupMatchingLocaleByPrefix, locale, and the uppercase transformation of codePoints)")
    NULL };
static const char *const js_str_anchor_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "B.2.2.2 step 2, CreateHTML steps 1-2 (contentsString is ToString(RequireObjectCoercible(string)))",
        "B.2.2.2 step 2, CreateHTML step 4.a (attrValueString is ToString(attrValue) — the \"name\" attribute)",
        SRV_NO_ARG1,
        "B.2.2.2 step 2, CreateHTML steps 3-8 (<a> around contentsString, with the attribute quoted and its quotation marks escaped)")
    NULL };
static const char *const js_str_big_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "B.2.2.3 step 2, CreateHTML steps 1-2 (contentsString is ToString(RequireObjectCoercible(string)))",
        "B.2.2.3 — no step: CreateHTML step 4 does not run for <big>, which takes no attribute",
        SRV_NO_ARG1,
        "B.2.2.3 step 2, CreateHTML steps 3-8 (<big> around contentsString)")
    NULL };
static const char *const js_str_blink_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "B.2.2.4 step 2, CreateHTML steps 1-2 (contentsString is ToString(RequireObjectCoercible(string)))",
        "B.2.2.4 — no step: CreateHTML step 4 does not run for <blink>, which takes no attribute",
        SRV_NO_ARG1,
        "B.2.2.4 step 2, CreateHTML steps 3-8 (<blink> around contentsString)")
    NULL };
static const char *const js_str_bold_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "B.2.2.5 step 2, CreateHTML steps 1-2 (contentsString is ToString(RequireObjectCoercible(string)))",
        "B.2.2.5 — no step: CreateHTML step 4 does not run for <b>, which takes no attribute",
        SRV_NO_ARG1,
        "B.2.2.5 step 2, CreateHTML steps 3-8 (<b> around contentsString)")
    NULL };
static const char *const js_str_fixed_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "B.2.2.6 step 2, CreateHTML steps 1-2 (contentsString is ToString(RequireObjectCoercible(string)))",
        "B.2.2.6 — no step: CreateHTML step 4 does not run for <tt>, which takes no attribute",
        SRV_NO_ARG1,
        "B.2.2.6 step 2, CreateHTML steps 3-8 (<tt> around contentsString)")
    NULL };
static const char *const js_str_fontcolor_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "B.2.2.7 step 2, CreateHTML steps 1-2 (contentsString is ToString(RequireObjectCoercible(string)))",
        "B.2.2.7 step 2, CreateHTML step 4.a (attrValueString is ToString(attrValue) — the \"color\" attribute)",
        SRV_NO_ARG1,
        "B.2.2.7 step 2, CreateHTML steps 3-8 (<font> around contentsString, with the attribute quoted and its quotation marks escaped)")
    NULL };
static const char *const js_str_fontsize_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "B.2.2.8 step 2, CreateHTML steps 1-2 (contentsString is ToString(RequireObjectCoercible(string)))",
        "B.2.2.8 step 2, CreateHTML step 4.a (attrValueString is ToString(attrValue) — the \"size\" attribute)",
        SRV_NO_ARG1,
        "B.2.2.8 step 2, CreateHTML steps 3-8 (<font> around contentsString, with the attribute quoted and its quotation marks escaped)")
    NULL };
static const char *const js_str_italics_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "B.2.2.9 step 2, CreateHTML steps 1-2 (contentsString is ToString(RequireObjectCoercible(string)))",
        "B.2.2.9 — no step: CreateHTML step 4 does not run for <i>, which takes no attribute",
        SRV_NO_ARG1,
        "B.2.2.9 step 2, CreateHTML steps 3-8 (<i> around contentsString)")
    NULL };
static const char *const js_str_link_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "B.2.2.10 step 2, CreateHTML steps 1-2 (contentsString is ToString(RequireObjectCoercible(string)))",
        "B.2.2.10 step 2, CreateHTML step 4.a (attrValueString is ToString(attrValue) — the \"href\" attribute)",
        SRV_NO_ARG1,
        "B.2.2.10 step 2, CreateHTML steps 3-8 (<a> around contentsString, with the attribute quoted and its quotation marks escaped)")
    NULL };
static const char *const js_str_small_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "B.2.2.11 step 2, CreateHTML steps 1-2 (contentsString is ToString(RequireObjectCoercible(string)))",
        "B.2.2.11 — no step: CreateHTML step 4 does not run for <small>, which takes no attribute",
        SRV_NO_ARG1,
        "B.2.2.11 step 2, CreateHTML steps 3-8 (<small> around contentsString)")
    NULL };
static const char *const js_str_strike_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "B.2.2.12 step 2, CreateHTML steps 1-2 (contentsString is ToString(RequireObjectCoercible(string)))",
        "B.2.2.12 — no step: CreateHTML step 4 does not run for <strike>, which takes no attribute",
        SRV_NO_ARG1,
        "B.2.2.12 step 2, CreateHTML steps 3-8 (<strike> around contentsString)")
    NULL };
static const char *const js_str_sub_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "B.2.2.13 step 2, CreateHTML steps 1-2 (contentsString is ToString(RequireObjectCoercible(string)))",
        "B.2.2.13 — no step: CreateHTML step 4 does not run for <sub>, which takes no attribute",
        SRV_NO_ARG1,
        "B.2.2.13 step 2, CreateHTML steps 3-8 (<sub> around contentsString)")
    NULL };
static const char *const js_str_sup_steps[] = {
    STRRECV_STAGES(JS_STEP_STAGE_LABEL,
        "B.2.2.14 step 2, CreateHTML steps 1-2 (contentsString is ToString(RequireObjectCoercible(string)))",
        "B.2.2.14 — no step: CreateHTML step 4 does not run for <sup>, which takes no attribute",
        SRV_NO_ARG1,
        "B.2.2.14 step 2, CreateHTML steps 3-8 (<sup> around contentsString)")
    NULL };

/* ============ ECMA-402 §6.2.1 IsWellFormedLanguageTag, and the ECMA-402 §15.5 operations it reads ===========
   THIS IS THE FIRST HOP OF THE LOCALE LAYER THE `locales` ARGUMENT STAGE BELOW CRASHES FOR, AND ITS BOUNDARY
   IS NOT A CONVENIENCE — IT IS WHERE THE ALGORITHM STOPS NEEDING DATA. Everything here is a predicate over
   ASCII bytes and a grammar printed in one table of UTS #35 Part 1 Core; the very next operation ECMA-402
   §9.2.1 calls, ECMA-402 §6.2.2 CanonicalizeUnicodeLocaleId, cannot be written at all without CLDR's alias
   table, because its step 1 is UTS #35 Part 1 Core Annex C LocaleId Canonicalization. So the two are split
   HERE rather than at some point chosen for size, and the split is checkable: this block reads no table.

   WHY IT LIVES IN quickjs.c RATHER THAN UNDER engine/host, WHICH IS A DECISION AND NOT AN ACCIDENT.
   `engine/build.mjs` compiles the fork's four translation units and every browser and solver source into ONE
   program with ONE include set (`ENGINE_INCLUDE_ROOTS` = qjs, host, host/browser, lexbor), so a host header
   CAN be included here and a host function CAN be called from here. Nothing in the build stops it. What stops
   it is that the dependency has never gone this way: every `#include` in this file names a fork-local header,
   `engine/host` has many files including "quickjs.h" and `engine/qjs` has none including a host header, and
   `quickjs-check.h` exists as a MIRROR of `engine/host/check.h` for exactly that reason. Putting an operation
   this file must call under `engine/host` would make the fork depend on the host — a LARGER delta, not a
   smaller one — and would falsify the mirror's own argument. The alternative, a new `.c` beside this one, is
   worse for a different reason: `engine/citegen.mjs`'s `walk()` skips the whole `qjs` directory and
   `defaultTargets()` re-adds exactly `qjs/quickjs.c` and `qjs/quickjs.h` BY NAME, so a new file here would be
   permanently unaudited and every citation in this banner would be read by nothing. quickjs.c is the one
   place that is both callable and audited, and ECMA-402 is this engine's own standard rather than the
   browser's. RETIREMENT: this paragraph goes when the fork declares a supported direction for that dependency
   — either a hook table entry the host installs, the way JSFlowControlHooks and JSTimeTravelHooks already do,
   or a stated licence to include host headers — at which point the choice is made by that declaration
   instead of by this comment.

   THE GRAMMAR, QUOTED FROM THE TABLES IN UTS #35 Part 1 Core "Unicode Language and Locale Identifiers":
     unicode_language_id = "root" | (unicode_language_subtag (sep unicode_script_subtag)?
                                     | unicode_script_subtag) (sep unicode_region_subtag)?
                                    (sep unicode_variant_subtag)* ;
     unicode_language_subtag = alpha{2,3} | alpha{5,8};   unicode_script_subtag = alpha{4} ;
     unicode_region_subtag   = (alpha{2} | digit{3}) ;
     unicode_variant_subtag  = (alphanum{5,8}| digit alphanum{3}) ;
     unicode_locale_id = unicode_language_id  extensions*  pu_extensions? ;
     extensions = unicode_locale_extensions| transformed_extensions | other_extensions ;
     unicode_locale_extensions = sep [uU]  ((sep keyword)+  |(sep uattribute)+ (sep ufield)*) ;
     transformed_extensions = sep [tT]  ((sep tlang (sep tfield)*)  | (sep tfield)+) ;
     pu_extensions = sep [xX]  (sep alphanum{1,8})+ ;
     other_extensions = sep [alphanum-[tTuUxX]]  (sep alphanum{2,8})+ ;
     ufield = ukey (sep uvalue)? ;   ukey = alphanum alpha ;   uvalue = alphanum{3,8} (sep alphanum{3,8})* ;
     uattribute = alphanum{3,8} ;
     tlang = unicode_language_subtag (sep unicode_script_subtag)? (sep unicode_region_subtag)?
             (sep unicode_variant_subtag)* ;
     tfield = tkey tvalue;   tkey = alpha digit ;   tvalue = alphanum{3,8}  (sep alphanum{3,8})+ ;
     sep = [-_] ;  digit = [0-9] ;  alpha = [A-Z a-z] ;  alphanum = [0-9 A-Z a-z] ;

   TWO OF THOSE LINES ARE NOT IMPLEMENTED AS PRINTED, AND THE REASON IS THAT AS PRINTED THEY MATCH NOTHING AT
   ALL — which is a stronger ground than preferring one reading to another. `tfield = tkey tvalue` puts no
   `sep` between the two, and `tkey` is exactly two characters while `tvalue` begins with `alphanum{3,8}`; a
   `tkey` abutting a `tvalue` with no separator is a single run of five or more alphanumerics, and no
   production admits one in that position. So the literal reading makes `transformed_extensions` UNMATCHABLE,
   and with it every `-t-` tag that has ever been written. The document's own examples settle what was meant,
   and they are not one or two: `af-t-k0-android`, `ja-t-k0-chromeos`, `en-t-hi-h0-hybrid`,
   `hi-t-en-h0-hybrid`, `en-Deva-t-hi-h0-hybrid`, `hi-Latn-t-en-h0-hybrid`, `ja-Kana-JP-t-it-latn-it`,
   `en-t-zh-hant`, `en-t-xxx-u-yyy` and `en-u-yyy-t-xxx` all appear in UTS #35 Part 1 Core. Each is a `tkey`
   followed by a separator and its value subtags, and `k0-android` carries exactly ONE value subtag where
   `tvalue`'s printed `(sep alphanum{3,8})+` demands two — while the sibling `uvalue` one row up is printed
   with a `*`. So this implements `tfield` as `tkey (sep alphanum{3,8})+`, which is the only reading under
   which the ten examples are well-formed. HOW A WRONG CHOICE HERE WOULD SHOW: `"x".toLocaleLowerCase("en-t-hi-h0-hybrid")`
   throwing a RangeError, where a browser returns a string.

   AND THE WELL-FORMEDNESS CONSTRAINTS PRINTED UNDER THAT TABLE ARE DELIBERATELY NOT ENFORCED HERE, WHICH IS
   NOT AN OMISSION BUT WHAT ECMA-402 §6.2.1's OWN SHAPE REQUIRES. Its step 2 asks whether lowerLocale "can be
   matched by the unicode_locale_id Unicode locale nonterminal" — the NONTERMINAL, which is the EBNF — and
   then its steps 6.a and 11.a go on to check for duplicate variant subtags and duplicate singleton subtags
   BY HAND. Those two are among the constraints printed beside the grammar, so a matcher that enforced them
   would make ECMA-402 §6.2.1's own steps dead code, and a reader would then be unable to tell which of the
   two places decides. One constraint is therefore enforced by the caller and one by this matcher only where
   the EBNF itself carries it: `extensions* pu_extensions?` is why a `-x-` sequence must come last, and that
   IS in the grammar. The constraint that "there cannot be more than one ukey or tkey" is enforced NOWHERE,
   by neither this matcher nor ECMA-402 §6.2.1, so `en-u-ca-buddhist-ca-islamic` is well-formed as far as this
   engine's answer to that question goes — which is what the algorithm says, and is a fact about ECMA-402
   rather than about this file.  =========================================================================== */

/* THE ALPHABET. The buffer these read is ASCII-lowercased by ECMA-402 §6.2.1 step 1 before anything else
   looks at it, so `A`-`Z` cannot occur — but the range stays in the predicate rather than being dropped,
   because these are the UTS #35 character classes and a class narrowed to what one caller happens to pass is
   a class that means something different from the one it is named after. */
static bool js_loc_is_alpha(int c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool js_loc_is_digit(int c)  { return c >= '0' && c <= '9'; }
static bool js_loc_is_alnum(int c)  { return js_loc_is_alpha(c) || js_loc_is_digit(c); }
static bool js_loc_is_sep(int c)    { return c == '-' || c == '_'; }

static bool js_loc_all_alpha(const char *s, int i, int n)
{ while (n-- > 0) if (!js_loc_is_alpha((unsigned char)s[i++])) return false; return true; }
static bool js_loc_all_digit(const char *s, int i, int n)
{ while (n-- > 0) if (!js_loc_is_digit((unsigned char)s[i++])) return false; return true; }
static bool js_loc_all_alnum(const char *s, int i, int n)
{ while (n-- > 0) if (!js_loc_is_alnum((unsigned char)s[i++])) return false; return true; }

/* EVERY PRODUCTION IN THAT TABLE REQUIRES A NON-EMPTY SUBTAG, so a leading separator, a trailing separator or
   two in a row cannot be matched by any of them. Rejecting all three up front is not an optimisation: it is
   what lets every cursor below take "the subtag at `i` has length >= 1" for granted, and in particular what
   makes `i - 1` the end of the region a matcher consumed. Without it `"en-"` would run the cursor to `len`
   with a zero-length subtag nobody ever examined and be accepted. */
static bool js_loc_no_empty_subtag(const char *s, int len)
{
    int i;
    if (len == 0) return false;
    if (js_loc_is_sep((unsigned char)s[0]) || js_loc_is_sep((unsigned char)s[len - 1])) return false;
    for (i = 1; i < len; i++)
        if (js_loc_is_sep((unsigned char)s[i]) && js_loc_is_sep((unsigned char)s[i - 1])) return false;
    return true;
}

/* THE ONE CURSOR CONVENTION, STATED ONCE BECAUSE EVERY MATCHER BELOW SHARES IT. A cursor is the index at
   which the NEXT UNCONSUMED SUBTAG STARTS, and it equals `len` when there is nothing left — never the index
   of a separator. `*pnext` is that cursor for the subtag beginning at `i`, and the return is that subtag's
   length. A matcher returns the new cursor, or -1 for no match; the END of what it consumed is `len` when
   the cursor reached the end and `cursor - 1` otherwise, which is exact precisely because the check above
   has already refused a doubled separator. */
static int js_loc_subtag(const char *s, int len, int i, int *pnext)
{
    int j = i;
    while (j < len && !js_loc_is_sep((unsigned char)s[j])) j++;
    *pnext = (j < len) ? j + 1 : len;
    return j - i;
}
static int js_loc_consumed_end(int len, int cursor) { return cursor >= len ? len : cursor - 1; }

static bool js_loc_is_language_subtag(const char *s, int i, int n)
{ return (n == 2 || n == 3 || (n >= 5 && n <= 8)) && js_loc_all_alpha(s, i, n); }
static bool js_loc_is_script_subtag(const char *s, int i, int n)
{ return n == 4 && js_loc_all_alpha(s, i, n); }
static bool js_loc_is_region_subtag(const char *s, int i, int n)
{ return (n == 2 && js_loc_all_alpha(s, i, n)) || (n == 3 && js_loc_all_digit(s, i, n)); }
static bool js_loc_is_variant_subtag(const char *s, int i, int n)
{ return (n >= 5 && n <= 8 && js_loc_all_alnum(s, i, n)) ||
         (n == 4 && js_loc_is_digit((unsigned char)s[i]) && js_loc_all_alnum(s, i, n)); }

/* `unicode_language_id`, and `tlang` when `allow_script_first` is false — ONE function because the two
   productions differ in exactly that clause: `tlang` is spelled `unicode_language_subtag (sep
   unicode_script_subtag)? (sep unicode_region_subtag)? (sep unicode_variant_subtag)*`, which is
   `unicode_language_id`'s second alternative with the bare-script opening removed. Two copies of one
   production is the shape that drifts.
   GREEDY WITH NO BACKTRACKING, AND THAT IS A PROPERTY OF THE GRAMMAR RATHER THAN AN ASSUMPTION. At every
   choice point the candidate productions are disjoint on the SHAPE of one subtag: a script is four alphas, a
   region is two alphas or three digits, a variant is five-to-eight alphanumerics or four beginning with a
   digit, and a language subtag is two, three, or five-to-eight alphas. The only length two productions share
   is four, where script is all-alpha and variant is digit-first. So no subtag can be taken by two of them and
   there is nothing a longer match could be traded for.
   The `"root"` alternative of `unicode_language_id` needs no arm of its own: `root` is four alphas, so the
   second alternative already matches it as a bare `unicode_script_subtag`, and does so with the region and
   variant suffixes the `"root"` arm has none of — which is the LONGER match, and length is what the one
   caller that cares (ECMA-402 §15.5.1) asks for. ECMA-402 §6.2.1 step 3 is where a leading `root` is
   refused, and it refuses it by name. */
static int js_loc_match_language_id(const char *s, int len, int i, bool allow_script_first)
{
    int n, next;
    if (i >= len) return -1;
    n = js_loc_subtag(s, len, i, &next);
    if (js_loc_is_language_subtag(s, i, n)) {
        i = next;
        if (i < len) {
            n = js_loc_subtag(s, len, i, &next);
            if (js_loc_is_script_subtag(s, i, n)) i = next;
        }
    } else if (allow_script_first && js_loc_is_script_subtag(s, i, n)) {
        i = next;
    } else {
        return -1;
    }
    if (i < len) {
        n = js_loc_subtag(s, len, i, &next);
        if (js_loc_is_region_subtag(s, i, n)) i = next;
    }
    for (;;) {
        if (i >= len) break;
        n = js_loc_subtag(s, len, i, &next);
        if (!js_loc_is_variant_subtag(s, i, n)) break;
        i = next;
    }
    return i;
}

/* ONE `extensions` ALTERNATIVE, entered with `i` at the SINGLETON subtag. `pu_extensions` is deliberately not
   one of the arms: `unicode_locale_id` is `unicode_language_id extensions* pu_extensions?`, so a private-use
   sequence is not an extension that happens to come last, it is a separate trailing production — which is
   exactly what makes "the private use extension must come after all other extensions" a rule the EBNF itself
   carries rather than one a caller has to check. Returning -1 for `x` is therefore the grammar, not a guard. */
static int js_loc_match_extension(const char *s, int len, int i)
{
    int n, next, c, count;
    n = js_loc_subtag(s, len, i, &next);
    if (n != 1 || !js_loc_is_alnum((unsigned char)s[i])) return -1;
    c = (unsigned char)s[i];
    i = next;
    if (c == 'x' || c == 'X') return -1;
    if (c == 'u' || c == 'U') {
        /* `(sep keyword)+ | (sep uattribute)+ (sep ufield)*`, where the table's own parenthetical says a
           `keyword` IS a `ufield`. Both alternatives are therefore a run of subtags each of which is a
           `ukey` (`alphanum alpha`, so exactly two characters with an alpha second) or a three-to-eight
           alphanumeric run — the latter reading as a `uattribute` before the first `ukey` and as part of a
           `uvalue` after it. Nothing in the grammar distinguishes those two by shape, and nothing needs to:
           whichever it is, the same subtag is admitted, so one loop is the whole of both alternatives. What
           the alternation does require is that the run be non-empty, which `count` carries. A two-character
           subtag whose second character is a digit is neither, and ends the run — after which the caller
           will try to read it as the next singleton, fail on its length, and reject the tag. */
        count = 0;
        for (;;) {
            if (i >= len) break;
            n = js_loc_subtag(s, len, i, &next);
            if (n == 2 && js_loc_is_alnum((unsigned char)s[i]) && js_loc_is_alpha((unsigned char)s[i + 1])) {
                /* ukey */
            } else if (n >= 3 && n <= 8 && js_loc_all_alnum(s, i, n)) {
                /* uattribute, or a subtag of a uvalue */
            } else {
                break;
            }
            count++;
            i = next;
        }
        return count >= 1 ? i : -1;
    }
    if (c == 't' || c == 'T') {
        /* `(sep tlang (sep tfield)*) | (sep tfield)+` — the alternation is decided by ONE subtag and needs no
           lookahead, because a `tkey` is `alpha digit` and every subtag a `tlang` may open with is all-alpha. */
        count = 0;
        if (i < len) {
            n = js_loc_subtag(s, len, i, &next);
            if (js_loc_is_language_subtag(s, i, n)) {
                int j = js_loc_match_language_id(s, len, i, false);
                if (j < 0) return -1;
                i = j;
                count++;
            }
        }
        for (;;) {
            int vals;
            if (i >= len) break;
            n = js_loc_subtag(s, len, i, &next);
            if (n != 2 || !js_loc_is_alpha((unsigned char)s[i]) || !js_loc_is_digit((unsigned char)s[i + 1]))
                break;
            /* `tfield = tkey tvalue` read as `tkey (sep alphanum{3,8})+` — see the banner above: as printed
               it has no separator between the two and `tvalue` demands two value subtags, and under that
               reading no `-t-` tag the standard itself prints is well-formed. */
            i = next;
            vals = 0;
            for (;;) {
                if (i >= len) break;
                n = js_loc_subtag(s, len, i, &next);
                if (!(n >= 3 && n <= 8 && js_loc_all_alnum(s, i, n))) break;
                vals++;
                i = next;
            }
            if (vals < 1) return -1;
            count++;
        }
        return count >= 1 ? i : -1;
    }
    /* `other_extensions = sep [alphanum-[tTuUxX]] (sep alphanum{2,8})+` */
    count = 0;
    for (;;) {
        if (i >= len) break;
        n = js_loc_subtag(s, len, i, &next);
        if (!(n >= 2 && n <= 8 && js_loc_all_alnum(s, i, n))) break;
        count++;
        i = next;
    }
    return count >= 1 ? i : -1;
}

/* `pu_extensions = sep [xX] (sep alphanum{1,8})+` */
static int js_loc_match_pu(const char *s, int len, int i)
{
    int n, next, count;
    n = js_loc_subtag(s, len, i, &next);
    if (n != 1 || (s[i] != 'x' && s[i] != 'X')) return -1;
    i = next;
    count = 0;
    for (;;) {
        if (i >= len) break;
        n = js_loc_subtag(s, len, i, &next);
        if (!(n >= 1 && n <= 8 && js_loc_all_alnum(s, i, n))) break;
        count++;
        i = next;
    }
    return count >= 1 ? i : -1;
}

/* `unicode_locale_id = unicode_language_id extensions* pu_extensions?`, matched against the WHOLE region — a
   prefix match is not what ECMA-402 §6.2.1 step 2 asks about, and the trailing `i == len` is the difference. */
static bool js_loc_match_locale_id(const char *s, int len)
{
    int i;
    if (!js_loc_no_empty_subtag(s, len)) return false;
    i = js_loc_match_language_id(s, len, 0, true);
    if (i < 0) return false;
    for (;;) {
        int j = js_loc_match_extension(s, len, i);
        if (j < 0) break;
        i = j;
    }
    if (i < len) {
        int j = js_loc_match_pu(s, len, i);
        if (j < 0) return false;
        i = j;
    }
    return i == len;
}

/* ECMA-402 §15.5.1 GetLocaleBaseName ( locale ) — "Return the longest prefix of locale matched by the
   unicode_language_id Unicode locale nonterminal", reported as a LENGTH because every caller wants a region
   of the buffer it already holds and a copy would be a second string that can disagree with the first. */
static int js_loc_base_name_len(const char *s, int len)
{
    int cur = js_loc_match_language_id(s, len, 0, true);
    DCHECK(cur >= 0, "ECMA-402 §15.5.1 GetLocaleBaseName step 1 asserts that its argument can be matched by "
                     "the unicode_locale_id Unicode locale nonterminal, and a unicode_locale_id opens with a "
                     "unicode_language_id — so a locale reaching here with no such prefix is one that never "
                     "went through ECMA-402 §6.2.1 step 2, which is the only caller that establishes it");
    return js_loc_consumed_end(len, cur);
}

/* ECMA-402 §15.5.5 GetLocaleVariants ( locale ) — `undefined` is a FALSE return rather than an empty region,
   because ECMA-402 §6.2.1 steps 6 and 11.g both test "is not undefined" before looking inside, and an empty
   region would answer that test the same way a present-but-empty one would. The String it otherwise returns
   is `s[*poff .. *poff + *pn)`.
   ITS STEP 3 SPLITS ON "-" AND THIS SPLITS ON THE UTS #35 `sep`, WHICH IS `[-_]`. The two agree only because
   ECMA-402 §6.2.1 step 3 has already refused every tag containing `_` before step 5 makes the first call, and
   the tlang handed to the second call at step 11.f is a substring of that same refused-or-clean buffer. That
   is a derivation about the ORDER of one algorithm's steps rather than a property of this function, so it is
   asserted here instead of trusted. */
static bool js_loc_variants(const char *s, int len, int *poff, int *pn)
{
    int bn, dash, i, next, n;
    DCHECK(memchr(s, '_', (size_t)len) == NULL,
           "ECMA-402 §15.5.5 GetLocaleVariants step 3 splits on \"-\" alone, and this walk treats the "
           "UTS #35 `sep` [-_] as the separator — which is the same split only for a locale ECMA-402 §6.2.1 "
           "step 3 has already refused for using the backwards-compatibility `_`; a caller that skipped "
           "step 3 would silently get a different set of subtags here than the standard names");
    bn = js_loc_base_name_len(s, len);
    n = js_loc_subtag(s, bn, 0, &next);
    dash = n;
    i = next;
    while (i < bn) {
        n = js_loc_subtag(s, bn, i, &next);
        if (js_loc_is_variant_subtag(s, i, n)) { *poff = dash + 1; *pn = bn - (dash + 1); return true; }
        dash = dash + n + 1;
        i = next;
    }
    return false;
}

/* ECMA-402 §6.2.1 step 6.a and step 11.g: "If variants contains any duplicate subtags, return false." */
static bool js_loc_dup_variant_subtags(const char *s, int off, int end)
{
    int i = off, n, next, j, jn, jnext;
    while (i < end) {
        n = js_loc_subtag(s, end, i, &next);
        for (j = next; j < end; j = jnext) {
            jn = js_loc_subtag(s, end, j, &jnext);
            if (jn == n && memcmp(s + i, s + j, (size_t)n) == 0) return true;
        }
        i = next;
    }
    return false;
}

/* ECMA-402 §6.2.1 step 11.a: "If extensions contains any duplicate singleton subtags, return false." A
   singleton is a one-character subtag, which in this region is an extension's leading letter and nothing
   else — every other production here is two characters or more. */
static bool js_loc_dup_singletons(const char *s, int off, int end)
{
    int i = off, n, next, j, jn, jnext;
    while (i < end) {
        n = js_loc_subtag(s, end, i, &next);
        if (n == 1) {
            for (j = next; j < end; j = jnext) {
                jn = js_loc_subtag(s, end, j, &jnext);
                if (jn == 1 && s[i] == s[j]) return true;
            }
        }
        i = next;
    }
    return false;
}

/* ECMA-402 §6.2.1 IsWellFormedLanguageTag ( locale ), over the ASCII-lowercased buffer its step 1 produces.
   ITS STEPS 2 AND 3 TOGETHER ARE WHAT THE OPERATION'S OWN DESCRIPTION CALLS A `unicode_bcp47_locale_id`, and
   seeing that is what makes step 3 checkable instead of a list to be copied. The description says it
   "determines whether locale is a well-formed language tag conforming with the well-formedness constraints
   of a unicode_bcp47_locale_id" while step 2 names the wider `unicode_locale_id`; UTS #35 Part 1 Core gives
   the difference as two constraints — "The EBNF sep is restricted to only [-]" and "The first subtag must be
   a unicode_language_subtag", the latter excluding both a script subtag and a "root" subtag — and its
   Section 3.3 BCP 47 Conformance lists the same three things as the syntax allowed "for backwards
   compatibility (not BCP 47-compatible)": the "_" separator, the "root" subtag, and a tag that "may begin
   with a script subtag rather than a language subtag". Step 3 is those three, and they are exactly the gap
   between the two nonterminals. The `root` test is REDUNDANT with the script test and is written anyway:
   `root` is four alphas, so step 2 can only have matched it as a `unicode_script_subtag`, and a reader who
   deletes the script arm would otherwise leave a clause of Section 3.3 unenforced with nothing to say so. */
static bool js_loc_is_well_formed(const char *s, int len)
{
    int bn, n, next, i, xend, voff, vn, tsing, tend, tl, tlend;

    if (!js_loc_match_locale_id(s, len)) return false;                       /* step 2 */

    for (i = 0; i < len; i++) if (s[i] == '_') return false;                 /* step 3, the `_` separator */
    n = js_loc_subtag(s, len, 0, &next);
    if (n == 4 && memcmp(s, "root", 4) == 0) return false;                   /* step 3, the `root` subtag */
    if (js_loc_is_script_subtag(s, 0, n)) return false;                      /* step 3, a leading script */

    bn = js_loc_base_name_len(s, len);                                       /* step 4 */
    if (js_loc_variants(s, bn, &voff, &vn))                                  /* step 5 */
        if (js_loc_dup_variant_subtags(s, voff, voff + vn)) return false;    /* step 6 */

    /* steps 7-10. `extensions` is the suffix of lowerLocale following baseName, so it is the region
       [bn, len) and it opens with a separator; step 9's StringIndexOf is a LITERAL search for "-x-", which
       can only land on a one-character `x` subtag because every subtag is alphanumeric, and a one-character
       `x` subtag can only be the opening of `pu_extensions` — `other_extensions` excludes [tTuUxX] by name
       and no other production admits a single character. So the truncation cuts exactly at the private-use
       sequence, which is what leaves a region that is still a whole number of `extensions`. */
    xend = len;
    for (i = bn; i + 3 <= len; i++)
        if (s[i] == '-' && s[i + 1] == 'x' && s[i + 2] == '-') { xend = i; break; }

    if (xend <= bn) return true;                                             /* step 11's test */
    if (js_loc_dup_singletons(s, bn + 1, xend)) return false;                /* step 11.a */

    /* step 11.b: the longest substring of extensions matched by `transformed_extensions`. Step 11.a has just
       refused a repeated singleton, so there is at most one `-t-` sequence and "longest" names it uniquely. */
    tsing = -1; tend = -1;
    i = bn + 1;
    for (;;) {
        int j = js_loc_match_extension(s, xend, i);
        if (j < 0) break;
        if (s[i] == 't') { tsing = i; tend = js_loc_consumed_end(xend, j); }
        i = j;
    }
    if (tsing < 0) return true;                                              /* step 11.b's early return */

    /* step 11.c's assert, and step 11.d's `tPrefix` = the substring from 3, which is (tsing-1)+3. */
    DCHECK(s[tsing - 1] == '-' && s[tsing] == 't' && s[tsing + 1] == '-',
           "ECMA-402 §6.2.1 step 11.c asserts that the substring of transformExtension from 0 to 3 is "
           "\"-t-\", and this transformExtension was located by walking `extensions` from its own leading "
           "separator, so a failure means the walk and the region disagree about where an extension begins");
    tl = js_loc_match_language_id(s, tend, tsing + 2, false);                /* step 11.e */
    if (tl < 0) return true;                                                 /* step 11.e's early return */
    tlend = js_loc_consumed_end(tend, tl);
    if (js_loc_variants(s + tsing + 2, tlend - (tsing + 2), &voff, &vn))     /* step 11.f */
        if (js_loc_dup_variant_subtags(s + tsing + 2, voff, voff + vn)) return false;  /* step 11.g */
    return true;                                                             /* step 12 */
}

/* ECMA-402 §6.2.1 step 1's `lowerLocale`, materialized as ASCII bytes: 0 built, 1 not ASCII, -1 exception.
   THREE OUTCOMES AND THREE ANSWERS, because a NULL buffer would say "not a language tag" and "the allocation
   failed" with one value, and those are not the same fact — one is a page's input and the other is this
   engine's floor.
   A CODE UNIT ABOVE 0x7F IS REFUSED RATHER THAN COPIED, and that is the same answer by the same reasoning one
   allocation earlier: the ASCII-lowercase of such a unit is itself, and every terminal in the UTS #35 grammar
   is an ASCII alphanumeric or a separator, so ECMA-402 §6.2.1 step 2 cannot match a string containing one.
   IT WALKS THE ROPE ITERATOR RATHER THAN `JS_VALUE_GET_STRING`, WHICH IS NOT A REFINEMENT — IT IS THE
   DIFFERENCE BETWEEN READING THE STRING AND READING A DIFFERENT STRUCT. `JS_IsString` answers true for
   JS_TAG_STRING_ROPE as well as JS_TAG_STRING, and `JS_VALUE_GET_STRING` on a rope hands back a
   `JSStringRope *` wearing a `JSString *`'s type, so `p->len` would be read out of a node that has no such
   field. `string_rope_iter_next` returns a flat leaf for either tag — its first act is to return the value
   itself when the tag is JS_TAG_STRING — so one walk covers both and there is no arm to forget.
   NOTHING CAPS THE LENGTH: a `char[N]` here would be a bound on how long a language tag a page may pass. */
static int js_loc_lower_ascii(JSContext *ctx, JSValueConst v, char **pbuf, int *plen)
{
    JSStringRopeIter it;
    JSString *p;
    int64_t total = 0;
    int i, n, k;
    char *buf;

    *pbuf = NULL;
    *plen = 0;
    string_rope_iter_init(&it, v);
    while ((p = string_rope_iter_next(&it)) != NULL) {
        for (i = 0, n = p->len; i < n; i++)
            if (string_get(p, i) > 0x7F) return 1;
        total += n;
    }
    DCHECK(total <= INT32_MAX, "a language tag longer than a 32-bit index can address reached ECMA-402 "
                               "§6.2.1 step 1 — quickjs's own string length invariant is what makes that "
                               "impossible, so this fires on a rope whose leaves outgrew it rather than on "
                               "anything a page can write");
    buf = js_malloc(ctx, (size_t)total + 1);
    if (!buf) return -1;
    k = 0;
    string_rope_iter_init(&it, v);
    while ((p = string_rope_iter_next(&it)) != NULL) {
        for (i = 0, n = p->len; i < n; i++) {
            int c = string_get(p, i);
            buf[k++] = (char)((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
        }
    }
    DCHECK(k == (int)total, "the two walks of one rope disagreed about how many code units it holds, and the "
                            "buffer was sized by the first");
    buf[k] = '\0';
    *pbuf = buf;
    *plen = k;
    return 0;
}

/* ECMA-402 §9.2.1 CanonicalizeLocaleList ( locales ) — THE ARM ITS STEP 3 TAKES FOR A STRING, WHICH IS THE
   ARM THAT RUNS NO PAGE CODE, AND THAT IS WHY THIS DIFF CAN EXIST WITHOUT A STEP MACHINE.
   Step 3 reads "If locales is a String or locales is an Object and locales has an [[InitializedLocale]]
   internal slot, then Let localesObj be CreateArrayFromList(« locales »)" — so a String never reaches step
   4's `? ToObject(locales)`. What steps 5-7 then iterate is an array this algorithm just minted with one
   element: `LengthOfArrayLike` reads an own data property of a fresh Array, `HasProperty("0")` finds an own
   property and never walks a prototype, `Get` reads it, and step 7.c.iv's `? ToString(element)` is handed a
   String. Not one of those can reach a getter, a Proxy trap or a `toString`, so the whole of ECMA-402 §9.2.1
   is inert for this argument shape and there is nothing for a suspend point to be needed for. The OTHER arm
   is the opposite in every one of those five places, which is why it crashes below instead of being
   approximated here — that is a separate algorithm's worth of rest points and it is subproblem (2d).
   THE `[[InitializedLocale]]` DISJUNCT OF STEP 3 IS UNREACHABLE AND NOT UNIMPLEMENTED: the slot is installed
   only by the constructor ECMA-402 §15.4 "Properties of Intl.Locale Instances" describes — "Intl.Locale
   instances have an [[InitializedLocale]] internal slot." — and `git grep -c InitializedLocale -- engine`
   answers NOTHING AT ALL,
   so no object in this engine can carry it. The name `Intl` occurs in one place — `browser/i18n_names.h`,
   which is a table of names solver/absent.c SUPPRESSES rather than an installation — so there is no
   constructor to reach the slot through either. When Intl.Locale is built, the disjunct's arm arrives WITH
   it, because the same diff is what makes an object able to answer the test.
   WHAT THIS RETURNS IS THE THROW AND NOTHING ELSE. Step 7.c.vi's canonicalized tag has nowhere to go until
   ECMA-402 §6.2.2 exists, so no `seen` List is built and no value is written — a List assembled here and
   read by nobody would be the write-with-no-reader defect, and the one observable ECMA-402 §9.2.1 owes at
   this point is step 7.c.v's RangeError, which IS produced, in dev and in release alike. */
static int js_loc_canonicalize_locale_list_string(JSContext *ctx, JSValueConst locales)
{
    char *buf;
    int len, r;
    bool ok;

    r = js_loc_lower_ascii(ctx, locales, &buf, &len);                        /* step 1 of ECMA-402 §6.2.1 */
    if (r < 0) return -1;
    ok = (r == 0) && js_loc_is_well_formed(buf, len);                        /* step 7.c.v */
    if (!ok) {
        /* "If IsWellFormedLanguageTag(tag) is false, throw a RangeError exception." The tag goes into the
           message because an engine that refuses a page's locale and will not say which one leaves the page
           author and this engine's own reader with the same non-answer. */
        JS_ThrowRangeError(ctx, "invalid language tag: %s", buf ? buf : "(not ASCII)");
        js_free(ctx, buf);
        return -1;
    }
    js_free(ctx, buf);
    return 0;
}

/* §22.1.3.22 String.prototype.slice ( start, end ) over an unknown receiver, by its own steps: where the receiver
   is a String in every world, step 3's ToString is the receiver; where `end` is undefined, step 6 takes `length`;
   where start reaches the receiver's own length in every world (JSConcolicHooks.string_length_reached), step 5's
   ToClampedIndex is `length`, so step 7's from ≥ to returns the empty String. JS_UNINITIALIZED otherwise. */
static JSValue js_str_slice_over_unknown(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || (argc > 1 && !JS_IsUndefined(argv[1])))
        return JS_UNINITIALIZED;
    if (!g_concolic.string_length_reached(ctx, argv[0], this_val))
        return JS_UNINITIALIZED;
    return JS_AtomToString(ctx, JS_ATOM_empty_string);
}

static int js_str_recv_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSStrRecv *s = st;
    JSString *p;
    int mode = s->hdr.arg;
    int r, a, b, len, idx, c;

    if (s->hdr.stage == SRV_THIS) {
        if (s->hdr.str_phase == STR_PH_START) {
            /* A zeroed state holds the INTEGER 0 in every JSValue field — freeable, which is what makes
               driver-owned allocation sound, but NOT a sentinel anything may read. normalize() with no form
               left `arg` at that zero and then read it as a JSString. Every field this machine will read is
               given its sentinel here, before the first thing that can suspend. */
            s->str = JS_UNDEFINED; s->arg = JS_UNDEFINED; s->result = JS_UNDEFINED;
        }
        r = step_thisstring_run(ctx, &s->hdr, cb_result, &s->str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = SRV_ARG0;
    }
    if (s->hdr.stage == SRV_ARG0) {
        switch (mode) {
        case STRRECV_AT: case STRRECV_CODEPOINTAT: case STRRECV_SUBSTRING:
        case STRRECV_CHARAT: case STRRECV_CHARCODEAT:
        case STRRECV_SLICE: case STRRECV_SUBSTR: case STRRECV_REPEAT:
        case STRRECV_PADSTART: case STRRECV_PADEND:
            r = step_toint64_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->idx, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            break;
        case STRRECV_INDEXOF: case STRRECV_LASTINDEXOF: case STRRECV_LOCALECOMPARE:
            /* 22.1.3.12 step 3 is `? ToString(that)` — unconditional, so an absent argument coerces the
               `undefined` rather than being skipped the way normalize's form is. */
            r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->arg, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            break;
        default:
            if (mode >= STRRECV_HTML_BASE) {
                /* step 4.a runs ONLY for a tag with an attribute; the others coerce nothing at all. */
                if (STRRECV_HTML_HAS_ATTR & (1u << (mode - STRRECV_HTML_BASE))) {
                    r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->arg, out_cb, out_argc);
                    cb_result = JS_UNDEFINED;
                    if (r) return r < 0 ? -1 : r;
                }
            }
            break;
        case STRRECV_TOLOCALELOWER: case STRRECV_TOLOCALEUPPER: {
            /* ECMA-402 §20.1.2.1 TransformCase step 1 is `? CanonicalizeLocaleList(locales)`, and ECMA-402 §9.2.1
               step 1 is "If locales is undefined, then Return a new empty List." — so undefined is not an
               argument this stage SKIPS, it is the one value the algorithm's own first step answers outright
               without reading it. That is why the test below is an equality with undefined and not a check on
               argc: `f(undefined)` and `f()` are the same call to ECMA-402 §9.2.1 and must stay the same call here.
               EVERY OTHER VALUE SPLITS AT ECMA-402 §9.2.1 STEP 3, AND THE SPLIT IS THE STANDARD'S OWN — a
               String takes `CreateArrayFromList(« locales »)` and an Object that is not an Intl.Locale takes
               step 4's `? ToObject(locales)`. The first of those runs no page code anywhere in steps 5-7 and
               is BUILT; the second can run the page's code at five separate operations and is not, so it
               crashes here naming what it needs. §Offensive-programming's category (2): a feature that
               should exist and does not, crashing at its site rather than returning the answer for a locale
               nobody resolved. Both crashes are reached by PAGE-SUPPLIED INPUT and that is the established
               shape here rather than an exception to §whose-bytes-state-the-value: neither asserts an
               invariant over a stranger's bytes, they declare unbuilt capabilities, exactly as the C-side
               [[GetPrototypeOf]]-reached-a-Proxy aborts do. The RangeError below is the other thing
               entirely — a page's malformed tag is INPUT, so it gets a spec-mandated throw and never an
               assert, in dev and release alike.
               HOW MUCH OF THE MIRRORED CORPUS REACHES ANY OF THIS is a question for a command and not for a
               number that rots as the corpus grows:
                 grep -rhoE 'toLocale(Lower|Upper)Case\([^)]' testing/corpus/mirror --include=*.js | wc -l
               against the same pattern ending `\(\)`. Run the first with a line you know matches before
               believing a zero, because the two answers are read in opposite directions. */
            JSValueConst locales = step_arg(&s->hdr, 0);
            if (JS_IsUndefined(locales))
                break;
            if (JS_IsString(locales)) {
                r = js_loc_canonicalize_locale_list_string(ctx, locales);
                if (r < 0) return -1;
                /* NAMED RESIDUAL. WHAT IS NOT COVERED: a well-formed tag is validated and then DISCARDED,
                   because ECMA-402 §9.2.1 step 7.c.vi's `CanonicalizeUnicodeLocaleId(tag)` has nowhere to
                   put its answer yet — so `"x".toLocaleLowerCase("tr")` still takes the `und` transformation
                   the body below computes. WHAT THE NEXT DIFF BUILDS, and the reason it is a diff of its own
                   rather than the rest of this one: ECMA-402 §6.2.2 CanonicalizeUnicodeLocaleId, whose step 1
                   is UTS #35 Part 1 Core Annex C LocaleId Canonicalization and therefore needs CLDR's alias
                   table (and, for Annex C's Territory Exception, its likely-subtag table) generated into this
                   tree the way `encgen.mjs`, `idnagen.mjs` and `pslgen.mjs` generate theirs, plus ECMA-402
                   §9.2.5 UnicodeExtensionComponents for its step 2.c. Everything above this line reads no
                   table at all, which is what makes that the seam rather than a size. Then ECMA-402 §9.2.3
                   LookupMatchingLocaleByPrefix and ECMA-402 §20.1.2.1 steps 2-6, and last the object arm
                   below with ECMA-402 §6.2.3 DefaultLocale, which ECMA-402 §20.1.2.1 step 3.a reaches only
                   when requestedLocales is EMPTY and a one-element String list never is.
                   HOW ITS ABSENCE WOULD SHOW: a reader driving this engine beside a browser sees the two
                   agree on every malformed tag — both throw a RangeError — and disagree on a well-formed one
                   naming a language the Unicode Character Database conditions its case mappings on, where the
                   browser's answer is the tailored one and this engine's is the default conversion.
                   RETIREMENT: goes when ECMA-402 §6.2.2 exists and this abort is replaced by the append to
                   `seen` that step 7.c.vii performs. */
                DFAIL("ECMA-402 §9.2.1 step 7.c.vi hands the tag to ECMA-402 §6.2.2 CanonicalizeUnicodeLocaleId "
                      "and this engine has no canonicalizer: UTS #35 Part 1 Core Annex C LocaleId "
                      "Canonicalization needs CLDR's alias data, which no table in this tree carries. A tag "
                      "reaching here is WELL-FORMED — ECMA-402 §6.2.1 has just said so — and returning it "
                      "uncanonicalized would be WRONG rather than narrower, because ECMA-402 §9.2.3 "
                      "LookupMatchingLocaleByPrefix then truncates `tur` to nothing where the canonical `tr` "
                      "matches. Build ECMA-402 §6.2.2 with a generated alias table, then ECMA-402 §9.2.5 for "
                      "its step 2.c, then ECMA-402 §9.2.3 and ECMA-402 §20.1.2.1 steps 2-6");
            } else {
                DFAIL("ECMA-402 §9.2.1 step 4 is `? ToObject(locales)` for a `locales` that is neither "
                      "undefined nor a String, and steps 5-7 then run LengthOfArrayLike, HasProperty, Get "
                      "and ToString over it — five operations that can each reach a getter, a Proxy trap or "
                      "a `toString`, which is the page's own code. That makes ECMA-402 §9.2.1 a STEP MACHINE "
                      "on this arm and not a function: it holds `seen`, `length` and `k` across a callback, "
                      "so it needs its own stages and its own `visit` for the List it accumulates. The "
                      "String arm above needs none of that because ECMA-402 §9.2.1 step 3's "
                      "`CreateArrayFromList(« locales »)` makes every one of those five operations inert. "
                      "Build the machine, then widen this arm to it");
            }
            break;
        }
        case STRRECV_NORMALIZE:
            /* an absent or undefined form is NFC and coerces nothing. The UTF-32 conversion the C body did first
               runs no user code, so reading the form before it is not observable. `arg` staying UNDEFINED IS the
               answer to "was there a form", so the two places cannot disagree. */
            if (!JS_IsUndefined(step_arg(&s->hdr, 0))) {
                r = step_tostring_run(ctx, &s->hdr, s->hdr.argv[0], cb_result, &s->arg, out_cb, out_argc);
                cb_result = JS_UNDEFINED;
                if (r) return r < 0 ? -1 : r;
            }
            break;
        }
        s->hdr.stage = SRV_ARG1;
    }
    p = JS_VALUE_GET_STRING(s->str);
    len = p->len;
    if (s->hdr.stage == SRV_ARG1) {
        s->idx2 = len;
        s->dpos = 0;
        if (mode == STRRECV_SUBSTRING || mode == STRRECV_SLICE || mode == STRRECV_SUBSTR) {
            if (!JS_IsUndefined(step_arg(&s->hdr, 1))) {
                r = step_toint64_run(ctx, &s->hdr, s->hdr.argv[1], cb_result, &s->idx2, out_cb, out_argc);
                cb_result = JS_UNDEFINED;
                if (r) return r < 0 ? -1 : r;
            }
        } else if (mode == STRRECV_PADSTART || mode == STRRECV_PADEND) {
            /* 22.1.3.17 StringPad step 4: an intMaxLength that does not exceed the receiver returns EARLY, before
               the filler is coerced at all — so the filler's ToString sits INSIDE that test. Getting it out of
               order would run a page's toString the spec never runs. */
            if (s->idx > len && s->hdr.argc > 1 && !JS_IsUndefined(step_arg(&s->hdr, 1))) {
                r = step_tostring_run(ctx, &s->hdr, s->hdr.argv[1], cb_result, &s->arg, out_cb, out_argc);
                cb_result = JS_UNDEFINED;
                if (r) return r < 0 ? -1 : r;
            }
        } else if ((mode == STRRECV_INDEXOF || mode == STRRECV_LASTINDEXOF) && s->hdr.argc > 1) {
            if (mode == STRRECV_LASTINDEXOF) {
                r = step_tofloat64_run(ctx, &s->hdr, s->hdr.argv[1], cb_result, &s->dpos, out_cb, out_argc);
            } else {
                r = step_toint64_run(ctx, &s->hdr, s->hdr.argv[1], cb_result, &s->idx2, out_cb, out_argc);
            }
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
        }
        s->hdr.stage = SRV_BODY;
    }
    JS_FreeValue(ctx, cb_result);

    if (mode <= STRRECV_TRIM_BOTH) {
        a = 0;
        b = len;
        if (mode & 1) while (a < len && lre_is_space(string_get(p, a))) a++;
        if (mode & 2) while (b > a && lre_is_space(string_get(p, b - 1))) b--;
        s->result = js_sub_string(ctx, p, a, b);
        return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
    }
    /* the saturating int64 narrows to int exactly as JS_ToInt32Sat's clamp did */
    idx = (s->idx < INT32_MIN) ? INT32_MIN : (s->idx > INT32_MAX) ? INT32_MAX : (int)s->idx;
    if (mode == STRRECV_AT) {
        if (idx < 0) idx = len + idx;
        if (idx < 0 || idx >= len) { s->result = JS_UNDEFINED; return 0; }
        s->result = js_new_string_char(ctx, string_get(p, idx));
        return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
    }
    if (mode == STRRECV_CHARAT || mode == STRRECV_CHARCODEAT) {
        /* 22.1.3.1 / 22.1.3.2: an out-of-range position is the empty string / NaN, never a throw. */
        if (idx < 0 || idx >= len) {
            s->result = (mode == STRRECV_CHARAT) ? js_empty_string(ctx->rt) : JS_NAN;
            return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
        }
        c = string_get(p, idx);
        s->result = (mode == STRRECV_CHARAT) ? js_new_string_char(ctx, c) : js_int32(c);
        return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
    }
    if (mode == STRRECV_CODEPOINTAT) {
        if (idx < 0 || idx >= len) { s->result = JS_UNDEFINED; return 0; }
        c = string_getc(p, &idx);
        s->result = js_int32(c);
        return 0;
    }
    if (mode == STRRECV_SUBSTRING) {
        /* JS_ToInt32Clamp(0, len, 0) on each index, then the pair is ordered */
        a = str_clamp(s->idx, len);
        b = JS_IsUndefined(step_arg(&s->hdr, 1)) ? len : str_clamp(s->idx2, len);
        s->result = js_sub_string(ctx, p, a < b ? a : b, a < b ? b : a);
        return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
    }
    if (mode == STRRECV_SLICE || mode == STRRECV_SUBSTR) {
        /* 22.1.3.22 slice / B.2.2.1 substr: the SAME two coercions as substring in the same order, differing only
           in how the pair is clamped — slice takes [start, end) with a negative index relative to the end, substr
           takes [start, start+length). The C bodies did both JS_ToInt32Clamp calls from a C entry, which is where
           `"abc".slice({valueOf(){ while(x){} }})` preempted with no flow base. */
        a = str_clamp_off(s->idx, len, len);
        if (mode == STRRECV_SLICE) {
            b = JS_IsUndefined(step_arg(&s->hdr, 1)) ? len : str_clamp_off(s->idx2, len, len);
            if (b < a) b = a;
        } else {
            b = a + (JS_IsUndefined(step_arg(&s->hdr, 1)) ? len - a : str_clamp_off(s->idx2, len - a, 0));
        }
        s->result = js_sub_string(ctx, p, a, b);
        return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
    }
    if (mode == STRRECV_REPEAT) {
        /* 22.1.3.16: ToIntegerOrInfinity(count), then the RangeError, then the build. Only the coercion is the
           page's code; everything after it is arithmetic on a string this machine already holds. */
        StringBuffer rb_s, *rb = &rb_s;
        int64_t cnt = s->idx;
        if (cnt < 0 || cnt > INT32_MAX)
            return JS_ThrowRangeError(ctx, "invalid repeat count"), -1;
        if (len == 0 || cnt == 1) { s->result = js_dup(s->str); return 0; }
        if (cnt * len > JS_STRING_LEN_MAX)
            return JS_ThrowRangeError(ctx, "invalid string length"), -1;
        if (string_buffer_init2(ctx, rb, (int)(cnt * len), p->is_wide_char))
            return -1;
        if (len == 1) {
            string_buffer_fill(rb, string_get(p, 0), (int)cnt);
        } else {
            int64_t k;
            for (k = 0; k < cnt; k++) string_buffer_concat(rb, p, 0, len);
        }
        s->result = string_buffer_end(rb);
        return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
    }
    if (mode == STRRECV_PADSTART || mode == STRRECV_PADEND) {
        /* 22.1.3.17/22.1.3.18: both coercions ran on the tramp above, in spec order and under spec conditions, so
           everything here is arithmetic on strings this machine already holds. */
        StringBuffer pb_s, *pb = &pb_s;
        JSString *p1 = NULL;
        int n = (s->idx < INT32_MIN) ? INT32_MIN : (s->idx > INT32_MAX) ? INT32_MAX : (int)s->idx;
        c = ' ';
        if (len >= n) { s->result = js_dup(s->str); return 0; }
        if (!JS_IsUndefined(s->arg)) {
            p1 = JS_VALUE_GET_STRING(s->arg);
            if (p1->len == 0) { s->result = js_dup(s->str); return 0; }
            if (p1->len == 1) { c = string_get(p1, 0); p1 = NULL; }
        }
        if (n > JS_STRING_LEN_MAX) { JS_ThrowRangeError(ctx, "invalid string length"); return -1; }
        if (string_buffer_init(ctx, pb, n)) return -1;
        n -= len;
        if (mode == STRRECV_PADEND && string_buffer_concat(pb, p, 0, len)) goto pad_fail;
        if (p1) {
            while (n > 0) {
                int chunk = min_int(n, p1->len);
                if (string_buffer_concat(pb, p1, 0, chunk)) goto pad_fail;
                n -= chunk;
            }
        } else if (string_buffer_fill(pb, c, n)) {
            goto pad_fail;
        }
        if (mode == STRRECV_PADSTART && string_buffer_concat(pb, p, 0, len)) goto pad_fail;
        s->result = string_buffer_end(pb);
        return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
    pad_fail:
        string_buffer_free(pb);
        return -1;
    }
    if (mode == STRRECV_INDEXOF || mode == STRRECV_LASTINDEXOF) {
        JSString *p1 = JS_VALUE_GET_STRING(s->arg);
        int v_len = p1->len, pos, start, stop, inc, i, ret = -1;
        if (mode == STRRECV_LASTINDEXOF) {
            pos = len - v_len;
            if (s->hdr.argc > 1 && !isnan(s->dpos)) {
                if (s->dpos <= 0) pos = 0;
                else if (s->dpos < pos) pos = (int)s->dpos;
            }
            start = pos; stop = 0; inc = -1;
        } else {
            pos = (s->hdr.argc > 1) ? str_clamp(s->idx2, len) : 0;
            start = pos; stop = len - v_len; inc = 1;
        }
        if (len >= v_len && inc * (stop - start) >= 0) {
            for (i = start;; i += inc) {
                if (!string_cmp(p, p1, i, 0, v_len)) { ret = i; break; }
                if (i == stop) break;
            }
        }
        s->result = js_int32(ret);
        return 0;
    }
    if (mode == STRRECV_LOCALECOMPARE) {
        /* 22.1.3.12 with the default (root) locale: both operands are NFC-normalised and compared by code
           point. Nothing below invokes user code — the two ToStrings above were the whole of it, and
           js_string_localeCompare ran them from its C entry. */
        uint32_t *as = NULL, *bs = NULL, *ts;
        int an, bn, n, i, cmp;
        an = to_utf32_buf(ctx, JS_VALUE_GET_STRING(s->str), &as);
        bn = (an < 0) ? -1 : to_utf32_buf(ctx, JS_VALUE_GET_STRING(s->arg), &bs);
        if (bn >= 0) {
            an = unicode_normalize(&ts, as, an, UNICODE_NFC, ctx, (DynBufReallocFunc *)js_realloc);
            if (an >= 0) { js_free(ctx, as); as = ts; }
        }
        if (an >= 0 && bn >= 0) {
            bn = unicode_normalize(&ts, bs, bn, UNICODE_NFC, ctx, (DynBufReallocFunc *)js_realloc);
            if (bn >= 0) { js_free(ctx, bs); bs = ts; }
        }
        if (an < 0 || bn < 0) { js_free(ctx, as); js_free(ctx, bs); return -1; }
        n = min_int(an, bn);
        for (i = 0; i < n; i++)
            if (as[i] != bs[i])
                break;
        cmp = (i < n) ? compare_u32(as[i], bs[i]) : compare_u32(an, bn);
        js_free(ctx, as); js_free(ctx, bs);
        s->result = js_int32(cmp);
        return 0;
    }
    if (mode >= STRRECV_HTML_BASE) {
        s->result = js_string_CreateHTML(ctx, s->str, s->arg, mode - STRRECV_HTML_BASE);
        return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
    }
    /* THE FOUR SHARE A BODY AND THAT IS A DERIVED RESULT RATHER THAN THE ALIASING THIS DIFF JUST ENDED.
       With `locales` undefined the whole of ECMA-402 §20.1.2.1 reduces, step by step, to what this call
       already computes. Step 1 gives an empty List (ECMA-402 §9.2.1 step 1). Step 2's test fails, so step 3.a takes
       DefaultLocale(); ECMA-402 §6.2.3's own Note says that "In browser environments, it should match
       navigator.language to avoid providing any additional distinguishing information.", and this engine's
       navigator.language carries the example `en-US`. Step 4's Available Locales List is "an Available Locales
       List which includes the language tags for which the Unicode Character Database contains
       language-sensitive case mappings" — the UCD's SpecialCasing.txt names exactly three languages in its
       conditional entries, and en is not among them, so step 5's LookupMatchingLocaleByPrefix truncates
       `en-US` to `en` to the empty String and returns undefined. Step 6 therefore gives locale = `und`, and
       step 8.a's transformation for it is the Unicode Default Case Conversion algorithm, which is what
       js_string_case_body performs: lre_case_conv's FULL mapping, multi-code-point results included, with
       Final_Sigma's context condition. ECMA-402 §20.1.2.1 permits this outright — "Regardless of tailoring, a
       conforming implementation's case transformation algorithm must always yield the same result given the
       same input code points, locale, and target case." — so this is the §NO-STUBS exception's own shape: the
       answer is COMPUTED and correct, not a placeholder standing in for one.
       NAMED RESIDUAL. WHAT IS NOT COVERED: a receiver holding a code point for which the Unicode Character
       Database states a LANGUAGE-CONDITIONAL case mapping, where the answer depends on a locale this engine
       does not resolve — and the locale is doubly unresolved, because navigator.language is concolic here, so
       the honest answer for such a receiver is not one string but a fork. WHAT THE NEXT DIFF BUILDS:
       ECMA-402 §20.1.2.1 steps 1-6, i.e. the locale layer the argument stage above crashes for, after which
       step 8.a is a transformation USING locale rather than one that ignores it. HOW ITS ABSENCE WOULD SHOW: these two
       functions' results are byte-identical to toLowerCase and toUpperCase for every receiver, so a reader
       driving this engine beside a browser whose locale is one the UCD conditions on sees this engine give the
       `und` answer where the browser does not — and sees it with NO argument passed, which is the case the
       crash above cannot reach. RETIREMENT: goes when steps 1-6 are built and the DCHECK above is deleted. */
    if (mode == STRRECV_TOLOWER || mode == STRRECV_TOUPPER ||
        mode == STRRECV_TOLOCALELOWER || mode == STRRECV_TOLOCALEUPPER) {
        s->result = js_string_case_body(ctx, s->str,
                                        mode == STRRECV_TOLOWER || mode == STRRECV_TOLOCALELOWER);
        return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
    }
    DCHECK(mode == STRRECV_NORMALIZE, "string receiver machine: unknown mode");
    {
        UnicodeNormalizationEnum n_type = UNICODE_NFC;
        uint32_t *buf = NULL, *out_buf;
        int buf_len, out_len, is_compat;
        if (!JS_IsUndefined(s->arg)) {
            JSString *f = JS_VALUE_GET_STRING(s->arg);
            const char *form = JS_ToCString(ctx, s->arg), *q;
            size_t form_len = f->len;
            if (!form) return -1;
            q = form;
            is_compat = false;
            if (q[0] != 'N' || q[1] != 'F') goto bad_form;
            q += 2;
            if (*q == 'K') { is_compat = true; q++; }
            if ((*q != 'C' && *q != 'D') || (size_t)(q + 1 - form) != form_len) {
            bad_form:
                JS_FreeCString(ctx, form);
                JS_ThrowRangeError(ctx, "bad normalization form");
                return -1;
            }
            n_type = UNICODE_NFC + is_compat * 2 + (*q - 'C');
            JS_FreeCString(ctx, form);
        }
        buf_len = JS_ToUTF32String(ctx, &buf, s->str);   /* runs no user code */
        if (buf_len < 0) return -1;
        out_len = unicode_normalize(&out_buf, buf, buf_len, n_type,
                                    ctx->rt, (DynBufReallocFunc *)js_realloc_rt);
        js_free(ctx, buf);
        if (out_len < 0) return -1;
        s->result = JS_NewUTF32String(ctx, out_buf, out_len);
        js_free(ctx, out_buf);
        return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
    }
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The receiver's ToString and the argument's are each the
   page's code, so a branch inside one forks the operation with its own coerced operands. */
static void js_str_recv_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSStrRecv *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->str);
    v->val(ctx, &s->arg);
}

static JSValue js_str_recv_fini(JSContext *ctx, void *st, bool take_result)
{
    JSStrRecv *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}


/* The all-strings fast path, kept because it is the common case: one allocation and memcpys instead of a chain
   of ToString + ConcatString. Returns 1 with *out set, or 0 when any operand is not a same-width string — in
   which case a ToPrimitive is due and only the step machine below can run it. */
static int js_string_concat_fast(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, JSValue *out)
{
    int i, is_wide_char;
    JSString *p, *q;
    int64_t len;
    uint32_t n;
    char *d;

    if (JS_TAG_STRING != JS_VALUE_GET_TAG(this_val))
        return 0;
    p = JS_VALUE_GET_STRING(this_val);
    len = p->len;
    is_wide_char = p->is_wide_char;
    for (i = 0; i < argc; i++) {
        if (JS_TAG_STRING != JS_VALUE_GET_TAG(argv[i]))
            return 0;
        p = JS_VALUE_GET_STRING(argv[i]);
        if (p->is_wide_char ^ is_wide_char)
            return 0;
        len += p->len;
    }
    if (len > INT_MAX>>is_wide_char) {
        *out = JS_ThrowOutOfMemory(ctx);
        return 1;
    }
    p = JS_VALUE_GET_STRING(this_val);
    if (p->len == len) {
        *out = js_dup(this_val);
        return 1;
    }
    q = js_alloc_string(ctx, len, is_wide_char);
    if (!q) { *out = JS_EXCEPTION; return 1; }
    d = strv(q);
    n = p->len << is_wide_char;
    memcpy(d, strv(p), n);
    d += n;
    for (i = 0; i < argc; i++) {
        p = JS_VALUE_GET_STRING(argv[i]);
        n = p->len << is_wide_char;
        memcpy(d, strv(p), n);
        d += n;
    }
    if (!is_wide_char)
        *d = '\0';
    *out = JS_MKPTR(JS_TAG_STRING, q);
    return 1;
}


/* The prologue is RequireObjectCoercible plus the all-strings fast path — no user code, but it still belongs in
   step 0 rather than in a phase that runs before the machine exists. */
/* 1 = every operand was already a String, so `acc` IS the answer and there is nothing to coerce; 0 = the
   coercions have to run; -1 = threw. */
static int js_str_concat_prologue(JSContext *ctx, JSStrConcat *s)
{
    if (JS_IsUndefined(s->hdr.this_val) || JS_IsNull(s->hdr.this_val)) {
        JS_ThrowTypeError(ctx, "cannot convert to object");   /* RequireObjectCoercible, step 1 */
        return -1;
    }
    if (js_string_concat_fast(ctx, s->hdr.this_val, s->hdr.argc, (JSValueConst *)s->hdr.argv, &s->acc)) {
        if (JS_IsException(s->acc)) { s->acc = JS_UNDEFINED; return -1; }
        return 1;
    }
    return 0;
}

/* ONE PIECE JOINS THE ACCUMULATOR. `val` is a primitive (or a concolic) and is CONSUMED; `first` says it is the
   receiver's, which seeds the accumulator rather than concatenating onto it.
   A CONCOLIC PIECE MAKES THE WHOLE CONCATENATION CONCOLIC. An untagged template literal COMPILES to
   String.prototype.concat, so `v${x}` is this machine — and stringifying unknown external input has no answer at
   the ToString boundary, which owes C a real string. The answer folds through the .add hook, which owns the
   derivation (shape joined, source kept, example produced by actually concatenating the examples) rather than
   inventing a second rule for the same operation — but it declares WHICH concatenation this is. 22.1.3.5 has
   already ToString'd every piece at steps 3 and 5.a, so its arm is the string one ALWAYS, where 13.15.3 step 1.c
   would take the numeric path over two numeric examples: `(x*2).concat(y*3)` is "1020" and never 30. */
static int js_str_concat_join(JSContext *ctx, JSStrConcat *s, JSValue val, bool first)
{
    JSValue pair[2], str;

    if (g_concolic.is(val)) {
        if (first) { s->acc = val; return 0; }
        pair[0] = s->acc; pair[1] = val;
        if (!g_concolic.add(ctx, pair + 2, JS_CONCOLIC_ADD_CONCAT))
            DFAIL("the concolic + hook declined a concolic concat operand");
        s->acc = pair[0];
        return 0;
    }
    /* a coercion settled: it is a PRIMITIVE, so ToString finishes it without user code. */
    str = JS_ToString(ctx, val);
    JS_FreeValue(ctx, val);
    if (JS_IsException(str)) return -1;
    if (first) { s->acc = str; return 0; }
    if (g_concolic.is(s->acc)) {
        /* the accumulator went concolic earlier: every later piece joins it the same way. */
        pair[0] = s->acc; pair[1] = str;
        if (!g_concolic.add(ctx, pair + 2, JS_CONCOLIC_ADD_CONCAT))
            DFAIL("the concolic + hook declined a concolic accumulator");
        s->acc = pair[0];
        return 0;
    }
    s->acc = JS_ConcatString(ctx, s->acc, str);
    if (JS_IsException(s->acc)) { s->acc = JS_UNDEFINED; return -1; }
    return 0;
}

/* 22.1.3.5 String.prototype.concat. WHICH OPERAND A DELIVERED PRIMITIVE BELONGS TO IS A STAGE, not a flag.
   It was `this_done` plus "is the delivered value undefined", and undefined is a value a ToPrimitive may
   legitimately ANSWER: `"a".concat({[Symbol.toPrimitive](){ return undefined; }})` is "aundefined" in V8 and
   here it HUNG — the delivery was read as "nothing arrived", the cursor never moved, and the machine re-issued
   the same request forever. A stage cannot be confused with a value. */
#define SCC_STAGES(X) \
    X(SCC_RECV, "22.1.3.5 steps 1-2 (thisValue is the this value, RequireObjectCoercible'd)") \
    X(SCC_THIS, "22.1.3.5 step 3 (string is ToString(thisValue)) — the ToPrimitive inside it, which is the page's code") \
    X(SCC_ARG,  "22.1.3.5 step 5.a (nextString is ToString(next)) — its ToPrimitive, once per argument")
enum { SCC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_str_concat_steps[] = { SCC_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_str_concat_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSStrConcat *s = st;
    JSValueConst v;
    int r;

    if (s->hdr.stage == SCC_RECV) {
        JS_FreeValue(ctx, cb_result);
        s->acc = JS_UNDEFINED;
        r = js_str_concat_prologue(ctx, s);
        if (r < 0) return -1;
        if (r > 0) return 0;              /* every operand was already a String: acc IS the answer */
        s->hdr.stage = SCC_THIS;
        if (JS_VALUE_GET_TAG(s->hdr.this_val) == JS_TAG_OBJECT) {
            s->cb[0] = s->hdr.this_val;   /* borrowed: the header holds the reference */
            *out_cb = s->cb; *out_argc = HINT_STRING;
            return 5;
        }
        if (js_str_concat_join(ctx, s, js_dup(s->hdr.this_val), true)) return -1;
        s->hdr.stage = SCC_ARG;
        goto args;
    }
    if (s->hdr.stage == SCC_THIS) {
        if (js_str_concat_join(ctx, s, cb_result, true)) return -1;
        s->hdr.stage = SCC_ARG;
        goto args;
    }
    DCHECK(s->hdr.stage == SCC_ARG, "String.prototype.concat resumed in an unknown stage");
    if (js_str_concat_join(ctx, s, cb_result, false)) return -1;
    s->k++;
 args:
    while (s->k < s->hdr.argc) {
        v = s->hdr.argv[s->k];
        if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
            s->cb[0] = v;                 /* borrowed: the header holds the reference */
            *out_cb = s->cb; *out_argc = HINT_STRING;
            return 5;                     /* the stage is already SCC_ARG */
        }
        if (js_str_concat_join(ctx, s, js_dup(v), false)) return -1;
        s->k++;
    }
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). One value: cb[0] is a borrowed view of a header capture, and
   the receiver and arguments are the header's. Each argument's coercion is the page's toString, so a concolic
   branch inside one forks the concatenation and each arm accumulates its own remainder. */
static void js_str_concat_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSStrConcat *s = st;
    v->val(ctx, &s->acc);
}

static JSValue js_str_concat_fini(JSContext *ctx, void *st, bool take_result)
{
    JSStrConcat *s = st;
    JSValue r = take_result ? s->acc : JS_UNDEFINED;
    if (take_result) s->acc = JS_UNDEFINED;   /* handed out, read above before the rest is released */
    return r;
}

static int string_cmp(JSString *p1, JSString *p2, int x1, int x2, int len)
{
    int i, c1, c2;
    for (i = 0; i < len; i++) {
        if ((c1 = string_get(p1, x1 + i)) != (c2 = string_get(p2, x2 + i)))
            return c1 - c2;
    }
    return 0;
}

static int string_indexof_char(JSString *p, int c, int from)
{
    /* assuming 0 <= from <= p->len */
    int i, len = p->len;
    if (p->is_wide_char) {
        for (i = from; i < len; i++) {
            if (str16(p)[i] == c)
                return i;
        }
    } else {
        if ((c & ~0xff) == 0) {
            for (i = from; i < len; i++) {
                if (str8(p)[i] == (uint8_t)c)
                    return i;
            }
        }
    }
    return -1;
}

static int string_indexof(JSString *p1, JSString *p2, int from)
{
    /* assuming 0 <= from <= p1->len */
    int c, i, j, len1 = p1->len, len2 = p2->len;
    if (len2 == 0)
        return from;
    for (i = from, c = string_get(p2, 0); i + len2 <= len1; i = j + 1) {
        j = string_indexof_char(p1, c, i);
        if (j < 0 || j + len2 > len1)
            break;
        if (!string_cmp(p1, p2, j + 1, 1, len2 - 1))
            return j;
    }
    return -1;
}

static int64_t string_advance_index(JSString *p, int64_t index, bool unicode)
{
    if (!unicode || index >= p->len || !p->is_wide_char) {
        index++;
    } else {
        int index32 = (int)index;
        string_getc(p, &index32);
        index = index32;
    }
    return index;
}

/* 22.1.3.10 isWellFormed / 22.1.3.31 toWellFormed. Their ONLY page-visible step is the receiver's
   `? RequireObjectCoercible(this)` then `? ToString(this)`, which they performed with JS_ToStringCheckObject from
   C — the case step_thisstring_run's own note names, "even a ZERO-ARGUMENT one could not stay a C body". The scan
   that follows sees only a JSString and invokes nothing, so the body keeps it and takes the coerced string. */
static JSValue js_string_isWellFormed_body(JSContext *ctx, JSValueConst strv);
static JSValue js_string_toWellFormed_body(JSContext *ctx, JSValueConst strv);

/* ONE STAGE EACH, and that is the whole algorithm: the only thing either runs of the page's is the receiver's
   ToString, whose suspension the header's own str_phase carries, and what follows walks a string this engine
   already holds. A one-stage machine still declares its stage — the label is what says the range after the
   coercion has no rest point of its own, rather than leaving a reader to assume it. */
#define SWF_STAGES(X, THIS) X(SWF_THIS, THIS)
enum { SWF_STAGES(JS_STEP_STAGE_ENUM, 0) };
static const char *const js_str_iswellformed_steps[] = {
    SWF_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.10 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue)), "
        "then step 4 (IsStringWellFormedUnicode(string))")
    NULL };
static const char *const js_str_towellformed_steps[] = {
    SWF_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.31 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue)), "
        "then steps 4-8 (each unpaired surrogate replaced with U+FFFD)")
    NULL };

static int js_string_wellformed_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSStrWellFormed *s = st;
    JSValue str = JS_UNDEFINED;
    int r = step_thisstring_run(ctx, &s->hdr, cb_result, &str, out_cb, out_argc);
    if (r) return r < 0 ? -1 : r;
    s->result = s->hdr.arg ? js_string_toWellFormed_body(ctx, str) : js_string_isWellFormed_body(ctx, str);
    JS_FreeValue(ctx, str);
    return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_string_wellformed_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSStrWellFormed *s = st;
    v->val(ctx, &s->result);
}

static JSValue js_string_wellformed_fini(JSContext *ctx, void *st, bool take_result)
{
    JSStrWellFormed *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static JSValue js_string_isWellFormed_body(JSContext *ctx, JSValueConst strv)
{
    JSValue str = js_dup(strv);
    JSValue ret;
    JSString *p;
    uint32_t c, i, n;

    ret = JS_TRUE;

    p = JS_VALUE_GET_STRING(str);
    if (!p->is_wide_char || p->len == 0)
        goto done; // by definition well-formed

    for (i = 0, n = p->len; i < n; i++) {
        c = str16(p)[i];
        if (!is_surrogate(c))
            continue;
        if (is_lo_surrogate(c) || i + 1 == n)
            break;
        c = str16(p)[++i];
        if (!is_lo_surrogate(c))
            break;
    }

    if (i < n)
        ret = JS_FALSE;

done:
    JS_FreeValue(ctx, str);
    return ret;
}

/* Handed the ALREADY-COERCED receiver by js_string_wellformed_step, which performed 22.1.3 step 1-2 as a
   request. The JS_ToStringCheckObject that stood here was a second coercion of a value that is already a
   string — harmless today only because it is, and a live C coercion the moment anything hands this an object. */
static JSValue js_string_toWellFormed_body(JSContext *ctx, JSValueConst strv)
{
    JSValue str;
    JSValue ret;
    JSString *p;
    uint32_t c, i, n;

    DCHECK(JS_IsString(strv), "the toWellFormed body takes the receiver its machine already coerced");
    str = js_dup(strv);

    p = JS_VALUE_GET_STRING(str);
    if (!p->is_wide_char || p->len == 0)
        return str; // by definition well-formed

    // TODO(bnoordhuis) don't clone when input is well-formed
    ret = js_new_string16_len(ctx, str16(p), p->len);
    JS_FreeValue(ctx, str);
    if (JS_IsException(ret))
        return JS_EXCEPTION;

    p = JS_VALUE_GET_STRING(ret);
    for (i = 0, n = p->len; i < n; i++) {
        c = str16(p)[i];
        if (!is_surrogate(c))
            continue;
        if (is_lo_surrogate(c) || i + 1 == n) {
            str16(p)[i] = 0xFFFD;
            continue;
        }
        c = str16(p)[++i];
        if (!is_lo_surrogate(c))
            str16(p)[--i] = 0xFFFD;
    }

    return ret;
}


/* return < 0 if exception or true/false */
static int js_is_regexp(JSContext *ctx, JSValueConst obj);

static JSRegExp *js_get_regexp(JSContext *ctx, JSValueConst obj, bool throw_error);

/* 22.1.3.8 includes / 22.1.3.7 endsWith / 22.1.3.24 startsWith. FOUR of their steps are the page's code and every
   one ran from C: the receiver's ToString, IsRegExp's `? Get(searchString, @@match)`, the search string's own
   ToString, and ToIntegerOrInfinity on the position. Their ORDER is observable and is the order the stages are in;
   everything after is a string compare that invokes nothing, which is why the search is its own function taking
   both strings already coerced — and why the last stage names a RANGE: it rests at the position's coercion and
   the compare after it has no rest point of its own.
   ONE stage list expanded once per algorithm, with that algorithm's own step text — see AFIND_STAGES. */
#define SINC_STAGES(X, THIS, MATCH, SEARCH, POS) \
    X(SI_THIS,   THIS) \
    X(SI_MATCH,  MATCH) \
    X(SI_SEARCH, SEARCH) \
    X(SI_POS,    POS)
enum { SINC_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0, 0) };
static const char *const js_str_includes_steps[] = {
    SINC_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.8 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue)) and "
        "7.2.6 IsRegExp step 1's `searchString is an Object` test",
        "22.1.3.8 steps 4-5 (isRegexp is IsRegExp(searchString) — its Get(searchString, %Symbol.match%); "
        "the TypeError)",
        "22.1.3.8 step 6 (searchString is set to ToString(searchString))",
        "22.1.3.8 step 8 (start clamps ToIntegerOrInfinity(position) between 0 and length) - only its "
        "ToIntegerOrInfinity half runs here, then steps 7, 9-12 (length; the assert; whether "
        "StringIndexOf(string, searchString, start) is not-found)")
    NULL };
static const char *const js_str_endswith_steps[] = {
    SINC_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.7 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue)) and "
        "7.2.6 IsRegExp step 1's `searchString is an Object` test",
        "22.1.3.7 steps 4-5 (isRegexp is IsRegExp(searchString) — its Get(searchString, %Symbol.match%); "
        "the TypeError)",
        "22.1.3.7 step 6 (searchString is set to ToString(searchString))",
        "22.1.3.7 step 8 (end is length when endPosition is undefined, else the clamp of "
        "ToIntegerOrInfinity(endPosition) into [0, length]) - only its ToIntegerOrInfinity half runs here, "
        "then steps 7, 9-15 (length; searchLength; true when it is 0; start is end - searchLength; false "
        "when start is negative; the substring compare)")
    NULL };
static const char *const js_str_startswith_steps[] = {
    SINC_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.24 steps 1-3 (thisValue is RequireObjectCoercible'd; string is ToString(thisValue)) and "
        "7.2.6 IsRegExp step 1's `searchString is an Object` test",
        "22.1.3.24 steps 4-5 (isRegexp is IsRegExp(searchString) — its Get(searchString, %Symbol.match%); "
        "the TypeError)",
        "22.1.3.24 step 6 (searchString is set to ToString(searchString))",
        "22.1.3.24 step 8 (start clamps ToIntegerOrInfinity(position) between 0 and length) - only its "
        "ToIntegerOrInfinity half runs here, then steps 7, 9-16 (length; the assert; searchLength; true "
        "when it is 0; end is start + searchLength; false when end exceeds length; the substring compare)")
    NULL };

static JSValue js_string_includes_search(JSContext *ctx, JSValueConst strv, JSValueConst vv, int pos_in,
                                         bool has_pos, int magic);

static int js_string_includes_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSStrIncludes *s = st;
    int magic = s->hdr.arg;
    JSValueConst search = step_arg(&s->hdr, 0), position = step_arg(&s->hdr, 1);
    bool has_pos = s->hdr.argc > 1 && !JS_IsUndefined(position);
    int pos = 0, r;

    if (s->hdr.stage == SI_THIS) {
        r = step_thisstring_run(ctx, &s->hdr, cb_result, &s->str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = SI_MATCH;
        if (!JS_IsObject(search))
            goto not_regexp;            /* IsRegExp step 1: a primitive is not one, and reads nothing */
        r = step_getprop_run(ctx, &s->hdr, search, JS_ATOM_Symbol_match, JS_UNDEFINED, &cb_result,
                             out_cb, out_argc);
        if (r) return r < 0 ? -1 : r;
    }
    if (s->hdr.stage == SI_MATCH) {
        JSValue m = JS_UNDEFINED;
        r = step_getprop_run(ctx, &s->hdr, search, JS_ATOM_Symbol_match, cb_result, &m, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        /* IsRegExp steps 2-4: a DEFINED @@match decides by ToBoolean, otherwise the [[RegExpMatcher]] slot does. */
        if (JS_IsUndefined(m) ? js_get_regexp(ctx, search, false) != NULL : JS_ToBoolFree(ctx, m)) {
            JS_ThrowTypeError(ctx, "regexp not supported");
            return -1;
        }
    not_regexp:
        s->hdr.stage = SI_SEARCH;
        r = step_tostring_run(ctx, &s->hdr, search, JS_UNDEFINED, &s->v, out_cb, out_argc);
        if (r) return r < 0 ? -1 : r;
        goto have_search;
    }
    if (s->hdr.stage == SI_SEARCH) {
        r = step_tostring_run(ctx, &s->hdr, search, cb_result, &s->v, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
    have_search:
        s->hdr.stage = SI_POS;
        if (!has_pos)
            goto have_pos;
        r = step_toint32sat_run(ctx, &s->hdr, position, JS_UNDEFINED, &pos, out_cb, out_argc);
        if (r) return r < 0 ? -1 : r;
        goto have_pos;
    }
    DCHECK(s->hdr.stage == SI_POS, "String.prototype.includes resumed in an unknown stage");
    r = step_toint32sat_run(ctx, &s->hdr, position, cb_result, &pos, out_cb, out_argc);
    if (r) return r < 0 ? -1 : r;
 have_pos:
    s->result = js_string_includes_search(ctx, s->str, s->v, pos, has_pos, magic);
    return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The two coerced strings, each held across the other's
   coercion. */
static void js_string_includes_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSStrIncludes *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->str);
    v->val(ctx, &s->v);
}

static JSValue js_string_includes_fini(JSContext *ctx, void *st, bool take_result)
{
    JSStrIncludes *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static JSValue js_string_includes_search(JSContext *ctx, JSValueConst strv, JSValueConst vv, int pos_in,
                                         bool has_pos, int magic)
{
    JSValueConst str = strv, v = vv;
    int i, len, v_len, pos, start, stop, ret;
    JSString *p;
    JSString *p1;

    p = JS_VALUE_GET_STRING(str);
    p1 = JS_VALUE_GET_STRING(v);
    len = p->len;
    v_len = p1->len;
    pos = (magic == 2) ? len : 0;
    if (has_pos)
        pos = min_int(max_int(pos_in, 0), len);
    len -= v_len;
    ret = 0;
    if (magic == 0) {
        start = pos;
        stop = len;
    } else {
        if (magic == 1) {
            if (pos > len)
                goto done;
        } else {
            pos -= v_len;
        }
        start = stop = pos;
    }
    if (start >= 0 && start <= stop) {
        for (i = start;; i++) {
            if (!string_cmp(p, p1, i, 0, v_len)) {
                ret = 1;
                break;
            }
            if (i == stop)
                break;
        }
    }
 done:
    return js_bool(ret);   /* both strings are BORROWED: the machine owns them and its fini frees them */
}

/* 22.1.3.13 String.prototype.match, 22.1.3.14 .matchAll and 22.1.3.21 .search — ONE machine, the @@-method's
   atom in hdr.arg, exactly as js_string_match was one body with a magic. It called the @@-method with
   JS_CallFree straight out of C, and that method is itself a step machine, so the call reached the DFAIL that
   says a step builtin was invoked outside the interpreter's dispatch. Every other step is the page's code too:
   GetMethod's read, matchAll's IsRegExp + `flags` + ToString, ToString(this), Construct(RegExp, ...) and the
   final Get + Call. */
/* ONE STAGE LIST EXPANDED ONCE PER ALGORITHM, with that algorithm's own step text — see RES_STAGES for the
   shape and JSTrampStepDef.steps for why the label rather than the index is a parked machine's rest point.
   MATCH'S AND SEARCH'S ARRAYS ARE SHORTER, and that is the declaration doing its job: only matchAll performs
   IsRegExp and the `flags` read, so its three extra rest points are a SEPARATE list appended after the seven
   they share. Two of the three algorithms therefore simply END at the Invoke, and a rest in one of matchAll's
   stages is a stage past the end of their array — which step_stage_check names on its own, with no
   placeholder step to invent and no DCHECK to write.
   The stage NUMBERS are consequently not the order the blocks appear in below; the label is the identity and
   the block order is the algorithm's. */
#define STRMATCH_STAGES(X, RECV, METHOD, CALLED, STR, CREATED, INVOKE, INVOKED) \
    X(SM_RECV,    RECV) \
    X(SM_METHOD,  METHOD) \
    X(SM_CALLED,  CALLED) \
    X(SM_STR,     STR) \
    X(SM_CREATED, CREATED) \
    X(SM_INVOKE,  INVOKE) \
    X(SM_INVOKED, INVOKED)
/* The three only matchAll reaches. */
#define STRMATCH_ALL_STAGES(X, ISREGEXP, FLAGS, FLAGSTR) \
    X(SM_ISREGEXP, ISREGEXP) \
    X(SM_FLAGS,    FLAGS) \
    X(SM_FLAGSTR,  FLAGSTR)
enum { STRMATCH_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0, 0, 0, 0, 0)
       STRMATCH_ALL_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0) };
static const char *const js_str_match_steps[] = {
    STRMATCH_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.13 steps 1-2 (thisValue is RequireObjectCoercible'd) and step 3's `regexpOrPattern is an "
        "Object` test",
        "22.1.3.13 step 3.a (matcher is GetMethod(regexpOrPattern, %Symbol.match%))",
        "22.1.3.13 step 3.b.i (return Call(matcher, regexpOrPattern, «thisValue»))",
        "22.1.3.13 step 4 (string is ToString(thisValue))",
        "22.1.3.13 step 5 (regexp is RegExpCreate(regexpOrPattern, undefined))",
        "22.1.3.13 step 6 (Invoke's Get(regexp, %Symbol.match%))",
        "22.1.3.13 step 6 (Invoke's Call(matcher, regexp, «string»))")
    NULL };
static const char *const js_str_matchAll_steps[] = {
    STRMATCH_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.14 steps 1-2 (thisValue is RequireObjectCoercible'd) and step 3's `regexpOrPattern is an "
        "Object` test",
        "22.1.3.14 step 3.c (matcher is GetMethod(regexpOrPattern, %Symbol.matchAll%))",
        "22.1.3.14 step 3.d.i (return Call(matcher, regexpOrPattern, «thisValue»))",
        "22.1.3.14 step 4 (string is ToString(thisValue))",
        "22.1.3.14 step 5 (regexp is RegExpCreate(regexpOrPattern, \"g\"))",
        "22.1.3.14 step 6 (Invoke's Get(regexp, %Symbol.matchAll%))",
        "22.1.3.14 step 6 (Invoke's Call(matcher, regexp, «string»))")
    STRMATCH_ALL_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.14 step 3.a (isRegexp is IsRegExp(regexpOrPattern) — its Get(regexpOrPattern, "
        "%Symbol.match%))",
        "22.1.3.14 steps 3.b.i-ii (flags is Get(regexpOrPattern, \"flags\"); "
        "RequireObjectCoercible(flags))",
        "22.1.3.14 step 3.b.iii (whether ToString(flags) contains \"g\")")
    NULL };
static const char *const js_str_search_steps[] = {
    STRMATCH_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.21 steps 1-2 (thisValue is RequireObjectCoercible'd) and step 3's `regexpOrPattern is an "
        "Object` test",
        "22.1.3.21 step 3.a (searcher is GetMethod(regexpOrPattern, %Symbol.search%))",
        "22.1.3.21 step 3.b.i (return Call(searcher, regexpOrPattern, «thisValue»))",
        "22.1.3.21 step 4 (string is ToString(thisValue))",
        "22.1.3.21 step 5 (regexp is RegExpCreate(regexpOrPattern, undefined))",
        "22.1.3.21 step 6 (Invoke's Get(regexp, %Symbol.search%))",
        "22.1.3.21 step 6 (Invoke's Call(searcher, regexp, «string»))")
    NULL };

static int js_str_match_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSStrMatch *s = st;
    JSAtom atom = (JSAtom)s->hdr.arg;
    JSValueConst regexp = step_arg(&s->hdr, 0);
    JSValue str;
    int r;

    /* match and search have no IsRegExp and no `flags` read, so their arrays end at SM_INVOKED and a rest in
       one of matchAll's three is a stage past the end — which step_stage_check names on its own. */
    if (s->hdr.stage == SM_RECV) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->matcher = JS_UNDEFINED; s->str = JS_UNDEFINED; s->rx = JS_UNDEFINED;
        s->flags = JS_UNDEFINED; s->result = JS_UNDEFINED; s->isRegExp = 0;
        /* step 1: RequireObjectCoercible(this) */
        if (JS_IsUndefined(s->hdr.this_val) || JS_IsNull(s->hdr.this_val))
            return JS_ThrowTypeError(ctx, "cannot convert to object"), -1;
        /* step 2 gates on `regexp` being an OBJECT: a primitive's prototype is never consulted, which
           cstm-matcher-on-string-primitive.js checks by making String.prototype[@@match] a throwing getter. */
        if (!JS_IsObject(regexp)) s->hdr.stage = SM_STR;
        else s->hdr.stage = (atom == JS_ATOM_Symbol_matchAll) ? SM_ISREGEXP : SM_METHOD;
    }
    if (s->hdr.stage == SM_ISREGEXP) {
        /* matchAll step 2.a, IsRegExp: `? Get(argument, @@match)` decides it unless it is undefined, in which
           case the [[RegExpMatcher]] slot does. */
        JSValue m;
        r = step_getprop_run(ctx, &s->hdr, regexp, JS_ATOM_Symbol_match, cb_result, &m, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        if (JS_IsUndefined(m))
            s->isRegExp = (JS_VALUE_GET_OBJ(regexp)->class_id == JS_CLASS_REGEXP);
        else
            s->isRegExp = JS_ToBool(ctx, m);
        JS_FreeValue(ctx, m);
        s->hdr.stage = s->isRegExp ? SM_FLAGS : SM_METHOD;
    }
    if (s->hdr.stage == SM_FLAGS) {
        r = step_getprop_run(ctx, &s->hdr, regexp, JS_ATOM_flags, cb_result, &s->flags, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        /* step 2.b.ii: RequireObjectCoercible(flags) */
        if (JS_IsUndefined(s->flags) || JS_IsNull(s->flags))
            return JS_ThrowTypeError(ctx, "cannot convert to object"), -1;
        s->hdr.stage = SM_FLAGSTR;
    }
    if (s->hdr.stage == SM_FLAGSTR) {
        r = step_tostring_run(ctx, &s->hdr, s->flags, cb_result, &str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        JS_FreeValue(ctx, s->flags);
        s->flags = str;
        if (string_indexof_char(JS_VALUE_GET_STRING(s->flags), 'g', 0) < 0)
            return JS_ThrowTypeError(ctx, "regexp must have the 'g' flag"), -1;
        JS_FreeValue(ctx, s->flags); s->flags = JS_UNDEFINED;
        s->hdr.stage = SM_METHOD;
    }
    if (s->hdr.stage == SM_METHOD) {
        /* step 2.a (2.c for matchAll): GetMethod(regexp, @@x) — the read is the page's; an uncallable
           non-nullish result throws */
        r = step_getprop_run(ctx, &s->hdr, regexp, atom, cb_result, &s->matcher, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        if (JS_IsUndefined(s->matcher) || JS_IsNull(s->matcher)) {
            JS_FreeValue(ctx, s->matcher); s->matcher = JS_UNDEFINED;
            s->hdr.stage = SM_STR;
        } else {
            if (check_function(ctx, s->matcher)) return -1;
            s->cb[0] = (JSValue)regexp;      /* this */
            s->cb[1] = s->matcher;           /* a borrowed view of what the state owns */
            s->cb[2] = s->hdr.this_val;
            *out_cb = s->cb; *out_argc = 1;
            s->hdr.stage = SM_CALLED;
            return 3;
        }
    }
    if (s->hdr.stage == SM_CALLED) {
        s->result = cb_result;
        return 0;
    }
    if (s->hdr.stage == SM_STR) {
        r = step_tostring_run(ctx, &s->hdr, s->hdr.this_val, cb_result, &s->str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        /* step 4: RegExpCreate(regexp, undefined) — or "g" for matchAll. It is a real Construct of whatever
           %RegExp% currently is, so a subclassed or patched constructor is the page's code. */
        s->cb[0] = ctx->regexp_ctor;
        s->cb[1] = (JSValue)regexp;
        if (atom == JS_ATOM_Symbol_matchAll) {
            /* the "g" rides the STATE, not the request buffer: the request's slots are borrowed views, so an
               abandon between this construct and its result would otherwise leak the string. */
            s->flags = js_new_string8(ctx, "g");
            if (JS_IsException(s->flags)) { s->flags = JS_UNDEFINED; return -1; }
            s->cb[2] = s->flags;
            *out_argc = 2;
        } else {
            *out_argc = 1;
        }
        *out_cb = s->cb;
        s->hdr.stage = SM_CREATED;
        return 4;
    }
    if (s->hdr.stage == SM_CREATED) {
        JS_FreeValue(ctx, s->flags); s->flags = JS_UNDEFINED;
        s->rx = cb_result; cb_result = JS_UNDEFINED;
        s->hdr.stage = SM_INVOKE;
    }
    if (s->hdr.stage == SM_INVOKE) {
        /* step 5: Invoke(rx, @@x, «S») — the read and the call are two separate observable steps */
        r = step_getprop_run(ctx, &s->hdr, s->rx, atom, cb_result, &s->matcher, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        if (check_function(ctx, s->matcher)) return -1;
        s->cb[0] = s->rx;
        s->cb[1] = s->matcher;
        s->cb[2] = s->str;
        *out_cb = s->cb; *out_argc = 1;
        s->hdr.stage = SM_INVOKED;
        return 3;
    }
    DCHECK(s->hdr.stage == SM_INVOKED, "String.prototype.match resumed in an unknown stage");
    s->result = cb_result;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). cb is a request buffer whose four slots are BORROWED views of
   these fields, the header's captures and the intrinsic RegExp constructor — never visited. */
static void js_str_match_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSStrMatch *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->matcher);
    v->val(ctx, &s->str);
    v->val(ctx, &s->rx);
    v->val(ctx, &s->flags);
}

static JSValue js_str_match_fini(JSContext *ctx, void *st, bool take_result)
{
    JSStrMatch *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* GetSubstitution (22.1.3.19.1) as a RESUMABLE sub-sequence. Two-phase like every other one: it parks on the
   `$<name>` read (or on the ToString of what that read returned) and answers at the SAME call site.
     0 = *pout is the finished string, >0 = the caller must return that step code, -1 = threw.
   Everything else in the template walk is pure string work — `$&`, `` $` ``, `$'` and `$1`..`$99` read a
   matched substring or the caller's own captures array, neither of which can run the page's code. */
static int step_getsubst_run(JSContext *ctx, JSStepHdr *h, JSGetSubst *gs,
                             JSValueConst matched, JSValueConst str, JSValueConst captures,
                             JSValueConst namedCaptures, JSValueConst rep,
                             JSValue in, JSValue *pout, JSValue **out_cb, int *out_argc)
{
    StringBuffer *b = &gs->b;
    JSString *sp, *rp;
    JSValue s, tmp;
    uint32_t len;
    int c, c1, k, k1, r;

    if (!JS_IsString(rep) || !JS_IsString(str)) {
        JS_FreeValue(ctx, in);
        JS_ThrowTypeError(ctx, "not a string");
        return -1;
    }
    sp = JS_VALUE_GET_STRING(str);
    rp = JS_VALUE_GET_STRING(rep);
    len = rp->len;

    getsubst_stage_check(gs);
    if (gs->stage == GS_START) {
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        string_buffer_init(ctx, b, 0);
        gs->active = 1;
        gs->pending = JS_UNDEFINED;
        gs->name_atom = JS_ATOM_NULL;
        gs->captures_len = 0;
        if (!JS_IsUndefined(captures)) {
            if (js_get_length32(ctx, &gs->captures_len, captures))
                goto exception;
        }
        if (js_get_length32(ctx, &gs->matched_len, matched))
            goto exception;
        gs->i = 0;
        gs->stage = GS_WALK;
    } else if (gs->stage == GS_NAMED_GET) {
        r = step_getprop_run(ctx, h, namedCaptures, gs->name_atom, in, &gs->pending, out_cb, out_argc);
        if (r) return r < 0 ? -1 : r;
        gs->stage = GS_NAMED_STR;
        in = JS_UNDEFINED;
        goto named_str;
    } else {
        DCHECK(gs->stage == GS_NAMED_STR,
               "GetSubstitution resumed at a cursor that is neither of its two rest points");
     named_str:
        if (JS_IsUndefined(gs->pending)) {
            JS_FreeValue(ctx, in);
        } else {
            r = step_tostring_run(ctx, h, gs->pending, in, &tmp, out_cb, out_argc);
            if (r) return r < 0 ? -1 : r;
            JS_FreeValue(ctx, gs->pending);
            gs->pending = JS_UNDEFINED;
            if (string_buffer_concat_value_free(b, tmp))
                goto exception;
        }
        JS_FreeAtom(ctx, gs->name_atom);
        gs->name_atom = JS_ATOM_NULL;
        gs->i = gs->j;
        gs->stage = GS_WALK;
    }

    for(;;) {
        gs->j = string_indexof_char(rp, '$', gs->i);
        if (gs->j < 0 || gs->j + 1 >= (int)len)
            break;
        string_buffer_concat(b, rp, gs->i, gs->j);
        gs->j0 = gs->j++;
        c = string_get(rp, gs->j++);
        if (c == '$') {
            string_buffer_putc8(b, '$');
        } else if (c == '&') {
            if (string_buffer_concat_value(b, matched))
                goto exception;
        } else if (c == '`') {
            string_buffer_concat(b, sp, 0, gs->position);
        } else if (c == '\'') {
            string_buffer_concat(b, sp, gs->position + gs->matched_len, sp->len);
        } else if (c >= '0' && c <= '9') {
            k = c - '0';
            if (gs->j < (int)len) {
                c1 = string_get(rp, gs->j);
                if (c1 >= '0' && c1 <= '9') {
                    /* This behavior is specified in ES6 and refined in ECMA 2019 */
                    /* ECMA 2019 does not have the extra test, but
                       Test262 S15.5.4.11_A3_T1..3 require this behavior */
                    k1 = k * 10 + c1 - '0';
                    if (k1 >= 1 && k1 < (int)gs->captures_len) {
                        k = k1;
                        gs->j++;
                    }
                }
            }
            if (k >= 1 && k < (int)gs->captures_len) {
                /* the caller's OWN array of already-coerced captures: an ordinary element read on an engine
                   object, which is why this one is not a request. */
                s = JS_GetPropertyInt64(ctx, captures, k);
                if (JS_IsException(s))
                    goto exception;
                if (!JS_IsUndefined(s)) {
                    if (string_buffer_concat_value_free(b, s))
                        goto exception;
                }
            } else {
                goto norep;
            }
        } else if (c == '<' && !JS_IsUndefined(namedCaptures)) {
            JSValue name;
            k = string_indexof_char(rp, '>', gs->j);
            if (k < 0)
                goto norep;
            name = js_sub_string(ctx, rp, gs->j, k);
            if (JS_IsException(name))
                goto exception;
            gs->name_atom = JS_ValueToAtom(ctx, name);
            JS_FreeValue(ctx, name);
            if (gs->name_atom == JS_ATOM_NULL)
                goto exception;
            /* THE SUSPENSION. `groups` is the page's object, so this read is a request and so is the ToString
               of whatever it returns. gs->j moves past the `>` on the way back in. */
            gs->j = k + 1;
            gs->stage = GS_NAMED_GET;
            r = step_getprop_run(ctx, h, namedCaptures, gs->name_atom, JS_UNDEFINED,
                                 &gs->pending, out_cb, out_argc);
            if (r) return r < 0 ? -1 : r;
            gs->stage = GS_NAMED_STR;
            in = JS_UNDEFINED;
            goto named_str;
        } else {
        norep:
            string_buffer_concat(b, rp, gs->j0, gs->j);
        }
        gs->i = gs->j;
    }
    string_buffer_concat(b, rp, gs->i, rp->len);
    gs->active = 0;
    gs->stage = GS_START;
    *pout = string_buffer_end(b);
    return JS_IsException(*pout) ? -1 : 0;
exception:
    string_buffer_free(b);
    gs->active = 0;
    gs->stage = GS_START;
    JS_FreeAtom(ctx, gs->name_atom);
    gs->name_atom = JS_ATOM_NULL;
    JS_FreeValue(ctx, gs->pending);
    gs->pending = JS_UNDEFINED;
    return -1;
}

/* Release a substitution abandoned mid-walk — the machine was torn down while parked on `groups[name]`. */
/* WHAT A SUBSTITUTION CURSOR OWNS. Nested inside its machine's declaration rather than beside it, because it
   is a sub-object with its own lifetime: `active` says whether the accumulator has been initialised at all,
   and the key parked between `$<` and `>` is an atom reference held across the read it names. */
static void js_getsubst_visit(JSContext *ctx, JSGetSubst *gs, JSStepVisit *v)
{
    if (gs->active) v->strbuf(ctx, &gs->b);
    v->atom(ctx, &gs->name_atom);
    v->val(ctx, &gs->pending);
}

static void js_getsubst_free(JSContext *ctx, JSGetSubst *gs)
{
    js_getsubst_visit(ctx, gs, (JSStepVisit *)&js_step_visit_free);
    gs->active = 0;
}

/* String.prototype.replace / replaceAll with a FUNCTION replacer (22.1.3.19). The builtin holds a StringBuffer
   accumulator, endOfLastMatch and the is_first flag ACROSS each callback invocation, so it is continuation-holding:
   it cannot be call-site-unwrapped, and per §C-stack it stays in C while its callback dispatch is hooked into the
   flow machinery. Drives one match per step; replaceAll's loop is therefore preemptible at every callback boundary
   AND inside the callback body at every back-edge.
   Only the STRING-searchValue path lives here. An OBJECT searchValue dispatches to @@replace (a different function
   with a two-phase structure) and is deliberately NOT recognized — a separate step in its own right. */



/* 22.1.3.19 String.prototype.replace and 22.1.3.20 .replaceAll — ONE machine, `arg` selecting which, so ONE
   stage list expanded once per algorithm with that algorithm's own step text (see RES_STAGES for the shape).
   REPLACE'S ARRAY IS SHORTER: only replaceAll performs IsRegExp and the `flags` read, so those three rest
   points are a SEPARATE list appended after the eight they share, and a rest in one of them under `replace` is
   a stage past the end of its array — which step_stage_check names on its own.
   The stage NUMBERS are not the order the blocks appear in below; the label is the identity. */
#define STRREP_STAGES(X, RECV, REPLACER, DISPATCH, STR, SEARCH, REP, CALL, CALLSTR) \
    X(SRP_RECV,     RECV) \
    X(SRP_REPLACER, REPLACER) \
    X(SRP_DISPATCH, DISPATCH) \
    X(SRP_STR,      STR) \
    X(SRP_SEARCH,   SEARCH) \
    X(SRP_REP,      REP) \
    X(SRP_CALL,     CALL) \
    X(SRP_CALLSTR,  CALLSTR)
/* The three only replaceAll reaches. */
#define STRREP_ALL_STAGES(X, ISREGEXP, FLAGS, FLAGSTR) \
    X(SRP_ISREGEXP, ISREGEXP) \
    X(SRP_FLAGS,    FLAGS) \
    X(SRP_FLAGSTR,  FLAGSTR)
enum { STRREP_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0, 0, 0, 0, 0, 0)
       STRREP_ALL_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0) };
static const char *const js_str_replace_steps[] = {
    STRREP_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.19 steps 1-2 (thisValue is RequireObjectCoercible'd) and step 3's `searchValue is an "
        "Object` test",
        "22.1.3.19 step 3.a (replacer is GetMethod(searchValue, @@replace))",
        "22.1.3.19 step 3.b.i (return Call(replacer, searchValue, «thisValue, replaceValue»))",
        "22.1.3.19 step 4 (string is ToString(thisValue))",
        "22.1.3.19 step 5 (searchString is ToString(searchValue))",
        "22.1.3.19 steps 6-7.a (functionalReplace is IsCallable(replaceValue); if not, ToString it)",
        "22.1.3.19 step 13.a (Call(replaceValue, undefined, «searchString, position, string»))",
        "22.1.3.19 step 13.a (ToString of what that Call returned)")
    NULL };
static const char *const js_str_replaceAll_steps[] = {
    STRREP_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.20 steps 1-2 (thisValue is RequireObjectCoercible'd) and step 3's `searchValue is an "
        "Object` test",
        "22.1.3.20 step 3.c (replacer is GetMethod(searchValue, @@replace))",
        "22.1.3.20 step 3.d.i (return Call(replacer, searchValue, «thisValue, replaceValue»))",
        "22.1.3.20 step 4 (string is ToString(thisValue))",
        "22.1.3.20 step 5 (searchString is ToString(searchValue))",
        "22.1.3.20 steps 6-7.a (functionalReplace is IsCallable(replaceValue); if not, ToString it)",
        "22.1.3.20 step 15.b.i (Call(replaceValue, undefined, «searchString, matchPosition, string»))",
        "22.1.3.20 step 15.b.i (ToString of what that Call returned)")
    STRREP_ALL_STAGES(JS_STEP_STAGE_LABEL,
        "22.1.3.20 step 3.a (isRegExp is IsRegExp(searchValue) — its Get(searchValue, @@match))",
        "22.1.3.20 steps 3.b.i-ii (flags is Get(searchValue, \"flags\"); RequireObjectCoercible(flags))",
        "22.1.3.20 step 3.b.iii (whether ToString(flags) contains \"g\")")
    NULL };

/* 0 = DONE (s->result is the answer), 3 = CALL the replacer on the tramp, 5 = ToString on its result, -1 = error.
   Mirrors the C loop exactly, split at the points where it re-enters JS — of which there are TWO, not one.
   22.1.3.19 states the replacer step as `? ToString(? Call(replaceValue, undefined, «searched, position,
   string»))`, so the CALL and the ToString of what it returned are two rest points of one step and the walk has
   to resume into the right one. They are two STAGES: `cb_pending` said the same thing privately, which is the
   numbering this conversion removes — a private cursor beside the stage is a suspension point the algorithm
   cannot name, and a park between the call and the coercion could only report itself as "somewhere in the
   walk". */
static int js_str_replace_step(JSContext *ctx, JSStrReplace *s, JSValue cb_result,
                               JSValue **out_cb, int *out_argc)
{
    JSString *sp = JS_VALUE_GET_STRING(s->str);
    JSString *searchp = JS_VALUE_GET_STRING(s->search_str);
    JSValue repl = JS_UNDEFINED;
    int r;

    if (s->hdr.stage == SRP_CALL) {
        /* the replacer returned: ToString its result AS A REQUEST, then splice it in at the recorded position.
           step_tostring_run dups the operand into the header when it has to hold it across the coercion, so
           this reference can be released either way — and the stage moves BEFORE the request, because the
           coercion answers at SRP_CALLSTR's call site and not at this one. */
        s->hdr.stage = SRP_CALLSTR;
        r = step_tostring_run(ctx, &s->hdr, cb_result, JS_UNDEFINED, &repl, out_cb, out_argc);
        JS_FreeValue(ctx, cb_result);
        if (r) return r < 0 ? -1 : r;
        goto have_replacement;
    }
    if (s->hdr.stage == SRP_CALLSTR) {
        r = step_tostring_run(ctx, &s->hdr, JS_UNDEFINED, cb_result, &repl, out_cb, out_argc);
        if (r) return r < 0 ? -1 : r;
        goto have_replacement;
    }
    /* the walk is ENTERED, not resumed: the prologue's last stage falls straight into the search below, and the
       driver hands a fresh entry nothing to deliver. */
    DCHECK(JS_IsUndefined(cb_result), "the replace walk was entered with an undelivered request answer");
    goto search;
 have_replacement:
    string_buffer_concat(&s->b, sp, s->endOfLastMatch, s->pos);
    string_buffer_concat_value_free(&s->b, repl);
    s->endOfLastMatch = s->pos + searchp->len;
    s->is_first = 0;
    if (!s->is_replaceAll)
        goto finish;
 search:
    for (;;) {
        if (unlikely(searchp->len == 0)) {
            if (s->is_first)
                s->pos = 0;
            else if (s->endOfLastMatch >= sp->len)
                s->pos = -1;
            else
                s->pos = s->endOfLastMatch + 1;
        } else {
            s->pos = string_indexof(sp, searchp, s->endOfLastMatch);
        }
        if (s->pos < 0) {
            if (s->is_first) {
                /* no match at all: the ORIGINAL string is returned untouched (not a rebuilt copy) */
                string_buffer_free(&s->b);
                s->result = js_dup(s->str);
                return 0;
            }
            break;
        }
        if (!s->functional) {
            /* same walk, different substitution SOURCE: GetSubstitution (22.1.3.19 step 14.c) instead of the
               callback, spliced by the SAME three lines the callback's answer is — one splice, not two copies
               of it. Not a separate no-callback body. */
            s->gs.stage = GS_START;
            s->gs.position = s->pos;
            /* No captures and no `groups`: `$<name>` is inert and `$1` has nothing to read, so this walk has
               no page-observable operation in it and cannot park. Same ONE implementation as @@replace's; the
               DCHECK is what says the difference is structural and not a second driver. */
            r = step_getsubst_run(ctx, &s->hdr, &s->gs, s->search_str, s->str, JS_UNDEFINED,
                                  JS_UNDEFINED, s->rep_val, JS_UNDEFINED, &repl, out_cb, out_argc);
            DCHECK(r <= 0, "String.prototype.replace's substitution asked to suspend, which needs a `groups` "
                           "object it is never given");
            if (r < 0)
                return -1;
            goto have_replacement;
        }
        s->cb_args[0] = JS_UNDEFINED;                /* thisArgument */
        s->cb_args[2] = s->search_str;               /* borrowed: matched */
        s->cb_args[3] = js_int32(s->pos);            /* position */
        s->cb_args[4] = s->str;                      /* borrowed: the whole string */
        s->hdr.stage = SRP_CALL;
        return 3;
    }
finish:
    string_buffer_concat(&s->b, sp, s->endOfLastMatch, sp->len);
    s->result = string_buffer_end(&s->b);
    if (JS_IsException(s->result))
        return -1;
    return 0;
}

/* Recognized only when every operand is already a primitive of the right type: this_val a STRING (so no ToString
   side effect is skipped), searchValue a STRING (an object would dispatch to @@replace), and the replacer a plain
   bytecode function (a C/bound replacer has no preemptible body, so the ordinary path is already correct). */

/* String.prototype.replace/replaceAll IS a step machine. The @@replace dispatch below used to be a JS_CallFree
   straight out of C, which is exactly what kept "aaa".replace(/a/g, fn) driving to completion: the callee is
   itself a step builtin, so calling it from C ran its callbacks through an inline JS_Call that cannot suspend.
   The dispatch is now step code 3 — @@replace is invoked ON THE TRAMP like any other call — so the whole chain
   (str.replace -> @@replace -> the replacer) is preemptible end to end, and no recognizer is involved anywhere. */
/* 22.1.3.19 step 3 and 22.1.3.20 step 3. FOUR reads of the page's code lived in a prologue that cannot suspend:
   IsRegExp's `? Get(searchValue, %Symbol.match%)`, replaceAll's `? Get(searchValue, "flags")`, the ToString on
   that result, and `? GetMethod(searchValue, %Symbol.replace%)`. Each was a JS_GetProperty / JS_ToStringFree
   straight out of C, so an accessor, a Proxy trap or a toString containing a loop had no flow base and aborted.
   They are stages now, in the spec's order — which is also observable: the flags read precedes the @@replace
   read, and only replaceAll performs the IsRegExp check at all. */

static int js_str_replace_vstep(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSStrReplace *s = st;
    JSValueConst searchValue = step_arg(&s->hdr, 0);
    int rr;
    if (s->hdr.stage == SRP_RECV) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (JS_IsUndefined(s->hdr.this_val) || JS_IsNull(s->hdr.this_val)) {
            JS_ThrowTypeError(ctx, "cannot convert to object");
            return -1;
        }
        s->is_replaceAll = s->hdr.arg;
        if (!JS_IsObject(searchValue))
            goto plain_walk;          /* step 2 does not apply to a primitive: it reads nothing */
        if (!s->is_replaceAll)
            goto have_gflag;          /* only replaceAll performs IsRegExp and the 'g' requirement */
        /* the stage moves only on the path that reaches it: `replace`'s array ends at SRP_CALLSTR, so naming
           one of replaceAll's three even in passing would put it in a stage its algorithm does not have. */
        s->hdr.stage = SRP_ISREGEXP;
        rr = step_getprop_run(ctx, &s->hdr, searchValue, JS_ATOM_Symbol_match, JS_UNDEFINED,
                              &cb_result, out_cb, out_argc);
        if (rr) return rr < 0 ? -1 : rr;
    }
    if (s->hdr.stage == SRP_ISREGEXP) {
        JSValue m = JS_UNDEFINED;
        rr = step_getprop_run(ctx, &s->hdr, searchValue, JS_ATOM_Symbol_match, cb_result, &m, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (rr) return rr < 0 ? -1 : rr;
        s->hdr.stage = SRP_FLAGS;
        /* IsRegExp steps 2-4: a DEFINED @@match decides by ToBoolean, otherwise the [[RegExpMatcher]] slot does */
        if (JS_IsUndefined(m) ? js_get_regexp(ctx, searchValue, false) == NULL : !JS_ToBoolFree(ctx, m))
            goto have_gflag;
        rr = step_getprop_run(ctx, &s->hdr, searchValue, JS_ATOM_flags, JS_UNDEFINED,
                              &cb_result, out_cb, out_argc);
        if (rr) return rr < 0 ? -1 : rr;
    }
    if (s->hdr.stage == SRP_FLAGS) {
        JSValue flags = JS_UNDEFINED;
        rr = step_getprop_run(ctx, &s->hdr, searchValue, JS_ATOM_flags, cb_result, &flags, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (rr) return rr < 0 ? -1 : rr;
        if (JS_IsUndefined(flags) || JS_IsNull(flags)) {
            JS_FreeValue(ctx, flags);
            JS_ThrowTypeError(ctx, "cannot convert to object");
            return -1;
        }
        s->flags_val = flags;         /* held across its own ToString, which is a separate suspension point */
        s->hdr.stage = SRP_FLAGSTR;
        rr = step_tostring_run(ctx, &s->hdr, s->flags_val, JS_UNDEFINED, &cb_result, out_cb, out_argc);
        if (rr) return rr < 0 ? -1 : rr;
    }
    if (s->hdr.stage == SRP_FLAGSTR) {
        JSValue fs = JS_UNDEFINED;
        int has_g;
        rr = step_tostring_run(ctx, &s->hdr, s->flags_val, cb_result, &fs, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (rr) return rr < 0 ? -1 : rr;
        has_g = string_indexof_char(JS_VALUE_GET_STRING(fs), 'g', 0);
        JS_FreeValue(ctx, fs);
        JS_FreeValue(ctx, s->flags_val);
        s->flags_val = JS_UNDEFINED;
        if (has_g < 0) {
            JS_ThrowTypeError(ctx, "regexp must have the 'g' flag");
            return -1;
        }
    have_gflag:
        s->hdr.stage = SRP_REPLACER;
        rr = step_getprop_run(ctx, &s->hdr, searchValue, JS_ATOM_Symbol_replace, JS_UNDEFINED,
                              &cb_result, out_cb, out_argc);
        if (rr) return rr < 0 ? -1 : rr;
    }
    if (s->hdr.stage == SRP_REPLACER) {
        JSValue replacer = JS_UNDEFINED;
        rr = step_getprop_run(ctx, &s->hdr, searchValue, JS_ATOM_Symbol_replace, cb_result, &replacer,
                              out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (rr) return rr < 0 ? -1 : rr;
        if (!JS_IsUndefined(replacer) && !JS_IsNull(replacer)) {
            const JSTrampStepDef *isd = tramp_step_def_of(replacer);
            if (isd) {
                /* the BUILT-IN @@replace, which is itself a step machine. Calling it would mean a step builtin
                   invoking a step builtin — the inline JS_Call in the driver cannot suspend, which is exactly the
                   drive-to-completion being removed. DELEGATE instead: run that one machine directly, so the whole
                   str.replace -> @@replace -> replacer chain is a single preemptible walk. */
                JSValueConst iargv[2];
                void *inner;
                iargv[0] = s->hdr.this_val;      /* the subject */
                iargv[1] = step_arg(&s->hdr, 1); /* the replacer */
                inner = tramp_step_state_new(ctx, isd, searchValue, 2, iargv, replacer);
                JS_FreeValue(ctx, replacer);
                if (!inner) return -1;
                s->inner = inner;
                s->mode = 3;
                s->hdr.stage = SRP_DISPATCH;
                goto dispatch;
            }
            /* a USER-DEFINED @@replace: an ordinary callable, invoked on the tramp like any other call */
            s->mode = 1;
            s->cb_args[0] = js_dup(searchValue);              /* this = the searchValue */
            s->cb_args[1] = replacer;                         /* owned */
            s->cb_args[2] = js_dup(s->hdr.this_val);          /* the subject */
            s->cb_args[3] = js_dup(step_arg(&s->hdr, 1));
            s->hdr.stage = SRP_DISPATCH;
            *out_cb = s->cb_args; *out_argc = 2;
            return 3;
        }
        JS_FreeValue(ctx, replacer);
    plain_walk:
        /* The plain string-search walk — ONE walk for both replacer kinds. A callable drives step-code 3;
           anything else is ToString'd once and fed to GetSubstitution inside the same loop. */
        s->functional = JS_IsFunction(ctx, step_arg(&s->hdr, 1));
        if (s->functional)
            s->cb_args[1] = js_dup(step_arg(&s->hdr, 1));
        /* MODE 0 FROM HERE, not from the end of the prologue. The replacer this just dup'd into cb_args[1] is
           declared by mode 0's visit and by no other, so the transient mode that used to stand between them was
           a window in which the machine owned a reference its own declaration did not name: a searchValue whose
           `toString` throws tore the state down through a visit that never saw cb_args[1], and the replacer
           leaked. The accumulator's own `b.ctx` already says it has not been built yet, which is what makes
           mode 0 correct this early. */
        s->mode = 0;
        s->hdr.stage = SRP_STR;
    }
    /* steps 3, 4 and 6.a: ToString on the receiver, the search value and — when it is not callable — the
       replacement. Each is the page's code, so each is its own stage. */
    if (s->hdr.stage == SRP_STR) {
        rr = step_tostring_run(ctx, &s->hdr, s->hdr.this_val, cb_result, &s->str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (rr) return rr < 0 ? -1 : rr;
        s->hdr.stage = SRP_SEARCH;
    }
    if (s->hdr.stage == SRP_SEARCH) {
        rr = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->search_str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (rr) return rr < 0 ? -1 : rr;
        s->hdr.stage = SRP_REP;
    }
    if (s->hdr.stage == SRP_REP) {
        if (!s->functional) {
            rr = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 1), cb_result, &s->rep_val, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (rr) return rr < 0 ? -1 : rr;
        }
        string_buffer_init(ctx, &s->b, 0);
        s->is_first = 1;
        s->gs.pending = JS_UNDEFINED; s->gs.name_atom = JS_ATOM_NULL; s->gs.active = 0;
        /* the stage stays at SRP_REP: the walk below is ENTERED from here, and the two stages it rests at are
           its own. */
    }
 dispatch:
    if (s->hdr.stage == SRP_DISPATCH) {
        /* step 2.b.i (2.d.i). A DELEGATED built-in @@replace is a machine on the chain, and the outer rests at
           this one step for as long as it runs; a user-defined one is an ordinary call whose result IS the
           answer. */
        if (s->mode == 3) {
            /* FORWARDING PUTS THE DRIVER'S HEADER AND THE STEPPED MACHINE'S HEADER OUT OF STEP, which is the
               limitation quickjs-step.h already states about this one call: the inner's requests are answered
               through cb_result and survive it, but anything the inner records ON ITS OWN HEADER lands where
               the driver — which is stepping the OUTER — will not look. The unknown operand a coercion records
               is exactly that, so it is MOVED here, at the one site that forwards, under the rule
               step_coerce_unknown keeps on one header: the first operand stays unless the inner stopped on one
               with no example. The identity the derivation then composes is the OUTER's algorithm and the
               OUTER's stage, which is the honest name for it: 22.1.3.19 is the algorithm the page called and
               22.2.6.11 is a sub-operation of it. */
            JSStepHdr *ih = s->inner;
            int r = ih->def->step(ctx, ih, cb_result, out_cb, out_argc);
            if (!JS_IsUninitialized(ih->unknown_operand)) {
                if (JS_IsUninitialized(s->hdr.unknown_operand) || r == JS_STEP_UNKNOWN) {
                    JS_FreeValue(ctx, s->hdr.unknown_operand);
                    s->hdr.unknown_operand = ih->unknown_operand;
                    s->hdr.unknown_stage = s->hdr.stage;
                } else {
                    JS_FreeValue(ctx, ih->unknown_operand);
                }
                ih->unknown_operand = JS_UNINITIALIZED;
            }
            return r;
        }
        DCHECK(s->mode == 1, "str.replace rested at the @@replace dispatch with no replacer to run");
        s->result = cb_result;
        return 0;
    }
    {   /* the string-search walk, one match per step. The walk sets its OWN request buffer for a
           ToString (step-code 5); only the replacer CALL uses the machine's cb_args. */
        int r = js_str_replace_step(ctx, s, cb_result, out_cb, out_argc);
        if (r == 3) { *out_cb = s->cb_args; *out_argc = 3; }
        return r;
    }
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit), which in mode 3 is a DELEGATED MACHINE — the built-in
   @@replace it handed its whole walk to. That composes: the delegate clones through the same path as any other
   machine, so a fork inside it is a fork inside a machine, not a hole. The subject strings are held in every
   mode; the accumulator and the cb_args are held per mode, and stating which is what stopped mode 0's replacer
   reference and mode 1's four operands from being released twice or not at all. */
static void js_str_replace_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSStrReplace *s = st;
    v->val(ctx, &s->str);
    v->val(ctx, &s->search_str);
    v->val(ctx, &s->flags_val);
    if (s->mode == 3) { v->machine(ctx, &s->inner); return; }   /* the delegate owns the rest of the walk */
    v->val(ctx, &s->result);
    v->val(ctx, &s->rep_val);
    js_getsubst_visit(ctx, &s->gs, v);
    if (s->mode == 1) {
        int i;
        for (i = 0; i < 4; i++) v->val(ctx, &s->cb_args[i]);
    } else if (s->mode == 0) {
        if (s->functional) v->val(ctx, &s->cb_args[1]);   /* the replacer dup'd by the prologue */
        if (s->b.ctx) v->strbuf(ctx, &s->b);              /* NULL ctx = the prologue threw before it was built */
    }
}

static JSValue js_str_replace_fini(JSContext *ctx, void *st, bool take_result)
{
    JSStrReplace *s = st;
    JSValue r;
    if (s->mode == 3) {
        /* the RESULT is the delegate's, so the hand-out is a teardown of the inner machine rather than a field
           of this one — the one thing the declaration above cannot say for it. */
        DCHECK(s->inner != NULL, "str.replace mode 3 without a delegated machine");
        r = tramp_step_state_free(ctx, s->inner, take_result);
        s->inner = NULL;
        return r;
    }
    r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    /* `gs.active` is LEFT SET. js_getsubst_visit visits the substitution's accumulator only while it is, and
       the discharge runs after this — clearing it here would hide that buffer from the one list that frees it. */
    return r;
}


/* 22.1.3.23 String.prototype.split, AS THE SPEC NUMBERS IT. The walk after step 6 runs none of the page's code —
   the array it fills is one this engine built and nothing on it is reachable until it is returned — so ONE stage
   names that whole range, and the label says which range it is. */
#define SPLIT_STAGES(X) \
    X(SPL_RECV,     "22.1.3.23 steps 1-2 (thisValue is RequireObjectCoercible'd) and step 3's `separator is an Object` test") \
    X(SPL_SPLITTER, "22.1.3.23 step 3.a (splitter is GetMethod(separator, %Symbol.split%))") \
    X(SPL_CALLED,   "22.1.3.23 step 3.b.i (return Call(splitter, separator, «thisValue, limit»))") \
    X(SPL_STR,      "22.1.3.23 step 4 (string is ToString(thisValue))") \
    X(SPL_LIM,      "22.1.3.23 step 5 (lim is ToUint32(limit), or 2**32-1 when it is undefined)") \
    X(SPL_SEP,      "22.1.3.23 step 6 (separatorString is ToString(separator))") \
    X(SPL_WALK,     "22.1.3.23 steps 7-18 (the scan and CreateArrayFromList — no page-observable operation)")
enum { SPLIT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_str_split_steps[] = { SPLIT_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_str_split_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSStrSplit *s = st;
    JSValueConst O = s->hdr.this_val;
    JSValueConst separator = step_arg(&s->hdr, 0);
    JSValue A, T;
    JSString *sp, *rp;
    uint32_t lengthA;
    int64_t p, q, slen, rlen, e;
    int r;

    if (s->hdr.stage == SPL_RECV) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        s->str = JS_UNDEFINED; s->sep = JS_UNDEFINED; s->result = JS_UNDEFINED;
        if (JS_IsUndefined(O) || JS_IsNull(O)) {
            JS_ThrowTypeError(ctx, "cannot convert to object");
            return -1;
        }
        s->hdr.stage = SPL_STR;
        if (JS_IsObject(separator)) {
            s->hdr.stage = SPL_SPLITTER;
            s->hdr.cb_coerce[0] = (JSValue)separator;   /* borrowed: hdr.argv holds it */
            *out_cb = s->hdr.cb_coerce; *out_argc = (int)JS_ATOM_Symbol_split;
            return 6;
        }
    }
    if (s->hdr.stage == SPL_SPLITTER) {                 /* the @@split method arrived */
        JSValue m = cb_result;
        cb_result = JS_UNDEFINED;
        s->hdr.stage = SPL_STR;
        if (!JS_IsUndefined(m) && !JS_IsNull(m)) {
            s->cb[0] = js_dup(separator);    /* this = the separator */
            s->cb[1] = m;                    /* owned */
            s->cb[2] = js_dup(O);
            s->cb[3] = js_dup(step_arg(&s->hdr, 1));
            s->hdr.stage = SPL_CALLED;
            *out_cb = s->cb; *out_argc = 2;
            return 3;
        }
        JS_FreeValue(ctx, m);
    }
    if (s->hdr.stage == SPL_CALLED) {                 /* @@split's result IS the answer */
        s->result = cb_result;
        return 0;
    }
    if (s->hdr.stage == SPL_STR) {
        r = step_tostring_run(ctx, &s->hdr, O, cb_result, &s->str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = SPL_LIM;
    }
    if (s->hdr.stage == SPL_LIM) {
        s->lim32 = -1;                       /* the whole 32-bit range, i.e. 0xffffffff read unsigned */
        if (!JS_IsUndefined(step_arg(&s->hdr, 1))) {
            /* ToUint32 is ToInt32's wrap read unsigned, so one conversion serves both */
            r = step_toint32_run(ctx, &s->hdr, s->hdr.argv[1], cb_result, &s->lim32, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
        }
        s->hdr.stage = SPL_SEP;
    }
    if (s->hdr.stage == SPL_SEP) {
        r = step_tostring_run(ctx, &s->hdr, separator, cb_result, &s->sep, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = SPL_WALK;
    }
    JS_FreeValue(ctx, cb_result);

    {   /* the walk: pure string scanning, no user code */
        uint32_t lim = (uint32_t)s->lim32;
        A = JS_NewArray(ctx);
        if (JS_IsException(A)) return -1;
        lengthA = 0;
        sp = JS_VALUE_GET_STRING(s->str);
        slen = sp->len;
        rp = JS_VALUE_GET_STRING(s->sep);
        rlen = rp->len;
        p = 0;
        if (lim == 0)
            goto done;
        if (JS_IsUndefined(separator))
            goto add_tail;
        if (slen == 0) {
            if (rlen != 0)
                goto add_tail;
            goto done;
        }
        for (q = p; (q += !rlen) <= slen - rlen - !rlen; q = p = e + rlen) {
            e = string_indexof(sp, rp, q);
            if (e < 0)
                break;
            T = js_sub_string(ctx, sp, p, e);
            if (JS_IsException(T)) goto fail;
            if (JS_CreateDataPropertyUint32(ctx, A, lengthA++, T, 0) < 0) goto fail;
            if (lengthA == lim)
                goto done;
        }
    add_tail:
        T = js_sub_string(ctx, sp, p, slen);
        if (JS_IsException(T)) goto fail;
        if (JS_CreateDataPropertyUint32(ctx, A, lengthA++, T, 0) < 0) goto fail;
    done:
        s->result = A;
        return 0;
    fail:
        JS_FreeValue(ctx, A);
        return -1;
    }
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). @@split is a method the page can define, and the separator and limit are coerced before it runs. */
static void js_str_split_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSStrSplit *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->str);
    v->val(ctx, &s->sep);
    v->val(ctx, &s->cb[0]);
    v->val(ctx, &s->cb[1]);
    v->val(ctx, &s->cb[2]);
    v->val(ctx, &s->cb[3]);
}

static JSValue js_str_split_fini(JSContext *ctx, void *st, bool take_result)
{
    JSStrSplit *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}







/* return 0 if before the first char */
static int string_prevc(JSString *p, int *pidx)
{
    int idx, c, c1;

    idx = *pidx;
    if (idx <= 0)
        return 0;
    idx--;
    if (p->is_wide_char) {
        c = str16(p)[idx];
        if (is_lo_surrogate(c) && idx > 0) {
            c1 = str16(p)[idx - 1];
            if (is_hi_surrogate(c1)) {
                c = from_surrogate(c1, c);
                idx--;
            }
        }
    } else {
        c = str8(p)[idx];
    }
    *pidx = idx;
    return c;
}

static bool test_final_sigma(JSString *p, int sigma_pos)
{
    int k, c1;

    /* before C: skip case ignorable chars and check there is
       a cased letter */
    k = sigma_pos;
    for(;;) {
        c1 = string_prevc(p, &k);
        if (!lre_is_case_ignorable(c1))
            break;
    }
    if (!lre_is_cased(c1))
        return false;

    /* after C: skip case ignorable chars and check there is
       no cased letter */
    k = sigma_pos + 1;
    for(;;) {
        if (k >= p->len)
            return true;
        c1 = string_getc(p, &k);
        if (!lre_is_case_ignorable(c1))
            break;
    }
    return !lre_is_cased(c1);
}

static int to_utf32_buf(JSContext *ctx, JSString *p, uint32_t **pbuf)
{
    uint32_t *b;
    int i, j, n;

    j = -1;
    n = p->len;
    b = js_malloc(ctx, max_int(1, n) * sizeof(*b));
    if (b)
        for (i = j = 0; i < n;)
            b[j++] = string_getc(p, &i);
    *pbuf = b;
    return j;
}

/* DELETED: js_string_localeCompare. Its two ToStrings — the receiver's and `that`'s — were the only user code
   in it and both ran from the C entry, so `"a".localeCompare({toString(){for(;;){}}})` preempted with no flow
   base. The comparison itself is STRRECV_LOCALECOMPARE's tail: the same machine that already owns "coerce the
   receiver to a string, coerce one argument, then compute". */

/* 22.1.3.28/29 and their locale twins, minus steps 1-2: the receiver is coerced by js_str_recv_step, which
   performs RequireObjectCoercible + ToString as a request. This body runs no user code at all. */
static JSValue js_string_case_body(JSContext *ctx, JSValueConst strv, int to_lower)
{
    JSValue val;
    StringBuffer b_s, *b = &b_s;
    JSString *p;
    int i, c, j, l;
    uint32_t res[LRE_CC_RES_LEN_MAX];

    DCHECK(JS_IsString(strv), "the case-conversion body takes the receiver its machine already coerced");
    val = js_dup(strv);
    p = JS_VALUE_GET_STRING(val);
    if (p->len == 0)
        return val;
    if (string_buffer_init(ctx, b, p->len))
        goto fail;
    for(i = 0; i < p->len;) {
        c = string_getc(p, &i);
        if (c == 0x3a3 && to_lower && test_final_sigma(p, i - 1)) {
            res[0] = 0x3c2; /* final sigma */
            l = 1;
        } else {
            l = lre_case_conv(res, c, to_lower);
        }
        for(j = 0; j < l; j++) {
            if (string_buffer_putc(b, res[j]))
                goto fail;
        }
    }
    JS_FreeValue(ctx, val);
    return string_buffer_end(b);
 fail:
    JS_FreeValue(ctx, val);
    string_buffer_free(b);
    return JS_EXCEPTION;
}

/* return (-1, NULL) if exception, otherwise (len, buf) */
/* THE OPERAND IS ALREADY A COERCED STRING. Its one caller is js_str_recv_step's normalize arm, whose receiver
   came from step_thisstring_run — §7.1.19 over unknown external input converts the operand's example there
   (or stops the machine where there is none), so the body only ever sees a String. 22.1.3.12
   localeCompare reads its two operands the same way and does not reach here at all. Asserted rather than left
   as a reachability argument, because the argument is about the caller and the crash would be here. */
static int JS_ToUTF32String(JSContext *ctx, uint32_t **pbuf, JSValue val1)
{
    JSValue val;
    int len;

    DCHECK(!(g_concolic.is(val1)),
           "JS_ToUTF32String over unknown external input — 22.1.3.15 String.prototype.normalize takes its "
           "receiver through step_thisstring_run, which collapses the machine to a derived concolic before any "
           "of this runs, so an unknown here means a caller coerced outside that sub-sequence");
    val = JS_ToString(ctx, val1);
    if (JS_IsException(val))
        return -1;
    len = to_utf32_buf(ctx, JS_VALUE_GET_STRING(val), pbuf);
    JS_FreeValue(ctx, val);
    return len;
}

static JSValue JS_NewUTF32String(JSContext *ctx, const uint32_t *buf, int len)
{
    int i;
    StringBuffer b_s, *b = &b_s;
    if (string_buffer_init(ctx, b, len))
        return JS_EXCEPTION;
    for(i = 0; i < len; i++) {
        if (string_buffer_putc(b, buf[i]))
            goto fail;
    }
    return string_buffer_end(b);
 fail:
    string_buffer_free(b);
    return JS_EXCEPTION;
}


/* String Iterator */

static JSValue js_string_iterator_next(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv,
                                       int *pdone, int magic)
{
    JSArrayIteratorData *it;
    uint32_t idx, c, start;
    JSString *p;

    it = array_iter_reach(ctx, this_val, JS_CLASS_STRING_ITERATOR);
    if (!it) {
        *pdone = false;
        return JS_EXCEPTION;
    }
    if (JS_IsUndefined(it->obj))
        goto done;
    p = JS_VALUE_GET_STRING(it->obj);
    idx = it->idx;
    if (idx >= p->len) {
        JS_FreeValue(ctx, it->obj);
        it->obj = JS_UNDEFINED;
    done:
        *pdone = true;
        return JS_UNDEFINED;
    }

    start = idx;
    c = string_getc(p, (int *)&idx);
    it->idx = idx;
    *pdone = false;
    if (c <= 0xffff) {
        return js_new_string_char(ctx, c);
    } else {
        return js_new_string16_len(ctx, str16(p) + start, 2);
    }
}

/* ES6 Annex B 2.3.2 etc. */
enum {
    magic_string_anchor,
    magic_string_big,
    magic_string_blink,
    magic_string_bold,
    magic_string_fixed,
    magic_string_fontcolor,
    magic_string_fontsize,
    magic_string_italics,
    magic_string_link,
    magic_string_small,
    magic_string_strike,
    magic_string_sub,
    magic_string_sup,
};

/* B.2.2.2.1 CreateHTML from step 3 on: the receiver's RequireObjectCoercible + ToString (steps 1-2) and the
   attribute value's ToString (step 4.a) are the page's code and are performed by js_str_recv_step as requests.
   `valuev` is the ALREADY-COERCED attribute string, or UNINITIALIZED for a tag that takes none. */
static JSValue js_string_CreateHTML(JSContext *ctx, JSValueConst strv, JSValueConst valuev, int magic)
{
    JSValue str;
    JSString *p;
    StringBuffer b_s, *b = &b_s;
    static struct { const char *tag, *attr; } const defs[] = {
        { "a", "name" }, { "big", NULL }, { "blink", NULL }, { "b", NULL },
        { "tt", NULL }, { "font", "color" }, { "font", "size" }, { "i", NULL },
        { "a", "href" }, { "small", NULL }, { "strike", NULL },
        { "sub", NULL }, { "sup", NULL },
    };

    DCHECK(JS_IsString(strv), "the CreateHTML body takes the receiver its machine already coerced");
    str = js_dup(strv);
    string_buffer_init(ctx, b, 7);
    string_buffer_putc8(b, '<');
    string_buffer_puts8(b, defs[magic].tag);
    if (defs[magic].attr) {
        // r += " " + attr + "=\"" + value + "\"";
        JSValue value;
        int i;

        string_buffer_putc8(b, ' ');
        string_buffer_puts8(b, defs[magic].attr);
        string_buffer_puts8(b, "=\"");
        /* step 4.a is `? ToString(value)`, NOT RequireObjectCoercible-then-ToString: `"x".fontcolor()` is
           <font color="undefined">x</font>, and JS_ToStringCheckObject threw a TypeError for it instead. No test
           covers this — annexB is not in test262.conf — which is why it survived; the spec is the oracle. */
        DCHECK(JS_IsString(valuev), "the CreateHTML body takes the attribute value its machine already coerced");
        value = js_dup(valuev);
        p = JS_VALUE_GET_STRING(value);
        for (i = 0; i < p->len; i++) {
            int c = string_get(p, i);
            if (c == '"') {
                string_buffer_puts8(b, "&quot;");
            } else {
                string_buffer_putc16(b, c);
            }
        }
        JS_FreeValue(ctx, value);
        string_buffer_putc8(b, '\"');
    }
    // return r + ">" + str + "</" + tag + ">";
    string_buffer_putc8(b, '>');
    string_buffer_concat_value_free(b, str);
    string_buffer_puts8(b, "</");
    string_buffer_puts8(b, defs[magic].tag);
    string_buffer_putc8(b, '>');
    return string_buffer_end(b);
}

static const JSCFunctionListEntry js_string_funcs[] = {
    JS_CFUNC_STEP_DEF("fromCharCode", 1, STEPDEF_STR_FROMCHARCODE ),
    JS_CFUNC_STEP_DEF("fromCodePoint", 1, STEPDEF_STR_FROMCODEPOINT ),
    JS_CFUNC_STEP_DEF("raw", 1, STEPDEF_STR_RAW ),
};

static const JSCFunctionListEntry js_string_proto_funcs[] = {
    JS_PROP_INT32_DEF("length", 0, JS_PROP_CONFIGURABLE ),
    JS_CFUNC_STEP_DEF("at", 1, STEPDEF_STR_AT ),
    JS_CFUNC_STEP_DEF("charCodeAt", 1, STEPDEF_STR_CHARCODEAT ),
    JS_CFUNC_STEP_DEF("charAt", 1, STEPDEF_STR_CHARAT ),
    JS_CFUNC_STEP_DEF("concat", 1, STEPDEF_STR_CONCAT ),
    JS_CFUNC_STEP_DEF("codePointAt", 1, STEPDEF_STR_CODEPOINTAT ),
    JS_CFUNC_STEP_DEF("isWellFormed", 0, STEPDEF_STR_ISWELLFORMED ),
    JS_CFUNC_STEP_DEF("toWellFormed", 0, STEPDEF_STR_TOWELLFORMED ),
    JS_CFUNC_STEP_DEF("indexOf", 1, STEPDEF_STR_INDEXOF ),
    JS_CFUNC_STEP_DEF("lastIndexOf", 1, STEPDEF_STR_LASTINDEXOF ),
    JS_CFUNC_STEP_DEF("includes", 1, STEPDEF_STR_INCLUDES ),
    JS_CFUNC_STEP_DEF("endsWith", 1, STEPDEF_STR_ENDSWITH ),
    JS_CFUNC_STEP_DEF("startsWith", 1, STEPDEF_STR_STARTSWITH ),
    JS_CFUNC_STEP_DEF("match", 1, STEPDEF_STR_MATCH ),
    JS_CFUNC_STEP_DEF("matchAll", 1, STEPDEF_STR_MATCHALL ),
    JS_CFUNC_STEP_DEF("search", 1, STEPDEF_STR_SEARCH ),
    JS_CFUNC_STEP_DEF("split", 2, STEPDEF_STR_SPLIT ),
    JS_CFUNC_STEP_DEF("substring", 2, STEPDEF_STR_SUBSTRING ),
    JS_CFUNC_STEP_DEF("substr", 2, STEPDEF_STR_SUBSTR ),
    JS_CFUNC_STEP_DEF("slice", 2, STEPDEF_STR_SLICE ),
    JS_CFUNC_STEP_DEF("repeat", 1, STEPDEF_STR_REPEAT ),
    JS_CFUNC_STEP_DEF("replace", 2, STEPDEF_STR_REPLACE ),
    JS_CFUNC_STEP_DEF("replaceAll", 2, STEPDEF_STR_REPLACE_ALL ),
    JS_CFUNC_STEP_DEF("padEnd", 1, STEPDEF_STR_PADEND ),
    JS_CFUNC_STEP_DEF("padStart", 1, STEPDEF_STR_PADSTART ),
    JS_CFUNC_STEP_DEF("trim", 0, STEPDEF_STR_TRIM ),
    JS_CFUNC_STEP_DEF("trimEnd", 0, STEPDEF_STR_TRIMEND ),
    JS_ALIAS_DEF("trimRight", "trimEnd" ),
    JS_CFUNC_STEP_DEF("trimStart", 0, STEPDEF_STR_TRIMSTART ),
    JS_ALIAS_DEF("trimLeft", "trimStart" ),
    JS_CFUNC_STEP_DEF("toString", 0, STEPDEF_STRING_TOSTRING ),
    JS_CFUNC_STEP_DEF("valueOf", 0, STEPDEF_STRING_VALUEOF ),
    JS_CFUNC_STEP_DEF("localeCompare", 1, STEPDEF_STR_LOCALECOMPARE ),
    JS_CFUNC_STEP_DEF("normalize", 0, STEPDEF_STR_NORMALIZE ),
    JS_CFUNC_STEP_DEF("toLowerCase", 0, STEPDEF_STR_TOLOWER ),
    JS_CFUNC_STEP_DEF("toUpperCase", 0, STEPDEF_STR_TOUPPER ),
    /* `length` stays 0: ECMA-402 §20.1.2 is "String.prototype.toLocaleLowerCase ( [ locales ] )" and a
       bracketed parameter is optional, so the count of non-optional arguments is unchanged by this split. */
    JS_CFUNC_STEP_DEF("toLocaleLowerCase", 0, STEPDEF_STR_TOLOCALELOWER ),
    JS_CFUNC_STEP_DEF("toLocaleUpperCase", 0, STEPDEF_STR_TOLOCALEUPPER ),
    JS_CFUNC_STEP_DEF("[Symbol.iterator]", 0, STEPDEF_STR_ITERATOR ),
    /* ES6 Annex B 2.3.2 etc. */
    JS_CFUNC_STEP_DEF("anchor", 1, STEPDEF_STR_HTML_BASE + magic_string_anchor ),
    JS_CFUNC_STEP_DEF("big", 0, STEPDEF_STR_HTML_BASE + magic_string_big ),
    JS_CFUNC_STEP_DEF("blink", 0, STEPDEF_STR_HTML_BASE + magic_string_blink ),
    JS_CFUNC_STEP_DEF("bold", 0, STEPDEF_STR_HTML_BASE + magic_string_bold ),
    JS_CFUNC_STEP_DEF("fixed", 0, STEPDEF_STR_HTML_BASE + magic_string_fixed ),
    JS_CFUNC_STEP_DEF("fontcolor", 1, STEPDEF_STR_HTML_BASE + magic_string_fontcolor ),
    JS_CFUNC_STEP_DEF("fontsize", 1, STEPDEF_STR_HTML_BASE + magic_string_fontsize ),
    JS_CFUNC_STEP_DEF("italics", 0, STEPDEF_STR_HTML_BASE + magic_string_italics ),
    JS_CFUNC_STEP_DEF("link", 1, STEPDEF_STR_HTML_BASE + magic_string_link ),
    JS_CFUNC_STEP_DEF("small", 0, STEPDEF_STR_HTML_BASE + magic_string_small ),
    JS_CFUNC_STEP_DEF("strike", 0, STEPDEF_STR_HTML_BASE + magic_string_strike ),
    JS_CFUNC_STEP_DEF("sub", 0, STEPDEF_STR_HTML_BASE + magic_string_sub ),
    JS_CFUNC_STEP_DEF("sup", 0, STEPDEF_STR_HTML_BASE + magic_string_sup ),
};

static const JSCFunctionListEntry js_string_iterator_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_string_iterator_next, 0 ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "String Iterator", JS_PROP_CONFIGURABLE ),
};


