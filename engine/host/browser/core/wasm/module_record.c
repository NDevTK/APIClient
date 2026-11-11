/* WebAssembly JavaScript Interface §5 "compile a WebAssembly module". See module_record.h. */
#include "core/wasm/module_record.h"

#include <string.h>

#include "check.h"
#include "core/agent_state.h"

static JSClassID g_class;

static void record_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    WasmModuleRecord *r = JS_GetAnyOpaque(val, &id);

    if (r == NULL) return;
    wasm_module_validator_free(&r->v);
    wasm_module_decoder_free(&r->d);
    js_free_rt(rt, r->bytes);
    js_free_rt(rt, r);
}

void wasm_module_record_class_init(JSContext *ctx)
{
    JSClassDef def = { .class_name = "WebAssemblyModuleRecord", .finalizer = record_finalizer };

    DCHECK(g_class == 0, "wasm_module_record_class_init ran twice; the class is declared once per agent");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &def);
    agent_state_class("wasm_module", &g_class, "JS API §5.1's [[Module]] and [[Bytes]] record class");
}

void wasm_module_record_class_free(void)
{
    g_class = 0;
}

WasmModuleRecord *wasm_module_record_of(JSValueConst owner)
{
    WasmModuleRecord *r = JS_GetOpaque(owner, g_class);

    DCHECK(r != NULL, "a WebAssembly module record was read from an object that owns none");
    return r;
}

JSValue wasm_module_record_new(JSContext *ctx, size_t n, WasmTypeRegistry *registry)
{
    WasmModuleRecord *r;
    JSValue owner;

    DCHECK(g_class != 0, "a WebAssembly module record was made before its class was declared");
    DCHECK(registry != NULL, "a WebAssembly module record was made without its registry");
    r = js_mallocz(ctx, sizeof *r);
    CHECK(r != NULL, "a WebAssembly module record could not be allocated: out of memory");
    r->bytes = js_malloc(ctx, n ? n : 1);
    CHECK(r->bytes != NULL, "a WebAssembly module's bytes could not be copied: out of memory");
    r->n = n;
    r->decoded = WASM_DECODE_MORE;
    r->validated = WASM_VALIDATE_MORE;
    wasm_module_decoder_init(&r->d, r->bytes, n);
    r->registry = registry;
    owner = JS_NewObjectProtoClass(ctx, JS_NULL, g_class);
    CHECK(!JS_IsException(owner), "a WebAssembly module record's owner could not be allocated: out of memory");
    JS_SetOpaque(owner, r);
    return owner;
}

/* The steps the copy takes: one a chunk. */
static size_t copy_steps(const WasmModuleRecord *r)
{
    return r->n / WASM_COPY_CHUNK + (r->n % WASM_COPY_CHUNK != 0);
}

bool wasm_module_record_copied(const WasmModuleRecord *r, size_t at)
{
    DCHECKF(at <= r->steps, "a WebAssembly module record's holder stands at step %zu of a compile that has taken "
            "%zu", at, r->steps);
    return at >= copy_steps(r);
}

bool wasm_module_record_decoded(const WasmModuleRecord *r, size_t at)
{
    DCHECKF(at <= r->steps, "a WebAssembly module record's holder stands at step %zu of a compile that has taken "
            "%zu", at, r->steps);
    return r->decoded == WASM_DECODE_DONE && at >= r->decode_steps;
}

WasmCompile wasm_module_record_step(WasmModuleRecord *r, size_t *at, const uint8_t *src, size_t n)
{
    size_t i = *at;

    /* A holder ahead of the frontier, or at the frontier of an answered compile, would step the decoder or the
       validator past its answer, which neither survives in a release build. */
    CHECK(i < r->steps || (i == r->steps && r->state == WASM_COMPILE_MORE),
          "a WebAssembly module record was stepped past its frontier or after its compile answered");
    JS_FlowRetireWork(1);
    *at = i + 1;
    if (i < copy_steps(r)) {
        size_t from = i * WASM_COPY_CHUNK, k = r->n - from < WASM_COPY_CHUNK ? r->n - from : WASM_COPY_CHUNK;

        DCHECKF(src != NULL && n == r->n, "a WebAssembly module's source bytes were %zu while their copy was "
                "taken, then %zu", r->n, n);
        if (i < r->steps) {
            DCHECKF(memcmp(r->bytes + from, src + from, k) == 0, "two holders of one WebAssembly module record "
                    "read different source bytes at %zu; a fork's copy of the machine reads what the machine "
                    "read", from);
            return WASM_COMPILE_MORE;
        }
        memcpy(r->bytes + from, src + from, k);
        r->steps++;
        return WASM_COMPILE_MORE;
    }
    /* A step another holder performed answers what it answered there: the compile's answer at the last one. */
    if (i < r->steps) return i + 1 == r->steps ? r->state : WASM_COMPILE_MORE;
    r->steps++;
    if (r->decoded != WASM_DECODE_DONE) {
        /* §5's module_decode, then module_validate over its module. */
        r->decoded = wasm_module_decoder_step(&r->d);
        if (r->decoded == WASM_DECODE_MORE) return WASM_COMPILE_MORE;
        if (r->decoded != WASM_DECODE_DONE) return r->state = WASM_COMPILE_MALFORMED;
        r->decode_steps = r->steps;
        wasm_module_validator_init(&r->v, wasm_module_decoder_module(&r->d), r->bytes, r->registry);
        return WASM_COMPILE_MORE;
    }
    r->validated = wasm_module_validator_step(&r->v);
    if (r->validated == WASM_VALIDATE_MORE) return WASM_COMPILE_MORE;
    return r->state = r->validated == WASM_VALIDATE_DONE ? WASM_COMPILE_DONE : WASM_COMPILE_INVALID;
}

void wasm_module_record_message(const WasmModuleRecord *r, char *buf, size_t cap)
{
    DCHECK(r->state == WASM_COMPILE_MALFORMED || r->state == WASM_COMPILE_INVALID, "a WebAssembly module "
           "record's message was asked of a compile that refused nothing");
    if (r->state == WASM_COMPILE_MALFORMED) wasm_module_decoder_message(&r->d, r->decoded, buf, cap);
    else wasm_module_validator_message(&r->v, r->validated, buf, cap);
}
