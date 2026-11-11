/* Steps 4-7 sit BETWEEN the two coercions and run none of the page's code, which is why they share the stage
   that rests at step 3. */
#define DVCTOR_STAGES(X) \
    X(DVCTOR_NEWTGT, "25.3.2.1 steps 1-2 (NewTarget is not undefined; buffer has [[ArrayBufferData]])") \
    X(DVCTOR_OFFSET, "25.3.2.1 step 3 (offset is ToIndex(byteOffset))") \
    X(DVCTOR_BOUNDS, "25.3.2.1 steps 4-7 (buffer is not detached; offset is within bufferByteLength)") \
    X(DVCTOR_LEN,    "25.3.2.1 step 9.a (viewByteLength is ToIndex(byteLength))") \
    X(DVCTOR_CREATE, "25.3.2.1 step 10 (O is OrdinaryCreateFromConstructor(NewTarget, \"%DataView.prototype%\"))")
enum { DVCTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_dataview_ctor_steps[] = { DVCTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_dataview_ctor_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSDataViewCtor *s = st;
    JSValueConst buffer = step_arg(&s->hdr, 0);
    JSArrayBuffer *abuf;
    JSTypedArray *ta;
    JSObject *p;
    int r;

    if (s->hdr.stage == DVCTOR_NEWTGT) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->result = JS_UNDEFINED; s->prim = JS_UNDEFINED;
        s->offset = 0; s->len = 0; s->recompute_len = 0; s->track_rab = 0;
        /* step 1: a plain call reaches the machine with new_target UNDEFINED, and the spec puts the rejection in
           the algorithm itself — ahead of the ArrayBuffer check and both coercions */
        if (JS_IsUndefined(s->hdr.this_val))
            return JS_ThrowTypeError(ctx, "must be called with new"), -1;
        /* step 2: the receiver must be an ArrayBuffer before any argument is touched */
        if (!js_get_array_buffer(ctx, buffer)) return -1;
        s->hdr.stage = (s->hdr.argc > 1) ? DVCTOR_OFFSET : DVCTOR_BOUNDS;
    }
    if (s->hdr.stage == DVCTOR_OFFSET) {
        /* step 3's ToIndex(byteOffset) */
        r = step_toprim_run(ctx, &s->hdr, step_arg(&s->hdr, 1), HINT_NUMBER, cb_result, &s->prim,
                            out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        if (JS_ToIndex(ctx, &s->offset, s->prim)) return -1;   /* a primitive: this invokes nothing */
        s->hdr.stage = DVCTOR_BOUNDS;
    }
    if (s->hdr.stage == DVCTOR_BOUNDS) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* steps 4-6 sit BETWEEN the two coercions, so a detached or too-short buffer is rejected before
           byteLength's valueOf ever runs */
        abuf = js_get_array_buffer(ctx, buffer);
        DCHECK(abuf != NULL, "the DataView constructor's argument stopped being an ArrayBuffer mid-sequence");
        if (abuf->detached) { JS_ThrowTypeErrorDetachedArrayBuffer(ctx); return -1; }
        if (s->offset > abuf->byte_length) { JS_ThrowRangeError(ctx, "invalid byteOffset"); return -1; }
        s->len = abuf->byte_length - s->offset;
        if (s->hdr.argc > 2 && !JS_IsUndefined(step_arg(&s->hdr, 2))) {
            s->hdr.stage = DVCTOR_LEN;
        } else {
            s->recompute_len = 1;
            s->track_rab = array_buffer_is_resizable(abuf);
            s->hdr.stage = DVCTOR_CREATE;
        }
    }
    if (s->hdr.stage == DVCTOR_LEN) {
        /* step 8's ToIndex(byteLength) */
        uint64_t l;
        r = step_toprim_run(ctx, &s->hdr, step_arg(&s->hdr, 2), HINT_NUMBER, cb_result, &s->prim,
                            out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        if (JS_ToIndex(ctx, &l, s->prim)) return -1;           /* a primitive: this invokes nothing */
        if (l > s->len) { JS_ThrowRangeError(ctx, "invalid byteLength"); return -1; }
        s->len = l;
        s->hdr.stage = DVCTOR_CREATE;
    }
    DCHECK(s->hdr.stage == DVCTOR_CREATE, "the DataView constructor reached an unknown stage");
    r = step_create_from_ctor_run(ctx, &s->hdr, s->hdr.this_val, JS_CLASS_DATAVIEW, cb_result, &s->result,
                                  out_cb, out_argc);
    if (r) return r < 0 ? -1 : r;

    /* the `prototype` read is page code like the coercions: it can detach or resize the buffer under us */
    abuf = js_get_array_buffer(ctx, buffer);
    DCHECK(abuf != NULL, "the DataView constructor's argument stopped being an ArrayBuffer mid-sequence");
    if (abuf->detached) { JS_ThrowTypeErrorDetachedArrayBuffer(ctx); return -1; }
    if (s->offset > abuf->byte_length) {
        goto out_of_bound;
    } else if (s->recompute_len) {
        s->len = abuf->byte_length - s->offset;
    } else if (s->offset + s->len > abuf->byte_length) {
    out_of_bound:
        JS_ThrowRangeError(ctx, "invalid byteOffset or byteLength");
        return -1;
    }
    ta = js_malloc(ctx, sizeof(*ta));
    if (!ta) return -1;
    p = JS_VALUE_GET_OBJ(s->result);
    ta->obj = p;
    ta->buffer = JS_VALUE_GET_OBJ(js_dup(buffer));
    ta->offset = s->offset;
    ta->length = s->len;
    ta->track_rab = s->track_rab;
    /* §25.3.2.1 DataView ( buffer [ , byteOffset [ , byteLength ] ] ): an absent byteLength is the buffer's
       length minus the offset */
    ta->length_unknown = s->recompute_len && !JS_IsUninitialized(abuf->length_unknown)
        ? js_ab_length_derive(ctx, abuf, "25.3.2.1 DataView ( buffer [ , byteOffset [ , byteLength ] ] ) step 8.a",
                              s->offset, 0, s->len)
        : JS_UNINITIALIZED;
    ta->offset_unknown = JS_UNINITIALIZED;
    list_add_tail(&ta->link, &abuf->array_list);
    p->u.typed_array = ta;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_dataview_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSDataViewCtor *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->prim);
}

