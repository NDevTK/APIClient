/* Typed Arrays */

static uint8_t const typed_array_size_log2[JS_TYPED_ARRAY_COUNT] = {
    0, 0, 0, 1, 1, 2, 2,
    3, 3,                   // BigInt64Array, BigUint64Array
    1, 2, 3                 // Float16Array, Float32Array, Float64Array
};

/* AllocateArrayBuffer from step 2 onwards, taking the object OrdinaryCreateFromConstructor already made and
   OWNING it from here. The split exists because that create is a `prototype` [[Get]] on new.target — page code —
   and only a step machine can suspend at it; every one of the engine's own allocations below builds on the
   intrinsic prototype, where the create runs nothing. */
static JSValue js_array_buffer_init(JSContext *ctx, JSValue obj,
                                    uint64_t len, uint64_t *max_len,
                                    JSClassID class_id,
                                    uint8_t *buf,
                                    JSFreeArrayBufferDataFunc *free_func,
                                    void *opaque, bool alloc_flag)
{
    JSRuntime *rt = ctx->rt;
    JSArrayBuffer *abuf = NULL;
    uint64_t sab_alloc_len;

    if (!alloc_flag && buf && max_len && free_func != js_array_buffer_free) {
        // not observable from JS land, only through C API misuse;
        // JS code cannot create externally managed buffers directly
        JS_FreeValue(ctx, obj);
        return JS_ThrowInternalError(ctx,
                                     "resizable ArrayBuffers not supported "
                                     "for externally managed buffers");
    }
    /* XXX: we are currently limited to 2 GB */
    if (len > INT32_MAX) {
        JS_ThrowRangeError(ctx, "invalid array buffer length");
        goto fail;
    }
    if (max_len && *max_len > INT32_MAX) {
        JS_ThrowRangeError(ctx, "invalid max array buffer length");
        goto fail;
    }
    abuf = js_malloc(ctx, sizeof(*abuf));
    if (!abuf)
        goto fail;
    /* js_malloc AND NOT js_mallocz, so every field is assigned here or it is garbage. An uninitialised span
       count is not a quiet wrong answer: js_ab_span_get would read it, walk a garbage pointer and hand the
       page whatever it found, which is why these three are set at the allocation rather than lazily at the
       first store. */
    abuf->span = NULL;
    abuf->n_span = 0;
    abuf->span_size = 0;
    abuf->length_unknown = JS_UNINITIALIZED;
    abuf->waiter_lists = JS_UNDEFINED;
    abuf->byte_length = len;
    abuf->max_byte_length = max_len ? *max_len : -1;
    if (class_id == JS_CLASS_SHARED_ARRAY_BUFFER) {
        JSSharedBlock *block;
        JSValue lists;

        /* A block is allocated only here, by §25.2.2.1 AllocateSharedArrayBuffer, and viewed again only through
           JS_NewSharedArrayBufferView: never over memory an embedder supplies. */
        CHECK(alloc_flag, "a SharedArrayBuffer was built over embedder memory: a Shared Data Block belongs to "
                          "its runtime, and another agent views it through JS_NewSharedArrayBufferView");
        /* §25.2.5.3 grow moves no storage, so the block holds maxByteLength from the start. */
        sab_alloc_len = max_len ? *max_len : len;
        lists = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(lists))
            goto fail;
        block = js_malloc(ctx, sizeof(*block) + max_int(sab_alloc_len, 1));
        if (!block) {
            JS_FreeValue(ctx, lists);
            goto fail;
        }
        block->ref_count = 1;
        block->waiter_lists = JS_VALUE_GET_OBJ(lists);
        memset(block->data, 0, sab_alloc_len);
        abuf->data = (uint8_t *)block->data;
        abuf->waiter_lists = lists;
        opaque = block;
        free_func = js_shared_block_release;
    } else if (alloc_flag) {
        /* the allocation must be done after the object creation */
        abuf->data = js_mallocz(ctx, max_int(len, 1));
        if (!abuf->data)
            goto fail;
    } else {
        if (buf) {
            abuf->data = buf;
        } else {
            abuf->data = js_mallocz(ctx, 1);
            if (!abuf->data)
                goto fail;
            free_func = js_array_buffer_free;
        }
    }
    init_list_head(&abuf->array_list);
    abuf->detached = false;
    abuf->immutable = false;
    abuf->shared = (class_id == JS_CLASS_SHARED_ARRAY_BUFFER);
    abuf->opaque = opaque;
    abuf->free_func = free_func;
    if (alloc_flag && buf)
        memcpy(abuf->data, buf, len);
    JS_SetOpaqueInternal(obj, abuf);
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    js_free(ctx, abuf);
    return JS_EXCEPTION;
}

static void js_array_buffer_free(JSRuntime *rt, void *opaque, void *ptr)
{
    js_free_rt(rt, ptr);
}

/* A viewing SharedArrayBuffer is gone: the block goes with its last one. */
static void js_shared_block_release(JSRuntime *rt, void *opaque, void *ptr)
{
    JSSharedBlock *block = opaque;

    DCHECK(block != NULL && (uint8_t *)block->data == ptr && block->ref_count > 0,
           "a SharedArrayBuffer released a block it does not view");
    if (--block->ref_count == 0)
        js_free_rt(rt, block);
}

JSValue JS_NewSharedArrayBufferView(JSContext *ctx, JSValueConst sab)
{
    JSObject *p;
    JSArrayBuffer *abuf, *view;
    JSSharedBlock *block;
    JSValue obj;

    CHECK(JS_GetClassID(sab) == JS_CLASS_SHARED_ARRAY_BUFFER, "a SharedArrayBuffer view was asked of a non-SAB");
    p = JS_VALUE_GET_OBJ(sab);
    abuf = p->u.array_buffer;
    block = abuf->opaque;
    DCHECK(block != NULL && abuf->free_func == js_shared_block_release,
           "a SharedArrayBuffer whose storage is not a Shared Data Block");
    obj = js_create_from_intrinsic(ctx, JS_CLASS_SHARED_ARRAY_BUFFER);
    if (JS_IsException(obj))
        return obj;
    view = js_malloc(ctx, sizeof(*view));
    if (!view) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    *view = *abuf;
    view->span = NULL; view->n_span = 0; view->span_size = 0;
    DCHECK(JS_IsUninitialized(abuf->length_unknown), "a SharedArrayBuffer's length is a codec image's");
    view->length_unknown = JS_UNINITIALIZED;
    init_list_head(&view->array_list);
    view->waiter_lists = js_dup(abuf->waiter_lists);
    block->ref_count++;
    JS_SetOpaqueInternal(obj, view);
    return obj;
}

/* the engine's OWN allocations: an intrinsic-prototype buffer, so there is no new.target and no read to suspend
   at. `new ArrayBuffer(...)` does not come through here — it is a step machine that owns the create itself. */
static JSValue js_array_buffer_constructor3(JSContext *ctx,
                                            uint64_t len, uint64_t *max_len,
                                            JSClassID class_id,
                                            uint8_t *buf,
                                            JSFreeArrayBufferDataFunc *free_func,
                                            void *opaque, bool alloc_flag)
{
    JSValue obj = js_create_from_intrinsic(ctx, class_id);
    if (JS_IsException(obj))
        return obj;
    return js_array_buffer_init(ctx, obj, len, max_len, class_id, buf, free_func, opaque, alloc_flag);
}

static JSValue js_array_buffer_constructor2(JSContext *ctx,
                                            uint64_t len, uint64_t *max_len,
                                            JSClassID class_id)
{
    return js_array_buffer_constructor3(ctx, len, max_len,
                                        class_id, NULL, js_array_buffer_free,
                                        NULL, true);
}

static JSValue js_array_buffer_constructor1(JSContext *ctx,
                                            uint64_t len, uint64_t *max_len)
{
    return js_array_buffer_constructor2(ctx, len, max_len,
                                        JS_CLASS_ARRAY_BUFFER);
}

JSValue JS_NewArrayBuffer(JSContext *ctx, uint8_t *buf, size_t len,
                          JSFreeArrayBufferDataFunc *free_func, void *opaque,
                          bool is_shared)
{
    JSClassID class_id =
        is_shared ? JS_CLASS_SHARED_ARRAY_BUFFER : JS_CLASS_ARRAY_BUFFER;
    return js_array_buffer_constructor3(ctx, len, NULL, class_id,
                                        buf, free_func, opaque, false);
}

bool JS_IsArrayBuffer(JSValueConst obj) {
    return JS_GetClassID(obj) == JS_CLASS_ARRAY_BUFFER;
}

/* create a new ArrayBuffer of length 'len' and copy 'buf' to it */
JSValue JS_NewArrayBufferCopy(JSContext *ctx, const uint8_t *buf, size_t len)
{
    return js_array_buffer_constructor3(ctx, len, NULL,
                                        JS_CLASS_ARRAY_BUFFER,
                                        (uint8_t *)buf,
                                        js_array_buffer_free, NULL,
                                        true);
}

/* TWO constructors, one machine. GetArrayBufferMaxByteLengthOption is named rather than numbered because it is
   the operation the two steps belong to; its Get and its ToIndex are the page's code and each is a stage. ONE
   stage list expanded once per algorithm; see AFIND_STAGES. */
#define ABCTOR_STAGES(X, NEWTGT, LEN, OPT, OPTLEN, ALLOC) \
    X(ABCTOR_NEWTGT, NEWTGT) \
    X(ABCTOR_LEN,    LEN)    \
    X(ABCTOR_OPT,    OPT)    \
    X(ABCTOR_OPTLEN, OPTLEN) \
    X(ABCTOR_ALLOC,  ALLOC)
