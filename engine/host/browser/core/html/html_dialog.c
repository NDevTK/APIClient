/* THE dialog ELEMENT — HTML §4.11.4.
 *
 * WHAT IS HERE, AND THE ORDER IT ARRIVED IN, WHICH IS §4.11.4's OWN DEPENDENCY ORDER AND NOT ITS IDL ORDER.
 * `showModal()` is the member every bundle calls and it is the LAST thing that can be built, because its step
 * 12 is "Assert: subject's close watcher is not null" — it does not establish the watcher, it asserts one is
 * already there. What establishes it is step 11's `open` attribute, through §4.11.4's attribute change steps →
 * the dialog setup steps → set the dialog close watcher → §6.10.2's establish. So the file is built from the
 * bottom of that chain up:
 *
 *   - CLOSE THE DIALOG, all 13 steps, because §4.10.22.3 step 11.6 performs it: a `method=dialog` form
 *     submission makes NO REQUEST, it closes the dialog its form sits in.
 *   - `returnValue`, which its step 9 writes and the page reads back.
 *   - `closedBy` and the COMPUTED CLOSED-BY STATE, because that state is one of the two disjuncts the DIALOG
 *     watcher's getEnabledState is made of.
 *   - THE STATE ITSELF — is modal, the close watcher, the two request-close fields, the enable-close-watcher
 *     boolean, and the previously focused element §4.11.4 declares for EVERY HTML element (which is why that
 *     one is exported: §6.12 The popover attribute writes the same field, and two copies of it answer
 *     differently at close the dialog step 12).
 *   - HTML §3.1.1's per-Document OPEN DIALOGS LIST, which the setup and cleanup steps maintain.
 *   - SET THE DIALOG CLOSE WATCHER, the DIALOG SETUP STEPS, the DIALOG CLEANUP STEPS, and the THREE DOM HOOKS
 *     that run them — the `open` ATTRIBUTE CHANGE STEPS, the DIALOG HTML ELEMENT INSERTION STEPS and the
 *     DIALOG HTML ELEMENT REMOVING STEPS. The whole chain needs no member call to run: `<dialog open>` in
 *     markup and `d.open = true` on a connected `<dialog>` each establish a watcher, and an Esc then closes it.
 *   - `close()` and `requestClose()`, and REQUEST TO CLOSE THE DIALOG under the second of them.
 *   - `show()`, SHOW A MODAL DIALOG, `showModal()` and the DIALOG FOCUSING STEPS, which are the top of that
 *     chain and therefore last. They arrived with three doors exported from core/html/popover.c — §6.12's
 *     HIDE POPOVERS UNTIL, its TOPMOST POPOVER ANCESTOR and its POPOVER SHOWING STATE — and with §6.6.4's
 *     FOCUS DELEGATE exported from core/html/focus.c. Not one of those is copied here: hide popovers until is
 *     stated over §6.12's showing hint popover list, its hint stack parent and its hide popover stack until,
 *     all three of which stay STATIC in that file, so §6.12's list algebra has exactly one reading in this
 *     build. A copy would not merely duplicate it — a popover left showing over a modal dialog is precisely
 *     what those steps prevent, so a drifted copy would be wrong on the screen.
 *   - HTML §6.3.1 Modal dialogs and inert subtrees' BLOCKED BY A MODAL DIALOG, which show a modal dialog step
 *     14 states and step 15 performs. It is DERIVED over css-position-4 §3's top layer and stores nothing, and
 *     core/html/focus.c's inert walk is its consumer — a clause that walk could not have before this file put
 *     a `dialog` in the layer.
 *
 * THE FOUR `realm_awaits` THAT STOOD IN THIS FILE ARE GONE, AND NOT BECAUSE THE MEMBERS THEY NAMED LANDED.
 * Each guarded a step whose only producer was said to be `show()`, `showModal()` or `requestClose()`, and each
 * step is now WRITTEN: is modal has storage and steps 6, 7 and 8 read and clear it, steps 10 and 11 clear the
 * two request-close fields that `requestClose()` writes, and step 12 restores the previously focused element —
 * which core/html/popover.c has been writing all along, so that probe was naming ONE producer of a field with
 * TWO and would have gone on reporting a reachable step as unreachable. That is §realm.h's own limit arriving
 * from the other side: the instrument asks whether a MEMBER is installed, and a step is owed work by whatever
 * WRITES its operand.
 *
 * THE STATE IS PER-FLOW BY CONSTRUCTION. Every one of this element's facts lives in an own slot on its wrapper
 * under a Symbol this file minted and never published, and the per-Document list is an Array on the Document's.
 * A slot written as a property write is captured by the COW delta, so two forked arms that close the same
 * dialog with two different submitters each read back their own `returnValue`, and a parked flow resumes with
 * the one it had. The `open` attribute is the DOM's and time-travels through dom_cow for the same reason. */

#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/css/top_layer.h"
#include "core/dom/attr_list.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/html/close_watcher.h"
#include "core/html/enumerated_attribute.h"
#include "core/html/autofocus.h"
#include "core/html/focus.h"
#include "core/html/html_dialog.h"
#include "core/html/html_element.h"
#include "core/html/popover.h"
#include "core/html/toggle_event.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/dom_cow.h"

/* §4.11.4's two state strings. They are the values the standard writes into `oldState` and `newState`, and they
   are named once because the toggle task carries one of them across a suspension. */
static const char DIALOG_STATE_OPEN[]   = "open";
static const char DIALOG_STATE_CLOSED[] = "closed";

/* §4.11.4's OWN PER-ELEMENT AND PER-DOCUMENT STATE, EVERY FIELD IN ONE TABLE. Each is a slot on the object's
   wrapper under a private Symbol this file mints and never publishes, so a write is a property write the heap
   COW delta captures: two forked arms that open the same dialog read back their own `returnValue`, their own
   is modal and their own close watcher, and a parked flow resumes with the ones it had. The table replaced
   three hand-rolled key/atom pairs — with six more fields arriving at once, three spellings of one mechanism
   is the shape that drifts, and a name table beside the enum is what makes a heap dump say whose slot it is.
   AN ABSENT SLOT IS THE INITIAL VALUE THE STANDARD NAMES, for every row: the empty string for `returnValue`,
   null for the tracker, the watcher and the two request-close fields, false for the two booleans, and the
   empty list for the open dialogs list. So a dialog a page never touches costs nothing. */
enum {
    DS_RETURN_VALUE = 0,     /* "the returnValue IDL attribute … when the element is created … the empty string" */
    DS_TRACKER,              /* "dialog toggle task tracker … initially null" — the slot holds its OLD STATE… */
    DS_TRACKER_TASK,         /* …and its TASK, as the JSTaskHandle JS_EnqueueCallTask answered, in a BigInt so
                                the full 64 bits survive. Two slots and not one because the tracker IS two
                                fields; they are written and cleared together, which the read asserts. */
    DS_IS_MODAL,             /* "Each dialog element has an is modal boolean, initially false" */
    DS_CLOSE_WATCHER,        /* "Each dialog element has a close watcher … initially null" */
    DS_REQ_CLOSE_RETURN,     /* "… a request close return value, which is a string or null, initially null" */
    DS_REQ_CLOSE_SOURCE,     /* "… a request close source element, which is an element or null, initially null" */
    DS_ENABLE_CW_REQ_CLOSE,  /* "… an enable close watcher for request close boolean, initially false" */
    /* "Each HTML element has a previously focused element, which is null or an element, and it is initially
       null." An HTML ELEMENT'S field declared by §4.11.4, so it is this file's and is exported — §6.12 The
       popover attribute writes the same one, and html_dialog.h says at length why there may not be two. */
    DS_PREV_FOCUSED,
    /* HTML §3.1.1 The Document object: "Each Document has an open dialogs list, which is a list of dialog
       elements, initially empty." A DOCUMENT's field, held on the Document wrapper as a JS Array for
       §PLATFORM-DATA-A-FLOW-QUEUES-IS-A-JS-VALUE's reason — the dialog setup and cleanup steps append to it and
       remove from its MIDDLE, and a malloc'd list captured as pointers reverts the pointers on a context
       switch and leaves the nodes reachable from nothing. */
    DS_DOC_OPEN_DIALOGS,
    DS_SLOT_N
};
static const char *const DS_SLOT_NAME[DS_SLOT_N] = {
    "dialogReturnValue", "dialogToggleTaskTracker", "dialogToggleTaskTrackerTask",
    "dialogIsModal", "dialogCloseWatcher", "dialogRequestCloseReturnValue",
    "dialogRequestCloseSourceElement", "dialogEnableCloseWatcherForRequestClose",
    "elementPreviouslyFocusedElement", "documentOpenDialogsList"
};
static JSValue g_slot_key[DS_SLOT_N];
static JSAtom  g_slot_atom[DS_SLOT_N];

static int     g_toggle_stepid = -1;
static int     g_id_return_value = -1;
static int     g_id_closed_by = -1;
static int     g_id_close = -1;
static int     g_id_request_close = -1;
static int     g_id_show = -1;
static int     g_id_show_modal = -1;

/* The slot's value, or JS_UNDEFINED for "the initial value the standard names". OWNED. */
static JSValue ds_get(JSContext *ctx, JSValueConst obj, int slot)
{
    JSValue v;

    DCHECK(slot >= 0 && slot < DS_SLOT_N, "a §4.11.4 state slot was read by an index this file has no name for");
    DCHECK(g_slot_atom[slot] != JS_ATOM_NULL,
           "a §4.11.4 state slot was read before html_dialog_declare minted its key");
    if (!JS_IsObject(obj)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &v, obj, g_slot_atom[slot]) > 0) return v;
    return JS_UNDEFINED;
}

/* CONSUMES `v`. CONFIGURABLE AND WRITABLE because every one of these is written again — a slot defined with no
   flags makes the second write a silent no-op. */
static void ds_set(JSContext *ctx, JSValueConst obj, int slot, JSValue v)
{
    DCHECK(slot >= 0 && slot < DS_SLOT_N, "a §4.11.4 state slot was written by an index this file has no name for");
    JS_DefinePropertyValue(ctx, (JSValue)obj, g_slot_atom[slot], v,
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
}

/* Back to the initial value the standard names — the ABSENCE, so "set X to null" and "set X to false" are the
   same operation on the same slot and neither leaves a value behind for a later read to find. */
static void ds_clear(JSContext *ctx, JSValueConst obj, int slot)
{
    JS_DeleteProperty(ctx, (JSValue)obj, g_slot_atom[slot], 0);
}

static bool ds_flag(JSContext *ctx, JSValueConst obj, int slot)
{
    JSValue v = ds_get(ctx, obj, slot);
    bool set = JS_ToBool(ctx, v) != 0;

    JS_FreeValue(ctx, v);
    return set;
}

/* "… is null" for a slot whose initial value is the standard's null — asked without leaving the read behind,
   because a DCHECK's condition must be side-effect-free and an owned value dropped inside one is a leak the
   runtime's own walk reports and nobody can trace. */
static bool ds_is_null(JSContext *ctx, JSValueConst obj, int slot)
{
    JSValue v = ds_get(ctx, obj, slot);
    bool null = JS_IsUndefined(v);

    JS_FreeValue(ctx, v);
    return null;
}

/* A slot whose initial value is the standard's null, AS the standard's null — JS_NULL rather than the
   JS_UNDEFINED the absence is spelled with, because every caller of these compares against "null" and one that
   was handed `undefined` would have to know this file's encoding. OWNED. */
static JSValue ds_get_or_null(JSContext *ctx, JSValueConst obj, int slot)
{
    JSValue v = ds_get(ctx, obj, slot);

    if (JS_IsUndefined(v)) return JS_NULL;
    return v;
}

bool html_dialog_is_dialog(const lxb_dom_node_t *n)
{
    size_t len = 0;
    const lxb_char_t *t;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT || n->ns != LXB_NS_HTML) return false;
    t = lxb_dom_element_local_name(lxb_dom_interface_element((lxb_dom_node_t *)n), &len);
    return t && len == 6 && memcmp(t, "dialog", 6) == 0;
}

static lxb_dom_element_t *dialog_elem_of(JSValueConst v)
{
    lxb_dom_node_t *n = node_of(v);

    return html_dialog_is_dialog(n) ? lxb_dom_interface_element(n) : NULL;
}

/* "If subject does not have an open attribute" — asked of §4.9's attribute LIST and not of its value, because
   the parser stores a valueless `<dialog open>` with no value buffer at all. */
static bool dialog_has_open(lxb_dom_element_t *el)
{
    return el != NULL && dom_attr_get_ns(el, NULL, "open") != NULL;
}

/* ---- the returnValue IDL attribute --------------------------------------------------------------------------- */

/* "When the element is created, it must be set to the empty string" — so an ABSENT slot is the empty string,
   which is also what makes this cost nothing for the dialogs a page never closes. */
static JSValue js_dialog_get_return_value(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue v;

    (void)magic;
    /* WEB IDL §3.7.6 Attributes' BRAND CHECK, and it is a THROW rather than an assert: a page reaches an
       accessor off the
       prototype with `.call` on anything at all, so the receiver is the PAGE's input. */
    if (!dialog_elem_of(this_val))
        return JS_ThrowTypeError(ctx, "HTMLDialogElement.returnValue read on something that is not a <dialog> "
                                      "element");
    v = ds_get(ctx, this_val, DS_RETURN_VALUE);
    if (JS_IsUndefined(v)) return JS_NewString(ctx, "");
    DCHECK(JS_IsString(v), "the dialog return-value slot held something that is not a string — every write to "
                           "it goes through the DOMString setter or §4.11.4 step 9, and both place a string");
    return v;
}

/* "On setting, it must be set to the new value." CONFIGURABLE AND WRITABLE because it is written again on every
   close, and a slot defined with no flags makes the second write a silent no-op. */
static void dialog_set_return_value(JSContext *ctx, JSValueConst dialog, JSValueConst v)
{
    DCHECK(JS_IsString(v), "the dialog return value was set to something that is not a string — the IDL types "
                           "it DOMString and §4.11.4 step 9 only reaches here with a non-null string result");
    ds_set(ctx, dialog, DS_RETURN_VALUE, JS_DupValue(ctx, v));
}

static JSValue js_dialog_set_return_value(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    (void)magic;
    if (!dialog_elem_of(this_val))
        return JS_ThrowTypeError(ctx, "HTMLDialogElement.returnValue set on something that is not a <dialog> "
                                      "element");
    dialog_set_return_value(ctx, this_val, val);
    return JS_UNDEFINED;
}

