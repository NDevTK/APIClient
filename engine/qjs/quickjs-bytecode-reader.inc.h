/* One half-built value. `obj` is the container being filled; `i`/`len` is where the fill has got to. */
typedef struct BCRFrame {
    uint8_t  kind;
    uint8_t  phase;
    JSValue  obj;       /* the container being filled, where the arm builds one up front */
    uint32_t i, len;
    bool     flag;      /* ARRAY: template, so a trailing `raw` follows the elements */
    JSAtom   atom;      /* OBJ: the key whose value is being read */
    JSValue  key;       /* MAP: the key, held across the value's own read */
    uint8_t  phase2_pending;  /* MAP: a key is in hand and the value is owed */
    void    *p1;        /* FUNC: JSFunctionBytecode*   MODULE: JSModuleDef* */
    uint32_t u1, u2, u3;/* TA: class id, length, byte offset (u3 = the reserved ref slot) */
} BCRFrame;
enum { BCR_READ, BCR_ARRAY, BCR_OBJ, BCR_MAP, BCR_DATE, BCR_OBJVAL,
       BCR_TA, BCR_FUNC, BCR_MODULE, BCR_SERIALIZABLE, BCR_ABSPAN };

typedef struct BCReaderState {
    JSContext *ctx;
    const uint8_t *buf_start, *ptr, *buf_end;
    uint32_t first_atom;
    uint32_t idx_to_atom_count;
    JSAtom *idx_to_atom;
    int error_state;
    bool allow_bytecode;
    bool allow_reference;
    /* object references */
    JSObject **objects;
    int objects_count;
    int objects_size;
    /* HTML §2.7.8's `memory`, the [[TransferredValues]] its transfer-receiving steps built before this read
       began, or NULL for a read that is not a StructuredDeserializeWithTransfer. A stream naming a transferred
       value is refused outright without it — the tag has no meaning a reader may invent. */
    const JSTransferReadHook *transfer;
    /* HTML §2.7.1's serializable seam — what creates an instance of a named interface in THIS realm and runs
       its deserialization steps — or NULL for a read that has none, which refuses the tag outright. */
    const JSSerializableReadHook *serializable;
    /* used for JS_DUMP_READ_OBJECT */
    const uint8_t *ptr_last;
    int level;
    /* THE READER'S FRAME STACK. The writer emits as it descends, so a work stack of pending items was enough
       for it. The reader BUILDS: every child has to be placed into a half-constructed parent, so a frame
       carries the container AND a cursor, and the finished child arrives in r_result. It lives here rather
       than in C locals for the same reason the parser's does — everything live at a frame boundary is in the
       struct, so the driver can return mid-graph and be re-entered.
       Arms not yet converted still call the old helper, which re-enters the driver as a nested activation;
       r_base is that activation's floor, exactly as the parser's pd_base was during its own conversion. */
    struct BCRFrame *r_stack;
    int r_sp, r_size, r_base;
    JSValue r_result;
} BCReaderState;

#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
static void JS_PRINTF_FORMAT_ATTR(2, 3) bc_read_trace(BCReaderState *s, JS_PRINTF_FORMAT const char *fmt, ...) {
    va_list ap;
    int i, n, n0;

    if (!check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT))
        return;

    if (!s->ptr_last)
        s->ptr_last = s->buf_start;

    n = n0 = 0;
    if (s->ptr > s->ptr_last || s->ptr == s->buf_start) {
        n0 = printf("%04x: ", (int)(s->ptr_last - s->buf_start));
        n += n0;
    }
    for (i = 0; s->ptr_last < s->ptr; i++) {
        if ((i & 7) == 0 && i > 0) {
            printf("\n%*s", n0, "");
            n = n0;
        }
        n += printf(" %02x", *s->ptr_last++);
    }
    if (*fmt == '}')
        s->level--;
    if (n < 32 + s->level * 2) {
        printf("%*s", 32 + s->level * 2 - n, "");
    }
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    if (strchr(fmt, '{'))
        s->level++;
}
#else
#define bc_read_trace(...)
#endif

static int bc_read_error_end(BCReaderState *s)
{
    if (!s->error_state) {
        JS_ThrowSyntaxError(s->ctx, "read after the end of the buffer");
    }
    return s->error_state = -1;
}

static int bc_get_u8(BCReaderState *s, uint8_t *pval)
{
    if (unlikely(s->buf_end - s->ptr < 1)) {
        *pval = 0; /* avoid warning */
        return bc_read_error_end(s);
    }
    *pval = *s->ptr++;
    return 0;
}

static int bc_get_u16(BCReaderState *s, uint16_t *pval)
{
    uint16_t v;
    if (unlikely(s->buf_end - s->ptr < 2)) {
        *pval = 0; /* avoid warning */
        return bc_read_error_end(s);
    }
    v = get_u16(s->ptr);
    *pval = v;
    s->ptr += 2;
    return 0;
}

static __maybe_unused int bc_get_u32(BCReaderState *s, uint32_t *pval)
{
    uint32_t v;
    if (unlikely(s->buf_end - s->ptr < 4)) {
        *pval = 0; /* avoid warning */
        return bc_read_error_end(s);
    }
    v = get_u32(s->ptr);
    *pval = v;
    s->ptr += 4;
    return 0;
}

static int bc_get_u64(BCReaderState *s, uint64_t *pval)
{
    uint64_t v;
    if (unlikely(s->buf_end - s->ptr < 8)) {
        *pval = 0; /* avoid warning */
        return bc_read_error_end(s);
    }
    v = get_u64(s->ptr);
    *pval = v;
    s->ptr += 8;
    return 0;
}

static int bc_get_leb128(BCReaderState *s, uint32_t *pval)
{
    int ret;
    ret = get_leb128(pval, s->ptr, s->buf_end);
    if (unlikely(ret < 0))
        return bc_read_error_end(s);
    s->ptr += ret;
    return 0;
}

static int bc_get_sleb128(BCReaderState *s, int32_t *pval)
{
    int ret;
    ret = get_sleb128(pval, s->ptr, s->buf_end);
    if (unlikely(ret < 0))
        return bc_read_error_end(s);
    s->ptr += ret;
    return 0;
}

/* XXX: used to read an `int` with a positive value */
static int bc_get_leb128_int(BCReaderState *s, int *pval)
{
    return bc_get_leb128(s, (uint32_t *)pval);
}

static int bc_get_leb128_u16(BCReaderState *s, uint16_t *pval)
{
    uint32_t val;
    if (bc_get_leb128(s, &val)) {
        *pval = 0;
        return -1;
    }
    *pval = val;
    return 0;
}

static int bc_get_buf(BCReaderState *s, void *buf, uint32_t buf_len)
{
    if (buf_len != 0) {
        if (unlikely(!buf || s->buf_end - s->ptr < buf_len))
            return bc_read_error_end(s);
        memcpy(buf, s->ptr, buf_len);
        s->ptr += buf_len;
    }
    return 0;
}

static int bc_idx_to_atom(BCReaderState *s, JSAtom *patom, uint32_t idx)
{
    JSAtom atom;

    if (__JS_AtomIsTaggedInt(idx)) {
        atom = idx;
    } else if (idx < s->first_atom) {
        atom = JS_DupAtom(s->ctx, idx);
    } else {
        idx -= s->first_atom;
        if (idx >= s->idx_to_atom_count) {
            JS_ThrowSyntaxError(s->ctx, "invalid atom index (pos=%u)",
                                (unsigned int)(s->ptr - s->buf_start));
            *patom = JS_ATOM_NULL;
            return s->error_state = -1;
        }
        atom = JS_DupAtom(s->ctx, s->idx_to_atom[idx]);
    }
    *patom = atom;
    return 0;
}

static int bc_get_atom(BCReaderState *s, JSAtom *patom)
{
    uint32_t v;
    if (bc_get_leb128(s, &v))
        return -1;
    if (v & 1) {
        *patom = __JS_AtomFromUInt32(v >> 1);
        return 0;
    } else {
        return bc_idx_to_atom(s, patom, v >> 1);
    }
}

