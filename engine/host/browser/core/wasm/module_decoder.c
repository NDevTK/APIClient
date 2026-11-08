/* WebAssembly Core Specification (Release 3.0) §5.5 "Modules": framing and section contents. See module_decoder.h. */
#include "core/wasm/module_decoder.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/encoding/decode.h>

#include "check.h"
#include "core/wasm/opcode_immediates.h"

enum {
    PH_PREAMBLE = 0,   /* §5.5.17's magic and version */
    PH_SECTION,        /* §5.5.2's next section header, or the end of the module */
    PH_NAME,           /* a §5.2.4 name, one code point per step, then `name_next` */
    PH_CUSTOM_REST,    /* §5.5.3: the custom section's bytes after its name */
    PH_TYPE,           /* §5.5.4: the next §5.3.8 rectype, or the section's end */
    PH_SUBTYPE,        /* the next subtype of the current rec group */
    PH_SUPERS,         /* the current subtype's supertype indices */
    PH_COMPTYPE,       /* its §5.3.7 composite type's head */
    PH_PARAMS,         /* a function type's parameters, then its results' count */
    PH_RESULTS,
    PH_FIELDS,         /* a struct type's fields */
    PH_ARRAY_FIELD,    /* an array type's field */
    PH_IMPORT,         /* §5.5.5: the next import's module name, or the section's end */
    PH_IMPORT_NAME,    /* its item name */
    PH_IMPORT_DESC,    /* its §5.3.14 external type */
    PH_FUNCTION,       /* §5.5.6: the next function's type index, or the section's end */
    PH_GLOBAL,         /* §5.5.9: the next global's type, or the section's end */
    PH_GLOBAL_END,     /* its initializer has been read */
    PH_TABLE,          /* §5.5.7: the next table, or the section's end */
    PH_TABLE_END,      /* its initializer has been read */
    PH_MEMORY,         /* §5.5.8: the next memory's limits, or the section's end */
    PH_TAG,            /* §5.5.16: the next tag type, or the section's end */
    PH_EXPORT,         /* §5.5.10: the next export's name, or the section's end */
    PH_EXPORT_DESC,    /* its external index */
    PH_ELEM,           /* §5.5.12: the next element segment's form, or the section's end */
    PH_ELEM_TYPE,      /* its offset has been read; its element type and items' count */
    PH_ELEM_ITEM,      /* its next item */
    PH_ELEM_EXPR_END,  /* an item expr has been read */
    PH_DATA,           /* §5.5.14: the next data segment's form, or the section's end */
    PH_DATA_BYTES,     /* its offset has been read; its bytes */
    PH_CODE,           /* §5.5.13: the next code entry's len and locals count, or the section's end */
    PH_LOCALS,         /* its next locals entry, then its body */
    PH_CODE_END,       /* its body has been read */
    PH_EXPR,           /* §5.4.10: the next instruction of an expr */
    PH_EXPR_LIST,      /* one element of an instruction's list immediate */
    PH_FINISHED
};

/* §5.5.17's module production: custom sections anywhere, every other section at most once in this order. The
   order is not the id order: the tag section (13) stands between memory and global, data count (12) between
   element and code. */
static const int SECTION_RANK[] = {
    [WASM_SECTION_CUSTOM] = 0,    [WASM_SECTION_TYPE] = 1,    [WASM_SECTION_IMPORT] = 2,
    [WASM_SECTION_FUNCTION] = 3,  [WASM_SECTION_TABLE] = 4,   [WASM_SECTION_MEMORY] = 5,
    [WASM_SECTION_TAG] = 6,       [WASM_SECTION_GLOBAL] = 7,  [WASM_SECTION_EXPORT] = 8,
    [WASM_SECTION_START] = 9,     [WASM_SECTION_ELEMENT] = 10, [WASM_SECTION_DATA_COUNT] = 11,
    [WASM_SECTION_CODE] = 12,     [WASM_SECTION_DATA] = 13,
};
#define SECTION_ID_LAST WASM_SECTION_TAG

void wasm_module_decoder_init(WasmModuleDecoder *d, const uint8_t *bytes, size_t n)
{
    DCHECK(d != NULL, "a WebAssembly decode was begun with no decoder");
    DCHECK(bytes != NULL || n == 0, "a WebAssembly decode was handed a length with no bytes");
    memset(d, 0, sizeof *d);
    d->in = bytes;
    d->in_len = n;
    d->phase = PH_PREAMBLE;
}

/* §5.2.2's uN: unsigned LEB128 in at most ceil(N/7) bytes, whose last byte sets no bit beyond the Nth. The
   checks run in the reference interpreter's order: the length before the read, the range after it. */
static WasmDecodeStatus read_uN(WasmModuleDecoder *d, int n, uint64_t *out)
{
    uint64_t v = 0;
    unsigned shift = 0;

    for (;;) {
        uint8_t b;

        if (n <= 0) return WASM_DECODE_INTEGER_TOO_LONG;
        if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
        b = d->in[d->pos++];
        if (n < 7 && (b & 0x7f) >= (1u << n)) return WASM_DECODE_INTEGER_TOO_LARGE;
        v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        n -= 7;
        if ((b & 0x80) == 0) {
            *out = v;
            return WASM_DECODE_DONE;
        }
    }
}

/* `*out` is written on success only, as every reader here does: a refused read leaves its operand as it was. */
static WasmDecodeStatus read_u32(WasmModuleDecoder *d, uint32_t *out)
{
    uint64_t v;
    WasmDecodeStatus st = read_uN(d, 32, &v);

    if (st == WASM_DECODE_DONE) *out = (uint32_t)v;
    return st;
}

/* §5.2.2's sN, as the reference interpreter reads it: at most ceil(N/7) bytes, and a last byte whose bits beyond
   N are all copies of the sign bit. */
static WasmDecodeStatus read_sN(WasmModuleDecoder *d, int n, int64_t *out)
{
    uint64_t v = 0;
    unsigned shift = 0;

    for (;;) {
        uint8_t b;

        if (n <= 0) return WASM_DECODE_INTEGER_TOO_LONG;
        if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
        b = d->in[d->pos++];
        if (n < 7) {
            uint8_t mask = (uint8_t)((0x7f << (n - 1)) & 0x7f);

            if ((b & mask) != 0 && (b & mask) != mask) return WASM_DECODE_INTEGER_TOO_LARGE;
        }
        v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        n -= 7;
        if ((b & 0x80) == 0) {
            if ((b & 0x40) != 0 && shift < 64) v |= ~(uint64_t)0 << shift;
            *out = (int64_t)v;
            return WASM_DECODE_DONE;
        }
    }
}

/* One byte read as an s7, the form every type constructor takes (§5.3's note: they are small negative sN
   values): a set continuation bit is an s7 too long, and the byte itself is the answer. */
static WasmDecodeStatus read_s7_byte(WasmModuleDecoder *d, uint8_t *out)
{
    if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
    *out = d->in[d->pos++];
    return (*out & 0x80) != 0 ? WASM_DECODE_INTEGER_TOO_LONG : WASM_DECODE_DONE;
}

/* A length (§5.1.3's list count, §5.5.2's and §5.5.13's len) as decode.ml's len32 bounds it: a u32 no larger
   than the bytes left counted from the length's own first byte. */
static WasmDecodeStatus read_list_length(WasmModuleDecoder *d, uint32_t *out)
{
    size_t at = d->pos;
    WasmDecodeStatus st = read_u32(d, out);

    if (st != WASM_DECODE_DONE) return st;
    return *out > d->in_len - at ? WASM_DECODE_LENGTH_OUT_OF_BOUNDS : WASM_DECODE_DONE;
}

/* §5.1.3's list(byte) (a name's or a data segment's bytes): its length as len32 bounds it, then the bytes
   themselves, which decode.ml's get_string refuses as an unexpected end when they run past the input. */
