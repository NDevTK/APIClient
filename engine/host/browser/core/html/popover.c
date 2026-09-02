/* THE POPOVER API — HTML §6.12 "The popover attribute".
 *
 *     partial interface HTMLElement {
 *       undefined showPopover(optional ShowPopoverOptions options = {});
 *       undefined hidePopover();
 *       boolean togglePopover(optional (TogglePopoverOptions or boolean) options = {});
 *       [CEReactions] attribute DOMString? popover;
 *     };
 *     dictionary ShowPopoverOptions { HTMLElement source; };
 *     dictionary TogglePopoverOptions : ShowPopoverOptions { boolean force; };
 *
 * NOTE WHAT THE PUBLISHED IDL DOES **NOT** SAY: none of the three METHODS carries `[CEReactions]`. Only the
 * `popover` attribute does, and that is core/html/html_element.c's reflection row. A member declared with a
 * reaction bracket it does not have would run §4.13.5 Upgrades' queue at a point the standard does not.
 *
 * WHY IT IS THE §Headless-is-not-valueless CASE IN ITS CLEAREST FORM. A popover has no IO device in it at all:
 * §6.12 is a VISIBILITY STATE, an ORDERED STACK (CSS Positioned Layout Level 4 §3's top layer, which
 * core/css/top_layer.c owns), a re-entrancy guard, and two events. Every one of those is computable with
 * nothing drawn, so there is no honest place here for a shape-only method — a `showPopover` that returned
 * undefined without moving the state would make `el.togglePopover()` answer false forever and every
 * `beforetoggle` handler in the bundle dead code, which is a WRONG answer rather than a missing one.
 *
 * ALL THREE MEMBERS ARE STEP MACHINES, and they have to be twice over. §6.12's show popover step 9 and hide a
 * popover step 12 FIRE `beforetoggle` — the page's own handlers, a loop, an `await`, a DOM mutation — and a C
 * activation hosting those is the drive-to-completion this engine aborts on. And `showPopover(options)` reads
 * `options["source"]`, which is a [[Get]] the page can answer with an accessor. So the three are ONE machine
 * with a magic: `togglePopover` runs at most one of show and hide, so one state serves all three and there is
 * no second implementation of either algorithm for the two to disagree about.
 *
 * THE STATE IS PER-FLOW BY CONSTRUCTION, exactly as core/html/html_dialog.c's is. §6.12's per-element facts —
 * the popover visibility state, the popover trigger, the popover hiding boolean, the popover toggle task
 * tracker, the previously focused element — and its per-Document facts — showing popover, hiding popover
 * nesting count — all live in own slots under Symbols this file mints and never publishes. A slot written as a
 * property write is captured by the COW delta, so a forked arm that showed a popover and one that did not each
 * read back their own state, and a parked flow resumes with the one it had.
 *
 * WHAT IS HONESTLY ABSENT, AND WHERE IT CRASHES. The AUTO and HINT states need TWO mechanisms this build has
 * none of — §6.12's hide popover stack until, and its topmost popover ancestor (a FLAT TREE descendant test).
 * That arm is show popover step 15, and it DFAILs there naming both: the split is the SPEC'S OWN and not a
 * convenience, because a Manual popover enters none of it — step 15's whole block, hide a popover step 11's
 * whole block, and every read of the showing auto/hint popover lists are reachable only from it.
 * `<div popover=manual>` is therefore complete here and `<div popover>` (whose empty value default is Auto) is
 * a named crash rather than a wrong answer.
 *   IT WAS THREE, AND THE THIRD WAS §6.10 Close requests and close watchers' close watcher, which step 15's
 * block establishes as its last sub-step. core/html/close_watcher.c holds §6.10.2's manager and its establish
 * and destroy, so that one is now a call this file makes rather than a mechanism it waits on. What §6.10 does
 * NOT yet have is the half that RUNS a watcher's actions — request to close, close, and process close watchers
 * — so a watcher this file establishes is never asked to close and a §6.12 popover goes on hiding only through
 * its own `hidePopover()`. That is a limit of §6.10 and not of this file, and close_watcher.h names it. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/css/top_layer.h"
#include "core/dom/attr_list.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/events/event_target.h"
#include "core/html/enumerated_attribute.h"
#include "core/html/focus.h"
#include "core/html/html_dialog.h"
#include "core/html/html_element.h"
#include "core/html/popover.h"
#include "core/html/toggle_event.h"
#include "core/idl_args.h"
#include "core/realm.h"

/* §6.12's KEYWORDS AND STATES, as its own table states them:
     auto   — Auto   — "Closes other popovers when opened; has light dismiss and responds to close requests."
     manual — Manual — "Does not close other popovers; does not light dismiss or respond to close requests."
     hint   — Hint   — "Closes other hint popovers when opened, but not other auto popovers; has light dismiss
                        and responds to close requests."
   THE THREE SPECIAL STATES ARE THREE DIFFERENT STATES — "the attribute's missing value default is the No
   Popover state, its invalid value default is the Manual state, and its empty value default is the Auto state"
   — which is the case core/html/enumerated_attribute.c's header records as having defeated an implementation
   that collapsed missing and invalid into one field. All three are observable through §2.6.1's getter: `<div>`
   reflects "" (No Popover has no keyword), `<div popover>` reflects "auto", `<div popover=x>` reflects
   "manual". */
static const EnumeratedKeyword POPOVER_KW[] = {
    { "auto", POPOVER_STATE_AUTO }, { "manual", POPOVER_STATE_MANUAL }, { "hint", POPOVER_STATE_HINT },
    { NULL, 0 }
};
const EnumeratedAttribute POPOVER_ATTRIBUTE = {
    POPOVER_KW, POPOVER_STATE_NONE, POPOVER_STATE_AUTO, POPOVER_STATE_MANUAL
};

int popover_attribute_state(const lxb_dom_element_t *el)
{
    return enumerated_attribute_state(el, "popover", POPOVER_ATTRIBUTE.keywords, POPOVER_ATTRIBUTE.missing,
                                      POPOVER_ATTRIBUTE.empty, POPOVER_ATTRIBUTE.invalid);
}

/* §6.12's two state strings, named once because a toggle task carries one of them across a suspension. */
static const char POPOVER_OPEN[]   = "open";
static const char POPOVER_CLOSED[] = "closed";

/* ---- §6.12's STATE, as own slots under private Symbols ---------------------------------------------------- */

/* Every one of these is a fact §6.12 states over an element or a Document. They are slots and not C fields for
   the reason core/css/top_layer.c's sets are Arrays: a flow WRITES them, a sibling flow must not see the write,
   and a parked flow must resume holding its own — which is what the COW delta gives a property write and gives
   nothing else. An ABSENT slot is the initial value the standard names, which is also what makes every one of
   them cost nothing for the elements a run never shows. */
enum {
    PS_VISIBILITY = 0,   /* "popover visibility state, initially hidden" — present-and-true IS showing */
    PS_TRIGGER,          /* "popover trigger, an HTML element or null, initially null" */
    PS_HIDING,           /* "popover hiding, a boolean, initially false" */
    PS_TRACKER,          /* "popover toggle task tracker … initially null" — the slot holds its OLD STATE */
    PS_TRACKER_TASK,     /* …and its TASK, as the JSTaskHandle JS_EnqueueCallTask answered, in a BigInt */
    PS_OPENED_MODE,      /* "opened in popover mode … 'auto', 'hint', or null, initially null" */
    PS_PREV_FOCUSED,     /* §6.6's previously focused element, which §6.12 steps 16 and 24 write */
    PS_DOC_SHOWING,      /* Document: "showing popover, a boolean, initially false" */
    PS_DOC_NESTING,      /* Document: "hiding popover nesting count, a number, initially 0" */
    PS_SLOT_N
};
static const char *const PS_SLOT_NAME[PS_SLOT_N] = {
    "popoverVisibilityState", "popoverTrigger", "popoverHiding", "popoverToggleTaskTracker",
    "popoverToggleTaskTrackerTask", "openedInPopoverMode", "popoverPreviouslyFocusedElement",
    "documentShowingPopover", "documentHidingPopoverNestingCount"
};
static JSValue g_slot_key[PS_SLOT_N];
static JSAtom  g_slot_atom[PS_SLOT_N];