static JSString *JS_ReadString(BCReaderState *s)
{
    uint32_t len;
    size_t size;
    bool is_wide_char;
    JSString *p;

    if (bc_get_leb128(s, &len))
        return NULL;
    is_wide_char = len & 1;
    len >>= 1;
    if (len > JS_STRING_LEN_MAX) {
        JS_ThrowInternalError(s->ctx, "string too long");
        return NULL;
    }
    p = js_alloc_string(s->ctx, len, is_wide_char);
    if (!p) {
        s->error_state = -1;
        return NULL;
    }
    size = (size_t)len << is_wide_char;
    if ((s->buf_end - s->ptr) < size) {
        bc_read_error_end(s);
        js_free_string(s->ctx->rt, p);
        return NULL;
    }
    memcpy(str8(p), s->ptr, size);
    s->ptr += size;
    if (!is_wide_char)
        str8(p)[size] = '\0'; /* add the trailing zero for 8 bit strings */
#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
    if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
        bc_read_trace(s, "%s", ""); // hex dump and indentation
        JS_DumpString(s->ctx->rt, p);
        printf("\n");
    }
#endif
    return p;
}

static uint32_t bc_get_flags(uint32_t flags, int *pidx, int n)
{
    uint32_t val;
    /* XXX: this does not work for n == 32 */
    val = (flags >> *pidx) & ((1U << n) - 1);
    *pidx += n;
    return val;
}

static int JS_ReadFunctionBytecode(BCReaderState *s, JSFunctionBytecode *b,
                                   int byte_code_offset, uint32_t bc_len)
{
    uint8_t *bc_buf;
    int pos, len, op;
    JSAtom atom;
    uint32_t idx;

    bc_buf = (uint8_t*)b + byte_code_offset;
    if (bc_get_buf(s, bc_buf, bc_len))
        return -1;
    b->byte_code_buf = bc_buf;

    pos = 0;
    while (pos < bc_len) {
        op = bc_buf[pos];
        len = short_opcode_info(op).size;
        switch(short_opcode_info(op).fmt) {
        case OP_FMT_atom:
        case OP_FMT_atom_u8:
        case OP_FMT_atom_u16:
        case OP_FMT_atom_label_u8:
        case OP_FMT_atom_label_u16:
            idx = get_u32(bc_buf + pos + 1);
            if (bc_idx_to_atom(s, &atom, idx)) {
                /* Note: the atoms will be freed up to this position */
                b->byte_code_len = pos;
                return -1;
            }
            put_u32(bc_buf + pos + 1, atom);
            break;
        default:
            break;
        }
#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
        if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
            const uint8_t *save_ptr = s->ptr;
            s->ptr = s->ptr_last + len;
            s->level -= 4;
            bc_read_trace(s, "%s", "");  // hex dump + indent
            dump_single_byte_code(s->ctx, bc_buf + pos, b,
                                  s->ptr - s->buf_start - len);
            s->level += 4;
            s->ptr = save_ptr;
        }
#endif
        pos += len;
    }
    return 0;
}

static JSValue JS_ReadBigInt(BCReaderState *s)
{
    JSValue obj = JS_UNDEFINED;
    uint32_t len, i, n;
    JSBigInt *p;
    js_limb_t v;
    uint8_t v8;

    if (bc_get_leb128(s, &len))
        goto fail;
    bc_read_trace(s, "len=%" PRId64 "\n", (int64_t)len);
    if (len == 0) {
        /* zero case */
        bc_read_trace(s, "}\n");
        return __JS_NewShortBigInt(s->ctx, 0);
    }
    p = js_bigint_new(s->ctx, (len - 1) / (JS_LIMB_BITS / 8) + 1);
    if (!p)
        goto fail;
    for(i = 0; i < len / (JS_LIMB_BITS / 8); i++) {
        if (bc_get_u32(s, &v))
            goto fail;
        p->tab[i] = v;
    }
    n = len % (JS_LIMB_BITS / 8);
    if (n != 0) {
        int shift;
        v = 0;
        for(i = 0; i < n; i++) {
            if (bc_get_u8(s, &v8))
                goto fail;
            v |= (js_limb_t)v8 << (i * 8);
        }
        shift = JS_LIMB_BITS - n * 8;
        /* extend the sign */
        if (shift != 0) {
            v = (js_slimb_t)(v << shift) >> shift;
        }
        p->tab[p->len - 1] = v;
    }
    bc_read_trace(s, "}\n");
    return JS_CompactBigInt(s->ctx, p);
 fail:
    JS_FreeValue(s->ctx, obj);
    return JS_EXCEPTION;
}

static int bcr_push(BCReaderState *s)
{
    if (s->r_sp >= s->r_size) {
        int n = s->r_size ? s->r_size * 2 : 16;
        BCRFrame *t = js_realloc(s->ctx, s->r_stack, sizeof(*t) * n);
        if (!t)
            return -1;
        s->r_stack = t;
        s->r_size = n;
    }
    memset(&s->r_stack[s->r_sp], 0, sizeof(s->r_stack[0]));
    s->r_stack[s->r_sp].obj = JS_UNDEFINED;
    s->r_stack[s->r_sp].key = JS_UNDEFINED;
    s->r_stack[s->r_sp].atom = JS_ATOM_NULL;
    s->r_sp++;
    return 0;
}

static JSValue js_map_set(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv, int magic);
static JSValue JS_ReadObjectRec(BCReaderState *s);

static int BC_add_object_ref1(BCReaderState *s, JSObject *p)
{
    if (s->allow_reference) {
        if (js_resize_array(s->ctx, (void *)&s->objects,
                            sizeof(s->objects[0]),
                            &s->objects_size, s->objects_count + 1))
            return -1;
        s->objects[s->objects_count++] = p;
    }
    return 0;
}

static int BC_add_object_ref(BCReaderState *s, JSValue obj)
{
    return BC_add_object_ref1(s, JS_VALUE_GET_OBJ(obj));
}

static JSValue JS_ReadFunctionTag(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    JSFunctionBytecode bc, *b;
    JSValue obj = JS_UNDEFINED;
    uint16_t v16;
    uint8_t v8;
    int idx, i, local_count, has_debug_info;
    int function_size, cpool_offset, byte_code_offset;
    int closure_var_offset, vardefs_offset;

    memset(&bc, 0, sizeof(bc));
    //bc.gc_header.mark = 0;

    if (bc_get_u16(s, &v16))
        goto fail;
    idx = 0;
    bc.has_prototype = bc_get_flags(v16, &idx, 1);
    bc.has_simple_parameter_list = bc_get_flags(v16, &idx, 1);
    bc.is_derived_class_constructor = bc_get_flags(v16, &idx, 1);
    bc.need_home_object = bc_get_flags(v16, &idx, 1);
    bc.func_kind = bc_get_flags(v16, &idx, 2);
    bc.new_target_allowed = bc_get_flags(v16, &idx, 1);
    bc.super_call_allowed = bc_get_flags(v16, &idx, 1);
    bc.super_allowed = bc_get_flags(v16, &idx, 1);
    bc.arguments_allowed = bc_get_flags(v16, &idx, 1);
    bc.backtrace_barrier = bc_get_flags(v16, &idx, 1);
    bc.from_eval = bc_get_flags(v16, &idx, 1);
    bc.is_program = bc_get_flags(v16, &idx, 1);
    has_debug_info = bc_get_flags(v16, &idx, 1);
    bc.from_inline_script = bc_get_flags(v16, &idx, 1);   /* last: see the writer for why the version holds */
    if (bc_get_u8(s, &v8))
        goto fail;
    bc.is_strict_mode = (v8 > 0);
    if (bc_get_atom(s, &bc.func_name))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.arg_count))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.var_count))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.defined_arg_count))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.stack_size))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.var_ref_count))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.closure_var_count))
        goto fail;
    if (bc_get_leb128_int(s, &bc.cpool_count))
        goto fail;
    if (bc_get_leb128_int(s, &bc.byte_code_len))
        goto fail;
    if (bc_get_leb128_int(s, &local_count))
        goto fail;

    function_size = sizeof(*b);
    cpool_offset = function_size;
    function_size += bc.cpool_count * sizeof(*bc.cpool);
    vardefs_offset = function_size;
    function_size += local_count * sizeof(*bc.vardefs);
    closure_var_offset = function_size;
    function_size += bc.closure_var_count * sizeof(*bc.closure_var);
    byte_code_offset = function_size;
    function_size += bc.byte_code_len;

    b = js_mallocz(ctx, function_size);
    if (!b)
        goto fail;

    memcpy(b, &bc, sizeof(*b));
    bc.func_name = JS_ATOM_NULL;
    JS_REF_COUNT(b) = 1;
    if (local_count != 0) {
        b->vardefs = (void *)((uint8_t*)b + vardefs_offset);
    }
    if (b->closure_var_count != 0) {
        b->closure_var = (void *)((uint8_t*)b + closure_var_offset);
    }
    if (b->cpool_count != 0) {
        b->cpool = (void *)((uint8_t*)b + cpool_offset);
    }

    add_gc_object(ctx->rt, &b->header, JS_GC_OBJ_TYPE_FUNCTION_BYTECODE);

    obj = JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, b);

