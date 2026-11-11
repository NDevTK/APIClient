static bool is_typed_array(JSClassID class_id)
{
    return class_id >= JS_CLASS_UINT8C_ARRAY && class_id <= JS_CLASS_FLOAT64_ARRAY;
}

// |p| must be a typed array, *not* a DataView
static bool typed_array_is_immutable(JSObject *p)
{
    JSArrayBuffer *abuf;
    JSTypedArray *ta;

    DCHECK(is_typed_array(p->class_id), "is_typed_array(p->class_id)");
    ta = p->u.typed_array;
    abuf = ta->buffer->u.array_buffer;
    return abuf->immutable;
}

// is the typed array detached or out of bounds relative to its RAB?
// |p| must be a typed array, *not* a DataView
static bool dataview_is_oob(JSObject *p);

static bool typed_array_is_oob(JSObject *p)
{
    JSArrayBuffer *abuf;
    JSTypedArray *ta;
    int len, size_elem;
    int64_t end;

    DCHECK(is_typed_array(p->class_id), "is_typed_array(p->class_id)");

    ta = p->u.typed_array;
    abuf = ta->buffer->u.array_buffer;
    if (abuf->detached)
        return true;
    /* A view whose length is an unknown over a buffer whose length is one was built on its range test's in-range
       arm (js_ta_ctor_step's TAC_RANGE), so its window is in bounds in this world wherever the example bytes end. */
    if (!JS_IsUninitialized(ta->length_unknown) && !JS_IsUninitialized(abuf->length_unknown))
        return false;
    len = abuf->byte_length;
    if (ta->offset > len)
        return true;
    if (ta->track_rab)
        return false;
    if (len < (int64_t)ta->offset + ta->length)
        return true;
    size_elem = 1 << typed_array_size_log2(p->class_id);
    end = (int64_t)ta->offset + (int64_t)p->u.array.count * size_elem;
    return end > len;
}

/* WARNING: 'p' must be a typed array. Works even if the array buffer
   is detached */
static uint32_t typed_array_length(JSObject *p)
{
    JSTypedArray *ta = p->u.typed_array;
    int size_log2 = typed_array_size_log2(p->class_id);
    return ta->length >> size_log2;
}

static int validate_typed_array(JSContext *ctx, JSValueConst this_val)
{
    JSObject *p;
    p = get_typed_array(ctx, this_val);
    if (!p)
        return -1;
    if (typed_array_is_oob(p)) {
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        return -1;
    }
    return 0;
}

static JSValue js_typed_array_get_length(JSContext *ctx, JSValueConst this_val)
{
    JSObject *p;
    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (!JS_IsUninitialized(p->u.typed_array->length_unknown) && !typed_array_is_oob(p))
        return js_dup(p->u.typed_array->length_unknown);   /* §23.2.3.21 step 6, TypedArrayLength */
    return js_int32(p->u.array.count);
}

static JSValue js_typed_array_get_buffer(JSContext *ctx, JSValueConst this_val)
{
    JSObject *p;
    JSTypedArray *ta;
    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    ta = p->u.typed_array;
    return js_dup(JS_MKPTR(JS_TAG_OBJECT, ta->buffer));
}

static JSValue js_typed_array_get_byteLength(JSContext *ctx, JSValueConst this_val)
{
    uint32_t size_log2;
    JSTypedArray *ta;
    JSObject *p;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_oob(p))
        return js_int32(0);
    ta = p->u.typed_array;
    if (!JS_IsUninitialized(ta->length_unknown)) {
        /* §23.2.3.3 step 6, TypedArrayByteLength: the length times the element size */
        JSValue ops[2] = { ta->length_unknown, js_int32(typed_array_size_log2(p->class_id)) }, r;

        r = g_concolic.derived(ctx, "23.2.3.3 get %TypedArray%.prototype.byteLength", ops, 2,
                               js_int64((int64_t)p->u.array.count << typed_array_size_log2(p->class_id)));
        DCHECK(!JS_IsUninitialized(r), "a view's unknown length derived no unknown byte length");
        return r;
    }
    if (!ta->track_rab)
        return js_uint32(ta->length);
    size_log2 = typed_array_size_log2(p->class_id);
    return js_int64((int64_t)p->u.array.count << size_log2);
}

static JSValue js_typed_array_get_byteOffset(JSContext *ctx, JSValueConst this_val)
{
    JSObject *p;
    JSTypedArray *ta;
    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_oob(p))
        return js_int32(0);
    ta = p->u.typed_array;
    if (!JS_IsUninitialized(ta->offset_unknown))
        return js_dup(ta->offset_unknown);   /* §23.2.3.4 step 7 */
    return js_uint32(ta->offset);
}

JSValue JS_NewTypedArray(JSContext *ctx, int argc, JSValueConst *argv,
                         JSTypedArrayEnum type)
{
    if (type < JS_TYPED_ARRAY_UINT8C || type > JS_TYPED_ARRAY_FLOAT64)
        return JS_ThrowRangeError(ctx, "invalid typed array type");

    return js_typed_array_constructor(ctx, JS_UNDEFINED, argc, argv,
                                      JS_CLASS_UINT8C_ARRAY + type);
}

/* THE UNDERLYING BUFFER OF A BUFFER SOURCE. Web IDL §3.2.26 Buffer source types names the concept and states
   it in two steps: a buffer type instance IS its own underlying buffer, and a view's is its
   [[ViewedArrayBuffer]]. The two predicates below are the whole of its clientele, and each of them is asked
   only of a value §3.2.26's own brand test has already accepted — so the tail is CHECK_FAIL and not a DCHECK:
   what stands past it is a read of `u.typed_array` on an object whose union holds something else, which is a
   type confusion and must not proceed in release either. */
static JSArrayBuffer *js_buffer_source_buffer(JSValueConst obj)
{
    JSObject *p;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        CHECK_FAIL("a buffer-source predicate was given a primitive — Web IDL §3.2.26 Buffer source types "
                   "brand-tests V before it asks either question of the buffer under it");
    p = JS_VALUE_GET_OBJ(obj);
    if (p->class_id == JS_CLASS_ARRAY_BUFFER || p->class_id == JS_CLASS_SHARED_ARRAY_BUFFER)
        return p->u.array_buffer;
    if (is_typed_array(p->class_id) || p->class_id == JS_CLASS_DATAVIEW)
        return p->u.typed_array->buffer->u.array_buffer;
    CHECK_FAIL("a buffer-source predicate was given an object that is neither a buffer nor a view — those are "
               "the only two kinds Web IDL §3.2.26 Buffer source types admits, and its brand test runs first");
}

/* ECMAScript §25.1.3.9 IsFixedLengthArrayBuffer ( arrayBuffer ), asked of the underlying buffer — which is
   exactly how Web IDL §3.2.26 Buffer source types asks it: of V for an ArrayBuffer, of V.[[ViewedArrayBuffer]]
   for a DataView or a typed array. ONE function therefore answers every arm of that conversion and the binding
   layer performs no walk of its own.
   §25.1.3.9 is two lines — "If arrayBuffer has an [[ArrayBufferMaxByteLength]] internal slot, return false.
   Return true." — and this engine's slot is `max_byte_length`, absent as a negative, which is precisely what
   array_buffer_is_resizable reads. §25.1.3.9 takes "an ArrayBuffer or a SharedArrayBuffer", so a GROWABLE
   SharedArrayBuffer is not fixed-length either and this answers for one without a second test.
   NO CONTEXT AND NO THROW, because §3.2.26 asks its questions BEFORE any detach or bounds test — it has
   none — so a detached or out-of-bounds view must still get an answer here rather than an exception. */
bool JS_IsFixedLengthBufferSource(JSValueConst obj)
{
    return !array_buffer_is_resizable(js_buffer_source_buffer(obj));
}

/* ECMAScript's IsSharedArrayBuffer over the same underlying buffer, for Web IDL §3.2.26's OTHER refusal — the
   step that precedes the fixed-length one in each of its four conversions. §4.2 BufferSource says in its own
   note that [AllowShared] "cannot be used with BufferSource as ArrayBuffer does not support it", and §4.1
   ArrayBufferView carries no extended attribute at all, so at both of this fork's declared buffer-source
   positions the refusal is unconditional. */
bool JS_IsSharedBufferSource(JSValueConst obj)
{
    return js_buffer_source_buffer(obj)->shared;
}

/* ECMAScript's IsDetachedBuffer over the same underlying buffer — the THIRD question Web IDL §3.2.26 Buffer
   source types asks of it, and the one that had no answer. Its two conversions above are refusals; this one is
   a POSITIVE STATEMENT inside an algorithm that produces a value: "get a copy of the bytes held by the buffer
   source" reads the view's window in its step 5 ("Set offset to jsBufferSource.[[ByteOffset]]", "Set length
   to jsBufferSource.[[ByteLength]]") and then, in step 7, "If IsDetachedBuffer(jsArrayBuffer) is true, then
   return the empty byte sequence".
   IT EXISTS BECAUSE THE WINDOW IS WHAT CANNOT BE READ FIRST. An embedder's only route from a VIEW to its
   buffer is JS_GetArrayBufferView, which refuses an out-of-bounds view — and a detached buffer makes every
   view over it out of bounds — so a caller performing that algorithm reached an EXCEPTION at step 5 for the
   input step 7 defines the answer to. The ArrayBuffer arm hid it: JS_GetBufferBytes answers NULL with a zero
   length for a detached buffer, so `digest(alg, detachedBuffer)` hashed the empty message correctly while
   `digest(alg, viewOntoDetachedBuffer)` aborted, and the abort named the brand test rather than the detach.
   NO CONTEXT AND NO THROW, exactly as its two siblings state and for the same reason: §3.2.26's algorithms
   have no detach test of their own to gate this behind, so a detached or out-of-bounds view must still be able
   to be ASKED. The brand test comes first, and anything else is fatal at the call. */
bool JS_IsDetachedBufferSource(JSValueConst obj)
{
    return js_buffer_source_buffer(obj)->detached != 0;
}

static size_t js_buffer_view_byte_length(JSObject *p);

/* The most recent span still describing byte `b`, or NULL — the recency and witness rules js_ab_span_get
   applies, asked per byte. */
static const JSArrayBufferSpan *js_ab_span_owner(const JSArrayBuffer *abuf, uint32_t b)
{
    int i;

    for (i = abuf->n_span - 1; i >= 0; i--) {
        const JSArrayBufferSpan *s = &abuf->span[i];

        if (b < s->offset || (uint64_t)b >= (uint64_t)s->offset + s->extent)
            continue;
        if (s->kind == JS_AB_SPAN_ANY)
            return s;
        if (s->kind == JS_AB_SPAN_IMAGE) {
            if (abuf->data[b] == str8(JS_VALUE_GET_STRING(s->image))[b - s->offset])
                return s;
            continue;
        }
        if ((int64_t)s->offset + s->extent <= (int64_t)abuf->byte_length &&
            memcmp(&abuf->data[s->offset], s->witness, s->extent) == 0)
            return s;
    }
    return NULL;
}

/* The unknown pieces of bytes [lo, hi) of `abuf` (whose object is `bufobj`), each run of bytes one span still
   describes, with offsets relative to `lo`; *ppieces is owned by the caller (JS_FreeBufferUnknowns). */
static int js_ab_unknown_pieces(JSContext *ctx, const JSArrayBuffer *abuf, JSObject *bufobj, uint32_t lo,
                                uint32_t hi, JSUnknownPiece **ppieces)
{
    JSUnknownPiece *pieces = NULL;
    uint32_t b, e;
    int n = 0, cap = 0;

    *ppieces = NULL;
    if (abuf->n_span == 0 || abuf->detached || abuf->data == NULL)
        return 0;
    for (b = lo; b < hi; b = e) {
        const JSArrayBufferSpan *s = js_ab_span_owner(abuf, b);
        JSUnknownPiece *u;

        e = b + 1;
        while (e < hi && js_ab_span_owner(abuf, e) == s)
            e++;
        if (!s)
            continue;
        if (n == cap) {
            JSUnknownPiece *g;

            cap = cap ? cap * 2 : 4;
            g = js_realloc(ctx, pieces, sizeof(*pieces) * (size_t)cap);
            CHECK(g != NULL, "OOM listing which bytes of a buffer source are unknown input");
            pieces = g;
        }
        u = &pieces[n++];
        u->offset = b - lo;
        u->length = e - b;
        u->codec = NULL;
        u->image_pos = 0;
        if (s->kind == JS_AB_SPAN_IMAGE) {
            u->unknown = js_dup(s->unknown);
            u->codec = s->codec;
            u->image_pos = s->image_pos + (b - s->offset);
            u->example = true;
        } else if (s->kind == JS_AB_SPAN_AT) {
            u->unknown = js_dup(s->unknown);
            u->example = g_concolic.example_held(s->unknown);
        } else if (!JS_IsUninitialized(s->unknown)) {
            u->unknown = js_dup(s->unknown);
            u->example = false;
        } else {
            u->unknown = g_concolic.key_read(ctx, JS_MKPTR(JS_TAG_OBJECT, bufobj), s->index);
            DCHECK(!JS_IsUninitialized(u->unknown), "an ANY span's unknown offset derived no unknown element");
            u->example = false;
        }
    }
    *ppieces = pieces;
    return n;
}

/* A window of `len` bytes whose [[ByteOffset]] is unknown: which bytes of the buffer it holds is unknown, so the
   whole window is one run, derived from its unknown start and from the `n` pieces its example window had (taken
   and freed), whose block bytes are its example where the offset and every such piece have one. Answers 1. */
static int js_ta_window_at_unknown_offset(JSContext *ctx, const JSTypedArray *ta, uint32_t len,
                                          JSUnknownPiece **ppieces, int n)
{
    JSUnknownPiece *pieces = *ppieces, *g;
    JSValue *ops;
    bool example = js_concolic_held(ta->offset_unknown);
    int i;

    ops = js_malloc(ctx, sizeof(*ops) * (size_t)(n + 2));
    CHECK(ops != NULL, "OOM listing the window of a buffer source at an unknown byteOffset");
    ops[0] = js_ta_byte_index(ctx, ta, 0);
    ops[1] = js_uint32(len);
    for (i = 0; i < n; i++) {
        ops[i + 2] = pieces[i].unknown;
        example = example && pieces[i].example;
    }
    g = js_realloc(ctx, pieces, sizeof(*pieces));
    CHECK(g != NULL, "OOM listing the window of a buffer source at an unknown byteOffset");
    g[0].unknown = g_concolic.derived(ctx, "the bytes of a buffer source at an unknown byteOffset", ops, n + 2,
                                      JS_UNDEFINED);
    DCHECK(!JS_IsUninitialized(g[0].unknown), "a window at an unknown byteOffset derived no unknown");
    for (i = 0; i < n + 2; i++)
        JS_FreeValue(ctx, ops[i]);
    js_free(ctx, ops);
    g[0].offset = 0;
    g[0].length = len;
    g[0].codec = NULL;
    g[0].image_pos = 0;
    g[0].example = example;
    *ppieces = g;
    return 1;
}

/* see quickjs-step.h */
int JS_BufferSourceUnknowns(JSContext *ctx, JSValueConst obj, JSUnknownPiece **ppieces)
{
    JSArrayBuffer *abuf = js_buffer_source_buffer(obj);
    JSObject *p = JS_VALUE_GET_OBJ(obj), *bufobj = p;
    JSTypedArray *ta = NULL;
    JSUnknownPiece *pieces, *g;
    JSValue extent, ops[3], ex;
    uint32_t lo = 0, hi;
    int n;

    *ppieces = NULL;
    if (abuf->detached)
        return 0;
    if (p->class_id == JS_CLASS_ARRAY_BUFFER || p->class_id == JS_CLASS_SHARED_ARRAY_BUFFER) {
        hi = (uint32_t)abuf->byte_length;
        extent = JS_IsUninitialized(abuf->length_unknown) ? JS_UNINITIALIZED
               : js_ab_length_derive(ctx, abuf, "25.1.6.1 get ArrayBuffer.prototype.byteLength", 0, 0,
                                     abuf->byte_length);
    } else {
        DCHECK(p->class_id == JS_CLASS_DATAVIEW ? !dataview_is_oob(p) : !typed_array_is_oob(p),
               "a buffer source's unknown bytes were asked of an out-of-bounds view — Web IDL §3.2.26's copy "
               "reads no window for one, so the caller has already answered it");
        ta = p->u.typed_array;
        bufobj = ta->buffer;
        lo = ta->offset;
        hi = lo + (uint32_t)js_buffer_view_byte_length(p);
        extent = js_dup(ta->length_unknown);
    }
    n = abuf->data == NULL ? 0 : js_ab_unknown_pieces(ctx, abuf, bufobj, lo, hi, ppieces);
    if (ta && !JS_IsUninitialized(ta->offset_unknown) && hi > lo)
        n = js_ta_window_at_unknown_offset(ctx, ta, hi - lo, ppieces, n);
    if (JS_IsUninitialized(extent))
        return n;
    /* The window's length is an unknown, so the bytes past its stored example are a run too: the tail, derived
       from the buffer's unknown (or its stated length) at the window's end and over that length, as an element
       read past the example is. It holds no bytes in the block; they are the example's exactly when the length has an example. */
    pieces = *ppieces;
    g = js_realloc(ctx, pieces, sizeof(*pieces) * (size_t)(n + 1));
    CHECK(g != NULL, "OOM listing the unknown tail of a buffer source");
    pieces = g;
    ops[0] = JS_IsUninitialized(abuf->length_unknown) ? js_int32(abuf->byte_length) : abuf->length_unknown;
    ops[1] = ta ? js_ta_byte_index(ctx, ta, (int64_t)(hi - lo)) : JS_NewInt64(ctx, (int64_t)hi);
    ops[2] = extent;
    pieces[n].offset = hi - lo;
    pieces[n].length = 0;
    pieces[n].codec = NULL;
    pieces[n].image_pos = 0;
    pieces[n].unknown = g_concolic.derived(ctx, "the bytes of a buffer source past its example", ops, 3,
                                           JS_UNDEFINED);
    DCHECK(!JS_IsUninitialized(pieces[n].unknown), "a buffer source's unknown tail derived no unknown");
    JS_FreeValue(ctx, ops[1]);
    ex = js_concolic_operand(ctx, extent);
    pieces[n].example = js_concolic_held(extent) && JS_IsNumber(ex);
    JS_FreeValue(ctx, ex);
    JS_FreeValue(ctx, extent);
    *ppieces = pieces;
    return n + 1;
}

void JS_FreeBufferUnknowns(JSContext *ctx, JSUnknownPiece *pieces, int n)
{
    int i;

    for (i = 0; i < n; i++)
        JS_FreeValue(ctx, pieces[i].unknown);
    js_free(ctx, pieces);
}

/* THE VIEW'S BYTE LENGTH AS THE SPEC COMPUTES IT, WHICH IS NOT ALWAYS `ta->length`. A length-tracking view has
   [[ByteLength]] = auto, and ECMAScript §10.4.5.12 TypedArrayByteLength ( taRecord ) and §25.3.1.3
   GetViewByteLength ( viewRecord ) both turn on that one line — §10.4.5.12's is "If obj.[[ByteLength]] is not
   auto, return obj.[[ByteLength]]" and §25.3.1.3's names its own local, "If view.[[ByteLength]] is not auto,
   return view.[[ByteLength]]" — and derive the answer from the BUFFER otherwise.
   `ta->length` is this engine's [[ByteLength]] slot and it is written ONCE,
   by typed_array_init; for a track_rab view it is therefore a record of the buffer's size at the instant the
   view was constructed and nothing else. js_array_buffer_update_typed_arrays maintains `p->u.array.count` for
   a typed array and `ta->length` for a DataView, and does not touch a typed array's.
   SO AN EMBEDDER HANDED `ta->length` WAS HANDED A WINDOW THE VIEW DOES NOT HAVE. `new Uint8Array(rab)` over a
   64-byte resizable buffer followed by `rab.resize(8)` left both exports below answering 8 bytes of storage
   with a 64-byte window, and their consumers are memcpy sites: it made `new Blob([v]).size` 64 out of an
   8-byte allocation, and `new TextEncoder().encodeInto(s, v)` write 64 bytes into it and report `written:64`.
   Both callers have already refused an out-of-bounds view, which is the ASSERT both spec operations open
   with, so neither derivation below can underflow. */
static size_t js_buffer_view_byte_length(JSObject *p)
{
    JSTypedArray *ta = p->u.typed_array;
    JSArrayBuffer *abuf;

    if (!ta->track_rab)
        return ta->length;
    if (p->class_id == JS_CLASS_DATAVIEW) {
        /* §25.3.1.3's last two steps, over the buffer's CURRENT length — and NOT `ta->length`, which the
           resize hook leaves alone when the offset lands exactly ON the new end (`ta->offset < len` is false
           there) while dataview_is_oob admits that same view (`ta->offset > byte_length` is false). */
        abuf = ta->buffer->u.array_buffer;
        DCHECK((int64_t)ta->offset <= (int64_t)abuf->byte_length,
               "§25.3.1.3 GetViewByteLength opens by asserting the view is not out of bounds, and every caller "
               "has run dataview_is_oob — a view whose offset is past its own buffer reached the length anyway");
        return (size_t)((int64_t)abuf->byte_length - (int64_t)ta->offset);
    }
    /* §10.4.5.12 over §10.4.5.13 TypedArrayLength's result: the element COUNT is the slot this engine keeps
       live across a resize, so the byte length is that count scaled by the element size. */
    return (size_t)p->u.array.count << typed_array_size_log2(p->class_id);
}

