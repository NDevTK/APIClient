/* WebAssembly Core Specification (Release 3.0) §4.2.3 "Store". See store.h. */
#include "core/wasm/store.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"
#include "core/wasm/linear_memory.h"
#include "core/wasm/reference_vector.h"
#include "solver/cow.h"

static JSClassID g_class;

/* Each entry's layout, the JSValues it owns being what the finalizer and gc_mark release and walk. Zeroed storage
   is the integer 0, which owns nothing, so an entry captured before its first write restores as no value. */
static const uint16_t TABLE_VALS[] = { offsetof(WasmTableInstance, refs) };
static const uint16_t ELEM_VALS[] = { offsetof(WasmElemInstance, refs) };
static const uint16_t MEM_VALS[] = { 0 };
static const CowRecord COUNT_REC = { sizeof(size_t), NULL, 0 };
static const CowRecord FUNC_REC = { sizeof(WasmFuncInstance), NULL, 0 };
static const CowRecord GLOBAL_REC = { sizeof(WasmGlobalInstance), NULL, 0 };
static const CowRecord TABLE_REC = { sizeof(WasmTableInstance), TABLE_VALS, 1 };
static const CowRecord MEM_REC = { sizeof(JSValue), MEM_VALS, 1 };
static const CowRecord ELEM_REC = { sizeof(WasmElemInstance), ELEM_VALS, 1 };
static const CowRecord DATA_REC = { sizeof(WasmDataInstance), NULL, 0 };
static const CowRecord POINTER_REC = { sizeof(void *), NULL, 0 };

/* Entry `i` of `l`, of `size` bytes, which must exist. */
static void *list_at(const WasmList *l, size_t i, size_t size, const char *what)
{
    DCHECKF(i < l->n, "a WebAssembly store has no %s at address %zu (it has %zu)", what, i, l->n);
    return l->chunks[i / WASM_LIST_CHUNK] + (i % WASM_LIST_CHUNK) * size;
}

/* A new zeroed entry of layout `rec` at the end of `l`, in a new chunk when the last is full. Chunks are shared by
   every flow and only grow; the count and the entry are the running flow's, captured before they are written. */
static void *list_append(WasmStore *s, WasmList *l, const CowRecord *rec)
{
    size_t size = rec->size;
    void *e;

    DCHECK(l->n <= l->n_chunks * WASM_LIST_CHUNK, "a WebAssembly store list counts more entries than its chunks");
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
    e = l->chunks[l->n / WASM_LIST_CHUNK] + (l->n % WASM_LIST_CHUNK) * size;
    cow_capture_host_record(s->owner, &l->n, &COUNT_REC);
    cow_capture_host_record(s->owner, e, rec);
    l->n++;
    return e;
}

static void list_free(WasmList *l)
{
    size_t i;

    for (i = 0; i < l->n_chunks; i++) free(l->chunks[i]);
    free(l->chunks);
    memset(l, 0, sizeof *l);
}

const WasmFuncInstance *wasm_store_func(const WasmStore *s, size_t a)
{
    return list_at(&s->funcs, a, sizeof(WasmFuncInstance), "function");
}

const WasmGlobalInstance *wasm_store_global(const WasmStore *s, size_t a)
{
    return list_at(&s->globals, a, sizeof(WasmGlobalInstance), "global");
}

const WasmTableInstance *wasm_store_table(const WasmStore *s, size_t a)
{
    return list_at(&s->tables, a, sizeof(WasmTableInstance), "table");
}

JSValueConst wasm_store_mem(const WasmStore *s, size_t a)
{
    return *(JSValue *)list_at(&s->mems, a, sizeof(JSValue), "memory");
}

const WasmElemInstance *wasm_store_elem(const WasmStore *s, size_t a)
{
    return list_at(&s->elems, a, sizeof(WasmElemInstance), "element segment");
}

const WasmDataInstance *wasm_store_data(const WasmStore *s, size_t a)
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

void wasm_global_set(WasmStore *s, size_t a, WasmValue v)
{
    WasmGlobalInstance *g = list_at(&s->globals, a, sizeof *g, "global");

    cow_capture_host_record(s->owner, g, &GLOBAL_REC);
    g->value = v;
}

void wasm_data_drop(WasmStore *s, size_t a)
{
    WasmDataInstance *d = list_at(&s->datas, a, sizeof *d, "data segment");

    cow_capture_host_record(s->owner, d, &DATA_REC);
    d->n = 0;
}

size_t wasm_store_add_func(WasmStore *s, WasmFuncInstance f)
{
    *(WasmFuncInstance *)list_append(s, &s->funcs, &FUNC_REC) = f;
    return s->funcs.n - 1;
}

