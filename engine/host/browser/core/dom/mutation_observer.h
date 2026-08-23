/* DOM §4.3 "Mutation observers" — the registered observer lists, the record queue, and the microtask that
   delivers them. See mutation_observer.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_MUTATION_OBSERVER_H
#define ENGINE_HOST_BROWSER_CORE_DOM_MUTATION_OBSERVER_H
#include <lexbor/dom/dom.h>
#include <stdbool.h>
#include "quickjs.h"

void mutation_observer_init(JSContext *ctx);
void mutation_observer_install_proto(JSContext *ctx);   /* §3.7: this realm's MutationObserver.prototype */
void mutation_observer_install(JSContext *ctx, JSValueConst global);
void mutation_observer_free(JSRuntime *rt);

/* §4.3.2 "QUEUE A MUTATION RECORD of `type` for `target` with name, namespace, oldValue, addedNodes,
   removedNodes, previousSibling and nextSibling" — the ONE entry every mutation reaches this component
   through, with the spec's own argument list in the spec's own order.
 *
 * `type` is one of mutation_record.h's MR_TYPE_*. `name`/`ns`/`old` are the spec's three nullable strings and
 * are NULL where it passes null — `ns` is the attribute's NAMESPACE and not a convenience, because §4.3.2's
 * interested-observer test rejects an attributeFilter match outright when the namespace is non-null, and
 * §4.3.3's `attributeNamespace` is what tells an `xlink:href` from the null-namespace `href` an observer also
 * watches. `old_len` is the byte length of `old`, because an attribute's value may contain a NUL.
 * `added`/`removed` are BORROWED arrays of node wrappers, or JS_UNDEFINED for the spec's « » — one record is
 * built per interested observer, each with its own copy, because §4.3.3 declares the two NodeLists
 * [SameObject] and two records sharing one list would answer `a.addedNodes === b.addedNodes` true.
 * `prev`/`next` may be NULL. */
void mutation_observer_queue_record(JSContext *ctx, int type, lxb_dom_node_t *target,
                                    const char *name, const char *ns, const char *old, size_t old_len,
                                    JSValueConst added, JSValueConst removed,
                                    lxb_dom_node_t *prev, lxb_dom_node_t *next);

/* §4.2.3's REMOVE step 15 — "for each inclusive ancestor of parent, and then for each registered of that
   node's registered observer list, if registered's options["subtree"] is true, append a new TRANSIENT
   registered observer … to node's registered observer list".
 *
 * It exists so that a node removed from a subtree an observer watches keeps reporting its own mutations until
 * the observer is next notified — which is the whole reason `subtree: true` survives a removal, and the part
 * an implementation omits SILENTLY: everything still works, and a page that removes a node and then mutates it
 * simply never hears about it. Called BEFORE the removal record is queued, which is the order the steps are
 * numbered in. */
void mutation_observer_transient_for_removal(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *parent);

/* §4.2.3's INSERT step 8 and REMOVE steps 15-16 — "queue a TREE mutation record for parent", registered into
   node.h's tree-steps list so it fires from the ONE chokepoint every tree write goes through. It answers at
   NODE_TREE_INSERTED and at NODE_TREE_REMOVING and at neither other phase: the record carries
   oldPreviousSibling and oldNextSibling, which §4.2.3's remove binds at its steps 5-6, BEFORE the detach — so
   the post-detach phase (which exists for the slot steps) has no siblings left to read and nothing to say.
   It is registered LAST of the tree hooks, because the list runs in registration order and §4.2.3 numbers the
   record AFTER the live-range steps, the NodeIterator pre-remove steps, the removing steps and the
   custom-element reactions. */
void mutation_observer_tree_steps(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *parent, int phase);

/* §4.2.3's MOVE STEPS 25-26 — "queue a tree mutation record for oldParent with « », « node »,
   oldPreviousSibling, and oldNextSibling" and "queue a tree mutation record for newParent with « node », « »,
   newPreviousSibling, and child".
 *
 * IT IS A CALL AND NOT THE TREE-HOOK REGISTRATION ABOVE, because a move fires no tree hook at all: §4.2.3's
 * move is a separate primitive from insert and remove and runs neither's steps, so the list that carries the
 * insertion and removing steps is exactly the list a move must not reach. What survives of the removal side is
 * these two records and nothing else — in particular NOT remove's step 15, whose transient registered
 * observers are absent from move's numbering, so a node moved out of a subtree an observer watches stops
 * reporting to it. That absence is the standard's, and reusing the hook would have silently added it back.
 *
 * TWO RECORDS FOR ONE NODE is what makes this its own entry rather than two calls at the caller: the node list
 * « node » is ONE array shared by both records, and building it is this component's private shape. The four
 * sibling arguments are all bound by the caller BEFORE its own step 13 detach (steps 11-12) or between the
 * detach and the insert (step 18), which is why they are passed rather than read back off the tree. */
void mutation_observer_move_steps(JSContext *ctx, lxb_dom_node_t *node,
                                  lxb_dom_node_t *old_parent, lxb_dom_node_t *old_prev, lxb_dom_node_t *old_next,
                                  lxb_dom_node_t *new_parent, lxb_dom_node_t *new_prev, lxb_dom_node_t *child);

/* §4.2.3's `suppressObservers`, as a SCOPE. Insert and replace are each ONE operation in the standard and N
   tree writes here, so the per-node hook queued N records where a browser queues one. `parent` is the
   operation's parent — the target of the record this scope will queue — and `suppressed` is the one child the
   operation removes with suppressObservers set (replace's `child`; NULL for insert). `from` is the FRAGMENT
   whose children insert step 4 removes, which gets its own record — NULL for replace, whose inner insert
   scope supplies it when the replacement is a fragment. Every OTHER removal seen inside the scope belongs to
   a different algorithm — adopt's step 2 — and is left to the per-node path so it keeps its own record, its
   old siblings and its order. Nests for `replaceChild(fragment, child)`; inert when no observer is
   registered. */
void mutation_observer_batch_begin(lxb_dom_node_t *parent, lxb_dom_node_t *suppressed,
                                  lxb_dom_node_t *from);
void mutation_observer_batch_end(void);

/* §4.10's "REPLACE DATA" step 4 — "queue a mutation record of `characterData` for node with null, null, node's
   data, « », « », null and null". Fired by the character-data chokepoint, BEFORE the write, because `node's
   data` there is the value the node still holds. */
void mutation_observer_character_data(JSContext *ctx, lxb_dom_node_t *node, const char *old, size_t old_len);

/* HAS THIS AGENT EVER REGISTERED AN OBSERVER? The chokepoint asks before walking a mutated node's ancestors,
   because with no observer in existence there is no list to find and the walk would materialize a wrapper per
   ancestor on every attribute write in the document.
 *
 * MONOTONIC — set by the first `observe()` and never cleared, which is what makes it sound under time travel.
 * A count that `disconnect()` decremented would be a count the COW swap could unapply out from under, and the
 * failure mode of a too-low count is an observer that never fires; a flag that is only ever set can only cost
 * a walk that finds nothing. */
bool mutation_observer_any(void);

/* §4.3 "QUEUE A MUTATION OBSERVER MICROTASK" — the agent's ONE flag and its ONE microtask. Exported because
   §4.2.2.5's "signal a slot change" ends in exactly this operation and must not have a flag of its own: §4.3
   is one algorithm that clones BOTH sets, delivers every record at step 6 and only THEN fires every
   `slotchange` at step 7, so two flags and two microtasks make that order whichever half was queued first. */
void mutation_observer_queue_microtask(JSContext *ctx);

#endif
