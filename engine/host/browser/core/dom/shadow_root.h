/* THE ShadowRoot INTERFACE AND "attach a shadow root" — DOM §4.8. See shadow_root.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_SHADOW_ROOT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_SHADOW_ROOT_H
#include <lexbor/dom/dom.h>
#include <stdbool.h>
#include "quickjs.h"

void shadow_root_init(JSContext *ctx);
/* §4.8's prototype for ONE realm — declared into core/realm.h's list. */
void shadow_root_install_proto(JSContext *ctx);
/* The `ShadowRoot` interface object, and Element's two §4.8 members (`attachShadow`, `shadowRoot`). */
void shadow_root_install(JSContext *ctx, JSValueConst global);
void shadow_root_install_element_members(JSContext *ctx, JSValueConst element_proto);
void shadow_root_free(JSContext *ctx);

/* IS THIS NODE A SHADOW ROOT — the one question every §4.8-aware algorithm asks, and the reason a shadow root
   carries lexbor's own node type rather than being a DocumentFragment with a flag: retargeting, the
   shadow-including root, "find a slot" and the event path all ask it with no realm in hand. §4.8 still says a
   ShadowRoot IS a DocumentFragment, which is node_is_document_fragment's job (node.h) and not this one's. */
bool shadow_root_is(const lxb_dom_node_t *n);
/* §4.8's HOST, which is never null for a shadow root. C state on a node the ATTACHING FLOW created, written
   once at creation and never again, so it needs no per-flow capture — the flow that created the node is the
   only one that can reach it until the node becomes baseline, after which it is immutable. */
lxb_dom_element_t *shadow_root_host(const lxb_dom_node_t *n);
/* §4.8's MODE, in lexbor's own enum. Asked by `element.shadowRoot` (a closed root answers null), by "find a
   slot"'s `open` flag, and by the event path's closed-tree levels. */
bool shadow_root_is_open(const lxb_dom_node_t *n);
/* §4.8's SLOT ASSIGNMENT, as the one question §4.2.2 asks of it: is this tree's assignment "manual". Named for
   the question rather than for the field, because "named" is the default and every algorithm branches on the
   other one. */
bool shadow_root_slot_assignment_is_manual(JSContext *ctx, const lxb_dom_node_t *n);

/* "SHADOW-INCLUDING ROOT" — DOM §4.2: the root's host's shadow-including root when the root is a shadow root,
   otherwise the root. What `getRootNode({composed:true})` answers and what §4.4's `isConnected` is stated
   over, which is why it lives beside node_root rather than inside one member. */
lxb_dom_node_t *shadow_root_shadow_including_root(lxb_dom_node_t *n);
/* §4.2's "A is a SHADOW-INCLUDING INCLUSIVE ANCESTOR of B" — the containment relation that CROSSES a shadow
   boundary, so a host is one of everything in its shadow tree. Every caller of it is a place where the plain
   ancestor walk answers `false` for a node inside a shadow tree: §2.9's event path uses it to find the boundary
   to retarget at, §4.13.7's `setValidity` to accept an anchor inside the element's own shadow tree. */
bool shadow_root_is_shadow_including_inclusive_ancestor(const lxb_dom_node_t *a, const lxb_dom_node_t *b);
/* §4.2's SHADOW-INCLUDING TREE ORDER, one step at a time — node_next_in's shape, with an element's shadow root
   visited just after the element and before its children. NULL once the walk leaves `root`, so
   "the shadow-including inclusive descendants of R, in shadow-including tree order" is
   `for (n = R; n; n = shadow_root_next_in_shadow_including(ctx, n, R))`. It takes a `ctx` because the
   element -> shadow root association is a per-flow fact kept on the element's WRAPPER. */
lxb_dom_node_t *shadow_root_next_in_shadow_including(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *root);

/* §4.8 "ATTACH A SHADOW ROOT", REACHED FROM C. `attachShadow` is one caller and HTML §13.2.6.4.4's template
   start tag is the other: the parser runs the SAME algorithm on an element it found in the tree it built, and a
   second copy of five refusals is five places for one of them to go missing. Returns the shadow root's wrapper
   (OWNED), or JS_EXCEPTION with the `NotSupportedError` pending — which the parser CATCHES, because tree
   construction throws nothing at the page. */
JSValue shadow_root_attach(JSContext *ctx, JSValueConst el_wrap, const char *mode, bool delegates_focus,
                           const char *slot_assignment, bool clonable, bool serializable);
/* DOM §4.4 "CLONE A NODE" STEPS 6.1-6.7, given the node being cloned and its copy. Step 6's three conditions
   are asked here — is `node` an element, is it a shadow host, is its shadow root's `clonable` true — because
   the second and third are §4.8 record reads and the record is this component's. Answers JS_NULL when any of
   them is false (there is no shadow root to clone), the COPY's new shadow root's wrapper (OWNED) when there is,
   and JS_EXCEPTION when step 6.5's attach threw. That last one is REACHABLE and step 6 does not catch it, so
   `cloneNode` throws out of the walk: `attachShadow` on an element whose local name has no definition yet
   succeeds, and a `define` for that name with `disabledFeatures: ["shadow"]` afterwards makes the same
   element's COPY fail §4.8 step 3.
   STEP 6.8 — cloning the shadow root's children into it — is NOT here. It is `clone a node` over a subtree,
   which is node.c's walk, and re-running that walk here would be a second clone implementation reached only
   through a shadow root. */
JSValue shadow_root_clone_onto(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *copy);

/* HTML §13.2.6.4.4's two writes on the root it just attached: "Set shadow's declarative to true" and "Set
   shadow's available to element internals to true". Both are fields of §4.8's record, which is this
   component's, which is why the parser asks rather than writes. This is `declarative`'s ONLY writer to true,
   and therefore the only thing that makes §4.8 step 4's re-attach branch reachable. */
void shadow_root_mark_declarative(JSContext *ctx, JSValueConst sr_wrap);

/* AN ELEMENT'S SHADOW ROOT, or NULL — DOM §4.9's "shadow root" association, read from the element's WRAPPER,
   which is where a per-flow fact belongs (the heap COW delta captures a property write; a C field on the
   lexbor element would be one answer for every flow). A shadow host always HAS a wrapper: `attachShadow` is an
   IDL member reached through one, and the identity map holds it for the node's whole life — so an element with
   no wrapper has no shadow root, with no allocation to ask. */
lxb_dom_node_t *shadow_root_of_element(JSContext *ctx, const lxb_dom_element_t *el);
/* The same association as the WRAPPER §4.8's members hand back. OWNED; JS_NULL when there is none. */
JSValue shadow_root_of_element_wrap(JSContext *ctx, JSValueConst el_wrap);

#endif
