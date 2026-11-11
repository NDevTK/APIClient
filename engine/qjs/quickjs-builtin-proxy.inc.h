/* Proxy */

static void js_proxy_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSProxyData *s = JS_GetOpaque(val, JS_CLASS_PROXY);
    if (s) {
        JS_FreeValueRT(rt, s->target);
        JS_FreeValueRT(rt, s->handler);
        js_free_rt(rt, s);
    }
}

static void js_proxy_mark(JSRuntime *rt, JSValueConst val,
                          JS_MarkFunc *mark_func)
{
    JSProxyData *s = JS_GetOpaque(val, JS_CLASS_PROXY);
    if (s) {
        JS_MarkValue(rt, s->target, mark_func);
        JS_MarkValue(rt, s->handler, mark_func);
    }
}

static JSValue JS_ThrowTypeErrorRevokedProxy(JSContext *ctx)
{
    return JS_ThrowTypeError(ctx, "revoked proxy");
}

/* PROXY [[Call]] — 10.5.12, as ONE machine, for the reason a Promise resolving function is one: step 5 is
   `GetMethod(handler, "apply")`, which is the PAGE'S CODE. It is an ordinary [[Get]], so the handler may be an
   accessor-bearing object or ANOTHER PROXY with a `get` trap, and the operator site read it with JS_GetProperty
   from C — an activation with no flow base. `new Proxy(f, new Proxy({}, {get(){…}}))()` aborted at
   js_proxy_get's DFAIL, and a handler with a plain `get apply(){ for(;;){} }` accessor ran that loop off the
   tramp without even that much notice.
   As a machine the trap read is a request like any other, so every handler shape suspends. Two more things
   follow from it rather than being arranged: the TRAPLESS forward (step 6) is a CALL request against the
   target, which the driver re-dispatches — so a proxy wrapping a proxy wrapping a function re-enters this
   machine per hop and needs no walk of its own; and the trap CALL (step 8) is an ordinary dispatch, so a trap
   that is a bound function, a C function, a step machine or another proxy is answered by the one convergence
   point instead of by a recognizer asking whether it had a bytecode body. */
typedef struct JSProxyCall {
    JSStepHdr hdr;      /* MUST be first; hdr.func_obj IS the proxy, hdr.this_val the thisArgument */
    JSValue result;     /* owned: the call's result */
    JSValue *cb;        /* [this, fn, args…] — its own block: the trapless shape is argc-sized, the trap's is 3 */
    int ncb;
} JSProxyCall;

static JSValue js_create_array(JSContext *ctx, int len, JSValueConst *tab);

/* THE TWO ARMS COMPLETE AT DIFFERENT STEPS, so they are different stages: a machine parked in the trapless
   forward is inside step 6 and one parked in the trap call is inside step 8, and one stage for both would name
   whichever of them was written down. [[Construct]]'s twin already had to split them, because its step 10 applies
   to one arm and not the other. */
#define PXCALL_STAGES(X) \
    X(PXCALL_ENTRY,   "10.5.12 steps 1-4 (handler is O.[[ProxyHandler]] and is not null; target is O.[[ProxyTarget]])") \
    X(PXCALL_GETTRAP, "10.5.12 step 5 (trap is GetMethod(handler, \"apply\"))") \
    X(PXCALL_FORWARD, "10.5.12 step 6 (Call(target, thisArgument, argumentsList))") \
    X(PXCALL_TRAP,    "10.5.12 step 8 (Call(trap, handler, <<target, thisArgument, argArray>>))")
enum { PXCALL_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_proxy_call_steps[] = { PXCALL_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_proxy_call_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSProxyCall *s = st;
    JSProxyData *pd = JS_GetOpaque(s->hdr.func_obj, JS_CLASS_PROXY);
    int i, n;

    DCHECK(pd != NULL, "the Proxy [[Call]] machine was entered on a callee that is not a Proxy");
    if (s->hdr.stage == PXCALL_ENTRY) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->result = JS_UNDEFINED;
        if (pd->is_revoked) { JS_ThrowTypeErrorRevokedProxy(ctx); return -1; }
        /* A proxy whose target is not callable HAS no [[Call]] — the TypeError is the caller's and precedes
           every step, which is why it is asked before the handler read rather than after it. */
        if (!pd->is_func) { JS_ThrowTypeErrorNotAFunction(ctx); return -1; }
        s->hdr.stage = PXCALL_GETTRAP;
    }
    if (s->hdr.stage == PXCALL_GETTRAP) {
        JSValue trap;
        int r = step_getprop_run(ctx, &s->hdr, pd->handler, JS_ATOM_apply, cb_result, &trap, out_cb, out_argc);
        if (r) return r < 0 ? -1 : r;
        if (JS_IsUndefined(trap) || JS_IsNull(trap)) {
            /* step 6: Call(target, thisArgument, argumentsList). */
            JS_FreeValue(ctx, trap);
            s->hdr.stage = PXCALL_FORWARD;
            n = s->hdr.argc;
            s->cb = js_malloc(ctx, sizeof(JSValue) * (size_t)(n + 2));
            if (unlikely(!s->cb)) return -1;
            s->ncb = n + 2;
            s->cb[0] = js_dup(s->hdr.this_val);
            s->cb[1] = js_dup(pd->target);
            for (i = 0; i < n; i++) s->cb[i + 2] = js_dup(s->hdr.argv[i]);
            *out_cb = s->cb; *out_argc = n;
            return 3;   /* CALL */
        }
        /* 7.3.11 GetMethod step 4: a present trap that is not callable is a TypeError HERE, before step 7's
           CreateArrayFromList. */
        if (!JS_IsFunction(ctx, trap)) {
            JS_FreeValue(ctx, trap);
            JS_ThrowTypeErrorNotAFunction(ctx);
            return -1;
        }
        {
            JSValue arr = js_create_array(ctx, s->hdr.argc, (JSValueConst *)s->hdr.argv);
            if (JS_IsException(arr)) { JS_FreeValue(ctx, trap); return -1; }
            s->cb = js_malloc(ctx, sizeof(JSValue) * 5);
            if (unlikely(!s->cb)) { JS_FreeValue(ctx, trap); JS_FreeValue(ctx, arr); return -1; }
            s->ncb = 5;
            s->cb[0] = js_dup(pd->handler);   /* the trap's `this` */
            s->cb[1] = trap;                  /* owned */
            s->cb[2] = js_dup(pd->target);
            s->cb[3] = js_dup(s->hdr.this_val);
            s->cb[4] = arr;
            s->hdr.stage = PXCALL_TRAP;
            *out_cb = s->cb; *out_argc = 3;
            return 3;   /* CALL: trap(target, thisArg, argArray) */
        }
    }
    /* [[Call]] has NO result invariant — unlike [[Get]] and [[Construct]], 10.5.12 returns the trap's value
       unexamined — so both arms complete here and nothing else happens. */
    DCHECK(s->hdr.stage == PXCALL_FORWARD || s->hdr.stage == PXCALL_TRAP, "Proxy [[Call]]: unknown stage");
    s->result = cb_result;
    return 0;
}

/* PROXY [[Construct]] — 10.5.13, the [[Call]] machine's twin and for the identical reason: step 6 is
   `GetMethod(handler, "construct")`, the page's [[Get]]. `new (new Proxy(f, new Proxy({}, {get(){…}})))()`
   aborted at js_proxy_get's DFAIL from js_tramp_proxy_construct's C read.
   Step 10's must-return-an-Object check is the one thing this owes that [[Call]] does not, and it RIDES the
   trap's own stage: the check is on the trap's RESULT, so the stage the machine is parked in while the trap runs
   is the stage that performs it — which is what deletes CONT_PROXY_CONSTRUCT and the JSProxyCtor that carried an
   outer sequence across it. The trapless arm (step 7) is a CONSTRUCT request against the target, so a proxy
   wrapping a proxy re-enters this machine per hop; step 10 does not apply to it, because Construct already
   guarantees an object, which is why the two arms complete at different stages. */