/* WEB IDL'S `BufferSource` — see quickjs-step.h. A DataView is one and is not a typed array, which is the whole
   reason this exists beside JS_GetTypedArrayBuffer: both wear u.typed_array, and only the class test differs. */
JSValue JS_GetArrayBufferView(JSContext *ctx, JSValueConst obj, size_t *pbyte_offset, size_t *pbyte_length)
{
    JSObject *p;
    JSTypedArray *ta;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return JS_ThrowTypeError(ctx, "not an ArrayBufferView");
    p = JS_VALUE_GET_OBJ(obj);
    if (!is_typed_array(p->class_id) && p->class_id != JS_CLASS_DATAVIEW)
        return JS_ThrowTypeError(ctx, "not an ArrayBufferView");
    /* THE OOB TEST IS PER KIND. typed_array_is_oob says in its own comment that its argument must not be a
       DataView — it reads a bytes-per-element off the class — and handing it one aborted on that assertion,
       which is what `new TextDecoder().decode(new DataView(buf))` did. A DataView has its own. */
    if (p->class_id == JS_CLASS_DATAVIEW ? dataview_is_oob(p) : typed_array_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);
    ta = p->u.typed_array;
    if (pbyte_offset) *pbyte_offset = ta->offset;
    /* §10.4.5.12 / §25.3.1.3 rather than the [[ByteLength]] slot — see js_buffer_view_byte_length. */
    if (pbyte_length) *pbyte_length = js_buffer_view_byte_length(p);
    return js_dup(JS_MKPTR(JS_TAG_OBJECT, ta->buffer));
}

/* Return the buffer associated to the typed array or an exception if
   it is not a typed array or if the buffer is detached. pbyte_offset,
   pbyte_length or pbytes_per_element can be NULL. */
JSValue JS_GetTypedArrayBuffer(JSContext *ctx, JSValueConst obj,
                               size_t *pbyte_offset,
                               size_t *pbyte_length,
                               size_t *pbytes_per_element)
{
    JSObject *p;
    JSTypedArray *ta;
    p = get_typed_array(ctx, obj);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);
    ta = p->u.typed_array;
    if (pbyte_offset)
        *pbyte_offset = ta->offset;
    /* §10.4.5.12 TypedArrayByteLength rather than the [[ByteLength]] slot — see js_buffer_view_byte_length.
       The same defect as the one next door and reached by a different door: a length-tracking view handed to
       an embedder through THIS export carried the stale window just as readily. */
    if (pbyte_length)
        *pbyte_length = js_buffer_view_byte_length(p);
    if (pbytes_per_element) {
        *pbytes_per_element = 1 << typed_array_size_log2(p->class_id);
    }
    return js_dup(JS_MKPTR(JS_TAG_OBJECT, ta->buffer));
}

/* return NULL if exception. WARNING: any JS call can detach the
   buffer and render the returned pointer invalid */
uint8_t *JS_GetUint8Array(JSContext *ctx, size_t *psize, JSValueConst obj)
{
    JSObject *p;
    JSTypedArray *ta;
    JSArrayBuffer *abuf;
    p = get_typed_array(ctx, obj);
    if (!p)
        goto fail;
    if (typed_array_is_oob(p)) {
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        goto fail;
    }
    if (p->class_id != JS_CLASS_UINT8_ARRAY && p->class_id != JS_CLASS_UINT8C_ARRAY) {
        JS_ThrowTypeError(ctx, "not a Uint8Array");
        goto fail;
    }
    ta = p->u.typed_array;
    abuf = ta->buffer->u.array_buffer;

    *psize = ta->length;
    return abuf->data + ta->offset;
 fail:
    *psize = 0;
    return NULL;
}

static JSValue js_typed_array_get_toStringTag(JSContext *ctx,
                                              JSValueConst this_val)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(this_val) != JS_TAG_OBJECT)
        return JS_UNDEFINED;
    p = JS_VALUE_GET_OBJ(this_val);
    if (!is_typed_array(p->class_id))
        return JS_UNDEFINED;
    return JS_AtomToString(ctx, ctx->rt->class_array[p->class_id].class_name);
}

/* The TYPED-ARRAY-source half of %TypedArray%.prototype.set. Past the validation it runs NO user code — the
   source's elements are already numbers or bigints — so it stays a C body. The array-like half does not: its
   length is LengthOfArrayLike and each element is coerced, so it lives in the machine's stages.
   `offset` arrives already coerced for the same reason. Returns 1 = handled, 0 = the source is not a typed array
   and the caller must run the element loop, -1 = threw. */
static int js_typed_array_set_ta(JSContext *ctx,
                                 JSValueConst dst,
                                 JSValueConst src_obj,
                                 int64_t offset,
                                 int64_t *pdst_len)
{
    JSObject *p, *src_p;
    uint32_t i;
    int64_t dst_len, src_len;
    JSValue val;

    p = get_typed_array(ctx, dst);
    if (!p)
        return -1;
    if (offset < 0)
        goto range_error;
    if (typed_array_is_oob(p)) {
    detached:
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        return -1;
    }
    dst_len = p->u.array.count;
    *pdst_len = dst_len;
    src_p = JS_VALUE_GET_OBJ(src_obj);
    if (!is_typed_array(src_p->class_id))
        return 0;                      /* the array-like half: the caller's stages own it */
    {
        JSTypedArray *dest_ta = p->u.typed_array;
        JSArrayBuffer *dest_abuf = dest_ta->buffer->u.array_buffer;
        JSTypedArray *src_ta = src_p->u.typed_array;
        JSArrayBuffer *src_abuf = src_ta->buffer->u.array_buffer;
        int shift = typed_array_size_log2(p->class_id);

        if (typed_array_is_oob(src_p))
            goto detached;
        src_len = src_p->u.array.count;
        if (offset > dst_len - src_len)
            goto range_error;
        if (src_p->class_id == p->class_id) {
            /* same type, use memmove */
            /* the DESTINATION's bytes only: the source is read. The element-by-element tail below needs no
               capture of its own — it stores through JS_SetPropertyUint32, which is routed. */
            cow_capture_bytes(ctx, p);
            DCHECK_TA_WINDOW_STATED(src_ta, src_len, "%TypedArray%.prototype.set's byte copy");
            js_ab_spans_copy(ctx, dest_abuf, dest_ta->offset + (uint32_t)(offset << shift),
                             src_abuf, src_ta->offset, (uint32_t)(src_len << shift));
            memmove(dest_abuf->data + dest_ta->offset + (offset << shift),
                    src_abuf->data + src_ta->offset, src_len << shift);
            return 1;
        }
        if (dest_abuf->data == src_abuf->data) {
            /* copying between the same buffer using different types of mappings
               would require a temporary buffer */
        }
        /* otherwise, default behavior is slow but correct */
    }
    for (i = 0; i < src_len; i++) {
        /* the source is a typed array, so every element is already a number or a bigint: the store's conversion
           runs nothing and this loop needs no steps. */
        val = JS_GetPropertyUint32(ctx, src_obj, i);
        if (JS_IsException(val))
            return -1;
        if (typed_array_is_oob(p))
            JS_FreeValue(ctx, val);
        else if (JS_SetPropertyUint32(ctx, dst, offset + i, val) < 0)
            return -1;
    }
    return 1;
range_error:
    JS_ThrowRangeError(ctx, "invalid array length");
    return -1;
}


/* 23.2.3.36 steps 7 onwards, with the index and the value ALREADY coerced and `len` captured at step 3 — a
   resizable buffer shrunk by the page's valueOf must still yield a result of the original length, which is why
   the caller holds it. Nothing below runs user code. */
