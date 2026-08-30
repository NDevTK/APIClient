/* THE EVENT INTERFACE — DOM §2.2.
 *
 * WHAT WAS HERE BEFORE. event_target.c built the listener's argument with JS_NewObject and hung eight data
 * properties on it. That is not an Event, and three separate things followed from it. There was no `Event`
 * global, so `new Event('x')`, `new CustomEvent(...)` and every `instanceof Event` a page writes found nothing
 * — and because `Event` is on the platform-names list, reading it THREW rather than being mistaken for app
 * state, so a page that feature-tested that way stopped there. `dispatchEvent` could not exist at all, because
 * it takes an Event and there was no Event to take. And the flags were PUBLIC data properties: a page could
 * assign `ev.defaultPrevented = true` and the engine would believe it, while the spec makes it a getter over an
 * internal slot that only preventDefault() sets.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, the same shape abort.c uses for [[Signal]]. That is not
 * a shortcut around a C struct — it is what makes the event's state TIME-TRAVEL for free: a flag set by one
 * forked arm's listener is a property write like any other, so the COW delta captures it and the sibling arm's
 * dispatch of the same event object never sees it. A C struct behind an opaque would need its own delta kind.
 * It is also the brand: a page cannot forge the symbol, so "does it carry the slot record" IS `instanceof
 * Event` for the algorithms, and it stays true across subclassing the way a class-id check would not.
 *
 * WHAT IS ABSENT AND WHY. CustomEvent and the typed events this tree has not reached yet (FocusEvent,
 * InputEvent, TouchEvent…) are their own interfaces with their own state; they are honestly missing rather
 * than approximated by an Event with extra properties, and the IDL audit names them. §2.2's `relatedTarget`
 * and `touch target list` are NOT with them, and reading the standard is what says so: they are associated
 * values of the EVENT, initially null and the empty list, and UIEvents and Touch Events only define
 * ATTRIBUTES over them. So they are slots here like every other, they are what §2.9 retargets at each path
 * item and what `invoke` writes back at each one — mouse_event.c's `relatedTarget` attribute reads THIS slot
 * for exactly that reason, and the touch target list is still waiting for the interface that fills it.
 * Retargeting of the TARGET is the third of the same family: it is §2.9's shadow-adjusted target, and
 * composedPath below is written over it. */
#include <stdbool.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_slots.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/events/event.h"
#include "core/events/message_event.h"
#include "core/events/error_event.h"
#include "core/events/page_transition_event.h"
#include "core/events/navigate_event.h"
#include "core/events/navigation_current_entry_change_event.h"
#include "core/events/pop_state_event.h"
#include "core/events/hash_change_event.h"
#include "core/events/before_unload_event.h"
#include "core/events/storage_event.h"
#include "core/events/ui_event.h"
#include "core/events/mouse_event.h"
#include "core/events/keyboard_event.h"
#include "core/events/focus_event.h"
#include "core/events/event_path.h"
#include "core/timing/hr_time.h"
#include "solver/concolic.h"   /* §2.2's timeStamp is a duration off a clock a moment may be unknown on */

/* The private key the event's internal slots hang off — a Symbol, so a page enumerating the object cannot see
   it and cannot collide with it, for the reason the platform uses internal slots. */
static JSValue g_key = JS_UNDEFINED;
static int     g_ready;
/* §3.7 GIVES EACH REALM ITS OWN Event.prototype, and here that decides ANSWERS and not just identities: a C
   member runs in the realm that DEFINED it (js_call_c_function takes `ctx` from the function object), so one
   prototype shared by every document answers every document's question out of whichever realm happened to
   build it first. Held in quickjs's own per-context class-proto slot, the same mechanism EventTarget uses. */
static JSClassID g_event_class;
static int     g_ctor_stepid = -1;
/* THE IDL DECLARATIONS ARE THE AGENT'S, THE INSTALLS ARE THE REALM'S. A declaration mints a pool entry and the
   pool is SEALED after agent init, so minting one from a per-realm install would trip idl_declared_before_seal
   on the second realm — invisible to a fixture whose realms are all built before the seal, fatal in a runner
   that builds one later. So the ids are taken once, here, and every realm's members carry the same ones. */
static int g_cancel_bubble_setid = -1, g_return_value_setid = -1, g_init_event_id = -1;

/* §2.2's initialised-event slots. One record, so a brand check is one read. */
static JSValue event_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "an Event's slots were asked for before event_init ran");
    if (!JS_IsObject(ev))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    /* AN OWN SLOT, never a property LOOKUP: a lookup on a page object walks its prototype chain and reaches the
       solver's absent-state seam, which mints a concolic for a name nobody defined. An internal slot is by
       definition an own slot, so it is read as one. */
    if (JS_GetOwnSlot(ctx, &slots, ev, k) <= 0)
        slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return slots;
}

static bool slot_flag(JSContext *ctx, JSValueConst slots, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, slots, name);
    bool b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

bool event_is(JSContext *ctx, JSValueConst v)
{
    JSValue slots = event_slots(ctx, v);
    bool ok = JS_IsObject(slots);
    JS_FreeValue(ctx, slots);
    return ok;
}

JSValue event_type(JSContext *ctx, JSValueConst ev)
{
    JSValue slots = event_slots(ctx, ev), t;
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_UNDEFINED; }
    t = JS_GetPropertyStr(ctx, slots, "type");
    JS_FreeValue(ctx, slots);
    return t;
}

static bool event_read_flag(JSContext *ctx, JSValueConst ev, const char *name)
{
    JSValue slots = event_slots(ctx, ev);
    bool b = JS_IsObject(slots) && slot_flag(ctx, slots, name);
    JS_FreeValue(ctx, slots);
    return b;
}

static void event_write_flag(JSContext *ctx, JSValueConst ev, const char *name, bool on)
{
    JSValue slots = event_slots(ctx, ev);
    if (JS_IsObject(slots))
        JS_SetPropertyStr(ctx, slots, name, JS_NewBool(ctx, on));
    JS_FreeValue(ctx, slots);
}

bool event_canceled(JSContext *ctx, JSValueConst ev)       { return event_read_flag(ctx, ev, "canceled"); }
/* The flag itself, written — see event.h for why this is not §2.2's set-the-canceled-flag algorithm. */
void event_set_canceled(JSContext *ctx, JSValueConst ev, bool on) { event_write_flag(ctx, ev, "canceled", on); }
/* §2.9 reads these while it walks: whether the event travels up the path at all, and whether a listener
   stopped it between targets. */
