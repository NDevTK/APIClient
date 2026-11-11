/*******************************************************************/
/* object list */

typedef struct {
    JSObject *obj;
    uint32_t hash_next; /* -1 if no next entry */
} JSObjectListEntry;

/* XXX: reuse it to optimize weak references */
typedef struct {
    JSObjectListEntry *object_tab;
    int object_count;
    int object_size;
    uint32_t *hash_table;
    uint32_t hash_size;
} JSObjectList;

static void js_object_list_init(JSObjectList *s)
{
    memset(s, 0, sizeof(*s));
}

static uint32_t js_object_list_get_hash(JSObject *p, uint32_t hash_size)
{
    return ((uintptr_t)p * 3163) & (hash_size - 1);
}

static int js_object_list_resize_hash(JSContext *ctx, JSObjectList *s,
                                 uint32_t new_hash_size)
{
    JSObjectListEntry *e;
    uint32_t i, h, *new_hash_table;

    new_hash_table = js_malloc(ctx, sizeof(new_hash_table[0]) * new_hash_size);
    if (!new_hash_table)
        return -1;
    js_free(ctx, s->hash_table);
    s->hash_table = new_hash_table;
    s->hash_size = new_hash_size;

    for(i = 0; i < s->hash_size; i++) {
        s->hash_table[i] = -1;
    }
    for(i = 0; i < s->object_count; i++) {
        e = &s->object_tab[i];
        h = js_object_list_get_hash(e->obj, s->hash_size);
        e->hash_next = s->hash_table[h];
        s->hash_table[h] = i;
    }
    return 0;
}

/* the reference count of 'obj' is not modified. Return 0 if OK, -1 if
   memory error */
static int js_object_list_add(JSContext *ctx, JSObjectList *s, JSObject *obj)
{
    JSObjectListEntry *e;
    uint32_t h, new_hash_size;

    if (js_resize_array(ctx, (void *)&s->object_tab,
                        sizeof(s->object_tab[0]),
                        &s->object_size, s->object_count + 1))
        return -1;
    if (unlikely((s->object_count + 1) >= s->hash_size)) {
        new_hash_size = max_uint32(s->hash_size, 4);
        while (new_hash_size <= s->object_count)
            new_hash_size *= 2;
        if (js_object_list_resize_hash(ctx, s, new_hash_size))
            return -1;
    }
    e = &s->object_tab[s->object_count++];
    h = js_object_list_get_hash(obj, s->hash_size);
    e->obj = obj;
    e->hash_next = s->hash_table[h];
    s->hash_table[h] = s->object_count - 1;
    return 0;
}

/* return -1 if not present or the object index */
static int js_object_list_find(JSContext *ctx, JSObjectList *s, JSObject *obj)
{
    JSObjectListEntry *e;
    uint32_t h, p;

    /* must test empty size because there is no hash table */
    if (s->object_count == 0)
        return -1;
    h = js_object_list_get_hash(obj, s->hash_size);
    p = s->hash_table[h];
    while (p != -1) {
        e = &s->object_tab[p];
        if (e->obj == obj)
            return p;
        p = e->hash_next;
    }
    return -1;
}

static void js_object_list_end(JSContext *ctx, JSObjectList *s)
{
    js_free(ctx, s->object_tab);
    js_free(ctx, s->hash_table);
}

/*******************************************************************/
/* binary object writer & reader */

typedef enum BCTagEnum {
    BC_TAG_NULL = 1,
    BC_TAG_UNDEFINED,
    BC_TAG_BOOL_FALSE,
    BC_TAG_BOOL_TRUE,
    BC_TAG_INT32,
    BC_TAG_FLOAT64,
    BC_TAG_STRING,
    BC_TAG_OBJECT,
    BC_TAG_ARRAY,
    BC_TAG_BIG_INT,
    BC_TAG_TEMPLATE_OBJECT,
    BC_TAG_FUNCTION_BYTECODE,
    BC_TAG_MODULE,
    BC_TAG_TYPED_ARRAY,
    BC_TAG_ARRAY_BUFFER,
    BC_TAG_REGEXP,
    BC_TAG_DATE,
    BC_TAG_OBJECT_VALUE,
    BC_TAG_OBJECT_REFERENCE,
    BC_TAG_MAP,
    BC_TAG_SET,
    BC_TAG_SYMBOL,
    /* HTML §2.7.7: a value the CALL named in its transfer list, reached from inside the message body. It
       carries the index of its data holder and nothing else, because the holder's contents do not exist yet —
       §2.7.7 serializes the body at step 3 and only runs the transfer steps at step 5. Readable only by a read
       given the map that resolves it (JSTransferReadHook); every other read refuses the tag. */
    BC_TAG_TRANSFER_REFERENCE,
    /* HTML §2.7.1: a platform object whose "primary interface is decorated with the [Serializable] IDL
       extended attribute". It carries HTML §2.7.3 step 19's "the identifier of the primary interface of
       value" and then ONE sub-value, which is that interface's serialization steps performed as a
       sub-serialization — the same walk under the same `memory`, so a cycle through a platform object
       terminates and a graph reaching one twice comes back as one object. Readable only by a read given the
       seam that creates an instance for a name (JSSerializableReadHook); every other read refuses the tag.
       NO BC_VERSION BUMP, and the reason is the same one the transfer tag was added under: a tag appended
       here changes the meaning of no byte any earlier write produced, so every record already in the cold
       tier stays readable — and a bump would make all of them unreadable, which is a regression rather than
       a protection. */
    BC_TAG_SERIALIZABLE,
    /* An ArrayBuffer whose bytes carry unknown input: BC_TAG_ARRAY_BUFFER's record, then a span section
       (BC_AB_SPAN_FORMAT and the live JSArrayBufferSpans as data, see JS_WriteArrayBuffer), then the spans'
       values as sub-values, so an unknown crosses as whatever the write's transfer hook makes of it and
       never as a pointer. A buffer with no live span keeps BC_TAG_ARRAY_BUFFER, so no earlier record changes
       meaning. */
    BC_TAG_ARRAY_BUFFER_SPANS,
} BCTagEnum;

/* The span section's own format byte, refused by the reader when it is not this one. */
#define BC_AB_SPAN_FORMAT 1
/* A span's sub-values, as bits of its mask byte, in the order they follow the section. */
#define BC_AB_SPAN_HAS_UNKNOWN 1
#define BC_AB_SPAN_HAS_INDEX   2
#define BC_AB_SPAN_HAS_IMAGE   4

/* THE LONGEST INTERFACE IDENTIFIER BC_TAG_SERIALIZABLE CARRIES. It is a bound on a NAME and not on a graph,
   so it is not the kind of cap that truncates work: Web IDL §2.4 Interfaces gives an interface an identifier,
   and no identifier any standard declares is near this. The reader needs it because the name arrives from
   bytes that may have crossed an instance, so it reads into a fixed buffer and refuses a longer one rather
   than allocating whatever a stream asks for. */
#define BC_SERIALIZABLE_NAME_MAX 63

/* 29: a regexp's compiled byte code changed shape — a capture or register index is a u32 where it was a
   byte, and the header grew with it.
   30: its group-name section lost a byte per group (LRE_GROUP_NAME_TRAILER_LEN 2 -> 1).
   32: a function's flag word carries from_eval and is_program, which were computed at parse time and then
   dropped on the way out -- so a deserialized function claimed to be neither eval code nor a program.
   31: a module's export/import entries carry an explicit is_namespace byte. It used to be inferred from the
   name being the atom for "*", which is a legal ModuleExportName, so the flag has to be written.
   Serialized byte code from an older build would be read as the new format and mis-execute, so the version
   byte is what refuses it. */
#define BC_VERSION 33
/* DELETED: the OP_COUNT and JS_ATOM_END asserts. They guarded ONE thing — the builtin-*.h bytecode COMPILED INTO
   this file, which named opcodes and atoms by index and so silently resolved the wrong one when either table
   shifted (Iterator.zip read `done` off the atom next to it, thousands of tests from the cause). Every one of
   those blobs is gone: Array.fromAsync was the last self-hosted builtin and this translation unit now
   deserializes nothing it was built with. What remains — qjsc output and the shell's generated sources — is
   regenerated by `make codegen` and version-checked at READ time by the BC_VERSION byte below, which is the real mechanism;
   a build-time magic number that has to be bumped for a guarantee nothing needs is scaffolding for a deleted
   system, and it made every added atom look like a wire-format change. */

