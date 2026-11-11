/* Reflect */

/* DELETED: js_reflect_apply's C body. It only forwarded to js_function_apply, which is deleted below for the
   same reason: Reflect.apply is STEPDEF_REFLECT_APPLY, the same machine Function.prototype.apply is. */

/* DELETED: js_reflect_construct's C body. Reflect.construct is STEPDEF_REFLECT_CONSTRUCT; this body was
   unregistered and unreachable, and it read its argsList with build_arg_list — so it read as a live path that
   performs 19.2.3.1's element getters from C with no flow base. */





/* Reflect.get / set / has / deleteProperty are the four BARE internal methods, and each of their two user-code
   steps is a request: ToPropertyKey on the key, then the method itself — which on a Proxy is a trap and on an
   ordinary object can be an accessor. Their C bodies performed the second with JS_GetPropertyInternal /
   JS_SetPropertyInternal2 / JS_HasProperty / JS_DeleteProperty, so a looping getter, setter or trap had no flow
   base. def->arg is the GP_* op. */
/* FOUR algorithms, one machine. Their first two steps are identical and their last is the internal method the
   spelling names; `get` and `set` additionally default the receiver at step 3, which runs none of the page's code
   and so belongs to the stage that rests on the method. ONE stage list expanded once per algorithm — see
   AFIND_STAGES. */
#define REFLPROP_STAGES(X, RECV, KEY, OP) \
    X(REFLPROP_RECV, RECV) \
    X(REFLPROP_KEY,  KEY)  \
    X(REFLPROP_OP,   OP)
enum { REFLPROP_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0) };
static const char *const js_reflect_get_steps[] = {
    REFLPROP_STAGES(JS_STEP_STAGE_LABEL,
        "28.1.5 step 1 (target is an Object)",
        "28.1.5 step 2 (key is ToPropertyKey(propertyKey))",
        "28.1.5 steps 3-4 (receiver defaults to target; target.[[Get]](key, receiver))")
    NULL };
static const char *const js_reflect_set_steps[] = {
    REFLPROP_STAGES(JS_STEP_STAGE_LABEL,
        "28.1.12 step 1 (target is an Object)",
        "28.1.12 step 2 (key is ToPropertyKey(propertyKey))",
        "28.1.12 steps 3-4 (receiver defaults to target; target.[[Set]](key, V, receiver))")
    NULL };
static const char *const js_reflect_has_steps[] = {
    REFLPROP_STAGES(JS_STEP_STAGE_LABEL,
        "28.1.8 step 1 (target is an Object)",
        "28.1.8 step 2 (key is ToPropertyKey(propertyKey))",
        "28.1.8 step 3 (target.[[HasProperty]](key))")
    NULL };
static const char *const js_reflect_del_steps[] = {
    REFLPROP_STAGES(JS_STEP_STAGE_LABEL,
        "28.1.4 step 1 (target is an Object)",
        "28.1.4 step 2 (key is ToPropertyKey(propertyKey))",
        "28.1.4 step 3 (target.[[Delete]](key))")
    NULL };

