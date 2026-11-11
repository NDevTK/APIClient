/* `prop` and `holder` are here ONLY so the abort has an address in it, and they are the write side's half of
   what the read side already carries. The old message was the whole of it — "call_setter ran a setter from C
   — route this write onto the tramp chain" — one line, identical at three call sites, over every property of
   every object in the engine: a correct spec fact and a correct instruction with nothing to apply either to.
   A reader who obeyed it had to read every C writer in the tree to find which write it meant.
   NAMED RESIDUAL — WHAT IS STILL MISSING IS THE C WRITER'S OWN file:line, AND THE NEXT DIFF IS THE ONE THAT
   BUILDS IT. The read path threads `file`/`line` from a macro over every public spelling of JS_GetProperty*
   (see the contract above JS_GetPropertyAt in quickjs.h); JS_SetProperty* has no such plumbing, so the write
   that reached here is identified by WHAT it wrote and WHOSE, and not by WHERE. That next diff is the same
   shape as the read side's: a JS_SetPropertyAt macro pair in quickjs.h and the parameter threaded down through
   JS_SetPropertyInternal2 to here. HOW ITS ABSENCE SHOWS: this abort fires with the property, both classes and
   both class strings filled in, and the js_why_backtrace tail bottoms out in the page's frames with no C
   file:line anywhere in the report — so the component is named and the line inside it is not. */
static int call_setter(JSContext *ctx, JSObject *setter,
                       JSValueConst this_obj, JSAtom prop, JSObject *holder, JSValue val, int flags)
{
    /* Read only by the assertion below, which a release build compiles out; the parameters stay in the
       signature there so ONE argument list serves both builds — the same shape as the read side's file/line. */
    (void)prop; (void)holder;
    if (likely(setter)) {
        /* THE SETTER RUNS PAGE CODE. Running it from here gives it a C activation with no flow base, so a loop
           in the setter body drives to completion; the write OPCODES route it onto the tramp chain as a 1-arg
           method call, the same way the read opcodes route a getter. The JS_CallFree that used to be here was
           measured over the whole corpus behind a DCHECK and never reached, so it is deleted rather than kept
           as a fallback for whatever the routing misses.
           The no-setter arm below is not a fallback at all, and its citation was WRONG in a way worth stating
           because a wrong number reads as authoritative: it said "10.1.9.2 step 4.e's TypeError", and
           §10.1.9.2 OrdinarySetWithOwnDescriptor ( obj, propertyKey, value, receiver, ownDesc ) has no step
           4.e and does not throw at all — its step 5 is "If setter is undefined, return false". The TypeError
           is §7.3.4 Set ( obj, propertyKey, value, throw ) step 2, "If success is false and throw is true,
           throw a TypeError exception", which is exactly what the JS_PROP_THROW test below is. It runs no user
           code and belongs exactly here. */
        JS_FreeValue(ctx, val);
#if APICLIENT_DEV
        {
            char pbuf[ATOM_GET_STR_BUF_SIZE], rbuf[ATOM_GET_STR_BUF_SIZE], hbuf[ATOM_GET_STR_BUF_SIZE];
            char rtag[128], htag[128], frames[2048], why[4096];
            JSObject *robj = JS_VALUE_GET_TAG(this_obj) == JS_TAG_OBJECT ? JS_VALUE_GET_OBJ(this_obj) : NULL;
            const char *rcls = "a primitive";

            DCHECK(prop != JS_ATOM_NULL,
                   "call_setter was handed no property atom — every call site has one in hand and passing it "
                   "is what stops this abort naming the whole engine at once");
            if (robj)
                rcls = JS_AtomGetStr(ctx, rbuf, sizeof(rbuf),
                                     ctx->rt->class_array[robj->class_id].class_name);
            js_why_backtrace(ctx, frames, sizeof frames);
            snprintf(why, sizeof why,
                     "A property WRITE reached a setter for `%s` on %s / %s (the accessor is held by %s / %s) "
                     "and it is being performed from C. §10.1.9.2 OrdinarySetWithOwnDescriptor ( obj, "
                     "propertyKey, value, receiver, ownDesc ) step 6 is `Perform ? Call(setter, receiver, "
                     "<< value >>)` — the page's "
                     "function, and a C activation has no flow base under it, so a loop in that body drives to "
                     "completion instead of parking. This is a capability this engine does not have rather "
                     "than a case to handle; the C-drives-JS body was DELETED, not left behind an assert. "
                     "ROUTE THE WRITE: the write opcodes hand the setter to the tramp chain as a 1-arg method "
                     "call, which is the same request Reflect.set issues. There is no C file:line in this "
                     "report because JS_SetProperty* does not yet carry one — see the residual above "
                     "call_setter. The page's call into this writer: %s",
                     JS_AtomGetStr(ctx, pbuf, sizeof(pbuf), prop), rcls,
                     js_why_class_string(ctx, robj, rtag, sizeof rtag),
                     holder ? JS_AtomGetStr(ctx, hbuf, sizeof(hbuf),
                                            ctx->rt->class_array[holder->class_id].class_name)
                            : "(no holder passed)",
                     js_why_class_string(ctx, holder, htag, sizeof htag),
                     frames);
            DFAIL(why);
        }
#endif
        JS_ThrowTypeError(ctx, "setter");
        return -1;
    } else {
        JS_FreeValue(ctx, val);
        if ((flags & JS_PROP_THROW) ||
            ((flags & JS_PROP_THROW_STRICT) && is_strict_mode(ctx))) {
            JS_ThrowTypeError(ctx, "no setter for property");
            return -1;
        }
        return false;
    }
}

/* THE DENSE PART, SHRUNK — the ONE place elements at or above an index are removed from a fast array, split out
   because a LENGTH write is not the only thing that asks for it. The other asker is the COW unapply, which
   removes a flow-created append by taking the dense count back down and must leave the length ALONE (the length
   is captured as the slot it is — see JS_ArrayTruncateDense). Keeping the two together made the length ride
   along with every element removal, which was the whole of that defect.
   `as_slot` is the same distinction it is everywhere else in this file: the OPERATION captures each removed
   index for the running flow's delta, the SLOT restore does not (it IS the delta, mid-context-switch). */
