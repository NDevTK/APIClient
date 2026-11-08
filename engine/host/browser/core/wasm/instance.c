/* WebAssembly Core Specification (Release 3.0) §4.7.2 "Instantiation". See instance.h. */
#include "core/wasm/instance.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"

WasmExecStatus wasm_instance_begin(WasmInstance *inst, const WasmModule *m, const uint8_t *bytes,
                                   const WasmSideTable *side)
{
    DCHECK(inst != NULL && m != NULL && bytes != NULL && side != NULL, "a WebAssembly instantiation began without "
                                                                       "its module");
    memset(inst, 0, sizeof *inst);
    inst->module = m;
    inst->bytes = bytes;
    inst->side = side;
    inst->globals = calloc(m->n_globals ? m->n_globals : 1, sizeof *inst->globals);
    return inst->globals != NULL ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
}

WasmExecStatus wasm_instance_step(WasmInstance *inst)
{
    const WasmModule *m = inst->module;
    WasmExecStatus st;

    DCHECK(inst->item <= m->n_globals, "a WebAssembly instantiation was stepped after it answered");
    if (!inst->evaluating) {
        WasmExpr e;

        if (inst->item == m->n_globals) return WASM_EXEC_DONE;
        e.offset = m->globals[inst->item].init_offset;
        e.size = m->globals[inst->item].init_size;
        st = wasm_interpreter_begin_expr(&inst->init, m, inst->bytes, inst->side, inst->globals, e);
        inst->evaluating = st == WASM_EXEC_MORE;
        return st;
    }
    st = wasm_interpreter_step(&inst->init);
    if (st == WASM_EXEC_MORE) return st;
    inst->evaluating = false;
    if (st == WASM_EXEC_DONE) {
        DCHECK(inst->init.n_vals == 1, "a validated WebAssembly global initializer gave other than one value");
        inst->globals[inst->item++] = inst->init.vals[0];
        st = WASM_EXEC_MORE;
    }
    wasm_interpreter_free(&inst->init);
    return st;
}

void wasm_instance_free(WasmInstance *inst)
{
    DCHECK(inst != NULL, "a WebAssembly instance was freed with no instance");
    if (inst->evaluating) wasm_interpreter_free(&inst->init);
    free(inst->globals);
    memset(inst, 0, sizeof *inst);
}
