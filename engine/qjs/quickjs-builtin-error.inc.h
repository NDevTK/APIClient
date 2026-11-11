/* ONE MACHINE, THREE CONSTRUCTORS. 20.5.1.1 Error, 20.5.6.1.1 NativeError and 20.5.7.1.1 AggregateError are
   the same five steps, of which AggregateError adds two more — its step 5 turns `errors` into a List, which is
   7.4.2's GetIterator and then 7.4.15's IteratorToList, and every one of those is the page's code. The stages
   are ONE list expanded once per algorithm, so a step cannot move in one and not the others. The first four are
   shared by all three; the iterator walk is AggregateError's, and the shared arms name the step of ITS
   numbering they are reached at, because they are literally the same steps. */
#define ERRCTOR_STAGES(X, WHERE) \
    X(ERRC_PH_CREATE,   WHERE " step 2 (OrdinaryCreateFromConstructor: Get(newTarget, \"prototype\"))") \
    X(ERRC_PH_MSG,      WHERE " step 3.a (msg is ToString(message))") \
    X(ERRC_PH_HASCAUSE, "20.5.8.1 InstallErrorCause step 1 (HasProperty(options, \"cause\")), reached from " \
                        WHERE " step 4") \
    X(ERRC_PH_GETCAUSE, "20.5.8.1 InstallErrorCause step 1.a (Get(options, \"cause\"))") \
    X(ERRC_PH_ITERM,    "7.4.2 GetIterator step 1 (GetMethod(errors, %Symbol.iterator%)), reached from " \
                        "20.5.7.1.1 step 5") \
    X(ERRC_PH_ITEROBJ,  "7.4.1 GetIteratorFromMethod step 1 (Call(method, errors))") \
    X(ERRC_PH_NEXTM,    "7.4.1 GetIteratorFromMethod step 2 (Get(iterator, \"next\"))") \
    X(ERRC_PH_CALLNEXT, "7.4.4 IteratorStepValue step 2 (Call(nextMethod, iterator)), from 7.4.15 " \
                        "IteratorToList step 3.a") \
    X(ERRC_PH_DONE,     "7.4.4 IteratorStepValue step 5 (IteratorComplete: Get(result, \"done\"))") \
    X(ERRC_PH_VALUE,    "7.4.4 IteratorStepValue step 8 (IteratorValue: Get(result, \"value\"))") \
    X(ERRC_PH_FINISH,   "20.5.7.1.1 step 6 (define \"errors\" from the collected List), which for " WHERE \
                        " is step 5 (return O)")
enum { ERRCTOR_STAGES(JS_STEP_STAGE_ENUM, "") };
static const char *const js_error_ctor_steps[] = {
    ERRCTOR_STAGES(JS_STEP_STAGE_LABEL, "20.5.1.1 Error / 20.5.6.1.1 NativeError / 20.5.7.1.1 AggregateError")
    NULL };

/* which argument carries `message`, and where `options` starts, per kind — the whole of the spec's per-kind
   branching before the tail. */
static int js_error_ctor_opts(int kind)
{
    if (kind == JS_AGGREGATE_ERROR) return 2;
    if (kind == JS_SUPPRESSED_ERROR) return 3;
    return 1;
}

