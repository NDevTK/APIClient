/* WebAssembly JavaScript Interface §5 "compile a WebAssembly module" from source bytes, as a step machine over a
 * record the JS API's Module object holds (§5.1's [[Module]] and [[Bytes]]): its own copy of the bytes, taken
 * WASM_COPY_CHUNK bytes a step, decoded by module_decoder.h and then validated by module_validator.h, one step a
 * production or an item, each step charged one unit of engine work. Its owner is an object of the agent's module
 * record class. The compile is a function of the source bytes alone, so the record is its memo and every holder
 * shares it: the machine that minted it and each copy a fork makes of that machine walk the same steps, each by
 * its own cursor. The step at the record's frontier is performed, an earlier one is read back, and both are
 * charged alike, so a flow's work and rest points do not depend on which holder ran first. Once compiled the
 * record is immutable, and module instances (instance.h) share it too. */
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
    uint8_t *bytes;              /* owned: the copy of the source bytes, a chunk taken by each copy step */
    size_t n;
    WasmModuleDecoder d;         /* the decode, then the module it answered */
    WasmModuleValidator v;       /* the validation, then the side table and the type identities */
    WasmTypeRegistry *registry;  /* borrowed: the agent's, which the validation interns in */
    WasmCompile state;
    WasmDecodeStatus decoded;    /* the decode's answer, MORE until it gives one */
    WasmValidateStatus validated;
    size_t steps;                /* the steps performed so far: the frontier every holder's cursor is at or below */
    size_t decode_steps;         /* the steps up to and including the one that ended the decode, once it has */
} WasmModuleRecord;

/* A record for a copy of `n` source bytes, to compile into `registry`'s identities (borrowed, outliving the
   record), as the reference to its owner. Its first holder's cursor is 0. */
JSValue wasm_module_record_new(JSContext *ctx, size_t n, WasmTypeRegistry *registry);
/* The record owned by `owner`, an object of the agent's module record class (asserted). */
WasmModuleRecord *wasm_module_record_of(JSValueConst owner);
/* Step `*at` of the compile for the holder whose cursor that is, which it advances: MORE, then DONE or a refusal
   at the last step, after which stepping again is misuse (checked in every build). `src` is the source bytes as
   the caller reads them now, read while the copy is taken and of the record's length (asserted); a holder
   reading back a copy step reads the bytes the performing holder read (asserted). */
WasmCompile wasm_module_record_step(WasmModuleRecord *r, size_t *at, const uint8_t *src, size_t n);
/* Whether the holder at cursor `at` has taken the whole copy of the source bytes. */
bool wasm_module_record_copied(const WasmModuleRecord *r, size_t at);
/* Whether the holder at cursor `at` has taken the step that ended the decode. */
bool wasm_module_record_decoded(const WasmModuleRecord *r, size_t at);
/* The reference interpreter's message for the refusal the record answered. */
void wasm_module_record_message(const WasmModuleRecord *r, char *buf, size_t cap);
/* The record class, declared once per agent (core/platform.c's `wasm_module` row), and its release. */
void wasm_module_record_class_init(JSContext *ctx);
void wasm_module_record_class_free(void);

#endif
