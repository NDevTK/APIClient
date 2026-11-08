/* WebAssembly Core Specification (Release 3.0) §4.7.2 "Instantiation" of a validated module, as a step machine,
 * in §4.7.2's order: each defined global's initializer, each defined table's (or ref.null of its heap type),
 * each element segment's items, then each active segment's table.init (one element a step) and the drop of
 * every active and declarative one. Initializers run in the interpreter (interpreter.h), one instruction a step.
 * The instance owns its globals, tables and element instances as plain values; a reference is its function
 * index plus one, 0 the null reference. The interpreter borrows the instance.
 * Named residual. Not covered: imports (§4.7.2's external values), memories and data segments, and the start
 * function. Next diff builds: imports, with host functions as calls that return to the scheduler. Absence shows
 * as: a module whose functions read memory or an import stops at the interpreter's CHECK_FAIL there. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_INSTANCE_H
#define ENGINE_HOST_BROWSER_CORE_WASM_INSTANCE_H

#include <stdbool.h>
#include <stddef.h>

#include "core/wasm/code_validator.h"
#include "core/wasm/interpreter.h"
#include "core/wasm/matching.h"
#include "core/wasm/wasm_module.h"

/* §4.2.8's table instance: `size` references, `type` its §5.3.13 limits. */
typedef struct {
    WasmValue *elems;
    uint64_t size, cap;
    WasmLimits type;
} WasmTableInstance;

/* §4.2.12's element instance: its references, none once dropped. */
typedef struct {
    WasmValue *refs;
    uint64_t n;
} WasmElemInstance;

struct WasmInstance {
    const WasmModule *module;    /* borrowed, with its bytes, side table and type identities */
    const uint8_t *bytes;
    const WasmSideTable *side;
    const WasmTypes *types;
    WasmValue *globals;          /* owned: one per defined global */
    WasmTableInstance *tables;   /* owned: one per defined table */
    WasmElemInstance *elems;     /* owned: one per element segment */
    int phase;                   /* instance.c's I_ phase, the item in it and the position inside the item */
    size_t item;
    uint64_t sub, offset;
    bool evaluating;
    WasmInterpreter init;        /* the initializer being evaluated */
};

/* Begin instantiating a module the validator answered DONE for: `side` its side table, `types` its identities
   (the validator's context). NO_ROOM when the instance's records cannot be allocated. */
WasmExecStatus wasm_instance_begin(WasmInstance *inst, const WasmModule *m, const uint8_t *bytes,
                                   const WasmSideTable *side, const WasmTypes *types);
/* One step: an instruction of an initializer, one element of a table.init, or the move to the next item.
   MORE, then DONE or a trap (an active segment out of its table's bounds). */
WasmExecStatus wasm_instance_step(WasmInstance *inst);
void wasm_instance_free(WasmInstance *inst);

#endif
