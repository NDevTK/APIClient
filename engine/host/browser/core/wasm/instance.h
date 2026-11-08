/* WebAssembly Core Specification (Release 3.0) §4.2.6 "Module Instances" and §4.7.2 "Instantiation" of a
 * validated module into a store (store.h), as a step machine, in §4.7.2's order: each defined function, global,
 * table and element segment allocated, each global's initializer, each table's (or ref.null of its heap type),
 * each element segment's items, then each active segment's table.init (one element a step) and the drop of
 * every active and declarative one. Initializers run in the interpreter (interpreter.h), one instruction a step.
 * The instance maps each index space to store addresses; an import's entry is SIZE_MAX until imports resolve.
 * Named residual. Not covered: imports (§4.7.2's external values), memories and data segments, and the start
 * function. Next diff builds: imports, with host functions as calls that return to the scheduler. Absence shows
 * as: a module whose functions reach an import or memory stops at the interpreter's CHECK_FAIL there. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_INSTANCE_H
#define ENGINE_HOST_BROWSER_CORE_WASM_INSTANCE_H

#include <stdbool.h>
#include <stddef.h>

#include "core/wasm/code_validator.h"
#include "core/wasm/interpreter.h"
#include "core/wasm/matching.h"
#include "core/wasm/store.h"
#include "core/wasm/wasm_module.h"

struct WasmInstance {
    WasmStore *store;            /* borrowed, and the module with its bytes, side table and type identities */
    size_t addr;                 /* this instance's own address in the store */
    const WasmModule *module;
    const uint8_t *bytes;
    const WasmSideTable *side;
    const WasmTypes *types;
    size_t *funcs, *globals, *tables, *elems;   /* owned: per index, its store address */
    size_t imported_funcs, imported_globals, imported_tables;
    int phase;                   /* instance.c's I_ phase, the item in it and the position inside the item */
    size_t item;
    uint64_t sub, offset;
    bool evaluating;
    WasmInterpreter init;        /* the initializer being evaluated */
};

/* Begin instantiating, into `store`, a module the validator answered DONE for: `side` its side table, `types`
   its identities (the validator's context). NO_ROOM when a table's or segment's storage cannot be had. */
WasmExecStatus wasm_instance_begin(WasmInstance *inst, WasmStore *store, const WasmModule *m, const uint8_t *bytes,
                                   const WasmSideTable *side, const WasmTypes *types);
/* One step: an instruction of an initializer, one element of a table.init, or the move to the next item.
   MORE, then DONE or a trap (an active segment out of its table's bounds). */
WasmExecStatus wasm_instance_step(WasmInstance *inst);
void wasm_instance_free(WasmInstance *inst);

#endif
