static JSValue js_call_c_function(JSContext *ctx, JSValueConst func_obj,
                                  JSValueConst this_obj,
                                  int argc, JSValueConst *argv, int flags)
{
    JSRuntime *rt = ctx->rt;
    JSCFunctionType func;
    JSObject *p;
    JSStackFrame sf_s, *sf = &sf_s, *prev_sf;
    JSValue ret_val;
    JSValueConst *arg_buf;
    int arg_count, i;
    JSCFunctionEnum cproto;

    p = JS_VALUE_GET_OBJ(func_obj);
    cproto = p->u.cfunc.cproto;
    arg_count = p->u.cfunc.length;

    /* better to always check stack overflow */
    if (js_check_stack_overflow(rt, sizeof(arg_buf[0]) * arg_count))
        return JS_ThrowStackOverflow(ctx);

    prev_sf = rt->current_stack_frame;
    sf->prev_frame = prev_sf;
    rt->current_stack_frame = sf;
    ctx = p->u.cfunc.realm; /* change the current realm */

    sf->is_strict_mode = false;
    sf->is_constructor = (flags & JS_CALL_FLAG_CONSTRUCTOR) != 0;
    sf->is_call_root = false;
    sf->step_func = sf->step_this = JS_UNDEFINED;
    sf->eval_hold = NULL;
    /* A C ACTIVATION HAS NO BYTECODE, SO IT HAS NO LOCALS, NO VAR-REF ARRAY AND NO OWNER — written as the
       absence it is, because this frame is a C-stack local and an unwritten field here is not zero. See
       get_captured_cell, whose bound DCHECK is what makes a frame that reaches it anyway say so. */
    sf->var_refs = NULL;
    sf->var_ref_count = 0;
    sf->cur_gc_obj = NULL;
    sf->cur_func = unsafe_unconst(func_obj);
    sf->this_val = this_obj;
    sf->arg_count = argc;
    arg_buf = argv;

    if (unlikely(argc < arg_count)) {
        /* ensure that at least argc_count arguments are readable */
        arg_buf = alloca(sizeof(arg_buf[0]) * arg_count);
        for(i = 0; i < argc; i++)
            arg_buf[i] = argv[i];
        for(i = argc; i < arg_count; i++)
            arg_buf[i] = JS_UNDEFINED;
        sf->arg_count = arg_count;
    }
    sf->arg_buf = (JSValue *)arg_buf;

    func = p->u.cfunc.c_function;
    /* THE POINTER THIS IS ABOUT TO CALL EXISTS. Four cprotos deliberately carry NO body — step, step_ctor,
       consume and iterdrive install `.generic = NULL`, because the machine IS the implementation and their arms
       below DFAIL rather than call. Every other cproto names a C body, and the arms reach it through a union
       member without ever asking whether it is there.
       This is asserted rather than assumed because the set of bodyless builtins GROWS: each conversion moves
       another row from JS_CFUNC_MAGIC_DEF to a declaration, and the seven promise combinators joined it most
       recently. A row converted while some path still routes it to a body arm calls through NULL, and the
       failure that produces is a jump to address 0 with no symbol and no return address — a backtrace that
       names nothing, in a process that was working a moment earlier. It is the C-FUNCTION twin of the XOR
       js_new_c_function_data now asserts for a closure, and it is asked here for the same reason: at the point
       the pointer is loaded, so it names the registration that half-happened rather than the instruction that
       ran. */
    DCHECK(func.generic != NULL || cproto == JS_CFUNC_step || cproto == JS_CFUNC_step_ctor
           || cproto == JS_CFUNC_consume || cproto == JS_CFUNC_iterdrive,
           "a C function whose cproto names a body carries none — its registration declared a machine and left "
           "the row pointing at a body arm, or a new bodyless cproto was added without joining this list");
    /* AND IF THIS BODY DECLARED THAT IT RUNS NONE OF THE PAGE'S CODE, THAT IS HELD OVER IT WHILE IT RUNS — see
       js_no_user_code_enter. It is raised HERE, at the one point every call shape converges on, rather than at
       the accessor arm that relies on the declaration: the claim is about the BODY, so a routed read, a direct
       C read, `Reflect.get` and a `.get.call(x)` off a descriptor must all be held to it, and four raise sites
       would be four chances for one to be missed.
       THE PAIR CANNOT BE SPLIT, and that is ASSERTED rather than read off the switch below: two of its arms
       (iterdrive, consume) RETURN instead of breaking, so an arm reached with this raised would leave it raised
       for the life of the thread and every later bytecode entry would abort naming an innocent member. They are
       unreachable here because JS_DeclareCFunctionNoUserCode refuses any cproto but an attribute getter's — and
       this is where that refusal is RELIED ON, so this is where it is restated. */
    if (unlikely(p->u.cfunc.no_user_code)) {
        DCHECK(cproto == JS_CFUNC_getter || cproto == JS_CFUNC_getter_magic,
               "a no-user-code declaration is riding a cproto that is not an attribute getter's — the arms "
               "below that RETURN rather than break would strand the raise, so the declaration's own cproto "
               "check and this one have drifted apart");
        js_no_user_code_enter();
    }
    switch(cproto) {
    case JS_CFUNC_constructor:
    case JS_CFUNC_constructor_or_func:
        if (!(flags & JS_CALL_FLAG_CONSTRUCTOR)) {
            if (cproto == JS_CFUNC_constructor) {
            not_a_constructor:
                ret_val = JS_ThrowTypeError(ctx, "must be called with new");
                break;
            } else {
                this_obj = JS_UNDEFINED;
            }
        }
        /* here this_obj is new_target */
        /* fall thru */
    case JS_CFUNC_generic:
        ret_val = func.generic(ctx, this_obj, argc, arg_buf);
        break;
    case JS_CFUNC_constructor_magic:
    case JS_CFUNC_constructor_or_func_magic:
        if (!(flags & JS_CALL_FLAG_CONSTRUCTOR)) {
            if (cproto == JS_CFUNC_constructor_magic) {
                goto not_a_constructor;
            } else {
                this_obj = JS_UNDEFINED;
            }
        }
        /* fall thru */
    case JS_CFUNC_generic_magic:
        ret_val = func.generic_magic(ctx, this_obj, argc, arg_buf,
                                     p->u.cfunc.magic);
        break;
    case JS_CFUNC_getter:
        ret_val = func.getter(ctx, this_obj);
        break;
    case JS_CFUNC_setter:
        ret_val = func.setter(ctx, this_obj, arg_buf[0]);
        break;
    case JS_CFUNC_getter_magic:
        ret_val = func.getter_magic(ctx, this_obj, p->u.cfunc.magic);
        break;
    case JS_CFUNC_setter_magic:
        ret_val = func.setter_magic(ctx, this_obj, arg_buf[0], p->u.cfunc.magic);
        break;
    case JS_CFUNC_f_f:
        {
            double d1;

            if (unlikely(JS_ToFloat64(ctx, &d1, arg_buf[0]))) {
                ret_val = JS_EXCEPTION;
                break;
            }
            ret_val = js_number(func.f_f(d1));
        }
        break;
    case JS_CFUNC_f_f_f:
        {
            double d1, d2;

            if (unlikely(JS_ToFloat64(ctx, &d1, arg_buf[0]))) {
                ret_val = JS_EXCEPTION;
                break;
            }
            if (unlikely(JS_ToFloat64(ctx, &d2, arg_buf[1]))) {
                ret_val = JS_EXCEPTION;
                break;
            }
            ret_val = js_number(func.f_f_f(d1, d2));
        }
        break;
    case JS_CFUNC_iterator_next:
        {
            int done;
            ret_val = func.iterator_next(ctx, this_obj, argc, arg_buf,
                                         &done, p->u.cfunc.magic);
            if (!JS_IsException(ret_val) && done != 2) {
                ret_val = js_create_iterator_result(ctx, ret_val, done);
            }
        }
        break;
    case JS_CFUNC_iterdrive:
        {
            /* A lazy Iterator Helper's .next() reached OUTSIDE the drive. There is no body — the drive IS the
               implementation — so the only ways here are the two the routing predicate declines, and both are
               spec ANSWERS rather than a fallback: a receiver with no [[UnderlyingIterator]], and a helper that
               is already driving (GeneratorValidate). Anything else is an unrouted call site, and the DCHECK
               says so instead of letting it look like a TypeError the page asked for. */
            return js_iter_helper_declined(ctx, this_obj);
        }
    case JS_CFUNC_step_ctor:
    case JS_CFUNC_consume:
        /* A consuming builtin reached OUTSIDE the interpreter's dispatch. There is no body to run — the walk IS
           the consume machine, which needs a tramp chain — so this names the unrouted call site rather than
           driving the source's .next() to completion from here. */
        DFAIL("a declared consume builtin was invoked outside the interpreter's dispatch — route that call site "
              "onto do_consumer_dispatch; there is no second driver");
        return JS_ThrowTypeError(ctx, "consume builtin not on the trampoline");
    case JS_CFUNC_step:
        {
            /* A step builtin reached OUTSIDE the interpreter's dispatch (a host JS_Call). It runs the SAME step
               machine — this is not a second implementation, there is no other one. A callback invoked from here
               cannot suspend (no tramp frame above us), which is exactly why the interpreter drives these on the
               chain instead; reaching this driver during a flow means a call path bypassed that. */
            /* DELETED: the inline JS_Call driver. It was a SECOND driver for the same step machine and a
               non-suspending one — a callback with a loop preempts inside it and aborts anyway, so it never
               degraded gracefully, it just moved where the crash surfaced. With it gone, every call site must
               route to do_step_tramp; one that does not crashes HERE, naming itself, instead of silently
               driving a callback to completion. */
            /* One cause now: a CALL SHAPE that was never routed. Route it onto do_step_tramp, the way .call /
               .apply / spread / Reflect.apply / Reflect.construct / a bound target / super() already are.
               The other cause used to be a coercion inside a step machine's own init — `[].every(f)` reading its
               length, `slice` clamping its indices — user code reached from C before the state existed, with no
               step to route it to. There is no init any more: the driver allocates the state and the prologue is
               step 0, so a coercion in a prologue requests a TOPRIMITIVE step exactly like one in a body. */
            /* NAME the builtin — from its DEF, never from a property read. This read the `name` property with
               JS_GetProperty and justified it as "safe because the very next statement is abort()". That claim
               is false in three ways and each one is a defect an abort path must not have. (1) A property read
               runs the property machinery, which has asserts of its own: a receiver whose `name` is an ACCESSOR
               aborts inside JS_GetPropertyInternal's own getter DFAIL, so the report names the wrong failure
               entirely and the mechanism this assert exists to name is never printed — an assert that fires
               about itself. (2) A getter IS page code, which is the one thing this engine forbids reaching from
               a C activation, so the diagnostic for a missing flow base was itself running without one.
               (3) DFAIL compiles out in release while the read does not, so a release build ran the getter and
               leaked both the JSValue and the CString on a path that then returned an exception nobody set.
               The def is the honest source and a better one: `.algorithm` is a static string naming the SPEC
               ALGORITHM, which is what a reader needs, where `name` is a page-writable property that any bundle
               can redefine. The whole block is dev-only, so release neither formats nor reads anything. */
#if APICLIENT_DEV
            {
                const JSTrampStepDef *sdef = tramp_step_def_of(func_obj);
                char why[256];
                DCHECK(sdef && sdef->algorithm,
                       "a JS_CFUNC_step callee carries no step def to name it — the cproto and the def are set "
                       "together at the declaration, so one without the other is a half-finished registration");
                snprintf(why, sizeof(why),
                         "the step machine for %s (STEPDEF id %d) was invoked outside the interpreter's "
                         "dispatch — route that call site onto do_step_tramp; there is no second driver",
                         sdef->algorithm, p->u.cfunc.magic);
                DFAIL(why);
            }
#endif
            ret_val = JS_EXCEPTION;
        }
        break;
    default:
        abort();
    }
    if (unlikely(p->u.cfunc.no_user_code))
        js_no_user_code_leave();

    rt->current_stack_frame = sf->prev_frame;
    return ret_val;
}

/* A bound function's §10.4.1.1 [[Call]] ( thisArg, argList ) and §10.4.1.2 [[Construct]] ( argList, newTarget )
   are rewrites the interpreter performs at its call and construct sites (tramp_bound_target): the ultimate
   target is dispatched on the running chain, and a call-root flow (JS_FlowNewCall) reaches the same sites. This
   class hook is reached only by C calling the bound function, which would run the target's body in a C
   activation with no flow base to park in, so it is refused rather than driven. */
static JSValue js_call_bound_function(JSContext *ctx, JSValueConst func_obj,
                                      JSValueConst this_obj,
                                      int argc, JSValueConst *argv, int flags)
{
    (void)func_obj; (void)this_obj; (void)argc; (void)argv; (void)flags;
    DFAIL("a bound function was called from C through its class hook: its target would run with no flow base. "
          "Route that C caller onto a CALL or CONSTRUCT request on the chain, or a call-root flow "
          "(JS_FlowNewCall), whose dispatch unwraps the bound function at the interpreter's call site");
    return JS_ThrowTypeError(ctx, "a bound function cannot be called from a C activation");
}

