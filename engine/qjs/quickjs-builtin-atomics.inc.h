/* Atomics */
#ifdef CONFIG_ATOMICS

/* AtomicsOpEnum now lives beside the step definitions: a definition carries the body's magic, so the enum has
   to be visible where the definitions are written. */
static JSObject *js_atomics_get_buf(JSContext *ctx,
                                    JSValueConst obj, JSValueConst idx_val,
                                    JSArrayBuffer **pabuf,
                                    uint64_t *pindex, int is_waitable)
{
    JSObject *p;
    JSTypedArray *ta;
    JSArrayBuffer *abuf;
    uint64_t idx;
    bool err;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        goto fail;
    p = JS_VALUE_GET_OBJ(obj);
    if (is_waitable)
        err = (p->class_id != JS_CLASS_INT32_ARRAY &&
               p->class_id != JS_CLASS_BIG_INT64_ARRAY);
    else
        err = !(p->class_id >= JS_CLASS_INT8_ARRAY &&
                p->class_id <= JS_CLASS_BIG_UINT64_ARRAY);
    if (err) {
    fail:
        JS_ThrowTypeError(ctx, "integer TypedArray expected");
        return NULL;
    }
    ta = p->u.typed_array;
    abuf = ta->buffer->u.array_buffer;
    if (!abuf->shared) {
        if (is_waitable == 2) {
            JS_ThrowTypeError(ctx, "not a SharedArrayBuffer TypedArray");
            return NULL;
        }
        if (abuf->detached) {
            JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
            return NULL;
        }
    }
    if (JS_ToIndex(ctx, &idx, idx_val)) {
        return NULL;
    }
    /* if the array buffer is detached, p->u.array.count = 0 */
    /* THE ONE BOUND TEST EVERY ATOMICS BODY WRITES UNDER, and the reason the bodies no longer repeat it. Upstream
       tested again just before the store because the VALUE coercion ran inside the body, so `{valueOf(){ ta.buffer
       .transfer() }}` could detach or shrink between this line and the write. In this fork every coerced argument
       is a ToPrimitive REQUEST the step machine makes before it calls the body (js_atomics_step), so the body is
       entered with primitives and JS_ToIndex/JS_ToBigInt64/JS_ToUint32 on a primitive reach no page code at all.
       Nothing between here and the write can move the storage, and a repeat of this test is a branch that cannot
       be taken — which is where its own leak lived, unreachable and untestable. The bodies assert it instead. */
    if (idx >= p->u.array.count) {
        JS_ThrowRangeError(ctx, "out-of-bound access");
        return NULL;
    }
    DCHECK_TA_WINDOW_STATED(ta, 1, "an Atomics operation");
    if (pabuf)
        *pabuf = abuf;
    *pindex = idx;
    return p;
}

/* 25.4.3.1 ValidateIntegerTypedArray and 25.4.3.2 step 1, which the spec performs BEFORE ToIndex(index). It
   reads only internal slots, so it invokes nothing — and the LENGTH it returns is the one the range test must
   use, because a growable buffer grown inside the index's own valueOf must not widen the window it was measured
   against. */
static JSObject *js_atomics_validate(JSContext *ctx, JSValueConst obj, int is_waitable, int is_write)
{
    JSObject *p;
    JSTypedArray *ta;
    JSArrayBuffer *abuf;
    bool err;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        goto fail;
    p = JS_VALUE_GET_OBJ(obj);
    if (is_waitable)
        err = (p->class_id != JS_CLASS_INT32_ARRAY && p->class_id != JS_CLASS_BIG_INT64_ARRAY);
    else
        err = !(p->class_id >= JS_CLASS_INT8_ARRAY && p->class_id <= JS_CLASS_BIG_UINT64_ARRAY);
    if (err) {
    fail:
        JS_ThrowTypeError(ctx, "integer TypedArray expected");
        return NULL;
    }
    /* ValidateTypedArray step 4: a ~write~ access mode rejects an IMMUTABLE buffer. The eight
       read-modify-write operations and store are writes; load, wait and notify are not. */
    if (is_write && typed_array_is_immutable(p)) {
        JS_ThrowTypeErrorImmutableArrayBuffer(ctx);
        return NULL;
    }
    ta = p->u.typed_array;
    abuf = ta->buffer->u.array_buffer;
    if (!abuf->shared) {
        if (is_waitable == 2) {
            JS_ThrowTypeError(ctx, "not a SharedArrayBuffer TypedArray");
            return NULL;
        }
        if (abuf->detached) {
            JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
            return NULL;
        }
    }
    return p;
}

/* 25.4.3.2 steps 2-4: ToIndex(requestIndex) against the length captured at step 1. The spec performs both
   BEFORE the value is coerced, so an out-of-range index must throw without the value's valueOf running at all —
   and against the CAPTURED length, so growing the buffer inside the index's own valueOf does not rescue it. The
   index is already primitive here, so its ToIndex invokes nothing. */
static int js_atomics_access(JSContext *ctx, JSAtomics *s)
{
    uint64_t idx;
    if (JS_ToIndex(ctx, &idx, s->argp[1]))
        return -1;
    if (idx >= s->length) {
        JS_ThrowRangeError(ctx, "out-of-bound access");
        return -1;
    }
    return 0;
}

/* ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). SHARED by all twelve
   entry points, and what it performs is not one of their algorithms but the validation-then-coercion prologue
   every one of them begins with; which body follows is what the definition's `body` says. */
#define ATOMICS_STAGES(X) \
    X(ATOM_VALIDATE, "25.4.3.1 ValidateIntegerTypedArray(typedArray, waitable) and 25.4.3.2 step 1 (length is " \
                     "TypedArrayLength(taRecord)) - both BEFORE the first coercion, which is why the length " \
                     "lives on the state") \
    X(ATOM_COERCE,   "25.4.3.2 steps 2-4 (accessIndex is ToIndex(requestIndex), tested against the captured " \
                     "length) and the operation's own ToNumber/ToBigInt on each declared value argument - and " \
                     "then the operation's body, over the primitives this stage put in place") \
    X(ATOM_SUSPENDED, "25.4.3.11 SuspendThisAgent ( waiterList, waiterRecord ) - Atomics.wait's agent is " \
                      "suspended until NotifyWaiter wakes it or its timeout job does")
enum { ATOMICS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_atomics_steps[] = { ATOMICS_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* A machine suspends its agent on `rec` (§25.4.3.11 SuspendThisAgent): the driver parks the flow, and the host
   takes `rec` to know when to resume it. Returns the code the machine returns. */
static int step_suspend_on(JSContext *ctx, JSValueConst rec)
{
    DCHECK(JS_IsUndefined(ctx->rt->suspended_on),
           "a flow suspended while the host still held an earlier suspension it never took (JS_TakeSuspension)");
    ctx->rt->suspended_on = js_dup(rec);
    return JS_STEP_SUSPEND;
}

static int js_atomics_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSAtomics *s = st;
    int arg = s->hdr.arg;
    unsigned mask = ATOMICS_MASK(arg);
    int nargs = ATOMICS_NARGS(arg);
    int i;

    if (s->hdr.stage == ATOM_SUSPENDED) {
        JS_FreeValue(ctx, cb_result);
        /* Woken by NotifyWaiter, or by the host at [[TimeoutTime]] (JS_TimeOutSuspension), which removed the
           record and set "timed-out" — the draft's DoWait returns [[Result]] after SuspendThisAgent without
           stating a timeout's own, so the timeout takes §25.4.3.15's steps, which test262's timeouts read. */
        if (!JS_SuspensionWoken(ctx, s->wait_rec))
            return step_suspend_on(ctx, s->wait_rec);
        s->result = js_waiter_get(ctx, s->wait_rec, WR_RESULT);
        JS_FreeValue(ctx, s->wait_rec);
        s->wait_rec = JS_UNDEFINED;
        return 0;
    }
    if (s->hdr.stage == ATOM_VALIDATE) {
        JSObject *p;
        JS_FreeValue(ctx, cb_result);
        s->result = JS_UNDEFINED;
        s->wait_rec = JS_UNDEFINED;
        /* steps 25.4.3.1 and 25.4.3.2 step 1, both BEFORE the first coercion */
        p = js_atomics_validate(ctx, step_arg(&s->hdr, 0), ATOMICS_WAITABLE(arg), ATOMICS_ISWRITE(arg));
        if (!p)
            return -1;
        s->length = p->u.array.count;
        s->nargp = s->hdr.argc > nargs ? s->hdr.argc : nargs;
        s->argp = js_malloc(ctx, sizeof(JSValue) * (size_t)(s->nargp > 0 ? s->nargp : 1));
        if (unlikely(!s->argp)) { s->nargp = 0; return -1; }
        for (i = 0; i < s->nargp; i++)
            s->argp[i] = js_dup(step_arg(&s->hdr, i));
        s->i = 0;
        s->hdr.stage = ATOM_COERCE;
    } else {
        DCHECK(s->i > 0, "an Atomics operation received a primitive with no coercion in flight");
        s->hdr.coercing = 0;
        JS_FreeValue(ctx, s->argp[s->i - 1]);
        s->argp[s->i - 1] = cb_result;   /* the primitive REPLACES the argument the body will read */
        if (prim_arg_check(ctx, s->argp[s->i - 1], HINT_NUMBER) < 0)
            return -1;
        if (s->i == 2 && js_atomics_access(ctx, s) < 0)
            return -1;
    }
    while (s->i < s->nargp) {
        i = s->i++;
        if (i >= 8 || !((mask >> i) & 1)) continue;
        if (JS_VALUE_GET_TAG(s->argp[i]) == JS_TAG_OBJECT) {
            s->cb[0] = s->argp[i];   /* borrowed: the vector holds the reference */
            *out_cb = s->cb; *out_argc = HINT_NUMBER;
            s->hdr.coercing = 1;
            return 5;
        }
        /* the argument was already primitive, so its conversion — and for the index 25.4.3.2 steps 2-4 — is
           due right here. A BigInt is legitimate on a BigInt64Array, so only the Symbol is rejected. */
        if (prim_arg_check(ctx, s->argp[i], HINT_NUMBER) < 0)
            return -1;
        if (i == 1 && js_atomics_access(ctx, s) < 0)
            return -1;
    }
    if (s->hdr.def == &js_atomics_wait_def) {
        /* §25.4.15 Atomics.wait: DoWait ( sync, … ), whose suspension is this machine's next stage. */
        JSValue done, list, promise;
        if (js_atomics_dowait(ctx, (JSValueConst *)s->argp, true, &done, &list, &s->wait_rec, &promise) < 0)
            return -1;
        JS_FreeValue(ctx, list);
        DCHECK(JS_IsUndefined(promise), "a blocking DoWait made a promise capability");
        if (!JS_IsUndefined(done)) {
            s->result = done;
            return 0;
        }
        s->hdr.stage = ATOM_SUSPENDED;
        return step_suspend_on(ctx, s->wait_rec);
    }
    DCHECK(s->hdr.def->body.generic != NULL, "an Atomics definition with no body");
    if (s->hdr.def->body_proto == JS_CFUNC_generic_magic) {
        s->result = s->hdr.def->body.generic_magic(ctx, s->hdr.this_val, s->hdr.argc,
                                                   (JSValueConst *)s->argp, s->hdr.def->body_magic);
    } else {
        DCHECK(s->hdr.def->body_proto == JS_CFUNC_generic, "an Atomics definition with an unhandled prototype");
        s->result = s->hdr.def->body.generic(ctx, s->hdr.this_val, s->hdr.argc, (JSValueConst *)s->argp);
    }
    return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The coerced argument vector — the machine's own block, since
   each coercion is the page's valueOf and a fork can land between two of them. cb is the ToPrimitive request's
   buffer and borrows out of that vector. */
static void js_atomics_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSAtomics *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->wait_rec);
    v->array(ctx, (void **)&s->argp, sizeof(JSValue), s->nargp, s->nargp, js_step_visit_value_elem);
}

