/* Note: the property value is not initialized. Return NULL if memory
   error. */
static JSProperty *add_property(JSContext *ctx,
                                JSObject *p, JSAtom prop, int prop_flags)
{
    JSShape *sh, *new_sh;

    if (unlikely(p->is_prototype)) {
        /* track addition of small integer properties to
           Array.prototype and Object.prototype */
        if (unlikely((p == JS_VALUE_GET_OBJ(ctx->class_proto[JS_CLASS_ARRAY]) ||
                      p == JS_VALUE_GET_OBJ(ctx->class_proto[JS_CLASS_OBJECT])) &&
                     __JS_AtomIsTaggedInt(prop))) {
            ctx->std_array_prototype = false;
        }
    }
    sh = p->shape;
    if (sh->is_hashed) {
        /* try to find an existing shape */
        new_sh = find_hashed_shape_prop(ctx->rt, sh, prop, prop_flags);
        if (new_sh) {
            /* matching shape found: use it */
            /*  the property array may need to be resized */
            if (new_sh->prop_size != sh->prop_size) {
                JSProperty *new_prop;
                new_prop = js_realloc(ctx, p->prop, sizeof(p->prop[0]) *
                                      new_sh->prop_size);
                if (!new_prop)
                    return NULL;
                p->prop = new_prop;
            }
            p->shape = js_dup_shape(new_sh);
            js_free_shape(ctx->rt, sh);
            return &p->prop[new_sh->prop_count - 1];
        } else if (JS_REF_COUNT(sh) != 1) {
            /* if the shape is shared, clone it */
            new_sh = js_clone_shape(ctx, sh);
            if (!new_sh)
                return NULL;
            /* hash the cloned shape */
            new_sh->is_hashed = true;
            js_shape_hash_link(ctx->rt, new_sh);
            js_free_shape(ctx->rt, p->shape);
            p->shape = new_sh;
        }
    }
    DCHECK(JS_REF_COUNT(p->shape) == 1, "JS_REF_COUNT(p->shape) == 1");
    if (add_shape_property(ctx, &p->shape, p, prop, prop_flags))
        return NULL;
    return &p->prop[p->shape->prop_count - 1];
}

/* can be called on JS_CLASS_ARRAY, JS_CLASS_ARGUMENTS or
   JS_CLASS_MAPPED_ARGUMENTS objects. return < 0 if memory alloc
   error. */
static no_inline __exception int convert_fast_array_to_array(JSContext *ctx,
                                                             JSObject *p)
{
    JSProperty *pr;
    JSShape *sh;
    uint32_t i, len, new_count;

    /* track modification of Array.prototype */
    if (unlikely(p == JS_VALUE_GET_OBJ(ctx->class_proto[JS_CLASS_ARRAY]))) {
        ctx->std_array_prototype = false;
    }
    if (js_shape_prepare_update(ctx, p, NULL))
        return -1;
    len = p->u.array.count;
    /* resize the properties once to simplify the error handling */
    sh = p->shape;
    new_count = sh->prop_count + len;
    if (new_count > sh->prop_size) {
        if (resize_properties(ctx, &p->shape, p, new_count))
            return -1;
    }

    if (p->class_id == JS_CLASS_MAPPED_ARGUMENTS) {
        JSVarRef **tab = p->u.array.u.var_refs;
        for(i = 0; i < len; i++) {
            /* add_property cannot fail here but
               __JS_AtomFromUInt32(i) fails for i > INT32_MAX */
            pr = add_property(ctx, p, __JS_AtomFromUInt32(i), JS_PROP_C_W_E | JS_PROP_VARREF);
            pr->u.var_ref = *tab++;
        }
    } else {
        JSValue *tab = p->u.array.u.values;
        for(i = 0; i < len; i++) {
            /* add_property cannot fail here but
               __JS_AtomFromUInt32(i) fails for i > INT32_MAX */
            pr = add_property(ctx, p, __JS_AtomFromUInt32(i), JS_PROP_C_W_E);
            pr->u.value = *tab++;
        }
    }
    js_free(ctx, p->u.array.u.values);
    p->u.array.count = 0;
    p->u.array.u.values = NULL; /* fail safe */
    p->u.array.u1.size = 0;
    p->fast_array = 0;
    return 0;
}

/* THE OPERATION AND THE STORAGE, ONE BODY, and `as_slot` says which of the two this call is.
   10.1.10.1 OrdinaryDelete asks whether the slot is CONFIGURABLE and hands an exotic object its own handler;
   a COW restore asks NEITHER. It removes storage the running flow created, and the flow that created it is
   very often the flow that made it non-configurable with a define — `Object.defineProperty(shared,"x",{value:1})`
   defaults C/W/E to false — so a removal that consults the flow's own attributes is a removal that REFUSES,
   and a refusal in a context switch has no flow to report to. That is the same line JS_SetOwnSlotDesc draws on
   the write side and JS_GetOwnSlotDesc on the read side: the scheduler is not the page, and a swap moves
   storage. The capture is likewise the OPERATION's — a slot removal made by the swap is not a write of the
   page's that any delta records.
   BOTH HALVES OF "NEITHER" ARE THE CODE'S NOW. The exotic half used to be an assert saying no delta could ever
   name an exotic object, and the GLOBAL OBJECT of this engine is one (JS_SetGlobalClass installs the Window
   class, whose exotic hooks answer §7.2.2.2's supported indices) — so it named the most-captured object there
   is. `delete globalThis.x` captures the absent slot at this function's head, removes nothing, and the next
   context switch asks this body to take out a slot that was never in the shape; on an ordinary object that is
   the `return true` below, and on the global it was an abort before one page script had finished. */