bool event_bubbles(JSContext *ctx, JSValueConst ev)        { return event_read_flag(ctx, ev, "bubbles"); }
bool event_stop_propagation(JSContext *ctx, JSValueConst ev) { return event_read_flag(ctx, ev, "stopPropagation"); }

/* §2.2 eventPhase, which the walk moves: AT_TARGET for the target itself, BUBBLING_PHASE for its ancestors. */
void event_set_phase(JSContext *ctx, JSValueConst ev, int phase)
{
    JSValue slots = event_slots(ctx, ev);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return; }
    JS_SetPropertyStr(ctx, slots, "eventPhase", JS_NewInt32(ctx, phase));
    JS_FreeValue(ctx, slots);
}
bool event_stop_immediate(JSContext *ctx, JSValueConst ev) { return event_read_flag(ctx, ev, "stopImmediate"); }
bool event_dispatch_flag(JSContext *ctx, JSValueConst ev)  { return event_read_flag(ctx, ev, "dispatch"); }
/* §2.2's initialized flag — set by every constructor and by initEvent, unset by §4.5's createEvent. */
bool event_initialized(JSContext *ctx, JSValueConst ev)    { return event_read_flag(ctx, ev, "initialized"); }

/* §4.5 createEvent steps 6-8, performed on an event its own interface has just built: the type goes back to
   the empty string, isTrusted to false, and the initialized flag is UNSET. It lives here rather than in the
   factory because the slots are this component's and nothing outside it may write one. */
void event_uninitialize(JSContext *ctx, JSValueConst ev)
{
    JSValue slots = event_slots(ctx, ev);

    DCHECK(JS_IsObject(slots), "createEvent uninitialised something that is not an Event — a row's maker "
                               "must answer with an object carrying Event's slot record");
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return; }
    JS_SetPropertyStr(ctx, slots, "type", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, slots, "isTrusted", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "initialized", JS_FALSE);
    JS_FreeValue(ctx, slots);
}
void event_set_dispatch_flag(JSContext *ctx, JSValueConst ev, bool on)
{
    event_write_flag(ctx, ev, "dispatch", on);
}

/* §2.9's legacyOutputDidListenersThrowFlag — see event.h for why the dispatch's second output rides the event.
   It is NOT cleared by event_end_dispatch: its reader runs after the dispatch has returned. */
bool event_listeners_threw(JSContext *ctx, JSValueConst ev) { return event_read_flag(ctx, ev, "listenersThrew"); }
void event_set_listeners_threw(JSContext *ctx, JSValueConst ev, bool on)
{
    event_write_flag(ctx, ev, "listenersThrew", on);
}

/* §2.9 step 3: an event the PAGE dispatches is not trusted, whatever it was when it was constructed. */
void event_set_trusted(JSContext *ctx, JSValueConst ev, bool trusted)
{
    event_write_flag(ctx, ev, "isTrusted", trusted);
}

/* §2.9 dispatch steps 7-10, WHICH ARE ONE THING AND SO ARE ONE CALL: eventPhase back to NONE, currentTarget to
   null, the PATH to the empty list, and the dispatch, stop propagation and stop immediate propagation flags
   unset. `target` STAYS — a page reads it after dispatchEvent returns, which is the difference between the two.
   The three flags were not unset at all, and each omission is observable: the SAME event re-dispatched after a
   listener called stopPropagation propagated nowhere the second time, which is exactly what step 10 exists to
   prevent and what Event-dispatch-redispatch asks. */
void event_end_dispatch(JSContext *ctx, JSValueConst ev)
{
    JSValue slots = event_slots(ctx, ev);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return; }
    JS_SetPropertyStr(ctx, slots, "currentTarget", JS_NULL);
    JS_SetPropertyStr(ctx, slots, "eventPhase", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, slots, "path", JS_NULL);
    JS_SetPropertyStr(ctx, slots, "dispatch", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "stopPropagation", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "stopImmediate", JS_FALSE);
    JS_FreeValue(ctx, slots);
}

/* §2.9's EVENT PATH, which is the event's own state and not the dispatch's — `composedPath()` reads "this's
   path", so a walk that kept it privately could only ever answer with the one target it happened to be standing
   on. Set by dispatch as it appends (step 6.3 and step 6.8.7's append), cleared by step 9. */
void event_set_path(JSContext *ctx, JSValueConst ev, JSValueConst path)
{
    JSValue slots = event_slots(ctx, ev);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return; }
    JS_SetPropertyStr(ctx, slots, "path", JS_DupValue(ctx, path));
    JS_FreeValue(ctx, slots);
}

/* §2.2's COMPOSED FLAG, and §2.9's FIRST EVENT PATH ITEM — the two halves DOM §4.8's get-the-parent condition
   needs off the EVENT ("returns null if event's composed flag is unset and shadow root is the ROOT of event's
   path's first event path item's invocation target"). The other half of that sentence is a question about the
   TREE, and it is answered where nodes are known: this file must not learn what a node is, for the reason
   event_target.c's own header gives — naming the DOM here makes every host that installs events link lexbor. */
bool event_composed(JSContext *ctx, JSValueConst ev)
{
    return event_read_flag(ctx, ev, "composed");
}

JSValue event_path_first_invocation_target(JSContext *ctx, JSValueConst ev)
{
    JSValue slots = event_slots(ctx, ev), path, item, first;

    path = JS_GetPropertyStr(ctx, slots, "path");
    JS_FreeValue(ctx, slots);
    /* No path is not a state §4.8 has an answer for: get the parent is only ever asked from inside dispatch,
       which appends the target at step 6.3 before the first ask. */
    DCHECK(JS_IsArray(path), "§2.9's first event path item was asked for outside a dispatch — the event has no "
                             "path, so §4.8's composed-flag condition has no subject to be about");
    DCHECK(event_path_length(ctx, path) > 0,
           "§2.9's event path is empty inside a dispatch — step 6.3 appends the target before the first get the "
           "parent, so §4.8's condition would be about no item at all");
    item = event_path_item(ctx, path, 0);
    first = event_path_invocation_target(ctx, item);
    JS_FreeValue(ctx, item);
    JS_FreeValue(ctx, path);
    return first;
}

/* §2.2's IN PASSIVE LISTENER FLAG. "inner invoke" sets it around a listener whose `passive` is true and unsets
   it after, and `preventDefault` is defined to do nothing while it is set — which is the whole of what passive
   means to the page. */
void event_set_in_passive(JSContext *ctx, JSValueConst ev, bool on)
{
    event_write_flag(ctx, ev, "inPassive", on);
}