typedef struct JSProxyConstruct {
    JSStepHdr hdr;      /* MUST be first; hdr.func_obj IS the proxy, hdr.this_val the NEW TARGET */
    JSValue result;     /* owned */
    JSValue *cb;        /* [target, args…] for the trapless CONSTRUCT, or [handler, trap, target, arr, ntgt] */
    int ncb;
} JSProxyConstruct;

/* the two completions differ in whether step 10's check applies, which is why they are two stages rather than
   one: they rest at different steps of the algorithm. */
#define PXCTOR_STAGES(X) \
    X(PXC_ST_ENTRY,   "10.5.13 steps 1-5 (handler is O.[[ProxyHandler]] and is not null; target is a constructor)") \
    X(PXC_ST_GETTRAP, "10.5.13 step 6 (trap is GetMethod(handler, \"construct\"))") \
    X(PXC_ST_TRAP,    "10.5.13 steps 9-10 (newObj is Call(trap, handler, <<target, argArray, newTarget>>), and newObj must be an Object)") \
    X(PXC_ST_FORWARD, "10.5.13 step 7 (Construct(target, argumentsList, newTarget))")
enum { PXCTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_proxy_construct_steps[] = { PXCTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

static JSValue JS_ThrowTypeErrorNotAnObject(JSContext *ctx);

static int js_proxy_construct_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSProxyConstruct *s = st;
    JSProxyData *pd = JS_GetOpaque(s->hdr.func_obj, JS_CLASS_PROXY);
    int i, n;

    DCHECK(pd != NULL, "the Proxy [[Construct]] machine was entered on a target that is not a Proxy");
    if (s->hdr.stage == PXC_ST_ENTRY) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->result = JS_UNDEFINED;
        if (pd->is_revoked) { JS_ThrowTypeErrorRevokedProxy(ctx); return -1; }
        /* a proxy whose target is not a constructor HAS no [[Construct]]: the TypeError precedes every step */
        if (!JS_IsConstructor(ctx, pd->target)) { JS_ThrowTypeErrorNotAConstructor(ctx, pd->target); return -1; }
        s->hdr.stage = PXC_ST_GETTRAP;
    }
    if (s->hdr.stage == PXC_ST_GETTRAP) {
        JSValue trap;
        int r = step_getprop_run(ctx, &s->hdr, pd->handler, JS_ATOM_construct, cb_result, &trap, out_cb, out_argc);
        if (r) return r < 0 ? -1 : r;
        n = s->hdr.argc;
        if (JS_IsUndefined(trap) || JS_IsNull(trap)) {
            /* step 7: Construct(target, argumentsList, newTarget). */
            JS_FreeValue(ctx, trap);
            s->cb = js_malloc(ctx, sizeof(JSValue) * (size_t)(n + 1));
            if (unlikely(!s->cb)) return -1;
            s->ncb = n + 1;
            s->cb[0] = js_dup(pd->target);
            for (i = 0; i < n; i++) s->cb[i + 1] = js_dup(s->hdr.argv[i]);
            s->hdr.ctor_ntgt = js_dup(s->hdr.this_val);
            s->hdr.stage = PXC_ST_FORWARD;
            *out_cb = s->cb; *out_argc = n;
            return 4;   /* CONSTRUCT */
        }
        if (!JS_IsFunction(ctx, trap)) {   /* 7.3.11 GetMethod step 4, before step 8's CreateArrayFromList */
            JS_FreeValue(ctx, trap);
            JS_ThrowTypeErrorNotAFunction(ctx);
            return -1;
        }
        {
            JSValue arr = js_create_array(ctx, n, (JSValueConst *)s->hdr.argv);
            if (JS_IsException(arr)) { JS_FreeValue(ctx, trap); return -1; }
            s->cb = js_malloc(ctx, sizeof(JSValue) * 5);
            if (unlikely(!s->cb)) { JS_FreeValue(ctx, trap); JS_FreeValue(ctx, arr); return -1; }
            s->ncb = 5;
            s->cb[0] = js_dup(pd->handler);   /* the trap's `this` */
            s->cb[1] = trap;                  /* owned */
            s->cb[2] = js_dup(pd->target);
            s->cb[3] = arr;
            s->cb[4] = js_dup(s->hdr.this_val);   /* newTarget */
            s->hdr.stage = PXC_ST_TRAP;
            *out_cb = s->cb; *out_argc = 3;
            return 3;   /* CALL: trap(target, argArray, newTarget) */
        }
    }
    if (s->hdr.stage == PXC_ST_TRAP && JS_VALUE_GET_TAG(cb_result) != JS_TAG_OBJECT) {
        /* step 10. It applies to the TRAP's value only — the forward arm's Construct answers with an object by
           construction, and running the check there would be asserting something the callee already owes. */
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeErrorNotAnObject(ctx);
        return -1;
    }
    DCHECK(s->hdr.stage == PXC_ST_TRAP || s->hdr.stage == PXC_ST_FORWARD, "Proxy [[Construct]]: unknown stage");
    s->result = cb_result;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The same shape as the [[Call]] machine's. */
static void js_proxy_construct_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSProxyConstruct *s = st;
    v->val(ctx, &s->result);
    v->array(ctx, (void **)&s->cb, sizeof(JSValue), s->ncb, s->ncb, js_step_visit_value_elem);
}

static JSValue js_proxy_construct_fini(JSContext *ctx, void *st, bool take_result)
{
    JSProxyConstruct *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static const JSTrampStepDef js_proxy_construct_def = {
    sizeof(JSProxyConstruct), js_proxy_construct_step, js_proxy_construct_fini, 0,
    .visit = js_proxy_construct_visit,
    .algorithm = "10.5.13 [[Construct]] ( argumentsList, newTarget ) of a Proxy exotic object",
    .steps = js_proxy_construct_steps
};

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). Its result and its own request block, whose LENGTH depends on
   which of the two shapes step 5's trap chose — which is why the count rides the state. */
static void js_proxy_call_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSProxyCall *s = st;
    v->val(ctx, &s->result);
    v->array(ctx, (void **)&s->cb, sizeof(JSValue), s->ncb, s->ncb, js_step_visit_value_elem);
}

