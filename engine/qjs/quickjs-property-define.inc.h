/* compute the property flags. For each flag: (JS_PROP_HAS_x forces
   it, otherwise def_flags is used)
   Note: makes assumption about the bit pattern of the flags
*/
static int get_prop_flags(int flags, int def_flags)
{
    int mask;
    mask = (flags >> JS_PROP_HAS_SHIFT) & JS_PROP_C_W_E;
    return (flags & mask) | (def_flags & ~mask);
}

static int JS_CreateProperty(JSContext *ctx, JSObject *p,
                             JSAtom prop, JSValueConst val,
                             JSValueConst getter, JSValueConst setter,
                             int flags)
{
    JSProperty *pr;
    int ret, prop_flags;

    DCHECK(!(flags & JS_PROP_MATERIALIZE) || (flags & JS_PROP_NO_EXOTIC),
           "a member was materialised through the exotic's own [[DefineOwnProperty]] — materialising is the "
           "class's own act and carries JS_PROP_NO_EXOTIC, so the class is never asked about a member it makes");
    /* forced-exec TIME-TRAVEL: the CREATE half of [[DefineOwnProperty]] has a SECOND entry -- 10.1.9.2 step
       3.d.i's CreateDataProperty, which JS_SetPropertyInternal2 performs on the RECEIVER. That function captures
       at its head, and what it names there is the object it WALKED: `Reflect.set(o, "x", v, receiver)` walks `o`
       and creates the slot on `receiver`, so the entry described an object the write never touched. Captured on
       the object the slot lands on. Reached from JS_DefineProperty the slot is already in the delta and this
       dedups in O(1); it is not folded into that caller because this is the one arm the caller cannot see. */
    cow_capture(ctx, JS_MKPTR(JS_TAG_OBJECT, p), prop);

    /* add a new property or modify an existing exotic one */
    if (p->is_exotic) {
        if (p->class_id == JS_CLASS_ARRAY) {
            uint32_t idx, len;

            if (p->fast_array) {
                if (__JS_AtomIsTaggedInt(prop)) {
                    idx = __JS_AtomToUInt32(prop);
                    if (idx == p->u.array.count) {
                        if (!p->extensible)
                            goto not_extensible;
                        if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET))
                            goto convert_to_array;
                        prop_flags = get_prop_flags(flags, 0);
                        if (prop_flags != JS_PROP_C_W_E)
                            goto convert_to_array;
                        return add_fast_array_element(ctx, p,
                                                      js_dup(val), flags);
                    } else {
                        goto convert_to_array;
                    }
                } else if (JS_AtomIsArrayIndex(ctx, &idx, prop)) {
                    /* convert the fast array to normal array */
                convert_to_array:
                    if (convert_fast_array_to_array(ctx, p))
                        return -1;
                    goto generic_array;
                }
            } else if (JS_AtomIsArrayIndex(ctx, &idx, prop)) {
                JSProperty *plen;
                JSShapeProperty *pslen;
            generic_array:
                /* update the length field */
                plen = &p->prop[0];
                JS_ToUint32(ctx, &len, plen->u.value);
                if ((idx + 1) > len) {
                    pslen = get_shape_prop(p->shape);
                    if (unlikely(!(pslen->flags & JS_PROP_WRITABLE)))
                        return JS_ThrowTypeErrorReadOnly(ctx, flags, JS_ATOM_length);
                    /* XXX: should update the length after defining
                       the property */
                    len = idx + 1;
                    /* forced-exec TIME-TRAVEL: the third length write, and the one furthest from anything that
                       looks like `length`. 10.4.2.1 step 3.f.i grows the length when an index at or above it is
                       defined, and on a SLOW array (or a fast one this define just converted) that write lands
                       here, under an atom that is the INDEX. The unapply used to take the length back down as a
                       side effect of removing the created index; it removes the index as the ordinary property
                       it is now, so the length is captured as the slot it is — same as add_fast_array_element's
                       and js_array_push_step's. */
                    cow_capture(ctx, JS_MKPTR(JS_TAG_OBJECT, p), JS_ATOM_length);
                    /* RE-FOUND ACROSS THE CAPTURE, for the reason JS_SetOwnSlotDesc re-finds `prs` across its
                       payload write: a capture may grow the host's delta, a growth may release a parked flow,
                       and a release walks the heap — so a JSProperty pointer taken before it is a pointer into
                       storage that may have been reallocated under it. */
                    plen = &p->prop[0];
                    set_value(ctx, &plen->u.value, js_uint32(len));
                }
            }
        } else if (is_typed_array(p->class_id)) {
            ret = JS_AtomIsNumericIndex(ctx, prop);
            if (ret != 0) {
                if (ret < 0)
                    return -1;
                return JS_ThrowTypeErrorOrFalse(ctx, flags, "cannot create numeric index in typed array");
            }
        } else if (!(flags & JS_PROP_NO_EXOTIC)) {
            const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
            if (em) {
                if (em->define_own_property) {
                    return em->define_own_property(ctx, JS_MKPTR(JS_TAG_OBJECT, p),
                                                   prop, val, getter, setter, flags);
                }
                ret = JS_IsExtensible(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
                if (ret < 0)
                    return -1;
                if (!ret)
                    goto not_extensible;
            }
        }
    }

    if (!p->extensible && !(flags & JS_PROP_MATERIALIZE)) {
    not_extensible:
        return JS_ThrowTypeErrorOrFalse(ctx, flags, "object is not extensible");
    }

    if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) {
        prop_flags = (flags & (JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE)) |
            JS_PROP_GETSET;
    } else {
        prop_flags = flags & JS_PROP_C_W_E;
    }
    pr = add_property(ctx, p, prop, prop_flags);
    if (unlikely(!pr))
        return -1;
    if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) {
        pr->u.getset.getter = NULL;
        if ((flags & JS_PROP_HAS_GET) && JS_IsFunction(ctx, getter)) {
            pr->u.getset.getter =
                JS_VALUE_GET_OBJ(js_dup(getter));
        }
        pr->u.getset.setter = NULL;
        if ((flags & JS_PROP_HAS_SET) && JS_IsFunction(ctx, setter)) {
            pr->u.getset.setter =
                JS_VALUE_GET_OBJ(js_dup(setter));
        }
    } else {
        if (flags & JS_PROP_HAS_VALUE) {
            pr->u.value = js_dup(val);
            JS_KEY_LEDGER_VALUE(ctx, val, 1);
        } else {
            pr->u.value = JS_UNDEFINED;
        }
    }
    return true;
}