static WasmDecodeStatus read_byte_length(WasmModuleDecoder *d, uint32_t *out)
{
    WasmDecodeStatus st = read_list_length(d, out);

    if (st != WASM_DECODE_DONE) return st;
    return *out > d->in_len - d->pos ? WASM_DECODE_UNEXPECTED_END : WASM_DECODE_DONE;
}

/* §5.5.17's preamble: two four-byte words, each read whole before it is compared. */
static WasmDecodeStatus read_preamble(WasmModuleDecoder *d)
{
    static const uint8_t MAGIC[4] = { 0x00, 0x61, 0x73, 0x6D };
    static const uint8_t VERSION[4] = { 0x01, 0x00, 0x00, 0x00 };

    if (d->in_len < 4) return WASM_DECODE_UNEXPECTED_END;
    if (memcmp(d->in, MAGIC, 4) != 0) return WASM_DECODE_MAGIC;
    if (d->in_len < 8) return WASM_DECODE_UNEXPECTED_END;
    if (memcmp(d->in + 4, VERSION, 4) != 0) return WASM_DECODE_VERSION;
    d->pos = 8;
    d->phase = PH_SECTION;
    return WASM_DECODE_MORE;
}

/* Room for one more element in a pool of `*n` elements of `elem` bytes. The pool's length is the input's to
   choose, so a failed allocation is the NO_ROOM refusal rather than a CHECK. */
static bool pool_room(void **pool, size_t *cap, size_t n, size_t elem)
{
    size_t grown_cap;
    void *grown;

    if (n < *cap) return true;
    grown_cap = *cap ? *cap * 2 : 8;
    if (grown_cap > SIZE_MAX / elem) return false;
    grown = realloc(*pool, grown_cap * elem);
    if (grown == NULL) return false;
    *pool = grown;
    *cap = grown_cap;
    return true;
}
#define POOL_ROOM(arr, n, cap) pool_room((void **)&(arr), &(cap), (n), sizeof *(arr))

static WasmDecodeStatus push_section(WasmModuleDecoder *d, WasmSectionId id, size_t offset, size_t size)
{
    WasmSection *s;

    if (!POOL_ROOM(d->sections, d->n_sections, d->cap_sections)) return WASM_DECODE_NO_ROOM;
    s = &d->sections[d->n_sections++];
    s->id = id;
    s->offset = offset;
    s->size = size;
    s->name_offset = 0;
    s->name_size = 0;
    return WASM_DECODE_MORE;
}

/* A section's contents end where its §5.5.2 len says; the reference interpreter asks once the list is read. */
static WasmDecodeStatus end_section(WasmModuleDecoder *d)
{
    if (d->pos != d->section_end) return WASM_DECODE_SECTION_SIZE;
    d->phase = PH_SECTION;
    return WASM_DECODE_MORE;
}

/* §5.2.4's name begins: list(byte)'s length, held to the bytes left; its code points are read by PH_NAME, then
   the decode goes on at `next`. */
static WasmDecodeStatus begin_name(WasmModuleDecoder *d, int next, size_t *offset, uint32_t *size)
{
    WasmDecodeStatus st = read_byte_length(d, size);

    if (st != WASM_DECODE_DONE) return st;
    *offset = d->pos;
    d->name_end = d->pos + *size;
    d->name_next = next;
    d->phase = PH_NAME;
    return WASM_DECODE_MORE;
}

/* §5.5.2: an id byte, then the contents' u32 length. A section's order is judged on its id alone, as the
   reference interpreter does by never reading past an id it did not expect. */
static WasmDecodeStatus read_section(WasmModuleDecoder *d)
{
    WasmDecodeStatus st;
    uint32_t size, name_size;
    uint8_t id;

    if (d->pos == d->in_len) {
        /* §5.5.17's side conditions over the whole module, in decode.ml's order. */
        if (d->module.n_func_types != d->module.n_codes) return WASM_DECODE_FUNC_CODE_LENGTHS;
        if (d->module.has_data_count && d->module.data_count != d->module.n_datas)
            return WASM_DECODE_DATA_COUNT_LENGTH;
        if (!d->module.has_data_count && d->code_names_data) return WASM_DECODE_DATA_COUNT_REQUIRED;
        return WASM_DECODE_DONE;
    }
    id = d->in[d->pos];
    if (id > SECTION_ID_LAST) return WASM_DECODE_SECTION_ID;
    if (id != WASM_SECTION_CUSTOM) {
        if (SECTION_RANK[id] <= d->last_rank) return WASM_DECODE_SECTION_ORDER;
        d->last_rank = SECTION_RANK[id];
    }
    d->pos++;
    st = read_list_length(d, &size);
    if (st != WASM_DECODE_DONE) return st;
    st = push_section(d, (WasmSectionId)id, d->pos, size);
    if (st != WASM_DECODE_MORE) return st;
    d->section_end = d->pos + size;
    if (id == WASM_SECTION_TYPE) {
        st = read_list_length(d, &d->items_left);
        if (st != WASM_DECODE_DONE) return st;
        d->phase = PH_TYPE;
        return WASM_DECODE_MORE;
    }
    if (id != WASM_SECTION_CUSTOM && id != WASM_SECTION_DATA_COUNT && id != WASM_SECTION_START) {
        static const int LIST_PHASE[] = {
            [WASM_SECTION_IMPORT] = PH_IMPORT,   [WASM_SECTION_FUNCTION] = PH_FUNCTION,
            [WASM_SECTION_TABLE] = PH_TABLE,     [WASM_SECTION_MEMORY] = PH_MEMORY,
            [WASM_SECTION_GLOBAL] = PH_GLOBAL,   [WASM_SECTION_EXPORT] = PH_EXPORT,
            [WASM_SECTION_ELEMENT] = PH_ELEM,    [WASM_SECTION_CODE] = PH_CODE,
            [WASM_SECTION_DATA] = PH_DATA,       [WASM_SECTION_TAG] = PH_TAG,
        };

        st = read_list_length(d, &d->items_left);
        if (st != WASM_DECODE_DONE) return st;
        d->phase = LIST_PHASE[id];
        DCHECK(d->phase != 0, "a WebAssembly section id with a list has no phase to read it");
        return WASM_DECODE_MORE;
    }
    if (id == WASM_SECTION_START) {
        /* §5.5.11's start: one funcidx. */
        st = read_u32(d, &d->module.start);
        if (st != WASM_DECODE_DONE) return st;
        d->module.has_start = true;
        return end_section(d);
    }
    if (id == WASM_SECTION_DATA_COUNT) {
        /* §5.5.15's datacnt: one u32. */
        st = read_u32(d, &d->module.data_count);
        if (st != WASM_DECODE_DONE) return st;
        d->module.has_data_count = true;
        return end_section(d);
    }
    /* §5.5.3's custom: a name, then bytes to the section's end. The name's length is bounded by the bytes, its
       encoding is read next, and only then is it held to the section, which is the reference order. */
    st = begin_name(d, PH_CUSTOM_REST, &d->sections[d->n_sections - 1].name_offset, &name_size);
    if (st == WASM_DECODE_MORE) d->sections[d->n_sections - 1].name_size = name_size;
    return st;
}

static WasmDecodeStatus read_custom_rest(WasmModuleDecoder *d)
{
    if (d->name_end > d->section_end || d->section_end > d->in_len) return WASM_DECODE_UNEXPECTED_END;
    d->pos = d->section_end;
    d->phase = PH_SECTION;
    return WASM_DECODE_MORE;
}

/* §5.2.4's name: one character of utf8 per step, through lexbor's UTF-8 decoder, which refuses exactly the byte
   sequences §5.2.4's utf8 does not produce (overlongs, surrogates, above U+10FFFF). A fresh decoder state is
   the whole of one: its `need` and `lower` are the only fields the single-character entry reads. */