/* One work item. `val` is owned iff own_val — a fetched element is ours to free, a borrowed slot is not. */
typedef struct BCWItem {
    uint8_t  kind;
    bool     own_val;
    bool     flag;                /* ARR: this array is a template, so a trailing `raw` follows */
    JSValue  val;                 /* VALUE: what to write.  ARR/OBJ: the container being iterated */
    JSObject *objp;               /* CLEAR_MARK: whose tmp_mark to drop */
    JSFunctionBytecode *fb;       /* CPOOL: whose constant pool is being walked */
    struct JSMapState *map;       /* MAPENT: which map/set */
    struct JSMapRecord *mr;       /* MAPENT: the cursor into it */
    JSShape *sh;                  /* OBJ_PROP: the shape captured when iteration began */
    uint32_t i, len;
} BCWItem;
enum {
    BCW_VALUE, BCW_CLEAR_MARK, BCW_ARR_ELEM, BCW_ARR_RAW,
    BCW_OBJ_PROP, BCW_CPOOL, BCW_FUNC_TAIL, BCW_MAPENT, BCW_FREE_VAL,
};

typedef struct BCWriterState {
    JSContext *ctx;
    DynBuf dbuf;
    bool allow_bytecode;
    bool allow_reference;
    bool allow_source;
    bool allow_debug;
    uint32_t first_atom;
    uint32_t *atom_to_idx;
    int atom_to_idx_size;
    JSAtom *idx_to_atom;
    int idx_to_atom_count;
    int idx_to_atom_size;
    /* list of referenced objects (used if allow_reference = true) */
    JSObjectList object_list;
    /* HTML §2.7.7's seeded `memory`, or NULL for a write that is not a StructuredSerializeWithTransfer. It is
       consulted BEFORE object_list, because §2.7.1 consults `memory` before it does anything else — and because
       a transferable must not take a reference slot the reader will not fill. */
    const JSTransferWriteHook *transfer;
    /* HTML §2.7.1's serializable seam, or NULL for a write that has none — under which every platform object
       takes HTML §2.7.3 step 20's refusal, which is the right answer for a write that cannot ask. */
    const JSSerializableWriteHook *serializable;
    /* THE SERIALISER'S WORK STACK. The writer was a nine-function recursion whose depth the WRITTEN GRAPH
       picks — a nested array through JS_WriteObject threw between depth 5000 and 20000, measured — and the
       js_check_stack_overflow in front of it turned a graph the format can represent into a RangeError.
       Output is a pre-order byte stream, so a LIFO stack reproduces it exactly: pop an item, emit its header,
       push its children so the first child pops next. Children are fetched ONE AT A TIME by re-pushing the
       iterator with an advanced cursor, never all up front — an element getter must not run before the
       previous element's own nested getters, which eager fetching would reorder. */
    struct BCWItem *w_stack;
    int w_sp, w_size;
} BCWriterState;

/* THE WRITER'S OWNED MEMORY, IN ONE PLACE, ADJACENT TO THE DECLARATION ABOVE — the same rule the COW layouts
   are built on: a record's owned-value list IS the list its release frees, so a field added to one and not
   the other is caught by reading them together.
   THE DEFECT SHAPE THIS ENDS: the release was a field-by-field list spelled TWICE inside JS_WriteObject4,
   at the success tail and at the fail tail, neither of them near the struct — so a field ADDED to the struct
   had to be added to two lists nobody reading the struct would see. `w_stack` is what this fork added when
   the writer stopped being a nine-function recursion, and it was named by neither, so every write lost its
   work stack.
   THE ASSERT THAT WOULD MAKE THIS TRUE BY CONSTRUCTION IS NOT AVAILABLE, AND NOT FOR WANT OF LOOKING — the
   canonical spelling in this file is a malloc-count conservation DCHECK across a scope (JS_OrphanTakeOne has
   one), and over a WRITE it would be a page-held abort switch: the driver fetches children one at a time
   precisely so that an element getter runs in order, so PAGE CODE RUNS INSIDE THIS SCOPE and may allocate
   anything it likes and keep it. A count over JS_WriteObject4 would fire on a page storing an object.
   RETIREMENT: this record goes when BCWriterState's owned fields are released by construction rather than by
   a list somebody keeps in step — at which point the two-list shape can no longer be re-derived. */
static void bcw_state_end(BCWriterState *s)
{
    JSContext *ctx = s->ctx;

    /* DRAINED ON BOTH PATHS: the driver's loop runs down to its own `base` and its unwind runs down to that
       same `base`, which is 0 at the outermost call. A surviving item is not a tidy-up this function could
       perform — it OWNS its fetched value when own_val is set, and a queued CLEAR_MARK still owes its object
       a tmp_mark reset. */
    DCHECK(s->w_sp == 0,
           "the bytecode writer reached its teardown with items still on its work stack — an arm returned "
           "without running the unwind, so every value those items own is lost and a queued CLEAR_MARK never "
           "dropped its tmp_mark, which makes a later write of the same object report a cycle that is not "
           "there");
    js_object_list_end(ctx, &s->object_list);
    js_free(ctx, s->atom_to_idx);
    js_free(ctx, s->idx_to_atom);
    js_free(ctx, s->w_stack);
    s->atom_to_idx = NULL;
    s->atom_to_idx_size = 0;
    s->idx_to_atom = NULL;
    s->idx_to_atom_count = 0;
    s->idx_to_atom_size = 0;
    s->w_stack = NULL;
    s->w_size = 0;
}

#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
static const char * const bc_tag_str[] = {
    "invalid",
    "null",
    "undefined",
    "false",
    "true",
    "int32",
    "float64",
    "string",
    "object",
    "array",
    "BigInt",
    "template",
    "function",
    "module",
    "TypedArray",
    "ArrayBuffer",
    "RegExp",
    "Date",
    "ObjectValue",
    "ObjectReference",
    "Map",
    "Set",
    "Symbol",
    "TransferReference",
    "Serializable",
};

static const char *bc_tag_name(uint8_t tag)
{
    if (tag >= countof(bc_tag_str))
        return "<bad tag>";
    return bc_tag_str[tag];
}
#endif

static void bc_put_u8(BCWriterState *s, uint8_t v)
{
    dbuf_putc(&s->dbuf, v);
}

static void bc_put_u16(BCWriterState *s, uint16_t v)
{
    dbuf_put_u16(&s->dbuf, v);
}

static __maybe_unused void bc_put_u32(BCWriterState *s, uint32_t v)
{
    dbuf_put_u32(&s->dbuf, v);
}

static void bc_put_u64(BCWriterState *s, uint64_t v)
{
    dbuf_put(&s->dbuf, (uint8_t *)&v, sizeof(v));
}

static void bc_put_leb128(BCWriterState *s, uint32_t v)
{
    dbuf_put_leb128(&s->dbuf, v);
}

static void bc_put_sleb128(BCWriterState *s, int32_t v)
{
    dbuf_put_sleb128(&s->dbuf, v);
}

static void bc_set_flags(uint32_t *pflags, int *pidx, uint32_t val, int n)
{
    *pflags = *pflags | (val << *pidx);
    *pidx += n;
}

