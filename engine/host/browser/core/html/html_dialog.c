/* THE dialog ELEMENT — HTML §4.11.4.
 *
 * WHAT IS HERE AND WHY IT IS ONLY THIS. HTMLDialogElement's IDL declares seven members; this file builds the
 * three the platform's own algorithms reach through, and nothing shaped like the rest:
 *
 *   - §4.11.4's CLOSE THE DIALOG, because §4.10.22.3 step 11.6 performs it. A `method=dialog` form submission
 *     makes NO REQUEST — it closes the dialog its form sits in, sets the dialog's return value from the
 *     submitter, and fires `close`. That is the whole of that row of step 26's table, and it is why this file
 *     exists at the moment it does.
 *   - `returnValue`, because step 9 of that algorithm WRITES it and the page reads it back: "the returnValue
 *     IDL attribute, on getting, must return the last value to which it was set. On setting, it must be set to
 *     the new value. When the element is created, it must be set to the empty string."
 *   - `closedBy` and the COMPUTED CLOSED-BY STATE behind it, because that state is what §4.11.4's set the
 *     dialog close watcher hands to §6.10.2's establish as the third of its three algorithms. It is the
 *     deepest thing under show a modal dialog's step 12 that nothing in this build supplied; see the section
 *     that builds it for why it lands before the four methods rather than after them.
 *
 * A NAMED RESIDUAL, because the code below is CORRECT for what it does and NARROWER than §4.11.4.
 *   NOT COVERED: no close watcher is established from a dialog yet, so §6.10.2's manager still has no dialog
 *     establisher. Between `closedBy` and that stand four things this build does not have — the DIALOG entry
 *     in core/html/close_watcher.h's `CloseWatcherKind` and its three algorithms; the per-Document OPEN
 *     DIALOGS LIST; the dialog setup steps and cleanup steps; and the `open` attribute change steps that run
 *     them. The IS MODAL boolean has no storage either, which is a `showModal()` obligation and is asserted at
 *     the one step that reads it rather than described here.
 *   THE NEXT DIFF: set the dialog close watcher, 3 steps, whose step 3 calls `close_watcher_establish` with a
 *     new appended `CloseWatcherKind` — that header already names §4.11.4 as the entry that appends next, and
 *     already quotes this dialog's getEnabledState as the reason its dispatch asks the kind instead of
 *     answering `true` from one place. Its closeAction is close the dialog, which this file already builds as
 *     `html_dialog_close_run`; its cancelAction fires `cancel`. That entry is APPENDED and never inserted, for
 *     the reason that header states: a parked watcher resumes holding its kind number.
 *   HOW ITS ABSENCE SHOWS: `document.createElement("dialog").closedBy` answers "none" and a page's Esc closes
 *     nothing, because with no watcher there is nothing for §6.10.1's close request to reach — and every
 *     Window's close watcher manager stays permanently empty, which is the state close_watcher_interface.c's
 *     own header records as the reason §6.10.2's arithmetic had never run.
 *
 * `show()`, `showModal()`, `close()` and `requestClose()` are ABSENT, honestly — a page calling one
 * gets its own TypeError, which is this engine's forcing function, rather than a shape-only method. Their
 * absence is also LOAD-BEARING here rather than a footnote, and it is asserted rather than assumed: three of
 * close-the-dialog's steps read state that only those members can produce (the is-modal flag, the previously
 * focused element, the request-close return value), and each of those steps is guarded by a `realm_awaits`
 * naming the member whose arrival makes the step real. The day one of them lands, the assert fires AT the step
 * that must then be written.
 *
 * THE STATE IS PER-FLOW BY CONSTRUCTION. Both of this element's own facts — its return value and its pending
 * toggle task — live in own slots on the element's wrapper under Symbols this file minted and never published.
 * A slot written as a property write is captured by the COW delta, so two forked arms that close the same
 * dialog with two different submitters each read back their own `returnValue`, and a parked flow resumes with
 * the one it had. The `open` attribute is the DOM's and time-travels through dom_cow for the same reason. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/attr_list.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/html/enumerated_attribute.h"
#include "core/html/html_dialog.h"
#include "core/html/toggle_event.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/dom_cow.h"

/* §4.11.4's two state strings. They are the values the standard writes into `oldState` and `newState`, and they
   are named once because the toggle task carries one of them across a suspension. */
static const char DIALOG_STATE_OPEN[]   = "open";
static const char DIALOG_STATE_CLOSED[] = "closed";

