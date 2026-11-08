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

JSValue wasm_module_record_new(JSContext *ctx, size_t n, WasmTypeRegistry *registry, const void *compiler)
{
    WasmModuleRecord *r;
    JSValue owner;

    DCHECK(g_class != 0, "a WebAssembly module record was made before its class was declared");
    DCHECK(registry != NULL && compiler != NULL, "a WebAssembly module record was made without its registry or "
           "compiler");
    r = js_mallocz(ctx, sizeof *r);
    CHECK(r != NULL, "a WebAssembly module record could not be allocated: out of memory");
    r->bytes = js_malloc(ctx, n ? n : 1);
    CHECK(r->bytes != NULL, "a WebAssembly module's bytes could not be copied: out of memory");
    r->n = n;
    r->compiler = compiler;
    r->decoded = WASM_DECODE_MORE;
    r->validated = WASM_VALIDATE_MORE;
    wasm_module_decoder_init(&r->d, r->bytes, n);
    r->registry = registry;
    owner = JS_NewObjectProtoClass(ctx, JS_NULL, g_class);
    CHECK(!JS_IsException(owner), "a WebAssembly module record's owner could not be allocated: out of memory");
    JS_SetOpaque(owner, r);
    return owner;
}

bool wasm_module_record_copied(const WasmModuleRecord *r)
{
    return r->copied == r->n;
}

WasmCompile wasm_module_record_step(WasmModuleRecord *r, const void *compiler, const uint8_t *src, size_t n)
{
    DCHECKF(r->compiler == compiler, "a WebAssembly module record was compiled by the machine state %p, which is "
            "not its compiler %p", compiler, r->compiler);
    DCHECK(r->state == WASM_COMPILE_MORE, "a WebAssembly module record was stepped after its compile answered");
    JS_FlowRetireWork(1);
    if (r->copied < r->n) {
        size_t k = r->n - r->copied < WASM_COPY_CHUNK ? r->n - r->copied : WASM_COPY_CHUNK;

        DCHECKF(src != NULL && n == r->n, "a WebAssembly module's source bytes were %zu while their copy was "
                "taken, then %zu", r->n, n);
        memcpy(r->bytes + r->copied, src + r->copied, k);
        r->copied += k;
        return WASM_COMPILE_MORE;
    }
    if (r->decoded != WASM_DECODE_DONE) {
        /* §5's module_decode, then module_validate over its module. */
        r->decoded = wasm_module_decoder_step(&r->d);
        if (r->decoded == WASM_DECODE_MORE) return WASM_COMPILE_MORE;
        if (r->decoded != WASM_DECODE_DONE) return r->state = WASM_COMPILE_MALFORMED;
        wasm_module_validator_init(&r->v, wasm_module_decoder_module(&r->d), r->bytes, r->registry);
        return WASM_COMPILE_MORE;
    }
    r->validated = wasm_module_validator_step(&r->v);
    if (r->validated == WASM_VALIDATE_MORE) return WASM_COMPILE_MORE;
    r->compiler = NULL;
    return r->state = r->validated == WASM_VALIDATE_DONE ? WASM_COMPILE_DONE : WASM_COMPILE_INVALID;
}

void wasm_module_record_message(const WasmModuleRecord *r, char *buf, size_t cap)
{
    DCHECK(r->state == WASM_COMPILE_MALFORMED || r->state == WASM_COMPILE_INVALID, "a WebAssembly module "
           "record's message was asked of a compile that refused nothing");
    if (r->state == WASM_COMPILE_MALFORMED) wasm_module_decoder_message(&r->d, r->decoded, buf, cap);
    else wasm_module_validator_message(&r->v, r->validated, buf, cap);
}
