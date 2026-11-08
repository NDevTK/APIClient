/* WebAssembly Core Specification (Release 3.0) §4.7.2 "Instantiation". See instance.h. */
#include "core/wasm/instance.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/wasm/linear_memory.h"
#include "core/wasm/reference_vector.h"

enum {
    I_IMPORTS = 0, I_ALLOC, I_ALLOC_IMPORTS, I_ALLOC_FUNCS, I_ALLOC_GLOBALS, I_ALLOC_TABLES, I_ALLOC_MEMS,
    I_ALLOC_ELEMS, I_ALLOC_DATAS, I_GLOBALS, I_TABLES, I_TABLE_FILL, I_ELEMS, I_ACTIVE, I_ACTIVE_COPY, I_DATA,
    I_DATA_COPY, I_START, I_DONE
};

static JSClassID g_class;

static void instance_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    const WasmInstance *inst = JS_GetAnyOpaque(val, &id);

    if (inst != NULL) JS_MarkValue(rt, inst->keep, mark_func);
}

static void instance_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    WasmInstance *inst = JS_GetAnyOpaque(val, &id);

    if (inst == NULL) return;
    JS_FreeValueRT(rt, inst->keep);
    js_free_rt(rt, inst->funcs);
    js_free_rt(rt, inst->globals);
    js_free_rt(rt, inst->tables);
    js_free_rt(rt, inst->mems);
    js_free_rt(rt, inst->elems);
    js_free_rt(rt, inst->datas);
    js_free_rt(rt, inst);
}

void wasm_instance_class_init(JSContext *ctx)
{
    JSClassDef def = { .class_name = "WebAssemblyModuleInstance", .finalizer = instance_finalizer,
                       .gc_mark = instance_gc_mark };

    DCHECK(g_class == 0, "wasm_instance_class_init ran twice; the instance class is declared once per agent");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &def);
    agent_state_class("wasm_instance", &g_class, "§4.2.6's module instance class");
}

void wasm_instance_class_free(void)
{
    g_class = 0;
}

WasmInstance *wasm_instance_of(JSValueConst owner)
{
    WasmInstance *inst = JS_GetOpaque(owner, g_class);

    DCHECK(inst != NULL, "a WebAssembly module instance was read from an object that owns none");
    return inst;
}

/* An index space of `n` addresses, unset. */
static size_t *space(JSContext *ctx, size_t n)
{
    size_t *a = n < SIZE_MAX / sizeof *a ? js_malloc(ctx, (n + 1) * sizeof *a) : NULL;

    CHECK(a != NULL, "a WebAssembly instance's index spaces could not be allocated: out of memory");
    return a;
}

JSValue wasm_instance_new(WasmStore *store, const WasmModule *m, const uint8_t *bytes, const WasmSideTable *side,
                          const WasmTypes *types, JSValueConst keep, const size_t imported[4])
{
    JSContext *ctx = store->ctx;
    WasmInstance *inst;
    JSValue owner;

    DCHECK(g_class != 0, "a WebAssembly instance was made before wasm_instance_class_init declared its class");
    DCHECK(m != NULL && bytes != NULL && side != NULL && types != NULL, "a WebAssembly instance was made without "
           "its module");
    inst = js_mallocz(ctx, sizeof *inst);
    CHECK(inst != NULL, "a WebAssembly instance could not be allocated: out of memory");
    inst->store = store;
    inst->addr = SIZE_MAX;
    inst->module = m;
    inst->bytes = bytes;
    inst->side = side;
    inst->types = types;
    inst->keep = JS_DupValue(ctx, keep);
    inst->imported_funcs = imported[0];
    inst->imported_globals = imported[1];
    inst->imported_tables = imported[2];
    inst->imported_mems = imported[3];
    inst->funcs = space(ctx, imported[0] + m->n_func_types);
    inst->globals = space(ctx, imported[1] + m->n_globals);
    inst->tables = space(ctx, imported[2] + m->n_tables);
    inst->mems = space(ctx, imported[3] + m->n_memories);
    inst->elems = space(ctx, m->n_elems);
    inst->datas = space(ctx, m->n_datas);
    owner = JS_NewObjectProtoClass(ctx, JS_NULL, g_class);
    CHECK(!JS_IsException(owner), "a WebAssembly instance's owner object could not be allocated: out of memory");
    JS_SetOpaque(owner, inst);
    return owner;
}