/* ---- §4.11.4's `closedby` CONTENT ATTRIBUTE, its COMPUTED CLOSED-BY STATE, and the `closedBy` member --------
 *
 * WHY THIS MEMBER AND NOT `showModal()`, WHICH IS THE ONE EVERY BUNDLE CALLS. Because `showModal()` sits on top
 * of this and not beside it. §4.11.4's show a modal dialog is 20 steps whose step 12 is "Assert: subject's close
 * watcher is not null" — it does not ESTABLISH the watcher, it asserts one is already there, because its step 11
 * adds the `open` attribute and §4.11.4's attribute change steps step 6 runs the dialog setup steps, whose step 5
 * is "Set the dialog close watcher with subject". Set the dialog close watcher is 3 steps whose step 3 establishes
 * the watcher with three algorithms, and the third of them is, whole: "getEnabledState being to return true if
 * dialog's enable close watcher for request close is true or dialog's computed closed-by state is not None;
 * otherwise false."
 *
 * So the COMPUTED CLOSED-BY STATE is a hard input to the establish that §6.10.2's manager arithmetic runs on, and
 * it is the deepest thing under showModal's step 12 that nothing in this build supplies. Landing it first is
 * §Do-subproblems-IN-ORDER, and it is also §4.11.4's OWN IDL ORDER: the interface declares `open`, `returnValue`,
 * `closedBy`, `show()`, `showModal()`, `close()`, `requestClose()`, and `closedBy` is the next member after the
 * two this file already builds.
 *
 * STEP NUMBERS HERE ARE DEPTH-TRACKED AND NOT `<li>` COUNTS, and for this algorithm the two disagree. Retrieve a
 * dialog's computed closed-by state is TWO steps, whose step 1 CONTAINS a nested two-item list; a flat count of
 * `<li>`s reports four and would renumber "step 2" as "step 4". The committed step corpus agrees at two, and
 * agrees at 20 for show a modal dialog, 11 for `show()`, 3 for set the dialog close watcher, 5 for the dialog
 * setup steps and 13 for close the dialog — which is the number this file's close-the-dialog run already uses. */

/* §4.11.4's keyword/state table, verbatim in the standard's own order and brief descriptions: `any` is the Any
   state ("Close requests or clicks outside close the dialog."), `closerequest` is the Close Request state
   ("Close requests close the dialog."), and `none` is the None state ("No user actions automatically close the
   dialog.").
   AUTO IS A STATE WITH NO KEYWORD, which is why it is not a row here: "The closedby attribute's invalid value
   default and missing value default are both the Auto state." §2.3.3's canonical keyword of a state with no
   keyword does not exist, and the getter below asserts it never has to ask for one. */
enum {
    DIALOG_CLOSEDBY_AUTO = 0,
    DIALOG_CLOSEDBY_ANY,
    DIALOG_CLOSEDBY_CLOSE_REQUEST,
    DIALOG_CLOSEDBY_NONE
};
static const EnumeratedKeyword DIALOG_CLOSEDBY_KW[] = {
    { "any",          DIALOG_CLOSEDBY_ANY },
    { "closerequest", DIALOG_CLOSEDBY_CLOSE_REQUEST },
    { "none",         DIALOG_CLOSEDBY_NONE },
    { NULL, 0 }
};

/* §4.11.4's "To retrieve a dialog's computed closed-by state, given a dialog dialog:", 2 steps.
   THE EMPTY VALUE DEFAULT IS THE INVALID ONE because §4.11.4 declares no empty value default — which is
   core/html/enumerated_attribute.h's own stated reduction of §2.3.3 for an attribute that names only two of the
   three special states, and not a guess made here. */
/* TAKES THE WRAPPER AND NOT THE RAW ELEMENT, because step 1.1 reads a per-element SLOT and this file's slots
   hang off the wrapper — which is what makes them per-flow. Its callers each already have one. */
static int dialog_computed_closed_by_state(JSContext *ctx, JSValueConst dialog)
{
    lxb_dom_element_t *el = dialog_elem_of(dialog);
    int state;

    DCHECK(el != NULL, "§4.11.4's computed closed-by state was asked of something that is not a <dialog> "
                       "element — both of its callers brand-check the receiver first, and every caller added "
                       "later owes the same");
    state = enumerated_attribute_state(el, "closedby", DIALOG_CLOSEDBY_KW,
                                       DIALOG_CLOSEDBY_AUTO,   /* missing value default */
                                       DIALOG_CLOSEDBY_AUTO,   /* no empty value default: §2.3.3 step 4 runs */
                                       DIALOG_CLOSEDBY_AUTO);  /* invalid value default */
    if (state == DIALOG_CLOSEDBY_AUTO) {                                                          /* step 1 */
        /* Step 1.1 — "If dialog's is modal is true, then return Close Request." The boolean is REAL per-element
           state now (see the slot table above), so this is the read the standard asks for rather than a
           condition standing in for one. A `realm_awaits` stood here naming `showModal()` as the producer that
           would give it work; the producer for the STORAGE is the flag itself, which close the dialog step 8
           and the removing steps' step 3 already clear, so the probe is retired with the thing it was waiting
           for rather than left to report a built capability as absent. */
        if (ds_flag(ctx, dialog, DS_IS_MODAL)) return DIALOG_CLOSEDBY_CLOSE_REQUEST;              /* step 1.1 */
        return DIALOG_CLOSEDBY_NONE;                                                              /* step 1.2 */
    }
    return state;                                                                                 /* step 2 */
}

/* §4.11.4: "The closedBy getter steps are to return the keyword corresponding to the computed closed-by state
   given this."
   THE TWO ASSERTS ARE TWO-SIDED AND NEITHER STANDS ON PAGE INPUT. The attribute's VALUE is the page's, and it is
   never asserted on: §2.3.3's determine the state maps every unrecognised value to the invalid value default, so
   `<dialog closedby=nonsense>` is Auto and not an error. What IS asserted is the answer THIS file computed —
   the algorithm above returns Close Request or None from step 1 and a keyword-bearing state from step 2, so Auto
   can never reach here and a canonical keyword always exists. */
static JSValue js_dialog_get_closed_by(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el;
    int state;

    (void)magic;
    /* Web IDL §3.7.6 Attributes' BRAND CHECK, a THROW and not an assert for the reason `returnValue`'s getter
       states above: the receiver is whatever the page called the accessor with. */
    if (!(el = dialog_elem_of(this_val)))
        return JS_ThrowTypeError(ctx, "HTMLDialogElement.closedBy read on something that is not a <dialog> "
                                      "element");
    state = dialog_computed_closed_by_state(ctx, this_val);
    DCHECK(state != DIALOG_CLOSEDBY_AUTO,
           "§4.11.4's computed closed-by state answered Auto — its step 1 exists precisely to replace Auto "
           "with Close Request or None, so an Auto here means that step stopped running");
    DCHECK(enumerated_attribute_state_has_keyword(DIALOG_CLOSEDBY_KW, state),
           "§4.11.4's computed closed-by state answered a state with no canonical keyword — the getter must "
           "return one, and §2.3.3 has no keyword to give for a state no row of the table names");
    return JS_NewString(ctx, enumerated_attribute_canonical_keyword(DIALOG_CLOSEDBY_KW, state));
}

/* The IDL is `[CEReactions, ReflectSetter] attribute DOMString closedBy`, so the SETTER is §2.6.1's plain "set
   the content attribute with the given value" while the getter above is §4.11.4's own algorithm — the same
   asymmetry `autocapitalize` has, and the same reason neither can be a reflection-registry row.
   IT HANDS OVER THE VALUE AND NEVER A `char *`. core/dom/element.h's two accessor pairs differ in exactly this,
   and its own prose says so at length: the JSValue form records the taint and stores the shape, while the
   `char *` form runs a ToString that provenance does not survive — so that form ASSERTS on a concolic rather
   than silently flattening one. (Paraphrased and deliberately NOT quoted: a quoted run standing beside a
   citation is read as the STANDARD's words, and engine/citegen.mjs cannot tell a fabricated sentence from a
   piece of this tree quoting itself — which is the note core/html/close_watcher.c's own close-action arm
   leaves for the same reason.) `dialog.closedBy = location.hash.slice(1)` is an ordinary thing for a page to
   write, so taking the C-string form here would trade a solved attacker-source derivation for an abort. */
static JSValue js_dialog_set_closed_by(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    (void)magic;
    if (!dialog_elem_of(this_val))
        return JS_ThrowTypeError(ctx, "HTMLDialogElement.closedBy set on something that is not a <dialog> "
                                      "element");
    element_attr_set_value(ctx, this_val, "closedby", val);
    return JS_UNDEFINED;
}

/* ---- §4.11.4's OWN STATE, AS THE PLATFORM ASKS FOR IT ---------------------------------------------------------
 *
 * Six of the ten rows above are read by somebody who is not this file, and each of those is exported rather
 * than reached for: §6.12 The popover attribute asks whether a `dialog` is modal and writes the previously
 * focused element, and §6.10.2 Close watcher infrastructure's DIALOG arm asks for the two request-close fields
 * and for the enabled state. What is NOT exported is the writing of anything §4.11.4 alone writes. */

/* "Let document be element's node document" — as the Document's WRAPPER, which is what the per-Document slot
   hangs off. OWNED; JS_NULL for an element with no owner document, which no element in a live tree is. */
static JSValue dialog_document_of(JSContext *ctx, JSValueConst el)
{
    lxb_dom_node_t *n = node_of(el);

    if (!n || !n->owner_document) return JS_NULL;
    return node_wrap(ctx, lxb_dom_interface_node(n->owner_document));
}

/* The REALM whose active document is this element's node document — §4.11.4's "dialog's relevant global
   object", and what "is not fully active" is asked in. NULL for a document no navigable holds, which is
   exactly the answer to that question. */
static JSContext *dialog_document_realm(JSValueConst el)
{
    lxb_dom_node_t *n = node_of(el);

    if (!n || !n->owner_document) return NULL;
    return document_active_realm_of(lxb_dom_interface_node(n->owner_document));
}

bool html_dialog_is_modal(JSContext *ctx, JSValueConst el)
{
    /* Asked of ANY element by §6.12's check popover validity, whose disjunct is "element is a dialog element
       and its is modal is set to true" — so the local-name conjunct is answered here and the caller states one
       question. A non-dialog carries no such slot, so the flag read would answer false anyway; the explicit
       test is what makes that a fact rather than an accident of which Symbols this file mints. */
    if (!dialog_elem_of(el)) return false;
    return ds_flag(ctx, el, DS_IS_MODAL);
}

JSValue html_dialog_previously_focused_element(JSContext *ctx, JSValueConst el)
{
    return ds_get_or_null(ctx, el, DS_PREV_FOCUSED);
}

void html_dialog_set_previously_focused_element(JSContext *ctx, JSValueConst el, JSValueConst v)
{
    DCHECK(JS_IsNull(v) || node_of(v) != NULL,
           "§4.11.4's PREVIOUSLY FOCUSED ELEMENT was set to something that is neither null nor a node — the "
           "standard types it \"null or an element\", and both of its writers bind it either to null or to the "
           "focused area's DOM anchor");
    if (JS_IsNull(v)) ds_clear(ctx, el, DS_PREV_FOCUSED);
    else              ds_set(ctx, el, DS_PREV_FOCUSED, JS_DupValue(ctx, v));
}

JSValue html_dialog_request_close_return_value(JSContext *ctx, JSValueConst dialog)
{
    return ds_get_or_null(ctx, dialog, DS_REQ_CLOSE_RETURN);
}

JSValue html_dialog_request_close_source_element(JSContext *ctx, JSValueConst dialog)
{
    return ds_get_or_null(ctx, dialog, DS_REQ_CLOSE_SOURCE);
}

bool html_dialog_close_watcher_enabled(JSContext *ctx, JSValueConst dialog)
{
    lxb_dom_element_t *el = dialog_elem_of(dialog);

    DCHECK(el != NULL,
           "§6.10.2's GET ENABLED STATE for the DIALOG kind was asked of something that is not a `dialog` "
           "element — the only establisher of that kind passes the dialog as the watcher's subject, and "
           "close_watcher.c reads that same subject back");
    /* "…to return true if dialog's enable close watcher for request close is true or dialog's computed
       closed-by state is not None; otherwise false." The first disjunct is what `requestClose()` raises across
       its own step 7, so a dialog whose closedby is None still answers an explicit request. */
    if (ds_flag(ctx, dialog, DS_ENABLE_CW_REQ_CLOSE)) return true;
    return dialog_computed_closed_by_state(ctx, dialog) != DIALOG_CLOSEDBY_NONE;
}

/* ---- HTML §6.3.1 Modal dialogs and inert subtrees ---------------------------------------------------------
 *
 * "A Document document is BLOCKED BY A MODAL DIALOG subject if subject is the topmost dialog element in
 * document's top layer. While document is so blocked, every node that is connected to document, with the
 * exception of the subject element and its flat tree descendants, must become inert."
 *
 * IT IS DERIVED AND NOTHING STORES IT, WHICH IS WHY show a modal dialog's STEP 14 WRITES NO FIELD. That step
 * reads "Set subject's node document to be blocked by the modal dialog subject", and a bit set there would be
 * a second answer to a question css-position-4 §3's top layer already answers — with the two free to disagree
 * the moment §4.11.4's own removing steps take the element out of the layer, or a rendering update processes a
 * pending removal, neither of which is a place anybody would remember to clear a flag. So step 14 is PERFORMED
 * BY STEP 15, which adds the subject to the top layer and thereby makes it the topmost `dialog` in it, and
 * step 14's site carries the two-sided assert instead of a write.
 *
 * "TOPMOST" IS css-position-4 §3's OWN ORDER, read through core/css/top_layer.h's walk rather than by indexing
 * the set here — "the last element in the top layer is rendered on top of everything else" — and that walk
 * reads the `top layer` rather than §3.3's "is in the top layer", which is the reading §6.3.1 wants: a modal
 * dialog whose removal is pending is still rendered, so it still blocks, until a rendering update processes it.
 *
 * It runs no page code: a backwards walk of an Array with a local-name predicate. OWNED, JS_NULL for a document
 * no modal dialog blocks. */
static bool dialog_topmost_dialog_pred(JSContext *ctx, JSValueConst el, void *opaque)
{
    (void)ctx;
    (void)opaque;
    return html_dialog_is_dialog(node_of(el));
}

JSValue html_dialog_blocked_by_modal_dialog(JSContext *ctx, JSValueConst document)
{
    DCHECK(JS_IsObject(document), "§6.3.1's BLOCKED BY A MODAL DIALOG was asked of something that is not a "
                                  "Document — the predicate is stated over one, and its callers resolve a "
                                  "node's owner document before asking");
    return top_layer_topmost(ctx, document, dialog_topmost_dialog_pred, NULL);
}

bool html_dialog_node_is_blocked_by_modal_dialog(JSContext *ctx, const lxb_dom_node_t *n)
{
    JSValue doc, subject;
    lxb_dom_node_t *sn;
    bool blocked;

    if (!n || !n->owner_document) return false;
    doc = node_wrap(ctx, lxb_dom_interface_node(n->owner_document));
    subject = html_dialog_blocked_by_modal_dialog(ctx, doc);
    JS_FreeValue(ctx, doc);
    if (JS_IsNull(subject)) return false;                     /* the document is blocked by nothing */
    sn = node_of(subject);
    JS_FreeValue(ctx, subject);
    DCHECK(sn != NULL, "§6.3.1's topmost `dialog` in a top layer has no element node — the set holds element "
                       "wrappers and nothing else puts a value in it");
    /* "every node that is CONNECTED TO document" — a node the tree no longer holds is not made inert by
       anything, which is also what keeps a detached subtree focusable while a modal dialog is up. */
    if (!node_is_connected((lxb_dom_node_t *)n)) return false;
    /* "with the exception of the SUBJECT ELEMENT and its FLAT TREE DESCENDANTS". The inclusive half is one
       comparison here and the strict half is CSS Scoping §4.1's relation, asked of the ONE walk this build has
       (see core/html/popover.h for why that door is where it is). */
    if (n == sn) return false;
    blocked = !popover_flat_tree_descendant_of(ctx, n, sn);
    return blocked;
}