static void array_dense_truncate(JSContext *ctx, JSObject *p, uint32_t len, bool as_slot)
{
    uint32_t old_len = p->u.array.count;
    uint32_t i;

    DCHECK(p->fast_array, "a dense-part shrink on an array with no dense part");
    if (len >= old_len)
        return;
    for (i = len; i < old_len; i++) {
        /* forced-exec TIME-TRAVEL: A SHRINK REMOVES ELEMENTS, and removing one is a shared-state mutation
           exactly as writing one is. This free reaches neither JS_SetPropertyInternal2 nor
           set_fast_array_element, so `arr.length = 0` on a SHARED array captured the length slot at the caller
           and NOT one of the values it drops: the unapply put the length back and the elements stayed gone, for
           this flow and for every sibling. The SLOW-array arm of array_set_length_slot has always captured
           them, because it removes each index through delete_property, whose own capture is right there -- so
           this is the two arms of one operation agreeing rather than a new rule. Captured per index, since that
           is what a slot is. */
        if (!as_slot)
            cow_capture(ctx, JS_MKPTR(JS_TAG_OBJECT, p), __JS_AtomFromUInt32(i));
        JS_FreeValue(ctx, p->u.array.u.values[i]);
        p->u.array.u.values[i] = JS_UNDEFINED;
    }
    DCHECK(p->u.array.count == old_len,
           "the dense part moved while its removed elements were being captured — a capture may grow the "
           "host's delta and a growth may release a PARKED flow, which is why it may never revert the running "
           "flow's heap");
    p->u.array.count = len;
}

/* THE LENGTH SLOT, WRITTEN — 10.4.2.4 steps 15-17 with neither its coercion (steps 3-4) nor its read-only test
   (step 12). The elements at or above `len` are removed and prop[0] is set to the length actually REACHED.
   Split out of set_array_length for the write that is NOT the operation. JS_SetOwnSlotDesc restores a captured
   `length` during a context switch, and it must not consult `writable`, because the flow whose baseline is being
   put back is very often the flow that froze the array. A restore that consults the flow's own attributes is a
   restore that refuses, and a refusal here has no flow to throw on — see the read twin.
   `as_slot` carries that same distinction DOWN INTO THE SHRINK, which is where it had been dropped: the shrink
   removes indices through delete_property, and the OPERATION's delete refuses a non-configurable one, so a
   restore to the baseline length was blocked by exactly the define this flow had made above it. The slot form
   removes the index whatever its attributes, so the reached length IS `len`; the operation form still stops at
   the highest non-configurable index and 10.4.2.4 step 17.b's TypeError is the caller's. */
static uint32_t array_set_length_slot(JSContext *ctx, JSObject *p, uint32_t len, bool as_slot)
{
    uint32_t idx, cur_len;
    int i, ret;

    cur_len = len;
    if (likely(p->fast_array)) {
        array_dense_truncate(ctx, p, len, as_slot);
        p->prop[0].u.value = js_uint32(len);
    } else {
        /* Note: length is always a uint32 because the object is an
           array */
        JS_ToUint32(ctx, &cur_len, p->prop[0].u.value);
        if (len < cur_len) {
            uint32_t d;
            JSShape *sh;
            JSShapeProperty *pr;

            d = cur_len - len;
            sh = p->shape;
            if (d <= sh->prop_count) {
                JSAtom atom;

                /* faster to iterate */
                while (cur_len > len) {
                    atom = JS_NewAtomUInt32(ctx, cur_len - 1);
                    ret = delete_property(ctx, p, atom, as_slot);
                    JS_FreeAtom(ctx, atom);
                    if (unlikely(!ret)) {
                        /* unlikely case: property is not
                           configurable */
                        break;
                    }
                    cur_len--;
                }
            } else {
                /* faster to iterate thru all the properties. Need two
                   passes in case one of the property is not
                   configurable */
                cur_len = len;
                for(i = 0, pr = get_shape_prop(sh); i < sh->prop_count;
                    i++, pr++) {
                    if (pr->atom != JS_ATOM_NULL &&
                        JS_AtomIsArrayIndex(ctx, &idx, pr->atom)) {
                        if (idx >= cur_len && !as_slot &&
                            !(pr->flags & JS_PROP_CONFIGURABLE)) {
                            cur_len = idx + 1;
                        }
                    }
                }

                for(i = 0, pr = get_shape_prop(sh); i < sh->prop_count;
                    i++, pr++) {
                    if (pr->atom != JS_ATOM_NULL &&
                        JS_AtomIsArrayIndex(ctx, &idx, pr->atom)) {
                        if (idx >= cur_len) {
                            /* remove the property */
                            delete_property(ctx, p, pr->atom, as_slot);
                            /* WARNING: the shape may have been modified */
                            sh = p->shape;
                            pr = &get_shape_prop(sh)[i];
                        }
                    }
                }
            }
        } else {
            cur_len = len;
        }
        set_value(ctx, &p->prop[0].u.value, js_uint32(cur_len));
    }
    return cur_len;
}

/* set the array length and remove the array elements if necessary. */
static int set_array_length(JSContext *ctx, JSObject *p, JSValue val,
                            int flags)
{
    uint32_t len, cur_len;
    int ret;

    /* 10.4.2.4 steps 3-4 are ToUint32(V) and ToNumber(V) — BOTH on the original V, which is why
       JS_ToArrayLengthFree does the conversion twice and compares. For an OBJECT V that is the page's
       valueOf/@@toPrimitive TWICE, and a C activation has no flow to run it on: the write hands that case to
       CONT_ARRAY_LEN, which produces the validated uint32 and re-issues the operation with it. Reaching here
       with an object means a write path was not routed — and the DEFINE path's comment used to claim `val is
       guaranted to be a Uint32`, which `Object.defineProperty([1], "length", {value:{valueOf(){for(;;){}}}})`
       disproved. */
    DCHECK(JS_VALUE_GET_TAG(val) != JS_TAG_OBJECT,
           "an Array length write reached set_array_length with an OBJECT value — route it through CONT_ARRAY_LEN");
    /* Note: this call can reallocate the properties of 'p' */
    ret = JS_ToArrayLengthFree(ctx, &len, val, false);
    if (ret)
        return -1;
    /* JS_ToArrayLengthFree() must be done before the read-only test */
    if (unlikely(!(get_shape_prop(p->shape)[0].flags & JS_PROP_WRITABLE)))
        return JS_ThrowTypeErrorReadOnly(ctx, flags, JS_ATOM_length);

    cur_len = array_set_length_slot(ctx, p, len, /*as_slot*/false);
    if (unlikely(cur_len > len))
        return JS_ThrowTypeErrorOrFalse(ctx, flags, "not configurable");
    return true;
}