static int bc_atom_to_idx(BCWriterState *s, uint32_t *pres, JSAtom atom)
{
    uint32_t v;

    if (atom < s->first_atom || __JS_AtomIsTaggedInt(atom)) {
        *pres = atom;
        return 0;
    }
    atom -= s->first_atom;
    if (atom < s->atom_to_idx_size && s->atom_to_idx[atom] != 0) {
        *pres = s->atom_to_idx[atom];
        return 0;
    }
    if (atom >= s->atom_to_idx_size) {
        int old_size, i;
        old_size = s->atom_to_idx_size;
        if (js_resize_array(s->ctx, (void **)&s->atom_to_idx,
                            sizeof(s->atom_to_idx[0]), &s->atom_to_idx_size,
                            atom + 1))
            return -1;
        /* XXX: could add a specific js_resize_array() function to do it */
        for(i = old_size; i < s->atom_to_idx_size; i++)
            s->atom_to_idx[i] = 0;
    }
    if (js_resize_array(s->ctx, (void **)&s->idx_to_atom,
                        sizeof(s->idx_to_atom[0]),
                        &s->idx_to_atom_size, s->idx_to_atom_count + 1))
        goto fail;

    v = s->idx_to_atom_count++;
    s->idx_to_atom[v] = atom + s->first_atom;
    v += s->first_atom;
    s->atom_to_idx[atom] = v;
    *pres = v;
    return 0;
 fail:
    *pres = 0;
    return -1;
}

static int bc_put_atom(BCWriterState *s, JSAtom atom)
{
    uint32_t v;

    if (__JS_AtomIsTaggedInt(atom)) {
        v = (__JS_AtomToUInt32(atom) << 1) | 1;
    } else {
        if (bc_atom_to_idx(s, &v, atom))
            return -1;
        v <<= 1;
    }
    bc_put_leb128(s, v);
    return 0;
}

static uint32_t bc_csum(const uint8_t *p, size_t n)
{
    uint32_t a, b, c, h;
    size_t i;

    h = 0;
    for (i = 0; i+4 < n; i += 4) {
        h += get_u32(p+i);
        h *= 0x9e370001;
    }
    a = b = c = 0;
    switch (n-i) {
    case 3:
        c = (uint32_t)p[i+2];
    case 2:
        b = (uint32_t)p[i+1];
    case 1:
        a = (uint32_t)p[i+0];
    case 0:
        break;
    }
    h += a | b<<8 | c<<16;
    h *= 0x9e370001;
    return h;
}

static int JS_WriteFunctionBytecode(BCWriterState *s,
                                    const JSFunctionBytecode *b)
{
    int pos, len, bc_len, op;
    JSAtom atom;
    uint8_t *bc_buf;
    uint32_t val;

    bc_len = b->byte_code_len;
    bc_buf = js_malloc(s->ctx, bc_len);
    if (!bc_buf)
        return -1;
    memcpy(bc_buf, b->byte_code_buf, bc_len);

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
            atom = get_u32(bc_buf + pos + 1);
            if (bc_atom_to_idx(s, &val, atom))
                goto fail;
            put_u32(bc_buf + pos + 1, val);
            break;
        default:
            break;
        }
        pos += len;
    }

    dbuf_put(&s->dbuf, bc_buf, bc_len);

    js_free(s->ctx, bc_buf);
    return 0;
 fail:
    js_free(s->ctx, bc_buf);
    return -1;
}

static void JS_WriteString(BCWriterState *s, JSString *p)
{
    int i;
    bc_put_leb128(s, ((uint32_t)p->len << 1) | p->is_wide_char);
    if (p->is_wide_char) {
        for(i = 0; i < p->len; i++)
            bc_put_u16(s, str16(p)[i]);
    } else {
        dbuf_put(&s->dbuf, str8(p), p->len);
    }
}

static int JS_WriteBigInt(BCWriterState *s, JSValueConst obj)
{
    JSBigIntBuf buf;
    JSBigInt *p;
    uint32_t len, i;
    js_limb_t v, b;
    int shift;

    bc_put_u8(s, BC_TAG_BIG_INT);

    if (JS_VALUE_GET_TAG(obj) == JS_TAG_SHORT_BIG_INT)
        p = js_bigint_set_short(&buf, obj);
    else
        p = JS_VALUE_GET_PTR(obj);
    if (p->len == 1 && p->tab[0] == 0) {
        /* zero case */
        len = 0;
    } else {
        /* compute the length of the two's complement representation
           in bytes */
        len = p->len * (JS_LIMB_BITS / 8);
        v = p->tab[p->len - 1];
        shift = JS_LIMB_BITS - 8;
        while (shift > 0) {
            b = (v >> shift) & 0xff;
            if (b != 0x00 && b != 0xff)
                break;
            if ((b & 1) != ((v >> (shift - 1)) & 1))
                break;
            shift -= 8;
            len--;
        }
    }
    bc_put_leb128(s, len);
    if (len > 0) {
        for(i = 0; i < (len / (JS_LIMB_BITS / 8)); i++) {
            bc_put_u32(s, p->tab[i]);
        }
        for(i = 0; i < len % (JS_LIMB_BITS / 8); i++) {
            bc_put_u8(s, (p->tab[p->len - 1] >> (i * 8)) & 0xff);
        }
    }
    return 0;
}

/* Push a work item, growing the stack. The item is taken by value; on failure it is freed here so a caller
   never has to unwind a half-pushed item. */
static int bcw_push(BCWriterState *s, BCWItem it)
{
    if (s->w_sp >= s->w_size) {
        int n = s->w_size ? s->w_size * 2 : 16;
        BCWItem *t = js_realloc(s->ctx, s->w_stack, sizeof(*t) * n);
        if (!t) {
            if (it.own_val)
                JS_FreeValue(s->ctx, it.val);
            return -1;
        }
        s->w_stack = t;
        s->w_size = n;
    }
    s->w_stack[s->w_sp++] = it;
    return 0;
}

static BCWItem bcw_val(JSValueConst v, bool own)
{
    BCWItem it;
    memset(&it, 0, sizeof(it));
    it.kind = BCW_VALUE;
    it.val = unsafe_unconst(v);
    it.own_val = own;
    return it;
}

/* Drop everything still queued — the error path, where the remaining items own fetched values. */
static void bcw_unwind(BCWriterState *s)
{
    while (s->w_sp > 0) {
        BCWItem *it = &s->w_stack[--s->w_sp];
        if (it->own_val)
            JS_FreeValue(s->ctx, it->val);
        /* a CLEAR_MARK still queued must still run: tmp_mark is the cycle detector and leaving it set would
           make a LATER write of the same object claim a circular reference that is not there. */
        if (it->kind == BCW_CLEAR_MARK)
            it->objp->tmp_mark = 0;
    }
}

static int JS_WriteObjectRec(BCWriterState *s, JSValueConst obj);

