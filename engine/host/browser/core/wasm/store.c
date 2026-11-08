/* WebAssembly Core Specification (Release 3.0) §4.2.3 "Store". See store.h. */
#include "core/wasm/store.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"
#include "core/wasm/linear_memory.h"

static JSClassID g_class;

/* Entry `i` of `l`, of `size` bytes, which must exist. */
static void *list_at(const WasmList *l, size_t i, size_t size, const char *what)
{
    DCHECKF(i < l->n, "a WebAssembly store has no %s at address %zu (it has %zu)", what, i, l->n);
    return l->chunks[i / WASM_LIST_CHUNK] + (i % WASM_LIST_CHUNK) * size;
}

/* A new zeroed entry of `size` bytes at the end of `l`, in a new chunk when the last is full. */
static void *list_append(WasmList *l, size_t size)
{
    if (l->n == l->n_chunks * WASM_LIST_CHUNK) {
        if (l->n_chunks == l->cap_chunks) {
            size_t cap = l->cap_chunks ? l->cap_chunks * 2 : 4;
            uint8_t **grown;

            CHECK(cap <= SIZE_MAX / sizeof *grown, "a WebAssembly store list could not grow: out of memory");
            grown = realloc(l->chunks, cap * sizeof *grown);
            CHECK(grown != NULL, "a WebAssembly store list could not grow: out of memory");
            l->chunks = grown;
            l->cap_chunks = cap;
        }
        l->chunks[l->n_chunks] = calloc(WASM_LIST_CHUNK, size);
        CHECK(l->chunks[l->n_chunks] != NULL, "a WebAssembly store list could not grow: out of memory");
        l->n_chunks++;
    }
    l->n++;
    return list_at(l, l->n - 1, size, "entry");
}

static void list_free(WasmList *l)
{
    size_t i;

    for (i = 0; i < l->n_chunks; i++) free(l->chunks[i]);
    free(l->chunks);
    memset(l, 0, sizeof *l);
}

WasmFuncInstance *wasm_store_func(const WasmStore *s, size_t a)
{
    return list_at(&s->funcs, a, sizeof(WasmFuncInstance), "function");
}

WasmGlobalInstance *wasm_store_global(const WasmStore *s, size_t a)
{
    return list_at(&s->globals, a, sizeof(WasmGlobalInstance), "global");
}

WasmTableInstance *wasm_store_table(const WasmStore *s, size_t a)
{
    return list_at(&s->tables, a, sizeof(WasmTableInstance), "table");
}

JSValueConst wasm_store_mem(const WasmStore *s, size_t a)
{
    return *(JSValue *)list_at(&s->mems, a, sizeof(JSValue), "memory");
}

WasmElemInstance *wasm_store_elem(const WasmStore *s, size_t a)
{
    return list_at(&s->elems, a, sizeof(WasmElemInstance), "element segment");
}

WasmDataInstance *wasm_store_data(const WasmStore *s, size_t a)
{
    return list_at(&s->datas, a, sizeof(WasmDataInstance), "data segment");
}

WasmInstance *wasm_store_instance(const WasmStore *s, size_t a)
{
    return *(WasmInstance **)list_at(&s->instances, a, sizeof(WasmInstance *), "module instance");
}

const WasmTypes *wasm_store_types(const WasmStore *s, size_t a)
{
    return *(const WasmTypes **)list_at(&s->types, a, sizeof(const WasmTypes *), "type space");
}

WasmValue *wasm_table_elem(const WasmTableInstance *t, uint64_t i)
{
    DCHECKF(i < t->size && t->size <= t->elems.n, "a WebAssembly table access at %llu is past its size %llu",
            (unsigned long long)i, (unsigned long long)t->size);
    return list_at(&t->elems, (size_t)i, sizeof(WasmValue), "table element");
}

void wasm_table_grow(WasmTableInstance *t, uint64_t n)
{
    uint64_t k;

    DCHECK(t->size + n >= t->size && t->size + n <= SIZE_MAX, "a WebAssembly table grew past what growtable "
           "admits");
    for (k = t->elems.n; k < t->size + n; k++) list_append(&t->elems, sizeof(WasmValue));
    t->size += n;
}

size_t wasm_store_add_func(WasmStore *s, WasmFuncInstance f)
{
    *(WasmFuncInstance *)list_append(&s->funcs, sizeof f) = f;
    return s->funcs.n - 1;
}

size_t wasm_store_add_global(WasmStore *s, WasmGlobalInstance g)
{
    *(WasmGlobalInstance *)list_append(&s->globals, sizeof g) = g;
    return s->globals.n - 1;
}

