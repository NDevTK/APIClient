/* WebAssembly Core Specification (Release 3.0) §5.2 and §5.3's readers. See decoder.h. */
#include "core/wasm/decoder.h"

#include <string.h>

#include "check.h"

/* §5.2.2's uN: unsigned LEB128 in at most ceil(N/7) bytes, whose last byte sets no bit beyond the Nth. The
   checks run in the reference interpreter's order: the length before the read, the range after it. */
WasmDecodeStatus wasm_read_uN(WasmDecoder *r, int n, uint64_t *out)
{
    uint64_t v = 0;
    unsigned shift = 0;

    for (;;) {
        uint8_t b;

        if (n <= 0) return WASM_DECODE_INTEGER_TOO_LONG;
        if (r->pos >= r->len) return WASM_DECODE_UNEXPECTED_END;
        b = r->in[r->pos++];
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
WasmDecodeStatus wasm_read_u32(WasmDecoder *r, uint32_t *out)
{
    uint64_t v;
    WasmDecodeStatus st = wasm_read_uN(r, 32, &v);

    if (st == WASM_DECODE_DONE) *out = (uint32_t)v;
    return st;
}

/* §5.2.2's sN, as the reference interpreter reads it: at most ceil(N/7) bytes, and a last byte whose bits beyond
   N are all copies of the sign bit. */
WasmDecodeStatus wasm_read_sN(WasmDecoder *r, int n, int64_t *out)
{
    uint64_t v = 0;
    unsigned shift = 0;

    for (;;) {
        uint8_t b;

        if (n <= 0) return WASM_DECODE_INTEGER_TOO_LONG;
        if (r->pos >= r->len) return WASM_DECODE_UNEXPECTED_END;
        b = r->in[r->pos++];
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
WasmDecodeStatus wasm_read_s7_byte(WasmDecoder *r, uint8_t *out)
{
    if (r->pos >= r->len) return WASM_DECODE_UNEXPECTED_END;
    *out = r->in[r->pos++];
    return (*out & 0x80) != 0 ? WASM_DECODE_INTEGER_TOO_LONG : WASM_DECODE_DONE;
}

/* A length (§5.1.3's list count, §5.5.2's and §5.5.13's len) as decode.ml's len32 bounds it: a u32 no larger
   than the bytes left counted from the length's own first byte. */
WasmDecodeStatus wasm_read_list_length(WasmDecoder *r, uint32_t *out)
{
    size_t at = r->pos;
    WasmDecodeStatus st = wasm_read_u32(r, out);

    if (st != WASM_DECODE_DONE) return st;
    return *out > r->len - at ? WASM_DECODE_LENGTH_OUT_OF_BOUNDS : WASM_DECODE_DONE;
}

/* §5.1.3's list(byte) (a name's or a data segment's bytes): its length as len32 bounds it, then the bytes
   themselves, which decode.ml's get_string refuses as an unexpected end when they run past the input. */
WasmDecodeStatus wasm_read_byte_length(WasmDecoder *r, uint32_t *out)
{
    WasmDecodeStatus st = wasm_read_list_length(r, out);

    if (st != WASM_DECODE_DONE) return st;
    return *out > r->len - r->pos ? WASM_DECODE_UNEXPECTED_END : WASM_DECODE_DONE;
}

/* §5.3.3's heaptype: an s33 that is not negative is a type index; otherwise the same byte, read again as an s7,
   must be an abstract heap type. Writes only `heap` and `heap_index`. */
WasmDecodeStatus wasm_read_heap_type(WasmDecoder *r, WasmValType *t)
{
    size_t at = r->pos;
    int64_t x;
    uint8_t b;
    WasmDecodeStatus st = wasm_read_sN(r, 33, &x);

    if (st == WASM_DECODE_DONE && x >= 0) {
        t->heap = 0;
        t->heap_index = (uint32_t)x;
        return WASM_DECODE_DONE;
    }
    r->pos = at;
    st = wasm_read_s7_byte(r, &b);
    if (st != WASM_DECODE_DONE) return st;
    if (b < WASM_HEAP_EXN || b > WASM_HEAP_NOEXN) return WASM_DECODE_HEAP_TYPE;
    t->heap = b;
    return WASM_DECODE_DONE;
}

/* §5.3.5's valtype: a number or vector type's byte, §5.3.4's 0x63 (ref null) or 0x64 (ref) before a heap type,
   or an abstract heap type's byte alone, which is its nullable reference. */
WasmDecodeStatus wasm_read_val_type(WasmDecoder *r, WasmValType *t)
{
    WasmValType v = { 0 };
    uint8_t b;
    WasmDecodeStatus st = wasm_read_s7_byte(r, &b);

    if (st != WASM_DECODE_DONE) return st;
    switch (b) {
    case WASM_TYPE_I32: case WASM_TYPE_I64: case WASM_TYPE_F32: case WASM_TYPE_F64: case WASM_TYPE_V128:
        v.code = b;
        break;
    case 0x63: case 0x64:
        v.code = WASM_TYPE_REF;
        v.nullable = b == 0x63;
        st = wasm_read_heap_type(r, &v);
        if (st != WASM_DECODE_DONE) return st;
        break;
    default:
        if (b < WASM_HEAP_EXN || b > WASM_HEAP_NOEXN) return WASM_DECODE_REF_TYPE;
        v.code = WASM_TYPE_REF;
        v.nullable = true;
        v.heap = b;
    }
    *t = v;
    return WASM_DECODE_DONE;
}

/* §5.3.7's storagetype: a value type, or else the same bytes as a packed type. */
WasmDecodeStatus wasm_read_storage_type(WasmDecoder *r, WasmValType *t)
{
    size_t at = r->pos;
    uint8_t b;
    WasmDecodeStatus st = wasm_read_val_type(r, t);

    if (st == WASM_DECODE_DONE) return st;
    r->pos = at;
    st = wasm_read_s7_byte(r, &b);
    if (st != WASM_DECODE_DONE) return st;
    if (b != WASM_TYPE_I8 && b != WASM_TYPE_I16) return WASM_DECODE_STORAGE_TYPE;
    memset(t, 0, sizeof *t);
    t->code = b;
    return WASM_DECODE_DONE;
}

/* §5.3.4's reftype: an abstract heap type's byte alone, or 0x63/0x64 before a heap type. */
WasmDecodeStatus wasm_read_ref_type(WasmDecoder *r, WasmValType *t)
{
    uint8_t b;
    WasmDecodeStatus st;

    if (r->pos >= r->len) return WASM_DECODE_UNEXPECTED_END;
    b = r->in[r->pos];
    if (b != 0x63 && b != 0x64 && (b < WASM_HEAP_EXN || b > WASM_HEAP_NOEXN)) {
        st = wasm_read_s7_byte(r, &b);
        return st == WASM_DECODE_DONE ? WASM_DECODE_REF_TYPE : st;
    }
    return wasm_read_val_type(r, t);
}

/* §5.3.9's limits: a flag byte (bit 0 a maximum, bit 2 the i64 address type), then u64 minimum and maximum. */
WasmDecodeStatus wasm_read_limits(WasmDecoder *r, WasmLimits *l)
{
    WasmLimits v = { 0 };
    uint8_t flags;
    WasmDecodeStatus st;

    if (r->pos >= r->len) return WASM_DECODE_UNEXPECTED_END;
    flags = r->in[r->pos++];
    if ((flags & 0xfa) != 0) return WASM_DECODE_LIMITS_FLAGS;
    v.has_max = (flags & 0x01) != 0;
    v.addr64 = (flags & 0x04) != 0;
    st = wasm_read_uN(r, 64, &v.min);
    if (st == WASM_DECODE_DONE && v.has_max) st = wasm_read_uN(r, 64, &v.max);
    if (st == WASM_DECODE_DONE) *l = v;
    return st;
}

WasmDecodeStatus wasm_read_bytes(WasmDecoder *r, size_t n)
{
    if (n > r->len - r->pos) return WASM_DECODE_UNEXPECTED_END;
    r->pos += n;
    return WASM_DECODE_DONE;
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