static JSValue js_proxy_call_fini(JSContext *ctx, void *st, bool take_result)
{
    JSProxyCall *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static const JSTrampStepDef js_proxy_call_def = {
    sizeof(JSProxyCall), js_proxy_call_step, js_proxy_call_fini, 0, .visit = js_proxy_call_visit,
    .algorithm = "10.5.12 [[Call]] ( thisArgument, argumentsList ) of a Proxy exotic object",
    .steps = js_proxy_call_steps
};

static JSProxyData *get_proxy_method(JSContext *ctx, JSValue *pmethod,
                                     JSValueConst obj, JSAtom name)
{
    JSProxyData *s = JS_GetOpaque(obj, JS_CLASS_PROXY);
    JSValue method;

    /* safer to test recursion in all proxy methods */
    if (js_check_stack_overflow(ctx->rt, 0)) {
        JS_ThrowStackOverflow(ctx);
        return NULL;
    }

    /* 's' should never be NULL */
    if (s->is_revoked) {
        JS_ThrowTypeErrorRevokedProxy(ctx);
        return NULL;
    }
    method = JS_GetProperty(ctx, s->handler, name);
    if (JS_IsException(method))
        return NULL;
    if (JS_IsNull(method))
        method = JS_UNDEFINED;
    *pmethod = method;
    return s;
}

/* Resolve the TRAPLESS PREFIX of a proxy chain for one operation. Every 10.5.x method forwards a trapless
   operation to the SAME operation on its target, and each hook spelled that forward as a call back into the
   generic entry — which re-enters this very hook when the target is itself a Proxy. That cost one C activation
   per hop, so a chain of trapless proxies hit the stack floor and was reported as a soft RangeError: a bound
   wearing an error's clothes, and one that every C caller of JS_GetProperty/JS_SetProperty inherited (reading
   `newTarget.prototype` through such a chain is why `new` died long after the interpreter's own entry had
   stopped recursing). Walking it here makes every hook's forward FLAT, and it is not a shortcut past anything
   observable: each hop still reads its own handler's trap, in order, exactly as the recursion did.
   Returns the node whose TRAP must run (*pmethod holds it), or the last proxy of the chain with *pmethod
   undefined — whose non-proxy target the caller forwards to as before. NULL with an exception pending on a
   revoked proxy or a throwing handler read. The chain needs no cycle guard: a proxy's [[ProxyTarget]] is fixed
   at construction and the target must already exist to be passed, so every hop moves to a strictly older
   object. */
/* DELETED: proxy_resolve_trapless. It was the shared prologue of every C-driven Proxy internal method — the
   revoked check and the READ of the trap off the handler, which is itself the page's code (a handler can be a Proxy,
   or carry an accessor). Its last caller went when the thirteenth hook body did. The routed path reads a trap
   through CONT_TRAP_GET, which is that read as a request. */

/* 10.5.1 steps 6-10 on the trap's RESULT: an Object or null, and — when the target is not extensible — the same
   value the target's own [[GetPrototypeOf]] gives. The trap CALL is the page's code and the check is not, the
   same split every proxy operation makes, so this is shared by the C hook and the tramp delivery.
   CONSUMES `ret`. */
static JSValue js_proxy_getproto_invariant(JSContext *ctx, JSValueConst obj, JSValue ret)
{
    JSProxyData *s = JS_VALUE_GET_OBJ(obj)->u.opaque;
    JSValue proto1;
    int res;

    if (JS_IsException(ret))
        return ret;
    if (JS_VALUE_GET_TAG(ret) != JS_TAG_NULL && JS_VALUE_GET_TAG(ret) != JS_TAG_OBJECT)
        goto fail;
    res = JS_IsExtensible(ctx, s->target);
    if (res < 0) { JS_FreeValue(ctx, ret); return JS_EXCEPTION; }
    if (!res) {
        proto1 = JS_GetPrototype(ctx, s->target);
        if (JS_IsException(proto1)) { JS_FreeValue(ctx, ret); return JS_EXCEPTION; }
        if (JS_VALUE_GET_OBJ(proto1) != JS_VALUE_GET_OBJ(ret)) {
            JS_FreeValue(ctx, proto1);
            goto fail;
        }
        JS_FreeValue(ctx, proto1);
    }
    return ret;
fail:
    JS_FreeValue(ctx, ret);
    return JS_ThrowTypeError(ctx, "proxy: inconsistent prototype");
}

/* 10.5.2 steps 7-9 on the trap's BOOLEAN: a true result is unconditionally fine while the TARGET is extensible;
   once it is not, the prototype the trap claims to have installed must be the one the target already has. Only
   the target is needed, which is what the request carries. Shared by the C hook and the tramp delivery. */
static int js_proxy_setproto_invariant(JSContext *ctx, JSValueConst target, JSValueConst proto_val, int res)
{
    JSValue proto1;
    int res2;

    if (res < 0)
        return -1;
    if (!res)
        return false;
    res2 = JS_IsExtensible(ctx, target);
    if (res2 < 0)
        return -1;
    if (!res2) {
        proto1 = JS_GetPrototype(ctx, target);
        if (JS_IsException(proto1))
            return -1;
        if (JS_VALUE_GET_OBJ(proto_val) != JS_VALUE_GET_OBJ(proto1)) {
            JS_FreeValue(ctx, proto1);
            JS_ThrowTypeError(ctx, "proxy: inconsistent prototype");
            return -1;
        }
        JS_FreeValue(ctx, proto1);
    }
    return true;
}

/* 10.5.3 step 7 / 10.5.4 step 8 on the trap's BOOLEAN. isExtensible: it must MATCH the target's own answer.
   preventExtensions: a true result requires the target to be non-extensible already. The trap CALL is the page's
   code and the check is not, so this is shared by the C hooks and the tramp delivery. */
static int js_proxy_ext_invariant(JSContext *ctx, JSValueConst obj, bool prevent, int res)
{
    JSProxyData *s = JS_VALUE_GET_OBJ(obj)->u.opaque;
    int res2;

    if (res < 0)
        return -1;
    if (prevent) {
        if (!res)
            return false;
        res2 = JS_IsExtensible(ctx, s->target);
        if (res2 < 0)
            return -1;
        if (res2) {
            JS_ThrowTypeError(ctx, "proxy: inconsistent preventExtensions");
            return -1;
        }
        return true;
    }
    res2 = JS_IsExtensible(ctx, s->target);
    if (res2 < 0)
        return -1;
    if (res != res2) {
        JS_ThrowTypeError(ctx, "proxy: inconsistent isExtensible");
        return -1;
    }
    return res;
}

static JSValue js_proxy_getPrototypeOf(JSContext *ctx, JSValueConst obj)
{
    /* THE BODY IS DELETED. Every consumer is routed, and the whole corpus runs with this DFAIL armed without
       reaching it — so what stood here was a second implementation of 10.5.1 — the `getPrototypeOf` trap and its invariant against a non-extensible target,
       run from an activation with no flow base. In RELEASE the DFAIL compiles out and there is deliberately
       nothing to fall back to: FAILING VISIBLY is right where answering wrongly is not. */
    DFAIL("a C-side [[GetPrototypeOf]] reached a Proxy — the page's trap has no flow base here; route that caller onto the keyed entry's GP_GETPROTO");
    (void)obj;
    JS_ThrowInternalError(ctx, "[[GetPrototypeOf]] on a Proxy is only implemented on the interpreter's chain");
    return JS_EXCEPTION;
}

static int js_proxy_setPrototypeOf(JSContext *ctx, JSValueConst obj,
                                   JSValueConst proto_val, bool throw_flag)
{
    /* THE BODY IS DELETED. Every consumer is routed, and the whole corpus runs with this DFAIL armed without
       reaching it — so what stood here was a second implementation of 10.5.2 — the `setPrototypeOf` trap and its invariant,
       run from an activation with no flow base. In RELEASE the DFAIL compiles out and there is deliberately
       nothing to fall back to: FAILING VISIBLY is right where answering wrongly is not. */
    DFAIL("a C-side [[SetPrototypeOf]] reached a Proxy — the page's trap has no flow base here; route that caller onto the keyed entry's GP_SETPROTO");
    (void)obj; (void)proto_val; (void)throw_flag;
    JS_ThrowInternalError(ctx, "[[SetPrototypeOf]] on a Proxy is only implemented on the interpreter's chain");
    return -1;
}

static int js_proxy_isExtensible(JSContext *ctx, JSValueConst obj)
{
    /* THE BODY IS DELETED. Every consumer is routed, and the whole corpus runs with this DFAIL armed without
       reaching it — so what stood here was a second implementation of 10.5.3 — the `isExtensible` trap and its must-agree-with-the-target invariant,
       run from an activation with no flow base. In RELEASE the DFAIL compiles out and there is deliberately
       nothing to fall back to: FAILING VISIBLY is right where answering wrongly is not. */
    DFAIL("a C-side [[IsExtensible]] reached a Proxy — the page's trap has no flow base here; route that caller onto the keyed entry's GP_ISEXT");
    (void)obj;
    JS_ThrowInternalError(ctx, "[[IsExtensible]] on a Proxy is only implemented on the interpreter's chain");
    return -1;
}

static int js_proxy_preventExtensions(JSContext *ctx, JSValueConst obj)
{
    /* THE BODY IS DELETED. Every consumer is routed, and the whole corpus runs with this DFAIL armed without
       reaching it — so what stood here was a second implementation of 10.5.4 — the `preventExtensions` trap and its invariant,
       run from an activation with no flow base. In RELEASE the DFAIL compiles out and there is deliberately
       nothing to fall back to: FAILING VISIBLY is right where answering wrongly is not. */
    DFAIL("a C-side [[PreventExtensions]] reached a Proxy — the page's trap has no flow base here; route that caller onto the keyed entry's GP_PREVEXT");
    (void)obj;
    JS_ThrowInternalError(ctx, "[[PreventExtensions]] on a Proxy is only implemented on the interpreter's chain");
    return -1;
}

/* [[HasProperty]] invariant: a trap that reports ABSENT must be telling the truth about a non-configurable own
   property of the target, and about a non-extensible target. ONE implementation, shared by the C path and by the
   trampolined trap's continuation — the check runs on the trap's RESULT, so the tramp path can only honour it
   from a continuation, exactly as [[Get]]'s does. `ret` is the trap's boolean; returns it, or -1 having thrown. */
/* DELETED: js_proxy_has_invariant. 10.5.7 step 9's check, performed with the target's [[GetOwnProperty]] and
   [[IsExtensible]] read FROM C — its own traps when the target is a Proxy. The routed path performs the same check
   over a JSDescFacts it obtained by request (CONT_KEYED_INV), and this survived only for the C hook whose body is
   gone. */

static int js_proxy_has(JSContext *ctx, JSValueConst obj, JSAtom atom)
{
    /* THE BODY IS DELETED. Every consumer is routed onto the keyed entry's GP_HAS and the whole corpus runs with
       this DFAIL armed without reaching it, so what stood here was a second implementation of 10.5.7 — the `has` trap, plus its invariant's [[GetOwnProperty]] and [[IsExtensible]] on the target,
       run from an activation with no flow base. In RELEASE the DFAIL compiles out and this FAILS VISIBLY rather
       than answering from the proxy's own empty shape — the honest outcome for a capability that only the routed
       path implements. */
    DFAIL("a C-side [[HasProperty]] reached a Proxy — the page's trap has no flow base here; route that caller onto the keyed entry's GP_HAS");
    (void)atom; (void)obj;
    JS_ThrowInternalError(ctx, "[[HasProperty]] on a Proxy is only implemented on the interpreter's chain");
    return -1;
}

/* [[Get]] invariant: a trap result for a non-configurable/non-writable own property of the target must be SameValue
   as the target's value, and must be undefined for a non-configurable accessor with no getter. ONE implementation,
   shared by the C path and by the trampolined trap's continuation — the check runs on the trap's RESULT, so the
   tramp path can only honour it from a continuation, which is what CONT_GETPROP carries. Consumes `ret`. */
/* The PURE tails of the three keyed invariants that compare the target descriptor's FIELDS, over the record a
   [[GetOwnProperty]] request answers into. No user code, so both forms reach the same three functions and only
   HOW the record was obtained differs — the C hooks read it from C, the machines ask for it. 0 = consistent,
   -1 = threw. */
static int js_proxy_get_check(JSContext *ctx, const JSDescFacts *f, JSValueConst ret)
{
    if (f->flags < 0)
        return 0;
    if ((f->flags & (JS_PROP_GETSET | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)) == 0) {
        if (!js_same_value(ctx, f->value, ret))
            goto fail;
    } else if ((f->flags & (JS_PROP_GETSET | JS_PROP_CONFIGURABLE)) == JS_PROP_GETSET) {
        if (JS_IsUndefined(f->getter) && !JS_IsUndefined(ret))
            goto fail;
    }
    return 0;
fail:
    JS_ThrowTypeError(ctx, "proxy: inconsistent get");
    return -1;
}

static int js_proxy_set_check(JSContext *ctx, const JSDescFacts *f, JSValueConst value)
{
    if (f->flags < 0)
        return 0;
    if ((f->flags & (JS_PROP_GETSET | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)) == 0) {
        if (!js_same_value(ctx, f->value, value))
            goto fail;
    } else if ((f->flags & (JS_PROP_GETSET | JS_PROP_CONFIGURABLE)) == JS_PROP_GETSET
               && JS_IsUndefined(f->setter)) {
        goto fail;
    }
    return 0;
fail:
    JS_ThrowTypeError(ctx, "proxy: inconsistent set");
    return -1;
}

/* `extensible` CHANGES NO ANSWER HERE WHEN THE TARGET HAS THE PROPERTY, AND IS STILL ALWAYS READ. 10.1.6.3
   ValidateAndApplyPropertyDescriptor consults it only at step 2.a, inside "If current is undefined" — so on the
   present-property arm (10.5.6 step 15.a's IsCompatiblePropertyDescriptor) its value is unused, and skipping the
   read looks free.
   IT IS NOT FREE, BECAUSE THE READ IS THE SIDE EFFECT. On a Proxy target IsExtensible runs that target's
   `isExtensible` trap, which is the page's code: it can log, mutate, throw, or hold a loop the scheduler must
   park in. 10.5.6 step 11 performs it UNCONDITIONALLY, before step 14 asks whether targetDesc is undefined, so a
   `defineProperty` that reports success on a target that already has the key must still fire that trap — and a
   page distinguishes fired from not-fired directly. A caller therefore passes what IsExtensible ANSWERED; there
   is no arm that may pass a constant.
   (The skip that used to live here cited "10.5.6 step 16.a". §10.5.6 step 16 is "Return true" and has no
   sub-steps; the step it needed does not exist.) */
static int js_proxy_define_check(JSContext *ctx, const JSDescFacts *f, JSValueConst val,
                                 JSValueConst getter, JSValueConst setter, int flags, int extensible)
{
    bool setting_not_configurable =
        ((flags & (JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE)) == JS_PROP_HAS_CONFIGURABLE);
    if (f->flags < 0) {
        if (!extensible || setting_not_configurable)
            goto fail;
        return 0;
    }
    if (!check_define_prop_flags(f->flags, flags)
        || ((f->flags & JS_PROP_CONFIGURABLE) && setting_not_configurable))
        goto fail;
    if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) {
        if ((f->flags & (JS_PROP_GETSET | JS_PROP_CONFIGURABLE)) == JS_PROP_GETSET) {
            if ((flags & JS_PROP_HAS_GET) && !js_same_value(ctx, getter, f->getter))
                goto fail;
            if ((flags & JS_PROP_HAS_SET) && !js_same_value(ctx, setter, f->setter))
                goto fail;
        }
    } else if (flags & JS_PROP_HAS_VALUE) {
        /* a non-configurable, NON-writable data property pins its value (10.5.6 step 16 ->
           ValidateAndApplyPropertyDescriptor step 4.d.ii). A non-configurable WRITABLE one does not: its value
           may change freely, and only its writable BIT is protected — which is the rule below. */
        if ((f->flags & (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)) == 0
            && !js_same_value(ctx, val, f->value))
            goto fail;
    }
    /* ...and that bit is protected only against a descriptor that actually CARRIES it. Both tests here read the
       WRITABLE bit without first asking whether the descriptor HAS a [[Writable]] field, and an absent field
       reads as false — so a value-only descriptor against a non-configurable writable target was rejected.
       `Reflect.defineProperty(proxyOverArray, "length", {value: 0})` is the shape, and OrdinarySetWithOwnDescriptor
       builds exactly that descriptor, so every `proxy.length = n` through a forwarding trap threw. */
    if ((flags & (JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE)) == JS_PROP_HAS_WRITABLE) {
        if ((f->flags & (JS_PROP_GETSET | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)) == JS_PROP_WRITABLE)
            goto fail;
    }
    return 0;
fail:
    JS_ThrowTypeError(ctx, "proxy: inconsistent defineProperty");
    return -1;
}

