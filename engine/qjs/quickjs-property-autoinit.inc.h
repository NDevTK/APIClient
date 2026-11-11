int JS_GetLength(JSContext *ctx, JSValueConst obj, int64_t *pres) {
    return js_get_length64(ctx, pres, obj);
}

int JS_SetLength(JSContext *ctx, JSValueConst obj, int64_t len) {
    return js_set_length64(ctx, obj, len);
}

/* return true, false or (-1) in case of exception */

/* DELETED: js_getOwnPropertyKeys. proposal-joint-iteration Iterator.zipKeyed step 4.a is
   `? iterables.[[OwnPropertyKeys]]()` and the self-hosted body reached it through this helper's
   JS_GetOwnPropertyNames2 — the `ownKeys` trap, plus 10.5.11's invariant, from C. It asked for strings AND
   symbols, which is Reflect.ownKeys exactly, and that is already a step machine; the helper is now that machine
   rather than a second spelling of it. */


/* return the value associated to the autoinit property or an exception */
typedef JSValue JSAutoInitFunc(JSContext *ctx, JSObject *p, JSAtom atom, void *opaque);

static JSAutoInitFunc *const js_autoinit_func_table[] = {
    js_instantiate_prototype, /* JS_AUTOINIT_ID_PROTOTYPE */
    js_module_ns_autoinit, /* JS_AUTOINIT_ID_MODULE_NS */
    JS_InstantiateFunctionListItem2, /* JS_AUTOINIT_ID_PROP */
};

/* warning: 'prs' is reallocated after it */
static int JS_AutoInitProperty(JSContext *ctx, JSObject *p, JSAtom prop,
                               JSProperty *pr, JSShapeProperty *prs)
{
    JSValue val;
    JSContext *realm;
    JSAutoInitFunc *func;

    if (js_shape_prepare_update(ctx, p, &prs))
        return -1;

    realm = js_autoinit_get_realm(pr);
    func = js_autoinit_func_table[js_autoinit_get_id(pr)];
    /* The value is as old as its slot: ECMAScript 10.2.5 MakeConstructor defines a function's "prototype" when the
       function is made, and a lazy intrinsic exists with its holder, so what the realization allocates takes the
       holder's fork generation. Under the running flow's it would be private to that flow (no write to it
       captured) while the holder's slot, shared with every sibling, hands the same object to them all. */
    {
        uint32_t running_gen = g_flow_gen;

        g_flow_gen = p->flow_gen;
        /* 'func' shall not modify the object properties 'pr' */
        val = func(realm, p, prop, pr->u.init.opaque);
        DCHECK(g_flow_gen == p->flow_gen,
               "an autoinit realization forked — the realizers run none of the page's code, so nothing inside one "
               "can take a branch");
        g_flow_gen = running_gen;
    }
    js_autoinit_free(ctx->rt, pr);
    prs->flags &= ~JS_PROP_TMASK;
    pr->u.value = JS_UNDEFINED;
    if (JS_IsException(val))
        return -1;
    pr->u.value = val;
    return 0;
}