/* The `returnValue` IDL attribute's storage, and §4.11.4's DIALOG TOGGLE TASK TRACKER. Both are per-element
   state under a private Symbol; the tracker is the struct's TWO fields in two slots — the pending task's OLD
   STATE and the task itself — and its ABSENCE is the spec's null tracker. */
static JSValue g_ret_key = JS_UNDEFINED;
static JSAtom  g_atom_ret = JS_ATOM_NULL;
static JSValue g_tracker_key = JS_UNDEFINED;
static JSAtom  g_atom_tracker = JS_ATOM_NULL;
/* …and the TASK half of that same struct, as the JSTaskHandle JS_EnqueueCallTask answered, in a BigInt so the
   full 64 bits survive. Two slots and not one because the tracker IS two fields and an absent slot is the
   spec's null; they are written and cleared together, which the read asserts. */
static JSValue g_tracker_task_key = JS_UNDEFINED;
static JSAtom  g_atom_tracker_task = JS_ATOM_NULL;
static int     g_toggle_stepid = -1;
static int     g_id_return_value = -1;
static int     g_id_closed_by = -1;

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
    DCHECK(g_atom_ret != JS_ATOM_NULL,
           "HTMLDialogElement.returnValue was read before html_dialog_declare minted its slot key");
    /* WEB IDL §3.7.6 Attributes' BRAND CHECK, and it is a THROW rather than an assert: a page reaches an
       accessor off the
       prototype with `.call` on anything at all, so the receiver is the PAGE's input. */
    if (!dialog_elem_of(this_val))
        return JS_ThrowTypeError(ctx, "HTMLDialogElement.returnValue read on something that is not a <dialog> "
                                      "element");
    if (JS_GetOwnSlot(ctx, &v, this_val, g_atom_ret) <= 0) return JS_NewString(ctx, "");
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
    JS_DefinePropertyValue(ctx, (JSValue)dialog, g_atom_ret, JS_DupValue(ctx, v),
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
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
static int dialog_computed_closed_by_state(JSContext *ctx, lxb_dom_element_t *el)
{
    int state;

    DCHECK(el != NULL, "§4.11.4's computed closed-by state was asked of a null element — its one caller in "
                       "this build reaches it through dialog_elem_of, which answers NULL for anything that is "
                       "not a <dialog>, and every caller added later owes the same");
    state = enumerated_attribute_state(el, "closedby", DIALOG_CLOSEDBY_KW,
                                       DIALOG_CLOSEDBY_AUTO,   /* missing value default */
                                       DIALOG_CLOSEDBY_AUTO,   /* no empty value default: §2.3.3 step 4 runs */
                                       DIALOG_CLOSEDBY_AUTO);  /* invalid value default */
    if (state == DIALOG_CLOSEDBY_AUTO) {                                                          /* step 1 */
        /* Step 1.1 is "If dialog's is modal is true, then return Close Request." §4.11.4 says "Each dialog
           element has an is modal boolean, initially false", and `showModal()` is the only thing that sets it
           true — so the boolean is false for every dialog this build can produce and step 1.1 does not fire.
           That is a condition EVALUATED at the step that asks it, not a skipped step, which is the same shape
           and the same argument as close-the-dialog's steps 6-8 below; and like those, it is ASSERTED rather
           than believed, so the day showModal lands this fires AT the step that must then be written. */
        realm_awaits(ctx, "HTMLDialogElement.prototype.showModal",
                     "§4.11.4 retrieve a dialog's computed closed-by state step 1.1 reads the dialog's IS "
                     "MODAL boolean and returns Close Request when it is true. `showModal()` is what sets it, "
                     "so with that member in this build the boolean needs real per-element storage and this "
                     "step must read it — and until it does, every modal dialog's computed closed-by state "
                     "answers None, which is exactly the value that makes its close watcher's getEnabledState "
                     "false and so makes an Esc on a modal dialog do nothing");
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
    state = dialog_computed_closed_by_state(ctx, el);
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
    JS_DeleteProperty(ctx, (JSValue)element, g_atom_tracker, 0);
    JS_DeleteProperty(ctx, (JSValue)element, g_atom_tracker_task, 0);
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
    if (JS_GetOwnSlot(ctx, &pending, element, g_atom_tracker) > 0) {
        JSValue task;

        DCHECK(JS_IsString(pending), "the dialog toggle task tracker's OLD STATE slot held something that is "
                                     "not a string — the only writer is step 3 below, which places one of "
                                     "§4.11.4's two state strings");
        ov = pending;                                                       /* step 1.1's carry of oldState */
        if (JS_GetOwnSlot(ctx, &task, element, g_atom_tracker_task) > 0) {
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
    JS_DefinePropertyValue(ctx, (JSValue)element, g_atom_tracker, JS_DupValue(ctx, ov),
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    JS_DefinePropertyValue(ctx, (JSValue)element, g_atom_tracker_task,
                           JS_NewBigUint64(ctx, (uint64_t)handle),
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    JS_FreeValue(ctx, nv);
    JS_FreeValue(ctx, ov);
    JS_FreeValue(ctx, fn);
}

/* ---- §4.11.4's CLOSE THE DIALOG -------------------------------------------------------------------------------- */

#define DIALOG_CLOSE_PHASES(X) \
    X(DCR_BEFORETOGGLE, "HTML §4.11.4 close the dialog step 2 (fire an event named beforetoggle, using " \
                        "ToggleEvent, with oldState \"open\", newState \"closed\" and source, at subject)") \
    X(DCR_FINISH,       "HTML §4.11.4 close the dialog steps 3-13 (the second open-attribute check, the queued " \
                        "toggle task, removing the open attribute, the return value, and the queued `close`)")
enum { DIALOG_CLOSE_PHASES(JS_STEP_STAGE_ENUM) };
static const char *const DIALOG_CLOSE_PHASE_STEPS[] = { DIALOG_CLOSE_PHASES(JS_STEP_STAGE_LABEL) NULL };

struct DialogCloseRun {
    uint8_t phase;      /* DCR_* */
    uint8_t fphase;     /* step 2's fire request's own phase */
    JSValue subject;    /* the dialog being closed (owned) */
    JSValue result;     /* the spec's "null or a string" (owned) */
    JSValue source;     /* the spec's "Element or null" (owned) */
    JSValue ev;         /* step 2's `beforetoggle` event (owned) */
    EventFireCb cb;     /* the fire request buffer: [this, dispatch, target, event] */
};

static void dialog_close_elem_visit(JSContext *ctx, void *elem, JSStepVisit *v)
{
    DialogCloseRun *r = elem;
    int k;

    v->val(ctx, &r->subject);
    v->val(ctx, &r->result);
    v->val(ctx, &r->source);
    v->val(ctx, &r->ev);
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
    STEP_CB_FOREACH(r->cb, k) JS_FreeValue(ctx, r->cb[k]);
    js_free(ctx, r);
    *slot = NULL;
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
        r->subject = r->result = r->source = r->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(r->cb, k) r->cb[k] = JS_UNDEFINED;
        r->phase = DCR_BEFORETOGGLE;
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
    DCHECK(DIALOG_CLOSE_PHASE_STEPS[DCR_FINISH] != NULL, "the phase list lost the step its last phase rests at");
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
        JS_FreeValue(ctx, r->ev);
        r->ev = JS_UNDEFINED;
        r->phase = DCR_FINISH;
    } else {
        JS_FreeValue(ctx, in);
    }
    DCHECK(r->phase == DCR_FINISH, "a close-the-dialog run resumed into a phase §4.11.4 does not have");
    /* Step 3: the attribute is read AGAIN, because a `beforetoggle` handler had the element in its hands. */
    if (!dialog_has_open(el)) return 0;
    dialog_queue_toggle_task(ctx, r->subject, DIALOG_STATE_OPEN, DIALOG_STATE_CLOSED, r->source);   /* step 4 */
    dom_cow_remove_attribute(el, "open");                                                           /* step 5 */
    /* Steps 6-8 read the IS MODAL flag: remove a modal dialog from the TOP LAYER, remember whether it was
       modal, and set the flag false. Nothing in this build can set it — `showModal()` is the only producer of
       a modal dialog and of a top-layer entry — so the flag is false here and all three steps are the
       identity. That is a condition EVALUATED at the step that asks it, not a skipped step, and it is asserted
       rather than believed: the day showModal lands, this fires AT the three steps that must then be written. */
    realm_awaits(ctx, "HTMLDialogElement.prototype.showModal",
                 "§4.11.4 close the dialog steps 6-8 read the dialog's IS MODAL flag — request the element's "
                 "removal from the TOP LAYER, latch wasModal for step 12.3's focus decision, and set the flag "
                 "false. `showModal()` is what sets it, so with that member in this build these three steps "
                 "must be written");
    if (!JS_IsNull(r->result))                                                                      /* step 9 */
        dialog_set_return_value(ctx, r->subject, r->result);
    /* Steps 10 and 11 clear the REQUEST CLOSE RETURN VALUE and the REQUEST CLOSE SOURCE ELEMENT, which only
       `requestClose()` ever writes, and step 12 restores the PREVIOUSLY FOCUSED ELEMENT, which only
       `show()`/`showModal()` ever writes. All three are null in this build, so the clears are the identity and
       the focus restoration has nothing to restore — asserted at the steps, for the same reason as above. */
    realm_awaits(ctx, "HTMLDialogElement.prototype.requestClose",
                 "§4.11.4 close the dialog steps 10 and 11 set the dialog's REQUEST CLOSE RETURN VALUE and "
                 "REQUEST CLOSE SOURCE ELEMENT to null — `requestClose()` is what writes them, so with that "
                 "member in this build those two steps must be written");
    realm_awaits(ctx, "HTMLDialogElement.prototype.show",
                 "§4.11.4 close the dialog step 12 restores focus to the dialog's PREVIOUSLY FOCUSED ELEMENT "
                 "and clears it — `show()`/`showModal()` are what set it, so with those members in this build "
                 "that step must run §6.6.4's focusing steps for the remembered element, and only when the "
                 "focused area is inside the dialog or wasModal is true");
    /* Step 13: "queue an element task on the user interaction task source given the subject element to fire an
       event named close at subject". QUEUED, which is what event_target_fire is — the dispatch becomes a
       first-class flow the one scheduler drives, so a `close` listener's body suspends and resumes like any
       other program. It is also why the submission that reached here has already returned by the time a page
       sees `close`. */
    event_target_fire(ctx, r->subject, event_new(ctx, "close", /*bubbles*/ false, /*cancelable*/ false),
                      JS_UNDEFINED);
    return 0;
}

/* ---- declare / install ---------------------------------------------------------------------------------------- */

void html_dialog_declare(JSContext *ctx)
{
    DCHECK(JS_IsUndefined(g_ret_key), "html_dialog_declare ran twice — one instance is one agent");
    g_ret_key = JS_NewSymbol(ctx, "dialogReturnValue", false);
    CHECK(!JS_IsException(g_ret_key), "the dialog return-value slot key allocation failed");
    g_atom_ret = JS_ValueToAtom(ctx, g_ret_key);
    CHECK(g_atom_ret != JS_ATOM_NULL, "the dialog return-value slot key could not be interned");
    g_tracker_key = JS_NewSymbol(ctx, "dialogToggleTaskTracker", false);
    CHECK(!JS_IsException(g_tracker_key), "the dialog toggle-task tracker slot key allocation failed");
    g_atom_tracker = JS_ValueToAtom(ctx, g_tracker_key);
    CHECK(g_atom_tracker != JS_ATOM_NULL, "the dialog toggle-task tracker slot key could not be interned");
    g_tracker_task_key = JS_NewSymbol(ctx, "dialogToggleTaskTrackerTask", false);
    CHECK(!JS_IsException(g_tracker_task_key), "the dialog toggle-task handle slot key allocation failed");
    g_atom_tracker_task = JS_ValueToAtom(ctx, g_tracker_task_key);
    CHECK(g_atom_tracker_task != JS_ATOM_NULL, "the dialog toggle-task handle slot key could not be interned");
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
}

void html_dialog_install(JSContext *ctx, JSValueConst dialog_proto)
{
    DCHECK(g_id_return_value >= 0, "§4.11.4's members were installed before they were declared");
    DCHECK(g_id_closed_by >= 0, "§4.11.4's `closedBy` was installed before html_dialog_declare minted its "
                                "setter id");
    DCHECK(JS_IsObject(dialog_proto), "the dialog members were installed with no HTMLDialogElement.prototype");
    /* IN §4.11.4's OWN IDL ORDER — `returnValue` then `closedBy` — because that is the order the interface
       declares them in and there is no other order to prefer. */
    idl_install_accessor(ctx, dialog_proto, "returnValue", js_dialog_get_return_value, 0, g_id_return_value);
    idl_install_accessor(ctx, dialog_proto, "closedBy", js_dialog_get_closed_by, 0, g_id_closed_by);
}

void html_dialog_free(JSRuntime *rt)
{
    toggle_event_free(rt);
    /* The slot keys are the AGENT's, so they are released with the agent — a Symbol nobody frees is a live GC
       object the runtime's own walk counts as a leak. */
    JS_FreeAtomRT(rt, g_atom_ret);
    g_atom_ret = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_ret_key);
    g_ret_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_tracker);
    g_atom_tracker = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_tracker_key);
    g_tracker_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_tracker_task);
    g_atom_tracker_task = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_tracker_task_key);
    g_tracker_task_key = JS_UNDEFINED;
    g_toggle_stepid = -1;
    g_id_return_value = -1;
    g_id_closed_by = -1;
}
