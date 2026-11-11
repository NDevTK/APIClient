/* Object class */

JSValue JS_ToObject(JSContext *ctx, JSValueConst val)
{
    int tag = JS_VALUE_GET_NORM_TAG(val);
    JSValue obj;

    switch(tag) {
    default:
    case JS_TAG_NULL:
    case JS_TAG_UNDEFINED:
        return JS_ThrowTypeError(ctx, "Cannot convert undefined or null to object");
    case JS_TAG_OBJECT:
    case JS_TAG_EXCEPTION:
        return js_dup(val);
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        obj = JS_NewObjectClass(ctx, JS_CLASS_BIG_INT);
        goto set_value;
    case JS_TAG_INT:
    case JS_TAG_FLOAT64:
        obj = JS_NewObjectClass(ctx, JS_CLASS_NUMBER);
        goto set_value;
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
        /* XXX: should call the string constructor */
        {
            JSValue str;
            str = JS_ToString(ctx, val); /* ensure that we never store a rope */
            if (JS_IsException(str))
                return JS_EXCEPTION;
            obj = JS_NewObjectClass(ctx, JS_CLASS_STRING);
            if (!JS_IsException(obj)) {
                JS_DefinePropertyValue(ctx, obj, JS_ATOM_length,
                                       JS_NewInt32(ctx, JS_VALUE_GET_STRING(str)->len), 0);
                JS_SetObjectData(ctx, obj, js_dup(str));
            }
            JS_FreeValue(ctx, str);
            return obj;
        }
        goto set_value;
    case JS_TAG_BOOL:
        obj = JS_NewObjectClass(ctx, JS_CLASS_BOOLEAN);
        goto set_value;
    case JS_TAG_SYMBOL:
        obj = JS_NewObjectClass(ctx, JS_CLASS_SYMBOL);
    set_value:
        if (!JS_IsException(obj))
            JS_SetObjectData(ctx, obj, js_dup(val));
        return obj;
    }
}

static JSValue JS_ToObjectFree(JSContext *ctx, JSValue val)
{
    if (JS_VALUE_GET_TAG(val) == JS_TAG_OBJECT)
        return val;
    JSValue obj = JS_ToObject(ctx, val);
    JS_FreeValue(ctx, val);
    return obj;
}

/* 6.2.6.5 ToPropertyDescriptor as a RESUMABLE cursor. Its six fields are read in spec order, each as a
   HasProperty and then — only if present — a Get: TWELVE keyed operations, every one of which can be a Proxy
   trap or an accessor. js_obj_to_desc performs them with JS_HasProperty / JS_GetProperty from C, so a looping
   `has` or `get` on the descriptor object ran with no flow base and aborted the back-edge preempt
   (`Object.defineProperty(o, "a", new Proxy(d, {get(){ for(;;){} }}))`).
   Driven from here they are ordinary requests. The cursor keeps everything the walk accumulates, because a
   request suspends the flow and no interpreter local survives that.
   Its consumers are Object.defineProperty, Reflect.defineProperty, Object.defineProperties and Object.create,
   which is all of them bar one: js_obj_to_desc survives only for the Proxy getOwnPropertyDescriptor
   TRAP-RESULT read, where the descriptor object was built by the trap and the read is the invariant check
   rather than a spec step of its own. JS_DefinePropertyDesc and JS_ObjectDefineProperties are gone with the
   last caller. */
static const struct { uint32_t atom; int has_flag, bool_flag; } js_desc_fields[6] = {
    { JS_ATOM_enumerable,   JS_PROP_HAS_ENUMERABLE,   JS_PROP_ENUMERABLE   },
    { JS_ATOM_configurable, JS_PROP_HAS_CONFIGURABLE, JS_PROP_CONFIGURABLE },
    { JS_ATOM_value,        JS_PROP_HAS_VALUE,        0                    },
    { JS_ATOM_writable,     JS_PROP_HAS_WRITABLE,     JS_PROP_WRITABLE     },
    { JS_ATOM_get,          JS_PROP_HAS_GET,          0                    },
    { JS_ATOM_set,          JS_PROP_HAS_SET,          0                    },
};
/* 0 = the descriptor is complete, -1 = threw, else a REQUEST CODE (7 HasProperty / 6 Get) whose result comes
   back as `in` on the next call. */
static int js_desc_cursor_run(JSContext *ctx, JSDescCursor *c, JSValueConst desc, JSValue in,
                              JSValue **out_cb, int *out_argc)
{
    for (;;) {
        if (c->phase == DESC_PH_ASK) {
            JS_FreeValue(ctx, in);
            in = JS_UNDEFINED;
            if (JS_IsUndefined(c->cb[0])) {
                /* step 1, and it runs BEFORE the first HasProperty — after ToPropertyKey, which is why it is
                   here and not in the machine's prologue. */
                if (!JS_IsObject(desc)) {
                    JS_ThrowTypeError(ctx, "Property description must be an object");
                    return -1;
                }
                c->cb[0] = js_dup(desc);
            }
            if (c->field >= countof(js_desc_fields)) break;
            c->phase = DESC_PH_HAS;
            *out_cb = c->cb; *out_argc = (int)js_desc_fields[c->field].atom;
            return 7;   /* HASPROPERTY */
        }
        if (c->phase == DESC_PH_HAS) {
            bool present = JS_ToBool(ctx, in);
            JS_FreeValue(ctx, in);
            in = JS_UNDEFINED;
            if (!present) { c->field++; c->phase = DESC_PH_ASK; continue; }
            c->flags |= js_desc_fields[c->field].has_flag;
            c->phase = DESC_PH_GET;
            *out_cb = c->cb; *out_argc = (int)js_desc_fields[c->field].atom;
            return 6;   /* GETPROP */
        }
        /* DESC_PH_GET: `in` is the field's value, owned. */
        switch (c->field) {
        case 2: c->value = in; break;
        case 4:
        case 5: {
            /* steps 7.b / 8.b: an accessor field must be callable or undefined, checked as it is read — before
               the NEXT field is even asked for, which a page observes through its `has` trap. */
            bool ok = JS_IsUndefined(in) || JS_IsFunction(ctx, in);
            if (c->field == 4) c->getter = in; else c->setter = in;
            if (!ok) {
                JS_ThrowTypeError(ctx, c->field == 4 ? "Getter must be a function"
                                                     : "Setter must be a function");
                return -1;
            }
            break;
        }
        default:
            if (JS_ToBoolFree(ctx, in)) c->flags |= js_desc_fields[c->field].bool_flag;
            break;
        }
        in = JS_UNDEFINED;
        c->field++; c->phase = DESC_PH_ASK;
    }
    /* step 9: an accessor descriptor cannot also carry value or writable. */
    if ((c->flags & (JS_PROP_HAS_SET | JS_PROP_HAS_GET)) &&
        (c->flags & (JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE))) {
        JS_ThrowTypeError(ctx, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
        return -1;
    }
    return 0;
}

/* DELETED: js_obj_to_desc. It was 6.2.6.5 ToPropertyDescriptor performed from C — six HasProperty/Get PAIRS on a
   descriptor OBJECT, i.e. twelve of the page's operations, since any of those fields can be an accessor or a Proxy
   trap. Every routed descriptor walk is JSDescCursor; this survived only for the Proxy getOwnPropertyDescriptor
   trap-result read, and that hook's body is now gone. The ToPropertyDescriptor C-read ratchet reaches 0 with it. */

/* 20.1.1.1 Object(value), everything AFTER step 1. Step 1 is OrdinaryCreateFromConstructor and it is the
   MACHINE's — a subclass's NewTarget makes it read `prototype`, which is the page's [[Get]] and can be a
   getter or a Proxy trap; js_object_constructor performed it with js_create_from_ctor from its C entry, so
   `Reflect.construct(Object, [], new Proxy(f, {get(){…}}))` aborted at js_proxy_get's DFAIL.
   The object arriving here therefore means "step 1 ran and this is its answer", which step 1 RETURNS. Steps 2
   and 3 are the other case, and neither invokes anything — which is exactly what the declaration asserts. */
static JSValue js_object_ctor_body(JSContext *ctx, JSValueConst obj_, int argc, JSValueConst *argv)
{
    JSValue obj = unsafe_unconst(obj_);
    int tag;
    if (!JS_IsUndefined(obj))
        return obj;                        /* step 1's object, already ours */
    tag = JS_VALUE_GET_NORM_TAG(argv[0]);
    if (tag == JS_TAG_NULL || tag == JS_TAG_UNDEFINED)
        return JS_NewObject(ctx);          /* step 2 */
    return JS_ToObject(ctx, argv[0]);      /* step 3 */
}

/* DELETED: js_object_getPrototypeOf's C body. Its whole content was one JS_GetPrototype, which on a Proxy runs
   the `getPrototypeOf` trap through JS_CallFree — the page's code with no flow base. It is a step machine now
   (js_getproto_step), issuing that [[GetPrototypeOf]] as a request. */

static JSValue js_object_setPrototypeOf(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JSValueConst obj = argv[0];
    if (JS_SetPrototypeInternal(ctx, obj, argv[1], true) < 0)
        return JS_EXCEPTION;
    return js_dup(obj);
}

/* magic = 1 if called as Reflect.defineProperty */
/* hdr.arg: 0 = parseFloat, 1 = parseInt (which additionally coerces a radix). */
/* TWO algorithms, one machine. parseFloat has no radix, so its middle stage coerces nothing and says so rather
   than naming a step it does not have. ONE stage list expanded once per algorithm; see AFIND_STAGES. */
#define PARSENUM_STAGES(X, STR, RADIX, PARSE) \
    X(PARSENUM_STR,   STR)   \
    X(PARSENUM_RADIX, RADIX) \
    X(PARSENUM_PARSE, PARSE)
enum { PARSENUM_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0) };
static const char *const js_parseInt_steps[] = {
    PARSENUM_STAGES(JS_STEP_STAGE_LABEL,
        "19.2.5 step 1 (inputString is ToString(string))",
        "19.2.5 step 5 (R is ToInt32(radix))",
        "19.2.5 steps 6-16 (the trimmed string is parsed in radix R)")
    NULL };
static const char *const js_parseFloat_steps[] = {
    PARSENUM_STAGES(JS_STEP_STAGE_LABEL,
        "19.2.4 step 1 (inputString is ToString(string))",
        "19.2.4 takes no radix: nothing is coerced here",
        "19.2.4 steps 2-5 (the trimmed string is parsed as a StrDecimalLiteral)")
    NULL };

static int js_parse_num_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSParseNum *s = st;
    const char *cs, *q;
    int radix, flags, r;

    if (s->hdr.stage == PARSENUM_STR) {
        if (s->hdr.str_phase == STR_PH_START) { s->str = JS_UNDEFINED; s->result = JS_UNDEFINED; }
        /* 19.2.5/19.2.4 OVER UNKNOWN INPUT. §solver states it directly: opacity SURVIVES numeric
           coercion, so parseInt of an unknown is an unknown NUMBER and a branch on it still forks —
           never NaN, which is a concrete answer this engine has no basis for, and never the crash the
           ToString below would give. */
        {
            JSValue c = js_concolic_derive(ctx, step_arg(&s->hdr, 0), "parseInt", JS_UNDEFINED);
            if (!JS_IsUninitialized(c)) { JS_FreeValue(ctx, cb_result); s->result = c; return 0; }
        }
        r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = PARSENUM_RADIX;
    }
    if (s->hdr.stage == PARSENUM_RADIX) {
        if (s->hdr.arg) {
            r = step_toint32_run(ctx, &s->hdr, step_arg(&s->hdr, 1), cb_result, &s->radix, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
        }
        s->hdr.stage = PARSENUM_PARSE;
    }
    DCHECK(s->hdr.stage == PARSENUM_PARSE, "parseInt resumed in no stage");
    JS_FreeValue(ctx, cb_result);

    cs = JS_ToCString(ctx, s->str);   /* the value is already a string: no user code */
    if (!cs) return -1;
    q = cs + skip_spaces(cs);
    if (!s->hdr.arg) {
        s->result = js_atof(ctx, q, NULL, 10, 0);
    } else {
        radix = s->radix;
        if (radix != 0 && (radix < 2 || radix > 36)) {
            s->result = JS_NAN;
        } else {
            flags = ATOD_INT_ONLY | ATOD_ACCEPT_PREFIX_AFTER_SIGN;
            s->result = js_atof(ctx, q, NULL, radix, flags);
        }
    }
    JS_FreeCString(ctx, cs);
    return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_parse_num_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSParseNum *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->str);
}

static JSValue js_parse_num_fini(JSContext *ctx, void *st, bool take_result)
{
    JSParseNum *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* Object.defineProperty and Reflect.defineProperty are one machine over two algorithms whose four steps are the
   same operations; they differ only in whether step 4 is DefinePropertyOrThrow or the bare [[DefineOwnProperty]]
   whose boolean IS the answer. ONE stage list expanded once per algorithm — see AFIND_STAGES. */
#define DEFPROP_STAGES(X, KEY, DESC, DEFINE) \
    X(DEFPROP_KEY,    KEY)  \
    X(DEFPROP_DESC,   DESC) \
    X(DEFPROP_DEFINE, DEFINE)
enum { DEFPROP_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0) };
static const char *const js_obj_defprop_steps[] = {
    DEFPROP_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.2.4 steps 1-2 (O is an Object; key is ToPropertyKey(P))",
        "20.1.2.4 step 3 (desc is ToPropertyDescriptor(Attributes))",
        "20.1.2.4 step 4 (DefinePropertyOrThrow(O, key, desc))")
    NULL };
static const char *const js_reflect_defprop_steps[] = {
    DEFPROP_STAGES(JS_STEP_STAGE_LABEL,
        "28.1.3 steps 1-2 (target is an Object; key is ToPropertyKey(propertyKey))",
        "28.1.3 step 3 (desc is ToPropertyDescriptor(attributes))",
        "28.1.3 step 4 (target.[[DefineOwnProperty]](key, desc))")
    NULL };

static int js_obj_defprop_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSObjDefProp *s = st;
    JSValueConst obj = step_arg(&s->hdr, 0);
    int ret, flags, r;

    if (s->hdr.stage == DEFPROP_KEY) {
        if (s->hdr.str_phase == STR_PH_START) {
            s->result = JS_UNDEFINED;
            s->atom = JS_ATOM_NULL;
            s->cb[0] = JS_UNDEFINED; s->cb[1] = JS_UNDEFINED;
            js_desc_cursor_init(&s->cur);
            if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) {
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeErrorNotAnObject(ctx);
                return -1;
            }
        }
        r = step_topropkey_run(ctx, &s->hdr, step_arg(&s->hdr, 1), cb_result, &s->atom, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = DEFPROP_DESC;
    }
    if (s->hdr.stage == DEFPROP_DESC) {
        /* ToPropertyDescriptor, one keyed operation at a time. It used to be a JS_DefinePropertyDesc call from
           here, which read all twelve of them from C. */
        r = js_desc_cursor_run(ctx, &s->cur, step_arg(&s->hdr, 2), cb_result, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        /* the DEFINE itself, issued from the tail of the descriptor stage rather than from a stage of its own:
           a stage between the two would rest at no step (it can only ever be passed through), and a stage that
           rests nowhere is what naming them exists to remove. On a Proxy target this is the `defineProperty`
           trap and 10.5.6's invariant on its result — the page's code, which JS_DefineProperty ran from C. It
           is a request now, carrying the whole descriptor rather than CreateDataProperty's fixed shape, and its
           THROW flag says which of the two forms the caller wants: Object.defineProperty performs
           DefinePropertyOrThrow, Reflect.defineProperty the bare [[DefineOwnProperty]] whose boolean IS its
           answer. */
        s->hdr.stage = DEFPROP_DEFINE;
        s->cb[0] = js_dup(obj);
        s->cb[1] = js_dup(s->cur.value);
        s->hdr.desc_get = s->cur.getter;   /* borrowed: the cursor holds them across the request */
        s->hdr.desc_set = s->cur.setter;
        /* JS_PROP_DEFINE_PROPERTY says this is a real [[DefineOwnProperty]] and not CreateDataProperty's
           shorthand — it is what makes an ABSENT attribute default to false instead of true — and its REFLECT
           variant is the same operation reporting `false` where the other throws. It doubles as the "not the
           default shape" marker the request tests for, so an empty descriptor still routes. */
        s->hdr.desc_flags = s->cur.flags | (s->hdr.arg ? JS_PROP_REFLECT_DEFINE_PROPERTY
                                                       : (JS_PROP_THROW | JS_PROP_DEFINE_PROPERTY));
        *out_cb = s->cb; *out_argc = (int)s->atom;
        return 10;   /* DEFINE */
    }
    /* the define answered: undefined for the throwing form, the boolean for the bare one. */
    DCHECK(s->hdr.stage == DEFPROP_DEFINE, "defineProperty resumed in no stage");
    if (JS_IsException(cb_result)) return -1;
    ret = s->hdr.arg ? JS_ToBool(ctx, cb_result) : 0;
    JS_FreeValue(ctx, cb_result);
    s->result = s->hdr.arg ? js_bool(ret) : js_dup(obj);
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit), the descriptor cursor's collected values included — every one
   of ToPropertyDescriptor's twelve reads is the page's code and can suspend between two of them. */
static void js_obj_defprop_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSObjDefProp *s = st;
    v->val(ctx, &s->result);
    v->atom(ctx, &s->atom);
    js_desc_cursor_visit(ctx, &s->cur, v);
    v->val(ctx, &s->cb[0]);
    v->val(ctx, &s->cb[1]);
}

