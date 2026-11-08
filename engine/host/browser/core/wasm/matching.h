/* WebAssembly Core Specification (Release 3.0) §3.3 "Matching" between the types of one module or of two, shared
 * by the module validator (module_validator.h), the code validator (code_validator.h) and execution. Type identity
 * is §3.3.10's closed defined type: two type indices are one type when their rec groups have one canonical id in
 * the agent's registry (type_registry.h) and the indices stand at one position in them, so both sides of a match
 * must have been validated against one registry.
 * Two defined types that are not one identity match when §3.3.10's supertype walk from the first reaches the
 * second. A match answers WASM_MATCH_CLIMB with the walk's start in `*climb` instead of walking, and the caller
 * steps the walk one supertype at a time with wasm_climb, so no answer costs more than a constant. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_MATCHING_H
#define ENGINE_HOST_BROWSER_CORE_WASM_MATCHING_H

#include <stdbool.h>
#include <stddef.h>

#include "core/wasm/type_registry.h"
#include "core/wasm/wasm_module.h"

/* The module's types, the registry its groups were interned in, and per type index that starts a rec group, its
   group's canonical id. All borrowed. */
typedef struct {
    const WasmModule *module;
    const WasmTypeRegistry *registry;
    const size_t *group_canon;
} WasmTypes;

typedef enum { WASM_MATCH_NO = 0, WASM_MATCH_YES, WASM_MATCH_CLIMB } WasmMatch;

/* §3.3.10's walk: the type it stands on (an index of the module the walk climbs) and the identity it looks for. */
typedef struct {
    size_t at;
    size_t target_group, target_pos;
} WasmClimb;

/* Type `x` of `ta`'s module and type `y` of `tb`'s are one closed defined type. */
bool wasm_same_type(const WasmTypes *ta, size_t x, const WasmTypes *tb, size_t y);
/* §3.3.3 over two abstract heap types. */
bool wasm_abstract_le(uint8_t a, uint8_t b);
/* The abstract heap type a defined type of kind `k` expands below: struct, array or func. */
uint8_t wasm_kind_heap(WasmCompKind k);
/* §3.3.3's heap types, §3.3.5's value types (and §3.3.9's storage types), §3.3.9's field types: `a`, a type of
   `ta`'s module, below `b`, one of `tb`'s. A CLIMB walks `ta`'s module. */
WasmMatch wasm_heap_le(const WasmTypes *ta, const WasmValType *a, const WasmTypes *tb, const WasmValType *b,
                       WasmClimb *climb);
WasmMatch wasm_val_le(const WasmTypes *ta, const WasmValType *a, const WasmTypes *tb, const WasmValType *b,
                      WasmClimb *climb);
WasmMatch wasm_field_le(const WasmTypes *ta, const WasmFieldType *a, const WasmTypes *tb, const WasmFieldType *b,
                        WasmClimb *climb);
/* Matching both ways: one type, or one identity. */
bool wasm_val_eq(const WasmTypes *ta, const WasmValType *a, const WasmTypes *tb, const WasmValType *b);
/* One step of the walk over `t`'s module: YES once it stands on the target's identity, NO when it runs out of
   supertypes, CLIMB after moving to the next supertype. */
WasmMatch wasm_climb(const WasmTypes *t, WasmClimb *climb);

#endif
