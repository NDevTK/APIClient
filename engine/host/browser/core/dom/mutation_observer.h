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
void mutation_observer_free(JSContext *ctx);

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

/* §4.2.3's `suppressObservers`, as a SCOPE. Inserting a DocumentFragment is one operation in the standard and N
   tree writes here, so the per-node hook queued N records where a browser queues one. Wrap the fragment's
   children loop: removals inside belong to the fragment (step 4's own record), insertions to the parent (the
   operation's record), and both are emitted once at the end. Nests; inert when no observer is registered. */
void mutation_observer_batch_begin(void);
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