/* §2.9 "invoke" step 3 and step 7 — TWO writes at two different steps, and they were one call taking both.
   `target` is the path item's shadow-adjusted target and is set once for the whole walk; `currentTarget` is
   whose listeners are running and moves at every item. A single setter forced the walk to restate the target it
   was not changing, and passing a placeholder for it is how an event ends up with `target` undefined. */
void event_set_target(JSContext *ctx, JSValueConst ev, JSValueConst target)
{
    JSValue slots = event_slots(ctx, ev);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return; }
    JS_SetPropertyStr(ctx, slots, "target", JS_DupValue(ctx, target));
    JS_FreeValue(ctx, slots);
}

/* DOM §2.2's SET THE CANCELED FLAG — see event.h for why it is one function and not three spellings.
   "if event's cancelable attribute value is true and event's in passive listener flag is unset, then set
   event's canceled flag to true". A passive listener's cancel does NOTHING, whichever of the three spellings
   it reached for; that is the whole of the guarantee `{passive:true}` gives the user agent. */
void event_set_the_canceled_flag(JSContext *ctx, JSValueConst ev)
{
    if (event_read_flag(ctx, ev, "cancelable") && !event_read_flag(ctx, ev, "inPassive"))
        event_write_flag(ctx, ev, "canceled", true);
}

/* §2.2's currentTarget, read. The slot is JS_NULL between dispatches — §2.9 steps 7-10 put it back — so the
   caller that needs an object DCHECKs for one rather than this answering with a placeholder. */
JSValue event_current_target(JSContext *ctx, JSValueConst ev)
{
    JSValue slots = event_slots(ctx, ev), v;

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_NULL; }
    v = JS_GetPropertyStr(ctx, slots, "currentTarget");
    JS_FreeValue(ctx, slots);
    return v;
}

void event_set_current(JSContext *ctx, JSValueConst ev, JSValueConst current)
{
    JSValue slots = event_slots(ctx, ev);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return; }
    JS_SetPropertyStr(ctx, slots, "currentTarget", JS_DupValue(ctx, current));
    JS_FreeValue(ctx, slots);   /* the PHASE is the walk's to set — it knows which step of the path this is */
}

/* §2.2's RELATEDTARGET and TOUCH TARGET LIST, read and written as slots. §2.9 asks for them at step 4 and at
   every step 6.9.3 — once per ancestor — and `invoke` writes them at every path item, so the getters answer
   with the slot itself rather than a copy of a list nobody may mutate. */
static JSValue event_slot_get(JSContext *ctx, JSValueConst ev, const char *name)
{
    JSValue slots = event_slots(ctx, ev), v;

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_NULL; }
    v = JS_GetPropertyStr(ctx, slots, name);
    JS_FreeValue(ctx, slots);
    return v;
}

static void event_slot_set(JSContext *ctx, JSValueConst ev, const char *name, JSValueConst v)
{
    JSValue slots = event_slots(ctx, ev);

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return; }
    JS_SetPropertyStr(ctx, slots, name, JS_DupValue(ctx, v));
    JS_FreeValue(ctx, slots);
}

JSValue event_related_target(JSContext *ctx, JSValueConst ev)
{
    JSValue v = event_slot_get(ctx, ev, "relatedTarget");

    DCHECK(JS_IsObject(v) || JS_IsNull(v),
           "§2.2's relatedTarget is a POTENTIAL event target — an EventTarget or null — and §4.8's retargeting "
           "is about to be run against it. Anything else means an interface that defines a relatedTarget "
           "attribute wrote its own value into the slot instead of an EventTarget");
    return v;
}

void event_set_related_target(JSContext *ctx, JSValueConst ev, JSValueConst related)
{
    DCHECK(JS_IsObject(related) || JS_IsNull(related),
           "§2.2's relatedTarget was set to something that is not a potential event target");
    event_slot_set(ctx, ev, "relatedTarget", related);
}

JSValue event_touch_target_list(JSContext *ctx, JSValueConst ev)
{
    JSValue v = event_slot_get(ctx, ev, "touchTargets");

    DCHECK(JS_IsArray(v) || JS_IsNull(v),
           "§2.2's touch target list is a LIST of potential event targets — an Array, or null for the empty "
           "list, which is what every event that is not a TouchEvent carries");
    return v;
}

void event_set_touch_target_list(JSContext *ctx, JSValueConst ev, JSValueConst list)
{
    DCHECK(JS_IsArray(list) || JS_IsNull(list),
           "§2.2's touch target list was set to something that is neither a list nor the empty list");
    event_slot_set(ctx, ev, "touchTargets", list);
}

/* §2.9 step 11. The target survives every other dispatch — a page reads `ev.target` after dispatchEvent
   returns — and this is the one case where none of the three may: the outermost thing the event called its
   target was inside a shadow tree, so handing any of them back would hand out a node from a tree the page was
   never given. */
void event_clear_targets(JSContext *ctx, JSValueConst ev)
{
    JSValue slots = event_slots(ctx, ev);

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return; }
    JS_SetPropertyStr(ctx, slots, "target", JS_NULL);
    JS_SetPropertyStr(ctx, slots, "relatedTarget", JS_NULL);
    JS_SetPropertyStr(ctx, slots, "touchTargets", JS_NULL);
    JS_FreeValue(ctx, slots);
}

/* §2.2 "initialize": the slot record every Event carries. `isTrusted` is the one thing that distinguishes an
   event the ENGINE fired from one the page constructed, and it is the reason this is a parameter rather than a
   constant — a page checks it. */
