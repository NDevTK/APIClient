/* SLOTS AND SLOTTABLES — DOM §4.2.2 and §4.2.9, and HTML §4.12.4's `<slot>`. See slot.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_SLOT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_SLOT_H
#include <lexbor/dom/dom.h>
#include <stdbool.h>
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/events/event_target.h"   /* EventFireCb — the width of the fire request this machine parks on */

void slot_init(JSContext *ctx);
/* HTML §4.12.4's three members, on the interface whose IDL declares them. */
void slot_install_slot_members(JSContext *ctx, JSValueConst slot_proto);
/* §4.2.9's `Slottable` mixin — one member, on each interface whose IDL INCLUDES it (Element and Text). */
void slot_install_slottable_mixin(JSContext *ctx, JSValueConst proto);
void slot_free(JSRuntime *rt);

/* IS THIS AN HTML `<slot>` ELEMENT — §4.2.2's "slot", which is the element and nothing else. */
bool slot_is(const lxb_dom_node_t *n);
/* §4.2.2.2's ASSIGNED SLOT, or NULL — the STORED association, and with it "a slottable is ASSIGNED", which the
   standard defines as exactly "its assigned slot is non-null". NOT §4.2.9's `assignedSlot` getter, which re-runs
   "find a slot" with the open flag so a closed tree stays hidden from script: this is the engine-internal fact
   §4.4's get the parent is stated over ("returns the node's assigned slot, if node is assigned"), and answering
   that one with the script-facing getter would route a closed tree's event around its own slot. */
lxb_dom_node_t *slot_assigned_slot(JSContext *ctx, const lxb_dom_node_t *n);

/* §4.2.2.4's "ASSIGN SLOTTABLES FOR A TREE, given a node root", for the one caller that is not a mutation. A
   browser reaches it through §4.2.3's insertion steps as each node of a shadow tree is inserted; a tree the
   PARSER built never passes through them, so HTML §13.2.6.4.4's declarative shadow root arrives fully populated
   with slots that have never been asked what they hold — and what they hold is the host's children, which the
   same parse already put in place. */
void slot_assign_for_a_tree(JSContext *ctx, lxb_dom_node_t *root);

/* §4.2.3's SLOT-RELATED MUTATION STEPS, called from the tree-hook list in the standard's own order. `parent` is
   the node's parent — passed rather than read off the node, because the REMOVE half runs AFTER the detach and
   the node has no parent left to read by then. */
void slot_insert_steps(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *parent);
void slot_removed_steps(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *parent);
/* §4.2.2's attribute change steps: `slot` on a slottable updates its name and re-assigns, `name` on a slot
   re-assigns that shadow tree. `ns` NULL is the null namespace, which is the only one either applies to. */
void slot_attribute_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local);

/* §4.3 "NOTIFY MUTATION OBSERVERS" STEPS 4-5 AND 7 — the signal-slots half, as a struct the ONE notify machine
   embeds. It is not a machine of its own, and that is the whole point: §4.3 is ONE algorithm with ONE
   agent-wide `mutation observer microtask queued` flag, and it clones BOTH sets, delivers every record at step
   6 and only THEN fires every `slotchange` at step 7. Two machines with two flags and two microtasks is two
   implementations of one algorithm whose ORDER is observable — a `slotchange` listener and a MutationObserver
   callback would run in whichever order the two microtasks happened to be queued.
   Same shape as report_exception.h's ReportExceptionWork and custom_elements.h's CustomElementQueue: the
   algorithm belongs here, the state belongs to the machine that parks in it. Firing an event runs the page's
   listeners, so `_run` parks: JS_STEP_CALL = return it, 0 = every slot has been fired at. */
typedef struct {
    uint8_t  fphase;   /* the fire request's own phase */
    uint32_t i;        /* the cursor into signalSet */
    JSValue  set;      /* step 4's CLONE of the agent's signal slots (owned) */
    JSValue  ev;       /* the slotchange event in flight (owned) */
    EventFireCb  cb;    /* the fire request's buffer — event_target_fire_run needs four slots */
} SlotChangeWork;

void slot_change_work_start(SlotChangeWork *w);
void slot_change_work_visit(JSContext *ctx, SlotChangeWork *w, JSStepVisit *v);
/* Steps 4 and 5: clone the agent's signal slots and empty it. The Array is not replaced — it is the agent's,
   and swapping the static would make one flow's replacement visible to every other. */
void slot_signal_slots_take(JSContext *ctx, SlotChangeWork *w);
/* Step 7: "for each slot of signalSet, fire an event named slotchange, with its bubbles attribute set to true,
   at slot" — one slot per resume, because each fire runs the page's listeners. */
int  slot_change_work_run(JSContext *ctx, SlotChangeWork *w, JSValue cb_result, JSValue **out_cb, int *out_argc);

#endif
