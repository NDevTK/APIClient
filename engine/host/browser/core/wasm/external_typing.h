/* WebAssembly Core Specification (Release 3.0) §4.5.2 "External Typing" under §3.3.16 "External Types"'
 * matching: whether a store address supplied for an import is valid with the import's external type, for
 * §4.7.2's instantiation. A function's type, or an immutable global's value type, may match by climbing the
 * exporter's supertypes, one supertype a step. A table's reference type and a mutable global's value type
 * must match both ways, which over closed types is one identity, so they take wasm_val_eq and never climb.
 * A table's or memory's external type is its instance's type, whose minimum §4.7.1 "Allocation"'s growtable and
 * growmem raise to the grown size, so its minimum here is its current size. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_EXTERNAL_TYPING_H
#define ENGINE_HOST_BROWSER_CORE_WASM_EXTERNAL_TYPING_H

#include <stdbool.h>
#include <stddef.h>

#include "core/wasm/matching.h"
#include "core/wasm/store.h"
#include "core/wasm/wasm_module.h"

/* A check that answered CLIMB: the walk, over the module of `types` (borrowed). */
typedef struct {
    const WasmTypes *types;
    WasmClimb climb;
} WasmExternCheck;

/* Check `addr`, an address of the kind `im` imports, against import `im` of the module `importer` describes:
   YES, NO, or CLIMB, after which wasm_extern_check_step walks one supertype a call. */
WasmMatch wasm_extern_check_begin(WasmExternCheck *c, const WasmStore *s, const WasmTypes *importer,
                                  const WasmImport *im, size_t addr);
WasmMatch wasm_extern_check_step(WasmExternCheck *c);

#endif
