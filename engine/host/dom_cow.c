/* Per-flow DOM COW delta — see dom_cow.h. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "dom_cow.h"
#include <lexbor/dom/dom.h>

typedef struct DomUndo {
    int kind; lxb_dom_element_t *el; char *name;
    lxb_char_t *old; size_t old_len; int had;                 /* kind 0: baseline attr */
    lxb_char_t *cur; size_t cur_len; int cur_had;             /* kind 0: flow's attr (valid while parked) */
    lxb_dom_node_t *node, *parent, *next;                     /* kind 1: inserted node + detach position */
    int detached;                                             /* kind 1: currently detached (unapplied) */
} DomUndo;

static DomUndo *g_dom_undo = NULL;
static int g_dom_undo_n = 0, g_dom_undo_cap = 0;
int g_dom_capture = 0;

static void dom_undo_push(DomUndo u) {
    if (g_dom_undo_n >= g_dom_undo_cap) {
        int nc = g_dom_undo_cap ? g_dom_undo_cap * 2 : 64;
        /* Skipping a DOM capture silently breaks DOM isolation (this flow's DOM write never reverts -> leaks
           into the next flow's baseline -> wrong sinks/taint). OOM is a should-never-happen: CRASH, never skip. */
        DomUndo *n = realloc(g_dom_undo, (size_t)nc * sizeof(DomUndo));
        if (!n) { fflush(stdout); fprintf(stderr, "@E {\"phase\":\"dom-cow-oom\",\"reason\":\"DOM undo-log realloc failed — DOM isolation would be silently corrupted\"}\n"); fflush(stderr); abort(); }
        g_dom_undo = n; g_dom_undo_cap = nc;
    }
    g_dom_undo[g_dom_undo_n++] = u;
}
void dom_attr_capture(lxb_dom_element_t *el, const char *name) {
    if (!g_dom_capture) return;
    size_t vl = 0; const lxb_char_t *cur = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &vl);
    DomUndo u; memset(&u, 0, sizeof u);
    u.kind = 0; u.el = el; u.name = strdup(name); u.had = cur ? 1 : 0;
    if (cur) { u.old = malloc(vl ? vl : 1); if (u.old) { memcpy(u.old, cur, vl); u.old_len = vl; } }
    dom_undo_push(u);
}
void dom_insert_capture(lxb_dom_node_t *node) {
    if (!g_dom_capture) return;
    DomUndo u; memset(&u, 0, sizeof u); u.kind = 1; u.node = node; dom_undo_push(u);
}
void dom_revert(void) {   /* DISCARD the running flow's DOM writes -> baseline (reverse order); empties the delta */
    for (int i = g_dom_undo_n - 1; i >= 0; i--) {
        DomUndo *u = &g_dom_undo[i];
        if (u->kind == 0) {   /* attribute: restore old value, or remove if it didn't exist */
            if (u->had && u->old) lxb_dom_element_set_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), u->old, u->old_len);
            else lxb_dom_element_remove_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name));
            free(u->name); free(u->old); free(u->cur);
        } else if (u->kind == 1 && !u->detached) {   /* inserted node: detach it (baseline had none) */
            lxb_dom_node_remove(u->node);
        }
    }
    g_dom_undo_n = 0;
}
/* UNAPPLY (flow -> parked): save the flow's DOM values, restore the baseline, so the next flow sees the
   baseline DOM. Reverse order. */
void dom_unapply(void) {
    for (int i = g_dom_undo_n - 1; i >= 0; i--) {
        DomUndo *u = &g_dom_undo[i];
        if (u->kind == 0) {
            size_t vl = 0; const lxb_char_t *c = lxb_dom_element_get_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), &vl);
            free(u->cur); u->cur = NULL; u->cur_len = 0; u->cur_had = c ? 1 : 0;   /* stash the flow's attr */
            if (c) { u->cur = malloc(vl ? vl : 1); if (u->cur) { memcpy(u->cur, c, vl); u->cur_len = vl; } }
            if (u->had && u->old) lxb_dom_element_set_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), u->old, u->old_len);
            else lxb_dom_element_remove_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name));
        } else if (u->kind == 1 && !u->detached) {
            u->parent = lxb_dom_interface_node(u->node)->parent;              /* remember re-insert position */
            u->next = lxb_dom_interface_node(u->node)->next;
            lxb_dom_node_remove(u->node); u->detached = 1;
        }
    }
}
/* APPLY (parked -> flow): restore the flow's DOM values over the baseline. Forward order. */
void dom_apply(void) {
    for (int i = 0; i < g_dom_undo_n; i++) {
        DomUndo *u = &g_dom_undo[i];
        if (u->kind == 0) {
            if (u->cur_had && u->cur) lxb_dom_element_set_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), u->cur, u->cur_len);
            else lxb_dom_element_remove_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name));
        } else if (u->kind == 1 && u->detached) {
            if (u->next) lxb_dom_node_insert_before(u->next, u->node);
            else if (u->parent) lxb_dom_node_insert_child(u->parent, u->node);
            u->detached = 0;
        }
    }
}
void dom_buf_free(void *buf, int n) {   /* free a parked DOM delta buffer (its nodes stay detached, owned by the doc) */
    DomUndo *b = (DomUndo *)buf;
    for (int i = 0; i < n; i++) { free(b[i].name); free(b[i].old); free(b[i].cur); }
    free(b);
}
void *dom_buf_take(int *n, int *cap) { void *b = g_dom_undo; *n = g_dom_undo_n; *cap = g_dom_undo_cap; g_dom_undo = NULL; g_dom_undo_n = 0; g_dom_undo_cap = 0; return b; }
void dom_buf_load(void *buf, int n, int cap) { g_dom_undo = (DomUndo *)buf; g_dom_undo_n = n; g_dom_undo_cap = cap; }
