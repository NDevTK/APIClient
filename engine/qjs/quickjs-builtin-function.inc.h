/* Function class */

static JSValue js_function_proto(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return JS_UNDEFINED;
}

/* §20.2.1.1.1 CreateDynamicFunction step 15's sourceString — "the string-concatenation of prefix,
   " anonymous(", paramString, 0x000A (LINE FEED), ") {", bodyParseString, and "}"", with step 14's
   bodyParseString being the body between two LINE FEEDs. Assembled from arguments that are already STRINGS, so
   it runs nothing and cannot fail except on allocation. (It was cited as step 18, which is the parameter
   parse's throw; the number was checked against the spec text rather than recalled.)
     THE ONE ADDITION IS THE PARENTHESES, and js_parse_fn_ctor_source is where that is argued: step 23 parses
   sourceText as a FunctionExpression, this engine needs the VALUE the expression evaluates to, and a
   parenthesised FunctionExpression pinned to end-of-input admits exactly the same texts.
     DYNSRC_PARAMS and DYNSRC_BODY are the same assembly with the OTHER half empty — steps 17-20's "each is
   valid alone" — and they are exact for the same reason: with the tail pinned, a half that closes the wrapper
   has nowhere to put what follows. */
enum { DYNSRC_ALL, DYNSRC_PARAMS, DYNSRC_BODY };

/* `*acc` and `piece` (consumed), both Strings or unknowns, joined as §6.1.4's string-concatenation. An unknown
   piece makes the whole unknown through the + hook's concatenation arm, as String.prototype.concat's join does:
   the shape is joined, the source kept and the example concatenated. */
static int js_string_cat_piece(JSContext *ctx, JSValue *acc, JSValue piece)
{
    JSValue pair[2];

    if (!js_value_is_concolic(*acc) && !js_value_is_concolic(piece)) {
        *acc = JS_ConcatString(ctx, *acc, piece);
        return JS_IsException(*acc) ? -1 : 0;
    }
    pair[0] = *acc;
    pair[1] = piece;
    if (!g_concolic.add(ctx, pair + 2, JS_CONCOLIC_ADD_CONCAT))
        DFAIL("the concolic + hook declined an unknown piece of a string-concatenation");
    *acc = pair[0];
    return 0;
}

/* §20.2.1.1.1 steps 12-16 over pieces of which one or more are unknown: sourceString is still the
   concatenation the steps state, so the code-execution sink is handed the unknown it is. */
static JSValue js_dynfunc_source_unknown(JSContext *ctx, JSFunctionKindEnum func_kind, JSValue *strs, int n,
                                         int mode)
{
    JSValue acc;
    char head[32];
    int i;

    snprintf(head, sizeof head, "(%sfunction%s anonymous(",
             func_kind == JS_FUNC_ASYNC || func_kind == JS_FUNC_ASYNC_GENERATOR ? "async " : "",
             func_kind == JS_FUNC_GENERATOR || func_kind == JS_FUNC_ASYNC_GENERATOR ? "*" : "");
    acc = JS_NewString(ctx, head);
    if (JS_IsException(acc))
        return acc;
    for (i = 0; mode != DYNSRC_BODY && i < n - 1; i++) {
        if ((i != 0 && js_string_cat_piece(ctx, &acc, JS_NewString(ctx, ","))) ||
            js_string_cat_piece(ctx, &acc, js_dup(strs[i])))
            return JS_EXCEPTION;
    }
    if (js_string_cat_piece(ctx, &acc, JS_NewString(ctx, "\n) {\n")))
        return JS_EXCEPTION;
    if (mode != DYNSRC_PARAMS && n > 0 && js_string_cat_piece(ctx, &acc, js_dup(strs[n - 1])))
        return JS_EXCEPTION;
    if (js_string_cat_piece(ctx, &acc, JS_NewString(ctx, "\n})")))
        return JS_EXCEPTION;
    return acc;
}

static JSValue js_dynfunc_source(JSContext *ctx, JSFunctionKindEnum func_kind, JSValue *strs, int n, int mode)
{
    StringBuffer b_s, *b = &b_s;
    int i;

    for (i = 0; i < n; i++)
        if (js_value_is_concolic(strs[i]))
            return js_dynfunc_source_unknown(ctx, func_kind, strs, n, mode);
    string_buffer_init(ctx, b, 0);
    string_buffer_putc8(b, '(');
    if (func_kind == JS_FUNC_ASYNC || func_kind == JS_FUNC_ASYNC_GENERATOR)
        string_buffer_puts8(b, "async ");
    string_buffer_puts8(b, "function");
    if (func_kind == JS_FUNC_GENERATOR || func_kind == JS_FUNC_ASYNC_GENERATOR)
        string_buffer_putc8(b, '*');
    string_buffer_puts8(b, " anonymous(");
    if (mode != DYNSRC_BODY) {
        for (i = 0; i < n - 1; i++) {
            if (i != 0)
                string_buffer_putc8(b, ',');
            if (string_buffer_concat_value(b, strs[i]))
                goto fail;
        }
    }
    string_buffer_puts8(b, "\n) {\n");
    if (mode != DYNSRC_PARAMS && n > 0 && string_buffer_concat_value(b, strs[n - 1]))
        goto fail;
    string_buffer_puts8(b, "\n})");
    return string_buffer_end(b);
 fail:
    string_buffer_free(b);
    return JS_EXCEPTION;
}

/* §20.2.1.1.1 CreateDynamicFunction steps 17-20: `parameters` and `body` are ALSO parsed on their OWN — step
   21's NOTE says it is "to ensure that each is valid alone" — and that is the whole of what rejects a
   `new Function` whose two arguments are the two halves of one block comment, which parse only once
   concatenated. (These used to be cited as steps 20-23, which are the whole-source parse's throw and the two
   NOTEs before it; the numbers were checked against the spec text rather than recalled.)
     THESE PROBES ARE NOT WHAT REJECTS AN ESCAPE, and reading them as if they were is how one lived here: a
   probe is the whole wrapper with the other half empty, so a half spelled `}); f(); ({` closes the wrapper and
   satisfies the probe exactly as it satisfied the whole-source parse. What rejects it is
   js_parse_fn_ctor_source's end-of-input, and that is also what makes these two probes EXACT — a half that
   parses inside the pinned wrapper is a half that parses alone under its own goal symbol. Assembling each probe
   from the SAME builder is what gives the parameter list the right goal for the kind (a generator's [+Yield],
   an async function's [+Await]) with no second spelling of that mapping. Parsing runs none of the page's code,
   so this needs no trampoline — and it announces nothing at the @S seam, because §20.2.1.1.1 performs
   §19.2.1.2 HostEnsureCanCompileStrings ONCE, at step 11, and not again for these probes. JS_EVAL_FLAG_COMPILE_ONLY
   is what says so to js_eval_is_string_to_code_sink. */
static int js_dynfunc_check_halves(JSContext *ctx, JSFunctionKindEnum func_kind, JSValue *strs, int n)
{
    int mode;

    for (mode = DYNSRC_PARAMS; mode <= DYNSRC_BODY; mode++) {
        JSValue src, fn;
        const char *cstr;
        size_t len;
        src = js_dynfunc_source(ctx, func_kind, strs, n, mode);
        if (JS_IsException(src))
            return -1;
        if (js_value_is_concolic(src)) {
            /* a half over unknown input parses as its example; with none its text is nobody's to parse */
            JSValue ex = g_concolic.example(ctx, src);

            JS_FreeValue(ctx, src);
            if (!JS_IsString(ex)) {
                JS_FreeValue(ctx, ex);
                continue;
            }
            src = ex;
        }
        cstr = JS_ToCStringLen(ctx, &len, src);
        JS_FreeValue(ctx, src);
        if (!cstr)
            return -1;
        fn = JS_EvalInternal(ctx, ctx->global_obj, cstr, len, JS_EVAL_SCRIPT_NAME, 1,
                             JS_EVAL_TYPE_INDIRECT | JS_EVAL_FLAG_COMPILE_ONLY |
                             JS_EVAL_FLAG_FUNCTION_CTOR, -1, NULL);
        JS_FreeCString(ctx, cstr);
        if (JS_IsException(fn))
            return -1;
        JS_FreeValue(ctx, fn);
    }
    return 0;
}

/* new Function(a, b, body) and the generator/async variants — 20.2.1.1.1 CreateDynamicFunction. THREE of its steps
   are the page's code and all three ran from C. The ToStrings (steps 8 and 10) were a coerce-then-compute
   declaration, which was already wrong for this builtin: that declaration asserts the body has no user code LEFT
   once its arguments are primitive, and this body then evaluated the synthesized source — a BYTECODE BODY, run by
   C recursion below a live flow with no way to park — and finished with GetPrototypeFromConstructor's
   `Get(newTarget, "prototype")` (step 25), a [[Get]] a Proxy new.target traps. Each of the three is a request
   here, and the parse-and-create is the shared program sub-sequence indirect eval also performs. */
typedef struct JSDynFunc {
    JSStepHdr hdr;
    JSValue result;
    JSValue func;    /* the created function, held across the `prototype` read */
    JSValue *strs;   /* the ToString'd arguments, in argument order */
    int nstrs;
} JSDynFunc;

/* ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). All four
   constructors run 20.2.1.1.1 with a different `kind`, so the STEPS are the same list and only the
   `algorithm` each definition names differs. */
#define DYNFUNC_STAGES(X) \
    X(DYNF_ENTRY,  "20.2.1.1.1 steps 1-7 (newTarget; the kind's prefix and parse symbols; argCount; " \
                   "parameterStrings is a new empty List)") \
    X(DYNF_STRS,   "20.2.1.1.1 steps 8-9 (each element of parameterArgs is ToString'd, then bodyString is " \
                   "ToString(bodyArg))") \
    X(DYNF_PARSE,  "20.2.1.1.1 steps 12-28 (P; bodyParseString; sourceString; the ParseText calls and their " \
                   "SyntaxErrors; F is OrdinaryFunctionCreate)") \
    X(DYNF_PROTO,  "20.2.1.1.1 step 25 (proto is GetPrototypeFromConstructor(newTarget, fallbackProto)), set " \
                   "on F")
