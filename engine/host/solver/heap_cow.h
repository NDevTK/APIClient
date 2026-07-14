/* Per-flow HEAP COW delta — the JS-heap half of the copy-on-write flow isolation (the DOM half is dom_cow.c).
 *
 * A flow must explore from the post-boot BASELINE, isolated from OTHER flows' writes to SHARED state (a global/
 * lexical/closure cell, or a baseline object's property one function mutates and another reads), yet see its OWN
 * writes within its run. Each flow owns a delta = a mutable HEAD (the entries it wrote) layered over a chain of
 * IMMUTABLE, refcounted, structurally-shared base segments (the persistent-versioned heap; a fork freezes the
 * head into a shared base so a snapshot-forked sibling shares the parent's O(N) delta in O(1)). A context-switch
 * UNAPPLIES the outgoing flow's writes (restoring the baseline) and APPLIES the incoming one's — writers
 * interleave, no serial revert.
 *
 * This component owns the delta model + versioning + verb-API + boot helpers, reaching object internals ONLY
 * through the two quickjs primitives JS_CowPropSlot / JS_ObjCowCapturable (find_own_property + the capture-policy
 * bitfields are private). quickjs.c keeps only g_cow_active (the interpreter-wide 'in exploration' signal,
 * read/written via JS_CowGetActive/JS_CowSetActive) and thin g_cow_active-gated CAPTURE DISPATCHERS at the
 * write-sites that forward to this component's hooks (installed by heap_cow_init). The twin of dom_cow.c;
 * the verb-API mirrors dom_apply, dom_unapply, dom_revert, dom_cow_fork, dom_buf_take, dom_base_take. */
#ifndef ENGINE_HOST_SOLVER_HEAP_COW_H
#define ENGINE_HOST_SOLVER_HEAP_COW_H

#include "quickjs.h"

void heap_cow_init(JSContext *ctx);   /* install the capture hooks with the engine (once, at engine setup) */

/* Scheduler verb-API — swap the running flow's heap writes (same discipline as dom_apply/unapply/revert). */
void heap_cow_unapply(JSContext *ctx);   /* flow -> parked: stash the flow's values, restore the baseline (head + base chain) */
void heap_cow_apply(JSContext *ctx);     /* parked -> flow: restore the flow's values over the baseline */
void heap_cow_revert(JSContext *ctx);    /* running flow ends: discard head writes, restore baseline, drop one base ref */
void *heap_cow_fork(JSContext *ctx);     /* freeze head -> shared immutable base (refcount 2); returns it (the sibling stores it) */

/* Park/resume the delta as opaque handles (the scheduler stores head + base on the Flow as void*). */
void *heap_cow_buf_take(int *n, int *cap);          /* detach the head buffer (returns it; head now empty) */
void heap_cow_buf_load(void *buf, int n, int cap);  /* install a parked head buffer (before heap_cow_apply) */
void heap_cow_buf_free(JSContext *ctx, void *buf, int n);   /* free an EVICTED parked head buffer */
void *heap_cow_base_take(void);                     /* detach the shared base chain (park it on the flow) */
void heap_cow_base_load(void *base);                /* install a parked flow's base chain */
void heap_cow_base_free(JSContext *ctx, void *base);/* drop a flow's reference to a base chain (free iff last) */
void heap_cow_base_ref(void *base);                 /* add ONE ref to a base chain (each orphan forks the document flow's shared delta) */

/* Boot-delta helpers (boot-as-flow / candidate isolation). */
void *heap_cow_boot_delta_merge(JSContext *ctx, void *dst, int *pn, int *pcap);   /* append the active head into a host boot-delta buffer */
void heap_cow_seed_boot_inverse(JSContext *ctx, void *boot_buf, int boot_n);      /* seed the flow delta with the boot-inverse (heap -> pre-boot) */

#endif
