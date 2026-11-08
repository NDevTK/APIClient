/* WebAssembly Core Specification (Release 3.0) §4.7.2 "Instantiation". See instance.h. */
#include "core/wasm/instance.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/wasm/linear_memory.h"
#include "core/wasm/reference_vector.h"

enum {
    I_IMPORTS = 0, I_ALLOC, I_GLOBALS, I_TABLES, I_TABLE_FILL, I_ELEMS, I_ACTIVE, I_ACTIVE_COPY, I_DATA, I_DATA_COPY,
    I_START, I_DONE
};

static size_t imported(const WasmModule *m, WasmExternKind kind)
{
    size_t n = 0, i;

    for (i = 0; i < m->n_imports; i++) n += m->imports[i].kind == kind;
    return n;
}

WasmExecStatus wasm_instance_begin(WasmInstance *inst, WasmStore *store, const WasmModule *m, const uint8_t *bytes,
                                   const WasmSideTable *side, const WasmTypes *types, const size_t *externs,
                                   size_t n_externs)
{
    size_t i, n_funcs, n_globals, n_tables, n_mems, k[4] = { 0, 0, 0, 0 };

    DCHECK(inst != NULL && store != NULL && m != NULL && bytes != NULL && side != NULL && types != NULL,
           "a WebAssembly instantiation began without its module");
    DCHECKF(n_externs == m->n_imports && (n_externs == 0 || externs != NULL), "a WebAssembly instantiation was "
            "given %zu external addresses for %zu imports", n_externs, m->n_imports);
    memset(inst, 0, sizeof *inst);
    inst->store = store;
    inst->module = m;
    inst->bytes = bytes;
    inst->side = side;
    inst->types = types;
    inst->addr = SIZE_MAX;
    inst->imported_funcs = imported(m, WASM_EXTERN_FUNC);
    inst->imported_globals = imported(m, WASM_EXTERN_GLOBAL);
    inst->imported_tables = imported(m, WASM_EXTERN_TABLE);
    inst->imported_mems = imported(m, WASM_EXTERN_MEMORY);
    n_funcs = inst->imported_funcs + m->n_func_types;
    n_globals = inst->imported_globals + m->n_globals;
    n_tables = inst->imported_tables + m->n_tables;
    n_mems = inst->imported_mems + m->n_memories;
    inst->funcs = malloc((n_funcs + 1) * sizeof *inst->funcs);
    inst->globals = malloc((n_globals + 1) * sizeof *inst->globals);
    inst->tables = malloc((n_tables + 1) * sizeof *inst->tables);
    inst->mems = malloc((n_mems + 1) * sizeof *inst->mems);
    inst->elems = malloc((m->n_elems + 1) * sizeof *inst->elems);
    inst->datas = malloc((m->n_datas + 1) * sizeof *inst->datas);
    CHECK(inst->funcs && inst->globals && inst->tables && inst->mems && inst->elems && inst->datas,
          "a WebAssembly instance's index spaces could not be allocated: out of memory");
    /* §4.7.2's moduleinst: the imports' index spaces come first, from the external addresses. */
    for (i = 0; i < m->n_imports; i++) {
        WasmExternKind kind = m->imports[i].kind;

        CHECK(kind != WASM_EXTERN_TAG, "a WebAssembly tag import is not built in instance.c");
        if (kind == WASM_EXTERN_FUNC) inst->funcs[k[0]++] = externs[i];
        else if (kind == WASM_EXTERN_GLOBAL) inst->globals[k[1]++] = externs[i];
        else if (kind == WASM_EXTERN_TABLE) inst->tables[k[2]++] = externs[i];
        else inst->mems[k[3]++] = externs[i];
    }
    inst->phase = I_IMPORTS;
    return WASM_EXEC_MORE;
}