/* Read a target's own descriptor into the record form, FROM C. This is the unrouted half — on a Proxy target it
   runs its `getOwnPropertyDescriptor` trap here — and it exists so the C hooks reach the same three checks the
   machines do. Returns 0 with `f` filled (flags -1 = absent), -1 having thrown. */
static int js_proxy_facts_from_c(JSContext *ctx, JSValueConst target, JSAtom atom, JSDescFacts *f)
{
    JSPropertyDescriptor d;
    int res;
    js_desc_facts_init(f);
    /* The target of a proxy MAY ITSELF BE A PROXY, and then this reads the inner trap from C. The routed path
       performs the same facts as requests (JSGopdDesc's GD_TARGET), so reaching here with a proxied target
       means the unrouted C hook is still live for that consumer — the thing to find, not to tolerate. */
    DCHECK(JS_VALUE_GET_OBJ(target)->class_id != JS_CLASS_PROXY,
           "a proxy invariant read its TARGET from C and the target is itself a Proxy — the inner trap would "
           "run with no flow base; route that consumer onto the request path");
    res = JS_GetOwnPropertyInternal(ctx, &d, JS_VALUE_GET_OBJ(target), atom);
    if (res < 0)
        return -1;
    if (res) {
        f->flags = d.flags;
        f->value = d.value; f->getter = d.getter; f->setter = d.setter;   /* ownership transferred */
    }
    return 0;
}