static JSValue js_obj_defprop_fini(JSContext *ctx, void *st, bool take_result)
{
    JSObjDefProp *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* 20.1.3.4 Object.prototype.propertyIsEnumerable: `? ToPropertyKey(V)` FIRST (the page's code), then
   `? ToObject(this)`, then `? O.[[GetOwnProperty]](key)` and whether it exists and is enumerable.
   js_object_propertyIsEnumerable ran the coercion and the descriptor read from C, and in the wrong ORDER besides.
   The RECORD answer shape is what this wants — the two attribute bits, not a descriptor object to read back off.
   It had a SECOND mode (HOE_ARGS, taking the object and an already-validated key as arguments) whose only caller
   was builtin-iterator-zip-keyed.js. That blob is gone and js_iterator_zip_keyed_step asks for the descriptor
   itself, so the mode went with it rather than staying as a shape nothing selects. */
typedef struct JSHasOwnEnum {
    JSStepHdr hdr;
    JSValue result;      /* DONE (owned) */
    JSValue obj;         /* the object asked (owned) — held across the request */
    JSAtom key;          /* the key being asked about (owned) */
    JSDescFacts facts;   /* the descriptor, as the bits the step tests (owned) */
} JSHasOwnEnum;
/* Steps 4-5 read the descriptor the request delivered as FACTS, which runs none of the page's code, so the
   machine ends in the stage that rests at step 3. */
#define PROPISENUM_STAGES(X) \
    X(PROPISENUM_KEY,  "20.1.3.4 steps 1-2 (P is ToPropertyKey(V); O is ToObject(this value))") \
    X(PROPISENUM_DESC, "20.1.3.4 step 3 (desc is O.[[GetOwnProperty]](P))")
enum { PROPISENUM_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_prop_is_enum_steps[] = { PROPISENUM_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_has_own_enum_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSHasOwnEnum *s = st;
    int r;

    if (s->hdr.stage == PROPISENUM_KEY) {
        if (s->hdr.str_phase == STR_PH_START) {
            s->result = JS_UNDEFINED;
            s->obj = JS_UNDEFINED;
            s->key = JS_ATOM_NULL;
            js_desc_facts_init(&s->facts);
        }
        r = step_topropkey_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->key, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->obj = JS_ToObject(ctx, s->hdr.this_val);              /* step 2, AFTER the coercion */
        if (JS_IsException(s->obj)) { s->obj = JS_UNDEFINED; return -1; }
        s->hdr.stage = PROPISENUM_DESC;
        s->hdr.desc_facts = &s->facts;
        s->hdr.cb_coerce[0] = s->obj;   /* borrowed: the state holds it across the request */
        *out_cb = s->hdr.cb_coerce; *out_argc = (int)s->key;
        return 12;
    }
    DCHECK(s->hdr.stage == PROPISENUM_DESC, "the own-enumerable probe resumed in no stage");
    if (JS_IsException(cb_result)) return -1;
    JS_FreeValue(ctx, cb_result);
    s->result = js_bool(s->facts.flags >= 0 && (s->facts.flags & JS_PROP_ENUMERABLE));
    return 0;
}
/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_has_own_enum_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSHasOwnEnum *s = st;
    js_desc_facts_visit(ctx, &s->facts, v);
    v->val(ctx, &s->result);
    v->val(ctx, &s->obj);
    v->atom(ctx, &s->key);
}

static JSValue js_has_own_enum_fini(JSContext *ctx, void *st, bool take_result)
{
    JSHasOwnEnum *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* 20.1.3.9.1 "Object.prototype.__defineGetter__ ( key, getter )" /
   20.1.3.9.2 "Object.prototype.__defineSetter__ ( key, setter )". These sites cited `B.2.2.2` and `B.2.2.3`,
   which in the edition the editors maintain are String.prototype.anchor ( name ) and String.prototype.big ( ) —
   Annex B numbers that still RESOLVE, so the retired citation read as authoritative and sent the reader to a
   clause about markup. The TITLE beside the number is what makes a move like that visible instead of silent,
   which is why every citation of these two carries one now. Step 5 is
   `? DefinePropertyOrThrow(O, key, desc)` — on a Proxy the `defineProperty` trap plus 10.5.6's invariant, the
   page's code — and step 4's ToPropertyKey is the page's code too. JS_DefineProperty ran the first from C, which
   is what `Object.prototype.__defineGetter__.call(new Proxy({}, {defineProperty(){for(;;){}}}), "x", f)` aborted
   on. It is the SAME GP_DEFINE request Object.defineProperty's machine issues, with the descriptor built here
   instead of read off an object; magic selects [[Get]] or [[Set]], which is the only difference between the two
   builtins and so is the only thing that branches. */
typedef struct JSObjDefAccessor {
    JSStepHdr hdr;
    JSValue result;       /* DONE (owned) */
    JSValue obj;          /* ToObject(this) (owned) — held across the key coercion and the define */
    JSAtom atom;          /* the coerced key (owned) */
    JSValue cb[2];        /* [target, value] for the DEFINE request (owned) */
} JSObjDefAccessor;
/* TWO algorithms, one machine, differing only in which accessor field step 3's descriptor carries. Steps 1-3 run
   none of the page's code, so they share the stage that rests at step 4's ToPropertyKey; step 6 is `return
   undefined`. ONE stage list expanded once per algorithm — see AFIND_STAGES. */
#define DEFACC_STAGES(X, KEY, DEFINE) \
    X(DEFACC_KEY,    KEY) \
    X(DEFACC_DEFINE, DEFINE)
enum { DEFACC_STAGES(JS_STEP_STAGE_ENUM, 0, 0) };
static const char *const js_obj_defgetter_steps[] = {
    DEFACC_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.3.9.1 steps 1-4 (O is ToObject(this value); IsCallable(getter); desc; key is ToPropertyKey(P))",
        "20.1.3.9.1 step 5 (DefinePropertyOrThrow(O, key, desc))")
    NULL };
static const char *const js_obj_defsetter_steps[] = {
    DEFACC_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.3.9.2 steps 1-4 (O is ToObject(this value); IsCallable(setter); desc; key is ToPropertyKey(P))",
        "20.1.3.9.2 step 5 (DefinePropertyOrThrow(O, key, desc))")
    NULL };

static int js_obj_defaccessor_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSObjDefAccessor *s = st;
    int r;

    if (s->hdr.stage == DEFACC_KEY) {
        if (s->hdr.str_phase == STR_PH_START) {
            s->result = JS_UNDEFINED;
            s->obj = JS_UNDEFINED;
            s->atom = JS_ATOM_NULL;
            s->cb[0] = JS_UNDEFINED; s->cb[1] = JS_UNDEFINED;
            /* step 1's ToObject and step 2's IsCallable both precede step 4's ToPropertyKey, and neither runs the
               page's code. Their ORDER is observable — a non-callable getter throws before the key is coerced. */
            s->obj = JS_ToObject(ctx, s->hdr.this_val);
            if (JS_IsException(s->obj)) { s->obj = JS_UNDEFINED; JS_FreeValue(ctx, cb_result); return -1; }
            if (check_function(ctx, step_arg(&s->hdr, 1))) { JS_FreeValue(ctx, cb_result); return -1; }
        }
        r = step_topropkey_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->atom, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        /* step 5, issued from the tail of the stage that finished step 4: a stage between them would rest at no
           step at all, which is exactly what naming them exists to remove. */
        s->hdr.stage = DEFACC_DEFINE;
        s->cb[0] = js_dup(s->obj);
        s->cb[1] = JS_UNDEFINED;                       /* an accessor descriptor has no [[Value]] */
        /* borrowed: the caller's argument outlives this */
        s->hdr.desc_get = s->hdr.arg ? JS_UNDEFINED : step_arg(&s->hdr, 1);
        s->hdr.desc_set = s->hdr.arg ? step_arg(&s->hdr, 1) : JS_UNDEFINED;
        s->hdr.desc_flags = JS_PROP_THROW | JS_PROP_DEFINE_PROPERTY
                          | JS_PROP_HAS_ENUMERABLE | JS_PROP_ENUMERABLE
                          | JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE
                          | (s->hdr.arg ? JS_PROP_HAS_SET : JS_PROP_HAS_GET);
        *out_cb = s->cb; *out_argc = (int)s->atom;
        return 10;   /* DEFINE */
    }
    /* step 6: undefined, whatever the define answered — the THROW flag has already turned a refusal into one. */
    DCHECK(s->hdr.stage == DEFACC_DEFINE, "__defineGetter__/__defineSetter__ resumed in no stage");
    if (JS_IsException(cb_result)) return -1;
    JS_FreeValue(ctx, cb_result);
    s->result = JS_UNDEFINED;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_obj_defaccessor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSObjDefAccessor *s = st;
    v->val(ctx, &s->result);
    v->atom(ctx, &s->atom);
    v->val(ctx, &s->obj);
    v->val(ctx, &s->cb[0]);
    v->val(ctx, &s->cb[1]);
}

static JSValue js_obj_defaccessor_fini(JSContext *ctx, void *st, bool take_result)
{
    JSObjDefAccessor *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* Object.getOwnPropertyDescriptor and Reflect.getOwnPropertyDescriptor are one operation with one difference:
   Reflect requires an Object where Object coerces one. Both of their user-code steps are requests — ToPropertyKey
   on the key, then [[GetOwnProperty]], which on a Proxy is the `getOwnPropertyDescriptor` trap — and the body
   that used to sit here performed the second with JS_GetOwnPropertyInternal from C, so a trap containing a loop
   had no flow base. def->arg: 1 = Reflect's receiver rule. */
/* Object/Reflect.getPrototypeOf. Its whole body is one [[GetPrototypeOf]], which on a Proxy is the
   `getPrototypeOf` trap — the page's code, run from C by JS_GetPrototype. As a machine it is a single request
   and a result. */
typedef struct JSGetProto {
    JSStepHdr hdr;
    JSValue result;   /* the prototype, or null (owned) */
} JSGetProto;

/* Object/Reflect.isExtensible and .preventExtensions. Each is one object-level internal method, which on a Proxy
   is its trap, run from C by JS_IsExtensible / JS_PreventExtensions. The definition's arg says WHICH of the four
   spellings this is: bit 0 = the Reflect form (a non-object is a TypeError rather than a shortcut), bit 1 = the
   preventExtensions operation. */
typedef struct JSExtOp {
    JSStepHdr hdr;
    JSValue result;   /* the boolean, or the object Object.preventExtensions returns (owned) */
} JSExtOp;
#define EXTOP_REFLECT 1
#define EXTOP_PREVENT 2

/* Object/Reflect.setPrototypeOf. One [[SetPrototypeOf]], which on a Proxy is its trap, run from C by
   JS_SetPrototypeInternal. The forms differ in what they REJECT and what they RETURN: Object requires only that
   its first argument be coercible and yields it; Reflect requires an Object and yields the boolean. */
typedef struct JSSetProto {
    JSStepHdr hdr;
    JSValue result;   /* the object (Object's form) or the boolean (Reflect's) (owned) */
} JSSetProto;

/* Object.setPrototypeOf and Reflect.setPrototypeOf are ONE machine and TWO algorithms: what the prologue rejects
   and what the epilogue answers differ, the [[SetPrototypeOf]] between them does not. ONE stage list expanded once
   per algorithm with that algorithm's own step text — see AFIND_STAGES. The prologue runs NONE of the page's code
   (RequireObjectCoercible and two type tests), which is what lets it be one stage rather than three. */
#define SETPROTO_STAGES(X, ENTRY, SET) \
    X(SETPROTO_ENTRY, ENTRY) \
    X(SETPROTO_SET,   SET)
enum { SETPROTO_STAGES(JS_STEP_STAGE_ENUM, 0, 0) };
static const char *const js_obj_setproto_steps[] = {
    SETPROTO_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.2.23 steps 1-3 (RequireObjectCoercible(O); proto is an Object or null; a non-object O is returned)",
        "20.1.2.23 step 4 (status is O.[[SetPrototypeOf]](proto))")
    NULL };
static const char *const js_reflect_setproto_steps[] = {
    SETPROTO_STAGES(JS_STEP_STAGE_LABEL,
        "28.1.13 steps 1-2 (target is an Object; proto is an Object or null)",
        "28.1.13 step 3 (status is target.[[SetPrototypeOf]](proto))")
    NULL };

static int js_setproto_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSSetProto *s = st;
    int reflect = s->hdr.def->arg;

    if (s->hdr.stage == SETPROTO_ENTRY) {
        JSValueConst obj = step_arg(&s->hdr, 0), proto = step_arg(&s->hdr, 1);
        JS_FreeValue(ctx, cb_result);
        s->result = JS_UNDEFINED;   /* FIRST, before anything that can throw */
        /* 28.1.13 STEP 1 IS THE TARGET, step 2 the prototype; 20.1.2.23 tests them the other way round because
           its step 1 is RequireObjectCoercible. Naming the stage after the steps is what showed the two were
           being run in one order for both — the same TypeError either way, so nothing could report it. */
        if (reflect && JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) {
            JS_ThrowTypeErrorNotAnObject(ctx);
            return -1;
        }
        if (JS_VALUE_GET_TAG(proto) != JS_TAG_NULL && JS_VALUE_GET_TAG(proto) != JS_TAG_OBJECT) {
            JS_ThrowTypeError(ctx, "not an object or null");
            return -1;
        }
        if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) {
            /* 20.1.2.23 steps 1 and 3: a nullish O is RequireObjectCoercible's TypeError, any other primitive is
               returned unchanged with no internal method performed at all. */
            if (JS_VALUE_GET_TAG(obj) == JS_TAG_NULL || JS_VALUE_GET_TAG(obj) == JS_TAG_UNDEFINED) {
                JS_ThrowTypeErrorNotAnObject(ctx);
                return -1;
            }
            s->result = js_dup(obj);
            return 0;
        }
        s->hdr.stage = SETPROTO_SET;
        s->hdr.cb_coerce[0] = obj;     /* borrowed: the caller's stack holds both across the request */
        s->hdr.cb_coerce[1] = proto;
        *out_cb = s->hdr.cb_coerce; *out_argc = 0;
        return 21;   /* SETPROTO */
    }
    if (JS_IsException(cb_result)) return -1;
    {
        int r = JS_ToBoolFree(ctx, cb_result);
        if (reflect) { s->result = js_bool(r); return 0; }
        /* Object.setPrototypeOf performs the THROWING form and yields its argument. */
        if (!r) { JS_ThrowTypeError(ctx, "proxy: bad prototype"); return -1; }
        s->result = js_dup(step_arg(&s->hdr, 0));
        return 0;
    }
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_setproto_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSSetProto *s = st;
    v->val(ctx, &s->result);
}

static JSValue js_setproto_fini(JSContext *ctx, void *st, bool take_result)
{
    JSSetProto *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* FOUR algorithms, one machine: Object and Reflect × isExtensible and preventExtensions. Each rests on its one
   object-level internal method, and each spells its entry differently — Object answers a primitive where Reflect
   rejects it. ONE stage list expanded once per algorithm — see AFIND_STAGES. Object.preventExtensions' steps 3-4
   (throw on false, return O) run none of the page's code, so they finish in the stage that rests at step 2. */
#define EXTOP_STAGES(X, ENTRY, OP) \
    X(EXTOP_ENTRY, ENTRY) \
    X(EXTOP_OP,    OP)
enum { EXTOP_STAGES(JS_STEP_STAGE_ENUM, 0, 0) };
static const char *const js_obj_isext_steps[] = {
    EXTOP_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.2.16 step 1 (a non-object O is false)",
        "20.1.2.16 step 2 (IsExtensible(O))")
    NULL };
static const char *const js_reflect_isext_steps[] = {
    EXTOP_STAGES(JS_STEP_STAGE_LABEL,
        "28.1.9 step 1 (target is an Object)",
        "28.1.9 step 2 (target.[[IsExtensible]]())")
    NULL };
static const char *const js_obj_prevext_steps[] = {
    EXTOP_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.2.20 step 1 (a non-object O is returned unchanged)",
        "20.1.2.20 step 2 (status is O.[[PreventExtensions]]())")
    NULL };
static const char *const js_reflect_prevext_steps[] = {
    EXTOP_STAGES(JS_STEP_STAGE_LABEL,
        "28.1.11 step 1 (target is an Object)",
        "28.1.11 step 2 (target.[[PreventExtensions]]())")
    NULL };

static int js_extop_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSExtOp *s = st;
    int arg = s->hdr.def->arg;

    if (s->hdr.stage == EXTOP_ENTRY) {
        JSValueConst obj = step_arg(&s->hdr, 0);
        JS_FreeValue(ctx, cb_result);
        s->result = JS_UNDEFINED;   /* FIRST, before anything that can throw */
        if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) {
            /* the Reflect forms reject a non-object; Object's answer it without any internal method at all —
               nothing is extensible-in-the-[[IsExtensible]]-sense but a primitive is already non-extensible. */
            if (arg & EXTOP_REFLECT) { JS_ThrowTypeErrorNotAnObject(ctx); return -1; }
            s->result = (arg & EXTOP_PREVENT) ? js_dup(obj) : JS_FALSE;
            return 0;
        }
        s->hdr.stage = EXTOP_OP;
        s->hdr.cb_coerce[0] = obj;   /* borrowed: the caller's stack holds it across the request */
        *out_cb = s->hdr.cb_coerce; *out_argc = 0;
        return (arg & EXTOP_PREVENT) ? 20 : 19;
    }
    if (JS_IsException(cb_result)) return -1;
    {
        int r = JS_ToBoolFree(ctx, cb_result);
        if (!(arg & EXTOP_PREVENT)) { s->result = js_bool(r); return 0; }
        if (arg & EXTOP_REFLECT) { s->result = js_bool(r); return 0; }
        /* Object.preventExtensions THROWS where Reflect's reports false, and yields its argument. */
        if (!r) { JS_ThrowTypeError(ctx, "proxy preventExtensions handler returned false"); return -1; }
        s->result = js_dup(step_arg(&s->hdr, 0));
        return 0;
    }
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_extop_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSExtOp *s = st;
    v->val(ctx, &s->result);
}

static JSValue js_extop_fini(JSContext *ctx, void *st, bool take_result)
{
    JSExtOp *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* Object.getPrototypeOf and Reflect.getPrototypeOf: one [[GetPrototypeOf]] each, differing only in what their
   first step does with a primitive. ONE stage list expanded once per algorithm — see AFIND_STAGES. */
#define GETPROTO_STAGES(X, RECV, GET) \
    X(GETPROTO_RECV, RECV) \
    X(GETPROTO_GET,  GET)
enum { GETPROTO_STAGES(JS_STEP_STAGE_ENUM, 0, 0) };
static const char *const js_obj_getproto_steps[] = {
    GETPROTO_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.2.12 step 1 (obj is ToObject(O))",
        "20.1.2.12 step 2 (obj.[[GetPrototypeOf]]())")
    NULL };
static const char *const js_reflect_getproto_steps[] = {
    GETPROTO_STAGES(JS_STEP_STAGE_LABEL,
        "28.1.7 step 1 (target is an Object)",
        "28.1.7 step 2 (target.[[GetPrototypeOf]]())")
    NULL };

static int js_getproto_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSGetProto *s = st;

    if (s->hdr.stage == GETPROTO_RECV) {
        JSValueConst val = step_arg(&s->hdr, 0);
        JS_FreeValue(ctx, cb_result);
        s->result = JS_UNDEFINED;   /* FIRST, before anything that can throw: the teardown frees what it holds */
        if (JS_VALUE_GET_TAG(val) != JS_TAG_OBJECT) {
            /* Object.getPrototypeOf boxes a primitive (the ES6 concession ES5.1 did not have); Reflect's form
               rejects it. Either way a nullish argument is a TypeError. */
            if (s->hdr.def->arg || JS_VALUE_GET_TAG(val) == JS_TAG_NULL
                || JS_VALUE_GET_TAG(val) == JS_TAG_UNDEFINED) {
                JS_ThrowTypeErrorNotAnObject(ctx);
                return -1;
            }
        }
        s->hdr.stage = GETPROTO_GET;
        s->hdr.cb_coerce[0] = val;   /* borrowed: the caller's stack holds it across the request */
        *out_cb = s->hdr.cb_coerce; *out_argc = 0;
        return 18;   /* GETPROTO */
    }
    if (JS_IsException(cb_result)) return -1;
    s->result = cb_result;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_getproto_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSGetProto *s = st;
    v->val(ctx, &s->result);
}

