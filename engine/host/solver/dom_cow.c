/* Per-flow DOM COW delta — see dom_cow.h. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "check.h"        /* CHECK — an OOM here corrupts DOM isolation, fatal in every build */
#include "solver/dom_cow.h"
#include "solver/attr_shadow.h"   /* the taint shadow rides the attribute delta (per-flow isolation of stashed taint) */
#include <lexbor/dom/dom.h>

typedef struct DomUndo {
    /* kind 0 covers a NAMED STRING SLOT on an element, and `slot` says which namespace it is in:
       ATTR_SLOT_ATTRIBUTE (a content attribute, whose VALUE lives in the Lexbor tree and whose TAINT lives in
       the shadow) or ATTR_SLOT_PROPERTY (a DOM property like textContent, whose value is already captured as
       Text NODES by kinds 1/2, so only its taint is here). One entry kind because it is one concept — without
       it a `el.textContent = location.hash` in one arm was visible to every other arm, since the shadow write
       bypassed the delta entirely. */
    int kind; int slot; lxb_dom_element_t *el; char *name;
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

/* THE CHAIN THAT IS CURRENTLY APPLIED TO THE DOCUMENT, and the whole reason a switch is not O(delta).
 *
 * The segments are already immutable, refcounted and structurally SHARED — two flows that forked from a common
 * point hold the same suffix by POINTER — and the swap threw all of that away: it unapplied the outgoing flow's
 * entire chain and applied the incoming flow's entire chain, redoing by hand the part they agree on. For a flow
 * that had appended three hundred nodes that is six hundred node detach/re-attach operations per switch, and
 * the scheduler switches often, so the cost of one scheduling DECISION grew with the size of the document —
 * exactly backwards for an engine whose whole point is interleaving flows over a real page.
 *
 * What is applied is a property of the DOCUMENT, not of any flow, so it is tracked here: everything from
 * `g_dom_installed` downward is live in the tree. A switch moves that pointer to the incoming flow's chain by
 * walking to the two chains' lowest common segment and touching only what lies above it. The shared suffix is
 * never unapplied and never re-applied, because both flows agree about it by construction — a segment is frozen
 * at a fork and immutable after, so any two holders read the same values from it.
 * The HEAD is different and is always swapped: it is mutable and private to one flow. */
static DomSeg *g_dom_installed = NULL;
static void dom_unapply_seg(DomSeg *s);   /* fwd: dom_revert (defined earlier) walks the base chain */
static void dom_seg_unref(DomSeg *s);

void dom_cow_set_ctx(JSContext *ctx) { g_cow_ctx = ctx; }

/* §4.2.3's insertion/removing steps. Fired from the chokepoint so a tree write cannot reach the tree without
   them: the browser layer registers what they MEAN and this file guarantees they run. */
static void (*g_tree_hook)(JSContext *ctx, lxb_dom_node_t *n, int inserted);
void dom_cow_set_tree_hook(void (*fn)(JSContext *ctx, lxb_dom_node_t *n, int inserted)) { g_tree_hook = fn; }

static void (*g_attr_hook)(JSContext *ctx, lxb_dom_element_t *el, const char *name,
                           const char *val, size_t val_len);
void dom_cow_set_attr_hook(void (*fn)(JSContext *ctx, lxb_dom_element_t *el, const char *name,
                                      const char *val, size_t val_len)) { g_attr_hook = fn; }

/* the current taint shadow for (el,name), dup'd (JS_UNDEFINED + *had=0 if none) */
static JSValue shadow_snapshot(lxb_dom_element_t *el, int slot, const char *name, int *had) {
    int si = attr_shadow_find(el, slot, name);
    *had = (si >= 0);
    return (si >= 0 && g_cow_ctx) ? JS_DupValue(g_cow_ctx, attr_shadow_opaque(si)) : JS_UNDEFINED;
}
/* set (el,name)'s taint shadow to `v` (borrowed; attr_shadow_set dups it), or clear it when !had */
static void shadow_restore(lxb_dom_element_t *el, int slot, const char *name, JSValueConst v, int had) {
    if (g_cow_ctx) attr_shadow_set(g_cow_ctx, el, slot, name, had ? v : JS_UNDEFINED);
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
    u.kind = 0; u.slot = ATTR_SLOT_ATTRIBUTE; u.el = el; u.name = strdup(name); u.had = cur ? 1 : 0;
    CHECK(u.name, "dom-cow-oom: attr name strdup failed");
    if (cur) { u.old = malloc(vl ? vl : 1); CHECK(u.old, "dom-cow-oom: baseline attr snapshot malloc failed — the delta could not restore its baseline"); memcpy(u.old, cur, vl); u.old_len = vl; }
    u.sh_old = shadow_snapshot(el, ATTR_SLOT_ATTRIBUTE, name, &u.sh_had);   /* baseline TAINT reverts with the value */
    u.sh_cur = JS_UNDEFINED;
    dom_undo_push(u);
}

/* The PROPERTY-slot twin of dom_attr_capture: a DOM property's taint, whose value half is already captured as
   Text nodes. Same entry kind, same revert/unapply/apply arms — only the Lexbor value work is skipped. */
static void dom_prop_taint_capture(lxb_dom_element_t *el, const char *name) {
    if (!g_dom_capture) return;
    DomUndo u; memset(&u, 0, sizeof u);
    u.kind = 0; u.slot = ATTR_SLOT_PROPERTY; u.el = el; u.name = strdup(name); u.had = 0;
    CHECK(u.name, "dom-cow-oom: property name strdup failed");
    u.sh_old = shadow_snapshot(el, ATTR_SLOT_PROPERTY, name, &u.sh_had);
    u.sh_cur = JS_UNDEFINED;
    dom_undo_push(u);
}

/* THE PROPERTY-TAINT CHOKEPOINT — capture-then-set, like every other DOM write. `opaque` JS_UNDEFINED clears. */
void dom_cow_set_prop_taint(JSContext *ctx, lxb_dom_element_t *el, const char *name, JSValueConst opaque) {
    dom_prop_taint_capture(el, name);
    attr_shadow_set(ctx, el, ATTR_SLOT_PROPERTY, name, opaque);
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
    /* BEFORE the write: the change steps take the OLD value, which the element still holds only until now. */
    if (g_attr_hook && g_cow_ctx) g_attr_hook(g_cow_ctx, el, name, val, val_len);
    lxb_dom_element_set_attribute(el, (const lxb_char_t *)name, strlen(name), (const lxb_char_t *)val, val_len);
}
/* Remove an ATTRIBUTE the baseline may own — the fourth thing a flow can change about the tree, and the one
   that had no chokepoint, so a boolean reflection (`el.hidden = false`, `script.async = false`) had no way to
   unset itself without going around the per-flow delta. Same capture-then-mutate order as the setter. */
void dom_cow_remove_attribute(lxb_dom_element_t *el, const char *name) {
    dom_attr_capture(el, name);
    if (g_attr_hook && g_cow_ctx) g_attr_hook(g_cow_ctx, el, name, NULL, 0);
    lxb_dom_element_remove_attribute(el, (const lxb_char_t *)name, strlen(name));
}

/* Remove a node that the baseline may own. Capture its position FIRST, then detach — the same order the
   attribute and insert chokepoints use, so a removal cannot bypass the per-flow delta either. */
void dom_cow_remove_child(lxb_dom_node_t *node) {
    if (!node) return;
    /* BEFORE the detach, because "was it connected" has no answer afterwards. */
    if (g_tree_hook && g_cow_ctx) g_tree_hook(g_cow_ctx, node, 0);
    if (g_dom_capture) {
        DomUndo u; memset(&u, 0, sizeof u);
        u.kind = 2; u.node = node;
        u.parent = node->parent; u.next = node->next;
        u.sh_old = u.sh_cur = JS_UNDEFINED;
        dom_undo_push(u);
    }
    lxb_dom_node_remove(node);
}

/* A CHARACTER-DATA node's VALUE (§4.10 `data`). The third thing a flow can change about the tree, after an
   attribute and a node's presence: `text.data = x` mutates bytes the baseline owns, in place, on a node whose
   IDENTITY must survive the write — so it cannot be modelled as a remove+insert of a replacement node the way
   textContent legitimately is. Same shape as the attribute entry, over the node instead of the element. */
void dom_cow_set_text(lxb_dom_node_t *node, const char *val, size_t val_len) {
    lxb_dom_character_data_t *cd;
    if (!node) return;
    DCHECK(node->type == LXB_DOM_NODE_TYPE_TEXT || node->type == LXB_DOM_NODE_TYPE_COMMENT,
           "dom_cow_set_text on a node that holds no character data");
    cd = lxb_dom_interface_character_data(node);
    if (g_dom_capture) {
        DomUndo u; memset(&u, 0, sizeof u);
        u.kind = 3; u.node = node; u.sh_old = u.sh_cur = JS_UNDEFINED;
        u.had = 1; u.old_len = cd->data.length;
        u.old = malloc(u.old_len ? u.old_len : 1);
        CHECK(u.old != NULL, "dom-cow-oom: the character-data baseline snapshot failed — unapply would lose the "
                             "text the baseline had");
        memcpy(u.old, cd->data.data, u.old_len);
        dom_undo_push(u);
    }
    lxb_dom_character_data_replace(cd, (const lxb_char_t *)val, val_len, 0, cd->data.length);
}

void dom_cow_append_child(lxb_dom_node_t *parent, lxb_dom_node_t *child) {
    dom_insert_capture(child);   /* record the insertion FIRST so it reverts per-flow (detached on unapply) */
    lxb_dom_node_insert_child(parent, child);
    if (g_tree_hook && g_cow_ctx) g_tree_hook(g_cow_ctx, child, 1);   /* AFTER: connectedness is the new tree's */
}
/* §4.2.3 "insert before": the same capture, at a POSITION. The insert entry remembers where it landed at
   unapply time rather than at capture time, so this differs from append only in the Lexbor call — which is
   exactly why insertBefore must come through here and not reach lxb_dom_node_insert_before directly. */
void dom_cow_insert_before(lxb_dom_node_t *ref, lxb_dom_node_t *child) {
    dom_insert_capture(child);
    lxb_dom_node_insert_before(ref, child);
    if (g_tree_hook && g_cow_ctx) g_tree_hook(g_cow_ctx, child, 1);
}
void dom_revert(void) {   /* DISCARD the running flow's DOM writes -> baseline (reverse order); empties the delta */
    for (int i = g_dom_undo_n - 1; i >= 0; i--) {
        DomUndo *u = &g_dom_undo[i];
        if (u->kind == 0) {   /* named slot: restore old value (attributes only) + old taint shadow */
            if (u->slot == ATTR_SLOT_ATTRIBUTE) {
                if (u->had && u->old) lxb_dom_element_set_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), u->old, u->old_len);
                else lxb_dom_element_remove_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name));
            }
            shadow_restore(u->el, u->slot, u->name, u->sh_old, u->sh_had);
            if (g_cow_ctx) { JS_FreeValue(g_cow_ctx, u->sh_old); JS_FreeValue(g_cow_ctx, u->sh_cur); }
            free(u->name); free(u->old); free(u->cur);
        } else if (u->kind == 3) {   /* character data: put the baseline's text back */
            lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(u->node);
            lxb_dom_character_data_replace(cd, u->old, u->old_len, 0, cd->data.length);
            free(u->old); free(u->cur);
        } else if (u->kind == 1 && !u->detached) {   /* inserted node: detach it (baseline had none) */
            lxb_dom_node_remove(u->node);
        } else if (u->kind == 2 && !u->reinserted) {   /* removed node: the baseline HAD it — put it back */
            if (u->next) lxb_dom_node_insert_before(u->next, u->node);
            else if (u->parent) lxb_dom_node_insert_child(u->parent, u->node);
        }
    }
    g_dom_undo_n = 0;
    /* A DISCARD, so the document goes all the way back to the baseline and NOTHING stays installed — the
       segments this flow held may be freed below, and an installed pointer into freed memory is what the next
       switch would walk. */
    dom_unapply_seg(g_dom_installed);
    g_dom_installed = NULL;
    dom_seg_unref(g_dom_base); g_dom_base = NULL;   /* drop this flow's reference; base freed iff no sibling holds it */
}
/* per-entry UNAPPLY (flow -> parked): stash the flow's value/taint into cur, restore the baseline. */
static void dom_unapply_entry(DomUndo *u) {
    if (u->kind == 0) {
        if (u->slot == ATTR_SLOT_ATTRIBUTE) {
            size_t vl = 0; const lxb_char_t *c = lxb_dom_element_get_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), &vl);
            free(u->cur); u->cur = NULL; u->cur_len = 0; u->cur_had = c ? 1 : 0;   /* stash the flow's attr value */
            if (c) { u->cur = malloc(vl ? vl : 1); CHECK(u->cur, "dom-cow-oom: parked flow attr snapshot malloc failed — apply would lose the flow's DOM write"); memcpy(u->cur, c, vl); u->cur_len = vl; }
        }
        if (g_cow_ctx) JS_FreeValue(g_cow_ctx, u->sh_cur);
        u->sh_cur = shadow_snapshot(u->el, u->slot, u->name, &u->sh_cur_had);   /* stash the flow's taint shadow */
        if (u->slot == ATTR_SLOT_ATTRIBUTE) {
            if (u->had && u->old) lxb_dom_element_set_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), u->old, u->old_len);
            else lxb_dom_element_remove_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name));
        }
        shadow_restore(u->el, u->slot, u->name, u->sh_old, u->sh_had);   /* restore the baseline taint */
    } else if (u->kind == 3) {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(u->node);
        free(u->cur); u->cur_len = cd->data.length; u->cur_had = 1;       /* stash the flow's text */
        u->cur = malloc(u->cur_len ? u->cur_len : 1);
        CHECK(u->cur != NULL, "dom-cow-oom: the parked character-data snapshot failed — apply would lose the "
                              "flow's text write");
        memcpy(u->cur, cd->data.data, u->cur_len);
        lxb_dom_character_data_replace(cd, u->old, u->old_len, 0, cd->data.length);   /* baseline back */
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
        if (u->slot == ATTR_SLOT_ATTRIBUTE) {
            if (u->cur_had && u->cur) lxb_dom_element_set_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), u->cur, u->cur_len);
            else lxb_dom_element_remove_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name));
        }
        shadow_restore(u->el, u->slot, u->name, u->sh_cur, u->sh_cur_had);   /* restore the flow's taint */
    } else if (u->kind == 3 && u->cur_had) {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(u->node);
        lxb_dom_character_data_replace(cd, u->cur, u->cur_len, 0, cd->data.length);   /* the flow's text back */
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
/* THE BASE CHAIN IS WALKED, NOT RECURSED. These two ran on every context switch, once per shared base segment,
   and the chain's depth is the fork depth — so a deeply forked frontier put an unbounded C recursion on the
   hottest path the scheduler has. All recursion is banned here for one reason: C stack cannot be suspended,
   parked or resumed, and a switch that cannot be interrupted mid-way is a switch that cannot time-travel. That
   neither of these runs any of the page's code makes no difference.
   UNAPPLY is head-first, so it is a plain loop. APPLY is deepest-ancestor-first, which the recursion got by
   unwinding; it gets it here by REVERSING the base pointers in place, walking forward, and reversing them back
   — O(depth), no allocation, and nothing to fail on a path where a failed allocation would corrupt the swap. */
/* How deep a chain is — the number of FORKS behind it, never the number of entries. Both walks below are
   O(depth) for that reason, and depth is the shape of the frontier rather than the size of the page. */
static int dom_seg_depth(DomSeg *s) { int d = 0; for (; s; s = s->base) d++; return d; }

/* The deepest segment BOTH chains hold. Pointer identity is the whole test: a segment is frozen once and never
   written again, so two chains that reach the same pointer agree about everything from there down. */
static DomSeg *dom_seg_common(DomSeg *a, DomSeg *b)
{
    int da = dom_seg_depth(a), db = dom_seg_depth(b);
    while (da > db) { a = a->base; da--; }
    while (db > da) { b = b->base; db--; }
    while (a != b) { a = a->base; b = b->base; }
    return a;
}

/* Apply `s` down to (not including) `stop`, deepest-first. `stop` stands in for NULL as the end of the part
   being reversed, so the shared suffix below it is not touched at all — not read, not written, not walked.
   UNAPPLY is head-first, so it is a plain loop. APPLY is deepest-ancestor-first, which a recursion would get by
   unwinding; it gets it here by REVERSING the base pointers in place, walking forward, and reversing them back
   — O(depth), no allocation, and nothing to fail on a path where a failed allocation would corrupt the swap.
   All recursion is banned here for one reason: C stack cannot be suspended, parked or resumed, and a switch
   that cannot be interrupted mid-way is a switch that cannot time-travel. */
static void dom_apply_seg_until(DomSeg *s, DomSeg *stop)
{
    DomSeg *prev = stop, *cur = s, *next;

    while (cur != stop) { next = cur->base; cur->base = prev; prev = cur; cur = next; }
    cur = prev; prev = stop;
    while (cur != stop) {
        for (int i = 0; i < cur->n; i++) dom_apply_entry(&cur->e[i]);
        next = cur->base; cur->base = prev; prev = cur; cur = next;
    }
}

/* Make `want` the installed chain: the ONE place the document's applied state changes, and the reason a switch
   between two related flows costs what they DIVERGED by rather than everything either of them has done. */
static void dom_install_chain(DomSeg *want)
{
    DomSeg *common = dom_seg_common(g_dom_installed, want);
    DomSeg *s;

    for (s = g_dom_installed; s != common; s = s->base)      /* head-first, as unapply always is */
        for (int i = s->n - 1; i >= 0; i--) dom_unapply_entry(&s->e[i]);
    dom_apply_seg_until(want, common);
    g_dom_installed = want;
}

static void dom_unapply_seg(DomSeg *s)
{
    for (; s; s = s->base)
        for (int i = s->n - 1; i >= 0; i--) dom_unapply_entry(&s->e[i]);
}
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
    /* ONLY THE HEAD. The base chain stays applied and `g_dom_installed` says so — the incoming flow's apply
       decides how much of it actually has to move, which for a sibling is none of it. */
    for (int i = g_dom_undo_n - 1; i >= 0; i--) dom_unapply_entry(&g_dom_undo[i]);
    DCHECK(g_dom_installed == g_dom_base,
           "the applied chain is not the running flow's — a base was loaded or taken without going through "
           "dom_apply, so the document is showing a chain nobody is running");
}
/* APPLY (parked -> flow): base chain forward (deepest first), then the head on top. */
void dom_apply(void) {
    dom_install_chain(g_dom_base);
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
    g_dom_installed = seg;   /* the frozen head belongs to the applied chain now, not to anyone's head */
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