static JSValue js_typed_array_with_build(JSContext *ctx, JSValueConst this_val, int64_t idx, JSValue val,
                                         uint32_t len)
{
    JSValue arr, buffer;
    JSObject *p;
    JSTypedArray *ta;
    JSArrayBuffer *src_abuf, *abuf;
    uint32_t newlen, copy_len;
    int size_log2;

    p = get_typed_array(ctx, this_val);
    if (!p) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    /* Step 7 is ONE predicate — IsValidIntegerIndex(O, actualIndex) — and its failure is ONE error. A detached or
       out-of-bounds view makes it false at its first clause exactly as an out-of-range index does at its last, so
       `ta.with(0, {valueOf(){ detach(ta.buffer); return 0 }})` is a RangeError. Splitting the OOB clause out into
       a TypeError of its own answered a step the spec does not have. */
    newlen = p->u.array.count;
    if (typed_array_is_oob(p) || idx < 0 || idx >= newlen) {
        JS_FreeValue(ctx, val);
        return JS_ThrowRangeError(ctx, "invalid typed array index");
    }

    copy_len = min_uint32(len, newlen);
    size_log2 = typed_array_size_log2(p->class_id);

    /* create new typed array with original length (zero-initialized) */
    arr = js_create_from_intrinsic(ctx, p->class_id);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    buffer = js_array_buffer_constructor1(ctx,
                                          (uint64_t)len << size_log2,
                                          NULL);
    if (JS_IsException(buffer)) {
        JS_FreeValue(ctx, val);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    /* Steps 8-9 (TypedArrayCreateSameType with an undefined new_target, then the backing buffer) run NO page
       code, so the view step 7 just validated cannot have become out-of-bounds underneath them. This was a
       second OOB test answering a step that does not exist; the invariant is what it was really asserting. */
    DCHECK(!typed_array_is_oob(p), "the source view went out of bounds with no page code in between");
    abuf = JS_GetOpaque(buffer, JS_CLASS_ARRAY_BUFFER);
    if (typed_array_init(ctx, arr, buffer, 0, len, /*track_rab*/false)) {
        JS_FreeValue(ctx, val);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }

    /* copy min(len, newlen) elements from source; rest stays zero */
    if (copy_len > 0) {
        ta = p->u.typed_array;
        src_abuf = ta->buffer->u.array_buffer;
        DCHECK_TA_WINDOW_STATED(ta, copy_len, "%TypedArray%.prototype.with's copy");
        js_ab_spans_copy(ctx, abuf, 0, src_abuf, ta->offset, (uint32_t)((size_t)copy_len << size_log2));
        memcpy(abuf->data, src_abuf->data + ta->offset,
               (size_t)copy_len << size_log2);
    }

    if (JS_SetPropertyInt64(ctx, arr, idx, val) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}


static JSValue js_create_typed_array_iterator(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv, int magic)
{
    if (validate_typed_array(ctx, this_val))
        return JS_EXCEPTION;
    return js_create_array_iterator(ctx, this_val, argc, argv, magic);
}

/* DELETED: js_typed_array_create. TypedArraySpeciesCreate's Construct is a real Construct(C, «len») on the
   trampoline now — the same conversion that took js_typed_array_from and js_typed_array_of below — and its
   validation lives with it (see the construct-settled arm that names this function). What was left here was the
   C twin: a second implementation of the same step, reachable by anyone who reached for it, running the page's
   subclass constructor with JS_CallConstructor from a C activation with no flow base. It had no callers at all,
   which is the only reason the interpreter's recursion cycle survived this long without anyone noticing whose
   edge it was. */

/* DELETED: js_typed_array_from. Every part of it is a phase of the consume walk it now declares at its own
   definition — the create is a real Construct(C, «len») on the tramp and the sets follow it — so the body was
   already only a DFAIL and no pointer names it. */

/* DELETED: js_typed_array_of. Every part of it is a phase of the consume walk it now declares at its own
   definition — the create is a real Construct(C, «len») on the tramp and the sets follow it — so the body was
   already only a DFAIL and no pointer names it. */

/* 23.2.3.6 steps 4 onwards, with every argument ALREADY a primitive and `len` captured at step 2 — the clamp is
   against the length the spec read BEFORE the coercions, and the re-clamp below is what a valueOf that resized the
   buffer leaves to do. Nothing here runs user code. */
/* 23.2.3.6 from step 8 onwards: every index is ALREADY the ToIntegerOrInfinity result, clamped against the `len`
   step 3 read. The conversions live in the step machine's coercion loop because the spec interleaves each one
   with its own argument's ToPrimitive; nothing below invokes user code. */
static JSValue js_typed_array_copywithin_build(JSContext *ctx, JSValueConst this_val, int to, int from,
                                               int final, int len)
{
    JSObject *p;
    int count, shift, space;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;

    if (typed_array_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);
    cow_capture_bytes(ctx, p);   /* the memmove below is a raw byte write — see cow_capture_bytes */

    // RAB may have been resized by evil .valueOf method
    space = p->u.array.count - max_int(to, from);
    count = min_int(final - from, len - to);
    count = min_int(count, space);
    if (count > 0) {
        JSArrayBuffer *cabuf = p->u.typed_array->buffer->u.array_buffer;

        shift = typed_array_size_log2(p->class_id);
        js_ab_spans_copy(ctx, cabuf, p->u.typed_array->offset + (uint32_t)(to << shift),
                         cabuf, p->u.typed_array->offset + (uint32_t)(from << shift), (uint32_t)(count << shift));
        memmove(p->u.array.u.uint8_ptr + (to << shift),
                p->u.array.u.uint8_ptr + (from << shift),
                count << shift);
    }
    return js_dup(this_val);
}

/* 23.2.3.9 step 4: the fill VALUE's numeric conversion, split out of the builder because the spec orders it
   BEFORE `start`'s ToIntegerOrInfinity — `fill(Symbol(), {valueOf(){…}})` must throw its TypeError without ever
   running the second argument's valueOf. The operand is already a primitive, so nothing here invokes user code. */
static int js_typed_array_fill_value(JSContext *ctx, JSObject *p, JSValueConst val, uint64_t *pv64)
{
    uint64_t v64;

    if (p->class_id == JS_CLASS_UINT8C_ARRAY) {
        int32_t v;
        if (JS_ToUint8ClampFree(ctx, &v, js_dup(val)))
            return -1;
        v64 = v;
    } else if (p->class_id <= JS_CLASS_UINT32_ARRAY) {
        uint32_t v;
        if (JS_ToUint32(ctx, &v, val))
            return -1;
        v64 = v;
    } else
    if (p->class_id <= JS_CLASS_BIG_UINT64_ARRAY) {
        if (JS_ToBigInt64(ctx, (int64_t *)&v64, val))
            return -1;
    } else {
        double d;
        if (JS_ToFloat64(ctx, &d, val))
            return -1;
        if (p->class_id == JS_CLASS_FLOAT16_ARRAY) {
            v64 = tofp16(d);
        } else if (p->class_id == JS_CLASS_FLOAT32_ARRAY) {
            union {
                float f;
                uint32_t u32;
            } u;
            u.f = d;
            v64 = u.u32;
        } else {
            JSFloat64Union u;
            u.d = d;
            v64 = u.u64;
        }
    }
    *pv64 = v64;
    return 0;
}

/* 23.2.3.9 from step 8 onwards, with the value ALREADY converted and both bounds ALREADY the clamped
   ToIntegerOrInfinity results — a resizable buffer shrunk by the page's valueOf must clamp against the length the
   spec read BEFORE the coercions, which is why the caller holds it. Nothing below invokes user code. */
static JSValue js_typed_array_fill_build(JSContext *ctx, JSValueConst this_val, uint64_t v64, int k, int final)
{
    JSObject *p;
    int shift;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;

    if (typed_array_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);
    cow_capture_bytes(ctx, p);   /* the memset/element loop below is a raw byte write — see cow_capture_bytes */

    // RAB may have been resized by evil .valueOf method
    final = min_int(final, p->u.array.count);
    shift = typed_array_size_log2(p->class_id);
    switch(shift) {
    case 0:
        if (k < final) {
            memset(p->u.array.u.uint8_ptr + k, v64, final - k);
        }
        break;
    case 1:
        for(; k < final; k++) {
            p->u.array.u.uint16_ptr[k] = v64;
        }
        break;
    case 2:
        for(; k < final; k++) {
            p->u.array.u.uint32_ptr[k] = v64;
        }
        break;
    case 3:
        for(; k < final; k++) {
            p->u.array.u.uint64_ptr[k] = v64;
        }
        break;
    default:
        abort();
    }
    return js_dup(this_val);
}



/* 23.2.3.16/.20/.14 from step 3 onwards, with the fromIndex ALREADY a primitive and `len` captured at step 2.
   The sought VALUE is never coerced — it is compared by type — and the scan below invokes nothing. */
static JSValue js_typed_array_indexof_build(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                            int len, int special)
{
    JSObject *p;
    int tag, is_int, is_bigint, k, stop, inc, res = -1;
    int64_t v64;
    double d;
    float f;
    uint16_t hf;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;

    if (len == 0)
        goto done;

    if (special == special_lastIndexOf) {
        k = len - 1;
        if (argc > 1) {
            int64_t k1;
            if (JS_ToInt64Clamp(ctx, &k1, argv[1], -1, len - 1, len))
                goto exception;
            k = k1;
            if (k < 0)
                goto done;
        }
        stop = -1;
        inc = -1;
    } else {
        k = 0;
        if (argc > 1) {
            if (JS_ToInt32Sat(ctx, &k, argv[1]))
                goto exception;
            if (k < 0) {
                k += len;
                if (k < 0)
                    k = 0;
            } else if (k > len) {
                k = len;
            }
        }
        stop = len;
        inc = 1;
    }

    /* if the array was detached, no need to go further (but no
       exception is raised) */
    if (typed_array_is_oob(p) ||
        (len > p->u.array.count && special == special_includes)) {
        /* includes() reads with Get (step 11.a), so an index past the CURRENT length yields undefined and a
           searched undefined matches. indexOf/lastIndexOf ask HasProperty instead: those indices are ABSENT, so
           the scan must CONTINUE below them rather than bail — the surviving elements are still in range,
           because the index arithmetic above used the ORIGINAL length. */
        if (special == special_includes)
            if (JS_IsUndefined(argv[0]))
                if (k < typed_array_length(p))
                    res = 0;
        goto done;
    }

    /* A SHRINK inside fromIndex's valueOf does not abandon the scan: the spec's loop asks HasProperty per index,
       so indices past the new length are absent and the scan continues BELOW them. The index arithmetic above
       still used the ORIGINAL length, which is what makes a negative fromIndex relative to it. */
    len = min_int(len, p->u.array.count);
    if (len == 0)
        goto done;
    if (special == special_lastIndexOf) {
        k = min_int(k, len - 1);   /* k is an INDEX here, so len-1: min(k, len) would read one past the end */
        if (k < 0)
            goto done;
    } else {
        k = min_int(k, len);
        stop = min_int(stop, len);
    }

    is_bigint = 0;
    is_int = 0; /* avoid warning */
    v64 = 0; /* avoid warning */
    tag = JS_VALUE_GET_NORM_TAG(argv[0]);
    if (tag == JS_TAG_INT) {
        is_int = 1;
        v64 = JS_VALUE_GET_INT(argv[0]);
        d = v64;
    } else
    if (tag == JS_TAG_FLOAT64) {
        d = JS_VALUE_GET_FLOAT64(argv[0]);
        if (d >= INT64_MIN && d < 0x1p63) {
            v64 = d;
            is_int = (v64 == d);
        }
    } else if (tag == JS_TAG_BIG_INT || tag == JS_TAG_SHORT_BIG_INT) {
        JSBigIntBuf buf1;
        JSBigInt *p1;
        int sz = (64 / JS_LIMB_BITS);
        if (tag == JS_TAG_SHORT_BIG_INT)
            p1 = js_bigint_set_short(&buf1, argv[0]);
        else
            p1 = JS_VALUE_GET_PTR(argv[0]);

        if (p->class_id == JS_CLASS_BIG_INT64_ARRAY) {
            if (p1->len > sz)
                goto done; /* does not fit an int64 : cannot be found */
        } else if (p->class_id == JS_CLASS_BIG_UINT64_ARRAY) {
            if (js_bigint_sign(p1))
                goto done; /* v < 0 */
            if (p1->len <= sz) {
                /* OK */
            } else if (p1->len == sz + 1 && p1->tab[sz] == 0) {
                /* 2^63 <= v <= 2^64-1 */
            } else {
                goto done;
            }
        } else {
            goto done;
        }
        if (JS_ToBigInt64(ctx, &v64, argv[0]))
            goto exception;
        d = 0;
        is_bigint = 1;
    } else {
        goto done;
    }

    switch (p->class_id) {
    case JS_CLASS_INT8_ARRAY:
        if (is_int && (int8_t)v64 == v64)
            goto scan8;
        break;
    case JS_CLASS_UINT8C_ARRAY:
    case JS_CLASS_UINT8_ARRAY:
        if (is_int && (uint8_t)v64 == v64) {
            const uint8_t *pv, *pp;
            uint16_t v;
        scan8:
            pv = p->u.array.u.uint8_ptr;
            v = v64;
            if (inc > 0) {
                pp = NULL;
                if (pv)
                    pp = memchr(pv + k, v, len - k);
                if (pp)
                    res = pp - pv;
            } else {
                for (; k != stop; k += inc) {
                    if (pv[k] == v) {
                        res = k;
                        break;
                    }
                }
            }
        }
        break;
    case JS_CLASS_INT16_ARRAY:
        if (is_int && (int16_t)v64 == v64)
            goto scan16;
        break;
    case JS_CLASS_UINT16_ARRAY:
        if (is_int && (uint16_t)v64 == v64) {
            const uint16_t *pv;
            uint16_t v;
        scan16:
            pv = p->u.array.u.uint16_ptr;
            v = v64;
            for (; k != stop; k += inc) {
                if (pv[k] == v) {
                    res = k;
                    break;
                }
            }
        }
        break;
    case JS_CLASS_INT32_ARRAY:
        if (is_int && (int32_t)v64 == v64)
            goto scan32;
        break;
    case JS_CLASS_UINT32_ARRAY:
        if (is_int && (uint32_t)v64 == v64) {
            const uint32_t *pv;
            uint32_t v;
        scan32:
            pv = p->u.array.u.uint32_ptr;
            v = v64;
            for (; k != stop; k += inc) {
                if (pv[k] == v) {
                    res = k;
                    break;
                }
            }
        }
        break;
    case JS_CLASS_FLOAT16_ARRAY:
        if (is_bigint)
            break;
        if (isnan(d)) {
            const uint16_t *pv = p->u.array.u.fp16_ptr;
            /* special case: indexOf returns -1, includes finds NaN */
            if (special != special_includes)
                goto done;
            for (; k != stop; k += inc) {
                if (isfp16nan(pv[k])) {
                    res = k;
                    break;
                }
            }
        } else if (d == 0) {
            // special case: includes also finds negative zero
            const uint16_t *pv = p->u.array.u.fp16_ptr;
            for (; k != stop; k += inc) {
                if (isfp16zero(pv[k])) {
                    res = k;
                    break;
                }
            }
        } else if (hf = tofp16(d), d == fromfp16(hf)) {
            const uint16_t *pv = p->u.array.u.fp16_ptr;
            for (; k != stop; k += inc) {
                if (pv[k] == hf) {
                    res = k;
                    break;
                }
            }
        }
        break;
    case JS_CLASS_FLOAT32_ARRAY:
        if (is_bigint)
            break;
        if (isnan(d)) {
            const float *pv = p->u.array.u.float_ptr;
            /* special case: indexOf returns -1, includes finds NaN */
            if (special != special_includes)
                goto done;
            for (; k != stop; k += inc) {
                if (isnan(pv[k])) {
                    res = k;
                    break;
                }
            }
        } else if ((f = (float)d) == d) {
            const float *pv = p->u.array.u.float_ptr;
            for (; k != stop; k += inc) {
                if (pv[k] == f) {
                    res = k;
                    break;
                }
            }
        }
        break;
    case JS_CLASS_FLOAT64_ARRAY:
        if (is_bigint)
            break;
        if (isnan(d)) {
            const double *pv = p->u.array.u.double_ptr;
            /* special case: indexOf returns -1, includes finds NaN */
            if (special != special_includes)
                goto done;
            for (; k != stop; k += inc) {
                if (isnan(pv[k])) {
                    res = k;
                    break;
                }
            }
        } else {
            const double *pv = p->u.array.u.double_ptr;
            for (; k != stop; k += inc) {
                if (pv[k] == d) {
                    res = k;
                    break;
                }
            }
        }
        break;
    case JS_CLASS_BIG_INT64_ARRAY:
        if (is_bigint) {
            goto scan64;
        }
        break;
    case JS_CLASS_BIG_UINT64_ARRAY:
        if (is_bigint) {
            const uint64_t *pv;
            uint64_t v;
        scan64:
            pv = p->u.array.u.uint64_ptr;
            v = v64;
            for (; k != stop; k += inc) {
                if (pv[k] == v) {
                    res = k;
                    break;
                }
            }
        }
        break;
    }

done:
    if (special == special_includes)
        return js_bool(res >= 0);
    else
        return js_int32(res);

exception:
    return JS_EXCEPTION;
}

static JSValue js_typed_array_reverse(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSObject *p;
    int len;

    len = js_typed_array_get_length_unsafe(ctx, this_val);
    if (len < 0)
        return JS_EXCEPTION;
    p = JS_VALUE_GET_OBJ(this_val);
    if (typed_array_is_immutable(p))
        return JS_ThrowTypeErrorImmutableArrayBuffer(ctx);
    cow_capture_bytes(ctx, p);   /* the swap loops below write the storage in place — see cow_capture_bytes */
    if (len > 0) {
        switch (typed_array_size_log2(p->class_id)) {
        case 0:
            {
                uint8_t *p1 = p->u.array.u.uint8_ptr;
                uint8_t *p2 = p1 + len - 1;
                while (p1 < p2) {
                    uint8_t v = *p1;
                    *p1++ = *p2;
                    *p2-- = v;
                }
            }
            break;
        case 1:
            {
                uint16_t *p1 = p->u.array.u.uint16_ptr;
                uint16_t *p2 = p1 + len - 1;
                while (p1 < p2) {
                    uint16_t v = *p1;
                    *p1++ = *p2;
                    *p2-- = v;
                }
            }
            break;
        case 2:
            {
                uint32_t *p1 = p->u.array.u.uint32_ptr;
                uint32_t *p2 = p1 + len - 1;
                while (p1 < p2) {
                    uint32_t v = *p1;
                    *p1++ = *p2;
                    *p2-- = v;
                }
            }
            break;
        case 3:
            {
                uint64_t *p1 = p->u.array.u.uint64_ptr;
                uint64_t *p2 = p1 + len - 1;
                while (p1 < p2) {
                    uint64_t v = *p1;
                    *p1++ = *p2;
                    *p2-- = v;
                }
            }
            break;
        default:
            abort();
        }
    }
    return js_dup(this_val);
}

static JSValue js_typed_array_toReversed(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    JSValue arr, ret;
    JSObject *p;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    arr = js_typed_array_constructor_ta(ctx, JS_UNDEFINED, this_val,
                                        p->class_id, p->u.array.count);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    ret = js_typed_array_reverse(ctx, arr, argc, argv);
    JS_FreeValue(ctx, arr);
    return ret;
}

/* %TypedArray%.prototype.slice, 23.2.3.27. Its TypedArraySpeciesCreate is a real Construct of the species
   constructor, which is USER code — reached through JS_CallConstructor from a C body it drove to completion, so a
   custom species ctor with a loop (or one that detaches the buffer, which is what the detached-buffer-custom-ctor
   tests do) had nowhere to suspend. As a step machine the create is a CONSTRUCT step; the copy that follows is
   pure C over two typed arrays and needs no further suspension. */

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_ta_slice_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSTASlice *s = st;
    v->val(ctx, &s->arr);
    v->val(ctx, &s->src);
    v->val(ctx, &s->cb_args[0]);
    v->val(ctx, &s->cb_args[1]);
    v->val(ctx, &s->prim[0]);
    v->val(ctx, &s->prim[1]);
    v->val(ctx, &s->start_v);
    v->val(ctx, &s->pred);
}

/* Request ToPrimitive on hdr.argv[i] when it is an object, so a user valueOf on `start` or `end` SUSPENDS.
   This is what init-as-step-0 buys: the coercions the spec puts in front of the copy are now ordinary steps.
   The request goes in the header's cb_coerce, NEVER in cb_args: cb_args[0] is the species constructor, which
   js_ta_slice_visit declares as owned, so parking a borrowed operand there over-freed the caller's object the
   moment the abrupt path started tearing the state down (it used to leak instead, which hid it). */
static bool ta_slice_toprim(JSTASlice *s, int i, JSValue **out_cb, int *out_argc)
{
    JSValueConst v = step_arg(&s->hdr, i);
    if (!js_toprim_operand(v))
        return false;                   /* a primitive, or an unknown, whose coercion is TASLICE_RANGE's */
    s->hdr.cb_coerce[0] = (JSValue)v;   /* borrowed: hdr.argv holds the reference */
    *out_cb = s->hdr.cb_coerce; *out_argc = HINT_NUMBER;
    return true;
}

/* 7.3.22 SpeciesConstructor's two reads share ONE stage here because step_speciesctor_run carries its own
   cursor across them — the sub-sequence pattern JSStepHdr's len_phase names — and the CONSTRUCT is the stage
   after it. */
#define TASLICE_STAGES(X) \
    X(TASLICE_VALIDATE, "23.2.3.27 steps 1-3 (taRecord is ValidateTypedArray(O, seq-cst); srcArrayLength)") \
    X(TASLICE_START,    "23.2.3.27 step 4 (startIndex is ToClampedIndex(start, srcArrayLength))") \
    X(TASLICE_END,      "23.2.3.27 steps 5-6 (an undefined end is srcArrayLength, else endIndex is " \
                        "ToClampedIndex(end, srcArrayLength); countBytes is max(endIndex - startIndex, 0))") \
    X(TASLICE_RANGE,    "23.2.3.27 steps 4-6 over an unknown start, end or sourceArrayLength (a stated start forks)") \
    X(TASLICE_SPECIES,  "23.2.3.27 step 7 via 7.3.22 (C is Get(O, \"constructor\"), then S is Get(C, @@species))") \
    X(TASLICE_CREATE,   "23.2.3.27 step 7 (resultArray is TypedArraySpeciesCreate(obj, << F(countBytes) >>))") \
    X(TASLICE_COPY,     "23.2.3.27 step 8 (if countBytes > 0, the elements are copied)")
enum { TASLICE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_ta_slice_steps[] = { TASLICE_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_ta_store_element(JSContext *ctx, JSObject *p, int64_t k, JSValue v);

/* 23.2.3.27 steps 8.7.3.1-2 over the target's example window where startIndex or a [[ByteOffset]] is unknown: each
   of the `n` elements is TypedArrayGetElement of the source at startIndex + n, stored by TypedArraySetElement
   (js_ta_store_element). An unknown startIndex makes each value an unknown derived from it and the element its
   example index reads. 0 or -1. */
static int js_ta_slice_copy_unknown(JSContext *ctx, JSTASlice *s, JSObject *p, JSObject *p1, int n)
{
    for (int k = 0; k < n; k++) {
        uint32_t at = (uint32_t)s->start + (uint32_t)k;
        JSValue v, ex, ops[3];

        if (at < p->u.array.count) {
            bool got = js_get_fast_array_element(ctx, p, at, &v);
            DCHECK(got, "a source element below its stored count has no value");
        } else {
            v = js_ta_get_past_example(ctx, p, at);
        }
        if (!JS_IsUninitialized(s->start_v)) {
            ex = js_value_is_concolic(v) ? (js_concolic_held(v) ? js_concolic_operand(ctx, v) : JS_UNDEFINED)
                                         : js_dup(v);
            ops[0] = s->start_v;
            ops[1] = js_int32(k);
            ops[2] = v;
            {
                JSValue d = g_concolic.derived(ctx, "23.2.3.27 step 8.7.3.1 (kValue is TypedArrayGetElement(obj, k))",
                                               ops, 3, ex);
                DCHECK(!JS_IsUninitialized(d), "an unknown startIndex derived no unknown element");
                JS_FreeValue(ctx, v);
                v = d;
            }
        }
        if (js_ta_store_element(ctx, p1, k, v) < 0)
            return -1;
    }
    return 0;
}

static int js_ta_slice_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    static const JSTAClampSteps steps = {
        "23.2.3.27 step 4 (startIndex is ToClampedIndex(start, sourceArrayLength))",
        "23.2.3.27 step 5 (endIndex is ToClampedIndex(end, sourceArrayLength))",
        "23.2.3.27 step 4 start within sourceArrayLength" };
    JSTASlice *s = st;
    JSObject *p, *p1;
    int n, shift, space, count, r, arm;
    JSValue val;

    switch (s->hdr.stage) {
    case TASLICE_VALIDATE:   /* ValidateTypedArray + capture the pre-coercion length, then coerce `start` */
        JS_FreeValue(ctx, cb_result);
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->src = s->arr = JS_UNDEFINED;
        s->cb_args[0] = s->cb_args[1] = JS_UNDEFINED;
        s->prim[0] = s->prim[1] = JS_UNDEFINED;
        s->start_v = s->pred = JS_UNINITIALIZED;
        s->unknown = s->have = s->copy_phase = 0;
        p = get_typed_array(ctx, s->hdr.this_val);
        if (!p) return -1;
        if (typed_array_is_oob(p)) { JS_ThrowTypeErrorArrayBufferOOB(ctx); return -1; }
        s->len = p->u.array.count;
        s->class_id = p->class_id;
        s->src = js_dup(s->hdr.this_val);
        s->hdr.stage = TASLICE_START;
        if (ta_slice_toprim(s, 0, out_cb, out_argc)) return 5;
        cb_result = js_dup(step_arg(&s->hdr, 0));
        /* fall through */
    case TASLICE_START:   /* cb_result is `start` as a primitive, or an unknown */
        s->prim[0] = cb_result;
        /* a stated start's ToIntegerOrInfinity runs here, before `end` is coerced, as step 4 is before step 5 */
        if (!js_value_is_concolic(s->prim[0]) && JS_ToInt32Clamp(ctx, &s->start, s->prim[0], 0, s->len, s->len))
            return -1;
        s->final = s->len;
        s->hdr.stage = TASLICE_END;
        if (s->hdr.argc > 1 && !JS_IsUndefined(s->hdr.argv[1])) {
            if (ta_slice_toprim(s, 1, out_cb, out_argc)) return 5;
            cb_result = js_dup(s->hdr.argv[1]);
        } else {
            cb_result = JS_UNINITIALIZED;   /* absent end: `final` stays at the length */
        }
        /* fall through */
    case TASLICE_END:   /* cb_result is `end` as a primitive or an unknown, or UNINITIALIZED when there was none */
        s->prim[1] = cb_result;
        cb_result = JS_UNDEFINED;
        if (!JS_IsUninitialized(s->prim[1]) && !js_value_is_concolic(s->prim[1]) &&
            JS_ToInt32Clamp(ctx, &s->final, s->prim[1], 0, s->len, s->len))
            return -1;
        p = get_typed_array(ctx, s->src);
        if (p && (!JS_IsUninitialized(p->u.typed_array->length_unknown) || js_value_is_concolic(s->prim[0]) ||
                  js_value_is_concolic(s->prim[1]))) {
            s->hdr.stage = TASLICE_RANGE;
        } else {
            s->count = max_int(s->final - s->start, 0);
            s->cb_args[1] = js_int32(s->count);
            s->hdr.stage = TASLICE_SPECIES;
        }
        /* fall through */
    case TASLICE_RANGE:
        if (s->hdr.stage == TASLICE_RANGE) {
            JSValue len, start_v, final_v, ops[2];
            int64_t start_ex = 0, final_ex = 0;
            bool have;

            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            p = get_typed_array(ctx, s->src);
            if (!p) return -1;
            len = JS_IsUninitialized(p->u.typed_array->length_unknown) ? js_int64(s->len)
                                                                        : js_dup(p->u.typed_array->length_unknown);
            r = js_ta_clamped_bounds(ctx, &s->hdr, len, s->len, s->prim[0], s->prim[1], &s->pred, &steps,
                                     &start_v, &final_v, &start_ex, &final_ex, &have);
            if (r) { JS_FreeValue(ctx, len); return r < 0 ? -1 : r; }
            JS_FreeValue(ctx, s->pred);
            s->pred = JS_UNINITIALIZED;
            ops[0] = final_v;
            ops[1] = start_v;
            s->cb_args[1] = g_concolic.derived(ctx, "23.2.3.27 step 6 (countBytes is max(endIndex - startIndex, 0))",
                                               ops, 2, have ? js_int64(max_int64(final_ex - start_ex, 0))
                                                            : JS_UNDEFINED);
            if (JS_IsUninitialized(s->cb_args[1]))
                s->cb_args[1] = js_int64(max_int64(final_ex - start_ex, 0));
            else
                g_concolic.type_fix(ctx, s->cb_args[1], JS_LANG_TYPE_NUMBER);   /* step 7's 𝔽(countBytes) */
            DCHECK(start_ex >= 0 && start_ex <= INT32_MAX && final_ex >= 0 && final_ex <= INT32_MAX,
                   "a clamped index's example is outside the typed array's index range");
            s->start = (int)start_ex;
            s->final = (int)final_ex;
            s->count = have ? max_int(s->final - s->start, 0) : 0;
            s->have = have;
            s->unknown = 1;
            if (js_value_is_concolic(start_v)) s->start_v = start_v;
            else JS_FreeValue(ctx, start_v);
            JS_FreeValue(ctx, final_v);
            JS_FreeValue(ctx, len);
            s->hdr.stage = TASLICE_SPECIES;
        }
        /* fall through */
    case TASLICE_SPECIES:
        /* SpeciesConstructor reads .constructor and @@species IN THIS ORDER, after the indices and before the
           length is computed into the argument — keep the observable reads where the spec puts them. BOTH are the
           page's [[Get]]s, so they are requests: JS_SpeciesConstructor ran them from C, and a getter with a loop
           in it aborted at its back-edge with no flow base. */
        { JSValue ctor;
          r = step_speciesctor_run(ctx, &s->hdr, s->hdr.this_val, JS_UNDEFINED, cb_result, &ctor, out_cb, out_argc);
          if (r) return r < 0 ? -1 : r;
          s->hdr.stage = TASLICE_CREATE;
          if (JS_IsUndefined(ctor) && js_value_is_concolic(s->cb_args[1]))
              ctor = JS_GetClassCtor(ctx, s->class_id);   /* the default constructor, routed as a Construct */
          if (JS_IsUndefined(ctor)) {
              /* no species: the default constructor for this element type, which is C — nothing to suspend, so
                 it is created here and the machine goes straight to the copy. */
              JSValue arr = js_typed_array_constructor(ctx, JS_UNDEFINED, 1, vc(&s->cb_args[1]), s->class_id);
              if (JS_IsException(arr)) return -1;
              s->arr = arr; s->created = 1;
          } else {
              s->cb_args[0] = ctor;   /* owned */
          } }
        cb_result = JS_UNDEFINED;
        break;
    }

    if (s->hdr.stage == TASLICE_CREATE) {
        if (!s->created) {
            if (!s->asked) {
                /* The FIRST entry carries JS_UNDEFINED, not UNINITIALIZED — do_step_tramp seeds the driver with
                   UNDEFINED — so "have I asked yet" is state, never a tag test. Reading it off the tag made the
                   first entry look like a settled construct, which validated JS_UNDEFINED as a typed array. */
                s->asked = 1;
                JS_FreeValue(ctx, cb_result);
                *out_cb = s->cb_args; *out_argc = 1;   /* [ctor, count] */
                return 4;
            }
            /* the construct settled: TypedArrayCreateFromConstructor's validation, exactly as
               js_typed_array_create performs it — ValidateTypedArray, then the immutable-buffer check; the length
               floor is TASLICE_COPY's first test. */
            s->arr = cb_result;
            s->created = 1;
            if (js_typed_array_get_length_unsafe(ctx, s->arr) < 0) return -1;
            if (typed_array_is_immutable(JS_VALUE_GET_OBJ(s->arr))) {
                JS_ThrowTypeErrorImmutableArrayBuffer(ctx);
                return -1;
            }
        } else {
            JS_FreeValue(ctx, cb_result);   /* the no-species path: nothing was requested, this is the seed */
        }
        cb_result = JS_UNDEFINED;
        s->hdr.stage = TASLICE_COPY;
    }

    DCHECK(s->hdr.stage == TASLICE_COPY, "%TypedArray%.prototype.slice resumed in no stage");
    JS_FreeValue(ctx, cb_result);
    p1 = JS_VALUE_GET_OBJ(s->arr);
    while (s->copy_phase < 2 && s->unknown) {
        /* 23.2.4.1 TypedArrayCreateFromConstructor step 4.3's length floor, then step 8's countBytes > 0:
           predicates over the result's length and the unknown count, so both arms run */
        if (JS_IsUninitialized(s->pred)) {
            JSValue sp[2];

            if (s->copy_phase == 0) {
                sp[0] = JS_IsUninitialized(p1->u.typed_array->length_unknown) ? js_int32(p1->u.array.count)
                                                                               : js_dup(p1->u.typed_array->length_unknown);
                sp[1] = js_dup(s->cb_args[1]);
            } else {
                sp[0] = js_int32(0);
                sp[1] = js_dup(s->cb_args[1]);
            }
            if (!js_value_is_concolic(sp[0]) && !js_value_is_concolic(sp[1])) {
                double a = 0, b = 0;

                JS_ToFloat64(ctx, &a, sp[0]);
                JS_ToFloat64(ctx, &b, sp[1]);
                s->pred = js_bool(a < b);
            } else {
                CHECK(g_concolic.rel(ctx, sp + 2, OP_lt), "the relation hook declined a slice's count test");
                s->pred = sp[0];
            }
        }
        r = step_tobool_run(ctx, &s->hdr, s->pred, s->copy_phase == 0
                            ? "23.2.4.1 TypedArrayCreateFromConstructor step 4.3 (length < argList[0])"
                            : "23.2.3.27 step 8 (countBytes > 0)", &arm);
        if (r) return r < 0 ? -1 : r;
        JS_FreeValue(ctx, s->pred);
        s->pred = JS_UNINITIALIZED;
        if (s->copy_phase == 0 && arm) { JS_ThrowTypeError(ctx, "TypedArray length is too small"); return -1; }
        if (s->copy_phase == 1 && !arm) return 0;
        s->copy_phase++;
    }
    if (!s->unknown && js_typed_array_get_length_unsafe(ctx, s->arr) < s->count) {
        JS_ThrowTypeError(ctx, "TypedArray length is too small");
        return -1;
    }
    /* The copy fills the target's example window: `count` is countBytes, or its example where it is unknown.
       Named residual. Not covered: the elements past that window where countBytes is unknown, which a read past
       it answers from the target's own unknown length rather than from the source's bytes. Next diff builds: a
       store past an unknown-length view's example window that 10.4.5.17's read past it answers. Absence shows
       as: a sink fed from such a slice whose root names the bounds' sources but not the source buffer's own. */
    count = s->count;
    if (count > 0) {
        if (validate_typed_array(ctx, s->src) || validate_typed_array(ctx, s->arr))
            return -1;
        p = get_typed_array(ctx, s->src);
        p1 = get_typed_array(ctx, s->arr);
        if (p && p1 && (!JS_IsUninitialized(s->start_v) || !JS_IsUninitialized(p->u.typed_array->offset_unknown) ||
                        !JS_IsUninitialized(p1->u.typed_array->offset_unknown))) {
            return js_ta_slice_copy_unknown(ctx, s, p, p1, min_int(count, (int)p1->u.array.count));
        }
        /* s->len is the length read BEFORE start/end were coerced and before the species constructor ran. If the
           receiver's length has MOVED since (a valueOf or a ctor that resizes the backing buffer), `count` is
           stale and the block copy would read past the end — take the element-wise path, which re-reads the live
           count. Losing this comparison in the restructure segfaulted resize-count-bytes-to-zero. */
        if (p && p1 && s->len == p->u.array.count && p->class_id == p1->class_id
            && typed_array_length(p1) >= count && typed_array_length(p) >= s->start + count) {
            shift = typed_array_size_log2(p->class_id);
            {
                uint8_t *dst = p1->u.array.u.uint8_ptr;
                const uint8_t *src = p->u.array.u.uint8_ptr + (s->start << shift);
                size_t nbytes = (size_t)count << shift, bi;

                DCHECK_TA_WINDOW_STATED(p->u.typed_array, nbytes, "%TypedArray%.prototype.slice's byte copy");
                cow_capture_bytes(ctx, p1);   /* @@species may answer a view over a buffer siblings share */
                js_ab_spans_copy(ctx, p1->u.typed_array->buffer->u.array_buffer, p1->u.typed_array->offset,
                                 p->u.typed_array->buffer->u.array_buffer,
                                 p->u.typed_array->offset + (uint32_t)(s->start << shift), (uint32_t)nbytes);
                /* 23.2.3.27 step 8.f.ix copies FORWARD, one unit at a time. When @@species returns a view on
                   the SAME buffer at a HIGHER offset, that propagates the first element through the overlap —
                   memmove deliberately does the opposite (it preserves the source), producing [20,30,40,…]
                   where the spec produces [20,20,20,…]. Overlap the other way, and non-overlap, are identical
                   either way. */
                if (dst > src && (size_t)(dst - src) < nbytes)
                    for (bi = 0; bi < nbytes; bi++)
                        dst[bi] = src[bi];
                else
                    memmove(dst, src, nbytes);
            }
        } else {
            space = p ? max_int(0, p->u.array.count - s->start) : 0;
            count = min_int(count, space);
            for (n = 0; n < count; n++) {
                val = JS_GetPropertyValue(ctx, s->src, js_int32(s->start + n), __FILE__, __LINE__);
                if (JS_IsException(val))
                    return -1;
                if (JS_SetPropertyValue(ctx, s->arr, js_int32(n), val, JS_PROP_THROW) < 0)
                    return -1;
            }
        }
    }
    return 0;
}

static JSValue js_ta_slice_fini(JSContext *ctx, void *st, bool take_result)
{
    JSTASlice *s = st;
    JSValue r = take_result ? s->arr : JS_UNDEFINED;
    if (take_result) s->arr = JS_UNDEFINED;   /* handed to the caller; the declaration releases everything else */
    return r;
}

/* TWO algorithms, one machine, and their stage sets DIVERGE after the shared prefix: `at` has one stage left
   after the index and `set` has five, so each names its own tail rather than one list carrying labels the other
   algorithm has no step for. The SET tail's constants continue from the shared prefix, which is what keeps the
   two spellings' shared stages the same numbers. */
#define TAIDX_STAGES(X, VALIDATE, INDEX, TAIL) \
    X(TAIDX_VALIDATE, VALIDATE) \
    X(TAIDX_INDEX,    INDEX)    \
    TAIL(X)
#define TAIDX_TAIL_AT(X) \
    X(TAIDX_READ, "23.2.3.1 steps 5-6 (out of range returns undefined, else Get(O, ToString(k))) - and " \
                  "step 4's negative-relative half, which resolves here")
#define TAIDX_TAIL_SET(X) \
    X(TAIDX_OFF_SIGN, "23.2.3.26 step 5 over an unknown targetOffset (targetOffset < 0 forks; that arm throws)") \
    X(TAIDX_SRC,   "23.2.3.26 step 6 and 23.2.3.26.1 step 3 (source is ToObject(source); a typed-array source " \
                   "is set whole)") \
    X(TAIDX_TA_RANGE, "23.2.3.26.2 steps 13-14 over an unknown length, byteOffset or targetOffset (the range " \
                      "test forks)") \
    X(TAIDX_LEN,   "23.2.3.26.1 step 4 (sourceLength is LengthOfArrayLike(source))") \
    X(TAIDX_RANGE, "23.2.3.26.1 steps 5-6 over an unknown targetOffset (the range test forks)") \
    X(TAIDX_NEXT,  "23.2.3.26.1 step 8 (the next index k of the source)") \
    X(TAIDX_VALUE, "23.2.3.26.1 step 8.4 via 10.4.5.18 steps 1-2 (number is ToBigInt or ToNumber)") \
    X(TAIDX_GET,   "23.2.3.26.1 step 8.2 (value is Get(source, propertyKey))")
enum { TAIDX_STAGES(JS_STEP_STAGE_ENUM, 0, 0, TAIDX_TAIL_AT) };
enum { TAIDX_SET_TAIL_BASE = TAIDX_INDEX, TAIDX_TAIL_SET(JS_STEP_STAGE_ENUM) };
static const char *const js_ta_at_steps[] = {
    TAIDX_STAGES(JS_STEP_STAGE_LABEL,
        "23.2.3.1 steps 1-3 (taRecord is ValidateTypedArray(O, seq-cst); len is TypedArrayLength(taRecord))",
        "23.2.3.1 step 4 (k is ToAbsoluteIndex(index, len)) - only its ToIntegerOrInfinity half runs "
        "in this stage, and that half is the page's code", TAIDX_TAIL_AT)
    NULL };
static const char *const js_ta_set_steps[] = {
    TAIDX_STAGES(JS_STEP_STAGE_LABEL,
        "23.2.3.26 steps 1-2 (target has [[TypedArrayName]]; its buffer is mutable)",
        "23.2.3.26 step 4 (targetOffset is ToIntegerOrInfinity(offset))", TAIDX_TAIL_SET)
    NULL };

/* Whether a typed array class's [[ContentType]] is bigint. */
static bool typed_array_is_bigint(JSClassID class_id)
{
    return class_id == JS_CLASS_BIG_INT64_ARRAY || class_id == JS_CLASS_BIG_UINT64_ARRAY;
}

/* Whether %TypedArray%.prototype.set's source is a typed array that SetTypedArrayFromTypedArray must read through
   an unknown: targetOffset, a length or a [[ByteOffset]] of either view, or a source holding unknown elements copied
   across element types (a same-type copy carries its spans with the bytes). */
static bool js_ta_set_from_ta_unknown_asks(JSContext *ctx, JSTAIdx *s)
{
    JSObject *p = get_typed_array(ctx, s->hdr.this_val), *sp;

    if (!p || JS_VALUE_GET_TAG(s->src) != JS_TAG_OBJECT || !is_typed_array(JS_VALUE_GET_OBJ(s->src)->class_id))
        return false;
    sp = JS_VALUE_GET_OBJ(s->src);
    return !JS_IsUninitialized(s->offset_v) ||
           !JS_IsUninitialized(p->u.typed_array->length_unknown) ||
           !JS_IsUninitialized(p->u.typed_array->offset_unknown) ||
           !JS_IsUninitialized(sp->u.typed_array->length_unknown) ||
           !JS_IsUninitialized(sp->u.typed_array->offset_unknown) ||
           (sp->class_id != p->class_id && sp->u.typed_array->buffer->u.array_buffer->n_span != 0);
}

/* 10.4.5.18 TypedArraySetElement of `v`, an element value another typed array answered (a Number, a BigInt or an
   unknown), at index `k` of `p`. An unknown is stored as the unknown-value arm stores it: its example through the
   ordinary store, then a span, or a write at an unknown offset where [[ByteOffset]] is one. Takes `v`. */
static int js_ta_store_element(JSContext *ctx, JSObject *p, int64_t k, JSValue v)
{
    JSTypedArray *ta = p->u.typed_array;
    int width = 1 << typed_array_size_log2(p->class_id);
    JSValue ex, at;

    if (!js_value_is_concolic(v))
        return JS_SetPropertyValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p), js_int64(k), v, JS_PROP_THROW) < 0 ? -1 : 0;
    if (typed_array_is_immutable(p) || typed_array_is_oob(p) || k >= p->u.array.count) {
        JS_FreeValue(ctx, v);                   /* step 3 skips the store */
        return 0;
    }
    if (!JS_IsUninitialized(ta->offset_unknown)) {
        at = js_ta_byte_index(ctx, ta, k * width);
        js_ab_span_set_any(ctx, ta->buffer, at, v);
        JS_FreeValue(ctx, at);
        JS_FreeValue(ctx, v);
        return 0;
    }
    ex = js_concolic_held(v) ? js_concolic_operand(ctx, v) : JS_UNDEFINED;
    DCHECK(!js_value_is_concolic(ex), "an unknown's example is another unknown");
    if (!JS_IsUndefined(ex) &&
        JS_SetPropertyValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p), js_int64(k), ex, JS_PROP_THROW) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    js_ab_span_set(ctx, ta->buffer, ta->offset + (uint32_t)(k * width), width, v);
    JS_FreeValue(ctx, v);
    return 0;
}