WasmExecStatus wasm_instantiation_begin(WasmInstantiation *run, WasmStore *store, const WasmModule *m,
                                        const uint8_t *bytes, const WasmSideTable *side, const WasmTypes *types,
                                        const size_t *externs, size_t n_externs, JSValueConst keep)
{
    DCHECK(run != NULL && store != NULL && m != NULL && bytes != NULL && side != NULL && types != NULL,
           "a WebAssembly instantiation began without its module");
    DCHECKF(n_externs == m->n_imports && (n_externs == 0 || externs != NULL), "a WebAssembly instantiation was "
            "given %zu external addresses for %zu imports", n_externs, m->n_imports);
    memset(run, 0, sizeof *run);
    run->store = store;
    run->module = m;
    run->bytes = bytes;
    run->side = side;
    run->types = types;
    run->keep = keep;
    run->addr = SIZE_MAX;
    run->n_externs = n_externs;
    run->externs = n_externs < SIZE_MAX / sizeof *externs ? js_malloc(store->ctx, (n_externs + 1) * sizeof *externs)
                                                           : NULL;
    if (run->externs == NULL) return WASM_EXEC_NO_ROOM;
    if (n_externs) memcpy(run->externs, externs, n_externs * sizeof *externs);
    run->phase = I_IMPORTS;
    return WASM_EXEC_MORE;
}

/* The index of import kind `kind` among the four kinds an instance's spaces hold. */
static size_t kind_index(WasmExternKind kind)
{
    CHECK(kind != WASM_EXTERN_TAG, "a WebAssembly tag import is not built in instance.c");
    return kind == WASM_EXTERN_FUNC ? 0 : kind == WASM_EXTERN_GLOBAL ? 1 : kind == WASM_EXTERN_TABLE ? 2 : 3;
}

/* The move to phase `phase`'s first item. */
static WasmExecStatus enter(WasmInstantiation *run, int phase)
{
    run->item = 0;
    run->sub = 0;
    run->phase = phase;
    return WASM_EXEC_MORE;
}

/* §4.7.2 steps 4 and 5: each external address is valid with its import's type, one import (or one supertype of
   its climb) a step. */
static WasmExecStatus check_import(WasmInstantiation *run)
{
    const WasmModule *m = run->module;
    WasmMatch le;
    size_t k;

    if (run->item == m->n_imports) return enter(run, I_ALLOC);
    k = kind_index(m->imports[run->item].kind);
    if (run->climbing) le = wasm_extern_check_step(&run->check);
    else le = wasm_extern_check_begin(&run->check, run->store, run->types, &m->imports[run->item],
                                      run->externs[run->item]);
    run->climbing = le == WASM_MATCH_CLIMB;
    if (le == WASM_MATCH_NO) return WASM_EXEC_LINK_INCOMPATIBLE;
    if (le == WASM_MATCH_YES) {
        run->passed[k]++;
        run->item++;
    }
    return WASM_EXEC_MORE;
}

/* §4.7.1's allocmodule, one entity a step: the instance record and its type space, then each import's address
   in its index space, then each defined function, global, table, memory, element and data segment. */
