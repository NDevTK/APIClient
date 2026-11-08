/* WebAssembly Core Specification (Release 3.0) §5.4 "Instructions": one instruction's opcode and immediates,
 * read from binary bytes by every stage that reads code in place: the module decoder (module_decoder.h), which
 * refuses what §5.4 does not define, and the validator, which reads bytes the decoder accepted.
 * A list immediate (br_table's labels, select's value types, try_table's catches) is not read with its
 * instruction: the instruction carries the list's length and the cursor stands at its first element, so a
 * caller reads one element per step with wasm_read_label, wasm_read_val_type and wasm_read_catch, and then
 * br_table's default label. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_INSTRUCTION_H
#define ENGINE_HOST_BROWSER_CORE_WASM_INSTRUCTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/wasm/decoder.h"
#include "core/wasm/opcode_immediates.h"

/* §5.4.2's blocktype: empty (0x40), one value type, or a type index. */
typedef enum { WASM_BLOCK_EMPTY = 0, WASM_BLOCK_VALUE, WASM_BLOCK_INDEX } WasmBlockKind;

/* §5.4.2's catch clause kinds, by their byte. */
typedef enum {
    WASM_CATCH = 0x00, WASM_CATCH_REF = 0x01, WASM_CATCH_ALL = 0x02, WASM_CATCH_ALL_REF = 0x03
} WasmCatchKind;

typedef struct {
    WasmCatchKind kind;
    uint32_t tag;      /* catch and catch_ref only */
    uint32_t label;
} WasmCatch;

/* One instruction. `prefix` is 0 or the 0xFB, 0xFC or 0xFD byte, and `opcode` the byte or the u32 under it.
   Which immediates are set is `shape`'s: IDX `index[0]`, IDX2 both (call_indirect's type then table); BLOCK,
   IF and TRY_TABLE the blocktype; MEMOP the memarg, MEMOP_LANE and LANE also `lane`; S32 and S64 `value`; F32,
   F64, V128 and SHUFFLE `bytes`, the offset of their raw bytes; HEAPTYPE `heap[0]` (a reference type's heap
   fields); BR_ON_CAST `cast_flags`, `index[0]` (the label) and both heap types; BR_TABLE, SELECT_T and
   TRY_TABLE `list_count`. Every other field is 0. */
typedef struct {
    uint32_t prefix, opcode;
    WasmImmediate shape;
    uint32_t index[2];
    WasmBlockKind block_kind;
    WasmValType block_type;
    uint32_t block_index;
    uint32_t align;
    uint32_t memory;
    uint64_t offset;
    uint8_t lane;
    int64_t value;
    size_t bytes;
    uint8_t cast_flags;
    WasmValType heap[2];
    uint32_t list_count;
} WasmInstruction;

/* An opcode: a byte, and under the 0xFB, 0xFC and 0xFD prefixes a u32 after it. */
WasmDecodeStatus wasm_read_opcode(WasmDecoder *r, uint32_t *prefix, uint32_t *opcode);
/* The immediates opcode_immediates.h gives the opcode, into `*out` with its opcode and shape. An opcode §5.4
   does not define, 0x05 (else) and 0x0B (end) among them, is WASM_DECODE_ILLEGAL_OPCODE. */
WasmDecodeStatus wasm_read_immediates(WasmDecoder *r, uint32_t prefix, uint32_t opcode, WasmInstruction *out);
/* One element of a list immediate: a label (§5.4.1's labelidx), or a catch clause. */
WasmDecodeStatus wasm_read_label(WasmDecoder *r, uint32_t *out);
WasmDecodeStatus wasm_read_catch(WasmDecoder *r, WasmCatch *out);

#endif