static int JS_WriteFunctionTag(BCWriterState *s, JSValueConst obj)
{
    JSFunctionBytecode *b = JS_VALUE_GET_PTR(obj);
    uint32_t flags;
    int idx, i;

    bc_put_u8(s, BC_TAG_FUNCTION_BYTECODE);
    flags = idx = 0;
    bc_set_flags(&flags, &idx, b->has_prototype, 1);
    bc_set_flags(&flags, &idx, b->has_simple_parameter_list, 1);
    bc_set_flags(&flags, &idx, b->is_derived_class_constructor, 1);
    bc_set_flags(&flags, &idx, b->need_home_object, 1);
    bc_set_flags(&flags, &idx, b->func_kind, 2);
    bc_set_flags(&flags, &idx, b->new_target_allowed, 1);
    bc_set_flags(&flags, &idx, b->super_call_allowed, 1);
    bc_set_flags(&flags, &idx, b->super_allowed, 1);
    bc_set_flags(&flags, &idx, b->arguments_allowed, 1);
    bc_set_flags(&flags, &idx, b->backtrace_barrier, 1);
    bc_set_flags(&flags, &idx, b->from_eval, 1);
    bc_set_flags(&flags, &idx, b->is_program, 1);
    bc_set_flags(&flags, &idx, s->allow_debug, 1);
    /* APPENDED LAST, AND THAT IS WHY BC_VERSION DOES NOT MOVE. Every bit before this one keeps its position,
       so a blob written by a build that predates this flag reads back identically and answers 0 here — which
       is "not an inline script", the conservative half: a deserialized function is treated as the bundle's, so
       the records it builds are never mistaken for the document's injected state. A bit inserted anywhere
       earlier would shift the rest and demand the version byte. */
    bc_set_flags(&flags, &idx, b->from_inline_script, 1);
    DCHECK(idx <= 16, "idx <= 16");
    bc_put_u16(s, flags);
    bc_put_u8(s, b->is_strict_mode);
    bc_put_atom(s, b->func_name);

    bc_put_leb128(s, b->arg_count);
    bc_put_leb128(s, b->var_count);
    bc_put_leb128(s, b->defined_arg_count);
    bc_put_leb128(s, b->stack_size);
    bc_put_leb128(s, b->var_ref_count);
    bc_put_leb128(s, b->closure_var_count);
    bc_put_leb128(s, b->cpool_count);
    bc_put_leb128(s, b->byte_code_len);
    if (b->vardefs) {
        /* XXX: this field is redundant */
        bc_put_leb128(s, b->arg_count + b->var_count);
        for(i = 0; i < b->arg_count + b->var_count; i++) {
            JSVarDef *vd = &b->vardefs[i];
            bc_put_atom(s, vd->var_name);
            bc_put_leb128(s, vd->scope_level);
            bc_put_leb128(s, vd->scope_next + 1);
            flags = idx = 0;
            bc_set_flags(&flags, &idx, vd->var_kind, 4);
            bc_set_flags(&flags, &idx, vd->is_const, 1);
            bc_set_flags(&flags, &idx, vd->is_lexical, 1);
            bc_set_flags(&flags, &idx, vd->is_captured, 1);
            DCHECK(idx <= 8, "idx <= 8");
            bc_put_u8(s, flags);
            if (vd->is_captured)
                bc_put_leb128(s, vd->var_ref_idx);
        }
    } else {
        bc_put_leb128(s, 0);
    }

    for(i = 0; i < b->closure_var_count; i++) {
        JSClosureVar *cv = &b->closure_var[i];
        bc_put_atom(s, cv->var_name);
        bc_put_leb128(s, cv->var_idx);
        flags = idx = 0;
        bc_set_flags(&flags, &idx, cv->closure_type, 3);
        bc_set_flags(&flags, &idx, cv->is_const, 1);
        bc_set_flags(&flags, &idx, cv->is_lexical, 1);
        bc_set_flags(&flags, &idx, cv->var_kind, 4);
        DCHECK(idx <= 16, "idx <= 16");
        bc_put_leb128(s, flags);
    }

    /* Constant pool BEFORE the code, so the code can be disassembled on the fly at read time. Both are
       queued: the tail is pushed first so it pops after every constant's whole subtree. */
    {
        BCWItem tail, pool;
        memset(&tail, 0, sizeof(tail));
        tail.kind = BCW_FUNC_TAIL;
        tail.fb = b;
        if (bcw_push(s, tail))
            goto fail;
        memset(&pool, 0, sizeof(pool));
        pool.kind = BCW_CPOOL;
        pool.fb = b;
        pool.len = b->cpool_count;
        if (bcw_push(s, pool))
            goto fail;
    }
    return 0;
 fail:
    return -1;
}

/* The half of a function record that follows its constant pool. */
static int bcw_func_tail(BCWriterState *s, JSFunctionBytecode *b)
{
    if (JS_WriteFunctionBytecode(s, b))
        goto fail;

    if (s->allow_debug) {
        bc_put_atom(s, b->filename);
        bc_put_leb128(s, b->line_num);
        bc_put_leb128(s, b->col_num);
        bc_put_leb128(s, b->pc2line_len);
        dbuf_put(&s->dbuf, b->pc2line_buf, b->pc2line_len);
        if (s->allow_source && b->source) {
            bc_put_leb128(s, b->source_len);
            dbuf_put(&s->dbuf, b->source, b->source_len);
        } else {
            bc_put_leb128(s, 0);
        }
    }
    return 0;
 fail:
    return -1;
}

static int JS_WriteModule(BCWriterState *s, JSValueConst obj)
{
    JSModuleDef *m = JS_VALUE_GET_PTR(obj);
    int i;

    bc_put_u8(s, BC_TAG_MODULE);
    bc_put_atom(s, m->module_name);

    bc_put_leb128(s, m->req_module_entries_count);
    for(i = 0; i < m->req_module_entries_count; i++) {
        JSReqModuleEntry *rme = &m->req_module_entries[i];
        bc_put_atom(s, rme->module_name);
    }

    bc_put_leb128(s, m->export_entries_count);
    for(i = 0; i < m->export_entries_count; i++) {
        JSExportEntry *me = &m->export_entries[i];
        bc_put_u8(s, me->export_type);
        if (me->export_type == JS_EXPORT_TYPE_LOCAL) {
            bc_put_leb128(s, me->u.local.var_idx);
        } else {
            bc_put_leb128(s, me->u.req_module_idx);
            bc_put_atom(s, me->local_name);
            /* `export * as ns from` is a property of the ENTRY, not of local_name — a module may legally
               export the name "*", so the reader cannot infer this back from the atom. */
            bc_put_u8(s, me->is_namespace);
        }
        bc_put_atom(s, me->export_name);
    }

    bc_put_leb128(s, m->star_export_entries_count);
    for(i = 0; i < m->star_export_entries_count; i++) {
        JSStarExportEntry *se = &m->star_export_entries[i];
        bc_put_leb128(s, se->req_module_idx);
    }

    bc_put_leb128(s, m->import_entries_count);
    for(i = 0; i < m->import_entries_count; i++) {
        JSImportEntry *mi = &m->import_entries[i];
        bc_put_leb128(s, mi->var_idx);
        bc_put_atom(s, mi->import_name);
        bc_put_u8(s, mi->is_namespace);
        bc_put_leb128(s, mi->req_module_idx);
    }

    bc_put_u8(s, m->has_tla);

    if (bcw_push(s, bcw_val(m->func_obj, false)))
        goto fail;
    return 0;
 fail:
    return -1;
}

/* HasOwnProperty FOR AN ARRAY'S INDEX, reaching no user code — the gate HTML §2.7.3
   StructuredSerializeInternal ( value, forStorage [ , memory ] ) puts in front of its element read, and the one
   thing that decides whether the read below is an operation the standard performs at all.
   IT IS EXACT RATHER THAN CONSERVATIVE, and the two representations are why it is written out. A FAST array
   holds its elements densely and cannot carry an index in its shape — putting one out of range is what converts
   it away from that representation — so `idx < count` IS own-ness there, and `length` may still run past `count`
   (`[].length = 10` keeps the fast form with no elements at all), which is exactly the case that used to walk.
   A SLOW array keeps its indices as ordinary shape slots, so the general own lookup answers, and it reaches
   nothing: JS_CLASS_ARRAY installs no exotic get_own_property hook (ARGUMENTS, MAPPED_ARGUMENTS, STRING,
   MODULE_NS and PROXY are the classes that do), and no Array carries a var_ref, so the -1 arm is unreachable
   and asserted rather than folded into the false answer — an absent element and a failed question must never
   be the same answer here, because one of them silently serialises a hole where a value was. */
static bool bcw_array_has_own_index(JSContext *ctx, JSValueConst arr, uint32_t idx)
{
    JSObject *p = JS_VALUE_GET_OBJ(arr);
    JSAtom atom;
    int r;

    DCHECK(JS_VALUE_GET_TAG(arr) == JS_TAG_OBJECT && p->class_id == JS_CLASS_ARRAY,
           "the bytecode writer's element gate was handed something that is not an Array — §2.7.3's Array arm "
           "is reached through JS_WriteObjectTag's class dispatch, which sends every other class elsewhere, and "
           "OrdinaryGetOwnProperty is only the right question because the receiver is an Array exotic object");
    if (p->fast_array)
        return idx < p->u.array.count;
    atom = JS_NewAtomUInt32(ctx, idx);
    CHECK(atom != JS_ATOM_NULL, "OOM interning an array index for a structured clone's element gate");
    r = JS_GetOwnPropertyInternal(ctx, NULL, p, atom);
    JS_FreeAtom(ctx, atom);
    DCHECK(r >= 0, "an Array's own-index lookup reported an abrupt completion — the only arm of "
                   "JS_GetOwnPropertyInternal2 that can is a module namespace's uninitialised var_ref, and an "
                   "Array has none, so this would mean the class dispatch let another class through");
    return r > 0;
}

