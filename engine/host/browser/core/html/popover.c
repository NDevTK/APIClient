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
 * THE AUTO AND HINT STATES ARE WHOLE HERE, AND THE STACK IS WHAT MAKES THEM SO. They used to be split down the
 * middle — the READ half present and the WRITE half named at the two sites that needed it — and the line was
 * whether a step runs the PAGE'S code. The READ half is the showing auto and hint popover lists, DERIVED each
 * time from CSS Positioned Layout Level 4 §3's top layer rather than stored, plus TOPMOST POPOVER ANCESTOR and
 * TOPMOST AUTO OR HINT POPOVER over them, all of them flat-tree or list questions with none of the page's own
 * code in them. The WRITE half is §6.12's HIDE POPOVER STACK UNTIL, whose steps 5 and 8 "run the hide popover
 * algorithm given popover" over each popover they hide, firing `beforetoggle` at the page's own listeners — so
 * it suspends at every member, and it is built here as a `_run` cursor two algorithms hold: hide a popover step
 * 11 runs it up to three times and show popover step 15 up to twice. See popover.h for why the hide it calls is
 * a step machine reached through step_call_run rather than a cursor the caller embeds, and why the recursion
 * between the two (a hide contains a stack-until contains a hide) is what settles that; the recursion passes
 * through the CALL, so each frame holds at most one cursor.
 *   WHAT THAT UNBLOCKED IS NOT ONE STEP BUT THE WHOLE CHAIN BEHIND IT. Show popover step 15.9 is the ONLY
 * writer of an element's OPENED IN POPOVER MODE, and both showing-popover lists filter on it, so until step
 * 15.3 could run, every read of those lists in this file was correct and VACUOUS and hide a popover step 11's
 * three conditions could never be true. With the stack built, steps 15.5 through 15.10 follow it: the
 * originalType re-read, the second check popover validity, step 15.8's shouldRestoreFocus, step 15.9's mode
 * write, and step 15.10's close watcher — which is what puts a POPOVER-kind watcher in §6.10.2's manager for
 * the first time. Step 17's originallyFocusedElement and step 24's restore become live with it, over HTML
 * §6.6.2 Data model's focused area, which core/html/focus.h exports as its DOM ANCHOR; hide a popover step
 * 20.2 asks the same reader and runs §6.6.4's focusing steps through the same door.
 *   WHAT REMAINS OWED TO §6.12, AND IT IS OWED FROM ELSEWHERE. §6.12.2 Popover light dismiss and §6.12's HIDE
 * POPOVERS UNTIL — which Fullscreen §2 Model's fullscreen an element step 2 runs — are the two other consumers
 * of the stack, and each belongs with the caller that needs it; core/fullscreen/fullscreen.h names the second
 * as part of what its next diff builds. And §6.10.2's close-action dispatch for a POPOVER watcher is written
 * and its watcher is now established and the action is still UNRUN — not because §6.10.1 Close requests has no
 * home (core/html/close_request.c is those nine steps) but because nothing DELIVERS a potential close request,
 * which is close_request.h's own named residual and what close_watcher.h's residuals (a) and (b) now say. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/css/top_layer.h"
#include "core/dom/attr_list.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/events/event_target.h"
#include "core/html/close_watcher.h"
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
    PS_CLOSE_WATCHER,    /* "popover close watcher, a close watcher or null, initially null" */
    /* "Each HTML element has a previously focused element which is null or an element, and it is initially
       null" — defined by HTML §4.11.4 The dialog element, NOT by §6.6 Focus, which is what this line used to
       say and what citegen's term check reports. §6.12 steps 16 and 24 write it and its step 19 reads it. */
    PS_PREV_FOCUSED,
    PS_DOC_SHOWING,      /* Document: "showing popover, a boolean, initially false" */
    PS_DOC_NESTING,      /* Document: "hiding popover nesting count, a number, initially 0" */
    PS_DOC_HINT_PARENT,  /* Document: "hint stack parent, an HTML element or null, initially null" */
    PS_SLOT_N
};
static const char *const PS_SLOT_NAME[PS_SLOT_N] = {
    "popoverVisibilityState", "popoverTrigger", "popoverHiding", "popoverToggleTaskTracker",
    "popoverToggleTaskTrackerTask", "openedInPopoverMode", "popoverCloseWatcher",
    "popoverPreviouslyFocusedElement", "documentShowingPopover", "documentHidingPopoverNestingCount",
    "documentHintStackParent"
};
static JSValue g_slot_key[PS_SLOT_N];
static JSAtom  g_slot_atom[PS_SLOT_N];

static int g_id_show = -1, g_id_hide = -1, g_id_toggle = -1;
static int g_toggle_task_stepid = -1;
/* §6.12's HIDE A POPOVER: its step-def id, which is the RUNTIME's, and the per-realm slot holding the function
   object §6.12's own prose and §6.10.2's close action reach it through. See popover.h. */
static int g_hide_stepid = -1;
static int g_hide_fn_slot = -1;

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

/* ---- §6.12's AUTO/HINT STACK, READ SIDE ----------------------------------------------------------------------
 *
 * THE STACK IS NOT A SECOND STRUCTURE. §6.12 keeps NO list of showing popovers anywhere: its showing auto
 * popover list and showing hint popover list are DERIVED, each time they are asked for, by filtering CSS
 * Positioned Layout Level 4 §3's top layer — "For each Element element of document's top layer … then append
 * element to popovers". So the ORDER of the stack is the top layer's order and nothing else, which is why this
 * half of §6.12 is a read over core/css/top_layer.c and holds no state of its own. A cached list beside the
 * layer would be the second copy of one fact that CLAUDE.md's §A-FIELD-A-CONSUMER-DEFAULTS is about, and it
 * would drift on exactly the operation §3.3 exists for: a removal from the MIDDLE.
 *
 * WHAT THIS SECTION IS AND IS NOT. It is the READ side — the two lists, TOPMOST POPOVER ANCESTOR and TOPMOST
 * AUTO OR HINT POPOVER, all of them questions with no page code in them, which is why every one of them is a
 * plain C function its callers ask without a stage. The WRITE side is HIDE POPOVER STACK UNTIL, whose steps 5
 * and 8 run the hide a popover algorithm over each popover they hide and so run the page's own `beforetoggle`
 * listeners: it is a `_run` cursor, and it has its own section below. */

/* §6.12's "opened in popover mode … 'auto', 'hint', or null, initially null", as one of POPOVER_STATE_AUTO,
   POPOVER_STATE_HINT, or POPOVER_STATE_NONE for the standard's null. The two spellings the standard writes as
   strings are held as the SAME enum the `popover` attribute's four states use, because they are the same
   vocabulary — "opened in popover mode" records which of §6.12's states the element was SHOWN in, and step 15's
   effectiveType is what decides it. An ABSENT slot is the initial null. */
static int popover_opened_mode(JSContext *ctx, JSValueConst el)
{
    JSValue v = ps_get(ctx, el, PS_OPENED_MODE);
    int32_t mode = POPOVER_STATE_NONE;

    if (!JS_IsUndefined(v)) JS_ToInt32(ctx, &mode, v);
    JS_FreeValue(ctx, v);
    DCHECK(mode == POPOVER_STATE_NONE || mode == POPOVER_STATE_AUTO || mode == POPOVER_STATE_HINT,
           "an element's §6.12 OPENED IN POPOVER MODE slot held a state that is neither Auto nor Hint — the "
           "standard's three values are \"auto\", \"hint\" and null, show popover step 15.9 is the only writer "
           "of the first two and hide a popover step 15 the only writer of the third (as the slot's absence)");
    return mode;
}

/* The predicate both list getters are, differing only in which mode they ask for. §6.12: "If all of the
   following are true: element is an HTML element; element's opened in popover mode is "auto"; and element's
   popover visibility state is showing, then append element to popovers."
   IT RUNS NO PAGE CODE, which is the contract top_layer.h states for a collect predicate: a brand check and two
   own-slot reads, no getter, no event, no §3.3 Top Layer Manipulation algorithm. */
static bool popover_showing_in_mode(JSContext *ctx, JSValueConst el, void *opaque)
{
    const int *want = opaque;

    DCHECK(want != NULL && (*want == POPOVER_STATE_AUTO || *want == POPOVER_STATE_HINT),
           "§6.12's showing-popover-list filter was asked for a mode that is neither Auto nor Hint — the "
           "standard defines exactly two such lists");
    if (!html_element_is(el)) return false;
    if (popover_opened_mode(ctx, el) != *want) return false;
    return popover_is_showing(ctx, el);
}

/* §6.12's "To get the showing auto popover list for a Document document" and its hint twin, which are the same
   three steps over the same set with one word changed. OWNED — a fresh Array in top layer order, which is what
   "Let popovers be « »" is and what makes an index into it an index into a value. */
static JSValue popover_showing_list(JSContext *ctx, JSValueConst doc, int mode)
{
    return top_layer_collect(ctx, doc, popover_showing_in_mode, &mode);
}

/* The length of one of the standard's own « » lists. These are this file's values, built by the collect above,
   so `length` is a plain own property and no page code can answer it. */
static uint32_t popover_list_len(JSContext *ctx, JSValueConst list)
{
    JSValue lv;
    uint32_t n = 0;

    DCHECK(JS_IsArray(list), "a §6.12 popover list was measured before it was built");
    lv = JS_GetPropertyStr(ctx, list, "length");
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    return n;
}

/* Infra's list CONTAINS, answering the POSITION rather than the boolean — because §6.12 asks for both over the
 * same lists and from the same place: hide popover stack until step 2 is "let lastHideIndex be 0 if popoverList
 * does not contain endpoint; otherwise the index of endpoint in popoverList plus 1", while its step 8's first
 * sub-step, hide a popover steps 9 and 10, and show popover step 15.9's two asserts each want only the
 * membership. One scan answers both, and a caller wanting the boolean writes `>= 0`.
 *
 * A POSITION IS PERMITTED HERE FOR EXACTLY ONE REASON: the list is a FRESH Array this file built out of the top
 * layer, so an index into it is an index into a VALUE and not into a view. That is the distinction
 * core/css/top_layer.h draws between its collect and its topmost walk — the walk refuses to hand out a rank
 * precisely because the top layer is mutated by algorithms a page reaches, and CLAUDE.md's
 * §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED permits an ordinal only over a set nothing can renumber
 * under a suspension. Nothing renumbers this one: it is the running flow's own snapshot, and hide popover stack
 * until's steps 6 and 7 exist precisely because the LIVE set may have moved and must be re-read rather than
 * re-indexed.
 *
 * IDENTITY IS THE WRAPPER'S, which is the same comparison check popover validity step 3 makes over documents: a
 * node has exactly one wrapper per realm, and every member of these lists came out of one collect. A null or
 * absent `item` is contained in nothing, which is what makes step 2's "does not contain" the answer for hide
 * popover stack until's null-endpoint callers rather than a case they have to test for themselves. */