/* return -1 if exception */
static int expand_fast_array(JSContext *ctx, JSObject *p, uint32_t new_len)
{
    uint32_t old_size, new_size;
    size_t slack;
    JSValue *new_array_prop;

    if (unlikely(new_len > (uint32_t)INT32_MAX)) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }

    old_size = p->u.array.u1.size;
    new_size = old_size + old_size/2;
    if (new_size < old_size) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    new_size = max_uint32(new_len, new_size);
    new_array_prop = js_realloc2(ctx, p->u.array.u.values, sizeof(JSValue) * new_size, &slack);
    if (!new_array_prop)
        return -1;
    new_size += slack / sizeof(*new_array_prop);
    p->u.array.u.values = new_array_prop;
    p->u.array.u1.size = new_size;
    return 0;
}

/* THE DENSE PART GROWN BACK OVER A TRUNCATED TAIL — the inverse of array_set_length_slot's shrink, and its only
   caller is the one that has to undo one: JS_SetOwnSlotDesc putting back a baseline element that the running flow's
   `arr.length = n` removed. The slots between the current count and `idx` are holes here and are filled by the
   entries restored AFTER this one (a segment restores highest-index-first, so the run walks back down to the
   baseline length exactly as the appends walked up from it).
   NOT JS_CreateProperty's convert_to_array, which is what a [[Set]] of an out-of-range index does: a context
   switch that de-optimises a SHARED array into a slow one has changed the baseline every sibling resumes on,
   and §Time-travel's resume is byte-identical or it is a cap. The length is carried up with the count exactly as
   add_fast_array_element carries it, so the fast array's own invariant (length >= count) holds at every
   intermediate restore; the captured `length` entry then puts the baseline length back over it.
   Returns -1 with an exception pending on an allocation failure — the one caller turns that into a CHECK. */
static int array_grow_dense_to(JSContext *ctx, JSObject *p, uint32_t idx)
{
    uint32_t new_len = idx + 1;

    DCHECK(p->class_id == JS_CLASS_ARRAY && p->fast_array,
           "a dense-part grow on something that is not a fast Array — only an Array's length write shrinks a "
           "dense part, so only an Array has one to grow back");
    DCHECK(idx >= p->u.array.count, "a dense-part grow to an index the dense part already holds");
    if (new_len > p->u.array.u1.size && expand_fast_array(ctx, p, new_len))
        return -1;
    while (p->u.array.count < new_len)
        p->u.array.u.values[p->u.array.count++] = JS_UNDEFINED;
    if (JS_VALUE_GET_TAG(p->prop[0].u.value) == JS_TAG_INT &&
        (uint32_t)JS_VALUE_GET_INT(p->prop[0].u.value) < new_len)
        p->prop[0].u.value = js_uint32(new_len);
    return 0;
}

/* Preconditions: 'p' must be of class JS_CLASS_ARRAY, p->fast_array =
   true and p->extensible = true */
static int add_fast_array_element(JSContext *ctx, JSObject *p,
                                  JSValue val, int flags)
{
    uint32_t new_len, array_len;
    /* extend the array by one */
    /* XXX: convert to slow array if new_len > 2^31-1 elements */
    new_len = p->u.array.count + 1;
    /* Forced-exec COW: a fast-array APPEND writes values[] + length directly, bypassing the property-set capture.
       Record the created element slot (index == old count, existed=0) so a snapshot-forked sibling that SHARES
       this array (a map/filter/push accumulator) is isolated. cow_unapply removes it by TRUNCATING the array to
       that index (set_array_length), not JS_DeleteProperty. cow_capture skips a flow_local (freshly-built,
       unshared) array — a literal build pays only the cheap skip; only a genuinely shared array is captured.
       O(1) append capture (known-new slot: no dedup scan / baseline lookup). */
    cow_capture_append(ctx, JS_MKPTR(JS_TAG_OBJECT, p), __JS_AtomFromUInt32(p->u.array.count));
    /* update the length if necessary. We assume that if the length is
       not an integer, then if it >= 2^31.  */
    if (likely(JS_VALUE_GET_TAG(p->prop[0].u.value) == JS_TAG_INT)) {
        array_len = JS_VALUE_GET_INT(p->prop[0].u.value);
        if (new_len > array_len) {
            if (unlikely(!(get_shape_prop(p->shape)->flags & JS_PROP_WRITABLE))) {
                JS_FreeValue(ctx, val);
                return JS_ThrowTypeErrorReadOnly(ctx, flags, JS_ATOM_length);
            }
            p->prop[0].u.value = js_int32(new_len);
        }
    }
    if (unlikely(new_len > p->u.array.u1.size)) {
        if (expand_fast_array(ctx, p, new_len)) {
            JS_FreeValue(ctx, val);
            return -1;
        }
    }
    p->u.array.u.values[new_len - 1] = val;
    p->u.array.count = new_len;
    return true;
}