enum { DYNFUNC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_dynfunc_steps[] = { DYNFUNC_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_dynfunc_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSDynFunc *s = st;
    JSFunctionKindEnum func_kind = s->hdr.arg;
    bool created = false;
    JSValue proto;
    int r;

    if (s->hdr.stage == DYNF_ENTRY) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->result = JS_UNDEFINED;
        s->func = JS_UNDEFINED;
        if (s->hdr.argc > 0) {
            s->strs = js_malloc(ctx, sizeof(JSValue) * (size_t)s->hdr.argc);
            if (unlikely(!s->strs))
                return -1;
        }
        s->hdr.stage = DYNF_STRS;
    }
    if (s->hdr.stage == DYNF_STRS) {
        JSValue src;
        while (s->nstrs < s->hdr.argc) {
            JSValue str;
            if (js_value_is_concolic(step_arg(&s->hdr, s->nstrs))) {
                /* §7.1.19 ToString over unknown input has no String to give, and these strings exist only to
                   be concatenated (steps 12-16), so the unknown is kept as its own piece of that
                   concatenation rather than completing the constructor as one unknown value. */
                str = js_dup(step_arg(&s->hdr, s->nstrs));
            } else {
                r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, s->nstrs), cb_result, &str, out_cb,
                                      out_argc);
                cb_result = JS_UNDEFINED;
                if (r) return r < 0 ? -1 : r;
            }
            s->strs[s->nstrs++] = str;
        }
        src = js_dynfunc_source(ctx, func_kind, s->strs, s->nstrs, DYNSRC_ALL);
        if (JS_IsException(src))
            return -1;
        s->hdr.stage = DYNF_PARSE;
        {
            /* HostEnsureCanCompileStrings(currentRealm, parameterStrings, bodyString, sourceString, function,
               parameterArgs, bodyArg): every argument but the last is a parameter and the last is the body; with
               no arguments the Function constructors pass the empty String as bodyArg. */
            JSValue empty = JS_AtomToString(ctx, JS_ATOM_empty_string);
            int np = s->nstrs > 0 ? s->nstrs - 1 : 0;
            JSCompileCheck cc = { JS_COMPILATION_FUNCTION, np, s->strs, s->hdr.argv,
                                  s->nstrs > 0 ? s->strs[np] : empty, s->nstrs > 0 ? s->hdr.argv[np] : empty };

            r = step_program_run(ctx, &s->hdr, ctx, src, JS_UNDEFINED, &s->func, out_cb, out_argc,
                                 JS_EVAL_FLAG_FUNCTION_CTOR, &cc);
            JS_FreeValue(ctx, empty);
        }
        JS_FreeValue(ctx, src);
        if (r) return r < 0 ? -1 : r;
        created = true;
    } else if (s->hdr.stage == DYNF_PARSE) {
        r = step_program_run(ctx, &s->hdr, ctx, JS_UNDEFINED, cb_result, &s->func, out_cb, out_argc,
                             JS_EVAL_FLAG_FUNCTION_CTOR, NULL);
        if (r) return r < 0 ? -1 : r;
        created = true;
    }
    if (created && js_value_is_concolic(s->func)) {
        /* sourceString was unknown input with no text for an example, so the sub-sequence handed the source
           back unevaluated (the sink was still told of it). F is then an unknown derived from that source, whose
           prototype is its own, so step 25 sets none. */
        s->result = js_concolic_derive(ctx, s->func, "20.2.1.1.1 CreateDynamicFunction F", JS_UNDEFINED);
        JS_FreeValue(ctx, s->func);
        s->func = JS_UNDEFINED;
        DCHECK(!JS_IsUninitialized(s->result), "an unknown dynamic-function source derived no unknown");
        return 0;
    }
    if (created) {
        /* Steps 17-20's separate parses of the parameters and the body. They follow the host check, which can
           run page code, and they stand after the whole-source parse here: every one of them can only throw a
           SyntaxError, and F is not yet observable, so which parse refuses first is not. */
        if (js_dynfunc_check_halves(ctx, func_kind, s->strs, s->nstrs) < 0)
            return -1;
        /* THE OTHER SIDE OF js_parse_fn_ctor_source'S REFUSAL. 20.2.1.1.1 returns an ECMAScript function
           object, and the only reason the value this sub-sequence hands back is one is that step 23's goal
           parse admits nothing but a FunctionExpression — so when that stopped being true, this returned
           whatever the trailing source element evaluated to. It did: a body of `}); f(); ({` made
           `new Function(...)` answer an OBJECT, with the page's `f()` already run. Asserted where the value
           ARRIVES because a wrong one is a fact about the parse and not about anything below this line. */
        DCHECK(JS_IsFunction(ctx, s->func),
               "20.2.1.1.1's synthesized source evaluated to something that is not a function — step 23 parses "
               "it as one FunctionExpression, so a second source element got past js_parse_fn_ctor_source");
        cb_result = JS_UNDEFINED;
        s->hdr.stage = DYNF_PROTO;
        /* The CALL form leaves new.target undefined, and the synthesized source already gave the function the
           intrinsic prototype of its own kind, so there is no read and nothing to set. */
        if (JS_IsUndefined(s->hdr.this_val)) {
            s->result = s->func;
            s->func = JS_UNDEFINED;
            return 0;
        }
    }
    DCHECK(s->hdr.stage == DYNF_PROTO, "CreateDynamicFunction resumed in an unknown stage");
    r = step_proto_from_ctor_run(ctx, &s->hdr, s->hdr.this_val, func_kind_to_class_id[func_kind],
                                 cb_result, &proto, out_cb, out_argc);
    if (r) return r < 0 ? -1 : r;
    r = JS_SetPrototypeInternal(ctx, s->func, proto, true);
    JS_FreeValue(ctx, proto);
    if (r < 0)
        return -1;
    s->result = s->func;
    s->func = JS_UNDEFINED;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The created function, held across the `prototype` read, and
   the ToString'd arguments — one per argument the call supplied, which is the block's extent whatever the
   coercion cursor has reached. */
static void js_dynfunc_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSDynFunc *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->func);
    v->array(ctx, (void **)&s->strs, sizeof(JSValue), s->nstrs, s->hdr.argc, js_step_visit_value_elem);
}