#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
    if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
        if (b->func_name) {
            bc_read_trace(s, "name: ");
            print_atom(s->ctx, b->func_name);
            printf("\n");
        }
    }
#endif
    bc_read_trace(s, "args=%d vars=%d defargs=%d closures=%d cpool=%d\n",
                  b->arg_count, b->var_count, b->defined_arg_count,
                  b->closure_var_count, b->cpool_count);
    bc_read_trace(s, "stack=%d bclen=%d locals=%d\n",
                  b->stack_size, b->byte_code_len, local_count);

    if (local_count != 0) {
        bc_read_trace(s, "vars {\n");
        bc_read_trace(s, "off flags scope name\n");
        for(i = 0; i < local_count; i++) {
            JSVarDef *vd = &b->vardefs[i];
            if (bc_get_atom(s, &vd->var_name))
                goto fail;
            if (bc_get_leb128_int(s, &vd->scope_level))
                goto fail;
            if (bc_get_leb128_int(s, &vd->scope_next))
                goto fail;
            vd->scope_next--;
            if (bc_get_u8(s, &v8))
                goto fail;
            idx = 0;
            vd->var_kind = bc_get_flags(v8, &idx, 4);
            vd->is_const = bc_get_flags(v8, &idx, 1);
            vd->is_lexical = bc_get_flags(v8, &idx, 1);
            vd->is_captured = bc_get_flags(v8, &idx, 1);
            if (vd->is_captured) {
                if (bc_get_leb128_u16(s, &vd->var_ref_idx))
                    goto fail;
            }
#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
            if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
                bc_read_trace(s, "%3d  %d%c%c%c %4d  ",
                              i, vd->var_kind,
                              vd->is_const ? 'C' : '.',
                              vd->is_lexical ? 'L' : '.',
                              vd->is_captured ? 'X' : '.',
                              vd->scope_level);
                print_atom(s->ctx, vd->var_name);
                printf("\n");
            }
#endif
        }
        bc_read_trace(s, "}\n");
    }
    if (b->closure_var_count != 0) {
        bc_read_trace(s, "closure vars {\n");
        bc_read_trace(s, "off  flags idx  name\n");
        for(i = 0; i < b->closure_var_count; i++) {
            JSClosureVar *cv = &b->closure_var[i];
            int var_idx, flags;
            if (bc_get_atom(s, &cv->var_name))
                goto fail;
            if (bc_get_leb128_int(s, &var_idx))
                goto fail;
            cv->var_idx = var_idx;
            if (bc_get_leb128_int(s, &flags))
                goto fail;
            idx = 0;
            cv->closure_type = bc_get_flags(flags, &idx, 3);
            cv->is_const = bc_get_flags(flags, &idx, 1);
            cv->is_lexical = bc_get_flags(flags, &idx, 1);
            cv->var_kind = bc_get_flags(flags, &idx, 4);
#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
            if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
                bc_read_trace(s, "%3d  %d:%d%c%c %3d  ",
                              i, cv->var_kind, cv->closure_type,
                              cv->is_const ? 'C' : '.',
                              cv->is_lexical ? 'X' : '.',
                              cv->var_idx);
                print_atom(s->ctx, cv->var_name);
                printf("\n");
            }
#endif
        }
        bc_read_trace(s, "}\n");
    }
    /* THE CONSTANT POOL IS A FRAME, and the bytecode that follows it is the frame's tail. Splitting here is
       what lets a nested function — a constant of its parent — be read without a C frame per level. The
       byte-code offset the tail needs is carried on the frame, not in a local this function no longer owns. */
    if (bcr_push(s) < 0)
        goto fail;
    {
        BCRFrame *f = &s->r_stack[s->r_sp - 1];
        f->kind = BCR_FUNC;
        f->obj = obj;
        f->p1 = b;
        f->len = b->cpool_count;
        f->u1 = byte_code_offset;
        f->u2 = has_debug_info;
    }
    return JS_UNDEFINED;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* Everything in a function record that follows its constant pool. */
static JSValue bcr_func_tail(BCReaderState *s, BCRFrame *fr)
{
    JSContext *ctx = s->ctx;
    JSValue obj = fr->obj;
    JSFunctionBytecode *b = fr->p1;
    uint32_t byte_code_offset = fr->u1;
    bool has_debug_info = fr->u2;

    {
        bc_read_trace(s, "bytecode {\n");
        if (JS_ReadFunctionBytecode(s, b, byte_code_offset, b->byte_code_len))
            goto fail;
        bc_read_trace(s, "}\n");
    }
    if (!has_debug_info)
        goto nodebug;

    /* read optional debug information */
    bc_read_trace(s, "debug {\n");
    if (bc_get_atom(s, &b->filename))
        goto fail;
    if (bc_get_leb128_int(s, &b->line_num))
        goto fail;
    if (bc_get_leb128_int(s, &b->col_num))
        goto fail;
#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
    if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
        bc_read_trace(s, "filename: ");
        print_atom(s->ctx, b->filename);
        printf(", line: %d, column: %d\n", b->line_num, b->col_num);
    }
#endif
    if (bc_get_leb128_int(s, &b->pc2line_len))
        goto fail;
    if (b->pc2line_len) {
        bc_read_trace(s, "positions: %d bytes\n", b->pc2line_len);
        b->pc2line_buf = js_mallocz(ctx, b->pc2line_len);
        if (!b->pc2line_buf)
            goto fail;
        if (bc_get_buf(s, b->pc2line_buf, b->pc2line_len))
            goto fail;
    }
    if (bc_get_leb128_int(s, &b->source_len))
        goto fail;
    if (b->source_len) {
        bc_read_trace(s, "source: %d bytes\n", b->source_len);
        if (s->ptr_last)
            s->ptr_last += b->source_len;  // omit source code hex dump
        /* b->source is a UTF-8 encoded null terminated C string */
        b->source = js_mallocz(ctx, b->source_len + 1);
        if (!b->source)
            goto fail;
        if (bc_get_buf(s, b->source, b->source_len))
            goto fail;
    }
    bc_read_trace(s, "}\n");

 nodebug:
    b->realm = JS_DupContext(ctx);
    return obj;

 fail:
    /* No bc.func_name release here: that local belongs to the header half, and by the time the tail runs the
       name is owned by b and is freed with it. */
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadModule(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    JSValue obj;
    JSModuleDef *m = NULL;
    JSAtom module_name;
    int i;
    uint8_t v8;

    if (bc_get_atom(s, &module_name))
        goto fail;
#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
    if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
        bc_read_trace(s, "name: ");
        print_atom(s->ctx, module_name);
        printf("\n");
    }