/* THE ONE IN-BOUNDS DENSE-ELEMENT STORE — the overwrite twin of add_fast_array_element above.
 *
 * A dense element lives in u.array.u.values and not in the shape, so a write to one reaches neither
 * JS_SetPropertyInternal2 nor add_property, and therefore neither COW capture. There were EIGHT such writes in
 * this file and exactly ONE of them captured — JS_SetPropertyValue's ARRAY arm, which is the one somebody
 * happened to look at. The other seven wrote straight into the slot: JS_SetPropertyValue's ARGUMENTS arm;
 * JS_DefineProperty's in-bounds arm, where every Object/Reflect.defineProperty on an existing index lands and
 * where JS_DefinePropertyValueUint32 takes every C caller; OP_put_array_el's overwrite fast path, which is
 * `a[i] = v` and so the commonest element write there is; OP_put_array_el's append fast path; copyWithin's
 * dense span; reverse's dense swap; and pop/shift's memmove. Each left one flow's mutation of a SHARED array
 * standing in the baseline for every sibling, and standing through the unapply whose whole job is to take it
 * back out — which is the isolation the whole scheduler rests on.
 *
 * The capture is therefore not a line each site remembers — the store IS this function. A dense element is
 * written in exactly three ways now: this one (in-bounds, on a possibly-shared object), add_fast_array_element
 * (the append, captured as a created slot), and arr_dense_put (a result array the engine allocated and no page
 * can see, which asserts that privacy). js_get_fast_array hands out a `const` pointer so a fourth cannot be
 * written without saying so. Consumes `val`. */
static void set_fast_array_element(JSContext *ctx, JSObject *p, uint32_t idx, JSValue val)
{
    DCHECK(p->class_id == JS_CLASS_ARRAY || p->class_id == JS_CLASS_ARGUMENTS,
           "a dense-element store on a class whose u.array holds something that is not a JSValue array");
    DCHECK(p->fast_array, "a dense-element store on an array already converted to slow properties");
    DCHECK(idx < p->u.array.count, "a dense-element store outside the array's dense part");
    cow_capture(ctx, JS_MKPTR(JS_TAG_OBJECT, p), __JS_AtomFromUInt32(idx));
    set_value(ctx, &p->u.array.u.values[idx], val);
}

/* Allocate a new fast array initialized to JS_UNDEFINED. Its maximum
   size is 2^31-1 elements. For convenience, 'len' is a 64 bit
   integer. */