static WasmDecodeStatus read_name(WasmModuleDecoder *d)
{
    lxb_encoding_decode_t dec;
    const lxb_char_t *q;
    lxb_codepoint_t cp;

    if (d->pos == d->name_end) {
        d->phase = d->name_next;
        return WASM_DECODE_MORE;
    }
    dec.u.utf_8.need = 0;
    dec.u.utf_8.lower = 0x00;
    q = d->in + d->pos;
    cp = lxb_encoding_decode_utf_8_single(&dec, &q, d->in + d->name_end);
    if (cp == LXB_ENCODING_DECODE_ERROR || cp == LXB_ENCODING_DECODE_CONTINUE) return WASM_DECODE_UTF8;
    DCHECK(cp <= LXB_ENCODING_DECODE_MAX_CODEPOINT && q > d->in + d->pos && q <= d->in + d->name_end,
           "lexbor's UTF-8 decoder answered a character without consuming bytes inside the name");
    d->pos = (size_t)(q - d->in);
    return WASM_DECODE_MORE;
}

/* §5.3.3's heaptype: an s33 that is not negative is a type index; otherwise the same byte, read again as an s7,
   must be an abstract heap type. */
static WasmDecodeStatus read_heap_type(WasmModuleDecoder *d, WasmValType *t)
{
    size_t at = d->pos;
    int64_t x;
    uint8_t b;
    WasmDecodeStatus st = read_sN(d, 33, &x);

    if (st == WASM_DECODE_DONE && x >= 0) {
        t->heap = 0;
        t->heap_index = (uint32_t)x;
        return WASM_DECODE_DONE;
    }
    d->pos = at;
    st = read_s7_byte(d, &b);
    if (st != WASM_DECODE_DONE) return st;
    if (b < WASM_HEAP_EXN || b > WASM_HEAP_NOEXN) return WASM_DECODE_HEAP_TYPE;
    t->heap = b;
    return WASM_DECODE_DONE;
}

/* §5.3.5's valtype: a number or vector type's byte, §5.3.4's 0x63 (ref null) or 0x64 (ref) before a heap type,
   or an abstract heap type's byte alone, which is its nullable reference. */
static WasmDecodeStatus read_val_type(WasmModuleDecoder *d, WasmValType *t)
{
    uint8_t b;
    WasmDecodeStatus st = read_s7_byte(d, &b);

    memset(t, 0, sizeof *t);
    if (st != WASM_DECODE_DONE) return st;
    switch (b) {
    case WASM_TYPE_I32: case WASM_TYPE_I64: case WASM_TYPE_F32: case WASM_TYPE_F64: case WASM_TYPE_V128:
        t->code = b;
        return WASM_DECODE_DONE;
    case 0x63: case 0x64:
        t->code = WASM_TYPE_REF;
        t->nullable = b == 0x63;
        return read_heap_type(d, t);
    }
    if (b < WASM_HEAP_EXN || b > WASM_HEAP_NOEXN) return WASM_DECODE_REF_TYPE;
    t->code = WASM_TYPE_REF;
    t->nullable = true;
    t->heap = b;
    return WASM_DECODE_DONE;
}

/* §5.3.7's storagetype: a value type, or else the same bytes as a packed type. */
static WasmDecodeStatus read_storage_type(WasmModuleDecoder *d, WasmValType *t)
{
    size_t at = d->pos;
    uint8_t b;
    WasmDecodeStatus st = read_val_type(d, t);

    if (st == WASM_DECODE_DONE) return st;
    d->pos = at;
    st = read_s7_byte(d, &b);
    if (st != WASM_DECODE_DONE) return st;
    if (b != WASM_TYPE_I8 && b != WASM_TYPE_I16) return WASM_DECODE_STORAGE_TYPE;
    memset(t, 0, sizeof *t);
    t->code = b;
    return WASM_DECODE_DONE;
}

static WasmSubType *current_type(WasmModuleDecoder *d)
{
    DCHECK(d->module.n_types > 0, "a WebAssembly type's contents were read with no type begun");
    return &d->module.types[d->module.n_types - 1];
}

/* §5.3.7's fieldtype: a storage type, then its mut byte. */
static WasmDecodeStatus read_field(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmFieldType f;
    WasmDecodeStatus st = read_storage_type(d, &f.storage);

    if (st != WASM_DECODE_DONE) return st;
    if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
    if (d->in[d->pos] > 0x01) return WASM_DECODE_MUTABILITY;
    f.mutable_ = d->in[d->pos++] == 0x01;
    if (!POOL_ROOM(m->fields, m->n_fields, m->cap_fields)) return WASM_DECODE_NO_ROOM;
    m->fields[m->n_fields++] = f;
    current_type(d)->n_fields++;
    return WASM_DECODE_MORE;
}

/* One value type of the current function type's parameters or results. */
static WasmDecodeStatus read_func_val_type(WasmModuleDecoder *d, size_t *count)
{
    WasmModule *m = &d->module;
    WasmValType t;
    WasmDecodeStatus st = read_val_type(d, &t);

    if (st != WASM_DECODE_DONE) return st;
    if (!POOL_ROOM(m->val_types, m->n_val_types, m->cap_val_types)) return WASM_DECODE_NO_ROOM;
    m->val_types[m->n_val_types++] = t;
    (*count)++;
    return WASM_DECODE_MORE;
}

/* §5.5.4's typesec is list(rectype); §5.3.8's rectype is 0x4E list(subtype), or one subtype as a group of one. */
static WasmDecodeStatus read_rec_type(WasmModuleDecoder *d)
{
    WasmDecodeStatus st;

    if (d->items_left == 0) return end_section(d);
    d->items_left--;
    if (d->pos < d->in_len && d->in[d->pos] == 0x4E) {
        d->pos++;
        st = read_list_length(d, &d->group_left);
        if (st != WASM_DECODE_DONE) return st;
    } else {
        d->group_left = 1;
    }
    d->rec_start = d->module.n_types;
    d->rec_size = d->group_left;
    d->phase = PH_SUBTYPE;
    return WASM_DECODE_MORE;
}

/* §5.3.8's subtype: 0x50 (open) or 0x4F (final) with its supertypes' indices, or a composite type alone,
   which is final with none. */
static WasmDecodeStatus read_sub_type(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmSubType *t;
    WasmDecodeStatus st;

    if (d->group_left == 0) {
        d->phase = PH_TYPE;
        return WASM_DECODE_MORE;
    }
    d->group_left--;
    if (!POOL_ROOM(m->types, m->n_types, m->cap_types)) return WASM_DECODE_NO_ROOM;
    t = &m->types[m->n_types++];
    memset(t, 0, sizeof *t);
    t->rec_start = d->rec_start;
    t->rec_size = d->rec_size;
    t->supers = m->n_type_indices;
    t->final = true;
    d->phase = PH_COMPTYPE;
    if (d->pos < d->in_len && (d->in[d->pos] == 0x50 || d->in[d->pos] == 0x4F)) {
        t->final = d->in[d->pos++] == 0x4F;
        st = read_list_length(d, &d->list_left);
        if (st != WASM_DECODE_DONE) return st;
        d->phase = PH_SUPERS;
    }
    return WASM_DECODE_MORE;
}

static WasmDecodeStatus read_super(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmDecodeStatus st;
    uint32_t x;

    if (d->list_left == 0) {
        d->phase = PH_COMPTYPE;
        return WASM_DECODE_MORE;
    }
    d->list_left--;
    st = read_u32(d, &x);
    if (st != WASM_DECODE_DONE) return st;
    if (!POOL_ROOM(m->type_indices, m->n_type_indices, m->cap_type_indices)) return WASM_DECODE_NO_ROOM;
    m->type_indices[m->n_type_indices++] = x;
    current_type(d)->n_supers++;
    return WASM_DECODE_MORE;
}

/* §5.3.7's comptype: 0x60 (func) with two result types, 0x5F (struct) with a list of fields, 0x5E (array) with
   one field. */