static int64_t popover_list_index_of(JSContext *ctx, JSValueConst list, uint32_t n, JSValueConst item)
{
    uint32_t i;

    DCHECK(JS_IsArray(list), "a §6.12 popover list was searched before it was built");
    if (JS_IsNull(item) || JS_IsUndefined(item)) return -1;
    for (i = 0; i < n; i++) {
        JSValue m = JS_GetPropertyUint32(ctx, list, i);
        bool same = JS_VALUE_GET_PTR(m) == JS_VALUE_GET_PTR(item);

        JS_FreeValue(ctx, m);
        if (same) return (int64_t)i;
    }
    return -1;
}

/* §6.12's "To find the topmost auto or hint popover given a Document document", 3 steps: "If document's showing
 * hint popover list is not empty, then return document's showing hint popover list's last element. If
 * document's showing auto popover list is not empty, then return document's showing auto popover list's last
 * element. Return null."
 *
 * THE HINT LIST IS ASKED FIRST AND THAT IS NOT AN OPTIMISATION — it is the same priority order step 15.2's note
 * states ("Hint popovers are lower priority than Auto popovers, so an Auto popover cannot have a Hint popover as
 * a 'parent'"), read from the other end: a showing hint popover is always above every auto popover, so it is
 * always the topmost of the two stacks. Its ONE caller is show popover step 15.8, whose only question is whether
 * the answer is NULL — "this ensures that focus is returned to the previously-focused element only for the first
 * popover in a stack" — but it is written as the standard writes it, returning the element, because §6.12.2
 * Popover light dismiss asks the same algorithm for the element itself.
 *
 * IT RUNS NO PAGE CODE: both lists are derived reads over the top layer with a C predicate, which is the
 * contract top_layer.h states for a collect. OWNED, and JS_NULL for the standard's null. */
static JSValue popover_topmost_auto_or_hint(JSContext *ctx, JSValueConst doc)
{
    JSValue hints = popover_showing_list(ctx, doc, POPOVER_STATE_HINT);
    JSValue autos, found = JS_NULL;
    uint32_t n = popover_list_len(ctx, hints);

    if (n > 0) found = JS_GetPropertyUint32(ctx, hints, n - 1);                                  /* step 1 */
    JS_FreeValue(ctx, hints);
    if (!JS_IsNull(found)) return found;
    autos = popover_showing_list(ctx, doc, POPOVER_STATE_AUTO);
    n = popover_list_len(ctx, autos);
    if (n > 0) found = JS_GetPropertyUint32(ctx, autos, n - 1);                                  /* step 2 */
    JS_FreeValue(ctx, autos);
    return found;                                                                                /* step 3 */
}

/* "`node` is a FLAT TREE DESCENDANT of `ancestor`" — CSS Scoping §4.1 "Flattening the DOM into an Element
 * Tree", which is the tree §6.12's topmost popover ancestor walks and NOT the node tree.
 *
 * THE NODE TREE IS THAT TREE EXACTLY WHEN NO SHADOW HOST IS ON THE PATH, which §4.1's own note states: "the flat
 * tree is the top-level DOM tree, but shadow hosts are filled with their shadow tree children instead of their
 * light tree children (and this proceeds recursively if the shadow tree contains any shadow hosts)". So a node
 * whose parent hosts a shadow root is not a flat-tree child of that parent at all — it is a child of whichever
 * `slot` it is assigned to, or of nothing — and a node whose parent IS a shadow root has the HOST as its
 * flat-tree parent. Both of those make the two trees different here, and this engine has only the second, so
 * both CRASH rather than answering from the tree the standard does not name. core/html/html_element_view.c
 * reaches the same wall from CSSOM VIEW §7's side and names the same two halves to build.
 *
 * THE WALK IS STRICT — DOM's "descendant" is not inclusive — and it is over NODES rather than elements, because
 * §6.12 declares the subject a Node ("given a Node newPopoverOrTopLayerElement") and Fullscreen §2's caller
 * hands it a top layer element that may not be a popover at all. */
static bool popover_flat_tree_descendant(JSContext *ctx, const lxb_dom_node_t *node,
                                         const lxb_dom_node_t *ancestor)
{
    const lxb_dom_node_t *cur;

    DCHECK(node != NULL && ancestor != NULL,
           "§6.12's flat tree descendant test was given no node or no candidate ancestor — its one caller "
           "walks a list this file built out of live top layer members and holds the subject across no "
           "suspension");
    for (cur = node; cur != NULL && cur->parent != NULL; cur = cur->parent) {
        lxb_dom_node_t *p = cur->parent;

        if (shadow_root_is(p))
            DFAIL("HTML §6.12 The popover attribute's topmost popover ancestor walks the FLAT TREE, and this "
                  "node's parent IS A SHADOW ROOT — CSS Scoping §4.1 Flattening the DOM into an Element Tree "
                  "puts a shadow root's children under the HOST, so the node tree's parent chain leaves the "
                  "tree here and the flat tree does not. Walking on would answer a popover ancestry question "
                  "with a chain the flat tree has no edge in — a WRONG ancestor, not an absent one. BUILD the "
                  "flat-tree parent (the host, for a shadow root's own children; DOM §4.2.2's assigned slot, "
                  "for a slottable) as one component and ask it here and at core/html/html_element_view.c's "
                  "CSSOM VIEW §7 walk, which is stopped by the same absence");
        if (p->type == LXB_DOM_NODE_TYPE_ELEMENT &&
            shadow_root_of_element(ctx, lxb_dom_interface_element(p)) != NULL)
            DFAIL("HTML §6.12 The popover attribute's topmost popover ancestor walks the FLAT TREE, and an "
                  "element on this chain HOSTS A SHADOW ROOT — CSS Scoping §4.1 Flattening the DOM into an "
                  "Element Tree fills a shadow host with its SHADOW tree's children instead of its light "
                  "ones, so this node is a flat-tree child of the `slot` it is assigned to (or of nothing at "
                  "all) and not of the host. Answering from the node tree would make a light child of a "
                  "shadow host count as nested inside a popover the flat tree never nests it in. BUILD the "
                  "flat-tree parent over DOM §4.2.2's assigned slot and slottable and ask it here");
        if (p == ancestor) return true;
    }
    return false;
}

/* "the index of the LAST item in `list` of which `node` is a flat tree descendant, otherwise -1" — the one
   question §6.12's topmost popover ancestor steps 5 and 7 both ask, of two different nodes over one list. The
   scan runs BACKWARDS and stops at the first hit, because the last match is the answer. */
static int64_t popover_last_flat_ancestor_index(JSContext *ctx, JSValueConst list, uint32_t n,
                                                const lxb_dom_node_t *node)
{
    uint32_t i;

    if (node == NULL) return -1;
    for (i = n; i > 0; i--) {
        JSValue cand = JS_GetPropertyUint32(ctx, list, i - 1);
        bool hit = popover_flat_tree_descendant(ctx, node, node_of(cand));

        JS_FreeValue(ctx, cand);
        if (hit) return (int64_t)(i - 1);
    }
    return -1;
}

/* §6.12's TOPMOST POPOVER ANCESTOR, ten steps: "To find the topmost popover ancestor, given a Node
 * newPopoverOrTopLayerElement, an HTML element or null source, and a boolean isPopover, perform the following
 * steps. They return an HTML element or null."
 *
 * ITS ORDER IS `combinedPopovers`' AND NOT THE TOP LAYER'S, WHICH IS THE ONE THING A PLAUSIBLE VERSION GETS
 * WRONG. Step 4 is "Let combinedPopovers be document's showing auto popover list extended with document's
 * showing hint popover list" — a CONCATENATION, so every hint popover sorts after every auto popover in it
 * whatever their relative positions in the top layer are. That is why this is not `top_layer_topmost` with a
 * predicate: that walk answers over §3's order, and over combinedPopovers the two orders are different lists
 * whenever both are non-empty. A single backwards walk of the top layer would answer with whichever of the two
 * candidates was shown later, where the standard answers with the HINT one.
 *
 * AND IT ASKS FOR A MAXIMUM OF TWO RANKS, which is the second thing `top_layer_topmost` structurally cannot do
 * and deliberately so — it answers with the member and never with its rank, precisely so no caller can record a
 * position into a live set. Steps 5 through 8 need `max(popoverAncestorIndex, sourceAncestorIndex)`, and those
 * are positions in the SNAPSHOT this algorithm itself built, which is a value nothing mutates: top_layer.h's
 * collect exists for exactly that distinction. The two searches are written as the standard writes them rather
 * than fused into one disjunctive scan, because the standard's two indices are two separate facts (a nesting
 * relationship and a `source` relationship) and its own note explains them apart.
 *
 * `source` is an HTML element or JS_NULL. The answer is OWNED, and JS_NULL for the standard's null. */
