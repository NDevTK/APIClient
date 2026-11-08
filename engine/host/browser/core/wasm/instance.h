/* WebAssembly Core Specification (Release 3.0) §4.2.6 "Module Instances" and §4.7.2 "Instantiation" of a
 * validated module into a store (store.h), as a step machine, in §4.7.2's order: each import's external address
 * checked against its type (§4.5.2, external_typing.h), then each defined function, global, table, memory,
 * element and data segment allocated, each global's initializer, each table's (or ref.null of its heap type),
 * each element segment's items, then each active element segment's table.init (one element a step) and the drop
 * of every active and declarative one, then each active data segment's memory.init (one byte a step) and its
 * drop, then the start function's invocation. Initializers and the start function run in the interpreter
 * (interpreter.h), one instruction a step; the start function's FORK and HOST_CALL are the caller's to answer, as
 * an invocation's are. The instance maps each index space to store addresses, an import's to the address it was
 * given.
 * Named residual. Not covered: tag imports. Next diff builds: the JS API's install. Absence shows as: a module
 * with a tag import stops at a CHECK. */
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

/* §4.2.6's module instance: each index space's store addresses, an import's the address it was given. Its owner
   is an object of the agent's instance class, which the store's instance list holds; nothing writes the record
   after §4.7.1's allocmodule, so the flows that fork after it share it. */
struct WasmInstance {
    WasmStore *store;            /* borrowed, and the module with its bytes, side table and type identities */
    size_t addr;                 /* this instance's own address in the store */
    const WasmModule *module;
    const uint8_t *bytes;
    const WasmSideTable *side;
    const WasmTypes *types;
    JSValue keep;                /* owned: what keeps the four above alive, or undefined where the caller does */
    size_t *funcs, *globals, *tables, *mems, *elems, *datas;   /* owned: per index, its store address */
    size_t imported_funcs, imported_globals, imported_tables, imported_mems;
};

/* §4.7.2's instantiation in progress, a step machine's state: every owned array is on the agent's allocator
   (js_malloc on `store->ctx`), so a state embedding it copies and frees it through its visit
   (wasm_instantiation_visit). `inst` is the store's record once I_ALLOC has made it. */
typedef struct {
    WasmStore *store;            /* borrowed, as are the module, its bytes, side table and identities */
    const WasmModule *module;
    const uint8_t *bytes;
    const WasmSideTable *side;
    const WasmTypes *types;
    JSValue keep;                /* borrowed: the new instance's `keep` */
    size_t *externs;             /* owned: one store address per import, in import order */
    size_t n_externs;
    WasmInstance *inst;          /* borrowed from the store, from I_ALLOC */
    size_t addr;                 /* the instance's store address, SIZE_MAX before I_ALLOC */
    size_t type_space;           /* the store address of the module's type space, from I_ALLOC */
    int phase;                   /* instance.c's I_ phase, the item in it and the position inside the item */
    size_t item;
    uint64_t sub, offset;
    bool evaluating, climbing, starting;
    WasmInterpreter init;        /* the initializer being evaluated, while `evaluating` */
    WasmInterpreter start;       /* the start function's invocation, while `starting` */
    WasmExternCheck check;       /* I_IMPORTS: the import being checked, while it climbs */
    size_t passed[4];            /* the function, global, table and memory imports checked, then placed */
} WasmInstantiation;

/* Begin instantiating, into `store`, a module the validator answered DONE for: `side` its side table, `types`
   its identities (the validator's context), `externs` a store address per import in the module's import order,
   of the import's kind (the embedder's resolution of the import names, copied), `keep` (borrowed) what keeps
   the module alive for the instance's life. Allocates only the copy; NO_ROOM when it cannot be had. */
WasmExecStatus wasm_instantiation_begin(WasmInstantiation *run, WasmStore *store, const WasmModule *m,
                                        const uint8_t *bytes, const WasmSideTable *side, const WasmTypes *types,
                                        const size_t *externs, size_t n_externs, JSValueConst keep);
/* One step, charged one unit of work: an import checked (or one supertype climbed), the instance record made,
   one entity allocated, an instruction of an initializer, one element of a table.init, one byte of a
   memory.init, an instruction of the start function, or the move to the next item. MORE, FORK or HOST_CALL
   (the start function's, see wasm_instantiation_choose and wasm_instantiation_return), then DONE,
   LINK_INCOMPATIBLE, NO_ROOM (a table's, memory's or segment's storage cannot be had) or a trap (an active
   segment out of its table's or memory's bounds, or the start function's). */
WasmExecStatus wasm_instantiation_step(WasmInstantiation *run);
/* Resume the start function after FORK with an arm, or after HOST_CALL with the host function's results
   (interpreter.h's wasm_interpreter_choose and wasm_interpreter_return); its arguments are `run->start`'s. */
WasmExecStatus wasm_instantiation_choose(WasmInstantiation *run, bool example);
WasmExecStatus wasm_instantiation_return(WasmInstantiation *run, const WasmValue *results, size_t n);
/* What `run` owns, declared to a step machine's visit (quickjs-step.h's JSStepVisit). */
void wasm_instantiation_visit(JSContext *ctx, WasmInstantiation *run, JSStepVisit *v);
/* Release what `run` owns; the instance, once made, is the store's. */
void wasm_instantiation_free(WasmInstantiation *run);

/* The instance owned by `owner`, an object of the agent's instance class (asserted). */
WasmInstance *wasm_instance_of(JSValueConst owner);
/* A new instance record of module `m` in `store` with `imported` function, global, table and memory imports,
   its index spaces sized and unset and its address SIZE_MAX, as the reference to its owner. */
JSValue wasm_instance_new(WasmStore *store, const WasmModule *m, const uint8_t *bytes, const WasmSideTable *side,
                          const WasmTypes *types, JSValueConst keep, const size_t imported[4]);
/* The instance class, declared once per agent (core/platform.c's `wasm_instance` row), and its release. */
void wasm_instance_class_init(JSContext *ctx);
void wasm_instance_class_free(void);

#endif