static JSValue js_getproto_fini(JSContext *ctx, void *st, bool take_result)
{
    JSGetProto *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

typedef struct JSGopd {
    JSStepHdr hdr;
    JSValue obj;      /* the receiver, held across both requests (owned) */
    JSValue result;   /* the descriptor object, or undefined when absent (owned) */
    JSAtom atom;      /* the coerced key (owned) */
} JSGopd;

/* Object.getOwnPropertyDescriptor and Reflect.getOwnPropertyDescriptor: the same three steps, differing only in
   whether step 1 boxes a primitive or rejects it. Step 4's FromPropertyDescriptor runs none of the page's code —
   the engine builds the object — so the machine ends in the stage that rests at step 3. ONE stage list expanded
   once per algorithm — see AFIND_STAGES. */
#define GOPD_STAGES(X, RECV, KEY, GET) \
    X(GOPD_RECV, RECV) \
    X(GOPD_KEY,  KEY)  \
    X(GOPD_GET,  GET)
enum { GOPD_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0) };
static const char *const js_obj_gopd_steps[] = {
    GOPD_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.2.8 step 1 (obj is ToObject(O))",
        "20.1.2.8 step 2 (key is ToPropertyKey(P))",
        "20.1.2.8 step 3 (desc is obj.[[GetOwnProperty]](key))")
    NULL };
static const char *const js_reflect_gopd_steps[] = {
    GOPD_STAGES(JS_STEP_STAGE_LABEL,
        "28.1.6 step 1 (target is an Object)",
        "28.1.6 step 2 (key is ToPropertyKey(propertyKey))",
        "28.1.6 step 3 (desc is target.[[GetOwnProperty]](key))")
    NULL };

static int js_gopd_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSGopd *s = st;
    int r;

    if (s->hdr.stage == GOPD_RECV) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->obj = JS_UNDEFINED; s->result = JS_UNDEFINED; s->atom = JS_ATOM_NULL;
        if (s->hdr.arg) {
            if (JS_VALUE_GET_TAG(step_arg(&s->hdr, 0)) != JS_TAG_OBJECT) {
                JS_ThrowTypeErrorNotAnObject(ctx); return -1;
            }
            s->obj = js_dup(step_arg(&s->hdr, 0));
        } else {
            s->obj = JS_ToObject(ctx, step_arg(&s->hdr, 0));   /* a primitive wrapper; runs no user code */
            if (JS_IsException(s->obj)) { s->obj = JS_UNDEFINED; return -1; }
        }
        s->hdr.stage = GOPD_KEY;
    }
    if (s->hdr.stage == GOPD_KEY) {
        r = step_topropkey_run(ctx, &s->hdr, step_arg(&s->hdr, 1), cb_result, &s->atom, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = GOPD_GET;
        s->hdr.cb_coerce[0] = s->obj;   /* borrowed: the machine holds it across the request */
        *out_cb = s->hdr.cb_coerce; *out_argc = (int)s->atom;
        return 12;
    }
    DCHECK(s->hdr.stage == GOPD_GET, "getOwnPropertyDescriptor: unknown stage");
    s->result = cb_result;
    return JS_IsException(s->result) ? -1 : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_gopd_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSGopd *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->obj);
    v->atom(ctx, &s->atom);
}

static JSValue js_gopd_fini(JSContext *ctx, void *st, bool take_result)
{
    JSGopd *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}


/* The KEY list of an object's own properties, as an Array: Object.keys / getOwnPropertyNames /
   getOwnPropertySymbols / Reflect.ownKeys. It used to take a `kind` and additionally READ each value for
   Object.values / Object.entries — reads that are the page's code and ran from here, off the tramp. Those two are
   the JSPropWalk step machine now, and every remaining caller wants keys, so the parameter and both value branches
   are gone rather than left as an unreachable second algorithm. */
static JSValue JS_GetOwnPropertyNames2(JSContext *ctx, JSValueConst obj1, int flags)
{
    JSValue obj, r, val;
    JSObject *p;
    JSPropertyEnum *atoms;
    uint32_t len, i, j;

    r = JS_UNDEFINED;
    val = JS_UNDEFINED;
    obj = JS_ToObject(ctx, obj1);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    p = JS_VALUE_GET_OBJ(obj);
    if (JS_GetOwnPropertyNamesInternal(ctx, &atoms, &len, p, flags & ~JS_GPN_ENUM_ONLY))
        goto exception;
    r = JS_NewArray(ctx);
    if (JS_IsException(r))
        goto exception;
    for(j = i = 0; i < len; i++) {
        JSAtom atom = atoms[i].atom;
        if (flags & JS_GPN_ENUM_ONLY) {
            int desc_flags, res;

            /* Check if property is still enumerable */
            res = JS_GetOwnPropertyFlagsInternal(ctx, &desc_flags, p, atom);
            if (res < 0)
                goto exception;
            if (!res)
                continue;
            if (!(desc_flags & JS_PROP_ENUMERABLE))
                continue;
        }
        val = JS_AtomToValue(ctx, atom);
        if (JS_IsException(val))
            goto exception;
        if (JS_CreateDataPropertyUint32(ctx, r, j++, val, 0) < 0)
            goto exception;
    }
    goto done;

exception:
    JS_FreeValue(ctx, r);
    r = JS_EXCEPTION;
done:
    js_free_prop_enum(ctx, atoms, len);
    JS_FreeValue(ctx, obj);
    return r;
}

/* Object.values / Object.entries — 7.3.23 EnumerableOwnProperties as a STEP MACHINE (see JSPropWalk). One read is
   requested per key through step_getprop_run, which is the shared entry that routes an accessor body and a Proxy
   `get` trap onto the tramp chain (and does a data slot inline), so a getter with a loop in it parks like any other
   call instead of driving to completion inside JS_GetOwnPropertyNames2. */
/* ONE list expanded once PER ALGORITHM, so a renumber carries its label with it (JSTrampStepDef.steps).
   EIGHT definitions over FIVE algorithms: values, keys and entries are 7.3.23 with three kinds and therefore
   ONE list; defineProperties and create are both 20.1.2.3.1; getOwnPropertyDescriptors, assign and the object
   spread each have their own. A definition whose stage cannot occur in its algorithm - the two DEFINE stages
   exist only for 20.1.2.3.1's second loop - has an array that ENDS before them, so resting there is a stage past
   the end of ITS algorithm and step_stage_check names it. */
#define PWALK_STAGES(X, ENTRY, SOURCE, KEYS, KEYHEAD, DESC, GET, PLACE, WRITE) \
    X(PW_ENTRY,   ENTRY)   \
    X(PW_SOURCE,  SOURCE)  \
    X(PW_KEYS,    KEYS)    \
    X(PW_KEYHEAD, KEYHEAD) \
    X(PW_DESC,    DESC)    \
    X(PW_GET,     GET)     \
    X(PW_PLACE,   PLACE)   \
    X(PW_WRITE,   WRITE)   \
    X(PW_SRCDONE, "the source is exhausted: its key snapshot and its coerced object are released before the " \
                  "next source replaces them. No step of any of these algorithms - a snapshot is this engine's, " \
                  "and nothing here can suspend")
#define PWALK_DEFINE_EXTRA(X, DEFINE, DEFINED) \
    X(PW_DEFINE,  DEFINE) \
    X(PW_DEFINED, DEFINED)
enum { PWALK_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0, 0, 0, 0, 0, 0)
       PWALK_DEFINE_EXTRA(JS_STEP_STAGE_ENUM, 0, 0) };
static const char *const js_enum_own_props_steps[] = {
    PWALK_STAGES(JS_STEP_STAGE_LABEL,
        "7.3.23, entered: results is a new empty List (step 2)",
        "7.3.23 step 1's receiver (O is ToObject of the argument) - and the dispatch of O.[[OwnPropertyKeys]]()",
        "7.3.23 step 1 (ownKeys arrives and is snapshotted)",
        "7.3.23 steps 3 and 3.a (Repeat over ownKeys: a Symbol key is skipped) - and the dispatch of step "
        "3.a.i's O.[[GetOwnProperty]](key)",
        "7.3.23 steps 3.a.i-3.a.ii.1 (desc arrives; a non-enumerable or absent key is skipped; kind key appends "
        "the key)",
        "7.3.23 step 3.a.ii.2.a (value is Get(O, key))",
        "7.3.23 steps 3.a.ii.2.b-3.a.ii.2.c (the value, or CreateArrayFromList of key and value, is appended)",
        "7.3.23: unreached - this walk performs no Set")
    NULL };
static const char *const js_obj_descs_steps[] = {
    PWALK_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.2.9 step 3 (descriptors is OrdinaryObjectCreate(%Object.prototype%))",
        "20.1.2.9 step 1 (obj is ToObject(O)) - and the dispatch of step 2's obj.[[OwnPropertyKeys]]()",
        "20.1.2.9 step 2 (ownKeys arrives and is snapshotted)",
        "20.1.2.9 step 4 (Repeat over ownKeys) - and the dispatch of step 4.a's obj.[[GetOwnProperty]](key)",
        "20.1.2.9 steps 4.a-4.c (desc arrives; descriptor is FromPropertyDescriptor(desc); "
        "CreateDataPropertyOrThrow(descriptors, key, descriptor))",
        "20.1.2.9: unreached - a descriptor walk performs no Get",
        "20.1.2.9: unreached - the descriptor is placed in the stage that reads it",
        "20.1.2.9: unreached - this walk performs no Set")
    NULL };
static const char *const js_obj_assign_steps[] = {
    PWALK_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.2.1 steps 1-2 (to is ToObject(target); one argument returns it)",
        "20.1.2.1 steps 3-3.a.i (the next nextSource; a nullish one is skipped; from is ToObject(nextSource)) - "
        "and the dispatch of step 3.a.ii's from.[[OwnPropertyKeys]]()",
        "20.1.2.1 step 3.a.ii (keys arrives and is snapshotted)",
        "20.1.2.1 step 3.a.iii (Repeat over keys) - and the dispatch of step 3.a.iii.1's "
        "from.[[GetOwnProperty]](nextKey)",
        "20.1.2.1 steps 3.a.iii.1-3.a.iii.2 (desc arrives; a non-enumerable or absent key is skipped)",
        "20.1.2.1 step 3.a.iii.2.a (propValue is Get(from, nextKey))",
        "20.1.2.1 step 3.a.iii.2.b, prepared (propValue is parked for the write that follows)",
        "20.1.2.1 step 3.a.iii.2.b (Set(to, nextKey, propValue, true))")
    NULL };
static const char *const js_obj_spread_steps[] = {
    PWALK_STAGES(JS_STEP_STAGE_LABEL,
        "7.3.25, entered: target is the object the opcode created and is only borrowed here",
        "7.3.25 steps 1-2 (a nullish source contributes nothing; from is ToObject(source)) - and the dispatch "
        "of step 3's from.[[OwnPropertyKeys]]()",
        "7.3.25 step 3 (keys arrives and is snapshotted)",
        "7.3.25 steps 4-4.c (Repeat over keys; excludedItems decides `excluded`) - and the dispatch of step "
        "4.c.i's from.[[GetOwnProperty]](nextKey)",
        "7.3.25 steps 4.c.i-4.c.ii (desc arrives; a non-enumerable or absent key is skipped)",
        "7.3.25 step 4.c.ii.1 (propValue is Get(from, nextKey))",
        "7.3.25 step 4.c.ii.2 (CreateDataPropertyOrThrow(target, nextKey, propValue))",
        "7.3.25: unreached - a spread's target is fresh, so it performs no Set")
    NULL };
static const char *const js_obj_defprops_steps[] = {
    PWALK_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.2.3.1, entered: O is validated as an Object before Properties is coerced, and descriptors is a "
        "new empty List (step 3)",
        "20.1.2.3.1 step 1 (props is ToObject(Properties)) - and the dispatch of step 2's "
        "props.[[OwnPropertyKeys]]()",
        "20.1.2.3.1 step 2 (keys arrives and is snapshotted)",
        "20.1.2.3.1 step 4 (Repeat over keys) - and the dispatch of step 4.a's "
        "props.[[GetOwnProperty]](nextKey)",
        "20.1.2.3.1 steps 4.a-4.b (propDesc arrives; a non-enumerable or absent key is skipped)",
        "20.1.2.3.1 step 4.b.i (descObj is Get(props, nextKey))",
        "20.1.2.3.1 steps 4.b.ii-4.b.iii (desc is ToPropertyDescriptor(descObj), itself a sequence of keyed "
        "reads; the Record is appended)",
        "20.1.2.3.1: unreached - this walk performs no Set")
    PWALK_DEFINE_EXTRA(JS_STEP_STAGE_LABEL,
        "20.1.2.3.1 step 5 (the SECOND loop) - and the dispatch of step 5.a's DefinePropertyOrThrow(O, "
        "property.[[Key]], property.[[Descriptor]])",
        "20.1.2.3.1 step 5.a (the define's answer arrives)")
    NULL };

