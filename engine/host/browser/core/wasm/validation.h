/* WebAssembly Core Specification (Release 3.0) §3 "Validation": the refusals both validators answer
 * (module_validator.h for §3.5's module, code_validator.h for §3.4's instructions) and §3.1.6's context, the
 * index spaces a module defines with imports first. A refusal's message is interpreter/valid/valid.ml's, which
 * the testsuite's `assert_invalid` matches by prefix. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_VALIDATION_H
#define ENGINE_HOST_BROWSER_CORE_WASM_VALIDATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/wasm/matching.h"
#include "core/wasm/wasm_module.h"

typedef enum {
    WASM_VALIDATE_DONE = 0,
    WASM_VALIDATE_MORE,
    WASM_VALIDATE_UNKNOWN_INDEX,      /* §3.1.6: an index past its context's list ("unknown <space> <index>") */
    WASM_VALIDATE_NOT_FUNCTION,       /* a type index that must name a function type names another */
    WASM_VALIDATE_MULTIPLE_SUPERS,    /* §3.2.11: a subtype with more than one supertype */
    WASM_VALIDATE_FORWARD_SUPER,      /* §3.2.11: a supertype index not below its subtype's */
    WASM_VALIDATE_FINAL_SUPER,        /* §3.2.11: a supertype that is final */
    WASM_VALIDATE_LIMITS_RANGE,       /* §3.2.12: a memory's or table's bound past its range */
    WASM_VALIDATE_LIMITS_ORDER,       /* §3.2.12: a minimum above its maximum */
    WASM_VALIDATE_TAG_RESULT,         /* §3.2.13: a tag type with results */
    WASM_VALIDATE_START_TYPE,         /* §3.5.10: a start function with parameters or results */
    WASM_VALIDATE_DUPLICATE_EXPORT,   /* §3.5.13: two exports of one name (nm* disjoint) */
    WASM_VALIDATE_SUB_MISMATCH,       /* §3.2.11: a subtype's composite type does not match its supertype's */
    WASM_VALIDATE_ELEM_MISMATCH,      /* §3.5.9: an active segment's type does not match its table's */
    WASM_VALIDATE_TYPE_MISMATCH,      /* §3.4: an operand, a label's or a result's type does not match the rule's */
    WASM_VALIDATE_NOT_CONSTANT,       /* §3.4.13: an instruction of a constant expression is not constant */
    WASM_VALIDATE_UNINITIALIZED_LOCAL, /* §3.4.3: local.get of a local not set on every path to it */
    WASM_VALIDATE_IMMUTABLE_GLOBAL,   /* §3.4.3: global.set of a constant global */
    WASM_VALIDATE_ALIGNMENT,          /* §3.4.5: a memarg's alignment above the access's natural one */
    WASM_VALIDATE_OFFSET_RANGE,       /* §3.4.5: an i32 memory's offset of 2^32 or more */
    WASM_VALIDATE_LANE_INDEX,         /* §3.4.5, §3.4.11: a lane index past the vector's lanes */
    WASM_VALIDATE_SELECT_ARITY,       /* §3.4.1: a typed select with other than one type */
    WASM_VALIDATE_NOT_STRUCT,         /* a type index that must name a structure type names another */
    WASM_VALIDATE_NOT_ARRAY,          /* a type index that must name an array type names another */
    WASM_VALIDATE_FIELD_PACKED,       /* §3.4.7: struct.get of a packed field, or get_s/get_u of another */
    WASM_VALIDATE_ARRAY_PACKED,       /* §3.4.7: the same of an array's field */
    WASM_VALIDATE_IMMUTABLE_FIELD,    /* §3.4.7: struct.set of an immutable field */
    WASM_VALIDATE_IMMUTABLE_ARRAY,    /* §3.4.7: a write to an immutable array */
    WASM_VALIDATE_NOT_DEFAULTABLE,    /* §3.4.7: a _default allocation of a type with a non-defaultable field */
    WASM_VALIDATE_NOT_NUMERIC,        /* §3.4.7: a data segment into an array of references */
    WASM_VALIDATE_ARRAY_TYPES,        /* §3.4.7: array.copy between arrays whose fields do not match */
    WASM_VALIDATE_UNDECLARED_FUNC,    /* §3.4.6: a body's ref.func of a function §3.5.13's C.refs lacks */
    WASM_VALIDATE_NO_ROOM             /* a table the validator keeps could not be allocated; its size is the input's */
} WasmValidateStatus;

/* §3.1.6's context over a decoded module: its types with their identity, and each index space's size with,
   per imported item of each WasmExternKind, its import's index (filled as validation passes the imports). The
   module validator owns the arrays; `bytes` is the decoded module's. C.refs is every function the module
   references outside function bodies and the start function: by export, by element segment, and by ref.func
   in a constant expression (marked as the code validator types each). */
typedef struct {
    WasmTypes types;
    const uint8_t *bytes;
    size_t n_funcs, n_tables, n_memories, n_globals, n_tags;
    size_t *imports[5];
    size_t n_imported[5];
    bool imports_passed;
    uint8_t *declared;      /* §3.5.13's C.refs: a bit per function index, set before any body is validated */
} WasmContext;

/* The index space entries, for an index below the space's size whose imports have been passed: a function's
   type index, a table's element type and limits, a memory's limits, a global's type and mutability, a tag's type
   index. */
uint32_t wasm_context_func(const WasmContext *c, size_t x);
const WasmValType *wasm_context_table(const WasmContext *c, size_t x, const WasmLimits **limits);
const WasmLimits *wasm_context_memory(const WasmContext *c, size_t x);
const WasmValType *wasm_context_global(const WasmContext *c, size_t x, bool *mutable_);
uint32_t wasm_context_tag(const WasmContext *c, size_t x);
bool wasm_context_declared(const WasmContext *c, size_t x);
void wasm_context_declare(const WasmContext *c, size_t x);

#endif