static WasmDecodeStatus read_comp_type(WasmModuleDecoder *d)
{
    WasmSubType *t = current_type(d);
    WasmDecodeStatus st;
    uint8_t b;

    st = read_s7_byte(d, &b);
    if (st != WASM_DECODE_DONE) return st;
    t->fields = d->module.n_fields;
    t->params = d->module.n_val_types;
    switch (b) {
    case WASM_COMP_FUNC:
        t->kind = WASM_COMP_FUNC;
        d->phase = PH_PARAMS;
        break;
    case WASM_COMP_STRUCT:
        t->kind = WASM_COMP_STRUCT;
        d->phase = PH_FIELDS;
        break;
    case WASM_COMP_ARRAY:
        t->kind = WASM_COMP_ARRAY;
        d->phase = PH_ARRAY_FIELD;
        return WASM_DECODE_MORE;
    default:
        return WASM_DECODE_COMP_TYPE;
    }
    st = read_list_length(d, &d->list_left);
    return st == WASM_DECODE_DONE ? WASM_DECODE_MORE : st;
}

/* A function type's parameters, then the results' length: the two §5.3.6 result types in their order. */
static WasmDecodeStatus read_param(WasmModuleDecoder *d)
{
    WasmSubType *t = current_type(d);
    WasmDecodeStatus st;

    if (d->list_left > 0) {
        d->list_left--;
        return read_func_val_type(d, &t->n_params);
    }
    t->results = d->module.n_val_types;
    st = read_list_length(d, &d->list_left);
    if (st != WASM_DECODE_DONE) return st;
    d->phase = PH_RESULTS;
    return WASM_DECODE_MORE;
}

static WasmDecodeStatus read_result(WasmModuleDecoder *d)
{
    if (d->list_left == 0) {
        d->phase = PH_SUBTYPE;
        return WASM_DECODE_MORE;
    }
    d->list_left--;
    return read_func_val_type(d, &current_type(d)->n_results);
}

static WasmDecodeStatus read_struct_field(WasmModuleDecoder *d)
{
    if (d->list_left == 0) {
        d->phase = PH_SUBTYPE;
        return WASM_DECODE_MORE;
    }
    d->list_left--;
    return read_field(d);
}

static WasmDecodeStatus read_array_field(WasmModuleDecoder *d)
{
    d->phase = PH_SUBTYPE;
    return read_field(d);
}

/* §5.3.4's reftype: an abstract heap type's byte alone, or 0x63/0x64 before a heap type. */
static WasmDecodeStatus read_ref_type(WasmModuleDecoder *d, WasmValType *t)
{
    uint8_t b;
    WasmDecodeStatus st;

    memset(t, 0, sizeof *t);
    if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
    b = d->in[d->pos];
    if (b != 0x63 && b != 0x64 && (b < WASM_HEAP_EXN || b > WASM_HEAP_NOEXN)) {
        st = read_s7_byte(d, &b);
        return st == WASM_DECODE_DONE ? WASM_DECODE_REF_TYPE : st;
    }
    return read_val_type(d, t);
}

/* §5.3.9's limits: a flag byte (bit 0 a maximum, bit 2 the i64 address type), then u64 minimum and maximum. */
static WasmDecodeStatus read_limits(WasmModuleDecoder *d, WasmLimits *l)
{
    uint8_t flags;
    WasmDecodeStatus st;

    memset(l, 0, sizeof *l);
    if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
    flags = d->in[d->pos++];
    if ((flags & 0xfa) != 0) return WASM_DECODE_LIMITS_FLAGS;
    l->has_max = (flags & 0x01) != 0;
    l->addr64 = (flags & 0x04) != 0;
    st = read_uN(d, 64, &l->min);
    if (st != WASM_DECODE_DONE || !l->has_max) return st;
    return read_uN(d, 64, &l->max);
}

/* §5.5.5's import: a module name and an item name, each a §5.2.4 name, then a §5.3.14 external type. */
static WasmDecodeStatus read_import(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmImport *im;
    uint32_t size;
    WasmDecodeStatus st;

    if (d->items_left == 0) return end_section(d);
    d->items_left--;
    if (!POOL_ROOM(m->imports, m->n_imports, m->cap_imports)) return WASM_DECODE_NO_ROOM;
    im = &m->imports[m->n_imports++];
    memset(im, 0, sizeof *im);
    st = begin_name(d, PH_IMPORT_NAME, &im->module_offset, &size);
    if (st == WASM_DECODE_MORE) im->module_size = size;
    return st;
}

static WasmDecodeStatus read_import_name(WasmModuleDecoder *d)
{
    WasmImport *im = &d->module.imports[d->module.n_imports - 1];
    uint32_t size;
    WasmDecodeStatus st = begin_name(d, PH_IMPORT_DESC, &im->name_offset, &size);

    if (st == WASM_DECODE_MORE) im->name_size = size;
    return st;
}

/* §5.3.14's externtype: 0x00 a func's type index, 0x01 a table type (§5.3.13: reftype, limits), 0x02 a memory
   type (§5.3.12: limits), 0x03 a global type (§5.3.11: valtype, mut), 0x04 a tag type (§5.3.10: 0x00, type
   index). */
static WasmDecodeStatus read_import_desc(WasmModuleDecoder *d)
{
    WasmImport *im = &d->module.imports[d->module.n_imports - 1];
    WasmDecodeStatus st;
    uint8_t kind;

    if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
    kind = d->in[d->pos++];
    switch (kind) {
    case WASM_EXTERN_FUNC:
        st = read_u32(d, &im->index);
        break;
    case WASM_EXTERN_TABLE:
        st = read_ref_type(d, &im->type);
        if (st == WASM_DECODE_DONE) st = read_limits(d, &im->limits);
        break;
    case WASM_EXTERN_MEMORY:
        st = read_limits(d, &im->limits);
        break;
    case WASM_EXTERN_GLOBAL:
        st = read_val_type(d, &im->type);
        if (st != WASM_DECODE_DONE) break;
        if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
        if (d->in[d->pos] > 0x01) return WASM_DECODE_MUTABILITY;
        im->mutable_ = d->in[d->pos++] == 0x01;
        break;
    case WASM_EXTERN_TAG:
        if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
        if (d->in[d->pos++] != 0x00) return WASM_DECODE_ZERO_BYTE;
        st = read_u32(d, &im->index);
        break;
    default:
        return WASM_DECODE_IMPORT_KIND;
    }
    if (st != WASM_DECODE_DONE) return st;
    im->kind = (WasmExternKind)kind;
    d->phase = PH_IMPORT;
    return WASM_DECODE_MORE;
}

/* §5.5.6's funcsec is list(typeidx): one function's type index. */
static WasmDecodeStatus read_function(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmDecodeStatus st;
    uint32_t x;

    if (d->items_left == 0) return end_section(d);
    d->items_left--;
    st = read_u32(d, &x);
    if (st != WASM_DECODE_DONE) return st;
    if (!POOL_ROOM(m->func_types, m->n_func_types, m->cap_func_types)) return WASM_DECODE_NO_ROOM;
    m->func_types[m->n_func_types++] = x;
    return WASM_DECODE_MORE;
}

/* §5.4.2's open structured instructions, a byte each on the decoder's block stack. */
enum { BK_BLOCK = 1, BK_IF_THEN, BK_IF_ELSE };
/* Which list immediate PH_EXPR_LIST reads: br_table's labels, select's value types, try_table's catches. */
enum { IL_LABELS = 1, IL_VALTYPES, IL_CATCHES };

static WasmDecodeStatus push_block(WasmModuleDecoder *d, uint8_t kind)
{
    if (!POOL_ROOM(d->blocks, d->n_blocks, d->cap_blocks)) return WASM_DECODE_NO_ROOM;
    d->blocks[d->n_blocks++] = kind;
    return WASM_DECODE_MORE;
}

/* Begin a §5.4.10 expr at the current byte; the decode goes on at `next` once its final 0x0B is read. */
static void begin_expr(WasmModuleDecoder *d, int next)
{
    d->expr_offset = d->pos;
    d->expr_next = next;
    d->n_blocks = 0;
    d->phase = PH_EXPR;
}