static JSValue js_atomics_fini(JSContext *ctx, void *st, bool take_result)
{
    JSAtomics *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static JSValue js_atomics_op(JSContext *ctx,
                             JSValueConst this_obj,
                             int argc, JSValueConst *argv, int op)
{
    int size_log2;
    uint64_t v, a, rep_val, idx;
    void *ptr;
    JSObject *p;
    JSValue ret;

    p = js_atomics_get_buf(ctx, argv[0], argv[1], NULL, &idx, 0);
    if (!p)
        return JS_EXCEPTION;
    size_log2 = typed_array_size_log2(p->class_id);
    rep_val = 0;
    if (op == ATOMICS_OP_LOAD) {
        v = 0;
    } else {
        if (size_log2 == 3) {
            int64_t v64;
            if (JS_ToBigInt64(ctx, &v64, argv[2]))
                return JS_EXCEPTION;
            v = v64;
            if (op == ATOMICS_OP_COMPARE_EXCHANGE) {
                if (JS_ToBigInt64(ctx, &v64, argv[3]))
                    return JS_EXCEPTION;
                rep_val = v64;
            }
        } else {
			uint32_t v32;
			if (JS_ToUint32(ctx, &v32, argv[2]))
				return JS_EXCEPTION;
			v = v32;
			if (op == ATOMICS_OP_COMPARE_EXCHANGE) {
				if (JS_ToUint32(ctx, &v32, argv[3]))
					return JS_EXCEPTION;
				rep_val = v32;
			}
        }
   }

    /* ALWAYS fatal rather than DEV-only: what it guards is an atomic read-modify-write straight into the heap
       at an index nothing re-validated. See js_atomics_get_buf for why this holds. */
    CHECK(idx < p->u.array.count,
          "an Atomics read-modify-write reached its storage out of bounds - js_atomics_get_buf's is the only "
          "bound test, and it holds because every coercion is a request the step machine makes before this body");

    ptr = p->u.array.u.uint8_ptr + ((uintptr_t)idx << size_log2);
    /* every op but LOAD writes the storage; LOAD reads it, and capturing there would be a spurious entry on
       every Atomics.load of a shared buffer. See cow_capture_bytes. */
    if (op != ATOMICS_OP_LOAD)
        cow_capture_bytes(ctx, p);

   switch(op | (size_log2 << 3)) {

#define OP(op_name, func_name)                          \
    case ATOMICS_OP_ ## op_name | (0 << 3):             \
       a = func_name((_Atomic uint8_t *)ptr, v);        \
       break;                                           \
    case ATOMICS_OP_ ## op_name | (1 << 3):             \
        a = func_name((_Atomic uint16_t *)ptr, v);      \
        break;                                          \
    case ATOMICS_OP_ ## op_name | (2 << 3):             \
        a = func_name((_Atomic uint32_t *)ptr, v);      \
        break;                                          \
    case ATOMICS_OP_ ## op_name | (3 << 3):             \
        a = func_name((_Atomic uint64_t *)ptr, v);      \
        break;
        OP(ADD, atomic_fetch_add)
        OP(AND, atomic_fetch_and)
        OP(OR, atomic_fetch_or)
        OP(SUB, atomic_fetch_sub)
        OP(XOR, atomic_fetch_xor)
        OP(EXCHANGE, atomic_exchange)
#undef OP

    case ATOMICS_OP_LOAD | (0 << 3):
        a = atomic_load((_Atomic uint8_t *)ptr);
        break;
    case ATOMICS_OP_LOAD | (1 << 3):
        a = atomic_load((_Atomic uint16_t *)ptr);
        break;
    case ATOMICS_OP_LOAD | (2 << 3):
        a = atomic_load((_Atomic uint32_t *)ptr);
        break;
    case ATOMICS_OP_LOAD | (3 << 3):
        a = atomic_load((_Atomic uint64_t *)ptr);
        break;
    case ATOMICS_OP_COMPARE_EXCHANGE | (0 << 3):
        {
            uint8_t v1 = v;
            atomic_compare_exchange_strong((_Atomic uint8_t *)ptr, &v1, rep_val);
            a = v1;
        }
        break;
    case ATOMICS_OP_COMPARE_EXCHANGE | (1 << 3):
        {
            uint16_t v1 = v;
            atomic_compare_exchange_strong((_Atomic uint16_t *)ptr, &v1, rep_val);
            a = v1;
        }
        break;
    case ATOMICS_OP_COMPARE_EXCHANGE | (2 << 3):
        {
            uint32_t v1 = v;
            atomic_compare_exchange_strong((_Atomic uint32_t *)ptr, &v1, rep_val);
            a = v1;
        }
        break;
    case ATOMICS_OP_COMPARE_EXCHANGE | (3 << 3):
        {
            uint64_t v1 = v;
            atomic_compare_exchange_strong((_Atomic uint64_t *)ptr, &v1, rep_val);
            a = v1;
        }
        break;
    default:
        abort();
    }

    switch(p->class_id) {
    case JS_CLASS_INT8_ARRAY:
        a = (int8_t)a;
        goto done;
    case JS_CLASS_UINT8_ARRAY:
        a = (uint8_t)a;
        goto done;
    case JS_CLASS_INT16_ARRAY:
        a = (int16_t)a;
        goto done;
    case JS_CLASS_UINT16_ARRAY:
        a = (uint16_t)a;
        goto done;
    case JS_CLASS_INT32_ARRAY:
    done:
        ret = js_int32(a);
        break;
    case JS_CLASS_UINT32_ARRAY:
        ret = js_uint32(a);
        break;
    case JS_CLASS_BIG_INT64_ARRAY:
        ret = JS_NewBigInt64(ctx, a);
        break;
    case JS_CLASS_BIG_UINT64_ARRAY:
        ret = JS_NewBigUint64(ctx, a);
        break;
    default:
        abort();
    }
    return ret;
}

static JSValue js_atomics_store(JSContext *ctx,
                                JSValueConst this_obj,
                                int argc, JSValueConst *argv)
{
    int size_log2;
    uint64_t idx;
    void *ptr;
    JSObject *p;
    JSValue ret;

    p = js_atomics_get_buf(ctx, argv[0], argv[1], NULL, &idx, 0);
    if (!p)
        return JS_EXCEPTION;
    cow_capture_bytes(ctx, p);   /* both branches below atomic_store into the storage — see cow_capture_bytes */
    size_log2 = typed_array_size_log2(p->class_id);
    if (size_log2 == 3) {
        int64_t v64;
        ret = JS_ToBigIntFree(ctx, js_dup(argv[2]));
        if (JS_IsException(ret))
            return ret;
        if (JS_ToBigInt64(ctx, &v64, ret)) {
            JS_FreeValue(ctx, ret);
            return JS_EXCEPTION;
        }
        CHECK(idx < p->u.array.count,
              "Atomics.store reached its storage out of bounds - see js_atomics_get_buf");
        ptr = p->u.array.u.uint8_ptr + ((uintptr_t)idx << size_log2);
        atomic_store((_Atomic uint64_t *)ptr, v64);
    } else {
        uint32_t v;
        /* XXX: spec, would be simpler to return the written value */
        ret = JS_ToIntegerFree(ctx, js_dup(argv[2]));
        if (JS_IsException(ret))
            return ret;
        if (JS_ToUint32(ctx, &v, ret)) {
            JS_FreeValue(ctx, ret);
            return JS_EXCEPTION;
        }
        CHECK(idx < p->u.array.count,
              "Atomics.store reached its storage out of bounds - see js_atomics_get_buf");
        ptr = p->u.array.u.uint8_ptr + ((uintptr_t)idx << size_log2);
        switch(size_log2) {
        case 0:
            atomic_store((_Atomic uint8_t *)ptr, v);
            break;
        case 1:
            atomic_store((_Atomic uint16_t *)ptr, v);
            break;
        case 2:
            atomic_store((_Atomic uint32_t *)ptr, v);
            break;
        default:
            abort();
        }
    }
    return ret;
}

static JSValue js_atomics_isLockFree(JSContext *ctx,
                                     JSValueConst this_obj,
                                     int argc, JSValueConst *argv)
{
    int v, ret;
    if (JS_ToInt32Sat(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    ret = (v == 1 || v == 2 || v == 4 || v == 8);
    return js_bool(ret);
}

/* ECMA-262 §25.4.1 Waiter Record, as a heap Array held by its WaiterList so it time-travels with the flow that
   added it: [[PromiseCapability]] as its resolve function (null for blocking) and its promise, [[TimeoutTime]],
   [[Result]], and whether a blocking waiter's agent has been woken (§25.4.3.12 NotifyWaiter's first arm). Its
   [[AgentSignifier]] is the agent of the resolve function's realm, or, for a blocking waiter, the agent suspended
   on the record. */

static JSValue js_waiter_get(JSContext *ctx, JSValueConst rec, int field)
{
    return JS_GetPropertyUint32(ctx, rec, (uint32_t)field);
}

static void js_waiter_set(JSContext *ctx, JSValueConst rec, int field, JSValue v)
{
    CHECK(JS_SetPropertyUint32(ctx, rec, (uint32_t)field, v) >= 0, "a Waiter Record could not be written");
}

/* §25.4.3.5 GetWaiterList ( block, i ): the Array of Waiter Records the block's store holds for byte index `i`,
   created on first use. Owned. */
static JSValue js_get_waiter_list(JSContext *ctx, JSArrayBuffer *abuf, uint32_t i)
{
    JSValue list;

    DCHECK(abuf->shared && JS_IsObject(abuf->waiter_lists), "a WaiterList was asked of a block that has none");
    list = JS_GetPropertyUint32(ctx, abuf->waiter_lists, i);
    if (JS_IsUndefined(list)) {
        list = JS_NewArray(ctx);
        CHECK(!JS_IsException(list), "a WaiterList could not be allocated");
        CHECK(JS_SetPropertyUint32(ctx, abuf->waiter_lists, i, js_dup(list)) >= 0,
              "a WaiterList could not be stored on its block");
    }
    return list;
}

static uint32_t js_waiter_list_length(JSContext *ctx, JSValueConst list)
{
    int64_t len = 0;

    CHECK(js_get_length64(ctx, &len, list) == 0, "a WaiterList's length could not be read");
    return (uint32_t)len;
}

/* §25.4.3.9 RemoveWaiter ( waiterList, waiterRecord ), answering whether the list contained it. */
static bool js_remove_waiter(JSContext *ctx, JSValueConst list, JSValueConst rec)
{
    uint32_t n = js_waiter_list_length(ctx, list), i, j;

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i);
        bool same = JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(rec);
        JS_FreeValue(ctx, e);
        if (same) {
            for (j = i + 1; j < n; j++)
                CHECK(JS_SetPropertyUint32(ctx, list, j - 1, JS_GetPropertyUint32(ctx, list, j)) >= 0,
                      "a WaiterList could not be compacted");
            CHECK(JS_SetPropertyStr(ctx, list, "length", js_uint32(n - 1)) >= 0,
                  "a WaiterList could not be shortened");
            return true;
        }
    }
    return false;
}

/* EnqueueResolveInAgentJob's job: settle the promise with the result, in the waiting agent. */
static JSValue js_atomics_resolve_job(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic, JSValueConst *data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    fulfill_or_reject_promise(ctx, data[0], data[1], false);
    return JS_UNDEFINED;
}

/* §25.4.3.12 NotifyWaiter ( waiterList, waiterRecord ). */
static void js_notify_waiter(JSContext *ctx, JSValueConst rec)
{
    JSValue resolve = js_waiter_get(ctx, rec, WR_RESOLVE), promise, result;
    JSContext *target;

    if (JS_IsNull(resolve)) {
        /* "Wake the agent whose signifier is waiterRecord.[[AgentSignifier]] from suspension": the host resumes
           the flow suspended on this record once it reads it woken (JS_SuspensionWoken). */
        js_waiter_set(ctx, rec, WR_WOKEN, JS_TRUE);
        return;
    }
    promise = js_waiter_get(ctx, rec, WR_PROMISE);
    result = js_waiter_get(ctx, rec, WR_RESULT);
    target = JS_GetFunctionRealm(ctx, resolve);
    CHECK(target != NULL, "a waitAsync capability's resolve function has no realm");
    if (JS_GetRealmAgent(ctx) == JS_GetRealmAgent(target)) {
        fulfill_or_reject_promise(ctx, promise, result, false);
    } else {
        /* §25.4.3.13 EnqueueResolveInAgentJob: HostEnqueueGenericJob in the resolve function's realm. */
        JSValueConst d[2] = { promise, result };
        JSValue job = JS_NewCFunctionData(target, js_atomics_resolve_job, 0, 0, 2, d);
        CHECK(!JS_IsException(job), "an EnqueueResolveInAgentJob job could not be allocated");
        CHECK(ctx->rt->host_jobs.enqueue_generic != NULL,
              "§9.5.4 HostEnqueueGenericJob was asked of a host that installed no job hooks (JS_SetHostJobHooks)");
        ctx->rt->host_jobs.enqueue_generic(target, job, ctx->rt->host_jobs.opaque);
        JS_FreeValue(ctx, job);
    }
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, promise);
    JS_FreeValue(ctx, resolve);
}

