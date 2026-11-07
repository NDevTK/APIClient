/* A decoded WebAssembly module: the abstract syntax of WebAssembly Core Specification (Release 3.0) §2.5
 * "Modules" that core/wasm/module_decoder.h's §5 decode produces, held as pools of plain records indexed by
 * position, so it carries no pointer into itself and a later stage (validation, instantiation) reads it as data.
 * Each pool is owned and grown by the decoder; wasm_module_free releases all of them. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_WASM_MODULE_H
#define ENGINE_HOST_BROWSER_CORE_WASM_WASM_MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A value type's or storage type's code: §5.3.1's number types, §5.3.2's vector type and §5.3.7's packed types
   by their byte, and WASM_TYPE_REF for every §5.3.4 reference type. */
typedef enum {
    WASM_TYPE_I32 = 0x7F, WASM_TYPE_I64 = 0x7E, WASM_TYPE_F32 = 0x7D, WASM_TYPE_F64 = 0x7C,
    WASM_TYPE_V128 = 0x7B, WASM_TYPE_I8 = 0x78, WASM_TYPE_I16 = 0x77, WASM_TYPE_REF = 0x64
} WasmTypeCode;

/* §5.3.3's abstract heap types, by their byte. */
typedef enum {
    WASM_HEAP_EXN = 0x69, WASM_HEAP_ARRAY = 0x6A, WASM_HEAP_STRUCT = 0x6B, WASM_HEAP_I31 = 0x6C,
    WASM_HEAP_EQ = 0x6D, WASM_HEAP_ANY = 0x6E, WASM_HEAP_EXTERN = 0x6F, WASM_HEAP_FUNC = 0x70,
    WASM_HEAP_NONE = 0x71, WASM_HEAP_NOEXTERN = 0x72, WASM_HEAP_NOFUNC = 0x73, WASM_HEAP_NOEXN = 0x74
} WasmHeapCode;

/* A §5.3.5 value type, or a §5.3.7 storage type. A reference's heap type is `heap` (a WasmHeapCode) when it is
   abstract and is type index `heap_index` when `heap` is 0; both are 0 for every other code. */
typedef struct {
    uint8_t code;
    bool nullable;
    uint8_t heap;
    uint32_t heap_index;
} WasmValType;

/* §5.3.7's field type: a storage type and its mutability. */
typedef struct {
    WasmValType storage;
    bool mutable_;
} WasmFieldType;

typedef enum { WASM_COMP_FUNC = 0x60, WASM_COMP_STRUCT = 0x5F, WASM_COMP_ARRAY = 0x5E } WasmCompKind;

/* One defined type: a §5.3.8 sub type with its §5.3.7 composite type, and the recursion group it was declared
   in ([rec_start, rec_start + rec_size) of the type index space). Lists are ranges of the module's pools:
   supers in `type_indices`, a function's params and results in `val_types`, a struct's or array's fields in
   `fields` (an array has one). */
typedef struct {
    WasmCompKind kind;
    bool final;
    size_t rec_start;
    uint32_t rec_size;
    size_t supers, n_supers;
    size_t params, n_params;
    size_t results, n_results;
    size_t fields, n_fields;
} WasmSubType;

typedef struct {
    WasmSubType *types;
    size_t n_types, cap_types;
    WasmValType *val_types;
    size_t n_val_types, cap_val_types;
    WasmFieldType *fields;
    size_t n_fields, cap_fields;
    uint32_t *type_indices;
    size_t n_type_indices, cap_type_indices;
} WasmModule;

/* Release every pool. Idempotent. */
void wasm_module_free(WasmModule *m);

#endif