static int js_prop_walk_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSPropWalk *s = st;
    int mode = s->hdr.arg;
    int assign = (mode == PROPWALK_ASSIGN);
    /* both descriptor sinks: the walk is identical, only the target differs. */
    int defprops = (mode == PROPWALK_DEFPROPS || mode == PROPWALK_OBJCREATE);
    /* @ATOMOWNER WHERE THE SOURCES START, AS ONE FACT RATHER THAN TWO EXPRESSIONS. A sink whose argv[0] is the
       TARGET (or, for Object.create, the PROTOTYPE) has its sources at 1; values/entries/keys/descs take
       argv[0] itself. Both ends of the source range read this ONE predicate, because they were two independent
       spellings of it and they DISAGREED: src_end already counted a SPREAD's range as ending at 2 — the same
       bound defprops uses for its single source at index 1 — while the start condition omitted SPREAD and left
       it at 0, so `{...src}` opened [0,2) and walked its own target as a source before the real one.
       THREE STATEMENTS IN THIS FILE SAID IT HAS ONE SOURCE and the code gave it two: PROPWALK_SPREAD's own
       define ("argv is [target, source, excludeList]"), src_i's field comment ("the others have one source"),
       and src_end's own comment ("a spread takes the one the opcode handed it"). OP_copy_data_properties
       settles which argument is which — its handler is commented `target source excludeList` and fills
       pw_args[0] from the target slot.
       IT IS OBSERVABLE AND NOT MERELY WASTED WORK: the self-pass [[Get]]s every own enumerable key of the
       target and DEFINES it back as a data property, so `{get a(){…}, ...src}` CALLED the literal's getter and
       replaced the accessor with a data property of its result.
       ECMAScript §13.2.5.6 "Runtime Semantics: PropertyDefinitionEvaluation" runs the spread arm as
       CopyDataProperties(obj, fromValue, excludedNames), which reads `fromValue` and never obj — the TITLE IS
       QUOTED so the number and the title are one claim a checker can contradict, which an unquoted title
       beside a number is not. It also captured every one of those keys into the running flow's COW
       delta, which is a per-spread cost in the size of the TARGET that nothing asked for. */
    int target_is_arg0 = (assign || defprops || mode == PROPWALK_SPREAD);
    int r;

    if (s->hdr.stage == PW_ENTRY) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds, so a field
           handed over late is a leak and a buffer initialised late is freed with a NULL context. */
        s->obj = JS_UNDEFINED; s->result = JS_UNDEFINED; s->el = JS_UNDEFINED;
        s->atoms = NULL; s->len = 0; s->i = 0; s->j = 0;
        s->dl = NULL; s->dk = NULL; s->nd = 0; s->di = 0;
        s->cb[0] = JS_UNDEFINED; s->cb[1] = JS_UNDEFINED;
        js_desc_cursor_init(&s->dcur);
        s->src_i = target_is_arg0 ? 1 : 0;
        if (mode == PROPWALK_DEFPROPS && JS_VALUE_GET_TAG(step_arg(&s->hdr, 0)) != JS_TAG_OBJECT) {
            /* 20.1.2.3 step 1, before Properties is even coerced. */
            JS_ThrowTypeError(ctx, "Object.defineProperties called on non-object");
            return -1;
        }
        if (mode == PROPWALK_OBJCREATE && !JS_IsObject(step_arg(&s->hdr, 0))
            && !JS_IsNull(step_arg(&s->hdr, 0))) {
            JS_ThrowTypeError(ctx, "object prototype may only be an Object or null");
            return -1;
        }
        /* 20.1.2.1 step 1 / 7.3.23 step 1: ToObject the target (assign) or build the output array. A SPREAD's
           target is the object the opcode just created — already an object, and it belongs to the opcode's stack,
           so it is only BORROWED here. `result` is what the machine hands back, which a spread discards. */
        s->result = mode == PROPWALK_OBJCREATE ? JS_NewObjectProto(ctx, step_arg(&s->hdr, 0))
                  : (assign || mode == PROPWALK_DEFPROPS) ? JS_ToObject(ctx, step_arg(&s->hdr, 0))
                  : (mode == PROPWALK_SPREAD ? js_dup(step_arg(&s->hdr, 0))
                     : mode == PROPWALK_DESCS ? JS_NewObject(ctx) : JS_NewArray(ctx));
        if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; return -1; }
        s->hdr.stage = PW_SOURCE;
    }
    for (;;) {
        if (s->hdr.stage == PW_SOURCE) {
            /* open the next SOURCE. assign has argc-1 of them and skips the nullish ones (step 3.a); the others
               have exactly one, argv[0]. */
            JSValueConst src;
            /* How many sources this sink has: assign takes every argument after the target, a spread takes the one
               the opcode handed it, values/entries take argv[0] — and for those two an EXTRA argument is not a
               second source, so the bound is a mode fact, not argc. */
            int src_end = assign ? s->hdr.argc : target_is_arg0 ? 2 : 1;
            if (s->src_i >= src_end) {
                if (defprops) { s->hdr.stage = PW_DEFINE; goto defprops_apply; }
                JS_FreeValue(ctx, cb_result); return 0;
            }
            /* 20.1.2.2 step 3: `Object.create(p)` with no Properties defines nothing at all — an ABSENT
               argument, not an empty one, so it never reaches ToObject's TypeError on undefined. */
            if (mode == PROPWALK_OBJCREATE && JS_IsUndefined(step_arg(&s->hdr, 1))) {
                JS_FreeValue(ctx, cb_result); return 0;
            }
            src = step_arg(&s->hdr, s->src_i);
            /* @ATOMOWNER a spread's target is a FRESH object the opcode just built and the page cannot name it,
               so it can never be the source — which makes this a statement about this engine's own logic and
               not about the page's. ECMAScript §7.3.25 "CopyDataProperties ( target, source, excludedItems )"
               says so itself: "The target passed in here is always a newly created object which is not
               directly accessible in case of an error being thrown." It is the assert that makes the source
               range's start impossible to get wrong again, rather than a comment saying it must not be. */
            DCHECK(mode != PROPWALK_SPREAD || JS_VALUE_GET_TAG(src) != JS_TAG_OBJECT ||
                   JS_VALUE_GET_PTR(src) != JS_VALUE_GET_PTR(s->result),
                   "a spread opened its own TARGET as a source — ECMAScript §13.2.5.6's CopyDataProperties "
                   "call reads the "
                   "source only, so self-copying CALLS a getter the object literal defined and replaces the "
                   "accessor with a data property of its result");
            /* a nullish source contributes nothing to assign (step 3.a) or to a spread (`{...null}` is empty), but
               values/entries THROW on it — their step 1 is ToObject of the argument itself. A non-object source is
               BOXED, which is how `{..."ab"}` yields {0:"a",1:"b"}. */
            if ((assign || mode == PROPWALK_SPREAD) && (JS_IsNull(src) || JS_IsUndefined(src))) {
                s->src_i++;
                continue;
            }
            s->obj = JS_ToObject(ctx, src);
            if (JS_IsException(s->obj)) { s->obj = JS_UNDEFINED; JS_FreeValue(ctx, cb_result); return -1; }
            /* the snapshot is taken WITHOUT JS_GPN_ENUM_ONLY and each key's enumerability re-checked below,
               because the reads run between the snapshot and the check and one of them can redefine a later key.
               [[OwnPropertyKeys]] is a REQUEST: on a Proxy it is the `ownKeys` trap, the page's code, and taking
               it with JS_GetOwnPropertyNamesInternal drove that trap from C. */
            s->hdr.stage = PW_KEYS;   /* the ASK is issued from the stage that collects its answer */
        }
        if (s->hdr.stage == PW_KEYS) {
            /* the key list arrives as an ARRAY. Turning it into the atom snapshot invokes nothing: it is a dense
               array this engine built, of Strings and Symbols the invariant already validated.
               assign and a spread copy SYMBOL keys too; values/entries take only the string ones. */
            JSValue keys;
            uint32_t klen = 0, ki, kept = 0;
            r = step_ownkeys_run(ctx, &s->hdr, s->obj, cb_result, &keys, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            if (js_get_length32(ctx, &klen, keys)) { JS_FreeValue(ctx, keys); return -1; }
            s->atoms = klen ? js_mallocz(ctx, sizeof(s->atoms[0]) * klen) : NULL;
            if (klen && !s->atoms) { JS_FreeValue(ctx, keys); return -1; }
            for (ki = 0; ki < klen; ki++) {
                JSValue kv = JS_GetPropertyUint32(ctx, keys, ki);
                JSAtom at;
                if (JS_IsException(kv)) { JS_FreeValue(ctx, keys); s->len = kept; return -1; }
                if (JS_IsSymbol(kv) && mode != PROPWALK_ASSIGN && mode != PROPWALK_SPREAD
                    && mode != PROPWALK_DESCS && !defprops) {
                    JS_FreeValue(ctx, kv);
                    continue;
                }
                at = JS_ValueToAtom(ctx, kv);
                JS_FreeValue(ctx, kv);
                if (at == JS_ATOM_NULL) { JS_FreeValue(ctx, keys); s->len = kept; return -1; }
                s->atoms[kept].atom = at;
                s->atoms[kept].is_enumerable = false;
                kept++;
            }
            JS_FreeValue(ctx, keys);
            s->len = kept;
            s->i = 0;
            s->hdr.stage = PW_KEYHEAD;
        }
        if (s->hdr.stage == PW_KEYHEAD) {
            int res;
            if (s->i >= s->len) { s->hdr.stage = PW_SRCDONE; goto source_done; }
            if (mode == PROPWALK_SPREAD) {
                /* `{a, ...rest}`: the keys already bound by the pattern are excluded. The list is an object
                   the COMPILER built for this destructuring, which no page can reach — so this lookup invokes
                   nothing. ASSERTED rather than claimed: for_in_is_ordinary is exactly the question, and if the
                   exclusion list ever stopped being ordinary this would be the page's code running from C. */
                JSValueConst excl = step_arg(&s->hdr, 2);
                if (JS_VALUE_GET_TAG(excl) == JS_TAG_OBJECT) {
                    DCHECK(for_in_is_ordinary(ctx, JS_VALUE_GET_OBJ(excl)),
                           "the spread's exclusion list must be the compiler-built object, never the page's");
                    res = JS_GetOwnPropertyInternal(ctx, NULL, JS_VALUE_GET_OBJ(excl), s->atoms[s->i].atom);
                    if (res < 0) { JS_FreeValue(ctx, cb_result); return -1; }
                    if (res) { s->i++; continue; }
                }
            }
            /* 7.3.23 step 3.a: `desc = ? O.[[GetOwnProperty]](key)` — a REQUEST, because on a Proxy it is the
               `getOwnPropertyDescriptor` trap. JS_GetOwnPropertyFlagsInternal ran that trap from C. */
            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
            s->hdr.stage = PW_DESC;
            s->hdr.cb_coerce[0] = s->obj;
            *out_cb = s->hdr.cb_coerce; *out_argc = (int)s->atoms[s->i].atom;
            return 12;
        }
        if (s->hdr.stage == PW_DESC) {
            /* the descriptor object, or undefined when the property is gone. Reading `enumerable` off it invokes
               nothing — FromPropertyDescriptor built it. */
            JSValue desc = cb_result, en;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(desc)) return -1;
            if (JS_IsUndefined(desc)) { s->i++; s->hdr.stage = PW_KEYHEAD; continue; }
            if (mode == PROPWALK_DESCS) {
                /* 20.1.2.9 step 4.b-c: the descriptor IS the element, kept for every key regardless of
                   enumerability, and defined on the result. */
                if (js_define_prop_borrowed_key(ctx, s->result, s->atoms[s->i].atom, desc,
                                                JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                    return -1;
                s->i++; s->hdr.stage = PW_KEYHEAD;
                continue;
            }
            {
                int enumerable = js_desc_object_is_enumerable(ctx, desc);
                JS_FreeValue(ctx, desc);
                if (enumerable < 0) return -1;
                if (!enumerable) { s->i++; s->hdr.stage = PW_KEYHEAD; continue; }
            }
            if (mode == PROPWALK_KEYS) {
                /* 7.3.23 with kind = key: the KEY is the element, and there is no Get at all. The element is
                   the page's, so the name is delivered as a VALUE — see js_enum_key_value. */
                JSValue kv = js_enum_key_value(ctx, s->atoms[s->i].atom);
                if (JS_IsException(kv)) return -1;
                if (JS_CreateDataPropertyUint32(ctx, s->result, s->j++, kv, 0) < 0) return -1;
                s->i++; s->hdr.stage = PW_KEYHEAD;
                continue;
            }
            s->hdr.stage = PW_GET;
        }
        if (s->hdr.stage == PW_GET) {
            r = step_getprop_run(ctx, &s->hdr, s->obj, s->atoms[s->i].atom, cb_result,
                                 &s->el, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->hdr.stage = PW_PLACE;
        }
        if (s->hdr.stage == PW_PLACE && defprops) {
            /* 20.1.2.3.1 step 4.a.ii: the value IS a descriptor object, and ToPropertyDescriptor reads twelve
               more keyed operations off it. The cursor is resumable, so this stage is re-entered until it says
               the descriptor is complete. */
            r = js_desc_cursor_run(ctx, &s->dcur, s->el, cb_result, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            JS_FreeValue(ctx, s->el); s->el = JS_UNDEFINED;
            if (!s->dl) {
                /* at most one descriptor per snapshot key; allocated once the first one is complete so a walk
                   that keeps none allocates nothing. */
                s->dl = js_mallocz(ctx, sizeof(*s->dl) * (size_t)(s->len ? s->len : 1));
                if (!s->dl) { JS_ThrowOutOfMemory(ctx); return -1; }
                s->dk = js_mallocz(ctx, sizeof(*s->dk) * (size_t)(s->len ? s->len : 1));
                if (!s->dk) { JS_ThrowOutOfMemory(ctx); return -1; }
            }
            DCHECK(s->nd < (s->len ? s->len : 1), "defineProperties collected more descriptors than keys");
            s->dl[s->nd].flags = s->dcur.flags;
            s->dl[s->nd].value = s->dcur.value;    /* the cursor's ownership MOVES into the list */
            s->dl[s->nd].getter = s->dcur.getter;
            s->dl[s->nd].setter = s->dcur.setter;
            s->dk[s->nd] = JS_DupAtom(ctx, s->atoms[s->i].atom);
            s->nd++;
            JS_FreeValue(ctx, s->dcur.cb[0]);
            js_desc_cursor_init(&s->dcur);         /* the next key starts a fresh one */
            s->i++;
            s->hdr.stage = PW_KEYHEAD;
            continue;
        }
        if (s->hdr.stage == PW_PLACE) {
            JSValue item = s->el;
            s->el = JS_UNDEFINED;   /* the state no longer owns it: every path below consumes it */
            if (assign) {
                /* 20.1.2.1 step 3.a.iii.2: Set(to, nextKey, propValue, true) — through the keyed-WRITE step, so a
                   SETTER or a Proxy `set` trap on the TARGET runs on the chain. The element is parked back on the
                   state because the write suspends and this function's locals do not survive that. */
                s->el = item;
                s->hdr.stage = PW_WRITE;
            } else if (mode == PROPWALK_SPREAD) {
                /* CreateDataPropertyOrThrow on a FRESH object: no setter, no trap, nothing that can suspend. */
                if (js_define_prop_borrowed_key(ctx, s->result, s->atoms[s->i].atom, item,
                                                JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                    return -1;
            } else if (mode == PROPWALK_ENTRIES) {
                JSValue pair = JS_NewArray(ctx), key;
                if (JS_IsException(pair)) { JS_FreeValue(ctx, item); return -1; }
                /* the pair's first element is the page's, so the name is delivered as a VALUE — see
                   js_enum_key_value. */
                key = js_enum_key_value(ctx, s->atoms[s->i].atom);
                if (JS_IsException(key)) { JS_FreeValue(ctx, pair); JS_FreeValue(ctx, item); return -1; }
                if (JS_CreateDataPropertyUint32(ctx, pair, 0, key, JS_PROP_THROW) < 0) {
                    JS_FreeValue(ctx, pair); JS_FreeValue(ctx, item); return -1;
                }
                if (JS_CreateDataPropertyUint32(ctx, pair, 1, item, JS_PROP_THROW) < 0) {
                    JS_FreeValue(ctx, pair); return -1;   /* item was consumed by the failing call */
                }
                if (JS_CreateDataPropertyUint32(ctx, s->result, s->j++, pair, 0) < 0)
                    return -1;
            } else {
                if (JS_CreateDataPropertyUint32(ctx, s->result, s->j++, item, 0) < 0)
                    return -1;
            }
            if (s->hdr.stage != PW_WRITE) {
                s->i++;
                s->hdr.stage = PW_KEYHEAD;
            }
        }
        if (s->hdr.stage == PW_WRITE) {
            r = step_setprop_run(ctx, &s->hdr, s->result, s->atoms[s->i].atom, s->el, cb_result,
                                 out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            JS_FreeValue(ctx, s->el); s->el = JS_UNDEFINED;
            s->i++;
            s->hdr.stage = PW_KEYHEAD;
        }
        if (s->hdr.stage == PW_DEFINE) {
        defprops_apply:
            /* 20.1.2.3.1 step 5, the SECOND loop. It is separate because the spec says so — every descriptor is
               validated before any is defined — and because each define is a REQUEST: the target can be a Proxy,
               whose `defineProperty` trap is the page's code. */
            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
            if (s->di >= s->nd) return 0;
            {
                JSPropertyDescriptor *d = &s->dl[s->di];
                JS_FreeValue(ctx, s->cb[0]); JS_FreeValue(ctx, s->cb[1]);
                s->cb[0] = js_dup(s->result);
                s->cb[1] = js_dup(d->value);
                s->hdr.desc_get = d->getter;   /* borrowed: the list holds them across the request */
                s->hdr.desc_set = d->setter;
                s->hdr.desc_flags = d->flags | JS_PROP_THROW | JS_PROP_DEFINE_PROPERTY;
                s->hdr.stage = PW_DEFINED;
                *out_cb = s->cb; *out_argc = (int)s->dk[s->di];
                return 10;   /* DEFINE */
            }
        }
        if (s->hdr.stage == PW_DEFINED) {
            if (JS_IsException(cb_result)) return -1;
            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
            s->di++;
            s->hdr.stage = PW_DEFINE;
            continue;
        }
        if (s->hdr.stage == PW_SRCDONE) {
        source_done:
            /* this source is exhausted: release its snapshot before the next one replaces it (the teardown frees
               only what the state currently holds, so overwriting either field without freeing it leaks). */
            js_free_prop_enum(ctx, s->atoms, s->len);
            s->atoms = NULL; s->len = 0;
            JS_FreeValue(ctx, s->obj); s->obj = JS_UNDEFINED;
            s->src_i++;
            s->hdr.stage = PW_SOURCE;
        }
    }
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit) for the walk itself — every source key read is the page's
   getter or a Proxy trap, which is what makes Object.values / entries / assign / spread machines a fork
   reaches. The DEFINEPROPS-only collection (dcur, dl, dk) is NOT here: it holds descriptor records whose deep
   copy is its own capability, and js_step_has_borrowed_link refuses a fork while one is in flight rather than
   letting the sibling share them. */
static void js_prop_desc_visit(JSContext *ctx, void *elem, JSStepVisit *v)
{
    JSPropertyDescriptor *d = elem;
    v->val(ctx, &d->value);
    v->val(ctx, &d->getter);
    v->val(ctx, &d->setter);
}
static void js_prop_dkey_visit(JSContext *ctx, void *elem, JSStepVisit *v) { v->atom(ctx, (JSAtom *)elem); }

static void js_prop_walk_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSPropWalk *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->el);
    v->val(ctx, &s->obj);
    v->val(ctx, &s->cb[0]);
    v->val(ctx, &s->cb[1]);
    v->props(ctx, &s->atoms, s->len);
    /* 20.1.2.3.1's TWO LOOPS: Object.defineProperties reads every descriptor before defining any, so the
       collected records and their keys are the machine's for the whole of the second loop. This used to be a
       REFUSED fork — the deep copy did not exist — and it is declared now that the element-array operation
       does, which is what removing a refusal looks like rather than keeping it as a permanent carve-out. */
    js_desc_cursor_visit(ctx, &s->dcur, v);
    /* one descriptor per snapshot key is the ALLOCATION; nd is how many are complete so far, and the walk keeps
       writing up to that allocation after a fork lands in one of the page's getters. */
    v->array(ctx, (void **)&s->dl, sizeof(JSPropertyDescriptor), (int)s->nd,
             (int)(s->len ? s->len : 1), js_prop_desc_visit);
    v->array(ctx, (void **)&s->dk, sizeof(JSAtom), (int)s->nd, (int)(s->len ? s->len : 1), js_prop_dkey_visit);
}

static JSValue js_prop_walk_fini(JSContext *ctx, void *st, bool take_result)
{
    JSPropWalk *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;   /* handed out, read above before the rest is released */
    return r;
}

/* Object.getOwnPropertyNames / getOwnPropertySymbols and Reflect.ownKeys are one operation with a filter:
   ToObject (or Reflect's flat Object requirement), then [[OwnPropertyKeys]], then keep the keys of the requested
   kind. Only the middle step can run the page's code — the `ownKeys` trap — and JS_GetOwnPropertyNames2 reached
   it through JS_GetOwnPropertyNamesInternal, i.e. JS_CallFree from C with no flow base, so a trap containing a
   loop aborted at its back-edge. The request is GP_OWNKEYS, answered by the one keyed-operation entry that
   already answers get/has/set/delete/define, and the 10.5.11 invariant rides its continuation.
   def->arg: the JS_GPN_* mask to keep, or 0 for Reflect.ownKeys, whose receiver rule differs (a primitive is a
   TypeError, not a wrapper) and which keeps everything. */
#define OWNKEYS_REFLECT 0
typedef struct JSOwnKeys {
    JSStepHdr hdr;
    JSValue obj;      /* ToObject(argv[0]) — held across the trap (owned) */
    JSValue keys;     /* the validated key ARRAY the request delivers (owned) */
} JSOwnKeys;

/* Object.getOwnPropertyNames and .getOwnPropertySymbols ARE 20.1.2.11.1 GetOwnPropertyKeys — their own bodies are
   one line each — so their two stages name ITS steps, exactly as find's walk names FindViaPredicate's.
   Reflect.ownKeys states the same two operations itself. Its step 3 and GetOwnPropertyKeys' steps 3-5 run none of
   the page's code (the key array is one this engine built), so both finish in the stage that rests on the
   internal method. ONE stage list expanded once per algorithm — see AFIND_STAGES. */
#define OWNKEYS_STAGES(X, RECV, KEYS) \
    X(OWNKEYS_RECV, RECV) \
    X(OWNKEYS_KEYS, KEYS)
enum { OWNKEYS_STAGES(JS_STEP_STAGE_ENUM, 0, 0) };
static const char *const js_ownkeys_names_steps[] = {
    OWNKEYS_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.2.11.1 step 1 (obj is ToObject(O))",
        "20.1.2.11.1 step 2 (keys is obj.[[OwnPropertyKeys]]())")
    NULL };
static const char *const js_ownkeys_syms_steps[] = {
    OWNKEYS_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.2.11.1 step 1 (obj is ToObject(O))",
        "20.1.2.11.1 step 2 (keys is obj.[[OwnPropertyKeys]]())")
    NULL };
static const char *const js_reflect_ownkeys_steps[] = {
    OWNKEYS_STAGES(JS_STEP_STAGE_LABEL,
        "28.1.10 step 1 (target is an Object)",
        "28.1.10 step 2 (keys is target.[[OwnPropertyKeys]]())")
    NULL };

static int js_ownkeys_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSOwnKeys *s = st;
    JSValueConst arg0 = step_arg(&s->hdr, 0);

    if (s->hdr.stage == OWNKEYS_RECV) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->obj = JS_UNDEFINED; s->keys = JS_UNDEFINED;
        if (s->hdr.arg == OWNKEYS_REFLECT) {
            /* 28.1.10 step 1: Reflect.ownKeys takes an Object, it does not coerce one. */
            if (JS_VALUE_GET_TAG(arg0) != JS_TAG_OBJECT) { JS_ThrowTypeErrorNotAnObject(ctx); return -1; }
            s->obj = js_dup(arg0);
        } else {
            s->obj = JS_ToObject(ctx, arg0);   /* a primitive wrapper; runs no user code */
            if (JS_IsException(s->obj)) { s->obj = JS_UNDEFINED; return -1; }
        }
        s->hdr.stage = OWNKEYS_KEYS;   /* the ASK is issued from the stage that collects its answer */
    }
    DCHECK(s->hdr.stage == OWNKEYS_KEYS, "Object.getOwnPropertyNames: unknown stage");
    {
        int r = step_ownkeys_run(ctx, &s->hdr, s->obj, cb_result, &s->keys, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
    }
    if (s->hdr.arg == OWNKEYS_REFLECT)
        return 0;   /* CreateArrayFromList over every key, which is what the request already delivered */
    {
        /* the same filter JS_GetOwnPropertyNames2 applies, over the delivered array. Reading it invokes nothing:
           it is a fresh dense array this engine built. */
        JSValue r = JS_NewArray(ctx);
        uint32_t len = 0, i, j = 0;
        if (JS_IsException(r)) return -1;
        if (js_get_length32(ctx, &len, s->keys)) { JS_FreeValue(ctx, r); return -1; }
        for (i = 0; i < len; i++) {
            JSValue kv = JS_GetPropertyUint32(ctx, s->keys, i);
            bool keep;
            if (JS_IsException(kv)) { JS_FreeValue(ctx, r); return -1; }
            keep = JS_IsSymbol(kv) ? ((s->hdr.arg & JS_GPN_SYMBOL_MASK) != 0)
                                   : ((s->hdr.arg & JS_GPN_STRING_MASK) != 0);
            if (!keep) { JS_FreeValue(ctx, kv); continue; }
            if (JS_CreateDataPropertyUint32(ctx, r, j++, kv, 0) < 0) { JS_FreeValue(ctx, r); return -1; }
        }
        JS_FreeValue(ctx, s->keys);
        s->keys = r;
    }
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). Both fields, and nothing borrowed — the request that suspends
   it is a Proxy `ownKeys` TRAP, which is the page's code and can therefore branch on unknown input. */
static void js_ownkeys_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSOwnKeys *s = st;
    v->val(ctx, &s->keys);
    v->val(ctx, &s->obj);
}

