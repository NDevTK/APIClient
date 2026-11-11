/* The element as the array or the data block holds it, consulting no span: the switch of
   js_get_fast_array_element below, and the example an IMAGE span's element read derives from. */
static bool js_get_fast_array_element_concrete(JSContext *ctx, JSObject *p, uint32_t idx, JSValue *pval)
{
    switch(p->class_id) {
    case JS_CLASS_ARRAY:
    case JS_CLASS_ARGUMENTS:
        if (unlikely(idx >= p->u.array.count)) return false;
        *pval = js_dup(p->u.array.u.values[idx]);
        return true;
    case JS_CLASS_MAPPED_ARGUMENTS:
        if (unlikely(idx >= p->u.array.count)) return false;
        *pval = js_dup(*p->u.array.u.var_refs[idx]->pvalue);
        return true;
    case JS_CLASS_INT8_ARRAY:
        if (unlikely(idx >= p->u.array.count)) return false;
        *pval = js_int32(p->u.array.u.int8_ptr[idx]);
        return true;
    case JS_CLASS_UINT8C_ARRAY:
    case JS_CLASS_UINT8_ARRAY:
        if (unlikely(idx >= p->u.array.count)) return false;
        *pval = js_int32(p->u.array.u.uint8_ptr[idx]);
        return true;
    case JS_CLASS_INT16_ARRAY:
        if (unlikely(idx >= p->u.array.count)) return false;
        *pval = js_int32(p->u.array.u.int16_ptr[idx]);
        return true;
    case JS_CLASS_UINT16_ARRAY:
        if (unlikely(idx >= p->u.array.count)) return false;
        *pval = js_int32(p->u.array.u.uint16_ptr[idx]);
        return true;
    case JS_CLASS_INT32_ARRAY:
        if (unlikely(idx >= p->u.array.count)) return false;
        *pval = js_int32(p->u.array.u.int32_ptr[idx]);
        return true;
    case JS_CLASS_UINT32_ARRAY:
        if (unlikely(idx >= p->u.array.count)) return false;
        *pval = js_uint32(p->u.array.u.uint32_ptr[idx]);
        return true;
    case JS_CLASS_BIG_INT64_ARRAY:
        if (unlikely(idx >= p->u.array.count)) return false;
        *pval = JS_NewBigInt64(ctx, p->u.array.u.int64_ptr[idx]);
        return true;
    case JS_CLASS_BIG_UINT64_ARRAY:
        if (unlikely(idx >= p->u.array.count)) return false;
        *pval = JS_NewBigUint64(ctx, p->u.array.u.uint64_ptr[idx]);
        return true;
    case JS_CLASS_FLOAT16_ARRAY:
        if (unlikely(idx >= p->u.array.count)) return false;
        *pval = js_float64(fromfp16(p->u.array.u.fp16_ptr[idx]));
        return true;
    case JS_CLASS_FLOAT32_ARRAY:
        if (unlikely(idx >= p->u.array.count)) return false;
        *pval = js_float64(p->u.array.u.float_ptr[idx]);
        return true;
    case JS_CLASS_FLOAT64_ARRAY:
        if (unlikely(idx >= p->u.array.count)) return false;
        *pval = js_float64(p->u.array.u.double_ptr[idx]);
        return true;
    default:
        return false;
    }
}

/* §10.4.5.17 TypedArrayGetElement ( O, index ) at an index past the view's stored count: undefined, unless the
   view's length is an unknown (JSTypedArray.length_unknown), where IsValidIntegerIndex holds in the worlds the
   length is longer. The answer is then an unknown derived from the buffer's unknown (over stated bytes, the
   view's length) at that position, with undefined, the example world's answer, as its example. */
static JSValue js_ta_get_past_example(JSContext *ctx, JSObject *p, uint32_t idx)
{
    JSTypedArray *ta = p->u.typed_array;
    JSArrayBuffer *abuf = ta->buffer->u.array_buffer;
    int size_log2 = typed_array_size_log2(p->class_id);
    JSValue ops[3], r;

    if (JS_IsUninitialized(ta->length_unknown) || typed_array_is_oob(p))
        return JS_UNDEFINED;
    ops[0] = JS_IsUninitialized(abuf->length_unknown) ? ta->length_unknown : abuf->length_unknown;
    ops[1] = js_ta_byte_index(ctx, ta, (int64_t)idx << size_log2);
    ops[2] = js_int32(1 << size_log2);
    r = g_concolic.derived(ctx, "10.4.5.17 TypedArrayGetElement past the example", ops, 3, JS_UNDEFINED);
    DCHECK(!JS_IsUninitialized(r), "a read past an unknown view length derived no unknown");
    JS_FreeValue(ctx, ops[1]);
    return r;
}

