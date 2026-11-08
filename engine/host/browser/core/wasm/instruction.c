/* WebAssembly Core Specification (Release 3.0) §5.4 "Instructions". See instruction.h. */
#include "core/wasm/instruction.h"

#include <string.h>

#include "check.h"

WasmDecodeStatus wasm_read_opcode(WasmDecoder *r, uint32_t *prefix, uint32_t *opcode)
{
    uint32_t sub;
    uint8_t op;
    WasmDecodeStatus st;

    if (r->pos >= r->len) return WASM_DECODE_UNEXPECTED_END;
    op = r->in[r->pos++];
    if (op != 0xFB && op != 0xFC && op != 0xFD) {
        *prefix = 0;
        *opcode = op;
        return WASM_DECODE_DONE;
    }
    st = wasm_read_u32(r, &sub);
    if (st != WASM_DECODE_DONE) return st;
    *prefix = op;
    *opcode = sub;
    return WASM_DECODE_DONE;
}

/* §5.4.2's blocktype: a non-negative s33 is a type index, else 0x40 is the empty type, else a value type, each
   tried from the same byte in decode.ml's order. */
static WasmDecodeStatus read_block_type(WasmDecoder *r, WasmInstruction *in)
{
    size_t at = r->pos;
    int64_t x;

    if (wasm_read_sN(r, 33, &x) == WASM_DECODE_DONE && x >= 0) {
        in->block_kind = WASM_BLOCK_INDEX;
        in->block_index = (uint32_t)x;
        return WASM_DECODE_DONE;
    }
    r->pos = at;
    if (r->pos < r->len && r->in[r->pos] == 0x40) {
        r->pos++;
        in->block_kind = WASM_BLOCK_EMPTY;
        return WASM_DECODE_DONE;
    }
    in->block_kind = WASM_BLOCK_VALUE;
    return wasm_read_val_type(r, &in->block_type);
}

/* §5.4.5's memarg: a u32 below 2^7 whose bit 6 says a memory index follows, then a u64 offset. Its low six bits
   are the alignment exponent. */
static WasmDecodeStatus read_memarg(WasmDecoder *r, WasmInstruction *in)
{
    uint32_t flags;
    WasmDecodeStatus st = wasm_read_u32(r, &flags);

    if (st != WASM_DECODE_DONE) return st;
    if (flags >= 0x80) return WASM_DECODE_MEMOP_FLAGS;
    in->align = flags & 0x3f;
    if ((flags & 0x40) != 0 && (st = wasm_read_u32(r, &in->memory)) != WASM_DECODE_DONE) return st;
    return wasm_read_uN(r, 64, &in->offset);
}

static WasmDecodeStatus read_raw(WasmDecoder *r, WasmInstruction *in, size_t n)
{
    in->bytes = r->pos;
    return wasm_read_bytes(r, n);
}

static WasmImmediate shape_of(uint32_t prefix, uint32_t opcode)
{
    switch (prefix) {
    case 0:
        DCHECKF(opcode < 0x100, "a one-byte WebAssembly opcode was read as %u", (unsigned)opcode);
        return (WasmImmediate)WASM_IMM_ONE[opcode];
    case 0xFB: return opcode < sizeof WASM_IMM_FB ? (WasmImmediate)WASM_IMM_FB[opcode] : WASM_IMM_ILLEGAL;
    case 0xFC: return opcode < sizeof WASM_IMM_FC ? (WasmImmediate)WASM_IMM_FC[opcode] : WASM_IMM_ILLEGAL;
    case 0xFD: return opcode < sizeof WASM_IMM_FD ? (WasmImmediate)WASM_IMM_FD[opcode] : WASM_IMM_ILLEGAL;
    default: break;
    }
    CHECK_FAIL("a WebAssembly opcode under a prefix wasm_read_opcode does not read");
}