/* 23.2.3.26 step 4 over an unknown offset: targetOffset is an unknown derived from it, whose example, where the
   offset's is a primitive, is ToIntegerOrInfinity over that; `idx` takes it where it is a safe integer. */
static void js_ta_set_offset_unknown(JSContext *ctx, JSTAIdx *s)
{
    JSValueConst off = step_arg(&s->hdr, 1);
    JSValue ex = JS_UNDEFINED;
    double d = 0;

    s->idx = 0;
    s->off_have = false;
    if (js_concolic_held(off)) {
        JSValue e = js_concolic_operand(ctx, off);
        int tag = JS_VALUE_GET_NORM_TAG(e);

        if ((JS_IsNumber(e) || tag == JS_TAG_STRING || tag == JS_TAG_BOOL || tag == JS_TAG_NULL ||
             tag == JS_TAG_UNDEFINED) && JS_ToFloat64(ctx, &d, e) == 0) {
            d = isnan(d) ? 0 : trunc(d) + 0.0;
            ex = js_float64(d);
            if (fabs(d) <= MAX_SAFE_INTEGER) {
                s->idx = (int64_t)d;
                s->off_have = true;
            }
        }
        JS_FreeValue(ctx, e);
    }
    s->offset_v = g_concolic.derived(ctx, "23.2.3.26 step 4 (targetOffset is ToIntegerOrInfinity(offset))", &off, 1,
                                     ex);
    DCHECK(!JS_IsUninitialized(s->offset_v), "an unknown offset derived no unknown targetOffset");
    g_concolic.type_fix(ctx, s->offset_v, JS_LANG_TYPE_NUMBER);
}

/* sourceLength + targetOffset, the left side of 23.2.3.26.1 step 6's and 23.2.3.26.2 step 14's range test (`step`
   names which): a Number where both are stated, else an unknown derived from them, whose example is the sum of
   their examples where both have one. `slen` is borrowed. Owned. */
static JSValue js_ta_set_extent(JSContext *ctx, JSTAIdx *s, JSValueConst slen, const char *step)
{
    bool off_stated = JS_IsUninitialized(s->offset_v);
    JSValue ops[2], ex, r;
    double d = 0;
    bool have;

    ex = !js_value_is_concolic(slen) ? js_dup(slen)
       : js_concolic_held(slen) ? js_concolic_operand(ctx, slen) : JS_UNDEFINED;
    have = JS_IsNumber(ex) && JS_ToFloat64(ctx, &d, ex) == 0 && (off_stated || s->off_have);
    JS_FreeValue(ctx, ex);
    if (!js_value_is_concolic(slen) && off_stated)
        return js_float64(d + (double)s->idx);
    ops[0] = slen;
    ops[1] = off_stated ? JS_NewInt64(ctx, s->idx) : s->offset_v;
    r = g_concolic.derived(ctx, step, ops, 2, have ? js_float64(d + (double)s->idx) : JS_UNDEFINED);
    DCHECK(!JS_IsUninitialized(r), "an unknown sourceLength or targetOffset derived no unknown extent");
    return r;
}

/* The store set's loops make at element targetOffset + k of `p` where targetOffset is the unknown s->offset_v: an
   ANY span over the buffer keyed by that index (js_ab_span_set_any), carrying `v` (borrowed) where it is unknown.
   `step` names the loop's index step. The example world's store at the example index is the caller's. */
static void js_ta_set_span_at(JSContext *ctx, JSTAIdx *s, JSObject *p, int64_t k, JSValueConst v, const char *step)
{
    JSValue ops[2], at;

    if (typed_array_is_oob(p)) return;          /* 10.4.5.18 step 3: no index of the view is valid */
    ops[0] = s->offset_v;
    ops[1] = js_int64(k);
    at = g_concolic.derived(ctx, step, ops, 2, s->off_have ? js_int64(s->idx + k) : JS_UNDEFINED);
    DCHECK(!JS_IsUninitialized(at), "an unknown targetOffset derived no unknown target index");
    js_ab_span_set_any(ctx, p->u.typed_array->buffer, at, js_value_is_concolic(v) ? v : JS_UNINITIALIZED);
    JS_FreeValue(ctx, at);
}

/* 23.2.3.26.1 step 8.4, TypedArraySetElement(target, targetOffset + k, `v`) for k = s->i, `v` a primitive (taken).
   Over an unknown targetOffset the example world stores at the example index where this arm keeps one, and an arm
   that keeps none still runs 10.4.5.18 steps 1-2's conversion, whose throw is the page's. 0 or -1. */