enum { ABCTOR_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0, 0, 0) };
static const char *const js_ab_ctor_steps[] = {
    ABCTOR_STAGES(JS_STEP_STAGE_LABEL,
        "25.1.4.1 step 1 (NewTarget is not undefined)",
        "25.1.4.1 step 2 (byteLength is ToIndex(length))",
        "25.1.4.1 step 3 via GetArrayBufferMaxByteLengthOption step 2 (Get(options, \"maxByteLength\"))",
        "25.1.4.1 step 3 via GetArrayBufferMaxByteLengthOption step 4 (ToIndex(maxByteLength))",
        "25.1.4.1 step 4 (AllocateArrayBuffer's OrdinaryCreateFromConstructor)")
    NULL };
static const char *const js_sab_ctor_steps[] = {
    ABCTOR_STAGES(JS_STEP_STAGE_LABEL,
        "25.2.3.1 step 1 (NewTarget is not undefined)",
        "25.2.3.1 step 2 (byteLength is ToIndex(length))",
        "25.2.3.1 step 3 via GetArrayBufferMaxByteLengthOption step 2 (Get(options, \"maxByteLength\"))",
        "25.2.3.1 step 3 via GetArrayBufferMaxByteLengthOption step 4 (ToIndex(maxByteLength))",
        "25.2.3.1 step 4 (AllocateSharedArrayBuffer's OrdinaryCreateFromConstructor)")
    NULL };

static int js_ab_ctor_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSABCtor *s = st;
    JSClassID class_id = s->hdr.arg;
    JSValue obj;
    int64_t i;
    int r;

    if (s->hdr.stage == ABCTOR_NEWTGT) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->result = JS_UNDEFINED; s->prim = JS_UNDEFINED;
        s->len = 0; s->max_len = 0; s->has_max = 0;
        /* step 1: a plain call reaches the machine with new_target UNDEFINED */
        if (JS_IsUndefined(s->hdr.this_val))
            return JS_ThrowTypeError(ctx, "must be called with new"), -1;
        s->hdr.stage = ABCTOR_LEN;
    }
    if (s->hdr.stage == ABCTOR_LEN) {
        /* step 2's ToIndex(length) */
        r = step_toprim_run(ctx, &s->hdr, step_arg(&s->hdr, 0), HINT_NUMBER, cb_result, &s->prim,
                            out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        if (JS_ToIndex(ctx, &s->len, s->prim)) return -1;   /* a primitive: this invokes nothing */
        /* GetArrayBufferMaxByteLengthOption step 1: a non-object options bag has no read at all */
        s->hdr.stage = (s->hdr.argc > 1 && JS_IsObject(step_arg(&s->hdr, 1))) ? ABCTOR_OPT : ABCTOR_ALLOC;
    }
    if (s->hdr.stage == ABCTOR_OPT) {
        JS_FreeValue(ctx, s->prim); s->prim = JS_UNDEFINED;
        r = step_getprop_run(ctx, &s->hdr, step_arg(&s->hdr, 1), JS_ATOM_maxByteLength, cb_result, &s->prim,
                             out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        /* step 3: an absent option leaves the buffer fixed-length */
        s->hdr.stage = JS_IsUndefined(s->prim) ? ABCTOR_ALLOC : ABCTOR_OPTLEN;
    }
    if (s->hdr.stage == ABCTOR_OPTLEN) {
        JSValue v = s->prim;
        s->prim = JS_UNDEFINED;
        r = step_toprim_run(ctx, &s->hdr, v, HINT_NUMBER, cb_result, &s->prim, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) { JS_FreeValue(ctx, v); return r < 0 ? -1 : r; }
        JS_FreeValue(ctx, v);
        if (JS_ToInt64Free(ctx, &i, s->prim)) { s->prim = JS_UNDEFINED; return -1; }
        s->prim = JS_UNDEFINED;
        // don't have to check i < 0 because len >= 0
        if (s->len > (uint64_t)i || i > MAX_SAFE_INTEGER)
            return JS_ThrowRangeError(ctx, "invalid array buffer max length"), -1;
        s->max_len = i;
        s->has_max = 1;
        s->hdr.stage = ABCTOR_ALLOC;
    }
    DCHECK(s->hdr.stage == ABCTOR_ALLOC, "the ArrayBuffer constructor reached an unknown stage");
    r = step_create_from_ctor_run(ctx, &s->hdr, s->hdr.this_val, class_id, cb_result, &obj, out_cb, out_argc);
    if (r) return r < 0 ? -1 : r;
    s->result = js_array_buffer_init(ctx, obj, s->len, s->has_max ? &s->max_len : NULL, class_id,
                                     NULL, js_array_buffer_free, NULL, true);
    return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_ab_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSABCtor *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->prim);
}

