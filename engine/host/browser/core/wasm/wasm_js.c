/* The WebAssembly JavaScript Interface's machines. See wasm_js.h. */
#include "core/wasm/wasm_js.h"

#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/wasm/instance.h"
#include "core/wasm/module_record.h"
#include "core/wasm/store.h"
#include "solver/concolic.h"

/* Where `new Module(bytes)` then `new Instance(module)` rests: at each step of the copy, the decode, the
   validation and the instantiation, and at the environment's choice inside the start function. */
#define INST_STAGES(X) \
    X(INST_COPY,        "WebAssembly JS API §5.1 Modules: the Module(bytes) constructor step 1 (let stableBytes " \
                        "be a copy of the bytes held by the buffer bytes)") \
    X(INST_DECODE,      "WebAssembly JS API §5 compile a WebAssembly module step 1 (let module be " \
                        "module_decode(bytes))") \
    X(INST_VALIDATE,    "WebAssembly JS API §5 compile a WebAssembly module step 2 (module_validate(module))") \
    X(INST_IMPORTS,     "WebAssembly JS API §5 read the imports step 1 (a module with imports and no import " \
                        "object)") \
    X(INST_INSTANTIATE, "WebAssembly JS API §5 instantiate the core of a WebAssembly module step 2 " \
                        "(module_instantiate(store, module, imports))") \
    X(INST_CHOOSE,      "WebAssembly Core §4.7.1 Allocation: growtable and growmem (whether the grow succeeds, " \
                        "which the environment decides)")
enum { INST_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const INST_STEPS[] = { INST_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;
    bool begun;                  /* the JSValues below hold values (a step state arrives zeroed) */
    JSValue record;              /* owned: the module record, compiled from INST_IMPORTS */
    JSValue over;                /* owned: INST_CHOOSE's outcome, an environment unknown */
    JSValue result;              /* owned: the instance's store address */
    WasmInstantiation run;       /* from INST_INSTANTIATE */
    bool running;                /* `run` has begun */
    char src[64];                /* INST_CHOOSE's outcome's identity */
} InstantiateState;

static int g_instantiate_stepid = -1;

static void instantiate_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    InstantiateState *s = st;

    if (!s->begun) return;
    v->val(ctx, &s->record);
    v->val(ctx, &s->over);
    v->val(ctx, &s->result);
    if (s->running) wasm_instantiation_visit(ctx, &s->run, v);
}

static JSValue instantiate_fini(JSContext *ctx, void *st, bool take_result)
{
    InstantiateState *s = st;
    JSValue r = JS_UNDEFINED;

    (void)ctx;
    if (take_result && s->begun) {
        r = s->result;
        s->result = JS_UNDEFINED;
    }
    return r;
}

/* The core's answer `st` other than DONE, thrown as JS API §5 "instantiate the core of a WebAssembly module"
   step 3 and §5.6 step 10 map it (§5.10's error objects): a link failure a LinkError, a trap a RuntimeError,
   storage the page's sizes ask for and cannot be had a RangeError (the out-of-memory case the step names). */
static int instantiate_throw(JSContext *ctx, WasmExecStatus st, const char *message)
{
    DCHECK(st != WASM_EXEC_DONE && st != WASM_EXEC_MORE && st != WASM_EXEC_FORK && st != WASM_EXEC_HOST_CALL,
           "a WebAssembly answer that is no failure was thrown");
    if (st == WASM_EXEC_LINK_INCOMPATIBLE) JS_ThrowWasmLinkError(ctx, "%s", message);
    else if (st == WASM_EXEC_NO_ROOM) JS_ThrowRangeError(ctx, "WebAssembly: %s", message);
    else JS_ThrowWasmRuntimeError(ctx, "%s", message);
    return JS_STEP_ABRUPT;
}

/* The status `st` of a step of the instantiation: rest, choose, or complete. */
static int instantiate_after(JSContext *ctx, InstantiateState *s, WasmExecStatus st)
{
    const WasmChoice *c = &s->run.start.choice;
    int w;

    if (st == WASM_EXEC_MORE) return JS_STEP_YIELD;
    if (st == WASM_EXEC_FORK) {
        /* The grow's success is the example, the -1 its assumption (decide.c prices the arm against it). */
        DCHECK(c->kind == WASM_CHOICE_TABLE_GROW || c->kind == WASM_CHOICE_MEMORY_GROW, "a WebAssembly start "
               "function forked on a choice that is neither grow");
        w = snprintf(s->src, sizeof s->src, "WebAssembly %s@%llu",
                     c->kind == WASM_CHOICE_TABLE_GROW ? "table.grow" : "memory.grow",
                     (unsigned long long)JS_FlowPeekRetiredWork());
        DCHECK(w > 0 && (size_t)w < sizeof s->src, "a WebAssembly grow's identity did not fit its buffer");
        (void)w;
        s->over = concolic_is_exploring()
                      ? concolic_new(ctx, c->kind == WASM_CHOICE_TABLE_GROW ? "{WebAssembly table.grow}"
                                                                            : "{WebAssembly memory.grow}",
                                     s->src, CONCOLIC_WHOSE_WORLD, CONCOLIC_TYF_BOOLEAN, JS_TRUE)
                      : JS_TRUE;
        CHECK(!JS_IsException(s->over), "a WebAssembly grow's outcome could not be allocated");
        s->hdr.stage = INST_CHOOSE;
        return JS_STEP_YIELD;
    }
    if (st == WASM_EXEC_HOST_CALL) {
        DFAIL("a WebAssembly start function called a host function; build JS API §5's run a host function as a "
              "step_call_run of the import");
        JS_ThrowTypeError(ctx, "WebAssembly: a host function call is not built");
        return JS_STEP_ABRUPT;
    }
    if (st != WASM_EXEC_DONE) {
        char m[160];

        wasm_interpreter_message(&s->run.start, st, m, sizeof m);
        return instantiate_throw(ctx, st, st == WASM_EXEC_LINK_INCOMPATIBLE ? "incompatible import type"
                                          : st == WASM_EXEC_NO_ROOM ? "out of memory" : m);
    }
    s->result = JS_NewInt64(ctx, (int64_t)s->run.addr);
    return JS_STEP_DONE;
}