static JSValue popover_topmost_ancestor(JSContext *ctx, JSValueConst subject, JSValueConst source,
                                        bool is_popover)
{
    JSValue doc, autos, hints, combined, found;
    uint32_t na, nh, n, i;
    int64_t popover_index, source_index, ancestor_index;

    if (is_popover) {
        /* Step 1's three asserts. */
        lxb_dom_node_t *sn = node_of(subject);
        int state;

        DCHECK(html_element_is(subject),
               "HTML §6.12 The popover attribute's topmost popover ancestor step 1 asserts that "
               "newPopoverOrTopLayerElement is an HTML element when isPopover is true — show popover step 15.1 "
               "is the caller that passes true, and its receiver has already been brand-checked");
        DCHECK(sn != NULL && sn->type == LXB_DOM_NODE_TYPE_ELEMENT, "a §6.12 popover has no element node");
        state = popover_attribute_state(lxb_dom_interface_element(sn));
        DCHECK(state == POPOVER_STATE_AUTO || state == POPOVER_STATE_HINT,
               "HTML §6.12 The popover attribute's topmost popover ancestor step 1 asserts that "
               "newPopoverOrTopLayerElement's popover attribute is in neither the No Popover state nor the "
               "Manual state — show popover step 15 is the caller, and its own condition is that originalType "
               "is Auto or Hint");
        DCHECK(!popover_is_showing(ctx, subject),
               "HTML §6.12 The popover attribute's topmost popover ancestor step 1 asserts that "
               "newPopoverOrTopLayerElement's popover visibility state is not showing — show popover step 3's "
               "check popover validity has already certified it hidden, and step 20 is what makes it showing");
    } else {
        /* Step 2. The `false` caller is Fullscreen §2 Model's fullscreen an element ("Let hideUntil be the
           result of running topmost popover ancestor given element, null, and false"), whose subject is a top
           layer element that need not be a popover at all — §6.12's own note: "If the provided element is a
           top layer element such as a dialog which is not showing as a popover, then topmost popover ancestor
           will only look in the node tree to find the first popover." */
        DCHECK(JS_IsNull(source) || JS_IsUndefined(source),
               "HTML §6.12 The popover attribute's topmost popover ancestor step 2 asserts that source is null "
               "when isPopover is false — a non-popover subject has no invoker, so a caller passing one has "
               "confused the two entries");
    }
    doc = popover_document_of(ctx, subject);                                                     /* step 3 */
    DCHECK(!JS_IsNull(doc), "§6.12's topmost popover ancestor was given a node with no node document");
    /* Step 4 — EXTENDED WITH, so the whole hint list follows the whole auto list. */
    autos = popover_showing_list(ctx, doc, POPOVER_STATE_AUTO);
    hints = popover_showing_list(ctx, doc, POPOVER_STATE_HINT);
    na = popover_list_len(ctx, autos);
    nh = popover_list_len(ctx, hints);
    combined = JS_NewArray(ctx);
    CHECK(!JS_IsException(combined), "§6.12's combinedPopovers list could not be allocated");
    for (i = 0; i < na; i++)
        JS_SetPropertyUint32(ctx, combined, i, JS_GetPropertyUint32(ctx, autos, i));
    for (i = 0; i < nh; i++)
        JS_SetPropertyUint32(ctx, combined, na + i, JS_GetPropertyUint32(ctx, hints, i));
    JS_FreeValue(ctx, hints);
    JS_FreeValue(ctx, autos);
    n = na + nh;

    popover_index = popover_last_flat_ancestor_index(ctx, combined, n, node_of(subject));        /* step 5 */
    source_index = -1;                                                                           /* step 6 */
    if (!JS_IsNull(source) && !JS_IsUndefined(source))                                            /* step 7 */
        source_index = popover_last_flat_ancestor_index(ctx, combined, n, node_of(source));
    ancestor_index = popover_index > source_index ? popover_index : source_index;                /* step 8 */
    found = ancestor_index < 0 ? JS_NULL                                                          /* step 9 */
                               : JS_GetPropertyUint32(ctx, combined, (uint32_t)ancestor_index);  /* step 10 */
    JS_FreeValue(ctx, combined);
    return found;
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

/* ---- §6.12's HIDE POPOVER STACK UNTIL ------------------------------------------------------------------------
 *
 * "To hide popover stack until, given a Document document, an HTML element or null endpoint, an Auto or Hint
 * stackType, a boolean focusPreviousElement, and a boolean fireEvents", 8 steps. This is the WRITE half of
 * §6.12's Auto/Hint stack — the half the read side above could not be, because its steps 5 and 8 "run the hide
 * popover algorithm given popover", and every one of those hides fires `beforetoggle` at the page's own
 * listeners.
 *
 * IT IS A `_run` CURSOR AND NOT A REGISTERED MACHINE, WHICH IS THE OTHER HALF OF popover.h's ARGUMENT. That file
 * says hide a popover must be a CALL because §6.12 is mutually recursive — a hide contains a stack-until
 * contains a hide, to a depth the page's markup decides — and a struct embedded in its caller cannot contain
 * itself. The recursion passes through that CALL, which is a heap frame, so each frame holds AT MOST ONE
 * stack-until cursor and a cursor is the right shape for this one: core/html/close_watcher.h's CloseWatcherRun
 * and core/html/focus.h's focus_element_run are the same shape for the same reason. Its two holders are the two
 * algorithms of §6.12 that run it — hide a popover step 11 and show popover step 15 — and neither can be inside
 * the other without a call in between.
 *
 * IT WALKS COPIES, AND THE STANDARD IS WHAT MAKES THEM COPIES. Steps 3 and 4 are SLICES of a popoverList that is
 * itself a fresh Array derived from the top layer, so the walk's order is fixed at step 3 whatever step 5's
 * hides do to the live set — and a member some `beforetoggle` handler hid meanwhile is not SKIPPED but ASKED,
 * because hide a popover step 1's check popover validity answers false for a popover that is no longer showing
 * and step 2 returns. That is the identical argument close_watcher.c records for copying a group before
 * walking it, and it is why steps 6 and 7 RE-READ rather than re-index: the standard's own note says what they
 * are for — "This happens if popovers are shown whilst hiding popovers. For example, in beforetoggle events."
 *
 * IT YIELDS BETWEEN MEMBERS. A showing-popover list is of the PAGE's size, so walking it inside one C activation
 * is the drive-to-completion this engine has no other bound against; JS_STEP_YIELD after each hide is what makes
 * a stack of ten thousand fair BFS work. It is not a bound — nothing is dropped, skipped or counted.
 *
 * EVERY ONE OF ITS EIGHT STEPS HOLDS NO SUB-LIST EXCEPT STEP 8, WHICH HOLDS EXACTLY ONE, so the bare `step 8.1`
 * and `step 8.2` below are unambiguous under CLAUDE.md's rule and steps 1-7 have no sub-numbers to be ambiguous
 * about. */
enum { HPSU_START = 0, HPSU_HIDE, HPSU_CHECK, HPSU_DONE };

typedef struct {
    uint8_t  stage;      /* HPSU_* — this algorithm's own cursor */
    uint8_t  hphase;     /* the hide a popover CALL's own phase, in whichever of steps 5 and 8 is walking */
    uint32_t i;          /* the index, into THIS RUN'S OWN copy, of the member step 5 or step 8 is standing on */
    JSValue  to_hide;    /* step 3's toHide, already in REVERSE order (owned) */
    JSValue  to_remain;  /* step 4's toRemain (owned) */
    JSValue  to_check;   /* step 7's toCheck, already in REVERSE order (owned) */
    /* step_call_run's [this, hide, popover, focusPreviousElement, fireEvents, throwExceptions, source] — named
       by popover.h's TYPE rather than by a width, so a caller cannot be an argument behind the algorithm. */
    PopoverHideCb cb;
} PopoverStackUntilRun;

/* Place the run's owned fields before anything can fail, and RESET its cursor — a holder calls this before each
   run, because §6.12 runs this algorithm up to three times in one hide a popover and twice in one show popover
   and each of those is the algorithm from its own step 1. */
static void popover_stack_until_init(PopoverStackUntilRun *r)
{
    int k;

    r->stage = HPSU_START;
    r->hphase = 0;
    r->i = 0;
    r->to_hide = r->to_remain = r->to_check = JS_UNDEFINED;
    STEP_CB_FOREACH(r->cb, k) r->cb[k] = JS_UNDEFINED;
}

/* WHAT THIS RUN OWNS — the holding machine's `visit` forwards to it, so a fork mid-walk gives each arm its own
   slices rather than two flows one, and a teardown releases them. */
static void popover_stack_until_visit(JSContext *ctx, PopoverStackUntilRun *r, JSStepVisit *v)
{
    int k;

    v->val(ctx, &r->to_hide);
    v->val(ctx, &r->to_remain);
    v->val(ctx, &r->to_check);
    STEP_CB_FOREACH(r->cb, k)
        v->val(ctx, &r->cb[k]);
}

/* Steps 5 and 8's shared body: "run the hide popover algorithm given popover, focusPreviousElement, <fireEvents
   or false>, false, and null." The FOURTH argument is false at both sites — this algorithm's callers have
   nobody to throw to — and the FIFTH is null, which is the argument popover.c's own member entry used to get
   wrong. Returns step_call_run's own contract: >0 for the caller to return, -1 with a throw live, 0 once the
   hide has finished. */
static int popover_stack_until_hide(JSContext *ctx, PopoverStackUntilRun *r, JSValueConst popover,
                                    bool focus_previous_element, bool fire_events, JSValue in,
                                    JSValue **out_cb, int *out_argc)
{
    JSValueConst argv[POPOVER_HIDE_ARGC];
    JSValue hide = popover_hide_algorithm(ctx);
    JSValue out = JS_UNDEFINED;
    int rc;

    argv[0] = popover;
    argv[1] = focus_previous_element ? JS_TRUE : JS_FALSE;
    argv[2] = fire_events ? JS_TRUE : JS_FALSE;
    argv[3] = JS_FALSE;   /* throwExceptions */
    argv[4] = JS_NULL;    /* source */
    rc = step_call_run(ctx, &r->hphase, STEP_CB(r->cb), hide, JS_UNDEFINED,
                       POPOVER_HIDE_ARGC, argv, in, &out, out_cb, out_argc);
    JS_FreeValue(ctx, hide);
    if (rc) return rc;
    if (JS_IsException(out)) return -1;
    DCHECK(JS_IsUndefined(out),
           "§6.12's hide a popover answered hide popover stack until with a value — it returns nothing, and "
           "throwExceptions is false at both of steps 5 and 8, so the only completions this call has are "
           "undefined and a throw the engine itself raised");
    JS_FreeValue(ctx, out);
    return 0;
}

/* `endpoint` is an HTML element or JS_NULL; `stack_type` is POPOVER_STATE_AUTO or POPOVER_STATE_HINT.
   Returns JS_STEP_CALL / JS_STEP_YIELD (the caller returns it), -1 with a throw live in the context, or 0 when
   the eight steps have finished — the same contract close_watcher.h's three run algorithms carry. */
static int popover_stack_until_run(JSContext *ctx, PopoverStackUntilRun *r, JSValueConst document,
                                   JSValueConst endpoint, int stack_type, bool focus_previous_element,
                                   bool fire_events, JSValue in, JSValue **out_cb, int *out_argc)
{
    int rc;

    DCHECK(stack_type == POPOVER_STATE_AUTO || stack_type == POPOVER_STATE_HINT,
           "§6.12's hide popover stack until was given a stackType that is neither of the two the standard "
           "declares — it takes \"an Auto or Hint stackType\", and every one of its five callers names one of "
           "those two words");
    DCHECK(JS_IsNull(endpoint) || html_element_is(endpoint),
           "§6.12's hide popover stack until was given an endpoint that is neither an HTML element nor null — "
           "it takes \"an HTML element or null endpoint\", and hide a popover step 11.2 is the caller that "
           "passes the null");

    if (r->stage == HPSU_START) {
        JSValue list;
        uint32_t n, last, i;
        int64_t at;

        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        list = popover_showing_list(ctx, document, stack_type);                                  /* step 1 */
        n = popover_list_len(ctx, list);
        at = popover_list_index_of(ctx, list, n, endpoint);                                      /* step 2 */
        last = at < 0 ? 0 : (uint32_t)at + 1;
        DCHECK(last <= n, "§6.12's hide popover stack until cut its popoverList past its own end");
        /* Step 3 — "a slice of popoverList from lastHideIndex, IN REVERSE ORDER". The reversal is done HERE,
           into the run's own value, rather than left to a backwards cursor, because step 7 needs the same
           operation over a DIFFERENT list and one shape serves both; a cursor that counted down would also be
           an index whose direction the resume has to re-derive. */
        r->to_hide = JS_NewArray(ctx);
        CHECK(!JS_IsException(r->to_hide), "§6.12's hide popover stack until could not allocate toHide");
        for (i = 0; i < n - last; i++)
            JS_SetPropertyUint32(ctx, r->to_hide, i, JS_GetPropertyUint32(ctx, list, n - 1 - i));
        r->to_remain = JS_NewArray(ctx);                                                         /* step 4 */
        CHECK(!JS_IsException(r->to_remain), "§6.12's hide popover stack until could not allocate toRemain");
        for (i = 0; i < last; i++)
            JS_SetPropertyUint32(ctx, r->to_remain, i, JS_GetPropertyUint32(ctx, list, i));
        JS_FreeValue(ctx, list);
        r->i = 0;
        r->stage = HPSU_HIDE;
    }

    if (r->stage == HPSU_HIDE) {
        /* Step 5 — "For each popover of toHide: run the hide popover algorithm given popover,
           focusPreviousElement, fireEvents, false, and null." */
        if (r->i < popover_list_len(ctx, r->to_hide)) {
            JSValue popover = JS_GetPropertyUint32(ctx, r->to_hide, r->i);

            rc = popover_stack_until_hide(ctx, r, popover, focus_previous_element, fire_events, in,
                                          out_cb, out_argc);
            JS_FreeValue(ctx, popover);
            if (rc) return rc;
            r->i++;
            /* ONE MEMBER PER TURN — see the section header. */
            return JS_STEP_YIELD;
        }
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        JS_FreeValue(ctx, r->to_hide);
        r->to_hide = JS_UNDEFINED;
        {
            /* Steps 6 and 7 — the list is RE-READ, because step 5's hides ran the page's own `beforetoggle`
               listeners and one of them may have SHOWN a popover; re-indexing the step-1 snapshot would name
               whichever member has shifted into a position, which is the defect this file's own index rule is
               about. */
            JSValue fresh = popover_showing_list(ctx, document, stack_type);                     /* step 6 */
            uint32_t n = popover_list_len(ctx, fresh), i;

            r->to_check = JS_NewArray(ctx);                                                      /* step 7 */
            CHECK(!JS_IsException(r->to_check), "§6.12's hide popover stack until could not allocate toCheck");
            for (i = 0; i < n; i++)
                JS_SetPropertyUint32(ctx, r->to_check, i, JS_GetPropertyUint32(ctx, fresh, n - 1 - i));
            JS_FreeValue(ctx, fresh);
        }
        r->i = 0;
        r->stage = HPSU_CHECK;
    }

    if (r->stage == HPSU_CHECK) {
        uint32_t n = popover_list_len(ctx, r->to_check);
        uint32_t nr = popover_list_len(ctx, r->to_remain);

        /* Step 8 — "For each popover of toCheck". ONE MEMBER PER TURN HERE TOO, INCLUDING THE SKIPS: `toCheck`
           is the whole showing list and `toRemain` is a slice of it, so a walk that resolved every skip inside
           one C activation would be an O(|toCheck| × |toRemain|) scan of a structure of the PAGE'S size, which
           is the drive-to-completion the yield exists for. A yield nobody is waiting on is re-entered
           immediately and costs one predicted call. */
        if (r->i < n) {
            JSValue popover = JS_GetPropertyUint32(ctx, r->to_check, r->i);

            /* Step 8.1's "If toRemain contains popover, then continue." RE-EVALUATED ON EVERY RE-ENTRY, and
               that is safe here for the reason STEP_GOTO's own note makes it dangerous elsewhere: both operands
               are this run's FROZEN values, so the test cannot decide differently on the way back in and walk
               away from a call it already issued. */
            if (popover_list_index_of(ctx, r->to_remain, nr, popover) >= 0) {
                JS_FreeValue(ctx, popover);
                JS_FreeValue(ctx, in);
                r->i++;
                return JS_STEP_YIELD;
            }
            /* Step 8.2 — "run the hide popover algorithm given popover, focusPreviousElement, false, false,
               and null". `fireEvents` is IGNORED here and false is used instead, which the standard says in as
               many words: "In this additional hiding phase, fireEvents is ignored, and false is used instead." */
            rc = popover_stack_until_hide(ctx, r, popover, focus_previous_element, /*fireEvents*/ false, in,
                                          out_cb, out_argc);
            JS_FreeValue(ctx, popover);
            if (rc) return rc;
            r->i++;
            return JS_STEP_YIELD;
        }
        JS_FreeValue(ctx, in);
        JS_FreeValue(ctx, r->to_check);
        JS_FreeValue(ctx, r->to_remain);
        r->to_check = r->to_remain = JS_UNDEFINED;
        r->stage = HPSU_DONE;
        return 0;
    }

    JS_FreeValue(ctx, in);
    DFAIL("§6.12's hide popover stack until was re-entered after it had finished — its cursor is reset by "
          "whoever starts a new run of it, which is what popover_stack_until_init is for, and §6.12 starts a "
          "new one at each of hide a popover steps 11.1, 11.2 and 11.3 and show popover steps 15.3 and 15.4");
    return 0;
}

/* ---- §6.12's HIDE A POPOVER, as its own step machine ---------------------------------------------------------
 *
 * "To hide a popover given an HTML element element, a boolean focusPreviousElement, a boolean fireEvents, a
 * boolean throwExceptions, and an HTML element or null source", 21 steps. See popover.h for WHY this is a
 * machine reached by CALL rather than a cursor a caller embeds, and why it is a function object of the realm
 * rather than an exported C function.
 *
 * ITS FIVE ARGUMENTS ARE THE STANDARD'S FIVE, IN ITS ORDER, and every one of the four booleans is read from the
 * argument list rather than assumed. That is not tidiness: it is the whole content of this diff, and it FIXED a
 * live defect the member entry could not express. `togglePopover({source: btn})` on a showing popover runs step
 * 5, whose words are "Run the hide popover algorithm given this, true, true, true, and null" — the FIFTH
 * argument is null, so the `source` the member read at step 4 belongs to show popover's step 6 and to nothing
 * else. Hardcoding the member's own `source` into the hide, which is what a single member-entry state does,
 * made `beforetoggle`'s and `toggle`'s `source` attribute the invoker where the standard says null.
 *
 * EVERY ONE OF ITS STEPS HOLDS EXACTLY ONE SUB-LIST, so a bare `step N.M` below is unambiguous: steps 2, 8, 11,
 * 12 and 20 each contain a single `<ol>` and no sibling list, which is the condition CLAUDE.md puts on writing
 * a sub-number without naming its list.
 *
 * IT IS WHOLE — all 21 steps, step 11's block over the document's hint stack parent and its two showing-popover
 * lists included, and step 20.2's §6.6.4 focusing steps over core/html/focus.h's DOM anchor. The one thing it
 * refuses is a previously focused element that is not an ELEMENT, which §6.6.2's viewport anchor legitimately
 * is; that crash names §6.6.4 step 1's getting-the-focusable-area as the door to build, and it fires only for a
 * popover whose focus was moved inside it before the hide. */

typedef struct {
    JSStepHdr hdr;
    uint8_t   fphase;         /* the `beforetoggle` fire request's own phase */
    uint8_t   focus_phase;    /* step 20.2's §6.6.4 focusing steps request's own phase */
    uint8_t   fire_events;    /* the `fireEvents` ARGUMENT, which step 6 may lower */
    uint8_t   nested_hide;    /* step 4's nestedHide */
    uint8_t   nesting_taken;  /* whether step 7's increment is still owed step 8's decrement */
    uint8_t   auto_contains;  /* step 9's autoPopoverListContainsElement */
    uint8_t   hint_contains;  /* step 10's hintPopoverListContainsElement */
    /* WHICH OF STEP 11'S THREE HIDE POPOVER STACK UNTIL RUNS IS NEXT — 0 = step 11.1, 1 = step 11.2, 2 = step
       11.3, 3 = they are done. An ordinal over a set THE STANDARD FIXES at three named sub-steps, which is
       CLAUDE.md's §AN-INDEX-NAMES-A-THING permitted case in its clearest form: nothing the page does can
       renumber the sub-steps of §6.12's step 11. */
    uint8_t   stack_call;
    JSValue   doc;            /* step 3's `document`, bound ONCE (owned) — see popover_hide_cleanup */
    JSValue   ev;             /* step 12.1's ToggleEvent, minted once and held across the dispatch (owned) */
    JSValue   prev_focused;   /* step 19's previouslyFocusedElement, held across step 20.2's request (owned) */
    PopoverStackUntilRun su;  /* step 11's HIDE POPOVER STACK UNTIL cursor, one run at a time */
    EventFireCb cb;           /* the fire request buffer: [this, dispatch, target, event, targetOverride] */
} PopoverHideState;

#define POPOVER_HIDE_STAGES(X) \
    X(PH_ENTER, "HTML §6.12 The popover attribute's hide a popover steps 1-10 and step 11's condition — a " \
                "range because every step in it is ONE O(1) engine action: a check popover validity whose four " \
                "steps are an attribute-state lookup and a connectedness walk, a slot read, a slot write, an " \
                "integer increment, or a derived read of a showing-popover list") \
    X(PH_STACK, "HTML §6.12 The popover attribute's hide a popover step 11 (its three HIDE POPOVER STACK UNTIL " \
                "runs, over the hint list, the hint stack parent and the auto list, then step 11.4's third " \
                "check popover validity)") \
    X(PH_FIRE,  "HTML §6.12 The popover attribute's hide a popover step 12 (fire an event named beforetoggle, " \
                "using ToggleEvent, with oldState \"open\", newState \"closed\" and source, at element, then " \
                "its second check popover validity and its top-layer removal request)") \
    X(PH_END,   "HTML §6.12 The popover attribute's hide a popover steps 13-20.1 (the immediate top-layer " \
                "removal, the three state clears, the hint stack parent, the queued toggle task and the " \
                "previously focused element) — one O(1) engine action each") \
    X(PH_FOCUS, "HTML §6.12 The popover attribute's hide a popover step 20.2 (run the §6.6.4 focusing steps " \
                "for the previously focused element) and step 21's cleanupSteps")
enum { POPOVER_HIDE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const POPOVER_HIDE_STEPS[] = { POPOVER_HIDE_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* Step 8's cleanupSteps, whole: lower the hiding boolean unless this is a nested hide, destroy the close
   watcher, decrement the nesting count. Idempotent through `nesting_taken`, because step 21 runs it on the way
   out and the machine's `fini` runs it again for a flow that never got there.
   IT DECREMENTS STEP 3'S `document` AND NEVER THE ELEMENT'S CURRENT ONE. A `beforetoggle` handler may adopt the
   element into another document between step 7's increment and this decrement — step 12.2's check popover
   validity is written to catch exactly that — and the count this owes belongs to the document that was
   incremented, which is why step 3's binding is held on the state rather than re-derived here. */
static void popover_hide_cleanup(JSContext *ctx, PopoverHideState *s)
{
    JSValueConst element;
    JSValue watcher;

    if (!s->nesting_taken) return;
    s->nesting_taken = 0;
    element = step_arg(&s->hdr, 0);
    if (!s->nested_hide) ps_clear(ctx, element, PS_HIDING);                                    /* step 8.1 */
    /* Step 8.2 — "If element's popover close watcher is not null: destroy element's popover close watcher; set
       element's popover close watcher to null." Show popover step 15.10 is the only thing that puts one there,
       and this is the only thing that takes it away, which is why the two are read together. */
    watcher = ps_get(ctx, element, PS_CLOSE_WATCHER);
    if (!JS_IsUndefined(watcher)) {
        JSContext *wctx = popover_document_realm(element);

        DCHECK(wctx != NULL,
               "HTML §6.12 The popover attribute's hide a popover step 8.2 must DESTROY the element's popover "
               "close watcher, and the element's node document is no realm's active document — HTML §6.10.2 "
               "Close watcher infrastructure's destroy reaches the manager of the WINDOW the watcher was "
               "established in, which show popover step 15.10 states is \"element's relevant global object\". "
               "Reaching here means a document stopped being active between the establish and this hide, so "
               "the watcher's own window must be carried on the watcher rather than re-derived here");
        close_watcher_destroy(wctx, watcher);                                              /* step 8.2's destroy */
        ps_clear(ctx, element, PS_CLOSE_WATCHER);                                          /* step 8.2's null */
    }
    JS_FreeValue(ctx, watcher);
    ps_set(ctx, s->doc, PS_DOC_NESTING, JS_NewInt32(ctx, popover_nesting(ctx, s->doc) - 1));    /* step 8.3 */
}

/* The TAIL steps 2, 11.5 and 12.3 share: "If throwExceptions is true and validityResult is a DOMException, then
   throw validityResult." followed by "Return."
   `validity` is popover_check_validity's 0 (it answered false, and there is nothing to throw) or -1 (it threw,
   and the exception is live in the context). BOTH of that function's throws ARE DOMExceptions, which is what
   makes the standard's second conjunct decided by the first. A caller passing throwExceptions=false is then
   ANSWERED rather than abandoned, which is what §6.12's hide popover stack until and §6.10.2's close action
   both require — and what the member entry, which hardcoded true, could not do. */
static int popover_hide_return(JSContext *ctx, int validity, bool throw_exceptions)
{
    DCHECK(validity == 0 || validity == -1,
           "§6.12's hide a popover reached one of its three not-true arms with a validityResult that IS true — "
           "each of steps 2, 11.5 and 12.3 is guarded by its own \"If validityResult is not true\"");
    if (validity == -1 && throw_exceptions) return JS_STEP_ABRUPT;
    if (validity == -1) JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_STEP_DONE;
}

/* STEP 12'S CONDITION AND THE STAGE THAT FOLLOWS IT — the one place step 11's two arms converge, written once
   because the decision is step 12's and not either arm's, and a copy at each arm is one condition two sites
   must stay abreast of. Returns the stage, or -1 when step 12.1's event could not be minted. */
static int popover_hide_stage_after_11(JSContext *ctx, PopoverHideState *s, JSValueConst source)
{
    if (!s->fire_events) return PH_END;
    /* Step 12.1's event, minted here so a failure to allocate it is answered before the fire's own phase is
       entered. NOT cancelable: §6.12 names three initialisers for this fire and no others, which is why a hide
       cannot be prevented and why step 12.2 re-reads validity instead of reading a canceled flag. */
    s->ev = toggle_event_new(ctx, "beforetoggle", POPOVER_OPEN, POPOVER_CLOSED, source,
                             /*bubbles*/ false, /*cancelable*/ false);
    if (JS_IsException(s->ev)) {
        s->ev = JS_UNDEFINED;
        return -1;   /* `fini` runs cleanupSteps for this exit, as it does for every other */
    }
    return PH_FIRE;
}

static int js_popover_hide_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    PopoverHideState *s = st;
    JSValueConst element = step_arg(&s->hdr, 0);
    JSValueConst source  = step_arg(&s->hdr, 4);
    bool focus_previous_element = JS_ToBool(ctx, step_arg(&s->hdr, 1)) != 0;
    bool throw_exceptions       = JS_ToBool(ctx, step_arg(&s->hdr, 3)) != 0;
    int validity, r;

    if (s->hdr.stage == PH_ENTER) {
        int k;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY owned field before the first thing that can fail — the failure path tears this state down
           through js_popover_hide_visit, which frees exactly what the state holds, and a zeroed block is not a
           block of JS_UNDEFINEDs. `nesting_taken` is zero in that block, which is what makes `fini` safe on a
           state whose first step never ran. */
        s->doc = s->ev = s->prev_focused = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        popover_stack_until_init(&s->su);
        s->fphase = s->focus_phase = 0;
        s->nested_hide = s->nesting_taken = 0;
        s->auto_contains = s->hint_contains = 0;
        s->stack_call = 0;
        /* THE ARITY IS ASSERTED BEFORE ANY ARGUMENT'S VALUE IS TAKEN, because step_arg answers JS_UNDEFINED
           out of range and JS_ToBool of that is FALSE — so a caller that supplied four arguments would be
           handed a silent `source = null` and a silent `throwExceptions = false` rather than a crash. §6.12
           declares five and every caller states five; none of them is optional. */
        DCHECK(s->hdr.argc == POPOVER_HIDE_ARGC,
               "§6.12's hide a popover was called with an argument count that is not its five — it takes "
               "\"an HTML element element, a boolean focusPreviousElement, a boolean fireEvents, a boolean "
               "throwExceptions\" and a source, and a short call reads the missing ones as false and null");
        DCHECK(html_element_is(element),
               "§6.12's hide a popover was called with something that is not an HTML element — it takes "
               "\"an HTML element element\", and its callers each hand it one they have brand-checked");
        DCHECK(JS_IsNull(source) || html_element_is(source),
               "§6.12's hide a popover was called with a `source` that is neither an HTML element nor null — "
               "every caller in the standard passes null, and show popover step 6's `source` is the only other "
               "value that vocabulary has");
        s->fire_events = JS_ToBool(ctx, step_arg(&s->hdr, 2)) ? 1 : 0;   /* the `fireEvents` ARGUMENT */
        validity = popover_check_validity(ctx, element, /*expectedToBeShowing*/ true, JS_NULL);  /* step 1 */
        if (validity != 1) return popover_hide_return(ctx, validity, throw_exceptions);          /* step 2 */
        s->doc = popover_document_of(ctx, element);                                              /* step 3 */
        DCHECK(!JS_IsNull(s->doc), "§6.12's hide a popover reached step 3 with an element that has no node "
                                   "document — step 1's check popover validity has already refused an element "
                                   "that is not connected");
        s->nested_hide = ps_flag(ctx, element, PS_HIDING) ? 1 : 0;                               /* step 4 */
        ps_set(ctx, element, PS_HIDING, JS_TRUE);                                                /* step 5 */
        if (s->nested_hide) s->fire_events = 0;                                                  /* step 6 */
        ps_set(ctx, s->doc, PS_DOC_NESTING, JS_NewInt32(ctx, popover_nesting(ctx, s->doc) + 1)); /* step 7 */
        s->nesting_taken = 1;                                                                    /* step 8 */
        /* Steps 9 and 10 — "let autoPopoverListContainsElement be true if document's showing auto popover list
           contains element; otherwise false", and its hint twin. BOTH ARE READ HERE, BEFORE step 11 hides
           anything, which is the whole reason the standard binds them to locals: step 11.3's condition is asked
           AFTER step 11.1 and 11.2 have run the page's `beforetoggle` listeners, and by then the live list no
           longer contains this element if one of them hid it. */
        {
            JSValue autos = popover_showing_list(ctx, s->doc, POPOVER_STATE_AUTO);
            JSValue hints = popover_showing_list(ctx, s->doc, POPOVER_STATE_HINT);

            s->auto_contains =
                popover_list_index_of(ctx, autos, popover_list_len(ctx, autos), element) >= 0;    /* step 9 */
            s->hint_contains =
                popover_list_index_of(ctx, hints, popover_list_len(ctx, hints), element) >= 0;   /* step 10 */
            JS_FreeValue(ctx, hints);
            JS_FreeValue(ctx, autos);
        }
        /* Step 11's condition, the standard's own — "If element's opened in popover mode is 'auto' or 'hint'".
           A Manual popover has none, so it takes step 12 directly and enters none of step 11's block. */
        if (!ps_is_null(ctx, element, PS_OPENED_MODE)) {
            s->stack_call = 0;
            STEP_GOTO(s->hdr.stage, PH_STACK, &s->fphase, &s->focus_phase, &s->su.hphase, NULL);
        } else {
            int next = popover_hide_stage_after_11(ctx, s, source);

            if (next < 0) return JS_STEP_ABRUPT;
            STEP_GOTO(s->hdr.stage, next, &s->fphase, &s->focus_phase, &s->su.hphase, NULL);
        }
    }

    if (s->hdr.stage == PH_STACK) {
        /* Steps 11.1, 11.2 and 11.3 — three HIDE POPOVER STACK UNTIL runs, each from its own step 1, each
           suspending at every popover it hides. The sub-index is over the standard's own three sub-steps and
           the CONDITION of each is re-read on every re-entry, which is safe because all three read state no
           step of this algorithm between them writes: steps 9 and 10's booleans are bound above, and the hint
           stack parent is written only by show popover step 19. */
        while (s->stack_call < 3) {
            JSValueConst endpoint = JS_NULL;
            bool run = false;
            int type = POPOVER_STATE_HINT;
            int rc;

            if (s->stack_call == 0) {
                /* Step 11.1 — "If hintPopoverListContainsElement is true, then run hide popover stack until
                   given document, element, Hint, focusPreviousElement, and fireEvents." */
                run = s->hint_contains != 0;
                endpoint = element;
            } else if (s->stack_call == 1) {
                /* Step 11.2 — "If element is document's hint stack parent, then run hide popover stack until
                   given document, null, Hint, focusPreviousElement, and fireEvents." Its note: "If the
                   document's hint stack parent is being hidden, then all hint popovers are hidden." */
                JSValue parent = ps_get(ctx, s->doc, PS_DOC_HINT_PARENT);

                run = !JS_IsUndefined(parent) && JS_VALUE_GET_PTR(parent) == JS_VALUE_GET_PTR(element);
                JS_FreeValue(ctx, parent);
                endpoint = JS_NULL;
            } else {
                /* Step 11.3 — "If autoPopoverListContainsElement is true, then run hide popover stack until
                   given document, element, Auto, focusPreviousElement, and fireEvents." */
                run = s->auto_contains != 0;
                endpoint = element;
                type = POPOVER_STATE_AUTO;
            }
            if (!run) {
                s->stack_call++;
                continue;
            }
            rc = popover_stack_until_run(ctx, &s->su, s->doc, endpoint, type, focus_previous_element,
                                         s->fire_events != 0, cb_result, out_cb, out_argc);
            if (rc > 0) return rc;
            if (rc < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            popover_stack_until_init(&s->su);   /* the next of the three is that algorithm from its own step 1 */
            s->stack_call++;
        }
        JS_FreeValue(ctx, cb_result);
        /* Step 11.4 — "check popover validity is called again because running hide popover stack until could
           have disconnected element or changed its popover attribute". */
        validity = popover_check_validity(ctx, element, /*expectedToBeShowing*/ true, JS_NULL);
        if (validity != 1) {                                                                  /* step 11.5 */
            popover_hide_cleanup(ctx, s);
            return popover_hide_return(ctx, validity, throw_exceptions);
        }
        {
            int next = popover_hide_stage_after_11(ctx, s, source);

            if (next < 0) return JS_STEP_ABRUPT;
            STEP_GOTO(s->hdr.stage, next, &s->fphase, &s->focus_phase, &s->su.hphase, NULL);
        }
    }

    if (s->hdr.stage == PH_FIRE) {
        r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), element, s->ev, JS_UNDEFINED, cb_result,
                                  NULL, out_cb, out_argc);                                     /* step 12.1 */
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        JS_FreeValue(ctx, s->ev);
        s->ev = JS_UNDEFINED;
        /* Step 12.2 — "called again because firing the beforetoggle event could have disconnected element or
           changed its popover attribute". */
        validity = popover_check_validity(ctx, element, /*expectedToBeShowing*/ true, JS_NULL);
        if (validity != 1) {                                                                   /* step 12.3 */
            popover_hide_cleanup(ctx, s);
            return popover_hide_return(ctx, validity, throw_exceptions);
        }
        /* Step 12.4's removal is a REQUEST, so the element stays in the top layer until §8.1.7's update the
           rendering step 23 processes top layer removals — which is what CSS Positioned Layout Level 4 §3.3's
           own note means by "Most of the time, requesting removal from the top layer is more appropriate."
           Step 12.5 sets the IMPLICIT ANCHOR ELEMENT to null, which is the residual named at show popover step
           22 and carries the same three clauses. */
        top_layer_request_removal(ctx, element);
        STEP_GOTO(s->hdr.stage, PH_END, &s->fphase, &s->focus_phase, &s->su.hphase, NULL);
    }

    if (s->hdr.stage == PH_END) {
        bool restore = false;   /* step 20.2's two conjuncts, as the ONE verdict this stage transitions on */

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* Step 13 — the "otherwise" of step 12's `fireEvents` condition, and the arm a NESTED hide takes. */
        if (!s->fire_events) top_layer_remove_immediately(ctx, element);
        ps_clear(ctx, element, PS_TRIGGER);                                                      /* step 14 */
        ps_clear(ctx, element, PS_OPENED_MODE);                                                  /* step 15 */
        ps_clear(ctx, element, PS_VISIBILITY);                                                   /* step 16 */
        /* Step 17 — "If element is document's hint stack parent, or document's showing hint popover list is
           empty, then set document's hint stack parent to null." BOTH disjuncts are asked, and the second is
           asked over the list as it stands NOW: step 15 above has just made this element not-showing, so an
           element that WAS the last showing hint popover empties the list here. */
        {
            JSValue parent = ps_get(ctx, s->doc, PS_DOC_HINT_PARENT);
            bool is_parent =
                !JS_IsUndefined(parent) && JS_VALUE_GET_PTR(parent) == JS_VALUE_GET_PTR(element);
            JSValue hints;

            JS_FreeValue(ctx, parent);
            hints = popover_showing_list(ctx, s->doc, POPOVER_STATE_HINT);
            if (is_parent || popover_list_len(ctx, hints) == 0)
                ps_clear(ctx, s->doc, PS_DOC_HINT_PARENT);
            JS_FreeValue(ctx, hints);
        }
        if (s->fire_events)                                                                      /* step 18 */
            popover_queue_toggle_task(ctx, element, POPOVER_OPEN, POPOVER_CLOSED, source);
        s->prev_focused = ps_get(ctx, element, PS_PREV_FOCUSED);                                 /* step 19 */
        /* Step 20, WITH ITS TWO HALVES APART: step 20.1's clear is unconditional inside the block, and only
           step 20.2's focusing steps are gated on `focusPreviousElement`. That distinction is invisible from a
           member entry, which passes true, and it is exactly what hide popover stack until's callers vary.
           THE VERDICT IS A LOCAL AND THE TRANSITION IS THE STAGE'S LAST STATEMENT, which is not style: a
           STEP_GOTO buried inside a conditional assigns the stage and then FALLS THROUGH to whatever the stage
           does next, so the machine would run step 21 and answer DONE with its own next stage never entered —
           the focus silently not restored, every value well-formed. */
        if (!JS_IsUndefined(s->prev_focused)) {                                                  /* step 20 */
            JSContext *docctx = popover_document_realm(element);
            JSValue anchor;
            bool inside;

            ps_clear(ctx, element, PS_PREV_FOCUSED);                                          /* step 20.1 */
            DCHECK(docctx != NULL,
                   "HTML §6.12 The popover attribute's hide a popover step 20.2 reads the document's FOCUSED "
                   "AREA OF THE DOCUMENT, and this element's node document is no realm's active document — "
                   "step 1's check popover validity refuses a document that is not fully active and step 12.2 "
                   "re-runs it after the `beforetoggle` listeners, so reaching here means a third way exists "
                   "for a document to stop being active mid-hide and the realm must be bound at step 3 beside "
                   "`document`");
            /* Step 20.2's second conjunct — "document's focused area of the document's DOM anchor is a
               shadow-including inclusive descendant of element". §6.6.2's anchor is a NODE and is this
               engine's Document whenever the focused area is the viewport (core/html/focus.h says so at the
               door), which is not a descendant of any element — so a popover the page never focused into does
               not restore focus, which is the browser-visible answer. */
            anchor = focus_focused_area_dom_anchor(docctx);
            inside = shadow_root_is_shadow_including_inclusive_ancestor(node_of(element), node_of(anchor));
            JS_FreeValue(ctx, anchor);
            restore = focus_previous_element && inside;
            if (restore) {
                if (!html_element_is(s->prev_focused))
                    DFAIL("HTML §6.12 The popover attribute's hide a popover step 20.2 runs HTML §6.6.4 "
                          "Processing model's FOCUSING STEPS for the element's PREVIOUSLY FOCUSED ELEMENT, and "
                          "this one is not an element. Show popover step 17 binds that value to \"document's "
                          "focused area of the document's DOM anchor\", and §6.6.2 Data model makes the "
                          "viewport's anchor the DOCUMENT — while §6.6.4 declares the focusing steps over \"an "
                          "object new focus target that is either a focusable area, or an element that is not "
                          "a focusable area, or a navigable\", which a Document is none of. BUILD §6.6.4 step "
                          "1's \"if new focus target is not a focusable area, then set new focus target to the "
                          "result of getting the focusable area for new focus target\" as a door on "
                          "core/html/focus.c beside focus_element_run, and route this call through it");
            }
        }
        if (!restore) {
            JS_FreeValue(ctx, s->prev_focused);
            s->prev_focused = JS_UNDEFINED;
            popover_hide_cleanup(ctx, s);                                                        /* step 21 */
            return JS_STEP_DONE;
        }
        STEP_GOTO(s->hdr.stage, PH_FOCUS, &s->fphase, &s->focus_phase, &s->su.hphase, NULL);
    }

    DCHECK(s->hdr.stage == PH_FOCUS, "§6.12's hide a popover resumed at a stage this algorithm does not have");
    {
        JSContext *docctx = popover_document_realm(element);

        DCHECK(docctx != NULL, "§6.12's hide a popover resumed into step 20.2 for an element whose node "
                               "document stopped being any realm's active document across the request");
        r = focus_element_run(docctx, s->prev_focused, &s->focus_phase, STEP_CB(s->cb), cb_result,
                              out_cb, out_argc);                                              /* step 20.2 */
        if (r) return r;
    }
    JS_FreeValue(ctx, s->prev_focused);
    s->prev_focused = JS_UNDEFINED;
    popover_hide_cleanup(ctx, s);                                                                /* step 21 */
    return JS_STEP_DONE;
}

static void js_popover_hide_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    PopoverHideState *s = st;
    int k;

    v->val(ctx, &s->doc);
    v->val(ctx, &s->ev);
    v->val(ctx, &s->prev_focused);
    popover_stack_until_visit(ctx, &s->su, v);
    STEP_CB_FOREACH(s->cb, k)
        v->val(ctx, &s->cb[k]);
}

