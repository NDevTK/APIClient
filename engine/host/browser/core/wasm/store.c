/* WebAssembly Core Specification (Release 3.0) §4.2.3 "Store". See store.h. */
#include "core/wasm/store.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"

static void room(void **p, size_t *cap, size_t n, size_t elem)
{
    size_t c;
    void *q;

    if (n < *cap) return;
    c = *cap ? *cap * 2 : 16;
    CHECK(c <= SIZE_MAX / elem, "a WebAssembly store's list could not grow: out of memory");
    q = realloc(*p, c * elem);
    CHECK(q != NULL, "a WebAssembly store's list could not grow: out of memory");
    *p = q;
    *cap = c;
}

void wasm_store_init(WasmStore *s)
{
    DCHECK(s != NULL, "a WebAssembly store was initialized with no store");
    memset(s, 0, sizeof *s);
}

size_t wasm_store_add_func(WasmStore *s, WasmFuncInstance f)
{
    room((void **)&s->funcs, &s->cap_funcs, s->n_funcs, sizeof *s->funcs);
    s->funcs[s->n_funcs] = f;
    return s->n_funcs++;
}

size_t wasm_store_add_global(WasmStore *s, WasmValue v)
{
    room((void **)&s->globals, &s->cap_globals, s->n_globals, sizeof *s->globals);
    s->globals[s->n_globals] = v;
    return s->n_globals++;
}

/* §4.7.1's alloctable: `min` null references, calloc's zeros; an initializer's other value is instance.c's to
   fill. The count is the page's input, so storage that cannot be had answers SIZE_MAX (JS API §5's RangeError),
   never a CHECK. */
size_t wasm_store_add_table(WasmStore *s, const WasmLimits *type)
{
    WasmTableInstance *t;
    WasmValue *elems;

    if (type->min > SIZE_MAX / sizeof *elems) return SIZE_MAX;
    elems = calloc(type->min ? (size_t)type->min : 1, sizeof *elems);
    if (elems == NULL) return SIZE_MAX;
    room((void **)&s->tables, &s->cap_tables, s->n_tables, sizeof *s->tables);
    t = &s->tables[s->n_tables];
    memset(t, 0, sizeof *t);
    t->type = *type;
    t->elems = elems;
    t->size = t->cap = type->min;
    return s->n_tables++;
}

size_t wasm_store_add_elem(WasmStore *s, uint64_t n)
{
    WasmElemInstance *e;
    WasmValue *refs;

    if (n > SIZE_MAX / sizeof *refs) return SIZE_MAX;
    refs = calloc(n ? (size_t)n : 1, sizeof *refs);
    if (refs == NULL) return SIZE_MAX;
    room((void **)&s->elems, &s->cap_elems, s->n_elems, sizeof *s->elems);
    e = &s->elems[s->n_elems];
    e->refs = refs;
    e->n = n;
    return s->n_elems++;
}

size_t wasm_store_add_instance(WasmStore *s, WasmInstance *inst)
{
    room((void **)&s->instances, &s->cap_instances, s->n_instances, sizeof *s->instances);
    s->instances[s->n_instances] = inst;
    return s->n_instances++;
}

void wasm_store_free(WasmStore *s)
{
    size_t i;

    DCHECK(s != NULL, "a WebAssembly store was freed with no store");
    for (i = 0; i < s->n_tables; i++) free(s->tables[i].elems);
    for (i = 0; i < s->n_elems; i++) free(s->elems[i].refs);
    free(s->funcs);
    free(s->globals);
    free(s->tables);
    free(s->elems);
    free(s->instances);
    memset(s, 0, sizeof *s);
}
