/* css-shadow-1 §4.1's flat tree (see flat_tree.h). §4.1's construction, for one pending node: "pending node is a
   shadow host: Append the child nodes of the shadow root of the shadow tree it hosts"; "pending node is a slot:
   Find slottables for pending node, and append them … If no slottables were found for pending node, instead
   append its children"; "Otherwise, Append the child nodes of pending node's light tree". The slot arm reads the
   assigned nodes DOM §4.2.2.4's "assign slottables" stored, which is "find slottables" run at the last
   mutation that could change it. */
#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/dom/document.h"
#include "core/dom/flat_tree.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/dom/slot.h"

/* The realm `n`'s wrappers live in: slot assignment and the shadow root association are per-flow facts stored
   there. A node layout reaches is in a document a realm was built for. */
static JSContext *ft_realm(const lxb_dom_node_t *n)
{
    JSContext *ctx = document_realm_of(n);

    DCHECK(ctx != NULL, "the flat tree was walked over a node whose document has no realm to read slot "
                        "assignment and shadow roots in");
    return ctx;
}

/* The shadow root `n` hosts, or NULL. A host always has a wrapper (shadow_root.h), so a node with none is no
   host, which keeps a document no realm was built for (a scratch parse) out of the realm lookup. */
static lxb_dom_node_t *ft_shadow_root(const lxb_dom_node_t *n)
{
    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || !JS_IsObject(node_wrap_peek(n))) return NULL;
    return shadow_root_of_element(ft_realm(n), lxb_dom_interface_element((lxb_dom_node_t *)n));
}

/* Whether `slot` takes its assigned nodes as its flat-tree children (§4.1's slot arm, slottables found). */
static bool ft_slot_assigned(const lxb_dom_node_t *n)
{
    return slot_is(n) && JS_IsObject(node_wrap_peek(n)) && slot_assigned_count(ft_realm(n), n) != 0;
}

/* `n`'s index in `slot`'s assigned nodes; `n` is assigned to it (asserted). */
static size_t ft_assigned_index(JSContext *ctx, const lxb_dom_node_t *slot, const lxb_dom_node_t *n)
{
    size_t i, count = slot_assigned_count(ctx, slot);

    for (i = 0; i < count; i++)
        if (slot_assigned_at(ctx, slot, i) == n) return i;
    DFAIL("a node's assigned slot does not hold it among its assigned nodes; DOM §4.2.2.4's assign slottables "
          "writes both sides at once");
    return 0;
}

lxb_dom_node_t *flat_tree_parent(const lxb_dom_node_t *n)
{
    lxb_dom_node_t *p, *slot;

    DCHECK(n != NULL && !shadow_root_is(n), "the flat-tree parent was asked of no node or of a shadow root, "
                                            "which css-shadow-1 §4.1 never inserts into the flat tree");
    p = n->parent;
    if (p == NULL) return NULL;
    if (shadow_root_is(p)) return lxb_dom_interface_node(shadow_root_host(p));
    if (ft_shadow_root(p) != NULL) {
        /* A host's light child is in the flat tree only as a slot's assigned node. */
        slot = slot_assigned_slot(ft_realm(n), n);
        return slot;
    }
    /* A slot's own children are its flat-tree children only when nothing is assigned to it. */
    if (ft_slot_assigned(p)) return NULL;
    return p;
}

lxb_dom_node_t *flat_tree_first_child(const lxb_dom_node_t *n)
{
    lxb_dom_node_t *sr;

    DCHECK(n != NULL, "the flat-tree children were asked of no node");
    sr = ft_shadow_root(n);
    if (sr != NULL) return sr->first_child;
    if (ft_slot_assigned(n)) return slot_assigned_at(ft_realm(n), n, 0);
    return n->first_child;
}

lxb_dom_node_t *flat_tree_last_child(const lxb_dom_node_t *n)
{
    lxb_dom_node_t *sr;

    DCHECK(n != NULL, "the flat-tree children were asked of no node");
    sr = ft_shadow_root(n);
    if (sr != NULL) return sr->last_child;
    if (ft_slot_assigned(n)) {
        JSContext *ctx = ft_realm(n);

        return slot_assigned_at(ctx, n, slot_assigned_count(ctx, n) - 1);
    }
    return n->last_child;
}

/* The sibling `step` places from `n` (+1 next, -1 previous) under its flat-tree parent. Only a slot's assigned
   nodes are ordered by something other than the DOM: a shadow root's children are its host's flat children in
   their own order, and every other parent's are its light children. */
static lxb_dom_node_t *ft_sibling(const lxb_dom_node_t *n, int step)
{
    lxb_dom_node_t *p = flat_tree_parent(n);
    JSContext *ctx;
    size_t i, count;

    DCHECK(p != NULL, "a flat-tree sibling was asked of the root or of a node in no flat tree");
    if (n->parent == p || shadow_root_is(n->parent)) return step > 0 ? n->next : n->prev;
    ctx = ft_realm(n);
    DCHECK(slot_is(p), "a node whose flat-tree parent is neither its DOM parent nor its shadow root's host is "
                       "not an assigned node of a slot");
    i = ft_assigned_index(ctx, p, n);
    count = slot_assigned_count(ctx, p);
    if (step > 0) return i + 1 < count ? slot_assigned_at(ctx, p, i + 1) : NULL;
    return i > 0 ? slot_assigned_at(ctx, p, i - 1) : NULL;
}

lxb_dom_node_t *flat_tree_next_sibling(const lxb_dom_node_t *n)
{
    return ft_sibling(n, 1);
}

lxb_dom_node_t *flat_tree_prev_sibling(const lxb_dom_node_t *n)
{
    return ft_sibling(n, -1);
}

lxb_dom_node_t *flat_tree_next_in(const lxb_dom_node_t *n, const lxb_dom_node_t *root, bool skip_subtree)
{
    lxb_dom_node_t *m, *s;

    DCHECK(n != NULL && root != NULL, "a flat-tree pre-order step was asked with no node or no root");
    if (!skip_subtree) {
        m = flat_tree_first_child(n);
        if (m != NULL) return m;
    }
    for (m = (lxb_dom_node_t *)n; m != root; m = flat_tree_parent(m)) {
        DCHECK(m != NULL, "a flat-tree pre-order step climbed out of the flat tree before reaching its root, so "
                          "the node it was asked from is no flat-tree descendant of that root");
        s = flat_tree_next_sibling(m);
        if (s != NULL) return s;
    }
    return NULL;
}