static int js_reflect_prop_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReflectProp *s = st;
    int op = s->hdr.arg, r;

    if (s->hdr.stage == REFLPROP_RECV) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->obj = JS_UNDEFINED; s->result = JS_UNDEFINED; s->atom = JS_ATOM_NULL;
        if (JS_VALUE_GET_TAG(step_arg(&s->hdr, 0)) != JS_TAG_OBJECT) {
            JS_ThrowTypeErrorNotAnObject(ctx); return -1;
        }
        s->obj = js_dup(step_arg(&s->hdr, 0));
        s->hdr.stage = REFLPROP_KEY;
    }
    if (s->hdr.stage == REFLPROP_KEY) {
        r = step_topropkey_run(ctx, &s->hdr, step_arg(&s->hdr, 1), cb_result, &s->atom, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = REFLPROP_OP;
        /* the receiver defaults to the object; `set` takes it one argument later than `get` does. */
        s->hdr.cb_coerce[0] = s->obj;
        if (op == GP_GET) {
            s->hdr.cb_coerce[1] = (s->hdr.argc > 2) ? s->hdr.argv[2] : s->obj;
        } else if (op == GP_SET) {
            s->hdr.cb_coerce[1] = (s->hdr.argc > 3) ? s->hdr.argv[3] : s->obj;
            s->hdr.cb_coerce[2] = (s->hdr.argc > 2) ? s->hdr.argv[2] : JS_UNDEFINED;
        }
        *out_cb = s->hdr.cb_coerce; *out_argc = (int)s->atom;
        return (op == GP_GET) ? 13 : (op == GP_SET) ? 14 : (op == GP_DELETE) ? 15 : 7;
    }
    DCHECK(s->hdr.stage == REFLPROP_OP, "Reflect property operation: unknown stage");
    s->result = cb_result;
    if (JS_IsException(s->result))
        return -1;
    /* `Reflect.has(window, "X")` ON A NAME NOTHING BINDS — a feature detect spelled as a CALL, recorded here
       because this is the only point at which the operation, its target and its answer are all in one hand.
       §28.1.8 "Reflect.has ( target, key )" step 3 is "Return ? target.[[HasProperty]](propertyKey)", the
       same §7.3.11 "HasProperty ( obj, propertyKey )" the `in` operator ends at and equally not a [[Get]], so
       the host RECORDS and this machine's answer is untouched either way. The gates are js_absent_note_has's
       and are not restated here: they are the same two facts for the same reason, and the one thing this site
       knows that it does not is WHICH of the four operations this machine is running.
       IT IS GATED ON GP_HAS AND THAT IS NOT A DETAIL OF THIS CALL — `js_reflect_prop_step` is FOUR algorithms
       sharing one machine (get, set, has, deleteProperty, selected by `js_reflect_*_def`'s own `gp_op`), so a
       site here with no arm test would hand the recorder `Reflect.get(window, "X")`'s `undefined` and
       `Reflect.deleteProperty`'s `true` as though they were HasProperty answers. `Reflect.get` is a [[Get]]
       and is already answered by the read hook, so it would also be counted twice.
       THE ANSWER IS READ BEFORE THE TEARDOWN AND THE OPERANDS ARE STILL THE MACHINE'S: `s->obj` is the target
       held across the request since REFLPROP_RECV and `s->atom` is ToPropertyKey's own result, both owned by
       this state and freed by its visit/fini rather than here. */
    if (op == GP_HAS)
        js_absent_note_has(ctx, s->obj, s->atom, s->result, JS_CONCOLIC_ABSENT_HAS_CALL);
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The key's ToPropertyKey is the page's code, and the atom it
   produced is held across the operation that uses it. */
static void js_reflect_prop_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReflectProp *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->obj);
    v->atom(ctx, &s->atom);
}

static JSValue js_reflect_prop_fini(JSContext *ctx, void *st, bool take_result)
{
    JSReflectProp *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static const JSCFunctionListEntry js_reflect_funcs[] = {
    JS_CFUNC_STEP_DEF("apply", 3, STEPDEF_REFLECT_APPLY ),
    JS_CFUNC_STEP_DEF("construct", 2, STEPDEF_REFLECT_CONSTRUCT ),
    JS_CFUNC_STEP_DEF("defineProperty", 3, STEPDEF_REFLECT_DEFINEPROPERTY ),
    JS_CFUNC_STEP_DEF("deleteProperty", 2, STEPDEF_REFLECT_DELETE ),
    JS_CFUNC_STEP_DEF("get", 2, STEPDEF_REFLECT_GET ),
    JS_CFUNC_STEP_DEF("getOwnPropertyDescriptor", 2, STEPDEF_REFLECT_GOPD ),
    JS_CFUNC_STEP_DEF("getPrototypeOf", 1, STEPDEF_REFLECT_GETPROTO ),
    JS_CFUNC_STEP_DEF("has", 2, STEPDEF_REFLECT_HAS ),
    JS_CFUNC_STEP_DEF("isExtensible", 1, STEPDEF_REFLECT_ISEXT ),
    JS_CFUNC_STEP_DEF("ownKeys", 1, STEPDEF_REFLECT_OWNKEYS ),
    JS_CFUNC_STEP_DEF("preventExtensions", 1, STEPDEF_REFLECT_PREVEXT ),
    JS_CFUNC_STEP_DEF("set", 3, STEPDEF_REFLECT_SET ),
    JS_CFUNC_STEP_DEF("setPrototypeOf", 2, STEPDEF_REFLECT_SETPROTO ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Reflect", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_reflect_obj[] = {
    JS_OBJECT_DEF("Reflect", js_reflect_funcs, countof(js_reflect_funcs), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),
};

