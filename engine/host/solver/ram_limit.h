/* The memory this instance may hold, as its platform states it now: the limit RAM pressure is a share of
 * (solver/engine.h's engine_set_ram_share), like `navigator.deviceMemory` is a fact the platform states to a page.
 * The wasm build's limit is its maximum linear memory. A native process is bounded by its soft RLIMIT_AS and by
 * every memory cgroup on its path. A cgroup is charged by every process in it, so what this process may hold
 * there is its own resident bytes plus the cgroup's headroom: the limit less the charges the kernel would not
 * reclaim before it kills (everything but the inactive file cache). That headroom moves with the other
 * processes' charges, so the native limit is read again at every ask; the binding source is the smaller. */
#ifndef APICLIENT_SOLVER_RAM_LIMIT_H
#define APICLIENT_SOLVER_RAM_LIMIT_H

#include <stdint.h>

typedef enum {
    RAM_LIMIT_NONE = 0,
    RAM_LIMIT_WASM_HEAP_MAX,
    RAM_LIMIT_RLIMIT_AS,
    RAM_LIMIT_MEMCG
} RamLimitSource;

typedef struct {
    RamLimitSource source;
    uint64_t bytes;     /* the limit; 0 exactly when `source` is RAM_LIMIT_NONE */
    uint64_t nonheap;   /* the part of `bytes` no heap block can use now; below `bytes` (CHECKed) */
} RamLimit;

/* Reads the limit now. `heap_footprint` is the allocator's address space at this instant (solver/c_alloc.h's
   c_alloc_footprint): under RLIMIT_AS the non-heap part is the mapped size less it, under a cgroup the resident
   size less it, and in wasm the static data and stack below `__heap_base`. */
void ram_limit_read(RamLimit *out, uint64_t heap_footprint);

/* The census arm naming `source`, as a one-arm object: `{"memcg":1}`. */
const char *ram_limit_source_json(RamLimitSource source);

/* The tightest memory cgroup limit on this process's path (cgroup v1's `memory.limit_in_bytes`, v2's
   `memory.max`), or UINT64_MAX when none is set. Native only; the wasm build's instance has no cgroup. */
uint64_t ram_limit_cgroup_max(void);

#endif
