/* WebAssembly Core Specification (Release 3.0) §3.3 "Matching" over a decoded module's types, shared by the
 * module validator (module_validator.h) and the code validator (code_validator.h). Type identity is §3.3.10's
 * closed defined type: two type indices are one type when their rec groups have one canonical id
 * (module_validator.c's identification) and the indices stand at one position in them.
 * Two defined types that are not one identity match when §3.3.10's supertype walk from the first reaches the
 * second. A match answers WASM_MATCH_CLIMB with the walk's start in `*climb` instead of walking, and the caller
 * steps the walk one supertype at a time with wasm_climb, so no answer costs more than a constant. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_MATCHING_H
#define ENGINE_HOST_BROWSER_CORE_WASM_MATCHING_H

#include <stdbool.h>
#include <stddef.h>

#include "core/wasm/wasm_module.h"

/* The module's types and, per type index that starts a rec group, its group's canonical id. Both borrowed. */
typedef struct {
    const WasmModule *module;
    const size_t *group_canon;
} WasmTypes;

typedef enum { WASM_MATCH_NO = 0, WASM_MATCH_YES, WASM_MATCH_CLIMB } WasmMatch;

/* §3.3.10's walk: the type it stands on and the type it looks for. */
typedef struct {
    size_t at, target;
} WasmClimb;

bool wasm_same_type(const WasmTypes *t, size_t x, size_t y);
/* §3.3.3 over two abstract heap types. */
bool wasm_abstract_le(uint8_t a, uint8_t b);
/* The abstract heap type a defined type of kind `k` expands below: struct, array or func. */
uint8_t wasm_kind_heap(WasmCompKind k);
/* §3.3.3's heap types, §3.3.5's value types (and §3.3.9's storage types), §3.3.9's field types. */
WasmMatch wasm_heap_le(const WasmTypes *t, const WasmValType *a, const WasmValType *b, WasmClimb *climb);
WasmMatch wasm_val_le(const WasmTypes *t, const WasmValType *a, const WasmValType *b, WasmClimb *climb);
WasmMatch wasm_field_le(const WasmTypes *t, const WasmFieldType *a, const WasmFieldType *b, WasmClimb *climb);
/* Matching both ways: one type, or one identity. */
bool wasm_val_eq(const WasmTypes *t, const WasmValType *a, const WasmValType *b);
/* One step of the walk: YES once it stands on the target's identity, NO when it runs out of supertypes, CLIMB
   after moving to the next supertype. */
WasmMatch wasm_climb(const WasmTypes *t, WasmClimb *climb);

#endif