static int g_id_show = -1, g_id_hide = -1, g_id_toggle = -1;
static int g_toggle_task_stepid = -1;

/* The slot's value, or JS_UNDEFINED for "the initial value the standard names". OWNED. */
static JSValue ps_get(JSContext *ctx, JSValueConst obj, int slot)
{
    JSValue v;

    DCHECK(slot >= 0 && slot < PS_SLOT_N, "a §6.12 state slot was read by an index this file has no name for");
    DCHECK(g_slot_atom[slot] != JS_ATOM_NULL,
           "a §6.12 state slot was read before popover_declare minted its key");
    if (!JS_IsObject(obj)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &v, obj, g_slot_atom[slot]) > 0) return v;
    return JS_UNDEFINED;
}

/* CONSUMES `v`. CONFIGURABLE AND WRITABLE for the reason html_dialog.c's return-value slot is: a slot defined
   with no flags makes the second write a silent no-op, and every one of these is written again on every show. */
static void ps_set(JSContext *ctx, JSValueConst obj, int slot, JSValue v)
{
    DCHECK(slot >= 0 && slot < PS_SLOT_N, "a §6.12 state slot was written by an index this file has no name for");
    JS_DefinePropertyValue(ctx, (JSValue)obj, g_slot_atom[slot], v,
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
}

/* Back to the initial value the standard names — the ABSENCE, so "set X to null" and "set X to false" are the
   same operation on the same slot and neither leaves a value behind for a later read to find. */
static void ps_clear(JSContext *ctx, JSValueConst obj, int slot)
{
    JS_DeleteProperty(ctx, (JSValue)obj, g_slot_atom[slot], 0);
}

static bool ps_flag(JSContext *ctx, JSValueConst obj, int slot)
{
    JSValue v = ps_get(ctx, obj, slot);
    bool set = JS_ToBool(ctx, v) != 0;

    JS_FreeValue(ctx, v);
    return set;
}

/* "…is null" for a slot whose initial value is the standard's null — asked without leaving the read behind,
   because a DCHECK's condition must be side-effect-free and an owned value dropped inside one is a leak the
   runtime's own walk reports and nobody can trace. */
static bool ps_is_null(JSContext *ctx, JSValueConst obj, int slot)
{
    JSValue v = ps_get(ctx, obj, slot);
    bool null = JS_IsUndefined(v);

    JS_FreeValue(ctx, v);
    return null;
}

/* §6.12: "Every HTML element has a popover visibility state, initially hidden, with these potential values:
   hidden, showing." Two values, so the boolean slot IS the state and no third answer exists. */
static bool popover_is_showing(JSContext *ctx, JSValueConst el)
{
    return ps_flag(ctx, el, PS_VISIBILITY);
}

/* "Every Document has a hiding popover nesting count, which is a number, initially 0." */
static int32_t popover_nesting(JSContext *ctx, JSValueConst doc)
{
    JSValue v = ps_get(ctx, doc, PS_DOC_NESTING);
    int32_t n = 0;

    if (!JS_IsUndefined(v)) JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

/* "Let document be element's node document" — as the Document's WRAPPER, which is what the per-Document slots
   hang off. OWNED; JS_NULL for an element with no owner document, which no element in a live tree is. */
static JSValue popover_document_of(JSContext *ctx, JSValueConst el)
{
    lxb_dom_node_t *n = node_of(el);

    if (!n || !n->owner_document) return JS_NULL;
    return node_wrap(ctx, lxb_dom_interface_node(n->owner_document));
}

/* The REALM whose active document is this element's node document — what §6.12's focus steps and §7.3.1's
   fully-active walk must be asked in. NULL for a document no navigable holds, which §6.12 step 3's
   "element's node document is not fully active" is exactly the answer to. */
static JSContext *popover_document_realm(JSValueConst el)
{
    lxb_dom_node_t *n = node_of(el);

    if (!n || !n->owner_document) return NULL;
    return document_active_realm_of(lxb_dom_interface_node(n->owner_document));
}

/* ---- §6.12's CHECK POPOVER VALIDITY -------------------------------------------------------------------------
 *
 * "To check popover validity for an HTML element element given a boolean expectedToBeShowing and a Document or
 * null expectedDocument … They throw an exception or return a boolean." Four steps. It runs NO page code — every
 * one of its questions is a content-attribute read, a slot read or a tree walk — which is why it is a plain
 * function and every one of its five call sites can ask it without a stage of its own.
 *
 * 1 = the standard's true, 0 = its false, -1 = it threw and the exception is live in the context. The caller
 * decides what to do with the throw, because §6.12's callers each state it: "if this throws an exception, catch
 * it, and set validityResult to that exception", and then "if throwExceptions is true and validityResult is a
 * DOMException, then throw validityResult". Both of the throws below ARE DOMExceptions, so the second condition
 * is decided by the first — asserted at the two call sites that discard one. */
static int popover_check_validity(JSContext *ctx, JSValueConst el, bool expected_showing,
                                  JSValueConst expected_document)
{
    lxb_dom_node_t *n = node_of(el);
    JSContext *docctx;

    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "§6.12 check popover validity was given something that is not an element — every caller reaches it "
           "having already brand-checked the receiver as an HTMLElement");
    /* Step 1. */
    if (popover_attribute_state(lxb_dom_interface_element(n)) == POPOVER_STATE_NONE) {
        JS_ThrowDOMException(ctx, "NotSupportedError",
                             "the element's popover attribute is in the No Popover state");
        return -1;
    }
    /* Step 2 — with only two visibility states, the section's two disjuncts are one comparison. */
    if (popover_is_showing(ctx, el) != expected_showing) return 0;
    /* Step 3's five disjuncts. */
    docctx = popover_document_realm(el);
    if (!node_is_connected(n) || docctx == NULL || !document_fully_active(docctx)) {
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "the popover element is not connected, or its node document is not fully active");
        return -1;
    }
    if (!JS_IsNull(expected_document) && !JS_IsUndefined(expected_document)) {
        JSValue mine = popover_document_of(ctx, el);
        bool same = JS_VALUE_GET_PTR(mine) == JS_VALUE_GET_PTR(expected_document);

        JS_FreeValue(ctx, mine);
        if (!same) {
            JS_ThrowDOMException(ctx, "InvalidStateError",
                                 "the popover element's node document is not the document the algorithm "
                                 "started in — a beforetoggle handler moved it");
            return -1;
        }
    }
    /* The fourth disjunct is "element is a dialog element and its IS MODAL is set to true", and the fifth is
       "element's fullscreen flag is set". Nothing in this build can set either — `showModal()` is the only
       producer of a modal dialog and Fullscreen's `requestFullscreen()` the only producer of the flag — so
       both are false here by construction and step 3 does not throw for them. That is a condition EVALUATED at
       the step that asks it rather than a skipped step, and the day either member lands, the assert fires AT
       the disjunct that must then be written. */
    realm_awaits(ctx, "HTMLDialogElement.prototype.showModal",
                 "HTML §6.12 The popover attribute's check popover validity step 3 refuses a `dialog` element "
                 "whose IS MODAL flag is set — `showModal()` is what sets it, so with that member in this "
                 "build this disjunct must be written and must throw an \"InvalidStateError\"");
    realm_awaits(ctx, "Element.prototype.requestFullscreen",
                 "HTML §6.12 The popover attribute's check popover validity step 3 refuses an element whose "
                 "FULLSCREEN FLAG is set — Fullscreen's `requestFullscreen()` is what sets it, so with that "
                 "member in this build this disjunct must be written and must throw an \"InvalidStateError\"");
    return 1;                                                                                    /* step 4 */
}

