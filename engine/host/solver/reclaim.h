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

/* ─────────────────────────────────────────────────────────────────────────────────────────────────────────
   AND THE THIRD ALLOCATOR, WHICH IS THE SOLVER'S OWN — the one with no injection point to take.
 *
 * With the runtime's allocator and lexbor's both routed, the wall run died at dom_cow.c's undo-log realloc:
 * `perFlowKiB 487464` against `domSegKiB 316067`, so roughly 800 MB of a 2 GB address space is the frontier's
 * own bookkeeping — its heap deltas, its DOM undo logs, the frozen segments they fork into, and the flows
 * themselves — allocated by direct calls to the C library. There is nothing to hand a set of function pointers
 * to; the sites have to ask.
 *
 * THE RULE IS ONE SENTENCE: AN ALLOCATION THE ENGINE MAKES FOR A RUNNING FLOW MUST BE ABLE TO SHRINK THE
 * FRONTIER BEFORE IT FAILS. In practice that is the frontier's own state — cow.c's delta entries and their
 * frozen segments, dom_cow.c's undo log and document segments, decide.c's and concolic.c's chains, flow.c's
 * registry and the Flow structs themselves — and the transients computed from it on the same path, which are
 * not worth telling apart: an engine holding a releasable working set should spend it rather than abort,
 * whichever of its own allocations happens to be the one that asks.
 * EVERY SUCH SITE KEEPS ITS `CHECK`. The edge is what makes the failure recoverable; the CHECK is still the
 * truth when the engine answers that it has nothing left to sell. A site that traded its CHECK for this has
 * traded a crash for a silently corrupted delta — dom_cow.c's undo log is the one that proves it, since a
 * capture it cannot record is a DOM write that never reverts.
 *
 * ONE PLACE IS DELIBERATELY NOT CONVERTED: the park document (cold.c's park_reserve). It is the SALE'S OWN
 * OUTPUT, so an allocation it cannot make is not one another sale could fund — every sale needs it first. Its
 * CHECK is the floor and says so.
 *
 * THE RECLAIM RE-ENTERS THE STRUCTURE BEING GROWN, and at these sites that is not a hazard to argue away but
 * the ordinary case: selling a flow frees delta entries and DOM segments, so a reclaim raised by dom_cow's own
 * realloc runs dom_cow's own code. Three things make it sound, and the third is the one converting a site has
 * to get right.
 *   - THE ONE-DEEP LATCH inside JS_ReclaimMemory (quickjs.h): an allocation made BY the sale refuses without
 *     asking again, so the frees the sale performs cannot recurse into another sale. This is the first place
 *     that latch is load-bearing rather than defensive.
 *   - THE FLOW SOLD IS NEVER THE FLOW THE ALLOCATION IS FOR. engine_reclaim_tail takes flow_worst(running)
 *     and asserts both halves of that, which at these sites is the sharper statement that the buffer the
 *     caller is about to write through cannot be the buffer the sale just freed. Hence the conversion rule:
 *     THE POINTER HANDED TO reclaim_realloc MUST BELONG TO THE RUNNING FLOW OR TO NO FLOW AT ALL. `dst`
 *     survives a refusal (C leaves it untouched when realloc returns NULL) and the retry hands it back, so a
 *     buffer a SALE could free is a use-after-free on the second attempt — and it would surface as the second
 *     attempt succeeding, which is the shape that never reproduces.
 *   - AND EVERY STRUCTURE DESCRIBES THE BUFFER THAT EXISTS, FOR THE WHOLE OF THE ASK. A capacity doubled
 *     BEFORE the allocation advertises room the array does not have to everything the sale runs; a segment
 *     allocated in the middle of a fork's unapply/re-apply round trip asks while the head is half-taken-down.
 *     So the capacity is published only after the allocation returns (cow.c's cow_room_for_one is the one
 *     copy of that, concolic.c and flow.c say it at their single sites) and the fork segments are allocated
 *     before their round trip begins. This is the half that is CONSTRUCTION rather than argument, and it is
 *     the half that survives the next site somebody adds.
 *
 * THERE IS NO reclaim_free, AND THAT IS NOT AN OMISSION. These three are the C allocator plus a retry, so what
 * they return is freed by plain free() and a converted site's free needs no edit at all. A refusal is the only
 * thing this file adds; ownership is untouched. */
void *reclaim_malloc(size_t size);
void *reclaim_realloc(void *ptr, size_t size);
void *reclaim_calloc(size_t num, size_t size);

/* Route this instance's OTHER allocators into `cb`, and install `cb` as the runtime's own reclaim hook. One
   call, because "which allocators refuse through the edge" is one fact and a second install site is a list to
   forget. Asserts it is not being pointed at a second runtime: lexbor's allocator is process-global, so two
   live runtimes in one process would have one of them selling the other's frontier. */
void reclaim_install(JSRuntime *rt, JSMemoryReclaimFunc *cb, void *opaque);

/* …and the frontier is no longer for sale. The wrappers stay installed (they are transparent with no runtime
   recorded); what ends is the claim that there is a frontier behind them. */
void reclaim_uninstall(JSRuntime *rt);

#endif