static JSValue event_make_proto(JSContext *ctx, JSValueConst proto, JSValueConst type, bool bubbles,
                                bool cancelable, bool composed, bool trusted)
{
    JSValue ev, slots;
    JSAtom k;

    DCHECK(g_ready, "an Event was minted before event_init ran");
    ev = JS_NewObjectProto(ctx, proto);
    if (JS_IsException(ev))
        return ev;
    slots = idl_slots_new(ctx);
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(!JS_IsException(slots) && k != JS_ATOM_NULL, "the Event slot record allocation failed");
    JS_SetPropertyStr(ctx, slots, "type", JS_DupValue(ctx, type));
    JS_SetPropertyStr(ctx, slots, "target", JS_NULL);
    /* §2.2: every event has a relatedTarget and a touch target list, "unless stated otherwise" null and the
       empty list. They are initialised here rather than left absent for the reason `path` is — §2.9 step 4
       retargets the relatedTarget of EVERY event it dispatches, so this read must be a slot and never a miss. */
    JS_SetPropertyStr(ctx, slots, "relatedTarget", JS_NULL);
    JS_SetPropertyStr(ctx, slots, "touchTargets", JS_NULL);
    JS_SetPropertyStr(ctx, slots, "currentTarget", JS_NULL);
    JS_SetPropertyStr(ctx, slots, "eventPhase", JS_NewInt32(ctx, 0));   /* NONE until it is dispatched */
    JS_SetPropertyStr(ctx, slots, "bubbles", JS_NewBool(ctx, bubbles));
    JS_SetPropertyStr(ctx, slots, "cancelable", JS_NewBool(ctx, cancelable));
    JS_SetPropertyStr(ctx, slots, "composed", JS_NewBool(ctx, composed));
    JS_SetPropertyStr(ctx, slots, "isTrusted", JS_NewBool(ctx, trusted));
    /* §2.2's timeStamp, initialized by THREE algorithms that name TWO different HIGH RESOLUTION TIME
     * operations — core/timing/hr_time.h carries that standard's number and title for each. This comment used
     * to attribute both sentences to §2.5, which is the wrong SECTION for one of them and the wrong OPERATION
     * for the other.
     *   - DOM §2.5 Constructing events' INNER EVENT CREATION STEPS step 3: "Initialize event's timeStamp
     *     attribute to the RELATIVE HIGH RESOLUTION COARSE TIME given time and event's relevant global
     *     object." That is what a constructor runs (§2.5's constructor step 1 passes `now`, which the spec
     *     leaves as plain prose rather than a defined term) and what `create an event` runs (its step 3 passes
     *     "the time of the occurrence that the event is signaling").
     *   - DOM §4.5 Interface Document's createEvent() step 7: "Initialize event's timeStamp attribute to the
     *     result of calling CURRENT HIGH RESOLUTION TIME with this's relevant global object."
     * ONE CALL ANSWERS ALL THREE HERE, and the reason is arithmetic rather than convenience: current high
     * resolution time IS the relative high resolution time of the unsafe shared current time, which IS the
     * relative high resolution coarse time of that moment coarsened at this environment's own grid. So the two
     * operations differ only in WHICH moment they are given, and every event this engine mints is minted at
     * the moment of its own occurrence — the fire is a task, and the task ran now.
     * THE OCCURRENCE MOMENT IS NOT A PARAMETER OF THIS MINT, which is the narrowing to watch: an event whose
     * occurrence genuinely precedes its mint (an input event queued while a flow was parked) would want §2.5's
     * `time` threaded from the caller, and it would show as a timestamp later than the occurrence a page
     * correlates it against. Nothing in this engine mints one yet.
     * `ctx` IS the relevant global object — an Event is minted in the realm whose algorithm is firing it, which
     * is what makes a child document's `event.timeStamp` its own environment's time origin and its own
     * coarsening grid rather than the root's. */
    {
        JSValue ts = hr_time_current(ctx);

        /* THE PRODUCER'S SHAPE IS CHECKED AT THE CONSUMER, never defaulted. §2.2 types this attribute
           DOMHighResTimeStamp, whose typedef is a double, and the duration hr_time_current answers is a number
           or — when the clock stands at a moment nothing computed (core/timing/event_loop.h: a timer whose
           delay was unknown external input) — a derivation of one. Anything else means the producer answered
           with something that is not a duration, and a page reading `ev.timeStamp` would get it as a plausible
           datum rather than as a crash. */
        DCHECK(JS_IsNumber(ts) || concolic_is(ts),
               "an Event's timeStamp is neither a DOMHighResTimeStamp nor a derivation of one — "
               "current high resolution time answers the duration from this environment's time origin to the "
               "unsafe shared current time, and both ends of that come off the event loop's clock");
        JS_SetPropertyStr(ctx, slots, "timeStamp", ts);
    }
    JS_SetPropertyStr(ctx, slots, "canceled", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "stopPropagation", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "stopImmediate", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "dispatch", JS_FALSE);
    /* §2.2's PATH — the empty list until a dispatch builds one, and the empty list again after step 9. It is
       initialised here rather than left absent so composedPath's read is a slot and never a miss. */
    JS_SetPropertyStr(ctx, slots, "path", JS_NULL);
    /* §2.2's IN PASSIVE LISTENER FLAG, unset until "inner invoke" raises it around a passive listener. */
    JS_SetPropertyStr(ctx, slots, "inPassive", JS_FALSE);
    /* §2.2's INITIALIZED FLAG. Every event that arrives through a constructor or through the engine's own
       firing is initialized; the ONE thing that produces an uninitialized event is §4.5's createEvent, and
       the flag is the only observable difference between what it returns and what `new Event("")` returns —
       dispatching one before initEvent is an InvalidStateError. */
    JS_SetPropertyStr(ctx, slots, "initialized", JS_TRUE);
    JS_SetProperty(ctx, ev, k, slots);
    JS_FreeAtom(ctx, k);
    return ev;
}

static JSValue event_make(JSContext *ctx, JSValueConst type, bool bubbles, bool cancelable,
                          bool composed, bool trusted)
{
    JSValue proto = event_proto(ctx);
    JSValue ev = event_make_proto(ctx, proto, type, bubbles, cancelable, composed, trusted);
    JS_FreeValue(ctx, proto);
    return ev;
}

JSValue event_new_derived(JSContext *ctx, JSValue proto, JSValueConst type,
                          bool bubbles, bool cancelable, bool composed, bool trusted)
{
    JSValue ev;

    DCHECK(JS_IsObject(proto), "a derived event was minted with no prototype — the base steps put the object "
                               "on the DERIVED interface's prototype, which is what makes it an instance of it");
    ev = event_make_proto(ctx, proto, type, bubbles, cancelable, composed, trusted);
    JS_FreeValue(ctx, proto);   /* CONSUMED — see event.h: the caller's `<Interface>_proto(ctx)` returns owned */
    return ev;
}

JSValue event_new(JSContext *ctx, const char *type, bool bubbles, bool cancelable)
{
    JSValue t = JS_NewString(ctx, type);
    JSValue ev = event_make(ctx, t, bubbles, cancelable, false, /*trusted*/ true);
    JS_FreeValue(ctx, t);
    return ev;
}

/* The same event with isTrusted FALSE — §3.2.2's synthetic click, which the spec says is untrusted because the
   page and not the user caused it, and which a page checks before acting on one. */
JSValue event_new_untrusted(JSContext *ctx, const char *type, bool bubbles, bool cancelable)
{
    JSValue t = JS_NewString(ctx, type);
    JSValue ev = event_make(ctx, t, bubbles, cancelable, false, /*trusted*/ false);
    JS_FreeValue(ctx, t);
    return ev;
}

/* §2.2 the read-only attributes, every one over a SLOT rather than over a property the page can assign.
   magic indexes SLOT_NAME. */
