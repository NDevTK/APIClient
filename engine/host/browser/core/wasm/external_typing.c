/* WebAssembly Core Specification (Release 3.0) §4.5.2 "External Typing". See external_typing.h. */
#include "core/wasm/external_typing.h"

#include "check.h"
#include "core/wasm/linear_memory.h"

/* The type space a store value type's concrete heap index reads; a type that names no defined type reads none,
   so any serves. */
static const WasmTypes *types_of(const WasmStore *s, const WasmStoreValType *t, const WasmTypes *any)
{
    if (t->types == SIZE_MAX) {
        DCHECK(t->type.code != WASM_TYPE_REF || t->type.heap != 0,
               "a WebAssembly store type with no type space names a defined type");
        return any;
    }
    DCHECK(t->types < s->types.n, "a WebAssembly store type's type space is past the store's");
    return wasm_store_types(s, t->types);
}

/* §3.3.11 under §3.3.14 and §3.3.15: at least the import's minimum, and within its maximum when it has one;
   the same address type. */
static bool limits_le(bool addr64, uint64_t min, bool has_max, uint64_t max, const WasmLimits *im)
{
    if (addr64 != im->addr64 || min < im->min) return false;
    return !im->has_max || (has_max && max <= im->max);
}

static WasmMatch answer(WasmExternCheck *c, const WasmTypes *types, WasmMatch le)
{
    if (le == WASM_MATCH_CLIMB) c->types = types;
    return le;
}

WasmMatch wasm_extern_check_begin(WasmExternCheck *c, const WasmStore *s, const WasmTypes *importer,
                                  const WasmImport *im, size_t addr)
{
    DCHECK(c != NULL && s != NULL && importer != NULL && im != NULL, "a WebAssembly import was checked with no "
           "store or module");
    switch (im->kind) {
    case WASM_EXTERN_FUNC: {
        const WasmFuncInstance *f;
        const WasmTypes *ft;
        WasmValType a = { 0 }, b = { 0 };

        DCHECK(addr < s->funcs.n, "a WebAssembly function import was given an address past the store's functions");
        f = wasm_store_func(s, addr);
        DCHECK(f->types < s->types.n, "a WebAssembly function instance's type space is past the store's");
        ft = wasm_store_types(s, f->types);
        a.code = b.code = WASM_TYPE_REF;
        a.heap_index = f->type;
        b.heap_index = im->index;
        return answer(c, ft, wasm_heap_le(ft, &a, importer, &b, &c->climb));
    }
    case WASM_EXTERN_TABLE: {
        const WasmTableInstance *t;
        const WasmTypes *tt;

        DCHECK(addr < s->tables.n, "a WebAssembly table import was given an address past the store's tables");
        t = wasm_store_table(s, addr);
        tt = types_of(s, &t->elem, importer);
        if (!limits_le(t->type.addr64, t->size, t->type.has_max, t->type.max, &im->limits)) return WASM_MATCH_NO;
        return wasm_val_eq(tt, &t->elem.type, importer, &im->type) ? WASM_MATCH_YES : WASM_MATCH_NO;
    }
    case WASM_EXTERN_GLOBAL: {
        const WasmGlobalInstance *g;
        const WasmTypes *gt;

        DCHECK(addr < s->globals.n, "a WebAssembly global import was given an address past the store's globals");
        g = wasm_store_global(s, addr);
        gt = types_of(s, &g->type, importer);
        if (g->mutable_ != im->mutable_) return WASM_MATCH_NO;
        if (g->mutable_) return wasm_val_eq(gt, &g->type.type, importer, &im->type) ? WASM_MATCH_YES : WASM_MATCH_NO;
        return answer(c, gt, wasm_val_le(gt, &g->type.type, importer, &im->type, &c->climb));
    }
    case WASM_EXTERN_MEMORY: {
        WasmLimits t;

        DCHECK(addr < s->mems.n, "a WebAssembly memory import was given an address past the store's memories");
        wasm_memory_type(wasm_store_mem(s, addr), &t);
        return limits_le(t.addr64, t.min, t.has_max, t.max, &im->limits) ? WASM_MATCH_YES : WASM_MATCH_NO;
    }
    case WASM_EXTERN_TAG:
        break;
    }
    CHECK_FAIL("a WebAssembly tag import is not built in external_typing.c");
}

WasmMatch wasm_extern_check_step(WasmExternCheck *c)
{
    DCHECK(c != NULL && c->types != NULL, "a WebAssembly import check was stepped without a climb");
    return wasm_climb(c->types, &c->climb);
}