/* return false if not OK */
static bool check_define_prop_flags(int prop_flags, int flags)
{
    bool has_accessor, is_getset;

    if (!(prop_flags & JS_PROP_CONFIGURABLE)) {
        if ((flags & (JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE)) ==
            (JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE)) {
            return false;
        }
        if ((flags & JS_PROP_HAS_ENUMERABLE) &&
            (flags & JS_PROP_ENUMERABLE) != (prop_flags & JS_PROP_ENUMERABLE))
            return false;
    }
    if (flags & (JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE |
                 JS_PROP_HAS_GET | JS_PROP_HAS_SET)) {
        if (!(prop_flags & JS_PROP_CONFIGURABLE)) {
            has_accessor = ((flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) != 0);
            is_getset = ((prop_flags & JS_PROP_TMASK) == JS_PROP_GETSET);
            if (has_accessor != is_getset)
                return false;
            if (!has_accessor && !is_getset && !(prop_flags & JS_PROP_WRITABLE)) {
                /* not writable: cannot set the writable bit */
                if ((flags & (JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE)) ==
                    (JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE))
                    return false;
            }
        }
    }
    return true;
}

/* ensure that the shape can be safely modified */
static int js_shape_prepare_update(JSContext *ctx, JSObject *p,
                                   JSShapeProperty **pprs)
{
    JSShape *sh;
    uint32_t idx = 0;    /* prevent warning */

    sh = p->shape;
    if (sh->is_hashed) {
        if (JS_REF_COUNT(sh) != 1) {
            if (pprs)
                idx = *pprs - get_shape_prop(sh);
            /* clone the shape (the resulting one is no longer hashed) */
            sh = js_clone_shape(ctx, sh);
            if (!sh)
                return -1;
            js_free_shape(ctx->rt, p->shape);
            p->shape = sh;
            if (pprs)
                *pprs = &get_shape_prop(sh)[idx];
        } else {
            js_shape_hash_unlink(ctx->rt, sh);
            sh->is_hashed = false;
        }
    }
    return 0;
}

static int js_update_property_flags(JSContext *ctx, JSObject *p,
                                    JSShapeProperty **pprs, int flags)
{
    if (flags != (*pprs)->flags) {
        if (js_shape_prepare_update(ctx, p, pprs))
            return -1;
        (*pprs)->flags = flags;
    }
    return 0;
}