static const char *const SLOT_NAME[] = {
    "type", "target", "currentTarget", "eventPhase", "bubbles", "cancelable", "composed", "isTrusted",
    "timeStamp",
};

static JSValue js_event_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = event_slots(ctx, this_val), v;

    DCHECK(magic >= 0 && magic < (int)(sizeof(SLOT_NAME) / sizeof(SLOT_NAME[0])),
           "an Event attribute was declared with a magic the slot table does not name");
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "an Event attribute was read on something that is not an Event");
    }
    v = JS_GetPropertyStr(ctx, slots, SLOT_NAME[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* §2.2 defaultPrevented is the CANCELED flag, and srcElement is target under its legacy name. */
static JSValue js_event_derived(JSContext *ctx, JSValueConst this_val, int magic)
{
    if (magic == 0) return JS_NewBool(ctx, event_canceled(ctx, this_val));
    DCHECK(magic == 1, "an Event derived attribute was declared with a magic this file does not name");
    return js_event_get(ctx, this_val, 1);   /* srcElement */
}

/* §2.2 cancelBubble — a getter AND a setter, and the setter only ever sets. `ev.cancelBubble = false` does
   nothing at all, which is the legacy behaviour the spec writes out and which a page relies on. */
static JSValue js_event_get_cancel_bubble(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    return JS_NewBool(ctx, event_read_flag(ctx, this_val, "stopPropagation"));
}

static JSValue js_event_set_cancel_bubble(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    (void)magic;
    if (JS_ToBool(ctx, val))
        event_write_flag(ctx, this_val, "stopPropagation", true);
    return JS_UNDEFINED;
}

/* §2.2 returnValue — the inverse legacy spelling of defaultPrevented: reading it is "not canceled", and
   assigning FALSE cancels. Assigning true does nothing, which is again what the spec writes out. */
static JSValue js_event_get_return_value(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    return JS_NewBool(ctx, !event_canceled(ctx, this_val));
}

static JSValue js_event_set_return_value(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    (void)magic;
    /* "The returnValue setter steps are to SET THE CANCELED FLAG with this if the given value is false" — the
       named algorithm, not a second copy of its condition. This body spelled the condition itself and had only
       half of it: `cancelable` without the in-passive-listener flag, so a `{passive:true}` listener cancelled
       through this spelling what preventDefault() in the same listener could not. */
    if (!JS_ToBool(ctx, val))
        event_set_the_canceled_flag(ctx, this_val);
    return JS_UNDEFINED;
}

/* §2.2 the three flag methods. Each writes its own flag, which is all the spec says they do — they were ONE
   shared no-op, and a no-op preventDefault is not a small inaccuracy: whether the default action was cancelled
   is the one thing dispatchEvent reports, so a page branching on it was reading a constant.
   magic: 0 = preventDefault, 1 = stopPropagation, 2 = stopImmediatePropagation. */
static JSValue js_event_flag(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv;
    if (!event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "an Event method was called on something that is not an Event");
    if (magic == 0) {
        /* §2.2: "The preventDefault() method steps are to set the canceled flag with this." */
        event_set_the_canceled_flag(ctx, this_val);
        return JS_UNDEFINED;
    }
    event_write_flag(ctx, this_val, "stopPropagation", true);
    if (magic == 2)
        event_write_flag(ctx, this_val, "stopImmediate", true);
    return JS_UNDEFINED;
}

/* §2.2 composedPath — THIS'S PATH, which §2.9 built and which this file now holds. It used to answer with the
   current target alone, because there was no walk to have a path from; there is one, and answering a walk's
   whole path with its last entry is not a smaller answer, it is a different one.
 *
 * THE ALGORITHM IS ABOUT CLOSED SHADOW TREES AND NOTHING ELSE, and it is written as three walks because a
 * listener may only see the parts of the path its own tree is allowed to see. It finds the CURRENT TARGET's
 * index by scanning the path BACKWARD from the end, counting how deep inside closed trees that target sits
 * (up one at every root-of-closed-tree it passes, down one at every slot-in-closed-tree); then it walks outward
 * in both directions from that index, emitting an entry only while the running level is no deeper than the
 * level the current target itself is at. A `maxHiddenLevel` that only ever DECREASES is what makes leaving a
 * closed tree permanent — once the walk has climbed out through a slot, everything further out that is deeper
 * again stays hidden.
 * The two outward walks are mirror images with the two flags SWAPPED, because going outward from the target the
 * boundary you cross first is a root going one way and a slot going the other. Writing one loop for both is how
 * this gets subtly wrong; they are written out. */
static JSValue js_event_composed_path(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue arr, path, slots, current;
    uint32_t n, out = 0;
    int32_t index, current_index = 0;
    int hidden, current_hidden, max_hidden;

    (void)argc; (void)argv;
    if (!event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "composedPath called on something that is not an Event");
    arr = JS_NewArray(ctx);          /* step 1 */
    slots = event_slots(ctx, this_val);
    path = JS_GetPropertyStr(ctx, slots, "path");   /* step 2 */
    current = JS_GetPropertyStr(ctx, slots, "currentTarget");
    JS_FreeValue(ctx, slots);
    /* step 3: an event that is not being dispatched has an empty path, and composedPath answers the empty list.
       Its `path` is JS_NULL rather than an empty Array, which is the same state said in one fewer allocation —
       and it is why this asks whether there is a path at all before asking the path how long it is. */
    n = JS_IsArray(path) ? event_path_length(ctx, path) : 0;
    if (n == 0) {
        JS_FreeValue(ctx, path);
        JS_FreeValue(ctx, current);
        return arr;
    }
    /* steps 4-6. The assert is the standard's own step 5: a non-empty path means a dispatch is in flight, and
       §2.9's `invoke` initialises currentTarget at every item it invokes. */
    DCHECK(JS_IsObject(current), "§2.2 composedPath step 5: the event has a path but no currentTarget — the "
                                 "walk is standing on an item whose invocation target it never published");
    /* steps 7-10: find the current target's index, and the closed-tree depth it sits at, scanning BACKWARD. */
    current_hidden = 0;
    for (index = (int32_t)n - 1; index >= 0; index--) {
        JSValue item = event_path_item(ctx, path, (uint32_t)index);
        JSValue invocation = event_path_invocation_target(ctx, item);
        bool is_current = JS_VALUE_GET_PTR(invocation) == JS_VALUE_GET_PTR(current);

        if (event_path_root_of_closed_tree(ctx, item)) current_hidden++;
        if (is_current) current_index = index;
        else if (event_path_slot_in_closed_tree(ctx, item)) current_hidden--;
        JS_FreeValue(ctx, invocation);
        JS_FreeValue(ctx, item);
        if (is_current) break;
    }
    /* steps 11-13: OUTWARD toward path[0], PREPENDING each kept entry. The walk runs from the current target
       DOWN through the indices, so it MEETS the entries in the reverse of the order the answer holds them —
       which is exactly what "prepend" says. They are collected in walk order and then read back in reverse into
       the front of the answer, rather than shifting the answer along by one per entry. */
    hidden = max_hidden = current_hidden;
    {
        uint32_t kept = 0, k;
        JSValue before = JS_NewArray(ctx);

        CHECK(!JS_IsException(before), "§2.2 composedPath's inner half could not be allocated");
        for (index = current_index - 1; index >= 0; index--) {
            JSValue item = event_path_item(ctx, path, (uint32_t)index);

            if (event_path_root_of_closed_tree(ctx, item)) hidden++;
            if (hidden <= max_hidden)
                JS_SetPropertyUint32(ctx, before, kept++, event_path_invocation_target(ctx, item));
            if (event_path_slot_in_closed_tree(ctx, item)) {
                hidden--;
                if (hidden < max_hidden) max_hidden = hidden;
            }
            JS_FreeValue(ctx, item);
        }
        for (k = kept; k-- > 0; )
            JS_SetPropertyUint32(ctx, arr, out++, JS_GetPropertyUint32(ctx, before, k));
        JS_FreeValue(ctx, before);
    }
    /* step 6, in its place in the ANSWER rather than in the algorithm's order: composedPath is built by
       appending the current target and then prepending the inner half, and the two together are the entries up
       to and including it, in path order. */
    JS_SetPropertyUint32(ctx, arr, out++, JS_DupValue(ctx, current));
    /* steps 14-16: outward toward the root, APPENDING, with the two flags swapped. */
    hidden = max_hidden = current_hidden;
    for (index = current_index + 1; index < (int32_t)n; index++) {
        JSValue item = event_path_item(ctx, path, (uint32_t)index);

        if (event_path_slot_in_closed_tree(ctx, item)) hidden++;
        if (hidden <= max_hidden)
            JS_SetPropertyUint32(ctx, arr, out++, event_path_invocation_target(ctx, item));
        if (event_path_root_of_closed_tree(ctx, item)) {
            hidden--;
            if (hidden < max_hidden) max_hidden = hidden;
        }
        JS_FreeValue(ctx, item);
    }
    JS_FreeValue(ctx, current);
    JS_FreeValue(ctx, path);
    return arr;   /* step 17 */
}

/* §2.2 initEvent(type, bubbles, cancelable) — the legacy initializer, and it is NOT a no-op: it re-initialises
   an event that is not currently being dispatched. The booleans are ToBoolean, which is total and runs none of
   the page's code, so only `type` is a coercion and the shared machine performs it. */
/* §2.2's initialise-an-existing-event steps, without the receiver check — a DERIVED interface's legacy
   initialiser (`initMessageEvent`) runs exactly these and then its own, so they are ONE implementation rather
   than a second copy that drifts. Returns false when the dispatch flag says the walk owns the event right now,
   which is the spec's early return and which the derived initialiser must honour before touching its own. */
bool event_reinit(JSContext *ctx, JSValueConst ev, JSValueConst type, bool bubbles, bool cancelable)
{
    JSValue slots = event_slots(ctx, ev);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return false; }
    /* §2.2 step 1: "If this's dispatch flag is set, then return." */
    if (slot_flag(ctx, slots, "dispatch")) { JS_FreeValue(ctx, slots); return false; }
    JS_SetPropertyStr(ctx, slots, "type",
                      JS_IsUndefined(type) ? JS_NewString(ctx, "undefined") : JS_DupValue(ctx, type));
    JS_SetPropertyStr(ctx, slots, "bubbles", JS_NewBool(ctx, bubbles));
    JS_SetPropertyStr(ctx, slots, "cancelable", JS_NewBool(ctx, cancelable));
    /* the spec's own re-init: the flags and the target go back to their initial state. */
    JS_SetPropertyStr(ctx, slots, "canceled", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "stopPropagation", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "stopImmediate", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "target", JS_NULL);
    /* §2.2 step 2: "Set this's initialized flag." An event built by createEvent becomes dispatchable here
       and nowhere else, which is what makes the legacy factory's two-call shape work at all. */
    JS_SetPropertyStr(ctx, slots, "initialized", JS_TRUE);
    JS_FreeValue(ctx, slots);
    return true;
}

static JSValue js_event_init_event(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    if (!event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "initEvent called on something that is not an Event");
    event_reinit(ctx, this_val, argc > 0 ? argv[0] : JS_UNDEFINED,
                 argc > 1 && JS_ToBool(ctx, argv[1]), argc > 2 && JS_ToBool(ctx, argv[2]));
    return JS_UNDEFINED;
}

/* THE CONSTRUCTOR — `new Event(type, optional EventInit eventInit = {})`. A step machine because BOTH of its
   arguments are the page's code: `type` is a DOMString (ToString), and EventInit is three property reads that
   an accessor or a Proxy trap turns into a call. It declares itself through the shared IDL machine rather than
   hand-rolling either, which is why there is no coercion code here at all — by the time this body runs, `type`
   is a real string and the dictionary is a plain object the engine built. */
static const IdlArgType EVENT_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember EVENT_INIT[] = {   /* EventInit, in the order the IDL declares it */
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
};

static JSValue js_event_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;

    (void)magic;
    /* JS_CFUNC_step_ctor delivers NEW_TARGET in the receiver slot and undefined for a plain call, which is how
       `Event('x')` is told apart from `new Event('x')` — the IDL declares a constructor, so the former throws. */
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor Event requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Event constructor requires a type");
    /* §2.2: an event the PAGE constructs is not trusted, which is the whole point of the flag. */
    return event_make(ctx, argv[0],
                      idl_dict_bool(ctx, init, "bubbles"),
                      idl_dict_bool(ctx, init, "cancelable"),
                      idl_dict_bool(ctx, init, "composed"),
                      /*trusted*/ false);
}