/* §25.4.3.15 EnqueueAtomicsWaitAsyncTimeoutJob's timeoutJob, over [waiterList, waiterRecord]. */
static JSValue js_atomics_timeout_job(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic, JSValueConst *data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    if (js_remove_waiter(ctx, data[0], data[1])) {
        js_waiter_set(ctx, data[1], WR_RESULT, JS_AtomToString(ctx, JS_ATOM_timed_out));
        js_notify_waiter(ctx, data[1]);
    }
    return JS_UNDEFINED;
}

/* §25.4.3.15 EnqueueAtomicsWaitAsyncTimeoutJob ( waiterList, waiterRecord ): HostEnqueueTimeoutJob in the current
   realm, after `ms`, which is [[TimeoutTime]] - now over the `now` DoWait read in the same step. */
static void js_enqueue_wait_timeout_job(JSContext *ctx, JSValueConst list, JSValueConst rec, double ms)
{
    JSValueConst d[2] = { list, rec };
    JSValue job = JS_NewCFunctionData(ctx, js_atomics_timeout_job, 0, 0, 2, d);

    CHECK(!JS_IsException(job), "an Atomics wait timeout job could not be allocated");
    CHECK(ctx->rt->host_jobs.enqueue_timeout != NULL,
          "§9.5.6 HostEnqueueTimeoutJob was asked of a host that installed no job hooks (JS_SetHostJobHooks)");
    ctx->rt->host_jobs.enqueue_timeout(ctx, job, ms, ctx->rt->host_jobs.opaque);
    JS_FreeValue(ctx, job);
}

