/* §4.10.5.4's `showPicker()` and the "show the picker, if applicable" algorithm behind it — see
 * input_picker.h for why the two live together and why this is not part of input_value.c.
 *
 * THE TWO ALGORITHMS, AS THE STANDARD WRITES THEM.
 *
 *   The HTMLInputElement showPicker() and HTMLSelectElement showPicker() method steps are:
 *     1. If this is not mutable, then throw an "InvalidStateError" DOMException.
 *     2. If this's relevant settings object's origin is not same origin with this's relevant settings object's
 *        top-level origin, and this is a select element, or this's type attribute is not in the File Upload
 *        state or Color state, then throw a "SecurityError" DOMException. [File and Color inputs are exempted
 *        from this check for historical reason: their input activation behavior also shows their pickers, and
 *        has never been guarded by an origin check.]
 *     3. If this's relevant global object does not have transient activation, then throw a "NotAllowedError"
 *        DOMException.
 *     4. If this is a select element, and this is not being rendered, then throw a "NotSupportedError"
 *        DOMException.
 *     5. Show the picker, if applicable, for this.
 *
 *   To show the picker, if applicable for an input or select element element:
 *     1. If element's relevant global object does not have transient activation, then return.
 *     2. If element is not mutable, then return.
 *     3. Consume user activation given element's relevant global object.
 *     4. If element does not support a picker, then return.
 *     5. If element is an input element and element's type attribute is in the File Upload state, then run
 *        these steps in parallel:
 *          1. Optionally, wait until any prior execution of this algorithm has terminated.
 *          2. Let dismissed be the result of WebDriver BiDi file dialog opened with element.
 *          3. If dismissed is false: display a prompt to the user requesting that the user specify some files.
 *             If the multiple attribute is not set on element, there must be no more than one file selected;
 *             otherwise, any number may be selected. … Wait for the user to have made their selection.
 *          4. If dismissed is true or if the user dismissed the prompt without changing their selection, then
 *             queue an element task on the user interaction task source given element to fire an event named
 *             cancel at element, with the bubbles attribute initialized to true.
 *          5. Otherwise, update the file selection for element.
 *        Otherwise, the user agent should show the relevant user interface for selecting a value for element,
 *        in the way it normally would when the user interacts with the control.
 *
 * STEP 4 OF THE METHOD IS A `select` ELEMENT'S, AND THERE IS NO SELECT COMPONENT IN THIS ENGINE — `select` is
 * one row of html_element.c's interface-name table and has no members, no list of options behind an IDL
 * attribute of its own and no "being rendered" to test. So this file declares the INPUT half of a member the
 * standard states for both, and the day §4.10.7 becomes a component it brings its own step 4 and its own
 * "supports a picker"; nothing here is written as if that element existed.
 *
 * WHY IT IS A STEP MACHINE, AND WHY EACH ASK IS ITS OWN STAGE. Neither algorithm calls the page's code — the
 * two fires it can end in are ELEMENT TASKS, queued rather than dispatched — so what suspends it is not a
 * listener: it is the ACTIVATION GATE. Whether a user has interacted is unknown external state, so §6.4.1's
 * question FORKS, and a fork is a request the driver answers and snapshots the flow at. `h`'s fork phase
 * remembers that ONE request is outstanding, so two asks in one stage would re-ask the first on every
 * re-entry and never converge — every ask below therefore has a stage to itself, and the transient chain's own
 * two questions are separated by the phase byte user_activation.h documents. */
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/file/file_device.h"
#include "core/frame/window_proxy.h"
#include "core/html/html_form.h"
#include "core/html/input_picker.h"
#include "core/html/input_value.h"
#include "core/html/user_activation.h"
#include "solver/concolic.h"

/* THE FILE DIALOG'S OUTCOME IS THE OTHER UNKNOWN IN THIS FILE, and it is the same kind as the activation
   gate's: step 5.2's `dismissed` is what the user did with a prompt, which a headless engine cannot observe.
   Both answers reach code a real bundle ships — the `change` handler that uploads, and the `cancel` handler
   that re-enables a button or reports "no file chosen" — so it is a source with an example rather than a
   decision this file takes. The EXAMPLE is "not dismissed", because that is what the modelled device does: it
   holds files and the prompt hands the accepted ones back (core/file/file_device.c), which is also why outcome
   0 is that arm — a run with no forking policy takes the ordinary completion. */