static JSValue js_ab_ctor_fini(JSContext *ctx, void *st, bool take_result)
{
    JSABCtor *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* also used for SharedArrayBuffer */
/* THE ELEMENT'S UNKNOWN, OR NOTHING — see JSArrayBufferSpan. Answers true with *pval OWNED.
   THE SCAN IS BACKWARDS AND THAT IS THE RECENCY RULE, not an optimisation on its own: the last span recorded
   over an element is the one that describes it, so a later AT write through the same offset supersedes an
   earlier one without anything having to delete it. It is also the order a serializer's own writes arrive in,
   so the element a page reads back is usually found at once. An ANY span is walked by the SAME loop and not
   ahead of it, so recency decides between the two kinds too. */
/* Whether any byte of [lo, lo + n) inside the IMAGE span `s` still holds the byte the codec wrote there. */
static bool js_ab_image_live(const JSArrayBuffer *abuf, const JSArrayBufferSpan *s, uint32_t lo, uint32_t n)
{
    JSString *img = JS_VALUE_GET_STRING(s->image);
    uint32_t b = lo > s->offset ? lo : s->offset;
    uint64_t end = (uint64_t)lo + n, send = (uint64_t)s->offset + s->extent;

    if (send < end) end = send;
    if ((int64_t)end > abuf->byte_length) end = (uint64_t)abuf->byte_length;
    for (; b < end; b++)
        if (abuf->data[b] == str8(img)[b - s->offset])
            return true;
    return false;
}

/* The unknown an element read inside the IMAGE span `s` answers: derived from the span's unknown at this
   element's codec position and width, with the element as the block holds it for its example. */
static JSValue js_ab_image_element(JSContext *ctx, JSObject *p, uint32_t idx, const JSArrayBufferSpan *s,
                                   uint32_t offset, int length)
{
    JSValue ex = JS_UNDEFINED, ops[3], r;
    char op[96];

    if (!js_get_fast_array_element_concrete(ctx, p, idx, &ex))
        DFAIL("an in-range typed-array element had no concrete value to serve as an IMAGE element's example");
    ops[0] = s->unknown;
    ops[1] = JS_NewInt64(ctx, (int64_t)s->image_pos + (int64_t)offset - (int64_t)s->offset);
    ops[2] = js_int32(length);
    snprintf(op, sizeof op, "%s[]", s->codec);
    r = g_concolic.derived(ctx, op, ops, 3, ex);
    DCHECK(!JS_IsUninitialized(r), "an IMAGE span's unknown is not an unknown — JS_NewUint8ArrayImage records "
                                   "a span only over one");
    return r;
}

static bool js_ab_span_get(JSContext *ctx, JSObject *p, uint32_t idx, JSValue *pval)
{
    JSObject *bufobj = p->u.typed_array->buffer;
    const JSArrayBuffer *abuf = bufobj->u.array_buffer;
    int length = 1 << typed_array_size_log2(p->class_id);
    uint32_t offset = p->u.typed_array->offset + idx * (uint32_t)length;
    int i;

    if (abuf->n_span == 0 || abuf->detached || abuf->data == NULL)
        return false;
    if ((int64_t)offset + length > (int64_t)abuf->byte_length)
        return false;
    for (i = abuf->n_span - 1; i >= 0; i--) {
        const JSArrayBufferSpan *s = &abuf->span[i];

        if (s->kind == JS_AB_SPAN_IMAGE) {
            if ((int64_t)offset + length <= (int64_t)s->offset ||
                (int64_t)offset >= (int64_t)s->offset + (int64_t)s->extent ||
                !js_ab_image_live(abuf, s, offset, (uint32_t)length))
                continue;
            *pval = js_ab_image_element(ctx, p, idx, s, offset, length);
            return true;
        }
        if (s->kind == JS_AB_SPAN_ANY) {
            /* A WRITE WHOSE OFFSET WAS UNKNOWN COVERS EVERY OFFSET IT COULD HAVE BEEN. Answering the block's
               byte here is the one error that may never be made: nothing was written at this offset, so the
               block holds a STALE byte, and handing it back deletes a world nothing contradicted — the
               wrong-narrowing move, arriving through a read instead of through a pin.
               FORKING OVER "DOES THIS SPAN COVER OFFSET n" IS REFUSED FOR THE REASON THE WRITE-SIDE FORK WAS:
               it is 2 arms per read and 2^k over a k-byte read, and the WFQ orders work rather than making an
               exponential finite. So the answer is OVER-GENERAL, which §Solver-half asks for — uncertainty
               keeps the arm, a contradicted branch is pruned sound-only, and an unproductive one starves. The
               cost is that a branch on a concrete framing byte now forks, which is work the ordering owns. */
            if (offset < s->offset || (int64_t)offset >= (int64_t)s->offset + (int64_t)s->extent)
                continue;
            if (!JS_IsUninitialized(s->unknown)) {
                /* THE VALUE'S OWN PROVENANCE WHERE THERE IS ONE, because a sink receiving this byte must name
                   the source a candidate is injected at, and the value is what was written. */
                *pval = js_dup(s->unknown);
                return true;
            }
            /* THE VALUE WAS CONCRETE, so what is unknown about this element is WHICH BYTE IT IS, and the
               derivation says exactly that: JSConcolicHooks.key_read is this engine's existing answer for
               `obj[x]` with an unknown x — "a lookup that names no particular slot" — and this is that lookup
               with the buffer as the base. Reaching for it rather than minting something new is why an unknown
               offset needs no new value-side mechanism at all. */
            {
                JSValue kv = g_concolic.key_read(ctx, JS_MKPTR(JS_TAG_OBJECT, bufobj), s->index);

                if (!JS_IsUninitialized(kv)) {
                    *pval = kv;
                    return true;
                }
            }
            continue;
        }
        if (s->offset != offset || s->extent != (uint32_t)length)
            continue;
        /* THE WITNESS, AND THE ONE DIRECTION IT MAY BE WRONG IN. Unchanged bytes mean nothing has overwritten
           this element since the unknown was stored, so the unknown still describes it. Changed bytes mean
           something did — fill, copyWithin, set, sort, reverse, a DataView setter, an Atomics store or an
           ordinary concrete assignment — and the span is spent. A concrete write that happens to store the
           SAME bytes leaves it standing, which answers with an unknown where the page wrote a number: that
           fabricates unknownness and §Solver-half prefers it, because uncertainty keeps the arm. The opposite
           error would hand back the example as a concrete number, which is the de-taint this record exists to
           prevent, and NO SPELLING OF THIS CHECK CAN MAKE THAT ERROR. */
        if (memcmp(&abuf->data[offset], s->witness, (size_t)length) != 0)
            continue;
        *pval = js_dup(s->unknown);
        return true;
    }
    return false;
}

/* 10.4.5.17 TypedArrayGetElement step 4's byteIndexInBuffer for the position `rel` bytes into the view's window:
   a Number where [[ByteOffset]] is stated, else an unknown derived from that offset, whose example is the
   example window's position where the offset has one. Owned. */
static JSValue js_ta_byte_index(JSContext *ctx, const JSTypedArray *ta, int64_t rel)
{
    JSValue ops[2], r;

    if (JS_IsUninitialized(ta->offset_unknown))
        return JS_NewInt64(ctx, (int64_t)ta->offset + rel);
    ops[0] = ta->offset_unknown;
    ops[1] = JS_NewInt64(ctx, rel);
    r = g_concolic.derived(ctx, "10.4.5.17 TypedArrayGetElement step 4 (byteIndexInBuffer is index * elementSize "
                                "+ offset)", ops, 2, js_concolic_held(ta->offset_unknown)
                           ? JS_NewInt64(ctx, (int64_t)ta->offset + rel) : JS_UNDEFINED);
    DCHECK(!JS_IsUninitialized(r), "a view's unknown byteOffset derived no unknown byteIndexInBuffer");
    return r;
}

/* 10.4.5.17 TypedArrayGetElement steps 2-6 where [[ByteOffset]] is an unknown: byteIndexInBuffer is one, so the
   element is an unknown derived from it and from the unknown the bytes at the example window are, if any, with
   that element as its example where the offset has one. `idx` is below the view's stored count. */
static JSValue js_ta_get_at_unknown_offset(JSContext *ctx, JSObject *p, uint32_t idx)
{
    JSTypedArray *ta = p->u.typed_array;
    int size_log2 = typed_array_size_log2(p->class_id);
    JSValue at = JS_UNDEFINED, ops[3], ex, r;
    int n = 2;
    bool got;

    DCHECK(!JS_IsUninitialized(ta->offset_unknown) && idx < p->u.array.count,
           "an element read at an unknown byteOffset was asked of a stated offset or past the stored count");
    got = (ta->buffer->u.array_buffer->n_span != 0 && js_ab_span_get(ctx, p, idx, &at)) ||
          js_get_fast_array_element_concrete(ctx, p, idx, &at);
    DCHECK(got, "an element below a view's stored count has no value at its example window");
    ops[0] = js_ta_byte_index(ctx, ta, (int64_t)idx << size_log2);
    ops[1] = js_int32(1 << size_log2);
    if (js_value_is_concolic(at)) {
        ops[n++] = at;
        ex = js_concolic_held(at) ? js_concolic_operand(ctx, at) : JS_UNDEFINED;
    } else {
        ex = js_dup(at);
    }
    if (!js_concolic_held(ta->offset_unknown)) {
        JS_FreeValue(ctx, ex);
        ex = JS_UNDEFINED;
    }
    r = g_concolic.derived(ctx, "10.4.5.17 TypedArrayGetElement step 6 (GetValueFromBuffer at byteIndexInBuffer)",
                           ops, n, ex);
    DCHECK(!JS_IsUninitialized(r), "an element read at an unknown byteIndexInBuffer derived no unknown");
    JS_FreeValue(ctx, ops[0]);
    JS_FreeValue(ctx, at);
    return r;
}

/* 10.4.5.18 TypedArraySetElement where [[ByteOffset]] is an unknown: steps 1-2 coerce, and step 3's store lands
   at an unknown byteIndexInBuffer, a write at an unknown offset (js_ab_span_set_any) that puts no byte in the
   block. Takes `val`; answers as JS_SetPropertyValue's typed-array arms do. */
static int js_ta_set_at_unknown_offset(JSContext *ctx, JSObject *p, uint32_t idx, JSValue val)
{
    JSTypedArray *ta = p->u.typed_array;
    JSValue num, at;

    if (p->class_id == JS_CLASS_BIG_INT64_ARRAY || p->class_id == JS_CLASS_BIG_UINT64_ARRAY)
        num = JS_ToBigIntFree(ctx, val);
    else
        num = JS_ToNumberFree(ctx, val);
    if (JS_IsException(num))
        return -1;
    JS_FreeValue(ctx, num);
    if (typed_array_is_immutable(p))
        return false;
    if (!typed_array_is_oob(p) && idx < p->u.array.count) {   /* step 3, IsValidIntegerIndex */
        at = js_ta_byte_index(ctx, ta, (int64_t)idx << typed_array_size_log2(p->class_id));
        js_ab_span_set_any(ctx, ta->buffer, at, JS_UNINITIALIZED);
        JS_FreeValue(ctx, at);
    }
    return true;
}

/* GROW THE LIST BY ONE AND HAND BACK THE NEW ENTRY. A LOST SPAN IS A SILENT DE-TAINT, which is why OOM here is
   fatal rather than a return code: dropping the record leaves an example byte in the block with nothing to say
   it is an example, so the next read hands the page a concrete number where it had unknown input and every arm
   behind the fork goes with it — arriving as an allocation failure nobody would connect to it. */
static JSArrayBufferSpan *js_ab_span_push(JSRuntime *rt, JSArrayBuffer *abuf)
{
    if (abuf->n_span == abuf->span_size) {
        int ns = abuf->span_size ? abuf->span_size * 2 : 8;
        JSArrayBufferSpan *nv = js_realloc_rt(rt, abuf->span, sizeof(*nv) * (size_t)ns);

        CHECK(nv != NULL,
              "OOM recording that a typed-array element holds unknown external input — dropping the span "
              "would leave its example byte in the data block as a concrete number");
        abuf->span = nv;
        abuf->span_size = ns;
    }
    return &abuf->span[abuf->n_span++];
}

/* RECORD THAT THIS ELEMENT'S BYTES ARE UNKNOWN EXTERNAL INPUT. Called AFTER the example (where there is one)
   has been written, because the witness is the bytes as they stand once the store is done. */
static void js_ab_span_set(JSContext *ctx, JSObject *bufobj, uint32_t offset, int length,
                           JSValueConst unknown)
{
    JSRuntime *rt = ctx->rt;
    JSArrayBuffer *abuf = bufobj->u.array_buffer;
    JSArrayBufferSpan *s = NULL;
    int i;

    DCHECK(length >= 1 && length <= 8,
           "a typed-array element span was recorded with a width no element has — 10.4.5.18's element types "
           "are 1, 2, 4 and 8 bytes wide and the witness buffer is sized for the largest");
    if (abuf->detached || abuf->data == NULL ||
        (int64_t)offset + length > (int64_t)abuf->byte_length)
        return;   /* 10.4.5.18 step 3 skipped the store, so there is nothing to describe */
    cow_capture_span(ctx, bufobj);
    for (i = abuf->n_span - 1; i >= 0; i--) {
        if (abuf->span[i].kind == JS_AB_SPAN_AT && abuf->span[i].offset == offset &&
            abuf->span[i].extent == (uint32_t)length) {
            s = &abuf->span[i];
            JS_FreeValueRT(rt, s->unknown);
            break;
        }
    }
    if (s == NULL)
        s = js_ab_span_push(rt, abuf);
    s->kind = JS_AB_SPAN_AT;
    s->offset = offset;
    s->extent = (uint32_t)length;
    memcpy(s->witness, &abuf->data[offset], (size_t)length);
    s->unknown = js_dup(unknown);
    s->index = JS_UNINITIALIZED;
    s->image = JS_UNINITIALIZED;
    s->image_pos = 0;
    s->codec = NULL;
}

/* RECORD THAT SOME ELEMENT OF THIS BUFFER HOLDS THIS VALUE AND NOTHING KNOWS WHICH — 10.4.5.18 with an unknown
   INDEX. Nothing is written to the block, because there is no offset to write at; the span IS the whole of what
   this store leaves behind, which is why it is recorded even though no byte moved.
   IT IS NEVER MERGED WITH AN EXISTING ONE, unlike the AT kind. Two unknown-index writes are two facts about two
   values, and a read covered by both answers with the later — picking the older would be a claim that the
   second write did not happen. */
static void js_ab_span_set_any(JSContext *ctx, JSObject *bufobj, JSValueConst index,
                               JSValueConst unknown)
{
    JSRuntime *rt = ctx->rt;
    JSArrayBuffer *abuf = bufobj->u.array_buffer;
    JSArrayBufferSpan *s;

    if (abuf->detached || abuf->data == NULL || abuf->byte_length <= 0)
        return;
    cow_capture_span(ctx, bufobj);
    s = js_ab_span_push(rt, abuf);
    s->kind = JS_AB_SPAN_ANY;
    s->offset = 0;
    s->extent = (uint32_t)abuf->byte_length;   /* see JSArrayBufferSpan: whole-buffer until a domain hook exists */
    memset(s->witness, 0, sizeof s->witness);
    s->unknown = JS_IsUninitialized(unknown) ? JS_UNINITIALIZED : js_dup(unknown);
    s->index = js_dup(index);
    s->image = JS_UNINITIALIZED;
    s->image_pos = 0;
    s->codec = NULL;
}

/* Record that [offset, offset + extent) of the block holds positions `pos`.. of `codec`'s output over
   `unknown`, as the bytes stand now. `codec` is a static string. */
static void js_ab_span_set_image(JSContext *ctx, JSArrayBuffer *abuf, uint32_t offset, uint32_t extent,
                                 JSValueConst unknown, uint32_t pos, const char *codec)
{
    JSArrayBufferSpan *s;
    JSValue image;

    DCHECK(extent > 0 && !abuf->detached && (int64_t)offset + extent <= (int64_t)abuf->byte_length,
           "an IMAGE span was recorded over bytes the buffer does not hold");
    CHECK(extent <= JS_STRING_LEN_MAX, "an IMAGE span is longer than a string can witness");
    image = js_new_string8_len(ctx, (const char *)abuf->data + offset, (int)extent);
    CHECK(!JS_IsException(image), "OOM witnessing a codec image of unknown input — a lost span would leave "
                                  "the example bytes in the block as concrete numbers");
    s = js_ab_span_push(ctx->rt, abuf);
    s->kind = JS_AB_SPAN_IMAGE;
    s->offset = offset;
    s->extent = extent;
    memset(s->witness, 0, sizeof s->witness);
    s->unknown = js_dup(unknown);
    s->index = JS_UNINITIALIZED;
    s->image = image;
    s->image_pos = pos;
    s->codec = codec;
}

/* Every owned field of one span, taken or released together. */
static void js_ab_span_dup_values(const JSArrayBufferSpan *s)
{
    (void)js_dup(s->unknown);
    (void)js_dup(s->index);
    (void)js_dup(s->image);
}

static void js_ab_span_free_values(JSRuntime *rt, JSArrayBufferSpan *s)
{
    JS_FreeValueRT(rt, s->unknown);
    JS_FreeValueRT(rt, s->index);
    JS_FreeValueRT(rt, s->image);
}

/* Whether span `sp` of `abuf` still describes the bytes it covers: an AT span whose witness stands, an IMAGE
   span whose image does, and every ANY span (it has no witness, see JSArrayBufferSpan). */
static bool js_ab_span_live(const JSArrayBuffer *abuf, const JSArrayBufferSpan *sp)
{
    if (abuf->detached || (uint64_t)sp->offset + sp->extent > (uint64_t)abuf->byte_length)
        return false;
    if (sp->kind == JS_AB_SPAN_AT)
        return memcmp(abuf->data + sp->offset, sp->witness, sp->extent) == 0;
    if (sp->kind == JS_AB_SPAN_IMAGE)
        return js_ab_image_live(abuf, sp, sp->offset, sp->extent);
    return true;
}

/* A block copy of [src_off, src_off + n) of `src` to `dst_off` of `dst` carries the spans describing those
   bytes, and runs before the bytes move so a source witness is read before an overlapping copy overwrites it.
   An AT span wholly inside the range keeps its witness, which the copy makes the destination's bytes; an IMAGE
   span is clipped to the range; an ANY span meeting the range covers the whole destination, its offset being
   unknown. A stale span describes nothing and is not carried. `src` may be `dst`. */
static void js_ab_spans_copy(JSContext *ctx, JSArrayBuffer *dst, uint32_t dst_off,
                             const JSArrayBuffer *src, uint32_t src_off, uint32_t n)
{
    int i, count = src->n_span;   /* spans this copy pushes onto `src` itself are not re-read */

    if (count == 0 || n == 0 || src->detached || dst->detached)
        return;
    for (i = 0; i < count; i++) {
        JSArrayBufferSpan s = src->span[i];   /* by value: a push may move the list when src == dst */
        uint64_t lo = s.offset > src_off ? s.offset : src_off;
        uint64_t hi = (uint64_t)s.offset + s.extent;
        JSArrayBufferSpan *d;

        if (hi > (uint64_t)src_off + n) hi = (uint64_t)src_off + n;
        if (lo >= hi)
            continue;
        if (s.kind == JS_AB_SPAN_AT) {
            if (lo != s.offset || hi != (uint64_t)s.offset + s.extent ||
                (int64_t)hi > (int64_t)src->byte_length ||
                memcmp(src->data + s.offset, s.witness, s.extent) != 0)
                continue;
            d = js_ab_span_push(ctx->rt, dst);
            *d = s;
            d->offset = dst_off + (s.offset - src_off);
            js_ab_span_dup_values(d);
        } else if (s.kind == JS_AB_SPAN_IMAGE) {
            JSValue img;

            if (!js_ab_image_live(src, &s, (uint32_t)lo, (uint32_t)(hi - lo)))
                continue;
            img = js_sub_string(ctx, JS_VALUE_GET_STRING(s.image), (int)(lo - s.offset), (int)(hi - s.offset));
            CHECK(!JS_IsException(img), "OOM carrying a codec image across a block copy — a lost span would "
                                        "leave the copied example bytes as concrete numbers");
            d = js_ab_span_push(ctx->rt, dst);
            *d = s;
            d->offset = dst_off + (uint32_t)(lo - src_off);
            d->extent = (uint32_t)(hi - lo);
            d->image_pos = s.image_pos + (uint32_t)(lo - s.offset);
            d->image = img;
            (void)js_dup(d->unknown);
        } else {
            DCHECK(s.kind == JS_AB_SPAN_ANY, "a buffer span has a kind this copy does not carry");
            if (dst->byte_length <= 0)
                continue;
            d = js_ab_span_push(ctx->rt, dst);
            *d = s;
            d->offset = 0;
            d->extent = (uint32_t)dst->byte_length;
            js_ab_span_dup_values(d);
        }
    }
}

/* THE BUFFER'S STORAGE IS GOING, SO EVERY SPAN NAMING IT GOES TOO — one counted reference per field that has
   one, exactly as JSBufferStateView's is released in JS_BufferStateFree. */
static void js_ab_spans_free(JSRuntime *rt, JSArrayBuffer *abuf)
{
    int i;

    for (i = 0; i < abuf->n_span; i++) {
        js_ab_span_free_values(rt, &abuf->span[i]);
    }
    js_free_rt(rt, abuf->span);
    abuf->span = NULL;
    abuf->n_span = 0;
    abuf->span_size = 0;
}

static void js_array_buffer_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSArrayBuffer *abuf = p->u.array_buffer;
    struct list_head *el, *el1;

    if (abuf) {
        js_ab_spans_free(rt, abuf);
        JS_FreeValueRT(rt, abuf->length_unknown);
        /* The ArrayBuffer finalizer may be called before the typed
           array finalizers using it, so abuf->array_list is not
           necessarily empty. */
        list_for_each_safe(el, el1, &abuf->array_list) {
            JSTypedArray *ta;
            JSObject *p1;

            ta = list_entry(el, JSTypedArray, link);
            ta->link.prev = NULL;
            ta->link.next = NULL;
            p1 = ta->obj;
            /* Note: the typed array length and offset fields are not modified */
            if (p1->class_id != JS_CLASS_DATAVIEW) {
                p1->u.array.count = 0;
                p1->u.array.u.ptr = NULL;
            }
        }
        if (abuf->free_func)
            abuf->free_func(rt, abuf->opaque, abuf->data);
        JS_FreeValueRT(rt, abuf->waiter_lists);
        js_free_rt(rt, abuf);
    }
}