WasmDecodeStatus wasm_read_immediates(WasmDecoder *r, uint32_t prefix, uint32_t opcode, WasmInstruction *out)
{
    WasmInstruction in;
    WasmDecodeStatus st = WASM_DECODE_DONE;
    uint64_t u = 0;
    int i;

    memset(&in, 0, sizeof in);
    in.prefix = prefix;
    in.opcode = opcode;
    in.shape = shape_of(prefix, opcode);
    switch (in.shape) {
    case WASM_IMM_ILLEGAL:
        return WASM_DECODE_ILLEGAL_OPCODE;
    case WASM_IMM_NONE:
        break;
    case WASM_IMM_BLOCK:
    case WASM_IMM_IF:
        st = read_block_type(r, &in);
        break;
    case WASM_IMM_TRY_TABLE:
        st = read_block_type(r, &in);
        if (st == WASM_DECODE_DONE) st = wasm_read_list_length(r, &in.list_count);
        break;
    case WASM_IMM_IDX:
    case WASM_IMM_IDX2:
        st = wasm_read_u32(r, &in.index[0]);
        if (st == WASM_DECODE_DONE && in.shape == WASM_IMM_IDX2) st = wasm_read_u32(r, &in.index[1]);
        break;
    case WASM_IMM_BR_TABLE:
    case WASM_IMM_SELECT_T:
        st = wasm_read_list_length(r, &in.list_count);
        break;
    case WASM_IMM_MEMOP:
    case WASM_IMM_MEMOP_LANE:
        st = read_memarg(r, &in);
        if (st == WASM_DECODE_DONE && in.shape == WASM_IMM_MEMOP_LANE) st = wasm_read_uN(r, 8, &u);
        in.lane = (uint8_t)u;
        break;
    case WASM_IMM_LANE:
        st = wasm_read_uN(r, 8, &u);
        in.lane = (uint8_t)u;
        break;
    case WASM_IMM_SHUFFLE:
        in.bytes = r->pos;
        for (i = 0; i < 16 && st == WASM_DECODE_DONE; i++) st = wasm_read_uN(r, 8, &u);
        break;
    case WASM_IMM_S32:
    case WASM_IMM_S64:
        st = wasm_read_sN(r, in.shape == WASM_IMM_S32 ? 32 : 64, &in.value);
        break;
    case WASM_IMM_F32: st = read_raw(r, &in, 4); break;
    case WASM_IMM_F64: st = read_raw(r, &in, 8); break;
    case WASM_IMM_V128: st = read_raw(r, &in, 16); break;
    case WASM_IMM_HEAPTYPE:
        in.heap[0].code = WASM_TYPE_REF;
        st = wasm_read_heap_type(r, &in.heap[0]);
        break;
    case WASM_IMM_BR_ON_CAST:
        if (r->pos >= r->len) return WASM_DECODE_UNEXPECTED_END;
        in.cast_flags = r->in[r->pos++];
        if ((in.cast_flags & 0xfc) != 0) return WASM_DECODE_BR_ON_CAST_FLAGS;
        in.heap[0].code = in.heap[1].code = WASM_TYPE_REF;
        st = wasm_read_u32(r, &in.index[0]);
        if (st == WASM_DECODE_DONE) st = wasm_read_heap_type(r, &in.heap[0]);
        if (st == WASM_DECODE_DONE) st = wasm_read_heap_type(r, &in.heap[1]);
        break;
    default:
        CHECK_FAIL("opcode_immediates.h names an immediate shape instruction.c does not read");
    }
    if (st == WASM_DECODE_DONE) *out = in;
    return st;
}

WasmDecodeStatus wasm_read_label(WasmDecoder *r, uint32_t *out)
{
    return wasm_read_u32(r, out);
}

/* §5.4.2's catch: 0x00 catch and 0x01 catch_ref take a tag and a label, 0x02 catch_all and 0x03 catch_all_ref a
   label. */
WasmDecodeStatus wasm_read_catch(WasmDecoder *r, WasmCatch *out)
{
    WasmCatch c = { 0 };
    WasmDecodeStatus st = WASM_DECODE_DONE;
    uint8_t b;

    if (r->pos >= r->len) return WASM_DECODE_UNEXPECTED_END;
    b = r->in[r->pos++];
    if (b > WASM_CATCH_ALL_REF) return WASM_DECODE_CATCH_CLAUSE;
    c.kind = (WasmCatchKind)b;
    if (b == WASM_CATCH || b == WASM_CATCH_REF) st = wasm_read_u32(r, &c.tag);
    if (st == WASM_DECODE_DONE) st = wasm_read_label(r, &c.label);
    if (st == WASM_DECODE_DONE) *out = c;
    return st;
}