/* §5.4.2's blocktype: a non-negative s33 is a type index, else 0x40 is the empty type, else a value type, each
   tried from the same byte in decode.ml's order. */
static WasmDecodeStatus read_block_type(WasmModuleDecoder *d)
{
    size_t at = d->pos;
    int64_t x;
    WasmValType t;

    if (read_sN(d, 33, &x) == WASM_DECODE_DONE && x >= 0) return WASM_DECODE_DONE;
    d->pos = at;
    if (d->pos < d->in_len && d->in[d->pos] == 0x40) {
        d->pos++;
        return WASM_DECODE_DONE;
    }
    return read_val_type(d, &t);
}

/* §5.4.5's memarg: a u32 below 2^7 whose bit 6 says a memory index follows, then a u64 offset. */
static WasmDecodeStatus read_memarg(WasmModuleDecoder *d)
{
    uint32_t flags, x;
    uint64_t offset;
    WasmDecodeStatus st = read_u32(d, &flags);

    if (st != WASM_DECODE_DONE) return st;
    if (flags >= 0x80) return WASM_DECODE_MEMOP_FLAGS;
    if ((flags & 0x40) != 0 && (st = read_u32(d, &x)) != WASM_DECODE_DONE) return st;
    return read_uN(d, 64, &offset);
}

static WasmDecodeStatus read_bytes(WasmModuleDecoder *d, size_t n)
{
    if (n > d->in_len - d->pos) return WASM_DECODE_UNEXPECTED_END;
    d->pos += n;
    return WASM_DECODE_DONE;
}

static WasmDecodeStatus begin_list(WasmModuleDecoder *d, int kind)
{
    WasmDecodeStatus st = read_list_length(d, &d->list_left);

    if (st != WASM_DECODE_DONE) return st;
    d->list_kind = kind;
    d->phase = PH_EXPR_LIST;
    return WASM_DECODE_MORE;
}

/* One instruction's immediates by opcode_immediates.h's shape. A shape that ends in a list hands the rest of the
   instruction to PH_EXPR_LIST. */
static WasmDecodeStatus read_immediates(WasmModuleDecoder *d, WasmImmediate shape)
{
    WasmDecodeStatus st;
    WasmValType t;
    uint64_t u;
    int64_t s;
    uint32_t x;
    uint8_t flags;
    int i;

    switch (shape) {
    case WASM_IMM_ILLEGAL:
        return WASM_DECODE_ILLEGAL_OPCODE;
    case WASM_IMM_NONE:
        return WASM_DECODE_MORE;
    case WASM_IMM_BLOCK:
    case WASM_IMM_IF:
        st = read_block_type(d);
        if (st != WASM_DECODE_DONE) return st;
        return push_block(d, shape == WASM_IMM_IF ? BK_IF_THEN : BK_BLOCK);
    case WASM_IMM_TRY_TABLE:
        st = read_block_type(d);
        if (st != WASM_DECODE_DONE) return st;
        return begin_list(d, IL_CATCHES);
    case WASM_IMM_IDX:
    case WASM_IMM_IDX2:
        st = read_u32(d, &x);
        if (st == WASM_DECODE_DONE && shape == WASM_IMM_IDX2) st = read_u32(d, &x);
        break;
    case WASM_IMM_BR_TABLE:
        return begin_list(d, IL_LABELS);
    case WASM_IMM_SELECT_T:
        return begin_list(d, IL_VALTYPES);
    case WASM_IMM_MEMOP:
    case WASM_IMM_MEMOP_LANE:
        st = read_memarg(d);
        if (st == WASM_DECODE_DONE && shape == WASM_IMM_MEMOP_LANE) st = read_uN(d, 8, &u);
        break;
    case WASM_IMM_LANE:
        st = read_uN(d, 8, &u);
        break;
    case WASM_IMM_SHUFFLE:
        for (i = 0, st = WASM_DECODE_DONE; i < 16 && st == WASM_DECODE_DONE; i++) st = read_uN(d, 8, &u);
        break;
    case WASM_IMM_S32:
    case WASM_IMM_S64:
        st = read_sN(d, shape == WASM_IMM_S32 ? 32 : 64, &s);
        break;
    case WASM_IMM_F32: st = read_bytes(d, 4); break;
    case WASM_IMM_F64: st = read_bytes(d, 8); break;
    case WASM_IMM_V128: st = read_bytes(d, 16); break;
    case WASM_IMM_HEAPTYPE:
        st = read_heap_type(d, &t);
        break;
    case WASM_IMM_BR_ON_CAST:
        if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
        flags = d->in[d->pos++];
        if ((flags & 0xfc) != 0) return WASM_DECODE_BR_ON_CAST_FLAGS;
        st = read_u32(d, &x);
        if (st == WASM_DECODE_DONE) st = read_heap_type(d, &t);
        if (st == WASM_DECODE_DONE) st = read_heap_type(d, &t);
        break;
    default:
        CHECK_FAIL("opcode_immediates.h names an immediate shape module_decoder.c does not read");
    }
    return st == WASM_DECODE_DONE ? WASM_DECODE_MORE : st;
}

/* One §5.4 instruction of an expr: 0x0B closes the innermost block or the expr itself, 0x05 turns the innermost
   if to its else arm (anywhere else it is a missing 0x0B, as decode.ml's end_ reads it), and any other opcode
   is read with its immediates, under its prefix (0xFB, 0xFC, 0xFD with a u32 sub-opcode) where it has one. */
static WasmDecodeStatus read_instruction(WasmModuleDecoder *d)
{
    WasmDecodeStatus st;
    uint32_t sub;
    uint8_t op;

    if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
    op = d->in[d->pos++];
    if (op == 0x0B) {
        if (d->n_blocks > 0) {
            d->n_blocks--;
            return WASM_DECODE_MORE;
        }
        d->phase = d->expr_next;
        return WASM_DECODE_MORE;
    }
    if (op == 0x05) {
        if (d->n_blocks == 0 || d->blocks[d->n_blocks - 1] != BK_IF_THEN) return WASM_DECODE_END_EXPECTED;
        d->blocks[d->n_blocks - 1] = BK_IF_ELSE;
        return WASM_DECODE_MORE;
    }
    d->illegal_prefix = 0;
    d->illegal_opcode = op;
    if (op != 0xFB && op != 0xFC && op != 0xFD) return read_immediates(d, (WasmImmediate)WASM_IMM_ONE[op]);
    st = read_u32(d, &sub);
    if (st != WASM_DECODE_DONE) return st;
    d->illegal_prefix = op;
    d->illegal_opcode = sub;
    /* §5.5.17's dataidx(func*): memory.init and data.drop (0xFC 8, 9), array.new_data and array.init_data
       (0xFB 9, 18) name a data segment. */
    if (d->in_code && ((op == 0xFC && (sub == 8 || sub == 9)) || (op == 0xFB && (sub == 9 || sub == 18))))
        d->code_names_data = true;
    if (op == 0xFB) return read_immediates(d, sub < sizeof WASM_IMM_FB ? (WasmImmediate)WASM_IMM_FB[sub] : WASM_IMM_ILLEGAL);
    if (op == 0xFC) return read_immediates(d, sub < sizeof WASM_IMM_FC ? (WasmImmediate)WASM_IMM_FC[sub] : WASM_IMM_ILLEGAL);
    return read_immediates(d, sub < sizeof WASM_IMM_FD ? (WasmImmediate)WASM_IMM_FD[sub] : WASM_IMM_ILLEGAL);
}

/* One element of an instruction's list immediate, then what follows the list: br_table's default label, or the
   try_table's block. */