/* The reference to the block's WaiterList store this view holds. */
static void js_shared_array_buffer_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSArrayBuffer *abuf = JS_VALUE_GET_OBJ(val)->u.array_buffer;

    if (abuf)
        JS_MarkValue(rt, abuf->waiter_lists, mark_func);
}

static JSValue js_array_buffer_isView(JSContext *ctx,
                                      JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSObject *p;

    if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_OBJECT) {
        p = JS_VALUE_GET_OBJ(argv[0]);
        return js_bool(is_typed_array(p->class_id) ||
                       p->class_id == JS_CLASS_DATAVIEW);
    }
    return JS_FALSE;
}

static const JSCFunctionListEntry js_array_buffer_funcs[] = {
    JS_CFUNC_DEF("isView", 1, js_array_buffer_isView ),
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
};

static JSValue JS_ThrowTypeErrorDetachedArrayBuffer(JSContext *ctx)
{
    return JS_ThrowTypeError(ctx, "ArrayBuffer is detached");
}

static JSValue JS_ThrowTypeErrorImmutableArrayBuffer(JSContext *ctx)
{
    return JS_ThrowTypeError(ctx, "ArrayBuffer is immutable");
}

static JSValue JS_ThrowTypeErrorArrayBufferOOB(JSContext *ctx)
{
    return JS_ThrowTypeError(ctx, "ArrayBuffer is detached or resized");
}

// #sec-get-arraybuffer.prototype.detached
static JSValue js_array_buffer_get_detached(JSContext *ctx,
                                            JSValueConst this_val)
{
    JSArrayBuffer *abuf = JS_GetOpaque2(ctx, this_val, JS_CLASS_ARRAY_BUFFER);
    if (!abuf)
        return JS_EXCEPTION;
    if (abuf->shared)
        return JS_ThrowTypeError(ctx, "detached called on SharedArrayBuffer");
    return js_bool(abuf->detached);
}

static JSValue js_array_buffer_get_immutable(JSContext *ctx,
                                             JSValueConst this_val)
{
    JSArrayBuffer *abuf = JS_GetOpaque2(ctx, this_val, JS_CLASS_ARRAY_BUFFER);
    if (!abuf)
        return JS_EXCEPTION;
    return js_bool(abuf->immutable);
}

