/* Per-flow DOM COW delta — see dom_cow.h. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "check.h"        /* CHECK — an OOM here corrupts DOM isolation, fatal in every build */
#include "solver/dom_cow.h"
#include "solver/attr_shadow.h"   /* the taint shadow rides the attribute delta (per-flow isolation of stashed taint) */
#include <lexbor/dom/dom.h>

typedef struct DomUndo {
    int kind; lxb_dom_element_t *el; char *name;
    lxb_char_t *old; size_t old_len; int had;                 /* kind 0: baseline attr VALUE */
    lxb_char_t *cur; size_t cur_len; int cur_had;             /* kind 0: flow's attr VALUE (valid while parked) */
    JSValue sh_old; int sh_had;                               /* kind 0: baseline attr TAINT shadow (opaque, or none) */
    JSValue sh_cur; int sh_cur_had;                           /* kind 0: flow's attr TAINT shadow (valid while parked) */
    lxb_dom_node_t *node, *parent, *next;                     /* kind 1/2: the node + its position */
    int detached;                                             /* kind 1: currently detached (unapplied) */
    /* kind 2: a node the BASELINE had and this flow REMOVED. The mirror of kind 1 in every direction: revert
       and unapply put it back, apply takes it away again. innerHTML= is what needs it — it REPLACES the
       children, and without a removal capture the old subtree would survive into the new markup and leak
       across a context switch into a sibling flow that never removed it. */
    int reinserted;                                           /* kind 2: currently back in the tree (unapplied) */
} DomUndo;

static DomUndo *g_dom_undo = NULL;
static int g_dom_undo_n = 0, g_dom_undo_cap = 0;
int g_dom_capture = 0;
static JSContext *g_cow_ctx = NULL;   /* for the shadow's JSValue dup/free (set at init) */

/* The DOM half of the persistent-versioned-heap: a mutable HEAD (g_dom_undo) layered over a chain of IMMUTABLE,
   refcounted, structurally-shared base segments (DomSeg), exactly mirroring the JS heap's CowSeg chain. A fork
   (dom_cow_fork) freezes the head into a segment BOTH the running flow and a snapshot-forked sibling reference
   (refcount 2), so a continuation/sibling SHARES the parent's O(N) DOM delta in O(1) instead of copying it. NULL
   until the first fork -> the base-chain walks are no-ops and behaviour is byte-identical to the flat buffer. */
typedef struct DomSeg { DomUndo *e; int n; struct DomSeg *base; int refcount; } DomSeg;
static DomSeg *g_dom_base = NULL;
static void dom_unapply_seg(DomSeg *s);   /* fwd: dom_revert (defined earlier) walks the base chain */
static void dom_seg_unref(DomSeg *s);

void dom_cow_set_ctx(JSContext *ctx) { g_cow_ctx = ctx; }

/* the current taint shadow for (el,name), dup'd (JS_UNDEFINED + *had=0 if none) */
static JSValue shadow_snapshot(lxb_dom_element_t *el, const char *name, int *had) {
    int si = attr_shadow_find(el, name);
    *had = (si >= 0);
    return (si >= 0 && g_cow_ctx) ? JS_DupValue(g_cow_ctx, attr_shadow_opaque(si)) : JS_UNDEFINED;
}
/* set (el,name)'s taint shadow to `v` (borrowed; attr_shadow_set dups it), or clear it when !had */
static void shadow_restore(lxb_dom_element_t *el, const char *name, JSValueConst v, int had) {
    if (g_cow_ctx) attr_shadow_set(g_cow_ctx, el, name, had ? v : JS_UNDEFINED);
}