/* WHAT THIS ALGORITHM TOOK AND MUST GIVE BACK ON EVERY EXIT — step 7's increment and step 5's hiding boolean,
   which step 8's cleanupSteps exist for. A flow abandoned while parked inside a `beforetoggle` handler never
   reaches step 21, so this is the only place they can be lowered on that path, and leaving the nesting count
   raised would make the document refuse every later `showPopover()` for the rest of the session (show popover
   step 2 throws on a non-zero count). Same obligation and same shape as core/html/html_form.c's submit fini.
   Hide a popover returns NOTHING, so the completion is undefined on every path. */
static JSValue js_popover_hide_fini(JSContext *ctx, void *st, bool take_result)
{
    PopoverHideState *s = st;

    (void)take_result;
    popover_hide_cleanup(ctx, s);
    return JS_UNDEFINED;
}

static const JSTrampStepDef js_popover_hide_def = {
    sizeof(PopoverHideState), js_popover_hide_step, js_popover_hide_fini, 0,
    .visit = js_popover_hide_visit,
    .algorithm = "HTML §6.12 The popover attribute's hide a popover",
    .steps = POPOVER_HIDE_STEPS
};

JSValue popover_hide_algorithm(JSContext *ctx)
{
    DCHECK(g_hide_fn_slot >= 0,
           "§6.12's hide a popover was asked for before popover_declare declared its per-realm slot");
    return realm_value_get(ctx, g_hide_fn_slot);   /* OWNED — realm_value_get asserts the realm ran its install */
}