/* ---- §6.12's QUEUE A POPOVER TOGGLE EVENT TASK ---------------------------------------------------------------
 *
 * Three steps. The task is a JOB, which in this engine is a call-root FLOW: preemptible, forkable and parkable.
 * It has to be, because step 2 FIRES an event and every listener body is the page's code — the same reason
 * html_dialog.c's dialog toggle task is one.
 *
 * THE TRACKER IS WHAT MAKES TWO STATE CHANGES IN ONE TURN PRODUCE ONE `toggle` CARRYING THE TRUE OLD STATE
 * rather than two. Its slot holds the pending task's OLD STATE, which is the only field of the struct anything
 * reads; its ABSENCE is the spec's null tracker. */
typedef struct {
    JSStepHdr hdr;
    uint8_t   fphase;   /* the toggle fire request's own phase */
    JSValue   ev;       /* the ToggleEvent, minted once and held across the suspension (owned) */
    EventFireCb cb;     /* the fire request buffer: [this, dispatch, target, event] */
} PopoverToggleTask;

#define POPOVER_TOGGLE_STAGES(X) \
    X(PT_ENTER, "HTML §6.12 The popover attribute's queue a popover toggle event task step 2 (the queued " \
                "element task begins: build the ToggleEvent with oldState, newState and source initialized)") \
    X(PT_FIRE,  "HTML §6.12 The popover attribute's queue a popover toggle event task step 2 (fire an event " \
                "named toggle at element using that ToggleEvent, then set the element's popover toggle task " \
                "tracker to null)")
enum { POPOVER_TOGGLE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const POPOVER_TOGGLE_STEPS[] = { POPOVER_TOGGLE_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_popover_toggle_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    PopoverToggleTask *s = st;
    JSValueConst element = step_arg(&s->hdr, 0);
    int r;

    if (s->hdr.stage == PT_ENTER) {
        const char *old_state, *new_state;
        int k;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY owned field before the first thing that can fail — the failure path tears this state down
           through js_popover_toggle_visit, which frees exactly what the state holds, and a zeroed block is not
           a block of JS_UNDEFINEDs. */
        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        s->fphase = 0;
        old_state = JS_ToCString(ctx, step_arg(&s->hdr, 1));
        new_state = JS_ToCString(ctx, step_arg(&s->hdr, 2));
        if (!old_state || !new_state) {
            JS_FreeCString(ctx, old_state);
            JS_FreeCString(ctx, new_state);
            return JS_STEP_ABRUPT;
        }
        /* Step 2 names three initialisers and no others: neither bubbling nor cancelable. §6.12's own show
           popover step 9 fires the OTHER one cancelable, which is why toggle_event_new takes both from the
           caller rather than guessing from the type. */
        s->ev = toggle_event_new(ctx, "toggle", old_state, new_state, step_arg(&s->hdr, 3),
                                 /*bubbles*/ false, /*cancelable*/ false);
        JS_FreeCString(ctx, old_state);
        JS_FreeCString(ctx, new_state);
        if (JS_IsException(s->ev)) {
            s->ev = JS_UNDEFINED;
            return JS_STEP_ABRUPT;
        }
        STEP_GOTO(s->hdr.stage, PT_FIRE, &s->fphase, NULL);
    }
    DCHECK(s->hdr.stage == PT_FIRE, "the popover toggle task resumed at a stage §6.12 does not have");
    r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), element, s->ev, JS_UNDEFINED, cb_result,
                              NULL, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    ps_clear(ctx, element, PS_TRACKER);   /* step 2's second sub-step */
    return JS_STEP_DONE;
}

static void js_popover_toggle_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    PopoverToggleTask *s = st;
    int k;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k)
        v->val(ctx, &s->cb[k]);
}

static const JSTrampStepDef js_popover_toggle_def = {
    sizeof(PopoverToggleTask), js_popover_toggle_step, NULL, 0,
    .visit = js_popover_toggle_visit,
    .algorithm = "HTML §6.12 The popover attribute's popover toggle event task",
    .steps = POPOVER_TOGGLE_STEPS
};

static void popover_queue_toggle_task(JSContext *ctx, JSValueConst element, const char *old_state,
                                      const char *new_state, JSValueConst source)
{
    JSValueConst argv[4];
    JSValue fn, ov, nv, pending;
    JSTaskHandle handle;

    /* STEP 1: "if element's popover toggle task tracker is not null: set oldState to element's popover toggle
       task tracker's old state; remove element's popover toggle task tracker's task from its task queue; set
       element's popover toggle task tracker to null."
       THIS IS WHAT MAKES `el.showPopover(); el.hidePopover();` IN ONE TURN FIRE ONE `toggle`, carrying the
       state the element was ACTUALLY in when the turn began ("closed") rather than the intermediate "open" —
       and it is why the tracker holds a TASK and not just a flag. `JS_RemoveQueuedTask` answers 0 as ordinarily
       as 1, which its own contract states: a handle outlives the task it names, so the task may already have
       run or be running right now (a `toggle` listener that hides the popover again asks for the removal of
       the very task dispatching it), and that is the standard's own no-op rather than a failure. */
    ov = JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &pending, element, g_slot_atom[PS_TRACKER]) > 0) {
        JSValue task;

        DCHECK(JS_IsString(pending), "the popover toggle task tracker's OLD STATE slot held something that is "
                                     "not a string — the only writer is step 3 below, which places one of "
                                     "§6.12's two state strings");
        ov = pending;                                                          /* step 1's carry of oldState */
        if (JS_GetOwnSlot(ctx, &task, element, g_slot_atom[PS_TRACKER_TASK]) > 0) {
            uint64_t h = 0;

            JS_ToBigUint64(ctx, &h, task);
            JS_FreeValue(ctx, task);
            JS_RemoveQueuedTask(JS_GetRuntime(ctx), (JSTaskHandle)h);          /* step 1's removal */
        } else {
            DFAIL("§6.12's popover toggle task tracker held an OLD STATE with no TASK — the two slots are one "
                  "struct and step 3 writes both, so one without the other means a write went missing and the "
                  "superseded `toggle` cannot be taken off its queue");
        }
        ps_clear(ctx, element, PS_TRACKER);
        ps_clear(ctx, element, PS_TRACKER_TASK);
    }
    if (JS_IsUndefined(ov)) {
        ov = JS_NewString(ctx, old_state);
        CHECK(!JS_IsException(ov), "the popover toggle task's old state could not be allocated");
    }
    if (g_toggle_task_stepid < 0)
        g_toggle_task_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_popover_toggle_def);
    /* THE CALLEE IS MINTED IN THE ENQUEUING REALM: a C function runs in the realm that DEFINED it, and this one
       fires an event at an element of THIS document. */
    fn = JS_NewCFunction2(ctx, NULL, "popoverToggle", 4, JS_CFUNC_step, g_toggle_task_stepid);
    CHECK(!JS_IsException(fn), "the popover toggle task's callee could not be allocated");
    nv = JS_NewString(ctx, new_state);
    CHECK(!JS_IsException(nv), "the popover toggle task's new state could not be allocated");
    argv[0] = element;
    argv[1] = ov;
    argv[2] = nv;
    argv[3] = source;
    /* Step 2 is "QUEUE AN ELEMENT TASK GIVEN THE DOM MANIPULATION TASK SOURCE and element", so it is a TASK and
       not a microtask — a different position in HTML §8.1.7's event loop and not a smaller one. */
    handle = JS_EnqueueCallTask(ctx, fn, 4, argv);
    /* Step 3: "set element's popover toggle task tracker to a struct with task set to the just-queued task and
       old state set to oldState" — BOTH fields, in the two slots that are the one struct. */
    ps_set(ctx, element, PS_TRACKER, JS_DupValue(ctx, ov));
    ps_set(ctx, element, PS_TRACKER_TASK, JS_NewBigUint64(ctx, (uint64_t)handle));
    JS_FreeValue(ctx, nv);
    JS_FreeValue(ctx, ov);
    JS_FreeValue(ctx, fn);
}