static void dom_undo_push(DomUndo u) {
    if (g_dom_undo_n >= g_dom_undo_cap) {
        int nc = g_dom_undo_cap ? g_dom_undo_cap * 2 : 64;
        /* Skipping a DOM capture silently breaks DOM isolation (this flow's DOM write never reverts -> leaks
           into the next flow's baseline -> wrong sinks/taint). OOM is a should-never-happen: CRASH, never skip. */
        DomUndo *n = realloc(g_dom_undo, (size_t)nc * sizeof(DomUndo));
        CHECK(n, "dom-cow-oom: DOM undo-log realloc failed — DOM isolation would be silently corrupted");
        g_dom_undo = n; g_dom_undo_cap = nc;
    }
    g_dom_undo[g_dom_undo_n++] = u;
}
void dom_attr_capture(lxb_dom_element_t *el, const char *name) {
    if (!g_dom_capture) return;
    size_t vl = 0; const lxb_char_t *cur = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &vl);
    DomUndo u; memset(&u, 0, sizeof u);
    u.kind = 0; u.el = el; u.name = strdup(name); u.had = cur ? 1 : 0;
    CHECK(u.name, "dom-cow-oom: attr name strdup failed");
    if (cur) { u.old = malloc(vl ? vl : 1); CHECK(u.old, "dom-cow-oom: baseline attr snapshot malloc failed — the delta could not restore its baseline"); memcpy(u.old, cur, vl); u.old_len = vl; }
    u.sh_old = shadow_snapshot(el, name, &u.sh_had);   /* snapshot the baseline TAINT so it reverts with the value */
    u.sh_cur = JS_UNDEFINED;
    dom_undo_push(u);
}
void dom_insert_capture(lxb_dom_node_t *node) {
    if (!g_dom_capture) return;
    DomUndo u; memset(&u, 0, sizeof u); u.kind = 1; u.node = node; u.sh_old = u.sh_cur = JS_UNDEFINED; dom_undo_push(u);
}
/* THE DOM-mutation CHOKEPOINT (see dom_cow.h): capture the baseline THEN mutate, so a write cannot bypass the
   per-flow delta. Every browser-component attribute write funnels through here — the capture-before-mutate order
   is guaranteed in ONE place, not re-remembered at each call site. */
void dom_cow_set_attribute(lxb_dom_element_t *el, const char *name, const char *val, size_t val_len) {
    dom_attr_capture(el, name);   /* record baseline into the running flow's delta FIRST (no-op if !g_dom_capture) */
    lxb_dom_element_set_attribute(el, (const lxb_char_t *)name, strlen(name), (const lxb_char_t *)val, val_len);
}
/* Remove a node that the baseline may own. Capture its position FIRST, then detach — the same order the
   attribute and insert chokepoints use, so a removal cannot bypass the per-flow delta either. */
void dom_cow_remove_child(lxb_dom_node_t *node) {
    if (!node) return;
    if (g_dom_capture) {
        DomUndo u; memset(&u, 0, sizeof u);
        u.kind = 2; u.node = node;
        u.parent = node->parent; u.next = node->next;
        u.sh_old = u.sh_cur = JS_UNDEFINED;
        dom_undo_push(u);
    }
    lxb_dom_node_remove(node);
}

