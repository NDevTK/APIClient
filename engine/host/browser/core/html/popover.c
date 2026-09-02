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
 * WHAT IS HONESTLY ABSENT, AND WHERE IT CRASHES. The AUTO and HINT states are split down the middle, and the
 * line is whether the step runs the PAGE'S code. The READ half is here: the showing auto and hint popover
 * lists, which are DERIVED each time from CSS Positioned Layout Level 4 §3's top layer rather than stored, and
 * TOPMOST POPOVER ANCESTOR over them, which is a flat-tree question with no page code in it. The WRITE half is
 * ONE algorithm — §6.12's HIDE POPOVER STACK UNTIL — and it is missing because its steps 5 and 8 run the hide a
 * popover algorithm over other elements, firing `beforetoggle` at the page's own listeners, which a C question
 * cannot host. Show popover step 15.3 is where that crashes, and it names the re-entrant hide a popover it
 * needs first. The split is the SPEC'S OWN and not a convenience, because a Manual popover enters none of it —
 * step 15's block, hide a popover step 11's block, and every read of the showing lists are reachable only from
 * it. `<div popover=manual>` is therefore complete here and `<div popover>` (whose empty value default is Auto)
 * is a named crash rather than a wrong answer.
 *   IT WAS THREE, AND THE THIRD WAS §6.10 Close requests and close watchers' close watcher, which step 15's
 * block establishes as its last sub-step. core/html/close_watcher.c holds ALL of §6.10.2 now — the manager,
 * establish and destroy, and the three algorithms that RUN a watcher's actions — so that one is a call this
 * file makes rather than a mechanism it waits on. What is owed in the OTHER direction is this file's: §6.10.2's
 * close-action dispatch has to run a POPOVER watcher's close action, which step 15 states is "to hide a popover
 * given element, true, true, false, and null", and hide a popover is exported here only as the `hidePopover()`
 * and `togglePopover()` members — so that arm DFAILs naming the export to make. It is unreachable rather than
 * wrong today, because step 15's block is the only establisher of that kind and it DFAILs first. */
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
 * WHAT THIS SECTION IS AND IS NOT. It is the READ side — the two lists and TOPMOST POPOVER ANCESTOR, all of them
 * questions with no page code in them. The WRITE side is HIDE POPOVER STACK UNTIL, whose step 5 runs the hide a
 * popover algorithm over each popover it hides, which fires `beforetoggle` at the page's own listeners: that one
 * cannot be a C question and is named at the two sites that need it. */

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
               is the block those two booleans gate. Both lists exist now (they are derived from the top layer
               above), but NOTHING in this build writes an element's opened in popover mode — show popover step
               15.9 is its only writer and step 15.3 DFAILs before it. So both lists are empty by construction,
               step 11's condition is false, and step 13's "otherwise" is the arm every hide in this build
               takes. The condition is EVALUATED at the step that asks it, and it is the standard's own —
               "If element's opened in popover mode is 'auto' or 'hint'": */
            if (!ps_is_null(ctx, s->el, PS_OPENED_MODE))
                DFAIL("HTML §6.12 The popover attribute's hide a popover step 11 runs HIDE POPOVER STACK UNTIL "
                      "over the document's showing auto and hint popover lists, up to three times. The lists "
                      "and TOPMOST POPOVER ANCESTOR are built; what is missing is HIDE POPOVER STACK UNTIL "
                      "itself — see show popover step 15.3's crash for why it cannot be a C question and for "
                      "the re-entrant hide a popover it needs first — and the document's HINT STACK PARENT, "
                      "whose only writer is show popover step 19 inside that same block. Reaching here means "
                      "step 15.9 was built and set this element's OPENED IN POPOVER MODE, so both must now be "
                      "written");
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

            /* Step 15 — the Auto/Hint block. It holds exactly ONE list, of ten items, so every `step 15.N`
               below names an item of that one list and there is no second list under this step for a bare
               sub-number to be ambiguous between. Its READ side runs here: step 15.1's TOPMOST POPOVER
               ANCESTOR over the two showing-popover lists this file derives from the top layer, and step
               15.2's effectiveType downgrade. What it stops at is step 15.3, the first HIDE POPOVER STACK
               UNTIL — the WRITE side, which hides other popovers and so runs the page's own listeners. A
               Manual popover enters none of this, which is the SPEC's own split and the reason this file is
               complete for one state and a named crash for the other two. */
            if (type == POPOVER_STATE_AUTO || type == POPOVER_STATE_HINT) {
                JSValue ancestor = popover_topmost_ancestor(ctx, s->el, s->source, true);      /* step 15.1 */
                int effective_type = type;                                       /* step 14's effectiveType */

                /* Step 15.2's three conjuncts — "ancestor is not null", "ancestor's opened in popover mode is
                   'hint'", and "effectiveType is the Auto state" — whose note says why: "Hint popovers are
                   lower priority than Auto popovers, so an Auto popover cannot have a Hint popover as a
                   'parent'. To resolve this case, the effectiveType is 'downgraded' to Hint." */
                if (!JS_IsNull(ancestor) && popover_opened_mode(ctx, ancestor) == POPOVER_STATE_HINT &&
                    effective_type == POPOVER_STATE_AUTO)
                    effective_type = POPOVER_STATE_HINT;
                JS_FreeValue(ctx, ancestor);
                DFAILF("HTML §6.12 The popover attribute's show popover step 15.3 runs HIDE POPOVER STACK "
                       "UNTIL given document, ancestor, Hint, shouldRestoreFocus and true, and step 15.4 runs "
                       "it again for Auto when effectiveType is Auto — this show's effectiveType is %s. THAT "
                       "IS THE ONE MECHANISM §6.12'S AUTO/HINT STACK IS STILL MISSING, and it is missing "
                       "because of what its steps 5 and 8 do rather than because of its slicing: steps 1-4 and "
                       "6-7 are slices of the two showing-popover lists this file now derives, while 5 and 8 "
                       "RUN THE HIDE A POPOVER ALGORITHM over each popover they hide, which fires "
                       "`beforetoggle` at the page's listeners. So hide a popover must first become RE-ENTRANT "
                       "— invocable over an element that is NOT the receiver, with focusPreviousElement, "
                       "fireEvents, throwExceptions=false and source=null passed in rather than hardcoded, "
                       "which this file's PopoverState cannot express because it carries the member entry "
                       "only — and hide popover stack until is then a `_run` with its own phase, like "
                       "focus_element_run and event_target_fire_run, that suspends at each hide. THAT SAME "
                       "EXPORT IS WHAT §6.10.2's close action already wants: core/html/close_watcher.c's "
                       "dispatch DFAILs for a POPOVER-kind watcher for exactly this reason, so the two are one "
                       "diff. With it, this block finishes: step 15.5's originalType re-read, steps 15.6 and "
                       "15.7's second check popover validity, step 15.8's TOPMOST AUTO OR HINT POPOVER setting "
                       "shouldRestoreFocus (which then makes step 17's originallyFocusedElement live, and step "
                       "24 needs the FOCUSED AREA OF THE DOCUMENT exported from core/html/focus.c, which today "
                       "exports only its two predicates), step 15.9's opened in popover mode write, and step "
                       "15.10's close_watcher_establish. Its other consumers are hide a popover step 11, HIDE "
                       "POPOVERS UNTIL — which Fullscreen §2 Model's fullscreen an element step 2 runs, its "
                       "step 1 being the topmost popover ancestor this file now has — and §6.12.2 Popover "
                       "light dismiss. `<div popover>` reaches here because the EMPTY VALUE DEFAULT that HTML "
                       "§2.3.3 Keywords and enumerated attributes gives this attribute is the Auto state",
                       effective_type == POPOVER_STATE_HINT ? "Hint" : "Auto");
            }
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
           Layout Level 4 §3.3's own note means by "Most of the time, requesting removal from the top layer is
           more appropriate." (the note's own words, whose "from the top layer" this file used to drop — a
           quotation trimmed where the sentence turns is the one shape citegen's quote channel exists for).
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