/* ---- the three members, as ONE machine ---------------------------------------------------------------------- */

enum { M_SHOW = 0, M_HIDE, M_TOGGLE };

/* WHICH ALGORITHM THIS INVOCATION RESOLVED TO. `togglePopover` steps 5-7 pick one of three, and `showPopover`
   and `hidePopover` each pick one unconditionally, so the pick is state rather than a re-read of the magic. */
enum { PA_NONE = 0, PA_SHOW, PA_HIDE };

#define POPOVER_STAGES(X) \
    X(PO_ENTER,      "HTML §6.12 The popover attribute: the invoked member's own steps (showPopover step 1, " \
                     "hidePopover step 1, togglePopover steps 1-4) and then, for show popover, its steps 1-8, " \
                     "or for hide a popover, its steps 1-11 — a range because every step in it is ONE O(1) " \
                     "engine action: a slot read, a slot write, or a check popover validity whose four steps " \
                     "are an attribute-state lookup and a connectedness walk") \
    X(PO_SHOW_FIRE,  "HTML §6.12 The popover attribute's show popover step 9 (fire an event named " \
                     "beforetoggle, using ToggleEvent, cancelable, with oldState \"closed\", newState " \
                     "\"open\" and source, at element)") \
    X(PO_SHOW_FOCUS, "HTML §6.12 The popover attribute's show popover step 23 (run the popover focusing " \
                     "steps given element)") \
    X(PO_HIDE_FIRE,  "HTML §6.12 The popover attribute's hide a popover step 12 (fire an event named " \
                     "beforetoggle, using ToggleEvent, with oldState \"open\", newState \"closed\" and " \
                     "source, at element)") \
    X(PO_HIDE_END,   "HTML §6.12 The popover attribute's hide a popover steps 12's tail through 21 (the " \
                     "second check popover validity, the top-layer removal, the four state writes, the " \
                     "queued toggle task and cleanupSteps) — one O(1) engine action each")
enum { IDL_STEP_STAGE_BASE(POPOVER_STAGES) POPOVER_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const POPOVER_STEPS[] = { POPOVER_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t which;            /* PA_* — which of §6.12's two algorithms this invocation entered */
    uint8_t fphase;           /* the beforetoggle fire request's own phase */
    uint8_t ua_phase;         /* §6.6.6's allow focus steps' fork phase */
    uint8_t focus_phase;      /* §6.6.4's focusing steps request's own phase */
    uint8_t fire_events;      /* hide a popover's `fireEvents` argument, which its step 6 may lower */
    uint8_t nested_hide;      /* hide a popover step 4's `nestedHide` */
    uint8_t nesting_taken;    /* whether step 7's increment is still owed a decrement by cleanupSteps */
    uint8_t showing_taken;    /* whether show popover step 7's latch is still owed cleanupShowingSteps */
    JSValue el;               /* the receiver, held across the suspension (owned) */
    JSValue doc;              /* "element's node document", resolved once at entry (owned) */
    JSValue source;           /* the `source` the member read, or null (owned) */
    JSValue ev;               /* the `beforetoggle` event, minted once (owned) */
    JSValue control;          /* the popover focusing steps' `control` (owned) */
    EventFireCb cb;           /* the fire request buffer, and the focusing steps' — the WIDER of the two */
} PopoverState;

static void popover_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    PopoverState *s = st;
    int k;

    v->val(ctx, &s->el);
    v->val(ctx, &s->doc);
    v->val(ctx, &s->source);
    v->val(ctx, &s->ev);
    v->val(ctx, &s->control);
    STEP_CB_FOREACH(s->cb, k)
        v->val(ctx, &s->cb[k]);
}

/* show popover step 8's cleanupShowingSteps: "Set document's showing popover to false." Idempotent, because
   steps 9, 11, 15 and 25 each run it on their own arm and only one of them is taken. */
static void popover_cleanup_showing(JSContext *ctx, PopoverState *s)
{
    if (!s->showing_taken) return;
    s->showing_taken = 0;
    ps_clear(ctx, s->doc, PS_DOC_SHOWING);
}

/* hide a popover step 8's cleanupSteps, whole: lower the hiding boolean unless this is a nested hide, destroy
   the close watcher, decrement the nesting count. */
static void popover_cleanup_hiding(JSContext *ctx, PopoverState *s)
{
    if (!s->nesting_taken) return;
    s->nesting_taken = 0;
    if (!s->nested_hide) ps_clear(ctx, s->el, PS_HIDING);
    /* The close-watcher half is "if element's popover close watcher is not null: destroy it; set it to null".
       Only show popover step 15's Auto/Hint arm establishes one and that arm DFAILs below, so the watcher is
       null here by construction — asserted at the step rather than assumed, through the slot that would hold
       one. The DESTROY this step owes it exists now (core/html/close_watcher.h's close_watcher_destroy, given
       the element's relevant global object, which is the realm step 15 establishes it in); what is still
       missing is the step-15 block that would put one there. */
    DCHECK(ps_is_null(ctx, s->el, PS_OPENED_MODE),
           "HTML §6.12 The popover attribute's hide a popover step 8's cleanupSteps found an element with an "
           "OPENED IN POPOVER MODE, which only show popover step 15's Auto/Hint arm sets — that arm DFAILs in "
           "this build, so reaching here means it was built and this step must now call close_watcher_destroy "
           "(HTML §6.10.2 Close watcher infrastructure) on the element's popover close watcher, and set that "
           "slot to null, before decrementing");
    ps_set(ctx, s->doc, PS_DOC_NESTING, JS_NewInt32(ctx, popover_nesting(ctx, s->doc) - 1));
}

/* §6.12's POPOVER FOCUSING STEPS, ten steps, run at show popover step 23.
 *
 * Returns JS_STEP_FORK / JS_STEP_CALL for the caller to return, or 0 when the ten steps have finished. Two of
 * them are REQUESTS: step 1's §6.6.6 allow focus steps FORKS (its second clause is §6.4.1's transient
 * activation, which is unknown external state), and step 6's §6.6.4 focusing steps fire `blur`, `focusout`,
 * `focus` and `focusin` at the page's listeners. */