static int JS_WriteArray(BCWriterState *s, JSValueConst obj)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    uint32_t i, len;
    JSValue val;
    int ret;
    bool is_template;

    if (s->allow_bytecode && !p->extensible) {
        /* not extensible array: we consider it is a
           template when we are saving bytecode */
        bc_put_u8(s, BC_TAG_TEMPLATE_OBJECT);
        is_template = true;
    } else {
        bc_put_u8(s, BC_TAG_ARRAY);
        is_template = false;
    }
    if (js_get_length32(s->ctx, &len, obj))
        goto fail1;
    bc_put_leb128(s, len);
    /* The elements are a QUEUED iteration, one fetch per pop: JS_GetPropertyUint32 can run a getter, and
       fetching them all here would run element i+1's getter before element i's own nested getters. */
    {
        BCWItem it;
        memset(&it, 0, sizeof(it));
        it.kind = BCW_ARR_ELEM;
        it.val = unsafe_unconst(obj);
        it.len = len;
        it.flag = is_template;
        if (bcw_push(s, it))
            goto fail1;
    }
    return 0;
 fail1:
    return -1;
}

static int JS_WriteObjectTag(BCWriterState *s, JSValueConst obj)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    uint32_t i, prop_count;
    JSShape *sh;
    JSShapeProperty *pr;
    int pass;
    JSAtom atom;

    bc_put_u8(s, BC_TAG_OBJECT);
    prop_count = 0;
    sh = p->shape;
    for(pass = 0; pass < 2; pass++) {
        if (pass == 1)
            bc_put_leb128(s, prop_count);
        for(i = 0, pr = get_shape_prop(sh); i < sh->prop_count; i++, pr++) {
            atom = pr->atom;
            if (atom != JS_ATOM_NULL && (pr->flags & JS_PROP_ENUMERABLE)) {
                if (pr->flags & JS_PROP_TMASK) {
                    JS_ThrowTypeError(s->ctx, "only value properties are supported");
                    goto fail;
                }
                if (pass == 0) {
                    prop_count++;
                } else {
                    /* pass 1 only establishes that the count is right; the writing is queued below */
                }
            }
        }
        if (pass == 1)
            break;   /* the second walk is the queued one */
    }
    {
        BCWItem it;
        memset(&it, 0, sizeof(it));
        it.kind = BCW_OBJ_PROP;
        it.val = unsafe_unconst(obj);
        it.sh = sh;   /* the shape the count was taken from, held exactly as the recursion held it */
        if (bcw_push(s, it))
            goto fail;
    }
    return 0;
 fail:
    return -1;
}

/* A DataView over a buffer, with BOTH bounds already known — the reader's case, and the one place a DataView is
   built from values that are not the page's. 25.3.2.1's constructor is a step machine because ToIndex on each
   bound runs the page's valueOf and OrdinaryCreateFromConstructor reads new.target's prototype; none of that
   applies to two integers this file just decoded, so the object-building tail is what is needed and the
   coercion prologue is not. The checks stay: a buffer that was detached between write and read, or bounds that
   do not fit it, must not produce a view pointing outside it. */
static JSValue js_dataview_new(JSContext *ctx, JSValueConst buffer, uint32_t offset, uint32_t len)
{
    JSArrayBuffer *abuf = js_get_array_buffer(ctx, buffer);
    JSTypedArray *ta;
    JSObject *p;
    JSValue obj;

    if (!abuf)
        return JS_EXCEPTION;
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    if ((int64_t)offset + len > abuf->byte_length)
        return JS_ThrowRangeError(ctx, "invalid byteOffset or byteLength");
    obj = JS_NewObjectClass(ctx, JS_CLASS_DATAVIEW);
    if (JS_IsException(obj))
        return obj;
    ta = js_malloc(ctx, sizeof(*ta));
    if (!ta) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    p = JS_VALUE_GET_OBJ(obj);
    ta->obj = p;
    ta->buffer = JS_VALUE_GET_OBJ(js_dup(buffer));
    ta->offset = offset;
    ta->length = len;
    ta->track_rab = false;
    ta->length_unknown = JS_UNINITIALIZED;   /* both bounds are stated */
    ta->offset_unknown = JS_UNINITIALIZED;
    list_add_tail(&ta->link, &abuf->array_list);
    p->u.typed_array = ta;
    return obj;
}

static int JS_WriteTypedArray(BCWriterState *s, JSValueConst obj)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    JSTypedArray *ta = p->u.typed_array;

    bc_put_u8(s, BC_TAG_TYPED_ARRAY);
    bc_put_u8(s, p->class_id - JS_CLASS_UINT8C_ARRAY);
    /* THE LENGTH THE MATCHING CONSTRUCTOR TAKES. A typed array's third argument counts ELEMENTS and lives in
       u.array.count; a DataView's counts BYTES and lives on the JSTypedArray record, because a DataView has no
       element type to count in. Writing one and reading it as the other is a view of the wrong length. */
    bc_put_leb128(s, p->class_id == JS_CLASS_DATAVIEW ? ta->length : p->u.array.count);
    DCHECK_TA_WINDOW_STATED(ta, 1, "the structured serialization of a typed array");
    bc_put_leb128(s, ta->offset);
    if (bcw_push(s, bcw_val(JS_MKPTR(JS_TAG_OBJECT, ta->buffer), false)))
        return -1;
    return 0;
}

static bool js_ab_span_live(const JSArrayBuffer *abuf, const JSArrayBufferSpan *sp);

static int JS_WriteArrayBuffer(BCWriterState *s, JSValueConst obj)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    JSArrayBuffer *abuf = p->u.array_buffer;
    bool has_len;
    int i, live = 0, k;

    if (abuf->detached) {
        JS_ThrowTypeErrorDetachedArrayBuffer(s->ctx);
        return -1;
    }
    for (i = 0; i < abuf->n_span; i++)
        live += js_ab_span_live(abuf, &abuf->span[i]);
    has_len = !JS_IsUninitialized(abuf->length_unknown);
    bc_put_u8(s, live || has_len ? BC_TAG_ARRAY_BUFFER_SPANS : BC_TAG_ARRAY_BUFFER);
    bc_put_leb128(s, abuf->byte_length);
    bc_put_leb128(s, abuf->max_byte_length);
    dbuf_put(&s->dbuf, abuf->data, abuf->byte_length);
    if (!live && !has_len)
        return 0;
    /* The facts about the bytes go with them, as data: each live span's fields, then its values as
       sub-values the transfer hook resolves (HTML §2.7's symbol numbering), so no live unknown crosses. A
       stale span describes bytes that are no longer there and is not written. */
    DCHECK(s->transfer != NULL,
           "an ArrayBuffer carrying unknown input was serialized with no transfer hook — its span values would "
           "be written as live objects; the record's owner (core/structured_clone.c) always passes one");
    bc_put_u8(s, BC_AB_SPAN_FORMAT);
    bc_put_u8(s, has_len);
    bc_put_leb128(s, live);
    for (i = 0; i < abuf->n_span; i++) {
        const JSArrayBufferSpan *sp = &abuf->span[i];
        uint8_t mask = 0;

        if (!js_ab_span_live(abuf, sp))
            continue;
        if (!JS_IsUninitialized(sp->unknown)) mask |= BC_AB_SPAN_HAS_UNKNOWN;
        if (!JS_IsUninitialized(sp->index)) mask |= BC_AB_SPAN_HAS_INDEX;
        if (!JS_IsUninitialized(sp->image)) mask |= BC_AB_SPAN_HAS_IMAGE;
        bc_put_u8(s, sp->kind);
        bc_put_leb128(s, sp->offset);
        bc_put_leb128(s, sp->extent);
        bc_put_u8(s, mask);
        if (sp->kind == JS_AB_SPAN_AT)
            dbuf_put(&s->dbuf, sp->witness, sizeof sp->witness);
        if (sp->kind == JS_AB_SPAN_IMAGE) {
            size_t cl = strlen(sp->codec);

            bc_put_leb128(s, sp->image_pos);
            bc_put_leb128(s, (uint32_t)cl);
            dbuf_put(&s->dbuf, (const uint8_t *)sp->codec, cl);
        }
    }
    /* The sub-values in section order: the work stack pops last-pushed first, so they are pushed reversed. */
    if (has_len && bcw_push(s, bcw_val(abuf->length_unknown, false)))
        return -1;
    for (i = abuf->n_span - 1; i >= 0; i--) {
        const JSArrayBufferSpan *sp = &abuf->span[i];
        JSValueConst f[3];

        if (!js_ab_span_live(abuf, sp))
            continue;
        f[0] = sp->unknown;
        f[1] = sp->index;
        f[2] = sp->image;
        for (k = 2; k >= 0; k--)
            if (!JS_IsUninitialized(f[k]) && bcw_push(s, bcw_val(f[k], false)))
                return -1;
    }
    return 0;
}

