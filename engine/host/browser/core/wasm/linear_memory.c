/* WebAssembly Core Specification (Release 3.0) §4.2.9 "Memory Instances", per flow. See linear_memory.h. */
#include "core/wasm/linear_memory.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"
#include "solver/cow.h"

/* The per-flow part of the instance: §4.2.9's byte length, in pages. */
typedef struct {
    uint64_t pages;
} WasmMemorySize;

/* The instance. `chunks[i]` is chunk i's storage, carved from `regions`, each one growmem's allocation;
   `n_chunks` is how many exist for any flow, which only the largest grow so far decides. */
typedef struct {
    WasmMemorySize size;
    bool addr64, has_max;
    uint64_t max;
    uint8_t **chunks;
    size_t n_chunks, cap_chunks;
    uint8_t **regions;
    size_t n_regions, cap_regions;
} WasmLinearMemory;

/* Neither record owns a JSValue. */
static const CowRecord SIZE_REC = { sizeof(WasmMemorySize), NULL, 0 };
static const CowRecord CHUNK_REC = { WASM_MEMORY_CHUNK, NULL, 0 };

#define CHUNKS_PER_PAGE (WASM_PAGE_SIZE / WASM_MEMORY_CHUNK)
_Static_assert(WASM_PAGE_SIZE % WASM_MEMORY_CHUNK == 0, "a page is a whole number of chunks");

static JSClassID g_class;

static WasmLinearMemory *memory_of(JSValueConst v)
{
    WasmLinearMemory *m = JS_GetOpaque(v, g_class);

    DCHECK(m != NULL, "a WebAssembly memory operation was handed something that is not a memory instance");
    return m;
}

static void memory_release(WasmLinearMemory *m)
{
    size_t i;

    for (i = 0; i < m->n_regions; i++) free(m->regions[i]);
    free(m->regions);
    free(m->chunks);
    free(m);
}

static void memory_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    WasmLinearMemory *m = JS_GetAnyOpaque(val, &id);

    (void)rt;
    if (m != NULL) memory_release(m);
}

/* Chunk storage for the first `pages` pages, shared by every flow: zeroed storage for the chunks not yet
   allocated, as one region. False where it cannot be allocated. */
static bool reserve(WasmLinearMemory *m, uint64_t pages)
{
    uint64_t want = pages * CHUNKS_PER_PAGE, add, i;
    uint8_t *region;

    if (want <= m->n_chunks) return true;
    add = want - m->n_chunks;
    if (want > SIZE_MAX / sizeof *m->chunks || add > SIZE_MAX / WASM_MEMORY_CHUNK) return false;
    if (want > m->cap_chunks) {
        uint8_t **grown = realloc(m->chunks, (size_t)want * sizeof *m->chunks);

        if (grown == NULL) return false;
        m->chunks = grown;
        m->cap_chunks = (size_t)want;
    }
    if (m->n_regions == m->cap_regions) {
        size_t cap = m->cap_regions ? m->cap_regions * 2 : 4;
        uint8_t **grown = realloc(m->regions, cap * sizeof *m->regions);

        if (grown == NULL) return false;
        m->regions = grown;
        m->cap_regions = cap;
    }
    region = calloc((size_t)add, WASM_MEMORY_CHUNK);
    if (region == NULL) return false;
    m->regions[m->n_regions++] = region;
    for (i = 0; i < add; i++) m->chunks[m->n_chunks + i] = region + (size_t)i * WASM_MEMORY_CHUNK;
    m->n_chunks = (size_t)want;
    return true;
}

JSValue wasm_memory_new(JSContext *ctx, const WasmLimits *type)
{
    WasmLinearMemory *m;
    JSValue obj;

    DCHECK(g_class != 0, "a WebAssembly memory was allocated before wasm_memory_init declared its class");
    DCHECK(type != NULL && (!type->has_max || type->min <= type->max),
           "a WebAssembly memory was allocated from a type validation refuses (§3.2.12's limits)");
    m = calloc(1, sizeof *m);
    if (m == NULL || !reserve(m, type->min)) {
        if (m != NULL) memory_release(m);
        return JS_ThrowRangeError(ctx, "WebAssembly.Memory(): could not allocate memory");
    }
    m->size.pages = type->min;
    m->addr64 = type->addr64;
    m->has_max = type->has_max;
    m->max = type->max;
    obj = JS_NewObjectProtoClass(ctx, JS_NULL, g_class);
    if (JS_IsException(obj)) {
        memory_release(m);
        return obj;
    }
    JS_SetOpaque(obj, m);
    return obj;
}