/* ---- HTML §3.1.1 The Document object's OPEN DIALOGS LIST -------------------------------------------------------
 *
 * "Each Document has an open dialogs list, which is a list of dialog elements, initially empty." §4.11.4's
 * dialog setup steps append to it and its cleanup steps remove from it; §4.11.5 Dialog light dismiss is what
 * READS it, asking whether it is empty and walking it — which is the reader this build does not have yet.
 *   A NAMED RESIDUAL, because the list is CORRECT and its only reader today is the setup steps' own step-3
 * assertion. NOT COVERED: §4.11.5's light dismiss, whose `pointerdown`/`pointerup` steps are stated over this
 * list and over the DIALOG POINTERDOWN TARGET that has no storage here. THE NEXT DIFF builds §4.11.5 and reads
 * this list from it. HOW ITS ABSENCE WOULD SHOW: a click outside an `<dialog closedby=any>` closes nothing,
 * where a browser closes the dialog. The list is written NOW rather than with that section because §4.11.4's
 * own setup steps are what write it and they are here. */

/* The document's list, minted on first append. OWNED; JS_UNDEFINED when the document has never had one, which
   is the standard's empty list and is what every read below treats it as. */
static JSValue dialog_open_dialogs_list(JSContext *ctx, JSValueConst doc, bool create)
{
    JSValue list = ds_get(ctx, doc, DS_DOC_OPEN_DIALOGS);

    if (!JS_IsUndefined(list)) {
        DCHECK(JS_IsArray(list), "a Document's OPEN DIALOGS LIST is not an array — the one site that mints it "
                                 "makes an Array and nothing else writes that slot");
        return list;
    }
    if (!create) return JS_UNDEFINED;
    list = JS_NewArray(ctx);
    CHECK(!JS_IsException(list), "a Document's HTML §3.1.1 open dialogs list could not be allocated");
    ds_set(ctx, doc, DS_DOC_OPEN_DIALOGS, JS_DupValue(ctx, list));
    return list;
}

static uint32_t dialog_list_len(JSContext *ctx, JSValueConst list)
{
    JSValue v;
    uint32_t n = 0;

    if (JS_IsUndefined(list)) return 0;
    v = JS_GetPropertyStr(ctx, list, "length");
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

/* Infra's "list contains" over the ONE identity a wrapper has — two wrappers of one element are one object in
   this engine, so pointer equality IS the standard's identity here. −1 for absent. */
static int64_t dialog_list_index_of(JSContext *ctx, JSValueConst list, JSValueConst el)
{
    uint32_t n = dialog_list_len(ctx, list), i;

    for (i = 0; i < n; i++) {
        JSValue m = JS_GetPropertyUint32(ctx, list, i);
        bool same = JS_VALUE_GET_PTR(m) == JS_VALUE_GET_PTR(el);

        JS_FreeValue(ctx, m);
        if (same) return (int64_t)i;
    }
    return -1;
}

/* ---- §4.11.4's CLOSE WATCHER: set it, the setup steps, the cleanup steps, and the two DOM hooks that run them
 *
 * WHAT THIS CHAIN IS AND WHY IT IS NOT PART OF ANY METHOD. `showModal()`'s step 12 is "Assert: subject's close
 * watcher is not null" — it does not establish one. Its step 11 adds the `open` attribute, and everything
 * below is what that write sets off: the attribute change steps run the dialog setup steps, whose step 5 sets
 * the dialog close watcher, whose step 3 establishes it. So the chain is reachable with NO member call at all —
 * `d.open = true` on a connected `<dialog>` establishes a watcher and `d.removeAttribute("open")` destroys it —
 * and it is the subproblem every one of §4.11.4's four methods stands on.
 *
 * THERE ARE TWO HOOKS AND NOT ONE, AND MISSING THE SECOND WOULD HAVE PUT AN ABORT ON ORDINARY MARKUP. The
 * attribute change steps' step 5 returns for an element that is not connected, and the standard's note beside
 * it says why: "This ensures that the dialog setup steps are not run on nodes that are disconnected, which
 * would result in a close watcher being established." A parsed `<dialog open>` has its attribute set BEFORE it
 * is inserted, so those steps see a disconnected node and do nothing — and the DIALOG HTML ELEMENT INSERTION
 * STEPS, 2 steps, are what cover it: they run the setup steps for a node that arrives already open. Between
 * the two hooks every edge that can make a dialog open-and-connected runs the setup steps exactly once, which
 * is what lets request to close the dialog ASSERT at its step 3 rather than test.
 *   THAT SECOND HOOK IS EASY TO MISS AND WAS MISSED HERE FOR ONE READING: §4.11.4 gives its setup steps,
 * cleanup steps and removing steps each a `<dfn>` of their own and gives the insertion steps NONE — they hang
 * off the shared "HTML element insertion steps" link — so an enumeration built from the section's definitions
 * is complete and wrong. It was caught by asking what `<dialog open>` in markup does at step 3's assert, which
 * is the question an enumeration cannot ask itself. */

/* §4.11.4's "To SET THE DIALOG CLOSE WATCHER, given a dialog element dialog:", 3 steps. */
static void dialog_set_close_watcher(JSContext *ctx, JSValueConst dialog)
{
    JSContext *wctx = dialog_document_realm(dialog);

    /* Step 1. */
    DCHECK(ds_is_null(ctx, dialog, DS_CLOSE_WATCHER),
           "§4.11.4's set the dialog close watcher step 1 asserts the dialog's close watcher is null, and this "
           "one already has one — its only caller is the dialog setup steps, which the attribute change steps "
           "reach only when the `open` attribute went from absent to present, and the cleanup steps clear the "
           "field on the mirror transition. A watcher here means one of those two edges was missed and the "
           "manager is holding a watcher nothing will ever destroy");
    /* Step 2, BOTH conjuncts. The realm is the second of them: a Document no navigable holds is not fully
       active, so a NULL realm is that half of the assertion answering false rather than a missing pointer. */
    DCHECK(dialog_has_open(dialog_elem_of(dialog)),
           "§4.11.4's set the dialog close watcher step 2 asserts the dialog has an open attribute — the setup "
           "steps assert the same thing at their own step 1, so a failure here is a `beforetoggle` listener "
           "that cannot exist yet");
    DCHECK(wctx != NULL && document_fully_active(wctx),
           "§4.11.4's set the dialog close watcher step 2 asserts the dialog's node document is fully active — "
           "the attribute change steps' step 4 refuses a document that is not, so this is the two-sided half "
           "of that guard");
    /* Step 3 — "Set dialog's close watcher to the result of establishing a close watcher given dialog's
       relevant global object, with: …". The three algorithms travel as core/html/close_watcher.h's DIALOG
       kind, which is where that header states them and why a kind id can hold three algorithms where a JS
       closure and a C function pointer cannot; this supplies the WINDOW and the SUBJECT, the only two things
       establish takes that vary. THE SUBJECT IS THE DIALOG ELEMENT, because all three of its algorithms are
       stated over it — the cancel action fires at it, the close action closes it, and the enabled state reads
       two of its fields. */
    ds_set(ctx, dialog, DS_CLOSE_WATCHER,
           close_watcher_establish(wctx, CLOSE_WATCHER_KIND_DIALOG, dialog));
}

/* §4.11.4's "DIALOG SETUP STEPS, given a dialog element subject", 5 steps. */
static void dialog_setup_steps(JSContext *ctx, JSValueConst subject)
{
    JSValue doc = dialog_document_of(ctx, subject);
    JSValue list;

    DCHECK(dialog_has_open(dialog_elem_of(subject)), "§4.11.4's dialog setup steps step 1");   /* step 1 */
    DCHECK(node_is_connected(node_of(subject)), "§4.11.4's dialog setup steps step 2");        /* step 2 */
    DCHECK(JS_IsObject(doc),
           "§4.11.4's dialog setup steps reached a connected `dialog` with no node document wrapper — a "
           "connected node has an owner document by construction");
    list = dialog_open_dialogs_list(ctx, doc, /*create*/ true);
    DCHECK(dialog_list_index_of(ctx, list, subject) < 0,                                       /* step 3 */
           "§4.11.4's dialog setup steps step 3 asserts the document's OPEN DIALOGS LIST does not contain the "
           "subject — these steps run only on the `open` attribute's absent-to-present edge and the cleanup "
           "steps remove the entry on the mirror edge, so an entry here means a dialog was set up twice and "
           "the cleanup that runs next will leave one of the two behind for ever");
    JS_SetPropertyUint32(ctx, list, dialog_list_len(ctx, list), JS_DupValue(ctx, subject));    /* step 4 */
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, doc);
    dialog_set_close_watcher(ctx, subject);                                                    /* step 5 */
}

/* §4.11.4's "DIALOG CLEANUP STEPS, given a dialog element subject", 2 steps. */
static void dialog_cleanup_steps(JSContext *ctx, JSValueConst subject)
{
    JSValue doc = dialog_document_of(ctx, subject);
    JSValue watcher;

    /* Step 1 — Infra's "remove", which is a no-op for an item the list does not hold. A dialog whose setup
       steps never ran (a parsed `<dialog open>`, the standard's own case) reaches here on its removal and has
       no entry, so the absence is ordinary rather than a fault. */
    if (JS_IsObject(doc)) {
        JSValue list = dialog_open_dialogs_list(ctx, doc, /*create*/ false);

        if (!JS_IsUndefined(list)) {
            int64_t at = dialog_list_index_of(ctx, list, subject);

            if (at >= 0) {
                JSValue splice = JS_GetPropertyStr(ctx, list, "splice");
                JSValueConst argv[2];
                JSValue n = JS_NewInt64(ctx, at), one = JS_NewInt32(ctx, 1);

                argv[0] = n;
                argv[1] = one;
                JS_FreeValue(ctx, JS_Call(ctx, splice, list, 2, argv));
                JS_FreeValue(ctx, one);
                JS_FreeValue(ctx, n);
                JS_FreeValue(ctx, splice);
            }
        }
        JS_FreeValue(ctx, list);
    }
    JS_FreeValue(ctx, doc);
    /* §4.11.4's step 2 — "If subject's close watcher is not null: Destroy subject's close watcher. Set
       subject's close watcher to null." §6.10.2's destroy is idempotent and reads and writes only the manager,
       so it is a plain call.
       THE CONDITION IS ASKED WITHOUT TAKING THE VALUE, so the step reads as the sentence it implements and the
       arm that needs the watcher is the only thing that owns one. */
    if (!ds_is_null(ctx, subject, DS_CLOSE_WATCHER)) {
        JSContext *wctx = dialog_document_realm(subject);

        /* "DESTROY subject's close watcher" IS A REMOVAL FROM THAT WATCHER'S WINDOW'S MANAGER, and §6.10.2
           keeps the manager as a per-realm record — so when the dialog's node document has stopped being any
           realm's active document, the manager it was in is gone with the realm and there is nothing left to
           remove it from. The work is EMPTY rather than skipped, which is why this is a condition evaluated at
           the step that asks it and not an assert: a page that keeps a reference to a navigated-away
           same-origin document and removes a `<dialog open>` out of it is in exactly that state, and its
           bytes are the PAGE's rather than a value this file computed.
           THE CLEAR BELOW IS NOT PART OF THAT CONDITION. §4.11.4's step 2 has a second half — "Set subject's
           close watcher to null" — and it is true on both arms — the field must stop naming a watcher whose manager no
           longer exists, or a later setup step's step-1 assert fires on a dialog that is legitimately fresh. */
        if (wctx != NULL) {
            watcher = ds_get(ctx, subject, DS_CLOSE_WATCHER);
            close_watcher_destroy(wctx, watcher);
            JS_FreeValue(ctx, watcher);
        }
        ds_clear(ctx, subject, DS_CLOSE_WATCHER);
    }
}

void html_dialog_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                              const char *old_val, const char *val)
{
    JSContext *rctx;
    JSValue subject;

    /* "…are used for dialog elements": §4.9's list is one hook for every element, so the local name of the
       ELEMENT is asked before any of the six steps, which are stated only for this one. */
    if (!html_dialog_is_dialog(lxb_dom_interface_node(el))) return;
    if (ns != NULL) return;                                                                    /* step 1 */
    if (strcmp(local, "open") != 0) return;                                                    /* step 2 */
    subject = node_wrap(ctx, lxb_dom_interface_node(el));
    if (val == NULL && old_val != NULL) dialog_cleanup_steps(ctx, subject);                    /* step 3 */
    /* Step 4 — "If element's node document is not fully active, then return." A Document no navigable holds is
       not fully active, so a NULL realm IS that answer rather than a missing pointer. */
    rctx = dialog_document_realm(subject);
    if (rctx == NULL || !document_fully_active(rctx)) { JS_FreeValue(ctx, subject); return; }
    if (!node_is_connected(lxb_dom_interface_node(el))) { JS_FreeValue(ctx, subject); return; }/* step 5 */
    if (val != NULL && old_val == NULL) dialog_setup_steps(ctx, subject);                      /* step 6 */
    JS_FreeValue(ctx, subject);
}

void html_dialog_insertion_steps(JSContext *ctx, JSValueConst el)
{
    lxb_dom_element_t *dialog = dialog_elem_of(el);
    JSContext *rctx;

    if (!dialog) return;
    /* Step 1 — "If insertedNode's node document is not fully active, then return." A Document no navigable
       holds is not fully active, so a NULL realm IS that answer. */
    rctx = dialog_document_realm(el);
    if (rctx == NULL || !document_fully_active(rctx)) return;
    /* Step 2 — "If insertedNode has an open attribute and is connected, then run the dialog setup steps given
       insertedNode."
       THIS IS THE STEP THAT MAKES A PARSED `<dialog open>` DISMISSABLE, and it is why §4.11.4's request to
       close the dialog may ASSERT at its step 3 that an open, connected, fully active dialog has a watcher.
       The attribute change steps cannot cover that case and the standard's own note beside their step 5 says
       so: the parser sets `open` before the element is inserted, so those steps see a disconnected node and
       return. Between the two hooks, every edge that can make a dialog open-and-connected runs the setup
       steps exactly once. */
    if (dialog_has_open(dialog) && node_is_connected(node_of(el))) dialog_setup_steps(ctx, el);
}

void html_dialog_parsed(JSContext *ctx, lxb_dom_node_t *root)
{
    lxb_dom_node_t *n;

    DCHECK(root != NULL, "§4.11.4's parsed-tree walk was given no tree to walk");
    DCHECK(g_slot_atom[DS_IS_MODAL] != JS_ATOM_NULL,
           "a parsed tree reached §4.11.4 with no state slot keys minted — html_dialog_declare mints them "
           "once per agent, and it must have run by the time any document is installed");
    /* SHADOW-INCLUDING, for the reason html_style_element_parsed's walk is: a `<template shadowrootmode>` has
       by now moved its contents into a shadow root, and a `<dialog open>` among them is connected. */
    for (n = root; n; n = shadow_root_next_in_shadow_including(ctx, n, root)) {
        if (!html_dialog_is_dialog(n)) continue;
        {
            JSValue w = node_wrap(ctx, n);

            html_dialog_insertion_steps(ctx, w);
            JS_FreeValue(ctx, w);
        }
    }
}