static int instantiate_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    InstantiateState *s = st;
    WasmStore *store = wasm_store_of(JS_StepClosureData(&s->hdr, 0));
    WasmModuleRecord *r;
    WasmCompile c;
    char m[160];

    (void)out_cb; (void)out_argc;
    DCHECK(store != NULL, "the WebAssembly instantiation's closure holds no store");
    if (!s->begun) {
        s->begun = true;
        s->record = s->over = s->result = JS_UNDEFINED;
    }
    switch (s->hdr.stage) {
    case INST_COPY: {
        size_t n = 0;
        const uint8_t *b;

        JS_FreeValue(ctx, cb_result);
        b = JS_GetUint8Array(ctx, &n, step_arg(&s->hdr, 0));
        if (b == NULL && JS_HasException(ctx)) return JS_STEP_ABRUPT;
        if (JS_IsUndefined(s->record)) s->record = wasm_module_record_new(ctx, n, store->registry, s);
        r = wasm_module_record_of(s->record);
        if (!wasm_module_record_copied(r)) {
            wasm_module_record_step(r, s, b, n);
            return JS_STEP_YIELD;
        }
        s->hdr.stage = INST_DECODE;
        return JS_STEP_YIELD;
    }
    case INST_DECODE:
    case INST_VALIDATE:
        JS_FreeValue(ctx, cb_result);
        r = wasm_module_record_of(s->record);
        c = wasm_module_record_step(r, s, NULL, r->n);
        if (c == WASM_COMPILE_MORE) {
            s->hdr.stage = r->decoded == WASM_DECODE_DONE ? INST_VALIDATE : INST_DECODE;
            return JS_STEP_YIELD;
        }
        if (c != WASM_COMPILE_DONE) {
            /* §5.1 Module(bytes) step 3: a module that does not compile is a CompileError. */
            wasm_module_record_message(r, m, sizeof m);
            JS_ThrowWasmCompileError(ctx, "%s", m);
            return JS_STEP_ABRUPT;
        }
        s->hdr.stage = INST_IMPORTS;
        return JS_STEP_YIELD;
    case INST_IMPORTS:
        JS_FreeValue(ctx, cb_result);
        r = wasm_module_record_of(s->record);
        if (r->d.module.n_imports != 0) {
            JS_ThrowTypeError(ctx, "WebAssembly instantiation: the module has imports and there is no import "
                                   "object");
            return JS_STEP_ABRUPT;
        }
        if (wasm_instantiation_begin(&s->run, store, &r->d.module, r->bytes, wasm_module_validator_side_table(&r->v),
                                     &r->v.ctx.types, NULL, 0, s->record) != WASM_EXEC_MORE)
            return instantiate_throw(ctx, WASM_EXEC_NO_ROOM, "out of memory");
        s->running = true;
        s->hdr.stage = INST_INSTANTIATE;
        return JS_STEP_YIELD;
    case INST_INSTANTIATE:
        JS_FreeValue(ctx, cb_result);
        return instantiate_after(ctx, s, wasm_instantiation_step(&s->run));
    case INST_CHOOSE: {
        int arm, rc;

        JS_FreeValue(ctx, cb_result);
        if (concolic_is(s->over)) {
            rc = step_fork_run(ctx, &s->hdr, s->over, INST_STEPS[INST_CHOOSE], 2, 0, &arm);
            if (rc) return rc;
        } else {
            /* A pin or a host that does not explore answers the outcome itself. */
            arm = JS_ToBool(ctx, s->over) ? 0 : 1;
        }
        DCHECK(arm == 0 || arm == 1, "a WebAssembly grow's two-armed fork answered a third arm");
        JS_FreeValue(ctx, s->over);
        s->over = JS_UNDEFINED;
        s->hdr.stage = INST_INSTANTIATE;
        return instantiate_after(ctx, s, wasm_instantiation_choose(&s->run, arm == 0));
    }
    default: break;
    }
    CHECK_FAIL("the WebAssembly instantiation resumed into a stage it does not have");
}

static const JSTrampStepDef instantiate_def = {
    sizeof(InstantiateState), instantiate_step, instantiate_fini, 0,
    .visit = instantiate_visit,
    .algorithm = "WebAssembly JS API §5.1 new Module(bytes), then §5.2 new Instance(module)",
    .steps = INST_STEPS
};

void wasm_js_init(JSContext *ctx)
{
    DCHECK(g_instantiate_stepid < 0, "wasm_js_init ran twice; the machines are registered once per agent");
    g_instantiate_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &instantiate_def);
    agent_state_id("wasm_js", &g_instantiate_stepid, "the WebAssembly instantiation's machine");
}

void wasm_js_free(void)
{
    g_instantiate_stepid = -1;
}

JSValue wasm_js_instantiate_closure(JSContext *ctx, JSValueConst store_owner)
{
    JSValueConst data[1] = { store_owner };

    DCHECK(g_instantiate_stepid >= 0, "a WebAssembly instantiation was minted before wasm_js_init");
    DCHECK(wasm_store_of(store_owner) != NULL, "a WebAssembly instantiation was minted over no store");
    return JS_NewStepClosure2(ctx, g_instantiate_stepid, "instantiate", 1, 1, data);
}