static WasmDecodeStatus read_instruction_list(WasmModuleDecoder *d)
{
    WasmDecodeStatus st = WASM_DECODE_DONE;
    WasmValType t;
    uint32_t x;
    uint8_t c;

    if (d->list_left == 0) {
        d->phase = PH_EXPR;
        if (d->list_kind == IL_LABELS) {
            st = read_u32(d, &x);
            return st == WASM_DECODE_DONE ? WASM_DECODE_MORE : st;
        }
        return d->list_kind == IL_CATCHES ? push_block(d, BK_BLOCK) : WASM_DECODE_MORE;
    }
    d->list_left--;
    switch (d->list_kind) {
    case IL_LABELS:
        st = read_u32(d, &x);
        break;
    case IL_VALTYPES:
        st = read_val_type(d, &t);
        break;
    case IL_CATCHES:
        /* §5.4.2's catch: 0x00 catch and 0x01 catch_ref take a tag and a label, 0x02 catch_all and 0x03
           catch_all_ref a label. */
        if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
        c = d->in[d->pos++];
        if (c > 0x03) return WASM_DECODE_CATCH_CLAUSE;
        st = read_u32(d, &x);
        if (st == WASM_DECODE_DONE && c < 0x02) st = read_u32(d, &x);
        break;
    default:
        CHECK_FAIL("an instruction list immediate of a kind module_decoder.c does not define");
    }
    return st == WASM_DECODE_DONE ? WASM_DECODE_MORE : st;
}

/* §5.5.13's code: a len held to the bytes left, then list(locals), then the body's expr, which must end at
   len. */
static WasmDecodeStatus read_code(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmCode *c;
    uint32_t size;
    WasmDecodeStatus st;

    if (d->items_left == 0) return end_section(d);
    d->items_left--;
    st = read_list_length(d, &size);
    if (st != WASM_DECODE_DONE) return st;
    d->code_end = d->pos + size;
    if (!POOL_ROOM(m->codes, m->n_codes, m->cap_codes)) return WASM_DECODE_NO_ROOM;
    c = &m->codes[m->n_codes++];
    memset(c, 0, sizeof *c);
    c->locals = m->n_locals;
    d->locals_total = 0;
    st = read_list_length(d, &d->list_left);
    if (st != WASM_DECODE_DONE) return st;
    d->phase = PH_LOCALS;
    return WASM_DECODE_MORE;
}

/* One locals entry: a u32 count and a value type. The entries' counts are summed and held below 2^32 once all
   are read, as decode.ml's `locals` does. */
static WasmDecodeStatus read_locals(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmLocals l;
    WasmDecodeStatus st;

    if (d->list_left == 0) {
        if (d->locals_total >= ((uint64_t)1 << 32)) return WASM_DECODE_TOO_MANY_LOCALS;
        d->in_code = true;
        begin_expr(d, PH_CODE_END);
        return WASM_DECODE_MORE;
    }
    d->list_left--;
    st = read_u32(d, &l.count);
    if (st == WASM_DECODE_DONE) st = read_val_type(d, &l.type);
    if (st != WASM_DECODE_DONE) return st;
    if (!POOL_ROOM(m->locals, m->n_locals, m->cap_locals)) return WASM_DECODE_NO_ROOM;
    m->locals[m->n_locals++] = l;
    m->codes[m->n_codes - 1].n_locals++;
    d->locals_total += l.count;
    return WASM_DECODE_MORE;
}

static WasmDecodeStatus read_code_end(WasmModuleDecoder *d)
{
    WasmCode *c = &d->module.codes[d->module.n_codes - 1];

    DCHECK(d->pos > d->expr_offset && d->in[d->pos - 1] == 0x0B,
           "a WebAssembly function body ended somewhere other than after its 0x0B");
    d->in_code = false;
    if (d->pos != d->code_end) return WASM_DECODE_SECTION_SIZE;
    c->body_offset = d->expr_offset;
    c->body_size = d->pos - 1 - d->expr_offset;
    d->phase = PH_CODE;
    return WASM_DECODE_MORE;
}

/* §5.5.9's global: a §5.3.11 global type in one step, then its expr. */
static WasmDecodeStatus read_global(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmGlobal *g;
    WasmDecodeStatus st;

    if (d->items_left == 0) return end_section(d);
    d->items_left--;
    if (!POOL_ROOM(m->globals, m->n_globals, m->cap_globals)) return WASM_DECODE_NO_ROOM;
    g = &m->globals[m->n_globals++];
    memset(g, 0, sizeof *g);
    st = read_val_type(d, &g->type);
    if (st != WASM_DECODE_DONE) return st;
    if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
    if (d->in[d->pos] > 0x01) return WASM_DECODE_MUTABILITY;
    g->mutable_ = d->in[d->pos++] == 0x01;
    begin_expr(d, PH_GLOBAL_END);
    return WASM_DECODE_MORE;
}

static WasmDecodeStatus read_global_end(WasmModuleDecoder *d)
{
    WasmGlobal *g = &d->module.globals[d->module.n_globals - 1];

    DCHECK(d->pos > d->expr_offset && d->in[d->pos - 1] == 0x0B,
           "a WebAssembly global's initializer ended somewhere other than after its 0x0B");
    g->init_offset = d->expr_offset;
    g->init_size = d->pos - 1 - d->expr_offset;
    d->phase = PH_GLOBAL;
    return WASM_DECODE_MORE;
}

/* §5.5.7's table: a §5.3.13 table type, or 0x40 0x00, a table type and an initializer. decode.ml tries the
   second form first and on any failure in it reads the first from the same byte, where 0x40 is no reference
   type: so every refusal inside the second form is "malformed reference type" (table_with_init). */
static WasmDecodeStatus read_table(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmTable *t;
    WasmDecodeStatus st;

    if (d->items_left == 0) return end_section(d);
    d->items_left--;
    if (!POOL_ROOM(m->tables, m->n_tables, m->cap_tables)) return WASM_DECODE_NO_ROOM;
    t = &m->tables[m->n_tables++];
    memset(t, 0, sizeof *t);
    if (d->pos < d->in_len && d->in[d->pos] == 0x40) {
        d->pos++;
        if (d->pos >= d->in_len || d->in[d->pos++] != 0x00) return WASM_DECODE_REF_TYPE;
        st = read_ref_type(d, &t->type);
        if (st == WASM_DECODE_DONE) st = read_limits(d, &t->limits);
        if (st != WASM_DECODE_DONE) return WASM_DECODE_REF_TYPE;
        t->has_init = true;
        d->table_with_init = true;
        begin_expr(d, PH_TABLE_END);
        return WASM_DECODE_MORE;
    }
    st = read_ref_type(d, &t->type);
    if (st == WASM_DECODE_DONE) st = read_limits(d, &t->limits);
    return st == WASM_DECODE_DONE ? WASM_DECODE_MORE : st;
}

/* The end of an expr this file records as a range: the bytes since begin_expr, less the final 0x0B. */
static WasmExpr expr_range(const WasmModuleDecoder *d)
{
    WasmExpr e;

    DCHECK(d->pos > d->expr_offset && d->in[d->pos - 1] == 0x0B,
           "a WebAssembly expr ended somewhere other than after its 0x0B");
    e.offset = d->expr_offset;
    e.size = d->pos - 1 - d->expr_offset;
    return e;
}

static WasmDecodeStatus read_table_end(WasmModuleDecoder *d)
{
    d->module.tables[d->module.n_tables - 1].init = expr_range(d);
    d->table_with_init = false;
    d->phase = PH_TABLE;
    return WASM_DECODE_MORE;
}

/* §5.5.8's mem: a §5.3.12 memory type, its limits. */
static WasmDecodeStatus read_memory(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmDecodeStatus st;

    if (d->items_left == 0) return end_section(d);
    d->items_left--;
    if (!POOL_ROOM(m->memories, m->n_memories, m->cap_memories)) return WASM_DECODE_NO_ROOM;
    st = read_limits(d, &m->memories[m->n_memories++]);
    return st == WASM_DECODE_DONE ? WASM_DECODE_MORE : st;
}