/* allowed flags:
   JS_PROP_CONFIGURABLE, JS_PROP_WRITABLE, JS_PROP_ENUMERABLE
   JS_PROP_HAS_GET, JS_PROP_HAS_SET, JS_PROP_HAS_VALUE,
   JS_PROP_HAS_CONFIGURABLE, JS_PROP_HAS_WRITABLE, JS_PROP_HAS_ENUMERABLE,
   JS_PROP_THROW, JS_PROP_NO_EXOTIC.
   If JS_PROP_THROW is set, return an exception instead of false.
   if JS_PROP_NO_EXOTIC is set, do not call the exotic
   define_own_property callback.
   return -1 (exception), false or true.
*/
int JS_DefineProperty(JSContext *ctx, JSValueConst this_obj,
                      JSAtom prop, JSValueConst val,
                      JSValueConst getter, JSValueConst setter, int flags)
{
    JSObject *p;
    JSShapeProperty *prs;
    JSProperty *pr;
    int mask, res;

    if (JS_VALUE_GET_TAG(this_obj) != JS_TAG_OBJECT) {
        JS_ThrowTypeErrorNotAnObject(ctx);
        return -1;
    }
    p = JS_VALUE_GET_OBJ(this_obj);

    /* forced-exec TIME-TRAVEL: [[DefineOwnProperty]] is the OTHER way a shared slot changes, and it is the one
       that had no capture at all. Every arm below writes the slot IN PLACE -- the data value, the getter/setter
       pair, the var_ref, the array length, and the attribute bits through js_update_property_flags -- and the
       tail creates one, so a flow's Object.defineProperty / Reflect.defineProperty / JS_DefinePropertyValue* /
       JS_DefinePropertyGetSet on a BASELINE object stood in the baseline for every sibling and survived the
       unapply whose whole job is to take it back out. A top-level `function f(){}` is one of them:
       JS_DefineGlobalFunction routes here, which is why the var/let DEFINE path below captures and its function
       twin did not.
       ONE capture at the OPERATION rather than one per arm, for the reason a dense element is captured inside
       set_fast_array_element rather than at its seven writers: a record the delta must see cannot be a line each
       arm remembers.
       WHAT THE ENTRY HOLDS IS THE WHOLE §6.2.6 DESCRIPTOR, which is what a define needs and a [[Set]] did not:
       this operation changes a slot's ATTRIBUTES and can change its SHAPE (data <-> accessor), so an entry that
       recorded only a value could express neither and had to abort on both. JS_GetOwnSlotDesc reads the pair or
       the value with its C/W/E on both sides of the round trip -- the baseline read this capture makes, and the
       flow-side read cow_save_cur makes when the flow is switched out -- and JS_SetOwnSlotDesc puts back
       whichever shape it was given.
       A TYPED ARRAY'S NUMERIC INDEX carries itself to the BYTES from inside cow_capture — 10.4.5.3's define
       delegates to TypedArraySetElement, which is JS_SetPropertyValue's typed-array arm and writes the buffer,
       so an entry naming (view, index) would be a second and weaker record of storage the byte entry owns. That
       is the entry `Object.defineProperty(ta, 0, {value:1})` used to produce, and it is routed at the one place
       rather than here, because this is not the only site that reaches a typed array with an index. */
    cow_capture(ctx, this_obj, prop);

 redo_prop_update:
    prs = find_own_property(&pr, p, prop);
    if (prs) {
        /* the range of the Array length property is always tested before */
        if ((prs->flags & JS_PROP_LENGTH) && (flags & JS_PROP_HAS_VALUE)) {
            uint32_t array_length;
            if (JS_ToArrayLengthFree(ctx, &array_length,
                                     js_dup(val), false)) {
                return -1;
            }
            /* this code relies on the fact that Uint32 are never allocated */
            val = js_uint32(array_length);
            /* prs may have been modified */
            prs = find_own_property(&pr, p, prop);
            DCHECK(prs != NULL, "prs != NULL");
        }
        /* property already exists */
        if (!check_define_prop_flags(prs->flags, flags)) {
        not_configurable:
            return JS_ThrowTypeErrorOrFalse(ctx, flags, "property is not configurable");
        }

        if ((prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
            /* Instantiate property and retry */
            if (JS_AutoInitProperty(ctx, p, prop, pr, prs))
                return -1;
            goto redo_prop_update;
        }

        if (flags & (JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE |
                     JS_PROP_HAS_GET | JS_PROP_HAS_SET)) {
            if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) {
                JSObject *new_getter, *new_setter;

                if (JS_IsFunction(ctx, getter)) {
                    new_getter = JS_VALUE_GET_OBJ(getter);
                } else {
                    new_getter = NULL;
                }
                if (JS_IsFunction(ctx, setter)) {
                    new_setter = JS_VALUE_GET_OBJ(setter);
                } else {
                    new_setter = NULL;
                }

                if ((prs->flags & JS_PROP_TMASK) != JS_PROP_GETSET) {
                    if (js_shape_prepare_update(ctx, p, &prs))
                        return -1;
                    /* convert to getset */
                    if ((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF) {
                        free_var_ref(ctx->rt, pr->u.var_ref);
                    } else {
                        JS_FreeValue(ctx, pr->u.value);
                    }
                    prs->flags = (prs->flags &
                                  (JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE)) |
                        JS_PROP_GETSET;
                    pr->u.getset.getter = NULL;
                    pr->u.getset.setter = NULL;
                } else {
                    if (!(prs->flags & JS_PROP_CONFIGURABLE)) {
                        if ((flags & JS_PROP_HAS_GET) &&
                            new_getter != pr->u.getset.getter) {
                            goto not_configurable;
                        }
                        if ((flags & JS_PROP_HAS_SET) &&
                            new_setter != pr->u.getset.setter) {
                            goto not_configurable;
                        }
                    }
                }
                if (flags & JS_PROP_HAS_GET) {
                    if (pr->u.getset.getter)
                        JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.getter));
                    if (new_getter)
                        js_dup(getter);
                    pr->u.getset.getter = new_getter;
                }
                if (flags & JS_PROP_HAS_SET) {
                    if (pr->u.getset.setter)
                        JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.setter));
                    if (new_setter)
                        js_dup(setter);
                    pr->u.getset.setter = new_setter;
                }
            } else {
                if ((prs->flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
                    /* convert to data descriptor */
                    if (js_shape_prepare_update(ctx, p, &prs))
                        return -1;
                    if (pr->u.getset.getter)
                        JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.getter));
                    if (pr->u.getset.setter)
                        JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.setter));
                    prs->flags &= ~(JS_PROP_TMASK | JS_PROP_WRITABLE);
                    pr->u.value = JS_UNDEFINED;
                } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF) {
                    /* Note: JS_PROP_VARREF is always writable */
                } else {
                    if ((prs->flags & (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)) == 0 &&
                        (flags & JS_PROP_HAS_VALUE)) {
                        if (!js_same_value(ctx, val, pr->u.value)) {
                            goto not_configurable;
                        } else {
                            return true;
                        }
                    }
                }
                if ((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF) {
                    if (flags & JS_PROP_HAS_VALUE) {
                        if (p->class_id == JS_CLASS_MODULE_NS) {
                            /* JS_PROP_WRITABLE is always true for variable
                               references, but they are write protected in module name
                               spaces. */
                            if (!js_same_value(ctx, val, *pr->u.var_ref->pvalue))
                                goto not_configurable;
                        }
                        /* update the reference */
                        set_value(ctx, pr->u.var_ref->pvalue, js_dup(val));
                    }
                    /* if writable is set to false, no longer a
                       reference (for mapped arguments) */
                    if ((flags & (JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE)) == JS_PROP_HAS_WRITABLE) {
                        JSValue val1;
                        if (js_shape_prepare_update(ctx, p, &prs))
                            return -1;
                        val1 = js_dup(*pr->u.var_ref->pvalue);
                        free_var_ref(ctx->rt, pr->u.var_ref);
                        pr->u.value = val1;
                        prs->flags &= ~(JS_PROP_TMASK | JS_PROP_WRITABLE);
                    }
                } else if (prs->flags & JS_PROP_LENGTH) {
                    if (flags & JS_PROP_HAS_VALUE) {
                        /* Note: no JS code is executable because
                           'val' is guaranted to be a Uint32 */
                        res = set_array_length(ctx, p, js_dup(val), flags);
                    } else {
                        res = true;
                    }
                    /* still need to reset the writable flag if
                       needed.  The JS_PROP_LENGTH is kept because the
                       Uint32 test is still done if the length
                       property is read-only. */
                    if ((flags & (JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE)) ==
                        JS_PROP_HAS_WRITABLE) {
                        prs = get_shape_prop(p->shape);
                        if (js_update_property_flags(ctx, p, &prs,
                                                     prs->flags & ~JS_PROP_WRITABLE))
                            return -1;
                    }
                    return res;
                } else {
                    if (flags & JS_PROP_HAS_VALUE) {
                        JS_KEY_LEDGER_VALUE(ctx, pr->u.value, -1);
                        JS_FreeValue(ctx, pr->u.value);
                        pr->u.value = js_dup(val);
                        JS_KEY_LEDGER_VALUE(ctx, val, 1);
                    }
                    if (flags & JS_PROP_HAS_WRITABLE) {
                        if (js_update_property_flags(ctx, p, &prs,
                                                     (prs->flags & ~JS_PROP_WRITABLE) |
                                                     (flags & JS_PROP_WRITABLE)))
                            return -1;
                    }
                }
            }
        }
        mask = 0;
        if (flags & JS_PROP_HAS_CONFIGURABLE)
            mask |= JS_PROP_CONFIGURABLE;
        if (flags & JS_PROP_HAS_ENUMERABLE)
            mask |= JS_PROP_ENUMERABLE;
        if (js_update_property_flags(ctx, p, &prs,
                                     (prs->flags & ~mask) | (flags & mask)))
            return -1;
        return true;
    }

    /* handle modification of fast array elements */
    if (p->fast_array) {
        uint32_t idx;
        uint32_t prop_flags;
        if (p->class_id == JS_CLASS_ARRAY) {
            if (__JS_AtomIsTaggedInt(prop)) {
                idx = __JS_AtomToUInt32(prop);
                if (idx < p->u.array.count) {
                    prop_flags = get_prop_flags(flags, JS_PROP_C_W_E);
                    if (prop_flags != JS_PROP_C_W_E)
                        goto convert_to_slow_array;
                    if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) {
                    convert_to_slow_array:
                        if (convert_fast_array_to_array(ctx, p))
                            return -1;
                        else
                            goto redo_prop_update;
                    }
                    if (flags & JS_PROP_HAS_VALUE) {
                        set_fast_array_element(ctx, p, idx, js_dup(val));
                    }
                    return true;
                }
            }
        } else if (is_typed_array(p->class_id)) {
            JSValue num;
            int ret;

            if (!__JS_AtomIsTaggedInt(prop)) {
                /* slow path with to handle all numeric indexes */
                num = JS_AtomIsNumericIndex1(ctx, prop);
                if (JS_IsUndefined(num))
                    goto typed_array_done;
                if (JS_IsException(num))
                    return -1;
                ret = JS_NumberIsInteger(ctx, num);
                if (ret < 0) {
                    JS_FreeValue(ctx, num);
                    return -1;
                }
                if (!ret) {
                    JS_FreeValue(ctx, num);
                    return JS_ThrowTypeErrorOrFalse(ctx, flags, "non integer index in typed array");
                }
                ret = JS_NumberIsNegativeOrMinusZero(ctx, num);
                JS_FreeValue(ctx, num);
                if (ret) {
                    return JS_ThrowTypeErrorOrFalse(ctx, flags, "negative index in typed array");
                }
                if (!__JS_AtomIsTaggedInt(prop))
                    goto typed_array_oob;
            }
            idx = __JS_AtomToUInt32(prop);
            /* if the typed array is detached, p->u.array.count = 0 */
            if (idx >= p->u.array.count) {
            typed_array_oob:
                return JS_ThrowTypeErrorOrFalse(ctx, flags, "out-of-bound index in typed array");
            }
            prop_flags = get_prop_flags(flags, JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
            if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET) ||
                prop_flags != (JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE)) {
                return JS_ThrowTypeErrorOrFalse(ctx, flags, "invalid descriptor flags");
            }
            if (flags & JS_PROP_HAS_VALUE) {
                return JS_SetPropertyValue(ctx, this_obj, js_int32(idx), js_dup(val), flags);
            }
            return true;
        typed_array_done: ;
        }
    }

    return JS_CreateProperty(ctx, p, prop, val, getter, setter, flags);
}

