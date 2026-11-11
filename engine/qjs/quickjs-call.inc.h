JSValue JS_Call(JSContext *ctx, JSValueConst func_obj, JSValueConst this_obj,
                int argc, JSValueConst *argv)
{
    return JS_CallInternal(ctx, func_obj, this_obj, JS_UNDEFINED,
                           argc, argv, JS_CALL_FLAG_COPY_ARGV);
}

/* warning: the refcount of the context is not incremented. Return
   NULL in case of exception (case of revoked proxy only) */
/* ECMAScript §7.3.24 "GetFunctionRealm ( func )". THE DEPTH IS THE PAGE'S: steps 2 and 3 hand the question to a
   bound function's target and a Proxy's target, and `new Proxy(new Proxy(…))` or `f.bind().bind()…` nests either
   as far as the page likes. Both of those steps are TAIL positions in the spec's own text, so the recursion was
   never structure — it was a C frame per link, and the one this fork may not spend. A loop over the current
   object is the same algorithm with the same answer at every step.
   It was invisible while it sat inside the interpreter's cycle, and then inside the error/property one; it
   surfaced as its own recursion the moment js_create_from_ctor stopped dragging JS_GetProperty along, which is
   the whole point of measuring cycles rather than counting call sites. */
JSContext *JS_GetFunctionRealm(JSContext *ctx, JSValueConst func_obj)
{
    JSValueConst cur = func_obj;

    for (;;) {
        JSObject *p;
        if (JS_VALUE_GET_TAG(cur) != JS_TAG_OBJECT)
            return ctx;
        p = JS_VALUE_GET_OBJ(cur);
        switch(p->class_id) {
        case JS_CLASS_C_FUNCTION:
            return p->u.cfunc.realm;
        case JS_CLASS_BYTECODE_FUNCTION:
        case JS_CLASS_GENERATOR_FUNCTION:
        case JS_CLASS_ASYNC_FUNCTION:
        case JS_CLASS_ASYNC_GENERATOR_FUNCTION:
            return p->u.func.function_bytecode->realm;
        case JS_CLASS_PROXY:
            {
                JSProxyData *s = p->u.opaque;
                if (!s)
                    return ctx;
                if (s->is_revoked) {
                    JS_ThrowTypeErrorRevokedProxy(ctx);
                    return NULL;
                }
                cur = s->target;          /* step 3.d, in tail position */
            }
            break;
        case JS_CLASS_BOUND_FUNCTION:
            cur = p->u.bound_function->func_obj;   /* step 2.b, in tail position */
            break;
        case JS_CLASS_WRAPPED_FUNCTION:
            /* step 1: an object that HAS a [[Realm]] slot answers with it. A wrapped function has one — that
               is the realm its arguments are wrapped INTO when it is called. */
            return p->u.wrapped_function_data->realm;
        default:
            return ctx;
        }
    }
}

/* THE REALM'S OWN PROTOTYPE, for an object no new.target names. OrdinaryCreateFromConstructor's whole body is
   GetPrototypeFromConstructor, and that is only an operation when there IS a constructor to read `prototype`
   off; with none, 10.1.13 step 2 says "use the intrinsic", which is a field read. Seven call sites passed a
   literal JS_UNDEFINED and went through the general entry to reach it, and that is not free: the general entry
   calls JS_GetProperty, so a Date, a DOMException and — inside error construction — a CallSite all appeared to
   run page code, which is what kept build_backtrace inside the error/property cycle.
   Same argument as js_error_add_own: asking a general operation a question whose answer is fixed does not just
   cost a branch, it inherits every path the general operation can take. */
static JSValue js_create_from_intrinsic(JSContext *ctx, int class_id)
{
    return JS_NewObjectProtoClass(ctx, ctx->class_proto[class_id], class_id);
}

static JSValue js_create_from_ctor(JSContext *ctx, JSValueConst ctor,
                                   int class_id)
{
    JSValue proto, obj;
    JSContext *realm;

    /* A ctor that is undefined at RUNTIME is a real case — several callers forward a new.target that a plain
       call left undefined — so this branch stays and delegates. What is gone is the seven call sites that
       passed a LITERAL JS_UNDEFINED: those reached JS_GetProperty in the reachability graph without ever
       being able to reach it in a run, which is what put a CallSite's creation, and through it error
       construction, inside the property cycle. */
    if (JS_IsUndefined(ctor))
        return js_create_from_intrinsic(ctx, class_id);
    {
        proto = JS_GetProperty(ctx, ctor, JS_ATOM_prototype);
        if (JS_IsException(proto))
            return proto;
        if (!JS_IsObject(proto)) {
            JS_FreeValue(ctx, proto);
            realm = JS_GetFunctionRealm(ctx, ctor);
            if (!realm)
                return JS_EXCEPTION;
            proto = js_dup(realm->class_proto[class_id]);
        }
    }
    obj = JS_NewObjectProtoClass(ctx, proto, class_id);
    JS_FreeValue(ctx, proto);
    return obj;
}