/* ---- the three members, as ONE machine ---------------------------------------------------------------------- */

enum { M_SHOW = 0, M_HIDE, M_TOGGLE };

/* WHICH ALGORITHM THIS INVOCATION RESOLVED TO. `togglePopover` steps 5-7 pick one of three, and `showPopover`
   and `hidePopover` each pick one unconditionally, so the pick is state rather than a re-read of the magic. */
enum { PA_NONE = 0, PA_SHOW, PA_HIDE };

#define POPOVER_STAGES(X) \
    X(PO_ENTER,      "HTML §6.12 The popover attribute: the invoked member's own steps (showPopover step 1, " \
                     "togglePopover steps 1-4 and their pick of an algorithm) and then, for show popover, its " \
                     "steps 1-8 — a range because every step in it is ONE O(1) engine action: a slot read, a " \
                     "slot write, or a check popover validity whose four steps are an attribute-state lookup " \
                     "and a connectedness walk") \
    X(PO_SHOW_FIRE,  "HTML §6.12 The popover attribute's show popover step 9 (fire an event named " \
                     "beforetoggle, using ToggleEvent, cancelable, with oldState \"closed\", newState " \
                     "\"open\" and source, at element)") \
    X(PO_SHOW_FOCUS, "HTML §6.12 The popover attribute's show popover step 23 (run the popover focusing " \
                     "steps given element)") \
    X(PO_SHOW_STACK, "HTML §6.12 The popover attribute's show popover steps 15.3 and 15.4 (run HIDE POPOVER " \
                     "STACK UNTIL for the Hint stack and, when effectiveType is Auto, for the Auto stack) and " \
                     "then steps 15.5 through 15.10") \
    X(PO_HIDE_CALL,  "HTML §6.12 The popover attribute's hidePopover() step 1 and togglePopover() step 5 (run " \
                     "the hide popover algorithm given this, true, true, true, and null)")
