/* WebAssembly Core Specification (Release 3.0) §4.2.6 "Module Instances" and §4.7.2 "Instantiation" of a
 * validated module into a store (store.h), as a step machine, in §4.7.2's order: each import's external address
 * checked against its type (§4.5.2, external_typing.h), then each defined function, global, table, memory,
 * element and data segment allocated, each global's initializer, each table's (or ref.null of its heap type),
 * each element segment's items, then each active element segment's table.init (one element a step) and the drop
 * of every active and declarative one, then each active data segment's memory.init (one byte a step) and its
 * drop. Initializers run in the interpreter (interpreter.h), one instruction a step. The instance maps each index
 * space to store addresses, an import's to the address it was given.
 * Named residual. Not covered: tag imports and the start function. Next diff builds: the start function.
 * Absence shows as: a module with a start section or a tag import stops at a CHECK_FAIL. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_INSTANCE_H
#define ENGINE_HOST_BROWSER_CORE_WASM_INSTANCE_H

#include <stdbool.h>
#include <stddef.h>

#include "core/wasm/code_validator.h"
#include "core/wasm/external_typing.h"
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
    size_t *funcs, *globals, *tables, *mems, *elems, *datas;   /* owned: per index, its store address */
    size_t imported_funcs, imported_globals, imported_tables, imported_mems;
    int phase;                   /* instance.c's I_ phase, the item in it and the position inside the item */
    size_t item;
    uint64_t sub, offset;
    bool evaluating, climbing;
    WasmInterpreter init;        /* the initializer being evaluated */
    WasmExternCheck check;       /* I_IMPORTS: the import being checked, while it climbs */
    size_t passed[4];            /* I_IMPORTS: the function, global, table and memory imports checked so far */
};

/* Begin instantiating, into `store`, a module the validator answered DONE for: `side` its side table, `types`
   its identities (the validator's context), `externs` a store address per import in the module's import order,
   of the import's kind (the embedder's resolution of the import names, copied). */
WasmExecStatus wasm_instance_begin(WasmInstance *inst, WasmStore *store, const WasmModule *m, const uint8_t *bytes,
                                   const WasmSideTable *side, const WasmTypes *types, const size_t *externs,
                                   size_t n_externs);
/* One step: an import checked (or one supertype climbed), the allocation, an instruction of an initializer, one
   element of a table.init, one byte of a memory.init, or the move to the next item. MORE, then DONE,
   LINK_INCOMPATIBLE, NO_ROOM (a table's, memory's or segment's storage cannot be had) or a trap (an active
   segment out of its table's or memory's bounds). */
WasmExecStatus wasm_instance_step(WasmInstance *inst);
void wasm_instance_free(WasmInstance *inst);

#endif
