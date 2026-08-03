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

/* The DOM-mutation CHOKEPOINT — capture-then-mutate ATOMICALLY, in ONE call. A browser component mutates the
   tree ONLY through these, never through raw lxb_dom_* mutators + a separate capture, so a DOM write cannot
   bypass time-travel capture (the browser engineer's "one place to reason about"). This is the fork-free answer
   to unbypassable capture: rather than fork Lexbor to hook its internal write primitives, we OWN the mutation
   API on top of Lexbor and funnel every write through here — enforced structurally by poisoning the raw
   lxb_dom_* mutators in the component build. */
void dom_cow_set_attribute(lxb_dom_element_t *el, const char *name, const char *val, size_t val_len);
/* the attribute setter's twin — removeAttribute, and what a boolean reflection does when set to false. */
void dom_cow_remove_attribute(lxb_dom_element_t *el, const char *name);
/* node-insert chokepoint — the tree-structure twin of dom_cow_set_attribute: capture the insertion THEN attach
   the child, so a subtree a flow appends reverts per-flow (detached on context-switch, re-attached on resume). */
void dom_cow_append_child(lxb_dom_node_t *parent, lxb_dom_node_t *child);
/* the same insert AT A POSITION — §4.2.3 insertBefore, and the reference child's own re-parenting. */
void dom_cow_insert_before(lxb_dom_node_t *ref, lxb_dom_node_t *child);
/* Detach a node the BASELINE may own — captured so it comes back on revert/unapply. innerHTML= needs it:
   it REPLACES children, and an uncaptured removal leaks the old subtree across a context switch. */
void dom_cow_remove_child(lxb_dom_node_t *node);
/* A character-data node's VALUE (§4.10 `data`) — the third thing a flow can change about the tree, on a node
   whose identity must survive the write, so it cannot be a remove+insert of a replacement. */
void dom_cow_set_text(lxb_dom_node_t *node, const char *val, size_t val_len);
/* A DOM PROPERTY's taint (textContent and its kind): the value half is already captured as Text nodes by the
   insert/remove chokepoints, so this captures the taint shadow so it reverts with them. JS_UNDEFINED clears. */
void dom_cow_set_prop_taint(JSContext *ctx, lxb_dom_element_t *el, const char *name, JSValueConst opaque);

/* Lower-level capture primitives the chokepoint is built on (record a mutation BEFORE it happens). Direct use is
   reserved for the mutation ops that compose them (the chokepoint above, and node-insert once it lands). */
void dom_attr_capture(lxb_dom_element_t *el, const char *name);   /* pre-write attribute baseline */
void dom_insert_capture(lxb_dom_node_t *node);                    /* an inserted node */

/* Scheduler hooks — swap the running flow's DOM writes. */
void dom_revert(void);      /* DISCARD the running flow's writes -> baseline (flow completed) */
void dom_unapply(void);     /* flow -> parked: stash the flow's values, restore the baseline */
void dom_apply(void);       /* parked -> flow: restore the flow's values over the baseline */

/* Park/resume the delta buffer as an opaque handle (the scheduler stores it on the Flow as void*). */
void *dom_buf_take(int *n, int *cap);          /* detach the current buffer (returns it; delta now empty) */
void dom_buf_load(void *buf, int n, int cap);  /* attach a parked buffer as the current delta */
void dom_buf_free(void *buf, int n);           /* free a parked buffer (its nodes stay owned by the doc) */

/* Persistent-versioned-DOM fork (mirrors the heap's JS_CowFork): freeze the running flow's DOM head into a
   shared immutable base segment (refcount 2) a snapshot-forked sibling references in O(1). The base chain rides
   alongside the head exactly like the heap's cow_base; the scheduler stores it on the Flow as void*. */
void *dom_cow_fork(void);       /* freeze head -> shared base; returns it (the sibling stores the 2nd reference) */
void *dom_base_take(void);      /* detach the shared base chain (park it on the flow) */
void dom_base_load(void *base); /* install a parked flow's base chain (before dom_apply) */
void dom_base_free(void *base); /* drop a flow's reference to a base chain (free iff last) */
void dom_base_ref(void *base);  /* add ONE ref (each orphan forks the document flow's shared DOM delta) */

#endif