/* §4.7.1's alloctable: `min` null references (calloc's zeros); an initializer's other value is instance.c's to
   fill. The count is the page's input, so storage a size_t cannot count answers SIZE_MAX (JS API §5's
   RangeError), never a CHECK. */
size_t wasm_store_add_table(WasmStore *s, const WasmLimits *type, WasmStoreValType elem)
{
    WasmTableInstance *t;

    if (type->min > SIZE_MAX / sizeof(WasmValue)) return SIZE_MAX;
    t = list_append(&s->tables, sizeof *t);
    t->type = *type;
    t->elem = elem;
    wasm_table_grow(t, type->min);
    return s->tables.n - 1;
}

/* §4.7.1's allocmem; storage the page's type asks for that cannot be had answers SIZE_MAX. */
size_t wasm_store_add_mem(WasmStore *s, const WasmLimits *type)
{
    JSValue mem = wasm_memory_new(s->ctx, type);

    if (JS_IsNull(mem)) return SIZE_MAX;
    *(JSValue *)list_append(&s->mems, sizeof mem) = mem;
    return s->mems.n - 1;
}

size_t wasm_store_add_elem(WasmStore *s, uint64_t n)
{
    WasmElemInstance *e;
    WasmValue *refs;

    if (n > SIZE_MAX / sizeof *refs) return SIZE_MAX;
    refs = calloc(n ? (size_t)n : 1, sizeof *refs);
    if (refs == NULL) return SIZE_MAX;
    e = list_append(&s->elems, sizeof *e);
    e->refs = refs;
    e->n = n;
    return s->elems.n - 1;
}

size_t wasm_store_add_data(WasmStore *s, WasmDataInstance d)
{
    DCHECK(d.inst < s->instances.n, "a WebAssembly data instance names no module instance");
    *(WasmDataInstance *)list_append(&s->datas, sizeof d) = d;
    return s->datas.n - 1;
}

size_t wasm_store_add_instance(WasmStore *s, WasmInstance *inst)
{
    *(WasmInstance **)list_append(&s->instances, sizeof inst) = inst;
    return s->instances.n - 1;
}

size_t wasm_store_add_types(WasmStore *s, const WasmTypes *types)
{
    DCHECK(types != NULL && types->registry != NULL, "a WebAssembly type space was added with no registry");
    DCHECK(s->types.n == 0 || wasm_store_types(s, 0)->registry == types->registry,
           "a WebAssembly store's type spaces use two type registries");
    *(const WasmTypes **)list_append(&s->types, sizeof types) = types;
    return s->types.n - 1;
}

/* Release every instance's storage, as the owner's finalizer; the module instances are their owners'. */
static void store_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    WasmStore *s = JS_GetAnyOpaque(val, &id);
    size_t i;

    if (s == NULL) return;
    for (i = 0; i < s->tables.n; i++) list_free(&wasm_store_table(s, i)->elems);
    for (i = 0; i < s->elems.n; i++) free(wasm_store_elem(s, i)->refs);
    for (i = 0; i < s->mems.n; i++) JS_FreeValueRT(rt, *(JSValue *)list_at(&s->mems, i, sizeof(JSValue), "memory"));
    list_free(&s->funcs);
    list_free(&s->globals);
    list_free(&s->tables);
    list_free(&s->mems);
    list_free(&s->elems);
    list_free(&s->datas);
    list_free(&s->instances);
    list_free(&s->types);
    free(s);
}

WasmStore *wasm_store_new(JSContext *ctx)
{
    WasmStore *s;

    DCHECK(ctx != NULL && g_class != 0, "a WebAssembly store was made before wasm_store_class_init declared its "
           "class");
    s = calloc(1, sizeof *s);
    CHECK(s != NULL, "a WebAssembly store could not be allocated: out of memory");
    s->ctx = ctx;
    s->owner = JS_NewObjectProtoClass(ctx, JS_NULL, g_class);
    CHECK(!JS_IsException(s->owner), "a WebAssembly store's owner object could not be allocated: out of memory");
    JS_SetOpaque(s->owner, s);
    return s;
}

void wasm_store_release(WasmStore *s)
{
    DCHECK(s != NULL, "a WebAssembly store was released with no store");
    JS_FreeValue(s->ctx, s->owner);
}

void wasm_store_class_init(JSContext *ctx)
{
    JSClassDef def = { .class_name = "WebAssemblyStore", .finalizer = store_finalizer };

    DCHECK(g_class == 0, "wasm_store_class_init ran twice; the store's class is declared once per agent");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &def);
    agent_state_class("wasm_store", &g_class, "§4.2.3's store class");
}

void wasm_store_class_free(void)
{
    g_class = 0;
}
