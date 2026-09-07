/* THE EVENT PATH AND ITS ITEMS — DOM §2.9 "append to an event path". See event_path.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_PATH_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_PATH_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

/* §2.9's path is a list of EVENT PATH ITEMS, not a list of targets, and the difference is the whole of shadow
   DOM's effect on dispatch: the item says both WHOSE listeners run (its invocation target) and WHAT the event's
   `target` reads as while they do (its shadow-adjusted target, null for every item that did not cross a shadow
   boundary). Two consumers read it and neither can be given a bare list — `invoke` steps 1-2 walk BACKWARD from
   an item to the nearest non-null shadow-adjusted target, and `composedPath()` walks outward in both directions
   counting closed-tree levels. Both live above this file; this one owns what an item IS.
   A LIST IS A JS ARRAY AND AN ITEM IS A JS OBJECT, for the reason every other queued platform datum in this
   engine is: the path is the EVENT's state, so it forks with the flow that built it, its writes are property
   writes the COW delta already captures, and it parks to the cold tier with the snapshot that names it. A
   malloc'd item list would revert its head pointer on a context switch and leak every item. */
JSValue event_path_new(JSContext *ctx);

/* "To APPEND TO AN EVENT PATH, given an event, an EventTarget invocationTarget, a potential event target
   shadowAdjustedTarget, a potential event target relatedTarget, a list of potential event targets touchTargets,
   and a boolean slotInClosedTree." `shadow_adjusted_target` is JS_NULL for an item the walk did not retarget
   at. `root_of_closed_tree` is the caller's because step 4 asks a TREE question ("invocationTarget is a shadow
   root whose mode is closed") and this file never learns what a node is — the same boundary that keeps the
   events layer free of lexbor. `invocation_target_in_shadow_tree` is the caller's for EXACTLY that reason and
   no other: the append's own steps 1-2 are "Let invocationTargetInShadowTree be false. If invocationTarget is
   a node and its root is a shadow root, then set invocationTargetInShadowTree to true", and both halves of
   that condition are tree questions. The caller answers it with the ONE predicate that already asks it —
   event_target.c's `dispatch_root_is_shadow_root` with `want_closed` false, which step 6.11 was reaching for
   first — rather than a second spelling that could answer differently.
   `related_target` AND `touch_targets` ARE THE ITEM'S OWN, ALREADY RETARGETED — §2.9 runs §4.8's retargeting
   against THIS item's invocation target before it appends, so the two are per-item values and not the event's.
   That is the whole reason they are fields: `invoke` steps 4-5 set the event's relatedTarget and touch target
   list FROM THE ITEM at every entry, so a listener outside a shadow tree reads the host where one inside reads
   the node. `touch_targets` is JS_NULL for the EMPTY LIST — the same state one allocation cheaper, and the
   spelling the event's own touch target list uses.
   ALL SEVEN OF THE STANDARD'S ITEM FIELDS ARE HERE. `invocation_target_in_shadow_tree` was the one that was
   not, and the residual that recorded its absence named its only reader exactly right — DOM §2.9 "Dispatching
   events"' inner invoke step 2.8.2 — while getting the OWNER of the member that step suppresses wrong: it is
   DOM §2.3 "Legacy extensions to the Window interface"' `event`, not HTML's, and @webref/idl carries that one
   line in `dom.idl` and in no other. The step number was the half that had already been repaired once, from
   2.7.2, and it was right: step 2.7 is "Let currentEvent be undefined" and holds no list, while step 2.8 is
   "If global is a Window object:" and holds the two sub-steps — 2.8.1 saves the current event, 2.8.2 is the
   one that reads this flag. The mis-attribution is recorded rather than merely deleted, because a reader who
   re-derives it will look for the attribute in the wrong standard, and it is one `grep` of the harvested IDL
   away from being checked either way. */
void event_path_append(JSContext *ctx, JSValueConst path, JSValueConst invocation_target,
                       bool invocation_target_in_shadow_tree, JSValueConst shadow_adjusted_target,
                       JSValueConst related_target, JSValueConst touch_targets, bool root_of_closed_tree,
                       bool slot_in_closed_tree);

uint32_t event_path_length(JSContext *ctx, JSValueConst path);
/* The item at `i`. OWNED. It is a DCHECK to ask past the end — every walk here is over the path's own size. */
JSValue event_path_item(JSContext *ctx, JSValueConst path, uint32_t i);

/* An item's fields. The targets are OWNED; the shadow-adjusted target is JS_NULL when the item has none. */
JSValue event_path_invocation_target(JSContext *ctx, JSValueConst item);
JSValue event_path_shadow_adjusted_target(JSContext *ctx, JSValueConst item);
/* OWNED. JS_NULL is a real answer for both: the item's relatedTarget is a POTENTIAL event target, and the
   touch target list is JS_NULL when it is empty. */
JSValue event_path_related_target(JSContext *ctx, JSValueConst item);
JSValue event_path_touch_targets(JSContext *ctx, JSValueConst item);
/* §2.9's "Let invocationTargetInShadowTree be pathItem's invocation-target-in-shadow-tree", which `invoke`
   reads once per path item and hands to inner invoke — where step 2.8.2 is its ONE reader and DOM §2.3's
   `event` is what it decides. It is the field that makes a listener inside a shadow tree read `window.event`
   as undefined while the host's listener reads the event, which is the standard's own note that the attribute
   "is inaccurate for events dispatched in shadow trees". */
bool    event_path_invocation_target_in_shadow_tree(JSContext *ctx, JSValueConst item);
bool    event_path_root_of_closed_tree(JSContext *ctx, JSValueConst item);
bool    event_path_slot_in_closed_tree(JSContext *ctx, JSValueConst item);

#endif
