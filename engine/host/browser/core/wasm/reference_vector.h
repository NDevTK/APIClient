/* WebAssembly Core Specification (Release 3.0) §4.2.8 "Table Instances" and §4.2.12 "Element Instances": the
 * `elem` vector of references both hold, per flow. A vector is an object of an internal class (core/platform.c's
 * `wasm_refs` row) that the store's table or element instance references. Its length is per flow and its
 * elements are chunks of WASM_REFS_CHUNK references, each captured through cow_capture_host_record before a
 * write, so forked flows share every chunk neither wrote. Chunk storage is shared and only grows: elements past a
 * flow's length are null in that flow's view, because every write past another flow's length is that flow's
 * captured record. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_REFERENCE_VECTOR_H
#define ENGINE_HOST_BROWSER_CORE_WASM_REFERENCE_VECTOR_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "core/wasm/store.h"

/* A scheduler-owned granularity of capture, a placeholder like linear_memory.h's chunk. */
#define WASM_REFS_CHUNK 64u

void wasm_refs_init(JSContext *ctx);
void wasm_refs_free(void);
/* A new vector of `n` null references; JS_NULL when its storage cannot be had (the count is the page's). */
JSValue wasm_refs_new(JSContext *ctx, uint64_t n);
bool wasm_refs_is(JSValueConst v);
uint64_t wasm_refs_length(JSValueConst v);
/* Element `i`, below the running flow's length (asserted). */
WasmValue wasm_refs_get(JSValueConst v, uint64_t i);
void wasm_refs_set(JSValueConst v, uint64_t i, WasmValue r);
/* Whether `n` more elements can be counted (§4.7.1's growtable admits only that); then append `n` null
   references, whose storage that cannot be had is a CHECK (out of memory). */
bool wasm_refs_growable(JSValueConst v, uint64_t n);
void wasm_refs_grow(JSValueConst v, uint64_t n);
/* §4.6.7's elem.drop: the length becomes 0. */
void wasm_refs_drop(JSValueConst v);

#endif
