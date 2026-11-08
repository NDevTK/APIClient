/* WebAssembly Core Specification (Release 3.0) §5.2 "Values" and §5.3 "Types": a cursor over binary bytes and
 * the readers every §5 decoder shares, the module decoder (module_decoder.h) and the instruction reader
 * (instruction.h). Each reader starts at `pos`, advances it past what it read, and writes its output on success
 * only; a refusal is a WasmDecodeStatus carrying the reference interpreter's message, never an assert, because
 * the bytes are input. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_DECODER_H
#define ENGINE_HOST_BROWSER_CORE_WASM_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "core/wasm/wasm_module.h"

typedef enum {
    WASM_DECODE_DONE = 0,             /* the bytes are a §5.5.17 preamble and well framed sections */
    WASM_DECODE_MORE,                 /* a step ended; the state is intact, step again */
    WASM_DECODE_UNEXPECTED_END,       /* the bytes, or a custom section, end inside a production */
    WASM_DECODE_MAGIC,                /* §5.5.17 magic is not 0x00 0x61 0x73 0x6D */
    WASM_DECODE_VERSION,              /* §5.5.17 version is not 0x01 0x00 0x00 0x00 */
    WASM_DECODE_SECTION_ID,           /* §5.5.2: an id byte its table does not list */
    WASM_DECODE_SECTION_ORDER,        /* §5.5.17: a non-custom section repeated or out of the prescribed order */
    WASM_DECODE_INTEGER_TOO_LONG,     /* §5.2.2: a u32 encoded in more than ceil(32/7) bytes */
    WASM_DECODE_INTEGER_TOO_LARGE,    /* §5.2.2: the last byte of a u32 sets a bit beyond 32 */
    WASM_DECODE_LENGTH_OUT_OF_BOUNDS, /* a u32 length runs past the end of the bytes */
    WASM_DECODE_UTF8,                 /* §5.2.4: a name's bytes are not utf8 of any character sequence */
    WASM_DECODE_SECTION_SIZE,         /* §5.5.2: a section's contents end before or after its len */
    WASM_DECODE_REF_TYPE,             /* §5.3.5: a byte that begins no value type */
    WASM_DECODE_HEAP_TYPE,            /* §5.3.3: neither a non-negative s33 nor an abstract heap type */
    WASM_DECODE_STORAGE_TYPE,         /* §5.3.7: a byte that begins no storage type */
    WASM_DECODE_COMP_TYPE,            /* §5.3.7: a byte that begins no composite type */
    WASM_DECODE_MUTABILITY,           /* §5.3.7: a mut byte other than 0x00 and 0x01 */
    WASM_DECODE_IMPORT_KIND,          /* §5.3.14: an external type's byte other than 0x00 to 0x04 */
    WASM_DECODE_LIMITS_FLAGS,         /* §5.3.9: a limits flag byte other than 0x00, 0x01, 0x04 and 0x05 */
    WASM_DECODE_ZERO_BYTE,            /* §5.3.10: a tag type's first byte is not 0x00 */
    WASM_DECODE_END_EXPECTED,         /* §5.4.2: a block, a function body or an expr not closed by 0x0B */
    WASM_DECODE_ILLEGAL_OPCODE,       /* §5.4: an opcode no production defines; see wasm_module_decoder_message */
    WASM_DECODE_MEMOP_FLAGS,          /* §5.4.5: a memarg's first u32 is 2^7 or more */
    WASM_DECODE_BR_ON_CAST_FLAGS,     /* §5.4.2: br_on_cast's flag byte sets a bit above the second */
    WASM_DECODE_CATCH_CLAUSE,         /* §5.4.2: a catch clause's byte other than 0x00 to 0x03 */
    WASM_DECODE_TOO_MANY_LOCALS,      /* §5.5.13: a code entry's locals number 2^32 or more */
    WASM_DECODE_FUNC_CODE_LENGTHS,    /* §5.5.17: the function and code sections' lists differ in length */
    WASM_DECODE_DATA_COUNT_REQUIRED,  /* §5.5.17: a code entry names a data segment and no data count is given */
    WASM_DECODE_DATA_COUNT_LENGTH,    /* §5.5.17: the data count differs from the data section's length */
    WASM_DECODE_EXPORT_KIND,          /* §5.5.1: an external index's byte other than 0x00 to 0x04 */
    WASM_DECODE_ELEM_KIND,            /* §5.5.12: an elemkind byte other than 0x00 */
    WASM_DECODE_ELEM_SEGMENT_KIND,    /* §5.5.12: an element segment's u32 above 7 */
    WASM_DECODE_DATA_SEGMENT_KIND,    /* §5.5.14: a data segment's u32 above 2 */
    WASM_DECODE_NO_ROOM               /* a list could not be allocated; its size is the input's */
} WasmDecodeStatus;

/* The reference interpreter's message for a refusal (interpreter/binary/decode.ml), which the testsuite's
   `assert_malformed` matches by prefix. A static string; NULL for DONE and MORE. */
const char *wasm_decode_status_message(WasmDecodeStatus st);

/* `in` is borrowed; `pos` is at most `len`. */
typedef struct {
    const uint8_t *in;
    size_t len;
    size_t pos;
} WasmDecoder;

WasmDecodeStatus wasm_read_uN(WasmDecoder *r, int n, uint64_t *out);
WasmDecodeStatus wasm_read_u32(WasmDecoder *r, uint32_t *out);
WasmDecodeStatus wasm_read_sN(WasmDecoder *r, int n, int64_t *out);
WasmDecodeStatus wasm_read_s7_byte(WasmDecoder *r, uint8_t *out);
WasmDecodeStatus wasm_read_list_length(WasmDecoder *r, uint32_t *out);
WasmDecodeStatus wasm_read_byte_length(WasmDecoder *r, uint32_t *out);
WasmDecodeStatus wasm_read_bytes(WasmDecoder *r, size_t n);
WasmDecodeStatus wasm_read_heap_type(WasmDecoder *r, WasmValType *t);
WasmDecodeStatus wasm_read_val_type(WasmDecoder *r, WasmValType *t);
WasmDecodeStatus wasm_read_storage_type(WasmDecoder *r, WasmValType *t);
WasmDecodeStatus wasm_read_ref_type(WasmDecoder *r, WasmValType *t);
WasmDecodeStatus wasm_read_limits(WasmDecoder *r, WasmLimits *l);

#endif