static JSValue js_dynfunc_fini(JSContext *ctx, void *st, bool take_result)
{
    JSDynFunc *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static __exception int js_get_length32(JSContext *ctx, uint32_t *pres,
                                       JSValueConst obj)
{
    JSValue len_val;
    len_val = JS_GetProperty(ctx, obj, JS_ATOM_length);
    if (JS_IsException(len_val)) {
        *pres = 0;
        return -1;
    }
    return JS_ToUint32Free(ctx, pres, len_val);
}

static __exception int js_get_length64(JSContext *ctx, int64_t *pres,
                                       JSValueConst obj)
{
    JSValue len_val;
    len_val = JS_GetProperty(ctx, obj, JS_ATOM_length);
    if (JS_IsException(len_val)) {
        *pres = 0;
        return -1;
    }
    return JS_ToLengthFree(ctx, pres, len_val);
}

static __exception int js_set_length64(JSContext *ctx, JSValueConst obj,
                                       int64_t len)
{
    return JS_SetProperty(ctx, obj, JS_ATOM_length, js_int64(len));
}

static void free_arg_list(JSContext *ctx, JSValue *tab, uint32_t len)
{
    uint32_t i;
    for(i = 0; i < len; i++) {
        JS_FreeValue(ctx, tab[i]);
    }
    js_free(ctx, tab);
}

/* DELETED: build_arg_list. It WAS 7.3.19 CreateListFromArrayLike performed from C — `? LengthOfArrayLike(obj)`
   with js_get_length32 and a `? Get(obj, index)` loop with JS_GetPropertyUint32 — which is the page's code at every
   step, and it had ten callers across the apply trampoline, the construct dispatch, two unreachable
   generator reshapes, OP_apply_eval and three unregistered C bodies. CONT_ARG_LIST performs the operation on the
   tramp with a cursor; arg_list_is_fast / arg_list_fast_build answer for the compiler-built lists that invoke
   nothing. There is no C form left, which is why this comment stands where the function did: the operation exists
   once. */

/* magic value: 0 = normal apply, 1 = apply for constructor, 2 =
   Reflect.apply */
/* DELETED: js_function_apply's C body. Function.prototype.apply and Reflect.apply are STEPDEF_FUNCTION_APPLY /
   STEPDEF_REFLECT_APPLY — ONE machine whose magic selects the spelling — and the direct spellings are
   call-site-resolved onto do_apply_tramp. Its last reachable use was the OP_apply residue, which is two throws in
   spec order and which that site now performs itself. While the body stood it built its argument list with
   build_arg_list from C, so an array-like whose `length` getter loops preempted with no flow base: a live defect
   in a body nothing was meant to reach, which is exactly why a superseded body is not allowed to keep compiling. */

/* THE coerce-then-compute machine. Stage 0 walks the declared argument positions, requesting a TOPRIMITIVE for
   each OBJECT among them; stage 1 receives one and replaces that argument. When none is left the body runs with
   an argument vector that holds only primitives, so every JS_ToIndex / JS_ToNumber / JS_ToBigInt /
   JS_ToPropertyKey inside it is pure arithmetic. */
/* Every conversion a coerce-then-compute declaration can name — ToNumber for HINT_NUMBER, ToString for
   HINT_STRING — throws an UNCONDITIONAL TypeError on a Symbol, and the spec performs argument i's conversion
   BEFORE it touches argument i+1. Splitting ToPrimitive from the narrowing put every later argument's valueOf
   in between, so `Math.atan2(Symbol(), {valueOf(){throw x}})` ran that valueOf and reported ITS throw where the
   spec reports the Symbol's TypeError — and the same for Math.pow, Math.max, BigInt.asIntN and Atomics.wait.
   Doing it here is the narrowing's one outcome that does not depend on what the body converts to. */
static int prim_arg_check(JSContext *ctx, JSValueConst v, int hint)
{
    if (unlikely(JS_VALUE_GET_TAG(v) == JS_TAG_SYMBOL)) {
        JS_ThrowTypeError(ctx, hint == HINT_STRING ? "cannot convert symbol to string"
                                                   : "cannot convert symbol to number");
        return -1;
    }
    return 0;
}

/* ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). This machine is
   SHARED by every coerce-then-compute builtin, and what it performs is not one of their algorithms: it is
   7.1.1 over the arguments each of them DECLARES it coerces, after which the body has no user code left to
   reach. So the STAGE LABELS name that operation — which is the half of a derived unknown's identity that says
   "this was the declared-argument coercion" — while the ALGORITHM half is the builtin's own, declared at its
   definition (see PRIMARGS_DEF_FULL). Naming the shared operation in BOTH halves is what would collapse sixty
   builtins' coercions into one identity. */
#define PRIMARGS_STAGES(X) \
    X(PRIMARGS_ENTRY,  "7.1.1 over a coerce-then-compute builtin's declared arguments: the leading validation " \
                       "the spec performs BEFORE any coercion (the definition's `precheck`), then the padded " \
                       "argument vector") \
    X(PRIMARGS_COERCE, "7.1.1 ToPrimitive ( input [ , preferredType ] ) applied to each declared argument " \
                       "in argument order, with the definition's `midcheck` interleaved after the first - and then the " \
                       "definition's `body`, over the primitives this stage put in place")
enum { PRIMARGS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_primargs_steps[] = { PRIMARGS_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_primargs_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSPrimArgs *s = st;
    unsigned mask = (unsigned)s->hdr.arg & 0xff;
    int hint = (s->hdr.arg & 0x100) ? HINT_STRING : HINT_NUMBER;
    int nargs = (s->hdr.arg >> 16) & 0xff;
    int i;

    if (s->hdr.stage == PRIMARGS_ENTRY) {
        JS_FreeValue(ctx, cb_result);
        s->result = JS_UNDEFINED;
        if (s->hdr.def->precheck && s->hdr.def->precheck(ctx, &s->hdr) < 0)
            return -1;
        s->nargp = s->hdr.argc > nargs ? s->hdr.argc : nargs;
        s->argp = js_malloc(ctx, sizeof(JSValue) * (size_t)(s->nargp > 0 ? s->nargp : 1));
        if (unlikely(!s->argp)) { s->nargp = 0; return -1; }
        for (i = 0; i < s->nargp; i++)
            s->argp[i] = js_dup(step_arg(&s->hdr, i));
        s->next = 0;
        s->hdr.stage = PRIMARGS_COERCE;
    } else {
        DCHECK(s->next > 0, "coerce-then-compute: a primitive arrived with no coercion in flight");
        s->hdr.coercing = 0;
        JS_FreeValue(ctx, s->argp[s->next - 1]);
        s->argp[s->next - 1] = cb_result;   /* the primitive REPLACES the argument the body will read */
        if (prim_arg_check(ctx, s->argp[s->next - 1], hint) < 0)
            return -1;
        if (s->next == 1 && s->hdr.def->midcheck
            && s->hdr.def->midcheck(ctx, (JSValueConst *)s->argp, s->hdr.def->body_magic) < 0)
            return -1;
    }
    while (s->next < s->nargp) {
        i = s->next++;
        if (!(s->hdr.arg & PRIMARGS_ALL) && (i >= 8 || !((mask >> i) & 1))) continue;
        if (JS_VALUE_GET_TAG(s->argp[i]) == JS_TAG_OBJECT) {
            s->cb[0] = s->argp[i];   /* borrowed: the vector holds the reference */
            *out_cb = s->cb; *out_argc = hint;
            s->hdr.coercing = 1;
            return 5;
        }
        /* the argument was already primitive, so its own conversion and interleaved validation are due here */
        if (prim_arg_check(ctx, s->argp[i], hint) < 0)
            return -1;
        if (i == 0 && s->hdr.def->midcheck && s->hdr.def->midcheck(ctx, (JSValueConst *)s->argp, s->hdr.def->body_magic) < 0)
            return -1;
    }
    DCHECK(s->hdr.def->body.generic != NULL, "a coerce-then-compute definition with no body");
    /* The f_f / f_f_f prototypes take a DOUBLE, so js_call_c_function did their ToNumber — which is where the whole
       Math surface reached a page's valueOf from C. The conversion happens here instead, in argument order, which
       is the order the spec's ToNumber steps are in.
       AND IT GOES THROUGH THE COERCION SUB-SEQUENCE rather than JS_ToFloat64, because 7.1.4 ToNumber ( argument )
       over UNKNOWN EXTERNAL INPUT has an answer and the conversion boundary is not where it is given. 7.1.1
       ToPrimitive over a concolic is the IDENTITY — the value is primitive in the page and wears an Object only
       as the solver's carrier — so the loop above hands one straight through, and every f_f builtin then asked
       that boundary for a real double and aborted the whole instance: `Math.floor({orphan0.arg0}.size)` is two
       lines of a real bundle and one @WHY for the document. step_tofloat64_run converts the operand's example
       instead (or reports JS_STEP_UNKNOWN without one), and this machine's completion becomes the unknown
       derived under the DEFINITION'S OWN algorithm — which is why PRIMARGS_DEF_FULL takes one.
       On anything else the sub-sequence invokes nothing and can only throw (a Symbol, a BigInt), and it cannot
       ask for a ToPrimitive: every declared argument is already primitive here, and a REQUEST would be re-entered
       above as an argument coercion's answer and overwrite the wrong slot. */
    if (s->hdr.def->body_proto == JS_CFUNC_f_f || s->hdr.def->body_proto == JS_CFUNC_f_f_f) {
        int nf = s->hdr.def->body_proto == JS_CFUNC_f_f ? 1 : 2;
        double d[2] = { 0, 0 };
        for (i = 0; i < nf; i++) {
            int r = step_tofloat64_run(ctx, &s->hdr, s->argp[i], JS_UNDEFINED, &d[i], out_cb, out_argc);
            DCHECK(r != 5, "a coerce-then-compute builtin's own ToNumber asked for a ToPrimitive — the argument "
                           "the declaration says was made primitive is still an object");
            if (r != 0)
                return r;
        }
        s->result = js_float64(nf == 1 ? s->hdr.def->body.f_f(d[0])
                                       : s->hdr.def->body.f_f_f(d[0], d[1]));
        return 0;
    }
    /* The BODY's argc is the call's REAL argc, never the padded vector length — exactly what js_call_c_function
       passes. nargp pads the BUFFER so a fixed-arity body may read argv[1] of a one-argument call; handing that
       count to the body instead made every VARIADIC one see phantom undefined arguments, so `Math.max()` computed
       max(undefined, undefined) = NaN where the spec says -Infinity, `String.fromCharCode()` returned "\0" instead
       of "", and `Date.UTC(1970)` filled in five undefined fields and returned NaN. */
    if (s->hdr.def->body_proto == JS_CFUNC_constructor_or_func) {
        /* A builtin that is BOTH callable and a constructor (BigInt) whose only user code is its argument's
           ToPrimitive. The step-ctor reshape already passes new_target where a method passes `this`, which is
           exactly this prototype's first parameter, so the declaration needs nothing construct-aware. */
        s->result = s->hdr.def->body.generic(ctx, s->hdr.this_val, s->hdr.argc, (JSValueConst *)s->argp);
    } else if (s->hdr.def->body_proto == JS_CFUNC_generic_magic) {
        s->result = s->hdr.def->body.generic_magic(ctx, s->hdr.this_val, s->hdr.argc,
                                                   (JSValueConst *)s->argp, s->hdr.def->body_magic);
    } else {
        DCHECK(s->hdr.def->body_proto == JS_CFUNC_generic, "coerce-then-compute: unhandled body prototype");
        s->result = s->hdr.def->body.generic(ctx, s->hdr.this_val, s->hdr.argc, (JSValueConst *)s->argp);
    }
    return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The COERCED argument vector is the machine's own block — each coercion is the page's valueOf, so a fork can land between two of them. */
static void js_primargs_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSPrimArgs *s = st;
    v->val(ctx, &s->result);
    v->array(ctx, (void **)&s->argp, sizeof(JSValue), s->nargp, s->nargp, js_step_visit_value_elem);
}

static JSValue js_primargs_fini(JSContext *ctx, void *st, bool take_result)
{
    JSPrimArgs *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* THE OrdinaryCreateFromConstructor MACHINE (10.1.13). A C constructor's first spec step reads new.target's
   `prototype` — a [[Get]] the page can define as a getter, so it is USER CODE, and every C constructor performed
   it inside js_create_from_ctor from a frame with no flow base: a loop in that getter aborts at its back-edge
   with "gen_state NULL". The declaration says only WHICH class the object is (def->arg) and what to run once it
   exists; stage 0 requests the read on the tramp, and the body is then called with the CREATED OBJECT where a
   plain C constructor receives new_target. That body is not a legacy twin — with the object already made it has
   no OrdinaryCreateFromConstructor left to perform, which is exactly what the declaration asserts, and it is the
   only implementation there is. A NULL body means the machine IS the whole constructor (the created object is the
   result), which is what a constructor that only creates looks like.
   The body OWNS obj: a constructor body already ends in `return obj` and frees it on every failure path, so
   taking it is the shape those bodies are written in. */
/* the CLASS and the body's DECLARED ARGUMENT COUNT, packed into the declaration's `arg`. */
#define CREATECTOR_ARG(class_id, nargs) ((class_id) | ((nargs) << 16))
#define CREATECTOR_CLASS(a) ((a) & 0xffff)
#define CREATECTOR_NARGS(a) (((a) >> 16) & 0xff)
/* A WRAPPER constructor — Boolean, and every other builtin whose CALL form returns a PRIMITIVE rather than an
   object (20.3.1.1 step 2 returns `b` when NewTarget is undefined). For those the call form is not
   OrdinaryCreateFromConstructor degenerating to the intrinsic prototype, it is a different algorithm: the body
   runs with NO object and its value IS the result. Declaring it is what lets the ONE create-from-ctor machine
   serve both kinds; without it, `Boolean(0)` would answer with a Boolean OBJECT. */
#define CREATECTOR_WRAP 0x1000000
#define CREATECTOR_ARG_WRAP(class_id, nargs) (CREATECTOR_ARG(class_id, nargs) | CREATECTOR_WRAP)
#define CREATECTOR_IS_WRAP(a) (((a) & CREATECTOR_WRAP) != 0)
/* 20.1.1.1 step 1 guards OrdinaryCreateFromConstructor with "NewTarget is neither undefined NOR THE ACTIVE
   FUNCTION OBJECT" — so `new Object()` performs no create at all and only a SUBCLASS's NewTarget does. That
   second half is Object's alone, and it is declared rather than written into a body because it decides whether
   the machine's own create step happens; a body cannot un-perform a [[Get]] the machine already made. */
#define CREATECTOR_SELF_PLAIN 0x2000000
#define CREATECTOR_ARG_SELF_PLAIN(class_id, nargs) \
    (CREATECTOR_ARG(class_id, nargs) | CREATECTOR_WRAP | CREATECTOR_SELF_PLAIN)
#define CREATECTOR_IS_SELF_PLAIN(a) (((a) & CREATECTOR_SELF_PLAIN) != 0)
typedef struct JSCreateCtor {
    JSStepHdr hdr;
    JSValue result;
    JSValue *argp;   /* a PADDED argument vector, only when the body reads further than the call supplied */
    int nargp;
} JSCreateCtor;

/* ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). Like the
   coerce-then-compute machine, this one is SHARED by every constructor that begins with
   OrdinaryCreateFromConstructor, and the operation it performs is 10.1.13 rather than any one of their
   algorithms - which constructor's body runs after it is what the definition's `body` says. */
#define CRECTOR_STAGES(X) \
    X(CRECTOR_ENTRY,  "10.1.13, entered: the validation the constructor's own algorithm performs before it " \
                      "(the definition's `precheck`)") \
    X(CRECTOR_CREATE, "10.1.13 steps 1-3 (proto is GetPrototypeFromConstructor(constructor, " \
                      "intrinsicDefaultProto), whose step 3 is Get(constructor, \"prototype\"); the object is " \
                      "OrdinaryObjectCreate(proto)) - then the constructor's own body over it")
enum { CRECTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_creatector_steps[] = { CRECTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_creatector_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSCreateCtor *s = st;
    JSValue obj;
    int r;
    bool plain;

    if (s->hdr.stage == CRECTOR_ENTRY) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->result = JS_UNDEFINED;
        if (s->hdr.def->precheck && s->hdr.def->precheck(ctx, &s->hdr) < 0)
            return -1;
        s->hdr.stage = CRECTOR_CREATE;
    }
    /* the NEW TARGET this algorithm treats as ABSENT. Undefined always is (the CALL form); Object additionally
       counts ITSELF, which is 20.1.1.1 step 1's second clause and the whole of the difference. */
    plain = JS_IsUndefined(s->hdr.this_val)
         || (CREATECTOR_IS_SELF_PLAIN(s->hdr.arg)
             && JS_VALUE_GET_TAG(s->hdr.this_val) == JS_TAG_OBJECT
             && JS_VALUE_GET_OBJ(s->hdr.this_val) == JS_VALUE_GET_OBJ(s->hdr.func_obj));
    if (CREATECTOR_IS_WRAP(s->hdr.arg) && plain) {
        /* the CALL form of a WRAPPER constructor: 20.3.1.1 step 2 returns the primitive, so there is no object to
           create and the body says what the value is. */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        obj = JS_UNDEFINED;
    } else {
        /* new.target UNDEFINED is the call form of a constructor_or_func builtin (`Array(3)`): the spec's
           OrdinaryCreateFromConstructor degenerates to the intrinsic prototype, which reads nothing. */
        r = step_create_from_ctor_run(ctx, &s->hdr, s->hdr.this_val, CREATECTOR_CLASS(s->hdr.arg), cb_result, &obj,
                                      out_cb, out_argc);
        if (r) return r < 0 ? -1 : r;
    }
    DCHECK(s->hdr.def->body.generic || !CREATECTOR_IS_WRAP(s->hdr.arg),
           "a wrapper constructor declares no body, so its call form has no value to answer with");
    if (!s->hdr.def->body.generic) {
        s->result = obj;
        return 0;
    }
    DCHECK(s->hdr.def->body_proto == JS_CFUNC_generic || s->hdr.def->body_proto == JS_CFUNC_generic_magic,
           "OrdinaryCreateFromConstructor: unhandled body prototype");
    {
        /* js_call_c_function PADS a C body's argument vector to its declared length, so a body may read argv[1]
           of a zero-argument call; a machine is handed the CALL's real operands, so the padding has to happen
           here or `new DOMException()` reads one slot past the captured vector. The count rides the declaration
           beside the class id, the same way the coerce-then-compute spec carries its own. */
        int nargs = CREATECTOR_NARGS(s->hdr.arg);
        JSValueConst *bargv = (JSValueConst *)s->hdr.argv;
        if (s->hdr.argc < nargs) {
            int i;
            DCHECK(s->argp == NULL, "the padded argument vector is built exactly once");
            s->argp = js_malloc(ctx, sizeof(JSValue) * (size_t)nargs);
            if (unlikely(!s->argp)) { JS_FreeValue(ctx, obj); return -1; }
            s->nargp = nargs;
            for (i = 0; i < nargs; i++)
                s->argp[i] = (i < s->hdr.argc) ? js_dup(s->hdr.argv[i]) : JS_UNDEFINED;
            bargv = (JSValueConst *)s->argp;
        }
        s->result = (s->hdr.def->body_proto == JS_CFUNC_generic_magic)
                  ? s->hdr.def->body.generic_magic(ctx, obj, s->hdr.argc, bargv, s->hdr.def->body_magic)
                  : s->hdr.def->body.generic(ctx, obj, s->hdr.argc, bargv);
    }
    return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The padded argument vector is the machine's own block. */
static void js_creatector_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSCreateCtor *s = st;
    v->val(ctx, &s->result);
    v->array(ctx, (void **)&s->argp, sizeof(JSValue), s->nargp, s->nargp, js_step_visit_value_elem);
}

static JSValue js_creatector_fini(JSContext *ctx, void *st, bool take_result)
{
    JSCreateCtor *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* 20.2.3.3's four steps hold ONE thing that reaches the page: the Call. So two stages — the entry, where the
   argument list is assembled out of operands nobody can observe, and the Call this machine is parked on. */
#define FCALL_STAGES(X) \
    X(FCALL_ASSEMBLE, "20.2.3.3 steps 1-3 (func is the this value; PrepareForTailCall) and step 4's argument " \
                      "list, which is the caller's own arguments past thisArg") \
    X(FCALL_CALL,     "20.2.3.3 step 4 (Call(func, thisArg, args))")
enum { FCALL_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_function_call_steps[] = { FCALL_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_function_call_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSFuncCall *s = st;
    int i, n;

    if (s->hdr.stage == FCALL_ASSEMBLE) {
        JS_FreeValue(ctx, cb_result);
        s->result = JS_UNDEFINED;
        n = s->hdr.argc > 0 ? s->hdr.argc - 1 : 0;
        s->cb = js_malloc(ctx, sizeof(JSValue) * (size_t)(n + 2));
        if (unlikely(!s->cb)) return -1;
        s->ncb = n + 2;
        s->cb[0] = js_dup(step_arg(&s->hdr, 0));   /* thisArg — absent means undefined, which is the spec's */
        s->cb[1] = js_dup(s->hdr.this_val);        /* the function; a non-callable throws at the dispatch */
        for (i = 0; i < n; i++)
            s->cb[2 + i] = js_dup(s->hdr.argv[1 + i]);
        s->hdr.stage = FCALL_CALL;
        *out_cb = s->cb; *out_argc = n;
        return 3;
    }
    DCHECK(s->hdr.stage == FCALL_CALL, "Function.prototype.call: unknown stage");
    s->result = cb_result;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). `cb` is its OWN block — [newThis, f, args…] with the CALLER's argument count — so it is an element array, not a borrowed view. */
static void js_function_call_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSFuncCall *s = st;
    v->val(ctx, &s->result);
    v->array(ctx, (void **)&s->cb, sizeof(JSValue), s->ncb, s->ncb, js_step_visit_value_elem);
}

static JSValue js_function_call_fini(JSContext *ctx, void *st, bool take_result)
{
    JSFuncCall *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* Function.prototype.apply and Reflect.apply — `call`'s twin, and neither had a machine. `.call` is a step
   builtin, so reached as a VALUE (a bind target, a callback, a Proxy `apply` trap) the convergence point answers
   it; these were plain C bodies, so the same spellings ran their target from C.
   CreateListFromArrayLike is the page's code TWICE — `length` and every element are a getter or a Proxy trap —
   and build_arg_list performs both from C, so an array-like whose getter loops preempted in an activation with no
   flow base. It is the existing sub-sequences here: step_length_run for `? ToLength(? Get(src,"length"))` and
   step_getidx_run per element, the same two the array walks already share.
   ONE WALK, THREE ALGORITHMS, ONE STAGE LIST expanded once per algorithm with that algorithm's own step
   numbers: validate, then 7.3.19 CreateListFromArrayLike's length and its per-element Get, then the internal
   method. Reflect.construct differs only in which internal method ends it, which is why it shares the list and
   not a hand-copied one. */
#define FAPPLY_STAGES(X, VALIDATE, TAIL) \
    X(FAPPLY_VALIDATE, VALIDATE) \
    X(FAPPLY_LENGTH,   "7.3.19 CreateListFromArrayLike step 3 (length is LengthOfArrayLike(argumentsList), " \
                       "which 7.3.18 LengthOfArrayLike is ToLength(Get(obj, \"length\")))") \
    X(FAPPLY_ELEMS,    "7.3.19 CreateListFromArrayLike step 6.b (next is Get(obj, indexName)), one " \
                       "element per step") \
    X(FAPPLY_CALL,     TAIL)
enum { FAPPLY_STAGES(JS_STEP_STAGE_ENUM, "", "") };
static const char *const js_function_apply_steps[] = {
    FAPPLY_STAGES(JS_STEP_STAGE_LABEL,
                  "20.2.3.1 steps 1-3 (func is the this value; IsCallable; a nullish argArray is an empty "
                  "list) and 7.3.19 step 2 (obj must be an Object)",
                  "20.2.3.1 step 6 (Call(func, thisArg, argList))") NULL };
static const char *const js_reflect_apply_steps[] = {
    FAPPLY_STAGES(JS_STEP_STAGE_LABEL,
                  "28.1.1 step 1 (IsCallable(target)) and 7.3.19 step 2 (argumentsList must be an Object)",
                  "28.1.1 step 4 (Call(target, thisArgument, args))") NULL };
static const char *const js_reflect_construct_steps[] = {
    FAPPLY_STAGES(JS_STEP_STAGE_LABEL,
                  "28.1.2 steps 1-3 (IsConstructor(target); newTarget defaults to target and must itself be a "
                  "constructor) and 7.3.19 step 2 (argumentsList must be an Object)",
                  "28.1.2 step 5 (Construct(target, args, newTarget))") NULL };

typedef struct JSFuncApply {
    JSStepHdr hdr;
    JSValue result;       /* DONE (owned) */
    JSValue *cb;          /* [thisArg, f, args…] — its own block, the arg count being the SOURCE's */
    int ncb;
    int64_t len;          /* LengthOfArrayLike(src) */
    int64_t i;            /* the collect cursor: an element read can suspend, so it cannot be a C local */
} JSFuncApply;

static int js_function_apply_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSFuncApply *s = st;
    /* 0 = Function.prototype.apply, whose function is the RECEIVER; 1 = Reflect.apply, which passes it as an
       argument. Everything after that is the same algorithm, which is why it is one machine and not two — the
       C bodies were already one function taking a magic. */
    int refl = (int)(intptr_t)s->hdr.arg;
    JSValueConst fn      = refl ? step_arg(&s->hdr, 0) : s->hdr.this_val;
    JSValueConst thisArg = step_arg(&s->hdr, refl ? 1 : 0);
    JSValueConst arr     = step_arg(&s->hdr, refl ? 2 : 1);
    JSValue in = cb_result;
    int r;

    if (s->hdr.stage == FAPPLY_VALIDATE) {
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        s->result = JS_UNDEFINED;
        if (check_function(ctx, fn))
            return -1;
        /* 20.2.3.1 step 2: a nullish argArray is an empty list. 28.1.1 step 2 has no such case — its
           CreateListFromArrayLike throws on anything that is not an object. */
        if (!refl && (JS_IsUndefined(arr) || JS_IsNull(arr))) {
            s->len = 0;
            s->hdr.stage = FAPPLY_ELEMS;
        } else {
            if (JS_VALUE_GET_TAG(arr) != JS_TAG_OBJECT) {
                JS_ThrowTypeError(ctx, "not a object");
                return -1;
            }
            s->hdr.stage = FAPPLY_LENGTH;
        }
    }
    if (s->hdr.stage == FAPPLY_LENGTH) {
        r = step_length_run(ctx, &s->hdr, arr, in, &s->len, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r) return r;
        if (s->len > JS_MAX_LOCAL_VARS) {
            JS_ThrowRangeError(ctx, "too many arguments in function call (only %d allowed)", JS_MAX_LOCAL_VARS);
            return -1;
        }
        s->hdr.stage = FAPPLY_ELEMS;
    }
    if (s->hdr.stage == FAPPLY_ELEMS) {
        if (!s->cb) {
            int64_t k;
            /* the state owns every slot BEFORE the first element read, which can throw or suspend: a teardown
               frees exactly ncb of them, so a slot handed over late would leak and one handed over early to a
               buffer that does not exist yet is a free of garbage. */
            s->cb = js_malloc(ctx, sizeof(JSValue) * (size_t)(s->len + 2));
            if (unlikely(!s->cb)) return -1;
            s->ncb = (int)s->len + 2;
            for (k = 0; k < s->len + 2; k++) s->cb[k] = JS_UNDEFINED;
            s->cb[0] = js_dup(thisArg);   /* absent means undefined, which is the spec's */
            s->cb[1] = js_dup(fn);
            s->i = 0;
        }
        while (s->i < s->len) {
            JSValue el = JS_UNDEFINED;
            r = step_getidx_run(ctx, &s->hdr, arr, s->i, in, &el, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r) return r;
            JS_FreeValue(ctx, s->cb[2 + s->i]);
            s->cb[2 + s->i] = el;
            s->i++;
        }
        s->hdr.stage = FAPPLY_CALL;
        *out_cb = s->cb; *out_argc = (int)s->len;
        return 3;
    }
    DCHECK(s->hdr.stage == FAPPLY_CALL, "apply: unknown stage");
    s->result = in;
    return 0;
}

/* Reflect.construct(target, argumentsList[, newTarget]) — apply's construct twin, and it had no machine either,
   so reached as a VALUE it built its argument list and drove the constructor from C. 28.1.2 is the same shape as
   28.1.1: validate, CreateListFromArrayLike, then the internal method — so it is the same collect, on the same
   sub-sequences, ending in a CONSTRUCT request instead of a CALL. */
static int js_reflect_construct_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSFuncApply *s = st;
    JSValueConst ctor = step_arg(&s->hdr, 0);
    JSValueConst arr  = step_arg(&s->hdr, 1);
    JSValueConst ntgt = (s->hdr.argc >= 3) ? step_arg(&s->hdr, 2) : ctor;
    JSValue in = cb_result;
    int r;

    if (s->hdr.stage == FAPPLY_VALIDATE) {
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        s->result = JS_UNDEFINED;
        /* 28.1.2 steps 1-3, in the spec's order: both constructor checks precede the list */
        if (!JS_IsConstructor(ctx, ctor)) {
            JS_ThrowTypeError(ctx, "not a constructor");
            return -1;
        }
        if (!JS_IsConstructor(ctx, ntgt)) {
            JS_ThrowTypeError(ctx, "not a constructor");
            return -1;
        }
        if (JS_VALUE_GET_TAG(arr) != JS_TAG_OBJECT) {
            JS_ThrowTypeError(ctx, "not a object");
            return -1;
        }
        s->hdr.stage = FAPPLY_LENGTH;
    }
    if (s->hdr.stage == FAPPLY_LENGTH) {
        r = step_length_run(ctx, &s->hdr, arr, in, &s->len, out_cb, out_argc);
        in = JS_UNDEFINED;
        if (r) return r;
        if (s->len > JS_MAX_LOCAL_VARS) {
            JS_ThrowRangeError(ctx, "too many arguments in function call (only %d allowed)", JS_MAX_LOCAL_VARS);
            return -1;
        }
        s->hdr.stage = FAPPLY_ELEMS;
    }
    if (s->hdr.stage == FAPPLY_ELEMS) {
        if (!s->cb) {
            int64_t k;
            /* [ctor, args…] — the shape step code 4 reads. Every slot is the state's BEFORE the first element
               read, which can throw or suspend. */
            s->cb = js_malloc(ctx, sizeof(JSValue) * (size_t)(s->len + 1));
            if (unlikely(!s->cb)) return -1;
            s->ncb = (int)s->len + 1;
            for (k = 0; k < s->len + 1; k++) s->cb[k] = JS_UNDEFINED;
            s->cb[0] = js_dup(ctor);
            s->i = 0;
        }
        while (s->i < s->len) {
            JSValue el = JS_UNDEFINED;
            r = step_getidx_run(ctx, &s->hdr, arr, s->i, in, &el, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r) return r;
            JS_FreeValue(ctx, s->cb[1 + s->i]);
            s->cb[1 + s->i] = el;
            s->i++;
        }
        s->hdr.stage = FAPPLY_CALL;
        s->hdr.ctor_ntgt = js_dup(ntgt);   /* the request's NEW TARGET operand */
        *out_cb = s->cb; *out_argc = (int)s->len;
        return 4;
    }
    DCHECK(s->hdr.stage == FAPPLY_CALL, "Reflect.construct: unknown stage");
    s->result = in;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). `cb` is its OWN block — [thisArg, f, args…] gathered one
   element at a time, and each read can suspend — so it is an element array rather than a borrowed view: the
   clone copies the block before taking references into it, which is the ordering the operation owns. */
static void js_function_apply_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSFuncApply *s = st;
    v->val(ctx, &s->result);
    v->array(ctx, (void **)&s->cb, sizeof(JSValue), s->ncb, s->ncb, js_step_visit_value_elem);
}

static JSValue js_function_apply_fini(JSContext *ctx, void *st, bool take_result)
{
    JSFuncApply *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* 20.2.3.2 Function.prototype.bind. BoundFunctionCreate runs nothing of the page's, but the three steps the
   spec states after it do: step 5's HasOwnProperty(Target, "length") is Target.[[GetOwnProperty]], and steps
   6.a and 8 Get "length" and "name" — an accessor or a Proxy trap in every case. The C body performed all
   three from C, one JS_GetOwnProperty and two JS_GetProperty, so
   `Function.prototype.bind.call(new Proxy(f, {get(){for(;;){}}}))` had no flow base. */
typedef struct JSFuncBind {
    JSStepHdr hdr;      /* MUST be first */
    JSValue func_obj;   /* the bound function, created BEFORE the reads the spec states after it (owned) */
    JSValue result;     /* DONE (owned) */
    int arg_count;      /* the bound arguments, which the target's `length` is reduced by */
} JSFuncBind;
/* ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). The stage that
   DISPATCHED the HasOwnProperty rested at no step - it was entered and left inside one step() call - so it is
   the tail of the stage that builds F now. */
#define FBIND_STAGES(X) \
    X(FB_ENTRY,       "20.2.3.2 steps 1-2 (Target is the this value; the IsCallable TypeError) - and the " \
                      "dispatch of step 3's BoundFunctionCreate, whose 10.4.1.3 step 1 is " \
                      "Target.[[GetPrototypeOf]]()") \
    X(FB_PROTO_GOT,   "20.2.3.2 step 3 -> 10.4.1.3 steps 1-6 (proto arrives; F holds [[BoundTargetFunction]], " \
                      "[[BoundThis]] and [[BoundArguments]]) - and the dispatch of step 5's " \
                      "HasOwnProperty(Target, \"length\")") \
    X(FB_LEN_OWN_GOT, "20.2.3.2 steps 4-5 (L is 0; targetHasLength arrives)") \
    X(FB_LEN_GOT,     "20.2.3.2 steps 6.a-7 (targetLen is Get(Target, \"length\"); L; SetFunctionLength(F, L))") \
    X(FB_NAME_GOT,    "20.2.3.2 steps 8-10 (targetName is Get(Target, \"name\"); SetFunctionName(F, " \
                      "targetName, \"bound\")) - and step 11, Return F")
enum { FBIND_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_func_bind_steps[] = { FBIND_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* steps 6-7's SetFunctionLength(F, L): the target's own `length`, reduced by the bound argument count and
   clamped at zero, and anything that is not a Number contributes nothing. CONSUMES len_val. */
static void js_func_bind_length(JSContext *ctx, JSFuncBind *s, JSValue len_val)
{
    int arg_count = s->arg_count;
    if (JS_VALUE_GET_TAG(len_val) == JS_TAG_INT) {
        int len1 = JS_VALUE_GET_INT(len_val);
        len_val = js_int32(len1 <= arg_count ? 0 : len1 - arg_count);
    } else if (JS_VALUE_GET_NORM_TAG(len_val) == JS_TAG_FLOAT64) {
        double d = JS_VALUE_GET_FLOAT64(len_val);
        if (isnan(d)) {
            d = 0.0;
        } else {
            d = trunc(d);
            if (d <= (double)arg_count)
                d = 0.0;               /* also converts -0 to +0 */
            else
                d -= (double)arg_count;
        }
        len_val = js_number(d);
    } else {
        JS_FreeValue(ctx, len_val);
        len_val = js_int32(0);
    }
    JS_DefinePropertyValue(ctx, s->func_obj, JS_ATOM_length, len_val, JS_PROP_CONFIGURABLE);
}

JSValue JS_NewBoundCall(JSContext *ctx, JSValueConst func, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSBoundFunction *bf;
    JSValue obj;
    int i;

    DCHECK(JS_IsFunction(ctx, func) && argc >= 0, "a bound call was made over something that cannot be called");
    obj = JS_NewObjectProtoClass(ctx, ctx->function_proto, JS_CLASS_BOUND_FUNCTION);
    if (JS_IsException(obj)) return obj;
    bf = js_malloc(ctx, sizeof(*bf) + (size_t)argc * sizeof(JSValue));
    if (!bf) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    bf->func_obj = js_dup(func);
    bf->this_val = js_dup(this_val);
    bf->argc = argc;
    for (i = 0; i < argc; i++) bf->argv[i] = js_dup(argv[i]);
    JS_VALUE_GET_OBJ(obj)->u.bound_function = bf;
    return obj;
}

static int js_func_bind_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSFuncBind *s = st;
    JSValue name1;

    if (s->hdr.stage == FB_ENTRY) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->func_obj = JS_UNDEFINED; s->result = JS_UNDEFINED;
        if (check_function(ctx, s->hdr.this_val))       /* step 2: IsCallable(Target) */
            return -1;
        /* 10.4.1.3 BoundFunctionCreate step 1: proto is ? Target.[[GetPrototypeOf]](), which is a Proxy's
           `getPrototypeOf` trap and, for an ordinary target, whatever the page set. The C body did not perform
           this operation AT ALL — it used the realm's %Function.prototype% unconditionally, so
           `Object.getPrototypeOf((new (class extends Function{})("")).bind())` answered %Function.prototype%
           instead of the subclass's prototype, and a Proxy target's trap never ran. */
        s->hdr.stage = FB_PROTO_GOT;
        s->hdr.cb_coerce[0] = s->hdr.this_val;   /* borrowed: the machine holds the target across the request */
        *out_cb = s->hdr.cb_coerce; *out_argc = 0;
        return 18;
    }
    if (s->hdr.stage == FB_PROTO_GOT) {
        JSBoundFunction *bf;
        JSObject *p;
        int i;
        if (JS_IsException(cb_result))
            return -1;
        s->func_obj = JS_NewObjectProtoClass(ctx, cb_result, JS_CLASS_BOUND_FUNCTION);   /* step 2 */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (JS_IsException(s->func_obj)) { s->func_obj = JS_UNDEFINED; return -1; }
        p = JS_VALUE_GET_OBJ(s->func_obj);
        p->is_constructor = JS_IsConstructor(ctx, s->hdr.this_val);
        s->arg_count = max_int(0, s->hdr.argc - 1);
        bf = js_malloc(ctx, sizeof(*bf) + s->arg_count * sizeof(JSValue));
        if (!bf)
            return -1;
        bf->func_obj = js_dup(s->hdr.this_val);
        bf->this_val = js_dup(step_arg(&s->hdr, 0));
        bf->argc = s->arg_count;
        for (i = 0; i < s->arg_count; i++)
            bf->argv[i] = js_dup(step_arg(&s->hdr, i + 1));
        p->u.bound_function = bf;                  /* 10.4.1.3 steps 3-6: the bound target, this and arguments */
        /* step 5: HasOwnProperty(Target, "length"). Only whether it EXISTS is wanted, so the ordinary
           descriptor-object answer says it — undefined means absent. Dispatched here rather than from a stage
           of its own: a stage with no request left in it is one nothing can park at. */
        s->hdr.stage = FB_LEN_OWN_GOT;
        s->hdr.cb_coerce[0] = s->hdr.this_val;   /* borrowed: the machine holds the target across the request */
        *out_cb = s->hdr.cb_coerce; *out_argc = (int)JS_ATOM_length;
        return 12;
    }
    switch (s->hdr.stage) {
    case FB_LEN_OWN_GOT: {
        bool has_len;
        if (JS_IsException(cb_result))
            return -1;
        has_len = !JS_IsUndefined(cb_result);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (has_len) {
            s->hdr.stage = FB_LEN_GOT;               /* step 6.a: Get(Target, "length") */
            s->hdr.cb_coerce[0] = s->hdr.this_val;
            *out_cb = s->hdr.cb_coerce; *out_argc = (int)JS_ATOM_length;
            return 6;
        }
        js_func_bind_length(ctx, s, js_int32(0));    /* step 4: L is 0 when the target has no own length */
        goto want_name;
    }
    case FB_LEN_GOT:
        if (JS_IsException(cb_result))
            return -1;
        js_func_bind_length(ctx, s, cb_result);      /* consumes it */
        cb_result = JS_UNDEFINED;
    want_name:
        s->hdr.stage = FB_NAME_GOT;                  /* step 8: Get(Target, "name") */
        s->hdr.cb_coerce[0] = s->hdr.this_val;
        *out_cb = s->hdr.cb_coerce; *out_argc = (int)JS_ATOM_name;
        return 6;
    default:
        DCHECK(s->hdr.stage == FB_NAME_GOT, "Function.prototype.bind's machine resumed in no stage");
        if (JS_IsException(cb_result))
            return -1;
        name1 = cb_result;
        if (!JS_IsString(name1)) {                   /* step 9 */
            JS_FreeValue(ctx, name1);
            name1 = js_empty_string(ctx->rt);
        }
        name1 = JS_ConcatString3(ctx, "bound ", name1, "");   /* step 10: SetFunctionName(F, name, "bound") */
        if (JS_IsException(name1))
            return -1;
        JS_DefinePropertyValue(ctx, s->func_obj, JS_ATOM_name, name1, JS_PROP_CONFIGURABLE);
        s->result = s->func_obj;
        s->func_obj = JS_UNDEFINED;
        return 0;
    }
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_func_bind_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSFuncBind *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->func_obj);
}

static JSValue js_func_bind_fini(JSContext *ctx, void *st, bool take_result)
{
    JSFuncBind *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* the function's OWN `name` data property, with no prototype walk, no accessor and no trap. An AUTOINIT slot is
   instantiated first — that is engine code (the property list's own initialiser) and invokes nothing. */
static JSValue js_function_own_name(JSContext *ctx, JSValueConst func)
{
    JSObject *p = JS_VALUE_GET_OBJ(func);
    JSProperty *pr;
    JSShapeProperty *prs = find_own_property(&pr, p, JS_ATOM_name);
    if (prs && (prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
        if (JS_AutoInitProperty(ctx, p, JS_ATOM_name, pr, prs))
            return JS_EXCEPTION;
        prs = find_own_property(&pr, p, JS_ATOM_name);
    }
    if (prs && (prs->flags & JS_PROP_TMASK) == 0 && JS_VALUE_GET_TAG(pr->u.value) == JS_TAG_STRING)
        return js_dup(pr->u.value);
    return js_empty_string(ctx->rt);
}

/* Can this built-in's `name` appear as the PropertyName of a NativeFunction? An optional `get `/`set ` prefix
   is the production's NativeFunctionAccessor and is not part of the name; what follows must be an
   IdentifierName, or the `[…]` form the engine already uses for a symbol-keyed accessor, or nothing. */
static bool js_native_name_is_emittable(JSContext *ctx, JSValueConst name)
{
    JSString *p;
    int i, len, c;

    if (!JS_IsString(name))
        return false;
    p = JS_VALUE_GET_STRING(name);
    len = p->len;
    i = 0;
    if (len >= 4 && (string_get(p, 0) == 'g' || string_get(p, 0) == 's')) {
        if ((string_get(p, 0) == 'g' && string_get(p, 1) == 'e' && string_get(p, 2) == 't' &&
             string_get(p, 3) == ' ') ||
            (string_get(p, 0) == 's' && string_get(p, 1) == 'e' && string_get(p, 2) == 't' &&
             string_get(p, 3) == ' '))
            i = 4;
    }
    if (i == len)
        return true;                      /* accessor with no name — the production allows it */
    if (string_get(p, i) == '[')
        return true;                      /* the computed form, already emitted for symbol keys */
    if (!lre_js_is_ident_first(string_get(p, i)))
        return false;
    for (i++; i < len; i++) {
        c = string_get(p, i);
        if (!lre_js_is_ident_next(c))
            return false;
    }
    return true;
}

/* "get " / "set " / "" — the part of an unemittable name that IS grammar and has to survive. */
static JSValue js_native_accessor_prefix(JSContext *ctx, JSValueConst func_obj)
{
    JSValue name = js_function_own_name(ctx, func_obj);
    JSString *p;
    bool is_get, is_set;

    if (!JS_IsString(name)) {
        JS_FreeValue(ctx, name);
        return JS_AtomToString(ctx, JS_ATOM_empty_string);
    }
    p = JS_VALUE_GET_STRING(name);
    is_get = p->len >= 4 && string_get(p, 0) == 'g' && string_get(p, 1) == 'e' &&
             string_get(p, 2) == 't' && string_get(p, 3) == ' ';
    is_set = p->len >= 4 && string_get(p, 0) == 's' && string_get(p, 1) == 'e' &&
             string_get(p, 2) == 't' && string_get(p, 3) == ' ';
    JS_FreeValue(ctx, name);
    if (is_get)
        return JS_NewString(ctx, "get ");
    if (is_set)
        return JS_NewString(ctx, "set ");
    return JS_AtomToString(ctx, JS_ATOM_empty_string);
}

static JSValue js_function_toString(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSObject *p;
    JSFunctionKindEnum func_kind = JS_FUNC_NORMAL;

    if (check_function(ctx, this_val))
        return JS_EXCEPTION;

    p = JS_VALUE_GET_OBJ(this_val);
    if (js_class_has_bytecode(p->class_id)) {
        JSFunctionBytecode *b = p->u.func.function_bytecode;
        /* `b->source` must be pure ASCII or UTF-8 encoded */
        if (b->source)
            return JS_NewStringLen(ctx, b->source, b->source_len);
    }
    {
        JSValue name;
        const char *pref, *suff;

        switch(func_kind) {
        default:
        case JS_FUNC_NORMAL:
            pref = "function ";
            break;
        case JS_FUNC_GENERATOR:
            pref = "function *";
            break;
        case JS_FUNC_ASYNC:
            pref = "async function ";
            break;
        case JS_FUNC_ASYNC_GENERATOR:
            pref = "async function *";
            break;
        }
        suff = "() {\n    [native code]\n}";
        /* 20.2.3.5 step 5: for a function with no [[SourceText]] the representation is IMPLEMENTATION-DEFINED,
           required only to conform to the NativeFunction production — so the name in it is this engine's choice,
           and reading it with JS_GetProperty made that choice the page's code. A Proxy's `get` trap ran from a C
           activation with no flow base, and an accessor `name` anywhere on the chain did the same. The OWN data
           property is the same string for every function that reaches here (a C builtin, a bound function, a
           class) and reading it invokes nothing, so the read is not routed — it is REMOVED. A Proxy has no own
           `name` slot and gets the empty one, which is what other engines produce for it. */
        name = js_function_own_name(ctx, this_val);
        /* …and it must still PARSE as one. A built-in's name can be anything a property key can be, and
           RegExp's legacy statics put `get $&`, `get $+`, "get $`" and `get $'` on an intrinsic: none of those
           tails is an IdentifierName, so emitting them produced `function get $&() { [native code] }`, which
           is not a NativeFunction. PropertyName is OPTIONAL in the production, so a name that cannot appear in
           it is omitted rather than mangled — the accessor prefix stays, because that part is grammar. */
        if (!js_native_name_is_emittable(ctx, name)) {
            JS_FreeValue(ctx, name);
            name = js_native_accessor_prefix(ctx, this_val);
        }
        return JS_ConcatString3(ctx, pref, name, suff);
    }
}

/* 13.10.2 InstanceofOperator and 7.3.21 OrdinaryHasInstance — ONE machine with two entries, because step 2 of
   the second IS the first: a BOUND function's `instanceof` recurses through the whole operator on its target,
   re-reading @@hasInstance there. The arg picks the entry; Function.prototype[@@hasInstance] is the ordinary
   one, and the operator is the other.

   Three of the page's operations live in them and every one ran from C: GetMethod(target, @@hasInstance), the
   `Get(C, "prototype")` of step 4, and a [[GetPrototypeOf]] per PROXY link of the walk. So

       ({}) instanceof new Proxy(function(){}, {get(){for(;;){}}})

   had no flow base. The operator ALSO read @@hasInstance a second time on the fallback path, through
   JS_IsInstanceOf, which 13.10.2 performs once.

   Step 2's GetMethod contract — a present, non-nullish, NON-CALLABLE @@hasInstance is a TypeError — is
   unchanged and was already right: the old path reached it through step 4's IsCallable check instead, and a
   probe suggesting otherwise had used `f[Symbol.hasInstance] = 1`, which sets nothing, because
   Function.prototype's is non-writable. */
typedef struct JSInstanceOf {
    JSStepHdr hdr;      /* MUST be first. arg: INSTOF_OPERATOR or INSTOF_ORDINARY */
    JSValue ctor;       /* C — what the value is tested against (owned) */
    JSValue val;        /* O — the value, then the chain link being walked (owned) */
    JSValue proto;      /* P, from step 4 (owned) */
    JSValue handler;    /* @@hasInstance, held across its call (owned) */
    JSValue test;       /* the handler's result while step 3.a's ToBoolean is asked (owned), else JS_UNINITIALIZED */
    JSValue result;     /* DONE (owned) */
    uint8_t mode;       /* the CURRENT entry: a bound target restarts at the operator whichever entry began */
} JSInstanceOf;
#define INSTOF_OPERATOR 0   /* 13.10.2, from the top */
#define INSTOF_ORDINARY 1   /* 7.3.21 directly — what Function.prototype[@@hasInstance] is */
/* ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). TWO algorithms and
   ONE list, unlike pop/shift: this machine does not pick one at its definition and stay there — 7.3.21 step 2.b
   RE-ENTERS 13.10.2 on a bound function's target, so a flow that started in either can be parked in the other,
   and each label names the operation the stage belongs to. (The prose here called OrdinaryHasInstance 7.3.20.) */
#define INSTOF_STAGES(X) \
    X(IO_ENTRY,     "13.10.2 steps 1-2 (target is an Object; the dispatch of GetMethod(target, " \
                    "%Symbol.hasInstance%)), re-entered here by 7.3.21 step 2.b for a bound target") \
    X(IO_HI_GOT,    "13.10.2 steps 2-3 (instOfHandler arrives; when it is not undefined, the dispatch of " \
                    "Call(instOfHandler, target, <<V>>))") \
    X(IO_CALL_GOT,  "13.10.2 step 3.a (ToBoolean of the handler's result)") \
    X(IO_ORDINARY,  "13.10.2 steps 4-5 -> 7.3.21 steps 1-4 (IsCallable(C); a bound C re-enters 13.10.2 on its " \
                    "[[BoundTargetFunction]]; O is an Object; the dispatch of Get(C, \"prototype\"))") \
    X(IO_PROTO_GOT, "7.3.21 steps 5-6.a (P arrives and is an Object; the dispatch of O.[[GetPrototypeOf]]())") \
    X(IO_LINK_GOT,  "7.3.21 steps 6.a-6.c (the link arrives; null returns false, SameValue(P, O) returns true, " \
                    "otherwise the next O.[[GetPrototypeOf]]())")
enum { INSTOF_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_instanceof_steps[] = { INSTOF_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* 7.3.21's step 6 numbers its completions through the SHARED prototype-walk numbering above (PROTO_LINK_*),
   because 20.1.3.3 step 3 is the same three-step loop and the two must not number it differently. The op
   string below is what keeps the two walks' constraint keys apart. */

/* steps 6.a-c over ORDINARY links, which have no [[GetPrototypeOf]] of their own to run: the shape's proto is
   the answer. Returns 0 = keep walking from s->val (a PROXY link needs the request), 1 = decided. */
static int js_instanceof_walk(JSContext *ctx, JSInstanceOf *s)
{
    JSObject *proto = JS_VALUE_GET_OBJ(s->proto);
    while (JS_VALUE_GET_TAG(s->val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(s->val);
        JSObject *nxt;
        if (p->class_id == JS_CLASS_PROXY)
            return 0;                       /* its [[GetPrototypeOf]] is the page's: ask for it */
        /* AND SO DOES A CLASS THAT COMPUTES ITS PROTOTYPE (JSClassExoticMethods.get_prototype) need the
           request, for the other half of the same reason: step 6.a is `O.[[GetPrototypeOf]]()`, and the shape
           link this loop follows is not that class's answer. The request's own trapless arm calls
           JS_GetPrototype, so ONE function computes the link whichever route reaches it — reading the hook
           here instead would be a second opinion, and a walk that decided `w instanceof Window` from the
           WindowProxy surface object is exactly the wrong answer this exists to stop. */
        if (js_exotic_getproto(ctx->rt, p))
            return 0;
        nxt = p->shape->proto;
        if (!nxt) { s->result = JS_FALSE; return 1; }          /* step 6.b */
        if (nxt == proto) { s->result = JS_TRUE; return 1; }   /* step 6.c */
        JS_FreeValue(ctx, s->val);
        s->val = js_dup(JS_MKPTR(JS_TAG_OBJECT, nxt));
    }
    s->result = JS_FALSE;
    return 1;
}

/* step_tobool_run's `op` for 13.10.2 InstanceofOperator step 3.a. */
#define IO_TEST_OP "InstanceofOperator step 3.a, ToBoolean of the @@hasInstance result"

static int js_instanceof_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSInstanceOf *s = st;

    if (s->hdr.stage == IO_ENTRY) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        s->result = JS_UNDEFINED; s->proto = JS_UNDEFINED; s->handler = JS_UNDEFINED; s->test = JS_UNINITIALIZED;
        s->mode = (uint8_t)s->hdr.arg;
        if (s->mode == INSTOF_ORDINARY) {
            /* Function.prototype[@@hasInstance](V): the receiver is C and the argument is O. */
            s->ctor = js_dup(s->hdr.this_val);
            s->val = js_dup(step_arg(&s->hdr, 0));
        } else {
            /* the operator's shape: [V, target] — the machine is created with them as its arguments. */
            s->val = js_dup(step_arg(&s->hdr, 0));
            s->ctor = js_dup(step_arg(&s->hdr, 1));
        }
    }
    for (;;) {
        if (s->hdr.stage == IO_ENTRY) {
            if (s->mode == INSTOF_ORDINARY) { s->hdr.stage = IO_ORDINARY; continue; }
            if (!JS_IsObject(s->ctor)) {                     /* step 1 */
                JS_ThrowTypeError(ctx, "invalid 'instanceof' right operand");
                return -1;
            }
            s->hdr.stage = IO_HI_GOT;                        /* step 2: GetMethod(target, @@hasInstance) */
            s->hdr.cb_coerce[0] = s->ctor;
            *out_cb = s->hdr.cb_coerce; *out_argc = (int)JS_ATOM_Symbol_hasInstance;
            return 6;
        }
        if (s->hdr.stage == IO_HI_GOT) {
            if (JS_IsException(cb_result)) return -1;
            s->handler = cb_result; cb_result = JS_UNDEFINED;
            if (JS_IsUndefined(s->handler) || JS_IsNull(s->handler)) {
                JS_FreeValue(ctx, s->handler); s->handler = JS_UNDEFINED;
                s->hdr.stage = IO_ORDINARY;                  /* steps 4-5 */
                continue;
            }
            if (!JS_IsFunction(ctx, s->handler)) {
                JS_ThrowTypeError(ctx, "not a function");   /* GetMethod step 3 */
                return -1;
            }
            s->hdr.stage = IO_CALL_GOT;                      /* step 3: Call(handler, target, <<V>>) */
            s->hdr.cb_coerce[0] = s->ctor;
            s->hdr.cb_coerce[1] = s->handler;
            s->hdr.cb_coerce[2] = s->val;
            *out_cb = s->hdr.cb_coerce; *out_argc = 1;
            return 3;
        }
        if (s->hdr.stage == IO_CALL_GOT) {
            /* Step 3.a's ToBoolean over unknown input is a fork through the branch seam: §7.1.2 answers true
               for every concolic. The handler's result is held on the state because the ask suspends and a
               sibling re-enters here with undefined placed. */
            int b, r;

            if (JS_IsUninitialized(s->test)) {
                if (JS_IsException(cb_result)) return -1;
                s->test = cb_result;
            } else {
                DCHECK(JS_IsUndefined(cb_result),
                       "an instanceof coercion was re-entered with a value placed while its operand is held");
                JS_FreeValue(ctx, cb_result);
            }
            cb_result = JS_UNDEFINED;
            r = step_tobool_run(ctx, &s->hdr, s->test, IO_TEST_OP, &b);
            if (r) return r < 0 ? -1 : r;
            JS_FreeValue(ctx, s->test);
            s->test = JS_UNINITIALIZED;
            s->result = js_bool(b);
            return 0;
        }
        if (s->hdr.stage == IO_ORDINARY) {
            JSObject *p;
            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
            if (!JS_IsFunction(ctx, s->ctor)) {
                if (s->mode == INSTOF_OPERATOR) {            /* 13.10.2 step 4 */
                    JS_ThrowTypeError(ctx, "invalid 'instanceof' right operand");
                    return -1;
                }
                s->result = JS_FALSE;                        /* 7.3.21 step 1 */
                return 0;
            }
            p = JS_VALUE_GET_OBJ(s->ctor);
            if (p->class_id == JS_CLASS_BOUND_FUNCTION) {
                /* step 2: the WHOLE operator on the bound target, @@hasInstance read again and all. */
                JSBoundFunction *bf = p->u.bound_function;
                JSValue tgt = js_dup(bf->func_obj);
                JS_FreeValue(ctx, s->ctor);
                s->ctor = tgt;
                s->mode = INSTOF_OPERATOR;
                s->hdr.stage = IO_ENTRY;
                continue;
            }
            if (!JS_IsObject(s->val)) {                      /* step 3 */
                s->result = JS_FALSE;
                return 0;
            }
            s->hdr.stage = IO_PROTO_GOT;                     /* step 4: Get(C, "prototype") */
            s->hdr.cb_coerce[0] = s->ctor;
            *out_cb = s->hdr.cb_coerce; *out_argc = (int)JS_ATOM_prototype;
            return 6;
        }
        if (s->hdr.stage == IO_PROTO_GOT) {
            if (JS_IsException(cb_result)) return -1;
            s->proto = cb_result; cb_result = JS_UNDEFINED;
            if (!JS_IsObject(s->proto)) {                    /* step 5 */
                JS_ThrowTypeError(ctx, "operand 'prototype' property is not an object");
                return -1;
            }
            s->hdr.stage = IO_LINK_GOT;
            if (js_instanceof_walk(ctx, s)) return 0;
            s->hdr.cb_coerce[0] = s->val;                    /* step 6.a on a PROXY link: its trap */
            *out_cb = s->hdr.cb_coerce; *out_argc = 0;
            return 18;
        }
        DCHECK(s->hdr.stage == IO_LINK_GOT, "the instanceof machine resumed in no stage");
        /* THREE WAYS TO ARRIVE HERE AND ONLY ONE OF THEM CARRIES A LINK. The ordinary one is step 6.a's
           request answering, whose result IS the link. The second is this stage's own fork being answered,
           and there `cb_result` is the driver's filler rather than a value from the algorithm — taking it as
           the link would overwrite the very operand the fork was asked about. THE THIRD is the SIBLING that
           fork minted, resuming at the ask. `fork_phase` was what told them apart and it CANNOT — it is
           FORK_PH_ASK on the request-answered entry AND in the sibling's snapshot, deliberately, so the
           sentence that stood here ("FORK_PH_ASK on every entry that is not a fork's delivery") was true of
           the value and false of what it was read to mean. step_fork_pending is the predicate; this arm read
           the sibling as a link, and js_instanceof_walk takes the `while` false for a non-object, so
           `x instanceof C` answered FALSE on the very arm the fork existed to explore. */
        if (!step_fork_pending(&s->hdr)) {
            if (JS_IsException(cb_result)) return -1;
            step_adopt_link(ctx, &s->val, &cb_result);
            STEP_CHECK_PROTO_LINK(s->val);
        }
        /* STEP 6.b AND 6.c OVER AN UNKNOWN LINK ARE NOT DECIDABLE IN C, so they are asked. Both are identity
           comparisons — `is null`, and SameValue against `proto` — and an unknown answers neither: it is an
           ordinary Object at the C level, so the tests below would silently take the "keep walking" arm at
           every link and this Repeat would never terminate. The operand is the LINK and never its depth, so
           the constraint key the seam composes is a fact about a value that survives a park; a position in
           the chain is an ordinal over something the page can lengthen, and a replayed ordinal names a
           different link than the one that recorded it.
           `s->val` IS WHERE THE OPERAND HAS TO LIVE. step_fork_run borrows it for the length of the request
           and the sibling's snapshot is taken at the ask, so it must be reachable from js_instanceof_visit —
           which is what makes this the machine's own field rather than a C local. */
        if (js_value_is_concolic(s->val)) {
            int arm, r;
            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
            /* `real` IS UNSTATED because nothing observed says which completion a real session reaches: the
               derived link carries no example, so this machine has no ground to declare one and a claim here
               would be a primacy nothing computed. */
            r = step_fork_run(ctx, &s->hdr, s->val, "OrdinaryHasInstance step 6", 3,
                              JS_OUTCOME_REAL_UNSTATED, &arm);
            if (r) return r;
            if (arm == PROTO_LINK_END) { s->result = JS_FALSE; return 0; }    /* step 6.b */
            if (arm == PROTO_LINK_MATCH) { s->result = JS_TRUE; return 0; }   /* step 6.c */
            /* PROTO_LINK_MORE — step 6.a on the next link. The ask is re-entered at the link that arrives,
               under ITS identity, so an unknown chain is a chain of forks and not one question repeated. */
            s->hdr.cb_coerce[0] = s->val;
            *out_cb = s->hdr.cb_coerce; *out_argc = 0;
            return 18;
        }
        if (JS_IsNull(s->val)) { s->result = JS_FALSE; return 0; }          /* step 6.b */
        if (JS_VALUE_GET_TAG(s->val) == JS_TAG_OBJECT
            && JS_VALUE_GET_OBJ(s->val) == JS_VALUE_GET_OBJ(s->proto)) {    /* step 6.c */
            s->result = JS_TRUE;
            return 0;
        }
        if (js_instanceof_walk(ctx, s)) return 0;
        s->hdr.cb_coerce[0] = s->val;
        *out_cb = s->hdr.cb_coerce; *out_argc = 0;
        return 18;
    }
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). @@hasInstance is the page's method and the prototype chain walk is a series of [[GetPrototypeOf]] a Proxy can trap. */
static void js_instanceof_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSInstanceOf *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->ctor);
    v->val(ctx, &s->val);
    v->val(ctx, &s->proto);
    v->val(ctx, &s->handler);
    v->val(ctx, &s->test);
}

static JSValue js_instanceof_fini(JSContext *ctx, void *st, bool take_result)
{
    JSInstanceOf *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static const JSCFunctionListEntry js_function_proto_funcs[] = {
    JS_CFUNC_STEP_DEF("call", 1, STEPDEF_FUNCTION_CALL ),
    JS_CFUNC_STEP_DEF("apply", 2, STEPDEF_FUNCTION_APPLY ),
    JS_CFUNC_STEP_DEF("bind", 1, STEPDEF_FUNC_BIND ),
    JS_CFUNC_DEF("toString", 0, js_function_toString ),
    JS_CFUNC_STEP_DEF("[Symbol.hasInstance]", 1, STEPDEF_ORDINARY_HAS_INSTANCE ),
    JS_CGETSET_DEF("fileName", js_function_proto_fileName, NULL ),
    JS_CGETSET_MAGIC_DEF("lineNumber", js_function_proto_int32, NULL,
                         offsetof(JSFunctionBytecode, line_num)),
    JS_CGETSET_MAGIC_DEF("columnNumber", js_function_proto_int32, NULL,
                         offsetof(JSFunctionBytecode, col_num)),
};