static int js_ta_set_arraylike_store(JSContext *ctx, JSTAIdx *s, JSValue v)
{
    JSObject *p;

    if (JS_IsUninitialized(s->offset_v))
        return JS_SetPropertyUint32(ctx, s->hdr.this_val, s->idx + s->i, v) < 0 ? -1 : 0;
    p = get_typed_array(ctx, s->hdr.this_val);
    if (!p) { JS_FreeValue(ctx, v); return -1; }
    if (s->off_have) {
        if (JS_SetPropertyUint32(ctx, s->hdr.this_val, s->idx + s->i, js_dup(v)) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
    } else if (!js_value_is_concolic(v)) {
        JSValue n = typed_array_is_bigint(p->class_id) ? JS_ToBigIntFree(ctx, js_dup(v))
                                                       : JS_ToNumberFree(ctx, js_dup(v));

        if (JS_IsException(n)) { JS_FreeValue(ctx, v); return -1; }
        JS_FreeValue(ctx, n);
    }
    js_ta_set_span_at(ctx, s, p, s->i, v, "23.2.3.26.1 step 8.3 (targetIndex is targetOffset + k)");
    JS_FreeValue(ctx, v);
    return 0;
}

/* 23.2.3.26.2 SetTypedArrayFromTypedArray where js_ta_set_from_ta_unknown_asks holds. Step 14's RangeError is a
   predicate over the lengths and targetOffset, so it forks; on its in-range arm the source's example window is read
   first (step 17's clone) as 10.4.5.17 answers each element, then stored as 10.4.5.18 does, and an unknown source
   length makes its elements past the example one write at an unknown offset of the target, derived from them. Over
   an unknown targetOffset each element is also a write at an unknown index (js_ta_set_span_at), and the example
   world's stores run only where this arm keeps the example's whole window. 0, a request code, or -1. */
static int js_ta_set_from_ta_unknown(JSContext *ctx, JSTAIdx *s, JSValue cb_result)
{
    static const char ta_index_step[] = "23.2.3.26.2 step 19 (targetByteIndex is targetOffset x targetElementSize + "
                                        "targetByteOffset), in elements";
    JSObject *p = get_typed_array(ctx, s->hdr.this_val), *sp = JS_VALUE_GET_OBJ(s->src);
    JSTypedArray *ta, *sta;
    JSValue *vals, v, at;
    int64_t n, k;
    int arm, r;

    JS_FreeValue(ctx, cb_result);
    if (!p) return -1;
    if (JS_IsUninitialized(s->offset_v) && s->idx < 0) {   /* 23.2.3.26 step 5; TAIDX_OFF_SIGN forks an unknown */
        JS_ThrowRangeError(ctx, "invalid array length");
        return -1;
    }
    if (typed_array_is_oob(p) || typed_array_is_oob(sp)) {   /* steps 2 and 5 */
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        return -1;
    }
    ta = p->u.typed_array;
    sta = sp->u.typed_array;
    if (JS_IsUninitialized(s->pred)) {
        JSValue sv[2], slen;

        slen = JS_IsUninitialized(sta->length_unknown) ? js_int64(sp->u.array.count) : js_dup(sta->length_unknown);
        sv[0] = js_ta_set_extent(ctx, s, slen, "23.2.3.26.2 step 14 (sourceLength + targetOffset)");
        JS_FreeValue(ctx, slen);
        sv[1] = JS_IsUninitialized(ta->length_unknown) ? js_int64(p->u.array.count) : js_dup(ta->length_unknown);
        if (!js_value_is_concolic(sv[0]) && !js_value_is_concolic(sv[1])) {
            /* both lengths stated (an offset is what is unknown): the test is decided */
            s->pred = js_bool((double)sp->u.array.count + (double)s->idx <= (double)p->u.array.count);
        } else {
            CHECK(g_concolic.rel(ctx, sv + 2, OP_lte), "the relation hook declined set's range test");
            s->pred = sv[0];
        }
    }
    r = step_tobool_run(ctx, &s->hdr, s->pred, "23.2.3.26.2 step 14 range test", &arm);
    if (r) return r;
    if (!arm) {
        JS_ThrowRangeError(ctx, "invalid array length");
        return -1;
    }
    if (typed_array_is_bigint(p->class_id) != typed_array_is_bigint(sp->class_id)) {   /* step 15 */
        JS_ThrowTypeError(ctx, "incompatible typed array types");
        return -1;
    }
    n = sp->u.array.count;
    if (!JS_IsUninitialized(s->offset_v)) {
        s->off_have = s->off_have && n <= (int64_t)p->u.array.count - s->idx;
        n = min_int64(n, p->u.array.count);
    } else if (n > (int64_t)p->u.array.count - s->idx) {
        if (!JS_IsUninitialized(ta->length_unknown)) {
            DFAIL("23.2.3.26.2 steps 21-22 past an unknown-length target's example bytes: build a store past the "
                  "example window that a read past it answers");
            return -1;
        }
        n = max_int64((int64_t)p->u.array.count - s->idx, 0);   /* this arm bounds the source's length */
    }
    vals = js_malloc(ctx, sizeof(*vals) * (size_t)max_int64(n, 1));
    if (!vals) return -1;
    for (k = 0; k < n; k++) {
        bool got = js_get_fast_array_element(ctx, sp, (uint32_t)k, &vals[k]);
        DCHECK(got, "a source element below its stored count has no value");
    }
    for (k = 0; k < n; k++) {
        if (!JS_IsUninitialized(s->offset_v))
            js_ta_set_span_at(ctx, s, p, k, vals[k], ta_index_step);
        if (!JS_IsUninitialized(s->offset_v) && !s->off_have) {
            JS_FreeValue(ctx, vals[k]);
        } else if (js_ta_store_element(ctx, p, s->idx + k, vals[k]) < 0) {
            for (k++; k < n; k++) JS_FreeValue(ctx, vals[k]);
            js_free(ctx, vals);
            return -1;
        }
    }
    js_free(ctx, vals);
    if (!JS_IsUninitialized(sta->length_unknown) && n == sp->u.array.count) {
        v = js_ta_get_past_example(ctx, sp, (uint32_t)n);
        if (js_value_is_concolic(v) && !typed_array_is_immutable(p)) {
            if (!JS_IsUninitialized(s->offset_v)) {
                js_ta_set_span_at(ctx, s, p, n, v, ta_index_step);
            } else {
                at = js_ta_byte_index(ctx, ta, (s->idx + n) << typed_array_size_log2(p->class_id));
                js_ab_span_set_any(ctx, ta->buffer, at, v);
                JS_FreeValue(ctx, at);
            }
        }
        JS_FreeValue(ctx, v);
    }
    return 0;
}

static int js_ta_idx_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSTAIdx *s = st;
    JSObject *p;
    int64_t idx;
    int r;

    if (s->hdr.stage == TAIDX_VALIDATE) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        s->result = JS_UNDEFINED;
        s->src = JS_UNDEFINED; s->el = JS_UNDEFINED;
        s->pred = JS_UNINITIALIZED;
        s->offset_v = JS_UNINITIALIZED;
        s->off_have = false;
        p = get_typed_array(ctx, s->hdr.this_val);
        if (!p) return -1;
        if (s->hdr.arg == TAIDX_AT) {
            if (typed_array_is_oob(p)) { JS_ThrowTypeErrorArrayBufferOOB(ctx); return -1; }
            s->len = p->u.array.count;
        } else if (typed_array_is_immutable(p)) {
            JS_ThrowTypeErrorImmutableArrayBuffer(ctx);
            return -1;
        }
        s->hdr.stage = TAIDX_INDEX;
    }
    if (s->hdr.stage == TAIDX_INDEX && s->hdr.arg == TAIDX_SET && js_value_is_concolic(step_arg(&s->hdr, 1))) {
        /* Named residual. Not covered: an offset object whose valueOf answers an unknown runs on its example.
           Next diff: take the coerced primitive here and derive from it as from a direct unknown. How its absence
           shows: a set at an offset a page wraps in an object writes only at the example index. */
        js_ta_set_offset_unknown(ctx, s);
        s->hdr.stage = TAIDX_OFF_SIGN;
    }
    if (s->hdr.stage == TAIDX_INDEX) {
        JSValueConst v = (s->hdr.arg == TAIDX_AT) ? step_arg(&s->hdr, 0) : step_arg(&s->hdr, 1);
        r = step_toint64_run(ctx, &s->hdr, v, cb_result, &s->idx, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        /* the two spellings' third stage is a different step of a different algorithm, which is why they
           declare different tails; the constant is the one this mode's own list names. */
        s->hdr.stage = (s->hdr.arg == TAIDX_AT) ? (uint16_t)TAIDX_READ : (uint16_t)TAIDX_SRC;
    }
    /* The two modes DIVERGE here, and `cb_result` is still live: set's stages 3 and 5 consume it. Freeing it
       once for both before the split handed those stages a value that had already been released — harmless only
       because the values that reach them happen to be numbers. Each mode owns the result it was resumed with. */
    if (s->hdr.arg == TAIDX_SET) {
        int64_t dst_len;
        if (s->hdr.stage == TAIDX_OFF_SIGN) {
            int arm;

            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            if (JS_IsUninitialized(s->pred)) {
                JSValue sv[2];

                sv[0] = js_int32(0);
                sv[1] = js_dup(s->offset_v);
                CHECK(g_concolic.rel(ctx, sv + 2, OP_lte), "the relation hook declined set's targetOffset sign test");
                s->pred = sv[0];
            }
            r = step_tobool_run(ctx, &s->hdr, s->pred, "23.2.3.26 step 5 (targetOffset is at least 0)", &arm);
            if (r) return r;
            JS_FreeValue(ctx, s->pred);
            s->pred = JS_UNINITIALIZED;
            if (!arm) {
                JS_ThrowRangeError(ctx, "invalid array length");
                return -1;
            }
            s->off_have = s->off_have && s->idx >= 0;
            s->hdr.stage = TAIDX_SRC;
        }
        if (s->hdr.stage == TAIDX_SRC) {
            int h;
            s->src = JS_ToObject(ctx, step_arg(&s->hdr, 0));
            if (JS_IsException(s->src)) { s->src = JS_UNDEFINED; return -1; }
            if (js_ta_set_from_ta_unknown_asks(ctx, s)) {
                s->hdr.stage = TAIDX_TA_RANGE;
            } else {
                h = js_typed_array_set_ta(ctx, s->hdr.this_val, s->src, s->idx, &s->len);
                if (h < 0) return -1;
                if (h) { s->result = JS_UNDEFINED; return 0; }   /* a typed-array source needs no steps at all */
                s->hdr.stage = TAIDX_LEN;
            }
        }
        if (s->hdr.stage == TAIDX_TA_RANGE) {
            r = js_ta_set_from_ta_unknown(ctx, s, cb_result);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->result = JS_UNDEFINED;
            return 0;
        }
        if (s->hdr.stage == TAIDX_LEN) {   /* LengthOfArrayLike on the source, re-entered until it finishes */
            int r2 = step_length_run(ctx, &s->hdr, s->src, cb_result, &s->src_len, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r2) return r2 < 0 ? -1 : r2;
            /* reading the length can change |dst|; per spec the range check is against its OLD length */
            dst_len = s->len;
            if (JS_IsUninitialized(s->offset_v) && s->idx > dst_len - s->src_len) {
                JS_ThrowRangeError(ctx, "invalid array length");
                return -1;
            }
            s->hdr.stage = JS_IsUninitialized(s->offset_v) ? (uint16_t)TAIDX_NEXT : (uint16_t)TAIDX_RANGE;
        }
        if (s->hdr.stage == TAIDX_RANGE) {   /* steps 5-6 over the unknown targetOffset; step 5's +inf fails 6 */
            JSObject *tp = get_typed_array(ctx, s->hdr.this_val);
            int arm;

            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            if (!tp) return -1;
            if (JS_IsUninitialized(s->pred)) {
                JSValue sv[2];

                sv[0] = js_ta_set_extent(ctx, s, js_int64(s->src_len),
                                         "23.2.3.26.1 step 6 (sourceLength + targetOffset)");
                sv[1] = JS_IsUninitialized(tp->u.typed_array->length_unknown) ? js_int64(s->len)
                      : js_dup(tp->u.typed_array->length_unknown);
                CHECK(g_concolic.rel(ctx, sv + 2, OP_lte), "the relation hook declined set's range test");
                s->pred = sv[0];
            }
            r = step_tobool_run(ctx, &s->hdr, s->pred, "23.2.3.26.1 step 6 range test", &arm);
            if (r) return r;
            if (!arm) {
                JS_ThrowRangeError(ctx, "invalid array length");
                return -1;
            }
            s->off_have = s->off_have && s->idx <= s->len - s->src_len;
            s->hdr.stage = TAIDX_NEXT;
        }
        for (;;) {
            if (s->hdr.stage == TAIDX_GET) {
                /* 23.2.3.26.1 step 6.b's `? Get(src, Pk)`, re-entered until it finishes. The source is an
                   ARRAY-LIKE, so the read is an accessor or a Proxy `get` trap — the page's code, which
                   JS_GetPropertyUint32 ran from C with no flow base. */
                int r3 = step_getidx_run(ctx, &s->hdr, s->src, s->i, cb_result, &s->el, out_cb, out_argc);
                cb_result = JS_UNDEFINED;
                if (r3) return r3 < 0 ? -1 : r3;
                s->hdr.stage = TAIDX_NEXT;
                goto ta_have_element;
            }
            if (s->hdr.stage == TAIDX_VALUE) {          /* the element's primitive arrived */
                s->hdr.stage = TAIDX_NEXT;
                JS_FreeValue(ctx, s->el);     /* the OBJECT the primitive came from; cb_coerce only borrowed it */
                s->el = JS_UNDEFINED;
                r = js_ta_set_arraylike_store(ctx, s, cb_result);
                cb_result = JS_UNDEFINED;
                if (r < 0) return -1;
                s->i++;
            }
            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
            if (s->i >= s->src_len) { s->result = JS_UNDEFINED; return 0; }
            {
                int r3;
                s->hdr.stage = TAIDX_GET;
                r3 = step_getidx_run(ctx, &s->hdr, s->src, s->i, JS_UNDEFINED, &s->el, out_cb, out_argc);
                if (r3) return r3 < 0 ? -1 : r3;
                s->hdr.stage = TAIDX_NEXT;
            }
        ta_have_element:
            /* 10.4.5.18 TypedArraySetElement ( obj, index, value ) is a COERCION and a CONDITIONAL STORE, and
               only the store is conditional: the value is coerced to the target's content type FIRST, by steps
               1-2 -- ? ToBigInt for a BigInt view, ? ToNumber otherwise -- and only then does step 3's
               10.4.5.16 IsValidIntegerIndex ( obj, index ) decide whether the store happens. Skipping the whole
               element when the view has gone out of bounds skipped the COERCION too, so
               `bigIntTA.set({get length(){ detach(); return 1 }, 0: {valueOf: () => "huzzah!"}})` threw nothing
               where ToBigInt owes a SyntaxError. The write path already coerces before it range-checks, and an
               out-of-bounds index is not an error there -- which is exactly the spec's shape, so the guard is
               not needed for the store either. */
            if (JS_VALUE_GET_TAG(s->el) == JS_TAG_OBJECT) {
                s->hdr.stage = TAIDX_VALUE;
                s->hdr.cb_coerce[0] = s->el;   /* borrowed: the state holds it */
                *out_cb = s->hdr.cb_coerce; *out_argc = HINT_NUMBER;
                return 5;
            }
            {
                JSValue el = s->el;
                s->el = JS_UNDEFINED;
                if (js_ta_set_arraylike_store(ctx, s, el) < 0)
                    return -1;
            }
            s->i++;
        }
    }

    JS_FreeValue(ctx, cb_result);

    DCHECK(s->hdr.stage == TAIDX_READ, "%TypedArray%.prototype.at resumed in no stage");
    /* the coercion above can RESIZE or detach the backing buffer, so the bound is re-checked against the live
       count — the length captured before it only decides what a negative index counts back from. */
    p = get_typed_array(ctx, s->hdr.this_val);
    if (!p) return -1;
    idx = s->idx;
    if (idx < 0)
        idx = s->len + idx;
    if (idx < 0 || idx >= p->u.array.count) {
        s->result = JS_UNDEFINED;
        return 0;
    }
    switch (p->class_id) {
    case JS_CLASS_INT8_ARRAY:     s->result = js_int32(p->u.array.u.int8_ptr[idx]); break;
    case JS_CLASS_UINT8C_ARRAY:
    case JS_CLASS_UINT8_ARRAY:    s->result = js_int32(p->u.array.u.uint8_ptr[idx]); break;
    case JS_CLASS_INT16_ARRAY:    s->result = js_int32(p->u.array.u.int16_ptr[idx]); break;
    case JS_CLASS_UINT16_ARRAY:   s->result = js_int32(p->u.array.u.uint16_ptr[idx]); break;
    case JS_CLASS_INT32_ARRAY:    s->result = js_int32(p->u.array.u.int32_ptr[idx]); break;
    case JS_CLASS_UINT32_ARRAY:   s->result = js_uint32(p->u.array.u.uint32_ptr[idx]); break;
    case JS_CLASS_FLOAT16_ARRAY:  s->result = js_float64(fromfp16(p->u.array.u.fp16_ptr[idx])); break;
    case JS_CLASS_FLOAT32_ARRAY:  s->result = js_float64(p->u.array.u.float_ptr[idx]); break;
    case JS_CLASS_FLOAT64_ARRAY:  s->result = js_float64(p->u.array.u.double_ptr[idx]); break;
    case JS_CLASS_BIG_INT64_ARRAY:  s->result = JS_NewBigInt64(ctx, p->u.array.u.int64_ptr[idx]); break;
    case JS_CLASS_BIG_UINT64_ARRAY: s->result = JS_NewBigUint64(ctx, p->u.array.u.uint64_ptr[idx]); break;
    default:
        DFAIL("TypedArray element read reached a class that is not a typed array");
        return -1;
    }
    return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). Each element of an array-like source is coerced, and a
   coercion is the page's code. */
static void js_ta_idx_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSTAIdx *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->src);
    v->val(ctx, &s->el);
    v->val(ctx, &s->pred);
    v->val(ctx, &s->offset_v);
}

static JSValue js_ta_idx_fini(JSContext *ctx, void *st, bool take_result)
{
    JSTAIdx *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}


/* TypedArray.prototype.sort */

static int js_cmp_doubles(double x, double y)
{
    if (isnan(x))    return isnan(y) ? 0 : +1;
    if (isnan(y))    return -1;
    if (x < y)       return -1;
    if (x > y)       return 1;
    if (x != 0)      return 0;
    if (signbit(x))  return signbit(y) ? 0 : -1;
    else             return signbit(y) ? 1 : 0;
}

static int js_TA_cmp_int8(const void *a, const void *b, void *opaque) {
    return *(const int8_t *)a - *(const int8_t *)b;
}

static int js_TA_cmp_uint8(const void *a, const void *b, void *opaque) {
    return *(const uint8_t *)a - *(const uint8_t *)b;
}

static int js_TA_cmp_int16(const void *a, const void *b, void *opaque) {
    return *(const int16_t *)a - *(const int16_t *)b;
}

static int js_TA_cmp_uint16(const void *a, const void *b, void *opaque) {
    return *(const uint16_t *)a - *(const uint16_t *)b;
}

static int js_TA_cmp_int32(const void *a, const void *b, void *opaque) {
    int32_t x = *(const int32_t *)a;
    int32_t y = *(const int32_t *)b;
    return (y < x) - (y > x);
}

static int js_TA_cmp_uint32(const void *a, const void *b, void *opaque) {
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (y < x) - (y > x);
}

static int js_TA_cmp_int64(const void *a, const void *b, void *opaque) {
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    return (y < x) - (y > x);
}

static int js_TA_cmp_uint64(const void *a, const void *b, void *opaque) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (y < x) - (y > x);
}

static int js_TA_cmp_float16(const void *a, const void *b, void *opaque) {
    return js_cmp_doubles(fromfp16(*(const uint16_t *)a),
                          fromfp16(*(const uint16_t *)b));
}

static int js_TA_cmp_float32(const void *a, const void *b, void *opaque) {
    return js_cmp_doubles(*(const float *)a, *(const float *)b);
}

static int js_TA_cmp_float64(const void *a, const void *b, void *opaque) {
    return js_cmp_doubles(*(const double *)a, *(const double *)b);
}

static JSValue js_TA_get_int8(JSContext *ctx, const void *a) {
    return js_int32(*(const int8_t *)a);
}

static JSValue js_TA_get_uint8(JSContext *ctx, const void *a) {
    return js_int32(*(const uint8_t *)a);
}

static JSValue js_TA_get_int16(JSContext *ctx, const void *a) {
    return js_int32(*(const int16_t *)a);
}

static JSValue js_TA_get_uint16(JSContext *ctx, const void *a) {
    return js_int32(*(const uint16_t *)a);
}

static JSValue js_TA_get_int32(JSContext *ctx, const void *a) {
    return js_int32(*(const int32_t *)a);
}

static JSValue js_TA_get_uint32(JSContext *ctx, const void *a) {
    return js_uint32(*(const uint32_t *)a);
}

static JSValue js_TA_get_int64(JSContext *ctx, const void *a) {
    return JS_NewBigInt64(ctx, *(int64_t *)a);
}

static JSValue js_TA_get_uint64(JSContext *ctx, const void *a) {
    return JS_NewBigUint64(ctx, *(uint64_t *)a);
}

static JSValue js_TA_get_float16(JSContext *ctx, const void *a) {
    return js_float64(fromfp16(*(const uint16_t *)a));
}

static JSValue js_TA_get_float32(JSContext *ctx, const void *a) {
    return js_float64(*(const float *)a);
}

static JSValue js_TA_get_float64(JSContext *ctx, const void *a) {
    return js_float64(*(const double *)a);
}

/* DELETED: struct TA_sort_context and js_TA_cmp_generic. The context existed only to smuggle the JSContext, the
   array and the comparator through rqsort's void*, and the comparator ran JS_Call inside rqsort's own recursion —
   the sort's state was the C stack, so nothing about it could suspend. Its detach/bounds guards
   (`typed_array_is_oob`, `a_idx >= count` -> 0) and its index-permutation write-back existed because it re-read
   the LIVE buffer at every comparison; SortIndexedProperties snapshots first, so there is nothing left for them
   to guard. The typed cmpfuns beside them stay: they are the default ordering, which invokes nothing. */

/* the element reader for a class, which is how SortIndexedProperties' List gets built. */
static JSValue (*js_ta_getfun(int class_id))(JSContext *ctx, const void *a)
{
    switch (class_id) {
    case JS_CLASS_INT8_ARRAY:       return js_TA_get_int8;
    case JS_CLASS_UINT8C_ARRAY:
    case JS_CLASS_UINT8_ARRAY:      return js_TA_get_uint8;
    case JS_CLASS_INT16_ARRAY:      return js_TA_get_int16;
    case JS_CLASS_UINT16_ARRAY:     return js_TA_get_uint16;
    case JS_CLASS_INT32_ARRAY:      return js_TA_get_int32;
    case JS_CLASS_UINT32_ARRAY:     return js_TA_get_uint32;
    case JS_CLASS_BIG_INT64_ARRAY:  return js_TA_get_int64;
    case JS_CLASS_BIG_UINT64_ARRAY: return js_TA_get_uint64;
    case JS_CLASS_FLOAT16_ARRAY:    return js_TA_get_float16;
    case JS_CLASS_FLOAT32_ARRAY:    return js_TA_get_float32;
    case JS_CLASS_FLOAT64_ARRAY:    return js_TA_get_float64;
    }
    DFAIL("a typed array class with no element reader");
    return NULL;
}

/* 23.2.4.8 CompareTypedArrayElements steps 3-10 as the raw in-place sort: with no comparator the comparison
   reads two elements of the buffer and nothing else, so the whole sort is a computation with no suspension
   point in it. */
static int (*js_ta_rawcmp(int class_id))(const void *a, const void *b, void *opaque)
{
    switch (class_id) {
    case JS_CLASS_INT8_ARRAY:       return js_TA_cmp_int8;
    case JS_CLASS_UINT8C_ARRAY:
    case JS_CLASS_UINT8_ARRAY:      return js_TA_cmp_uint8;
    case JS_CLASS_INT16_ARRAY:      return js_TA_cmp_int16;
    case JS_CLASS_UINT16_ARRAY:     return js_TA_cmp_uint16;
    case JS_CLASS_INT32_ARRAY:      return js_TA_cmp_int32;
    case JS_CLASS_UINT32_ARRAY:     return js_TA_cmp_uint32;
    case JS_CLASS_BIG_INT64_ARRAY:  return js_TA_cmp_int64;
    case JS_CLASS_BIG_UINT64_ARRAY: return js_TA_cmp_uint64;
    case JS_CLASS_FLOAT16_ARRAY:    return js_TA_cmp_float16;
    case JS_CLASS_FLOAT32_ARRAY:    return js_TA_cmp_float32;
    case JS_CLASS_FLOAT64_ARRAY:    return js_TA_cmp_float64;
    }
    DFAIL("a typed array class with no element comparison");
    return NULL;
}

/* 23.2.3.29 step 9 / 23.2.3.34 step 8: Set(obj, ! ToString(𝔽(j)), sortedList[j], true). On a typed array that is
   10.4.5.5 -> TypedArraySetElement, whose conversion runs nothing here because the value came out of a typed
   array of the same class; and an index that is no longer valid is a NO-OP, which is exactly what a comparator
   that shrank a resizable buffer leaves behind. */
static int js_ta_sort_put(JSContext *ctx, JSValueConst obj, int64_t j, JSValueConst v)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    uint64_t v64;
    if (typed_array_is_oob(p) || j >= (int64_t)p->u.array.count)
        return 0;
    if (js_typed_array_fill_value(ctx, p, v, &v64))
        return -1;
    cow_capture_bytes(ctx, p);   /* the writeback below stores raw bytes — see cow_capture_bytes */
    switch (typed_array_size_log2(p->class_id)) {
    case 0: p->u.array.u.uint8_ptr[j]  = (uint8_t)v64;  break;
    case 1: p->u.array.u.uint16_ptr[j] = (uint16_t)v64; break;
    case 2: p->u.array.u.uint32_ptr[j] = (uint32_t)v64; break;
    case 3: p->u.array.u.uint64_ptr[j] = v64;           break;
    default: DFAIL("a typed array element size that is not 1, 2, 4 or 8");
    }
    return 0;
}

/* %TypedArray%'s sort and toSorted are ONE machine over two algorithms, so ONE stage list expanded twice with
   each one's own step text — see ACB_STAGES. Its prologue spans a RANGE and says so: the comparator check, the
   validate, the create and SortIndexedProperties' whole read of the List are steps in which nothing the page
   wrote can run, so there is nothing in them to rest at. The MERGE is the one place there is. */
