/* Per-flow DOM COW delta — the DOM half of the copy-on-write flow isolation (the JS heap half is in quickjs).
 *
 * Every DOM mutation a flow makes (an attribute write, a node insert) is RECORDED into the running flow's
 * delta while capture is on. On a context-switch the scheduler UNAPPLIES the outgoing flow's writes (restoring
 * the shared baseline) and APPLIES the incoming flow's — so any number of flows interleave without seeing each
 * other's DOM writes, and a parked flow's delta rides along as an opaque buffer. Self-contained: it owns the
 * delta buffer and only calls the Lexbor DOM API; the host-edges call the capture hooks, the scheduler calls
 * apply/unapply/buf. */
#ifndef ENGINE_HOST_DOM_COW_H
#define ENGINE_HOST_DOM_COW_H

#include <lexbor/html/html.h>
#include "quickjs.h"

/* Capture gate: while non-zero, DOM mutations are recorded into the running flow's delta. The scheduler sets
   it (0 pre-baseline / during a shared chunk eval, 1 once the per-flow baseline is fixed). */
extern int g_dom_capture;

/* The DOM delta also carries each captured attribute's TAINT SHADOW (attr_shadow) so a source stashed in a DOM
   attribute is isolated per flow EXACTLY like the attribute's value — set once at init so the swap logic can
   dup/free the shadow's opaque JSValues without threading a context through every host-edge/scheduler call. */
void dom_cow_set_ctx(JSContext *ctx);

/* Host-edge hooks — record a mutation BEFORE it happens so it can be reverted. */
void dom_attr_capture(lxb_dom_element_t *el, const char *name);   /* pre-write attribute baseline */
void dom_insert_capture(lxb_dom_node_t *node);                    /* an inserted node */

/* Scheduler hooks — swap the running flow's DOM writes. */
void dom_revert(void);      /* DISCARD the running flow's writes -> baseline (flow completed) */
void dom_unapply(void);     /* flow -> parked: stash the flow's values, restore the baseline */
void dom_apply(void);       /* parked -> flow: restore the flow's values over the baseline */

/* Park/resume the delta buffer as an opaque handle (the scheduler stores it on the Flow as void*). */
void *dom_buf_take(int *n, int *cap);          /* detach the current buffer (returns it; delta now empty) */
void dom_buf_load(void *buf, int n, int cap);  /* attach a parked buffer as the current delta */
void *dom_buf_snapshot(int *out_n, int *out_cap);  /* copy the applied ATTRIBUTE delta for a deferred continuation flow to inherit */
void dom_buf_free(void *buf, int n);           /* free a parked buffer (its nodes stay owned by the doc) */

/* Persistent-versioned-DOM fork (mirrors the heap's JS_CowFork): freeze the running flow's DOM head into a
   shared immutable base segment (refcount 2) a snapshot-forked sibling references in O(1). The base chain rides
   alongside the head exactly like the heap's cow_base; the scheduler stores it on the Flow as void*. */
void *dom_cow_fork(void);       /* freeze head -> shared base; returns it (the sibling stores the 2nd reference) */
void *dom_base_take(void);      /* detach the shared base chain (park it on the flow) */
void dom_base_load(void *base); /* install a parked flow's base chain (before dom_apply) */
void dom_base_free(void *base); /* drop a flow's reference to a base chain (free iff last) */

#endif