/* A length the buffer's bytes give no number for: derived from the unknown whose codec output they are
   (JSArrayBuffer.length_unknown) under the getter's algorithm `op`, with `example` (a byte or element count) as
   its example. `offset` and `size_log2` are the view's window, 0 for the buffer itself. The bytes are the
   codec's run over the unknown's example only when that example is a held primitive; over any other unknown
   the buffer holds no example bytes, so the length has no example either. */
static JSValue js_ab_length_derive(JSContext *ctx, const JSArrayBuffer *abuf, const char *op, uint32_t offset,
                                   int size_log2, int64_t example)
{
    JSValue ops[3], r, ex;
    bool held;

    DCHECK(!JS_IsUninitialized(abuf->length_unknown) && !abuf->detached,
           "a length was derived for a buffer whose length is stated");
    ex = js_concolic_operand(ctx, abuf->length_unknown);
    held = js_concolic_codec_operand(abuf->length_unknown, ex);
    JS_FreeValue(ctx, ex);
    DCHECK(held || abuf->byte_length == 0, "a buffer holds example bytes for a codec output whose unknown has no "
                                           "primitive example to have run the codec over");
    ops[0] = abuf->length_unknown;
    ops[1] = js_uint32(offset);
    ops[2] = js_int32(size_log2);
    r = g_concolic.derived(ctx, op, ops, 3, held ? js_int64(example) : JS_UNDEFINED);
    DCHECK(!JS_IsUninitialized(r), "a buffer's unknown length derived no unknown");
    return r;
}

static JSValue js_array_buffer_get_byteLength(JSContext *ctx,
                                              JSValueConst this_val,
                                              int class_id)
{
    JSArrayBuffer *abuf = JS_GetOpaque2(ctx, this_val, class_id);
    if (!abuf)
        return JS_EXCEPTION;
    if (!JS_IsUninitialized(abuf->length_unknown))
        return js_ab_length_derive(ctx, abuf, "25.1.6.1 get ArrayBuffer.prototype.byteLength", 0, 0,
                                   abuf->byte_length);
    /* return 0 if detached */
    return js_uint32(abuf->byte_length);
}

static JSValue js_array_buffer_get_maxByteLength(JSContext *ctx,
                                                 JSValueConst this_val,
                                                 int class_id)
{
    JSArrayBuffer *abuf = JS_GetOpaque2(ctx, this_val, class_id);
    if (!abuf)
        return JS_EXCEPTION;
    if (array_buffer_is_resizable(abuf))
        return js_uint32(abuf->max_byte_length);
    return js_uint32(abuf->byte_length);
}

static JSValue js_array_buffer_get_resizable(JSContext *ctx,
                                             JSValueConst this_val,
                                             int class_id)
{
    JSArrayBuffer *abuf = JS_GetOpaque2(ctx, this_val, class_id);
    if (!abuf)
        return JS_EXCEPTION;
    return js_bool(array_buffer_is_resizable(abuf));
}

void JS_DetachArrayBuffer(JSContext *ctx, JSValueConst obj)
{
    JSArrayBuffer *abuf = JS_GetOpaque(obj, JS_CLASS_ARRAY_BUFFER);
    struct list_head *el;

    if (!abuf || abuf->detached)
        return;
    /* forced-exec TIME-TRAVEL: a detach FREES the storage a byte entry names and empties every view over it,
       which is the same lifetime fact a resize is and is refused at the same place. cow_state_save's NULL-bytes
       CHECK names it too, and only for a buffer already captured — `buf.transfer()` before this flow ever wrote
       a byte reaches that CHECK never. */
    cow_capture_buffer_lifetime(ctx, obj);
    if (abuf->free_func) {
        abuf->free_func(ctx->rt, abuf->opaque, abuf->data);
        /* The backing data has been released. Do not release it again when
           the detached ArrayBuffer object is later finalized. */
        abuf->free_func = NULL;
        abuf->opaque = NULL;
    }
    abuf->data = NULL;
    abuf->byte_length = 0;
    abuf->detached = true;
    /* §25.1.3.5 DetachArrayBuffer step 5: [[ArrayBufferByteLength]] is 0, a stated length */
    JS_FreeValue(ctx, abuf->length_unknown);
    abuf->length_unknown = JS_UNINITIALIZED;
    /* Step 4's [[ArrayBufferData]] is null, so the facts about its bytes go with them: a transfer moves the
       spans into its data holder rather than leaving a copy behind. */
    js_ab_spans_free(ctx->rt, abuf);

    list_for_each(el, &abuf->array_list) {
        JSTypedArray *ta;
        JSObject *p;

        ta = list_entry(el, JSTypedArray, link);
        p = ta->obj;
        /* Note: the typed array length and offset fields are not modified */
        if (p->class_id != JS_CLASS_DATAVIEW) {
            p->u.array.count = 0;
            p->u.array.u.ptr = NULL;
        }
    }
}

int JS_IsImmutableArrayBuffer(JSValueConst obj)
{
    JSArrayBuffer *abuf = JS_GetOpaque(obj, JS_CLASS_ARRAY_BUFFER);
    if (!abuf)
        return -1;
    return abuf->immutable;
}

int JS_SetImmutableArrayBuffer(JSValueConst obj, bool immutable)
{
    JSArrayBuffer *abuf = JS_GetOpaque(obj, JS_CLASS_ARRAY_BUFFER);
    if (!abuf)
        return -1;
    abuf->immutable = immutable;
    return 0;
}

/* get an ArrayBuffer or SharedArrayBuffer */
static JSArrayBuffer *js_get_array_buffer(JSContext *ctx, JSValueConst obj)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        goto fail;
    p = JS_VALUE_GET_OBJ(obj);
    if (p->class_id != JS_CLASS_ARRAY_BUFFER &&
        p->class_id != JS_CLASS_SHARED_ARRAY_BUFFER) {
    fail:
        JS_ThrowTypeErrorInvalidClass(ctx, JS_CLASS_ARRAY_BUFFER);
        return NULL;
    }
    return p->u.array_buffer;
}

/* return NULL if exception. WARNING: any JS call can detach the
   buffer and render the returned pointer invalid */
uint8_t *JS_GetArrayBuffer(JSContext *ctx, size_t *psize, JSValueConst obj)
{
    JSArrayBuffer *abuf = js_get_array_buffer(ctx, obj);
    if (!abuf)
        goto fail;
    if (abuf->detached) {
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    *psize = abuf->byte_length;
    return abuf->data;
 fail:
    *psize = 0;
    return NULL;
}

/* THE BUFFER'S BYTES, READ — see quickjs.h. NOT JS_GetArrayBuffer, which is an operation: it THROWS on a
   detached buffer and on a non-buffer, and the COW swap that calls this has no flow base to run an exception on,
   exactly as JS_GetOwnSlotDesc is not [[GetOwnProperty]]. A DETACHED buffer answers NULL/0 rather than aborting,
   because a detached buffer holds no bytes and there is nothing for a flow to have written. */
const uint8_t *JS_GetBufferBytes(JSValueConst obj, uint32_t *plen)
{
    JSObject *p;
    JSArrayBuffer *abuf;

    *plen = 0;
    DCHECK(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT,
           "a COW delta holds a buffer's bytes on something that is not an object");
    p = JS_VALUE_GET_OBJ(obj);
    DCHECK(p->class_id == JS_CLASS_ARRAY_BUFFER || p->class_id == JS_CLASS_SHARED_ARRAY_BUFFER,
           "a COW delta holds a buffer's bytes on a class that owns no ArrayBuffer storage — the capture named "
           "a VIEW where it must name the buffer that owns the bytes");
    abuf = p->u.array_buffer;
    if (abuf->detached)
        return NULL;
    DCHECK(abuf->byte_length >= 0, "an ArrayBuffer with a negative byte length");
    *plen = (uint32_t)abuf->byte_length;
    return abuf->data;
}

static bool array_buffer_is_resizable(const JSArrayBuffer *abuf)
{
    return abuf->max_byte_length >= 0;
}

static void js_array_buffer_update_typed_arrays(JSArrayBuffer *abuf)
{
    uint32_t size_log2, size_elem;
    struct list_head *el;
    JSTypedArray *ta;
    JSObject *p;
    uint8_t *data;
    int64_t len;

    len = abuf->byte_length;
    data = abuf->data;
    list_for_each(el, &abuf->array_list) {
        ta = list_entry(el, JSTypedArray, link);
        p = ta->obj;
        if (p->class_id == JS_CLASS_DATAVIEW) {
            if (ta->track_rab && ta->offset < len)
                ta->length = len - ta->offset;
            continue;
        }
        p->u.array.count = 0;
        p->u.array.u.ptr = NULL;
        size_log2 = typed_array_size_log2(p->class_id);
        size_elem = 1 << size_log2;
        if (ta->track_rab) {
            if (len >= (int64_t)ta->offset + size_elem) {
                p->u.array.count = (len - ta->offset) >> size_log2;
                p->u.array.u.ptr = &data[ta->offset];
            }
        } else {
            if (len >= (int64_t)ta->offset + ta->length) {
                p->u.array.count = ta->length >> size_log2;
                p->u.array.u.ptr = &data[ta->offset];
            }
        }
    }
}

/* A BUFFER'S WHOLE STORAGE STATE — see quickjs.h for what it holds and why the views are in it. */
typedef struct JSBufferStateView {
    JSObject *obj;      /* ONE COUNTED REFERENCE, held for the life of the blob: a delta outlives a flow, and a
                           view the page dropped would otherwise be a dangling name in it */
    uint32_t length;    /* the view's cached ta->length — the one window the re-derivation cannot reach */
} JSBufferStateView;

typedef struct JSBufferState {
    int byte_length;
    int max_byte_length;
    uint8_t detached;
    uint8_t immutable;      /* RECORDED TO BE ASSERTED, never to be written back — see the restore */
    uint8_t engine_owned;   /* the block was ours (js_array_buffer_free), so a restore may hand one back */
    uint8_t *data;          /* an OWNED copy of byte_length bytes; NULL iff detached */
    int n_view;
    JSBufferStateView *view;
    /* THE SPANS, BECAUSE THEY ARE PART OF THE STORAGE STATE AND NOT A SECOND ENTRY BESIDE IT — see
       JSArrayBufferSpan. A span is a live fact ABOUT these bytes and has to SWAP WITH THEM: a flow that stored
       an unknown at offset 3 and parked must resume finding it there, and a sibling that never stored one must
       not see it at all. Two entries over one buffer have an ORDER in the head and apply replays forward, which
       is the same argument that put the extent, the detached bit and the per-view window in this one blob; and
       JS_BufferStateRestore's immutable DCHECK already tells a future reader to carry a new fact "in the
       buffer-state blob's restore beside the extent", which is exactly this. Each holds ONE COUNTED REFERENCE,
       released in JS_BufferStateFree like the views'. */
    int n_span;
    JSArrayBufferSpan *span;
    JSValue length_unknown;   /* JSArrayBuffer.length_unknown, one counted reference when set */
} JSBufferState;

void JS_BufferStateFree(JSRuntime *rt, void *blob)
{
    JSBufferState *st = blob;
    int i;

    if (!st)
        return;
    for (i = 0; i < st->n_view; i++)
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, st->view[i].obj));
    for (i = 0; i < st->n_span; i++) {
        js_ab_span_free_values(rt, &st->span[i]);
    }
    js_free_rt(rt, st->span);
    js_free_rt(rt, st->view);
    js_free_rt(rt, st->data);
    JS_FreeValueRT(rt, st->length_unknown);
    js_free_rt(rt, st);
}