/* §5.5.16's tag: a §5.3.10 tag type, 0x00 and a type index. */
static WasmDecodeStatus read_tag(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmDecodeStatus st;
    uint32_t x;

    if (d->items_left == 0) return end_section(d);
    d->items_left--;
    if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
    if (d->in[d->pos++] != 0x00) return WASM_DECODE_ZERO_BYTE;
    st = read_u32(d, &x);
    if (st != WASM_DECODE_DONE) return st;
    if (!POOL_ROOM(m->tags, m->n_tags, m->cap_tags)) return WASM_DECODE_NO_ROOM;
    m->tags[m->n_tags++] = x;
    return WASM_DECODE_MORE;
}

/* §5.5.10's export: a §5.2.4 name, then a §5.5.1 externidx. */
static WasmDecodeStatus read_export(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmExport *e;
    uint32_t size;
    WasmDecodeStatus st;

    if (d->items_left == 0) return end_section(d);
    d->items_left--;
    if (!POOL_ROOM(m->exports, m->n_exports, m->cap_exports)) return WASM_DECODE_NO_ROOM;
    e = &m->exports[m->n_exports++];
    memset(e, 0, sizeof *e);
    st = begin_name(d, PH_EXPORT_DESC, &e->name_offset, &size);
    if (st == WASM_DECODE_MORE) e->name_size = size;
    return st;
}

static WasmDecodeStatus read_export_desc(WasmModuleDecoder *d)
{
    WasmExport *e = &d->module.exports[d->module.n_exports - 1];
    WasmDecodeStatus st;
    uint8_t kind;

    if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
    kind = d->in[d->pos++];
    if (kind > WASM_EXTERN_TAG) return WASM_DECODE_EXPORT_KIND;
    e->kind = (WasmExternKind)kind;
    st = read_u32(d, &e->index);
    if (st != WASM_DECODE_DONE) return st;
    d->phase = PH_EXPORT;
    return WASM_DECODE_MORE;
}

/* §5.5.12's elem, by its u32 form: bit 0 passive or declarative, bit 1 an explicit table index (active) or
   declarative (otherwise), bit 2 a reference type and exprs instead of an elemkind and function indices. This
   step reads the form, an active segment's table index, and begins its offset expr. */
static WasmDecodeStatus read_elem(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmElem *e;
    WasmDecodeStatus st;
    uint32_t form;

    if (d->items_left == 0) return end_section(d);
    d->items_left--;
    st = read_u32(d, &form);
    if (st != WASM_DECODE_DONE) return st;
    if (form > 7) return WASM_DECODE_ELEM_SEGMENT_KIND;
    if (!POOL_ROOM(m->elems, m->n_elems, m->cap_elems)) return WASM_DECODE_NO_ROOM;
    e = &m->elems[m->n_elems++];
    memset(e, 0, sizeof *e);
    d->elem_form = form;
    e->exprs = (form & 4) != 0;
    e->mode = (form & 1) == 0 ? WASM_SEGMENT_ACTIVE : (form & 2) != 0 ? WASM_SEGMENT_DECLARATIVE
                                                                       : WASM_SEGMENT_PASSIVE;
    if (e->mode == WASM_SEGMENT_ACTIVE && (form & 2) != 0) {
        st = read_u32(d, &e->table);
        if (st != WASM_DECODE_DONE) return st;
    }
    if (e->mode == WASM_SEGMENT_ACTIVE) begin_expr(d, PH_ELEM_TYPE);
    else d->phase = PH_ELEM_TYPE;
    return WASM_DECODE_MORE;
}

/* The segment's element type (forms 0 and 4 state none: ref func and ref null func), then its items' count. */
static WasmDecodeStatus read_elem_type(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmElem *e = &m->elems[m->n_elems - 1];
    WasmDecodeStatus st = WASM_DECODE_DONE;

    if (e->mode == WASM_SEGMENT_ACTIVE) e->offset = expr_range(d);
    e->type.code = WASM_TYPE_REF;
    e->type.heap = WASM_HEAP_FUNC;
    if (d->elem_form == 0 || d->elem_form == 4) {
        e->type.nullable = d->elem_form == 4;
    } else if (!e->exprs) {
        /* §5.5.12's elemkind: 0x00, ref func. */
        if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
        if (d->in[d->pos++] != 0x00) return WASM_DECODE_ELEM_KIND;
    } else {
        st = read_ref_type(d, &e->type);
    }
    if (st == WASM_DECODE_DONE) st = read_list_length(d, &d->elem_left);
    if (st != WASM_DECODE_DONE) return st;
    e->items = e->exprs ? m->n_elem_exprs : m->n_elem_funcs;
    d->phase = PH_ELEM_ITEM;
    return WASM_DECODE_MORE;
}

/* One item: a function index in one step, or an expr, which PH_ELEM_EXPR_END records. */
static WasmDecodeStatus read_elem_item(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmElem *e = &m->elems[m->n_elems - 1];
    WasmDecodeStatus st;
    uint32_t x;

    if (d->elem_left == 0) {
        d->phase = PH_ELEM;
        return WASM_DECODE_MORE;
    }
    d->elem_left--;
    if (e->exprs) {
        begin_expr(d, PH_ELEM_EXPR_END);
        return WASM_DECODE_MORE;
    }
    st = read_u32(d, &x);
    if (st != WASM_DECODE_DONE) return st;
    if (!POOL_ROOM(m->elem_funcs, m->n_elem_funcs, m->cap_elem_funcs)) return WASM_DECODE_NO_ROOM;
    m->elem_funcs[m->n_elem_funcs++] = x;
    e->n_items++;
    return WASM_DECODE_MORE;
}

static WasmDecodeStatus read_elem_expr_end(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;

    if (!POOL_ROOM(m->elem_exprs, m->n_elem_exprs, m->cap_elem_exprs)) return WASM_DECODE_NO_ROOM;
    m->elem_exprs[m->n_elem_exprs++] = expr_range(d);
    m->elems[m->n_elems - 1].n_items++;
    d->phase = PH_ELEM_ITEM;
    return WASM_DECODE_MORE;
}

/* §5.5.14's data, by its u32 form: 0 active in memory 0, 1 passive, 2 active with a memory index. This step reads
   the form and the index, and begins an active segment's offset expr. */
static WasmDecodeStatus read_data(WasmModuleDecoder *d)
{
    WasmModule *m = &d->module;
    WasmData *s;
    WasmDecodeStatus st;
    uint32_t form;

    if (d->items_left == 0) return end_section(d);
    d->items_left--;
    st = read_u32(d, &form);
    if (st != WASM_DECODE_DONE) return st;
    if (form > 2) return WASM_DECODE_DATA_SEGMENT_KIND;
    if (!POOL_ROOM(m->datas, m->n_datas, m->cap_datas)) return WASM_DECODE_NO_ROOM;
    s = &m->datas[m->n_datas++];
    memset(s, 0, sizeof *s);
    s->mode = form == 1 ? WASM_SEGMENT_PASSIVE : WASM_SEGMENT_ACTIVE;
    if (form == 2) {
        st = read_u32(d, &s->memory);
        if (st != WASM_DECODE_DONE) return st;
    }
    if (s->mode == WASM_SEGMENT_ACTIVE) begin_expr(d, PH_DATA_BYTES);
    else d->phase = PH_DATA_BYTES;
    return WASM_DECODE_MORE;
}

/* The segment's bytes: list(byte), held to the bytes left and kept as a range. */
static WasmDecodeStatus read_data_bytes(WasmModuleDecoder *d)
{
    WasmData *s = &d->module.datas[d->module.n_datas - 1];
    WasmDecodeStatus st;
    uint32_t size;

    if (s->mode == WASM_SEGMENT_ACTIVE) s->offset = expr_range(d);
    st = read_byte_length(d, &size);
    if (st != WASM_DECODE_DONE) return st;
    s->bytes_offset = d->pos;
    s->bytes_size = size;
    d->pos += size;
    d->phase = PH_DATA;
    return WASM_DECODE_MORE;
}