static WasmExecStatus allocate(WasmInstantiation *run)
{
    const WasmModule *m = run->module;
    WasmStore *store = run->store;
    WasmInstance *inst = run->inst;
    size_t i = run->item, a;
    JSValue owner;

    switch (run->phase) {
    case I_ALLOC:
        owner = wasm_instance_new(store, m, run->bytes, run->side, run->types, run->keep, run->passed);
        run->addr = wasm_store_add_instance(store, owner);
        JS_FreeValue(store->ctx, owner);
        run->inst = wasm_store_instance(store, run->addr);
        run->inst->addr = run->addr;
        run->type_space = wasm_store_add_types(store, run->types);
        memset(run->passed, 0, sizeof run->passed);
        return enter(run, I_ALLOC_IMPORTS);
    case I_ALLOC_IMPORTS: {
        size_t k;

        if (i == m->n_imports) return enter(run, I_ALLOC_FUNCS);
        k = kind_index(m->imports[i].kind);
        (k == 0 ? inst->funcs : k == 1 ? inst->globals : k == 2 ? inst->tables : inst->mems)[run->passed[k]++]
            = run->externs[i];
        break;
    }
    case I_ALLOC_FUNCS: {
        WasmFuncInstance f = { false, inst->addr, i, 0, run->type_space, 0 };

        if (i == m->n_func_types) return enter(run, I_ALLOC_GLOBALS);
        f.type = m->func_types[i];
        inst->funcs[inst->imported_funcs + i] = wasm_store_add_func(store, f);
        break;
    }
    case I_ALLOC_GLOBALS: {
        WasmGlobalInstance g = { { 0, 0 }, { { 0 }, 0 }, false };

        if (i == m->n_globals) return enter(run, I_ALLOC_TABLES);
        g.type = (WasmStoreValType){ m->globals[i].type, run->type_space };
        g.mutable_ = m->globals[i].mutable_;
        inst->globals[inst->imported_globals + i] = wasm_store_add_global(store, g);
        break;
    }
    case I_ALLOC_TABLES:
        if (i == m->n_tables) return enter(run, I_ALLOC_MEMS);
        a = wasm_store_add_table(store, &m->tables[i].limits,
                                 (WasmStoreValType){ m->tables[i].type, run->type_space });
        if (a == SIZE_MAX) return WASM_EXEC_NO_ROOM;
        inst->tables[inst->imported_tables + i] = a;
        break;
    case I_ALLOC_MEMS:
        if (i == m->n_memories) return enter(run, I_ALLOC_ELEMS);
        a = wasm_store_add_mem(store, &m->memories[i]);
        if (a == SIZE_MAX) return WASM_EXEC_NO_ROOM;
        inst->mems[inst->imported_mems + i] = a;
        break;
    case I_ALLOC_ELEMS:
        if (i == m->n_elems) return enter(run, I_ALLOC_DATAS);
        inst->elems[i] = wasm_store_add_elem(store, m->elems[i].n_items);
        if (inst->elems[i] == SIZE_MAX) return WASM_EXEC_NO_ROOM;
        break;
    case I_ALLOC_DATAS:
        if (i == m->n_datas) return enter(run, I_GLOBALS);
        inst->datas[i] = wasm_store_add_data(store, (WasmDataInstance){ inst->addr, m->datas[i].bytes_offset,
                                                                         m->datas[i].bytes_size });
        break;
    default:
        CHECK_FAIL("a WebAssembly instantiation allocated in a phase that is not allocmodule's");
    }
    run->item++;
    return WASM_EXEC_MORE;
}

/* Evaluate constant expression `e`, one instruction a step: MORE while it runs; DONE with its value in `*out`. */
static WasmExecStatus evaluate(WasmInstantiation *run, WasmExpr e, WasmValue *out)
{
    WasmExecStatus st;

    if (!run->evaluating) {
        st = wasm_interpreter_begin_expr(&run->init, run->inst, e);
        run->evaluating = st == WASM_EXEC_MORE;
        if (!run->evaluating) wasm_interpreter_free(&run->init);
        return st;
    }
    st = wasm_interpreter_step(&run->init);
    if (st == WASM_EXEC_MORE) return st;
    run->evaluating = false;
    if (st == WASM_EXEC_DONE) {
        DCHECK(run->init.n_vals == 1, "a validated WebAssembly constant expression gave other than one value");
        *out = run->init.vals[0];
    }
    wasm_interpreter_free(&run->init);
    return st;
}

/* The owner of memory `x` of the instance. */
static JSValueConst memory(WasmInstantiation *run, size_t x)
{
    DCHECKF(run->inst->mems[x] < run->store->mems.n, "a WebAssembly instance's memory %zu has no store address",
            x);
    return wasm_store_mem(run->store, run->inst->mems[x]);
}

static WasmExecStatus next(WasmInstantiation *run, int phase)
{
    run->item++;
    run->sub = 0;
    run->phase = phase;
    return WASM_EXEC_MORE;
}

static const WasmTableInstance *table(WasmInstantiation *run, size_t x)
{
    DCHECKF(run->inst->tables[x] < run->store->tables.n, "a WebAssembly instance's table %zu has no store address",
            x);
    return wasm_store_table(run->store, run->inst->tables[x]);
}

/* The start function's status `st`: while it runs, the instantiation's; once it answers, the instantiation's
   end. */