static int JS_DefineAutoInitProperty(JSContext *ctx, JSValueConst this_obj,
                                     JSAtom prop, JSAutoInitIDEnum id,
                                     void *opaque, int flags)
{
    JSObject *p;
    JSProperty *pr;

    if (JS_VALUE_GET_TAG(this_obj) != JS_TAG_OBJECT)
        return false;

    p = JS_VALUE_GET_OBJ(this_obj);

    if (find_own_property(&pr, p, prop)) {
        /* property already exists */
        abort();
        return false;
    }

    /* Specialized CreateProperty */
    pr = add_property(ctx, p, prop, (flags & JS_PROP_C_W_E) | JS_PROP_AUTOINIT);
    if (unlikely(!pr))
        return -1;
    pr->u.init.realm_and_id = (uintptr_t)JS_DupContext(ctx);
    DCHECK((pr->u.init.realm_and_id & 3) == 0, "(pr->u.init.realm_and_id & 3) == 0");
    DCHECK(id <= 3, "id <= 3");
    pr->u.init.realm_and_id |= id;
    pr->u.init.opaque = opaque;
    return true;
}

/* Like JS_DefinePropertyValue but borrows val (does not free it) */
static int JS_DefinePropertyValueConst(JSContext *ctx, JSValueConst this_obj,
                                       JSAtom prop, JSValueConst val, int flags)
{
    return JS_DefineProperty(ctx, this_obj, prop, val, JS_UNDEFINED, JS_UNDEFINED,
                             flags | JS_PROP_HAS_VALUE | JS_PROP_HAS_CONFIGURABLE |
                             JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE);
}