void html_dialog_removing_steps(JSContext *ctx, JSValueConst el)
{
    lxb_dom_element_t *dialog = dialog_elem_of(el);

    if (!dialog) return;
    /* Step 1 — the cleanup steps, which the attribute change steps do NOT run for a removal, because removing
       a node changes no attribute. This is the other half of the watcher's life: a `<dialog open>` taken out
       of the document must stop holding a place in its Window's close watcher manager. */
    if (dialog_has_open(dialog)) dialog_cleanup_steps(ctx, el);
    /* Step 2 — "If removedNode's node document's top layer contains removedNode, then remove an element from
       the top layer immediately given removedNode." IMMEDIATELY and not the request: a removed element has no
       rendering update coming that would process a pending removal, so the deferred form would leave it in the
       set for ever. */
    if (top_layer_is_in(ctx, el)) top_layer_remove_immediately(ctx, el);
    ds_clear(ctx, el, DS_IS_MODAL);                                                            /* step 3 */
}

/* ---- §4.11.4's QUEUE A DIALOG TOGGLE EVENT TASK ---------------------------------------------------------------
 *
 * The task is a JOB, which in this engine is a call-root FLOW: preemptible, forkable and parkable. It has to
 * be, because it FIRES an event and every listener body is the page's code — the same reason navigable.c's
 * step-14 load is a job and not a call.
 *
 * The tracker is what makes two state changes in one turn produce ONE `toggle` carrying the TRUE old state
 * rather than two. Its first half is built here; its second half — removing the already-queued task — names a
 * capability this engine's job queue does not have, at the step that needs it. */
typedef struct {
    JSStepHdr hdr;
    uint8_t   fphase;   /* the toggle fire request's own phase */
    JSValue   ev;       /* the ToggleEvent, minted once and held across the suspension (owned) */
    EventFireCb cb;     /* the fire request buffer: [this, dispatch, target, event] */
} DialogToggleTask;

#define DIALOG_TOGGLE_STAGES(X) \
    X(DT_ENTER, "HTML §4.11.4 queue a dialog toggle event task step 2 (the queued element task begins: build " \
                "the ToggleEvent with oldState, newState and source initialized)") \
    X(DT_FIRE,  "HTML §4.11.4 queue a dialog toggle event task step 2 (fire an event named toggle at element " \
                "using that ToggleEvent, then set the element's dialog toggle task tracker to null)")
enum { DIALOG_TOGGLE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DIALOG_TOGGLE_STEPS[] = { DIALOG_TOGGLE_STAGES(JS_STEP_STAGE_LABEL) NULL };

static void dialog_tracker_clear(JSContext *ctx, JSValueConst element)
{
    ds_clear(ctx, element, DS_TRACKER);
    ds_clear(ctx, element, DS_TRACKER_TASK);
}

static int js_dialog_toggle_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    DialogToggleTask *s = st;
    JSValueConst element = step_arg(&s->hdr, 0);
    int r;

    if (s->hdr.stage == DT_ENTER) {
        const char *old_state, *new_state;
        int k;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY owned field before the first thing that can fail — the failure path tears this state down
           through js_dialog_toggle_visit, which frees exactly what the state holds, and a zeroed block is not
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
        /* Neither bubbling nor cancelable: §4.11.4 names three initialisers for this fire and no others. */
        s->ev = toggle_event_new(ctx, "toggle", old_state, new_state, step_arg(&s->hdr, 3),
                                 /*bubbles*/ false, /*cancelable*/ false);
        JS_FreeCString(ctx, old_state);
        JS_FreeCString(ctx, new_state);
        if (JS_IsException(s->ev)) {
            s->ev = JS_UNDEFINED;
            return JS_STEP_ABRUPT;
        }
        STEP_GOTO(s->hdr.stage, DT_FIRE, &s->fphase, NULL);
    }
    DCHECK(s->hdr.stage == DT_FIRE, "the dialog toggle task resumed at a stage §4.11.4 does not have");
    r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), element, s->ev, JS_UNDEFINED, cb_result,
                              NULL, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    dialog_tracker_clear(ctx, element);
    return JS_STEP_DONE;
}

static void js_dialog_toggle_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    DialogToggleTask *s = st;
    int k;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k)
        v->val(ctx, &s->cb[k]);
}

static const JSTrampStepDef js_dialog_toggle_def = {
    sizeof(DialogToggleTask), js_dialog_toggle_step, NULL, 0,
    .visit = js_dialog_toggle_visit,
    .algorithm = "HTML §4.11.4 the dialog toggle event task",
    .steps = DIALOG_TOGGLE_STEPS
};

static void dialog_queue_toggle_task(JSContext *ctx, JSValueConst element, const char *old_state,
                                     const char *new_state, JSValueConst source)
{
    JSValueConst argv[4];
    JSValue fn, ov, nv, pending;
    JSTaskHandle handle;

    /* Step 1: "if element's dialog toggle task tracker is not null" — carry its OLD STATE forward, REMOVE its
       task from its task queue, and clear it. All three run here.
       THE REMOVAL USED TO BE A DFAIL SAYING THIS ENGINE HAD NO HANDLE TO REMOVE A TASK BY, and that claim was
       FALSE by the time anybody read it: JS_EnqueueCallTask answers a JSTaskHandle and JS_RemoveQueuedTask
       takes one, and that function's own contract names this very step ("HTML's 'remove <task> from its task
       queue', the step every toggle task tracker rests on (§4.11.1 details, §4.11.4 dialog, §6.12 popover)").
       It is deleted rather than re-aimed: a crash that says a capability is absent tells the next reader not to
       go and look, which is the one kind of stale claim nothing catches.
       JS_RemoveQueuedTask answers 0 as ordinarily as 1 — a handle outlives the task it names, so the task may
       already have run or be running right now, which is the standard's own no-op. */
    ov = JS_UNDEFINED;
    pending = ds_get(ctx, element, DS_TRACKER);
    if (!JS_IsUndefined(pending)) {
        JSValue task;

        DCHECK(JS_IsString(pending), "the dialog toggle task tracker's OLD STATE slot held something that is "
                                     "not a string — the only writer is step 3 below, which places one of "
                                     "§4.11.4's two state strings");
        ov = pending;                                                       /* step 1.1's carry of oldState */
        task = ds_get(ctx, element, DS_TRACKER_TASK);
        if (!JS_IsUndefined(task)) {
            uint64_t h = 0;

            JS_ToBigUint64(ctx, &h, task);
            JS_FreeValue(ctx, task);
            JS_RemoveQueuedTask(JS_GetRuntime(ctx), (JSTaskHandle)h);       /* step 1.2's removal */
        } else {
            DFAIL("§4.11.4's dialog toggle task tracker held an OLD STATE with no TASK — the two slots are one "
                  "struct and step 3 writes both, so one without the other means a write went missing and the "
                  "superseded `toggle` cannot be taken off its queue");
        }
        dialog_tracker_clear(ctx, element);                                 /* step 1.3 */
    }
    if (JS_IsUndefined(ov)) {
        ov = JS_NewString(ctx, old_state);
        CHECK(!JS_IsException(ov), "the dialog toggle task's old state could not be allocated");
    }
    if (g_toggle_stepid < 0)
        g_toggle_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_dialog_toggle_def);
    /* THE CALLEE IS MINTED IN THE ENQUEUING REALM: a C function runs in the realm that DEFINED it, and this
       one fires an event at an element of THIS document. */
    fn = JS_NewCFunction2(ctx, NULL, "dialogToggle", 4, JS_CFUNC_step, g_toggle_stepid);
    CHECK(!JS_IsException(fn), "the dialog toggle task's callee could not be allocated");
    nv = JS_NewString(ctx, new_state);
    CHECK(!JS_IsException(nv), "the dialog toggle task's new state could not be allocated");
    argv[0] = element;
    argv[1] = ov;
    argv[2] = nv;
    argv[3] = source;
    /* Step 2 is "QUEUE AN ELEMENT TASK GIVEN THE DOM MANIPULATION TASK SOURCE and element to run the following
       steps", so it is a task and it was a microtask — a different position in HTML §8.1.7's event loop and
       not a smaller one. The one caller this build has is §4.10.22.3 step 11.6's close-the-dialog, reached by
       submitting a `<form method=dialog>` inside an open `<dialog>`, so a microtask fired `toggle` inside the
       checkpoint of the script that called `submit()` — ahead of every task already standing, an expired
       timer or a delivered message among them. */
    handle = JS_EnqueueCallTask(ctx, fn, 4, argv);   /* §4.11.4: the DOM manipulation task source */
    /* Step 3: "set element's dialog toggle task tracker to a struct with task set to the just-queued task and
       old state set to oldState" — BOTH fields, in the two slots that are the one struct. */
    ds_set(ctx, element, DS_TRACKER, JS_DupValue(ctx, ov));
    ds_set(ctx, element, DS_TRACKER_TASK, JS_NewBigUint64(ctx, (uint64_t)handle));
    JS_FreeValue(ctx, nv);
    JS_FreeValue(ctx, ov);
    JS_FreeValue(ctx, fn);
}

/* ---- §4.11.4's CLOSE THE DIALOG -------------------------------------------------------------------------------- */

#define DIALOG_CLOSE_PHASES(X) \
    X(DCR_BEFORETOGGLE, "HTML §4.11.4 close the dialog step 2 (fire an event named beforetoggle, using " \
                        "ToggleEvent, with oldState \"open\", newState \"closed\" and source, at subject)") \
    X(DCR_FINISH,       "HTML §4.11.4 close the dialog steps 3-12 (the second open-attribute check, the queued " \
                        "toggle task, removing the open attribute, the top-layer removal, the return value, " \
                        "the two request-close clears, and step 12's focus decision)") \
    X(DCR_FOCUS,        "HTML §4.11.4 close the dialog step 12.3 (run HTML §6.6.4 Processing model's focusing " \
                        "steps for the dialog's previously focused element) and step 13's queued `close`")
enum { DIALOG_CLOSE_PHASES(JS_STEP_STAGE_ENUM) };
static const char *const DIALOG_CLOSE_PHASE_STEPS[] = { DIALOG_CLOSE_PHASES(JS_STEP_STAGE_LABEL) NULL };

struct DialogCloseRun {
    uint8_t phase;      /* DCR_* */
    uint8_t fphase;     /* step 2's fire request's own phase */
    uint8_t focus_phase;/* step 12.3's §6.6.4 focusing-steps request's own phase */
    uint8_t was_modal;  /* step 7's wasModal, latched before step 8 clears the flag and read at step 12.3 */
    JSValue subject;    /* the dialog being closed (owned) */
    JSValue result;     /* the spec's "null or a string" (owned) */
    JSValue source;     /* the spec's "Element or null" (owned) */
    JSValue ev;         /* step 2's `beforetoggle` event (owned) */
    JSValue prev;       /* step 12.1's element, held across step 12.3's request (owned) */
    /* ONE BUFFER FOR BOTH REQUESTS, because they stand in DIFFERENT PHASES and at most one is ever in flight:
       step 2's fire is finished before step 12.3 is reached. It is declared as the WIDER of the two —
       EventFireCb is 5 slots and FOCUS_ELEMENT_CB_SLOTS is 3 — which is what core/events/event_target.h's own
       note asks a caller serving several requests to do rather than counting at each site. */
    EventFireCb cb;
};

static void dialog_close_elem_visit(JSContext *ctx, void *elem, JSStepVisit *v)
{
    DialogCloseRun *r = elem;
    int k;

    v->val(ctx, &r->subject);
    v->val(ctx, &r->result);
    v->val(ctx, &r->source);
    v->val(ctx, &r->ev);
    v->val(ctx, &r->prev);
    STEP_CB_FOREACH(r->cb, k)
        v->val(ctx, &r->cb[k]);
}

void html_dialog_close_visit(JSContext *ctx, DialogCloseRun **slot, JSStepVisit *v)
{
    /* ONE operation rather than a buffer copy plus a loop, because the two consumers need OPPOSITE ORDER — the
       clone must copy the block before taking references into it, the teardown must release those references
       before freeing it. The same reason constraint_validation_visit is written this way. */
    v->array(ctx, (void **)slot, sizeof(struct DialogCloseRun), 1, 1, dialog_close_elem_visit);
}

void html_dialog_close_release(JSContext *ctx, DialogCloseRun **slot)
{
    DialogCloseRun *r = *slot;
    int k;

    if (!r) return;
    JS_FreeValue(ctx, r->subject);
    JS_FreeValue(ctx, r->result);
    JS_FreeValue(ctx, r->source);
    JS_FreeValue(ctx, r->ev);
    JS_FreeValue(ctx, r->prev);
    STEP_CB_FOREACH(r->cb, k) JS_FreeValue(ctx, r->cb[k]);
    js_free(ctx, r);
    *slot = NULL;
}

/* §4.11.4's close the dialog STEP 13, which every one of step 12's four exits reaches: "Queue an element task
   on the user interaction task source given the subject element to fire an event named close at subject."
   QUEUED, which is what event_target_fire is — the dispatch becomes a first-class flow the one scheduler
   drives, so a `close` listener's body suspends and resumes like any other program, and the member or the
   submission that reached here has already returned by the time a page sees `close`. It is one function rather
   than four copies because §12's own structure gives it four callers. */
static int dialog_close_tail(JSContext *ctx, DialogCloseRun *r)
{
    event_target_fire(ctx, r->subject, event_new(ctx, "close", /*bubbles*/ false, /*cancelable*/ false),
                      JS_UNDEFINED);
    return 0;
}

