/* The instance's allocators refusing through one edge — see reclaim.h for what that is and why a reclaim
   inside a lexbor call is safe. */
#include "solver/reclaim.h"
#include "check.h"

#include <stdlib.h>

/* base.h, not core/lexbor.h: the four allocator pointers and lexbor_memory_setup are declared there, and
   LXB_STATUS_OK — which is what says the setup took — is declared only here. */
#include <lexbor/core/base.h>

/* THE RUNTIME WITH A FRONTIER TO SELL, or NULL. It is a module static because lexbor's allocator is four
   GLOBAL function pointers taking no opaque, which is a fact about lexbor and not a choice here — and it is
   exactly the "one fact answered from one place for many agents" shape CLAUDE.md warns about, so the install
   asserts that only one runtime ever claims it. */
static JSRuntime *g_rt;

/* THE FOUR WRAPPERS. Each is the plain C allocator plus the refusal edge: try, and if the answer is no, ask
   the engine to give a flow back and try again — for as long as it keeps saying it gave one. The loop's end is
   the engine answering that it has nothing left, which is the physical floor, and never a count of attempts. */
static void *lx_malloc(size_t size)
{
    for (;;) {
        void *p = malloc(size);
        /* A ZERO-SIZE REQUEST'S NULL IS AN ANSWER, NOT A REFUSAL. Selling a flow to satisfy a request for no
           bytes would page the frontier out one member at a time and end with the same NULL. */
        if (p || size == 0 || !g_rt || !JS_ReclaimMemory(g_rt, size))
            return p;
    }
}

static void *lx_realloc(void *dst, size_t size)
{
    for (;;) {
        void *p = realloc(dst, size);
        /* `dst` SURVIVES A REFUSAL, which is what makes the retry sound rather than a second read of a freed
           block: C requires realloc to leave the original allocation untouched when it returns NULL. THE ONE
           CASE WHERE IT DOES NOT is size 0, which IS a free and whose NULL is the successful answer — retrying
           that one would hand realloc a pointer it has already released, so it is excluded by the value that
           makes it a free rather than by a flag. */
        if (p || size == 0 || !g_rt || !JS_ReclaimMemory(g_rt, size))
            return p;
    }
}

static void *lx_calloc(size_t num, size_t size)
{
    for (;;) {
        void *p = calloc(num, size);
        if (p || num == 0 || size == 0 || !g_rt || !JS_ReclaimMemory(g_rt, num * size))
            return p;
    }
}

/* THE ONE OF THE FOUR THAT RETURNS VOID — lexbor_memory_free_f is `void (*)(void *)`, while the public
   lexbor_free that calls it answers NULL for the caller's convenience. Writing this one to the public
   function's shape instead of the pointer's is an incompatible function pointer, so the difference is stated
   rather than discovered. A free has no refusal and so has no edge; it is here because lexbor_memory_setup
   takes all four or none. */
static void lx_free(void *dst)
{
    free(dst);
}

void reclaim_install(JSRuntime *rt, JSMemoryReclaimFunc *cb, void *opaque)
{
    DCHECK(rt != NULL && cb != NULL,
           "the reclaim edge was installed with no runtime or no callback — the wrappers would then be plain "
           "malloc for the whole session and the frontier would climb to the floor with nothing to sell");
    /* ONE RUNTIME, ASSERTED, because the allocator this points lexbor at is PROCESS-GLOBAL. Two live runtimes
       in one process would share these wrappers, so the second one's parser allocations would be answered by
       paging the FIRST one's frontier — freeing memory in a document that is not short of it while the one
       that is keeps climbing. An instance is one runtime and one frontier; a host that wants two must give
       lexbor a per-runtime allocator first. */
    DCHECK(g_rt == NULL || g_rt == rt,
           "a second runtime claimed the browser half's allocator — lexbor's is four process-global function "
           "pointers, so the two would share one edge and one instance's allocation would be answered by "
           "selling the other instance's flows");
    JS_SetMemoryReclaimHook(rt, cb, opaque);
    g_rt = rt;
    /* IT IS THE HTML PARSER'S MEMORY THAT THIS IS ABOUT. Installed here rather than at engine start-up because
       this call is the one that also names the runtime: a wrapper installed with no runtime recorded is a
       wrapper that would have to be installed twice to be useful, and the second install is the one that gets
       forgotten. Re-setting the same four pointers on a later session is a no-op by construction. */
    CHECK(lexbor_memory_setup(lx_malloc, lx_realloc, lx_calloc, lx_free) == LXB_STATUS_OK,
          "the browser half's allocator could not be routed through the engine's refusal edge — every DOM "
          "allocation would then reach the RAM floor with a frontier full of paged-out-able flows behind it");
}

void reclaim_uninstall(JSRuntime *rt)
{
    DCHECK(g_rt == NULL || g_rt == rt,
           "a session closed the reclaim edge that a different runtime had opened");
    JS_SetMemoryReclaimHook(rt, NULL, NULL);
    /* THE WRAPPERS STAY. They are transparent with no runtime recorded, and lexbor's allocator is global:
       putting malloc/realloc/calloc/free back would mean a document parsed between two sessions used a
       different allocator from the one that will free it, which is only safe because they are the same
       underlying functions — and "only safe because" is the kind of reasoning this file exists to remove. */
    g_rt = NULL;
}