static JSValue js_proxy_get_invariant(JSContext *ctx, JSValueConst target, JSAtom atom, JSValue ret)
{
    JSDescFacts f;
    if (JS_IsException(ret))
        return ret;
    if (js_proxy_facts_from_c(ctx, target, atom, &f) < 0) {
        JS_FreeValue(ctx, ret);
        return JS_EXCEPTION;
    }
    if (js_proxy_get_check(ctx, &f, ret) < 0) {
        js_desc_facts_free(ctx, &f);
        JS_FreeValue(ctx, ret);
        return JS_EXCEPTION;
    }
    js_desc_facts_free(ctx, &f);
    return ret;
}

static JSValue js_proxy_get(JSContext *ctx, JSValueConst obj, JSAtom atom,
                            JSValueConst receiver)
{
    /* THE BODY IS DELETED. Every consumer is routed onto the keyed entry's GP_GET and the whole corpus runs with
       this DFAIL armed without reaching it, so what stood here was a second implementation of 10.5.8 — the `get` trap and its result invariant against the target's own descriptor,
       run from an activation with no flow base. In RELEASE the DFAIL compiles out and this FAILS VISIBLY rather
       than answering from the proxy's own empty shape — the honest outcome for a capability that only the routed
       path implements. */
    DFAIL("a C-side [[Get]] reached a Proxy — the page's trap has no flow base here; route that caller onto the keyed entry's GP_GET");
    (void)atom; (void)obj; (void)receiver;
    JS_ThrowInternalError(ctx, "[[Get]] on a Proxy is only implemented on the interpreter's chain");
    return JS_EXCEPTION;
}

/* [[Set]] invariant: a trap that reports SUCCESS must be telling the truth — a non-configurable, non-writable data
   property of the target must have been written its own value (SameValue), and a non-configurable accessor with no
   setter cannot have been written at all. ONE implementation, shared by the C path and by the trampolined trap's
   continuation — the check runs on the trap's RESULT, so the tramp path can only honour it from a continuation, and
   that is why the continuation has to carry the written VALUE as well as (target, key). `ret` is the trap's boolean;
   returns it, or -1 having thrown. The strict-mode throw a `false` result owes stays with the caller, which is the
   only place that knows the write's flags. */
/* DELETED: js_proxy_set_invariant, 10.5.9 step 9's check with the target's descriptor read from C. */

static int js_proxy_set(JSContext *ctx, JSValueConst obj, JSAtom atom,
                        JSValueConst value, JSValueConst receiver, int flags)
{
    /* THE BODY IS DELETED. Every consumer is routed onto the keyed entry's GP_SET and the whole corpus runs with
       this DFAIL armed without reaching it, so what stood here was a second implementation of 10.5.9 — the `set` trap and its result invariant,
       run from an activation with no flow base. In RELEASE the DFAIL compiles out and this FAILS VISIBLY rather
       than answering from the proxy's own empty shape — the honest outcome for a capability that only the routed
       path implements. */
    DFAIL("a C-side [[Set]] reached a Proxy — the page's trap has no flow base here; route that caller onto the keyed entry's GP_SET");
    (void)atom; (void)obj; (void)value; (void)receiver; (void)flags;
    JS_ThrowInternalError(ctx, "[[Set]] on a Proxy is only implemented on the interpreter's chain");
    return -1;
}

static JSValue js_desc_to_object(JSContext *ctx, JSPropertyDescriptor *desc)
{
    int flags = JS_PROP_C_W_E | JS_PROP_THROW;
    JSValue ret;
    /* the ONE representation of an accessor in a descriptor RECORD. A record built from a descriptor OBJECT
       carries JS_PROP_HAS_GET/_SET as well, and reporting only those would build a data descriptor here. */
    DCHECK(!(desc->flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) || (desc->flags & JS_PROP_GETSET),
           "a [[GetOwnProperty]] record reports an accessor without JS_PROP_GETSET");
    ret = JS_NewObject(ctx);
    if (JS_IsException(ret)) { js_free_desc(ctx, desc); return ret; }
    if (desc->flags & JS_PROP_GETSET) {
        if (JS_DefinePropertyValueConst(ctx, ret, JS_ATOM_get, desc->getter, flags) < 0
        ||  JS_DefinePropertyValueConst(ctx, ret, JS_ATOM_set, desc->setter, flags) < 0)
            goto fail;
    } else {
        if (JS_DefinePropertyValueConst(ctx, ret, JS_ATOM_value, desc->value, flags) < 0
        ||  JS_DefinePropertyValue(ctx, ret, JS_ATOM_writable,
                                   js_bool(desc->flags & JS_PROP_WRITABLE), flags) < 0)
            goto fail;
    }
    if (JS_DefinePropertyValue(ctx, ret, JS_ATOM_enumerable,
                               js_bool(desc->flags & JS_PROP_ENUMERABLE), flags) < 0
    ||  JS_DefinePropertyValue(ctx, ret, JS_ATOM_configurable,
                               js_bool(desc->flags & JS_PROP_CONFIGURABLE), flags) < 0)
        goto fail;
    js_free_desc(ctx, desc);
    return ret;
 fail:
    js_free_desc(ctx, desc);
    JS_FreeValue(ctx, ret);
    return JS_EXCEPTION;
}

static JSValue js_create_desc(JSContext *ctx, JSValueConst val,
                              JSValueConst getter, JSValueConst setter,
                              int flags)
{
    JSValue ret;
    ret = JS_NewObject(ctx);
    if (JS_IsException(ret))
        return ret;
    if (flags & JS_PROP_HAS_GET) {
        JS_DefinePropertyValueConst(ctx, ret, JS_ATOM_get, getter,
                                    JS_PROP_C_W_E);
    }
    if (flags & JS_PROP_HAS_SET) {
        JS_DefinePropertyValueConst(ctx, ret, JS_ATOM_set, setter,
                                    JS_PROP_C_W_E);
    }
    if (flags & JS_PROP_HAS_VALUE) {
        JS_DefinePropertyValueConst(ctx, ret, JS_ATOM_value, val,
                                    JS_PROP_C_W_E);
    }
    if (flags & JS_PROP_HAS_WRITABLE) {
        JS_DefinePropertyValue(ctx, ret, JS_ATOM_writable,
                               js_bool(flags & JS_PROP_WRITABLE),
                               JS_PROP_C_W_E);
    }
    if (flags & JS_PROP_HAS_ENUMERABLE) {
        JS_DefinePropertyValue(ctx, ret, JS_ATOM_enumerable,
                               js_bool(flags & JS_PROP_ENUMERABLE),
                               JS_PROP_C_W_E);
    }
    if (flags & JS_PROP_HAS_CONFIGURABLE) {
        JS_DefinePropertyValue(ctx, ret, JS_ATOM_configurable,
                               js_bool(flags & JS_PROP_CONFIGURABLE),
                               JS_PROP_C_W_E);
    }
    return ret;
}

