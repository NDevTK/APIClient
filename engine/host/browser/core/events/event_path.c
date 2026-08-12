/* THE EVENT PATH — DOM §2.9 "append to an event path", and what one item of that path is.
 *
 * WHY IT IS ITS OWN FILE. The path used to be an Array of TARGETS, which is the right answer for a document
 * with no shadow tree in it and a different answer for one with: every item's `target` was path[0], where the
 * standard gives each item its OWN shadow-adjusted target and leaves it null for the items that did not cross a
 * boundary, and `composedPath()` answered every entry where the standard hides the ones behind a closed root.
 * Both of those are reads of a FIELD an item has to carry, and the two readers live in different components —
 * the dispatch machine (event_target.c) and `composedPath` (event.c). One definition of the item, read by both,
 * is the only shape in which they cannot disagree about what an item is.
 *
 * WHAT AN ITEM IS MADE OF. An engine-built null-prototype record, the same as §2.7's listener and every other
 * internal slot record here: reading a field out of it runs none of the page's code, and writing one is an
 * ordinary property write the per-flow COW delta captures — so a path built by one arm of a fork is that arm's,
 * and it parks and resumes with the flow that built it. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "core/events/event_path.h"

JSValue event_path_new(JSContext *ctx)
{
    JSValue path = JS_NewArray(ctx);

    CHECK(!JS_IsException(path), "§2.9's event path could not be allocated — a dropped path is a dispatch with "
                                 "no propagation and a `composedPath()` that answers about another walk");
    return path;
}

uint32_t event_path_length(JSContext *ctx, JSValueConst path)
{
    JSValue v;
    uint32_t n = 0;

    DCHECK(JS_IsArray(path), "§2.9's event path was measured on something that is not a path");
    v = JS_GetPropertyStr(ctx, path, "length");
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

void event_path_append(JSContext *ctx, JSValueConst path, JSValueConst invocation_target,
                       JSValueConst shadow_adjusted_target, JSValueConst related_target,
                       JSValueConst touch_targets, bool root_of_closed_tree, bool slot_in_closed_tree)
{
    JSValue item = idl_slots_new(ctx);

    DCHECK(JS_IsArray(path), "§2.9's append to an event path was given something that is not a path");
    DCHECK(JS_IsObject(invocation_target),
           "§2.9's append to an event path was given no invocation target — every item names the EventTarget "
           "whose listeners the walk will invoke, and an item without one is a listener list nobody owns");
    DCHECK(JS_IsObject(shadow_adjusted_target) || JS_IsNull(shadow_adjusted_target),
           "§2.9's shadow-adjusted target is a POTENTIAL event target — an EventTarget or null, and null is the "
           "answer `invoke` step 1 walks backward past. Undefined is neither, and would end that walk at an "
           "item that is not the target");
    DCHECK(JS_IsObject(related_target) || JS_IsNull(related_target),
           "§2.9's item relatedTarget is a POTENTIAL event target — an EventTarget or null. Undefined is "
           "neither, and `invoke` step 4 hands it straight to the event, where a page reads it as "
           "`event.relatedTarget`");
    DCHECK(JS_IsArray(touch_targets) || JS_IsNull(touch_targets),
           "§2.9's item touch target list is a LIST of potential event targets — an Array, or null for the "
           "empty list. Anything else is a list step 6.11 cannot look inside and `invoke` step 5 cannot set");
    CHECK(!JS_IsException(item), "§2.9's event path item could not be allocated");
    JS_SetPropertyStr(ctx, item, "invocationTarget", JS_DupValue(ctx, invocation_target));
    JS_SetPropertyStr(ctx, item, "shadowAdjustedTarget", JS_DupValue(ctx, shadow_adjusted_target));
    JS_SetPropertyStr(ctx, item, "relatedTarget", JS_DupValue(ctx, related_target));
    JS_SetPropertyStr(ctx, item, "touchTargets", JS_DupValue(ctx, touch_targets));
    JS_SetPropertyStr(ctx, item, "rootOfClosedTree", JS_NewBool(ctx, root_of_closed_tree));
    JS_SetPropertyStr(ctx, item, "slotInClosedTree", JS_NewBool(ctx, slot_in_closed_tree));
    JS_SetPropertyUint32(ctx, (JSValue)path, event_path_length(ctx, path), item);
}

JSValue event_path_item(JSContext *ctx, JSValueConst path, uint32_t i)
{
    JSValue item;

    DCHECK(JS_IsArray(path), "an item was asked of something that is not an event path");
    item = JS_GetPropertyUint32(ctx, path, i);
    DCHECK(JS_IsObject(item), "§2.9's event path was indexed past its own size — every walk over it is bounded "
                              "by the path's length, so a miss here is a cursor that outran the list");
    return item;
}

static JSValue item_target(JSContext *ctx, JSValueConst item, const char *field)
{
    JSValue v;

    DCHECK(JS_IsObject(item), "an event path item's field was read off something that is not an item");
    v = JS_GetPropertyStr(ctx, item, field);
    DCHECK(JS_IsObject(v) || JS_IsNull(v), "an event path item's target field holds neither an EventTarget nor "
                                           "null — the item was built by something that is not the append");
    return v;
}

JSValue event_path_invocation_target(JSContext *ctx, JSValueConst item)
{
    JSValue t = item_target(ctx, item, "invocationTarget");

    DCHECK(JS_IsObject(t), "an event path item has no invocation target — see the append's own assert");
    return t;
}

JSValue event_path_shadow_adjusted_target(JSContext *ctx, JSValueConst item)
{
    return item_target(ctx, item, "shadowAdjustedTarget");
}

JSValue event_path_related_target(JSContext *ctx, JSValueConst item)
{
    return item_target(ctx, item, "relatedTarget");
}

JSValue event_path_touch_targets(JSContext *ctx, JSValueConst item)
{
    JSValue v;

    DCHECK(JS_IsObject(item), "an event path item's touch target list was read off something that is not an item");
    v = JS_GetPropertyStr(ctx, item, "touchTargets");
    DCHECK(JS_IsArray(v) || JS_IsNull(v),
           "an event path item's touch target list is neither a list nor the empty list — the item was built by "
           "something that is not the append, and step 6.11 would then look for shadow-tree nodes inside it");
    return v;
}

static bool item_flag(JSContext *ctx, JSValueConst item, const char *field)
{
    JSValue v;
    bool b;

    DCHECK(JS_IsObject(item), "an event path item's flag was read off something that is not an item");
    v = JS_GetPropertyStr(ctx, item, field);
    DCHECK(JS_IsBool(v), "an event path item's closed-tree flag is not a boolean — composedPath counts hidden "
                         "levels with it, and a missing field would count as false and expose a closed tree");
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

bool event_path_root_of_closed_tree(JSContext *ctx, JSValueConst item)
{
    return item_flag(ctx, item, "rootOfClosedTree");
}

bool event_path_slot_in_closed_tree(JSContext *ctx, JSValueConst item)
{
    return item_flag(ctx, item, "slotInClosedTree");
}
