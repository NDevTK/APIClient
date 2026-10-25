/* The C heap's live bytes, kept where they move: this process's malloc family is replaced (c_alloc.c) by entries
 * that forward to the allocator underneath (glibc's __libc_* natively, emscripten's dlmalloc in the wasm build) and
 * keep a running total of the chunk every live block occupies, so the total is O(1) to read on both builds rather
 * than a walk of the allocator's bins or chunks. Under a sanitizer, which owns malloc itself, the sanitizer's own
 * O(1) counter answers. */
#ifndef APICLIENT_SOLVER_C_ALLOC_H
#define APICLIENT_SOLVER_C_ALLOC_H

#include <stddef.h>

/* The chunk bytes (usable size plus header) of every live block the malloc family handed out, quickjs's js_malloc
   and Lexbor's arenas included, since both route to malloc. A gauge. */
size_t c_alloc_live(void);

/* The allocator's own reading, one walk of it, asked per census only. `arena` is the address space the allocator
   has taken from the system (in wasm, linear memory, which only grows). `residue` is its in-use bytes beyond the
   running total's chunks (usable plus one header word each), checked here: never negative; on glibc the mmapped
   chunks (usable plus two words) equal `hblkhd` exactly, and the heap half is net of glibc's tcache control block,
   so with its tcache off (GLIBC_TUNABLES=glibc.malloc.tcache_count=0) it is exact. What remains is glibc's
   tcache-held chunks, or on wasm dlmalloc's segment overhead and the C library's own `emscripten_builtin_*` blocks.
   -1 under a sanitizer, which keeps no such reading. */
typedef struct CAllocCensus {
    size_t    arena;
    long long residue;
} CAllocCensus;
void c_alloc_census(CAllocCensus *out);

#endif