static int js_error_ctor_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSErrorCtor *s = st;
    int kind = s->hdr.arg, opts = js_error_ctor_opts(kind);
    int r;

    if (s->hdr.stage == ERRC_PH_CREATE) {
        JSValue proto;
        JSValueConst nt;
        if (s->hdr.get_phase == GET_PH_START) {
            /* the ONE-TIME init, guarded by the sub-sequence's own cursor rather than by the stage: the
               `prototype` read below suspends WITHOUT advancing the stage, so a stage-only guard would skip the
               resume entirely — and re-running the init would clobber everything the state holds.
               FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
            s->obj = JS_UNDEFINED; s->held = JS_UNDEFINED; s->iter = JS_UNDEFINED;
            s->next = JS_UNDEFINED; s->arr = JS_UNDEFINED;
            s->cb[0] = JS_UNDEFINED; s->cb[1] = JS_UNDEFINED;
            s->k = 0; s->present = 0;
        }
        /* step 1: called as a FUNCTION, the new target is the active function — which for a step machine is the
           callee it captured, not whatever frame happens to be running. */
        nt = JS_IsUndefined(s->hdr.this_val) ? s->hdr.func_obj : s->hdr.this_val;
        r = step_getprop_run(ctx, &s->hdr, nt, JS_ATOM_prototype, cb_result, &proto, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        if (!JS_IsObject(proto)) {
            /* OrdinaryCreateFromConstructor's fallback: the intrinsic prototype of the NEW TARGET's realm. */
            JSContext *realm;
            JS_FreeValue(ctx, proto);
            realm = JS_GetFunctionRealm(ctx, nt);
            if (!realm) return -1;
            proto = js_dup(kind == JS_PLAIN_ERROR ? realm->class_proto[JS_CLASS_ERROR]
                                                  : realm->native_error_proto[kind]);
        }
        s->obj = JS_NewObjectProtoClass(ctx, proto, JS_CLASS_ERROR);
        JS_FreeValue(ctx, proto);
        if (JS_IsException(s->obj)) { s->obj = JS_UNDEFINED; return -1; }
        s->hdr.stage = ERRC_PH_MSG;
    }
    if (s->hdr.stage == ERRC_PH_MSG) {
        JSValueConst message = step_arg(&s->hdr, opts - 1);
        if (!JS_IsUndefined(message)) {
            /* 20.5.1.1 step 3 OVER UNKNOWN INPUT: `message` is coerced, and an Error built out of unknown
               input has an unknown message rather than none. Errors made of attacker input are a real path —
               a bundle wraps a failed parse of a URL parameter and logs or displays `e.message`, which is a
               sink. Defining the derived unknown keeps the source attached to it. */
            {
                JSValue c = js_concolic_derive(ctx, message, "Error.message", js_concolic_operand_str(ctx, message));
                if (!JS_IsUninitialized(c)) {
                    JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
                    JS_DefinePropertyValue(ctx, s->obj, JS_ATOM_message, c,
                                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
                    s->hdr.stage = ERRC_PH_HASCAUSE;
                    goto errc_after_msg;
                }
            }
            r = step_tostring_run(ctx, &s->hdr, message, cb_result, &s->held, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            JS_DefinePropertyValue(ctx, s->obj, JS_ATOM_message, s->held,
                                   JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
            s->held = JS_UNDEFINED;   /* the define consumed it */
        }
        s->hdr.stage = ERRC_PH_HASCAUSE;
    }
errc_after_msg:
    if (s->hdr.stage == ERRC_PH_HASCAUSE) {
        /* InstallErrorCause: only when `options` is an Object, and then a HasProperty before the Get. */
        if (s->hdr.argc > opts && JS_VALUE_GET_TAG(s->hdr.argv[opts]) == JS_TAG_OBJECT) {
            r = step_hasprop_run(ctx, &s->hdr, s->hdr.argv[opts], JS_ATOM_cause, cb_result,
                                 &s->present, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
        } else {
            s->present = 0;
        }
        s->hdr.stage = ERRC_PH_GETCAUSE;
    }
    if (s->hdr.stage == ERRC_PH_GETCAUSE) {
        if (s->present) {
            r = step_getprop_run(ctx, &s->hdr, s->hdr.argv[opts], JS_ATOM_cause, cb_result,
                                 &s->held, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            JS_DefinePropertyValue(ctx, s->obj, JS_ATOM_cause, s->held,
                                   JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
            s->held = JS_UNDEFINED;
        }
        s->hdr.stage = (kind == JS_AGGREGATE_ERROR) ? ERRC_PH_ITERM : ERRC_PH_FINISH;
    }
    /* ---- AggregateError's IteratorToList(GetIterator(errors, sync)) ---- */
    if (s->hdr.stage == ERRC_PH_ITERM) {
        JSValue m = JS_UNDEFINED;
        r = step_getprop_run(ctx, &s->hdr, step_arg(&s->hdr, 0), JS_ATOM_Symbol_iterator, cb_result,
                             &m, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        if (JS_IsUndefined(m) || JS_IsNull(m) || !JS_IsFunction(ctx, m)) {
            JS_FreeValue(ctx, m);
            JS_ThrowTypeError(ctx, "value is not iterable");
            return -1;
        }
        s->arr = JS_NewArray(ctx);
        if (JS_IsException(s->arr)) { s->arr = JS_UNDEFINED; JS_FreeValue(ctx, m); return -1; }
        s->cb[0] = js_dup(step_arg(&s->hdr, 0));
        s->cb[1] = m;
        s->hdr.stage = ERRC_PH_ITEROBJ;
        *out_cb = s->cb; *out_argc = 0;
        return 3;
    }
    if (s->hdr.stage == ERRC_PH_ITEROBJ) {
        JS_FreeValue(ctx, s->cb[0]); s->cb[0] = JS_UNDEFINED;
        JS_FreeValue(ctx, s->cb[1]); s->cb[1] = JS_UNDEFINED;
        if (!JS_IsObject(cb_result)) {   /* GetIterator step 5 */
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeErrorNotAnObject(ctx);
            return -1;
        }
        s->iter = cb_result; cb_result = JS_UNDEFINED;
        s->hdr.stage = ERRC_PH_NEXTM;
    }
    if (s->hdr.stage == ERRC_PH_NEXTM) {
        r = step_getprop_run(ctx, &s->hdr, s->iter, JS_ATOM_next, cb_result, &s->next, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = ERRC_PH_CALLNEXT;
    }
    /* the collect loop — entered only by AggregateError; every other kind reached FINISH above */
    while (s->hdr.stage != ERRC_PH_FINISH) {
        if (s->hdr.stage == ERRC_PH_CALLNEXT) {
            s->cb[0] = js_dup(s->iter);
            s->cb[1] = js_dup(s->next);
            s->hdr.stage = ERRC_PH_DONE;
            *out_cb = s->cb; *out_argc = 0;
            return 3;
        }
        if (s->hdr.stage == ERRC_PH_DONE) {
            if (JS_IsUndefined(s->held)) {   /* the .next() call just returned */
                JS_FreeValue(ctx, s->cb[0]); s->cb[0] = JS_UNDEFINED;
                JS_FreeValue(ctx, s->cb[1]); s->cb[1] = JS_UNDEFINED;
                if (!JS_IsObject(cb_result)) {   /* IteratorNext step 3 */
                    JS_FreeValue(ctx, cb_result);
                    JS_ThrowTypeError(ctx, "iterator result not an object");
                    return -1;
                }
                s->held = cb_result; cb_result = JS_UNDEFINED;
            }
            {
                JSValue dv = JS_UNDEFINED;
                r = step_getprop_run(ctx, &s->hdr, s->held, JS_ATOM_done, cb_result, &dv, out_cb, out_argc);
                cb_result = JS_UNDEFINED;
                if (r) return r < 0 ? -1 : r;
                if (JS_ToBoolFree(ctx, dv)) {
                    JS_FreeValue(ctx, s->held); s->held = JS_UNDEFINED;
                    s->hdr.stage = ERRC_PH_FINISH;
                    break;
                }
            }
            s->hdr.stage = ERRC_PH_VALUE;
        }
        DCHECK(s->hdr.stage == ERRC_PH_VALUE, "the Error constructor's errors walk is in an unknown stage");
        {
            JSValue vv = JS_UNDEFINED;
            r = step_getprop_run(ctx, &s->hdr, s->held, JS_ATOM_value, cb_result, &vv, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            JS_FreeValue(ctx, s->held); s->held = JS_UNDEFINED;
            if (JS_DefinePropertyValueInt64(ctx, s->arr, s->k, vv, JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                return -1;
            s->k++;
            s->hdr.stage = ERRC_PH_CALLNEXT;
        }
    }
    DCHECK(s->hdr.stage == ERRC_PH_FINISH, "the Error constructor finished in an unknown stage");
    if (kind == JS_AGGREGATE_ERROR) {
        JS_DefinePropertyValue(ctx, s->obj, JS_ATOM_errors, s->arr,
                               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        s->arr = JS_UNDEFINED;
    }
    if (kind == JS_SUPPRESSED_ERROR) {
        JS_DefinePropertyValue(ctx, s->obj, JS_ATOM_error, js_dup(step_arg(&s->hdr, 0)),
                               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        JS_DefinePropertyValue(ctx, s->obj, JS_ATOM_suppressed, js_dup(step_arg(&s->hdr, 1)),
                               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }
    /* SKIP the Error constructor's own activation: naming Error() as the place the error came from is the one
       thing a stack trace must not do. The skip had to be dropped when a step machine pushed no frame at all —
       it was eating the PAGE's frame instead — and comes back now that the machine has one of its own. */
    build_backtrace(ctx, s->obj, JS_UNDEFINED, NULL, 0, 0, JS_BACKTRACE_FLAG_SKIP_FIRST_LEVEL, NULL);
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The error it is building — which is also its result — the one
   value held across whichever suspension is in flight, AggregateError's iterator record and collected list, and
   the request buffer this machine dup'd into. */
static void js_error_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSErrorCtor *s = st;
    int i;
    v->val(ctx, &s->obj);
    v->val(ctx, &s->held);
    v->val(ctx, &s->iter);
    v->val(ctx, &s->next);
    v->val(ctx, &s->arr);
    for (i = 0; i < 2; i++) v->val(ctx, &s->cb[i]);
}

static JSValue js_error_ctor_fini(JSContext *ctx, void *st, bool take_result)
{
    JSErrorCtor *s = st;
    JSValue r = take_result ? s->obj : JS_UNDEFINED;
    if (take_result) s->obj = JS_UNDEFINED;
    return r;
}

/* 20.5.3.4, one operation at a time. The two READS and the two ToStrings are the page's code; the stage says
   which of the four an answer belongs to, and `raw` holds a property between its read and its coercion because
   that coercion suspends. */
#define ETS_STAGES(X) \
    X(ETS_NAME_GET, "20.5.3.4 steps 2-3 (O must be an Object; name is Get(O, \"name\"))") \
    X(ETS_NAME_STR, "20.5.3.4 step 4 (an undefined name is \"Error\"; otherwise ToString(name))") \
    X(ETS_MSG_GET,  "20.5.3.4 step 5 (msg is Get(O, \"message\"))") \
    X(ETS_MSG_STR,  "20.5.3.4 steps 6-9 (an undefined msg is \"\"; otherwise ToString(msg), then the two " \
                    "empty-string tests and the concatenation)")
enum { ETS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_error_tostring_steps[] = { ETS_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_error_tostring_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSErrToString *s = st;
    JSValue msg;
    int r;

    if (s->hdr.stage == ETS_NAME_GET) {
        if (s->hdr.get_phase == GET_PH_START) {
            s->result = JS_UNDEFINED; s->name = JS_UNDEFINED; s->raw = JS_UNDEFINED;
            if (!JS_IsObject(s->hdr.this_val)) {
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeErrorNotAnObject(ctx);
                return -1;
            }
        }
        r = step_getprop_run(ctx, &s->hdr, s->hdr.this_val, JS_ATOM_name, cb_result, &s->raw, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = ETS_NAME_STR;
    }
    if (s->hdr.stage == ETS_NAME_STR) {
        /* step 4: an ABSENT name is "Error" and coerces nothing */
        if (JS_IsUndefined(s->raw)) {
            s->name = JS_AtomToString(ctx, JS_ATOM_Error);
            if (JS_IsException(s->name)) { s->name = JS_UNDEFINED; return -1; }
        } else {
            r = step_tostring_run(ctx, &s->hdr, s->raw, cb_result, &s->name, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            JS_FreeValue(ctx, s->raw); s->raw = JS_UNDEFINED;
        }
        s->hdr.stage = ETS_MSG_GET;
    }
    if (s->hdr.stage == ETS_MSG_GET) {
        r = step_getprop_run(ctx, &s->hdr, s->hdr.this_val, JS_ATOM_message, cb_result, &s->raw, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = ETS_MSG_STR;
    }
    DCHECK(s->hdr.stage == ETS_MSG_STR, "the Error toString resumed in no stage");
    if (JS_IsUndefined(s->raw)) {
        msg = js_empty_string(ctx->rt);
        if (JS_IsException(msg)) return -1;
    } else {
        r = step_tostring_run(ctx, &s->hdr, s->raw, cb_result, &msg, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        JS_FreeValue(ctx, s->raw); s->raw = JS_UNDEFINED;
    }
    /* steps 7-9: two tests and a concatenation, none of it the page's */
    if (!JS_IsEmptyString(s->name) && !JS_IsEmptyString(msg))
        s->name = JS_ConcatString3(ctx, "", s->name, ": ");
    s->result = JS_ConcatString(ctx, s->name, msg);
    s->name = JS_UNDEFINED;   /* both consumed by the concatenations */
    return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_error_tostring_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSErrToString *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->name);
    v->val(ctx, &s->raw);
}

static JSValue js_error_tostring_fini(JSContext *ctx, void *st, bool take_result)
{
    JSErrToString *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* `get Error.prototype.stack`. u.object_data for an Error is EXACTLY one of: UNDEFINED (no trace was built),
   the FORMATTED stack (a string, or null when the format failed), or the PENDING PAIR — the two-element array
   [prepareStackTrace, callsites] that build_backtrace parks there instead of calling the page's hook from a
   throw. Formatting it is this machine's only job, and the result is memoized so the hook runs once. */
static bool js_error_stack_is_pending(JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return false;
    return JS_VALUE_GET_OBJ(v)->class_id == JS_CLASS_ARRAY;
}

static int js_error_get_stack_step(JSContext *ctx, void *st, JSValue cb_result,
                                   JSValue **out_cb, int *out_argc);

/* IS THIS OBJECT ALREADY BEING JOINED further out on this flow's stack? `a[0] = a; String(a)` asks join for
   the string of an element that IS the array, so join coerces it, which calls join, forever. Every engine
   answers "" for the self-referential element, and that answer is not optional: the operation has no other
   result, and without it the flow never finishes. It is not a bound — nothing distinct is truncated, because
   the recursion has no output to lose; it is what the operation MEANS on a cyclic input.

   Asked of the FRAMES, like the prepareStackTrace guard above and for the same reason: two flows have two
   stacks, and one parked inside a join must not decide what another flow's join does. A nested join reaches
   its outer one through the frame that outer pushed to coerce the element — cont_kind CONT_STEP, cont_state
   the outer machine — which is what this walk reads. */

/* IS A prepareStackTrace CALL ALREADY RUNNING ON THIS FLOW? V8 keeps this on the isolate
   (Isolate::formatting_stack_trace) and returns the DEFAULT formatting while it is set — which is not a
   fallback but the documented contract: mjsunit's own formatter ends with `catch (e) {}; return error.stack;`,
   deliberately re-entering to get the default. Without the answer that re-entry calls the hook again, and with
   no stack bound in this engine (the calls trampoline onto the heap) it does not overflow — it runs until the
   machine is killed, which is how a V8 regression file that finishes in milliseconds there became an OOM here.
   The scope of the question is a CALL STACK, so it is asked of the running flow's frames rather than of a
   process-wide flag: two flows have two stacks, and one parked inside a hook must not answer for the other. */
static bool js_error_stack_formatting(void)
{
    const TrampFrame *f;

    for (f = g_step_chain; f != NULL; f = f->up)
        if (f->cont_kind == CONT_STEP &&
            ((const JSStepHdr *)f->cont_state)->def->step == js_error_get_stack_step)
            return true;
    return false;
}

/* `Error.prototype.stack` IS NOT ECMASCRIPT — it is V8's stack-trace API, and its steps are that API's, so
   they are cited by their documented names rather than by a clause number that does not exist. The read either
   answers a string the capture already formatted, or CALLS the page's `Error.prepareStackTrace(error,
   callsites)` and memoizes what it returns; the call is the one thing that reaches the page, and it is the
   stage this machine parks at. */
#define ESTACK_STAGES(X) \
    X(ESTACK_READ,    "V8 stack-trace API: read the Error's captured stack — a formatted string is the answer, " \
                      "and a pending [prepareStackTrace, callsites] pair becomes the call below") \
    X(ESTACK_PREPARE, "V8 stack-trace API: Error.prepareStackTrace(error, structuredStackTrace), whose result " \
                      "is memoized onto the Error")
enum { ESTACK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_error_get_stack_steps[] = { ESTACK_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_error_get_stack_step(JSContext *ctx, void *st, JSValue cb_result,
                                   JSValue **out_cb, int *out_argc)
{
    JSErrGetStack *s = st;
    JSObject *p;

    if (s->hdr.stage == ESTACK_READ) {
        JSValue d;
        JS_FreeValue(ctx, cb_result);
        s->hdr.stage = ESTACK_PREPARE;
        s->result = JS_UNDEFINED;
        s->cb[0] = JS_UNDEFINED; s->cb[1] = JS_UNDEFINED;
        s->cb[2] = JS_UNDEFINED; s->cb[3] = JS_UNDEFINED;
        if (JS_VALUE_GET_TAG(s->hdr.this_val) != JS_TAG_OBJECT) {
            JS_ThrowTypeErrorNotAnObject(ctx);
            return -1;
        }
        p = JS_VALUE_GET_OBJ(s->hdr.this_val);
        if (p->class_id != JS_CLASS_ERROR)
            return 0;
        d = p->u.object_data;
        if (!js_error_stack_is_pending(d)) {
            DCHECK(JS_IsUndefined(d) || JS_IsString(d) || JS_IsNull(d),
                   "an Error's u.object_data is neither a formatted stack nor the pending prepare pair — "
                   "something other than build_backtrace wrote it");
            s->result = js_dup(d);
            return 0;
        }
        /* [prepare, callsites] -> prepare.call(Error, error, callsites). The pair's own elements are taken
           into the request buffer so the memoizing write below cannot free them out from under the call. */
        s->cb[0] = js_dup(ctx->error_ctor);           /* `this` */
        s->cb[1] = js_error_stack_pair_get(d, 0);    /* the callee: prepareStackTrace */
        s->cb[2] = js_dup(s->hdr.this_val);
        s->cb[3] = js_error_stack_pair_get(d, 1);
        /* THE DEFAULT RENDERING IS THE OTHER HALF OF THIS ONE OPERATION, not a case the CAPTURE decided.
           build_backtrace used to render it, which is what made building an error reach the string builder and
           through it every throw; it parks the frames now and the choice is made here, where it is a read of
           the value the capture recorded. Two ways to reach it, and they differ:
           `prepare` is not callable — nobody installed a hook, or installed something that is not a function —
           so 20.5's own formatting applies, and it MEMOIZES like the hook's answer because nothing about it
           can change.
           Or a hook IS installed and is already running on this flow: V8 returns the default while
           Isolate::formatting_stack_trace is set, the documented contract mjsunit's own formatter is built on
           (it ends with `catch (e) {}; return error.stack;`). That one does NOT memoize — the pending pair
           stays, so a later read from outside the hook still runs the hook. */
        if (!JS_IsFunction(ctx, s->cb[1]) || js_error_stack_formatting()) {
            bool memoize = !JS_IsFunction(ctx, s->cb[1]);
            s->result = js_callsite_array_render(ctx, s->cb[3]);
            if (JS_IsException(s->result))
                return -1;
            if (memoize) {
                /* forced-exec: a MEMOISATION is a shared-state write — one flow reading `err.stack` fixed the
                   formatted value for every sibling. */
                cow_capture_obj(ctx, p);
                JS_FreeValue(ctx, p->u.object_data);
                p->u.object_data = js_dup(s->result);
            }
            return 0;
        }
        *out_cb = s->cb; *out_argc = 2;
        return 3;
    }
    DCHECK(s->hdr.stage == ESTACK_PREPARE, "the Error stack accessor resumed in an unknown stage");
    /* the hook's THROW propagates out of the accessor, exactly as any getter's does. build_backtrace used to
       swallow it because it ran under a live exception; there is no exception here to protect. */
    if (JS_IsException(cb_result))
        return -1;
    p = JS_VALUE_GET_OBJ(s->hdr.this_val);
    DCHECK(p->class_id == JS_CLASS_ERROR && js_error_stack_is_pending(p->u.object_data),
           "the Error whose stack was being formatted stopped being one mid-call");
    cow_capture_obj(ctx, p);                      /* forced-exec: the memoisation is a shared-state write */
    JS_FreeValue(ctx, p->u.object_data);
    p->u.object_data = cb_result;                 /* memoized: the hook runs once per error */
    s->result = js_dup(p->u.object_data);
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). Error.prepareStackTrace is the page's function and the
   CallSite array is handed to it, so both ride the request. */
static void js_error_get_stack_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSErrGetStack *s = st;
    int i;
    v->val(ctx, &s->result);
    for (i = 0; i < 4; i++) v->val(ctx, &s->cb[i]);
}

static JSValue js_error_get_stack_fini(JSContext *ctx, void *st, bool take_result)
{
    JSErrGetStack *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* DELETED: js_error_set_stack's C body. The error-stack accessor's setter IS
   SetterThatIgnoresPrototypeProperties(E, %Error.prototype%, "stack", v), which %Iterator.prototype%'s two
   accessors already have a machine for — and the C body ran all THREE of its page-visible operations from C:
   `E.[[GetOwnProperty]]("stack")` (JS_GetOwnPropertyFlagsInternal, which is a `getOwnPropertyDescriptor` trap on a
   Proxy receiver — what test262's setter tests reached), CreateDataPropertyOrThrow and Set(E,"stack",v,true).
   Only its step 3 is not in the AO, so only step 3 is declared, as the definition's precheck. */
static int js_error_stack_precheck(JSContext *ctx, const struct JSStepHdr *h)
{
    if (!JS_IsString(step_arg(h, 0))) {   /* step 3: v must ALREADY be a String; nothing is coerced */
        JS_ThrowTypeError(ctx, "Error.prototype.stack setter expects a string");
        return -1;
    }
    return 0;
}

static const JSCFunctionListEntry js_error_proto_funcs[] = {
    JS_CFUNC_STEP_DEF("toString", 0, STEPDEF_ERROR_TOSTRING ),
    JS_PROP_STRING_DEF("name", "Error", JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),
    JS_PROP_STRING_DEF("message", "", JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),
    JS_CGETSET_STEP_BOTH_DEF("stack", STEPDEF_ERROR_GET_STACK, STEPDEF_ERROR_SET_STACK ),
};

static JSValue js_error_isError(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    return js_bool(JS_IsError(argv[0]));
}

static JSValue js_error_get_stackTraceLimit(JSContext *ctx, JSValueConst this_val)
{
    JSValue val;

    val = JS_ToObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    JS_FreeValue(ctx, val);
    return js_dup(ctx->error_stack_trace_limit);
}

static JSValue js_error_set_stackTraceLimit(JSContext *ctx, JSValueConst this_val, JSValueConst value)
{
    if (JS_IsUndefined(this_val) || JS_IsNull(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    ctx->error_stack_trace_limit = js_dup(value);
    return JS_UNDEFINED;
}

static JSValue js_error_get_prepareStackTrace(JSContext *ctx, JSValueConst this_val)
{
    JSValue val;

    val = JS_ToObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    JS_FreeValue(ctx, val);
    return js_dup(ctx->error_prepare_stack);
}

static JSValue js_error_set_prepareStackTrace(JSContext *ctx, JSValueConst this_val, JSValueConst value)
{
    if (JS_IsUndefined(this_val) || JS_IsNull(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    JS_FreeValue(ctx, ctx->error_prepare_stack);
    ctx->error_prepare_stack = js_dup(value);
    return JS_UNDEFINED;
}

/* proposal-error-capturestacktrace §20.5.2.1 Error.captureStackTrace ( O [ , limit ] ), under its data-property
   strategy. Steps 1-3 build the string and run none of the page's code; the define is DefinePropertyOrThrow on
   the page's O, which on a Proxy is the `defineProperty` trap, so it is a DEFINE request. */
typedef struct JSErrCapture {
    JSStepHdr hdr;
    JSValue result;       /* DONE (owned) */
    JSValue cb[2];        /* [O, string] for the DEFINE request (owned) */
} JSErrCapture;
#define ERRCAP_STAGES(X) \
    X(ERRCAP_BUILD,  "proposal-error-capturestacktrace §20.5.2.1 steps 1-5 (O is an Object; the stack string; " \
                     "the realm's strategy, which is data-property here)") \
    X(ERRCAP_DEFINE, "proposal-error-capturestacktrace §20.5.2.1 step 6 (DefinePropertyOrThrow(O, \"stack\", " \
                     "{ [[Value]]: string, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: true }))")
enum { ERRCAP_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_error_capture_steps[] = { ERRCAP_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_error_capture_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSErrCapture *s = st;

    if (s->hdr.stage == ERRCAP_BUILD) {
        JSValueConst v = step_arg(&s->hdr, 0);
        JSValue stack;

        JS_FreeValue(ctx, cb_result);
        s->result = JS_UNDEFINED;
        s->cb[0] = JS_UNDEFINED; s->cb[1] = JS_UNDEFINED;
        if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT) {
            JS_ThrowTypeErrorNotAnObject(ctx);
            return -1;
        }
        build_backtrace(ctx, v, step_arg(&s->hdr, 1), NULL, 0, 0,
                        JS_BACKTRACE_FLAG_SKIP_FIRST_LEVEL | JS_BACKTRACE_FLAG_FILTER_FUNC, &stack);
        if (JS_IsUndefined(stack))
            return 0;                      /* a capture was already in progress; there is nothing to install */
        s->cb[0] = js_dup(v);
        s->cb[1] = stack;
        s->hdr.desc_flags = JS_PROP_THROW | JS_PROP_DEFINE_PROPERTY
                          | JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE
                          | JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE;
        s->hdr.stage = ERRCAP_DEFINE;
        *out_cb = s->cb; *out_argc = (int)JS_ATOM_stack;
        return 10;   /* DEFINE */
    }
    DCHECK(s->hdr.stage == ERRCAP_DEFINE, "Error.captureStackTrace resumed in no stage");
    if (JS_IsException(cb_result)) return -1;
    JS_FreeValue(ctx, cb_result);
    return 0;
}

static void js_error_capture_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSErrCapture *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->cb[0]);
    v->val(ctx, &s->cb[1]);
}

static JSValue js_error_capture_fini(JSContext *ctx, void *st, bool take_result)
{
    JSErrCapture *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static const JSCFunctionListEntry js_error_funcs[] = {
    JS_CFUNC_DEF("isError", 1, js_error_isError ),
    JS_CFUNC_STEP_DEF("captureStackTrace", 2, STEPDEF_ERROR_CAPTURE ),
    JS_CGETSET_DEF("stackTraceLimit", js_error_get_stackTraceLimit, js_error_set_stackTraceLimit ),
    JS_CGETSET_DEF("prepareStackTrace", js_error_get_prepareStackTrace, js_error_set_prepareStackTrace ),
};

