/* THE INSTANCE'S ALLOCATORS, ALL OF THEM, REFUSING THROUGH ONE EDGE.
 *
 * The engine survives the RAM floor by selling the frontier's tail (engine.c's engine_reclaim_tail): at the
 * moment an allocation cannot be satisfied, the lowest-weight flow is written to the cold tier as a recipe and
 * its memory given back, and the allocation is retried. That works only where the refusal is SEEN, and the
 * runtime's allocator is not the only one this instance has.
 *
 * MEASURED, NOT ASSUMED, WHICH IS WHY THIS FILE EXISTS. With the refusal edge in js_malloc_rt/js_calloc_rt/
 * js_realloc_rt and a 2 GB address-space wall, the fixture did not page a single flow: it aborted at
 * `lxb_html_parser_create` with 9462 flows live and 9462 still tracked by the census, because the allocation
 * that reached the wall was LEXBOR'S. The shape of that frontier says it was not bad luck — 315721 KiB of
 * frozen DOM segments against 23831 KiB of heap segments, so the memory this frontier is made of is mostly
 * allocated by the HTML parser, and the odds that the first refusal lands in the runtime's allocator are
 * correspondingly small. An edge only the runtime crosses is an edge most of this engine's memory walks past.
 *
 * LEXBOR'S ALLOCATOR IS FOUR GLOBAL FUNCTION POINTERS (lexbor/core/lexbor.h: lexbor_memory_setup), not a
 * per-document allocator and not one that carries an opaque — so the wrappers reach the runtime through this
 * file's own record of it, which is set with the session that has a frontier to sell and cleared with it. With
 * no session installed the wrappers are exactly malloc/realloc/calloc/free, which is what lexbor uses by
 * default and what every allocation before the first session gets.
 *
 * WHY A RECLAIM INSIDE A LEXBOR CALL IS SAFE, read out of lexbor's own source rather than assumed, because the
 * reclaim frees DOM nodes and therefore re-enters the library it was called from:
 *   - every lexbor allocation is a LEAF. `lexbor_mem_alloc` calls `lexbor_mem_chunk_make` and only then links
 *     the chunk (`mem->chunk->next = …; mem->chunk = …`), and `lexbor_array_push` re-reads `array->list` and
 *     `array->length` after `lexbor_array_expand` returns. No structure is left half-updated across the
 *     allocation that refuses, so the reclaim never observes one.
 *   - the FREE path does not re-enter the pool whose allocation is suspended. Destroying a node is
 *     `lexbor_dobject_free` (a push onto `dobject->cache`, an array) and `lexbor_mraw_free` (an insert into
 *     `mraw->cache`, a BST with its own pools); neither calls `lexbor_mem_alloc` on the `mem` the outer call is
 *     inside of.
 *   - and the nodes it frees are the TAIL FLOW's own creations, which per-flow COW isolation guarantees the
 *     running flow's operation cannot be holding: a flow's created nodes are recorded in its own delta and are
 *     detached the moment it is switched out.
 * The one-deep latch inside JS_ReclaimMemory closes the rest: an allocation made BY the reclaim (an array
 * expanding as a node returns to its cache) refuses without asking again, so lexbor's own status-returning
 * error path handles it exactly as it does today. That is the same reason the runtime's edge refuses during a
 * collection — a callback that freed an object while the collector walked its list would unlink a node out of
 * the walk — and it is deliberately ONE mechanism rather than a second reclaimer with its own guards. */
#ifndef ENGINE_HOST_SOLVER_RECLAIM_H
#define ENGINE_HOST_SOLVER_RECLAIM_H

#include "quickjs.h"

/* Route this instance's OTHER allocators into `cb`, and install `cb` as the runtime's own reclaim hook. One
   call, because "which allocators refuse through the edge" is one fact and a second install site is a list to
   forget. Asserts it is not being pointed at a second runtime: lexbor's allocator is process-global, so two
   live runtimes in one process would have one of them selling the other's frontier. */
void reclaim_install(JSRuntime *rt, JSMemoryReclaimFunc *cb, void *opaque);

/* …and the frontier is no longer for sale. The wrappers stay installed (they are transparent with no runtime
   recorded); what ends is the claim that there is a frontier behind them. */
void reclaim_uninstall(JSRuntime *rt);

#endif
