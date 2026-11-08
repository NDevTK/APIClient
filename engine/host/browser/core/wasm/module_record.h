/* WebAssembly JavaScript Interface §5 "compile a WebAssembly module" from source bytes, as a step machine over a
 * record the JS API's Module object holds (§5.1's [[Module]] and [[Bytes]]): its own copy of the bytes, taken
 * WASM_COPY_CHUNK bytes a step, decoded by module_decoder.h and then validated by module_validator.h, one step a
 * production or an item, each step charged one unit of engine work. Its owner is an object of the agent's module
 * record class. A compile in progress is written only by the machine that compiles it (`compiler`); once
 * compiled the record is immutable, so module instances (instance.h) and the flows that fork after the compile
 * share it. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_MODULE_RECORD_H
#define ENGINE_HOST_BROWSER_CORE_WASM_MODULE_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"
#include "core/wasm/module_decoder.h"
#include "core/wasm/module_validator.h"
#include "core/wasm/type_registry.h"

typedef enum { WASM_COMPILE_MORE = 0, WASM_COMPILE_DONE, WASM_COMPILE_MALFORMED, WASM_COMPILE_INVALID } WasmCompile;

/* The bytes one step copies: a placeholder for the scheduler-owned granularity, as linear_memory.h's chunk
   is. */
#define WASM_COPY_CHUNK 4096u

typedef struct {
    uint8_t *bytes;              /* owned: the copy of the source bytes, [0, copied) of them taken so far */
    size_t n, copied;
    WasmModuleDecoder d;         /* the decode, then the module it answered */
    WasmModuleValidator v;       /* the validation, then the side table and the type identities */
    WasmTypeRegistry *registry;  /* borrowed: the agent's, which the validation interns in */
    WasmCompile state;
    WasmDecodeStatus decoded;    /* the decode's answer, MORE until it gives one */
    WasmValidateStatus validated;
    const void *compiler;        /* borrowed: the state of the machine compiling it */
} WasmModuleRecord;

/* A record for a copy of `n` source bytes, to compile into `registry`'s identities (borrowed, outliving the
   record) by the machine whose state is `compiler`, as the reference to its owner. */
JSValue wasm_module_record_new(JSContext *ctx, size_t n, WasmTypeRegistry *registry, const void *compiler);
/* The record owned by `owner`, an object of the agent's module record class (asserted). */
WasmModuleRecord *wasm_module_record_of(JSValueConst owner);
/* One step of the compile by the machine whose state is `compiler` (asserted to be the record's): MORE, then
   DONE or a refusal, after which stepping again is misuse (asserted). `src` is the source bytes as the caller
   reads them now, read while the copy is taken and of the record's length (asserted). */
WasmCompile wasm_module_record_step(WasmModuleRecord *r, const void *compiler, const uint8_t *src, size_t n);
/* Whether the copy of the source bytes is complete. */
bool wasm_module_record_copied(const WasmModuleRecord *r);
/* The reference interpreter's message for the refusal the record answered. */
void wasm_module_record_message(const WasmModuleRecord *r, char *buf, size_t cap);
/* The record class, declared once per agent (core/platform.c's `wasm_module` row), and its release. */
void wasm_module_record_class_init(JSContext *ctx);
void wasm_module_record_class_free(void);

#endif
