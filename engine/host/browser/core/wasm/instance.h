/* WebAssembly Core Specification (Release 3.0) §4.7.2 "Instantiation" of a validated module, as a step machine:
 * its defined globals, each initialized by evaluating its constant expression with the interpreter
 * (interpreter.h), one instruction a step, in the order §4.7.2 evaluates them, each seeing those before it.
 * The instance owns its globals as plain values; the interpreter borrows them.
 * Named residual. Not covered: imports (§4.7.2's external values), tables, memories, element and data
 * segments, and the start function. Next diff builds: tables, with element segments evaluated and active ones
 * applied. Absence shows as: a module whose functions read a table has no table to read (the interpreter's
 * CHECK_FAIL at the table instruction). */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_INSTANCE_H
#define ENGINE_HOST_BROWSER_CORE_WASM_INSTANCE_H

#include <stddef.h>

#include "core/wasm/code_validator.h"
#include "core/wasm/interpreter.h"
#include "core/wasm/wasm_module.h"

typedef struct {
    const WasmModule *module;    /* borrowed, with its bytes and side table */
    const uint8_t *bytes;
    const WasmSideTable *side;
    WasmValue *globals;          /* owned: one per defined global */
    size_t item;                 /* the global being initialized */
    bool evaluating;
    WasmInterpreter init;        /* its initializer's evaluation */
} WasmInstance;

/* Begin instantiating a module the validator answered DONE for, with `side` its side table. NO_ROOM when the
   globals cannot be allocated. */
WasmExecStatus wasm_instance_begin(WasmInstance *inst, const WasmModule *m, const uint8_t *bytes,
                                   const WasmSideTable *side);
/* One instruction of the current initializer, or the move to the next. MORE, then DONE or a trap. */
WasmExecStatus wasm_instance_step(WasmInstance *inst);
void wasm_instance_free(WasmInstance *inst);

#endif