#define TAS_STAGES(X, PROLOGUE, MERGE, WRITEBACK) \
    X(TAS_PROLOGUE,  PROLOGUE) \
    X(TAS_MERGE,     MERGE) \
    X(TAS_WRITEBACK, WRITEBACK)
enum { TAS_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0) };
static const char *const js_ta_sort_steps[] = {
    TAS_STAGES(JS_STEP_STAGE_LABEL,
        "23.2.3.29 steps 1-4 and 23.1.3.30.1 steps 1-3 (the comparator check; ValidateTypedArray; the whole "
        "List is read before any comparison)",
        "23.2.4.8 CompareTypedArrayElements step 2.a (result is ToNumber(Call(comparator, undefined, <<x, y>>)))",
        "23.2.3.29 step 9.a (Set(obj, ToString(j), sortedList[j], true), the whole write-back)")
    NULL };
static const char *const js_ta_toSorted_steps[] = {
    TAS_STAGES(JS_STEP_STAGE_LABEL,
        "23.2.3.33 steps 1-5 and 23.1.3.30.1 steps 1-3 (the comparator check; ValidateTypedArray; A is "
        "TypedArrayCreateSameType(O, <<len>>); the whole List is read)",
        "23.2.4.8 CompareTypedArrayElements step 2.a (result is ToNumber(Call(comparator, undefined, <<x, y>>)))",
        "23.2.3.33 step 9.a (Set(A, ToString(j), sortedList[j], true), the whole write-back)")
    NULL };

static int js_ta_sort_vstep(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSTASort *s = st;
    JSValueConst method = step_arg(&s->hdr, 0);
    int rc;

    if (s->hdr.stage == TAS_PROLOGUE) {
        JSObject *p;
        JSValue (*getfun)(JSContext *, const void *);
        int elt_size;
        int64_t j;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* js_mallocz leaves a JSValue reading as JS_TAG_INT 0, not JS_UNDEFINED, so every owned field is stated
           here — the coercion's own DCHECK is what catches the omission, and did. */
        s->obj = JS_UNDEFINED; s->cmpres = JS_UNDEFINED; s->order = JS_UNINITIALIZED;
        s->items = NULL; s->tmp = NULL; s->pending = 0;
        s->len = 0; s->width = 0; s->i = 0; s->lo = 0; s->mid = 0; s->hi = 0;
        s->l = 0; s->r = 0; s->k = 0; s->wb = 0;
        { int q; for (q = 0; q < 4; q++) s->cb_args[q] = JS_UNDEFINED; }
        /* step 1: a non-callable comparator throws BEFORE the receiver is even validated */
        if (!JS_IsUndefined(method) && check_function(ctx, method))
            return -1;
        p = get_typed_array(ctx, s->hdr.this_val);            /* step 3: ValidateTypedArray */
        if (!p)
            return -1;
        if (typed_array_is_oob(p))
            return JS_ThrowTypeErrorArrayBufferOOB(ctx), -1;
        s->len = p->u.array.count;                            /* step 4: TypedArrayLength */
        if (s->hdr.arg) {
            /* 23.2.3.34 step 5: TypedArrayCreateSameType(O, «len») — an ordinary allocation, no species */
            s->obj = js_typed_array_constructor_ta(ctx, JS_UNDEFINED, s->hdr.this_val,
                                                   p->class_id, (uint32_t)s->len);
            if (JS_IsException(s->obj)) { s->obj = JS_UNDEFINED; return -1; }
        } else {
            if (typed_array_is_immutable(p))
                return JS_ThrowTypeErrorImmutableArrayBuffer(ctx), -1;
            s->obj = js_dup(s->hdr.this_val);
        }
        if (s->len <= 1)
            return 0;
        if (JS_IsUndefined(method)) {
            /* the default ordering: nothing in it can suspend, so it is done in place right here. toSorted sorts
               its COPY, which already holds the receiver's elements. */
            JSObject *d = JS_VALUE_GET_OBJ(s->obj);
            elt_size = 1 << typed_array_size_log2(d->class_id);
            rqsort(d->u.array.u.ptr, (size_t)s->len, elt_size, js_ta_rawcmp(d->class_id), NULL);
            return 0;
        }
        /* steps 5-7: SortIndexedProperties reads the whole List BEFORE any comparison, which is what makes the
           comparator unable to change what a later comparison sees. Each read is an element of a typed array,
           so not one of them is the page's code. */
        getfun = js_ta_getfun(p->class_id);
        elt_size = 1 << typed_array_size_log2(p->class_id);
        s->items = js_malloc(ctx, (size_t)s->len * sizeof(JSValue));
        if (!s->items)
            return -1;
        for (j = 0; j < s->len; j++)
            s->items[j] = getfun(ctx, (char *)p->u.array.u.ptr + j * (size_t)elt_size);
        s->tmp = js_malloc(ctx, (size_t)s->len * sizeof(JSValue));
        if (!s->tmp)
            return -1;
        s->width = 1; s->i = 0;
        s->lo = 0; s->mid = 1 < s->len ? 1 : s->len; s->hi = 2 < s->len ? 2 : s->len;
        s->l = 0; s->r = s->mid; s->k = 0;
        s->hdr.stage = TAS_MERGE;
    }

    if (s->hdr.stage == TAS_MERGE) {
        if (s->pending) {
            double v = 0;
            int y_first;

            /* §23.2.4.8 CompareTypedArrayElements step 2.a coerces what the comparator returned, so an object
               result runs its valueOf; held on the state because that coercion suspends. An unknown result is
               the merge's order question (sort_order_unknown), asked as the Array merge asks it; a sibling
               re-enters here with undefined placed. Step 2.b's NaN is +0, which `v > 0` answers with x first. */
            if (JS_IsUninitialized(s->order)) {
                if (s->hdr.num_phase == NUM_PH_START) {
                    DCHECK(JS_IsUndefined(s->cmpres), "a comparator result is already being coerced on this sort");
                    s->cmpres = cb_result;
                    cb_result = JS_UNDEFINED;
                }
                rc = step_tofloat64_run(ctx, &s->hdr, s->cmpres, cb_result, &v, out_cb, out_argc);
                cb_result = JS_UNDEFINED;
                if (rc == JS_STEP_UNKNOWN) {
                    s->order = sort_order_unknown(ctx, sort_take_unknown(&s->hdr), js_int32(0), OP_gt);
                    if (JS_IsException(s->order)) { s->order = JS_UNINITIALIZED; return -1; }
                } else if (rc) {
                    return rc < 0 ? -1 : rc;
                }
                JS_FreeValue(ctx, s->cmpres); s->cmpres = JS_UNDEFINED;
            } else {
                DCHECK(JS_IsUndefined(cb_result), "a sort re-entered its order question with a value placed");
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
            }
            if (!JS_IsUninitialized(s->order)) {
                rc = step_tobool_run(ctx, &s->hdr, s->order, SORT_ORDER_OP, &y_first);
                if (rc) return rc < 0 ? -1 : rc;
                JS_FreeValue(ctx, s->order);
                s->order = JS_UNINITIALIZED;
            } else {
                y_first = v > 0;
            }
            s->pending = 0;
            if (!y_first) s->tmp[s->k++] = s->items[s->l++];
            else          s->tmp[s->k++] = s->items[s->r++];
        } else {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
        }
        for (;;) {
            if (s->width >= s->len) break;
            if (s->i >= s->len) {
                s->width *= 2; s->i = 0;
                if (s->width >= s->len) break;
                s->lo = 0; s->mid = s->width < s->len ? s->width : s->len;
                s->hi = 2 * s->width < s->len ? 2 * s->width : s->len;
                s->l = 0; s->r = s->mid; s->k = 0;
                continue;
            }
            /* the merge invariants: an active block with ordered cursors, and the exactly-conserved output count.
               A violation is a state-machine bug, so it crashes where it is born. */
            DCHECK(s->lo <= s->l && s->l <= s->mid && s->mid <= s->r && s->r <= s->hi && s->hi <= s->len,
                   "the typed-array merge left its block");
            DCHECK(s->k - s->lo == (s->l - s->lo) + (s->r - s->mid),
                   "the typed-array merge placed a different number of elements than it consumed");
            if (s->l < s->mid && s->r < s->hi) {
                s->cb_args[0] = JS_UNDEFINED;      /* the comparator gets this = undefined */
                s->cb_args[1] = method;
                s->cb_args[2] = s->items[s->l];    /* borrowed: items owns them for the machine's whole life */
                s->cb_args[3] = s->items[s->r];
                s->pending = 1;
                *out_cb = s->cb_args; *out_argc = 2;
                return 3;
            }
            while (s->l < s->mid) s->tmp[s->k++] = s->items[s->l++];
            while (s->r < s->hi)  s->tmp[s->k++] = s->items[s->r++];
            { int64_t t; for (t = s->lo; t < s->hi; t++) s->items[t] = s->tmp[t]; }
            s->i += 2 * s->width;
            if (s->i < s->len) {
                s->lo = s->i; s->mid = s->i + s->width < s->len ? s->i + s->width : s->len;
                s->hi = s->i + 2 * s->width < s->len ? s->i + 2 * s->width : s->len;
                s->l = s->lo; s->r = s->mid; s->k = s->lo;
            }
        }
        s->wb = 0;
        s->hdr.stage = TAS_WRITEBACK;
    }

    DCHECK(s->hdr.stage == TAS_WRITEBACK, "the typed-array sort resumed in no stage");
    JS_FreeValue(ctx, cb_result);
    /* the write-back invokes nothing, so it is one pass rather than a resumable loop — the values are already
       of the array's own kind and an out-of-range index is the spec's no-op. */
    for (; s->wb < s->len; s->wb++) {
        if (js_ta_sort_put(ctx, s->obj, s->wb, s->items[s->wb]))
            return -1;
    }
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). `items` is the ONE owner of the sorted list — tmp's entries
   ALIAS it within the block being merged, and every element stays reachable from items until the copy-back
   permutes them — so items is an element array and tmp is a plain buffer. Getting those two the wrong way round
   is a double free in one direction and a lost merge block in the other, which is exactly why the operations
   are separate and the machine says which is which. */
static void js_ta_sort_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSTASort *s = st;
    v->val(ctx, &s->cmpres);
    v->val(ctx, &s->order);
    v->val(ctx, &s->obj);
    v->array(ctx, (void **)&s->items, sizeof(JSValue), (int)s->len, (int)s->len, js_step_visit_value_elem);
    v->buf(ctx, (void **)&s->tmp, (size_t)s->len * sizeof(JSValue));
}

static JSValue js_ta_sort_vfini(JSContext *ctx, void *st, bool take_result)
{
    JSTASort *s = st;
    JSValue r = take_result ? s->obj : JS_UNDEFINED;
    if (take_result) s->obj = JS_UNDEFINED;
    return r;
}

static const JSCFunctionListEntry js_typed_array_base_funcs[] = {
    JS_CFUNC_CONSUME_DEF("from", 1, ITERCONS_TA_FROM ),
    JS_CFUNC_CONSUME_DEF("of", 0, ITERCONS_TA_OF ),
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
};

static const JSCFunctionListEntry js_typed_array_base_proto_funcs[] = {
    JS_CGETSET_DEF("length", js_typed_array_get_length, NULL ),
    JS_CFUNC_STEP_DEF("at", 1, STEPDEF_TA_AT ),
    JS_CFUNC_STEP_DEF("with", 2, STEPDEF_TA_WITH ),
    JS_CGETSET_DEF("buffer", js_typed_array_get_buffer, NULL ),
    JS_CGETSET_DEF("byteLength", js_typed_array_get_byteLength, NULL ),
    JS_CGETSET_DEF("byteOffset", js_typed_array_get_byteOffset, NULL ),
    JS_CFUNC_STEP_DEF("set", 1, STEPDEF_TA_SET ),
    JS_CFUNC_MAGIC_DEF("values", 0, js_create_typed_array_iterator, JS_ITERATOR_KIND_VALUE ),
    JS_ALIAS_DEF("[Symbol.iterator]", "values" ),
    JS_CFUNC_MAGIC_DEF("keys", 0, js_create_typed_array_iterator, JS_ITERATOR_KIND_KEY ),
    JS_CFUNC_MAGIC_DEF("entries", 0, js_create_typed_array_iterator, JS_ITERATOR_KIND_KEY_AND_VALUE ),
    JS_CGETSET_DEF("[Symbol.toStringTag]", js_typed_array_get_toStringTag, NULL ),
    JS_CFUNC_STEP_DEF("copyWithin", 2, STEPDEF_TA_COPYWITHIN ),
    JS_CFUNC_STEP_DEF("every", 1, STEPDEF_TA_EVERY ),
    JS_CFUNC_STEP_DEF("some", 1, STEPDEF_TA_SOME ),
    JS_CFUNC_STEP_DEF("forEach", 1, STEPDEF_TA_FOREACH ),
    JS_CFUNC_STEP_DEF("map", 1, STEPDEF_TA_MAP ),
    JS_CFUNC_STEP_DEF("filter", 1, STEPDEF_TA_FILTER ),
    JS_CFUNC_STEP_DEF("reduce", 1, STEPDEF_TA_REDUCE ),
    JS_CFUNC_STEP_DEF("reduceRight", 1, STEPDEF_TA_REDUCE_RIGHT ),
    JS_CFUNC_STEP_DEF("fill", 1, STEPDEF_TA_FILL ),
    JS_CFUNC_STEP_DEF("find", 1, STEPDEF_TA_FIND ),
    JS_CFUNC_STEP_DEF("findIndex", 1, STEPDEF_TA_FIND_INDEX ),
    JS_CFUNC_STEP_DEF("findLast", 1, STEPDEF_TA_FIND_LAST ),
    JS_CFUNC_STEP_DEF("findLastIndex", 1, STEPDEF_TA_FIND_LAST_INDEX ),
    JS_CFUNC_DEF("reverse", 0, js_typed_array_reverse ),
    JS_CFUNC_DEF("toReversed", 0, js_typed_array_toReversed ),
    JS_CFUNC_STEP_DEF("slice", 2, STEPDEF_TA_SLICE ),
    JS_CFUNC_STEP_DEF("subarray", 2, STEPDEF_TA_SUBARRAY ),
    JS_CFUNC_STEP_DEF("sort", 1, STEPDEF_TA_SORT ),
    JS_CFUNC_STEP_DEF("toSorted", 1, STEPDEF_TA_TOSORTED ),
    JS_CFUNC_STEP_DEF("join", 1, STEPDEF_TA_JOIN ),
    JS_CFUNC_STEP_DEF("toLocaleString", 0, STEPDEF_TA_TOLOCALESTRING ),
    JS_CFUNC_STEP_DEF("indexOf", 1, STEPDEF_TA_INDEXOF ),
    JS_CFUNC_STEP_DEF("lastIndexOf", 1, STEPDEF_TA_LASTINDEXOF ),
    JS_CFUNC_STEP_DEF("includes", 1, STEPDEF_TA_INCLUDES ),
    //JS_ALIAS_BASE_DEF("toString", "toString", 2 /* Array.prototype. */), @@@
};

static const JSCFunctionListEntry js_typed_array_funcs[] = {
    JS_PROP_INT32_DEF("BYTES_PER_ELEMENT", 1, 0),
    JS_PROP_INT32_DEF("BYTES_PER_ELEMENT", 2, 0),
    JS_PROP_INT32_DEF("BYTES_PER_ELEMENT", 4, 0),
    JS_PROP_INT32_DEF("BYTES_PER_ELEMENT", 8, 0),
};

static JSValue js_typed_array_base_constructor(JSContext *ctx,
                                               JSValueConst this_val,
                                               int argc, JSValueConst *argv)
{
    return JS_ThrowTypeError(ctx, "cannot be called");
}

/* 'obj' must be an allocated typed array object */
static int typed_array_init(JSContext *ctx, JSValue obj, JSValue buffer,
                            uint64_t offset, uint64_t len, bool track_rab)
{
    JSTypedArray *ta;
    JSObject *p, *pbuffer;
    JSArrayBuffer *abuf;
    int size_log2;

    p = JS_VALUE_GET_OBJ(obj);
    size_log2 = typed_array_size_log2(p->class_id);
    ta = js_malloc(ctx, sizeof(*ta));
    if (!ta) {
        JS_FreeValue(ctx, buffer);
        return -1;
    }
    pbuffer = JS_VALUE_GET_OBJ(buffer);
    abuf = pbuffer->u.array_buffer;
    ta->obj = p;
    ta->buffer = pbuffer;
    ta->offset = offset;
    ta->length = len << size_log2;
    ta->track_rab = track_rab;
    ta->length_unknown = JS_UNINITIALIZED;   /* a caller whose len is an unknown's example sets it */
    ta->offset_unknown = JS_UNINITIALIZED;   /* a caller whose offset is an unknown's example sets it */
    list_add_tail(&ta->link, &abuf->array_list);
    p->u.typed_array = ta;
    p->u.array.count = len;
    p->u.array.u.ptr = abuf->data + offset;
    return 0;
}



/* AllocateTypedArrayBuffer(O, len) — 23.2.5.1's second half. The OBJECT is allocated earlier (step 6.a.i, before
   @@iterator is even read), and only its BUFFER waits for the length, so the two are separate operations here too.
   The ELEMENT writes that follow are user code (each is a ToNumber), so they belong to whichever machine is
   filling the array; this is the part that invokes nothing. */
static int js_typed_array_alloc_len(JSContext *ctx, JSValueConst obj, int classid, int64_t len)
{
    JSValue buffer;

    /* typed_array_init writes the view's own JSObject record (u.typed_array, u.array) and links it into the
       buffer's array_list, and no capture takes that record per flow. That is sound because the view is held
       by its consume machine alone until this attach: js_iter_consume_clone gives a forked sibling its own. */
    DCHECK(JS_REF_COUNT(JS_VALUE_GET_OBJ(obj)) == 1,
           "AllocateTypedArrayBuffer on a view something besides its consume machine holds — a fork that "
           "shares the unattached view has both flows attach a buffer to one uncaptured record");
    buffer = js_array_buffer_constructor1(ctx, len << typed_array_size_log2(classid), NULL);
    if (JS_IsException(buffer))
        return -1;
    return typed_array_init(ctx, obj, buffer, 0, len, /*track_rab*/false);
}

/* 23.2.5.1 steps 5-6, `new TypedArray(object)`, has NO body left here. Both of its algorithms — the iterable
   one (step 5: GetIterator, collect, then element-set) and the array-like one (step 6: LengthOfArrayLike, then
   element-set) — run on the consume machine, chosen by the acquire from the real GetMethod rather than by a
   probe, and every construct spelling reaches it: `new TA(x)`, a subclass's super(), Reflect.construct with a
   new.target, a bound ctor, a proxied one, and a spread.
   What is gone with it is js_array_from_iterator: a `for(;;) JS_IteratorNext` collect in a C activation with no
   flow base, so an argument whose .next() contains a loop aborted at its back-edge. It was the last of those
   loops. Reaching this entry means a construct site was not routed; there is no second driver. */
static JSValue js_typed_array_constructor_obj(JSContext *ctx,
                                              JSValueConst new_target,
                                              JSValueConst obj,
                                              int classid)
{
    DFAIL("new TypedArray(object) reached its C entry — route that construct site onto the consume machine "
          "(do_consumer_dispatch / do_construct_dispatch); there is no second driver");
    return JS_EXCEPTION;
}

/* 23.2.5.1 step 5's tail: the object EXISTS (10.1.14 having been performed by whoever created it), so this is
   InitializeTypedArrayFromTypedArray from the source re-validation on. Split out for the reason the view branch
   split: the `prototype` read is the page's code and belongs to the machine, and everything here invokes none.
   Consumes `obj`. */