static const JSCFunctionListEntry js_event_proto[] = {
    JS_CGETSET_MAGIC_DEF("type", js_event_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("target", js_event_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("currentTarget", js_event_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("eventPhase", js_event_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("bubbles", js_event_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("cancelable", js_event_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("composed", js_event_get, NULL, 6),
    JS_CGETSET_MAGIC_DEF("isTrusted", js_event_get, NULL, 7),
    JS_CGETSET_MAGIC_DEF("timeStamp", js_event_get, NULL, 8),
    JS_CGETSET_MAGIC_DEF("defaultPrevented", js_event_derived, NULL, 0),
    JS_CGETSET_MAGIC_DEF("srcElement", js_event_derived, NULL, 1),
    JS_CFUNC_MAGIC_DEF("preventDefault", 0, js_event_flag, 0),
    JS_CFUNC_MAGIC_DEF("stopPropagation", 0, js_event_flag, 1),
    JS_CFUNC_MAGIC_DEF("stopImmediatePropagation", 0, js_event_flag, 2),
    JS_CFUNC_DEF("composedPath", 0, js_event_composed_path),
};

/* §2.2 the phase constants. Web IDL puts a `const` on the prototype AND the interface object, so one table
   installs both — reached by NAME rather than by slicing the tail off the member list, which is a silent break
   the day a member is appended. */
static const JSCFunctionListEntry js_event_consts[] = {
    JS_PROP_INT32_DEF("NONE", 0, 0),
    JS_PROP_INT32_DEF("CAPTURING_PHASE", 1, 0),
    JS_PROP_INT32_DEF("AT_TARGET", 2, 0),
    JS_PROP_INT32_DEF("BUBBLING_PHASE", 3, 0),
};

/* THE SUBCLASSES ARE DECLARED HERE, WITH THE INTERFACE THEY EXTEND, and that is not tidying. Each host had its
 * own copy of the list — `event_init(ctx); message_event_init(ctx); error_event_init(ctx);` — which is the
 * hand-picked list CLAUDE.md warns about, and it had already gone wrong: test_forced.c declared Event and
 * ErrorEvent and NOT MessageEvent, so a `message` event in that host was a bare Event and `new MessageEvent`
 * was a missing global, silently, in the one host the smoke test runs.
 *
 * WHY THIS IS THE RIGHT PLACE rather than a fourth list somewhere: an Event subclass's prototype chains to
 * Event.prototype, so it is meaningless without this interface and can never be wanted separately. A build that
 * has Event has all of them. The one list every host already goes through is therefore this function, and a
 * subclass added to the tree is added HERE — one edit, and no host can be missing it.
 *
 * ORDER: after this interface's own class and members, because each subclass declares a prototype whose chain
 * ends at Event.prototype and reads the ids declared above. */
static void event_declare_subclasses(JSContext *ctx);
static void event_free_subclasses(JSRuntime *rt);

void event_init(JSContext *ctx)
{
    JSClassDef d = { "Event" };
    static const IdlArgType INIT_ARGS[3] = { IDL_DOMSTRING, IDL_ANY, IDL_ANY };

    DCHECK(!g_ready, "event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "eventSlots", false);
    CHECK(!JS_IsException(g_key), "the Event slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_event_class);
    JS_NewClass(JS_GetRuntime(ctx), g_event_class, &d);
    /* cancelBubble and returnValue are the two LEGACY attributes with setters, and each setter only ever sets
       — which is the behaviour that makes them legacy and the reason they are not aliases. */
    g_cancel_bubble_setid = idl_setter_id(ctx, IDL_ANY, false, js_event_set_cancel_bubble, 0);
    g_return_value_setid  = idl_setter_id(ctx, IDL_ANY, false, js_event_set_return_value, 0);
    g_init_event_id = idl_method_id(ctx, INIT_ARGS, 3, js_event_init_event, 0);
    idl_optional_from(1);   /* §2.2: `initEvent(type, optional bubbles, optional cancelable)` */
    g_ctor_stepid = idl_method_id_dict(ctx, EVENT_CTOR_ARGS, 2, EVENT_INIT,
                                      (int)(sizeof(EVENT_INIT) / sizeof(EVENT_INIT[0])),
                                      js_event_ctor, 0);
    idl_optional_from(1);   /* §2.2: `constructor(DOMString type, optional EventInit eventInitDict = {})` */
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. This row's own seven; the
       thirteen subclasses declare THEIR slots under this same name, because a sub-component names the row
       whose RELEASE gives its slots back and event_free is what reaches every one of them. Until this row had
       a release column at all, none of the sixty-six could be declared: platform_check_agent_state fires on a
       row with no release that declared agent state, which is why the two halves of this diff are ordered. */
    agent_state_flag("event", &g_ready, "DOM §2.2 Interface Event's declaration latch");
    agent_state_class("event", &g_event_class,
                      "DOM §2.2 Interface Event's class, held for its per-realm prototype slot");
    agent_state_value("event", &g_key, "the private Symbol DOM §2.2 Interface Event's slot record hangs off");
    agent_state_id("event", &g_ctor_stepid,
                   "DOM §2.2 Interface Event's `constructor(DOMString type, optional EventInit eventInitDict "
                   "= {})`");
    agent_state_id("event", &g_cancel_bubble_setid, "DOM §2.2 Interface Event's legacy `cancelBubble` setter");
    agent_state_id("event", &g_return_value_setid, "DOM §2.2 Interface Event's legacy `returnValue` setter");
    agent_state_id("event", &g_init_event_id,
                   "DOM §2.2 Interface Event's `initEvent(type, optional bubbles, optional cancelable)`");
    realm_declare_intrinsic(event_install_proto);
    event_declare_subclasses(ctx);
}

/* §2.2's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM — run where a realm's other intrinsics are added, exactly
   once per realm. */
void event_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_ready, "a realm asked for Event.prototype before event_init declared the interface");
    prev = JS_GetClassProto(ctx, g_event_class);
    DCHECK(JS_IsNull(prev),
           "event_install_proto ran twice in one realm — §3.7 gives a realm ONE Event.prototype, and a second "
           "would leave every event already chained to the first answering out of a discarded one");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Event.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Event");
    JS_SetPropertyFunctionList(ctx, proto, js_event_proto,
                               (int)(sizeof(js_event_proto) / sizeof(js_event_proto[0])));
    JS_SetPropertyFunctionList(ctx, proto, js_event_consts,
                               (int)(sizeof(js_event_consts) / sizeof(js_event_consts[0])));
    idl_install_accessor(ctx, proto, "cancelBubble", js_event_get_cancel_bubble, 0, g_cancel_bubble_setid);
    idl_install_accessor(ctx, proto, "returnValue", js_event_get_return_value, 0, g_return_value_setid);
    /* Web IDL §3.7.7 Operations: "Let length be the length of the shortest argument list in the entries in S",
       over the effective overload set computed "with argument count 0". §2.2's
       `initEvent(DOMString type, optional boolean bubbles = false, optional boolean cancelable = false)` has
       two trailing optional arguments, so §2.5.8 Overloading's step 5.9 loop puts entries of length 2 and 1 in
       S and stops at the required `type` — the shortest is 1, which is the same number the declaration above
       already states as `idl_optional_from(1)`. The 3 that stood here was the DECLARED arity, which is what
       §3.7.7 explicitly is not. */
    idl_install_method(ctx, proto, "initEvent", 1, g_init_event_id);
    JS_SetClassProto(ctx, g_event_class, proto);   /* the realm owns it from here */
}

JSValue event_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_event_class);
    DCHECK(!JS_IsNull(proto),
           "Event.prototype was asked for in a realm that never ran event_install_proto — a realm whose "
           "intrinsics were not all installed answers a derived interface's chain out of another document");
    return proto;   /* OWNED */
}

void event_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ready, "Event was installed before event_init built its prototype");
    /* A step machine that is also a CONSTRUCTOR: `new Event(type, init)` converts two page-reachable arguments
       before the body runs, and JS_CFUNC_step_ctor is what makes the declaration usable with `new`. */
    ctor = idl_step_constructor(ctx, "Event", 2, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the Event interface object could not be allocated");
    {
        JSValue proto = event_proto(ctx);
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyFunctionList(ctx, ctor, js_event_consts,
                               (int)(sizeof(js_event_consts) / sizeof(js_event_consts[0])));
    JS_SetPropertyStr(ctx, (JSValue)global, "Event", ctor);
}

static void event_declare_subclasses(JSContext *ctx)
{
    message_event_init(ctx);
    error_event_init(ctx);
    page_transition_event_init(ctx);
    /* HTML §7.2.7.2 and §7.2.7.3 — the two events a SESSION HISTORY TRAVERSAL fires. They are declared here
       rather than from core/frame/session_history.c for the same reason every other Event subclass is: their
       prototypes chain to this realm's Event.prototype, and realm.h runs the intrinsics in declaration order. */
    pop_state_event_init(ctx);
    hash_change_event_init(ctx);
    /* HTML §7.2.7.1 — the event a NAVIGATION API entry-list change fires. It is declared here for the same
       reason: its prototype chains to this realm's Event.prototype. Its `from` member brands against
       core/frame/navigation_history_entry.c's class, which core/platform.c declares before this row. */
    navigation_current_entry_change_event_init(ctx);
    /* HTML §7.2.6.10.1 — the event a NAVIGATION fires before it commits. Declared here for the same reason and
       with a second one of its own: its `destination` member brands against core/frame/navigation_destination.c's
       class and its `signal` against core/dom/abort.c's, and core/platform.c declares the first of those before
       this row while the second comes AFTER it — which is why NavigateEvent reads both class ids at its
       PER-REALM install rather than here. A class id is agent-scoped, so reading it later reads the same one. */
    navigate_event_init(ctx);
    before_unload_event_init(ctx);
    /* HTML §12.2.4 — the event a WEB STORAGE broadcast fires at the other same-origin documents. Declared here
       for the same reason as the rest: its prototype chains to this realm's Event.prototype. Its `storageArea`
       member brands against core/storage/storage.c's class, which core/platform.c declares BEFORE this row —
       so unlike NavigateEvent's two, it can be read at the declaration, and storage_event_init asserts it. */
    storage_event_init(ctx);
    /* THE ORDER IS THE CHAIN. Each of these declares a per-realm install and realm.h runs them in declaration
       order, so an interface must declare AFTER the one it extends or its prototype chains to a slot no realm
       has filled yet: `MouseEvent : UIEvent : Event`, `KeyboardEvent : UIEvent : Event` and
       `FocusEvent : UIEvent : Event`. */
    ui_event_init(ctx);
    mouse_event_init(ctx);
    keyboard_event_init(ctx);
    focus_event_init(ctx);
}

/* REVERSE DECLARATION ORDER, which is the rule core/platform.c's release column runs by and is the rule here
   for the same reason: the list above is ordered BY THE CHAIN (`FocusEvent : UIEvent : Event`), so undoing it
   forwards would release the interface an interface extends before the interface itself. Nothing in this
   family reads another's agent state at its release today — each gives back its own Symbol, its own class and
   its own member declarations — so what this buys is that the day one of them does, the order is already the
   one it needs, decided by a rule instead of by two authors happening to agree. */
static void event_free_subclasses(JSRuntime *rt)
{
    focus_event_free(rt);
    keyboard_event_free(rt);
    mouse_event_free(rt);
    ui_event_free(rt);
    storage_event_free(rt);
    before_unload_event_free(rt);
    navigate_event_free(rt);
    navigation_current_entry_change_event_free(rt);
    hash_change_event_free(rt);
    pop_state_event_free(rt);
    page_transition_event_free(rt);
    error_event_free(rt);
    message_event_free(rt);
}

/* THE RUNTIME, NOT A REALM — this is core/platform.h's release column, and what it gives back is the AGENT's.
   It was a line in each of THREE hosts' hand-written teardowns instead, written after platform_agent_free had
   already run the whole column, and the three did not agree on where: main.c and test_forced.c had
   `realm_intrinsics_free(); report_exception_free(ctx); event_free(ctx);` while wpt_runner.c had
   `report_exception_free(ctx); event_free(ctx); realm_intrinsics_free();`. Nothing was missing and the order
   was still three different answers — which is the whole of why that column exists.
   AND WHAT THE ROW BUYS BEYOND THE DRIFT: a row with an empty release column may declare no agent state
   (platform_check_agent_state), so the sixty-six slots this component and its thirteen subclasses hold could
   not be declared at all while the release lived out there, and every one of them — fourteen class ids among
   them — was given back by a line this file could not see and asserted by nothing. */
void event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. The release is the inverse of the DECLARATION and rides the same row of
       core/platform.c's one list, whose declare pass is unconditional and whose table asserts that a release
       row has a declare — so an early return here would silently absorb a teardown reached without an init
       instead of naming it. */
    DCHECK(g_ready, "DOM §2.2 Interface Event was released in an agent that never declared it — core/platform.c "
                    "runs its declare pass unconditionally, so there is no arm that reaches here undeclared");
    event_free_subclasses(rt);
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because the id doubles
       as the init latch and a carried one names a class in a runtime that is gone. Nothing WEARS this class —
       event_make_proto mints every event in this engine through JS_NewObjectProto, so JS_GetClassID of an
       Event is JS_CLASS_OBJECT — so there is no finalizer and no gc_mark here to owe the JS_GetAnyOpaque the
       zeroing costs a component whose objects do wear one. */
    g_event_class = 0;
    g_ctor_stepid = -1;
    g_cancel_bubble_setid = g_return_value_setid = g_init_event_id = -1;
}