static JSValue js_ownkeys_fini(JSContext *ctx, void *st, bool take_result)
{
    JSOwnKeys *s = st;
    JSValue r = take_result ? s->keys : JS_UNDEFINED;
    if (take_result) s->keys = JS_UNDEFINED;   /* handed out, read above before this releases the rest */
    return r;
}



/* 20.1.3.2 Object.prototype.hasOwnProperty and 20.1.2.14 Object.hasOwn: ONE machine, because they are the same
   three steps and differ only in their ORDER — hasOwnProperty coerces the KEY first and hasOwn the OBJECT — which
   is a spec operand and not a variation to hide behind two implementations. Both steps are the page's code:
   ToPropertyKey runs a key object's toString, and HasOwnProperty(O, P) is O.[[GetOwnProperty]](P), a trap on a
   Proxy. The C bodies ran both, and interned the key with JS_ValueToAtom rather than ToPropertyKey. */
typedef struct JSHasOwn {
    JSStepHdr hdr;    /* MUST be first */
    JSValue obj;      /* ToObject's result (owned) */
    JSAtom key;       /* ToPropertyKey's result (owned) */
    JSValue result;   /* DONE (owned) */
} JSHasOwn;
#define HASOWN_STATIC 1   /* Object.hasOwn(O, P); 0 = Object.prototype.hasOwnProperty(V) */

/* ToObject on the operand this form takes. It runs no user code — a primitive wraps, an object is itself — but
   it THROWS for null/undefined, which is step 2 of one form and step 1 of the other. */
static int js_hasown_toobject(JSContext *ctx, JSHasOwn *s)
{
    JSValueConst src = (s->hdr.arg & HASOWN_STATIC) ? step_arg(&s->hdr, 0) : s->hdr.this_val;
    s->obj = JS_ToObject(ctx, src);
    return JS_IsException(s->obj) ? -1 : 0;
}

/* TWO algorithms, one machine, and the ORDER of their first two steps is the whole difference — so the shared
   stage takes its text from the algorithm rather than describing one of them. ONE stage list expanded once per
   algorithm; see AFIND_STAGES. */
#define HASOWN_STAGES(X, KEY, HAS) \
    X(HASOWN_KEY, KEY) \
    X(HASOWN_HAS, HAS)
enum { HASOWN_STAGES(JS_STEP_STAGE_ENUM, 0, 0) };
static const char *const js_hasown_steps[] = {
    HASOWN_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.2.14 steps 1-2 (obj is ToObject(O); key is ToPropertyKey(P))",
        "20.1.2.14 step 3 (HasOwnProperty(obj, key))")
    NULL };
static const char *const js_hasownprop_steps[] = {
    HASOWN_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.3.2 steps 1-2 (P is ToPropertyKey(V); O is ToObject(this value))",
        "20.1.3.2 step 3 (HasOwnProperty(O, P))")
    NULL };

static int js_hasown_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSHasOwn *s = st;
    bool statik = (s->hdr.arg & HASOWN_STATIC) != 0;
    JSValueConst keysrc = statik ? step_arg(&s->hdr, 1) : step_arg(&s->hdr, 0);
    int r;

    if (s->hdr.stage == HASOWN_KEY) {
        if (s->hdr.str_phase == STR_PH_START) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            s->obj = JS_UNDEFINED; s->key = JS_ATOM_NULL; s->result = JS_UNDEFINED;
            if (statik && js_hasown_toobject(ctx, s) < 0)   /* 20.1.2.14 step 1 */
                return -1;
        }
        /* ToPropertyKey: step 1 of the prototype form, step 2 of the static one. */
        r = step_topropkey_run(ctx, &s->hdr, keysrc, cb_result, &s->key, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r != 0)
            return r;
        if (!statik && js_hasown_toobject(ctx, s) < 0)   /* 20.1.3.2 step 2 */
            return -1;
        /* HasOwnProperty(O, P) — the object's [[GetOwnProperty]], which on a Proxy is its trap. Only whether it
           EXISTS is wanted, so the ordinary descriptor-object answer says it: undefined means absent. */
        s->hdr.stage = HASOWN_HAS;
        s->hdr.cb_coerce[0] = s->obj;   /* borrowed: the machine holds the receiver across the request */
        *out_cb = s->hdr.cb_coerce; *out_argc = (int)s->key;
        return 12;
    }
    DCHECK(s->hdr.stage == HASOWN_HAS, "Object.hasOwn's machine resumed in no stage");
    if (JS_IsException(cb_result))
        return -1;
    s->result = js_bool(!JS_IsUndefined(cb_result));
    JS_FreeValue(ctx, cb_result);
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_hasown_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSHasOwn *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->obj);
    v->atom(ctx, &s->key);
}