static JSValue js_ta_copy_finish(JSContext *ctx, JSValue obj, JSValueConst src_obj,
                                 int classid, uint32_t len)
{
    JSObject *p, *src_buffer;
    JSTypedArray *ta;
    JSValue buffer;
    uint32_t i;
    int size_log2;
    JSArrayBuffer *src_abuf, *abuf;

    p = JS_VALUE_GET_OBJ(src_obj);
    if (typed_array_is_oob(p)) {
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        goto fail;
    }
    if (len > p->u.array.count) {
        JS_ThrowRangeError(ctx, "length out of bounds");
        goto fail;
    }
    ta = p->u.typed_array;
    src_buffer = ta->buffer;
    src_abuf = src_buffer->u.array_buffer;
    size_log2 = typed_array_size_log2(classid);
    buffer = js_array_buffer_constructor1(ctx,
                                          (uint64_t)len << size_log2,
                                          NULL);
    if (JS_IsException(buffer))
        goto fail;
    /* necessary because it could have been detached */
    if (typed_array_is_oob(p)) {
        JS_FreeValue(ctx, buffer);
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        goto fail;
    }
    abuf = JS_GetOpaque(buffer, JS_CLASS_ARRAY_BUFFER);
    if (typed_array_init(ctx, obj, buffer, 0, len, /*track_rab*/false))
        goto fail;
    if (p->class_id == classid) {
        /* same type: copy the content */
        DCHECK_TA_WINDOW_STATED(ta, abuf->byte_length, "InitializeTypedArrayFromTypedArray's byte copy");
        js_ab_spans_copy(ctx, abuf, 0, src_abuf, ta->offset, (uint32_t)abuf->byte_length);
        memcpy(abuf->data, src_abuf->data + ta->offset, abuf->byte_length);
    } else {
        for(i = 0; i < len; i++) {
            JSValue val;
            val = JS_GetPropertyUint32(ctx, src_obj, i);
            if (JS_IsException(val))
                goto fail;
            if (JS_SetPropertyUint32(ctx, obj, i, val) < 0)
                goto fail;
        }
    }
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* The unrouted entry: an internal caller with new_target = undefined, whose 10.1.14 reads nothing. */
static JSValue js_typed_array_constructor_ta(JSContext *ctx, JSValueConst new_target,
                                             JSValueConst src_obj, int classid, uint32_t len)
{
    JSValue obj = js_create_from_ctor(ctx, new_target, classid);
    if (JS_IsException(obj))
        return obj;
    return js_ta_copy_finish(ctx, obj, src_obj, classid, len);
}

static int ta_view_offset(JSContext *ctx, JSValueConst byte_offset, int size_log2, uint64_t *poffset);
static int js_ta_view_plan(JSContext *ctx, JSTAViewPlan *pl, int argc, JSValueConst *argv, int classid,
                           uint64_t offset);
static JSValue js_ta_view_finish(JSContext *ctx, JSValue obj, JSTAViewPlan *pl);

/* The buffer branch's two coercions, as requests. Reached ONLY with an ArrayBuffer first argument
   (ta_buffer_ctor_ready), so the object branches the consume walk owns never arrive here and the body's own
   js_typed_array_constructor_obj DFAIL stays unreachable. The body is not a legacy twin: with both operands
   primitive its JS_ToIndex calls invoke nothing, which is exactly what this entry asserts, and it keeps its C
   entry because six internal callers construct typed arrays with operands the engine itself made. */
/* 23.2.5.1 branches on WHAT the first argument is, and the two arms order their user code oppositely: an
   OBJECT first argument reaches step 6.b.i, which ALLOCATES -- reading NewTarget.prototype -- before
   InitializeTypedArrayFromArrayBuffer coerces a byteOffset or looks at the buffer at all; a PRIMITIVE one
   reaches step 6.c.ii, whose ToIndex runs first and whose allocation is last. One order was implemented for
   both, so `Reflect.construct(Int32Array, [buffer, poisoned, 0], ctorWithThrowingPrototype)` reported the
   byteOffset's throw where the spec reports the prototype getter's. */
/* The view constructor's stages. AllocateTypedArray's prototype read happens EARLY for an object first argument
   and LATE for the element-count form — the spec orders it either side of the coercions — which is why the two
   reads are two stages rather than one stage asked twice. */
#define TACTOR_STAGES(X) \
    X(TAC_START,       "23.2.5.1 steps 1-6.a (NewTarget is not undefined; firstArgument)") \
    X(TAC_PROTO_EARLY, "23.2.5.1 step 6.b.i via 23.2.5.1.1 step 1 (proto is GetPrototypeFromConstructor(newTarget))") \
    X(TAC_OFFSET,      "23.2.5.1.3 step 2 (offset is ToIndex(byteOffset))") \
    X(TAC_OFFSET_FORK, "23.2.5.1.3 steps 2-3 over an unknown offset (ToIndex's interval and the alignment fork)") \
    X(TAC_LENGTH,      "23.2.5.1.3 step 5.a (newLength is ToIndex(length))") \
    X(TAC_COUNT_FORK,  "23.2.5.1 step 9 over an unknown firstArg (elementLength is ToIndex(firstArg): its interval)") \
    X(TAC_RANGE,       "23.2.5.1.3 steps 8-9 over an unknown buffer length or offset (the range test forks)") \
    X(TAC_PROTO,       "23.2.5.1 steps 5 and 6.c.iii via 23.2.5.1.1 (AllocateTypedArray, then the initialization)")
enum { TACTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_ta_ctor_steps[] = { TACTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* 23.2.5.1.3 step 2 over unknown input: offset is an unknown derived from byteOffset under 7.1.24 ToIndex, whose
   example, where `with_example` holds, is §7.1.5 ToIntegerOrInfinity run on byteOffset's example when that runs
   no page code. `offset` holds the example where it is an aligned index (off_example), else 0. */
static void js_ta_ctor_offset_unknown(JSContext *ctx, JSTACtor *s, bool with_example)
{
    int size_log2 = typed_array_size_log2(s->hdr.arg);
    JSValue ex = JS_UNDEFINED, v;
    double d = 0;

    s->offset = 0;
    s->off_example = 0;
    if (with_example && js_concolic_held(s->argp[1])) {
        int tag;

        v = js_concolic_operand(ctx, s->argp[1]);
        tag = JS_VALUE_GET_NORM_TAG(v);
        if ((JS_IsNumber(v) || tag == JS_TAG_STRING || tag == JS_TAG_BOOL || tag == JS_TAG_NULL ||
             tag == JS_TAG_UNDEFINED) && JS_ToFloat64(ctx, &d, v) == 0) {
            d = isnan(d) ? 0 : trunc(d) + 0.0;
            ex = js_float64(d);
            if (d >= 0 && d <= MAX_SAFE_INTEGER && ((uint64_t)d & (((uint64_t)1 << size_log2) - 1)) == 0) {
                s->offset = (uint64_t)d;
                s->off_example = 1;
            }
        }
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, s->off_unknown);
    s->off_unknown = g_concolic.derived(ctx, "7.1.24 ToIndex ( arg ) step 1 (int is ToIntegerOrInfinity(arg))",
                                        (JSValueConst *)&s->argp[1], 1, ex);
    DCHECK(!JS_IsUninitialized(s->off_unknown), "an unknown byteOffset derived no unknown offset");
}

static void js_ta_ctor_take_offset(JSContext *ctx, JSTACtor *s, JSObject *p);

/* The predicate TAC_OFFSET_FORK asks at s->off_fork: 7.1.24 step 2's two bounds on the unknown offset, then
   23.2.5.1.3 step 3's alignment, spelled as a remainder at most 0 since the offset is at least 0 by then. Owned. */
static JSValue js_ta_ctor_offset_test(JSContext *ctx, JSTACtor *s, int size_log2)
{
    JSValue sp[2], ops[2], v, ex;
    double d = 0;

    switch (s->off_fork) {
    case 0:
        sp[0] = js_int32(0);
        sp[1] = js_dup(s->off_unknown);
        break;
    case 1:
        sp[0] = js_dup(s->off_unknown);
        sp[1] = js_float64(MAX_SAFE_INTEGER);
        break;
    default:
        DCHECK(s->off_fork == 2 && size_log2 > 0, "TAC_OFFSET_FORK asked a test past its alignment test");
        ops[0] = s->off_unknown;
        ops[1] = js_int32(1 << size_log2);
        v = js_concolic_held(s->off_unknown) ? js_concolic_operand(ctx, s->off_unknown) : JS_UNDEFINED;
        ex = JS_IsNumber(v) && JS_ToFloat64(ctx, &d, v) == 0 && isfinite(d)
             ? js_float64(fmod(d, (double)(1 << size_log2))) : JS_UNDEFINED;
        JS_FreeValue(ctx, v);
        sp[0] = g_concolic.derived(ctx, "23.2.5.1.3 step 3 (offset modulo elementSize)", ops, 2, ex);
        DCHECK(!JS_IsUninitialized(sp[0]), "an unknown offset derived no unknown remainder");
        sp[1] = js_int32(0);
        break;
    }
    CHECK(g_concolic.rel(ctx, sp + 2, OP_lte), "the relation hook declined an unknown byteOffset");
    return sp[0];
}

/* 23.2.5.1 step 9's 7.1.24 ToIndex step 1 over an unknown firstArg: the integer, a Number, carrying `example`
   (consumed). Owned. */
static JSValue js_ta_ctor_count_int(JSContext *ctx, JSTACtor *s, JSValue example)
{
    JSValue r = g_concolic.derived(ctx, "7.1.24 ToIndex ( arg ) step 1 (int is ToIntegerOrInfinity(arg))",
                                   (JSValueConst *)&s->argp[0], 1, example);

    DCHECK(!JS_IsUninitialized(r), "an unknown element count derived no unknown integer");
    g_concolic.type_fix(ctx, r, JS_LANG_TYPE_NUMBER);
    return r;
}

static int js_ta_ctor_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSTACtor *s = st;
    int r;

    /* THE ARMS FALL THROUGH ONE INTO THE NEXT, WHICH IS WHAT THIS ALGORITHM IS: each coercion that finds its
       operand already primitive runs the next one in the same entry, and each that does not returns its request
       and is re-entered at its own arm. That was a chain of `if (stage == X)` tests reading the stage the block
       above had just written — the same list stated a second time, once per test — and the dispatch expresses it
       with the list itself. */
    STEP_DISPATCH(TACTOR_STAGES, s->hdr.stage, s->hdr.def->algorithm, -1);

    STEP_ARM(TAC_START);
    {
        int i;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->result = JS_UNDEFINED;
        s->proto = JS_UNDEFINED;
        s->len_unknown = JS_UNINITIALIZED;
        s->range_pred = JS_UNINITIALIZED;
        s->off_unknown = JS_UNINITIALIZED;
        s->off_pred = JS_UNINITIALIZED;
        s->off_fork = 0;
        s->off_example = 0;
        s->count_pred = JS_UNINITIALIZED;
        s->count_fork = 0;
        s->plan.in_range = false;
        s->plan.offset_unknown = false;
        s->plan.len_unknown = false;
        s->offset = 0;
        for (i = 0; i < 3; i++)
            s->argp[i] = js_dup(step_arg(&s->hdr, i));
        DCHECK(ta_buffer_ctor_ready(ctx, (JSValueConst *)s->argp, s->hdr.argc > 0 ? s->hdr.argc : 1),
               "the TypedArray view entry was reached with a source the consume walk owns");
        if (JS_VALUE_GET_TAG(s->argp[0]) != JS_TAG_OBJECT || js_ta_first_arg_count_unknown(ctx, s->argp[0])) {
            /* step 6.c: ToIndex(firstArgument) THEN AllocateTypedArray. The argument is already primitive, so
               that ToIndex invokes nothing and the plan in the tail performs it, or TAC_COUNT_FORK where it is an
               unknown. */
            s->hdr.stage = TAC_OFFSET;
            goto have_length;
        }
        s->is_copy = is_typed_array(JS_VALUE_GET_OBJ(s->argp[0])->class_id);
        s->hdr.stage = TAC_PROTO_EARLY;
        cb_result = JS_UNDEFINED;
    }
    STEP_ARM(TAC_PROTO_EARLY);
    {
        /* step 6.b.i AllocateTypedArray: NewTarget.prototype is an ordinary [[Get]] a Proxy traps, and for an
           object first argument it is the FIRST page code the constructor runs. */
        r = step_proto_from_ctor_run(ctx, &s->hdr, s->hdr.this_val, s->hdr.arg, cb_result, &s->proto,
                                     out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->proto_read = 1;
        s->hdr.stage = TAC_OFFSET;
        if (s->is_copy)
            goto have_length;                /* InitializeTypedArrayFromTypedArray coerces nothing */
        if (JS_VALUE_GET_TAG(s->argp[1]) != JS_TAG_OBJECT)
            goto have_offset;                /* already primitive: its ToIndex invokes nothing */
        r = step_toprim_run(ctx, &s->hdr, s->argp[1], HINT_NUMBER, JS_UNDEFINED, &cb_result, out_cb, out_argc);
        if (r) return r < 0 ? -1 : r;
    }
    STEP_ARM(TAC_OFFSET);
    {
        JSValue prim = JS_UNDEFINED;
        r = step_toprim_run(ctx, &s->hdr, s->argp[1], HINT_NUMBER, cb_result, &prim, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        JS_FreeValue(ctx, s->argp[1]);
        s->argp[1] = prim;                   /* the primitive REPLACES the operand the body reads */
    have_offset:
        if (js_value_is_concolic(s->argp[1])) {
            js_ta_ctor_offset_unknown(ctx, s, true);
            s->hdr.stage = TAC_OFFSET_FORK;
            STEP_JUMP(TAC_OFFSET_FORK);
        }
        s->hdr.stage = TAC_LENGTH;
        /* Steps 2-3 HERE, not in the plan: the alignment RangeError is ordered before step 5's ToIndex(length),
           and that ToIndex may need a ToPrimitive this machine can only perform as a request. Which is also why
           the length is a stage of its own rather than a second bit in one mask. */
        if (ta_view_offset(ctx, s->argp[1], typed_array_size_log2(s->hdr.arg), &s->offset) < 0)
            return -1;
        goto length_prim;
    }
    STEP_ARM(TAC_OFFSET_FORK);
    {
        /* Step 2's interval test and step 3's alignment test are predicates over the unknown offset, so each
           forks and its failing arm throws step 2's or step 3's RangeError, before step 5 coerces anything. */
        int size_log2 = typed_array_size_log2(s->hdr.arg), arm;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        while (s->off_fork < (size_log2 ? 3 : 2)) {
            static const char *const asks[3] = {
                "7.1.24 ToIndex step 2 (int is at least 0)", "7.1.24 ToIndex step 2 (int is at most 2**53 - 1)",
                "23.2.5.1.3 step 3 (offset modulo elementSize is 0)" };

            if (JS_IsUninitialized(s->off_pred))
                s->off_pred = js_ta_ctor_offset_test(ctx, s, size_log2);
            r = step_tobool_run(ctx, &s->hdr, s->off_pred, asks[s->off_fork], &arm);
            if (r) return r < 0 ? -1 : r;
            JS_FreeValue(ctx, s->off_pred);
            s->off_pred = JS_UNINITIALIZED;
            if (!arm) {
                JS_ThrowRangeError(ctx, "invalid offset");
                return -1;
            }
            s->off_fork++;
        }
        if (!s->off_example && js_concolic_held(s->off_unknown))
            js_ta_ctor_offset_unknown(ctx, s, false);   /* this arm contradicts the example: the view has none */
        s->hdr.stage = TAC_LENGTH;
    length_prim:
        if (JS_IsUndefined(s->argp[2]) || JS_VALUE_GET_TAG(s->argp[2]) != JS_TAG_OBJECT)
            goto have_length;
        r = step_toprim_run(ctx, &s->hdr, s->argp[2], HINT_NUMBER, JS_UNDEFINED, &cb_result, out_cb, out_argc);
        if (r) return r < 0 ? -1 : r;
    }
    STEP_ARM(TAC_LENGTH);
    {
        JSValue prim = JS_UNDEFINED;
        r = step_toprim_run(ctx, &s->hdr, s->argp[2], HINT_NUMBER, cb_result, &prim, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        JS_FreeValue(ctx, s->argp[2]);
        s->argp[2] = prim;
    }
 have_length:
    s->hdr.stage = TAC_COUNT_FORK;
    STEP_ARM(TAC_COUNT_FORK);
    if (!s->is_copy && js_ta_first_arg_count_unknown(ctx, s->argp[0])) {
        /* 23.2.5.1 step 9 over unknown input: 7.1.24 ToIndex's interval forks and its failing arm throws the
           RangeError; the passing arm allocates at the integer's example (0 where it has none, or its example
           leaves the interval), and the integer becomes [[ArrayLength]] (the tail, from len_unknown). */
        static const char *const asks[2] = { "7.1.24 ToIndex step 2 (int is at least 0)",
                                             "7.1.24 ToIndex step 2 (int is at most 2**53 - 1)" };
        int arm;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (JS_IsUninitialized(s->len_unknown)) {
            JSValue ex = js_concolic_held(s->argp[0]) ? js_concolic_operand(ctx, s->argp[0]) : JS_UNDEFINED;
            double d = 0;
            bool have = JS_IsNumber(ex) && JS_ToFloat64(ctx, &d, ex) == 0;

            JS_FreeValue(ctx, ex);
            d = isnan(d) ? 0 : trunc(d) + 0.0;
            s->len_unknown = js_ta_ctor_count_int(ctx, s, have ? js_float64(d) : JS_UNDEFINED);
        }
        while (s->count_fork < 2) {
            if (JS_IsUninitialized(s->count_pred)) {
                JSValue sp[2];

                sp[0] = s->count_fork == 0 ? js_int32(0) : js_dup(s->len_unknown);
                sp[1] = s->count_fork == 0 ? js_dup(s->len_unknown) : js_float64(MAX_SAFE_INTEGER);
                CHECK(g_concolic.rel(ctx, sp + 2, OP_lte), "the relation hook declined an unknown element count");
                s->count_pred = sp[0];
            }
            r = step_tobool_run(ctx, &s->hdr, s->count_pred, asks[s->count_fork], &arm);
            if (r) return r < 0 ? -1 : r;
            JS_FreeValue(ctx, s->count_pred);
            s->count_pred = JS_UNINITIALIZED;
            if (!arm) {
                JS_ThrowRangeError(ctx, "invalid array length");
                return -1;
            }
            s->count_fork++;
        }
        {
            JSValue ex = js_concolic_held(s->len_unknown) ? js_concolic_operand(ctx, s->len_unknown) : JS_UNDEFINED;
            double d = -1;

            if (!JS_IsNumber(ex) || JS_ToFloat64(ctx, &d, ex) || !(d >= 0 && d <= MAX_SAFE_INTEGER)) {
                d = 0;
                if (js_concolic_held(s->len_unknown)) {
                    /* this arm contradicts the example: the integer has none here */
                    JSValue none = js_ta_ctor_count_int(ctx, s, JS_UNDEFINED);

                    JS_FreeValue(ctx, s->len_unknown);
                    s->len_unknown = none;
                }
            }
            JS_FreeValue(ctx, ex);
            JS_FreeValue(ctx, s->argp[0]);
            s->argp[0] = js_float64(d);
        }
    }
    /* 23.2.5.1.3 step 5.a ToIndex(length) over unknown input: the view is built at the length's example and the
       unknown becomes its [[ArrayLength]], so its elements stay the buffer's indexed bytes. The plan's range
       check runs on the example. */
    if (!s->is_copy && JS_VALUE_GET_TAG(s->argp[0]) == JS_TAG_OBJECT && js_value_is_concolic(s->argp[2]) &&
        JS_IsUninitialized(s->len_unknown)) {
        JSValue ex = js_concolic_operand(ctx, s->argp[2]);
        double d = 0;

        if (!js_concolic_held(s->argp[2])) {
            /* no example: the view holds no example bytes, and every element read derives past them */
            JS_FreeValue(ctx, ex);
            ex = js_int32(0);
        } else if (!JS_IsNumber(ex) || JS_ToFloat64(ctx, &d, ex) ||
                   !(d >= 0 && d <= MAX_SAFE_INTEGER && d == trunc(d))) {
            JS_FreeValue(ctx, ex);
            DFAIL("23.2.5.1.3 step 5.a ToIndex(length) over unknown input whose example is no index: build the "
                  "fork over the example's own ToIndex outcome");
            return -1;
        }
        s->len_unknown = s->argp[2];
        s->argp[2] = ex;
    }
    /* ARRIVING FROM ABOVE IS NOT THE SAME AS BEING RE-ENTERED HERE, and the arm is what says so. This was
       `if (s->hdr.stage != TAC_PROTO)` around the reset — the negation asking "have I been here already", which
       is the one question a machine can never answer from its stage. It is answered by WHERE the dispatch
       enters instead: a resume at TAC_PROTO jumps past these two lines, so the value the prototype read has
       just delivered is still in cb_result. Without that, 10.1.14's realm fallback handed back the intrinsic
       prototype and `Reflect.construct(BigInt64Array, [], customProto)` silently ignored the custom one. State
       that must survive a suspension cannot be re-initialised on the path the suspension returns to. */
    s->hdr.stage = TAC_RANGE;
    cb_result = JS_UNDEFINED;
    STEP_ARM(TAC_RANGE);
    {
        /* Steps 8-9's RangeError over an unknown buffer length or offset: a predicate over them, so both arms run,
           and the in-range arm builds its view even where it reaches past the example bytes. */
        JSObject *bp = !s->is_copy && JS_VALUE_GET_TAG(s->argp[0]) == JS_TAG_OBJECT
                       ? JS_VALUE_GET_OBJ(s->argp[0]) : NULL;
        JSArrayBuffer *abuf = bp && bp->class_id == JS_CLASS_ARRAY_BUFFER ? bp->u.array_buffer : NULL;
        bool off_unknown = !JS_IsUninitialized(s->off_unknown);
        int size_log2 = typed_array_size_log2(s->hdr.arg), arm;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (off_unknown && (!abuf || array_buffer_is_resizable(abuf))) {
            DFAIL("23.2.5.1.3 over a SharedArrayBuffer or a resizable buffer at an unknown offset: build the "
                  "length-tracking window and Atomics' reads over that offset");
            return -1;
        }
        s->plan.offset_unknown = off_unknown;
        s->plan.len_unknown = !JS_IsUninitialized(s->len_unknown);
        if (abuf && !abuf->detached &&
            (!JS_IsUninitialized(abuf->length_unknown) || off_unknown || s->plan.len_unknown)) {
            if (JS_IsUninitialized(s->range_pred)) {
                JSValue sp[2], off;

                off = off_unknown ? js_dup(s->off_unknown) : JS_NewInt64(ctx, (int64_t)s->offset);
                if (JS_IsUndefined(s->argp[2])) {
                    sp[0] = off;
                } else {
                    JSValue ops[3];
                    uint64_t len = 0;

                    if (JS_IsUninitialized(s->len_unknown) && JS_ToIndex(ctx, &len, s->argp[2])) {
                        JS_FreeValue(ctx, off);
                        return -1;                       /* a primitive: invokes nothing */
                    }
                    ops[0] = JS_IsUninitialized(s->len_unknown) ? JS_NewInt64(ctx, (int64_t)len) : s->len_unknown;
                    ops[1] = off;
                    ops[2] = js_int32(size_log2);
                    sp[0] = off_unknown || !JS_IsUninitialized(s->len_unknown)
                        ? g_concolic.derived(ctx, "23.2.5.1.3 step 9.b.ii offset + newByteLength", ops, 3,
                                             JS_IsUninitialized(s->len_unknown) && s->off_example
                                             ? JS_NewInt64(ctx, (int64_t)(s->offset + (len << size_log2)))
                                             : JS_UNDEFINED)
                        : JS_NewInt64(ctx, (int64_t)(s->offset + (len << size_log2)));
                    DCHECK(!JS_IsUninitialized(sp[0]), "a view's unknown length or offset derived no byte extent");
                    JS_FreeValue(ctx, off);
                }
                sp[1] = JS_IsUninitialized(abuf->length_unknown) ? JS_NewInt64(ctx, abuf->byte_length)
                      : js_ab_length_derive(ctx, abuf, "25.1.6.1 get ArrayBuffer.prototype.byteLength", 0, 0,
                                            abuf->byte_length);
                CHECK(g_concolic.rel(ctx, sp + 2, OP_lte), "the relation hook declined a view's unknown range");
                s->range_pred = sp[0];
            }
            r = step_tobool_run(ctx, &s->hdr, s->range_pred, "23.2.5.1.3 steps 8-9 range test", &arm);
            if (r) return r < 0 ? -1 : r;
            if (!arm) {
                JS_ThrowRangeError(ctx, "invalid length");
                return -1;
            }
            s->plan.in_range = true;
            if (s->off_example && JS_IsUninitialized(abuf->length_unknown)) {
                /* over stated bytes this arm bounds the offset; where its example breaks the bound, it has none */
                uint64_t len = 0;

                if (!JS_IsUndefined(s->argp[2]) && JS_ToIndex(ctx, &len, s->argp[2])) return -1;
                if (s->offset + (len << size_log2) > (uint64_t)abuf->byte_length)
                    js_ta_ctor_offset_unknown(ctx, s, false);
            }
        }
        s->hdr.stage = TAC_PROTO;
    }
    STEP_ARM(TAC_PROTO);
    {
        /* Every operand is primitive now, so the plan's ToIndex and range checks invoke nothing and can run
           wherever the spec puts them: AFTER the allocation for an object first argument (the read already
           happened), BEFORE it for the element-count form, whose ToIndex is step 6.c.ii. */
        JSValue proto, obj;
        if (!s->planned && !s->is_copy) {
            if (js_ta_view_plan(ctx, &s->plan, s->hdr.argc, (JSValueConst *)s->argp, s->hdr.arg, s->offset) < 0)
                return -1;
            s->planned = 1;
        }
        if (s->proto_read) {
            proto = s->proto; s->proto = JS_UNDEFINED;
        } else {
            proto = JS_UNDEFINED;
            r = step_proto_from_ctor_run(ctx, &s->hdr, s->hdr.this_val, s->hdr.arg, cb_result, &proto,
                                         out_cb, out_argc);
            if (r) return r < 0 ? -1 : r;
        }
        obj = JS_NewObjectProtoClass(ctx, proto, s->hdr.arg);
        JS_FreeValue(ctx, proto);
        if (JS_IsException(obj))
            return -1;
        if (s->is_copy) {
            /* InitializeTypedArrayFromTypedArray reads the source's length AFTER the allocation, so a
               `prototype` getter that resizes or detaches the source is observed by this read. */
            JSObject *sp0 = JS_VALUE_GET_OBJ(s->argp[0]);
            s->result = js_ta_copy_finish(ctx, obj, s->argp[0], s->hdr.arg, sp0->u.array.count);
        } else {
            s->planned = 0;                  /* the finish takes the plan's buffer */
            s->result = js_ta_view_finish(ctx, obj, &s->plan);
            if (!JS_IsException(s->result) && s->plan.in_range && JS_IsUninitialized(s->len_unknown) &&
                !JS_IsUndefined(s->argp[2])) {
                uint64_t want = 0;

                if (JS_ToIndex(ctx, &want, s->argp[2]) == 0 && JS_VALUE_GET_OBJ(s->result)->u.array.count < want)
                    DFAIL("23.2.5.1.3 step 9.b.ii over a buffer whose length is unknown: a stated length reaches past "
                          "the buffer's example bytes — build a view whose stated length holds and whose bytes "
                          "past the example derive as a read past an unknown length does");
            }
            if (!JS_IsException(s->result) && !JS_IsUninitialized(s->len_unknown) &&
                JS_VALUE_GET_TAG(s->argp[0]) != JS_TAG_OBJECT) {
                /* 23.2.5.1.5 AllocateTypedArrayBuffer step 3: the new buffer's byteLength is elementSize ×
                   length, an unknown wherever length is one */
                JSTypedArray *nta = JS_VALUE_GET_OBJ(s->result)->u.typed_array;
                JSArrayBuffer *nab = nta->buffer->u.array_buffer;
                JSValue ops[2];

                DCHECK(JS_IsUninitialized(nab->length_unknown), "a buffer the element-count form allocated already "
                                                                "states an unknown length");
                ops[0] = s->len_unknown;
                ops[1] = js_int32(1 << typed_array_size_log2(s->hdr.arg));
                nab->length_unknown = g_concolic.derived(ctx, "23.2.5.1.5 AllocateTypedArrayBuffer step 3 (byteLength "
                                                         "is elementSize * length)", ops, 2,
                                                         js_concolic_held(s->len_unknown)
                                                         ? js_int64(nab->byte_length) : JS_UNDEFINED);
                DCHECK(!JS_IsUninitialized(nab->length_unknown), "an unknown element count derived no byte length");
            }
            if (!JS_IsException(s->result) && !JS_IsUninitialized(s->len_unknown)) {
                JSTypedArray *nta = JS_VALUE_GET_OBJ(s->result)->u.typed_array;

                JS_FreeValue(ctx, nta->length_unknown);
                nta->length_unknown = s->len_unknown;
                s->len_unknown = JS_UNINITIALIZED;
            }
            if (!JS_IsException(s->result) && !JS_IsUninitialized(s->off_unknown))
                js_ta_ctor_take_offset(ctx, s, JS_VALUE_GET_OBJ(s->result));
        }
    }
    return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
}

/* 23.2.5.1.3 steps 9.a.ii and 11 for a view `p` built at the example of an unknown offset: [[ByteOffset]] is that
   unknown, and a length taken from the buffer (step 9.a.ii's bufferByteLength - offset) is an unknown derived
   from it, with the stored count as its example where the offset has one. */
static void js_ta_ctor_take_offset(JSContext *ctx, JSTACtor *s, JSObject *p)
{
    JSTypedArray *ta = p->u.typed_array;
    JSArrayBuffer *abuf = ta->buffer->u.array_buffer;
    JSValue ops[3];

    DCHECK(JS_IsUninitialized(ta->offset_unknown) && (uint64_t)ta->offset == s->offset,
           "a view built at an unknown offset's example was given another offset");
    if (s->plan.len_from_buffer) {
        ops[0] = JS_IsUninitialized(abuf->length_unknown) ? js_int32(abuf->byte_length) : abuf->length_unknown;
        ops[1] = s->off_unknown;
        ops[2] = js_int32(typed_array_size_log2(p->class_id));
        JS_FreeValue(ctx, ta->length_unknown);
        ta->length_unknown = g_concolic.derived(ctx, "23.2.5.1.3 step 9.a.ii (newByteLength is bufferByteLength - "
                                                "offset)", ops, 3, s->off_example ? js_int32(p->u.array.count)
                                                : JS_UNDEFINED);
        DCHECK(!JS_IsUninitialized(ta->length_unknown), "a window over an unknown offset derived no length");
    }
    ta->offset_unknown = s->off_unknown;
    s->off_unknown = JS_UNINITIALIZED;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The three operands with their coerced primitives in place, the
   allocation's prototype when it was read first, and — only once the plan has been made — the buffer that plan
   holds. `planned` is that condition, which is why the buffer is an `if` around the visit rather than a field in
   a list; `proto` needs no such guard because it is UNDEFINED until it is read. */
static void js_ta_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSTACtor *s = st;
    int i;
    v->val(ctx, &s->result);
    v->val(ctx, &s->proto);
    for (i = 0; i < 3; i++) v->val(ctx, &s->argp[i]);
    v->val(ctx, &s->len_unknown);
    v->val(ctx, &s->range_pred);
    v->val(ctx, &s->off_unknown);
    v->val(ctx, &s->off_pred);
    v->val(ctx, &s->count_pred);
    if (s->planned) v->val(ctx, &s->plan.buffer);
}

static JSValue js_ta_ctor_fini(JSContext *ctx, void *st, bool take_result)
{
    JSTACtor *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

#define TACTOR_DEF(k) { sizeof(JSTACtor), js_ta_ctor_step, js_ta_ctor_fini, JS_CLASS_UINT8C_ARRAY + (k), \
                        .visit = js_ta_ctor_visit, .algorithm = "23.2.5.1 %TypedArray% ( ...args )", \
                        .steps = js_ta_ctor_steps }
/* one per element type; `arg` carries the class id the body switches on. No STEPDEF id: the dispatch names the
   definition by pointer, the way a DELEGATE does. */
static const JSTrampStepDef js_ta_ctor_defs[JS_TYPED_ARRAY_COUNT] = {
    TACTOR_DEF(0), TACTOR_DEF(1), TACTOR_DEF(2), TACTOR_DEF(3), TACTOR_DEF(4), TACTOR_DEF(5),
    TACTOR_DEF(6), TACTOR_DEF(7), TACTOR_DEF(8), TACTOR_DEF(9), TACTOR_DEF(10), TACTOR_DEF(11),
};
#undef TACTOR_DEF
_Static_assert(JS_TYPED_ARRAY_COUNT == 12,
               "an element type was added: give it a TACTOR_DEF row, or its buffer-view constructor's argument "
               "coercions silently lose their definition");

/* `obj_in` is the object 10.1.14 produced when the CALLER performed that read as a request (the routed view
   entry); UNDEFINED means "create it here", which is what an internal caller with new_target = undefined wants. */
/* 23.2.5.1 up to but NOT including AllocateTypedArray: pick the buffer and the view's offset and length, with
   every ToIndex and every range check the spec puts BEFORE the `prototype` read. Splitting here is what lets the
   routed entry perform that read as a request without moving it: a Symbol element count must throw ToIndex's
   TypeError before new.target's prototype getter runs, and a detached buffer must throw before it too. */
/* InitializeTypedArrayFromArrayBuffer steps 2-3, ALONE. They are their own function because the spec runs them
   before step 5's ToIndex(length), and that ToIndex may need a ToPrimitive the machine can only perform as a
   request — so `new Int32Array(buffer, 1, {valueOf(){ throw }})` must report the misaligned offset's RangeError
   and never reach the length at all. */
static int ta_view_offset(JSContext *ctx, JSValueConst byte_offset, int size_log2, uint64_t *poffset)
{
    DCHECK(!js_value_is_concolic(byte_offset), "a view's unknown byteOffset reached the stated-offset conversion: "
                                               "js_ta_ctor_step's TAC_OFFSET_FORK owns it");
    if (JS_ToIndex(ctx, poffset, byte_offset))
        return -1;
    if ((*poffset & (((uint64_t)1 << size_log2) - 1)) != 0) {
        JS_ThrowRangeError(ctx, "invalid offset");
        return -1;
    }
    return 0;
}

static int js_ta_view_plan(JSContext *ctx, JSTAViewPlan *pl, int argc, JSValueConst *argv, int classid,
                           uint64_t offset)
{
    bool track_rab = false, len_from_buffer = false;
    JSValue buffer;
    JSArrayBuffer *abuf;
    int size_log2;
    uint64_t len;

    size_log2 = typed_array_size_log2(classid);
    if (JS_VALUE_GET_TAG(argv[0]) != JS_TAG_OBJECT) {
        if (JS_ToIndex(ctx, &len, argv[0]))
            return -1;
        buffer = js_array_buffer_constructor1(ctx, len << size_log2, NULL);
        if (JS_IsException(buffer))
            return -1;
        offset = 0;
    } else {
        JSObject *p = JS_VALUE_GET_OBJ(argv[0]);
        DCHECK(p->class_id == JS_CLASS_ARRAY_BUFFER || p->class_id == JS_CLASS_SHARED_ARRAY_BUFFER,
               "the view plan was handed a source the consume walk or the typed-array copy owns");
        uint64_t elem_size = (uint64_t)1 << size_log2, buf_len;
        bool fixed_length;
        abuf = p->u.array_buffer;
        /* InitializeTypedArrayFromArrayBuffer steps 4-10, in the order it states them. Every operand reaching
           here is already primitive, so no page code runs BETWEEN these steps — what the order decides is WHICH
           error a bad call reports, and testing detachment before the alignment made
           `new Int32Array(detachedBuffer, 1, 0)` answer the TypeError of step 6 where the spec answers the
           RangeError of step 3. */
        DCHECK((offset & (elem_size - 1)) == 0,
               "steps 2-3 ran before the length was coerced; the offset reaching the plan is already aligned");
        fixed_length = !array_buffer_is_resizable(abuf);        /* step 4 */
        if (!JS_IsUndefined(argv[2]) && JS_ToIndex(ctx, &len, argv[2]))   /* step 5 */
            return -1;
        if (abuf->detached) {                                   /* step 6 */
            JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
            return -1;
        }
        buf_len = abuf->byte_length;                            /* step 7 */
        if (pl->in_range) {
            /* the caller's fork decided steps 8-9 over the unknown length or offset; the stored bytes and
               `offset` are their examples */
            DCHECK(!JS_IsUninitialized(abuf->length_unknown) || pl->offset_unknown || pl->len_unknown,
                   "an in-range plan over a buffer whose length, the offset and the view's length are all stated");
            len_from_buffer = JS_IsUndefined(argv[2]);
            if (len_from_buffer && fixed_length && JS_IsUninitialized(abuf->length_unknown) &&
                (buf_len & (elem_size - 1)) != 0)
                goto invalid_length;                            /* step 9.a.i over a stated length */
            if (len_from_buffer)
                len = offset <= buf_len ? (buf_len - offset) >> size_log2 : 0;
        } else if (JS_IsUndefined(argv[2])) {
            track_rab = !fixed_length;
            len_from_buffer = true;
            /* step 8 for a length-tracking view, step 9 for a fixed one: the first only requires the offset to
               be inside the buffer, the second also requires the WHOLE buffer to divide into elements. */
            if (fixed_length && (buf_len & (elem_size - 1)) != 0)
                goto invalid_length;
            if (offset > buf_len)
                goto invalid_length;
            len = (buf_len - offset) >> size_log2;
        } else {
            if ((offset + (len << size_log2)) > buf_len) {       /* step 9.b.ii */
            invalid_length:
                JS_ThrowRangeError(ctx, "invalid length");
                return -1;
            }
        }
        buffer = js_dup(argv[0]);
    }

    pl->buffer = buffer;
    pl->len = len;
    pl->offset = offset;
    pl->track_rab = track_rab;
    pl->len_from_buffer = len_from_buffer;
    pl->size_log2 = size_log2;
    return 0;
}

/* 23.2.5.1's tail: the object exists, so re-validate the buffer (the `prototype` read is user code and can have
   detached or resized it) and initialise the view. Takes the plan and the created object, both owned. */
static JSValue js_ta_view_finish(JSContext *ctx, JSValue obj, JSTAViewPlan *pl)
{
    JSValue buffer = pl->buffer;
    JSArrayBuffer *abuf;
    int size_log2 = pl->size_log2;
    uint64_t len = pl->len, offset = pl->offset;
    bool track_rab = pl->track_rab;

    // Re-validate buffer after js_create_from_ctor which may have run JS code
    // that resized or detached the ArrayBuffer
    abuf = JS_VALUE_GET_OBJ(buffer)->u.array_buffer;
    if (abuf->detached) {
        JS_FreeValue(ctx, buffer);
        JS_FreeValue(ctx, obj);
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    }
    if (pl->in_range && (!JS_IsUninitialized(abuf->length_unknown) || pl->len_unknown)) {
        /* In range in this world: the view holds the example bytes its window covers, possibly none, and its
           length is an unknown (below, or the constructor's), which every read past those bytes derives from. */
        uint64_t have = offset <= abuf->byte_length ? (abuf->byte_length - offset) >> size_log2 : 0;

        if (!pl->len_from_buffer)
            len = len < have ? len : have;
        if (typed_array_init(ctx, obj, buffer, offset, len, false)) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        if (pl->len_from_buffer)
            JS_VALUE_GET_OBJ(obj)->u.typed_array->length_unknown =
                js_ab_length_derive(ctx, abuf, "23.2.5.1.3 InitializeTypedArrayFromArrayBuffer step 9",
                                    (uint32_t)offset, size_log2, (int64_t)len);
        return obj;
    }
    if (offset > abuf->byte_length) {
        JS_FreeValue(ctx, buffer);
        JS_FreeValue(ctx, obj);
        return JS_ThrowRangeError(ctx, "invalid offset");
    }
    if (track_rab) {
        // Recalculate length for RAB-backed view
        len = (abuf->byte_length - offset) >> size_log2;
    } else if ((offset + (len << size_log2)) > abuf->byte_length) {
        JS_FreeValue(ctx, buffer);
        JS_FreeValue(ctx, obj);
        return JS_ThrowRangeError(ctx, "invalid length");
    }
    if (typed_array_init(ctx, obj, buffer, offset, len, track_rab)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (pl->len_from_buffer && !JS_IsUninitialized(abuf->length_unknown)) {
        JS_VALUE_GET_OBJ(obj)->u.typed_array->length_unknown =
            js_ab_length_derive(ctx, abuf, "23.2.5.1.3 InitializeTypedArrayFromArrayBuffer step 9", (uint32_t)offset,
                                size_log2, (int64_t)len);
    }
    return obj;
}

/* The unrouted entry: internal callers (which pass new_target = undefined, so 10.1.14 reads nothing) and the two
   OBJECT-SOURCE branches, which have their own routes and whose C bodies assert as much. */
static JSValue js_typed_array_constructor(JSContext *ctx,
                                          JSValueConst new_target,
                                          int argc, JSValueConst *argv,
                                          int classid)
{
    JSTAViewPlan pl = { .in_range = false };
    JSValue obj;
    uint64_t offset = 0;

    if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(argv[0]);
        if (p->class_id != JS_CLASS_ARRAY_BUFFER && p->class_id != JS_CLASS_SHARED_ARRAY_BUFFER) {
            if (is_typed_array(p->class_id))
                return js_typed_array_constructor_ta(ctx, new_target, argv[0], classid, p->u.array.count);
            return js_typed_array_constructor_obj(ctx, new_target, argv[0], classid);
        }
        if (ta_view_offset(ctx, argv[1], typed_array_size_log2(classid), &offset) < 0)
            return JS_EXCEPTION;
    }
    if (js_ta_view_plan(ctx, &pl, argc, argv, classid, offset) < 0)
        return JS_EXCEPTION;
    obj = js_create_from_ctor(ctx, new_target, classid);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, pl.buffer);
        return JS_EXCEPTION;
    }
    return js_ta_view_finish(ctx, obj, &pl);
}

static void js_typed_array_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSTypedArray *ta = p->u.typed_array;
    if (ta) {
        /* during the GC the finalizers are called in an arbitrary
           order so the ArrayBuffer finalizer may have been called */
        if (ta->link.next) {
            list_del(&ta->link);
        }
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, ta->buffer));
        JS_FreeValueRT(rt, ta->length_unknown);
        JS_FreeValueRT(rt, ta->offset_unknown);
        js_free_rt(rt, ta);
    }
}

static void js_typed_array_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSTypedArray *ta = p->u.typed_array;
    if (ta) {
        JS_MarkValue(rt, JS_MKPTR(JS_TAG_OBJECT, ta->buffer), mark_func);
        JS_MarkValue(rt, ta->length_unknown, mark_func);
        JS_MarkValue(rt, ta->offset_unknown, mark_func);
    }
}