enum { IDL_STEP_STAGE_BASE(POPOVER_STAGES) POPOVER_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const POPOVER_STEPS[] = { POPOVER_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t which;            /* PA_* — which of §6.12's two algorithms this invocation entered */
    uint8_t fphase;           /* the beforetoggle fire request's own phase, and the hide CALL's */
    uint8_t ua_phase;         /* §6.6.6's allow focus steps' fork phase */
    uint8_t focus_phase;      /* §6.6.4's focusing steps request's own phase */
    uint8_t showing_taken;    /* whether show popover step 7's latch is still owed cleanupShowingSteps */
    uint8_t original_type;    /* show popover step 13's originalType */
    uint8_t effective_type;   /* step 14's effectiveType, which step 15.2 may downgrade to Hint */
    uint8_t should_restore;   /* step 12's shouldRestoreFocus, which step 15.8 may raise */
    /* WHICH OF STEP 15'S TWO HIDE POPOVER STACK UNTIL RUNS IS NEXT — 0 = step 15.3, 1 = step 15.4, 2 = done.
       An ordinal over the standard's own two sub-steps, which nothing the page does can renumber. */
    uint8_t stack_call;
    JSValue el;               /* the receiver, held across the suspension (owned) */
    JSValue doc;              /* "element's node document", resolved once at entry (owned) */
    JSValue source;           /* the `source` the member read, or null (owned) */
    JSValue ev;               /* the `beforetoggle` event, minted once (owned) */
    JSValue control;          /* the popover focusing steps' `control` (owned) */
    JSValue ancestor;         /* step 15.1's ancestor, bound there and read again at step 19 (owned) */
    JSValue originally_focused;  /* step 17's originallyFocusedElement, read again at step 24 (owned) */
    PopoverStackUntilRun su;  /* step 15's HIDE POPOVER STACK UNTIL cursor, one run at a time */
    /* THE WIDEST OF THE THREE REQUESTS THIS BODY MAKES — show popover step 9's fire (EVENT_FIRE_CB_SLOTS = 5),
       the popover focusing steps' step 6 (FOCUS_ELEMENT_CB_SLOTS = 3), and the hide a popover CALL at
       PO_HIDE_CALL (POPOVER_HIDE_CB_SLOTS = 7), which is the widest. Named by that TYPE rather than by a
       number, so a request that grows carries its capacity here instead of leaving one restated. */
    PopoverHideCb cb;
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
    v->val(ctx, &s->ancestor);
    v->val(ctx, &s->originally_focused);
    popover_stack_until_visit(ctx, &s->su, v);
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

/* SHOW POPOVER STEPS 16 THROUGH 22 — the tail step 15's two arms converge on, written once because these seven
   steps are the SAME steps whether or not step 15's Auto/Hint block ran, and a copy at each arm is seven steps
   two sites must stay abreast of. Every one of them is an O(1) engine action and none runs the page's code, so
   it is a plain function rather than a stage. */
static void popover_show_tail(JSContext *ctx, PopoverState *s)
{
    JSContext *docctx = popover_document_realm(s->el);

    ps_clear(ctx, s->el, PS_PREV_FOCUSED);                                                        /* step 16 */
    /* Step 17 — "Let originallyFocusedElement be document's focused area of the document's DOM anchor." READ
       HERE AND NOT AT STEP 24, because that is where the standard reads it and because step 23's popover
       focusing steps move the focus: a read deferred to step 24 would answer with whatever this show just
       focused, which is the opposite of what step 24's restore is for. §6.6.2 Data model's anchor is a NODE,
       and it is the DOCUMENT whenever the focused area is the viewport — see core/html/focus.h. */
    DCHECK(docctx != NULL,
           "HTML §6.12 The popover attribute's show popover step 17 reads the document's FOCUSED AREA OF THE "
           "DOCUMENT and this element's node document is no realm's active document — step 3's check popover "
           "validity refuses a document that is not fully active and step 10 re-runs it after the "
           "`beforetoggle` listeners, so reaching here means a third way exists for a document to stop being "
           "active mid-show and the realm must be bound at step 1 beside `document`");
    s->originally_focused = focus_focused_area_dom_anchor(docctx);                                /* step 17 */
    top_layer_add(ctx, s->el);                                                                    /* step 18 */
    /* Step 19 — "If effectiveType is Hint and ancestor's opened in popover mode is 'auto', then set document's
       hint stack parent to ancestor." Reachable only through step 15's block, which is the only thing that
       binds `ancestor` and the only way effectiveType becomes Hint; a Manual show leaves both alone. §6.12's
       own note on the field says why the second conjunct is there: "when the hint stack parent is not null, it
       will have an opened in popover mode of 'auto'". */
    if (s->effective_type == POPOVER_STATE_HINT && !JS_IsNull(s->ancestor) &&
        !JS_IsUndefined(s->ancestor) && popover_opened_mode(ctx, s->ancestor) == POPOVER_STATE_AUTO)
        ps_set(ctx, s->doc, PS_DOC_HINT_PARENT, JS_DupValue(ctx, s->ancestor));
    ps_set(ctx, s->el, PS_VISIBILITY, JS_TRUE);                                                   /* step 20 */
    if (!JS_IsNull(s->source)) ps_set(ctx, s->el, PS_TRIGGER, JS_DupValue(ctx, s->source));
    else                       ps_clear(ctx, s->el, PS_TRIGGER);                                  /* step 21 */
    /* Step 22 sets the element's IMPLICIT ANCHOR ELEMENT to source. NOT COVERED: that is CSS Anchor
       Positioning's per-element state, and this engine has no consumer for it — `grep -rn "implicit anchor"
       engine/host` is empty, there is no `anchor-name`, no `position-anchor` and no `anchor()` in the cascade.
       THE NEXT DIFF that builds any of those builds this write with them, in the component that owns the anchor
       state. ITS ABSENCE WOULD SHOW as a popover positioned with `position-anchor: implicit` resolving against
       nothing: `getComputedStyle(p).top` answers as if the popover had no invoker, where a browser answers
       relative to the button that showed it. It is named here and not crashed on because the write is CORRECT
       to omit while nothing can read it — a crash would fire on every `showPopover()` this file gets right. */
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
        s->ancestor = s->originally_focused = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        popover_stack_until_init(&s->su);
        s->which = PA_NONE;
        s->fphase = s->ua_phase = s->focus_phase = 0;
        s->showing_taken = 0;
        s->original_type = s->effective_type = POPOVER_STATE_NONE;
        s->should_restore = 0;
        s->stack_call = 0;
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
            STEP_GOTO(hdr->stage, PO_SHOW_FIRE, &s->fphase, &s->ua_phase, &s->focus_phase,
                      &s->su.hphase, NULL);
        } else {
            /* HIDE A POPOVER. `hidePopover()`'s one step and togglePopover step 5 state the SAME five
               arguments, word for word — "Run the hide popover algorithm given this, true, true, true, and
               null" — so the member has nothing of its own to compute and the whole of the algorithm is the
               CALL at PO_HIDE_CALL. THE FIFTH ARGUMENT IS NULL AND NOT `s->source`: togglePopover step 4 reads
               `options["source"]` for step 6's SHOW, and its step 5 passes null to the hide, so the `source`
               attribute of the `beforetoggle` and `toggle` events a hide fires is null however the member was
               invoked. */
            STEP_GOTO(hdr->stage, PO_HIDE_CALL, &s->fphase, &s->ua_phase, &s->focus_phase,
                      &s->su.hphase, NULL);
        }
    }

