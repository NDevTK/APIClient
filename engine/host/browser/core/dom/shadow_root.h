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

/* AN ELEMENT'S SHADOW ROOT, or NULL — DOM §4.9's "shadow root" association, read from the element's WRAPPER,
   which is where a per-flow fact belongs (the heap COW delta captures a property write; a C field on the
   lexbor element would be one answer for every flow). A shadow host always HAS a wrapper: `attachShadow` is an
   IDL member reached through one, and the identity map holds it for the node's whole life — so an element with
   no wrapper has no shadow root, with no allocation to ask. */
lxb_dom_node_t *shadow_root_of_element(JSContext *ctx, const lxb_dom_element_t *el);
/* The same association as the WRAPPER §4.8's members hand back. OWNED; JS_NULL when there is none. */
JSValue shadow_root_of_element_wrap(JSContext *ctx, JSValueConst el_wrap);

#endif