static int JS_WriteRegExp(BCWriterState *s, JSRegExp regexp)
{
    JSString *bc = regexp.bytecode;
    DCHECK(bc != NULL && regexp.pattern != NULL,
           "the serializer reached a RegExp RegExpInitialize has not filled, which only its constructor holds");
    DCHECK(!bc->is_wide_char, "!bc->is_wide_char");
    JS_WriteString(s, regexp.pattern);
    JS_WriteString(s, bc);
    return 0;
}

static int JS_WriteMap(BCWriterState *s, struct JSMapState *map_state);
static int JS_WriteSet(BCWriterState *s, struct JSMapState *map_state);

/* ONE ITEM. Emits obj's header and PUSHES whatever must follow it; it never writes a child itself, which is
   what keeps the C stack flat. No stack guard: the depth is the stack's, not the C stack's. */
static int bcw_step_value(BCWriterState *s, JSValueConst obj)
{
    uint32_t tag;

    tag = JS_VALUE_GET_NORM_TAG(obj);
    switch(tag) {
    case JS_TAG_NULL:
        bc_put_u8(s, BC_TAG_NULL);
        break;
    case JS_TAG_UNDEFINED:
        bc_put_u8(s, BC_TAG_UNDEFINED);
        break;
    case JS_TAG_BOOL:
        bc_put_u8(s, BC_TAG_BOOL_FALSE + JS_VALUE_GET_INT(obj));
        break;
    case JS_TAG_INT:
        bc_put_u8(s, BC_TAG_INT32);
        bc_put_sleb128(s, JS_VALUE_GET_INT(obj));
        break;
    case JS_TAG_FLOAT64:
        {
            JSFloat64Union u;
            bc_put_u8(s, BC_TAG_FLOAT64);
            u.d = JS_VALUE_GET_FLOAT64(obj);
            bc_put_u64(s, u.u64);
        }
        break;
    case JS_TAG_STRING:
        {
            JSString *p = JS_VALUE_GET_STRING(obj);
            bc_put_u8(s, BC_TAG_STRING);
            JS_WriteString(s, p);
        }
        break;
    case JS_TAG_STRING_ROPE:
        {
            JSValue str;
            int ret;
            str = JS_ToString(s->ctx, obj);
            if (JS_IsException(str))
                goto fail;
            if (bcw_push(s, bcw_val(str, true)))   /* takes ownership of str */
                goto fail;
        }
        break;
    case JS_TAG_FUNCTION_BYTECODE:
        if (!s->allow_bytecode)
            goto invalid_tag;
        if (JS_WriteFunctionTag(s, obj))
            goto fail;
        break;
    case JS_TAG_MODULE:
        if (!s->allow_bytecode)
            goto invalid_tag;
        if (JS_WriteModule(s, obj))
            goto fail;
        break;
    case JS_TAG_OBJECT:
        {
            JSObject *p = JS_VALUE_GET_OBJ(obj);
            int ret, idx;

            /* HTML §2.7.3 STEP 2, "If memory[value] exists, then return memory[value]" — asked FIRST, which is
               where the standard asks it. THE NUMBER WAS §2.7.1 AND WAS A RETIRED EDITION'S: `memory` is
               StructuredSerializeInternal's, and §2.7.1 is "Serializable objects", which is the seam the
               default arm below now asks. A value the call put in its transfer list is not cloned here and not
               refused below: it is written as the index of its data holder, and the reader turns that back into
               the object §2.7.8's transfer-receiving steps built. Reached twice, it writes the same index
               twice and comes back as one object, which is the identity `memory` exists to preserve. */
            if (s->transfer) {
                int ti = s->transfer->index_of(s->ctx, s->transfer->opaque, obj);
                if (ti >= 0) {
                    bc_put_u8(s, BC_TAG_TRANSFER_REFERENCE);
                    bc_put_leb128(s, ti);
                    break;
                }
            }
            if (s->allow_reference) {
                idx = js_object_list_find(s->ctx, &s->object_list, p);
                if (idx >= 0) {
                    bc_put_u8(s, BC_TAG_OBJECT_REFERENCE);
                    bc_put_leb128(s, idx);
                    break;
                } else {
                    if (js_object_list_add(s->ctx, &s->object_list, p))
                        goto fail;
                }
            } else {
                if (p->tmp_mark) {
                    JS_ThrowTypeError(s->ctx, "circular reference");
                    goto fail;
                }
                p->tmp_mark = 1;
                /* Queued BEFORE the children so it pops AFTER all of them — the recursion cleared the mark on
                   the way out of this object, and the whole subtree must still be inside it. */
                {
                    BCWItem cm;
                    memset(&cm, 0, sizeof(cm));
                    cm.kind = BCW_CLEAR_MARK;
                    cm.objp = p;
                    if (bcw_push(s, cm)) {
                        p->tmp_mark = 0;
                        goto fail;
                    }
                }
            }
            switch(p->class_id) {
            case JS_CLASS_ARRAY:
                ret = JS_WriteArray(s, obj);
                break;
            case JS_CLASS_OBJECT:
                ret = JS_WriteObjectTag(s, obj);
                break;
            case JS_CLASS_ARRAY_BUFFER:
                ret = JS_WriteArrayBuffer(s, obj);
                break;
            /* A SharedArrayBuffer's block is its runtime's (JSSharedBlock), so a byte stream cannot carry one;
               another agent of the runtime views it through JS_NewSharedArrayBufferView. */
            case JS_CLASS_SHARED_ARRAY_BUFFER:
                goto invalid_tag;
            case JS_CLASS_REGEXP:
                bc_put_u8(s, BC_TAG_REGEXP);
                ret = JS_WriteRegExp(s, p->u.regexp);
                break;
            case JS_CLASS_DATE:
                bc_put_u8(s, BC_TAG_DATE);
                ret = bcw_push(s, bcw_val(p->u.object_data, false));
                break;
            case JS_CLASS_NUMBER:
            case JS_CLASS_STRING:
            case JS_CLASS_BOOLEAN:
            case JS_CLASS_BIG_INT:
                bc_put_u8(s, BC_TAG_OBJECT_VALUE);
                ret = bcw_push(s, bcw_val(p->u.object_data, false));
                break;
            case JS_CLASS_MAP:
                bc_put_u8(s, BC_TAG_MAP);
                ret = JS_WriteMap(s, p->u.map_state);
                break;
            case JS_CLASS_SET:
                bc_put_u8(s, BC_TAG_SET);
                ret = JS_WriteSet(s, p->u.map_state);
                break;
            default:
                if (is_typed_array(p->class_id) || p->class_id == JS_CLASS_DATAVIEW) {
                    ret = JS_WriteTypedArray(s, obj);
                } else {
                    /* HTML §2.7.3 STEPS 19 AND 20, IN THAT ORDER AND IN THIS PLACE. Steps 7-18 are the
                       JavaScript-specification arms above; what reaches here is what none of them took, which
                       is step 19's "Otherwise, if value is a platform object that is a serializable object:"
                       followed by step 20's "Otherwise, if value is a platform object, then throw a
                       \"DataCloneError\" DOMException". The host answers the first question because
                       "serializable" is a property of an INTERFACE and this writer knows only classes.
                       A TRANSFERABLE one never arrives here at all: the transfer map above answered for it,
                       and if it did not, then the call did not name it and refusing it is the answer. */
                    const char *iface = s->serializable
                        ? s->serializable->interface_of(s->ctx, s->serializable->opaque, obj)
                        : NULL;

                    if (iface) {
                        /* STEP 26's THIRD ARM, "perform the serialization steps for value's primary
                           interface", as the ONE value those steps answer with. It is fetched BEFORE the tag
                           is emitted so that a throw from them leaves no half-written record behind; the
                           object is already in object_list, which is what step 25's "Set memory[value] to
                           serialized" does and is why the reader may register its instance at the same point. */
                        JSValue sub = s->serializable->serialize(s->ctx, s->serializable->opaque, obj);
                        size_t nlen = strlen(iface);

                        if (JS_IsException(sub)) {
                            ret = -1;
                        } else {
                            DCHECK(nlen > 0 && nlen <= BC_SERIALIZABLE_NAME_MAX,
                                   "a serializable interface answered an identifier this stream cannot carry "
                                   "— HTML §2.7.6 step 22 chooses the deserialization steps BY that name, so "
                                   "a name the reader cannot read back is a record no realm can resolve");
                            bc_put_u8(s, BC_TAG_SERIALIZABLE);
                            bc_put_leb128(s, (uint32_t)nlen);
                            dbuf_put(&s->dbuf, (const uint8_t *)iface, nlen);
                            /* THE SUB-SERIALIZATION IS ONE MORE ITEM ON THIS WRITER'S OWN STACK. HTML §2.7.3
                               step 26 defines it as "an operation which takes as input a value subValue, and
                               returns StructuredSerializeInternal(subValue, forStorage, memory)" — the same
                               walk under the same `memory` — so a nested JS_WriteObject would be a second
                               object_list and would neither terminate a cycle nor preserve `===`. */
                            ret = bcw_push(s, bcw_val(sub, true));
                        }
                    } else {
                        JS_ThrowTypeError(s->ctx, "unsupported object class");
                        ret = -1;
                    }
                }
                break;
            }
            if (ret)
                goto fail;
        }
        break;
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        if (JS_WriteBigInt(s, obj))
            goto fail;
        break;
    case JS_TAG_SYMBOL:
        {
            JSAtomStruct *p = JS_VALUE_GET_PTR(obj);
            /* HTML §2.7.3 "StructuredSerializeInternal ( value, forStorage [ , memory ] )" step 5, "If value is
               a Symbol, then throw a \"DataCloneError\" DOMException" — asked HERE because step 5 is a step of
               the RECURSIVE algorithm, which step 26 re-enters for every nested value. It was asked once, at
               the top level, by the caller in core/structured_clone.c, whose own comment gave the reason as
               "the engine's writer has no arm that refuses one"; the arm BELOW is that arm and it ACCEPTED one,
               so `structuredClone({s: Symbol()})` came back with an `s` where a browser throws. A top-level
               guard cannot be the answer to a step of a recursive algorithm, and that file HAS no per-value arm
               to move it into — its only per-value decision was the transfer map's index_of — which is the
               same gap HTML §2.7.1 "Serializable objects" needed closed. That gap IS closed: the arm below
               asks a host seam per value, and a Symbol stays here rather than moving there because this
               question is about a VALUE KIND the engine already knows and that one is about an INTERFACE,
               which it does not.
               THE DISCRIMINATOR IS THE BYTECODE FLAG AND IT IS NOT INCIDENTAL. §2.7.1 requires a serialized
               form "independent of any given realm", and a symbol has none: the tag below writes an ATOM, and
               the reader mints a symbol out of the reading runtime's atom table — a NEW symbol with the same
               description, which is not the value that went in. That is meaningful only where the producer and
               the consumer are the one trusted pair the bytecode format is for, which is what JS_READ_OBJ_BYTECODE's
               own warning already says. So the arm stays for a bytecode write and is refused for every other. */
            if (!s->allow_bytecode) {
                JS_ThrowTypeError(s->ctx, "a Symbol has no realm-independent serialization");
                goto fail;
            }
            if (p->atom_type != JS_ATOM_TYPE_GLOBAL_SYMBOL && p->atom_type != JS_ATOM_TYPE_SYMBOL) {
                JS_ThrowTypeError(s->ctx, "unsupported symbol type");
                goto fail;
            }
            JSAtom atom = js_get_atom_index(s->ctx->rt, p);
            bc_put_u8(s, BC_TAG_SYMBOL);
            bc_put_atom(s, atom);
        }
        break;
    default:
    invalid_tag:
        JS_ThrowInternalError(s->ctx, "unsupported tag (%d)", tag);
        goto fail;
    }
    return 0;

 fail:
    return -1;
}

