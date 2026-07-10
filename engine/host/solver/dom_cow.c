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
    lxb_dom_node_t *node, *parent, *next;                     /* kind 1: inserted node + detach position */
    int detached;                                             /* kind 1: currently detached (unapplied) */
} DomUndo;

static DomUndo *g_dom_undo = NULL;
static int g_dom_undo_n = 0, g_dom_undo_cap = 0;
int g_dom_capture = 0;
static JSContext *g_cow_ctx = NULL;   /* for the shadow's JSValue dup/free (set at init) */

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
        }
    }
    g_dom_undo_n = 0;
}
/* UNAPPLY (flow -> parked): save the flow's DOM value + taint shadow, restore the baseline, so the next flow
   sees the baseline DOM AND baseline taint. Reverse order. */
void dom_unapply(void) {
    for (int i = g_dom_undo_n - 1; i >= 0; i--) {
        DomUndo *u = &g_dom_undo[i];
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
        }
    }
}
/* APPLY (parked -> flow): restore the flow's DOM value + taint shadow over the baseline. Forward order. */
void dom_apply(void) {
    for (int i = 0; i < g_dom_undo_n; i++) {
        DomUndo *u = &g_dom_undo[i];
        if (u->kind == 0) {
            if (u->cur_had && u->cur) lxb_dom_element_set_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), u->cur, u->cur_len);
            else lxb_dom_element_remove_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name));
            shadow_restore(u->el, u->name, u->sh_cur, u->sh_cur_had);   /* restore the flow's taint */
        } else if (u->kind == 1 && u->detached) {
            if (u->next) lxb_dom_node_insert_before(u->next, u->node);
            else if (u->parent) lxb_dom_node_insert_child(u->parent, u->node);
            u->detached = 0;
        }
    }
}
void dom_buf_free(void *buf, int n) {   /* free a parked DOM delta buffer (its nodes stay detached, owned by the doc) */
    DomUndo *b = (DomUndo *)buf;
    for (int i = 0; i < n; i++) {
        free(b[i].name); free(b[i].old); free(b[i].cur);
        if (g_cow_ctx) { JS_FreeValue(g_cow_ctx, b[i].sh_old); JS_FreeValue(g_cow_ctx, b[i].sh_cur); }
    }
    free(b);
}
/* SNAPSHOT the applied DOM delta's ATTRIBUTE mutations into a detached buffer whose dom_apply (by a deferred
   CONTINUATION flow) re-establishes the current live attribute values + taint — so a callback deferred from a
   handler inherits the handler's attribute writes (el.dataset.x = tainted; setTimeout(()=>read el.dataset.x)).
   Only kind-0 (attribute) entries are copied: they are (el,name) identity-based and pointer-safe. kind-1
   (inserted node) entries are SKIPPED — sharing a node pointer between the scheduling flow's revert (which
   detaches it) and the deferred flow's apply (which re-inserts it) is fragile, and a deferred query for a
   handler-inserted node is the rarer case; leaving it means the deferred flow simply doesn't see the node
   (no regression), never corruption. The scheduling flow is untouched. Mirrors JS_CowBufSnapshot. */
void *dom_buf_snapshot(int *out_n, int *out_cap) {
    *out_n = 0; *out_cap = 0;
    if (g_dom_undo_n == 0) return NULL;
    DomUndo *cp = malloc((size_t)g_dom_undo_n * sizeof(DomUndo));
    CHECK(cp, "dom-cow-oom: snapshot buffer malloc failed");
    int m = 0;
    for (int i = 0; i < g_dom_undo_n; i++) {
        DomUndo *u = &g_dom_undo[i];
        if (u->kind != 0) continue;
        DomUndo d; memset(&d, 0, sizeof d);
        d.kind = 0; d.el = u->el; d.name = strdup(u->name); CHECK(d.name, "dom-cow-oom: snapshot attr name strdup");
        d.had = u->had;                                          /* baseline value copied from the scheduling entry */
        if (u->had && u->old) { d.old = malloc(u->old_len ? u->old_len : 1); CHECK(d.old, "dom-cow-oom: snapshot baseline attr malloc"); memcpy(d.old, u->old, u->old_len); d.old_len = u->old_len; }
        d.sh_old = (g_cow_ctx && u->sh_had) ? JS_DupValue(g_cow_ctx, u->sh_old) : JS_UNDEFINED; d.sh_had = u->sh_had;
        size_t vl = 0; const lxb_char_t *c = lxb_dom_element_get_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), &vl);
        d.cur_had = c ? 1 : 0;                                   /* current live flow value -> what the deferred flow applies */
        if (c) { d.cur = malloc(vl ? vl : 1); CHECK(d.cur, "dom-cow-oom: snapshot flow attr malloc"); memcpy(d.cur, c, vl); d.cur_len = vl; }
        d.sh_cur = shadow_snapshot(u->el, u->name, &d.sh_cur_had);
        cp[m++] = d;
    }
    if (m == 0) { free(cp); return NULL; }
    *out_n = m; *out_cap = g_dom_undo_n;
    return cp;
}
void *dom_buf_take(int *n, int *cap) { void *b = g_dom_undo; *n = g_dom_undo_n; *cap = g_dom_undo_cap; g_dom_undo = NULL; g_dom_undo_n = 0; g_dom_undo_cap = 0; return b; }
void dom_buf_load(void *buf, int n, int cap) { g_dom_undo = (DomUndo *)buf; g_dom_undo_n = n; g_dom_undo_cap = cap; }