    if (hdr->stage == PO_HIDE_CALL) {
        JSValueConst hide_argv[POPOVER_HIDE_ARGC];
        JSValue hide = popover_hide_algorithm(ctx);
        JSValue ignored = JS_UNDEFINED;

        hide_argv[0] = s->el;
        hide_argv[1] = JS_TRUE;    /* focusPreviousElement */
        hide_argv[2] = JS_TRUE;    /* fireEvents */
        hide_argv[3] = JS_TRUE;    /* throwExceptions — so this member THROWS what the algorithm refuses on */
        hide_argv[4] = JS_NULL;    /* source */
        r = step_call_run(ctx, &s->fphase, STEP_CB(s->cb), hide, JS_UNDEFINED,
                          POPOVER_HIDE_ARGC, hide_argv, cb_result, &ignored, out_cb, out_argc);
        JS_FreeValue(ctx, hide);
        if (r) return r;
        /* Hide a popover returns nothing; a THROW does not arrive here at all — an abrupt request result
           abandons this machine and re-raises out of the member, which is what `hidePopover()` on a
           disconnected element owes its caller. */
        DCHECK(JS_IsUndefined(ignored),
               "§6.12's hide a popover answered with a value — it returns nothing, and this call site is the "
               "only thing that reads its completion");
        JS_FreeValue(ctx, ignored);
        *presult = magic == M_TOGGLE ? JS_NewBool(ctx, popover_is_showing(ctx, s->el)) : JS_UNDEFINED;
        return JS_STEP_DONE;                                                      /* togglePopover step 8 */
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
        /* Steps 12, 13 and 14: shouldRestoreFocus is false, and originalType and effectiveType are both the
           current state of the element's `popover` attribute. All three are held on the state, because step
           15's stack-until runs suspend between here and every one of their readers. */
        s->should_restore = 0;                                                                    /* step 12 */
        s->original_type =
            (uint8_t)popover_attribute_state(lxb_dom_interface_element(node_of(s->el)));          /* step 13 */
        s->effective_type = s->original_type;                                                     /* step 14 */
        /* Step 15 — the Auto/Hint block. It holds exactly ONE list, of ten items, so every `step 15.N` below
           names an item of that one list; its step 15.9 is the one item that holds TWO sibling lists, so that
           step's sub-items are named by their list rather than by a bare number. A Manual popover enters none
           of this, which is the SPEC's own split. */
        if (s->original_type == POPOVER_STATE_AUTO || s->original_type == POPOVER_STATE_HINT) {
            s->ancestor = popover_topmost_ancestor(ctx, s->el, s->source, true);               /* step 15.1 */
            /* Step 15.2's three conjuncts — "ancestor is not null", "ancestor's opened in popover mode is
               'hint'", and "effectiveType is the Auto state" — whose note says why: "Hint popovers are lower
               priority than Auto popovers, so an Auto popover cannot have a Hint popover as a 'parent'. To
               resolve this case, the effectiveType is 'downgraded' to Hint." */
            if (!JS_IsNull(s->ancestor) && popover_opened_mode(ctx, s->ancestor) == POPOVER_STATE_HINT &&
                s->effective_type == POPOVER_STATE_AUTO)
                s->effective_type = POPOVER_STATE_HINT;
            s->stack_call = 0;
            STEP_GOTO(hdr->stage, PO_SHOW_STACK, &s->fphase, &s->ua_phase, &s->focus_phase,
                      &s->su.hphase, NULL);
        } else {
            DCHECK(s->original_type == POPOVER_STATE_MANUAL,
                   "HTML §6.12 The popover attribute's show popover reached step 16 with a `popover` attribute "
                   "that is in neither the Auto, the Hint nor the Manual state — step 3's check popover "
                   "validity has already refused the No Popover state, and §6.12's table names no fourth");
            popover_show_tail(ctx, s);                                                       /* steps 16-22 */
            STEP_GOTO(hdr->stage, PO_SHOW_FOCUS, &s->fphase, &s->ua_phase, &s->focus_phase,
                      &s->su.hphase, NULL);
        }
    }

