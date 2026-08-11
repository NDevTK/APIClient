/* SLOTS AND SLOTTABLES — DOM §4.2.2 and §4.2.9, and HTML §4.12.4's `<slot>`. See slot.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_SLOT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_SLOT_H
#include <lexbor/dom/dom.h>
#include <stdbool.h>
#include "quickjs.h"

void slot_init(JSContext *ctx);
/* §4.2.2's per-realm half: the `slotchange` notification driver, which is a FUNCTION OBJECT and therefore
   carries the realm it was minted in. Declared into core/realm.h's list. */
void slot_install_proto(JSContext *ctx);
/* HTML §4.12.4's three members, on the interface whose IDL declares them. */
void slot_install_slot_members(JSContext *ctx, JSValueConst slot_proto);
/* §4.2.9's `Slottable` mixin — one member, on each interface whose IDL INCLUDES it (Element and Text). */
void slot_install_slottable_mixin(JSContext *ctx, JSValueConst proto);
void slot_free(JSContext *ctx);

/* IS THIS AN HTML `<slot>` ELEMENT — §4.2.2's "slot", which is the element and nothing else. */
bool slot_is(const lxb_dom_node_t *n);

/* §4.2.3's SLOT-RELATED MUTATION STEPS, called from the tree-hook list in the standard's own order. `parent` is
   the node's parent — passed rather than read off the node, because the REMOVE half runs AFTER the detach and
   the node has no parent left to read by then. */
void slot_insert_steps(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *parent);
void slot_removed_steps(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *parent);
/* §4.2.2's attribute change steps: `slot` on a slottable updates its name and re-assigns, `name` on a slot
   re-assigns that shadow tree. `ns` NULL is the null namespace, which is the only one either applies to. */
void slot_attribute_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local);

#endif