/* 10.5.5 steps 9-17, the half of [[GetOwnProperty]] that runs on the trap's RESULT: it must be an Object or
   undefined, and it must be consistent with the target's own descriptor (a non-configurable target property
   cannot be reported absent or redefined incompatibly, a non-extensible target cannot gain one, and a
   non-configurable report has to be true of the target). The trap CALL is the page's code and the check is not,
   the same split every proxy operation makes — so the check lives here, called both by the C hook below and by
   the tramp continuation, which has already run the trap on the interpreter's chain.
   CONSUMES trap_result_obj. Returns 1 with *pdesc filled, 0 for absent, -1 having thrown. */
/* 10.5.5 steps 9-12: the trap result's shape, the TARGET's own descriptor and its extensibility — every one of
   them ordered BEFORE step 13's ToPropertyDescriptor. Split out because that ToPropertyDescriptor is twelve
   keyed operations on an object the page returned, so on the interpreter's chain it suspends, and everything the
   invariant needs afterwards has to have been computed and parked first.
   Returns 1 = go on to the descriptor walk (trap_result_obj NOT consumed), 0 = the property is absent (consumed),
   -1 = threw (consumed). */
/* 10.5.5 step 9 alone: the trap result must be an Object or undefined. No user code, and it is ordered BEFORE
   the target is touched, so both callers run it first. 0 = fine, -1 = threw. */
static int js_proxy_gopd_shape(JSContext *ctx, JSValueConst trap_result_obj)
{
    if (JS_IsObject(trap_result_obj) || JS_IsUndefined(trap_result_obj))
        return 0;
    JS_ThrowTypeError(ctx, "proxy: inconsistent getOwnPropertyDescriptor");
    return -1;
}

/* Does step 11 need 11.c's IsExtensible at all? 11.a and 11.b both finish without it, and asking anyway would
   run a Proxy target's `isExtensible` trap for a step the spec never reaches. The routed path asks this before
   issuing the request and the C hook before making the call, so the condition is stated ONCE. */
static bool js_proxy_gopd_needs_ext(const JSDescFacts *tf)
{
    return tf->flags >= 0 && (tf->flags & JS_PROP_CONFIGURABLE);
}

/* 10.5.5 step 11, the arm taken when the trap returned undefined, given the target facts steps 10 and 11.c
   produced. `extensible` is consulted only when js_proxy_gopd_needs_ext said it would be.
   0 = the property is absent, -1 = threw. */
static int js_proxy_gopd_absent(JSContext *ctx, const JSDescFacts *tf, int extensible)
{
    if (tf->flags < 0)
        return 0;                                          /* step 11.a */
    if (!(tf->flags & JS_PROP_CONFIGURABLE))               /* step 11.b */
        goto fail;
    if (!extensible)                                       /* step 11.d, on 11.c's IsExtensible(target) */
        goto fail;
    return 0;
fail:
    JS_ThrowTypeError(ctx, "proxy: inconsistent getOwnPropertyDescriptor");
    return -1;
}

/* The C-driven form of steps 9-12: the target facts read FROM C, which is exactly what makes this the unrouted
   path — a Proxy target's [[GetOwnProperty]] and [[IsExtensible]] are its traps and they run here with no flow
   base. The routed path issues both as requests (GD_TARGET / GD_EXT) and reaches the same three helpers above,
   so the CHECKS cannot drift between them; only how the facts are obtained differs.
   Returns 1 = go on to the descriptor walk (trap_result_obj NOT consumed), 0 = the property is absent
   (consumed), -1 = threw (consumed). */
/* DELETED: js_proxy_gopd_pre. It was 10.5.5 steps 9-12 with the TARGET FACTS read from C — the target's
   [[GetOwnProperty]] and [[IsExtensible]], which for a Proxy target are its own traps. The routed path obtains the
   same facts as GD_TARGET / GD_EXT requests and reaches the same invariant helpers, so deleting this removes a WAY
   OF ASKING and not a check. */

/* 10.5.5 steps 14-17, on the PARSED descriptor. CONSUMES result_desc. Returns 1 with *pdesc filled, -1 having
   thrown. */
static int js_proxy_gopd_post(JSContext *ctx, JSPropertyDescriptor *presult, const JSDescFacts *tf,
                              int extensible_target, JSPropertyDescriptor *pdesc)
{
    JSPropertyDescriptor result_desc = *presult;
    int target_desc_ret = (tf->flags >= 0);
    int target_desc_flags = target_desc_ret ? tf->flags : 0;
    int flags1;
    {
        /* js_obj_to_desc reads a descriptor OBJECT and reports which fields were PRESENT (JS_PROP_HAS_*); a
           [[GetOwnProperty]] result is a descriptor RECORD, whose accessor-ness is JS_PROP_GETSET. Every other
           object kind fills the record form, so a proxy has to as well — otherwise the same accessor property
           reads back as an accessor descriptor directly and as a DATA one through a trap that merely forwards it,
           which is what `Object.getOwnPropertyDescriptor(new Proxy(acc, {getOwnPropertyDescriptor: Reflect
           .getOwnPropertyDescriptor}), "g")` returned: {value, writable} for a getter. */
        if (result_desc.flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET))
            result_desc.flags |= JS_PROP_GETSET;

        if (target_desc_ret) {
            /* convert result_desc.flags to defineProperty flags */
            flags1 = result_desc.flags | JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE;
            if (result_desc.flags & JS_PROP_GETSET)
                flags1 |= JS_PROP_HAS_GET | JS_PROP_HAS_SET;
            else
                flags1 |= JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE;
            if (!check_define_prop_flags(target_desc_flags, flags1))
                goto fail1;
            /* IsCompatiblePropertyDescriptor's other half. check_define_prop_flags is the FLAG comparison, and
               that was all this step did — the trap could report any value it liked for a property the target
               pins. ValidateAndApplyPropertyDescriptor 4.d.i-ii and 4.e.ii are the SameValue tests on the
               contents, and the trap result is a COMPLETED descriptor so every field it needs is present. */
            if (!(target_desc_flags & JS_PROP_CONFIGURABLE)) {
                if ((target_desc_flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
                    DCHECK(result_desc.flags & JS_PROP_GETSET,
                           "the flag check already rejected a data descriptor against an accessor target");
                    if (!js_same_value(ctx, result_desc.getter, tf->getter) ||
                        !js_same_value(ctx, result_desc.setter, tf->setter))
                        goto fail1;
                } else if (!(target_desc_flags & JS_PROP_WRITABLE)) {
                    DCHECK(!(result_desc.flags & JS_PROP_GETSET),
                           "the flag check already rejected an accessor descriptor against a data target");
                    if (!js_same_value(ctx, result_desc.value, tf->value))
                        goto fail1;
                }
            }
        } else {
            if (!extensible_target)
                goto fail1;
        }
        if (!(result_desc.flags & JS_PROP_CONFIGURABLE)) {
            if (!target_desc_ret || (target_desc_flags & JS_PROP_CONFIGURABLE))
                goto fail1;
            if ((result_desc.flags &
                 (JS_PROP_GETSET | JS_PROP_WRITABLE)) == 0 &&
                target_desc_ret &&
                (target_desc_flags & JS_PROP_WRITABLE) != 0) {
                /* proxy-missing-checks */
            fail1:
                js_free_desc(ctx, &result_desc);
                JS_ThrowTypeError(ctx, "proxy: inconsistent getOwnPropertyDescriptor");
                return -1;
            }
        }
        if (pdesc) {
            *pdesc = result_desc;
        } else {
            js_free_desc(ctx, &result_desc);
        }
    }
    return 1;
}