/* shortcut to add or redefine a new property value */
int JS_DefinePropertyValue(JSContext *ctx, JSValueConst this_obj,
                           JSAtom prop, JSValue val, int flags)
{
    int ret;
    ret = JS_DefinePropertyValueConst(ctx, this_obj, prop, val, flags);
    JS_FreeValue(ctx, val);
    return ret;
}

/* A DATA PROPERTY UNDER A KEY THE CALLER ONLY BORROWS — the spelling every snapshot walk needs, and the one
   JS_DefinePropertyValue's signature does not state.
 *
 * ITS TWO ARGUMENTS HAVE TWO DIFFERENT OWNERSHIPS: it frees its VALUE and never its KEY. JS_DefinePropertyValueValue
 * directly below is the proof — it JS_FreeAtoms the atom it built the moment the define returns, because nothing
 * else will. So a machine whose own state OWNS the key has nothing to hand over, and the JS_DupAtom that looks
 * like it balances the call is an increment nobody matches: one leaked atom reference per property, per walk.
 * Three sites had exactly that — Iterator.zipKeyed's tuple, Object.getOwnPropertyDescriptors, and object SPREAD —
 * each of them a machine holding an [[OwnPropertyKeys]] snapshot it releases itself.
 *
 * THE ASSERT IS THE LEDGER, and it is here rather than at those sites so the next key-holding builtin inherits
 * it. A define takes AT MOST the one reference the new shape property owns — and none at all when the key is
 * already in the shape or a hashed shape that already names it is reused — PLUS whatever the TIME-TRAVEL
 * CAPTURE took, PLUS the value slot's reference when the value is the key's own string (KEY_OWNER_VALUE).
 * [[DefineOwnProperty]] on a SHARED object records the slot in the running flow's delta, and the delta's entry
 * NAMES that slot by its atom and owns a reference on it (g_key_ledger_atom carries the whole argument). So
 * the admitted rise is `1 + capture + value`, each measured rather than assumed: an armed window over that
 * one atom, credited only where the hook actually runs. Anything above that came from the CALLER and is a
 * reference the runtime will never get back. The value is freed AFTER the reading, which is why this does not
 * simply wrap JS_DefinePropertyValue: releasing it can drop the last object that interned this same name, and
 * that moves the very number the ledger is about.
 * THE WINDOW IS SAVED AND RESTORED rather than merely armed, because the ledger is a mechanism the next
 * key-holding builtin inherits and none of them may silently zero an outer one's account. */