/* THE BUFFER, AS THIS FLOW FOUND IT. Answers NULL on OOM only — the caller CHECKs it, because a lost baseline
   leaks one flow's storage into every sibling. */
void *JS_BufferStateSave(JSContext *ctx, JSValueConst obj)
{
    JSRuntime *rt = ctx->rt;
    JSObject *p;
    JSArrayBuffer *abuf;
    JSBufferState *st;
    struct list_head *el;
    JSTypedArray *ta;
    int n = 0, i = 0;

    DCHECK(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT,
           "a COW delta is saving the storage state of something that is not an object");
    p = JS_VALUE_GET_OBJ(obj);
    DCHECK(p->class_id == JS_CLASS_ARRAY_BUFFER || p->class_id == JS_CLASS_SHARED_ARRAY_BUFFER,
           "a COW delta is saving the storage state of a class that owns no ArrayBuffer storage — the capture "
           "named a VIEW where it must name the buffer that owns the bytes");
    abuf = p->u.array_buffer;
    DCHECK(abuf->byte_length >= 0, "an ArrayBuffer with a negative byte length");
    DCHECK(abuf->detached == (abuf->data == NULL),
           "an ArrayBuffer's detached bit and its data block disagree — §25.1.3.4 IsDetachedBuffer is the one "
           "question 'is [[ArrayBufferData]] null', so a restore reading one of them would decide the other "
           "wrong");
    st = js_mallocz_rt(rt, sizeof(*st));
    if (!st)
        return NULL;
    st->byte_length = abuf->byte_length;
    st->length_unknown = js_dup(abuf->length_unknown);
    st->max_byte_length = abuf->max_byte_length;
    st->detached = abuf->detached;
    st->immutable = abuf->immutable;
    st->engine_owned = !abuf->detached && abuf->free_func == js_array_buffer_free;
    if (!abuf->detached) {
        st->data = js_malloc_rt(rt, max_int(abuf->byte_length, 1));
        if (!st->data)
            goto fail;
        memcpy(st->data, abuf->data, abuf->byte_length);
    }
    /* ONLY THE VIEWS WHOSE WINDOW IS NOT RE-DERIVABLE, which is a test and not a shortcut: every other view's
       cached extent is a function of the buffer's, so recording it would be a second copy of a computed fact
       and would cost a reference per view on the hot byte-write path. What is left is exactly the field
       js_array_buffer_update_typed_arrays declines to write — a LENGTH-TRACKING DATAVIEW's `ta->length`, which
       that update leaves alone whenever the view's offset lands at or past the buffer's length, so the value
       it keeps is history rather than a derivation, and js_dv_get_step's bounds test reads it back. */
    list_for_each(el, &abuf->array_list) {
        ta = list_entry(el, JSTypedArray, link);
        if (ta->obj->class_id == JS_CLASS_DATAVIEW && ta->track_rab)
            n++;
    }
    if (n > 0) {
        st->view = js_malloc_rt(rt, sizeof(*st->view) * (size_t)n);
        if (!st->view)
            goto fail;
        list_for_each(el, &abuf->array_list) {
            ta = list_entry(el, JSTypedArray, link);
            DCHECK(ta->obj != NULL, "a view is on a buffer's list with no back pointer to its own object — the "
                                    "blob would hold a reference on nothing and restore a window to nobody");
            if (ta->obj->class_id != JS_CLASS_DATAVIEW || !ta->track_rab)
                continue;
            DCHECK(i < n, "a buffer's view list grew between the count and the walk");
            st->view[i].obj = ta->obj;
            st->view[i].length = ta->length;
            (void)js_dup(JS_MKPTR(JS_TAG_OBJECT, ta->obj));
            /* PUBLISHED PER ENTRY, not after the loop: the count is what the free walks, so a bail-out that
               left it at 0 would drop every reference this loop had already taken. */
            st->n_view = ++i;
        }
        DCHECK(i == n, "a buffer's view list changed under the walk that records it");
    }
    /* THE SPANS ARE COPIED, NOT MOVED, because this blob is the baseline and a restore may put it back more
       than once — the flow it belongs to can be unapplied and reapplied at every context switch. Publishing
       n_span per entry is the views' rule for the views' reason: the count is what the free walks, so a
       bail-out that left it at 0 would drop every reference already taken. */
    if (abuf->n_span > 0) {
        st->span = js_malloc_rt(rt, sizeof(*st->span) * (size_t)abuf->n_span);
        if (!st->span)
            goto fail;
        for (i = 0; i < abuf->n_span; i++) {
            st->span[i] = abuf->span[i];
            /* EVERY OWNED FIELD, not just the first. An ANY span holds a second reference — the unknown OFFSET
               the read derives from — and a copy that dup'd one of the two would hand the baseline a value
               whose count the free then over-releases. */
            js_ab_span_dup_values(&abuf->span[i]);
            st->n_span = i + 1;
        }
    }
    return st;
 fail:
    JS_BufferStateFree(rt, st);
    return NULL;
}

/* …AND PUT BACK. The three operations that move storage are put back by their inverses, and then every DERIVED
   per-view field is re-derived by the engine's own js_array_buffer_update_typed_arrays rather than by a second
   stored copy of a computed fact — see quickjs.h for which single field that re-derivation cannot reach. */
