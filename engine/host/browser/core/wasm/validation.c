/* WebAssembly Core Specification (Release 3.0) §3.1.6 "Contexts": the index spaces. See validation.h. */
#include "core/wasm/validation.h"

#include "check.h"

/* The import index of imported item `x` of `kind`, or SIZE_MAX for a defined one (x past the imports). */
static size_t imported(const WasmContext *c, WasmExternKind kind, size_t x, size_t n)
{
    DCHECKF(x < n, "a WebAssembly index %zu was looked up past its index space of %zu", x, n);
    DCHECK(c->imports_passed, "a WebAssembly index space was looked up before validation passed its imports");
    return x < c->n_imported[kind] ? c->imports[kind][x] : SIZE_MAX;
}

uint32_t wasm_context_func(const WasmContext *c, size_t x)
{
    size_t i = imported(c, WASM_EXTERN_FUNC, x, c->n_funcs);

    if (i != SIZE_MAX) return c->types.module->imports[i].index;
    return c->types.module->func_types[x - c->n_imported[WASM_EXTERN_FUNC]];
}

const WasmValType *wasm_context_table(const WasmContext *c, size_t x, const WasmLimits **limits)
{
    const WasmModule *m = c->types.module;
    size_t i = imported(c, WASM_EXTERN_TABLE, x, c->n_tables);
    const WasmTable *t;

    if (i != SIZE_MAX) {
        if (limits != NULL) *limits = &m->imports[i].limits;
        return &m->imports[i].type;
    }
    t = &m->tables[x - c->n_imported[WASM_EXTERN_TABLE]];
    if (limits != NULL) *limits = &t->limits;
    return &t->type;
}

const WasmLimits *wasm_context_memory(const WasmContext *c, size_t x)
{
    size_t i = imported(c, WASM_EXTERN_MEMORY, x, c->n_memories);

    if (i != SIZE_MAX) return &c->types.module->imports[i].limits;
    return &c->types.module->memories[x - c->n_imported[WASM_EXTERN_MEMORY]];
}

const WasmValType *wasm_context_global(const WasmContext *c, size_t x, bool *mutable_)
{
    const WasmModule *m = c->types.module;
    size_t i = imported(c, WASM_EXTERN_GLOBAL, x, c->n_globals);
    const WasmGlobal *g;

    if (i != SIZE_MAX) {
        *mutable_ = m->imports[i].mutable_;
        return &m->imports[i].type;
    }
    g = &m->globals[x - c->n_imported[WASM_EXTERN_GLOBAL]];
    *mutable_ = g->mutable_;
    return &g->type;
}

uint32_t wasm_context_tag(const WasmContext *c, size_t x)
{
    size_t i = imported(c, WASM_EXTERN_TAG, x, c->n_tags);

    if (i != SIZE_MAX) return c->types.module->imports[i].index;
    return c->types.module->tags[x - c->n_imported[WASM_EXTERN_TAG]];
}