size_t wasm_store_add_global(WasmStore *s, WasmGlobalInstance g)
{
    *(WasmGlobalInstance *)list_append(s, &s->globals, &GLOBAL_REC) = g;
    return s->globals.n - 1;
}

/* §4.7.1's alloctable: `min` null references (calloc's zeros); an initializer's other value is instance.c's to
   fill. The count is the page's input, so storage a size_t cannot count answers SIZE_MAX (JS API §5's
   RangeError), never a CHECK. */
size_t wasm_store_add_table(WasmStore *s, const WasmLimits *type, WasmStoreValType elem)
{
    JSValue refs = wasm_refs_new(s->ctx, type->min);
    WasmTableInstance *t;

    if (JS_IsNull(refs)) return SIZE_MAX;
    t = list_append(s, &s->tables, &TABLE_REC);
    t->type = *type;
    t->elem = elem;
    cow_record_set(s->ctx, t, &TABLE_REC, &t->refs, refs);
    return s->tables.n - 1;
}

/* §4.7.1's allocmem; storage the page's type asks for that cannot be had answers SIZE_MAX. */
size_t wasm_store_add_mem(WasmStore *s, const WasmLimits *type)
{
    JSValue mem = wasm_memory_new(s->ctx, type);
    JSValue *slot;

    if (JS_IsNull(mem)) return SIZE_MAX;
    slot = list_append(s, &s->mems, &MEM_REC);
    cow_record_set(s->ctx, slot, &MEM_REC, slot, mem);
    return s->mems.n - 1;
}

size_t wasm_store_add_elem(WasmStore *s, uint64_t n)
{
    JSValue refs = wasm_refs_new(s->ctx, n);
    WasmElemInstance *e;

    if (JS_IsNull(refs)) return SIZE_MAX;
    e = list_append(s, &s->elems, &ELEM_REC);
    cow_record_set(s->ctx, e, &ELEM_REC, &e->refs, refs);
    return s->elems.n - 1;
}

size_t wasm_store_add_data(WasmStore *s, WasmDataInstance d)
{
    DCHECK(d.inst < s->instances.n, "a WebAssembly data instance names no module instance");
    *(WasmDataInstance *)list_append(s, &s->datas, &DATA_REC) = d;
    return s->datas.n - 1;
}

size_t wasm_store_add_instance(WasmStore *s, WasmInstance *inst)
{
    *(WasmInstance **)list_append(s, &s->instances, &POINTER_REC) = inst;
    return s->instances.n - 1;
}

size_t wasm_store_add_types(WasmStore *s, const WasmTypes *types)
{
    DCHECK(types != NULL && types->registry != NULL, "a WebAssembly type space was added with no registry");
    DCHECK(s->types.n == 0 || wasm_store_types(s, 0)->registry == types->registry,
           "a WebAssembly store's type spaces use two type registries");
    *(const WasmTypes **)list_append(s, &s->types, &POINTER_REC) = types;
    return s->types.n - 1;
}

/* Release the objects the entries own, as the owner's finalizer; the module instances are their owners'. */
static void store_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    const WasmStore *s = JS_GetAnyOpaque(val, &id);
    size_t i;

    if (s == NULL) return;
    for (i = 0; i < s->tables.n; i++) JS_MarkValue(rt, wasm_store_table(s, i)->refs, mark_func);
    for (i = 0; i < s->elems.n; i++) JS_MarkValue(rt, wasm_store_elem(s, i)->refs, mark_func);
    for (i = 0; i < s->mems.n; i++) JS_MarkValue(rt, wasm_store_mem(s, i), mark_func);
}

static void store_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    WasmStore *s = JS_GetAnyOpaque(val, &id);
    size_t i;

    if (s == NULL) return;
    for (i = 0; i < s->tables.n; i++) JS_FreeValueRT(rt, wasm_store_table(s, i)->refs);
    for (i = 0; i < s->elems.n; i++) JS_FreeValueRT(rt, wasm_store_elem(s, i)->refs);
    for (i = 0; i < s->mems.n; i++) JS_FreeValueRT(rt, wasm_store_mem(s, i));
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

WasmStore *wasm_store_of(JSValueConst owner)
{
    return JS_GetOpaque(owner, g_class);
}

void wasm_store_release(WasmStore *s)
{
    DCHECK(s != NULL, "a WebAssembly store was released with no store");
    JS_FreeValue(s->ctx, s->owner);
}

void wasm_store_class_init(JSContext *ctx)
{
    JSClassDef def = { .class_name = "WebAssemblyStore", .finalizer = store_finalizer, .gc_mark = store_gc_mark };

    DCHECK(g_class == 0, "wasm_store_class_init ran twice; the store's class is declared once per agent");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &def);
    agent_state_class("wasm_store", &g_class, "§4.2.3's store class");
}

void wasm_store_class_free(void)
{
    g_class = 0;
}