int html_dialog_close_run(JSContext *ctx, DialogCloseRun **slot, JSValueConst subject, JSValueConst result,
                          JSValueConst source, JSValue in, JSValue **out_cb, int *out_argc)
{
    DialogCloseRun *r = *slot;
    lxb_dom_element_t *el;
    int rc;

    if (!r) {
        int k;

        r = js_mallocz(ctx, sizeof *r);
        CHECK(r != NULL, "dialog: OOM allocating §4.11.4's close-the-dialog state");
        *slot = r;
        /* EVERY owned field before anything that can fail: an abandoned run is released through the one
           declaration above, which frees exactly what the state holds and nothing else. */
        r->subject = r->result = r->source = r->ev = r->prev = JS_UNDEFINED;
        STEP_CB_FOREACH(r->cb, k) r->cb[k] = JS_UNDEFINED;
        r->phase = DCR_BEFORETOGGLE;
        r->focus_phase = 0;
        r->was_modal = 0;
        DCHECK(dialog_elem_of(subject) != NULL,
               "close the dialog was performed on something that is not a <dialog> element — every caller "
               "reaches it from a walk that already answered html_dialog_is_dialog");
        DCHECK(JS_IsNull(result) || JS_IsString(result),
               "close the dialog was given a result that is neither null nor a string, which is the type §4.11.4 "
               "declares for it");
        DCHECK(JS_IsNull(source) || node_of(source) != NULL,
               "close the dialog was given a source that is neither null nor an element");
        r->subject = JS_DupValue(ctx, subject);
        r->result = JS_DupValue(ctx, result);
        r->source = JS_DupValue(ctx, source);
    }
    DCHECK(DIALOG_CLOSE_PHASE_STEPS[DCR_FOCUS] != NULL, "the phase list lost the step its last phase rests at");
    el = dialog_elem_of(r->subject);
    /* `in` IS CONSUMED, which is the contract every request forwarder here keeps: exactly one path hands it on
       (the fire request, which takes ownership) and every other path releases it before returning. */
    if (r->phase == DCR_BEFORETOGGLE) {
        if (!dialog_has_open(el)) { JS_FreeValue(ctx, in); return 0; }        /* step 1 */
        if (JS_IsUndefined(r->ev)) {
            /* Step 2 names three initialisers and no others, so the fire is neither bubbling nor cancelable —
               `beforetoggle` for a CLOSE cannot be prevented, which is why step 3 re-reads the attribute
               instead of reading a canceled flag. */
            r->ev = toggle_event_new(ctx, "beforetoggle", DIALOG_STATE_OPEN, DIALOG_STATE_CLOSED, r->source,
                                     /*bubbles*/ false, /*cancelable*/ false);
            if (JS_IsException(r->ev)) {
                r->ev = JS_UNDEFINED;
                JS_FreeValue(ctx, in);
                return -1;
            }
        }
        rc = event_target_fire_run(ctx, &r->fphase, STEP_CB(r->cb), r->subject, r->ev, JS_UNDEFINED, in,
                                   NULL, out_cb, out_argc);
        if (rc > 0) return rc;
        if (rc < 0) return -1;
        in = JS_UNDEFINED;   /* the fire request TOOK it; `in` is spent from here on */
        JS_FreeValue(ctx, r->ev);
        r->ev = JS_UNDEFINED;
        r->phase = DCR_FINISH;
    }
    if (r->phase == DCR_FINISH) {
        /* NOTHING IN STEPS 3-12 ASKS ANYTHING, so a completion arriving in this phase is spent — and it is
           released HERE rather than at the entry, because the phase below this one DOES ask and its re-entry
           carries the focusing steps' answer. An unconditional free at the door would have eaten it. */
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        /* Step 3: the attribute is read AGAIN, because a `beforetoggle` handler had the element in its
           hands. */
        if (!dialog_has_open(el)) return 0;
        dialog_queue_toggle_task(ctx, r->subject, DIALOG_STATE_OPEN, DIALOG_STATE_CLOSED, r->source); /* 4 */
        /* Step 5 — and it is far more than an attribute write. The removal goes through the DOM mutation
           chokepoint, so §4.11.4's own attribute change steps run: their step 3 sees `open` go from present to
           absent and runs the DIALOG CLEANUP STEPS, which take the dialog out of its document's open dialogs
           list and DESTROY its close watcher. That is the standard's design and not a side effect to work
           around — reached from a close request, §6.10.2's close a close watcher has already destroyed the
           watcher at its own step 4, and destroy is idempotent for exactly this. */
        dom_cow_remove_attribute(el, "open");
        /* Step 6 — "If is modal of subject is true, then request an element to be removed from the top layer
           given subject." THE REQUEST AND NOT THE IMMEDIATE REMOVAL: CSS Positioned Layout Level 4 §3.3 keeps
           the element in the set with its removal PENDING until a rendering update processes it, which is what
           lets a page close and re-show a dialog in one task without the set losing it in between. */
        if (ds_flag(ctx, r->subject, DS_IS_MODAL)) top_layer_request_removal(ctx, r->subject);
        r->was_modal = ds_flag(ctx, r->subject, DS_IS_MODAL) ? 1 : 0;                             /* step 7 */
        ds_clear(ctx, r->subject, DS_IS_MODAL);                                                   /* step 8 */
        if (!JS_IsNull(r->result))                                                                /* step 9 */
            dialog_set_return_value(ctx, r->subject, r->result);
        ds_clear(ctx, r->subject, DS_REQ_CLOSE_RETURN);                                          /* step 10 */
        ds_clear(ctx, r->subject, DS_REQ_CLOSE_SOURCE);                                          /* step 11 */
        /* Step 12's condition, and steps 12.1 and 12.2 inside it. The CLEAR is unconditional within the block
           and only step 12.3's focusing steps are gated, which is why the two are apart here. */
        DCHECK(JS_IsUndefined(r->prev), "a close-the-dialog run reached step 12 already holding a previously "
                                        "focused element — step 12.1 is the only writer of that field and the "
                                        "phase it stands in runs once");
        r->prev = html_dialog_previously_focused_element(ctx, r->subject);                       /* step 12.1 */
        if (JS_IsNull(r->prev)) {
            JS_FreeValue(ctx, r->prev);
            r->prev = JS_UNDEFINED;
            return dialog_close_tail(ctx, r);
        }
        html_dialog_set_previously_focused_element(ctx, r->subject, JS_NULL);                    /* step 12.2 */
        {
            /* Step 12.3's first disjunct — "subject's node document's focused area of the document's DOM
               anchor is a shadow-including inclusive descendant of subject". §6.6.2's anchor is a NODE and is
               this engine's Document whenever the focused area is the viewport (core/html/focus.h says so at
               its door), which is a descendant of nothing — so a dialog the page never focused into does not
               restore focus unless it was MODAL, which is the second disjunct and the browser-visible answer.
               THE REALM MAY BE ABSENT HERE and that is not an assertion failure: step 5's attribute removal ran
               the page's nothing, but a `beforetoggle` listener two phases back may have moved the dialog into
               a document no navigable holds. With no focused area to compare against, the first disjunct is
               false and only wasModal can carry the step. */
            JSContext *docctx = dialog_document_realm(r->subject);
            bool inside = false;

            if (docctx != NULL) {
                JSValue anchor = focus_focused_area_dom_anchor(docctx);

                inside = shadow_root_is_shadow_including_inclusive_ancestor(node_of(r->subject),
                                                                           node_of(anchor));
                JS_FreeValue(ctx, anchor);
            }
            /* THE REALM IS PART OF THE CONDITION AND NOT A SEPARATE GUARD. §6.6.4's focusing steps are stated
               over a Document's focused area, and a document that is no realm's active document has none — so
               with no realm there is nothing for step 12.3 to run, on EITHER disjunct. Close the dialog has no
               fully-active certification of its own (its step 1 asks only about the `open` attribute), which
               is what separates this from §6.12's hide a popover, whose step 1 does and which therefore
               asserts here instead. */
            if (docctx == NULL || !(inside || r->was_modal)) {
                JS_FreeValue(ctx, r->prev);
                r->prev = JS_UNDEFINED;
                return dialog_close_tail(ctx, r);
            }
            if (!html_element_is(r->prev))
                DFAIL("HTML §4.11.4 The dialog element's close the dialog step 12.3 runs HTML §6.6.4 "
                      "Processing model's FOCUSING STEPS for the dialog's PREVIOUSLY FOCUSED ELEMENT, and this "
                      "one is not an element. That field is bound to \"the focused element\" by §4.11.4's own "
                      "show()/showModal() and to \"document's focused area of the document's DOM anchor\" by "
                      "§6.12 The popover attribute's show popover step 17, and §6.6.2 Data model makes the "
                      "viewport's anchor the DOCUMENT — while §6.6.4 declares the focusing steps over \"an "
                      "object new focus target that is either a focusable area, or an element that is not a "
                      "focusable area, or a navigable\", which a Document is none of. BUILD §6.6.4 step 1's "
                      "\"if new focus target is not a focusable area, then set new focus target to the result "
                      "of getting the focusable area for new focus target\" as a door on core/html/focus.c "
                      "beside focus_element_run, and route this call and §6.12's hide a popover step 20.2 — "
                      "which carries the same crash for the same missing door — through it");
        }
        r->phase = DCR_FOCUS;
    }
    DCHECK(r->phase == DCR_FOCUS, "a close-the-dialog run resumed into a phase §4.11.4 does not have");
    {
        JSContext *docctx = dialog_document_realm(r->subject);

        DCHECK(docctx != NULL,
               "§4.11.4's close the dialog resumed into step 12.3 for a dialog whose node document stopped "
               "being any realm's active document across the focusing request — the disjunct that admitted "
               "this path was evaluated with that realm in hand");
        rc = focus_element_run(docctx, r->prev, &r->focus_phase, STEP_CB(r->cb), in, out_cb, out_argc);
        if (rc > 0) return rc;
        if (rc < 0) return -1;
    }
    JS_FreeValue(ctx, r->prev);
    r->prev = JS_UNDEFINED;
    return dialog_close_tail(ctx, r);
}

enum { M_SHOW = 0, M_SHOW_MODAL };

/* WHERE THE SHARED TAIL STANDS. Three positions, and each is a place the tail can be RE-ENTERED at: its
   entry, its hide-popovers-until request, and its dialog focusing steps. An ordinal over the standard's own
   sub-sequence, which nothing the page does can renumber. */
enum { DSH_TAIL_ENTER = 0, DSH_TAIL_HIDE, DSH_TAIL_FOCUS };

#define DIALOG_SHOW_STAGES(X) \
    X(DSH_ENTER, "HTML §4.11.4 The dialog element's `show()` steps 1-2 or its show a modal dialog steps 1-5: " \
                 "the two early returns and the three or four \"InvalidStateError\" throws, every one of them " \
                 "an attribute read, a slot read, a connectedness walk or a §6.12 popover-visibility slot read") \
    X(DSH_FIRE,  "HTML §4.11.4 The dialog element's `show()` step 3 or its show a modal dialog step 6 (fire an " \
                 "event named beforetoggle, using ToggleEvent, cancelable, with oldState \"closed\" and " \
                 "newState \"open\" — and, for the modal algorithm, source — at the subject)") \
    X(DSH_AFTER, "HTML §4.11.4 The dialog element's `show()` steps 4-6 or its show a modal dialog steps 7-15: " \
                 "the second look at the attribute (and, for the modal algorithm, at connectedness and the " \
                 "popover showing state), the queued dialog toggle event task, the `open` attribute, and the " \
                 "modal algorithm's is modal, its blocked-by-a-modal-dialog assert and its top-layer add") \
    X(DSH_TAIL,  "HTML §4.11.4 The dialog element's `show()` steps 7-11 or its show a modal dialog steps " \
                 "16-20, which are the same five sentences: the previously focused element, the node " \
                 "document, TOPMOST POPOVER ANCESTOR, HIDE POPOVERS UNTIL, and the DIALOG FOCUSING STEPS")
enum { IDL_STEP_STAGE_BASE(DIALOG_SHOW_STAGES) DIALOG_SHOW_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DIALOG_SHOW_STEPS[] = { DIALOG_SHOW_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t  tail;                  /* DSH_TAIL_* — where the shared tail stands */
    uint8_t  fphase;                /* the `beforetoggle` fire request's own phase */
    JSValue  subject;               /* `this`, the brand-checked dialog (owned) */
    JSValue  source;                /* show a modal dialog's `source`, which both members pass as null (owned) */
    JSValue  ev;                    /* the `beforetoggle` event, minted once and held across the fire (owned) */
    JSValue  doc;                   /* step 8 / 17's `document` (owned) */
    JSValue  hide_until_endpoint;   /* step 9 / 18's hideUntil (owned) */
    PopoverHideUntilRun *hiding;    /* step 10 / 19's request — OPAQUE and heap-held, named in the visit */
    DialogFocusingRun   *focusing;  /* step 11 / 20's request — the same shape, and this file owns that one */
    /* THE ONE REQUEST THIS BODY MAKES ITSELF — the `beforetoggle` fire — named by its TYPE rather than by a
       number, which is what core/events/event_target.h asks. The two delegated requests hold their own buffers
       inside their own states and do not draw on this one. */
    EventFireCb cb;
} DialogShowState;

static void dialog_show_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    DialogShowState *s = st;
    int k;

    v->val(ctx, &s->subject);
    v->val(ctx, &s->source);
    v->val(ctx, &s->ev);
    v->val(ctx, &s->doc);
    v->val(ctx, &s->hide_until_endpoint);
    popover_hide_popovers_until_visit(ctx, &s->hiding, v);
    html_dialog_focusing_visit(ctx, &s->focusing, v);
    STEP_CB_FOREACH(s->cb, k)
        v->val(ctx, &s->cb[k]);
}

/* ---- §4.11.4's `show()`, SHOW A MODAL DIALOG, and the DIALOG FOCUSING STEPS -----------------------------------
 *
 * THE THREE ARE ONE MACHINE WITH A MAGIC, for the reason `close()` and `requestClose()` are one below: the two
 * members differ in WHICH algorithm they delegate to and in nothing else, and every step after the fork is
 * literally the same sentence in both. §4.11.4's `showModal()` method steps are ONE sentence — "the showModal()
 * method steps are to show a modal dialog given this and null" — so the member contributes no step of its own.
 *
 * THE TAIL IS WRITTEN ONCE AND THAT IS THE STANDARD'S DOING, NOT A FACTORING. `show()`'s steps 7 through 11 and
 * show a modal dialog's steps 16 through 20 are word for word the same five sentences with `this` and `subject`
 * exchanged: set the previously focused element to the focused element, let document be the node document, let
 * hideUntil be topmost popover ancestor given the subject, null and false, run hide popovers until given
 * document, hideUntil, false and true, and run the dialog focusing steps. A copy at each arm would be five
 * steps two sites had to stay abreast of, which is the shape that drifts.
 *
 * WHAT THIS DIFF EXPORTED FROM core/html/popover.c AND WHY IT IS THE MINIMUM. Steps 9/18 and 10/19 are §6.12's
 * TOPMOST POPOVER ANCESTOR and its HIDE POPOVERS UNTIL, and show a modal dialog's steps 5 and 9 are §6.12's
 * POPOVER SHOWING STATE — three doors. Everything hide popovers until is BUILT from stays static there: its
 * showing hint popover list, its hint stack parent and its hide popover stack until are named by no section
 * outside §6.12, so §6.12's list algebra keeps exactly one reading. Copying that algebra here would not merely
 * duplicate it — a popover left showing over a modal dialog is exactly what those steps prevent, so the copy
 * would be wrong on the screen and not only in the tree.
 *
 * `show()` HAS NO POPOVER-SHOWING-STATE CHECK AND show a modal dialog HAS TWO, which is worth stating because
 * the opposite is easy to assume. §4.11.4 gives `show()` eleven steps and none of them asks; show a modal
 * dialog asks at its step 5, where the answer THROWS an "InvalidStateError" DOMException, and again at its step
 * 9, where it merely RETURNS — because step 6 fired `beforetoggle` at the page in between and a listener may
 * have shown the subject as a popover. The same asymmetry runs through the pair: show a modal dialog re-asks
 * connectedness at step 8 and `show()` never asks it at all.
 *
 * STEP 14 WRITES NOTHING. "Set subject's node document to be blocked by the modal dialog subject" is a
 * statement §6.3.1 DERIVES from css-position-4 §3's top layer — "if subject is the topmost dialog element in
 * document's top layer" — so what performs it is step 15's add, and step 14's site carries the two-sided assert
 * that the add did perform it. A bit here would be the flag that announces an outcome instead of performing
 * one, and it would be a second answer to a question the layer already answers.
 *
 * STEP 12 ASSERTS AND DOES NOT ESTABLISH. "Assert: subject's close watcher is not null" is satisfied by step
 * 11's `open` attribute going through the DOM mutation chokepoint: §4.11.4's attribute change steps run the
 * dialog setup steps, whose step 5 sets the dialog close watcher. That chain is why this file was built from
 * the bottom up and why these two members are the last of the four to land. */