static int popover_focusing_steps(JSContext *ctx, PopoverState *s, JSStepHdr *hdr, JSValue in,
                                  JSValue **out_cb, int *out_argc)
{
    JSContext *docctx = popover_document_realm(s->el);
    lxb_dom_node_t *n = node_of(s->el);
    bool allow = false;
    int r;

    DCHECK(docctx != NULL, "§6.12's popover focusing steps were reached for an element whose node document has "
                           "no active realm — show popover step 3's check popover validity has already refused "
                           "a document that is not fully active");
    if (JS_IsUndefined(s->control)) {
        /* `in` is the driver's re-entry value and NOTHING between here and step 6 consumes one: step 1's
           request forks rather than calling, and steps 2-5 run no page code at all. It is released here, once,
           so the only path that forwards it is the one that owes it to step 6. A re-entry that already HAS a
           control skips this block entirely and hands `in` straight to that request. */
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        /* Step 1. */
        r = focus_allow_focus_steps_run(docctx, hdr, &s->ua_phase, &allow);
        if (r) return r;
        if (!allow) return 0;
        /* Step 2. §4.11.4's DIALOG FOCUSING STEPS are a different algorithm over the same subject, and this
           build has none of them — html_dialog.c builds close-the-dialog and `returnValue` and nothing shaped
           like the rest. A `<dialog popover>` is ordinary markup, so this is reachable. */
        if (html_dialog_is_dialog(n))
            DFAIL("HTML §6.12 The popover attribute's popover focusing steps step 2 runs HTML §4.11.4 The "
                  "dialog element's DIALOG FOCUSING STEPS for a `dialog` element and returns — this build has "
                  "no dialog focusing steps (core/html/html_dialog.c builds close-the-dialog and `returnValue` "
                  "only), so a `<dialog popover>` reaching showPopover() has no focus model to run. Build "
                  "§4.11.4's dialog focusing steps beside close-the-dialog, then call them here");
        /* Step 3. */
        if (dom_attr_get_ns(lxb_dom_interface_element(n), NULL, "autofocus") != NULL) {
            s->control = JS_DupValue(ctx, s->el);
        } else {
            /* Step 4 — "let control be the autofocus delegate for subject given 'other'", whose §6.6.4 steps
               open with "for each descendant descendant of focus target, in tree order: if descendant does not
               have an autofocus content attribute, then continue". That first clause is asked here, because it
               is the whole of the answer whenever no descendant carries the attribute: the delegate is then
               NULL and step 5 returns, which is every popover a page writes without one.
               WHAT IS NOT BUILT is the rest of the walk — "let focusable area be descendant, if descendant is
               a focusable area; otherwise the result of GETTING THE FOCUSABLE AREA for descendant given focus
               trigger" — because core/html/focus.c's delegate_search runs §6.6.4's autofocus pass and its
               plain pass together, per level, and the autofocus delegate is the autofocus pass of the OUTER
               level alone. */
            lxb_dom_node_t *d = node_next_in(n, n);

            while (d) {
                if (d->type == LXB_DOM_NODE_TYPE_ELEMENT &&
                    dom_attr_get_ns(lxb_dom_interface_element(d), NULL, "autofocus") != NULL)
                    DFAIL("HTML §6.12 The popover attribute's popover focusing steps step 4 asks for the "
                          "AUTOFOCUS DELEGATE (HTML §6.6.4 Processing model) of a popover that has a "
                          "descendant carrying the `autofocus` content attribute, and core/html/focus.c "
                          "exports no such door: its delegate_search runs §6.6.4's autofocus pass and its "
                          "plain descendant pass together at each level, while the autofocus delegate is the "
                          "autofocus pass of the OUTERMOST level alone. Export it from focus.c — the level's "
                          "two passes are already separate there — and call it here");
                d = node_next_in(d, n);
            }
            return 0;                                                                          /* step 5 */
        }
    }
    DCHECK(!JS_IsUndefined(s->control), "§6.12's popover focusing steps reached step 6 with no control — "
                                        "step 5 returns for a null one, and both of step 3 and step 4's arms "
                                        "either set one or return through it");
    /* Step 6. */
    r = focus_element_run(docctx, s->control, &s->focus_phase, STEP_CB(s->cb), in, out_cb, out_argc);
    if (r) return r;
    /* Steps 7-10 empty the TOP-LEVEL document's autofocus candidates and set its autofocus processed flag —
       reachable only when step 3 or 4 produced a control, which in this build means the subject itself carried
       `autofocus`. core/html/autofocus.c owns both, and neither is exported: its door is the insertion steps
       that FILL the list and the flush that drains it. */
    DFAIL("HTML §6.12 The popover attribute's popover focusing steps steps 7-10 resolve the control's node "
          "navigable's top-level traversable's active document, compare origins, EMPTY that document's "
          "AUTOFOCUS CANDIDATES and set its AUTOFOCUS PROCESSED FLAG — core/html/autofocus.c owns both and "
          "exports neither (its doors are the insertion steps that fill the list and §6.6.7's flush that "
          "drains it). Export the empty-and-mark pair from autofocus.c and call it here");
    return 0;   /* release: the four steps above are skipped with the assert that names them */
}

/* ---- §6.12's SHOW POPOVER and HIDE A POPOVER, and the three members over them ------------------------------- */