/* THE DRIVER. One item per turn: emit, queue, pop. The C stack is flat at every depth the written graph can
   reach, which is why the guard that used to stand here is gone — it reported the C stack's limit as if the
   format could not represent the value. */
static int JS_WriteObjectRec(BCWriterState *s, JSValueConst obj)
{
    int base = s->w_sp;

    if (bcw_push(s, bcw_val(obj, false)))
        goto fail;

    while (s->w_sp > base) {
        BCWItem it = s->w_stack[--s->w_sp];   /* popped BY VALUE: re-pushes below may move the array */
        switch (it.kind) {
        case BCW_VALUE:
            {
                int r;
                /* AN OWNED VALUE OUTLIVES ITS STEP. bcw_step_value QUEUES work that still refers to this value
                   — an array's element cursor, an object's property cursor — so freeing it when the step
                   returns would hand those items a dead reference. The release is queued FIRST, so it pops
                   after everything the step pushed. */
                if (it.own_val) {
                    BCWItem fr;
                    memset(&fr, 0, sizeof(fr));
                    fr.kind = BCW_FREE_VAL;
                    fr.val = it.val;
                    fr.own_val = true;
                    if (bcw_push(s, fr))
                        goto fail;
                }
                r = bcw_step_value(s, it.val);
                if (r)
                    goto fail;
            }
            break;
        case BCW_FREE_VAL:
            JS_FreeValue(s->ctx, it.val);
            break;
        case BCW_CLEAR_MARK:
            it.objp->tmp_mark = 0;
            break;
        case BCW_ARR_ELEM:
            if (it.i < it.len) {
                JSValue val;
                uint32_t idx = it.i++;
                if (bcw_push(s, it))            /* the cursor, advanced */
                    goto fail;
                /* HTML §2.7.3 StructuredSerializeInternal ( value, forStorage [ , memory ] ), the deep arm the
                   Array case falls into: "for each key in ! EnumerableOwnProperties(value, key): If !
                   HasOwnProperty(value, key) is true: Let inputValue be ? value.[[Get]](key, value)". THE GATE
                   IS THE FIRST HALF OF THAT SENTENCE AND THIS WALK DID NOT HAVE IT — it read every index in
                   0..length-1 through [[Get]], and a [[Get]] on an index the array does not own WALKS THE
                   PROTOTYPE. So `Object.prototype[50] = 'x'; structuredClone(a)` on any array whose length runs
                   past its elements serialised `'x'` AS THE ARRAY'S ELEMENT — a value the standard appends
                   nothing for — and an ACCESSOR there ran the page's getter from a C activation with no flow
                   base under it, which is the abort that named this line. Both are one missing question.
                   THE READ THAT REMAINS IS THE ONE THE STANDARD PERFORMS, and it is still a [[Get]]: an own
                   enumerable index whose slot is an accessor invokes that getter, by the sentence above, so
                   this site's remaining abort is a real capability and not a wrong read — see the residual on
                   the else arm. */
                if (bcw_array_has_own_index(s->ctx, it.val, idx)) {
                    val = JS_GetPropertyUint32(s->ctx, it.val, idx);
                    if (JS_IsException(val))
                        goto fail;
                } else {
                    /* RESIDUAL — narrower than §2.7.3 and CORRECT for what it does, so there is nothing here to
                       crash on. WHAT IS NOT COVERED: the standard appends NOTHING for a non-own index, so the
                       deserialised array has a HOLE there; this writes `undefined`, because the wire format is
                       positional — JS_WriteArray emits `length` and then exactly `length` values, and the
                       reader (BC_TAG_ARRAY -> BCR_ARRAY) reconstructs by that count, so skipping a slot would
                       desynchronise it and read the next element's bytes as this one's. WHAT THE NEXT DIFF
                       BUILDS: §2.7.3's own shape, which is a [[Length]] beside a List of {[[Key]], [[Value]]}
                       records — the writer emits a count of PRESENT entries with each key, the reader
                       CreateDataProperty's at that key, and both the hole and an array's non-index own
                       enumerable keys (`a.foo = 1`, which EnumerableOwnProperties covers and this arm cannot
                       reach at all) fall out of the same change. HOW ITS ABSENCE SHOWS: `1 in structuredClone(
                       [0,,2])` is true here and false in a browser, and `Object.keys(structuredClone([0,,2]))`
                       has three entries rather than two. */
                    val = JS_UNDEFINED;
                }
                if (bcw_push(s, bcw_val(val, true)))
                    goto fail;
            } else if (it.flag) {
                it.kind = BCW_ARR_RAW;
                if (bcw_push(s, it))
                    goto fail;
            }
            break;
        case BCW_ARR_RAW:
            {
                JSValue val = JS_GetProperty(s->ctx, it.val, JS_ATOM_raw);
                if (JS_IsException(val))
                    goto fail;
                if (bcw_push(s, bcw_val(val, true)))
                    goto fail;
            }
            break;
        case BCW_OBJ_PROP:
            {
                JSObject *p = JS_VALUE_GET_OBJ(it.val);
                JSShape *sh = it.sh;
                JSShapeProperty *pr;
                uint32_t k = it.i;
                while (k < sh->prop_count) {
                    pr = get_shape_prop(sh) + k;
                    if (pr->atom != JS_ATOM_NULL && (pr->flags & JS_PROP_ENUMERABLE))
                        break;
                    k++;
                }
                if (k < sh->prop_count) {
                    pr = get_shape_prop(sh) + k;
                    bc_put_atom(s, pr->atom);
                    it.i = k + 1;
                    if (bcw_push(s, it))
                        goto fail;
                    if (bcw_push(s, bcw_val(p->prop[k].u.value, false)))
                        goto fail;
                }
            }
            break;
        case BCW_CPOOL:
            if (it.i < it.len) {
                uint32_t idx = it.i++;
                if (bcw_push(s, it))
                    goto fail;
                if (bcw_push(s, bcw_val(it.fb->cpool[idx], false)))
                    goto fail;
            }
            break;
        case BCW_FUNC_TAIL:
            if (bcw_func_tail(s, it.fb))
                goto fail;
            break;
        case BCW_MAPENT:
            {
                JSMapRecord *mr = it.mr;
                if (&mr->link != &it.map->records) {
                    JSValue half;
                    if (it.i == 0) {
                        half = mr->key;
                        /* a SET record is key-only; a MAP comes back for the value half */
                        if (it.flag) {
                            it.mr = list_entry(mr->link.next, JSMapRecord, link);
                        } else {
                            it.i = 1;
                        }
                    } else {
                        half = mr->value;
                        it.i = 0;
                        it.mr = list_entry(mr->link.next, JSMapRecord, link);
                    }
                    if (bcw_push(s, it))
                        goto fail;
                    if (bcw_push(s, bcw_val(half, false)))
                        goto fail;
                }
            }
            break;
        default:
            DFAIL("the bytecode writer's work stack holds an item kind it has no arm for");
        }
    }
    return 0;
 fail:
    while (s->w_sp > base) {
        BCWItem *it = &s->w_stack[--s->w_sp];
        if (it->own_val)
            JS_FreeValue(s->ctx, it->val);
        if (it->kind == BCW_CLEAR_MARK)
            it->objp->tmp_mark = 0;
    }
    return -1;
}

