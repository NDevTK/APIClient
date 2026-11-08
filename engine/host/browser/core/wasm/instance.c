/* WebAssembly Core Specification (Release 3.0) §4.7.2 "Instantiation". See instance.h. */
#include "core/wasm/instance.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"

enum { I_GLOBALS = 0, I_TABLES, I_TABLE_FILL, I_ELEMS, I_ACTIVE, I_ACTIVE_COPY, I_DONE };

static size_t imported(const WasmModule *m, WasmExternKind kind)
{
    size_t n = 0, i;

    for (i = 0; i < m->n_imports; i++) n += m->imports[i].kind == kind;
    return n;
}

WasmExecStatus wasm_instance_begin(WasmInstance *inst, const WasmModule *m, const uint8_t *bytes,
                                   const WasmSideTable *side, const WasmTypes *types)
{
    DCHECK(inst != NULL && m != NULL && bytes != NULL && side != NULL && types != NULL,
           "a WebAssembly instantiation began without its module");
    memset(inst, 0, sizeof *inst);
    inst->module = m;
    inst->bytes = bytes;
    inst->side = side;
    inst->types = types;
    inst->globals = calloc(m->n_globals ? m->n_globals : 1, sizeof *inst->globals);
    inst->tables = calloc(m->n_tables ? m->n_tables : 1, sizeof *inst->tables);
    inst->elems = calloc(m->n_elems ? m->n_elems : 1, sizeof *inst->elems);
    if (inst->globals == NULL || inst->tables == NULL || inst->elems == NULL) return WASM_EXEC_NO_ROOM;
    inst->phase = I_GLOBALS;
    return WASM_EXEC_MORE;
}

/* Evaluate constant expression `e`, one instruction a step: MORE while it runs; DONE with its value in `*out`. */
static WasmExecStatus evaluate(WasmInstance *inst, WasmExpr e, WasmValue *out)
{
    WasmExecStatus st;

    if (!inst->evaluating) {
        st = wasm_interpreter_begin_expr(&inst->init, inst, e);
        inst->evaluating = st == WASM_EXEC_MORE;
        return st;
    }
    st = wasm_interpreter_step(&inst->init);
    if (st == WASM_EXEC_MORE) return st;
    inst->evaluating = false;
    if (st == WASM_EXEC_DONE) {
        DCHECK(inst->init.n_vals == 1, "a validated WebAssembly constant expression gave other than one value");
        *out = inst->init.vals[0];
    }
    wasm_interpreter_free(&inst->init);
    return st;
}

static WasmExecStatus next(WasmInstance *inst, int phase)
{
    inst->item++;
    inst->sub = 0;
    inst->phase = phase;
    return WASM_EXEC_MORE;
}