/* @ATOMLEAK THE ABORT CARRIES ITS OWN OPERANDS AND ITS CALLER'S SITE, because without them it names an
 * action with no object. TWO of this function's three callers are the SAME function — js_prop_walk_step's
 * DESCS arm and its SPREAD arm — so a backtrace prints one line for both, and the message carried no key,
 * no magnitude and no ledger value. MEASURED: attributing one fire on a real page took four gdb-instrumented
 * drives to recover exactly the four numbers below. The file/line pair is captured AT THE CALLER through the
 * macro, which is the spelling this file already uses for JS_ValueToAtomInternal.
 *
 * AND THE REMEDY THIS MESSAGE USED TO NAME IS RETIRED, BECAUSE THE LEDGER IS STRUCTURALLY BLIND TO IT. It
 * ended "a JS_DupAtom in the argument list of a define that borrows its key", and `rc0` is read INSIDE this
 * call — after the caller's argument list has already been evaluated — so such a dup is ALREADY IN rc0 and
 * contributes EXACTLY ZERO to the measured delta. The three sites that had that defect were repaired by
 * reading them; this check could not have reported any of them. What the delta measures is what THIS CALL
 * took, so the remedy is to name which owner inside the define kept a reference.
 *
 * @ATOMOWNER THE ABORT NAMES THE OWNER THAT TOOK THE EXCESS, WHICH IS WHAT A TOTAL CANNOT DO. The budget is
 * `the new shape property` + `what the capture took` + `the value slot's take`, and beside it is a PARTITION
 * of the measured rise over the doors an atom's refcount word can be raised through, censused at the act
 * rather than read off the end. See the block beside JS_DupAtomRT for why it is over DOORS and not over a
 * list of owners. The message prints every term, so the reader is handed the answer rather than a magnitude:
 * `add_shape_property 1` alone is the ordinary add and means the excess is elsewhere; a nonzero
 * `js_clone_shape` means this key was ALREADY in the shape, which makes it a redefine rather than an add and
 * moves the whole diagnosis; an untagged atom-API take names a site nobody has tagged yet.
 *
 * `rest` is what the rise exceeds the partition by: a reference through no instrumented door, which is a
 * JSValue dup of the key's own string somewhere other than a data property's value slot. */