WasmDecodeStatus wasm_module_decoder_step(WasmModuleDecoder *d)
{
    WasmDecodeStatus st;

    DCHECK(d != NULL, "a WebAssembly decode step was handed no decoder");
    DCHECK(d->phase != PH_FINISHED, "a WebAssembly decode was stepped after it answered");
    DCHECK(d->pos <= d->in_len, "a WebAssembly decode stands past the end of its bytes");
    switch (d->phase) {
    case PH_PREAMBLE: st = read_preamble(d); break;
    case PH_SECTION:  st = read_section(d); break;
    case PH_NAME:     st = read_name(d); break;
    case PH_TYPE:     st = read_rec_type(d); break;
    case PH_SUBTYPE:  st = read_sub_type(d); break;
    case PH_SUPERS:   st = read_super(d); break;
    case PH_COMPTYPE: st = read_comp_type(d); break;
    case PH_PARAMS:   st = read_param(d); break;
    case PH_RESULTS:  st = read_result(d); break;
    case PH_FIELDS:   st = read_struct_field(d); break;
    case PH_ARRAY_FIELD: st = read_array_field(d); break;
    case PH_CUSTOM_REST: st = read_custom_rest(d); break;
    case PH_IMPORT:   st = read_import(d); break;
    case PH_IMPORT_NAME: st = read_import_name(d); break;
    case PH_IMPORT_DESC: st = read_import_desc(d); break;
    case PH_FUNCTION: st = read_function(d); break;
    case PH_GLOBAL:   st = read_global(d); break;
    case PH_GLOBAL_END: st = read_global_end(d); break;
    case PH_TABLE:    st = read_table(d); break;
    case PH_TABLE_END: st = read_table_end(d); break;
    case PH_MEMORY:   st = read_memory(d); break;
    case PH_TAG:      st = read_tag(d); break;
    case PH_EXPORT:   st = read_export(d); break;
    case PH_EXPORT_DESC: st = read_export_desc(d); break;
    case PH_ELEM:     st = read_elem(d); break;
    case PH_ELEM_TYPE: st = read_elem_type(d); break;
    case PH_ELEM_ITEM: st = read_elem_item(d); break;
    case PH_ELEM_EXPR_END: st = read_elem_expr_end(d); break;
    case PH_DATA:     st = read_data(d); break;
    case PH_DATA_BYTES: st = read_data_bytes(d); break;
    case PH_CODE:     st = read_code(d); break;
    case PH_LOCALS:   st = read_locals(d); break;
    case PH_CODE_END: st = read_code_end(d); break;
    case PH_EXPR:     st = read_instruction(d); break;
    case PH_EXPR_LIST: st = read_instruction_list(d); break;
    default:
        CHECK_FAIL("a WebAssembly decode stands in a phase this file does not define");
    }
    if (d->table_with_init && st != WASM_DECODE_MORE && st != WASM_DECODE_NO_ROOM) st = WASM_DECODE_REF_TYPE;
    if (st != WASM_DECODE_MORE) d->phase = PH_FINISHED;
    return st;
}

WasmDecodeStatus wasm_module_decoder_run(WasmModuleDecoder *d)
{
    WasmDecodeStatus st;

    do st = wasm_module_decoder_step(d);
    while (st == WASM_DECODE_MORE);
    return st;
}

size_t wasm_module_decoder_section_count(const WasmModuleDecoder *d)
{
    DCHECK(d != NULL, "a WebAssembly decode's sections were asked of no decoder");
    return d->n_sections;
}

const WasmSection *wasm_module_decoder_section(const WasmModuleDecoder *d, size_t i)
{
    DCHECK(d != NULL && i < d->n_sections, "a WebAssembly section was asked for past the sections read");
    return &d->sections[i];
}

void wasm_module_decoder_free(WasmModuleDecoder *d)
{
    DCHECK(d != NULL, "a WebAssembly decode was freed with no decoder");
    free(d->sections);
    d->sections = NULL;
    d->n_sections = 0;
    d->cap_sections = 0;
    free(d->blocks);
    d->blocks = NULL;
    d->n_blocks = 0;
    d->cap_blocks = 0;
    wasm_module_free(&d->module);
}

void wasm_module_decoder_message(const WasmModuleDecoder *d, WasmDecodeStatus st, char *buf, size_t cap)
{
    const char *m = wasm_decode_status_message(st);

    DCHECK(d != NULL && buf != NULL && cap > 0, "a WebAssembly decode's message was asked with nowhere to write it");
    if (m == NULL) m = "";
    if (st != WASM_DECODE_ILLEGAL_OPCODE) snprintf(buf, cap, "%s", m);
    else if (d->illegal_prefix == 0 || d->illegal_prefix == 0xFD)
        snprintf(buf, cap, "%s %02x", m, (unsigned)d->illegal_opcode);
    else snprintf(buf, cap, "%s %02x %02x", m, (unsigned)d->illegal_prefix, (unsigned)d->illegal_opcode);
}

const WasmModule *wasm_module_decoder_module(const WasmModuleDecoder *d)
{
    DCHECK(d != NULL, "a WebAssembly decode's module was asked of no decoder");
    return &d->module;
}

const char *wasm_decode_status_message(WasmDecodeStatus st)
{
    switch (st) {
    case WASM_DECODE_DONE:
    case WASM_DECODE_MORE:                 return NULL;
    case WASM_DECODE_UNEXPECTED_END:       return "unexpected end of section or function";
    case WASM_DECODE_MAGIC:                return "magic header not detected";
    case WASM_DECODE_VERSION:              return "unknown binary version";
    case WASM_DECODE_SECTION_ID:           return "malformed section id";
    case WASM_DECODE_SECTION_ORDER:        return "unexpected content after last section";
    case WASM_DECODE_INTEGER_TOO_LONG:     return "integer representation too long";
    case WASM_DECODE_INTEGER_TOO_LARGE:    return "integer too large";
    case WASM_DECODE_LENGTH_OUT_OF_BOUNDS: return "length out of bounds";
    case WASM_DECODE_UTF8:                 return "malformed UTF-8 encoding";
    case WASM_DECODE_SECTION_SIZE:         return "section size mismatch";
    case WASM_DECODE_REF_TYPE:             return "malformed reference type";
    case WASM_DECODE_HEAP_TYPE:            return "malformed heap type";
    case WASM_DECODE_STORAGE_TYPE:         return "malformed storage type";
    case WASM_DECODE_COMP_TYPE:            return "malformed definition type";
    case WASM_DECODE_MUTABILITY:           return "malformed mutability";
    case WASM_DECODE_IMPORT_KIND:          return "malformed import kind";
    case WASM_DECODE_LIMITS_FLAGS:         return "malformed limits flags";
    case WASM_DECODE_ZERO_BYTE:            return "zero byte expected";
    case WASM_DECODE_END_EXPECTED:         return "END opcode expected";
    case WASM_DECODE_ILLEGAL_OPCODE:       return "illegal opcode";
    case WASM_DECODE_MEMOP_FLAGS:          return "malformed memop flags";
    case WASM_DECODE_BR_ON_CAST_FLAGS:     return "malformed br_on_cast flags";
    case WASM_DECODE_CATCH_CLAUSE:         return "malformed catch clause";
    case WASM_DECODE_TOO_MANY_LOCALS:      return "too many locals";
    case WASM_DECODE_FUNC_CODE_LENGTHS:    return "function and code section have inconsistent lengths";
    case WASM_DECODE_DATA_COUNT_REQUIRED:  return "data count section required";
    case WASM_DECODE_DATA_COUNT_LENGTH:    return "data count and data section have inconsistent lengths";
    case WASM_DECODE_EXPORT_KIND:          return "malformed export kind";
    case WASM_DECODE_ELEM_KIND:            return "malformed element kind";
    case WASM_DECODE_ELEM_SEGMENT_KIND:    return "malformed elements segment kind";
    case WASM_DECODE_DATA_SEGMENT_KIND:    return "malformed data segment kind";
    case WASM_DECODE_NO_ROOM:              return "out of memory";
    }
    CHECK_FAIL("a WebAssembly decode status this file does not define");
}