/* §25.4.3.14 DoWait's steps from GetWaiterList on, both modes, over arguments the machine already made primitive.
   Answers the not-equal or immediate-timeout result in *pdone, or the new Waiter Record and its list. */
static int js_atomics_dowait(JSContext *ctx, JSValueConst *argv, bool sync, JSValue *pdone, JSValue *plist,
                             JSValue *prec, JSValue *ppromise)
{
    JSArrayBuffer *abuf;
    JSObject *p;
    uint64_t idx;
    int64_t v;
    int32_t v32;
    double d, timeout, timeout_time;
    int size_log2;
    bool differ;
    void *ptr;
    JSValue list, rec, resolving[2], promise = JS_UNDEFINED;

    *pdone = *plist = *prec = *ppromise = JS_UNDEFINED;
    p = js_atomics_get_buf(ctx, argv[0], argv[1], &abuf, &idx, 2);
    if (!p)
        return -1;
    size_log2 = typed_array_size_log2(p->class_id);
    if (size_log2 == 3) {
        if (JS_ToBigInt64(ctx, &v, argv[2]))
            return -1;
    } else {
        if (JS_ToInt32(ctx, &v32, argv[2]))
            return -1;
        v = v32;
    }
    if (JS_ToFloat64(ctx, &d, argv[3]))
        return -1;
    timeout = isnan(d) ? INFINITY : d < 0 ? 0 : d;
    if (sync && !JS_AgentCanBlock(JS_GetRealmAgent(ctx))) {
        JS_ThrowTypeError(ctx, "cannot block in this thread");
        return -1;
    }
    CHECK(!sync || ctx->rt->host_jobs.now != NULL,
          "an agent that can block waited in a runtime whose host installed no clock (JSHostJobHooks.now), so its "
          "[[TimeoutTime]] has no time to be read on");
    CHECK(idx < p->u.array.count, "an Atomics wait reached its storage out of bounds - see js_atomics_get_buf");
    ptr = p->u.array.u.uint8_ptr + ((uintptr_t)idx << size_log2);
    list = js_get_waiter_list(ctx, abuf, p->u.typed_array->offset + ((uint32_t)idx << size_log2));
    differ = size_log2 == 3 ? *(int64_t *)ptr != v : *(int32_t *)ptr != v;
    if (differ || (!sync && timeout == 0)) {
        JS_FreeValue(ctx, list);
        *pdone = JS_AtomToString(ctx, differ ? JS_ATOM_not_equal : JS_ATOM_timed_out);
        return 0;
    }
    /* "Let now be the time value (UTC) identifying the current time": the host's. Only a blocking waiter's is
       kept, since the host wakes it at that time; a waitAsync's is only ever read back as `timeout`. */
    timeout_time = sync ? ctx->rt->host_jobs.now(ctx, ctx->rt->host_jobs.opaque) + timeout : INFINITY;
    rec = JS_NewArray(ctx);
    CHECK(!JS_IsException(rec), "a Waiter Record could not be allocated");
    if (sync) {
        js_waiter_set(ctx, rec, WR_RESOLVE, JS_NULL);   /* blocking */
        js_waiter_set(ctx, rec, WR_PROMISE, JS_UNDEFINED);
    } else {
        promise = JS_NewPromiseCapability(ctx, resolving);
        if (JS_IsException(promise)) {
            JS_FreeValue(ctx, rec);
            JS_FreeValue(ctx, list);
            return -1;
        }
        js_waiter_set(ctx, rec, WR_RESOLVE, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        js_waiter_set(ctx, rec, WR_PROMISE, js_dup(promise));
    }
    js_waiter_set(ctx, rec, WR_TIMEOUT_TIME, js_float64(timeout_time));
    js_waiter_set(ctx, rec, WR_RESULT, JS_AtomToString(ctx, JS_ATOM_ok));
    js_waiter_set(ctx, rec, WR_WOKEN, JS_FALSE);
    js_waiter_set(ctx, rec, WR_LIST, sync ? js_dup(list) : JS_UNDEFINED);
    /* §25.4.3.8 AddWaiter */
    CHECK(JS_SetPropertyUint32(ctx, list, js_waiter_list_length(ctx, list), js_dup(rec)) >= 0,
          "a Waiter Record could not be added to its WaiterList");
    if (!sync && isfinite(timeout))
        js_enqueue_wait_timeout_job(ctx, list, rec, timeout);
    *plist = list;
    *prec = rec;
    *ppromise = promise;
    return 0;
}

/* §25.4.16 Atomics.waitAsync ( ta, index, value, timeout ): DoWait ( async, … ), over primitive arguments. */
static JSValue js_atomics_wait_async(JSContext *ctx, JSValueConst this_obj, int argc, JSValueConst *argv)
{
    JSValue done, list, rec, promise, res;

    (void)this_obj; (void)argc;
    if (js_atomics_dowait(ctx, argv, false, &done, &list, &rec, &promise) < 0)
        return JS_EXCEPTION;
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, rec);
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) {
        JS_FreeValue(ctx, done);
        JS_FreeValue(ctx, promise);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValue(ctx, res, JS_ATOM_async, js_bool(JS_IsUndefined(done)), JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValue(ctx, res, JS_ATOM_value, JS_IsUndefined(done) ? promise : done,
                               JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

/* §25.4.10 Atomics.notify ( ta, index, count ), from step 5 on: the machine made the arguments primitive and
   validated the access against the length it captured first. */
static JSValue js_atomics_notify(JSContext *ctx, JSValueConst this_obj, int argc, JSValueConst *argv)
{
    int size_log2;
    int32_t count, n, i;
    uint64_t idx;
    JSObject *p;
    JSArrayBuffer *abuf;
    JSValue list;
    uint32_t len;

    (void)this_obj; (void)argc;
    p = JS_VALUE_GET_OBJ(argv[0]);
    abuf = p->u.typed_array->buffer->u.array_buffer;
    size_log2 = typed_array_size_log2(p->class_id);
    if (JS_ToIndex(ctx, &idx, argv[1]))   /* already primitive: invokes nothing */
        return JS_EXCEPTION;
    /* step 3: the count, whose coercion the machine has already made primitive */
    if (JS_IsUndefined(argv[2])) {
        count = INT32_MAX;
    } else {
        if (JS_ToInt32Clamp(ctx, &count, argv[2], 0, INT32_MAX, 0))
            return JS_EXCEPTION;
    }
    /* step 5: a non-shared buffer notifies nobody and returns +0, before anything looks at the memory */
    if (!abuf->shared)
        return js_int32(0);
    list = js_get_waiter_list(ctx, abuf, p->u.typed_array->offset + ((uint32_t)idx << size_log2));
    /* §25.4.3.10 RemoveWaiters ( waiterList, count ), then NotifyWaiter for each, in list order */
    len = js_waiter_list_length(ctx, list);
    n = (uint32_t)count < len ? count : (int32_t)len;
    {
        JSValue removed = JS_NewArray(ctx);
        CHECK(!JS_IsException(removed), "the removed Waiter Records could not be held");
        for (i = 0; i < n; i++)
            CHECK(JS_SetPropertyUint32(ctx, removed, (uint32_t)i, JS_GetPropertyUint32(ctx, list, (uint32_t)i)) >= 0,
                  "a removed Waiter Record could not be held");
        for (i = n; i < (int32_t)len; i++)
            CHECK(JS_SetPropertyUint32(ctx, list, (uint32_t)(i - n), JS_GetPropertyUint32(ctx, list, (uint32_t)i)) >= 0,
                  "a WaiterList could not be compacted");
        CHECK(JS_SetPropertyStr(ctx, list, "length", js_uint32(len - (uint32_t)n)) >= 0,
              "a WaiterList could not be shortened");
        for (i = 0; i < n; i++) {
            JSValue rec = JS_GetPropertyUint32(ctx, removed, (uint32_t)i);
            js_notify_waiter(ctx, rec);
            JS_FreeValue(ctx, rec);
        }
        JS_FreeValue(ctx, removed);
    }
    JS_FreeValue(ctx, list);
    return js_int32(n);
}

bool JS_SuspensionWoken(JSContext *ctx, JSValueConst rec)
{
    JSValue w = js_waiter_get(ctx, rec, WR_WOKEN);
    bool woken = JS_ToBool(ctx, w);

    DCHECK(JS_VALUE_GET_TAG(w) == JS_TAG_BOOL, "a suspension names no suspension record");
    JS_FreeValue(ctx, w);
    return woken;
}

JSValue JS_NewSuspension(JSContext *ctx, double timeout_time)
{
    JSValue rec = JS_NewArray(ctx);

    CHECK(!JS_IsException(rec), "a suspension record could not be allocated");
    DCHECK(!isnan(timeout_time), "a suspension's timeout time is NaN");
    js_waiter_set(ctx, rec, WR_TIMEOUT_TIME, js_float64(timeout_time));
    js_waiter_set(ctx, rec, WR_WOKEN, JS_FALSE);
    js_waiter_set(ctx, rec, WR_LIST, JS_UNDEFINED);
    return rec;
}

void JS_WakeSuspension(JSContext *ctx, JSValueConst rec)
{
    JSValue list = js_waiter_get(ctx, rec, WR_LIST);

    DCHECK(JS_IsUndefined(list), "a host woke a blocking Waiter Record; only NotifyWaiter or its timeout wakes one");
    JS_FreeValue(ctx, list);
    js_waiter_set(ctx, rec, WR_WOKEN, JS_TRUE);
}

double JS_SuspensionTimeoutTime(JSContext *ctx, JSValueConst rec)
{
    JSValue t = js_waiter_get(ctx, rec, WR_TIMEOUT_TIME);
    double d;

    DCHECK(JS_IsNumber(t), "a suspension record carries no timeout time");
    JS_ToFloat64(ctx, &d, t);   /* a number: invokes nothing */
    return d;
}

/* §25.4.3.11 SuspendThisAgent's wake "due to a timeout". A blocking waiter takes §25.4.3.15's timeoutJob steps:
   it is still in its list, since a NotifyWaiter that removed it also woke it. */
void JS_TimeOutSuspension(JSContext *ctx, JSValueConst rec)
{
    JSValue list = js_waiter_get(ctx, rec, WR_LIST);

    DCHECK(!JS_SuspensionWoken(ctx, rec), "a suspension was timed out after it was woken");
    if (!JS_IsUndefined(list)) {
        bool listed = js_remove_waiter(ctx, list, rec);
        DCHECK(listed, "a blocking waiter neither woken nor in its WaiterList");
        (void)listed;
        js_waiter_set(ctx, rec, WR_RESULT, JS_AtomToString(ctx, JS_ATOM_timed_out));
    }
    js_waiter_set(ctx, rec, WR_WOKEN, JS_TRUE);
    JS_FreeValue(ctx, list);
}

int step_suspend_run(JSContext *ctx, JSStepHdr *h, JSValueConst rec)
{
    (void)h;
    return step_suspend_on(ctx, rec);
}

// no-op: Atomics.pause() is not allowed to block or yield to another
// thread, only to hint the CPU that it should back off for a bit;
// the amount of work we do here is a good enough substitute
static JSValue js_atomics_pause(JSContext *ctx, JSValueConst this_obj,
                                int argc, JSValueConst *argv)
{
    double d;

    if (argc > 0) {
        switch (JS_VALUE_GET_NORM_TAG(argv[0])) {
        case JS_TAG_FLOAT64: // accepted if and only if fraction == 0.0
            d = JS_VALUE_GET_FLOAT64(argv[0]);
            if (isfinite(d))
                if (0 == modf(d, &d))
                    break;
            // fallthru
        default:
            return JS_ThrowTypeError(ctx, "not an integral number");
        case JS_TAG_UNDEFINED:
        case JS_TAG_INT:
            break;
        }
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_atomics_funcs[] = {
    JS_CFUNC_STEP_DEF("add", 3, STEPDEF_ATOMICS_ADD ),
    JS_CFUNC_STEP_DEF("and", 3, STEPDEF_ATOMICS_AND ),
    JS_CFUNC_STEP_DEF("or", 3, STEPDEF_ATOMICS_OR ),
    JS_CFUNC_STEP_DEF("sub", 3, STEPDEF_ATOMICS_SUB ),
    JS_CFUNC_STEP_DEF("xor", 3, STEPDEF_ATOMICS_XOR ),
    JS_CFUNC_STEP_DEF("exchange", 3, STEPDEF_ATOMICS_EXCHANGE ),
    JS_CFUNC_STEP_DEF("compareExchange", 4, STEPDEF_ATOMICS_CMPXCHG ),
    JS_CFUNC_STEP_DEF("load", 2, STEPDEF_ATOMICS_LOAD ),
    JS_CFUNC_STEP_DEF("store", 3, STEPDEF_ATOMICS_STORE ),
    JS_CFUNC_STEP_DEF("isLockFree", 1, STEPDEF_ATOMICS_ISLOCKFREE ),
    JS_CFUNC_DEF("pause", 0, js_atomics_pause ),
    JS_CFUNC_STEP_DEF("wait", 4, STEPDEF_ATOMICS_WAIT ),
    JS_CFUNC_STEP_DEF("waitAsync", 4, STEPDEF_ATOMICS_WAITASYNC ),
    JS_CFUNC_STEP_DEF("notify", 3, STEPDEF_ATOMICS_NOTIFY ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Atomics", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_atomics_obj[] = {
    JS_OBJECT_DEF("Atomics", js_atomics_funcs, countof(js_atomics_funcs), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),
};

/* TODO(saghul) make this public and not dependent on typed arrays? */
static int JS_AddIntrinsicAtomics(JSContext *ctx)
{
    /* add Atomics as autoinit object */
    return JS_SetPropertyFunctionList(ctx, ctx->global_obj, js_atomics_obj, countof(js_atomics_obj));
}

#endif /* CONFIG_ATOMICS */