/* The C form: pre, the C-side ToPropertyDescriptor, post. Its ONE remaining caller is js_proxy_get_own_property,
   the C [[GetOwnProperty]] hook — every consumer routed onto GP_GETOWNPROP runs the same three steps on the
   interpreter's chain, with the walk as twelve requests instead. CONSUMES trap_result_obj. */
/* DELETED: js_proxy_gopd_check, the C form of 10.5.5 steps 9-17. Its one caller was the C [[GetOwnProperty]]
   hook, whose body is gone. */

static int js_proxy_get_own_property(JSContext *ctx, JSPropertyDescriptor *pdesc,
                                     JSValueConst obj, JSAtom prop)
{
    /* THE BODY IS DELETED, for the same reason the [[OwnPropertyKeys]] hook's is: every consumer is routed onto
       GP_GETOWNPROP and the whole corpus runs with this DFAIL armed without reaching it. What stood here was a
       second implementation of 10.5.5 — the `getOwnPropertyDescriptor` trap through JS_CallFree, then
       js_proxy_gopd_check: the target's [[GetOwnProperty]] and [[IsExtensible]], and js_obj_to_desc reading the
       trap RESULT's twelve descriptor fields with JS_HasProperty / JS_GetProperty. Every one is the page's code
       from an activation with no flow base. The routed path keeps the INVARIANT helpers, so the checks cannot
       drift; only the C way of obtaining the facts is gone.
       In RELEASE the DFAIL compiles out and this FAILS visibly rather than answering from the proxy's own empty
       shape, which is the honest outcome for a capability only the routed path implements. */
    DFAIL("a C-side [[GetOwnProperty]] reached a Proxy — the page's trap has no flow base here; route that caller onto the keyed entry's GP_GETOWNPROP");
    (void)pdesc; (void)prop; (void)obj;
    JS_ThrowInternalError(ctx, "[[GetOwnProperty]] on a Proxy is only implemented on the interpreter's chain");
    return -1;
}

static int js_proxy_define_own_property(JSContext *ctx, JSValueConst obj,
                                        JSAtom prop, JSValueConst val,
                                        JSValueConst getter,
                                        JSValueConst setter, int flags)
{
    /* THE BODY IS DELETED. Every consumer is routed onto the keyed entry's GP_DEFINE and the whole corpus runs with
       this DFAIL armed without reaching it, so what stood here was a second implementation of 10.5.6 — the `defineProperty` trap, the descriptor object it is handed, and its result invariant,
       run from an activation with no flow base. In RELEASE the DFAIL compiles out and this FAILS VISIBLY rather
       than answering from the proxy's own empty shape — the honest outcome for a capability that only the routed
       path implements. */
    DFAIL("a C-side [[DefineOwnProperty]] reached a Proxy — the page's trap has no flow base here; route that caller onto the keyed entry's GP_DEFINE");
    (void)prop; (void)obj; (void)val; (void)getter; (void)setter; (void)flags;
    JS_ThrowInternalError(ctx, "[[DefineOwnProperty]] on a Proxy is only implemented on the interpreter's chain");
    return -1;
}

/* [[DefineOwnProperty]] invariant (10.5.6 steps 13-16): a trap that reports SUCCESS must be consistent with the
   target's own property and with its extensibility. ONE implementation, shared by the C path above and by the
   trampolined trap's continuation — it runs on the trap's RESULT, so the tramp path can only honour it from a
   continuation, exactly as [[Get]]'s, [[HasProperty]]'s and [[Delete]]'s do. `ret` is the trap's boolean;
   returns 1, 0 (rejected with no THROW flag), or -1 having thrown. */
/* DELETED: js_proxy_define_invariant, 10.5.6 steps 15-17 with the target's descriptor and extensibility read
   from C. */

/* [[Delete]] invariant: a trap that reports SUCCESS must be telling the truth about a non-configurable own property
   of the target, and about a non-extensible target. ONE implementation, shared by the C path and by the trampolined
   trap's continuation — the check runs on the trap's RESULT, so the tramp path can only honour it from a
   continuation, exactly as [[Get]]'s and [[HasProperty]]'s do. `ret` is the trap's boolean; returns it, or -1
   having thrown. */
/* DELETED: js_proxy_delete_invariant, 10.5.10 steps 10-12, the same story as its `has` sibling. */

static int js_proxy_delete_property(JSContext *ctx, JSValueConst obj,
                                    JSAtom atom)
{
    /* THE BODY IS DELETED. Every consumer is routed onto the keyed entry's GP_DELETE and the whole corpus runs with
       this DFAIL armed without reaching it, so what stood here was a second implementation of 10.5.10 — the `deleteProperty` trap and its non-configurable invariant,
       run from an activation with no flow base. In RELEASE the DFAIL compiles out and this FAILS VISIBLY rather
       than answering from the proxy's own empty shape — the honest outcome for a capability that only the routed
       path implements. */
    DFAIL("a C-side [[Delete]] reached a Proxy — the page's trap has no flow base here; route that caller onto the keyed entry's GP_DELETE");
    (void)atom; (void)obj;
    JS_ThrowInternalError(ctx, "[[Delete]] on a Proxy is only implemented on the interpreter's chain");
    return -1;
}

/* return the index of the property or -1 if not found */
static int find_prop_key(const JSPropertyEnum *tab, int n, JSAtom atom)
{
    int i;
    for(i = 0; i < n; i++) {
        if (tab[i].atom == atom)
            return i;
    }
    return -1;
}

/* 10.5.11 steps 8-22, the half of [[OwnPropertyKeys]] that runs on the trap's RESULT: it must be a List of
   Strings and Symbols with no duplicates, it must contain every non-configurable key the target has, and when the
   target is not extensible it must match the target's keys exactly. The trap CALL is the page's code and the
   check is not, which is exactly the split every proxy operation makes — so the check lives here, called both by
   the C hook below (whose trap call is still a JS_CallFree) and by the tramp continuation, which has already run
   the trap on the interpreter's chain. CONSUMES prop_array either way. */
/* DELETED: js_proxy_ownkeys_check. It was 10.5.11 steps 5-11 performed from C — CreateListFromArrayLike on the
   TRAP RESULT (a page array, so its `length` and every element are the page's code), then [[OwnPropertyKeys]] and
   [[IsExtensible]] on the target and a [[GetOwnProperty]] per key. The machine performs all of it as requests, and
   its only remaining caller was the C hook whose body is now gone with it. */

/* DELETED: js_proxy_ownkeys_result. It was 10.5.11's whole check performed from C at the routed delivery — the
   trap-result array walk and the three internal methods on the target — and the machine above performs it as
   requests. Its last caller went with it; keeping it beside the machine is the dual system these conversions
   exist to close. js_proxy_ownkeys_check survives ONLY for the C hook, which is the counted, unrouted path. */

static int js_proxy_get_own_property_names(JSContext *ctx,
                                           JSPropertyEnum **ptab,
                                           uint32_t *plen,
                                           JSValueConst obj)
{
    /* THE BODY IS DELETED. Every consumer of [[OwnPropertyKeys]] is routed onto GP_OWNKEYS, and the whole corpus
       runs with this DFAIL armed without reaching it — so what stood here was a second implementation of 10.5.11
       that nothing could reach: the `ownKeys` trap through JS_CallFree, and then js_proxy_ownkeys_check's SIX
       page-visible operations (the trap-result array's `length` and every element, plus [[GetOwnProperty]] per key,
       [[OwnPropertyKeys]] and [[IsExtensible]] on the target), all from an activation with no flow base.
       In RELEASE the DFAIL is compiled out and there is nothing to fall back to, which is correct: the capability
       is not supportable outside the routed path, so it FAILS visibly rather than answering from the proxy's own
       empty shape. */
    DFAIL("a C-side [[OwnPropertyKeys]] reached a Proxy — the page's trap has no flow base here; route that caller onto the keyed entry's GP_OWNKEYS");
    (void)ptab; (void)plen; (void)obj;
    JS_ThrowInternalError(ctx, "[[OwnPropertyKeys]] on a Proxy is only implemented on the interpreter's chain");
    return -1;
}