/* §4.11.4's "The DIALOG FOCUSING STEPS, given a dialog element subject", 10 steps. Two of them are REQUESTS —
 * step 1's §6.6.6 allow focus steps FORKS (its second clause is §6.4.1's transient activation, which is unknown
 * external state) and step 6's §6.6.4 focusing steps fire `blur`, `focusout`, `focus` and `focusin` at the
 * page's listeners — so this is a cursor its holder drives rather than a call.
 *
 * IT IS EXPORTED, AND THE CRASH THAT ASKED FOR IT IS DELETED IN THE SAME DIFF. HTML §6.12 The popover
 * attribute's POPOVER FOCUSING STEPS step 2 is "If subject is a dialog element, then run the dialog focusing
 * steps given subject and return", and popover.c carried a `DFAIL` there saying this build had no such
 * algorithm and naming what to build. A crash that goes on saying a capability is absent after it lands is the
 * one stale claim nothing catches, so the crash goes with the absence rather than after it.
 *
 * ITS STEPS 7 TO 10 ARE §6.12's POPOVER FOCUSING STEPS' STEPS 7 TO 10, and core/html/autofocus.h says so at the
 * one door both take: they are §6.6.7's state, so the component that owns the autofocus candidates list and the
 * processed flag writes them, and neither of the two focusing-steps algorithms holds a copy.
 *
 * STEP 4's FOCUS DELEGATE IS §6.6.4's AND IS ASKED OF core/html/focus.h. That search is §6.6.4's one
 * implementation and it already narrows its plain pass to SEQUENTIALLY focusable areas when the focus target is
 * a `dialog`, which is step 6 of the focus delegate's own seven — so the `dialog`-specific half of this step is
 * answered by the algorithm the standard states it in rather than by a condition here. */
struct DialogFocusingRun {
    uint8_t ua_phase;      /* step 1's fork phase */
    uint8_t focus_phase;   /* step 6's request phase */
    JSValue subject;       /* the algorithm's `subject` (owned) */
    JSValue control;       /* steps 2-5's `control`; JS_UNDEFINED is "step 2's null, not yet decided" (owned) */
    JSValue cb[FOCUS_ELEMENT_CB_SLOTS];   /* step 6's request buffer, by the type focus.h declares for it */
};

static void dialog_focusing_elem_visit(JSContext *ctx, void *elem, JSStepVisit *v)
{
    DialogFocusingRun *r = elem;
    int k;

    v->val(ctx, &r->subject);
    v->val(ctx, &r->control);
    STEP_CB_FOREACH(r->cb, k)
        v->val(ctx, &r->cb[k]);
}

void html_dialog_focusing_visit(JSContext *ctx, DialogFocusingRun **slot, JSStepVisit *v)
{
    /* ONE operation rather than a buffer copy plus a loop, because the two consumers need OPPOSITE ORDER — see
       html_dialog_close_visit, which is written this way for the same reason. */
    v->array(ctx, (void **)slot, sizeof(struct DialogFocusingRun), 1, 1, dialog_focusing_elem_visit);
}

void html_dialog_focusing_release(JSContext *ctx, DialogFocusingRun **slot)
{
    DialogFocusingRun *r = *slot;
    int k;

    if (!r) return;
    JS_FreeValue(ctx, r->subject);
    JS_FreeValue(ctx, r->control);
    STEP_CB_FOREACH(r->cb, k) JS_FreeValue(ctx, r->cb[k]);
    js_free(ctx, r);
    *slot = NULL;
}

int html_dialog_focusing_run(JSContext *ctx, JSStepHdr *hdr, DialogFocusingRun **slot, JSValueConst subject,
                             JSValue in, JSValue **out_cb, int *out_argc)
{
    DialogFocusingRun *r = *slot;
    JSContext *docctx;
    lxb_dom_element_t *el;
    bool allow = false;
    int rc;

    if (!r) {
        int k;

        r = js_mallocz(ctx, sizeof *r);
        CHECK(r != NULL, "dialog: OOM allocating §4.11.4's dialog-focusing-steps state");
        *slot = r;
        /* EVERY owned field before anything that can fail — an abandoned run is released through the one
           declaration above, which frees exactly what this state holds, and a js_mallocz'd block is not a block
           of JS_UNDEFINEDs. */
        r->subject = r->control = JS_UNDEFINED;
        STEP_CB_FOREACH(r->cb, k) r->cb[k] = JS_UNDEFINED;
        r->ua_phase = r->focus_phase = 0;
        DCHECK(dialog_elem_of(subject) != NULL,
               "§4.11.4's DIALOG FOCUSING STEPS were given something that is not a `dialog` element — the "
               "standard states them over a dialog element subject, and both of their callers reach them with "
               "one in hand: §4.11.4's own two show algorithms from a brand-checked receiver, and §6.12's "
               "popover focusing steps step 2 from its own local-name test");
        r->subject = JS_DupValue(ctx, subject);
    }
    docctx = dialog_document_realm(r->subject);
    el = dialog_elem_of(r->subject);
    /* THE ONLY CALLER THAT CAN ARRIVE HERE WITH NO REALM IS `show()`, AND THAT IS A CAPABILITY AND NOT AN
       INVARIANT — which is why this is a DFAIL and not the DCHECK its two sibling algorithms carry. Show a
       modal dialog's step 3 THREW for a document that is not fully active and §6.12's show popover step 3
       refused one, so for those two a null realm really would be a broken invariant. §4.11.4 gives `show()`
       eleven steps and NOT ONE of them asks about the node document at all, so `document.implementation
       .createHTMLDocument()`'s `<dialog>` reaches step 11 with a document no navigable holds — ordinary code,
       not an edge. */
    if (docctx == NULL)
        DFAIL("HTML §4.11.4 The dialog element's DIALOG FOCUSING STEPS step 1 runs HTML §6.6.6 Focus management "
              "APIs' ALLOW FOCUS STEPS given the subject's NODE DOCUMENT, and this document is no realm's "
              "active document — which `show()` permits, because none of its eleven steps asks whether the node "
              "document is fully active. §6.6.6 states both clauses over the Document: the first asks whether "
              "target is allowed to use the `focus-without-user-activation` feature and the second whether "
              "target's relevant global object has transient activation. core/html/focus.h's door takes a REALM "
              "because every caller so far had one. BUILD the Document-keyed entry beside "
              "focus_allow_focus_steps_run — the permissions policy is on the Document and the relevant global "
              "object is reachable from it — and route this call through it. ANSWERING false HERE WOULD BE A "
              "GUESS: it is probably the right answer for a document nobody displays, and probably is not the "
              "standard this file implements");
    if (JS_IsUndefined(r->control)) {
        /* `in` is the holder's re-entry value and NOTHING between here and step 6 consumes one: step 1's
           request FORKS rather than calling, and steps 2 through 5 run no page code at all. It is released
           here, once, so the only path that forwards it is the one that owes it to step 6. A re-entry that
           already HAS a control skips this block and hands `in` straight to that request. */
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        /* Step 1 — "If the allow focus steps given subject's node document return false, then return." */
        rc = focus_allow_focus_steps_run(docctx, hdr, &r->ua_phase, &allow);
        if (rc) return rc;
        if (!allow) return 0;
        /* Step 2 is "Let control be null" and is the absence this cursor already holds.
           Step 3 — "If subject has the autofocus attribute, then set control to subject." */
        if (dom_attr_get_ns(el, NULL, "autofocus") != NULL) {
            r->control = JS_DupValue(ctx, r->subject);
        } else {
            r->control = focus_delegate(ctx, r->subject);                                        /* step 4 */
            if (JS_IsNull(r->control)) {
                /* Step 5 — "If control is null, then set control to subject." A `dialog` with no focusable
                   descendant focuses ITSELF, which is what makes step 6's own note ("if control is not
                   focusable, this will do nothing") the ordinary outcome rather than an error. */
                JS_FreeValue(ctx, r->control);
                r->control = JS_DupValue(ctx, r->subject);
            }
        }
    }
    DCHECK(!JS_IsUndefined(r->control),
           "§4.11.4's dialog focusing steps reached step 6 with no control — steps 3, 4 and 5 between them "
           "leave one bound on every arm, step 5 being the one that binds the subject itself");
    /* Step 6 — "Run the focusing steps for control." */
    rc = focus_element_run(docctx, r->control, &r->focus_phase, STEP_CB(r->cb), in, out_cb, out_argc);
    if (rc) return rc;
    /* Steps 7 to 10, through the one door §6.12's popover focusing steps take. THE REALM IS THE CONTROL'S AND
       NOT THE SUBJECT'S: both steps name `control`, and step 4's focus delegate can legitimately answer with an
       element of another document — a `dialog` containing an `iframe` whose content navigable's active document
       holds the delegate — so passing the subject's would answer step 8 about the wrong document. */
    autofocus_focusing_steps_tail(dialog_document_realm(r->control));
    return 0;
}

/* `show()`'s STEP 7 THROUGH 11 AND SHOW A MODAL DIALOG's STEP 16 THROUGH 20, which are the same five sentences.
   Returns the holder's contract: >0 to return, -1 with a throw live, 0 when the five have finished. `in` is
   CONSUMED. */
static int dialog_show_tail(JSContext *ctx, JSStepHdr *hdr, DialogShowState *s, JSValue in,
                            JSValue **out_cb, int *out_argc)
{
    int rc;

    if (s->tail == DSH_TAIL_ENTER) {
        JSValue focused;

        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        /* `show()` step 7 / show a modal dialog step 16 — "Set <subject>'s previously focused element to the
           FOCUSED element." §6.6.2's own term, scoped to the TOP-LEVEL TRAVERSABLE, which is not the same
           question §6.12's show popover step 17 asks ("document's focused area of the document's DOM anchor")
           and is asked of the door core/html/focus.h declares for this sentence. A page that has focused
           nothing has no focused ELEMENT — this engine designates the VIEWPORT, whose DOM anchor is the
           Document — so the field is set to the standard's null, and close the dialog step 12 then restores
           nothing, which is what a browser does. */
        focused = focus_focused_element(ctx);
        html_dialog_set_previously_focused_element(ctx, s->subject, focused);
        JS_FreeValue(ctx, focused);
        /* Step 8 / 17 — "Let document be <subject>'s node document." */
        DCHECK(JS_IsUndefined(s->doc), "a §4.11.4 show run bound its `document` twice — the tail runs once and "
                                       "this is its only writer");
        s->doc = dialog_document_of(ctx, s->subject);
        DCHECK(JS_IsObject(s->doc),
               "§4.11.4's show tail reached a `dialog` with no node document wrapper — every path into it has "
               "already run this element's attribute change steps, which resolve the same document");
        /* Step 9 / 18 — "Let hideUntil be the result of running TOPMOST POPOVER ANCESTOR given <subject>, null,
           and false." FALSE, so §6.12's step 2 arm runs: the subject need not be a popover at all, and that
           algorithm's own note says what the false means — "if the provided element is a top layer element such
           as a dialog which is not showing as a popover, then topmost popover ancestor will only look in the
           node tree to find the first popover". */
        DCHECK(JS_IsUndefined(s->hide_until_endpoint), "a §4.11.4 show run bound `hideUntil` twice");
        s->hide_until_endpoint = popover_topmost_popover_ancestor(ctx, s->subject, JS_NULL, /*isPopover*/ false);
        s->tail = DSH_TAIL_HIDE;
    }
    if (s->tail == DSH_TAIL_HIDE) {
        /* Step 10 / 19 — "Run HIDE POPOVERS UNTIL given document, hideUntil, false, and true." The two booleans
           are focusPreviousElement FALSE and fireEvents TRUE: the popovers this closes must NOT pull the focus
           back, because step 11 is about to place it inside the dialog, and they must fire their `beforetoggle`
           and `toggle` events, which is why this is a request and not a call. */
        rc = popover_hide_popovers_until_run(ctx, &s->hiding, s->doc, s->hide_until_endpoint,
                                            /*focusPreviousElement*/ false, /*fireEvents*/ true,
                                            in, out_cb, out_argc);
        if (rc) return rc;
        in = JS_UNDEFINED;   /* the request CONSUMED it */
        popover_hide_popovers_until_release(ctx, &s->hiding);
        s->tail = DSH_TAIL_FOCUS;
    }
    DCHECK(s->tail == DSH_TAIL_FOCUS, "a §4.11.4 show run resumed into a tail phase this machine does not have");
    /* Step 11 / 20 — "Run THE DIALOG FOCUSING STEPS given <subject>." */
    rc = html_dialog_focusing_run(ctx, hdr, &s->focusing, s->subject, in, out_cb, out_argc);
    if (rc) return rc;
    html_dialog_focusing_release(ctx, &s->focusing);
    return 0;
}

/* THE PROLOGUE OF BOTH ALGORITHMS — `show()`'s steps 1-2 and show a modal dialog's steps 1-5. Returns 1 for
   "this algorithm has returned" and 0 to go on to the fire; a throw is left live and answered by -1. */
static int dialog_show_prologue(JSContext *ctx, DialogShowState *s, int magic)
{
    lxb_dom_element_t *el = dialog_elem_of(s->subject);
    JSContext *docctx;

    if (magic == M_SHOW) {
        /* `show()` step 1 — "If this has an open attribute and is modal of this is FALSE, then return." The
           polarity is the mirror of show a modal dialog's step 1 and it is the whole of what makes `show()` on
           an already-modal dialog a THROW rather than a no-op: a modal dialog falls through to step 2. */
        if (dialog_has_open(el) && !ds_flag(ctx, s->subject, DS_IS_MODAL)) return 1;
        /* Step 2. */
        if (dialog_has_open(el)) {
            JS_ThrowDOMException(ctx, "InvalidStateError",
                                 "show() was called on a dialog that is already showing as a modal dialog");
            return -1;
        }
        /* `show()` HAS NO FURTHER GUARD, which is not an omission of this file's: §4.11.4 gives it eleven steps
           and none of them asks about the node document being fully active, about connectedness, or about the
           popover showing state. All three are show a modal dialog's, and every one of them is there because a
           MODAL dialog takes the top layer and the focus. */
        return 0;
    }
    DCHECK(magic == M_SHOW_MODAL, "a §4.11.4 show machine ran with a magic this file has no algorithm for");
    /* Show a modal dialog step 1 — "If subject has an open attribute and is modal of subject is TRUE, then
       return." */
    if (dialog_has_open(el) && ds_flag(ctx, s->subject, DS_IS_MODAL)) return 1;
    if (dialog_has_open(el)) {                                                                   /* step 2 */
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "showModal() was called on a dialog that is already showing non-modally");
        return -1;
    }
    /* Step 3 — "If subject's node document is not fully active, then throw." A Document no navigable holds is
       not fully active, so a NULL realm IS that answer rather than a missing pointer. */
    docctx = dialog_document_realm(s->subject);
    if (docctx == NULL || !document_fully_active(docctx)) {
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "showModal() was called on a dialog whose node document is not fully active");
        return -1;
    }
    if (!node_is_connected(node_of(s->subject))) {                                               /* step 4 */
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "showModal() was called on a dialog that is not connected");
        return -1;
    }
    /* Step 5 — "If subject is in the POPOVER SHOWING STATE, then throw." §6.12's own state, asked of the one
       door core/html/popover.h exports for it. Step 9 asks the same question again and merely RETURNS, because
       step 6 fires `beforetoggle` at the page in between. */
    if (popover_element_is_showing(ctx, s->subject)) {
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "showModal() was called on a dialog that is already showing as a popover");
        return -1;
    }
    return 0;
}