static JSValue js_hasown_fini(JSContext *ctx, void *st, bool take_result)
{
    JSHasOwn *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* DELETED: js_object_hasOwn's C body, and js_object_hasOwnProperty's. Both performed HasOwnProperty(O, P) with
   JS_GetOwnPropertyInternal from C — a Proxy's `getOwnPropertyDescriptor` trap with no flow base — and both
   interned the key with JS_ValueToAtom rather than ToPropertyKey, so a key object's `toString` ran from C too.
   They are one step machine (STEPDEF_OBJ_HASOWN), the arg choosing which of the two ORDERS the spec states. */

static JSValue js_object_valueOf(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return JS_ToObject(ctx, this_val);
}

/* 20.1.3.6 Object.prototype.toString. Everything before step 15 runs no page code — ToObject wraps a
   primitive, IsArray walks a Proxy's targets without a trap, and the builtin tag is a class id — but step 15's
   `Get(O, @@toStringTag)` is the page's, on an accessor or a Proxy. The C body read it with JS_GetProperty, so
   `Object.prototype.toString.call(new Proxy(o, {get(){for(;;){}}}))` had no flow base — and type detection by
   this call is in every bundle. */
typedef struct JSObjToString {
    JSStepHdr hdr;     /* MUST be first */
    JSValue result;    /* DONE (owned) */
    JSValue obj;       /* ToObject's result, HELD across the request that borrows it (owned) */
    JSAtom builtin;    /* the step 5-14 tag, used when step 16 rejects what the Get produced */
} JSObjToString;

/* Steps 16-17 test what the Get produced and build the string, neither of which runs the page's code, so the
   machine ends in the stage that rests at step 15. */
#define OBJTOSTR_STAGES(X) \
    X(OBJTOSTR_TAG_OF,  "20.1.3.6 steps 1-14 (O is ToObject(this value); builtinTag from IsArray and the slots)") \
    X(OBJTOSTR_TAG_GET, "20.1.3.6 step 15 (tag is Get(O, @@toStringTag))")
enum { OBJTOSTR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_obj_tostring_steps[] = { OBJTOSTR_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_object_tostring_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSObjToString *s = st;
    JSValue tag;

    if (s->hdr.stage == OBJTOSTR_TAG_OF) {
        JSValueConst this_val = s->hdr.this_val;
        JSValue obj;
        int is_array;
        JSObject *p;
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        s->result = JS_UNDEFINED; s->obj = JS_UNDEFINED; s->builtin = JS_ATOM_NULL;
        if (JS_IsNull(this_val)) {                       /* steps 1-2 */
            s->result = js_new_string8(ctx, "[object Null]");
            return JS_IsException(s->result) ? -1 : 0;
        }
        if (JS_IsUndefined(this_val)) {
            s->result = js_new_string8(ctx, "[object Undefined]");
            return JS_IsException(s->result) ? -1 : 0;
        }
        obj = JS_ToObject(ctx, this_val);                /* step 3 */
        if (JS_IsException(obj))
            return -1;
        s->obj = obj;   /* the STATE owns it from here: cb_coerce is BORROWED and the request outlives this call */
        is_array = js_is_array(ctx, obj);                /* step 4: 7.2.6, no trap on a Proxy */
        if (is_array < 0) return -1;
        if (is_array) {
            s->builtin = JS_ATOM_Array;
        } else if (JS_IsFunction(ctx, obj)) {
            s->builtin = JS_ATOM_Function;
        } else {
            p = JS_VALUE_GET_OBJ(obj);
            switch (p->class_id) {                       /* steps 5-14 */
            case JS_CLASS_STRING:
            case JS_CLASS_ARGUMENTS:
            case JS_CLASS_MAPPED_ARGUMENTS:
            case JS_CLASS_ERROR:
            case JS_CLASS_BOOLEAN:
            case JS_CLASS_NUMBER:
            case JS_CLASS_DATE:
            case JS_CLASS_REGEXP:
                s->builtin = ctx->rt->class_array[p->class_id].class_name;
                break;
            default:
                s->builtin = JS_ATOM_Object;
                break;
            }
        }
        s->builtin = JS_DupAtom(ctx, s->builtin);
        s->hdr.stage = OBJTOSTR_TAG_GET;                 /* step 15: Get(O, @@toStringTag) */
        s->hdr.cb_coerce[0] = s->obj;                    /* borrowed: the state holds it across the request */
        *out_cb = s->hdr.cb_coerce; *out_argc = (int)JS_ATOM_Symbol_toStringTag;
        return 6;
    }
    DCHECK(s->hdr.stage == OBJTOSTR_TAG_GET, "Object.prototype.toString's machine resumed in no stage");
    if (JS_IsException(cb_result))
        return -1;
    tag = cb_result;
    if (!JS_IsString(tag)) {                             /* step 16 */
        JS_FreeValue(ctx, tag);
        tag = JS_AtomToString(ctx, s->builtin);
        if (JS_IsException(tag))
            return -1;
    }
    s->result = JS_ConcatString3(ctx, "[object ", tag, "]");   /* step 17 */
    return JS_IsException(s->result) ? -1 : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_object_tostring_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSObjToString *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->obj);
    v->atom(ctx, &s->builtin);
}

static JSValue js_object_tostring_fini(JSContext *ctx, void *st, bool take_result)
{
    JSObjToString *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* The HOST printer's tag for a value — 20.1.3.6 steps 1-14 and NOT step 15. Both callers (the shell's `print`
   and the harness's) reach it only after JS_ToCString on the value THREW, so it is the last resort for showing
   something about an object whose own toString failed; running the page's @@toStringTag getter there would be
   more page code on a path that exists because page code just raised. The spec algorithm, step 15 included, is
   the STEPDEF_OBJ_TOSTRING machine, which is the only thing script can reach. */
JSValue JS_ToObjectString(JSContext *ctx, JSValueConst val)
{
    JSAtom atom = JS_ATOM_Object;
    JSValue obj, tag;
    int is_array;

    if (JS_IsNull(val))
        return js_new_string8(ctx, "[object Null]");
    if (JS_IsUndefined(val))
        return js_new_string8(ctx, "[object Undefined]");
    obj = JS_ToObject(ctx, val);
    if (JS_IsException(obj))
        return obj;
    is_array = js_is_array(ctx, obj);
    if (is_array < 0) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (is_array) {
        atom = JS_ATOM_Array;
    } else if (JS_IsFunction(ctx, obj)) {
        atom = JS_ATOM_Function;
    } else {
        JSObject *p = JS_VALUE_GET_OBJ(obj);
        switch (p->class_id) {
        case JS_CLASS_STRING:
        case JS_CLASS_ARGUMENTS:
        case JS_CLASS_MAPPED_ARGUMENTS:
        case JS_CLASS_ERROR:
        case JS_CLASS_BOOLEAN:
        case JS_CLASS_NUMBER:
        case JS_CLASS_DATE:
        case JS_CLASS_REGEXP:
            atom = ctx->rt->class_array[p->class_id].class_name;
            break;
        default:
            break;
        }
    }
    JS_FreeValue(ctx, obj);
    tag = JS_AtomToString(ctx, atom);
    if (JS_IsException(tag))
        return tag;
    return JS_ConcatString3(ctx, "[object ", tag, "]");
}

/* WEB IDL §3.2.11's "convert a DOMString to a scalar value string" — see quickjs-step.h. `str` is OWNED. */
JSValue JS_ToScalarValueString(JSContext *ctx, JSValue str)
{
    JSString *p;
    uint16_t *buf;
    int i, len, bad = 0;
    JSValue r;

    if (JS_VALUE_GET_TAG(str) != JS_TAG_STRING)
        return str;
    p = JS_VALUE_GET_STRING(str);
    if (!p->is_wide_char)
        return str;   /* a one-byte string holds no surrogate at all */
    len = p->len;
    for (i = 0; i < len; i++) {
        uint16_t c = str16(p)[i];
        if (is_hi_surrogate(c)) {
            if (i + 1 < len && is_lo_surrogate(str16(p)[i + 1])) { i++; continue; }
            bad = 1; break;
        }
        if (is_lo_surrogate(c)) { bad = 1; break; }
    }
    if (!bad)
        return str;
    buf = js_malloc(ctx, sizeof(uint16_t) * (size_t)(len ? len : 1));
    if (!buf) { JS_FreeValue(ctx, str); return JS_EXCEPTION; }
    for (i = 0; i < len; i++) {
        uint16_t c = str16(p)[i];
        if (is_hi_surrogate(c)) {
            if (i + 1 < len && is_lo_surrogate(str16(p)[i + 1])) {
                buf[i] = c;
                buf[i + 1] = str16(p)[i + 1];
                i++;
                continue;
            }
            buf[i] = 0xfffd;
        } else if (is_lo_surrogate(c)) {
            buf[i] = 0xfffd;
        } else {
            buf[i] = c;
        }
    }
    r = js_new_string16_len(ctx, buf, len);
    js_free(ctx, buf);
    JS_FreeValue(ctx, str);
    return r;
}

/* A DIAGNOSTIC READ: the value of a DATA property named `prop` on `v` or its prototype chain, JS_UNDEFINED if
   there is none. IT NEVER INVOKES AN ACCESSOR, and that is not a simplification — reaching a getter from C is
   the one thing this engine refuses outright, because there is no flow base under a C activation and a loop in
   the getter's body would drive to completion instead of parking. A diagnostic is the worst possible place to
   run code: it is on the path that REPORTS an error, so a getter that throws replaces the exception being
   reported with its own.
   IT WAS AN ORDINARY READ, and DOMException's `name` and `message` are IDL ACCESSORS — so every DOMException
   the host reported aborted the process at that DCHECK, three layers from anything a test could see. An object
   whose name is behind a getter now falls through to the "[object Class]" form below, which is the honest
   answer rather than a crash. */
static JSValue JS_DiagGetData(JSContext *ctx, JSValueConst v, const char *prop)
{
    JSAtom atom = JS_NewAtom(ctx, prop);
    JSObject *p;
    JSValue out = JS_UNDEFINED;

    if (atom == JS_ATOM_NULL) return JS_UNDEFINED;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT) { JS_FreeAtom(ctx, atom); return JS_UNDEFINED; }
    for (p = JS_VALUE_GET_OBJ(v); p != NULL; p = p->shape->proto) {
        JSShapeProperty *prs;
        JSProperty *pr;
        prs = find_own_property(&pr, p, atom);
        if (!prs) continue;
        if ((prs->flags & JS_PROP_TMASK) == 0) out = js_dup(pr->u.value);
        break;   /* an accessor, a varref or an autoinit shadows the chain — and none of them may be run here */
    }
    JS_FreeAtom(ctx, atom);
    return out;
}

/* A DIAGNOSTIC string for a value — see quickjs-step.h. */
const char *JS_DiagCString(JSContext *ctx, JSValueConst v, char **powned)
{
    JSValue n, m, t;
    const char *ns, *ms, *r;
    size_t need;

    *powned = NULL;
    if (!JS_IsObject(v)) {
        const char *prim = JS_ToCString(ctx, v);   /* a primitive's conversion invokes nothing */

        /* …AND YET IT CAN FAIL, WHICH IS A DIFFERENT CLAIM FROM THE ONE THE LINE ABOVE MAKES AND WAS READ AS
           THE SAME ONE. "Invokes nothing" is about USER CODE and is true of every primitive; a SYMBOL is the
           one primitive whose ToString THROWS ("cannot convert symbol to string", the only arm of
           JS_ToStringInternal's JS_TAG_SYMBOL case a caller without JS_TO_STRING_IS_PROPERTY_KEY can reach),
           and this path returned NULL with that TypeError STILL LIVE IN THE CONTEXT.
           THE OBJECT PATH BELOW ALREADY CLEARS ONE — `JS_FreeValue(ctx, JS_GetException(ctx))` — so this
           function disagreed with itself about its own contract, and the half that was silent is the half a
           caller cannot see: every caller reads the NULL and none of them looks at the context.
           WHAT IT COSTS IS NOT A MESSAGE, IT IS THE CALLER'S ALGORITHM. This function's reason for existing is
           that "a host reporting what went wrong must not depend on the code that went wrong", and its one
           browser-side caller is HTML §8.1.4.6 Runtime script errors' extract error information — which goes
           on to FIRE an `error` event, so the page's listeners ran with a stray throw standing, and the
           TypeError then leaked out as the completion of whatever had reported. `<script>throw Symbol()</script>`
           reaches it through §8.1.4.4's report of an abrupt classic script and §8.2's `reportError` reaches it
           in one call, so it is a page-held wrong answer either way and never an invariant to assert on.
           NULL IS THE HONEST ANSWER AND NOT A NARROWING: §8.1.4.6 says message, filename, lineno and colno are
           "implementation-defined values derived from exception", so the caller's own no-string fallback is
           conformant, and a symbol description would be a better diagnostic rather than a required one. */
        if (!prim)
            JS_FreeValue(ctx, JS_GetException(ctx));
        return prim;
    }

    /* A THROWN FUNCTION IS DESCRIBED BY WHERE IT WAS WRITTEN, because it carries nothing else. A function has no
       `message` and no [[ErrorData]], so the name path below reported a named one as its bare name — which reads
       as an Error KIND, `foo` beside `TypeError` — and an anonymous one as the EMPTY STRING, which the host's
       page-error surface drops as "nothing to say": a value thrown and never reported. NOT THE `[object
       Function]` ROW A REAL APPLICATION REPORTS: that one is the solver's own CONCOLIC class, which is callable
       and so tags as Function — this arm was landed against it and a drive refuted the attribution, and the
       concolic is named by the host's report (solver/result.c) because this string also becomes a PAGE-VISIBLE
       ErrorEvent `message`, where the solver's shapes must never appear. The definition site is a STORED field of the
       bytecode (the same three Function.prototype's fileName/lineNumber/columnNumber accessors return), so
       reading it runs no page code. A function with no bytecode — a C function, a bound function, a Proxy —
       keeps the forms below, which is narrower and honest: a callable Proxy is `[object Function]` because
       nothing about its target may be read without its traps. */
    {
        JSFunctionBytecode *b = JS_GetFunctionBytecode(v);
        if (b) {
            JSValue fn = JS_DiagGetData(ctx, v, "name");
            const char *fns = JS_IsString(fn) ? JS_ToCString(ctx, fn) : NULL;
            const char *file = b->filename != JS_ATOM_NULL ? JS_AtomToCString(ctx, b->filename) : NULL;

            JS_FreeValue(ctx, fn);
            need = (fns ? strlen(fns) : 0) + (file ? strlen(file) : 0) + 96;
            *powned = js_malloc_rt(ctx->rt, need);
            if (*powned) {
                if (fns && *fns)
                    snprintf(*powned, need, "a function named `%s` was thrown, defined at %s:%d:%d",
                             fns, file && *file ? file : "(no file name)", b->line_num, b->col_num);
                else
                    snprintf(*powned, need, "an anonymous function was thrown, defined at %s:%d:%d",
                             file && *file ? file : "(no file name)", b->line_num, b->col_num);
            }
            if (fns) JS_FreeCString(ctx, fns);
            if (file) JS_FreeCString(ctx, file);
            if (*powned)
                return *powned;
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }

    n = JS_DiagGetData(ctx, v, "name");
    /* AN EMPTY NAME IS NO NAME. `ns` = "" composed an empty description, which the page-error surface drops, so
       an object whose `name` is the empty string vanished from the report instead of falling back to its
       constructor's name or its class. */
    if (JS_IsString(n) && JS_VALUE_GET_STRING(n)->len == 0) {
        JS_FreeValue(ctx, n);
        n = JS_UNDEFINED;
    }
    if (!JS_IsString(n)) {
        JSValue c = JS_DiagGetData(ctx, v, "constructor");
        JS_FreeValue(ctx, n);
        n = JS_IsObject(c) ? JS_DiagGetData(ctx, c, "name") : JS_UNDEFINED;
        JS_FreeValue(ctx, c);
    }
    m = JS_DiagGetData(ctx, v, "message");
    ns = JS_IsString(n) && JS_VALUE_GET_STRING(n)->len != 0 ? JS_ToCString(ctx, n) : NULL;   /* the constructor's may be "" too */
    ms = JS_IsString(m) ? JS_ToCString(ctx, m) : NULL;
    JS_FreeValue(ctx, n);
    JS_FreeValue(ctx, m);
    if (ns) {
        need = strlen(ns) + (ms ? strlen(ms) + 2 : 0) + 1;
        /* js_malloc_rt, not js_malloc: this runs on a path that may ALREADY hold a pending exception (the
           caller is reporting one), and the throwing allocator would replace it. NULL falls through to the
           class-name form below, which is the honest answer when there is no memory for the better one. */
        *powned = js_malloc_rt(ctx->rt, need);
        if (*powned) {
            if (ms && *ms) snprintf(*powned, need, "%s: %s", ns, ms);
            else           snprintf(*powned, need, "%s", ns);
        }
    }
    if (ns) JS_FreeCString(ctx, ns);
    if (ms) JS_FreeCString(ctx, ms);
    if (*powned)
        return *powned;
    JS_FreeValue(ctx, JS_GetException(ctx));
    t = JS_ToObjectString(ctx, v);   /* "[object Class]": a class read, no user code */
    if (JS_IsException(t))
        return NULL;
    r = JS_ToCString(ctx, t);
    JS_FreeValue(ctx, t);
    return r;
}

void JS_DiagFreeCString(JSContext *ctx, const char *s, char *owned)
{
    if (owned) js_free(ctx, owned);
    else if (s) JS_FreeCString(ctx, s);
}

/* 20.1.3.5 Object.prototype.toLocaleString is `? Invoke(O, "toString")` — a Get and then a CALL, both the
   page's: the method is whatever `toString` resolves to on the receiver, commonly an override. JS_Invoke ran
   both from C, so `({toString(){for(;;){}}}).toLocaleString()` drove to completion — and once the intrinsic
   Object.prototype.toString became a machine, the default receiver reached its backstop too, which is how the
   corpus named this site. */
typedef struct JSObjToLocale {
    JSStepHdr hdr;      /* MUST be first */
    JSValue result;     /* DONE (owned) */
    JSValue method;     /* `toString`, held across the call (owned) */
} JSObjToLocale;

/* Its one step IS Invoke(O, "toString"), and 7.3.20 Invoke is a GetV and a Call — two of the page's operations,
   so the machine rests once in each half of the step. */
#define OBJTOLOCALE_STAGES(X) \
    X(OBJTOLOCALE_ENTRY, "20.1.3.5 step 1 (Invoke(O, \"toString\"), before its GetV)") \
    X(OBJTOLOCALE_GET,   "20.1.3.5 step 1 via 7.3.20 step 2 (func is GetV(V, P))") \
    X(OBJTOLOCALE_CALL,  "20.1.3.5 step 1 via 7.3.20 step 3 (Call(func, V, argumentsList))")
enum { OBJTOLOCALE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_obj_tolocale_steps[] = { OBJTOLOCALE_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_object_tolocale_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSObjToLocale *s = st;

    if (s->hdr.stage == OBJTOLOCALE_ENTRY) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        s->result = JS_UNDEFINED; s->method = JS_UNDEFINED;
        s->hdr.stage = OBJTOLOCALE_GET;              /* Invoke's Get */
        s->hdr.cb_coerce[0] = s->hdr.this_val;       /* borrowed: the machine holds the receiver */
        *out_cb = s->hdr.cb_coerce; *out_argc = (int)JS_ATOM_toString;
        return 6;
    }
    if (s->hdr.stage == OBJTOLOCALE_GET) {
        if (JS_IsException(cb_result)) return -1;
        s->method = cb_result;
        cb_result = JS_UNDEFINED;
        if (!JS_IsFunction(ctx, s->method)) {
            /* Invoke's Call raises this for anything not callable, including undefined. */
            JS_ThrowTypeError(ctx, "not a function");
            return -1;
        }
        s->hdr.stage = OBJTOLOCALE_CALL;             /* Invoke's Call, with no arguments */
        s->hdr.cb_coerce[0] = s->hdr.this_val;
        s->hdr.cb_coerce[1] = s->method;
        *out_cb = s->hdr.cb_coerce; *out_argc = 0;
        return 3;
    }
    DCHECK(s->hdr.stage == OBJTOLOCALE_CALL, "Object.prototype.toLocaleString's machine resumed in no stage");
    if (JS_IsException(cb_result)) return -1;
    s->result = cb_result;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_object_tolocale_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSObjToLocale *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->method);
}

static JSValue js_object_tolocale_fini(JSContext *ctx, void *st, bool take_result)
{
    JSObjToLocale *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}


/* 7.3.15 SetIntegrityLevel and 7.3.16 TestIntegrityLevel — Object.seal / freeze / isSealed / isFrozen. ONE
   machine, because the two algorithms are the same [[OwnPropertyKeys]] walk with a [[GetOwnProperty]] per key
   and differ only in the operation they OPEN with and what they do per key; the level is the other arg bit.
   Four of the page's operations live in them — [[PreventExtensions]] or [[IsExtensible]], [[OwnPropertyKeys]],
   a [[GetOwnProperty]] per key, and for a Set a [[DefineOwnProperty]] per key — and the C bodies ran every one
   from C, so `Object.freeze(new Proxy(o, {ownKeys(){for(;;){}}}))` had no flow base.

   TestIntegrityLevel's extensibility check also moves to where the spec states it: step 1, BEFORE the keys are
   read, with step 2 returning false immediately. js_object_isSealed asked it LAST, so an extensible Proxy saw
   an ownKeys trap and a gopd per key that the spec never performs. */
typedef struct JSIntegrity {
    JSStepHdr hdr;        /* MUST be first. arg = INTEG_* bits */
    JSValue obj;          /* the receiver (owned) */
    JSValue keys;         /* [[OwnPropertyKeys]]' answer, an array this engine built (owned) */
    JSValue result;       /* DONE (owned) */
    JSDescFacts facts;    /* the current key's descriptor record (owned) */
    JSAtom key;           /* the key being processed (owned) */
    uint32_t i, len;
} JSIntegrity;
#define INTEG_FREEZE 1    /* the FROZEN level; 0 = sealed */
#define INTEG_TEST   2    /* TestIntegrityLevel; 0 = SetIntegrityLevel */
/* FOUR algorithms over two abstract operations: Object.seal / freeze ARE 7.3.15 SetIntegrityLevel and
   Object.isSealed / isFrozen ARE 7.3.16 TestIntegrityLevel, the LEVEL being an operand of each rather than a
   different algorithm — which is why seal and freeze rest at the same steps and so do the two tests. Only the
   entry stage is the caller's own step, so it is the one operand. ONE stage list expanded once per algorithm;
   see AFIND_STAGES.
   The DEFINE stage is its own sub-list because TestIntegrityLevel HAS no define: the two test arrays end one
   stage earlier, so a test machine resting there is a stage past its end, which step_stage_check names on its
   own rather than a label having to say "this one cannot happen". */
#define INTEG_STAGES(X, ENTRY, WALK) \
    X(IG_ENTRY, ENTRY) \
    WALK(X)
#define INTEG_WALK_SET(X) \
    X(IG_OPEN_GOT, "7.3.15 step 1 (status is O.[[PreventExtensions]]())") \
    X(IG_KEYS_GOT, "7.3.15 step 3 (keys is O.[[OwnPropertyKeys]]())") \
    X(IG_NEXT,     "7.3.15 steps 4.a / 5.b (the next element k of keys)") \
    X(IG_DESC_GOT, "7.3.15 step 5.b.i (currentDesc is O.[[GetOwnProperty]](k))")
#define INTEG_WALK_TEST(X) \
    X(IG_OPEN_GOT, "7.3.16 step 1 (extensible is IsExtensible(O))") \
    X(IG_KEYS_GOT, "7.3.16 step 3 (keys is O.[[OwnPropertyKeys]]())") \
    X(IG_NEXT,     "7.3.16 step 4 (the next element k of keys)") \
    X(IG_DESC_GOT, "7.3.16 step 4.a (currentDesc is O.[[GetOwnProperty]](k))")
#define INTEG_DEFINE_STAGE(X) \
    X(IG_DEFINE_GOT, "7.3.15 steps 4.a.i / 5.b.ii.3 (DefinePropertyOrThrow(O, k, desc))")
enum { INTEG_STAGES(JS_STEP_STAGE_ENUM, 0, INTEG_WALK_SET) INTEG_DEFINE_STAGE(JS_STEP_STAGE_ENUM) };
static const char *const js_obj_seal_steps[] = {
    INTEG_STAGES(JS_STEP_STAGE_LABEL, "20.1.2.22 step 1 (a non-object O is returned unchanged)", INTEG_WALK_SET)
    INTEG_DEFINE_STAGE(JS_STEP_STAGE_LABEL) NULL };
static const char *const js_obj_freeze_steps[] = {
    INTEG_STAGES(JS_STEP_STAGE_LABEL, "20.1.2.6 step 1 (a non-object O is returned unchanged)", INTEG_WALK_SET)
    INTEG_DEFINE_STAGE(JS_STEP_STAGE_LABEL) NULL };
static const char *const js_obj_issealed_steps[] = {
    INTEG_STAGES(JS_STEP_STAGE_LABEL, "20.1.2.18 step 1 (a non-object O is already sealed)", INTEG_WALK_TEST)
    NULL };
static const char *const js_obj_isfrozen_steps[] = {
    INTEG_STAGES(JS_STEP_STAGE_LABEL, "20.1.2.17 step 1 (a non-object O is already frozen)", INTEG_WALK_TEST)
    NULL };

static int js_integrity_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIntegrity *s = st;
    bool is_test = (s->hdr.arg & INTEG_TEST) != 0;
    bool is_freeze = (s->hdr.arg & INTEG_FREEZE) != 0;

    if (s->hdr.stage == IG_ENTRY) {
        JSValueConst arg0 = step_arg(&s->hdr, 0);
        JSObject *p;
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        s->obj = JS_UNDEFINED; s->keys = JS_UNDEFINED; s->result = JS_UNDEFINED;
        s->key = JS_ATOM_NULL;
        js_desc_facts_init(&s->facts);
        if (!JS_IsObject(arg0)) {
            /* each caller's step 1: a primitive is already sealed and frozen, and sealing one is a no-op. */
            s->result = is_test ? JS_TRUE : js_dup(arg0);
            return 0;
        }
        s->obj = js_dup(arg0);
        p = JS_VALUE_GET_OBJ(s->obj);
        if (!is_test) {
            if (p->class_id == JS_CLASS_MODULE_NS) {
                JS_ThrowTypeError(ctx, "cannot %s module namespace", is_freeze ? "freeze" : "seal");
                return -1;
            }
            /* DELETED: the freeze-only "cannot freeze resizable typed array" throw. It was this algorithm's
               own copy of 10.4.5.4 step 2, and it answered for FREEZE alone — so `Object.seal` and
               `Object.preventExtensions` on a length-tracking typed array silently succeeded, and
               `Reflect.preventExtensions` reported true. [[PreventExtensions]] refuses at its root now, and
               step 3 below turns that refusal into this algorithm's TypeError for every one of them.
               The OOB half went with it for the same reason: an out-of-bounds view is out of bounds because
               its buffer is resizable, which the root test already covers. */
        }
        s->hdr.stage = IG_OPEN_GOT;
        s->hdr.cb_coerce[0] = s->obj;   /* borrowed: the machine holds the receiver across every request */
        *out_cb = s->hdr.cb_coerce; *out_argc = 0;
        return is_test ? 19 : 20;       /* 7.3.16 step 1 IsExtensible / 7.3.15 step 1 PreventExtensions */
    }
    if (s->hdr.stage == IG_OPEN_GOT) {
        int ok;
        if (JS_IsException(cb_result)) return -1;
        ok = JS_ToBoolFree(ctx, cb_result); cb_result = JS_UNDEFINED;
        if (is_test && ok) {            /* step 2: an extensible object is at no integrity level */
            s->result = JS_FALSE;
            return 0;
        }
        if (!is_test && !ok) {
            /* 7.3.15 step 2 reported false, which is step 3 of each of Object.seal and Object.freeze — the
               THROW is the caller's step, not the abstract operation's. A Proxy trap is one way to get here and a
               length-tracking TypedArray is another, so the message names the operation rather than one of its
               causes — it said "proxy" and was then reached by an ordinary typed array. */
            JS_ThrowTypeError(ctx, "object refused to become non-extensible");
            return -1;
        }
        s->hdr.stage = IG_KEYS_GOT;     /* step 3: O.[[OwnPropertyKeys]]() — asked from the stage that collects it */
    }
    if (s->hdr.stage == IG_KEYS_GOT) {
        int kr = step_ownkeys_run(ctx, &s->hdr, s->obj, cb_result, &s->keys, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (kr) return kr < 0 ? -1 : kr;
        if (js_get_length32(ctx, &s->len, s->keys)) return -1;
        s->i = 0;
        s->hdr.stage = IG_NEXT;
    }
    for (;;) {
        if (s->hdr.stage == IG_NEXT) {
            JSValue kv;
            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
            JS_FreeAtom(ctx, s->key); s->key = JS_ATOM_NULL;
            if (s->i >= s->len) {
                s->result = is_test ? JS_TRUE : js_dup(s->obj);
                return 0;
            }
            /* the key list is an array this engine built and the invariant walk has already validated its
               elements are Strings and Symbols, so reading one runs nothing of the page's. */
            kv = JS_GetPropertyUint32(ctx, s->keys, s->i);
            if (JS_IsException(kv)) return -1;
            s->key = JS_ValueToAtom(ctx, kv);
            JS_FreeValue(ctx, kv);
            if (s->key == JS_ATOM_NULL) return -1;
            if (!is_test && !is_freeze) {
                /* step 4: the SEALED level defines {configurable:false} without reading the descriptor. */
                s->hdr.stage = IG_DEFINE_GOT;
                s->hdr.desc_get = JS_UNDEFINED; s->hdr.desc_set = JS_UNDEFINED;
                s->hdr.desc_flags = JS_PROP_HAS_CONFIGURABLE | JS_PROP_THROW | JS_PROP_DEFINE_PROPERTY;
                s->hdr.cb_coerce[0] = s->obj;
                s->hdr.cb_coerce[1] = JS_UNDEFINED;
                *out_cb = s->hdr.cb_coerce; *out_argc = (int)s->key;
                return 10;
            }
            js_desc_facts_free(ctx, &s->facts);
            s->hdr.stage = IG_DESC_GOT;   /* step 4/5's O.[[GetOwnProperty]](k), as a RECORD */
            s->hdr.desc_facts = &s->facts;
            s->hdr.cb_coerce[0] = s->obj;
            *out_cb = s->hdr.cb_coerce; *out_argc = (int)s->key;
            return 12;
        }
        if (s->hdr.stage == IG_DESC_GOT) {
            if (JS_IsException(cb_result)) return -1;
            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
            if (s->facts.flags < 0) {     /* the property went away: both algorithms skip it */
                s->i++;
                s->hdr.stage = IG_NEXT;
                continue;
            }
            if (is_test) {
                if ((s->facts.flags & JS_PROP_CONFIGURABLE)
                    || (is_freeze && !(s->facts.flags & JS_PROP_GETSET)
                        && (s->facts.flags & JS_PROP_WRITABLE))) {
                    s->result = JS_FALSE;   /* steps 4.b.i / 4.b.ii */
                    return 0;
                }
                s->i++;
                s->hdr.stage = IG_NEXT;
                continue;
            }
            /* step 5.b: an accessor keeps only {configurable:false}; a data property also loses [[Writable]]. */
            s->hdr.stage = IG_DEFINE_GOT;
            s->hdr.desc_get = JS_UNDEFINED; s->hdr.desc_set = JS_UNDEFINED;
            s->hdr.desc_flags = JS_PROP_HAS_CONFIGURABLE | JS_PROP_THROW | JS_PROP_DEFINE_PROPERTY;
            if (!(s->facts.flags & JS_PROP_GETSET))
                s->hdr.desc_flags |= JS_PROP_HAS_WRITABLE;
            s->hdr.cb_coerce[0] = s->obj;
            s->hdr.cb_coerce[1] = JS_UNDEFINED;
            *out_cb = s->hdr.cb_coerce; *out_argc = (int)s->key;
            return 10;
        }
        DCHECK(s->hdr.stage == IG_DEFINE_GOT, "the integrity-level machine resumed in no stage");
        if (JS_IsException(cb_result)) return -1;
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        s->i++;
        s->hdr.stage = IG_NEXT;
    }
}

/* WHAT A DESCRIPTOR RECORD OWNS. Nested in its machine's declaration, like the substitution cursor's: it is a
   sub-object whose three values are held across the request that produced them. */
static void js_desc_facts_visit(JSContext *ctx, JSDescFacts *f, JSStepVisit *v)
{
    v->val(ctx, &f->value);
    v->val(ctx, &f->getter);
    v->val(ctx, &f->setter);
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). Every key it walks is a [[GetOwnProperty]] the page can trap,
   so a fork lands mid-walk with its own key, its own descriptor record and its own answer. */
static void js_integrity_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSIntegrity *s = st;
    js_desc_facts_visit(ctx, &s->facts, v);
    v->atom(ctx, &s->key);
    v->val(ctx, &s->obj);
    v->val(ctx, &s->keys);
    v->val(ctx, &s->result);
}