#define PICKER_DIALOG_SHAPE "{file dialog dismissed}"
#define PICKER_DIALOG_SRC   "input.showPicker().dismissed"
#define PICKER_DIALOG_OP    "HTML §4.10.5.4 show the picker step 5.2 (WebDriver BiDi file dialog opened)"

/* §4.10.5.1: "The input element can support a picker. … Whether an input element supports a picker depends on
   the type attribute state and IMPLEMENTATION-DEFINED BEHAVIOR. An input element must support a picker when
   its type attribute is in the File Upload state."
   THIS USER AGENT'S ANSWER IS THE FILE UPLOAD STATE AND NOTHING ELSE, and that is a decision rather than a
   gap: the implementation-defined half asks which pickers this agent actually presents, and it presents
   exactly one — core/file/file_device.c's, the mock device §4.10.5.1.17's prompt chooses from. A date or color
   control has no device here to choose a value with, so claiming a picker for it would be claiming a user
   interface that does not exist, which is the shape §NO STUBS names. The consumption at step 3 still happens
   for those controls, because step 3 precedes step 4 — the page spends its activation either way, which is
   what a browser does too. */
static bool input_supports_picker(HtmlInputState st)
{
    return st == INPUT_STATE_FILE;
}

/* STEP 5.4's OTHER HALF — "queue an element task on the user interaction task source given element to fire an
   event named cancel at element, with the bubbles attribute initialized to true". QUEUED, which is what
   event_target_fire is: the dispatch becomes a first-class flow the one scheduler drives, so a `cancel`
   listener's body — loop, await, generator — suspends and resumes like any other program instead of running
   inside a C activation that cannot park. It is also the order the standard describes, since these steps run
   IN PARALLEL and end by queueing: showPicker() returns before the listener sees anything. */
static void picker_fire_cancel(JSContext *ctx, JSValueConst element)
{
    event_target_fire(ctx, element, event_new(ctx, "cancel", /*bubbles*/ true, /*cancelable*/ false),
                      JS_UNDEFINED);
}

/* WHERE THIS MACHINE RESTS, AS THE STANDARD NUMBERS IT. Five stages, and four of them exist because an
   UNKNOWN is asked at them — one question per stage, because a machine may have exactly one request
   outstanding and a stage that asks twice would re-ask the first on every re-entry and never converge. The
   first is the pair of throws that precede any ask, kept out of the others so a re-entry cannot re-run a step
   that has already thrown. */
#define SHOWPICKER_STAGES(X) \
    X(SP_ENTER,     "HTML §4.10.5.4 showPicker() steps 1-2 (this is mutable, and the same-origin-with-top " \
                    "check the File Upload and Color states are exempt from)") \
    X(SP_ALLOWED,   "HTML §4.10.5.4 showPicker() step 3 (this's relevant global object has TRANSIENT " \
                    "ACTIVATION — unknown external state, so the NotAllowedError arm and the picker arm are " \
                    "two worlds and this is where the flow forks)") \
    X(SP_APPLICABLE, "HTML §4.10.5.4 show the picker, if applicable steps 1-2 (the transient-activation test " \
                    "the shared algorithm makes for itself, then `element is not mutable`)") \
    X(SP_CONSUME,   "HTML §4.10.5.4 show the picker, if applicable steps 3-4 (CONSUME USER ACTIVATION given " \
                    "element's relevant global object, then `element does not support a picker`)") \
    X(SP_DIALOG,    "HTML §4.10.5.4 show the picker, if applicable steps 5.2-5.5 (the file dialog's outcome, " \
                    "then either the queued `cancel` or update the file selection)")
enum { IDL_STEP_STAGE_BASE(SHOWPICKER_STAGES) SHOWPICKER_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SHOWPICKER_STEPS[] = { SHOWPICKER_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    /* THE ACTIVATION QUESTIONS' PHASE, which user_activation.h requires the CALLING machine to own: the
       transient state is a chain of two questions and which of them a parked flow is at cannot live in a C
       local. One byte serves all of this machine's asks, because each is answered before the next begins. */
    uint8_t  ua_phase;
    /* THIS'S `type` ATTRIBUTE STATE, read ONCE at the entry and CARRIED. Both algorithms are stated over the
       state the call was made in, and every stage after the first is a rest point — so re-reading the
       attribute at the stage that uses it would read it at the wrong TIME, which is the defect that arrives
       with every conversion from a call to a job. The two mutability tests below are NOT this: the standard
       makes them twice, at two of its own steps, so each is a read the algorithm asks for. */
    uint8_t  type_state;
    /* THE FILE DIALOG'S UNKNOWN (owned), minted at the end of SP_CONSUME and held across SP_DIALOG's request.
       It is the machine's own value rather than a borrowed one because nothing else holds a reference to it —
       step_fork_run borrows the operand for the length of the request, and the owner has to be somewhere the
       fork's SNAPSHOT carries. */
    JSValue  dismissed;
} ShowPickerState;

static void picker_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    ShowPickerState *s = st;