/* create the atom table */
static int JS_WriteObjectAtoms(BCWriterState *s)
{
    JSRuntime *rt = s->ctx->rt;
    DynBuf dbuf1;
    int i, atoms_size;

    dbuf1 = s->dbuf;
    js_dbuf_init(s->ctx, &s->dbuf);
    bc_put_u8(s, BC_VERSION);
    bc_put_u32(s, 0); // checksum, filled in after serialization

    bc_put_leb128(s, s->idx_to_atom_count);
    for(i = 0; i < s->idx_to_atom_count; i++) {
        JSAtom atom = s->idx_to_atom[i];
        if (__JS_AtomIsConst(atom)) {
            bc_put_u8(s, 0 /* the type */);
            /* TODO(saghul): encoding for tagged integers and keyword-ish atoms could be
               more efficient. */
            bc_put_u32(s, atom);
        } else {
            JSAtomStruct *p = rt->atom_array[atom];
            uint8_t type = p->atom_type;
            DCHECK(type != JS_ATOM_TYPE_PRIVATE, "type != JS_ATOM_TYPE_PRIVATE");
            bc_put_u8(s, type);
            JS_WriteString(s, p);
        }
    }
    /* XXX: should check for OOM in above phase */

    /* move the atoms at the start */
    /* XXX: could just append dbuf1 data, but it uses more memory if
       dbuf1 is larger than dbuf */
    atoms_size = s->dbuf.size;
    if (dbuf_claim(&dbuf1, atoms_size))
        goto fail;
    memmove(dbuf1.buf + atoms_size, dbuf1.buf, dbuf1.size);
    memcpy(dbuf1.buf, s->dbuf.buf, atoms_size);
    dbuf1.size += atoms_size;
    dbuf_free(&s->dbuf);
    s->dbuf = dbuf1;
    return 0;
 fail:
    dbuf_free(&dbuf1);
    return -1;
}

uint8_t *JS_WriteObject4(JSContext *ctx, size_t *psize, JSValueConst obj,
                         int flags,
                         const JSTransferWriteHook *transfer,
                         const JSSerializableWriteHook *serializable)
{
    BCWriterState ss, *s = &ss;
    uint32_t h;
    DynBuf *d;

    DCHECK(!transfer || transfer->index_of != NULL,
           "a transfer map was supplied to the writer without the function that answers it — HTML §2.7.7's "
           "`memory` IS that lookup, so a map that cannot be asked is not a seeded memory at all");
    DCHECK(!serializable || (serializable->interface_of != NULL && serializable->serialize != NULL),
           "HTML §2.7.1's serializable seam was supplied without both of its halves — step 19 names the "
           "interface and step 26's third arm performs its serialization steps, and a seam that answers one "
           "of those emits a tag whose sub-value nothing wrote");
    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    s->transfer = transfer;
    s->serializable = serializable;
    s->allow_bytecode = ((flags & JS_WRITE_OBJ_BYTECODE) != 0);
    s->allow_reference = ((flags & JS_WRITE_OBJ_REFERENCE) != 0);
    s->allow_source = ((flags & JS_WRITE_OBJ_STRIP_SOURCE) == 0);
    s->allow_debug = ((flags & JS_WRITE_OBJ_STRIP_DEBUG) == 0);
    /* XXX: could use a different version when bytecode is included */
    if (s->allow_bytecode)
        s->first_atom = JS_ATOM_END;
    else
        s->first_atom = 1;
    js_dbuf_init(ctx, &s->dbuf);
    js_object_list_init(&s->object_list);

    if (JS_WriteObjectRec(s, obj))
        goto fail;
    if (JS_WriteObjectAtoms(s))
        goto fail;
    /* THE DBUF IS THE ONE THING THIS FUNCTION HANDS OUT — it is the caller's from here,
       released with js_free, which is what structured_clone.c's structured_data_free does. Everything else
       the writer allocated goes back above. */
    bcw_state_end(s);
    *psize = s->dbuf.size;
    // don't include version and checksum fields in checksum
    d = &s->dbuf;
    h = bc_csum(&d->buf[5], d->size - 5);
    put_u32(&d->buf[1], h);
    return d->buf;
 fail:
    bcw_state_end(s);
    dbuf_free(&s->dbuf);
    *psize = 0;
    return NULL;
}

uint8_t *JS_WriteObject3(JSContext *ctx, size_t *psize, JSValueConst obj,
                         int flags, const JSTransferWriteHook *transfer)
{
    return JS_WriteObject4(ctx, psize, obj, flags, transfer, NULL);
}

uint8_t *JS_WriteObject(JSContext *ctx, size_t *psize, JSValueConst obj,
                        int flags)
{
    return JS_WriteObject3(ctx, psize, obj, flags, NULL);
}