static JSValue js_integrity_fini(JSContext *ctx, void *st, bool take_result)
{
    JSIntegrity *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* 7.3.15 SetIntegrityLevel over an object this ENGINE built, where every one of its four operations is
   ordinary and reaches no page code. The %ThrowTypeError% intrinsic is the only such caller, and it is frozen
   before any script exists. It is a separate primitive rather than the machine because a machine needs a flow to
   run on and there is none during intrinsic setup — and separate from the DELETED js_object_seal, whose whole
   point was that its four operations are the PAGE's. The DCHECK is what keeps the two apart: an exotic receiver
   here would mean a trap running from C during setup. */
static int js_freeze_ordinary(JSContext *ctx, JSValueConst obj)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    JSPropertyEnum *props;
    uint32_t len, i;
    int res = 0;
    DCHECK(p->class_id != JS_CLASS_PROXY && !JS_VALUE_GET_OBJ(obj)->is_exotic,
           "js_freeze_ordinary on an exotic object: its integrity level is the page's operations, not these");
    if (JS_PreventExtensions(ctx, obj) < 0)
        return -1;
    if (JS_GetOwnPropertyNamesInternal(ctx, &props, &len, p,
                                       JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK))
        return -1;
    for (i = 0; i < len && res == 0; i++) {
        if (JS_DefineProperty(ctx, obj, props[i].atom, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED,
                              JS_PROP_THROW | JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_WRITABLE) < 0)
            res = -1;
    }
    js_free_prop_enum(ctx, props, len);
    return res;
}

/* DELETED: js_object_fromEntries. Its body was already only a DFAIL; with the capability declared at the
   definition there is no function pointer left to name it, and js_call_c_function's JS_CFUNC_consume arm is the
   one backstop for a call site that was never routed. */

static JSValue js_object_is(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    return js_bool(js_same_value(ctx, argv[0], argv[1]));
}

/* DELETED: JS_SpeciesConstructor. 7.3.22's two reads are step_speciesctor_run, and %TypedArray%.prototype.slice
   was the last site still performing them from C. */

/* 20.1.3.8.1 "get Object.prototype.__proto__" / 20.1.3.8.2 "set Object.prototype.__proto__" — BOTH halves of
   Object.prototype.__proto__. These sites cited `B.2.2.1.1` and `B.2.2.1.2`, which the edition the editors
   maintain numbers nothing at all: the accessors are 20.1.3.8's, and Annex B's
   `B.2.2.1` is String.prototype.substr ( start, length ). Each half is one internal method on the receiver:
   the getter's `? O.[[GetPrototypeOf]]()` and the setter's `? O.[[SetPrototypeOf]](proto)`. On a Proxy those are
   the `getPrototypeOf` / `setPrototypeOf` traps plus 10.5.1 / 10.5.2's invariants — the page's code — and
   JS_GetPrototype / JS_SetPrototypeInternal ran them from C, so `proxy.__proto__` aborted at a looping trap's
   back-edge. ONE machine for both, `arg` naming which internal method, because the two differ only in that and in
   the operand. */
/* 22.2.6.4 `get RegExp.prototype.flags` performs a Get for each of the eight flag names, in a fixed order, on
   the receiver — and on a subclass or a Proxy every one of those is the page's code. js_regexp_get_flags did all
   eight with JS_GetPropertyStr from C, so `class R extends RegExp { get global() { for(;;){} } }` and
   `new Proxy(re, {get(){for(;;){}}})` preempted in an activation with no flow base. One stage per Get. */
typedef struct JSRegExpFlags {
    JSStepHdr hdr;        /* MUST be first: the generic step driver casts the state to JSStepHdr * */
    JSValue result;       /* DONE (owned) */
    char buf[8];          /* the flags collected so far, in spec order */
    int n;
} JSRegExpFlags;
static const char * const js_regexp_flag_names[8] = {
    "hasIndices", "global", "ignoreCase", "multiline", "dotAll", "unicode", "unicodeSets", "sticky",
};
static const char js_regexp_flag_chars[8] = { 'd', 'g', 'i', 'm', 's', 'u', 'v', 'y' };

/* 22.2.6.17 RegExp.prototype.toString: `? ToString(? Get(R, "source"))` then the same for "flags". Both Gets
   are accessors — the page's code on a subclass or a Proxy, and `flags` is now itself a step machine — and both
   ToStrings can be a user `toString`. js_regexp_toString did all four from C, which is what the step getter's
   backstop DFAIL named the moment `flags` stopped being a plain C body. */
/* 22.2.7.2 RegExpBuiltinExec. Its body is almost entirely the matcher and the construction of a result object
   out of objects it creates itself — none of which reaches page code — but THREE property accesses do, and
   js_regexp_exec performed all three from C: step 3's `Get(R, "lastIndex")` (an accessor if the page redefined
   it), step 4's ToLength on whatever that returned (a valueOf), and steps 12.a/15/18's `Set(R, "lastIndex", …)`
   (a setter). It ran as js_primargs_step's plain body, and that machine's contract is "coerce the arguments,
   then run a body that invokes nothing" — so exec needed its own.
   THE BYTECODE IS HELD, which the C body could not do: it kept `re_bytecode` in a local across JS_SetProperty,
   so a `lastIndex` setter calling `re.compile(…)` freed the matcher out from under the group-name walk that
   follows. The state owns a reference for exactly as long as the result is being built from it. */
typedef struct JSReStrIter {
    JSStepHdr hdr;        /* MUST be first: the generic step driver casts the state to JSStepHdr * */
    JSValue result;       /* DONE (owned) */
    JSValue match;        /* RegExpExec's result (owned) */
    JSValue matchStr;     /* `Get(match, 0)` and then its ToString (owned) */
    JSValue exec_buf[3];  /* the 3-slot CALL shape step_reexec_run lends to issue exec(S) */
    int64_t thisIndex;
} JSReStrIter;

typedef struct JSRegExpExec {
    JSStepHdr hdr;        /* MUST be first: the generic step driver casts the state to JSStepHdr * */
    JSValue result;       /* DONE (owned) */
    JSValue str_val;      /* ToString(S) (owned) */
    JSValue li_val;       /* Get(R,"lastIndex"), held across its ToLength (owned) */
    /* the matcher this run used, HELD so a re-compile mid-run cannot free it under the result build. A
       JSValue rather than a bare JSString*, because that is what makes it an ordinary owned reference the
       declaration below can name — the bare pointer was the one owned thing here that no visitor operation
       could express. UNDEFINED until stage 4 reads the regexp's slots. */
    JSValue bytecode;
    uint8_t **capture;    /* the match's capture buffer (owned, js_free) */
    /* its LENGTH, which is lre_get_alloc_count — capture_count*2 PLUS the executor's registers, not
       capture_count*2. Stated for the reason REExecContext::reach_size is: a match that suspends can be
       forked, and the fork has to copy this block. */
    int cap_alloc;
    REExecContext ec;     /* THE MATCH, by value: a pattern that backtracks catastrophically parks in here */
    uint8_t matching;     /* 1 = ec is live and owes an lre_exec_end */
    int64_t last_index;
    int rc;               /* the match's answer, held across the lastIndex write */
    int32_t set_val;      /* the value stage 5 writes, when it writes one */
    uint8_t set_pending;  /* 1 = steps 12.a/15/18 owe a Set(R,"lastIndex") before the result is built */
} JSRegExpExec;

typedef struct JSRegExpToString {
    JSStepHdr hdr;        /* MUST be first: the generic step driver casts the state to JSStepHdr * */
    JSValue result;       /* DONE (owned) */
    JSValue pattern;      /* Get(R,"source") and then its ToString (owned) */
    JSValue flags;        /* Get(R,"flags") and then its ToString (owned) */
} JSRegExpToString;

typedef struct JSProtoAccessor {
    JSStepHdr hdr;
    JSValue result;   /* DONE (owned) */
    JSValue obj;      /* ToObject(this) for the getter, the receiver itself for the setter (owned) */
} JSProtoAccessor;
enum { PROTOACC_GET = 0, PROTOACC_SET };
/* TWO algorithms, one machine: each is one internal method on the receiver, reached after tests that run none of
   the page's code. ONE stage list expanded once per algorithm; see AFIND_STAGES. */
#define PROTOACC_STAGES(X, ENTRY, OP) \
    X(PROTOACC_ENTRY, ENTRY) \
    X(PROTOACC_OP,    OP)
enum { PROTOACC_STAGES(JS_STEP_STAGE_ENUM, 0, 0) };
static const char *const js_proto_get_steps[] = {
    PROTOACC_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.3.8.1 step 1 (O is ToObject(this value))",
        "20.1.3.8.1 step 2 (O.[[GetPrototypeOf]]())")
    NULL };
static const char *const js_proto_set_steps[] = {
    PROTOACC_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.3.8.2 steps 1-4 (RequireObjectCoercible(this value); proto is an Object or null; O is an Object)",
        "20.1.3.8.2 step 5 (status is O.[[SetPrototypeOf]](proto))")
    NULL };

static int js_proto_accessor_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSProtoAccessor *s = st;
    bool is_set = s->hdr.def->arg == PROTOACC_SET;

    if (s->hdr.stage == PROTOACC_ENTRY) {
        JS_FreeValue(ctx, cb_result);
        s->result = JS_UNDEFINED;
        s->obj = JS_UNDEFINED;
        if (is_set) {
            /* 20.1.3.8.2 steps 1-4: RequireObjectCoercible, then a non-object non-null VALUE is a no-op and so is a
               non-object receiver — both decided BEFORE any internal method runs. */
            JSValueConst proto = step_arg(&s->hdr, 0);
            if (JS_IsUndefined(s->hdr.this_val) || JS_IsNull(s->hdr.this_val)) {
                JS_ThrowTypeErrorNotAnObject(ctx);
                return -1;
            }
            if (!JS_IsObject(proto) && !JS_IsNull(proto))
                return 0;
            if (!JS_IsObject(s->hdr.this_val))
                return 0;
            s->obj = js_dup(s->hdr.this_val);
            s->hdr.stage = PROTOACC_OP;
            s->hdr.cb_coerce[0] = s->obj;              /* borrowed: the state holds it across the request */
            s->hdr.cb_coerce[1] = (JSValue)proto;
            *out_cb = s->hdr.cb_coerce; *out_argc = 0;
            return 21;                                 /* SETPROTO */
        }
        s->obj = JS_ToObject(ctx, s->hdr.this_val);     /* 20.1.3.8.1 step 1 */
        if (JS_IsException(s->obj)) { s->obj = JS_UNDEFINED; return -1; }
        s->hdr.stage = PROTOACC_OP;
        s->hdr.cb_coerce[0] = s->obj;
        *out_cb = s->hdr.cb_coerce; *out_argc = 0;
        return 18;                                     /* GETPROTO */
    }
    DCHECK(s->hdr.stage == PROTOACC_OP, "the __proto__ accessor's machine resumed in no stage");
    if (JS_IsException(cb_result)) return -1;
    if (is_set) {
        /* 20.1.3.8.2 step 6: a FALSE status from step 5 is a TypeError, which is the accessor's own step and not
           the internal method's — Reflect.setPrototypeOf yields the same boolean instead. Discarding it made
           `__proto__ = cycle` and a non-extensible receiver silently succeed, which set-cycle.js and
           set-non-extensible.js caught. */
        bool ok = JS_ToBool(ctx, cb_result);
        JS_FreeValue(ctx, cb_result);
        if (!ok) {
            JS_ThrowTypeError(ctx, "not a valid prototype");
            return -1;
        }
        s->result = JS_UNDEFINED;
    } else {
        s->result = cb_result;
    }
    return 0;
}
/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_proto_accessor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSProtoAccessor *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->obj);
}

static JSValue js_proto_accessor_fini(JSContext *ctx, void *st, bool take_result)
{
    JSProtoAccessor *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* DELETED: js_object_set___proto__'s C body — it is the machine above, PROTOACC_SET. */

/* 20.1.3.3 Object.prototype.isPrototypeOf(V) — a WALK of `? V.[[GetPrototypeOf]]()`, which on a Proxy is the
   `getPrototypeOf` trap plus 10.5.1's invariant: the page's code once per link, and the C body ran the whole loop
   with JS_GetPrototypeFree. It also carried a js_poll_interrupts "avoid infinite loop (possible with proxies)"
   guard, which is a BOUND — a proxy chain that keeps answering is unbounded work the scheduler is supposed to
   preempt and page, not something a builtin cuts short. Routing the read removes the reason the guard existed: the
   loop now suspends at every link like any other flow. */
typedef struct JSProtoChain {
    JSStepHdr hdr;
    JSValue result;   /* DONE (owned) */
    JSValue obj;      /* ToObject(this) (owned) */
    JSValue cur;      /* the link being walked (owned) */
} JSProtoChain;
/* THE THREE COMPLETIONS OF ONE STEP OF A PROTOTYPE-CHAIN WALK, over a link that is unknown external input.
   ONE numbering for BOTH walks, because 20.1.3.3 step 3 and 7.3.21 step 6 are literally the same three-step
   loop: advance to the next link, return false if it is null, return true if SameValue says it is the object
   we are looking for. Two copies of this would be two chances to number it differently.
   EVERY ONE OF THE THREE IS A COMPLETION OF THE ITERATION, which is the part that is easy to get wrong: the
   question is not "does the walk stop?" — that names the false arm and the true arm at once and then cannot
   say which, and asked as a BOOLEAN it deletes the world in which the operator is TRUE. Enumerate what the
   page can observe (false, true, not yet), never the branches of the prose.
   ZERO ENDS THE WALK AND CLAIMS NOTHING, and that is forced rather than chosen. The driver takes outcome 0
   when no forking policy answers — the @S candidate re-fire — so a numbering whose 0 were MORE would walk an
   unknown chain for ever in exactly the run that has no scheduler to park it; and of the two that do end it,
   the false arm asserts nothing about the value while the true arm asserts a relationship.
   WHAT EACH SITE MUST NOT SHARE IS THE OPERATION NAME. The constraint key is (operand, operation, completion),
   and these two walks ask DIFFERENT questions about the same link — one compares it against the constructor's
   `prototype`, the other against the receiver — so a shared op string would let a flow that answered one
   decide the other. */
#define PROTO_LINK_END   0   /* the chain ends at this link: the operator is false */
#define PROTO_LINK_MATCH 1   /* this link IS the object being looked for: the operator is true */
#define PROTO_LINK_MORE  2   /* neither: ask this link for ITS [[GetPrototypeOf]] */

/* The walk rests at step 3.a once per link; steps 3.b and 3.c decide from what that link answered and run none
   of the page's code. */
#define PROTOCHAIN_STAGES(X) \
    X(PROTOCHAIN_ENTRY, "20.1.3.3 steps 1-2 (a non-object V is false; O is ToObject(this value))") \
    X(PROTOCHAIN_LINK,  "20.1.3.3 step 3.a (V is V.[[GetPrototypeOf]]())")
enum { PROTOCHAIN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_proto_chain_steps[] = { PROTOCHAIN_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_proto_chain_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSProtoChain *s = st;

    if (s->hdr.stage == PROTOCHAIN_ENTRY) {
        JSValueConst v = step_arg(&s->hdr, 0);
        JS_FreeValue(ctx, cb_result);
        s->result = JS_UNDEFINED;
        s->obj = JS_UNDEFINED;
        s->cur = JS_UNDEFINED;
        if (!JS_IsObject(v)) { s->result = JS_FALSE; return 0; }      /* step 1 */
        s->obj = JS_ToObject(ctx, s->hdr.this_val);                   /* step 2 */
        if (JS_IsException(s->obj)) { s->obj = JS_UNDEFINED; return -1; }
        s->cur = js_dup(v);
        s->hdr.stage = PROTOCHAIN_LINK;
        goto ask;
    }
    DCHECK(s->hdr.stage == PROTOCHAIN_LINK, "isPrototypeOf's walk resumed in no stage");
    /* THREE WAYS TO ARRIVE HERE AND ONLY ONE OF THEM CARRIES A LINK — step 3.a's request answering; this
       stage's own fork being ANSWERED; and the SIBLING that fork minted, resuming at the ask. The last two
       carry the driver's filler, and taking it as the link would overwrite the very operand the fork was
       asked about. step_fork_pending is what tells them apart and `fork_phase` is NOT — see its definition.
       This arm read the sibling as a link, and since JS_UNDEFINED is neither concolic nor null and its
       [[GetPrototypeOf]] is null, the walk answered `false` for a chain it had not walked: the SILENT half of
       the defect the enumeration walk aborts on. See the instanceof walk, which is the same algorithm. */
    if (!step_fork_pending(&s->hdr)) {
        if (JS_IsException(cb_result)) return -1;
        step_adopt_link(ctx, &s->cur, &cb_result);                    /* step 3.a's answer */
        STEP_CHECK_PROTO_LINK(s->cur);
    }
    /* STEPS 3.b AND 3.c OVER AN UNKNOWN LINK ARE NOT DECIDABLE IN C. Both are identity comparisons and an
       unknown rides an ordinary Object, so both tests below would silently take the keep-walking arm at every
       link and this walk would never terminate — which is what it did the moment the solver's value class
       stopped aborting in its [[GetPrototypeOf]] and started answering a derivation. The operand is the LINK
       and never its depth, and it lives in `s->cur` because step_fork_run borrows it across the request and
       the sibling's snapshot is taken at the ask, so it has to be reachable from js_proto_chain_visit. */
    if (js_value_is_concolic(s->cur)) {
        int arm, r;
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* `real` UNSTATED: the derived link carries no example, so this machine has no ground to say which
           completion a real session reaches. */
        r = step_fork_run(ctx, &s->hdr, s->cur, "isPrototypeOf step 3", 3, JS_OUTCOME_REAL_UNSTATED, &arm);
        if (r) return r;
        if (arm == PROTO_LINK_END) { s->result = JS_FALSE; return 0; }    /* step 3.b */
        if (arm == PROTO_LINK_MATCH) { s->result = JS_TRUE; return 0; }   /* step 3.c */
        goto ask;                                                         /* PROTO_LINK_MORE: step 3.a again */
    }
    if (JS_IsNull(s->cur)) { s->result = JS_FALSE; return 0; }         /* step 3.b */
    if (JS_VALUE_GET_TAG(s->cur) == JS_TAG_OBJECT
        && JS_VALUE_GET_OBJ(s->obj) == JS_VALUE_GET_OBJ(s->cur)) {
        s->result = JS_TRUE;                                          /* step 3.c */
        return 0;
    }
ask:
    s->hdr.cb_coerce[0] = s->cur;   /* borrowed: the state holds the link across the request */
    *out_cb = s->hdr.cb_coerce; *out_argc = 0;
    return 18;                      /* GETPROTO */
}
/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). Each [[GetPrototypeOf]] along the chain is a trap the page can define. */
static void js_proto_chain_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSProtoChain *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->obj);
    v->val(ctx, &s->cur);
}