static int delete_property(JSContext *ctx, JSObject *p, JSAtom atom, bool as_slot)
{
    JSShape *sh;
    JSShapeProperty *pr, *lpr, *prop;
    JSProperty *pr1;
    uint32_t lpr_idx;
    intptr_t h, h1;

    /* forced-exec TIME-TRAVEL: a DELETE of a baseline slot is a shared-state mutation exactly like a write —
       record the slot's value FIRST (existed=1, base=value) so unapply re-creates it, and so the flow's own
       deletion is REPLAYED AS A DELETION rather than as a write of undefined (the host's entry records whether
       the flow HAS the slot, not only what value it holds — see cow_capture_hook). Note this runs before the
       walk below knows whether the slot is there at all, so an absent one is captured too, and the same
       absent-side record is what keeps apply from inventing a property out of it. Gated by the hook
       (flow-local + not-installed skip). */
    if (!as_slot)
        cow_capture(ctx, JS_MKPTR(JS_TAG_OBJECT, p), atom);

 redo:
    sh = p->shape;
    h1 = atom & sh->prop_hash_mask;
    h = prop_hash_end(sh)[-h1 - 1];
    prop = get_shape_prop(sh);
    lpr = NULL;
    lpr_idx = 0;   /* prevent warning */
    while (h != 0) {
        pr = &prop[h - 1];
        if (likely(pr->atom == atom)) {
            /* found ! */
            if (!(pr->flags & JS_PROP_CONFIGURABLE) && !as_slot)
                return false;
            /* realloc the shape if needed */
            if (lpr)
                lpr_idx = lpr - get_shape_prop(sh);
            if (js_shape_prepare_update(ctx, p, &pr))
                return -1;
            sh = p->shape;
            /* remove property */
            if (lpr) {
                lpr = &get_shape_prop(sh)[lpr_idx];
                lpr->hash_next = pr->hash_next;
            } else {
                prop_hash_end(sh)[-h1 - 1] = pr->hash_next;
            }
            sh->deleted_prop_count++;
            /* free the entry */
            pr1 = &p->prop[h - 1];
            free_property(ctx->rt, pr1, pr->flags);
            JS_FreeAtom(ctx, pr->atom);
            /* put default values */
            pr->flags = 0;
            pr->atom = JS_ATOM_NULL;
            pr1->u.value = JS_UNDEFINED;

            /* compact the properties if too many deleted properties */
            if (sh->deleted_prop_count >= 8 &&
                sh->deleted_prop_count >= ((unsigned)sh->prop_count / 2)) {
                compact_properties(ctx, p);
            }
            return true;
        }
        lpr = pr;
        h = pr->hash_next;
    }

    if (p->is_exotic) {
        if (p->fast_array) {
            uint32_t idx;
            if (JS_AtomIsArrayIndex(ctx, &idx, atom) &&
                idx < p->u.array.count) {
                if (p->class_id == JS_CLASS_ARRAY ||
                    p->class_id == JS_CLASS_ARGUMENTS ||
                    p->class_id == JS_CLASS_MAPPED_ARGUMENTS) {
                    if (convert_fast_array_to_array(ctx, p))
                        return -1;
                    goto redo;
                } else {
                    return false;
                }
            }
        } else if (!as_slot) {
            /* 10.1.10.1 step 1's `obj.[[GetOwnProperty]]` on an exotic object is the CLASS's, and this is
               where the OPERATION asks it. A slot removal does not: an exotic named-property hook is a VIEW
               over storage that is NOT this object's shape — Storage's is a JS map object written through
               JS_SetProperty, DOMStringMap's is the element's attributes written through dom_cow's chokepoints,
               Window's supported indices are child navigables and a String object's are the string's own code
               units — so the storage a flow can mutate through the view is captured on the object that OWNS it
               and never as a slot on the viewer. Consulting the hook here would remove state the delta never
               named, mid-context-switch, with no flow base to run it on. */
            const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
            if (em && em->delete_property)
                return em->delete_property(ctx, JS_MKPTR(JS_TAG_OBJECT, p), atom);
        }
    }
    /* not found */
    return true;
}

/* THE SLOT-LEVEL REMOVAL — see quickjs.h. It CANNOT FAIL, for the reason JS_SetOwnSlotDesc cannot: the only
   two ways it could are a refusal (which the `as_slot` body does not make) and an allocation failure, which is
   a CHECK at its origin rather than a status a caller might default. */
void JS_DeleteOwnSlot(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    JSObject *p;
    int res;

    DCHECK(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT,
           "a COW delta holds a slot on something that is not an object");
    p = JS_VALUE_GET_OBJ(obj);
    DCHECK(p->class_id != JS_CLASS_PROXY,
           "a COW delta holds a Proxy: capturing or swapping it would run the page's traps mid-context-switch");
    res = delete_property(ctx, p, prop, /*as_slot*/true);
    CHECK(res >= 0, "a COW slot removal could not reshape the object: a slot this flow created would stand in "
                    "the baseline for every sibling with nothing left that could take it out");
    DCHECK(res > 0,
           "a COW slot removal was refused by a fast-array class that owns no removable index — a typed array's "
           "elements are the buffer's BYTES and the byte entry owns them, so route the capture that made this "
           "entry through cow_capture_bytes");
}