    if (hdr->stage == PO_SHOW_STACK) {
        /* Steps 15.3 and 15.4 — "Run hide popover stack until given document, ancestor, Hint,
           shouldRestoreFocus, and true", then "If effectiveType is the Auto state, then run hide popover stack
           until given document, ancestor, Auto, shouldRestoreFocus, and true." Each is that algorithm from its
           own step 1 and each suspends at every popover it hides, so the sub-index says which of the two is
           next; both conditions are re-read on every re-entry and neither can move, because nothing between
           here and step 15.8 writes effectiveType or shouldRestoreFocus.
           THE FOURTH ARGUMENT IS shouldRestoreFocus AND NOT `true`, which is exactly what a member entry could
           never express: at this point step 12's false is still standing (step 15.8 is what may raise it), so
           these two runs hide the stack WITHOUT restoring focus and the popover being shown is the one that
           will own the restore. */
        while (s->stack_call < 2) {
            int type = s->stack_call == 0 ? POPOVER_STATE_HINT : POPOVER_STATE_AUTO;
            int rc;

            if (s->stack_call == 1 && s->effective_type != POPOVER_STATE_AUTO) {          /* step 15.4's if */
                s->stack_call++;
                continue;
            }
            rc = popover_stack_until_run(ctx, &s->su, s->doc, s->ancestor, type,
                                         /*focusPreviousElement*/ s->should_restore != 0,
                                         /*fireEvents*/ true, cb_result, out_cb, out_argc);
            if (rc > 0) return rc;
            if (rc < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            popover_stack_until_init(&s->su);
            s->stack_call++;
        }
        JS_FreeValue(ctx, cb_result);
        /* Step 15.5 — "If originalType is not equal to the value of element's popover attribute", which the
           hides above may have changed through a `beforetoggle` listener. */
        if (popover_attribute_state(lxb_dom_interface_element(node_of(s->el))) != s->original_type) {
            popover_cleanup_showing(ctx, s);                                               /* step 15.5.1 */
            /* Step 15.5.2 — "if throwExceptions is true, then throw an InvalidStateError DOMException". Every
               caller of show popover in this build passes true. */
            JS_ThrowDOMException(ctx, "InvalidStateError",
                                 "a beforetoggle handler changed the element's popover attribute while "
                                 "showPopover() was hiding the popover stack");
            return JS_STEP_ABRUPT;
        }
        /* Step 15.6 — "check popover validity is called again because running hide popover stack until above
           could have fired the beforetoggle event, and an event handler could have disconnected this element
           or changed its popover attribute". */
        validity = popover_check_validity(ctx, s->el, /*expectedToBeShowing*/ false, s->doc);
        if (validity != 1) {                                                                 /* step 15.7 */
            popover_cleanup_showing(ctx, s);                                               /* step 15.7.1 */
            if (validity < 0) return JS_STEP_ABRUPT;                                       /* step 15.7.2 */
            *presult = JS_UNDEFINED;
            return JS_STEP_DONE;                                                           /* step 15.7.3 */
        }
        /* Step 15.8 — "If the result of running topmost auto or hint popover on document is null, then set
           shouldRestoreFocus to true", whose note says why: "This ensures that focus is returned to the
           previously-focused element only for the first popover in a stack." */
        {
            JSValue topmost = popover_topmost_auto_or_hint(ctx, s->doc);

            if (JS_IsNull(topmost)) s->should_restore = 1;
            JS_FreeValue(ctx, topmost);
        }
        /* Step 15.9's two lists — the Auto one and its otherwise-list. They are named by their list and not by
           a bare sub-number, because this is the one item of step 15 that holds TWO sibling `<ol>`s and a bare
           `15.9.2` would name a different step in each. THIS WRITE IS WHAT MAKES THE SHOWING AUTO AND SHOWING
           HINT POPOVER LISTS NON-EMPTY: it is their only writer, so every read of them above and every one of
           hide a popover step 11's three conditions is decided by it. */
        if (s->effective_type == POPOVER_STATE_AUTO) {
            JSValue autos = popover_showing_list(ctx, s->doc, POPOVER_STATE_AUTO);

            DCHECK(popover_list_index_of(ctx, autos, popover_list_len(ctx, autos), s->el) < 0,
                   "HTML §6.12 The popover attribute's show popover step 15.9's Auto list asserts that the "
                   "document's SHOWING AUTO POPOVER LIST does not contain the element — that list filters on "
                   "an OPENED IN POPOVER MODE this very step is the only writer of, so an element already in "
                   "it means a show completed without its matching hide clearing the mode at hide a popover "
                   "step 15");
            JS_FreeValue(ctx, autos);
            ps_set(ctx, s->el, PS_OPENED_MODE, JS_NewInt32(ctx, POPOVER_STATE_AUTO));
        } else {
            JSValue hints;

            DCHECK(s->effective_type == POPOVER_STATE_HINT,
                   "HTML §6.12 The popover attribute's show popover step 15.9's otherwise-list asserts that "
                   "effectiveType is Hint — step 14 binds it to originalType, which step 15's own condition "
                   "has already narrowed to Auto or Hint, and step 15.2 only ever moves it toward Hint");
            hints = popover_showing_list(ctx, s->doc, POPOVER_STATE_HINT);
            DCHECK(popover_list_index_of(ctx, hints, popover_list_len(ctx, hints), s->el) < 0,
                   "HTML §6.12 The popover attribute's show popover step 15.9's otherwise-list asserts that "
                   "the document's SHOWING HINT POPOVER LIST does not contain the element — see the Auto "
                   "list's twin above for why that can only mean a mode outlived its hide");
            JS_FreeValue(ctx, hints);
            ps_set(ctx, s->el, PS_OPENED_MODE, JS_NewInt32(ctx, POPOVER_STATE_HINT));
        }
        /* Step 15.10 — "Set element's popover close watcher to the result of establishing a close watcher
           given element's relevant global object, with: cancelAction being to return true. closeAction being
           to hide a popover given element, true, true, false, and null. getEnabledState being to return true."
           The three actions are named by core/html/close_watcher.h's POPOVER kind, which is where that file
           states them; this supplies the WINDOW and the SUBJECT, which are the only two things §6.10.2's
           establish takes that vary. THIS IS THE ONLY ESTABLISHER OF THAT KIND, so it is what makes §6.10.2's
           POPOVER close-action arm reachable at all. */
        {
            JSContext *wctx = popover_document_realm(s->el);

            DCHECK(wctx != NULL,
                   "HTML §6.12 The popover attribute's show popover step 15.10 establishes a close watcher "
                   "given \"element's relevant global object\", and this element's node document is no realm's "
                   "active document — step 15.6's check popover validity has just certified it fully active");
            ps_set(ctx, s->el, PS_CLOSE_WATCHER,
                   close_watcher_establish(wctx, CLOSE_WATCHER_KIND_POPOVER, s->el));
        }
        popover_show_tail(ctx, s);                                                           /* steps 16-22 */
        STEP_GOTO(hdr->stage, PO_SHOW_FOCUS, &s->fphase, &s->ua_phase, &s->focus_phase, &s->su.hphase, NULL);
    }

    DCHECK(hdr->stage == PO_SHOW_FOCUS, "a §6.12 member resumed into a stage this algorithm does not have — "
                                        "its four are the entry, the hide a popover call, and show popover's "
                                        "two requests");
    r = popover_focusing_steps(ctx, s, hdr, cb_result, out_cb, out_argc);                         /* step 23 */
    if (r) return r;
    /* Step 24 — "If shouldRestoreFocus is true and element's popover attribute is not in the No Popover state,
       then set element's previously focused element to originallyFocusedElement." The SECOND conjunct is a
       fresh read of the attribute and not step 13's originalType: step 23's focusing steps fire `blur`,
       `focusout`, `focus` and `focusin` at the page, and a handler may have removed the attribute since. */
    if (s->should_restore &&
        popover_attribute_state(lxb_dom_interface_element(node_of(s->el))) != POPOVER_STATE_NONE) {
        DCHECK(!JS_IsUndefined(s->originally_focused),
               "HTML §6.12 The popover attribute's show popover step 24 stores originallyFocusedElement, and "
               "step 17 never bound one — shouldRestoreFocus is raised only at step 15.8, which every path "
               "reaches before step 17's read, so an unbound value here means the two steps came apart");
        ps_set(ctx, s->el, PS_PREV_FOCUSED, JS_DupValue(ctx, s->originally_focused));
    }
    popover_cleanup_showing(ctx, s);                                                              /* step 25 */
    popover_queue_toggle_task(ctx, s->el, POPOVER_CLOSED, POPOVER_OPEN, s->source);               /* step 26 */
    *presult = magic == M_TOGGLE ? JS_NewBool(ctx, popover_is_showing(ctx, s->el)) : JS_UNDEFINED;
    return JS_STEP_DONE;
}

/* WHAT THIS MEMBER TOOK AND MUST GIVE BACK ON EVERY EXIT — the latch show popover step 8's cleanupShowingSteps
   exists for. A flow that is discarded while parked on a `beforetoggle` handler never comes back, so this is the
   only place it can be lowered on that path, and leaving it raised would make the document refuse every later
   show for the rest of the session (show popover step 2 throws while showing popover is true).
   IT IS ONE LATCH AND NOT TWO NOW. Hide a popover step 8's cleanupSteps used to be lowered here as well, because
   the member's own state carried that algorithm's nesting count; the algorithm is its own machine, so its
   obligation is discharged by ITS `fini` — js_popover_hide_fini — which is where an abandoned hide can be
   reached whether the member called it or §6.10.2's close action did. */
static void popover_release(JSContext *ctx, void *state)
{
    PopoverState *s = state;

    if (JS_IsUndefined(s->doc) || JS_IsNull(s->doc)) return;
    popover_cleanup_showing(ctx, s);
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
    /* §6.12's HIDE A POPOVER. The step-def id is the RUNTIME's and the slot id is the AGENT's, so both are
       taken here, EAGERLY — a def registered on first use would be registered inside whichever flow happened to
       call the algorithm first, and a slot declared lazily is a per-realm fact minted from one realm. What each
       REALM then holds is the function object, which popover_install mints. */
    g_hide_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_popover_hide_def);
    g_hide_fn_slot = realm_value_declare(ctx, "HTML §6.12 hide a popover (spec prose)");
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
    /* §6.12's HIDE A POPOVER, AS A FUNCTION OBJECT OF THIS REALM AND ON NO PROTOTYPE. It is not installed
       anywhere a page can reach: §6.12's own prose and §6.10.2's close action reach it through
       popover_hide_algorithm, and a page reassigning `hidePopover` above must not redirect either of them —
       core/dom/observable.c mints §2.2.1's subscribe-to for that same reason and states it there. Minted PER
       REALM because a C function runs in the realm that DEFINED it, and this one fires `beforetoggle` at an
       element of this realm's document. */
    {
        JSValue fn = JS_NewCFunction2(ctx, NULL, "popoverHide", POPOVER_HIDE_ARGC, JS_CFUNC_step, g_hide_stepid);

        CHECK(JS_IsFunction(ctx, fn), "§6.12's hide a popover could not be minted for this realm");
        realm_value_set(ctx, g_hide_fn_slot, fn);
    }
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
    /* The hide a popover FUNCTION OBJECTS are the realms' — each is released with its context, which is what
       core/realm.h's per-realm store is for. What the agent holds is a step-def id and a slot id, and both are
       ids in a runtime that is going away with them. */
    g_hide_stepid = g_hide_fn_slot = -1;
}