static JSValue js_allocate_fast_array(JSContext *ctx, int64_t len)
{
    JSValue arr;
    JSObject *p;
    int i;

    if (len > INT32_MAX)
        return JS_ThrowRangeError(ctx, "invalid array length");
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    if (len > 0) {
        p = JS_VALUE_GET_OBJ(arr);
        if (expand_fast_array(ctx, p, len) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        p->u.array.count = len;
        for(i = 0; i < len; i++)
            p->u.array.u.values[i] = JS_UNDEFINED;
        /* update the 'length' field */
        set_value(ctx, &p->prop[0].u.value, js_int32(len));
    }
    return arr;
}

/* `!` CreateDataPropertyOrThrow(A, ! ToString(𝔽(i)), v) where A is the dense base Array a change-array-by-copy
   method allocated for its result. A is engine-private and never consulted Array[@@species], so this reaches no
   accessor, no Proxy trap and no prototype lookup, and cannot fail — which is exactly what the spec's `!`
   asserts. It is the same computation as the keyed-write step with no observable step, not a way around one.
   Consumes v. */
static void arr_dense_put(JSContext *ctx, JSValueConst arr, int64_t i, JSValue v)
{
    JSObject *p = JS_VALUE_GET_OBJ(arr);
    DCHECK(JS_VALUE_GET_TAG(arr) == JS_TAG_OBJECT && p->class_id == JS_CLASS_ARRAY && p->fast_array,
           "the private result of a change-array-by-copy method left its dense shape");
    DCHECK(i >= 0 && i < p->u.array.count,
           "a change-array-by-copy store landed outside the result it allocated");
    JS_FreeValue(ctx, p->u.array.u.values[i]);
    p->u.array.u.values[i] = v;
}

static void js_free_desc(JSContext *ctx, JSPropertyDescriptor *desc)
{
    JS_FreeValue(ctx, desc->getter);
    JS_FreeValue(ctx, desc->setter);
    JS_FreeValue(ctx, desc->value);
}

/* return -1 in case of exception or true or false. Warning: 'val' is
   freed by the function. 'flags' is a bitmask of JS_PROP_NO_ADD,
   JS_PROP_THROW or JS_PROP_THROW_STRICT. If JS_PROP_NO_ADD is set,
   the new property is not added and an error is raised.
   'obj' must be an object when obj != this_obj.

   `recv_pending` is how a caller takes back 10.1.9.2 step 3 — the completion performed on the RECEIVER, whose
   two internal methods are the page's `getOwnPropertyDescriptor` and `defineProperty` traps when the receiver is
   a Proxy, and a step when its class answers the key by one. Set it to true and this returns having done nothing
   but the walk; the caller owns the completion and has somewhere to run it (the keyed-operation entry, as
   CONT_SET_RECV). Passing NULL asserts the receiver is neither, and the DCHECK at the tail holds every caller to
   that.
   */
static bool tramp_step_exotic(JSContext *ctx, JSObject *p, JSAtom atom);
static int JS_SetPropertyInternal2(JSContext *ctx, JSValueConst obj, JSAtom prop,
                                   JSValue val, JSValueConst this_obj, int flags,
                                   bool *recv_pending)
{
    JSObject *p, *p1;
    JSShapeProperty *prs;
    JSProperty *pr;
    JSPropertyDescriptor desc;
    int desc_flags;
    int ret;

    DCHECK(!g_compile_stint,
           "a [[Set]] ran inside a compile stint — a compile writes no state of any world; a parse product's own "
           "slots are defined, never set through the prototype chain");
    cow_capture(ctx, obj, prop);   /* forced-exec: record the baseline object's pre-write state for per-flow revert */
    if (unlikely(g_concolic.unit_seq != NULL) && JS_VALUE_GET_PTR(obj) == JS_VALUE_GET_PTR(this_obj))
        js_record_write_note(ctx, obj, prop);

    switch(JS_VALUE_GET_TAG(this_obj)) {
    case JS_TAG_NULL:
    case JS_TAG_UNDEFINED:
        /* A nullish RECEIVER is not an error. OrdinarySetWithOwnDescriptor step 2.b returns FALSE for any
           Receiver that is not an Object, and its accessor arm calls the setter with that this-value — the same
           treatment every other primitive receiver gets below, which is why this shares that arm rather than
           having one of its own. `Reflect.set({}, "x", 0, undefined)` is false, not a throw.
           The TypeError is PutValue's step 5.a ToObject(BASE), a different operation, and it is reached only
           when the base itself is the nullish value. */
        if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
            p = NULL;
            p1 = JS_VALUE_GET_OBJ(obj);
            goto prototype_lookup;
        }
        DCHECK(js_same_value(ctx, obj, this_obj),
               "a nullish receiver over a primitive base is neither PutValue's error nor OrdinarySet's false");
        if (JS_IsNull(this_obj))
            JS_ThrowTypeErrorAtom(ctx, "cannot set property '%s' of null", prop);
        else
            JS_ThrowTypeErrorAtom(ctx, "cannot set property '%s' of undefined", prop);
        goto fail;
    case JS_TAG_OBJECT:
        p = JS_VALUE_GET_OBJ(this_obj);
        p1 = JS_VALUE_GET_OBJ(obj);
        if (p == p1)
            break;
        goto retry2;
    default:
        if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
            obj = JS_GetPrototypePrimitive(ctx, obj);
        p = NULL;
        p1 = JS_VALUE_GET_OBJ(obj);
        goto prototype_lookup;
    }

retry:
    prs = find_own_property(&pr, p1, prop);
    if (prs) {
        if (likely((prs->flags & (JS_PROP_TMASK | JS_PROP_WRITABLE |
                                  JS_PROP_LENGTH)) == JS_PROP_WRITABLE)) {
            /* fast case */
            set_value(ctx, &pr->u.value, val);
            return true;
        } else if (prs->flags & JS_PROP_LENGTH) {
            DCHECK(p->class_id == JS_CLASS_ARRAY, "p->class_id == JS_CLASS_ARRAY");
            DCHECK(prop == JS_ATOM_length, "prop == JS_ATOM_length");
            return set_array_length(ctx, p, val, flags);
        } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
            return call_setter(ctx, pr->u.getset.setter, this_obj, prop, p1, val, flags);
        } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF) {
            /* JS_PROP_WRITABLE is always true for variable
               references, but they are write protected in module name
               spaces. */
            if (p->class_id == JS_CLASS_MODULE_NS)
                goto read_only_prop;
            set_value(ctx, pr->u.var_ref->pvalue, val);
            return true;
        } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
            /* Instantiate property and retry (potentially useless) */
            if (JS_AutoInitProperty(ctx, p, prop, pr, prs))
                goto fail;
            goto retry;
        } else {
            goto read_only_prop;
        }
    }

    for(;;) {
        if (p1->is_exotic) {
            if (p1->fast_array) {
                if (__JS_AtomIsTaggedInt(prop)) {
                    uint32_t idx = __JS_AtomToUInt32(prop);
                    if (idx < p1->u.array.count) {
                        if (unlikely(p == p1))
                            return JS_SetPropertyValue(ctx, this_obj, js_int32(idx), val, flags);
                        else
                            break;
                    } else if (is_typed_array(p1->class_id)) {
                        goto typed_array_oob;
                    }
                } else if (is_typed_array(p1->class_id)) {
                    ret = JS_AtomIsNumericIndex(ctx, prop);
                    if (ret != 0) {
                        if (ret < 0)
                            goto fail;
                    typed_array_oob:
                        /* 10.4.5.5 [[Set]](P, V, Receiver) with a canonical numeric index that is NOT a valid
                           index. Step 1 (O and Receiver are the SAME object) runs TypedArraySetElement, which
                           coerces V and then discards it. Step 2 (a DIFFERENT receiver) returns true outright
                           — V is never read, so coercing it here fabricated an observable side effect and, for
                           a plain receiver, a property the spec never creates. */
                        if (p != p1) {
                            JS_FreeValue(ctx, val);
                            return true;
                        }
                        // per spec: evaluate value for side effects
                        if (p1->class_id == JS_CLASS_BIG_INT64_ARRAY ||
                            p1->class_id == JS_CLASS_BIG_UINT64_ARRAY) {
                            int64_t v;
                            if (JS_ToBigInt64Free(ctx, &v, val))
                                return -1;
                        } else {
                            val = JS_ToNumberFree(ctx, val);
                            JS_FreeValue(ctx, val);
                            if (JS_IsException(val))
                                return -1;
                        }
                        return true;
                    }
                }
            } else {
                const JSClassExoticMethods *em = ctx->rt->class_array[p1->class_id].exotic;
                if (em) {
                    JSValue obj1;
                    if (em->set_property) {
                        /* set_property can free the prototype */
                        obj1 = js_dup(JS_MKPTR(JS_TAG_OBJECT, p1));
                        ret = em->set_property(ctx, obj1, prop,
                                               val, this_obj, flags);
                        JS_FreeValue(ctx, obj1);
                        JS_FreeValue(ctx, val);
                        return ret;
                    }
                    if (em->get_own_property && !em->set_ignores_named_properties) {
                        /* get_own_property can free the prototype */
                        obj1 = js_dup(JS_MKPTR(JS_TAG_OBJECT, p1));
                        ret = em->get_own_property(ctx, &desc,
                                                   obj1, prop);
                        JS_FreeValue(ctx, obj1);
                        if (ret < 0)
                            goto fail;
                        if (ret) {
                            if (desc.flags & JS_PROP_GETSET) {
                                JSObject *setter;
                                if (JS_IsUndefined(desc.setter))
                                    setter = NULL;
                                else
                                    setter = JS_VALUE_GET_OBJ(desc.setter);
                                ret = call_setter(ctx, setter, this_obj, prop, p1, val, flags);
                                JS_FreeValue(ctx, desc.getter);
                                JS_FreeValue(ctx, desc.setter);
                                return ret;
                            } else {
                                JS_FreeValue(ctx, desc.value);
                                if (!(desc.flags & JS_PROP_WRITABLE))
                                    goto read_only_prop;
                                if (likely(p == p1)) {
                                    ret = JS_DefineProperty(ctx, this_obj, prop, val,
                                                            JS_UNDEFINED, JS_UNDEFINED,
                                                            JS_PROP_HAS_VALUE);
                                    JS_FreeValue(ctx, val);
                                    return ret;
                                } else {
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        p1 = js_walk_proto(ctx, p1);
    prototype_lookup:
        if (!p1)
            break;

    retry2:
        prs = find_own_property(&pr, p1, prop);
        if (prs) {
            if ((prs->flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
                return call_setter(ctx, pr->u.getset.setter, this_obj, prop, p1, val, flags);
            } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
                /* Instantiate property and retry (potentially useless) */
                if (JS_AutoInitProperty(ctx, p1, prop, pr, prs))
                    return -1;
                goto retry2;
            } else if (!(prs->flags & JS_PROP_WRITABLE)) {
                goto read_only_prop;
            } else {
                break;
            }
        }
    }

    if (unlikely(flags & JS_PROP_NO_ADD)) {
        JS_ThrowReferenceErrorNotDefined(ctx, prop);
        goto fail;
    }

    if (unlikely(!p)) {
        ret = JS_ThrowTypeErrorOrFalse(ctx, flags, "not an object");
        goto done;
    }

    if (p == JS_VALUE_GET_OBJ(obj)) {
        if (unlikely(!p->extensible)) {
            ret = JS_ThrowTypeErrorOrFalse(ctx, flags, "object is not extensible");
            goto done;
        }
        if (p->is_exotic) {
            if (p->class_id == JS_CLASS_ARRAY && p->fast_array &&
                __JS_AtomIsTaggedInt(prop)) {
                uint32_t idx = __JS_AtomToUInt32(prop);
                if (idx == p->u.array.count) {
                    /* fast case */
                    return add_fast_array_element(ctx, p, val, flags);
                }
            }
            goto generic_create_prop;
        } else {
            pr = add_property(ctx, p, prop, JS_PROP_C_W_E);
            if (!pr)
                goto fail;
            pr->u.value = val;
            return true;
        }
    }

    // TODO(bnoordhuis) return JSProperty slot and update in place
    // when plain property (not is_exotic/setter/etc.) to avoid
    // calling find_own_property() thrice?
    /* 10.1.9.2 STEP 3, on the Receiver. A Proxy receiver answers both of its internal methods with a trap, which
       is the page's code, and a receiver whose class answers the key by a step (tramp_step_exotic) rests or forks;
       neither runs in this C activation, so the caller that has a chain takes the completion instead. Every
       other receiver class answers out of its own slots. */
    if (unlikely(p->class_id == JS_CLASS_PROXY || tramp_step_exotic(ctx, p, prop))) {
        DCHECK(recv_pending,
               "a [[Set]] completed step 3 from C on a Proxy receiver or one whose [[GetOwnProperty]] answers by a "
               "step: route that call site through CONT_SET_RECV");
        *recv_pending = true;
        JS_FreeValue(ctx, val);
        return false;   /* not the answer: the caller performs step 3 and produces one */
    }
    ret = JS_GetOwnPropertyFlagsInternal(ctx, &desc_flags, p, prop);
    if (ret < 0)
        goto fail;

    if (ret) {
        if (desc_flags & JS_PROP_GETSET) {
            ret = JS_ThrowTypeErrorOrFalse(ctx, flags, "setter is forbidden");
            goto done;
        } else if (!(desc_flags & JS_PROP_WRITABLE) ||
                   p->class_id == JS_CLASS_MODULE_NS) {
        read_only_prop:
            ret = JS_ThrowTypeErrorReadOnly(ctx, flags, prop);
            goto done;
        }
        ret = JS_DefineProperty(ctx, this_obj, prop, val,
                                JS_UNDEFINED, JS_UNDEFINED,
                                JS_PROP_HAS_VALUE);
    } else {
        if (unlikely(!p->extensible)) {
            ret = JS_ThrowTypeErrorOrFalse(ctx, flags, "object is not extensible");
            goto done;
        }
    generic_create_prop:
        ret = JS_CreateProperty(ctx, p, prop, val, JS_UNDEFINED, JS_UNDEFINED,
                                flags |
                                JS_PROP_HAS_VALUE |
                                JS_PROP_HAS_ENUMERABLE |
                                JS_PROP_HAS_WRITABLE |
                                JS_PROP_HAS_CONFIGURABLE |
                                JS_PROP_C_W_E);
    }

done:
    JS_FreeValue(ctx, val);
    return ret;
fail:
    JS_FreeValue(ctx, val);
    return -1;
}

static int JS_SetPropertyInternal(JSContext *ctx, JSValueConst obj, JSAtom prop,
                                  JSValue val, int flags)
{
    /* obj IS the receiver, so 10.1.9.2 step 3's `Receiver is O` branch runs and the receiver completion is
       never reached: nothing here can be a Proxy's trap. */
    return JS_SetPropertyInternal2(ctx, obj, prop, val, obj, flags, NULL);
}

int JS_SetProperty(JSContext *ctx, JSValueConst this_obj, JSAtom prop, JSValue val)
{
    return JS_SetPropertyInternal(ctx, this_obj, prop, val, JS_PROP_THROW);
}

/* flags can be JS_PROP_THROW or JS_PROP_THROW_STRICT */
static int JS_SetPropertyValue(JSContext *ctx, JSValueConst this_obj,
                               JSValue prop, JSValue val, int flags)
{
    /* 10.4.5.18 TypedArraySetElement ( obj, index, value ) OVER AN UNKNOWN INDEX — the OTHER of the two
       values that algorithm reads from the page, and the one that ABORTED until now.
       WHAT THE ENGINE DID WITH ONE BEFORE WAS WRONG RATHER THAN INCOMPLETE, and that is why this was a crash
       and not a residual. A concolic is a real JSObject, so it is no tagged int and took the slow path, where
       JSConcolicHooks.key_name converts it — and key_name's contract is that an unknown key DENOTES A REAL
       STRING, its own shape, which is "what makes `o[x] = 1` then `o[x]` find one slot". That is correct and
       is written for an ORDINARY OBJECT. On a TYPED ARRAY it took an arm no world the unknown denotes would
       take: 10.4.5.5 [[Set]] routes a key to TypedArraySetElement only when it is a CANONICAL NUMERIC INDEX,
       and a shape like `{state}.len` is not one, so the write landed as a named property and NOT ONE BYTE WAS
       STORED — while every integer the unknown could actually be IS a canonical numeric index. The serializer's
       buffer was silently short, the request was still built, and solver/endpoint.c recorded it under EPB_SENT
       as bytes the page sent, which a reviewer REPLAYS.
       THE ANSWER IS A SPAN WITH NO OFFSET, AND IT NEEDS NO NEW MECHANISM. The record the VALUE arm keeps
       already separates the BYTES from the FACT, so a write whose offset is unknown is a fact with no offset:
       nothing is written to the block, because there is no offset to write at, and the span is the whole of
       what the store leaves behind. Every read inside the span's range then answers UNKNOWN — see
       js_ab_span_get for why answering the block's stale byte is the one error that may never be made, and why
       forking over "does this span cover offset n" is refused for exactly the reason the write-side
       per-position fork was.
       THE NAMED PROPERTY IS NOT WRITTEN EITHER, which is the half that makes this a fix rather than an
       addition: leaving it would keep the wrong arm alive beside the right one, and a `buf[x]` read would then
       find a slot whose value no byte-consumer can see. `prop` and `val` are consumed here exactly as the slow
       path would have consumed them.
       THE DOMAIN IS THE WHOLE BUFFER AND THAT IS AN ENGINE/HOST CONTRACT LIMIT, NOT A CHOICE — see
       JSArrayBufferSpan. The right extent is the offset unknown's own constraint domain, which
       solver/concolic.c's concolic_bound_read holds, and the engine reaches the solver only through
       JSConcolicHooks, whose twenty members carry no domain accessor. Whole-buffer is the sound degradation:
       over-general, never a cap, and it narrows the day that hook exists with no other line here changing. */
    if (unlikely(JS_VALUE_GET_TAG(this_obj) == JS_TAG_OBJECT && js_value_is_concolic(prop)
                 && is_typed_array(JS_VALUE_GET_OBJ(this_obj)->class_id))) {
        JSObject *tap = JS_VALUE_GET_OBJ(this_obj);

        /* THE VALUE'S OWN PROVENANCE IS KEPT WHERE IT HAS ONE. `buf[x] = y` with both unknown must answer a
           later read with the VALUE's identity, because that is the source an @S candidate is injected at; a
           concrete `y` leaves nothing to carry, and the read derives from the unknown OFFSET instead, which is
           the honest statement that what is unknown about the element is WHICH BYTE IT IS. */
        js_ab_span_set_any(ctx, tap->u.typed_array->buffer, prop,
                           js_value_is_concolic(val) ? val : JS_UNINITIALIZED);
        JS_FreeValue(ctx, prop);
        JS_FreeValue(ctx, val);
        /* 10.4.5.5 [[Set]] returns true and 10.4.5.3 step 1.g returns true: a store the spec skips still
           SUCCEEDS, and here the store did not happen for a reason the spec has no step for. */
        return true;
    }
    if (likely(JS_VALUE_GET_TAG(this_obj) == JS_TAG_OBJECT &&
               JS_VALUE_GET_TAG(prop) == JS_TAG_INT)) {
        JSObject *p;
        uint32_t idx;
        double d;
        int32_t v;

        /* fast path for array access */
        p = JS_VALUE_GET_OBJ(this_obj);
        idx = JS_VALUE_GET_INT(prop);
        /* `ta[i] = v` — the ten typed-array arms below all end in a raw store through u.array.u.<T>_ptr, so not
           one of them reached a COW capture and a flow's write to a SHARED buffer stood in the baseline for
           every sibling. ONE capture at the operation and not a line each arm remembers, for the reason
           set_fast_array_element captures rather than its seven writers; and BEFORE the coercions, because a
           capture whose store never lands puts back bytes that never changed while a coercion that runs the
           page's valueOf can reach a store this has not yet seen. */
        if (is_typed_array(p->class_id)) {
            if (unlikely(!JS_IsUninitialized(p->u.typed_array->offset_unknown)))
                return js_ta_set_at_unknown_offset(ctx, p, idx, val);
            cow_capture_bytes(ctx, p);
        }
        switch(p->class_id) {
        case JS_CLASS_ARRAY:
            if (unlikely(idx >= (uint32_t)p->u.array.count)) {
                /* fast path to add an element to the array */
                if (unlikely(idx != (uint32_t)p->u.array.count ||
                             !p->fast_array ||
                             !p->extensible ||
                             p->shape->proto != JS_VALUE_GET_OBJ(ctx->class_proto[JS_CLASS_ARRAY]) ||
                             !ctx->std_array_prototype)) {
                    goto slow_path;
                }
                /* add element */
                return add_fast_array_element(ctx, p, val, flags);
            }
            set_fast_array_element(ctx, p, idx, val);
            break;
        case JS_CLASS_ARGUMENTS:
            if (unlikely(idx >= (uint32_t)p->u.array.count))
                goto slow_path;
            set_fast_array_element(ctx, p, idx, val);
            break;
        case JS_CLASS_MAPPED_ARGUMENTS:
            if (unlikely(idx >= (uint32_t)p->u.array.count))
                goto slow_path;
            /* a MAPPED arguments element IS the function's local slot, so its storage is a closure CELL and the
               capture is the cell's, not the element's — the one dense write in this switch that
               set_fast_array_element must not take. Without it a flow that assigns `arguments[0]` mutates the
               shared cell for every sibling, exactly as an uncaptured element write does. */
            cow_capture_vr(ctx, p->u.array.u.var_refs[idx]);
            set_value(ctx, p->u.array.u.var_refs[idx]->pvalue, val);
            break;
        case JS_CLASS_UINT8C_ARRAY:
            if (JS_ToUint8ClampFree(ctx, &v, val))
                goto ta_cvt_fail;
            if (typed_array_is_immutable(p))
                goto ta_immutable;
            /* Note: the conversion can detach the typed array, so the
               array bound check must be done after */
            if (unlikely(idx >= (uint32_t)p->u.array.count))
                goto ta_out_of_bound;
            p->u.array.u.uint8_ptr[idx] = v;
            break;
        case JS_CLASS_INT8_ARRAY:
        case JS_CLASS_UINT8_ARRAY:
            if (JS_ToInt32Free(ctx, &v, val))
                goto ta_cvt_fail;
            if (typed_array_is_immutable(p))
                goto ta_immutable;
            if (unlikely(idx >= (uint32_t)p->u.array.count))
                goto ta_out_of_bound;
            p->u.array.u.uint8_ptr[idx] = v;
            break;
        case JS_CLASS_INT16_ARRAY:
        case JS_CLASS_UINT16_ARRAY:
            if (JS_ToInt32Free(ctx, &v, val))
                goto ta_cvt_fail;
            if (typed_array_is_immutable(p))
                goto ta_immutable;
            if (unlikely(idx >= (uint32_t)p->u.array.count))
                goto ta_out_of_bound;
            p->u.array.u.uint16_ptr[idx] = v;
            break;
        case JS_CLASS_INT32_ARRAY:
        case JS_CLASS_UINT32_ARRAY:
            if (JS_ToInt32Free(ctx, &v, val))
                goto ta_cvt_fail;
            if (typed_array_is_immutable(p))
                goto ta_immutable;
            if (unlikely(idx >= (uint32_t)p->u.array.count))
                goto ta_out_of_bound;
            p->u.array.u.uint32_ptr[idx] = v;
            break;
        case JS_CLASS_BIG_INT64_ARRAY:
        case JS_CLASS_BIG_UINT64_ARRAY:
            /* XXX: need specific conversion function */
            {
                int64_t v;
                if (JS_ToBigInt64Free(ctx, &v, val))
                    goto ta_cvt_fail;
                if (typed_array_is_immutable(p))
                    goto ta_immutable;
                if (unlikely(idx >= (uint32_t)p->u.array.count))
                    goto ta_out_of_bound;
                p->u.array.u.uint64_ptr[idx] = v;
            }
            break;
        case JS_CLASS_FLOAT16_ARRAY:
            if (JS_ToFloat64Free(ctx, &d, val))
                goto ta_cvt_fail;
            if (typed_array_is_immutable(p))
                goto ta_immutable;
            if (unlikely(idx >= (uint32_t)p->u.array.count))
                goto ta_out_of_bound;
            p->u.array.u.fp16_ptr[idx] = tofp16(d);
            break;
        case JS_CLASS_FLOAT32_ARRAY:
            if (JS_ToFloat64Free(ctx, &d, val))
                goto ta_cvt_fail;
            if (typed_array_is_immutable(p))
                goto ta_immutable;
            if (unlikely(idx >= (uint32_t)p->u.array.count))
                goto ta_out_of_bound;
            p->u.array.u.float_ptr[idx] = d;
            break;
        case JS_CLASS_FLOAT64_ARRAY:
            if (JS_ToFloat64Free(ctx, &d, val)) {
            ta_cvt_fail:
                if (flags & JS_PROP_REFLECT_DEFINE_PROPERTY) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    return false;
                }
                return -1;
            }
            if (typed_array_is_immutable(p)) {
            ta_immutable:
                return false;
            }
            if (unlikely(idx >= (uint32_t)p->u.array.count)) {
            ta_out_of_bound:
                if (typed_array_is_oob(p))
                    if (flags & JS_PROP_DEFINE_PROPERTY)
                        return JS_ThrowTypeErrorOrFalse(ctx, flags, "out-of-bound numeric index");
                return true; // per spec: no OOB exception
            }
            p->u.array.u.double_ptr[idx] = d;
            break;
        default:
            goto slow_path;
        }
        return true;
    } else {
        JSAtom atom;
        int ret;
    slow_path:
        atom = JS_ValueToAtom(ctx, prop);
        JS_FreeValue(ctx, prop);
        if (unlikely(atom == JS_ATOM_NULL)) {
            JS_FreeValue(ctx, val);
            return -1;
        }
        ret = JS_SetPropertyInternal(ctx, this_obj, atom, val, flags);
        JS_FreeAtom(ctx, atom);
        return ret;
    }
}

int JS_SetPropertyUint32(JSContext *ctx, JSValueConst this_obj,
                         uint32_t idx, JSValue val)
{
    return JS_SetPropertyValue(ctx, this_obj, js_uint32(idx), val,
                               JS_PROP_THROW);
}

int JS_SetPropertyInt64(JSContext *ctx, JSValueConst this_obj,
                        int64_t idx, JSValue val)
{
    JSAtom prop;
    int res;

    if ((uint64_t)idx <= INT32_MAX) {
        /* fast path for fast arrays */
        return JS_SetPropertyValue(ctx, this_obj, js_int32(idx), val,
                                   JS_PROP_THROW);
    }
    prop = JS_NewAtomInt64(ctx, idx);
    if (prop == JS_ATOM_NULL) {
        JS_FreeValue(ctx, val);
        return -1;
    }
    res = JS_SetProperty(ctx, this_obj, prop, val);
    JS_FreeAtom(ctx, prop);
    return res;
}

/* `prop` may be pure ASCII or UTF-8 encoded */
int JS_SetPropertyStr(JSContext *ctx, JSValueConst this_obj,
                      const char *prop, JSValue val)
{
    JSAtom atom;
    int ret;
    atom = JS_NewAtom(ctx, prop);
    if (atom == JS_ATOM_NULL) {
        JS_FreeValue(ctx, val);
        return -1;
    }
    ret = JS_SetPropertyInternal(ctx, this_obj, atom, val, JS_PROP_THROW);
    JS_FreeAtom(ctx, atom);
    return ret;
}

