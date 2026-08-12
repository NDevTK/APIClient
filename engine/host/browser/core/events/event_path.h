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
   shadowAdjustedTarget, ... and a boolean slotInClosedTree." `shadow_adjusted_target` is JS_NULL for an item
   the walk did not retarget at. `root_of_closed_tree` is the caller's because step 4 asks a TREE question
   ("invocationTarget is a shadow root whose mode is closed") and this file never learns what a node is — the
   same boundary that keeps the events layer free of lexbor.
   TWO OF THE STANDARD'S SEVEN ITEM FIELDS ARE ABSENT, BY NAME, because nothing in this engine can produce a
   value for them: `relatedTarget` and `touch target list` are the retargeted forms of an event's relatedTarget
   and touch target list, and there is no MouseEvent, FocusEvent or TouchEvent here for either to come from —
   §2.9's steps 4, 6.1-6.2 and 6.9.3-6.9.4 have nothing to retarget, and `invoke` steps 3-4 have nothing to
   write. `invocation-target-in-shadow-tree` is absent for the same reason at one remove: its only reader is
   inner invoke step 2.7.2, which suppresses HTML's `window.event`, and this engine has no `window.event`. */
void event_path_append(JSContext *ctx, JSValueConst path, JSValueConst invocation_target,
                       JSValueConst shadow_adjusted_target, bool root_of_closed_tree, bool slot_in_closed_tree);

uint32_t event_path_length(JSContext *ctx, JSValueConst path);
/* The item at `i`. OWNED. It is a DCHECK to ask past the end — every walk here is over the path's own size. */
JSValue event_path_item(JSContext *ctx, JSValueConst path, uint32_t i);

/* An item's fields. The targets are OWNED; the shadow-adjusted target is JS_NULL when the item has none. */
JSValue event_path_invocation_target(JSContext *ctx, JSValueConst item);
JSValue event_path_shadow_adjusted_target(JSContext *ctx, JSValueConst item);
bool    event_path_root_of_closed_tree(JSContext *ctx, JSValueConst item);
bool    event_path_slot_in_closed_tree(JSContext *ctx, JSValueConst item);

#endif