static JSValue js_dataview_ctor_fini(JSContext *ctx, void *st, bool take_result)
{
    JSDataViewCtor *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

// is the DataView out of bounds relative to its parent arraybuffer?
static bool dataview_is_oob(JSObject *p)
{
    JSArrayBuffer *abuf;
    JSTypedArray *ta;

    DCHECK(p->class_id == JS_CLASS_DATAVIEW, "p->class_id == JS_CLASS_DATAVIEW");
    ta = p->u.typed_array;
    abuf = ta->buffer->u.array_buffer;
    if (abuf->detached)
        return true;
    if (ta->offset > abuf->byte_length)
        return true;
    if (ta->track_rab)
        return false;
    return (int64_t)ta->offset + ta->length > abuf->byte_length;
}

static JSObject *get_dataview(JSContext *ctx, JSValueConst this_val)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(this_val) != JS_TAG_OBJECT)
        goto fail;
    p = JS_VALUE_GET_OBJ(this_val);
    if (p->class_id != JS_CLASS_DATAVIEW) {
    fail:
        JS_ThrowTypeError(ctx, "not a DataView");
        return NULL;
    }
    return p;
}

static JSValue js_dataview_get_buffer(JSContext *ctx, JSValueConst this_val)
{
    JSObject *p;
    JSTypedArray *ta;
    p = get_dataview(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    ta = p->u.typed_array;
    return js_dup(JS_MKPTR(JS_TAG_OBJECT, ta->buffer));
}

static JSValue js_dataview_get_byteLength(JSContext *ctx, JSValueConst this_val)
{
    JSArrayBuffer *abuf;
    JSTypedArray *ta;
    JSObject *p;

    p = get_dataview(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (dataview_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);
    ta = p->u.typed_array;
    abuf = ta->buffer->u.array_buffer;
    if (!JS_IsUninitialized(ta->length_unknown))
        return js_dup(ta->length_unknown);   /* §25.3.4.2 step 6, GetViewByteLength */
    if (ta->track_rab)
        return js_uint32(abuf->byte_length - ta->offset);
    return js_uint32(ta->length);
}

static JSValue js_dataview_get_byteOffset(JSContext *ctx, JSValueConst this_val)
{
    JSTypedArray *ta;
    JSObject *p;

    p = get_dataview(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (dataview_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);
    ta = p->u.typed_array;
    return js_uint32(ta->offset);
}

/* The element type names of ECMAScript §25.3.1.5 GetViewValue ( view, requestIndex, isLittleEndian, type )'s
   `type` (Table 71, The TypedArray Constructors), in class order from Uint8ClampedArray. */
static const char *const js_view_type_names[JS_TYPED_ARRAY_COUNT] = {
    "uint8clamped", "int8", "uint8", "int16", "uint16", "int32", "uint32", "bigint64", "biguint64", "float16",
    "float32", "float64",
};

/* GetViewValue over bytes a span still describes: the value is unknown, derived from each piece's unknown at
   its position and width and from the byte order, with the value the block's bytes give (`real`, consumed) as
   its example. A window no span describes answers `real` itself. */
static JSValue js_dataview_span_value(JSContext *ctx, JSObject *bufobj, uint32_t start, int size,
                                      bool little_endian, int class_id, JSValue real)
{
    JSUnknownPiece *pc;
    JSValue *ops, r;
    char op[80];
    int n, i, k = 0;

    n = js_ab_unknown_pieces(ctx, bufobj->u.array_buffer, bufobj, start, start + (uint32_t)size, &pc);
    if (n == 0)
        return real;
    ops = js_malloc(ctx, sizeof(JSValue) * (size_t)(4 * n + 1));
    CHECK(ops != NULL, "OOM deriving a DataView read over unknown bytes — dropping it would hand the page the "
                       "example as a concrete number");
    for (i = 0; i < n; i++) {
        ops[k++] = pc[i].unknown;
        ops[k++] = js_int32((int32_t)pc[i].offset);
        ops[k++] = JS_NewInt64(ctx, pc[i].codec ? (int64_t)pc[i].image_pos : (int64_t)start + pc[i].offset);
        ops[k++] = js_int32((int32_t)pc[i].length);
    }
    ops[k++] = JS_NewBool(ctx, little_endian);
    snprintf(op, sizeof op, "25.3.1.5 GetViewValue %s", js_view_type_names[class_id - JS_CLASS_UINT8C_ARRAY]);
    r = g_concolic.derived(ctx, op, ops, k, real);
    DCHECK(!JS_IsUninitialized(r), "a DataView read over a span derived no unknown — every span records one");
    js_free(ctx, ops);
    JS_FreeBufferUnknowns(ctx, pc, n);
    return r;
}

/* §25.3.1.5 GetViewValue's last step, GetValueFromBuffer, over `pos` inside the view's stored extent. */
static JSValue js_dataview_read(JSContext *ctx, JSTypedArray *ta, uint64_t pos, bool littleEndian, int class_id)
{
    JSArrayBuffer *abuf = ta->buffer->u.array_buffer;
    bool is_swap = littleEndian ^ !is_be();
    int size = 1 << typed_array_size_log2(class_id);
    uint8_t *ptr;
    uint32_t v;
    JSValue ret = JS_UNDEFINED;

    DCHECK(!abuf->detached && pos + size <= ta->length && (int64_t)ta->offset + ta->length <= abuf->byte_length,
           "a DataView read was made outside the extent its bounds tests admitted");
    ptr = abuf->data + ta->offset + pos;

    switch(class_id) {
    case JS_CLASS_INT8_ARRAY:
        ret = js_int32(*(int8_t *)ptr); break;
    case JS_CLASS_UINT8_ARRAY:
        ret = js_int32(*(uint8_t *)ptr); break;
    case JS_CLASS_INT16_ARRAY:
        v = get_u16(ptr);
        if (is_swap)
            v = bswap16(v);
        ret = js_int32((int16_t)v); break;
    case JS_CLASS_UINT16_ARRAY:
        v = get_u16(ptr);
        if (is_swap)
            v = bswap16(v);
        ret = js_int32(v); break;
    case JS_CLASS_INT32_ARRAY:
        v = get_u32(ptr);
        if (is_swap)
            v = bswap32(v);
        ret = js_int32(v); break;
    case JS_CLASS_UINT32_ARRAY:
        v = get_u32(ptr);
        if (is_swap)
            v = bswap32(v);
        ret = js_uint32(v); break;
    case JS_CLASS_BIG_INT64_ARRAY:
        {
            uint64_t v;
            v = get_u64(ptr);
            if (is_swap)
                v = bswap64(v);
            ret = JS_NewBigInt64(ctx, v); break;
        }
        break;
    case JS_CLASS_BIG_UINT64_ARRAY:
        {
            uint64_t v;
            v = get_u64(ptr);
            if (is_swap)
                v = bswap64(v);
            ret = JS_NewBigUint64(ctx, v); break;
        }
        break;
    case JS_CLASS_FLOAT16_ARRAY:
        {
            uint16_t v;
            v = get_u16(ptr);
            if (is_swap)
                v = bswap16(v);
            ret = js_float64(fromfp16(v)); break;
        }
    case JS_CLASS_FLOAT32_ARRAY:
        {
            union {
                float f;
                uint32_t i;
            } u;
            v = get_u32(ptr);
            if (is_swap)
                v = bswap32(v);
            u.i = v;
            ret = js_float64(u.f); break;
        }
    case JS_CLASS_FLOAT64_ARRAY:
        {
            union {
                double f;
                uint64_t i;
            } u;
            u.i = get_u64(ptr);
            if (is_swap)
                u.i = bswap64(u.i);
            ret = js_float64(u.f); break;
        }
    default:
        abort();
    }
    return js_dataview_span_value(ctx, ta->buffer, ta->offset + (uint32_t)pos, size, littleEndian, class_id, ret);
}

/* §25.3.1.5 GetViewValue ( view, requestIndex, isLittleEndian, type ), the DataView getters' algorithm, as a
   machine: ToIndex runs the page's valueOf, and over a view whose length is an unknown the in-range test is a
   fork whose arms are a value and the RangeError. */
#define DVGET_STAGES(X) \
    X(DVGET_ENTRY, "25.3.1.5 GetViewValue: RequireInternalSlot(view, [[DataView]])") \
    X(DVGET_INDEX, "25.3.1.5 GetViewValue: getIndex is ToIndex(requestIndex)") \
    X(DVGET_RANGE, "25.3.1.5 GetViewValue: getIndex + elementSize > viewSize throws a RangeError, then " \
                   "GetValueFromBuffer")
enum { DVGET_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_dv_get_steps[] = { DVGET_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_dv_get_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSDvGet *s = st;
    int class_id = s->hdr.arg, size = 1 << typed_array_size_log2(class_id), r, arm;
    JSTypedArray *ta;
    JSArrayBuffer *abuf;
    bool little;

    if (s->hdr.stage == DVGET_ENTRY) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->result = JS_UNDEFINED;
        s->pred = JS_UNINITIALIZED;
        s->index = 0;
        if (!JS_GetOpaque2(ctx, s->hdr.this_val, JS_CLASS_DATAVIEW))
            return -1;
        s->hdr.stage = DVGET_INDEX;
    }
    if (s->hdr.stage == DVGET_INDEX) {
        double d;

        r = step_todouble_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &d, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        /* §7.1.22 ToIndex: ToIntegerOrInfinity, then a RangeError outside [0, 2^53 - 1] */
        d = isnan(d) ? 0 : trunc(d) + 0.0;
        if (d < 0 || d > MAX_SAFE_INTEGER) {
            JS_ThrowRangeError(ctx, "invalid array index");
            return -1;
        }
        s->index = d;
        s->hdr.stage = DVGET_RANGE;
    }
    DCHECK(s->hdr.stage == DVGET_RANGE, "a DataView getter resumed in no stage");
    JS_FreeValue(ctx, cb_result);
    ta = JS_GetOpaque(s->hdr.this_val, JS_CLASS_DATAVIEW);
    abuf = ta->buffer->u.array_buffer;
    DCHECK(s->hdr.argc < 2 || !js_value_is_concolic(step_arg(&s->hdr, 1)),
           "a DataView getter's littleEndian is unknown input: build its ToBoolean as step_tobool_run's fork");
    little = s->hdr.argc > 1 && JS_ToBool(ctx, step_arg(&s->hdr, 1));
    if (abuf->detached) {
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        return -1;
    }
    if ((int64_t)ta->offset + ta->length > abuf->byte_length) {
        JS_ThrowTypeError(ctx, "out of bound");
        return -1;
    }
    if (!JS_IsUninitialized(ta->length_unknown)) {
        if (JS_IsUninitialized(s->pred)) {
            JSValue sp[2];

            sp[0] = js_float64(s->index + size);
            sp[1] = js_dup(ta->length_unknown);
            CHECK(g_concolic.rel(ctx, sp + 2, OP_lte), "the relation hook declined a view's unknown length");
            s->pred = sp[0];
        }
        r = step_tobool_run(ctx, &s->hdr, s->pred, "25.3.1.5 GetViewValue in-range test", &arm);
        if (r) return r < 0 ? -1 : r;
        if (!arm) {
            JS_ThrowRangeError(ctx, "out of bound");
            return -1;
        }
        if (s->index + size > ta->length) {
            /* in range in this world and past the example's bytes: a value nobody has bytes for */
            JSValue ops[4];

            ops[0] = abuf->length_unknown;
            ops[1] = JS_NewInt64(ctx, (int64_t)ta->offset + (int64_t)s->index);
            ops[2] = js_int32(size);
            ops[3] = JS_NewBool(ctx, little);
            DCHECK(!JS_IsUninitialized(abuf->length_unknown),
                   "a view's length is unknown over a buffer whose length is stated");
            s->result = g_concolic.derived(ctx, "25.3.1.5 GetViewValue past the example", ops, 4, JS_UNDEFINED);
            return 0;
        }
    } else if (s->index + size > ta->length) {
        JS_ThrowRangeError(ctx, "out of bound");
        return -1;
    }
    s->result = js_dataview_read(ctx, ta, (uint64_t)s->index, little, class_id);
    return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_dv_get_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSDvGet *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->pred);
}

static JSValue js_dv_get_fini(JSContext *ctx, void *st, bool take_result)
{
    JSDvGet *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* 25.3.4.16 step 4: ToIndex(getIndex) — and its RangeError for a fractional or negative offset — completes
   BEFORE the value is read. The argument is a primitive by the time this runs, so the conversion itself runs
   none of the page's code; only its rejection is observable. */
static int js_dataview_set_midcheck(JSContext *ctx, JSValueConst *argp, int magic)
{
    uint64_t pos;
    return JS_ToIndex(ctx, &pos, argp[0]) ? -1 : 0;
}

/* 25.3.4.16 SetViewValue steps 1-3: the receiver must be a DataView over a MUTABLE buffer, and that is decided
   BEFORE either argument is read — test262 pins the ordering, so the coerce-then-compute machine runs this
   first and the body re-checks it on the way through. */
static int js_dataview_set_precheck(JSContext *ctx, const JSStepHdr *h)
{
    JSValueConst this_val = h->this_val;
    JSTypedArray *ta = JS_GetOpaque2(ctx, this_val, JS_CLASS_DATAVIEW);
    if (!ta)
        return -1;
    if (ta->buffer->u.array_buffer->immutable) {
        JS_ThrowTypeErrorImmutableArrayBuffer(ctx);
        return -1;
    }
    return 0;
}

static JSValue js_dataview_setValue(JSContext *ctx,
                                    JSValueConst this_obj,
                                    int argc, JSValueConst *argv, int class_id)
{
    JSTypedArray *ta;
    JSArrayBuffer *abuf;
    bool littleEndian, is_swap;
    int size;
    uint8_t *ptr;
    uint64_t v64;
    uint32_t v;
    uint64_t pos;
    JSValueConst val;

    ta = JS_GetOpaque2(ctx, this_obj, JS_CLASS_DATAVIEW);
    if (!ta)
        return JS_EXCEPTION;
    abuf = ta->buffer->u.array_buffer;
    if (abuf->immutable)
        return JS_ThrowTypeErrorImmutableArrayBuffer(ctx);
    size = 1 << typed_array_size_log2(class_id);
    if (JS_ToIndex(ctx, &pos, argv[0]))
        return JS_EXCEPTION;
    val = argv[1];
    v = 0; /* avoid warning */
    v64 = 0; /* avoid warning */
    if (class_id <= JS_CLASS_UINT32_ARRAY) {
        if (JS_ToUint32(ctx, &v, val))
            return JS_EXCEPTION;
    } else if (class_id <= JS_CLASS_BIG_UINT64_ARRAY) {
        if (JS_ToBigInt64(ctx, (int64_t *)&v64, val))
            return JS_EXCEPTION;
    } else {
        double d;
        if (JS_ToFloat64(ctx, &d, val))
            return JS_EXCEPTION;
        if (class_id == JS_CLASS_FLOAT16_ARRAY) {
            v = tofp16(d);
        } else if (class_id == JS_CLASS_FLOAT32_ARRAY) {
            union {
                float f;
                uint32_t i;
            } u;
            u.f = d;
            v = u.i;
        } else {
            JSFloat64Union u;
            u.d = d;
            v64 = u.u64;
        }
    }
    littleEndian = argc > 2 && JS_ToBool(ctx, argv[2]);
    is_swap = littleEndian ^ !is_be();
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    // order matters: this check should come before the next one
    if ((pos + size) > ta->length)
        return JS_ThrowRangeError(ctx, "out of bound");
    // test262 expects a TypeError for this and V8, in its infinite wisdom,
    // throws a "detached array buffer" exception, but IMO that doesn't make
    // sense because the buffer is not in fact detached, it's still there
    if ((int64_t)ta->offset + ta->length > abuf->byte_length)
        return JS_ThrowTypeError(ctx, "out of bound");
    ptr = abuf->data + ta->offset + pos;
    /* THE WRITE THAT PROVES THE UNIT. `dv.setFloat64(3, x)` writes bytes 3..10 of the buffer: a DataView
       exposes no indexed properties, so there is no (object, index) slot an entry could name, and those bytes
       cross the element boundary of every view over the same storage. Captured on the buffer like every other
       byte write — see cow_capture_bytes. */
    cow_capture_bytes(ctx, JS_VALUE_GET_OBJ(this_obj));

    switch(class_id) {
    case JS_CLASS_INT8_ARRAY:
    case JS_CLASS_UINT8_ARRAY:
        *ptr = v;
        break;
    case JS_CLASS_INT16_ARRAY:
    case JS_CLASS_UINT16_ARRAY:
    case JS_CLASS_FLOAT16_ARRAY:
        if (is_swap)
            v = bswap16(v);
        put_u16(ptr, v);
        break;
    case JS_CLASS_INT32_ARRAY:
    case JS_CLASS_UINT32_ARRAY:
    case JS_CLASS_FLOAT32_ARRAY:
        if (is_swap)
            v = bswap32(v);
        put_u32(ptr, v);
        break;
    case JS_CLASS_BIG_INT64_ARRAY:
    case JS_CLASS_BIG_UINT64_ARRAY:
    case JS_CLASS_FLOAT64_ARRAY:
        if (is_swap)
            v64 = bswap64(v64);
        put_u64(ptr, v64);
        break;
    default:
        abort();
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_dataview_proto_funcs[] = {
    JS_CGETSET_DEF("buffer", js_dataview_get_buffer, NULL ),
    JS_CGETSET_DEF("byteLength", js_dataview_get_byteLength, NULL ),
    JS_CGETSET_DEF("byteOffset", js_dataview_get_byteOffset, NULL ),
    JS_CFUNC_STEP_DEF("getInt8", 1, STEPDEF_DV_GET_INT8 ),
    JS_CFUNC_STEP_DEF("getUint8", 1, STEPDEF_DV_GET_UINT8 ),
    JS_CFUNC_STEP_DEF("getInt16", 1, STEPDEF_DV_GET_INT16 ),
    JS_CFUNC_STEP_DEF("getUint16", 1, STEPDEF_DV_GET_UINT16 ),
    JS_CFUNC_STEP_DEF("getInt32", 1, STEPDEF_DV_GET_INT32 ),
    JS_CFUNC_STEP_DEF("getUint32", 1, STEPDEF_DV_GET_UINT32 ),
    JS_CFUNC_STEP_DEF("getBigInt64", 1, STEPDEF_DV_GET_BIG_INT64 ),
    JS_CFUNC_STEP_DEF("getBigUint64", 1, STEPDEF_DV_GET_BIG_UINT64 ),
    JS_CFUNC_STEP_DEF("getFloat16", 1, STEPDEF_DV_GET_FLOAT16 ),
    JS_CFUNC_STEP_DEF("getFloat32", 1, STEPDEF_DV_GET_FLOAT32 ),
    JS_CFUNC_STEP_DEF("getFloat64", 1, STEPDEF_DV_GET_FLOAT64 ),
    JS_CFUNC_STEP_DEF("setInt8", 2, STEPDEF_DV_SET_INT8 ),
    JS_CFUNC_STEP_DEF("setUint8", 2, STEPDEF_DV_SET_UINT8 ),
    JS_CFUNC_STEP_DEF("setInt16", 2, STEPDEF_DV_SET_INT16 ),
    JS_CFUNC_STEP_DEF("setUint16", 2, STEPDEF_DV_SET_UINT16 ),
    JS_CFUNC_STEP_DEF("setInt32", 2, STEPDEF_DV_SET_INT32 ),
    JS_CFUNC_STEP_DEF("setUint32", 2, STEPDEF_DV_SET_UINT32 ),
    JS_CFUNC_STEP_DEF("setBigInt64", 2, STEPDEF_DV_SET_BIG_INT64 ),
    JS_CFUNC_STEP_DEF("setBigUint64", 2, STEPDEF_DV_SET_BIG_UINT64 ),
    JS_CFUNC_STEP_DEF("setFloat16", 2, STEPDEF_DV_SET_FLOAT16 ),
    JS_CFUNC_STEP_DEF("setFloat32", 2, STEPDEF_DV_SET_FLOAT32 ),
    JS_CFUNC_STEP_DEF("setFloat64", 2, STEPDEF_DV_SET_FLOAT64 ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "DataView", JS_PROP_CONFIGURABLE ),
};