void JS_BufferStateRestore(JSContext *ctx, JSValueConst obj, void *blob)
{
    JSRuntime *rt = ctx->rt;
    JSObject *p;
    JSArrayBuffer *abuf;
    JSBufferState *st = blob;
    uint8_t *data;
    int i;

    DCHECK(st != NULL, "a buffer's storage state was re-applied before any unapply had recorded one — the "
                       "context switch that parked this flow did not run");
    DCHECK(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT,
           "a COW delta is putting a buffer's storage state back on something that is not an object");
    p = JS_VALUE_GET_OBJ(obj);
    DCHECK(p->class_id == JS_CLASS_ARRAY_BUFFER || p->class_id == JS_CLASS_SHARED_ARRAY_BUFFER,
           "a COW delta is putting a buffer's storage state back on a class that owns no ArrayBuffer storage");
    abuf = p->u.array_buffer;
    DCHECK(st->max_byte_length == abuf->max_byte_length,
           "a buffer's maxByteLength changed under a delta — no operation in §25.1 or §25.2 writes "
           "[[ArrayBufferMaxByteLength]] after allocation, so whatever moved it is not a resize and this blob "
           "describes a different buffer than the one it is being put back into");
    /* IMMUTABILITY IS RECORDED AND ASSERTED, NOT RESTORED, and that is a positive statement rather than a gap:
       no JS operation flips an EXISTING buffer's immutable bit — `transferToImmutable` sets it on the buffer it
       creates, which is this flow's own object — so a flow cannot make one visible to a sibling and there is
       nothing per-flow to swap. The day a path does write it, this fires instead of that write standing in the
       baseline for every sibling, which is what a bit with a writer and no entry would do. */
    DCHECK(st->immutable == abuf->immutable,
           "a flow changed a shared ArrayBuffer's IMMUTABLE bit: nothing in §25.1 writes it after construction, "
           "so a path was added that does — carry the bit in the buffer-state blob's restore beside the extent, "
           "or the arm that set it stands for every sibling");
    if (abuf->shared) {
        DCHECK(!st->detached,
               "a SharedArrayBuffer was recorded DETACHED — §25.1.3.5 DetachArrayBuffer step 1 is \"Assert: "
               "IsSharedArrayBuffer(arrayBuffer) is false.\", so nothing can have detached this and the blob "
               "names a state no operation produces");
        DCHECK(!abuf->detached && abuf->data != NULL, "a SharedArrayBuffer is detached");
        /* §25.2.5.3 SharedArrayBuffer.prototype.grow moves no storage: js_array_buffer_constructor3 commits the
           max-sized block upfront and the grow arm sets byte_length alone, so the extent is put back the same
           way it was taken and there is nothing to reallocate.
           …WHICH IS TRUE ONLY THROUGH sab_alloc, and this says so rather than leaning on it. The non-sab arm of
           that constructor allocates the INITIAL length, so growing such a buffer already writes past its own
           block — the page's own byte write does it one byte at a time and this would do it in one memcpy,
           which is the same defect with a worse symptom. It fires only where the engine is already corrupting
           and it names what to build. */
        DCHECK(abuf->free_func == js_shared_block_release,
               "a SharedArrayBuffer's storage is not a Shared Data Block, which holds maxByteLength from the start");
        memcpy(abuf->data, st->data, st->byte_length);
        abuf->byte_length = st->byte_length;
    } else if (st->detached) {
        if (!abuf->detached) {
            /* §25.1.3.5 steps 4 and 5, and the engine's own release beside them — the same three writes
               JS_DetachArrayBuffer makes, so a flow's transfer replays as the detach it was. */
            if (abuf->free_func) {
                abuf->free_func(rt, abuf->opaque, abuf->data);
                abuf->free_func = NULL;
                abuf->opaque = NULL;
            }
            abuf->data = NULL;
            abuf->byte_length = 0;
            abuf->detached = true;
        }
        /* else: already detached, and no operation re-attaches — there is nothing left to put back */
    } else if (!abuf->detached && st->byte_length == abuf->byte_length) {
        /* THE CONTENTS HALF ALONE — the storage did not move, so nothing is reallocated and an EMBEDDER's block
           is written in place exactly as the page writes it. This is the hot path: a flow that only wrote bytes
           has a blob whose extent equals the buffer's. */
        memcpy(abuf->data, st->data, st->byte_length);
    } else {
        /* THE STORAGE MOVES — a resize's realloc (§25.1.6.6 steps 14 and 15) or the re-attach that inverts a
           detach. Only a block this engine allocated can be handed back: an embedder gave us memory and a
           free_func, and a restore that replaced it with js_malloc'd storage would leave the buffer holding a
           block the embedder never gave and never hears about. */
        CHECK(abuf->detached ? st->engine_owned : abuf->free_func == js_array_buffer_free,
              "a flow moved the storage of an ArrayBuffer whose block an EMBEDDER owns (a non-engine "
              "free_func): the swap can only hand back storage it allocated itself, so build the "
              "embedder-storage arm of the buffer-state entry — the blob must carry the embedder's free_func "
              "and opaque and re-present the SAME block rather than a copy of it");
        data = js_realloc_rt(rt, abuf->data, max_int(st->byte_length, 1));
        CHECK(data, "cow: OOM restoring an ArrayBuffer's storage — a lost extent leaks one flow's resize into "
                    "every sibling, and there is no shorter buffer that would be the right answer");
        memcpy(data, st->data, st->byte_length);
        abuf->data = data;
        abuf->byte_length = st->byte_length;
        abuf->detached = false;
        abuf->free_func = js_array_buffer_free;
        abuf->opaque = NULL;
    }
    js_array_buffer_update_typed_arrays(abuf);
    /* AND THE ONE WINDOW THAT RE-DERIVATION CANNOT REACH, put back AFTER the update because the update is what
       would otherwise leave the flow's value standing. Where the update DID write this field it wrote the same
       number, so this is an overwrite only in the case it exists for. */
    for (i = 0; i < st->n_view; i++) {
        JSObject *vp = st->view[i].obj;
        DCHECK(vp->u.typed_array != NULL,
               "a view the blob holds a reference on has no JSTypedArray record — the reference is what makes "
               "that impossible, so whatever freed the record did it under a live object");
        DCHECK(vp->class_id == JS_CLASS_DATAVIEW && vp->u.typed_array->track_rab,
               "a recorded view is not a length-tracking DataView — every other view's cached extent is derived "
               "from the buffer's, so writing one back over what the re-derivation just computed would install "
               "a window this buffer does not have");
        DCHECK(vp->u.typed_array->buffer == p,
               "a view recorded on one buffer's list names a different buffer — a view's [[ViewedArrayBuffer]] "
               "is fixed at construction, so the blob is being put back into a buffer it did not come from");
        vp->u.typed_array->length = st->view[i].length;
    }
    /* AND THE SPANS, PUT BACK WITH THE BYTES THEY DESCRIBE — see JSArrayBufferSpan. This is the whole reason
       they live in the storage state rather than anywhere else: the bytes above and the spans here are restored
       in ONE operation, so a witness can never be compared against bytes from a different flow. Restoring the
       bytes alone would leave this flow's spans standing over a sibling's bytes, where every witness mismatches
       and every unknown silently becomes the concrete byte underneath it — the de-taint, arriving through the
       context switch instead of through the store.
       THE LIVE LIST IS RELEASED AND THE BLOB'S IS COPIED, never moved: the baseline is put back at every
       unapply, so a move would empty the blob on the first one and every later restore would answer "no
       unknowns" for a flow that had them. */
    JS_FreeValueRT(rt, abuf->length_unknown);
    abuf->length_unknown = js_dup(st->length_unknown);
    js_ab_spans_free(rt, abuf);
    if (st->n_span > 0) {
        abuf->span = js_malloc_rt(rt, sizeof(*abuf->span) * (size_t)st->n_span);
        CHECK(abuf->span,
              "cow: OOM restoring the record of which of an ArrayBuffer's bytes are unknown external input — "
              "dropping it would hand the page the example bytes as concrete numbers and delete every fork "
              "behind them, which is the one failure this record exists to prevent");
        abuf->span_size = st->n_span;
        for (i = 0; i < st->n_span; i++) {
            abuf->span[i] = st->span[i];
            js_ab_span_dup_values(&st->span[i]);
            abuf->n_span = i + 1;
        }
    }
}


// ES #sec-arraybuffer.prototype.transfer
static JSValue js_array_buffer_transfer(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv, int magic)
{
    JSArrayBuffer *abuf;
    uint64_t new_len, old_len, max_len, *pmax_len;
    uint8_t *bs, *new_bs;
    JSValue ret, length_unknown = JS_UNINITIALIZED;
    bool len_stated;

    abuf = JS_GetOpaque2(ctx, this_val, JS_CLASS_ARRAY_BUFFER);
    if (!abuf)
        return JS_EXCEPTION;
    if (abuf->shared)
        return JS_ThrowTypeError(ctx, "cannot transfer a SharedArrayBuffer");
    // Spec (ArrayBufferCopyAndDetach): the newLength argument must be
    // coerced (its valueOf observed) before the buffer's detachability /
    // immutability is checked, so side effects of coercion are visible.
    len_stated = !(argc < 1 || JS_IsUndefined(argv[0]));
    if (!len_stated)
        new_len = abuf->byte_length;
    else if (JS_ToIndex(ctx, &new_len, argv[0]))
        return JS_EXCEPTION;
    if (abuf->immutable)
        return JS_ThrowTypeErrorImmutableArrayBuffer(ctx);
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    pmax_len = NULL;
    if (magic == JS_ARRAY_BUFFER_TRANSFER) {
        if (array_buffer_is_resizable(abuf)) { // carry over maxByteLength
            max_len = abuf->max_byte_length;
            if (new_len > max_len)
                return JS_ThrowTypeError(ctx, "invalid array buffer length");
            // TODO(bnoordhuis) support externally managed RABs
            if (abuf->free_func == js_array_buffer_free)
                pmax_len = &max_len;
        }
    }
    /* forced-exec TIME-TRAVEL: a transfer DETACHES this buffer, and the arm below does it inline rather than
       through JS_DetachArrayBuffer — so the declaration is made once here, above both arms, after every throw
       and before either of them touches the storage. */
    cow_capture_buffer_lifetime(ctx, this_val);
    /* create an empty AB */
    if (new_len == 0) {
        if (!len_stated)
            length_unknown = js_dup(abuf->length_unknown);
        JS_DetachArrayBuffer(ctx, this_val);
        ret = js_array_buffer_constructor2(ctx, 0, pmax_len,
                                           JS_CLASS_ARRAY_BUFFER);
        if (!JS_IsException(ret)) {
            JS_VALUE_GET_OBJ(ret)->u.array_buffer->length_unknown = length_unknown;
            length_unknown = JS_UNINITIALIZED;
        }
        JS_FreeValue(ctx, length_unknown);
        goto fini;
    }
    bs = abuf->data;
    old_len = abuf->byte_length;
    /* if length mismatch, realloc. Otherwise, use the same backing buffer. */
    if (new_len != old_len) {
        new_bs = js_realloc(ctx, bs, new_len);
        if (!new_bs)
            return JS_EXCEPTION;
        bs = new_bs;
        if (new_len > old_len)
            memset(bs + old_len, 0, new_len - old_len);
    }
    /* §25.1.3.3 ArrayBufferCopyAndDetach copies the bytes, so the facts about them go with them: the spans
       over the copied prefix, and a length nobody stated when newLength was absent. */
    if (!len_stated)
        length_unknown = js_dup(abuf->length_unknown);
    /* neuter the backing buffer */
    abuf->data = NULL;
    abuf->byte_length = 0;
    abuf->detached = true;
    JS_FreeValue(ctx, abuf->length_unknown);
    abuf->length_unknown = JS_UNINITIALIZED;
    js_array_buffer_update_typed_arrays(abuf);
    ret = js_array_buffer_constructor3(ctx, new_len, pmax_len,
                                       JS_CLASS_ARRAY_BUFFER, bs,
                                       abuf->free_func, NULL,
                                       /*alloc_flag*/false);
    if (!JS_IsException(ret)) {
        JSArrayBuffer *nabuf = JS_VALUE_GET_OBJ(ret)->u.array_buffer, src = *abuf;

        src.data = bs;
        src.byte_length = (int)(new_len < old_len ? new_len : old_len);
        src.detached = false;
        js_ab_spans_copy(ctx, nabuf, 0, &src, 0, (uint32_t)src.byte_length);
        nabuf->length_unknown = length_unknown;
        length_unknown = JS_UNINITIALIZED;
    }
    JS_FreeValue(ctx, length_unknown);
    js_ab_spans_free(ctx->rt, abuf);
fini:
    if (magic == JS_ARRAY_BUFFER_TRANSFER_TO_IMMUTABLE) {
        if (JS_IsException(ret))
            return JS_EXCEPTION;
        abuf = JS_GetOpaque2(ctx, ret, JS_CLASS_ARRAY_BUFFER);
        if (!abuf)
            return JS_EXCEPTION;
        abuf->immutable = true;
    }
    return ret;
}

