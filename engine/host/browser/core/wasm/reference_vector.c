/* WebAssembly Core Specification (Release 3.0) §4.2.8 and §4.2.12's vector of references, per flow. See
 * reference_vector.h. */
#include "core/wasm/reference_vector.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"
#include "solver/cow.h"

/* The per-flow part: the length. */
typedef struct {
    uint64_t n;
} WasmRefsLength;

/* `chunks[i]` holds elements [i * WASM_REFS_CHUNK, (i + 1) * WASM_REFS_CHUNK); `n_chunks` is how many exist for
   any flow, which only the longest vector so far decides. */
typedef struct {
    WasmRefsLength length;
    WasmValue **chunks;
    size_t n_chunks, cap_chunks;
} WasmRefs;

/* Neither record owns a JSValue. */
static const CowRecord LENGTH_REC = { sizeof(WasmRefsLength), NULL, 0 };
static const CowRecord CHUNK_REC = { WASM_REFS_CHUNK * sizeof(WasmValue), NULL, 0 };

static JSClassID g_class;

static WasmRefs *refs_of(JSValueConst v)
{
    WasmRefs *r = JS_GetOpaque(v, g_class);

    DCHECK(r != NULL, "a WebAssembly reference vector operation was handed something that is not one");
    return r;
}

static void refs_release(WasmRefs *r)
{
    size_t i;

    for (i = 0; i < r->n_chunks; i++) free(r->chunks[i]);
    free(r->chunks);
    free(r);
}

static void refs_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    WasmRefs *r = JS_GetAnyOpaque(val, &id);

    (void)rt;
    if (r != NULL) refs_release(r);
}

/* Zeroed chunks, shared by every flow, for the first `n` elements; false where they cannot be allocated. */
static bool reserve(WasmRefs *r, uint64_t n)
{
    uint64_t want = n / WASM_REFS_CHUNK + (n % WASM_REFS_CHUNK != 0);

    if (want <= r->n_chunks) return true;
    if (want > SIZE_MAX / sizeof *r->chunks) return false;
    if (want > r->cap_chunks) {
        size_t cap = r->cap_chunks <= SIZE_MAX / sizeof *r->chunks / 2 && r->cap_chunks * 2 > want
                     ? r->cap_chunks * 2 : (size_t)want;
        WasmValue **grown = realloc(r->chunks, cap * sizeof *grown);

        if (grown == NULL) return false;
        r->chunks = grown;
        r->cap_chunks = cap;
    }
    while (r->n_chunks < want) {
        r->chunks[r->n_chunks] = calloc(WASM_REFS_CHUNK, sizeof(WasmValue));
        if (r->chunks[r->n_chunks] == NULL) return false;
        r->n_chunks++;
    }
    return true;
}

JSValue wasm_refs_new(JSContext *ctx, uint64_t n)
{
    WasmRefs *r;
    JSValue obj;

    DCHECK(g_class != 0, "a WebAssembly reference vector was made before wasm_refs_init declared its class");
    r = calloc(1, sizeof *r);
    CHECK(r != NULL, "a WebAssembly reference vector could not be allocated: out of memory");
    if (n > SIZE_MAX / sizeof(WasmValue) || !reserve(r, n)) {
        refs_release(r);
        return JS_NULL;
    }
    r->length.n = n;
    obj = JS_NewObjectProtoClass(ctx, JS_NULL, g_class);
    CHECK(!JS_IsException(obj), "a WebAssembly reference vector's object could not be allocated: out of memory");
    JS_SetOpaque(obj, r);
    return obj;
}

bool wasm_refs_is(JSValueConst v)
{
    return JS_GetOpaque(v, g_class) != NULL;
}

uint64_t wasm_refs_length(JSValueConst v)
{
    return refs_of(v)->length.n;
}

static WasmValue *element(const WasmRefs *r, uint64_t i)
{
    DCHECKF(i < r->length.n, "a WebAssembly reference vector access at %llu is past its length %llu",
            (unsigned long long)i, (unsigned long long)r->length.n);
    DCHECK(i / WASM_REFS_CHUNK < r->n_chunks, "a WebAssembly reference vector's length runs past its chunks");
    return &r->chunks[i / WASM_REFS_CHUNK][i % WASM_REFS_CHUNK];
}

WasmValue wasm_refs_get(JSValueConst v, uint64_t i)
{
    return *element(refs_of(v), i);
}

void wasm_refs_set(JSValueConst v, uint64_t i, WasmValue x)
{
    WasmRefs *r = refs_of(v);
    WasmValue *e = element(r, i);

    cow_capture_host_record(v, r->chunks[i / WASM_REFS_CHUNK], &CHUNK_REC);
    *e = x;
}

bool wasm_refs_growable(JSValueConst v, uint64_t n)
{
    uint64_t len = refs_of(v)->length.n;

    return n <= SIZE_MAX / sizeof(WasmValue) - len;
}

void wasm_refs_grow(JSValueConst v, uint64_t n)
{
    WasmRefs *r = refs_of(v);

    DCHECK(wasm_refs_growable(v, n), "a WebAssembly reference vector grew past what can be counted");
    CHECK(reserve(r, r->length.n + n), "a WebAssembly reference vector could not grow: out of memory");
    cow_capture_host_record(v, &r->length, &LENGTH_REC);
    r->length.n += n;
}

void wasm_refs_drop(JSValueConst v)
{
    WasmRefs *r = refs_of(v);

    cow_capture_host_record(v, &r->length, &LENGTH_REC);
    r->length.n = 0;
}

void wasm_refs_init(JSContext *ctx)
{
    JSClassDef def = { .class_name = "WebAssemblyReferenceVector", .finalizer = refs_finalizer };

    DCHECK(g_class == 0, "wasm_refs_init ran twice; the reference vector's class is declared once per agent");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &def);
    agent_state_class("wasm_refs", &g_class, "§4.2.8's and §4.2.12's reference vector class");
}

void wasm_refs_free(void)
{
    g_class = 0;
}
