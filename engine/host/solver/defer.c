/* Deferred / callback FLOWS — see defer.h. Split out of scheduler.c: a callback becomes a first-class flow. */
#include <string.h>
#include "solver/defer.h"
#include "solver/scheduler.h"   /* reg_add + the running-flow state (g_running/g_cur_val/g_cur_flow) + g_reg/g_reg_n */
#include "solver/heap_cow.h"    /* heap_cow_fork — the continuation SHARES the current heap delta (O(1) base) */
#include "solver/dom_cow.h"     /* dom_cow_fork — and the DOM delta likewise */

void flow_defer_callback(JSContext *ctx, JSValueConst cb) {
    Flow *f = reg_add(ctx, JS_DupValue(ctx, cb), g_running ? g_cur_val : 1.0, NULL, 0);
    /* CONTINUATION: a callback deferred from a running (revert-)flow inherits that flow's live PROPERTY delta,
       so it sees state the flow wrote before deferring (obj.x = tainted; setTimeout(()=>use(obj.x))) — a
       fresh-baseline flow reads it undefined. Closure-var state already rides the callback's own closure, so
       only property writes need this snapshot. During the INITIAL boot g_cur_flow is NULL (the forking boot
       flow has no reg-entry), so nothing is snapshotted here: boot's writes commit to the post-boot baseline,
       which a boot-deferred flow already sees. */
    if (g_running && g_cur_flow) {
        /* SHARE the delta, don't COPY it — the persistent-versioned-heap BRANCH: heap_cow_fork freezes the running
           flow's current heap delta into an immutable, refcounted base SEGMENT that this continuation references
           in O(1) (not the O(delta) copy it replaces). The parent continues over a fresh empty head; the callback
           sees the defer-point state (the shared base) plus its own later writes. Superior to the copy in two
           ways: O(1) instead of O(delta), and it carries the closure-var (slot) writes the copy dropped as
           pointer-fragile — so a callback reading a closure var the handler wrote before deferring now sees it.
           The parent keeps its own reference to the same segment (g_cow_base), released on its suspend/complete;
           refcount 2 balances parent + this sibling. */
        f->cow_base = heap_cow_fork(ctx); f->cow = NULL; f->cow_n = f->cow_cap = 0;
        f->dom_base = dom_cow_fork(); f->dom = NULL; f->dom_n = f->dom_cap = 0;   /* SHARE the DOM delta too (O(1) base segment), symmetric to the heap */
    }
}

void drive_opaque_cb(JSContext *ctx, JSValueConst cb, JSValueConst coll) {
    /* Register cb as a starter FLOW. NO seen-set: an unbounded recursion that calls `x.forEach(cb)` per level
       registers cb per level, but every level past the first drives cb with the SAME opaque args -> emits
       nothing new -> the WFQ STARVES those flows to ~0 CPU and RAM-pressure PARKS the tail to the IDB cold
       tier (unbounded-until-disk, the intended design). A dedup keyed by function identity was a BANNED
       seen-set (§NO BOUNDS: "only emitted output — never identity — proves a flow is done") masking the
       recursion as a hang; the cooperative quantum keeps the worker responsive while it starves + parks. */
    flow_defer_callback(ctx, cb);
    /* PROVENANCE: the element `cb` receives is an element OF `coll` (the reply/injected opaque the method was
       called on), so tag it with coll's shape — the starter drives arg0 as {reply} (not a bare {}), keeping
       the collection's taint through f.key etc. A non-opaque/untagged receiver leaves drive_src NULL (default
       g_concolic args). Registered flow is the last one appended by flow_defer_callback. */
    const char *cs = JS_IsConcolic(coll) ? JS_ConcolicShapeC(coll) : NULL;
    if (cs && cs[0] && strcmp(cs, "{}") != 0 && g_reg_n > 0) g_reg[g_reg_n - 1].drive_src = strdup(cs);
}