static JSValue js_proxy_call_constructor(JSContext *ctx, JSValueConst func_obj,
                                         JSValueConst new_target,
                                         int argc, JSValueConst *argv)
{
    /* THE BODY IS DELETED. Every consumer is routed, and the whole corpus runs with this DFAIL armed without
       reaching it — so what stood here was a second implementation of 10.5.13 — the `construct` trap, the argument array it is handed, and its must-return-an-object invariant,
       run from an activation with no flow base. In RELEASE the DFAIL compiles out and there is deliberately
       nothing to fall back to: FAILING VISIBLY is right where answering wrongly is not. */
    DFAIL("a C-side [[Construct]] reached a Proxy — the page's trap has no flow base here; route that caller onto the construct dispatch");
    (void)func_obj; (void)new_target; (void)argc; (void)argv;
    JS_ThrowInternalError(ctx, "[[Construct]] on a Proxy is only implemented on the interpreter's chain");
    return JS_EXCEPTION;
}

static JSValue js_proxy_call(JSContext *ctx, JSValueConst func_obj,
                             JSValueConst this_obj,
                             int argc, JSValueConst *argv, int flags)
{
    /* THE BODY IS DELETED. Every consumer is routed, and the whole corpus runs with this DFAIL armed without
       reaching it — so what stood here was a second implementation of 10.5.12 — the `apply` trap and the argument array it is handed,
       run from an activation with no flow base. In RELEASE the DFAIL compiles out and there is deliberately
       nothing to fall back to: FAILING VISIBLY is right where answering wrongly is not. */
    DFAIL("a C-side [[Call]] reached a Proxy — the page's trap has no flow base here; route that caller onto the call convergence point");
    (void)func_obj; (void)this_obj; (void)argc; (void)argv; (void)flags;
    JS_ThrowInternalError(ctx, "[[Call]] on a Proxy is only implemented on the interpreter's chain");
    return JS_EXCEPTION;
}

/* DELETED: js_proxy_isArray. It was the other half of IsArray's two-function cycle and nothing else called it. */

static const JSClassExoticMethods js_proxy_exotic_methods = {
    .get_own_property = js_proxy_get_own_property,
    .define_own_property = js_proxy_define_own_property,
    .delete_property = js_proxy_delete_property,
    .get_own_property_names = js_proxy_get_own_property_names,
    .has_property = js_proxy_has,
    .get_property = js_proxy_get,
    .set_property = js_proxy_set,
};

JSValue JS_NewProxy(JSContext *ctx, JSValueConst target, JSValueConst handler)
{
    JSProxyData *s;
    JSValue obj;

    if (JS_VALUE_GET_TAG(target) != JS_TAG_OBJECT ||
        JS_VALUE_GET_TAG(handler) != JS_TAG_OBJECT) {
        return JS_ThrowTypeErrorNotAnObject(ctx);
    }
    obj = JS_NewObjectProtoClass(ctx, JS_NULL, JS_CLASS_PROXY);
    if (JS_IsException(obj))
        return obj;
    s = js_malloc(ctx, sizeof(*s));
    if (!s) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    s->target = js_dup(target);
    s->handler = js_dup(handler);
    s->is_func = JS_IsFunction(ctx, target);
    s->is_revoked = false;
    JS_SetOpaqueInternal(obj, s);
    JS_SetConstructorBit(ctx, obj, JS_IsConstructor(ctx, target));
    return obj;
}

static JSValue js_proxy_constructor(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    return JS_NewProxy(ctx, argv[0], argv[1]);
}

static JSValue js_proxy_revoke(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic,
                               JSValueConst *func_data)
{
    JSProxyData *s = JS_GetOpaque(func_data[0], JS_CLASS_PROXY);
    if (s) {
        /* We do not free the handler and target in case they are
           referenced as constants in the C call stack */
        s->is_revoked = true;
        JS_FreeValue(ctx, unsafe_unconst(func_data[0]));
        func_data[0] = JS_NULL;
    }
    return JS_UNDEFINED;
}

static JSValue js_proxy_revoke_constructor(JSContext *ctx,
                                           JSValueConst proxy_obj)
{
    return JS_NewCFunctionData(ctx, js_proxy_revoke, 0, 0, 1, &proxy_obj);
}

static JSValue js_proxy_revocable(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue proxy_obj, revoke_obj = JS_UNDEFINED, obj;

    proxy_obj = js_proxy_constructor(ctx, JS_UNDEFINED, argc, argv);
    if (JS_IsException(proxy_obj))
        goto fail;
    revoke_obj = js_proxy_revoke_constructor(ctx, proxy_obj);
    if (JS_IsException(revoke_obj))
        goto fail;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        goto fail;
    // XXX: exceptions?
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_proxy, proxy_obj, JS_PROP_C_W_E);
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_revoke, revoke_obj, JS_PROP_C_W_E);
    return obj;
 fail:
    JS_FreeValue(ctx, proxy_obj);
    JS_FreeValue(ctx, revoke_obj);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry js_proxy_funcs[] = {
    JS_CFUNC_DEF("revocable", 2, js_proxy_revocable ),
};

static const JSClassShortDef js_proxy_class_def[] = {
    { JS_ATOM_Object, js_proxy_finalizer, js_proxy_mark }, /* JS_CLASS_PROXY */
};

int JS_AddIntrinsicProxy(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    JSValue obj1;

    if (!JS_IsRegisteredClass(rt, JS_CLASS_PROXY)) {
        if (init_class_range(rt, js_proxy_class_def, JS_CLASS_PROXY,
                             countof(js_proxy_class_def)))
            return -1;
        rt->class_array[JS_CLASS_PROXY].exotic = &js_proxy_exotic_methods;
        rt->class_array[JS_CLASS_PROXY].call = js_proxy_call;
    }

    /* additional fields: name, length */
    obj1 = JS_NewCFunction3(ctx, js_proxy_constructor, "Proxy", 2,
                            JS_CFUNC_constructor, 0,
                            ctx->function_proto, countof(js_proxy_funcs) + 2);
    if (JS_IsException(obj1))
        return -1;
    JS_SetConstructorBit(ctx, obj1, true);
    if (JS_SetPropertyFunctionList(ctx, obj1, js_proxy_funcs,
                                   countof(js_proxy_funcs)))
        goto fail;
    if (JS_DefinePropertyValueStr(ctx, ctx->global_obj, "Proxy",
                                  obj1, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        goto fail;
    return 0;
 fail:
    JS_FreeValue(ctx, obj1);
    return -1;
}

bool JS_IsProxy(JSValueConst val)
{
    if (JS_VALUE_GET_TAG(val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(val);
        return p->class_id == JS_CLASS_PROXY;
    }
    return false;
}

static JSValue js_get_proxy_field(JSContext *ctx, JSValueConst proxy,
                                  int offset)
{
    if (JS_VALUE_GET_TAG(proxy) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(proxy);
        if (p->class_id == JS_CLASS_PROXY) {
            JSProxyData *s = JS_GetOpaque(proxy, JS_CLASS_PROXY);
            if (s->is_revoked)
                return JS_ThrowTypeErrorRevokedProxy(ctx);
            return js_dup(*(JSValue *)((char *)s + offset));
        }
    }
    return JS_ThrowTypeError(ctx, "not a proxy");
}

JSValue JS_GetProxyTarget(JSContext *ctx, JSValueConst proxy)
{
    return js_get_proxy_field(ctx, proxy, offsetof(JSProxyData, target));
}

JSValue JS_GetProxyHandler(JSContext *ctx, JSValueConst proxy)
{
    return js_get_proxy_field(ctx, proxy, offsetof(JSProxyData, handler));
}