bool wasm_memory_is(JSValueConst v)
{
    return JS_GetOpaque(v, g_class) != NULL;
}

uint64_t wasm_memory_pages(JSValueConst mem)
{
    return memory_of(mem)->size.pages;
}

/* Whether [addr, addr + n) lies inside the running flow's size, compared in pages: a byte length of 2^48 pages
   does not fit in 64 bits. An end past 2^64 is outside every memory reserve could have allocated. */
static bool in_bounds(const WasmLinearMemory *m, uint64_t addr, size_t n)
{
    uint64_t end;

    if (n > UINT64_MAX - addr) return false;
    end = addr + n;
    return end / WASM_PAGE_SIZE + (end % WASM_PAGE_SIZE != 0) <= m->size.pages;
}

bool wasm_memory_load(JSValueConst mem, uint64_t addr, size_t n, uint8_t *out)
{
    const WasmLinearMemory *m = memory_of(mem);
    size_t done = 0;

    if (!in_bounds(m, addr, n)) return false;
    while (done < n) {
        uint64_t at = addr + done;
        size_t off = (size_t)(at % WASM_MEMORY_CHUNK), k = WASM_MEMORY_CHUNK - off;

        if (k > n - done) k = n - done;
        DCHECK(at / WASM_MEMORY_CHUNK < m->n_chunks, "a WebAssembly memory's size runs past its chunk storage");
        memcpy(out + done, m->chunks[at / WASM_MEMORY_CHUNK] + off, k);
        done += k;
    }
    return true;
}

bool wasm_memory_store(JSValueConst mem, uint64_t addr, size_t n, const uint8_t *in)
{
    WasmLinearMemory *m = memory_of(mem);
    size_t done = 0;

    if (!in_bounds(m, addr, n)) return false;
    while (done < n) {
        uint64_t at = addr + done;
        size_t off = (size_t)(at % WASM_MEMORY_CHUNK), k = WASM_MEMORY_CHUNK - off;
        uint8_t *chunk;

        if (k > n - done) k = n - done;
        DCHECK(at / WASM_MEMORY_CHUNK < m->n_chunks, "a WebAssembly memory's size runs past its chunk storage");
        chunk = m->chunks[at / WASM_MEMORY_CHUNK];
        cow_capture_host_record(mem, chunk, &CHUNK_REC);
        memcpy(chunk + off, in + done, k);
        done += k;
    }
    return true;
}

int64_t wasm_memory_grow(JSValueConst mem, uint64_t delta)
{
    WasmLinearMemory *m = memory_of(mem);
    uint64_t old = m->size.pages, bound = m->addr64 ? (uint64_t)1 << 48 : (uint64_t)1 << 16;

    /* §4.7.1's growmem steps 2 to 5: i' within the maximum, and within 2^(|at|-16) pages. */
    if (delta > bound || old > bound - delta) return -1;
    if (m->has_max && old + delta > m->max) return -1;
    if (!reserve(m, old + delta)) return -1;
    cow_capture_host_record(mem, &m->size, &SIZE_REC);
    m->size.pages = old + delta;
    return (int64_t)old;
}

void wasm_memory_init(JSContext *ctx)
{
    JSClassDef def = { .class_name = "WebAssemblyMemoryInstance", .finalizer = memory_finalizer };

    DCHECK(g_class == 0, "wasm_memory_init ran twice — the memory instance's class is declared once per agent");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &def);
    agent_state_class("wasm_memory", &g_class, "§4.2.9's memory instance class");
}

void wasm_memory_free(void)
{
    g_class = 0;
}