static int js_define_prop_borrowed_key_at(JSContext *ctx, JSValueConst this_obj,
                                          JSAtom prop, JSValue val, int flags,
                                          const char *file, int line)
{
    int ret;
#if APICLIENT_DEV
    JSAtom outer_atom = g_key_ledger_atom;
    int outer_by[KEY_OWNER_COUNT], outer_in_capture = g_key_ledger_in_capture;
    int rc0, rc1, i, tagged, rest, val_is_key;
    memcpy(outer_by, g_key_ledger_by, sizeof(outer_by));
    memset(g_key_ledger_by, 0, sizeof(g_key_ledger_by));
    g_key_ledger_in_capture = 0;
    /* a constant or tagged-integer atom (an array index key) has no refcount for the ledger to account */
    g_key_ledger_atom = __JS_AtomIsConst(prop) ? JS_ATOM_NULL : prop;
    rc0 = js_atom_refcount(ctx->rt, prop);
    /* CONSTANTS ONLY, and read BEFORE the define so no arm can have replaced `val` under it: is the value this
       call stores the atom's own JSString? A const atom has no entry to compare against and no refcount to
       move, so it answers no and every term below is trivially 0. */
    val_is_key = !__JS_AtomIsConst(prop) &&
                 JS_VALUE_GET_TAG(val) == JS_TAG_STRING &&
                 JS_VALUE_GET_STRING(val) == ctx->rt->atom_array[prop];
#else
    (void)file;
    (void)line;
#endif
    ret = JS_DefinePropertyValueConst(ctx, this_obj, prop, val, flags);
#if APICLIENT_DEV
    /* the ledger, spelled as a guarded DFAIL rather than a DCHECK because a DCHECK's condition is still
       TYPE-CHECKED in a release build (`(void)sizeof(cond)`), and every term of this one is dev-only. */
    rc1 = js_atom_refcount(ctx->rt, prop);
    tagged = 0;
    for (i = 0; i < KEY_OWNER_COUNT; i++)
        tagged += g_key_ledger_by[i];
    rest = (rc1 - rc0) - tagged;
    if (ret >= 0 && rc1 > rc0 + 1 + g_key_ledger_by[KEY_OWNER_CAPTURE] + g_key_ledger_by[KEY_OWNER_VALUE]) {
        /* @ATOMOWNER THE BUDGET IS ADDED TO RATHER THAN CARVED OUT OF, so no message that fitted before this
           partition fits differently now. The worst case is arithmetic and is stated so the next person to
           lengthen this text can re-derive it: 809 bytes of format literal (the em dash is three of them),
           less 2 per specifier, plus a 63-byte atom name, 118 bytes of owner names, seven signed counts and two
           more numbers, plus __FILE__ — which is NOT `quickjs.c` but whatever absolute path the build handed
           the compiler, and under a frozen snapshot that is a lane-and-revision directory. At 1024 that
           overflowed by ~100 bytes and snprintf would have TRUNCATED IN SILENCE, taking the tail — which is
           the half that stops the next reader hunting the remedy this abort no longer names. The essential
           facts are composed FIRST for the same reason: a truncation must cost prose and never a number. */
        char abuf[ATOM_GET_STR_BUF_SIZE], why[2048];
        snprintf(why, sizeof(why),
                 "a define under a BORROWED key raised its atom's refcount by more than the one reference the "
                 "new property owns, the one the time-travel capture took and the one a value slot holding the "
                 "key's own string took: key `%s` (id %d) went %d -> %d across the define, from %s:%d. WHO TOOK "
                 "IT: %s %+d, %s %+d, %s %+d, %s %+d, %s %+d, %s %+d, and %+d through no instrumented door (a "
                 "JSValue dup of the key's own string outside a value slot; `val` %s that string). Read this "
                 "function's own comment "
                 "before hunting a caller, because a JS_DupAtom in the argument list is already inside the %d "
                 "and cannot be what fired this. The excess is leaked for the lifetime of the runtime and "
                 "would otherwise surface only as an `[atomleak]` line at JS_FreeRuntime, with the run already "
                 "over and every frame that could name this site gone",
                 JS_AtomGetStrRT(ctx->rt, abuf, sizeof(abuf), prop), (int)prop, rc0, rc1, file, line,
                 js_key_ledger_owner_name[KEY_OWNER_SHAPE_ADD], g_key_ledger_by[KEY_OWNER_SHAPE_ADD],
                 js_key_ledger_owner_name[KEY_OWNER_SHAPE_CLONE], g_key_ledger_by[KEY_OWNER_SHAPE_CLONE],
                 js_key_ledger_owner_name[KEY_OWNER_SHAPE_FREE], g_key_ledger_by[KEY_OWNER_SHAPE_FREE],
                 js_key_ledger_owner_name[KEY_OWNER_CAPTURE], g_key_ledger_by[KEY_OWNER_CAPTURE],
                 js_key_ledger_owner_name[KEY_OWNER_ATOM_API], g_key_ledger_by[KEY_OWNER_ATOM_API],
                 js_key_ledger_owner_name[KEY_OWNER_VALUE], g_key_ledger_by[KEY_OWNER_VALUE],
                 rest, val_is_key ? "IS" : "is NOT", rc0);
        DFAIL(why);
    }
    g_key_ledger_atom = outer_atom;
    g_key_ledger_in_capture = outer_in_capture;
    memcpy(g_key_ledger_by, outer_by, sizeof(outer_by));
#endif
    JS_FreeValue(ctx, val);
    return ret;
}
#define js_define_prop_borrowed_key(ctx, this_obj, prop, val, flags) \
    js_define_prop_borrowed_key_at((ctx), (this_obj), (prop), (val), (flags), __FILE__, __LINE__)

int JS_DefinePropertyValueValue(JSContext *ctx, JSValueConst this_obj,
                                JSValue prop, JSValue val, int flags)
{
    JSAtom atom;
    int ret;
    atom = JS_ValueToAtom(ctx, prop);
    JS_FreeValue(ctx, prop);
    if (unlikely(atom == JS_ATOM_NULL)) {
        JS_FreeValue(ctx, val);
        return -1;
    }
    ret = JS_DefinePropertyValue(ctx, this_obj, atom, val, flags);
    JS_FreeAtom(ctx, atom);
    return ret;
}