/* §4.7.1's allocmodule: the instance and its defined entities get addresses. */
static WasmExecStatus allocate(WasmInstance *inst)
{
    const WasmModule *m = inst->module;
    WasmStore *store = inst->store;
    size_t i, types;

    inst->addr = wasm_store_add_instance(store, inst);
    types = wasm_store_add_types(store, inst->types);
    for (i = 0; i < m->n_func_types; i++) {
        WasmFuncInstance f = { false, inst->addr, i, 0, types, m->func_types[i] };

        inst->funcs[inst->imported_funcs + i] = wasm_store_add_func(store, f);
    }
    for (i = 0; i < m->n_globals; i++) {
        WasmGlobalInstance g = { { 0, 0 }, { m->globals[i].type, types }, m->globals[i].mutable_ };

        inst->globals[inst->imported_globals + i] = wasm_store_add_global(store, g);
    }
    for (i = 0; i < m->n_tables; i++) {
        WasmStoreValType elem = { m->tables[i].type, types };
        size_t a = wasm_store_add_table(store, &m->tables[i].limits, elem);

        if (a == SIZE_MAX) return WASM_EXEC_NO_ROOM;
        inst->tables[inst->imported_tables + i] = a;
    }
    for (i = 0; i < m->n_memories; i++) {
        size_t a = wasm_store_add_mem(store, &m->memories[i]);

        if (a == SIZE_MAX) return WASM_EXEC_NO_ROOM;
        inst->mems[inst->imported_mems + i] = a;
    }
    for (i = 0; i < m->n_elems; i++) {
        inst->elems[i] = wasm_store_add_elem(store, m->elems[i].n_items);
        if (inst->elems[i] == SIZE_MAX) return WASM_EXEC_NO_ROOM;
    }
    for (i = 0; i < m->n_datas; i++) {
        WasmDataInstance d = { inst->addr, m->datas[i].bytes_offset, m->datas[i].bytes_size };

        inst->datas[i] = wasm_store_add_data(store, d);
    }
    inst->item = 0;
    inst->phase = I_GLOBALS;
    return WASM_EXEC_MORE;
}

