/* WebAssembly Core Specification (Release 3.0) §5.5 "Modules": the binary format's preamble and its sequence of
 * sections, decoded as a step machine over a complete byte sequence.
 * One step reads one production instance: the §5.5.17 preamble, a §5.5.2 section header with a list's count, a
 * code point of a §5.2.4 name, an item of a section's list or of a type's (a §5.3.8 rectype or subtype head, a
 * supertype, a composite head, a value type, a field, an import's or export's name length or index, a function's
 * type index, a table, memory, tag or global type, a segment's head or item, a code entry's len or locals
 * entry), or one §5.4 instruction of an expr with its immediates (an element of a list immediate is a step of
 * its own). The caller owns how many steps a rest covers; the whole state is the WasmModuleDecoder. The bytes are input: every malformed encoding is a
 * WasmDecodeStatus refusal carrying the reference interpreter's message, never an assert. A DONE decode is
 * §5's module_decode: every section and every side condition of §5.5.17 has been read; module_validate is
 * not this file's. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_MODULE_DECODER_H
#define ENGINE_HOST_BROWSER_CORE_WASM_MODULE_DECODER_H

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
    WASM_DECODE_BR_ON_CAST_FLAGS,     /* §5.4.6: br_on_cast's flag byte sets a bit above the second */
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

/* §5.5.2's section ids. */
typedef enum {
    WASM_SECTION_CUSTOM = 0, WASM_SECTION_TYPE = 1, WASM_SECTION_IMPORT = 2, WASM_SECTION_FUNCTION = 3,
    WASM_SECTION_TABLE = 4, WASM_SECTION_MEMORY = 5, WASM_SECTION_GLOBAL = 6, WASM_SECTION_EXPORT = 7,
    WASM_SECTION_START = 8, WASM_SECTION_ELEMENT = 9, WASM_SECTION_CODE = 10, WASM_SECTION_DATA = 11,
    WASM_SECTION_DATA_COUNT = 12, WASM_SECTION_TAG = 13
} WasmSectionId;

/* One section: offsets into the decoded bytes. A custom section's name is [name_offset, +name_size); both are 0
   for every other id. */
typedef struct {
    WasmSectionId id;
    size_t offset;
    size_t size;
    size_t name_offset;
    size_t name_size;
} WasmSection;

/* The decode's whole state. `in` is borrowed and outlives the decode; `sections` and `module` are owned. */
typedef struct {
    const uint8_t *in;
    size_t in_len;
    size_t pos;
    int phase;
    int last_rank;          /* §5.5.17's place of the last non-custom section, 0 before the first */
    size_t name_end;        /* while a name is read: the byte after it */
    int name_next;          /* the phase that follows the name */
    size_t section_end;     /* while a section's contents are read: the byte after its len */
    uint32_t items_left;    /* the section's list: items not yet read */
    uint32_t group_left;    /* §5.3.8's current rec group: subtypes not yet read */
    uint32_t list_left;     /* the current subtype's or instruction's list being read: items not yet read */
    int list_kind;          /* which instruction immediate that list is (module_decoder.c's IL_ values) */
    uint8_t *blocks;        /* §5.4.2's open structured instructions, innermost last (module_decoder.c's BK_) */
    size_t n_blocks, cap_blocks;
    size_t expr_offset;     /* where the §5.4.10 expr being read began */
    int expr_next;          /* the phase that follows it */
    uint32_t illegal_prefix, illegal_opcode;   /* for WASM_DECODE_ILLEGAL_OPCODE's message */
    size_t code_end;        /* while a code entry is read: the byte after its len */
    uint64_t locals_total;  /* its locals so far */
    bool in_code;           /* the expr being read is a function body */
    bool code_names_data;   /* some function body holds a data index (§5.5.17's dataidx(func*)) */
    bool table_with_init;   /* the expr being read is a 0x40 0x00 table's initializer */
    uint32_t elem_form;     /* the element segment being read: its §5.5.12 form, and its items not yet read */
    uint32_t elem_left;
    size_t rec_start;       /* the current rec group's first type index, and its size */
    uint32_t rec_size;
    WasmSection *sections;
    size_t n_sections;
    size_t cap_sections;
    WasmModule module;
} WasmModuleDecoder;

/* Begin a decode of `n` bytes at `bytes` (borrowed). Allocates nothing. */
void wasm_module_decoder_init(WasmModuleDecoder *d, const uint8_t *bytes, size_t n);
/* Read one production instance. MORE while the decode continues, then DONE or a refusal, after which stepping
   again is this codebase's misuse (asserted). */
WasmDecodeStatus wasm_module_decoder_step(WasmModuleDecoder *d);
/* Step until an answer, for a caller with nothing to interleave. */
WasmDecodeStatus wasm_module_decoder_run(WasmModuleDecoder *d);
/* The sections read so far, in the order of the bytes. */
size_t wasm_module_decoder_section_count(const WasmModuleDecoder *d);
const WasmSection *wasm_module_decoder_section(const WasmModuleDecoder *d, size_t i);
/* What has been decoded so far; complete after WASM_DECODE_DONE. Owned by the decoder. */
const WasmModule *wasm_module_decoder_module(const WasmModuleDecoder *d);
/* Release the section list and the module. Idempotent. */
void wasm_module_decoder_free(WasmModuleDecoder *d);

/* The reference interpreter's message for the refusal `d` answered, written to `buf` (at most `cap` bytes, NUL
   terminated): wasm_decode_status_message's, with the opcode appended for WASM_DECODE_ILLEGAL_OPCODE as
   decode.ml's `illegal` and `illegal2` spell it. */
void wasm_module_decoder_message(const WasmModuleDecoder *d, WasmDecodeStatus st, char *buf, size_t cap);

/* The reference interpreter's message for a refusal (interpreter/binary/decode.ml), which the testsuite's
   `assert_malformed` matches by prefix. A static string; NULL for DONE and MORE. */
const char *wasm_decode_status_message(WasmDecodeStatus st);

#endif