static JSValue js_proto_chain_fini(JSContext *ctx, void *st, bool take_result)
{
    JSProtoChain *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* DELETED: js_object_propertyIsEnumerable's C body. 20.1.3.4 is ToPropertyKey (the page's code), ToObject, then
   `? O.[[GetOwnProperty]](P)` (a `getOwnPropertyDescriptor` trap on a Proxy) — and it ran the first from C with
   JS_ValueToAtom, the third from C with JS_GetOwnPropertyFlagsInternal, and the two in the WRONG ORDER besides.
   It is STEPDEF_PROP_IS_ENUM, the same machine Iterator.zipKeyed's own-enumerable probe uses. */

/* 20.1.3.9.3 Object.prototype.__lookupGetter__ ( key ) / 20.1.3.9.4 Object.prototype.__lookupSetter__ ( key ):
   ToObject, ToPropertyKey, then walk the prototype chain asking each link for its own descriptor until one
   answers. THREE of the page's operations, and the C body ran all three — ToPropertyKey as a bare
   JS_ValueToAtom, and both [[GetOwnProperty]] and [[GetPrototypeOf]] per link, which on a Proxy are its traps
   with no flow base. The walk is the shape for-in's slow path takes, and the descriptor it needs is the
   RECORD, because the answer IS propertyDesc.[[Getter]] or propertyDesc.[[Setter]].
   ONE machine: the two forms differ in which accessor they return, which is the arg.
   THESE SITES CITED B.2.2.4 AND B.2.2.5, WHICH IS NOT A RENUMBERING A READER CAN SURVIVE: in the edition the
   editors maintain those two numbers are String.prototype.blink ( ) and String.prototype.bold ( ), so the old
   citation resolves, reads as authoritative, and sends the reader to a clause about markup. A citation is owed
   to the maintained document, and the TITLE beside the number is what makes a move like this visible instead
   of silent — which is why every label below carries one. */
typedef struct JSLookupAcc {
    JSStepHdr hdr;      /* MUST be first. arg: 0 = __lookupGetter__, 1 = __lookupSetter__ */
    JSValue cur;        /* the chain link being examined (owned) */
    JSAtom key;         /* ToPropertyKey's result (owned) */
    JSValue result;     /* DONE (owned) */
    JSDescFacts facts;  /* this link's own descriptor (owned) */
} JSLookupAcc;
/* TWO algorithms, one machine, differing only in which half of an accessor step 3.b.i yields. THE STAGE
   CONSTANTS AND THE LABELS ARE ONE LIST — this machine is why that rule is not a style preference: it carried
   `enum { LA_KEY = 0, LA_DESC, LA_DESC_GOT, LA_PROTO, LA_PROTO_GOT }` while its body switched on the literals 1
   to 5, so every name in that enum was off by one from the stage it claimed to be and nothing could see it.
   ONE stage list expanded once per algorithm; see AFIND_STAGES. */
#define LOOKUPACC_STAGES(X, KEY, DESC, PROTO) \
    X(LA_KEY,   KEY)  \
    X(LA_DESC,  DESC) \
    X(LA_PROTO, PROTO)
enum { LOOKUPACC_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0) };
static const char *const js_lookupgetter_steps[] = {
    LOOKUPACC_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.3.9.3 steps 1-2 (obj is ToObject(this value); propertyKey is ToPropertyKey(key))",
        "20.1.3.9.3 step 3.a (propertyDesc is obj.[[GetOwnProperty]](propertyKey))",
        "20.1.3.9.3 step 3.c (obj is obj.[[GetPrototypeOf]]())")
    NULL };
static const char *const js_lookupsetter_steps[] = {
    LOOKUPACC_STAGES(JS_STEP_STAGE_LABEL,
        "20.1.3.9.4 steps 1-2 (obj is ToObject(this value); propertyKey is ToPropertyKey(key))",
        "20.1.3.9.4 step 3.a (propertyDesc is obj.[[GetOwnProperty]](propertyKey))",
        "20.1.3.9.4 step 3.c (obj is obj.[[GetPrototypeOf]]())")
    NULL };

static int js_lookup_acc_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSLookupAcc *s = st;
    int r;

    if (s->hdr.stage == LA_KEY) {
        if (s->hdr.str_phase == STR_PH_START) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            s->cur = JS_UNDEFINED; s->key = JS_ATOM_NULL; s->result = JS_UNDEFINED;
            js_desc_facts_init(&s->facts);
            s->cur = JS_ToObject(ctx, s->hdr.this_val);   /* step 1, before the key: it throws for nullish */
            if (JS_IsException(s->cur)) { s->cur = JS_UNDEFINED; return -1; }
        }
        /* step 2's ToPropertyKey, which runs a key object's toString */
        r = step_topropkey_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->key, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r != 0) return r;
        goto ask_desc;
    }
    if (s->hdr.stage == LA_DESC) {
        if (JS_IsException(cb_result)) return -1;
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        if (s->facts.flags >= 0) {
            /* step 3.b: found. An ACCESSOR yields the half this form asks for; anything else yields
               undefined, and either way the walk stops here.
               NAMED RESIDUAL — CORRECT, AND NARROWER THAN 20.1.3.9.3 OVER AN UNKNOWN CHAIN.
               NOT COVERED: a link that is unknown external input and carries no EXAMPLE. Its [[GetOwnProperty]]
               is answered from the example's own slots, so for a link with no example and no materialised
               member it reports absent for EVERY name, with no question asked anywhere — and this arm cannot
               fire on such a link however many the step-3.d fork below explores. It fires normally for a link
               that carries an example, for a slot the page defined on one, and for a member the enumeration
               materialised; the narrowing is the pristine derived link and only that.
               NEXT DIFF: a per-NAME own-member question on the record — the named sibling of the own-key-set
               predicate the enumeration already asks (JSConcolicHooks.own_keys_pred, whose true arm
               own_key_mint performs). The table today carries no per-name form; it would be minted by the HOST
               over the record and the name so both readers spell one key, asked HERE, where a step machine has
               a flow base and the internal method has none, and read back by the record's own
               [[GetOwnProperty]] so one question keeps one answer.
               HOW ITS ABSENCE SHOWS: `'k' in p` answers true for every name on such a link, because
               [[HasProperty]] is the derived-unknown door and answers a name the page wrote — while
               `Object.getOwnPropertyDescriptor(p, 'k')` and `p.__lookupGetter__('k')` answer undefined for
               every name. Two internal methods disagreeing about one record, with only the second's answer
               decided by nothing the run observed. */
            if (s->facts.flags & JS_PROP_GETSET)
                s->result = js_dup(s->hdr.arg ? s->facts.setter : s->facts.getter);
            else
                s->result = JS_UNDEFINED;
            return 0;
        }
        /* step 3.c: O.[[GetPrototypeOf]](), which on a Proxy is its trap */
        s->hdr.stage = LA_PROTO;
        s->hdr.cb_coerce[0] = s->cur;
        *out_cb = s->hdr.cb_coerce; *out_argc = 0;
        return 18;
    }
    DCHECK(s->hdr.stage == LA_PROTO, "__lookupGetter__'s walk resumed in no stage");
    /* THREE WAYS TO ARRIVE HERE AND ONLY ONE OF THEM CARRIES A LINK — step 3.c's request answering, whose
       result IS the link; this stage's own fork being ANSWERED; and the SIBLING that fork minted, resuming at
       the ask. The last two carry the driver's filler, and taking it as the link would overwrite the operand
       the fork was asked about. step_fork_pending is what tells them apart and `fork_phase` is NOT — see its
       definition. This arm read the sibling as a link and carried JS_UNDEFINED into step 3.a's
       [[GetOwnProperty]] request, whose trapless arm takes JS_VALUE_GET_OBJ of its operand: not a wrong
       answer but a NULL dereference, and the worst of the four consequences one predicate had. */
    if (!step_fork_pending(&s->hdr)) {
        if (JS_IsException(cb_result)) return -1;
        step_adopt_link(ctx, &s->cur, &cb_result);
        STEP_CHECK_PROTO_LINK(s->cur);
    }
    /* STEP 3.d OVER AN UNKNOWN LINK IS NOT DECIDABLE IN C. `If obj is null` is this Repeat's only stopping
       test that the walk itself owns, and an unknown rides an ordinary Object, so the C test below would take
       the keep-walking arm at every link and this Repeat would never terminate.
       TWO COMPLETIONS, COUNTED FROM WHAT THE PAGE OBSERVES. The chain ends at this link and the method answers
       undefined, or there is another link and step 3.a is asked of it. There is no third: step 3.b's outcome
       is not this ask's to declare — it is decided one internal method down, by the link's own
       [[GetOwnProperty]], and a walk that answered it here would file a second answer to that question under
       a key the record's own method cannot spell.
       BOTH FORMS SHARE THE OPERATION STRING, WHICH IS THE OPPOSITE OF PROTO_LINK_END's WARNING AND FOR ITS OWN
       REASON: that note keeps two walks apart because they ask DIFFERENT things about one link. These two ask
       the SAME thing — whether the chain ends here — and the answer cannot depend on which half of an accessor
       the caller wanted, so one key is correct and two would be one world recorded twice.
       THE CRASH THAT STOOD HERE was right that both exits of this loop are unreachable over a pristine derived
       chain, and its remedy clause named the three-way numbering, which would have declared a MATCH arm whose
       value this machine cannot produce without fabricating a descriptor. The reachability half is kept as a
       NAMED RESIDUAL at step 3.b above, where the unreachable arm actually is. */
    if (js_value_is_concolic(s->cur)) {
        int arm;
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* `real` UNSTATED: the derived link carries no example, so nothing observed says which completion a
           real session reaches. */
        r = step_fork_run(ctx, &s->hdr, s->cur, "legacy accessor lookup step 3.d", 2,
                          JS_OUTCOME_REAL_UNSTATED, &arm);
        if (r) return r;
        if (arm == PROTO_CHAIN_ENDS) {   /* step 3.d */
            s->result = JS_UNDEFINED;
            return 0;
        }
        goto ask_desc;                   /* PROTO_CHAIN_MORE: step 3.a on the link that arrived */
    }
    if (JS_IsNull(s->cur)) {   /* step 3.d: the chain ended */
        s->result = JS_UNDEFINED;
        return 0;
    }
ask_desc:
    /* step 3.a: O.[[GetOwnProperty]](key), answered as a RECORD. Reached from the key coercion and from each
       link the walk moves to; a stage of its own would rest at no step, since it is entered and left inside one
       step() call. */
    js_desc_facts_free(ctx, &s->facts);   /* the previous link's, before this link answers */
    s->hdr.stage = LA_DESC;
    s->hdr.desc_facts = &s->facts;
    s->hdr.cb_coerce[0] = s->cur;   /* borrowed: the machine holds the link across the request */
    *out_cb = s->hdr.cb_coerce; *out_argc = (int)s->key;
    return 12;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). __lookupGetter__ walks the chain, and each link's own descriptor is a record held across the read that produced it. */
static void js_lookup_acc_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSLookupAcc *s = st;
    js_desc_facts_visit(ctx, &s->facts, v);
    v->val(ctx, &s->result);
    v->val(ctx, &s->cur);
    v->atom(ctx, &s->key);
}

static JSValue js_lookup_acc_fini(JSContext *ctx, void *st, bool take_result)
{
    JSLookupAcc *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static const JSCFunctionListEntry js_object_funcs[] = {
    JS_CFUNC_STEP_DEF("create", 2, STEPDEF_OBJ_CREATE ),
    JS_CFUNC_STEP_DEF("getPrototypeOf", 1, STEPDEF_OBJ_GETPROTO ),
    JS_CFUNC_STEP_DEF("setPrototypeOf", 2, STEPDEF_OBJ_SETPROTO ),
    JS_CFUNC_STEP_DEF("defineProperty", 3, STEPDEF_OBJ_DEFINEPROPERTY ),
    JS_CFUNC_STEP_DEF("defineProperties", 2, STEPDEF_OBJ_DEFINEPROPERTIES ),
    JS_CFUNC_STEP_DEF("getOwnPropertyNames", 1, STEPDEF_OWNKEYS_NAMES ),
    JS_CFUNC_STEP_DEF("getOwnPropertySymbols", 1, STEPDEF_OWNKEYS_SYMBOLS ),
    JS_CFUNC_CONSUME_DEF("groupBy", 2, ITERCONS_GROUPBY_BASE + GROUPBY_PROPERTY ),
    JS_CFUNC_STEP_DEF("keys", 1, STEPDEF_OBJ_KEYS ),
    JS_CFUNC_STEP_DEF("values", 1, STEPDEF_OBJ_VALUES ),
    JS_CFUNC_STEP_DEF("entries", 1, STEPDEF_OBJ_ENTRIES ),
    JS_CFUNC_STEP_DEF("isExtensible", 1, STEPDEF_OBJ_ISEXT ),
    JS_CFUNC_STEP_DEF("preventExtensions", 1, STEPDEF_OBJ_PREVEXT ),
    JS_CFUNC_STEP_DEF("getOwnPropertyDescriptor", 2, STEPDEF_OBJ_GOPD ),
    JS_CFUNC_STEP_DEF("getOwnPropertyDescriptors", 1, STEPDEF_OBJ_DESCS ),
    JS_CFUNC_DEF("is", 2, js_object_is ),
    JS_CFUNC_STEP_DEF("assign", 2, STEPDEF_OBJ_ASSIGN ),
    JS_CFUNC_STEP_DEF("seal", 1, STEPDEF_OBJ_SEAL ),
    JS_CFUNC_STEP_DEF("freeze", 1, STEPDEF_OBJ_FREEZE ),
    JS_CFUNC_STEP_DEF("isSealed", 1, STEPDEF_OBJ_ISSEALED ),
    JS_CFUNC_STEP_DEF("isFrozen", 1, STEPDEF_OBJ_ISFROZEN ),
    JS_CFUNC_CONSUME_DEF("fromEntries", 1, ITERCONS_OBJENTRIES ),
    JS_CFUNC_STEP_DEF("hasOwn", 2, STEPDEF_OBJ_HASOWN ),
};

static const JSCFunctionListEntry js_object_proto_funcs[] = {
    JS_CFUNC_STEP_DEF("toString", 0, STEPDEF_OBJ_TOSTRING ),
    JS_CFUNC_STEP_DEF("toLocaleString", 0, STEPDEF_OBJ_TOLOCALESTRING ),
    JS_CFUNC_DEF("valueOf", 0, js_object_valueOf ),
    JS_CFUNC_STEP_DEF("hasOwnProperty", 1, STEPDEF_OBJ_HASOWNPROP ),
    JS_CFUNC_STEP_DEF("isPrototypeOf", 1, STEPDEF_PROTO_CHAIN ),
    JS_CFUNC_STEP_DEF("propertyIsEnumerable", 1, STEPDEF_PROP_IS_ENUM ),
    /* 20.1.3.8 "Object.prototype.__proto__" (this said `B.2.2.1`, which is String.prototype.substr): BOTH
       halves are step machines, because each performs ONE internal method on the receiver and on a Proxy that
       is a trap. */
    JS_CGETSET_STEP_BOTH_DEF("__proto__", STEPDEF_PROTO_GET, STEPDEF_PROTO_SET ),
    JS_CFUNC_STEP_DEF("__defineGetter__", 2, STEPDEF_OBJ_DEFGETTER ),
    JS_CFUNC_STEP_DEF("__defineSetter__", 2, STEPDEF_OBJ_DEFSETTER ),
    JS_CFUNC_STEP_DEF("__lookupGetter__", 1, STEPDEF_OBJ_LOOKUPGETTER ),
    JS_CFUNC_STEP_DEF("__lookupSetter__", 1, STEPDEF_OBJ_LOOKUPSETTER ),
};