/* SHOW A MODAL DIALOG'S STEPS 12 THROUGH 15 — the four the modal algorithm has and `show()` does not. */
static void dialog_show_modal_steps_12_to_15(JSContext *ctx, DialogShowState *s)
{
    /* Step 12 — "Assert: subject's close watcher is not null." IT ASSERTS AND DOES NOT ESTABLISH: step 11's
       `open` attribute went through the DOM mutation chokepoint, so §4.11.4's attribute change steps ran the
       dialog setup steps, whose step 5 SET the dialog close watcher. A null here means that chain did not run
       for this element, which is the same absence request to close the dialog's own step 3 names. */
    DCHECK(!ds_is_null(ctx, s->subject, DS_CLOSE_WATCHER),
           "§4.11.4's show a modal dialog step 12 asserts the dialog's close watcher is not null, and this one "
           "has none. Step 11 added the `open` attribute one step ago, which reaches §4.11.4's attribute change "
           "steps through core/dom/element.c's mutation chokepoint and runs the dialog setup steps, whose step "
           "5 is \"Set the dialog close watcher with subject\". A null here means the write did not reach that "
           "hook — the attribute was placed past the chokepoint — so a close request would not close this "
           "modal dialog and Esc would do nothing");
    ds_set(ctx, s->subject, DS_IS_MODAL, JS_TRUE);                                              /* step 13 */
    /* Step 14 — "Set subject's node document to be blocked by the modal dialog subject." IT WRITES NOTHING,
       AND THE ASSERT BELOW IS WHY THAT IS THE IMPLEMENTATION RATHER THAN A GAP. §6.3.1 DERIVES the predicate —
       "A Document document is blocked by a modal dialog subject if subject is the topmost dialog element in
       document's top layer" — so what makes the sentence TRUE is step 15's add, and a bit set here would be a
       second answer to a question css-position-4 §3's layer already answers. §4.11.4's own note beside this
       step describes the CONSEQUENCE — "this will cause the focused area of the document to become inert" —
       rather than a field, and that consequence is core/html/focus.c's inert walk, which asks §6.3.1's
       predicate directly. */
    /* Step 15 — "If subject's node document's top layer does not already CONTAIN subject, then add an element
       to the top layer given subject." THE RAW SET, which is the term §4.11.4's own markup links to here
       (css-position-4's `top layer`, with Infra's `contain`) and NOT §3.3's "is in the top layer" — the two
       differ for a dialog whose removal is pending, which `d.close(); d.showModal();` in one task produces.
       core/css/top_layer.h states that difference at the door. */
    if (!top_layer_set_contains(ctx, s->subject)) top_layer_add(ctx, s->subject);
    /* THE TWO-SIDED HALF OF STEP 14, STANDING AFTER THE STEP THAT PERFORMS IT — AND IT ASSERTS THAT THE
       DOCUMENT IS BLOCKED, NEVER THAT IT IS BLOCKED BY THIS SUBJECT. The stronger assert was written here
       first and it is wrong, because the standard's own two sentences can disagree and the sequence that
       makes them is one a page writes: `a.showModal(); b.showModal(); a.close(); a.showModal();` in ONE task
       leaves `a` CONTAINED in the layer with its removal pending, so step 15 SKIPS the add, `b` is still the
       last member, and §6.3.1 derives that the document is blocked by `b` while step 14 says `a`. That is a
       disagreement in the text, not a defect this file can assert away — and an assert that fires on
       legitimate page input is a guard on an EXPECTATION rather than on an invariant.
       WHAT IS GUARANTEED after step 15 is that the subject is a `dialog` contained in the layer, so the
       topmost `dialog` in it is SOME element rather than null — which is what makes step 14's sentence true at
       all, and which is a fact about the set this file just wrote. It is also
       the one reader §6.3.1's predicate has inside this component, so the derivation is exercised on every
       modal show rather than only from core/html/focus.c's inert walk. */
    {
        JSValue doc = dialog_document_of(ctx, s->subject);
        JSValue blocking = html_dialog_blocked_by_modal_dialog(ctx, doc);
        bool blocked = !JS_IsNull(blocking);

        JS_FreeValue(ctx, blocking);
        JS_FreeValue(ctx, doc);
        DCHECK(blocked,
               "§4.11.4's show a modal dialog step 14 says the subject's node document is now BLOCKED BY A "
               "MODAL DIALOG, and §6.3.1 derives that from the top layer — \"A Document document is blocked by "
               "a modal dialog subject if subject is the topmost dialog element in document's top layer\" — so "
               "step 15's add is what performs it. The derivation answers NULL, which means the layer holds no "
               "`dialog` at all one step after this one put the subject in it: either step 15's add did not "
               "append, or the topmost walk is reading a different document than the one the add wrote");
    }
}

static int dialog_show_body(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    DialogShowState *s = state;
    int magic = idl_step_magic(hdr);
    lxb_dom_element_t *el;
    bool not_canceled = true;
    int rc;

    (void)argc;
    (void)argv;
    if (hdr->stage == DSH_ENTER) {
        int k;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY owned field before the first thing that can fail — the teardown discharges exactly what the
           declaration names, so a field handed over late is a field nothing releases. */
        s->subject = s->source = s->ev = s->doc = s->hide_until_endpoint = JS_UNDEFINED;
        s->hiding = NULL;
        s->focusing = NULL;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        s->tail = DSH_TAIL_ENTER;
        s->fphase = 0;
        /* Web IDL §3.7.7 Operations' BRAND CHECK, a THROW rather than an assert for the reason this file's two
           accessors state: a page reaches a method off the prototype with `.call` on anything at all, so the
           receiver is the PAGE's input and never a value this codebase computed. */
        if (!dialog_elem_of(hdr->this_val)) {
            JS_ThrowTypeError(ctx, "show()/showModal() was called on something that is not a <dialog> element");
            return JS_STEP_ABRUPT;
        }
        s->subject = JS_DupValue(ctx, hdr->this_val);
        /* `showModal()`'s one step is "show a modal dialog given this and NULL", and `show()` states no source
           at all — its step 5 queues the toggle task with null and its step 3's fire names no source
           initializer. So the source is the standard's null for both members, and it is a FIELD rather than a
           constant because show a modal dialog takes it as an argument and §4.11.5 Dialog light dismiss is the
           caller that will one day pass an element. */
        s->source = JS_NULL;
        rc = dialog_show_prologue(ctx, s, magic);
        if (rc < 0) return JS_STEP_ABRUPT;
        if (rc > 0) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
        STEP_GOTO(hdr->stage, DSH_FIRE, &s->fphase, NULL);
    }
    el = dialog_elem_of(s->subject);
    if (hdr->stage == DSH_FIRE) {
        if (JS_IsUndefined(s->ev)) {
            /* `show()` step 3 / show a modal dialog step 6. CANCELABLE — this is the one `beforetoggle` in
               §4.11.4 that is, which is why steps 4 and 7 exist: a handler that calls `preventDefault()` makes
               the fire answer false and the algorithm returns.
               THE `source` INITIALIZER IS NAMED BY SHOW A MODAL DIALOG AND NOT BY `show()`, AND THE TWO REACH
               THE SAME VALUE BY DIFFERENT ROUTES, which is why one argument serves both: the modal algorithm
               initializes it to its own `source`, which both members pass as null, and `show()` names no
               initializer at all, so ToggleEventInit's declared default stands — `Element? source = null`.
               Passing this field is therefore the standard's answer on both arms, and it is what carries the
               difference the day §4.11.5 Dialog light dismiss supplies an element. */
            s->ev = toggle_event_new(ctx, "beforetoggle", DIALOG_STATE_CLOSED, DIALOG_STATE_OPEN, s->source,
                                     /*bubbles*/ false, /*cancelable*/ true);
            if (JS_IsException(s->ev)) {
                s->ev = JS_UNDEFINED;
                return JS_STEP_ABRUPT;
            }
        }
        rc = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), s->subject, s->ev, JS_UNDEFINED, cb_result,
                                   &not_canceled, out_cb, out_argc);
        if (rc > 0) return rc;
        if (rc < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;   /* the fire request TOOK it */
        JS_FreeValue(ctx, s->ev);
        s->ev = JS_UNDEFINED;
        /* "If the result of firing an event … is FALSE, then return." */
        if (!not_canceled) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
        STEP_GOTO(hdr->stage, DSH_AFTER, &s->fphase, NULL);
    }
    if (hdr->stage == DSH_AFTER) {
        /* NOTHING IN THIS STAGE ASKS ANYTHING, so a completion arriving here is spent — and it is released
           HERE rather than at the door, because the stage below DOES ask and its re-entry carries an answer an
           unconditional free would have eaten. */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* `show()` step 4 / show a modal dialog step 7 — the attribute is read AGAIN, because a `beforetoggle`
           handler had the element in its hands. */
        if (!dialog_has_open(el)) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
        if (magic == M_SHOW_MODAL) {
            /* Steps 8 and 9 — the second look at connectedness and at the popover showing state, both of
               which a `beforetoggle` listener can have changed, and both of which merely RETURN here where
               steps 4 and 5 threw. `show()` has neither. */
            if (!node_is_connected(node_of(s->subject))) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
            if (popover_element_is_showing(ctx, s->subject)) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
        }
        /* `show()` step 5 / show a modal dialog step 10 — "Queue a dialog toggle event task given <subject>,
           'closed', 'open', and <source>." */
        dialog_queue_toggle_task(ctx, s->subject, DIALOG_STATE_CLOSED, DIALOG_STATE_OPEN, s->source);
        /* `show()` step 6 / show a modal dialog step 11 — "Add an open attribute to <subject>, whose value is
           the empty string." IT IS FAR MORE THAN AN ATTRIBUTE WRITE: it goes through the DOM mutation
           chokepoint, so §4.11.4's own attribute change steps run and their step 6 runs the DIALOG SETUP
           STEPS, which append the element to its document's open dialogs list and SET THE DIALOG CLOSE
           WATCHER. That is what step 12 below asserts and what makes an Esc close this dialog. */
        dom_cow_set_attribute(el, "open", "", 0, JS_UNDEFINED);
        if (magic == M_SHOW_MODAL) dialog_show_modal_steps_12_to_15(ctx, s);
        STEP_GOTO(hdr->stage, DSH_TAIL, &s->fphase, NULL);
    }
    DCHECK(hdr->stage == DSH_TAIL, "a §4.11.4 show machine resumed at a stage it does not have");
    rc = dialog_show_tail(ctx, hdr, s, cb_result, out_cb, out_argc);
    if (rc > 0) return rc;
    if (rc < 0) return JS_STEP_ABRUPT;
    *presult = JS_UNDEFINED;
    return JS_STEP_DONE;
}

/* WHAT THIS MACHINE TOOK AND MUST GIVE BACK ON EVERY EXIT. The hide-popovers-until state is a REFERENCE the
   visit above names, so the teardown that follows this hook releases it; what belongs here is anything this
   machine LATCHED, and it has latched nothing — `show()` and show a modal dialog raise no flag that a later
   step owes back, which is the whole difference from `requestClose()`'s enable-close-watcher latch. It is
   declared rather than left NULL so the declaration reads as a decision instead of an omission. */
static void dialog_show_release(JSContext *ctx, void *st)
{
    (void)ctx;
    (void)st;
}

/* THE TAIL IS NAMED, for the reason the close()/requestClose() declaration states: a POSITIONAL initializer
   that runs to the end of the struct silently re-aims every value after the next field added.
   `catches_abrupt` is 0 because neither algorithm catches: an abrupt from the fire or from HIDE POPOVERS UNTIL
   is the page's own `beforetoggle` handler throwing, and §4.11.4 gives these members no step that swallows it.
   `unforkable` is NULL because this machine holds only JSValues, its own cursors and the hide-popovers-until
   state, every one of which the visit above names. */
static const IdlStepDecl DIALOG_SHOW_DECL = {
    dialog_show_body, sizeof(DialogShowState), dialog_show_visit, dialog_show_release,
    "HTML §4.11.4 The dialog element's show() and showModal()", DIALOG_SHOW_STEPS,
    .catches_abrupt = 0, .unforkable = NULL
};

/* ---- §4.11.4's REQUEST TO CLOSE THE DIALOG, AND THE TWO MEMBERS OVER IT ---------------------------------------
 *
 * `close()` and `requestClose()` are ONE machine with a magic, for core/html/close_watcher_interface.c's
 * reason: the two differ in WHICH algorithm they delegate to and in nothing else, and a second implementation
 * of the receiver resolution and the "if returnValue is not given" step is a second place for them to disagree.
 */

enum { M_CLOSE = 0, M_REQUEST_CLOSE };

#define DIALOG_MEMBER_STAGES(X) \
    X(DM_ENTER, "HTML §4.11.4 The dialog element's close()/requestClose() prologue: Web IDL §3.7.7 " \
                "Operations' brand check on the receiver and step 1's \"if returnValue is not given, then set " \
                "it to null\" — one local-name comparison and one §3.6 given-ness question, neither of which " \
                "runs the page's code") \
    X(DM_RUN,   "HTML §4.11.4 The dialog element's close() step 2 (\"close the dialog this with returnValue " \
                "and null\") or requestClose() step 2 (\"request to close the dialog this with returnValue " \
                "and null\") — one stage for either, because each delegate parks at its own rest points and " \
                "resumes at this one call site with this stage unmoved")
enum { IDL_STEP_STAGE_BASE(DIALOG_MEMBER_STAGES) DIALOG_MEMBER_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DIALOG_MEMBER_STEPS[] = { DIALOG_MEMBER_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSValue         subject;    /* `this`, the brand-checked dialog, held across the suspension (owned) */
    JSValue         retval;     /* step 1's returnValue: JS_NULL for "not given" (owned) */
    JSValue         watcher;    /* request to close the dialog step 7's close watcher (owned) */
    DialogCloseRun *closing;    /* close()'s delegate — OPAQUE and heap-held, named in the visit below */
    CloseWatcherRun run;        /* requestClose()'s §6.10.2 request, whose cursors and event this holds */
    /* REQUEST TO CLOSE THE DIALOG STEP 4'S LATCH, WHICH ITS STEP 8 OWES BACK. It is a FLAG and not a
       reference, so no declaration can name it: a flow abandoned inside the page's own `cancel` handler must
       not leave this dialog's enable close watcher for request close TRUE for the rest of the session, which
       would make its watcher's getEnabledState answer true for every later close request regardless of the
       closedby attribute. Same shape and same reason as close_watcher.h's own `running`. */
    uint8_t         enabled_set;
    uint8_t         rphase;     /* whether step 7's request has been entered — DM_RUN's own cursor */
} DialogMemberState;

static void dialog_member_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    DialogMemberState *s = st;