int JS_DefinePropertyValueUint32(JSContext *ctx, JSValueConst this_obj,
                                 uint32_t idx, JSValue val, int flags)
{
    return JS_DefinePropertyValueValue(ctx, this_obj, js_uint32(idx),
                                       val, flags);
}

int JS_DefinePropertyValueInt64(JSContext *ctx, JSValueConst this_obj,
                                int64_t idx, JSValue val, int flags)
{
    return JS_DefinePropertyValueValue(ctx, this_obj, js_int64(idx),
                                       val, flags);
}

/* `prop` may be pure ASCII or UTF-8 encoded */
int JS_DefinePropertyValueStr(JSContext *ctx, JSValueConst this_obj,
                              const char *prop, JSValue val, int flags)
{
    JSAtom atom;
    int ret;
    atom = JS_NewAtom(ctx, prop);
    if (atom == JS_ATOM_NULL) {
        JS_FreeValue(ctx, val);
        return -1;
    }
    ret = JS_DefinePropertyValue(ctx, this_obj, atom, val, flags);
    JS_FreeAtom(ctx, atom);
    return ret;
}

/* shortcut to add getter & setter */
int JS_DefinePropertyGetSet(JSContext *ctx, JSValueConst this_obj,
                            JSAtom prop, JSValue getter, JSValue setter,
                            int flags)
{
    int ret;
    ret = JS_DefineProperty(ctx, this_obj, prop, JS_UNDEFINED, getter, setter,
                            flags | JS_PROP_HAS_GET | JS_PROP_HAS_SET |
                            JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE);
    JS_FreeValue(ctx, getter);
    JS_FreeValue(ctx, setter);
    return ret;
}

static int JS_CreateDataPropertyUint32(JSContext *ctx, JSValueConst this_obj,
                                       int64_t idx, JSValue val, int flags)
{
    return JS_DefinePropertyValueValue(ctx, this_obj, js_int64(idx),
                                       val, flags | JS_PROP_CONFIGURABLE |
                                       JS_PROP_ENUMERABLE | JS_PROP_WRITABLE);
}

/* Like JS_DefinePropertyValueInt64 but borrows val (does not free it) */
static int JS_DefinePropertyValueInt64Const(JSContext *ctx, JSValueConst this_obj,
                                            int64_t idx, JSValueConst val, int flags)
{
    JSAtom atom;
    int ret;
    atom = JS_ValueToAtom(ctx, js_int64(idx));
    if (unlikely(atom == JS_ATOM_NULL))
        return -1;
    ret = JS_DefinePropertyValueConst(ctx, this_obj, atom, val, flags);
    JS_FreeAtom(ctx, atom);
    return ret;
}

/* Like JS_CreateDataPropertyUint32 but borrows val (does not free it) */
static int JS_CreateDataPropertyUint32Const(JSContext *ctx, JSValueConst this_obj,
                                            int64_t idx, JSValueConst val, int flags)
{
    return JS_DefinePropertyValueInt64Const(ctx, this_obj, idx, val,
                                            flags | JS_PROP_CONFIGURABLE |
                                            JS_PROP_ENUMERABLE | JS_PROP_WRITABLE);
}


/* return true if 'obj' has a non empty 'name' string */
static bool js_object_has_name(JSContext *ctx, JSValue obj)
{
    JSProperty *pr;
    JSShapeProperty *prs;
    JSValue val;
    JSString *p;

    prs = find_own_property(&pr, JS_VALUE_GET_OBJ(obj), JS_ATOM_name);
    if (!prs)
        return false;
    if ((prs->flags & JS_PROP_TMASK) != JS_PROP_NORMAL)
        return true;
    val = pr->u.value;
    if (JS_VALUE_GET_TAG(val) != JS_TAG_STRING)
        return true;
    p = JS_VALUE_GET_STRING(val);
    return (p->len != 0);
}

static int JS_DefineObjectName(JSContext *ctx, JSValue obj,
                               JSAtom name, int flags)
{
    if (name != JS_ATOM_NULL
    &&  JS_IsObject(obj)
    &&  !js_object_has_name(ctx, obj)
    &&  JS_DefinePropertyValue(ctx, obj, JS_ATOM_name, JS_AtomToString(ctx, name), flags) < 0) {
        return -1;
    }
    return 0;
}

static int JS_DefineObjectNameComputed(JSContext *ctx, JSValue obj,
                                       JSValue str, int flags)
{
    if (JS_IsObject(obj) &&
        !js_object_has_name(ctx, obj)) {
        JSAtom prop;
        JSValue name_str;
        prop = JS_ValueToAtom(ctx, str);
        if (prop == JS_ATOM_NULL)
            return -1;
        name_str = js_get_function_name(ctx, prop);
        JS_FreeAtom(ctx, prop);
        if (JS_IsException(name_str))
            return -1;
        if (JS_DefinePropertyValue(ctx, obj, JS_ATOM_name, name_str, flags) < 0)
            return -1;
    }
    return 0;
}