static int popover_body(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    PopoverState *s = state;
    int magic = idl_step_magic(hdr);
    int r, validity;

    if (hdr->stage == PO_ENTER) {
        JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
        bool force_given = false, force = false;
        int k;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY owned field before the first thing that can throw: the failure path tears this state down
           through popover_visit, which frees exactly what the state holds and nothing else. */
        s->el = s->doc = s->source = s->ev = s->control = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        s->which = PA_NONE;
        s->fphase = s->ua_phase = s->focus_phase = 0;
        s->fire_events = s->nested_hide = s->nesting_taken = s->showing_taken = 0;
        /* Web IDL §3.7.5's BRAND CHECK. All three members are HTMLElement's, and one invoked on anything else
           is a TypeError rather than a silent return — a page that wrote `f.call(x)` must find out. */
        if (!html_element_is(hdr->this_val)) {
            JS_ThrowTypeError(ctx, "showPopover/hidePopover/togglePopover was called on something that is not "
                                   "an HTMLElement");
            return JS_STEP_ABRUPT;
        }
        s->el = JS_DupValue(ctx, hdr->this_val);
        s->doc = popover_document_of(ctx, s->el);
        DCHECK(!JS_IsNull(s->doc), "a §6.12 member ran on an element with no node document — every element "
                                   "lexbor builds is created in one");
        /* THE MEMBERS' OWN STEPS. `source` is `options["source"]` for showPopover (step 1) and togglePopover
           (step 4); hidePopover declares no options at all and its one step passes null. */
        if (magic != M_HIDE) {
            /* togglePopover's argument is `(TogglePopoverOptions or boolean)` — §3.2.25's union, whose object
               arm is the dictionary and whose every other value is the boolean. §6.12 steps 2 and 3 read the
               two arms apart itself ("If options is a boolean, set force to options. Otherwise, if
               options[\"force\"] exists, set force to options[\"force\"]"), which is what IDL_BOOL_OR_DICT's
               declaration delivers: the boolean arm places the boolean and the dictionary arm places the built
               dictionary, and the body tells them apart with JS_IsBool. That row's name records the union it
               was first written for, `(boolean or ScrollIntoViewOptions)`; §3.2.25's conversion does not
               depend on which member the IDL writes first, and the destination — the boolean itself rather
               than a member it is flattened into — is what makes this that row and not IDL_DICT_OR_BOOL_FIRST. */
            if (magic == M_TOGGLE && JS_IsBool(opts)) {
                force_given = true;                                                  /* toggle step 2 */
                force = JS_ToBool(ctx, opts) != 0;
                opts = JS_UNDEFINED;
            } else if (magic == M_TOGGLE) {
                JSValue f = idl_dict_get(ctx, opts, "force");                        /* toggle step 3 */

                if (!JS_IsUndefined(f)) {
                    DCHECK(JS_IsBool(f), "TogglePopoverOptions.force arrived as something that is not a "
                                         "boolean — the member is declared IDL_BOOLEAN, and the declaration's "
                                         "conversion is what places it");
                    force_given = true;
                    force = JS_ToBool(ctx, f) != 0;
                }
                JS_FreeValue(ctx, f);
            }
            s->source = idl_dict_get(ctx, opts, "source");         /* show step 1 / toggle step 4 */
            if (JS_IsUndefined(s->source)) s->source = JS_NULL;
            DCHECK(JS_IsNull(s->source) || html_element_is(s->source),
                   "ShowPopoverOptions.source arrived as something that is not an HTMLElement — the member is "
                   "declared IDL_INTERFACE with this file's own class and narrowing, and §3.2.15's brand check "
                   "is what refuses anything else");
        } else {
            s->source = JS_NULL;
        }
        /* THE PICK. showPopover step 2 runs show popover; hidePopover step 1 runs hide a popover; and
           togglePopover steps 5, 6 and 7 pick one of show, hide and a bare check popover validity. */
        if (magic == M_SHOW) {
            s->which = PA_SHOW;
        } else if (magic == M_HIDE) {
            s->which = PA_HIDE;
        } else if (popover_is_showing(ctx, s->el) && (!force_given || !force)) {
            s->which = PA_HIDE;                                                      /* toggle step 5 */
        } else if (!force_given || force) {
            s->which = PA_SHOW;                                                      /* toggle step 6 */
        } else {
            /* Toggle step 7 — "let expectedToBeShowing be true if this's popover visibility state is showing;
               otherwise false. Run check popover validity given this, expectedToBeShowing, and null." Its
               boolean result is DISCARDED by the standard: what this step is for is the THROW. */
            validity = popover_check_validity(ctx, s->el, popover_is_showing(ctx, s->el), JS_NULL);
            if (validity < 0) return JS_STEP_ABRUPT;
            *presult = JS_NewBool(ctx, popover_is_showing(ctx, s->el));              /* toggle step 8 */
            return JS_STEP_DONE;
        }

        if (s->which == PA_SHOW) {
            /* SHOW POPOVER, given element, throwExceptions = true (every caller in this build passes true),
               and source. Steps 1-8. */
            if (ps_flag(ctx, s->doc, PS_DOC_SHOWING) || popover_nesting(ctx, s->doc) != 0) {   /* step 2 */
                JS_ThrowDOMException(ctx, "InvalidStateError",
                                     "showPopover() was called during the show or hide of another popover");
                return JS_STEP_ABRUPT;
            }
            validity = popover_check_validity(ctx, s->el, /*expectedToBeShowing*/ false, JS_NULL);  /* step 3 */
            if (validity < 0) return JS_STEP_ABRUPT;                                            /* step 4 */
            if (validity == 0) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
            DCHECK(ps_is_null(ctx, s->el, PS_TRIGGER),
                   "HTML §6.12 The popover attribute's show popover step 5 asserts that the element's POPOVER "
                   "TRIGGER is null — hide a popover step 14 is the only thing that clears it and show popover "
                   "step 21 the only thing that sets it, so a non-null trigger on an element step 3 has just "
                   "certified hidden means those two writes have gone out of step");              /* step 5 */
            DCHECK(!top_layer_contains(ctx, s->el),
                   "HTML §6.12 The popover attribute's show popover step 6 asserts that the element is not in "
                   "the document's TOP LAYER — an element hidden by step 2's check but still in the layer "
                   "means hide a popover's removal (step 12 or step 13) did not run");            /* step 6 */
            ps_set(ctx, s->doc, PS_DOC_SHOWING, JS_TRUE);                                        /* step 7 */
            s->showing_taken = 1;                                                                /* step 8 */
            s->ev = toggle_event_new(ctx, "beforetoggle", POPOVER_CLOSED, POPOVER_OPEN, s->source,
                                     /*bubbles*/ false, /*cancelable*/ true);
            if (JS_IsException(s->ev)) {
                s->ev = JS_UNDEFINED;
                popover_cleanup_showing(ctx, s);
                return JS_STEP_ABRUPT;
            }
            STEP_GOTO(hdr->stage, PO_SHOW_FIRE, &s->fphase, &s->ua_phase, &s->focus_phase, NULL);
        } else {
            /* HIDE A POPOVER, given element, focusPreviousElement = true, fireEvents = true, throwExceptions =
               true and source = null — the arguments both `hidePopover()` step 1 and togglePopover step 5
               pass. Steps 1-11. */
            s->fire_events = 1;
            validity = popover_check_validity(ctx, s->el, /*expectedToBeShowing*/ true, JS_NULL);   /* step 1 */
            if (validity < 0) return JS_STEP_ABRUPT;                                                /* step 2 */
            if (validity == 0) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
            s->nested_hide = ps_flag(ctx, s->el, PS_HIDING) ? 1 : 0;                                /* step 4 */
            ps_set(ctx, s->el, PS_HIDING, JS_TRUE);                                                 /* step 5 */
            if (s->nested_hide) s->fire_events = 0;                                                 /* step 6 */
            ps_set(ctx, s->doc, PS_DOC_NESTING, JS_NewInt32(ctx, popover_nesting(ctx, s->doc) + 1)); /* step 7 */
            s->nesting_taken = 1;                                                                   /* step 8 */
            /* Steps 9 and 10 read the SHOWING AUTO POPOVER LIST and the SHOWING HINT POPOVER LIST, and step 11
               is the block those two booleans gate. Both lists are "for each Element element of document's top
               layer … element's opened in popover mode is 'auto'/'hint'", and NOTHING in this build writes
               that mode — show popover step 15 is its only writer and it DFAILs. So both lists are empty by
               construction, step 11's condition is false, and step 13's "otherwise" is the arm every hide in
               this build takes. The condition is EVALUATED at the step that asks it: */
            if (!ps_is_null(ctx, s->el, PS_OPENED_MODE))
                DFAIL("HTML §6.12 The popover attribute's hide a popover step 11 runs HIDE POPOVER STACK UNTIL "
                      "over the document's showing auto and hint popover lists — this build has no such "
                      "algorithm, no TOPMOST POPOVER ANCESTOR (which needs a FLAT TREE descendant test) and no "
                      "HINT STACK PARENT. Reaching here means show popover step 15's Auto/Hint arm was built "
                      "and set this element's OPENED IN POPOVER MODE, so all three must now be written");
            if (s->fire_events) {
                s->ev = toggle_event_new(ctx, "beforetoggle", POPOVER_OPEN, POPOVER_CLOSED, s->source,
                                         /*bubbles*/ false, /*cancelable*/ false);
                if (JS_IsException(s->ev)) {
                    s->ev = JS_UNDEFINED;
                    popover_cleanup_hiding(ctx, s);
                    return JS_STEP_ABRUPT;
                }
                STEP_GOTO(hdr->stage, PO_HIDE_FIRE, &s->fphase, &s->ua_phase, &s->focus_phase, NULL);
            } else {
                STEP_GOTO(hdr->stage, PO_HIDE_END, &s->fphase, &s->ua_phase, &s->focus_phase, NULL);
            }
        }
    }

    if (hdr->stage == PO_SHOW_FIRE) {
        bool not_canceled = true;

        /* Step 9 — the fire is CANCELABLE, so a handler calling `preventDefault()` stops the show, which is
           the whole reason `beforetoggle` exists. */
        r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), s->el, s->ev, JS_UNDEFINED, cb_result,
                                  &not_canceled, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        JS_FreeValue(ctx, s->ev);
        s->ev = JS_UNDEFINED;
        if (!not_canceled) {
            popover_cleanup_showing(ctx, s);
            *presult = JS_UNDEFINED;
            return JS_STEP_DONE;
        }
        /* Step 10 — "check popover validity is called again because firing the beforetoggle event could have
           disconnected this element or changed its popover attribute", and this time with `document` as the
           expected document, which is what catches a handler that adopted the element into another one. */
        validity = popover_check_validity(ctx, s->el, /*expectedToBeShowing*/ false, s->doc);
        if (validity != 1) {                                                                     /* step 11 */
            popover_cleanup_showing(ctx, s);
            if (validity < 0) return JS_STEP_ABRUPT;
            *presult = JS_UNDEFINED;
            return JS_STEP_DONE;
        }
        /* Steps 12-14: shouldRestoreFocus is false, and originalType and effectiveType are both the current
           state of the element's `popover` attribute. */
        {
            lxb_dom_element_t *e = lxb_dom_interface_element(node_of(s->el));
            int type = popover_attribute_state(e);

            /* Step 15 — the whole Auto/Hint block, which is where the mechanisms §6.12 has that this build has
               not are reached: the topmost popover ancestor, hide popover stack until, the hint stack parent
               and the opened-in-popover-mode writes. Its LAST sub-step, establish a close watcher, is no
               longer among them — core/html/close_watcher.c holds §6.10.2's establish, so the block's own two
               are all that is left. A Manual popover enters none of it, which is the SPEC's own split and the
               reason this file is complete for one state and a named crash for the other two. */
            if (type == POPOVER_STATE_AUTO || type == POPOVER_STATE_HINT)
                DFAIL("HTML §6.12 The popover attribute's show popover step 15 is the Auto/Hint block, and it "
                      "needs two mechanisms this build has none of, BOTH OF THEM §6.12'S OWN: TOPMOST POPOVER "
                      "ANCESTOR (whose index walk is over FLAT TREE descendants of the showing auto and hint "
                      "popover lists) and HIDE POPOVER STACK UNTIL. Its last sub-step, ESTABLISH A CLOSE "
                      "WATCHER, is built — call close_watcher_establish (core/html/close_watcher.h) with "
                      "CLOSE_WATCHER_KIND_POPOVER and this element, given the element's relevant global "
                      "object, and keep the result in the element's popover close watcher slot for hide a "
                      "popover step 8's cleanupSteps to destroy. What §6.10 still lacks is the half that RUNS "
                      "a watcher's actions (request to close, close, process close watchers), so a popover "
                      "that establishes one will not yet hide on a close request; that is §6.10's residual and "
                      "does not block this block. Build these two, then this block, then hide a popover step "
                      "11 and §6.12.2 Popover light dismiss, which are its other two consumers. `<div popover>` "
                      "reaches here because §6.12's EMPTY VALUE DEFAULT is the Auto state");
            DCHECK(type == POPOVER_STATE_MANUAL,
                   "HTML §6.12 The popover attribute's show popover reached step 16 with a `popover` attribute "
                   "that is in neither the Auto, the Hint nor the Manual state — step 3's check popover "
                   "validity has already refused the No Popover state, and §6.12's table names no fourth");
            ps_clear(ctx, s->el, PS_PREV_FOCUSED);                                                /* step 16 */
            /* Step 17 lets originallyFocusedElement be the document's FOCUSED AREA of its DOM anchor, and its
               ONE consumer is step 24, which is guarded by shouldRestoreFocus — a local only step 15's block
               sets true. That block DFAILs above, so step 24 cannot run and the read has no consumer; the
               crash for it therefore sits at step 24, where the value would be needed, rather than here. */
            top_layer_add(ctx, s->el);                                                            /* step 18 */
            /* Step 19 sets the hint stack parent, and its condition is "effectiveType is Hint", which step 15's
               DFAIL above makes unreachable. */
            ps_set(ctx, s->el, PS_VISIBILITY, JS_TRUE);                                           /* step 20 */
            if (!JS_IsNull(s->source)) ps_set(ctx, s->el, PS_TRIGGER, JS_DupValue(ctx, s->source));
            else                       ps_clear(ctx, s->el, PS_TRIGGER);                          /* step 21 */
            /* Step 22 sets the element's IMPLICIT ANCHOR ELEMENT to source. NOT COVERED: that is CSS Anchor
               Positioning's per-element state, and this engine has no consumer for it — `grep -rn "implicit
               anchor" engine/host` is empty, there is no `anchor-name`, no `position-anchor` and no `anchor()`
               in the cascade. THE NEXT DIFF that builds any of those builds this write with them, in the
               component that owns the anchor state. ITS ABSENCE WOULD SHOW as a popover positioned with
               `position-anchor: implicit` resolving against nothing: `getComputedStyle(p).top` answers as if
               the popover had no invoker, where a browser answers relative to the button that showed it. It is
               named here and not crashed on because the write is CORRECT to omit while nothing can read it —
               a crash would fire on every `showPopover()` this file gets right. */
        }
        STEP_GOTO(hdr->stage, PO_SHOW_FOCUS, &s->fphase, &s->ua_phase, &s->focus_phase, NULL);
    }

    if (hdr->stage == PO_SHOW_FOCUS) {
        r = popover_focusing_steps(ctx, s, hdr, cb_result, out_cb, out_argc);                     /* step 23 */
        if (r) return r;
        /* Step 24 — "if shouldRestoreFocus is true …". shouldRestoreFocus is step 12's false and is set true
           only inside step 15's Auto/Hint block, which DFAILs above, so this arm is unreachable in this build.
           It is written as the standard writes it, and the read step 17 owes it crashes here. */
        popover_cleanup_showing(ctx, s);                                                          /* step 25 */
        popover_queue_toggle_task(ctx, s->el, POPOVER_CLOSED, POPOVER_OPEN, s->source);           /* step 26 */
        *presult = magic == M_TOGGLE ? JS_NewBool(ctx, popover_is_showing(ctx, s->el)) : JS_UNDEFINED;
        return JS_STEP_DONE;
    }

    if (hdr->stage == PO_HIDE_FIRE) {
        /* Step 12's first sub-step. NOT cancelable: §6.12 names three initialisers for this fire and no
           others, which is why a hide cannot be prevented and why the sub-step after it re-reads validity
           instead of reading a canceled flag. */
        r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), s->el, s->ev, JS_UNDEFINED, cb_result,
                                  NULL, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        JS_FreeValue(ctx, s->ev);
        s->ev = JS_UNDEFINED;
        /* Step 12's second sub-step — "called again because firing the beforetoggle event could have
           disconnected element or changed its popover attribute". */
        validity = popover_check_validity(ctx, s->el, /*expectedToBeShowing*/ true, JS_NULL);
        if (validity != 1) {
            popover_cleanup_hiding(ctx, s);
            if (validity < 0) return JS_STEP_ABRUPT;
            *presult = JS_UNDEFINED;
            return JS_STEP_DONE;
        }
        /* Step 12's last two sub-steps: the removal is a REQUEST, so the element stays in the top layer until
           §8.1.7's update the rendering step 23 processes top layer removals — which is what CSS Positioned
           Layout Level 4 §3.3's own note means by "most of the time, requesting removal is more appropriate".
           The IMPLICIT ANCHOR ELEMENT write is the same residual named at show popover step 22. */
        top_layer_request_removal(ctx, s->el);
        STEP_GOTO(hdr->stage, PO_HIDE_END, &s->fphase, &s->ua_phase, &s->focus_phase, NULL);
    }

    DCHECK(hdr->stage == PO_HIDE_END, "a §6.12 member resumed into a stage this algorithm does not have");
    JS_FreeValue(ctx, cb_result);
    /* Step 13 — the "otherwise" of step 12's `fireEvents` condition, and the arm a NESTED hide takes. */
    if (!s->fire_events) top_layer_remove_immediately(ctx, s->el);
    ps_clear(ctx, s->el, PS_TRIGGER);                                                            /* step 14 */
    ps_clear(ctx, s->el, PS_OPENED_MODE);                                                        /* step 15 */
    ps_clear(ctx, s->el, PS_VISIBILITY);                                                         /* step 16 */
    /* Step 17 clears the document's HINT STACK PARENT, whose only writer is show popover step 19 inside the
       Auto/Hint block that DFAILs above — so it is null here and the clear is the identity. */
    if (s->fire_events)                                                                          /* step 18 */
        popover_queue_toggle_task(ctx, s->el, POPOVER_OPEN, POPOVER_CLOSED, s->source);
    /* Steps 19 and 20 restore focus to the element's PREVIOUSLY FOCUSED ELEMENT, which only show popover step
       24 writes — and step 24 is guarded by shouldRestoreFocus, which only step 15's Auto/Hint block sets. So
       the slot is absent here by construction and step 20's arm is unreachable; the crash for the two reads it
       owes sits inside that arm rather than above it. */
    if (!ps_is_null(ctx, s->el, PS_PREV_FOCUSED))
        DFAIL("HTML §6.12 The popover attribute's hide a popover step 20 restores focus to the element's "
              "PREVIOUSLY FOCUSED ELEMENT, and its condition is that the document's FOCUSED AREA OF ITS DOM "
              "ANCHOR is a shadow-including inclusive descendant of the element — core/html/focus.c owns "
              "§6.6.2's focused area and exports no getter for it (only the two predicates §6.6.7's flush "
              "needs). Reaching here means show popover step 24 was built, so export the focused area from "
              "focus.c, ask it here, and run §6.6.4's focusing steps for the remembered element through "
              "focus_element_run");
    popover_cleanup_hiding(ctx, s);                                                              /* step 21 */
    *presult = magic == M_TOGGLE ? JS_NewBool(ctx, popover_is_showing(ctx, s->el)) : JS_UNDEFINED;
    return JS_STEP_DONE;
}

