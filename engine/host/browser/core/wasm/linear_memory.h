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

/* §4.7.1's allocmem: a memory of type `type`, its min pages of zeros, owned by the returned object; JS_NULL when
   the bytes cannot be allocated, which the page's type chose (JS API §5.3's constructor throws a RangeError). */
JSValue wasm_memory_new(JSContext *ctx, const WasmLimits *type);
/* Whether `v` is a memory instance's owner. */
bool wasm_memory_is(JSValueConst v);
/* The running flow's size in pages. */
uint64_t wasm_memory_pages(JSValueConst mem);
/* §4.2.9's meminst.type in the running flow: the declared type with the size as its minimum, which growmem
   raises (§4.5.2's external type). */
void wasm_memory_type(JSValueConst mem, WasmLimits *out);
/* Whether [addr, addr + n) lies inside the running flow's size. */
bool wasm_memory_holds(JSValueConst mem, uint64_t addr, uint64_t n);
/* Read or write `n` bytes at `addr` of the running flow's memory; false, with nothing read or written, when
   [addr, addr + n) is not inside its size (§4.6.8's out-of-bounds trap). */
bool wasm_memory_load(JSValueConst mem, uint64_t addr, size_t n, uint8_t *out);
bool wasm_memory_store(JSValueConst mem, uint64_t addr, size_t n, const uint8_t *in);
/* Whether §4.7.1's growmem by `delta` pages succeeds (within the type's maximum and the address type's bound)
   to a size whose bytes a size_t counts. */
bool wasm_memory_growable(JSValueConst mem, uint64_t delta);
/* growmem by `delta` pages, which wasm_memory_growable admits: the old size. Storage that cannot be had is a
   CHECK (out of memory). */
uint64_t wasm_memory_grow(JSValueConst mem, uint64_t delta);

#endif