/* §4.7.2's import check, one import (or one supertype of its climb) a step. */
static WasmExecStatus check_import(WasmInstance *inst)
{
    const WasmModule *m = inst->module;
    const WasmImport *im;
    size_t k, a;
    WasmMatch le;

    if (inst->item == m->n_imports) {
        inst->phase = I_ALLOC;
        return WASM_EXEC_MORE;
    }
    im = &m->imports[inst->item];
    k = im->kind == WASM_EXTERN_FUNC ? 0 : im->kind == WASM_EXTERN_GLOBAL ? 1 : im->kind == WASM_EXTERN_TABLE ? 2 : 3;
    JS_FlowRetireWork(1);
    if (inst->climbing) {
        le = wasm_extern_check_step(&inst->check);
    } else {
        a = k == 0 ? inst->funcs[inst->passed[0]] : k == 1 ? inst->globals[inst->passed[1]]
          : k == 2 ? inst->tables[inst->passed[2]] : inst->mems[inst->passed[3]];
        le = wasm_extern_check_begin(&inst->check, inst->store, inst->types, im, a);
    }
    inst->climbing = le == WASM_MATCH_CLIMB;
    if (le == WASM_MATCH_NO) return WASM_EXEC_LINK_INCOMPATIBLE;
    if (le == WASM_MATCH_YES) {
        inst->passed[k]++;
        inst->item++;
    }
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

/* The owner of memory `x` of the instance. */
static JSValueConst memory(WasmInstance *inst, size_t x)
{
    DCHECKF(inst->mems[x] < inst->store->mems.n, "a WebAssembly instance's memory %zu has no store address", x);
    return wasm_store_mem(inst->store, inst->mems[x]);
}

static WasmExecStatus next(WasmInstance *inst, int phase)
{
    inst->item++;
    inst->sub = 0;
    inst->phase = phase;
    return WASM_EXEC_MORE;
}

static const WasmTableInstance *table(WasmInstance *inst, size_t x)
{
    DCHECKF(inst->tables[x] < inst->store->tables.n, "a WebAssembly instance's table %zu has no store address",
            x);
    return wasm_store_table(inst->store, inst->tables[x]);
}

/* The start function's status `st`: while it runs, the instance's; once it answers, the instantiation's end. */
static WasmExecStatus started(WasmInstance *inst, WasmExecStatus st)
{
    if (st == WASM_EXEC_MORE || st == WASM_EXEC_FORK || st == WASM_EXEC_HOST_CALL) return st;
    wasm_interpreter_free(&inst->start);
    inst->starting = false;
    inst->phase = I_DONE;
    return st;
}

WasmExecStatus wasm_instance_choose(WasmInstance *inst, bool example)
{
    DCHECK(inst != NULL && inst->starting, "a WebAssembly instantiation's choice was made with no start function "
           "running");
    return started(inst, wasm_interpreter_choose(&inst->start, example));
}

WasmExecStatus wasm_instance_return(WasmInstance *inst, const WasmValue *results, size_t n)
{
    DCHECK(inst != NULL && inst->starting, "a WebAssembly instantiation's host call was answered with no start "
           "function running");
    return started(inst, wasm_interpreter_return(&inst->start, results, n));
}

WasmExecStatus wasm_instance_step(WasmInstance *inst)
{
    const WasmModule *m = inst->module;
    WasmStore *s = inst->store;
    WasmExecStatus st;
    WasmValue v = { 0, 0 };

    switch (inst->phase) {
    case I_IMPORTS:
        return check_import(inst);
    case I_ALLOC:
        return allocate(inst);
    case I_GLOBALS:
        if (inst->item == m->n_globals) {
            inst->item = 0;
            inst->phase = I_TABLES;
            return WASM_EXEC_MORE;
        } else {
            WasmExpr e = { m->globals[inst->item].init_offset, m->globals[inst->item].init_size };

            st = evaluate(inst, e, &v);
            if (st != WASM_EXEC_DONE) return st;
            wasm_global_set(s, inst->globals[inst->imported_globals + inst->item], v);
            return next(inst, I_GLOBALS);
        }
    case I_TABLES:
        /* A table's initializer: ref.null's 0 is the store's calloc'd zero; another value fills its table. */
        if (inst->item == m->n_tables) {
            inst->item = 0;
            inst->phase = I_ELEMS;
            return WASM_EXEC_MORE;
        }
        if (!m->tables[inst->item].has_init) return next(inst, I_TABLES);
        st = evaluate(inst, m->tables[inst->item].init, &v);
        if (st != WASM_EXEC_DONE) return st;
        if (v.lo == 0) return next(inst, I_TABLES);
        inst->offset = v.lo;   /* I_TABLE_FILL's reference */
        inst->phase = I_TABLE_FILL;
        return WASM_EXEC_MORE;
    case I_TABLE_FILL: {
        const WasmTableInstance *t = table(inst, inst->imported_tables + inst->item);

        if (inst->sub == wasm_refs_length(t->refs)) return next(inst, I_TABLES);
        wasm_refs_set(t->refs, inst->sub++, (WasmValue){ inst->offset, 0 });
        JS_FlowRetireWork(1);
        return WASM_EXEC_MORE;
    }
    case I_ELEMS: {
        /* §4.7.2's element segments: each item a reference, a function index's or its expression's value. */
        const WasmElem *e;
        const WasmElemInstance *x;

        if (inst->item == m->n_elems) {
            inst->item = 0;
            inst->phase = I_ACTIVE;
            return WASM_EXEC_MORE;
        }
        e = &m->elems[inst->item];
        x = wasm_store_elem(s, inst->elems[inst->item]);
        if (inst->sub == e->n_items) return next(inst, I_ELEMS);
        if (!e->exprs) {
            uint32_t f = m->elem_funcs[e->items + inst->sub];

            wasm_refs_set(x->refs, inst->sub++, (WasmValue){ (uint64_t)inst->funcs[f] + 1, 0 });
            JS_FlowRetireWork(1);
            return WASM_EXEC_MORE;
        }
        st = evaluate(inst, m->elem_exprs[e->items + inst->sub], &v);
        if (st != WASM_EXEC_DONE) return st;
        wasm_refs_set(x->refs, inst->sub++, v);
        return WASM_EXEC_MORE;
    }
    case I_ACTIVE: {
        /* §4.7.2 steps for an active segment: table.init over all of it at its offset, then elem.drop; a
           declarative one is dropped. */
        const WasmElem *e;

        if (inst->item == m->n_elems) {
            inst->item = 0;
            inst->phase = I_DATA;
            return WASM_EXEC_MORE;
        }
        e = &m->elems[inst->item];
        if (e->mode == WASM_SEGMENT_PASSIVE) return next(inst, I_ACTIVE);
        if (e->mode == WASM_SEGMENT_DECLARATIVE) {
            wasm_refs_drop(wasm_store_elem(s, inst->elems[inst->item])->refs);
            return next(inst, I_ACTIVE);
        }
        st = evaluate(inst, e->offset, &v);
        if (st != WASM_EXEC_DONE) return st;
        if (v.lo > wasm_refs_length(table(inst, e->table)->refs)
            || e->n_items > wasm_refs_length(table(inst, e->table)->refs) - v.lo)
            return WASM_EXEC_TRAP_TABLE_BOUNDS;
        inst->offset = v.lo;
        inst->phase = I_ACTIVE_COPY;
        return WASM_EXEC_MORE;
    }
    case I_ACTIVE_COPY: {
        const WasmElem *e = &m->elems[inst->item];
        const WasmElemInstance *x = wasm_store_elem(s, inst->elems[inst->item]);

        if (inst->sub == e->n_items) {
            wasm_refs_drop(x->refs);
            return next(inst, I_ACTIVE);
        }
        wasm_refs_set(table(inst, e->table)->refs, inst->offset + inst->sub, wasm_refs_get(x->refs, inst->sub));
        inst->sub++;
        JS_FlowRetireWork(1);
        return WASM_EXEC_MORE;
    }
    case I_DATA: {
        /* §4.7.2's rundata for an active segment: memory.init over all of it at its offset, then data.drop. */
        const WasmData *d;

        if (inst->item == m->n_datas) {
            inst->phase = I_START;
            return WASM_EXEC_MORE;
        }
        d = &m->datas[inst->item];
        if (d->mode == WASM_SEGMENT_PASSIVE) return next(inst, I_DATA);
        DCHECK(d->mode == WASM_SEGMENT_ACTIVE, "a decoded WebAssembly data segment is neither passive nor active");
        st = evaluate(inst, d->offset, &v);
        if (st != WASM_EXEC_DONE) return st;
        if (!wasm_memory_holds(memory(inst, d->memory), v.lo, wasm_store_data(s, inst->datas[inst->item])->n))
            return WASM_EXEC_TRAP_MEMORY_BOUNDS;
        inst->offset = v.lo;
        inst->phase = I_DATA_COPY;
        return WASM_EXEC_MORE;
    }
    case I_DATA_COPY: {
        const WasmDataInstance *x = wasm_store_data(s, inst->datas[inst->item]);
        uint8_t byte;

        if (inst->sub == x->n) {
            wasm_data_drop(s, inst->datas[inst->item]);
            return next(inst, I_DATA);
        }
        byte = wasm_store_instance(s, x->inst)->bytes[x->offset + inst->sub];
        CHECK(wasm_memory_store(memory(inst, m->datas[inst->item].memory), inst->offset + inst->sub, 1, &byte),
              "a WebAssembly data segment's byte fell outside the bounds its memory.init checked");
        inst->sub++;
        JS_FlowRetireWork(1);
        return WASM_EXEC_MORE;
    }
    case I_START:
        /* §4.7.2 step 29: the start function is invoked as (call x) in the instance's frame. */
        if (inst->starting) return started(inst, wasm_interpreter_step(&inst->start));
        if (!m->has_start) {
            inst->phase = I_DONE;
            return WASM_EXEC_DONE;
        }
        inst->starting = true;
        return started(inst, wasm_interpreter_begin(&inst->start, inst, m->start, NULL, 0));
    default: break;
    }
    CHECK_FAIL("a WebAssembly instantiation was stepped after it answered");
}

void wasm_instance_free(WasmInstance *inst)
{
    DCHECK(inst != NULL, "a WebAssembly instance was freed with no instance");
    if (inst->evaluating) wasm_interpreter_free(&inst->init);
    if (inst->starting) wasm_interpreter_free(&inst->start);
    free(inst->funcs);
    free(inst->globals);
    free(inst->tables);
    free(inst->mems);
    free(inst->elems);
    free(inst->datas);
    memset(inst, 0, sizeof *inst);
}