/* WHAT THIS MEMBER TOOK AND MUST GIVE BACK ON EVERY EXIT — the two latches show popover step 8's
   cleanupShowingSteps and hide a popover step 8's cleanupSteps exist for. A flow that is discarded while parked
   on a `beforetoggle` handler never comes back, so this is the only place they can be lowered on that path, and
   leaving either raised would make the document refuse every later show for the rest of the session. */
static void popover_release(JSContext *ctx, void *state)
{
    PopoverState *s = state;

    if (JS_IsUndefined(s->doc) || JS_IsNull(s->doc)) return;
    popover_cleanup_showing(ctx, s);
    popover_cleanup_hiding(ctx, s);
}

static const IdlStepDecl POPOVER_DECL = {
    popover_body, sizeof(PopoverState), popover_visit, popover_release,
    "HTML §6.12 The popover attribute's showPopover(), hidePopover() and togglePopover()", POPOVER_STEPS
};

/* §6.12's `dictionary ShowPopoverOptions { HTMLElement source; }` and `dictionary TogglePopoverOptions :
   ShowPopoverOptions { boolean force; }`. Neither member is `required` and neither has a default, so an absent
   one is absent — which is exactly what §6.12's "if it exists" asks.

   THE ORDER IS TWO FACTS AND THE `level` COLUMN IS WHERE THE SECOND ONE IS STATED. Web IDL §3.2.17 Dictionary
   types' conversion reads its members over TWO nested loops: step 3 is "let dictionaries be a list consisting
   of D and all of D's inherited dictionaries, in order from least to most derived", and step 4 is "for each
   dictionary dictionary in dictionaries, in order: for each dictionary member member declared on dictionary, in
   lexicographical order". So the ancestry decides the outer order and the spelling decides only the inner one —
   `source` is `ShowPopoverOptions`' and `force` is `TogglePopoverOptions`' own, so `source` is read first
   THOUGH `force` sorts before it.
   THIS FILE WROTE THAT SENTENCE IN PROSE AND LEFT BOTH LEVELS AT 0, which declares the pair as ONE level whose
   spelling must ascend — and `force` < `source`, so idl_dict_order_check aborted at declaration and took every
   engine stage down with it. core/events/hash_change_event.c's own comment names this exact trap and its
   dictionary is the lucky half of it: `newURL`/`oldURL` sort the same way the levels do, so a missing level
   there would have gone unnoticed. Here the two orders DISAGREE, which is why the level is not decoration —
   it is the only place the inheritance is written down. */