static bool js_get_fast_array_element(JSContext *ctx, JSObject *p,
                                      uint32_t idx, JSValue *pval)
{
    /* 10.4.5.17 TypedArrayGetElement ( obj, index ) OVER AN ELEMENT SOME FLOW STORED UNKNOWN EXTERNAL INPUT
       INTO — the READ half of 10.4.5.18's store, and it is here because this is the ONE point every
       typed-array element read converges on: [[Get]], JS_GetPropertyUint32 and the interpreter's own fast path
       all reach the switch below, which is the property that makes the pair ONE mechanism rather than a store
       with a list of readers to remember.
       IT LANDS BEFORE THE SWITCH, NOT INSIDE AN ARM, because the answer is the same for every element type: the
       block's bytes are the unknown's EXAMPLE and the unknown is the value. Answering from an arm would have
       given each of the ten a copy of that decision to drift from.
       WHY THE PAIR IS ONE LANDING AND WAS NEVER SPLIT: a store that accepts an unknown beside a read that
       cannot hand it back is not a partial build, it is a silent DE-TAINT — `ta[i] = x` then `ta[i]` would
       return the example as a concrete number, deleting the fork and everything behind the arm it never
       explored, with nothing anywhere to say so. That is strictly worse than the abort this replaces.
       THE COST FOR AN ORDINARY BUFFER IS THE LOAD OF A ZERO. n_span is 0 for every buffer no flow has stored an
       unknown into, which is nearly all of them, so the branch below is predictable and the scan never runs. */
    if (unlikely(is_typed_array(p->class_id)) && idx < p->u.array.count) {
        if (unlikely(!JS_IsUninitialized(p->u.typed_array->offset_unknown))) {
            *pval = js_ta_get_at_unknown_offset(ctx, p, idx);
            return true;
        }
        if (unlikely(p->u.typed_array->buffer->u.array_buffer->n_span != 0) && js_ab_span_get(ctx, p, idx, pval))
            return true;
    }
    return js_get_fast_array_element_concrete(ctx, p, idx, pval);
}

static JSValue JS_GetPropertyValue(JSContext *ctx, JSValueConst this_obj,
                                   JSValue prop, const char *file, int line)
{
    JSAtom atom;
    JSValue ret;
    uint32_t tag;

    tag = JS_VALUE_GET_TAG(this_obj);
    if (likely(tag == JS_TAG_OBJECT)) {
        if (JS_VALUE_GET_TAG(prop) == JS_TAG_INT) {
            JSObject *p = JS_VALUE_GET_OBJ(this_obj);
            uint32_t idx = JS_VALUE_GET_INT(prop);
            JSValue val;
            /* fast path for array and typed array access */
            if (js_get_fast_array_element(ctx, p, idx, &val))
                return val;
        }
    } else if (unlikely(tag == JS_TAG_NULL || tag == JS_TAG_UNDEFINED)) {
        // per spec: not allowed to call ToPropertyKey before ToObject
        // so we must ensure to not invoke JS anything that's observable
        // from JS code
        atom = JS_ValueToAtomInternal(ctx, prop, JS_TO_STRING_NO_SIDE_EFFECTS);
        JS_FreeValue(ctx, prop);
        if (unlikely(atom == JS_ATOM_NULL))
            return JS_EXCEPTION;
        if (tag == JS_TAG_NULL) {
            JS_ThrowTypeErrorAtom(ctx, "cannot read property '%s' of null", atom);
        } else {
            JS_ThrowTypeErrorAtom(ctx, "cannot read property '%s' of undefined", atom);
        }
        JS_FreeAtom(ctx, atom);
        return JS_EXCEPTION;
    }
    atom = JS_ValueToAtom(ctx, prop);
    JS_FreeValue(ctx, prop);
    if (unlikely(atom == JS_ATOM_NULL))
        return JS_EXCEPTION;
    ret = JS_GetPropertyAt(ctx, this_obj, atom, file, line);
    JS_FreeAtom(ctx, atom);
    return ret;
}

JSValue JS_GetPropertyUint32At(JSContext *ctx, JSValueConst this_obj,
                               uint32_t idx, const char *file, int line)
{
    /* THE CALLER'S SITE, FORWARDED — not this line's. The macro captured it one frame up; re-capturing here
       would name quickjs.c for every JS_GetPropertyUint32 in the tree, which is the one site that is never the
       answer. Same reason the header spells all four as macros rather than letting three forward through
       JS_GetProperty. */
    return JS_GetPropertyInt64At(ctx, this_obj, idx, file, line);
}

/* DELETED: JS_TryGetPropertyInt64. It was the has-then-get pair every array builtin used to walk a sparse
   source, and its last caller went when those builtins became step machines over step_hasidx_run and
   step_getidx_run — but the body kept compiling, so it kept being a C entry that runs a Proxy's `has` and `get`
   traps with no flow base, and the [[HasProperty]] ratchet kept counting it as a consumer still to convert. A
   superseded body that still compiles is the fallback this project forbids, whether or not anything reaches it. */
JSValue JS_GetPropertyInt64At(JSContext *ctx, JSValueConst obj, int64_t idx,
                              const char *file, int line)
{
    JSAtom prop;
    JSValue val;

    if (likely(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT &&
               (uint64_t)idx <= INT32_MAX)) {
        /* fast path for array and typed array access */
        JSObject *p = JS_VALUE_GET_OBJ(obj);
        if (js_get_fast_array_element(ctx, p, idx, &val))
            return val;
    }
    prop = JS_NewAtomInt64(ctx, idx);
    if (prop == JS_ATOM_NULL)
        return JS_EXCEPTION;

    val = JS_GetPropertyAt(ctx, obj, prop, file, line);
    JS_FreeAtom(ctx, prop);
    return val;
}

/* `prop` may be pure ASCII or UTF-8 encoded */
JSValue JS_GetPropertyStrAt(JSContext *ctx, JSValueConst this_obj,
                            const char *prop, const char *file, int line)
{
    JSAtom atom;
    JSValue ret;
    atom = JS_NewAtom(ctx, prop);
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    ret = JS_GetPropertyAt(ctx, this_obj, atom, file, line);
    JS_FreeAtom(ctx, atom);
    return ret;
}