void dom_cow_append_child(lxb_dom_node_t *parent, lxb_dom_node_t *child) {
    dom_insert_capture(child);   /* record the insertion FIRST so it reverts per-flow (detached on unapply) */
    lxb_dom_node_insert_child(parent, child);
}
void dom_revert(void) {   /* DISCARD the running flow's DOM writes -> baseline (reverse order); empties the delta */
    for (int i = g_dom_undo_n - 1; i >= 0; i--) {
        DomUndo *u = &g_dom_undo[i];
        if (u->kind == 0) {   /* attribute: restore old value + old taint shadow, or remove/clear if it didn't exist */
            if (u->had && u->old) lxb_dom_element_set_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), u->old, u->old_len);
            else lxb_dom_element_remove_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name));
            shadow_restore(u->el, u->name, u->sh_old, u->sh_had);
            if (g_cow_ctx) { JS_FreeValue(g_cow_ctx, u->sh_old); JS_FreeValue(g_cow_ctx, u->sh_cur); }
            free(u->name); free(u->old); free(u->cur);
        } else if (u->kind == 1 && !u->detached) {   /* inserted node: detach it (baseline had none) */
            lxb_dom_node_remove(u->node);
        } else if (u->kind == 2 && !u->reinserted) {   /* removed node: the baseline HAD it — put it back */
            if (u->next) lxb_dom_node_insert_before(u->next, u->node);
            else if (u->parent) lxb_dom_node_insert_child(u->parent, u->node);
        }
    }
    g_dom_undo_n = 0;
    dom_unapply_seg(g_dom_base);   /* restore baseline through the (now head-reverted) base chain */
    dom_seg_unref(g_dom_base); g_dom_base = NULL;   /* drop this flow's reference; base freed iff no sibling holds it */
}
/* per-entry UNAPPLY (flow -> parked): stash the flow's value/taint into cur, restore the baseline. */
static void dom_unapply_entry(DomUndo *u) {
    if (u->kind == 0) {
        size_t vl = 0; const lxb_char_t *c = lxb_dom_element_get_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), &vl);
        free(u->cur); u->cur = NULL; u->cur_len = 0; u->cur_had = c ? 1 : 0;   /* stash the flow's attr value */
        if (c) { u->cur = malloc(vl ? vl : 1); CHECK(u->cur, "dom-cow-oom: parked flow attr snapshot malloc failed — apply would lose the flow's DOM write"); memcpy(u->cur, c, vl); u->cur_len = vl; }
        if (g_cow_ctx) JS_FreeValue(g_cow_ctx, u->sh_cur);
        u->sh_cur = shadow_snapshot(u->el, u->name, &u->sh_cur_had);   /* stash the flow's taint shadow */
        if (u->had && u->old) lxb_dom_element_set_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), u->old, u->old_len);
        else lxb_dom_element_remove_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name));
        shadow_restore(u->el, u->name, u->sh_old, u->sh_had);   /* restore the baseline taint */
    } else if (u->kind == 1 && !u->detached) {
        u->parent = lxb_dom_interface_node(u->node)->parent;              /* remember re-insert position */
        u->next = lxb_dom_interface_node(u->node)->next;
        lxb_dom_node_remove(u->node); u->detached = 1;
    } else if (u->kind == 2 && !u->reinserted) {
        /* the baseline HAD this node and this flow removed it: parking restores the baseline. */
        if (u->next) lxb_dom_node_insert_before(u->next, u->node);
        else if (u->parent) lxb_dom_node_insert_child(u->parent, u->node);
        u->reinserted = 1;
    }
}
/* per-entry APPLY (parked -> flow): restore the flow's value/taint over the baseline. */
static void dom_apply_entry(DomUndo *u) {
    if (u->kind == 0) {
        if (u->cur_had && u->cur) lxb_dom_element_set_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), u->cur, u->cur_len);
        else lxb_dom_element_remove_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name));
        shadow_restore(u->el, u->name, u->sh_cur, u->sh_cur_had);   /* restore the flow's taint */
    } else if (u->kind == 2 && u->reinserted) {
        /* resuming the flow: it had removed this node, so take it back out. */
        lxb_dom_node_remove(u->node); u->reinserted = 0;
    } else if (u->kind == 1 && u->detached) {
        if (u->next) lxb_dom_node_insert_before(u->next, u->node);
        else if (u->parent) lxb_dom_node_insert_child(u->parent, u->node);
        u->detached = 0;
    }
}
/* apply a base chain FORWARD (deepest ancestor first, then up); unapply is the mirror. NULL-safe (flat delta). */
static void dom_apply_seg(DomSeg *s) { if (!s) return; dom_apply_seg(s->base); for (int i = 0; i < s->n; i++) dom_apply_entry(&s->e[i]); }
static void dom_unapply_seg(DomSeg *s) { if (!s) return; for (int i = s->n - 1; i >= 0; i--) dom_unapply_entry(&s->e[i]); dom_unapply_seg(s->base); }
/* drop a chain reference: refcount--, free the segment's entries (parked: old/cur held) when it hits 0, recurse. */
static void dom_seg_unref(DomSeg *s) {
    while (s && --s->refcount <= 0) {
        DomSeg *base = s->base;
        for (int i = 0; i < s->n; i++) { DomUndo *u = &s->e[i]; free(u->name); free(u->old); free(u->cur);
            if (g_cow_ctx) { JS_FreeValue(g_cow_ctx, u->sh_old); JS_FreeValue(g_cow_ctx, u->sh_cur); } }
        free(s->e); free(s);
        s = base;
    }
}
/* UNAPPLY (flow -> parked): head reverse, then the shared base chain. Restores the baseline DOM+taint. */
void dom_unapply(void) {
    for (int i = g_dom_undo_n - 1; i >= 0; i--) dom_unapply_entry(&g_dom_undo[i]);
    dom_unapply_seg(g_dom_base);   /* NULL until a fork shares a base -> no-op (flat behavior) */
}
/* APPLY (parked -> flow): base chain forward (deepest first), then the head on top. */
void dom_apply(void) {
    dom_apply_seg(g_dom_base);
    for (int i = 0; i < g_dom_undo_n; i++) dom_apply_entry(&g_dom_undo[i]);
}
/* FORK the DOM delta: freeze the running flow's HEAD into a shared immutable base segment that BOTH the running
   flow and a snapshot-forked sibling reference (refcount 2) — the sibling SHARES the parent's O(N) DOM delta
   instead of copying it. Head is applied, so UNAPPLY it to parked state (values -> cur, DOM -> baseline), freeze
   that, then RE-APPLY so the running flow continues byte-identically. Capture is suspended across the round-trip
   (defensive symmetry with the heap fork — the internal set/remove goes straight to Lexbor, not the capturing
   host-edge, so no re-capture, but the guard documents+enforces the invariant). Returns the shared base. */