static WasmExecStatus started(WasmInstantiation *run, WasmExecStatus st)
{
    if (st == WASM_EXEC_MORE || st == WASM_EXEC_FORK || st == WASM_EXEC_HOST_CALL) return st;
    wasm_interpreter_free(&run->start);
    run->starting = false;
    run->phase = I_DONE;
    return st;
}

WasmExecStatus wasm_instantiation_choose(WasmInstantiation *run, bool example)
{
    DCHECK(run != NULL && run->starting, "a WebAssembly instantiation's choice was made with no start function "
           "running");
    return started(run, wasm_interpreter_choose(&run->start, example));
}

WasmExecStatus wasm_instantiation_return(WasmInstantiation *run, const WasmValue *results, size_t n)
{
    DCHECK(run != NULL && run->starting, "a WebAssembly instantiation's host call was answered with no start "
           "function running");
    return started(run, wasm_interpreter_return(&run->start, results, n));
}

/* One step of the phases after allocmodule. */
static WasmExecStatus initialize(WasmInstantiation *run)
{
    const WasmModule *m = run->module;
    WasmStore *s = run->store;
    WasmInstance *inst = run->inst;
    WasmExecStatus st;
    WasmValue v = { 0, 0 };

    switch (run->phase) {
    case I_GLOBALS:
        /* §4.7.2 step 19's evalglobal, each value written to the global allocmodule made. */
        if (run->item == m->n_globals) return enter(run, I_TABLES);
        st = evaluate(run, (WasmExpr){ m->globals[run->item].init_offset, m->globals[run->item].init_size }, &v);
        if (st != WASM_EXEC_DONE) return st;
        wasm_global_set(s, inst->globals[inst->imported_globals + run->item], v);
        return next(run, I_GLOBALS);
    case I_TABLES:
        /* Step 20: a table's initializer. ref.null's 0 is the store's zeroed storage; another value fills it. */
        if (run->item == m->n_tables) return enter(run, I_ELEMS);
        if (!m->tables[run->item].has_init) return next(run, I_TABLES);
        st = evaluate(run, m->tables[run->item].init, &v);
        if (st != WASM_EXEC_DONE) return st;
        if (v.lo == 0) return next(run, I_TABLES);
        run->offset = v.lo;   /* I_TABLE_FILL's reference */
        run->phase = I_TABLE_FILL;
        return WASM_EXEC_MORE;
    case I_TABLE_FILL: {
        const WasmTableInstance *t = table(run, inst->imported_tables + run->item);

        if (run->sub == wasm_refs_length(t->refs)) return next(run, I_TABLES);
        wasm_refs_set(t->refs, run->sub++, (WasmValue){ run->offset, 0 });
        return WASM_EXEC_MORE;
    }
    case I_ELEMS: {
        /* Step 21: each element segment's items, a function index's reference or its expression's value. */
        const WasmElem *e;
        const WasmElemInstance *x;

        if (run->item == m->n_elems) return enter(run, I_ACTIVE);
        e = &m->elems[run->item];
        x = wasm_store_elem(s, inst->elems[run->item]);
        if (run->sub == e->n_items) return next(run, I_ELEMS);
        if (!e->exprs) {
            uint32_t f = m->elem_funcs[e->items + run->sub];

            wasm_refs_set(x->refs, run->sub++, (WasmValue){ (uint64_t)inst->funcs[f] + 1, 0 });
            return WASM_EXEC_MORE;
        }
        st = evaluate(run, m->elem_exprs[e->items + run->sub], &v);
        if (st != WASM_EXEC_DONE) return st;
        wasm_refs_set(x->refs, run->sub++, v);
        return WASM_EXEC_MORE;
    }
    case I_ACTIVE: {
        /* Step 27, §4.7.2's runelem: an active segment's table.init over all of it at its offset, then
           elem.drop; a declarative one is dropped. */
        const WasmElem *e;

        if (run->item == m->n_elems) return enter(run, I_DATA);
        e = &m->elems[run->item];
        if (e->mode == WASM_SEGMENT_PASSIVE) return next(run, I_ACTIVE);
        if (e->mode == WASM_SEGMENT_DECLARATIVE) {
            wasm_refs_drop(wasm_store_elem(s, inst->elems[run->item])->refs);
            return next(run, I_ACTIVE);
        }
        st = evaluate(run, e->offset, &v);
        if (st != WASM_EXEC_DONE) return st;
        if (v.lo > wasm_refs_length(table(run, e->table)->refs)
            || e->n_items > wasm_refs_length(table(run, e->table)->refs) - v.lo)
            return WASM_EXEC_TRAP_TABLE_BOUNDS;
        run->offset = v.lo;
        run->phase = I_ACTIVE_COPY;
        return WASM_EXEC_MORE;
    }
    case I_ACTIVE_COPY: {
        const WasmElem *e = &m->elems[run->item];
        const WasmElemInstance *x = wasm_store_elem(s, inst->elems[run->item]);

        if (run->sub == e->n_items) {
            wasm_refs_drop(x->refs);
            return next(run, I_ACTIVE);
        }
        wasm_refs_set(table(run, e->table)->refs, run->offset + run->sub, wasm_refs_get(x->refs, run->sub));
        run->sub++;
        return WASM_EXEC_MORE;
    }
    case I_DATA: {
        /* Step 28, §4.7.2's rundata: an active segment's memory.init over all of it at its offset, then
           data.drop. */
        const WasmData *d;

        if (run->item == m->n_datas) return enter(run, I_START);
        d = &m->datas[run->item];
        if (d->mode == WASM_SEGMENT_PASSIVE) return next(run, I_DATA);
        DCHECK(d->mode == WASM_SEGMENT_ACTIVE, "a decoded WebAssembly data segment is neither passive nor active");
        st = evaluate(run, d->offset, &v);
        if (st != WASM_EXEC_DONE) return st;
        if (!wasm_memory_holds(memory(run, d->memory), v.lo, wasm_store_data(s, inst->datas[run->item])->n))
            return WASM_EXEC_TRAP_MEMORY_BOUNDS;
        run->offset = v.lo;
        run->phase = I_DATA_COPY;
        return WASM_EXEC_MORE;
    }
    case I_DATA_COPY: {
        const WasmDataInstance *x = wasm_store_data(s, inst->datas[run->item]);
        uint8_t byte;

        if (run->sub == x->n) {
            wasm_data_drop(s, inst->datas[run->item]);
            return next(run, I_DATA);
        }
        byte = wasm_store_instance(s, x->inst)->bytes[x->offset + run->sub];
        CHECK(wasm_memory_store(memory(run, m->datas[run->item].memory), run->offset + run->sub, 1, &byte),
              "a WebAssembly data segment's byte fell outside the bounds its memory.init checked");
        run->sub++;
        return WASM_EXEC_MORE;
    }
    case I_START:
        /* Step 29: the start function is invoked as (call x) in the instance's frame. */
        if (run->starting) return started(run, wasm_interpreter_step(&run->start));
        if (!m->has_start) {
            run->phase = I_DONE;
            return WASM_EXEC_DONE;
        }
        run->starting = true;
        return started(run, wasm_interpreter_begin(&run->start, inst, m->start, NULL, 0));
    default: break;
    }
    CHECK_FAIL("a WebAssembly instantiation was stepped after it answered");
}

WasmExecStatus wasm_instantiation_step(WasmInstantiation *run)
{
    DCHECK(run != NULL && run->store != NULL, "a WebAssembly instantiation was stepped before it began");
    /* An initializer's and the start function's instructions are the interpreter's to charge. */
    if (!run->evaluating && !run->starting) JS_FlowRetireWork(1);
    if (run->phase == I_IMPORTS) return check_import(run);
    if (run->phase <= I_ALLOC_DATAS) return allocate(run);
    return initialize(run);
}

void wasm_instantiation_visit(JSContext *ctx, WasmInstantiation *run, JSStepVisit *v)
{
    v->buf(ctx, (void **)&run->externs, (run->n_externs + 1) * sizeof *run->externs);
    wasm_interpreter_visit(ctx, &run->init, v);
    wasm_interpreter_visit(ctx, &run->start, v);
}

void wasm_instantiation_free(WasmInstantiation *run)
{
    DCHECK(run != NULL && run->store != NULL, "a WebAssembly instantiation was freed before it began");
    wasm_interpreter_free(&run->init);
    wasm_interpreter_free(&run->start);
    js_free(run->store->ctx, run->externs);
    memset(run, 0, sizeof *run);
}