#endif
    m = js_new_module_def(ctx, module_name);
    if (!m)
        goto fail;
    obj = js_dup(JS_MKPTR(JS_TAG_MODULE, m));
    if (bc_get_leb128_int(s, &m->req_module_entries_count))
        goto fail;
    obj = JS_NewModuleValue(ctx, m);
    if (m->req_module_entries_count != 0) {
        m->req_module_entries_size = m->req_module_entries_count;
        m->req_module_entries = js_mallocz(ctx, sizeof(m->req_module_entries[0]) * m->req_module_entries_size);
        if (!m->req_module_entries)
            goto fail;
        for(i = 0; i < m->req_module_entries_count; i++) {
            JSReqModuleEntry *rme = &m->req_module_entries[i];
            if (bc_get_atom(s, &rme->module_name))
                goto fail;
            /* A DESERIALISED RECORD IS A PARSED ONE: [[RequestedModules]] from the stream, [[LoadedModules]]
               EMPTY. This asked the module loader for every request right here, which is the same conflation
               the parser had — a synchronous host call on a path with nowhere to park, made worse by being
               inside JS_ReadObject, which is not a flow and never could wait. JS_WriteModule does not
               serialise a graph transitively anyway, so this could only ever ask the loader; the load phase
               (js_module_load_requested, reached through JS_EvalFunction) asks it for exactly the same
               specifiers, once, and can park while it does. */
        }
    }

    if (bc_get_leb128_int(s, &m->export_entries_count))
        goto fail;
    if (m->export_entries_count != 0) {
        m->export_entries_size = m->export_entries_count;
        m->export_entries = js_mallocz(ctx, sizeof(m->export_entries[0]) * m->export_entries_size);
        if (!m->export_entries)
            goto fail;
        for(i = 0; i < m->export_entries_count; i++) {
            JSExportEntry *me = &m->export_entries[i];
            if (bc_get_u8(s, &v8))
                goto fail;
            me->export_type = v8;
            if (me->export_type == JS_EXPORT_TYPE_LOCAL) {
                if (bc_get_leb128_int(s, &me->u.local.var_idx))
                    goto fail;
            } else {
                if (bc_get_leb128_int(s, &me->u.req_module_idx))
                    goto fail;
                if (bc_get_atom(s, &me->local_name))
                    goto fail;
                if (bc_get_u8(s, &v8))
                    goto fail;
                me->is_namespace = (v8 != 0);
            }
            if (bc_get_atom(s, &me->export_name))
                goto fail;
        }
    }

    if (bc_get_leb128_int(s, &m->star_export_entries_count))
        goto fail;
    if (m->star_export_entries_count != 0) {
        m->star_export_entries_size = m->star_export_entries_count;
        m->star_export_entries = js_mallocz(ctx, sizeof(m->star_export_entries[0]) * m->star_export_entries_size);
        if (!m->star_export_entries)
            goto fail;
        for(i = 0; i < m->star_export_entries_count; i++) {
            JSStarExportEntry *se = &m->star_export_entries[i];
            if (bc_get_leb128_int(s, &se->req_module_idx))
                goto fail;
        }
    }

    if (bc_get_leb128_int(s, &m->import_entries_count))
        goto fail;
    if (m->import_entries_count != 0) {
        m->import_entries_size = m->import_entries_count;
        m->import_entries = js_mallocz(ctx, sizeof(m->import_entries[0]) * m->import_entries_size);
        if (!m->import_entries)
            goto fail;
        for(i = 0; i < m->import_entries_count; i++) {
            JSImportEntry *mi = &m->import_entries[i];
            if (bc_get_leb128_int(s, &mi->var_idx))
                goto fail;
            if (bc_get_atom(s, &mi->import_name))
                goto fail;
            if (bc_get_u8(s, &v8))
                goto fail;
            mi->is_namespace = (v8 != 0);
            if (bc_get_leb128_int(s, &mi->req_module_idx))
                goto fail;
        }
    }

    if (bc_get_u8(s, &v8))
        goto fail;
    m->has_tla = (v8 != 0);

    /* func_obj is the last field and it is a whole value, so it is a FRAME: the module object is already
       built and reference-registered, and the driver drops the child straight into m->func_obj. */
    if (bcr_push(s) < 0)
        goto fail;
    {
        BCRFrame *f = &s->r_stack[s->r_sp - 1];
        f->kind = BCR_MODULE;
        f->obj = obj;
        f->p1 = m;
    }
    return JS_UNDEFINED;
 fail:
    if (m) {
        js_free_module_def(ctx, m);
    }
    return JS_EXCEPTION;
}

