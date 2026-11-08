/* WebAssembly Core Specification (Release 3.0) §3.3 "Matching". See matching.h. */
#include "core/wasm/matching.h"

#include "check.h"

static size_t rec_start(const WasmTypes *t, size_t x)
{
    DCHECK(x < t->module->n_types, "a WebAssembly type identity was asked of a type index past the module's");
    return t->module->types[x].rec_start;
}

/* §3.3.10's closed defined types are equal: the same canonical group, the same index in it. */
bool wasm_same_type(const WasmTypes *t, size_t x, size_t y)
{
    return t->group_canon[rec_start(t, x)] == t->group_canon[rec_start(t, y)]
        && x - rec_start(t, x) == y - rec_start(t, y);
}

/* None below every type of the any hierarchy, i31, struct and array below eq, eq below any, and nofunc,
   noextern and noexn below func, extern and exn. */
bool wasm_abstract_le(uint8_t a, uint8_t b)
{
    if (a == b) return true;
    switch (a) {
    case WASM_HEAP_NONE:
        return b == WASM_HEAP_I31 || b == WASM_HEAP_STRUCT || b == WASM_HEAP_ARRAY || b == WASM_HEAP_EQ
            || b == WASM_HEAP_ANY;
    case WASM_HEAP_I31: case WASM_HEAP_STRUCT: case WASM_HEAP_ARRAY:
        return b == WASM_HEAP_EQ || b == WASM_HEAP_ANY;
    case WASM_HEAP_EQ: return b == WASM_HEAP_ANY;
    case WASM_HEAP_NOFUNC: return b == WASM_HEAP_FUNC;
    case WASM_HEAP_NOEXTERN: return b == WASM_HEAP_EXTERN;
    case WASM_HEAP_NOEXN: return b == WASM_HEAP_EXN;
    }
    return false;
}

uint8_t wasm_kind_heap(WasmCompKind k)
{
    return k == WASM_COMP_STRUCT ? WASM_HEAP_STRUCT : k == WASM_COMP_ARRAY ? WASM_HEAP_ARRAY : WASM_HEAP_FUNC;
}

WasmMatch wasm_heap_le(const WasmTypes *t, const WasmValType *a, const WasmValType *b, WasmClimb *climb)
{
    const WasmModule *m = t->module;

    if (a->heap != 0 && b->heap != 0) return wasm_abstract_le(a->heap, b->heap) ? WASM_MATCH_YES : WASM_MATCH_NO;
    if (a->heap == 0 && b->heap != 0)
        return wasm_abstract_le(wasm_kind_heap(m->types[a->heap_index].kind), b->heap) ? WASM_MATCH_YES : WASM_MATCH_NO;
    if (a->heap != 0) {
        uint8_t k = wasm_kind_heap(m->types[b->heap_index].kind);

        if (a->heap == WASM_HEAP_NONE) return k != WASM_HEAP_FUNC ? WASM_MATCH_YES : WASM_MATCH_NO;
        return a->heap == WASM_HEAP_NOFUNC && k == WASM_HEAP_FUNC ? WASM_MATCH_YES : WASM_MATCH_NO;
    }
    if (wasm_same_type(t, a->heap_index, b->heap_index)) return WASM_MATCH_YES;
    if (m->types[a->heap_index].n_supers == 0) return WASM_MATCH_NO;
    climb->at = m->type_indices[m->types[a->heap_index].supers];
    climb->target = b->heap_index;
    return WASM_MATCH_CLIMB;
}

/* A number, vector or packed type matches only itself, a reference type by nullability (§3.3.4) and heap type. */
WasmMatch wasm_val_le(const WasmTypes *t, const WasmValType *a, const WasmValType *b, WasmClimb *climb)
{
    if (a->code != b->code) return WASM_MATCH_NO;
    if (a->code != WASM_TYPE_REF) return WASM_MATCH_YES;
    if (a->nullable && !b->nullable) return WASM_MATCH_NO;
    return wasm_heap_le(t, a, b, climb);
}

bool wasm_val_eq(const WasmTypes *t, const WasmValType *a, const WasmValType *b)
{
    if (a->code != b->code || a->nullable != b->nullable || a->heap != b->heap) return false;
    return a->code != WASM_TYPE_REF || a->heap != 0 || wasm_same_type(t, a->heap_index, b->heap_index);
}

/* The same mutability, then a mutable field's storage both ways and an immutable one's covariantly. */
WasmMatch wasm_field_le(const WasmTypes *t, const WasmFieldType *a, const WasmFieldType *b, WasmClimb *climb)
{
    if (a->mutable_ != b->mutable_) return WASM_MATCH_NO;
    if (a->mutable_) return wasm_val_eq(t, &a->storage, &b->storage) ? WASM_MATCH_YES : WASM_MATCH_NO;
    return wasm_val_le(t, &a->storage, &b->storage, climb);
}

WasmMatch wasm_climb(const WasmTypes *t, WasmClimb *climb)
{
    const WasmModule *m = t->module;

    if (wasm_same_type(t, climb->at, climb->target)) return WASM_MATCH_YES;
    if (m->types[climb->at].n_supers == 0) return WASM_MATCH_NO;
    climb->at = m->type_indices[m->types[climb->at].supers];
    return WASM_MATCH_CLIMB;
}