void *dom_cow_fork(void) {
    int sv = g_dom_capture; g_dom_capture = 0;
    for (int i = g_dom_undo_n - 1; i >= 0; i--) dom_unapply_entry(&g_dom_undo[i]);
    DomSeg *seg = malloc(sizeof(DomSeg));
    CHECK(seg, "dom-cow-oom: fork segment alloc failed — a shared DOM delta would be corrupted");
    seg->e = g_dom_undo; seg->n = g_dom_undo_n; seg->base = g_dom_base; seg->refcount = 2;   /* running flow + sibling */
    g_dom_undo = NULL; g_dom_undo_n = 0; g_dom_undo_cap = 0;   /* fresh empty head for the running flow */
    g_dom_base = seg;
    for (int i = 0; i < seg->n; i++) dom_apply_entry(&seg->e[i]);   /* re-apply head -> running flow continues */
    g_dom_capture = sv;
    return seg;
}
/* Take / install the shared BASE chain alongside the head (a flow's full DOM delta is head + base chain). */
void *dom_base_take(void) { void *b = g_dom_base; g_dom_base = NULL; return b; }
void dom_base_load(void *base) { g_dom_base = (DomSeg *)base; }
void dom_base_free(void *base) { if (base) dom_seg_unref((DomSeg *)base); }
void dom_base_ref(void *base) { if (base) ((DomSeg *)base)->refcount++; }   /* each orphan forks the document flow's shared DOM delta */
void dom_buf_free(void *buf, int n) {   /* free a parked DOM delta buffer (its nodes stay detached, owned by the doc) */
    DomUndo *b = (DomUndo *)buf;
    for (int i = 0; i < n; i++) {
        free(b[i].name); free(b[i].old); free(b[i].cur);
        if (g_cow_ctx) { JS_FreeValue(g_cow_ctx, b[i].sh_old); JS_FreeValue(g_cow_ctx, b[i].sh_cur); }
    }
    free(b);
}
/* dom_buf_snapshot (the O(delta) selective COPY of a continuation's DOM attribute delta) is DELETED: its sole
   caller (flow_defer_callback) now SHARES the delta via dom_cow_fork's refcounted immutable base segment —
   O(1), and it carries the inserted-node (kind-1) entries the copy dropped as pointer-fragile. */
void *dom_buf_take(int *n, int *cap) { void *b = g_dom_undo; *n = g_dom_undo_n; *cap = g_dom_undo_cap; g_dom_undo = NULL; g_dom_undo_n = 0; g_dom_undo_cap = 0; return b; }
void dom_buf_load(void *buf, int n, int cap) { g_dom_undo = (DomUndo *)buf; g_dom_undo_n = n; g_dom_undo_cap = cap; }