/* OrdinaryCreateFromConstructor (10.1.13) for a CONSTRUCTOR STEP MACHINE. Its `prototype` read is an ordinary
   [[Get]], so a new.target carrying an accessor or a Proxy `get` trap runs page code there — and the C body
   above has no flow base to run it on, which is why `Reflect.construct(Date, [], proxyNewTarget)` aborted.
   Every constructor machine ends in this same read, so it is a shared sub-sequence over step_getprop_run rather
   than a stage each of them re-spells. The realm fallback below inspects the constructor without invoking it.
     0 = *pout holds the new object, 6 = the caller must return that step code, -1 = threw. */
/* 7.3.22 SpeciesConstructor ( O, defaultConstructor ) — `? Get(O, "constructor")` then `? Get(C, @@species)`,
   both the page's code, and JS_SpeciesConstructor ran them from C. It yields the CONSTRUCTOR and stops there:
   the Construct that follows differs per call site (@@matchAll passes «R, flags», @@split «rx, newFlags»), so
   folding it in would be one operation pretending to be two.
   It shares spc_phase with ArraySpeciesCreate because a machine performs one or the other, never both at once.
   0 = *pout is filled, 6 = the caller must return that step code, -1 = threw. */
static int step_speciesctor_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSValueConst def,
                                JSValue in, JSValue *pout, JSValue **out_cb, int *out_argc)
{
    switch (h->spc_phase) {
    case SPC_START:
        JS_FreeValue(ctx, in);
        h->cb_coerce[0] = (JSValue)obj;   /* borrowed: the machine holds the receiver */
        *out_cb = h->cb_coerce; *out_argc = (int)JS_ATOM_constructor;
        h->spc_phase = SPC_CTOR;
        return 6;
    case SPC_CTOR:
        if (JS_IsUndefined(in)) { h->spc_phase = SPC_START; *pout = js_dup(def); return 0; }
        if (!JS_IsObject(in)) {
            JS_FreeValue(ctx, in);
            h->spc_phase = SPC_START;
            JS_ThrowTypeErrorNotAnObject(ctx);
            return -1;
        }
        h->coerce = in;                   /* owned across the @@species read */
        h->cb_coerce[0] = h->coerce;      /* borrowed view */
        *out_cb = h->cb_coerce; *out_argc = (int)JS_ATOM_Symbol_species;
        h->spc_phase = SPC_SPECIES;
        return 6;
    default:
        DCHECK(h->spc_phase == SPC_SPECIES, "SpeciesConstructor resumed in an unknown phase");
        JS_FreeValue(ctx, h->coerce);
        h->coerce = JS_UNDEFINED;
        h->spc_phase = SPC_START;
        if (JS_IsUndefined(in) || JS_IsNull(in)) { JS_FreeValue(ctx, in); *pout = js_dup(def); return 0; }
        if (!JS_IsConstructor(ctx, in)) {
            JS_FreeValue(ctx, in);
            JS_ThrowTypeError(ctx, "not a constructor");
            return -1;
        }
        *pout = in;
        return 0;
    }
}

/* GetPrototypeFromConstructor (10.1.14) on its own. 10.1.13 is this operation followed by OrdinaryObjectCreate,
   and CreateDynamicFunction's function is created by the PARSER rather than by that step while still ending in
   this same read (20.2.1.1.1 step 29) — which is what makes the two separate sub-sequences instead of one.
     0 = *pout holds the prototype, 6 = the caller must return that step code, -1 = threw. */
static int step_proto_from_ctor_run(JSContext *ctx, JSStepHdr *h, JSValueConst ctor, int class_id,
                                    JSValue in, JSValue *pout, JSValue **out_cb, int *out_argc)
{
    JSContext *realm;
    int r;

    if (JS_IsUndefined(ctor)) {
        JS_FreeValue(ctx, in);
        *pout = js_dup(ctx->class_proto[class_id]);
        return 0;
    }
    r = step_getprop_run(ctx, h, ctor, JS_ATOM_prototype, in, pout, out_cb, out_argc);
    if (r) return r;
    if (!JS_IsObject(*pout)) {
        JS_FreeValue(ctx, *pout);
        realm = JS_GetFunctionRealm(ctx, ctor);
        if (!realm) { *pout = JS_UNDEFINED; return -1; }
        *pout = js_dup(realm->class_proto[class_id]);
    }
    return 0;
}

