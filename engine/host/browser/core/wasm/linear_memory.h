/* WebAssembly Core Specification (Release 3.0) §4.2.9 "Memory Instances": a linear memory whose bytes are
 * per-flow state (design note §2). The bytes are fixed-size chunks; a store captures each chunk it writes into
 * the running flow's COW delta first (solver/cow.h's cow_capture_host_record, the chunk being a record of no
 * owned values), so forks share every chunk neither wrote and a flow's memory is its dirty chunk set over the
 * shared baseline. The size in pages is a record of its own, captured before §4.7.1's growmem changes it.
 * The instance is owned by a JS object of an internal class (no interface: JS API §5.3's Memory object, when it
 * is installed, references this one), which every delta entry holds, so a parked flow's chunks outlive every
 * other reference. Chunk storage is allocated as memory grows and never moves or shrinks; storage another flow
 * grew into reads zero in a flow that never wrote it, because every write over the baseline is captured.
 * Addresses and lengths are the page's input: an access past the flow's size is answered false, never
 * asserted. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_LINEAR_MEMORY_H
#define ENGINE_HOST_BROWSER_CORE_WASM_LINEAR_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"
#include "core/wasm/wasm_module.h"

#define WASM_PAGE_SIZE 65536u

/* The chunk, the unit a flow's first write copies. It is a placeholder, not a measured value: a smaller chunk
   copies less at a flow's first write to it and costs more entries per written range, and 4 KiB, the host
   page, also keeps untouched chunk storage free of resident memory. Evidence would be the written-byte to
   copied-byte ratio of the sites' modules once the interpreter runs them. */
#define WASM_MEMORY_CHUNK 4096u

void wasm_memory_init(JSContext *ctx);
void wasm_memory_free(void);

/* §4.7.1's allocmem: a memory of type `type`, its min pages of zeros, owned by the returned object. A RangeError
   when the bytes cannot be allocated (JS API §5.3's constructor throws one). */
JSValue wasm_memory_new(JSContext *ctx, const WasmLimits *type);
/* Whether `v` is a memory instance's owner. */
bool wasm_memory_is(JSValueConst v);
/* The running flow's size in pages, and its byte length. */
uint64_t wasm_memory_pages(JSValueConst mem);
/* Read or write `n` bytes at `addr` of the running flow's memory; false, with nothing read or written, when
   [addr, addr + n) is not inside its size (§4.6.8's out-of-bounds trap). */
bool wasm_memory_load(JSValueConst mem, uint64_t addr, size_t n, uint8_t *out);
bool wasm_memory_store(JSValueConst mem, uint64_t addr, size_t n, const uint8_t *in);
/* §4.7.1's growmem by `delta` pages: the old size, or -1 where growmem fails (the type's maximum, the address
   type's bound) or the storage cannot be allocated. */
int64_t wasm_memory_grow(JSValueConst mem, uint64_t delta);

#endif