    v->val(ctx, &s->dismissed);
}

/* A machine torn down BEFORE its first entry holds a zeroed state, and a zeroed JSValue is the INTEGER 0 —
   which JS_FreeValue leaves alone, because an integer owns nothing. So this needs no started flag: it releases
   exactly what the state holds, whether that is the dialog's unknown or nothing at all. */
/* §4.10.5.4's TWO ALGORITHMS AS ONE MACHINE. The method's steps 1-4 are the throwing prologue and step 5 hands
   over to "show the picker, if applicable", which repeats the first two of them — deliberately, because that
   algorithm is also reached from the File Upload state's INPUT ACTIVATION BEHAVIOR where no method ran first.
   Repeating them here costs nothing but a request round trip: the flow has already decided §6.4.1's transient
   question, so the second ask is answered from its own path constraint and forks nothing. */
static int picker_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                       JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    ShowPickerState *s = st;
    bool ok = false;
    int rc;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;

    if (hdr->stage == SP_ENTER) {
        HtmlInputState state = html_form_input_state(node_of(hdr->this_val));

        /* EVERY OWNED FIELD IN PLACE BEFORE THE FIRST THING THAT CAN THROW — the throw path tears this state
           down through picker_visit, which names exactly what the state owns and nothing else. */
        s->dismissed = JS_UNDEFINED;
        s->ua_phase = 0;
        s->type_state = (uint8_t)state;
        /* WEB IDL §3.7.5's BRAND CHECK. `HTMLInputElement.prototype.showPicker.call(textarea)` is a TypeError
           and a page tells that apart from every DOMException below it. */
        if (state == INPUT_STATE_NONE) {
            JS_ThrowTypeError(ctx, "HTMLInputElement's `showPicker` was called on something that is not an "
                                   "HTMLInputElement");
            return -1;
        }
        if (!html_form_input_is_mutable(ctx, hdr->this_val)) {           /* step 1 */
            JS_ThrowDOMException(ctx, "InvalidStateError",
                                 "showPicker() was called on an input element that is not mutable");
            return -1;
        }
        /* Step 2. The File Upload and Color states are exempt "for historical reason: their input activation
           behavior also shows their pickers, and has never been guarded by an origin check" — so the exemption
           is about which pickers a CLICK could already have opened, and it is stated by state, not by whether
           this agent has a device for one. */
        /* "this's relevant settings object's origin is not same origin with this's relevant settings object's
           top-level origin" — core/frame/window_proxy.h's one implementation of that sentence, which File
           System Access §2.2 and §3.1 ask in the same words. */
        if (!window_proxy_same_origin_with_top(ctx) && state != INPUT_STATE_FILE && state != INPUT_STATE_COLOR) {
            JS_ThrowDOMException(ctx, "SecurityError",
                                 "showPicker() was called in a document that is not same origin with its "
                                 "top-level document");
            return -1;
        }
        /* Step 4 is a `select` element's and this member's receiver is an `input`, so the method's remaining
           step is step 3 — asked at its own stage below. */
        STEP_GOTO(hdr->stage, SP_ALLOWED, &s->ua_phase, NULL);
    }

    if (hdr->stage == SP_ALLOWED) {
        rc = user_activation_transient_run(ctx, hdr, &s->ua_phase, &ok);   /* step 3 */
        if (rc) return rc;
        if (!ok) {
            JS_ThrowDOMException(ctx, "NotAllowedError",
                                 "showPicker() requires transient user activation");
            return -1;
        }
        STEP_GOTO(hdr->stage, SP_APPLICABLE, &s->ua_phase, NULL);           /* step 5 */
    }

    if (hdr->stage == SP_APPLICABLE) {
        rc = user_activation_transient_run(ctx, hdr, &s->ua_phase, &ok);   /* show the picker step 1 */
        if (rc) return rc;
        /* Steps 1 and 2 RETURN rather than throw — this algorithm's callers include an activation behavior,
           which has nowhere to throw to. */
        if (!ok || !html_form_input_is_mutable(ctx, hdr->this_val)) return JS_STEP_DONE;   /* step 2 */
        STEP_GOTO(hdr->stage, SP_CONSUME, &s->ua_phase, NULL);
    }

    if (hdr->stage == SP_CONSUME) {
        /* STEP 3 — "Consume user activation given element's relevant global object", which is §6.4.2's
           EXHAUSTIVE walk over every navigable in the page: the transient state becomes false everywhere while
           the sticky state stays true, so a second showPicker() on the same interaction throws. That is the
           abuse §6.4.2's note names the walk to prevent, and it is what makes this member the first real
           consumer of it. */
        rc = user_activation_consume_run(ctx, hdr, &s->ua_phase);
        if (rc) return rc;
        /* Step 4 — and a control this agent has no picker for stops HERE, with its activation already spent. */
        if (!input_supports_picker((HtmlInputState)s->type_state)) return JS_STEP_DONE;
        DCHECK((HtmlInputState)s->type_state == INPUT_STATE_FILE,
               "§4.10.5.4 reached step 5 for a state input_supports_picker admitted and this file has no branch "
               "for — step 5's OTHERWISE arm shows a user interface for selecting a value, so a state that "
               "supports a picker here must have a device to select one with");
        /* STEP 5.2's `dismissed`, minted here so the fork at the next stage has an operand the SNAPSHOT
           carries. A device holding NO FILES AT ALL is not a second unknown but a decided one: there is
           nothing for step 5.3's prompt to offer, so no selection is reachable and the dismissal is the only
           completion — forking it would park a sibling flow that does the same thing this one does. (A device
           that holds files none of which this control's `accept` admits still forks, and both arms end in the
           cancel; narrowing that would mean running the filter twice, and the filter is the prompt.) */
        if (file_device_count(ctx) == 0) {
            picker_fire_cancel(ctx, hdr->this_val);
            return JS_STEP_DONE;
        }
        s->dismissed = concolic_source_wrap(ctx, PICKER_DIALOG_SHAPE, PICKER_DIALOG_SRC, JS_FALSE);
        CHECK(!JS_IsException(s->dismissed),
              "showPicker: the file dialog's outcome could not be allocated");
        STEP_GOTO(hdr->stage, SP_DIALOG, &s->ua_phase, NULL);
    }

    DCHECK(hdr->stage == SP_DIALOG, "showPicker resumed into a stage §4.10.5.4 does not have");
    {
        int arm = 0;

        if (concolic_is(s->dismissed)) {
            rc = step_fork_run(ctx, hdr, s->dismissed, PICKER_DIALOG_OP, 2, JS_OUTCOME_REAL_UNSTATED, &arm);
            if (rc) return rc;
        } else {
            /* A host with no source overlay (a conformance run) gets the plain example back, so there is one
               completion and it is the one the modelled device produces. */
            arm = JS_ToBool(ctx, s->dismissed) ? 1 : 0;
        }
        /* STEP 5.5 — "Otherwise, update the file selection for element", which input_value.c performs over the
           device's answer: it replaces the list of selected files and queues the element task that fires
           `input` and then `change`. A selection of NOTHING is step 5.4's other half, "the user dismissed the
           prompt without changing their selection". */
        if (arm == 0 && input_files_pick(ctx, hdr->this_val) > 0) return JS_STEP_DONE;
        picker_fire_cancel(ctx, hdr->this_val);                            /* step 5.4 */
    }
    return JS_STEP_DONE;
}

static const IdlStepDecl SHOWPICKER_DECL = { picker_step, sizeof(ShowPickerState), picker_visit, NULL,
                                             "HTML §4.10.5.4 HTMLInputElement.showPicker()", SHOWPICKER_STEPS };
static int g_id_show_picker = -1;

void input_picker_declare(JSContext *ctx)
{
    DCHECK(g_id_show_picker < 0, "input_picker_declare ran twice — the member is declared once per AGENT, and "
                                 "a second declaration would mint the machine per realm");
    g_id_show_picker = idl_method_id_step(ctx, NULL, 0, NULL, 0, &SHOWPICKER_DECL, 0);
}

void input_picker_install(JSContext *ctx, JSValueConst input_proto)
{
    DCHECK(g_id_show_picker >= 0, "§4.10.5.4's showPicker was installed before input_picker_declare declared it");
    DCHECK(JS_IsObject(input_proto), "showPicker was installed with no HTMLInputElement.prototype");
    idl_install_method(ctx, input_proto, "showPicker", 0, g_id_show_picker);
}

void input_picker_free(void)
{
    g_id_show_picker = -1;
}