static int step_create_from_ctor_run(JSContext *ctx, JSStepHdr *h, JSValueConst ctor, int class_id,
                                     JSValue in, JSValue *pout, JSValue **out_cb, int *out_argc)
{
    JSValue proto;
    int r = step_proto_from_ctor_run(ctx, h, ctor, class_id, in, &proto, out_cb, out_argc);
    if (r) return r;
    *pout = JS_NewObjectProtoClass(ctx, proto, class_id);
    JS_FreeValue(ctx, proto);
    return JS_IsException(*pout) ? (*pout = JS_UNDEFINED, -1) : 0;
}

/* argv[] is modified if (flags & JS_CALL_FLAG_COPY_ARGV) = 0. */
static JSValue JS_CallConstructorInternal(JSContext *ctx,
                                          JSValueConst func_obj,
                                          JSValueConst new_target,
                                          int argc, JSValueConst *argv,
                                          int flags)
{
    JSObject *p;
    JSFunctionBytecode *b;

    if (js_poll_interrupts(ctx))
        return JS_EXCEPTION;
    flags |= JS_CALL_FLAG_CONSTRUCTOR;
    if (unlikely(JS_VALUE_GET_TAG(func_obj) != JS_TAG_OBJECT))
        goto not_a_function;
    p = JS_VALUE_GET_OBJ(func_obj);
    if (unlikely(!p->is_constructor))
        return JS_ThrowTypeErrorNotAConstructor(ctx, func_obj);
    if (unlikely(p->class_id != JS_CLASS_BYTECODE_FUNCTION)) {
        JSClassCall *call_func;
        call_func = ctx->rt->class_array[p->class_id].call;
        if (!call_func) {
        not_a_function:
            return JS_ThrowTypeErrorNotAFunction(ctx);
        }
        return call_func(ctx, func_obj, new_target, argc,
                         argv, flags);
    }

    /* A BYTECODE CONSTRUCTOR DOES NOT RUN HERE, and there is no longer a body that could.
     *
     * This is where `new C()` used to re-enter the interpreter from C — JS_CallInternal on the constructor's
     * body — and it is the last edge of the interpreter's own recursion cycle. A constructor entered that way
     * is on the C stack: it cannot suspend, so a loop in it drives to completion, and the whole point of the
     * trampoline is that no page code is ever in that position.
     *
     * Every route here is converted. The interpreter's construct point sends a bytecode ctor to
     * do_construct_tramp, where its body runs on the caller's heap-frame chain; a bound target is flattened at
     * the operator site and re-dispatched, so it answers exactly as a bare one; a step machine's Construct is a
     * request. The last C caller was js_typed_array_create, deleted above as the twin of a conversion that had
     * already happened.
     *
     * So the branch is DELETED rather than kept behind a check. Keeping it would be the legacy fallback this
     * engine's whole discipline is against: a second implementation of Construct, reachable by anyone who
     * reaches for the public entry, silently un-suspendable. What is left is the DFAIL, which names what to
     * build for the caller that finds it — route the Construct through the trampoline, as every caller in the
     * engine already does. */
    DFAIL("a BYTECODE constructor was constructed from C — its body would run on the C stack with no flow base, "
          "so a loop in it drives to completion; route this Construct through the trampoline "
          "(do_construct_tramp, or a step machine's CONSTRUCT request) as every caller in the engine does");
    return JS_ThrowTypeError(ctx, "constructing a scripted constructor from C is not supported");
}

JSValue JS_CallConstructor2(JSContext *ctx, JSValueConst func_obj,
                            JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    return JS_CallConstructorInternal(ctx, func_obj, new_target,
                                      argc, argv,
                                      JS_CALL_FLAG_COPY_ARGV);
}

JSValue JS_CallConstructor(JSContext *ctx, JSValueConst func_obj,
                           int argc, JSValueConst *argv)
{
    return JS_CallConstructorInternal(ctx, func_obj, func_obj,
                                      argc, argv,
                                      JS_CALL_FLAG_COPY_ARGV);
}

/* READ-THEN-CALL FROM C, which is two pieces of page code in one line: the [[Get]] can be an accessor or a
   Proxy trap, and the method it produces is a body with loops in it. Every internal user of this became a step
   machine — the promise resolve/then reads, @@toPrimitive, @@species, the iterator protocol — and the last one
   left, JS_ToLocaleStringFree, had no callers at all. The JS_CallFree it ended in is gone with the rest, which
   is what takes JS_CallInternal's own cycle apart.
   It stays EXPORTED because it is quickjs.h's API and an embedder's call is not this fork's to silently
   redefine; what it can no longer do is run that call on the C stack, so it names the routing to build. */
JSValue JS_Invoke(JSContext *ctx, JSValueConst this_val, JSAtom atom,
                  int argc, JSValueConst *argv)
{
    DFAIL("JS_Invoke ran a method from C — route this call onto the tramp chain");
    return JS_ThrowTypeError(ctx, "invoke");
}

