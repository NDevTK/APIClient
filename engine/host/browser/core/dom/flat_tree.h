/* css-shadow-1 §4.1 "Flattening the DOM into an Element Tree": the flat tree, "which CSS uses for all purposes
 * after Selectors (including inheritance and box construction)". The same nodes as the DOM, with three
 * differences: a shadow host's children are its shadow root's children, a slot's children are its assigned
 * nodes (its own children when nothing is assigned), and a host's light child that no slot was assigned is in no
 * flat tree at all. Every layout and style walk over parents or children uses these accessors, so the walks
 * agree on one tree; Blink's FlatTreeTraversal is the analogue. Slot assignment and the shadow root association
 * live on the wrappers (per flow), and are read in the node's document's realm. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_FLAT_TREE_H
#define ENGINE_HOST_BROWSER_CORE_DOM_FLAT_TREE_H

#include <lexbor/dom/dom.h>

/* `n`'s flat-tree parent: its assigned slot, the host of the shadow root it is a child of, or its DOM parent.
   NULL for the root, and for a node in no flat tree (a host's unassigned light child, the children of a slot
   that has assigned nodes, and everything below either). `n` is never a shadow root, which is no flat-tree node
   (asserted). */
lxb_dom_node_t *flat_tree_parent(const lxb_dom_node_t *n);

/* `n`'s first and last flat-tree children, NULL when it has none. */
lxb_dom_node_t *flat_tree_first_child(const lxb_dom_node_t *n);
lxb_dom_node_t *flat_tree_last_child(const lxb_dom_node_t *n);

/* `n`'s next and previous flat-tree siblings under `flat_tree_parent(n)`, NULL at either end; `n` is in a flat
   tree and is not its root (asserted). */
lxb_dom_node_t *flat_tree_next_sibling(const lxb_dom_node_t *n);
lxb_dom_node_t *flat_tree_prev_sibling(const lxb_dom_node_t *n);

#endif