static bool popover_source_is_html_element(JSValueConst v) { return html_element_is(v); }

/* NOT `const`, and the reason is the one core/dom/node.c's class id is: a JSClassID is minted at agent start
   and a static initializer cannot ask for one, so the member's own §3.2.15 class is filled in at declare. The
   NARROWING beside it is what a class alone cannot say — every DOM node wrapper is one class, so the class says
   "a Node" and `HTMLElement source` says more. */
static IdlDictMember SHOW_POPOVER_OPTIONS[] = {
    { "source", IDL_INTERFACE, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL, 0,
      popover_source_is_html_element }
};
static IdlDictMember TOGGLE_POPOVER_OPTIONS[] = {
    { "source", IDL_INTERFACE, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL, 0,
      popover_source_is_html_element },
    /* LEVEL 1: `force` is declared on TogglePopoverOptions itself, and `source` above is inherited from
       ShowPopoverOptions — §3.2.17 step 3's "least to most derived". */
    { "force", IDL_BOOLEAN, false, NULL, 1, NULL, IDL_DEFAULT_NONE, NULL, 0, NULL }
};

void popover_declare(JSContext *ctx)
{
    static const IdlArgType SHOW_ARGS[]   = { IDL_DICT };
    static const IdlArgType TOGGLE_ARGS[] = { IDL_BOOL_OR_DICT };
    int i;

    DCHECK(g_id_show < 0, "popover_declare ran twice — the slot keys and the three members' pool ids are the "
                          "AGENT's");
    for (i = 0; i < PS_SLOT_N; i++) {
        g_slot_key[i] = JS_NewSymbol(ctx, PS_SLOT_NAME[i], false);
        CHECK(!JS_IsException(g_slot_key[i]), "a §6.12 popover state slot key allocation failed");
        g_slot_atom[i] = JS_ValueToAtom(ctx, g_slot_key[i]);
        CHECK(g_slot_atom[i] != JS_ATOM_NULL, "a §6.12 popover state slot key could not be interned");
    }
    /* CSS Positioned Layout Level 4 §3's top layer, declared from here because §6.12's show popover step 18 is
       what puts an element in it and this file is that step's home. The day §4.11.4's `showModal()` or
       Fullscreen lands, that component declares nothing of its own — it calls the same three algorithms. */
    top_layer_declare(ctx);
    /* Each dictionary's interface-typed member carries its own §3.2.15 class — see the declaration above. */
    SHOW_POPOVER_OPTIONS[0].iface = node_class_id();
    TOGGLE_POPOVER_OPTIONS[0].iface = node_class_id();

    g_id_show = idl_method_id_step(ctx, SHOW_ARGS, 1, SHOW_POPOVER_OPTIONS, 1, &POPOVER_DECL, M_SHOW);
    idl_optional_from(0);
    g_id_hide = idl_method_id_step(ctx, NULL, 0, NULL, 0, &POPOVER_DECL, M_HIDE);
    g_id_toggle = idl_method_id_step(ctx, TOGGLE_ARGS, 1, TOGGLE_POPOVER_OPTIONS, 2, &POPOVER_DECL, M_TOGGLE);
    idl_optional_from(0);
}

void popover_install(JSContext *ctx, JSValueConst html_proto)
{
    DCHECK(g_id_show >= 0, "§6.12's members were installed on a realm's prototype before popover_declare "
                           "declared them");
    DCHECK(JS_IsObject(html_proto), "§6.12's members were installed with no HTMLElement.prototype");
    /* `idl_install_method` AND NOT `idl_install_step_method`, WHICH IS A STATEMENT ABOUT THE DECLARATION AND
       NOT ABOUT THE ALGORITHM. Both installers mint a JS_CFUNC_step, so the choice is not "is this member a
       step machine" — all three of these are one. It is "does this member have a POOL ENTRY", and these three
       do: each is declared through idl_method_id_step above, which is what converts their `options` dictionary
       and what §3.7.7 Operations' `length` is then DERIVED from. idl_install_step_method exists for the other
       kind — a raw JS_RegisterStepDef machine with no declared arguments (core/html/html_form.c's `submit`,
       observable_ops.c's operators) — and it takes a hand-written `length` precisely because there is no
       declaration to compute one from. Its own residual comment says so, and the two installers assert against
       each other so neither can be used for the other by mistake.
       THIS FILE USED THE WRONG ONE FOR ALL THREE, and the number it was passing (0) happened to equal the
       derived answer, so the mistake was invisible in behaviour and visible only to that assert: `showPopover`
       and `togglePopover` declare their one argument optional (idl_optional_from(0)) and `hidePopover`
       declares none, so §3.7.7's length is 0 either way. What the wrong installer really costs is the
       derivation — the day an argument of one of these stops being optional, the pool would know and a
       hand-written 0 would not. */
    idl_install_method(ctx, html_proto, "showPopover", g_id_show);
    idl_install_method(ctx, html_proto, "hidePopover", g_id_hide);
    idl_install_method(ctx, html_proto, "togglePopover", g_id_toggle);
}

void popover_free(JSRuntime *rt)
{
    int i;

    top_layer_free(rt);
    /* The slot keys are the AGENT's — a Symbol nobody frees is a live GC object the runtime's own walk counts
       as a leak. */
    for (i = 0; i < PS_SLOT_N; i++) {
        JS_FreeAtomRT(rt, g_slot_atom[i]);
        g_slot_atom[i] = JS_ATOM_NULL;
        JS_FreeValueRT(rt, g_slot_key[i]);
        g_slot_key[i] = JS_UNDEFINED;
    }
    g_id_show = g_id_hide = g_id_toggle = -1;
    g_toggle_task_stepid = -1;
}