static JSValue JS_ReadObjectTag(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    JSValue obj;
    uint32_t prop_count;

    obj = JS_NewObject(ctx);
    if (BC_add_object_ref(s, obj))
        goto fail;
    if (bc_get_leb128(s, &prop_count))
        goto fail;
    /* The properties are a FRAME: each value is a whole nested read, and the key must be held across it. */
    if (bcr_push(s) < 0)
        goto fail;
    {
        BCRFrame *f = &s->r_stack[s->r_sp - 1];
        f->kind = BCR_OBJ;
        f->obj = obj;
        f->len = prop_count;
        f->atom = JS_ATOM_NULL;
    }
    return JS_UNDEFINED;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadTypedArray(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    uint8_t array_tag;
    uint32_t offset, len, idx;

    if (bc_get_u8(s, &array_tag))
        return JS_EXCEPTION;
    /* A DATAVIEW rides the same tag, one past the typed arrays. HTML 2.7's StructuredSerialize puts both in
       its ArrayBufferView branch — a DataView is a view over a buffer exactly as a Uint8Array is, and the only
       difference on the wire is which constructor rebuilds it. Refusing it here is what made
       `structuredClone(new DataView(b))` a DataCloneError and what made Fetch's `response.clone()` unable to
       give its second branch a DataView chunk. */
    if (array_tag > JS_CLASS_DATAVIEW - JS_CLASS_UINT8C_ARRAY)
        return JS_ThrowTypeError(ctx, "invalid typed array");
    if (bc_get_leb128(s, &len))
        return JS_EXCEPTION;
    if (bc_get_leb128(s, &offset))
        return JS_EXCEPTION;
    /* The reference slot is RESERVED before the buffer is read and filled in after the view exists, which is
       the same hack the recursive arm used and the same order the ids must be assigned in. */
    idx = s->objects_count;
    if (BC_add_object_ref1(s, NULL))
        return JS_EXCEPTION;
    if (bcr_push(s) < 0)
        return JS_EXCEPTION;
    {
        BCRFrame *f = &s->r_stack[s->r_sp - 1];
        f->kind = BCR_TA;
        f->u1 = array_tag;
        f->u2 = len;
        f->u3 = offset;
        f->i = idx;
    }
    return JS_UNDEFINED;
}

static const char *js_ab_codec_intern(JSRuntime *rt, const uint8_t *name, uint32_t len)
{
    int i;
    char *c;

    for (i = 0; i < rt->ab_codec_n; i++)
        if (strlen(rt->ab_codec[i]) == len && memcmp(rt->ab_codec[i], name, len) == 0)
            return rt->ab_codec[i];
    if (rt->ab_codec_n == rt->ab_codec_size) {
        int ns = rt->ab_codec_size ? rt->ab_codec_size * 2 : 4;
        char **nv = js_realloc_rt(rt, rt->ab_codec, sizeof(*nv) * (size_t)ns);

        if (!nv)
            return NULL;
        rt->ab_codec = nv;
        rt->ab_codec_size = ns;
    }
    c = js_malloc_rt(rt, (size_t)len + 1);
    if (!c)
        return NULL;
    memcpy(c, name, len);
    c[len] = '\0';
    rt->ab_codec[rt->ab_codec_n++] = c;
    return c;
}

static JSValue JS_ReadArrayBuffer(BCReaderState *s);

/* BC_TAG_ARRAY_BUFFER_SPANS: the buffer as BC_TAG_ARRAY_BUFFER reads it, then its span section. Each span's
   values are owed by its sub-values: an owed field holds JS_UNDEFINED, which no span field otherwise holds, and
   a BCR_ABSPAN frame fills them in section order (bcr_abspan_take). */
static JSValue JS_ReadArrayBufferSpans(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    JSValue obj = JS_ReadArrayBuffer(s);
    JSArrayBuffer *abuf;
    uint8_t fmt, has_len, kind, mask;
    uint32_t n, i, owed = 0, off, ext, pos, cl;
    BCRFrame *f;

    if (JS_IsException(obj))
        return obj;
    abuf = JS_VALUE_GET_OBJ(obj)->u.array_buffer;
    if (bc_get_u8(s, &fmt) || bc_get_u8(s, &has_len) || bc_get_leb128(s, &n))
        goto fail;
    if (fmt != BC_AB_SPAN_FORMAT || has_len > 1) {
        JS_ThrowSyntaxError(ctx, "invalid array buffer span section (format %u, expected %u)", fmt,
                            BC_AB_SPAN_FORMAT);
        goto fail;
    }
    for (i = 0; i < n; i++) {
        JSArrayBufferSpan *sp;

        if (bc_get_u8(s, &kind) || bc_get_leb128(s, &off) || bc_get_leb128(s, &ext) || bc_get_u8(s, &mask))
            goto fail;
        if (kind > JS_AB_SPAN_IMAGE || mask > 7 || ext == 0 || (uint64_t)off + ext > (uint64_t)abuf->byte_length ||
            (kind == JS_AB_SPAN_AT && ext > 8)) {
            JS_ThrowSyntaxError(ctx, "invalid array buffer span");
            goto fail;
        }
        sp = js_ab_span_push(ctx->rt, abuf);
        memset(sp, 0, sizeof *sp);
        sp->kind = kind;
        sp->offset = off;
        sp->extent = ext;
        sp->unknown = (mask & BC_AB_SPAN_HAS_UNKNOWN) ? JS_UNDEFINED : JS_UNINITIALIZED;
        sp->index = (mask & BC_AB_SPAN_HAS_INDEX) ? JS_UNDEFINED : JS_UNINITIALIZED;
        sp->image = (mask & BC_AB_SPAN_HAS_IMAGE) ? JS_UNDEFINED : JS_UNINITIALIZED;
        owed += !!(mask & 1) + !!(mask & 2) + !!(mask & 4);
        if (kind == JS_AB_SPAN_AT) {
            if (s->buf_end - s->ptr < (ptrdiff_t)sizeof sp->witness) {
                bc_read_error_end(s);
                goto fail;
            }
            memcpy(sp->witness, s->ptr, sizeof sp->witness);
            s->ptr += sizeof sp->witness;
        }
        if (kind == JS_AB_SPAN_IMAGE) {
            if (bc_get_leb128(s, &pos) || bc_get_leb128(s, &cl))
                goto fail;
            if (s->buf_end - s->ptr < (ptrdiff_t)cl) {
                bc_read_error_end(s);
                goto fail;
            }
            sp->image_pos = pos;
            sp->codec = js_ab_codec_intern(ctx->rt, s->ptr, cl);
            if (!sp->codec) {
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
            s->ptr += cl;
        }
    }
    if (has_len) {
        abuf->length_unknown = JS_UNDEFINED;
        owed++;
    }
    if (owed == 0)
        return obj;
    if (bcr_push(s) < 0)
        goto fail;
    f = &s->r_stack[s->r_sp - 1];
    f->kind = BCR_ABSPAN;
    f->obj = obj;
    f->i = 0;
    f->len = owed;
    return JS_UNDEFINED;   /* the driver sees the pushed frame */
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* One sub-value of a BC_TAG_ARRAY_BUFFER_SPANS record, into the first owed field in section order. A value
   the read resolved to something not unknown is a determination the receiving flow holds (HTML §2.7's symbol
   rebuild answers a pinned source plainly), so the field is left unset and bcr_abspan_settle drops a span that
   no longer states anything. Takes `v`. */
static int bcr_abspan_take(JSContext *ctx, JSArrayBuffer *abuf, JSValue v)
{
    int i;

    for (i = 0; i < abuf->n_span; i++) {
        JSArrayBufferSpan *sp = &abuf->span[i];
        JSValue *slot = JS_IsUndefined(sp->unknown) ? &sp->unknown
                      : JS_IsUndefined(sp->index) ? &sp->index
                      : JS_IsUndefined(sp->image) ? &sp->image : NULL;

        if (!slot)
            continue;
        if (slot == &sp->image) {
            if (!JS_IsString(v) || JS_VALUE_GET_STRING(v)->is_wide_char ||
                JS_VALUE_GET_STRING(v)->len != sp->extent) {
                JS_FreeValue(ctx, v);
                JS_ThrowSyntaxError(ctx, "invalid array buffer span image");
                return -1;
            }
            *slot = v;
            return 0;
        }
        if (js_value_is_concolic(v)) {
            *slot = v;
        } else {
            *slot = JS_UNINITIALIZED;
            JS_FreeValue(ctx, v);
        }
        return 0;
    }
    DCHECK(JS_IsUndefined(abuf->length_unknown),
           "a span section's sub-value arrived with no field owing it — the frame's count is the owed fields'");
    if (js_value_is_concolic(v)) {
        abuf->length_unknown = v;
    } else {
        abuf->length_unknown = JS_UNINITIALIZED;
        JS_FreeValue(ctx, v);
    }
    return 0;
}

/* Every owed field is filled: keep the spans that still state an unknown. AT and IMAGE describe their
   `unknown`; ANY derives a read from its `index`, so it needs that. */
static void bcr_abspan_settle(JSRuntime *rt, JSArrayBuffer *abuf)
{
    int i, j = 0;

    for (i = 0; i < abuf->n_span; i++) {
        JSArrayBufferSpan *sp = &abuf->span[i];
        bool keep = sp->kind == JS_AB_SPAN_ANY ? !JS_IsUninitialized(sp->index)
                                               : !JS_IsUninitialized(sp->unknown);

        if (sp->kind == JS_AB_SPAN_IMAGE)
            keep = keep && !JS_IsUninitialized(sp->image);
        if (!keep) {
            js_ab_span_free_values(rt, sp);
            continue;
        }
        abuf->span[j++] = *sp;
    }
    abuf->n_span = j;
}

static JSValue JS_ReadArrayBuffer(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    uint32_t byte_length, max_byte_length;
    uint64_t max_byte_length_u64, *pmax_byte_length = NULL;
    JSValue obj;

    if (bc_get_leb128(s, &byte_length))
        return JS_EXCEPTION;
    if (bc_get_leb128(s, &max_byte_length))
        return JS_EXCEPTION;
    if (max_byte_length < byte_length)
        return JS_ThrowTypeError(ctx, "invalid array buffer");
    if (max_byte_length != UINT32_MAX) {
        max_byte_length_u64 = max_byte_length;
        pmax_byte_length = &max_byte_length_u64;
    }
    if (unlikely(s->buf_end - s->ptr < byte_length)) {
        bc_read_error_end(s);
        return JS_EXCEPTION;
    }
    // makes a copy of the input
    obj = js_array_buffer_constructor3(ctx,
                                       byte_length, pmax_byte_length,
                                       JS_CLASS_ARRAY_BUFFER,
                                       (uint8_t*)s->ptr,
                                       js_array_buffer_free, NULL,
                                       /*alloc_flag*/true);
    if (JS_IsException(obj))
        goto fail;
    if (BC_add_object_ref(s, obj))
        goto fail;
    s->ptr += byte_length;
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadRegExp(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    JSString *pattern;
    JSString *bc;

    pattern = JS_ReadString(s);
    if (!pattern)
        return JS_EXCEPTION;

    bc = JS_ReadString(s);
    if (!bc) {
        js_free_string(ctx->rt, pattern);
        return JS_EXCEPTION;
    }

    if (bc->is_wide_char ||
        lre_check_bytecode(str8(bc), bc->len) != 0) {
        js_free_string(ctx->rt, pattern);
        js_free_string(ctx->rt, bc);
        return JS_ThrowInternalError(ctx, "bad regexp bytecode");
    }

    return js_regexp_constructor_internal(ctx, JS_UNDEFINED,
                                          JS_MKPTR(JS_TAG_STRING, pattern),
                                          JS_MKPTR(JS_TAG_STRING, bc));
}

static JSValue JS_ReadDate(BCReaderState *s)
{
    /* The child comes FIRST here — the recursive arm read the number before creating the Date, so the object
       reference ids are assigned in that order and the frame must keep it. */
    if (bcr_push(s) < 0)
        return JS_EXCEPTION;
    s->r_stack[s->r_sp - 1].kind = BCR_DATE;
    return JS_UNDEFINED;
}

static JSValue JS_ReadObjectValue(BCReaderState *s)
{
    if (bcr_push(s) < 0)
        return JS_EXCEPTION;
    s->r_stack[s->r_sp - 1].kind = BCR_OBJVAL;
    return JS_UNDEFINED;
}

/* HTML §2.7.6 STEPS 22 AND 23 — and the SPLIT between them and step 24 is the whole of why this is a frame.
   Step 22 creates the instance, step 23 is "Set memory[serialized] to value", and only step 24 reads what the
   serialization steps wrote. So the object is in the reference list BEFORE its sub-value, which is what makes
   a graph reaching one platform object twice come back as ONE object — and it is NOT BC_TAG_TYPED_ARRAY's
   reserve-and-patch, which exists because a typed array cannot be built before its buffer. HTML §2.7.1 says
   of a serializable that value "will be a newly-created instance of the platform object type in question,
   with none of its internal data set up; setting that up is the job of these steps", so it can. */
static JSValue JS_ReadSerializable(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    char name[BC_SERIALIZABLE_NAME_MAX + 1];
    uint32_t len;
    JSValue obj;

    /* THE BYTES ARE INPUT AND THIS IS A REFUSAL, NOT AN ASSERT. A record read back in a later turn — a
       history entry, a queued delivery, the cold tier — is not this activation's own value, and a read given
       no seam has no meaning to give the tag. */
    if (!s->serializable)
        return JS_ThrowSyntaxError(ctx, "a serialized platform object is named by a stream read with no "
                                        "serializable seam to create one");
    if (bc_get_leb128(s, &len))
        return JS_EXCEPTION;
    if (len == 0 || len > BC_SERIALIZABLE_NAME_MAX)
        return JS_ThrowSyntaxError(ctx, "a serialized platform object names an interface identifier of %u "
                                        "bytes, which no Web IDL interface has", len);
    if (bc_get_buf(s, name, len))
        return JS_EXCEPTION;
    name[len] = '\0';
    /* STEP 22, including its own "If the interface identified by interfaceName is not exposed in targetRealm,
       then throw a \"DataCloneError\" DOMException" — which is the seam's answer and not this reader's,
       because exposure is a fact about a realm that only the host holds. */
    obj = s->serializable->create(ctx, s->serializable->opaque, name);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    /* STEP 23, BEFORE the sub-value is read. */
    if (BC_add_object_ref(s, obj))
        goto fail;
    if (bcr_push(s) < 0)
        goto fail;
    {
        BCRFrame *f = &s->r_stack[s->r_sp - 1];
        f->kind = BCR_SERIALIZABLE;
        f->obj = obj;
    }
    return JS_UNDEFINED;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadMap(BCReaderState *s);
static JSValue JS_ReadSet(BCReaderState *s);


/* READ ONE VALUE. Returns it, or — for a tag whose arm is on the frame stack — PUSHES a frame and returns
   JS_UNDEFINED with the stack grown, which the driver detects and drives.
   NO STACK GUARD. Every arm that used to re-enter this function now pushes a frame instead, so the C depth
   here is a constant the serialised graph cannot pick. The guard that stood in front of the recursion turned
   a graph the format can represent into a RangeError: measured, a nested array round-trip threw between depth
   3000 and 5000, and it reaches 200000 now. Deleting it is what makes the flat-stack claim falsifiable. */
static JSValue bcr_read_one(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    uint8_t tag;
    JSValue obj = JS_UNDEFINED;

    if (bc_get_u8(s, &tag))
        return JS_EXCEPTION;

    bc_read_trace(s, "%s {\n", bc_tag_name(tag));

    switch(tag) {
    case BC_TAG_NULL:
        obj = JS_NULL;
        break;
    case BC_TAG_UNDEFINED:
        obj = JS_UNDEFINED;
        break;
    case BC_TAG_BOOL_FALSE:
    case BC_TAG_BOOL_TRUE:
        obj = js_bool(tag - BC_TAG_BOOL_FALSE);
        break;
    case BC_TAG_INT32:
        {
            int32_t val;
            if (bc_get_sleb128(s, &val))
                return JS_EXCEPTION;
            bc_read_trace(s, "%d\n", val);
            obj = js_int32(val);
        }
        break;
    case BC_TAG_FLOAT64:
        {
            JSFloat64Union u;
            if (bc_get_u64(s, &u.u64))
                return JS_EXCEPTION;
            bc_read_trace(s, "%g\n", u.d);
            obj = js_float64(u.d);
        }
        break;
    case BC_TAG_STRING:
        {
            JSString *p;
            p = JS_ReadString(s);
            if (!p)
                return JS_EXCEPTION;
            obj = JS_MKPTR(JS_TAG_STRING, p);
        }
        break;
    case BC_TAG_FUNCTION_BYTECODE:
        if (!s->allow_bytecode)
            goto no_allow_bytecode;
        obj = JS_ReadFunctionTag(s);
        break;
    case BC_TAG_MODULE:
        if (!s->allow_bytecode) {
        no_allow_bytecode:
            return JS_ThrowSyntaxError(ctx, "no bytecode allowed");
        }
        obj = JS_ReadModule(s);
        break;
    case BC_TAG_OBJECT:
        obj = JS_ReadObjectTag(s);
        break;
    case BC_TAG_ARRAY:
    case BC_TAG_TEMPLATE_OBJECT:
        /* ON THE FRAME STACK. The container and the reference id are established in the same order the
           recursive arm did — JS_NewArray then BC_add_object_ref then the length — because the reference
           index a later BC_TAG_OBJECT_REFERENCE resolves against is assigned by that order. */
        {
            BCRFrame *f;
            uint32_t len;
            obj = JS_NewArray(ctx);
            if (JS_IsException(obj))
                return JS_EXCEPTION;
            if (BC_add_object_ref(s, obj))
                goto arr_fail;
            if (bc_get_leb128(s, &len))
                goto arr_fail;
            bc_read_trace(s, "%u\n", len);
            if (bcr_push(s) < 0)
                goto arr_fail;
            f = &s->r_stack[s->r_sp - 1];
            f->kind = BCR_ARRAY;
            f->phase = 0;
            f->obj = obj;
            f->i = 0;
            f->len = len;
            f->flag = (tag == BC_TAG_TEMPLATE_OBJECT);
            return JS_UNDEFINED;   /* the driver sees the pushed frame */
        arr_fail:
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
    case BC_TAG_TYPED_ARRAY:
        obj = JS_ReadTypedArray(s);
        break;
    case BC_TAG_ARRAY_BUFFER:
        obj = JS_ReadArrayBuffer(s);
        break;
    case BC_TAG_ARRAY_BUFFER_SPANS:
        obj = JS_ReadArrayBufferSpans(s);
        break;
    case BC_TAG_REGEXP:
        obj = JS_ReadRegExp(s);
        break;
    case BC_TAG_DATE:
        obj = JS_ReadDate(s);
        break;
    case BC_TAG_OBJECT_VALUE:
        obj = JS_ReadObjectValue(s);
        break;
    case BC_TAG_BIG_INT:
        obj = JS_ReadBigInt(s);
        break;
    case BC_TAG_OBJECT_REFERENCE:
        {
            uint32_t val;
            if (!s->allow_reference)
                return JS_ThrowSyntaxError(ctx, "object references are not allowed");
            if (bc_get_leb128(s, &val))
                return JS_EXCEPTION;
            bc_read_trace(s, "%u\n", val);
            if (val >= s->objects_count || !s->objects[val]) {
                return JS_ThrowSyntaxError(ctx, "invalid object reference (%u >= %u)",
                                           val, s->objects_count);
            }
            obj = js_dup(JS_MKPTR(JS_TAG_OBJECT, s->objects[val]));
        }
        break;
    case BC_TAG_TRANSFER_REFERENCE:
        {
            uint32_t val;
            /* HTML §2.7.2 STEP 2 against §2.7.8's seeded `memory`. The answer is the object the receiving steps
               ALREADY built and appended to [[TransferredValues]] — the same object, which is what makes a
               moved port inside a message the port the receiver also gets in `ports`. */
            if (!s->transfer)
                return JS_ThrowSyntaxError(ctx, "a transferred value is named by a stream read with no "
                                                "transfer map to resolve it against");
            if (bc_get_leb128(s, &val))
                return JS_EXCEPTION;
            bc_read_trace(s, "%u\n", val);
            if (val >= s->transfer->count) {
                return JS_ThrowSyntaxError(ctx, "invalid transfer reference (%u >= %u)",
                                           val, s->transfer->count);
            }
            obj = s->transfer->value_at(ctx, s->transfer->opaque, val);
        }
        break;
    case BC_TAG_SERIALIZABLE:
        obj = JS_ReadSerializable(s);
        break;
    case BC_TAG_MAP:
        obj = JS_ReadMap(s);
        break;
    case BC_TAG_SET:
        obj = JS_ReadSet(s);
        break;
    case BC_TAG_SYMBOL:
        {
            JSAtom atom;
            /* THE WRITER'S REFUSAL, ASSERTED FROM THE OTHER SIDE. The arm above no longer emits this tag on a
               non-bytecode write, so a stream that carries one did not come from this engine's serializer —
               and core/structured_clone.c's bytes are read back from a LATER turn and, for a routed message,
               from ANOTHER INSTANCE, which SECURITY.md grades as attacker-controlled. A reader that accepts
               what no writer produces is a hole, so the two sides are made to agree by construction rather
               than by the writer being the only producer anybody happens to have. This is a REFUSAL and not a
               DCHECK: the bytes crossed a boundary, so they are input. */
            if (!s->allow_bytecode)
                return JS_ThrowSyntaxError(ctx, "a Symbol is named by a stream that no non-bytecode write "
                                                "produces");
            if (bc_get_atom(s, &atom))
                return JS_EXCEPTION;
            if (__JS_AtomIsConst(atom)) {
                obj = JS_AtomToValue(s->ctx, atom);
            } else {
                JSAtomStruct *p = s->ctx->rt->atom_array[atom];
                obj = JS_NewSymbolFromAtom(s->ctx, atom, p->atom_type);
            }
        }
        break;
    default:
    invalid_tag:
        return JS_ThrowSyntaxError(ctx, "invalid tag (tag=%d pos=%u)",
                                   tag, (unsigned int)(s->ptr - s->buf_start));
    }
    bc_read_trace(s, "}\n");
    return obj;
}

/* THE DRIVER. One value at a time: read, deliver to the frame waiting for it, advance. Everything live at a
   frame boundary is in BCReaderState, so this loop can be left and re-entered — which is what makes the walk
   suspendable rather than merely flat. r_base is this activation's floor: an arm that still calls
   JS_ReadObjectRec re-enters here and gets its own floor, exactly as the parser worked mid-conversion. */
static JSValue JS_ReadObjectRec(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    int base = s->r_sp;
    int saved_base = s->r_base;
    JSValue result = JS_UNDEFINED;
    bool need_read = true;

    s->r_base = base;
    for (;;) {
        if (need_read) {
            int before = s->r_sp;
            result = bcr_read_one(s);
            if (JS_IsException(result))
                goto fail;
            need_read = false;
            if (s->r_sp > before)
                result = JS_UNDEFINED;   /* a frame was pushed; it owns what comes next */
        }
        if (s->r_sp == base) {
            s->r_base = saved_base;
            return result;
        }
        {
            BCRFrame *f = &s->r_stack[s->r_sp - 1];
            if (f->phase == 1) {
                /* a child just arrived: install it */
                if (f->kind == BCR_OBJ) {
                    int r = JS_DefinePropertyValue(ctx, f->obj, f->atom, result, JS_PROP_C_W_E);
                    JS_FreeAtom(ctx, f->atom);
                    f->atom = JS_ATOM_NULL;
                    result = JS_UNDEFINED;
                    if (r < 0)
                        goto fail;
                    f->i++;
                    f->phase = 0;
                    continue;
                }
                if (f->kind == BCR_MAP) {
                    JSValue rv;
                    if (!f->flag && f->phase2_pending == 0) {
                        /* the key of a map: hold it and come back for the value */
                        f->key = result;
                        result = JS_UNDEFINED;
                        f->phase2_pending = 1;
                        f->phase = 0;
                        continue;
                    }
                    {
                        JSValue argv[2];
                        argv[0] = f->flag ? result : f->key;
                        argv[1] = f->flag ? JS_UNDEFINED : result;
                        rv = js_map_set(ctx, f->obj, 2, vc(argv), f->u1);
                        JS_FreeValue(ctx, argv[0]);
                        JS_FreeValue(ctx, argv[1]);
                        f->key = JS_UNDEFINED;
                        result = JS_UNDEFINED;
                        f->phase2_pending = 0;
                        if (JS_IsException(rv))
                            goto fail;
                        JS_FreeValue(ctx, rv);
                    }
                    f->i++;
                    f->phase = 0;
                    continue;
                }
                if (f->kind == BCR_DATE) {
                    JSValue d;
                    if (!JS_IsNumber(result)) {
                        JS_ThrowTypeError(ctx, "Number tag expected for date");
                        goto fail;
                    }
                    d = JS_NewObjectProtoClass(ctx, ctx->class_proto[JS_CLASS_DATE], JS_CLASS_DATE);
                    if (JS_IsException(d))
                        goto fail;
                    if (BC_add_object_ref(s, d)) {
                        JS_FreeValue(ctx, d);
                        goto fail;
                    }
                    JS_SetObjectData(ctx, d, result);   /* takes the number */
                    result = d;
                    s->r_sp--;
                    continue;
                }
                if (f->kind == BCR_OBJVAL) {
                    JSValue o = JS_ToObject(ctx, result);
                    if (JS_IsException(o))
                        goto fail;
                    if (BC_add_object_ref(s, o)) {
                        JS_FreeValue(ctx, o);
                        goto fail;
                    }
                    JS_FreeValue(ctx, result);
                    result = o;
                    s->r_sp--;
                    continue;
                }
                if (f->kind == BCR_TA) {
                    JSValueConst args[3];
                    JSValue o;
                    int cid = JS_CLASS_UINT8C_ARRAY + f->u1;
                    if (!js_get_array_buffer(ctx, result))
                        goto fail;
                    args[0] = result;
                    args[1] = js_int64(f->u3);
                    args[2] = js_int64(f->u2);
                    /* A DataView's length is BYTES and a typed array's is ELEMENTS, which is why the writer
                       stores `u.array.count` for one and the byte length for the other and why the two
                       constructors are not interchangeable. */
                    o = cid == JS_CLASS_DATAVIEW
                      ? js_dataview_new(ctx, result, f->u3, f->u2)
                      : js_typed_array_constructor(ctx, JS_UNDEFINED, 3, args, cid);
                    if (JS_IsException(o))
                        goto fail;
                    if (s->allow_reference)
                        s->objects[f->i] = JS_VALUE_GET_OBJ(o);
                    JS_FreeValue(ctx, result);
                    result = o;
                    s->r_sp--;
                    continue;
                }
                if (f->kind == BCR_SERIALIZABLE) {
                    /* HTML §2.7.6 STEP 24's LAST ARM: "Perform the appropriate deserialization steps for the
                       interface identified by serialized.[[Type]], given serialized, value, and targetRealm."
                       `result` is the sub-deserialization of the one value the serialization steps wrote —
                       "a specialization of StructuredDeserialize to be consistent within this invocation",
                       which is what reading it through this same driver under this same object list is. */
                    int r;

                    DCHECK(s->serializable != NULL,
                           "a serializable frame is being filled by a read that has no seam — the frame is "
                           "pushed only by the arm that already refused the tag without one, so the read's "
                           "hooks changed under a graph it was half way through");
                    r = s->serializable->deserialize(ctx, s->serializable->opaque, f->obj, result);
                    JS_FreeValue(ctx, result);
                    result = f->obj;
                    f->obj = JS_UNDEFINED;
                    s->r_sp--;
                    if (r < 0)
                        goto fail;
                    continue;
                }
                if (f->kind == BCR_ABSPAN) {
                    int r = bcr_abspan_take(ctx, JS_VALUE_GET_OBJ(f->obj)->u.array_buffer, result);

                    result = JS_UNDEFINED;
                    if (r < 0)
                        goto fail;
                    f->i++;
                    f->phase = 0;
                    continue;
                }
                if (f->kind == BCR_MODULE) {
                    JSModuleDef *m = f->p1;
                    m->func_obj = result;
                    result = f->obj;
                    f->obj = JS_UNDEFINED;
                    s->r_sp--;
                    continue;
                }
                if (f->kind == BCR_FUNC) {
                    JSFunctionBytecode *b = f->p1;
                    b->cpool[f->i] = result;
                    result = JS_UNDEFINED;
                    f->i++;
                    f->phase = 0;
                    continue;
                }
                if (f->kind == BCR_ARRAY) {
                    int pf = f->flag ? JS_PROP_ENUMERABLE : JS_PROP_C_W_E;
                    if (f->i < f->len) {
                        if (JS_DefinePropertyValueUint32(ctx, f->obj, f->i, result, pf) < 0) {
                            result = JS_UNDEFINED;
                            goto fail;
                        }
                        f->i++;
                    } else {
                        /* the template's `raw` */
                        if (!JS_IsUndefined(result)) {
                            if (JS_DefinePropertyValue(ctx, f->obj, JS_ATOM_raw, result, 0) < 0) {
                                result = JS_UNDEFINED;
                                goto fail;
                            }
                        } else {
                            JS_FreeValue(ctx, result);
                        }
                        JS_PreventExtensions(ctx, f->obj);
                        f->flag = false;   /* raw consumed */
                        f->len = f->i;     /* and no elements remain */
                        result = f->obj;
                        f->obj = JS_UNDEFINED;
                        s->r_sp--;
                        continue;
                    }
                }
                result = JS_UNDEFINED;
                f->phase = 0;
                continue;
            }
            /* phase 0: does this frame still want a child? */
            if (f->kind == BCR_OBJ) {
                if (f->i < f->len) {
                    if (bc_get_atom(s, &f->atom))
                        goto fail;
                    f->phase = 1;
                    need_read = true;
                    continue;
                }
            } else if (f->kind == BCR_MAP) {
                if (f->i < f->len || f->phase2_pending) {
                    f->phase = 1;
                    need_read = true;
                    continue;
                }
            } else if (f->kind == BCR_DATE || f->kind == BCR_OBJVAL ||
                       f->kind == BCR_TA || f->kind == BCR_MODULE ||
                       f->kind == BCR_SERIALIZABLE) {
                f->phase = 1;
                need_read = true;
                continue;
            } else if (f->kind == BCR_ABSPAN) {
                if (f->i < f->len) {
                    f->phase = 1;
                    need_read = true;
                    continue;
                }
                bcr_abspan_settle(ctx->rt, JS_VALUE_GET_OBJ(f->obj)->u.array_buffer);
            } else if (f->kind == BCR_FUNC) {
                if (f->i < f->len) {
                    f->phase = 1;
                    need_read = true;
                    continue;
                }
                result = bcr_func_tail(s, f);
                f->obj = JS_UNDEFINED;
                s->r_sp--;
                if (JS_IsException(result))
                    goto fail;
                continue;
            } else if (f->i < f->len || f->flag) {
                f->phase = 1;
                need_read = true;
                continue;
            }
            result = f->obj;
            f->obj = JS_UNDEFINED;
            s->r_sp--;
        }
    }
 fail:
    JS_FreeValue(ctx, result);
    while (s->r_sp > base) {
        s->r_sp--;
        JS_FreeValue(ctx, s->r_stack[s->r_sp].obj);
        JS_FreeValue(ctx, s->r_stack[s->r_sp].key);
        JS_FreeAtom(ctx, s->r_stack[s->r_sp].atom);
        s->r_stack[s->r_sp].obj = JS_UNDEFINED;
        s->r_stack[s->r_sp].key = JS_UNDEFINED;
        s->r_stack[s->r_sp].atom = JS_ATOM_NULL;
    }
    s->r_base = saved_base;
    return JS_EXCEPTION;
}

static int JS_ReadObjectAtoms(BCReaderState *s)
{
    uint8_t v8, type;
    JSString *p;
    uint32_t h;
    int i;
    JSAtom atom;

    if (bc_get_u8(s, &v8))
        return -1;
    if (v8 != BC_VERSION) {
        JS_ThrowSyntaxError(s->ctx, "invalid version (%d expected=%d)",
                            v8, BC_VERSION);
        return -1;
    }
    if (bc_get_u32(s, &h))
        return -1;
    // allow UINT32_MAX as an escape hatch, otherwise updating
    // the test corpus in tests/test_bjson.js gets too tedious
    if (h != UINT32_MAX && h != bc_csum(s->ptr, s->buf_end - s->ptr)) {
        JS_ThrowSyntaxError(s->ctx, "checksum error");
        return -1;
    }
    if (bc_get_leb128(s, &s->idx_to_atom_count))
        return -1;
    if (s->idx_to_atom_count > 1000*1000) {
        JS_ThrowInternalError(s->ctx, "unreasonable atom count: %u",
                              s->idx_to_atom_count);
        return -1;
    }

    bc_read_trace(s, "%u atom indexes {\n", s->idx_to_atom_count);

    if (s->idx_to_atom_count != 0) {
        s->idx_to_atom = js_mallocz(s->ctx, s->idx_to_atom_count *
                                    sizeof(s->idx_to_atom[0]));
        if (!s->idx_to_atom)
            return s->error_state = -1;
    }
    for(i = 0; i < s->idx_to_atom_count; i++) {
        if (bc_get_u8(s, &type)) {
            return -1;
        }
        if (type == 0) {
            if (bc_get_u32(s, &atom))
                return -1;
            if (!__JS_AtomIsConst(atom)) {
                JS_ThrowInternalError(s->ctx, "out of range atom");
                return -1;
            }
        } else {
            if (type < JS_ATOM_TYPE_STRING || type >= JS_ATOM_TYPE_PRIVATE) {
                JS_ThrowInternalError(s->ctx, "invalid symbol type %d", type);
                return -1;
            }
            p = JS_ReadString(s);
            if (!p)
                return -1;
            atom = __JS_NewAtom(s->ctx->rt, p, type);
        }
        if (atom == JS_ATOM_NULL)
            return s->error_state = -1;
        s->idx_to_atom[i] = atom;
    }
    bc_read_trace(s, "}\n");
    return 0;
}

/* THE READER'S OWNED MEMORY. `r_stack` was missing here for exactly the reason `w_stack` was missing from the
   writer's two tails — the frame stack is what this fork added when the reader stopped recursing, and this
   list predates it — and it is repaired in the same diff rather than left for a later run to surface, because
   a leak is found by reading the ownership contract at the site that breaks it and the reading that finds one
   of these finds both. NOTHING HAD EVER REPORTED THIS ONE: the runtime's own leak gate walks gc_obj_list, and
   a js_malloc block is not a GC object, so no gate in this tree can see either of them.
   RETIREMENT: this record goes when a raw js_malloc block surviving a runtime teardown is itself a verdict.
   JS_FreeRuntime's `malloc_count` arm is the one that could say so and it is still behind ENABLE_DUMPS and a
   JS_DUMP_LEAKS flag — which is the shape JS_FreeRuntime's own `[atomleak]` census was repaired out of, in a
   paragraph that states the argument against it verbatim: a leak report that exists only in some builds
   reports only some leaks. */
static void bc_reader_free(BCReaderState *s)
{
    int i;

    /* DRAINED ON BOTH PATHS, as the writer's is: the driver returns only where `s->r_sp == base` and its
       unwind runs down to that same `base`, 0 at the outermost call. A surviving frame holds an owned `obj`,
       an owned `key` and an owned `atom` — three losses per frame, larger than the array freed below. */
    DCHECK(s->r_sp == 0,
           "the bytecode reader reached its teardown with frames still on its stack — an arm returned without "
           "running the unwind, so each surviving frame's obj, key and atom are lost");
    if (s->idx_to_atom) {
        for(i = 0; i < s->idx_to_atom_count; i++) {
            JS_FreeAtom(s->ctx, s->idx_to_atom[i]);
        }
        js_free(s->ctx, s->idx_to_atom);
    }
    js_free(s->ctx, s->objects);
    js_free(s->ctx, s->r_stack);
    s->idx_to_atom = NULL;
    s->idx_to_atom_count = 0;
    s->objects = NULL;
    s->objects_count = 0;
    s->objects_size = 0;
    s->r_stack = NULL;
    s->r_size = 0;
}

JSValue JS_ReadObject4(JSContext *ctx, const uint8_t *buf, size_t buf_len,
                       int flags,
                       const JSTransferReadHook *transfer,
                       const JSSerializableReadHook *serializable)
{
    BCReaderState ss, *s = &ss;
    JSValue obj;

    ctx->binary_object_count += 1;
    ctx->binary_object_size += buf_len;

    DCHECK(!transfer || transfer->value_at != NULL,
           "a transfer map was supplied to the reader without the function that answers it — a map that cannot "
           "be asked would let the tag through and then have nothing to resolve it to");
    DCHECK(!serializable || (serializable->create != NULL && serializable->deserialize != NULL),
           "HTML §2.7.1's serializable seam was supplied to the reader without both of its halves — step 22 "
           "creates the instance and step 24's last arm fills it, and a seam that answers one of those hands "
           "back an instance with none of its internal data set up");
    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    s->transfer = transfer;
    s->serializable = serializable;
    s->buf_start = buf;
    s->buf_end = buf + buf_len;
    s->ptr = buf;
    s->allow_bytecode = ((flags & JS_READ_OBJ_BYTECODE) != 0);
    s->allow_reference = ((flags & JS_READ_OBJ_REFERENCE) != 0);
    if (s->allow_bytecode)
        s->first_atom = JS_ATOM_END;
    else
        s->first_atom = 1;
    if (JS_ReadObjectAtoms(s)) {
        obj = JS_EXCEPTION;
    } else {
        obj = JS_ReadObjectRec(s);
    }
    bc_reader_free(s);
    return obj;
}

JSValue JS_ReadObject3(JSContext *ctx, const uint8_t *buf, size_t buf_len,
                       int flags, const JSTransferReadHook *transfer)
{
    return JS_ReadObject4(ctx, buf, buf_len, flags, transfer, NULL);
}

JSValue JS_ReadObject(JSContext *ctx, const uint8_t *buf, size_t buf_len,
                      int flags)
{
    return JS_ReadObject3(ctx, buf, buf_len, flags, NULL);
}