/* resize/grow step 2-3 and transfer's RequireInternalSlot: the receiver checks the spec orders BEFORE
   ToIndex(newLength), which is why they are a precheck rather than the head of the body — a coerce-then-compute
   machine coerces first, and `ArrayBuffer.prototype.resize.call({}, {valueOf(){...}})` must throw without
   running that valueOf. */
static int js_array_buffer_resize_precheck(JSContext *ctx, const JSStepHdr *h)
{
    JSValueConst this_val = h->this_val;
    int class_id = h->def->body_magic;
    JSArrayBuffer *abuf = JS_GetOpaque2(ctx, this_val, class_id);
    if (!abuf)
        return -1;
    if (abuf->immutable)
        return JS_ThrowTypeErrorImmutableArrayBuffer(ctx), -1;
    return 0;
}

static int js_array_buffer_transfer_precheck(JSContext *ctx, const JSStepHdr *h)
{
    JSValueConst this_val = h->this_val;
    JSArrayBuffer *abuf = JS_GetOpaque2(ctx, this_val, JS_CLASS_ARRAY_BUFFER);
    if (!abuf)
        return -1;
    if (abuf->shared)
        return JS_ThrowTypeError(ctx, "cannot transfer a SharedArrayBuffer"), -1;
    return 0;
}

static JSValue js_array_buffer_resize(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int class_id)
{
    JSArrayBuffer *abuf;
    uint8_t *data;
    int64_t len;

    abuf = JS_GetOpaque2(ctx, this_val, class_id);
    if (!abuf)
        return JS_EXCEPTION;
    if (abuf->immutable)
        return JS_ThrowTypeErrorImmutableArrayBuffer(ctx);
    if (JS_ToInt64(ctx, &len, argv[0]))
        return JS_EXCEPTION;
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    if (!array_buffer_is_resizable(abuf))
        return JS_ThrowTypeError(ctx, "array buffer is not resizable");
    // TODO(bnoordhuis) support externally managed RABs
    /* a Shared Data Block is never an embedder's (js_array_buffer_init refuses one) */
    if (!abuf->shared && abuf->free_func != js_array_buffer_free)
        return JS_ThrowTypeError(ctx, "external array buffer is not resizable");
    if (len < 0 || len > abuf->max_byte_length) {
    bad_length:
        return JS_ThrowRangeError(ctx, "invalid array buffer length");
    }
    /* A SAB CAN ONLY GROW — §25.2.5.3 SharedArrayBuffer.prototype.grow step 12.4, "If newByteLength <
       currentByteLength or newByteLength > obj.[[ArrayBufferMaxByteLength]], throw a RangeError exception.",
       hoisted out of the branch below so that the single lifetime capture can stand AFTER every refusal and
       BEFORE the storage moves. It is the same refusal at the same point (no user code runs between the two)
       and it is what lets the capture be one line rather than one per arm.
       THE NUMBER STOOD HERE AS `25.2.5.4 step 8` AND WAS WRONG IN BOTH HALVES: §25.2.5.4 is `get
       SharedArrayBuffer.prototype.growable`, a five-step accessor with no RangeError in it. The sub-number is
       safe to write because step 12 ("Repeat,") holds exactly ONE list. */
    if (abuf->shared && len < abuf->byte_length)
        goto bad_length;
    /* forced-exec TIME-TRAVEL: THE STORAGE IS ABOUT TO MOVE, and that is a mutation of the buffer OBJECT that
       the byte entry cannot express — an entry holds `a_len` bytes, so a resize leaves it describing a length
       the buffer no longer has. Declared HERE rather than left to the save-side CHECKs, because those can only
       fire for a buffer this flow had already captured: `rab.resize(n)` BEFORE the first write creates the
       entry afterwards, over the post-resize bytes, and nothing downstream ever disagrees with itself. The
       host skips a buffer the running flow created and aborts on a shared one, naming the entry to build. */
    if (len != abuf->byte_length)
        cow_capture_buffer_lifetime(ctx, this_val);
    // SABs can only grow and we don't need to realloc because
    // js_array_buffer_constructor3 commits all memory upfront;
    // regular RABs are resizable both ways and realloc
    if (abuf->shared) {
        // Note this is off-spec; there's supposed to be a single atomic
        // |byteLength| property that's shared across SABs but we store
        // it per SAB instead. That means when thread A calls sab.grow(2)
        // at time t0, and thread B calls sab.grow(1) at time t1, we don't
        // throw a TypeError in thread B as the spec says we should,
        // instead both threads get their own view of the backing memory,
        // 2 bytes big in A, and 1 byte big in B
        abuf->byte_length = len;
    } else {
        data = js_realloc(ctx, abuf->data, max_int(len, 1));
        if (!data)
            return JS_EXCEPTION;
        if (len > abuf->byte_length)
            memset(&data[abuf->byte_length], 0, len - abuf->byte_length);
        abuf->byte_length = len;
        abuf->data = data;
    }
    /* §25.1.6.6 step 15: the new [[ArrayBufferByteLength]] is the stated newLength */
    JS_FreeValue(ctx, abuf->length_unknown);
    abuf->length_unknown = JS_UNINITIALIZED;
    js_array_buffer_update_typed_arrays(abuf);
    return JS_UNDEFINED;
}

/* 25.1.6.7 from the species result onwards: the bounds are already primitives, `start`/`new_len` are computed from
   the length read BEFORE them, and `new_obj` is whatever the species (or the intrinsic) built. Every check below is
   a re-validation the spec performs after that user code, and none of them runs any more of it. */
static JSValue js_array_buffer_slice_build(JSContext *ctx, JSValueConst this_val, JSValue new_obj,
                                           int64_t start, int64_t new_len, int magic)
{
    JSArrayBuffer *abuf, *new_abuf;
    bool immutable = magic & 1;
    int class_id = magic >> 1;

    abuf = JS_GetOpaque2(ctx, this_val, class_id);
    if (!abuf) { JS_FreeValue(ctx, new_obj); return JS_EXCEPTION; }
    if (JS_IsException(new_obj))
        return new_obj;
    new_abuf = JS_GetOpaque2(ctx, new_obj, class_id);
    if (!new_abuf)
        goto fail;
    if (new_abuf->immutable) {
        JS_ThrowTypeErrorImmutableArrayBuffer(ctx);
        goto fail;
    }
    if (js_same_value(ctx, new_obj, this_val)) {
        JS_ThrowTypeError(ctx, "cannot use identical ArrayBuffer");
        goto fail;
    }
    if (new_abuf->detached) {
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    if (new_abuf->byte_length < new_len) {
        JS_ThrowTypeError(ctx, "new ArrayBuffer is too small");
        goto fail;
    }
    /* must test again because of side effects */
    if (abuf->detached) {
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    if (abuf->byte_length < start + new_len) {
        if (immutable)
            JS_ThrowRangeError(ctx, "invalid array buffer length");
        else
            JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    js_ab_spans_copy(ctx, new_abuf, 0, abuf, (uint32_t)start, (uint32_t)new_len);
    memcpy(new_abuf->data, abuf->data + start, new_len);
    new_abuf->immutable = immutable;
    return new_obj;
 fail:
    JS_FreeValue(ctx, new_obj);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry js_array_buffer_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("byteLength", js_array_buffer_get_byteLength, NULL, JS_CLASS_ARRAY_BUFFER ),
    JS_CGETSET_MAGIC_DEF("maxByteLength", js_array_buffer_get_maxByteLength, NULL, JS_CLASS_ARRAY_BUFFER ),
    JS_CGETSET_MAGIC_DEF("resizable", js_array_buffer_get_resizable, NULL, JS_CLASS_ARRAY_BUFFER ),
    JS_CGETSET_DEF("detached", js_array_buffer_get_detached, NULL ),
    JS_CGETSET_DEF("immutable", js_array_buffer_get_immutable, NULL ),
    JS_CFUNC_STEP_DEF("resize", 1, STEPDEF_AB_RESIZE ),
    JS_CFUNC_STEP_DEF("slice", 2, STEPDEF_AB_SLICE ),
    JS_CFUNC_STEP_DEF("sliceToImmutable", 2, STEPDEF_AB_SLICE_IMM ),
    JS_CFUNC_STEP_DEF("transfer", 0, STEPDEF_AB_TRANSFER ),
    JS_CFUNC_STEP_DEF("transferToImmutable", 0, STEPDEF_AB_TRANSFER_IMM ),
    JS_CFUNC_STEP_DEF("transferToFixedLength", 0, STEPDEF_AB_TRANSFER_FIX ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "ArrayBuffer", JS_PROP_CONFIGURABLE ),
};

/* SharedArrayBuffer */

static const JSCFunctionListEntry js_shared_array_buffer_funcs[] = {
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
};

static const JSCFunctionListEntry js_shared_array_buffer_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("byteLength", js_array_buffer_get_byteLength, NULL, JS_CLASS_SHARED_ARRAY_BUFFER ),
    JS_CGETSET_MAGIC_DEF("maxByteLength", js_array_buffer_get_maxByteLength, NULL, JS_CLASS_SHARED_ARRAY_BUFFER ),
    JS_CGETSET_MAGIC_DEF("growable", js_array_buffer_get_resizable, NULL, JS_CLASS_SHARED_ARRAY_BUFFER ),
    JS_CFUNC_STEP_DEF("grow", 1, STEPDEF_SAB_GROW ),
    JS_CFUNC_STEP_DEF("slice", 2, STEPDEF_SAB_SLICE ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "SharedArrayBuffer", JS_PROP_CONFIGURABLE ),
};