WasmExecStatus wasm_instance_step(WasmInstance *inst)
{
    const WasmModule *m = inst->module;
    WasmExecStatus st;
    WasmValue v = { 0, 0 };

    switch (inst->phase) {
    case I_GLOBALS:
        if (inst->item == m->n_globals) {
            inst->item = 0;
            inst->phase = I_TABLES;
            return WASM_EXEC_MORE;
        } else {
            WasmExpr e = { m->globals[inst->item].init_offset, m->globals[inst->item].init_size };

            st = evaluate(inst, e, &inst->globals[inst->item]);
            return st == WASM_EXEC_DONE ? next(inst, I_GLOBALS) : st;
        }
    case I_TABLES: {
        /* §4.7.1's alloctable: min references of the initializer's value; ref.null's is 0, calloc's zero. */
        WasmTableInstance *t;

        if (inst->item == m->n_tables) {
            inst->item = 0;
            inst->phase = I_ELEMS;
            return WASM_EXEC_MORE;
        }
        t = &inst->tables[inst->item];
        if (t->elems == NULL) {
            t->type = m->tables[inst->item].limits;
            if (t->type.min > SIZE_MAX / sizeof *t->elems) return WASM_EXEC_NO_ROOM;
            t->elems = calloc(t->type.min ? (size_t)t->type.min : 1, sizeof *t->elems);
            if (t->elems == NULL) return WASM_EXEC_NO_ROOM;
            t->size = t->cap = t->type.min;
        }
        if (!m->tables[inst->item].has_init) return next(inst, I_TABLES);
        st = evaluate(inst, m->tables[inst->item].init, &v);
        if (st != WASM_EXEC_DONE) return st;
        if (v.lo == 0) return next(inst, I_TABLES);
        inst->offset = v.lo;   /* I_TABLE_FILL's reference */
        inst->phase = I_TABLE_FILL;
        return WASM_EXEC_MORE;
    }
    case I_TABLE_FILL: {
        WasmTableInstance *t = &inst->tables[inst->item];

        if (inst->sub == t->size) return next(inst, I_TABLES);
        t->elems[inst->sub++].lo = inst->offset;
        JS_FlowRetireWork(1);
        return WASM_EXEC_MORE;
    }
    case I_ELEMS: {
        /* §4.7.2's element segments: each item a reference, a function index's or its expression's value. */
        const WasmElem *e;
        WasmElemInstance *x;

        if (inst->item == m->n_elems) {
            inst->item = 0;
            inst->phase = I_ACTIVE;
            return WASM_EXEC_MORE;
        }
        e = &m->elems[inst->item];
        x = &inst->elems[inst->item];
        if (x->refs == NULL) {
            x->refs = calloc(e->n_items ? e->n_items : 1, sizeof *x->refs);
            if (x->refs == NULL) return WASM_EXEC_NO_ROOM;
            x->n = e->n_items;
        }
        if (inst->sub == e->n_items) return next(inst, I_ELEMS);
        if (!e->exprs) {
            x->refs[inst->sub].lo = (uint64_t)m->elem_funcs[e->items + inst->sub] + 1;
            inst->sub++;
            JS_FlowRetireWork(1);
            return WASM_EXEC_MORE;
        }
        st = evaluate(inst, m->elem_exprs[e->items + inst->sub], &x->refs[inst->sub]);
        if (st == WASM_EXEC_DONE) inst->sub++;
        return st == WASM_EXEC_DONE ? WASM_EXEC_MORE : st;
    }
    case I_ACTIVE: {
        /* §4.7.2 steps for an active segment: table.init over all of it at its offset, then elem.drop; a
           declarative one is dropped. */
        const WasmElem *e;
        size_t table;

        if (inst->item == m->n_elems) {
            inst->phase = I_DONE;
            return WASM_EXEC_DONE;
        }
        e = &m->elems[inst->item];
        if (e->mode == WASM_SEGMENT_PASSIVE) return next(inst, I_ACTIVE);
        if (e->mode == WASM_SEGMENT_DECLARATIVE) {
            inst->elems[inst->item].n = 0;
            return next(inst, I_ACTIVE);
        }
        st = evaluate(inst, e->offset, &v);
        if (st != WASM_EXEC_DONE) return st;
        CHECKF(e->table >= imported(m, WASM_EXTERN_TABLE), "an active element segment of imported table %u is not "
               "built in instance.c", e->table);
        table = e->table - imported(m, WASM_EXTERN_TABLE);
        if (v.lo > inst->tables[table].size || e->n_items > inst->tables[table].size - v.lo)
            return WASM_EXEC_TRAP_TABLE_BOUNDS;
        inst->offset = v.lo;
        inst->phase = I_ACTIVE_COPY;
        return WASM_EXEC_MORE;
    }
    case I_ACTIVE_COPY: {
        const WasmElem *e = &m->elems[inst->item];
        WasmTableInstance *t = &inst->tables[e->table - imported(m, WASM_EXTERN_TABLE)];

        if (inst->sub == e->n_items) {
            inst->elems[inst->item].n = 0;
            return next(inst, I_ACTIVE);
        }
        t->elems[inst->offset + inst->sub] = inst->elems[inst->item].refs[inst->sub];
        inst->sub++;
        JS_FlowRetireWork(1);
        return WASM_EXEC_MORE;
    }
    default: break;
    }
    CHECK_FAIL("a WebAssembly instantiation was stepped after it answered");
}

void wasm_instance_free(WasmInstance *inst)
{
    size_t i;

    DCHECK(inst != NULL, "a WebAssembly instance was freed with no instance");
    if (inst->evaluating) wasm_interpreter_free(&inst->init);
    for (i = 0; inst->tables != NULL && i < inst->module->n_tables; i++) free(inst->tables[i].elems);
    for (i = 0; inst->elems != NULL && i < inst->module->n_elems; i++) free(inst->elems[i].refs);
    free(inst->tables);
    free(inst->elems);
    free(inst->globals);
    memset(inst, 0, sizeof *inst);
}
