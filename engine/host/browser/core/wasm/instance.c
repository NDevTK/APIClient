/* WebAssembly Core Specification (Release 3.0) §4.7.2 "Instantiation". See instance.h. */
#include "core/wasm/instance.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"

enum { I_IMPORTS = 0, I_ALLOC, I_GLOBALS, I_TABLES, I_TABLE_FILL, I_ELEMS, I_ACTIVE, I_ACTIVE_COPY, I_DONE };

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
    size_t i, n_funcs, n_globals, n_tables, k[3] = { 0, 0, 0 };

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
    n_funcs = inst->imported_funcs + m->n_func_types;
    n_globals = inst->imported_globals + m->n_globals;
    n_tables = inst->imported_tables + m->n_tables;
    inst->funcs = malloc((n_funcs + 1) * sizeof *inst->funcs);
    inst->globals = malloc((n_globals + 1) * sizeof *inst->globals);
    inst->tables = malloc((n_tables + 1) * sizeof *inst->tables);
    inst->elems = malloc((m->n_elems + 1) * sizeof *inst->elems);
    CHECK(inst->funcs && inst->globals && inst->tables && inst->elems,
          "a WebAssembly instance's index spaces could not be allocated: out of memory");
    /* §4.7.2's moduleinst: the imports' index spaces come first, from the external addresses. */
    for (i = 0; i < m->n_imports; i++) {
        WasmExternKind kind = m->imports[i].kind;

        CHECKF(kind != WASM_EXTERN_MEMORY && kind != WASM_EXTERN_TAG, "a WebAssembly %s import is not built in "
               "instance.c", kind == WASM_EXTERN_MEMORY ? "memory" : "tag");
        if (kind == WASM_EXTERN_FUNC) inst->funcs[k[0]++] = externs[i];
        else if (kind == WASM_EXTERN_GLOBAL) inst->globals[k[1]++] = externs[i];
        else inst->tables[k[2]++] = externs[i];
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
    for (i = 0; i < m->n_elems; i++) {
        inst->elems[i] = wasm_store_add_elem(store, m->elems[i].n_items);
        if (inst->elems[i] == SIZE_MAX) return WASM_EXEC_NO_ROOM;
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
    k = im->kind == WASM_EXTERN_FUNC ? 0 : im->kind == WASM_EXTERN_GLOBAL ? 1 : 2;
    JS_FlowRetireWork(1);
    if (inst->climbing) {
        le = wasm_extern_check_step(&inst->check);
    } else {
        a = k == 0 ? inst->funcs[inst->passed[0]] : k == 1 ? inst->globals[inst->passed[1]]
                                                           : inst->tables[inst->passed[2]];
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

static WasmExecStatus next(WasmInstance *inst, int phase)
{
    inst->item++;
    inst->sub = 0;
    inst->phase = phase;
    return WASM_EXEC_MORE;
}

static WasmTableInstance *table(WasmInstance *inst, size_t x)
{
    DCHECKF(inst->tables[x] < inst->store->n_tables, "a WebAssembly instance's table %zu has no store address",
            x);
    return &inst->store->tables[inst->tables[x]];
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
            s->globals[inst->globals[inst->imported_globals + inst->item]].value = v;
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
        WasmTableInstance *t = table(inst, inst->imported_tables + inst->item);

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
        x = &s->elems[inst->elems[inst->item]];
        if (inst->sub == e->n_items) return next(inst, I_ELEMS);
        if (!e->exprs) {
            uint32_t f = m->elem_funcs[e->items + inst->sub];

            x->refs[inst->sub].lo = (uint64_t)inst->funcs[f] + 1;
            inst->sub++;
            JS_FlowRetireWork(1);
            return WASM_EXEC_MORE;
        }
        st = evaluate(inst, m->elem_exprs[e->items + inst->sub], &v);
        if (st != WASM_EXEC_DONE) return st;
        s->elems[inst->elems[inst->item]].refs[inst->sub++] = v;
        return WASM_EXEC_MORE;
    }
    case I_ACTIVE: {
        /* §4.7.2 steps for an active segment: table.init over all of it at its offset, then elem.drop; a
           declarative one is dropped. */
        const WasmElem *e;

        if (inst->item == m->n_elems) {
            inst->phase = I_DONE;
            return WASM_EXEC_DONE;
        }
        e = &m->elems[inst->item];
        if (e->mode == WASM_SEGMENT_PASSIVE) return next(inst, I_ACTIVE);
        if (e->mode == WASM_SEGMENT_DECLARATIVE) {
            s->elems[inst->elems[inst->item]].n = 0;
            return next(inst, I_ACTIVE);
        }
        st = evaluate(inst, e->offset, &v);
        if (st != WASM_EXEC_DONE) return st;
        if (v.lo > table(inst, e->table)->size || e->n_items > table(inst, e->table)->size - v.lo)
            return WASM_EXEC_TRAP_TABLE_BOUNDS;
        inst->offset = v.lo;
        inst->phase = I_ACTIVE_COPY;
        return WASM_EXEC_MORE;
    }
    case I_ACTIVE_COPY: {
        const WasmElem *e = &m->elems[inst->item];
        WasmElemInstance *x = &s->elems[inst->elems[inst->item]];

        if (inst->sub == e->n_items) {
            x->n = 0;
            return next(inst, I_ACTIVE);
        }
        table(inst, e->table)->elems[inst->offset + inst->sub] = x->refs[inst->sub];
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
    DCHECK(inst != NULL, "a WebAssembly instance was freed with no instance");
    if (inst->evaluating) wasm_interpreter_free(&inst->init);
    free(inst->funcs);
    free(inst->globals);
    free(inst->tables);
    free(inst->elems);
    memset(inst, 0, sizeof *inst);
}