    v->val(ctx, &s->subject);
    v->val(ctx, &s->retval);
    v->val(ctx, &s->watcher);
    html_dialog_close_visit(ctx, &s->closing, v);
    close_watcher_run_visit(ctx, &s->run, v);
}

/* WHAT THIS MEMBER TOOK AND MUST GIVE BACK ON EVERY EXIT. Two latches, and neither is a reference — so no
   declaration can name either, and both READ state this hook still holds, which is why they run here and
   before the declaration is discharged. `closing` and the run's own JSValues are NOT freed here: they are
   references, the visit above names them, and the teardown that follows this hook is what releases them. */
static void dialog_member_release(JSContext *ctx, void *st)
{
    DialogMemberState *s = st;

    if (s->enabled_set && JS_IsObject(s->subject)) {
        ds_clear(ctx, s->subject, DS_ENABLE_CW_REQ_CLOSE);                  /* step 8, for an abandoned run */
        s->enabled_set = 0;
    }
    close_watcher_run_unlock(ctx, &s->run);
}

/* §4.11.4's "To request to close dialog element subject, given null or a string returnValue and null or an
   element source", 8 steps. Its step 7 is a REQUEST, so the whole algorithm is one; it lives inside this
   machine rather than beside it because §4.11.4 gives it exactly one caller. */
static int dialog_request_to_close_run(JSContext *ctx, JSStepHdr *hdr, DialogMemberState *s, JSValue in,
                                       JSValue **out_cb, int *out_argc)
{
    lxb_dom_element_t *el = dialog_elem_of(s->subject);
    JSContext *wctx;
    bool proceed = true;
    int rc;

    if (!s->rphase) {
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        if (!dialog_has_open(el)) return 0;                                                   /* step 1 */
        wctx = dialog_document_realm(s->subject);                                             /* step 2 */
        if (!node_is_connected(node_of(s->subject)) || wctx == NULL || !document_fully_active(wctx))
            return 0;
        s->watcher = ds_get(ctx, s->subject, DS_CLOSE_WATCHER);
        DCHECK(JS_IsObject(s->watcher),                                                       /* step 3 */
               "§4.11.4's request to close the dialog step 3 asserts the dialog's close watcher is not null, "
               "and this open, connected dialog in a fully active document has none. THREE ENTRIES ESTABLISH "
               "one and between them they are meant to cover every edge that makes a dialog open AND "
               "connected: the `open` attribute's change steps (script adds the attribute to a connected "
               "element), the dialog HTML element insertion steps (an already-open element is inserted), and "
               "html_dialog_parsed's walk (a load's parse, which reaches neither hook because HTML §13.2.6 "
               "Tree construction writes past DOM §4.2.3's mutation algorithms). A null here names a FOURTH "
               "way for markup to become connected that runs none of the three — build the walk for that load "
               "path beside html_dialog_parsed rather than testing here, because §4.11.4 asserts at this step "
               "and a test would hide the missing hook from every other caller of the setup steps");
        ds_set(ctx, s->subject, DS_ENABLE_CW_REQ_CLOSE, JS_TRUE);                             /* step 4 */
        s->enabled_set = 1;
        /* Steps 5 and 6 — the two fields §6.10.2's close action for this kind reads when it RUNS, which is
           why they are written here and not handed to the establish: the watcher was established when the
           `open` attribute arrived, and these are this request's own arguments. */
        if (JS_IsNull(s->retval)) ds_clear(ctx, s->subject, DS_REQ_CLOSE_RETURN);             /* step 5 */
        else                      ds_set(ctx, s->subject, DS_REQ_CLOSE_RETURN,
                                         JS_DupValue(ctx, s->retval));
        ds_clear(ctx, s->subject, DS_REQ_CLOSE_SOURCE);        /* step 6: the member passes a null source */
        s->rphase = 1;
    }
    DCHECK(JS_IsObject(s->watcher), "§4.11.4's request to close the dialog resumed into step 7 with no close "
                                    "watcher — step 3 asserts one and step 7 is the only thing past it");
    wctx = dialog_document_realm(s->subject);
    DCHECK(wctx != NULL,
           "§4.11.4's request to close the dialog resumed into step 7 for a dialog whose node document stopped "
           "being any realm's active document across the request — §6.10.2's algorithms are stated over the "
           "watcher's Window and step 2 has already certified that document fully active");
    /* Step 7 — "Request to close subject's close watcher with false." FALSE is the whole of the difference
       from the close-request path: §6.10.2's step 6 then makes canPreventClose true without asking §6.4.1
       anything, so a page's own `requestClose()` always hands its `cancel` handler a cancelable event. The
       boolean the algorithm answers is DISCARDED — the member returns `undefined` and the page learns what
       happened from its two handlers. */
    rc = close_watcher_request_to_close_run(wctx, hdr, &s->run, s->watcher, /*requireHistory*/ false,
                                            in, &proceed, out_cb, out_argc);
    if (rc) return rc;
    /* Step 8 — and it runs on BOTH of step 7's outcomes, which is why it is here rather than inside the arm
       that closed: a cancelled request leaves the dialog open and must still put the flag back. */
    ds_clear(ctx, s->subject, DS_ENABLE_CW_REQ_CLOSE);
    s->enabled_set = 0;
    return 0;
}

static int dialog_member_body(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                              JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    DialogMemberState *s = state;
    int magic = idl_step_magic(hdr);
    int rc;

    if (hdr->stage == DM_ENTER) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY owned field before the first thing that can fail — the teardown discharges exactly what the
           declaration names, so a field handed over late is a field nothing releases. */
        s->subject = s->retval = s->watcher = JS_UNDEFINED;
        s->closing = NULL;
        close_watcher_run_init(&s->run);
        s->enabled_set = 0;
        s->rphase = 0;
        /* Web IDL §3.7.7 Operations' BRAND CHECK, and it is a THROW rather than an assert for the reason this
           file's two accessors state: a page reaches a method off the prototype with `.call` on anything at
           all, so the receiver is the PAGE's input and never a value this codebase computed. */
        if (!dialog_elem_of(hdr->this_val)) {
            JS_ThrowTypeError(ctx, "close()/requestClose() was called on something that is not a <dialog> "
                                   "element");
            return JS_STEP_ABRUPT;
        }
        s->subject = JS_DupValue(ctx, hdr->this_val);
        /* Step 1 — "If returnValue is not given, then set it to null." §3.6's own question, which is about
           WHICH KIND of entry stands at the position and never about how far the page reached; the declaration
           makes position 0 optional with no default, so an absent argument and an explicit `undefined` are the
           same "missing" and both become the standard's null. */
        s->retval = idl_arg_given(argc, argv, 0) ? JS_DupValue(ctx, argv[0]) : JS_NULL;
        DCHECK(JS_IsNull(s->retval) || JS_IsString(s->retval),
               "§4.11.4's close()/requestClose() reached step 2 with a returnValue that is neither null nor a "
               "string — the declaration types position 0 `optional DOMString`, so the conversion has already "
               "run the page's own toString and placed a string");
        STEP_GOTO(hdr->stage, DM_RUN, NULL);
    }
    DCHECK(hdr->stage == DM_RUN, "a §4.11.4 member resumed at a stage this machine does not have");
    if (magic == M_CLOSE) {
        /* Step 2 — "Close the dialog this with returnValue and null." The SOURCE is null because a member
           invocation is not an element-sourced close the way §4.11.5's light dismiss is. */
        rc = html_dialog_close_run(ctx, &s->closing, s->subject, s->retval, JS_NULL, cb_result,
                                   out_cb, out_argc);
        if (rc > 0) return rc;
        if (rc < 0) return JS_STEP_ABRUPT;
        html_dialog_close_release(ctx, &s->closing);
    } else {
        DCHECK(magic == M_REQUEST_CLOSE, "a §4.11.4 member ran with a magic this file has no member for");
        rc = dialog_request_to_close_run(ctx, hdr, s, cb_result, out_cb, out_argc);
        if (rc > 0) return rc;
        if (rc < 0) return JS_STEP_ABRUPT;
    }
    *presult = JS_UNDEFINED;
    return JS_STEP_DONE;
}

/* THE TAIL IS NAMED, for the reason core/html/close_watcher_interface.c's dictionary row states: a POSITIONAL
   initializer that runs to the end of the struct silently re-aims every value after the next field added, and
   IdlStepDecl has gained two since the first member declared one. `catches_abrupt` is 0 because neither
   delegate catches: an abrupt from `close the dialog` or from §6.10.2's request is the page's own `beforetoggle`
   or `cancel` handler throwing, and §4.11.4 gives the member no step that swallows it. `unforkable` is NULL
   because both delegates hold only JSValues and their own cursors, every one of which the visit above names. */
static const IdlStepDecl DIALOG_MEMBER_DECL = {
    dialog_member_body, sizeof(DialogMemberState), dialog_member_visit, dialog_member_release,
    "HTML §4.11.4 The dialog element's close() and requestClose()", DIALOG_MEMBER_STEPS,
    .catches_abrupt = 0, .unforkable = NULL
};

/* ---- declare / install ---------------------------------------------------------------------------------------- */

void html_dialog_declare(JSContext *ctx)
{
    static const IdlArgType CLOSE_ARGS[1] = { IDL_DOMSTRING };
    int i;

    DCHECK(g_id_return_value < 0, "html_dialog_declare ran twice — one instance is one agent");
    for (i = 0; i < DS_SLOT_N; i++) {
        g_slot_key[i] = JS_NewSymbol(ctx, DS_SLOT_NAME[i], false);
        CHECK(!JS_IsException(g_slot_key[i]), "a §4.11.4 dialog state slot key allocation failed");
        g_slot_atom[i] = JS_ValueToAtom(ctx, g_slot_key[i]);
        CHECK(g_slot_atom[i] != JS_ATOM_NULL, "a §4.11.4 dialog state slot key could not be interned");
    }
    /* HTML §6.5.1 The ToggleEvent interface's ToggleEvent, which both of §4.11.4's fires use — declared from
       here because this file is the only thing in the build that mints one, and §4.11.4 is where the standard
       first reaches it.
       THE NUMBER STOOD HERE AS §4.11.5 AND WAS MIS-AIMED: §4.11.5 is "Dialog light dismiss", and the eight
       sites in the component that OWNS this interface (core/html/toggle_event.c and its header) all cite
       §6.5.1. citegen is blind to this by construction — it asks whether a quotation's words occur in the
       cited section, never whether that section GOVERNS the code beneath it — so the outlier was found by
       diffing the siblings, which is the check CLAUDE.md's §N-SITES-QUOTING-ONE-SENTENCE prescribes and the
       only one that could have found it. */
    toggle_event_init(ctx);
    g_id_return_value = idl_setter_id(ctx, IDL_DOMSTRING, false, js_dialog_set_return_value, 0);
    /* `[CEReactions, ReflectSetter] attribute DOMString closedBy` — no `[LegacyNullToEmptyString]`, so a page
       assigning null gets Web IDL §3.2.10 DOMString's ordinary ToString and the attribute reads "null", which
       is what `null_to_empty` being false means here — Web IDL §3.4.6 [LegacyNullToEmptyString] is the
       extended attribute that would make it the empty string instead, and this member does not carry it. */
    g_id_closed_by = idl_setter_id(ctx, IDL_DOMSTRING, false, js_dialog_set_closed_by, 0);
    /* `[CEReactions] undefined close(optional DOMString returnValue)` and its twin. ONE declaration each, both
       naming the same machine with a magic, and `idl_optional_from(0)` is what makes `d.close()` a legal
       zero-argument call rather than Web IDL §3.6's arity TypeError — and what makes §3.6 step 15.4.2 place
       "missing" for the `undefined` a page passes, which is step 1's own question. */
    g_id_close = idl_method_id_step(ctx, CLOSE_ARGS, 1, NULL, 0, &DIALOG_MEMBER_DECL, M_CLOSE);
    idl_optional_from(0);
    g_id_request_close = idl_method_id_step(ctx, CLOSE_ARGS, 1, NULL, 0, &DIALOG_MEMBER_DECL,
                                            M_REQUEST_CLOSE);
    idl_optional_from(0);
    /* `[CEReactions] undefined show()` and `[CEReactions] undefined showModal()` — ZERO arguments each, so
       neither takes an argument-type list, and both name the ONE machine with a magic for the reason the pair
       above do: §4.11.4's `showModal()` method steps are a single sentence delegating to show a modal dialog,
       and every step after the two algorithms' prologues is the same sentence in both. */
    g_id_show = idl_method_id_step(ctx, NULL, 0, NULL, 0, &DIALOG_SHOW_DECL, M_SHOW);
    g_id_show_modal = idl_method_id_step(ctx, NULL, 0, NULL, 0, &DIALOG_SHOW_DECL, M_SHOW_MODAL);
    /* §4.9's ATTRIBUTE CHANGE STEPS and §4.2.3's REMOVING STEPS are registered by core/dom/element.c, which
       owns both lists and the ORDER within them; this file supplies the two entry points and says so at their
       declarations in html_dialog.h. Registering them from here would put §4.11.4 in a list whose ordering
       argument lives in another file. */
}

void html_dialog_install(JSContext *ctx, JSValueConst dialog_proto)
{
    DCHECK(g_id_return_value >= 0, "§4.11.4's members were installed before they were declared");
    DCHECK(g_id_closed_by >= 0, "§4.11.4's `closedBy` was installed before html_dialog_declare minted its "
                                "setter id");
    DCHECK(JS_IsObject(dialog_proto), "the dialog members were installed with no HTMLDialogElement.prototype");
    /* IN §4.11.4's OWN IDL ORDER — `returnValue` then `closedBy` — because that is the order the interface
       declares them in and there is no other order to prefer. */
    DCHECK(g_id_close >= 0 && g_id_request_close >= 0,
           "§4.11.4's close()/requestClose() were installed before html_dialog_declare took their pool ids");
    DCHECK(g_id_show >= 0 && g_id_show_modal >= 0,
           "§4.11.4's show()/showModal() were installed before html_dialog_declare took their pool ids");
    idl_install_accessor(ctx, dialog_proto, "returnValue", js_dialog_get_return_value, 0, g_id_return_value);
    idl_install_accessor(ctx, dialog_proto, "closedBy", js_dialog_get_closed_by, 0, g_id_closed_by);
    idl_install_method(ctx, dialog_proto, "show", g_id_show);
    idl_install_method(ctx, dialog_proto, "showModal", g_id_show_modal);
    idl_install_method(ctx, dialog_proto, "close", g_id_close);
    idl_install_method(ctx, dialog_proto, "requestClose", g_id_request_close);
}

void html_dialog_free(JSRuntime *rt)
{
    toggle_event_free(rt);
    /* The slot keys are the AGENT's, so they are released with the agent — a Symbol nobody frees is a live GC
       object the runtime's own walk counts as a leak. */
    int i;

    for (i = 0; i < DS_SLOT_N; i++) {
        JS_FreeAtomRT(rt, g_slot_atom[i]);
        g_slot_atom[i] = JS_ATOM_NULL;
        JS_FreeValueRT(rt, g_slot_key[i]);
        g_slot_key[i] = JS_UNDEFINED;
    }
    g_toggle_stepid = -1;
    g_id_return_value = -1;
    g_id_closed_by = -1;
    g_id_close = -1;
    g_id_request_close = -1;
    g_id_show = -1;
    g_id_show_modal = -1;
}
