/* HTML §4.10.20 "APIs for the text control selections" — see text_control_selection.h for why the six members
   of two interfaces are one component and why the state is per flow. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/node.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/html/html_form.h"
#include "core/html/input_value.h"
#include "core/html/text_control_selection.h"
#include "core/idl_args.h"
#include "solver/attr_shadow.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"

/* THE THREE PER-ELEMENT SLOTS, in the per-flow property shadow the control's value already uses — so the
   cursor time-travels with the value it points into. Their ABSENCE is §4.10.20's own initial state and is read
   as the positive statement it is: "The initial state must consist of a text entry cursor at the beginning of
   the control", so no entry means offset 0, and "The initial selection direction must be \"none\" if the
   platform supports that direction", which this one does. Nothing writes them at parse time for that reason —
   a control the page has not touched is not a control with missing state. */
#define TCS_START "selectionStart"
#define TCS_END   "selectionEnd"
#define TCS_DIR   "selectionDirection"

/* §4.10.20's three directions. The standard's "The exact meaning of the selection direction depends on the
   platform" is about what a USER's arrow keys do with it, not about the value: the three tokens and the
   transitions between them are fully specified, so they are modelled exactly and nothing here is a shrug. */
#define TCS_DIR_NONE     "none"
#define TCS_DIR_FORWARD  "forward"
#define TCS_DIR_BACKWARD "backward"

/* WHICH MEMBER, AND WHICH INTERFACE DECLARED IT — both halves live in the magic. §4.10.20's six members are
   declared SEPARATELY by HTMLInputElement and by HTMLTextAreaElement (the section shares ALGORITHMS, not a
   mixin), so Web IDL §3.7.5's brand check is per interface: `HTMLInputElement.prototype.select.call(textarea)`
   is a TypeError and not a select, exactly as html_form.c's `js_input_brand` states for `value`. One shared C
   body cannot ask which prototype it was reached through, so the INSTALL says. */
enum { TCS_M_SELECT = 0, TCS_M_START, TCS_M_END, TCS_M_DIRECTION, TCS_M_SET_RANGE_TEXT, TCS_M_SET_RANGE };
#define TCS_TEXTAREA        0x100
#define TCS_MEMBER(m)       ((m) & 0xff)
#define TCS_IS_TEXTAREA(m)  (((m) & TCS_TEXTAREA) != 0)

/* ---- §4.10.20's APPLICABILITY, which is §4.10.5.1's per-state bookkeeping read as two predicates ----------
 *
 * The lists below are transcribed from the twenty-one state sections' own "The following IDL attributes and
 * methods do not apply to the element" sentences, and they are TWO lists because the standard gives two: the
 * five offset members and `select()` do not apply to the same set of states. Getting that wrong in either
 * direction is a real page-visible difference — `email.select()` selects (Email is not in `select()`'s
 * exclusion list) while `email.selectionStart` is null (Email IS in the offset members'), and a single
 * predicate would answer one of those two wrongly for eight states. */

/* §4.10.20's `selectionStart`, `selectionEnd`, `selectionDirection`, `setRangeText()` and `setSelectionRange()`
   — listed as NOT applying by every state except Text, Search, Telephone, URL and Password, whose four
   sections list only `checked`/`files`/`valueAsDate`/`valueAsNumber`/`stepDown`/`stepUp` (Password also
   `list`). Written as the states that DO apply, because that is the shorter list and because a state added to
   the enum then has to be classified rather than silently inheriting a default. */
static bool tcs_offsets_apply(HtmlInputState st)
{
    switch (st) {
    case INPUT_STATE_TEXT: case INPUT_STATE_SEARCH: case INPUT_STATE_TEL:
    case INPUT_STATE_URL:  case INPUT_STATE_PASSWORD:
        return true;
    case INPUT_STATE_NONE:
    case INPUT_STATE_HIDDEN: case INPUT_STATE_EMAIL: case INPUT_STATE_DATE: case INPUT_STATE_MONTH:
    case INPUT_STATE_WEEK:   case INPUT_STATE_TIME:  case INPUT_STATE_DATETIME_LOCAL:
    case INPUT_STATE_NUMBER: case INPUT_STATE_RANGE: case INPUT_STATE_COLOR:
    case INPUT_STATE_CHECKBOX: case INPUT_STATE_RADIO: case INPUT_STATE_FILE:
    case INPUT_STATE_SUBMIT: case INPUT_STATE_IMAGE: case INPUT_STATE_RESET: case INPUT_STATE_BUTTON:
        return false;
    }
    DFAIL("§4.10.5.1 names twenty-one states of the `type` attribute and §4.10.20's applicability was asked of "
          "a twenty-second — the two lists in this file are per state on purpose, so a new state is a "
          "classification somebody makes rather than one a default makes for them");
    return false;
}

/* §4.10.20's `select()`, which applies WIDER: only Hidden, Range, Checkbox, Radio Button, Submit Button, Image
   Button, Reset Button and Button list it among the methods that do not apply. So it applies to the whole text
   family, to the date-and-time family, and to Number, Color, Email and File Upload — none of which have offset
   members. That is not an inconsistency in the standard: `select()` ends in set the selection range, which
   operates on the relevant value and fires `select`, and none of that needs an exposed offset. */
static bool tcs_select_applies(HtmlInputState st)
{
    switch (st) {
    case INPUT_STATE_NONE:
    case INPUT_STATE_HIDDEN: case INPUT_STATE_RANGE: case INPUT_STATE_CHECKBOX: case INPUT_STATE_RADIO:
    case INPUT_STATE_SUBMIT: case INPUT_STATE_IMAGE:  case INPUT_STATE_RESET:    case INPUT_STATE_BUTTON:
        return false;
    case INPUT_STATE_TEXT:  case INPUT_STATE_SEARCH: case INPUT_STATE_TEL: case INPUT_STATE_URL:
    case INPUT_STATE_EMAIL: case INPUT_STATE_PASSWORD: case INPUT_STATE_DATE: case INPUT_STATE_MONTH:
    case INPUT_STATE_WEEK:  case INPUT_STATE_TIME:   case INPUT_STATE_DATETIME_LOCAL:
    case INPUT_STATE_NUMBER: case INPUT_STATE_COLOR: case INPUT_STATE_FILE:
        return true;
    }
    DFAIL("§4.10.5.1 names twenty-one states of the `type` attribute and `select()`'s applicability was asked "
          "of a twenty-second");
    return false;
}

/* §4.10.20 step 1 of `select()`'s second condition: "or THE CORRESPONDING CONTROL HAS NO SELECTABLE TEXT". The
   standard leaves this to the user agent and gives one example of what it means — "in a user agent where
   <input type=color> is rendered as a color well with a picker, AS OPPOSED TO a text control accepting a
   hexadecimal color code, there would be no selectable text".
   THIS AGENT RENDERS NOTHING, so the alternative the example names is the one that describes it: a Color
   control here IS a text control holding a hexadecimal color code, and its text is selectable. The one state
   that has no selectable text under any rendering is File Upload, whose relevant value is the fake path
   "C:\\fakepath\\..." that §4.10.5.4 synthesises rather than text anybody typed — the value mode says so, which
   is why this asks the mode rather than naming the state a second time. */
static bool tcs_has_selectable_text(HtmlInputState st)
{
    return input_value_mode(st) != INPUT_VALUE_MODE_FILENAME;
}

bool text_control_selection_applies(const lxb_dom_node_t *n)
{
    if (html_form_is_textarea(n)) return true;
    return tcs_offsets_apply(html_form_input_state(n));
}

/* ---- the control, and its relevant value ------------------------------------------------------------------ */

/* WHAT THIS RECEIVER IS. `is_textarea` is the INSTALL's claim (which prototype the member came off) and the
   node is the receiver's reality; disagreement is Web IDL §3.7.5's TypeError and never a silent answer. */
typedef struct { bool is_textarea; HtmlInputState st; lxb_dom_element_t *el; } TcsControl;

static bool tcs_receiver(JSContext *ctx, JSValueConst wrap, int magic, const char *member, TcsControl *out)
{
    lxb_dom_node_t *n = node_of(wrap);

    out->is_textarea = TCS_IS_TEXTAREA(magic);
    out->st = INPUT_STATE_NONE;
    out->el = NULL;
    if (out->is_textarea) {
        if (!html_form_is_textarea(n)) {
            JS_ThrowTypeError(ctx, "HTMLTextAreaElement's `%s` was accessed on something that is not an "
                                   "HTMLTextAreaElement", member);
            return false;
        }
    } else {
        out->st = html_form_input_state(n);
        if (out->st == INPUT_STATE_NONE) {
            JS_ThrowTypeError(ctx, "HTMLInputElement's `%s` was accessed on something that is not an "
                                   "HTMLInputElement", member);
            return false;
        }
    }
    out->el = lxb_dom_interface_element(n);
    return true;
}

/* §4.10.20's RELEVANT VALUE: "For input elements, these APIs must operate on the element's VALUE. For textarea
   elements, these APIs must operate on the element's API VALUE." The section names the distinction and then
   spends a worked example on it, because the API value has newlines normalized and the raw value does not —
   `demo.value = "A\r\nB"; demo.setRangeText("replaced", 0, 2)` is "replacedB" over the API value and would be
   "replaced\nB" over the raw one. OWNED. */
static JSValue tcs_relevant_value(JSContext *ctx, JSValueConst wrap, const TcsControl *c)
{
    return c->is_textarea ? html_form_textarea_api_value(ctx, wrap) : input_value_get(ctx, wrap);
}

/* THE LENGTH OF THE RELEVANT VALUE IN UTF-16 CODE UNITS, which is the unit §4.10.20 measures every offset in:
   "measured in offsets into the CODE UNITS of the control's relevant value", and again at set the selection
   range ("the code unit at the startth position"). A byte count is a different number the moment the value
   holds anything outside Latin-1 — `"😀ab".length` is 4 and its UTF-8 length is 6, so `setSelectionRange(0,2)`
   over bytes would cut the emoji in half and land the selection where no browser puts it.
   AN UNKNOWN VALUE IS MEASURED THROUGH ITS EXAMPLE, which is the engine running the real algorithm on the real
   bytes it has (core/idl_args.h's `idl_number_of` states the same rule for a numeric argument). With no
   example there is nothing concrete to measure and the answer is 0 — a POSITIVE statement, not a default: a
   control whose value the run never computed behaves as the empty one §4.10.20 starts every control as. */
static uint32_t tcs_relevant_length(JSContext *ctx, JSValueConst wrap, const TcsControl *c)
{
    JSValue v = tcs_relevant_value(ctx, wrap, c);
    JSValue text = concolic_is(v) ? concolic_example(ctx, v) : JS_DupValue(ctx, v);
    uint32_t n = 0;

    if (JS_IsString(text)) {
        size_t units = 0;
        const uint16_t *u = JS_ToCStringLenUTF16(ctx, &units, text);

        CHECK(u != NULL, "text control selection: a relevant value could not be measured in code units");
        DCHECK(units <= UINT32_MAX, "a text control's relevant value is longer than an `unsigned long` offset "
                                    "can name — §4.10.20's offsets are Web IDL `unsigned long`");
        n = (uint32_t)units;
        JS_FreeCStringUTF16(ctx, u);
    }
    JS_FreeValue(ctx, text);
    JS_FreeValue(ctx, v);
    return n;
}

/* ---- the stored selection --------------------------------------------------------------------------------- */

static uint32_t tcs_slot_offset(JSContext *ctx, lxb_dom_element_t *el, const char *slot)
{
    int i = attr_shadow_find(el, ATTR_SLOT_PROPERTY, NULL, slot);
    uint32_t v = 0;

    if (i < 0) return 0;   /* §4.10.20's initial text entry cursor, at the beginning of the control */
    JS_ToUint32(ctx, &v, attr_shadow_opaque(i));
    return v;
}

/* The stored direction, BORROWED. JS_UNDEFINED when nothing has set one, which IS "none" — see the slot
   comment at the top of this file. It is a JSValue rather than a token because an UNKNOWN direction stays
   unknown: `el.selectionDirection = location.hash` narrows to one of three tokens and the engine does not know
   which, so the slot holds the unknown and a later `=== "forward"` forks through the ordinary machinery
   instead of being decided here against a shape. */
static JSValueConst tcs_slot_direction(lxb_dom_element_t *el)
{
    int i = attr_shadow_find(el, ATTR_SLOT_PROPERTY, NULL, TCS_DIR);

    return i < 0 ? JS_UNDEFINED : attr_shadow_opaque(i);
}

static void tcs_store_offset(JSContext *ctx, lxb_dom_element_t *el, const char *slot, uint32_t v)
{
    JSValue n = JS_NewUint32(ctx, v);

    dom_cow_set_prop_taint(ctx, el, slot, n);   /* BORROWED by the shadow, which dups it */
    JS_FreeValue(ctx, n);
}

/* ---- §4.10.20's `select` event ---------------------------------------------------------------------------- */

/* "Queue an element task on the user interaction task source given the element to fire an event named select at
 * the element, with the bubbles attribute initialized to true."
 *
 * IT IS A STEP MACHINE BECAUSE THE FIRE RUNS THE PAGE'S CODE — a `select` listener is the page's, so the
 * dispatch suspends and resumes like any other flow (CLAUDE.md §scheduler: every enqueued job is a first-class
 * flow, and a C loop that drove the listeners to completion would be the second scheduler that file forbids).
 * core/html/input_value.c's update-the-file-selection task is the same shape with two events instead of one. */
#define TCS_SELECT_STAGES(X) \
    X(TSE_EVENT, "HTML §4.10.20 set the selection range's last step (construct the event named select, with " \
                 "the bubbles attribute initialized to true)") \
    X(TSE_FIRE,  "HTML §4.10.20 set the selection range's last step (fire that event at the element)")
enum { TCS_SELECT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TCS_SELECT_STEPS[] = { TCS_SELECT_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr   hdr;
    uint8_t     fphase;
    JSValue     ev;       /* the event held across the suspension (owned) */
    EventFireCb cb;
} TcsSelectTask;

static int g_id_select_task = -1;

static int js_tcs_select_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    TcsSelectTask *s = st;
    JSValueConst wrap = step_arg(&s->hdr, 0);
    int r;

    STEP_DISPATCH(TCS_SELECT_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    /* TWO STAGES AND NOT A `JS_IsUndefined(s->ev)` TEST, because a fresh state is NOT full of undefineds: it
       is js_mallocz'd, and a zeroed JSValue is JS_TAG_INT 0 (JS_TAG_UNDEFINED is 3). A first-entry test
       written that way reads false on the very first call and hands the fire the INTEGER ZERO as its event.
       The stage is the state that already answers "where am I", so it answers this too. */
    STEP_ARM(TSE_EVENT);
    {
        int k;

        /* EVERY OWNED FIELD BEFORE THE FIRST THING THAT CAN FAIL — the failure path tears this machine down
           through js_tcs_select_visit, which frees exactly what the state holds and nothing else, so a field
           handed over after a throwing call is a field the teardown walks uninitialised. */
        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        s->fphase = 0;
        JS_FreeValue(ctx, cb_result);
        /* "an event named select ... with the bubbles attribute initialized to true" — and nothing else
           initialized, so it is a plain Event that does not compose and is not cancelable. */
        s->ev = event_new(ctx, "select", /*bubbles*/ true, /*cancelable*/ false);
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        STEP_GOTO(s->hdr.stage, TSE_FIRE, &s->fphase, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(TSE_FIRE);
    DCHECK(JS_IsObject(s->ev), "§4.10.20's `select` task resumed at its fire with no event to dispatch");
    r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), wrap, s->ev, JS_UNDEFINED, cb_result,
                              NULL, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    JS_FreeValue(ctx, s->ev);
    s->ev = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static void js_tcs_select_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    TcsSelectTask *s = st;
    int k;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static const JSTrampStepDef js_tcs_select_def = {
    sizeof(TcsSelectTask), js_tcs_select_step, NULL, 0,
    .visit = js_tcs_select_visit,
    .algorithm = "HTML §4.10.20 set the selection range",
    .steps = TCS_SELECT_STEPS
};

static void tcs_queue_select_event(JSContext *ctx, JSValueConst wrap)
{
    JSValueConst argv[1];
    JSValue fn;

    DCHECK(g_id_select_task >= 0,
           "§4.10.20's `select` task was queued before text_control_selection_declare declared its machine");
    /* THE CALLEE IS MINTED IN THE ENQUEUING REALM: a C function runs in the realm that DEFINED it, and this one
       fires an event at an element of THIS document. */
    fn = JS_NewCFunction2(ctx, NULL, "fireSelect", 1, JS_CFUNC_step, g_id_select_task);
    CHECK(!JS_IsException(fn), "§4.10.20's `select` task callee could not be allocated");
    argv[0] = wrap;
    JS_EnqueueCallTask(ctx, fn, 1, argv);   /* §4.10.20: the user interaction task source */
    JS_FreeValue(ctx, fn);
}

/* ---- §4.10.20's SET THE SELECTION RANGE — the algorithm every other member ends in ------------------------- */

/* "To set the selection range with an INTEGER OR NULL start, an INTEGER OR NULL OR THE SPECIAL VALUE INFINITY
   end, and optionally a string direction". The infinity is spelled as the standard spells it — `select()` is
   its one caller — rather than as a sentinel inside the uint32, which would hide a third state inside a number
   the arithmetic below also reads.
   THE NULL IS NOT SPELLED, BECAUSE NO CALLER CAN PRODUCE ONE, and the proof is short enough to check: the only
   operands that could be null are the ATTRIBUTE VALUES the three offset setters pass ("Let end be the value of
   this element's selectionEnd attribute"), and a getter returns null exactly when the member DOES NOT APPLY —
   which every one of those setters has already thrown an "InvalidStateError" for, one step earlier. The other
   three callers pass computed integers or infinity. So steps 1 and 2 ("If start is null, let start be 0", "If
   end is null, let end be 0") have no operand that reaches them here, and an unreachable arm carrying the
   standard's sentence would read like a state this engine can be in. */
typedef struct { bool infinity; uint32_t v; } TcsOffset;

static TcsOffset tcs_off(uint32_t v)    { TcsOffset o = { false, v }; return o; }
static TcsOffset tcs_off_infinity(void) { TcsOffset o = { true, 0 }; return o; }

/* §4.10.20's set the selection range, in full. `direction` is BORROWED and JS_UNINITIALIZED means "the
   direction argument was not given", which step 3 reads as its own case. */
static void tcs_set_selection_range(JSContext *ctx, JSValueConst wrap, const TcsControl *c,
                                    TcsOffset start, TcsOffset end, JSValueConst direction)
{
    uint32_t len = tcs_relevant_length(ctx, wrap, c);
    uint32_t s, e, old_s, old_e;
    JSValueConst old_dir;
    JSValue dir;
    bool changed;

    DCHECK(!start.infinity,
           "§4.10.20's set the selection range declares INFINITY for its `end` argument only — a start of "
           "infinity is a caller that has swapped the two");
    s = start.v;
    e = end.infinity ? len : end.v;
    /* Step 3: "Arguments greater than the length of the relevant value of the text control (including the
       special value infinity) must be treated as pointing at the end of the text control." */
    if (s > len) s = len;
    if (e > len) e = len;
    /* Step 3 again: "If end is less than or equal to start, then the start of the selection and the end of the
       selection must both be placed immediately before the character with offset end." Note it is END that
       wins, not start — `setSelectionRange(9, 2)` puts the cursor at 2 and not at 9. */
    if (e <= s) s = e;

    old_s = tcs_slot_offset(ctx, c->el, TCS_START);
    old_e = tcs_slot_offset(ctx, c->el, TCS_END);
    old_dir = tcs_slot_direction(c->el);

    /* Steps 4 and 5: "If direction is not identical to either \"backward\" or \"forward\", or if the direction
       argument was not given, set direction to \"none\". Set the selection direction of the text control to
       direction" — and "to set the selection direction ... unless the direction is \"none\" and the platform
       does not support that direction", which this platform does, so the unless never fires.
       AN UNKNOWN DIRECTION STAYS UNKNOWN. Its domain is the three tokens and this engine cannot say which, so
       collapsing it to "none" here would delete the forward/backward worlds and answer a later
       `=== "forward"` with a concrete false. The slot keeps the unknown; the ordinary comparison machinery
       forks when the page asks. */
    if (JS_IsUninitialized(direction) || JS_IsUndefined(direction)) {
        dir = JS_NewString(ctx, TCS_DIR_NONE);
    } else if (concolic_is(direction)) {
        dir = JS_DupValue(ctx, direction);
    } else {
        const char *d = JS_ToCString(ctx, direction);
        bool named;

        CHECK(d != NULL, "text control selection: a selection direction could not be read");
        named = strcmp(d, TCS_DIR_FORWARD) == 0 || strcmp(d, TCS_DIR_BACKWARD) == 0;
        dir = JS_NewString(ctx, named ? d : TCS_DIR_NONE);
        JS_FreeCString(ctx, d);
    }
    CHECK(!JS_IsException(dir), "text control selection: a selection direction could not be allocated");

    /* Step 6: "If the previous steps caused the selection of the text control to be MODIFIED (IN EITHER EXTENT
       OR DIRECTION), then queue an element task ... to fire an event named select". Both halves are asked, and
       an UNKNOWN on either side counts as modified — uncertainty keeps the arm, which here means the page's
       `select` listener runs rather than being silently skipped on a comparison the engine cannot decide. */
    changed = s != old_s || e != old_e;
    if (!changed) {
        if (concolic_is(dir) || concolic_is(old_dir)) {
            /* TWO UNKNOWN DIRECTIONS ARE THE SAME DIRECTION ONLY WHEN THEY ARE THE SAME VALUE, and that case
               is the common one rather than a curiosity: `selectionStart`'s setter passes "the value of this
               element's selectionDirection attribute" straight back in, so the operand IS the object already
               stored. Firing `select` for it would be an event no browser fires. Anything else is a
               comparison this engine cannot decide, and undecided counts as modified. */
            changed = !JS_IsSameValue(ctx, dir, old_dir);
        } else {
            const char *a = JS_IsUndefined(old_dir) ? NULL : JS_ToCString(ctx, old_dir);
            const char *b = JS_ToCString(ctx, dir);

            CHECK(b != NULL, "text control selection: a selection direction could not be compared");
            changed = strcmp(a ? a : TCS_DIR_NONE, b) != 0;
            if (a) JS_FreeCString(ctx, a);
            JS_FreeCString(ctx, b);
        }
    }

    tcs_store_offset(ctx, c->el, TCS_START, s);
    tcs_store_offset(ctx, c->el, TCS_END, e);
    dom_cow_set_prop_taint(ctx, c->el, TCS_DIR, dir);
    JS_FreeValue(ctx, dir);
    if (changed) tcs_queue_select_event(ctx, wrap);
}

/* ---- the two operations the value setters owe this component ---------------------------------------------- */

/* THE RECEIVER OF THE TWO EXPORTED OPERATIONS, which are reached from the value setters rather than from a
   member, so there is no install magic to state which interface this is — the node says. Answers false for an
   element with no text entry cursor position, which is what §4.10.5.4 step 5's own condition asks. */
static bool tcs_control_of(JSValueConst wrap, TcsControl *out)
{
    lxb_dom_node_t *n = node_of(wrap);

    out->is_textarea = html_form_is_textarea(n);
    out->st = out->is_textarea ? INPUT_STATE_NONE : html_form_input_state(n);
    out->el = NULL;
    if (!out->is_textarea && !tcs_offsets_apply(out->st)) return false;
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    out->el = lxb_dom_interface_element(n);
    return true;
}

void text_control_selection_value_changed(JSContext *ctx, JSValueConst wrap)
{
    TcsControl c;
    uint32_t len, s, e;

    if (!tcs_control_of(wrap, &c)) return;
    /* "If the element has a selection: if the start of the selection is now past the end of the relevant value,
       set it to the end of the relevant value; if the end of the selection is now past the end ... Otherwise,
       the element must have a text entry cursor position ... If it is now past the end of the relevant value,
       set it to the end."
       THE TWO BRANCHES ARE ONE CLAMP HERE, and that is a fact about this agent rather than a shortcut: a
       cursor is a selection whose start and end are equal (this user agent supports empty selection, which is
       the "In UAs where there is no concept of an empty selection" arm NOT taken at set the selection range),
       so clamping both offsets performs the selection branch and the cursor branch identically. The third
       sub-step, "If the user agent does not support empty selection", is the same arm and is likewise not
       taken. */
    len = tcs_relevant_length(ctx, wrap, &c);
    s = tcs_slot_offset(ctx, c.el, TCS_START);
    e = tcs_slot_offset(ctx, c.el, TCS_END);
    if (s > len) tcs_store_offset(ctx, c.el, TCS_START, len);
    if (e > len) tcs_store_offset(ctx, c.el, TCS_END, len);
}

void text_control_selection_move_to_end(JSContext *ctx, JSValueConst wrap)
{
    TcsControl c;
    uint32_t len;
    JSValue none;

    if (!tcs_control_of(wrap, &c)) return;
    /* "move the text entry cursor position to the end of the text control, unselecting any selected text and
       resetting the selection direction to \"none\"" — written out rather than routed through set the selection
       range, because that algorithm would fire a `select` event and this step is not one of its callers. */
    len = tcs_relevant_length(ctx, wrap, &c);
    tcs_store_offset(ctx, c.el, TCS_START, len);
    tcs_store_offset(ctx, c.el, TCS_END, len);
    none = JS_NewString(ctx, TCS_DIR_NONE);
    CHECK(!JS_IsException(none), "text control selection: the `none` direction could not be allocated");
    dom_cow_set_prop_taint(ctx, c.el, TCS_DIR, none);
    JS_FreeValue(ctx, none);
}

/* ---- the members ------------------------------------------------------------------------------------------ */

/* THE OFFSET AN ARGUMENT DENOTES. §3.2's conversion has already run, so this argument is a Number or unknown
   external input; `idl_number_of` answers the second from the value's own example through the one copy of
   §3.2.4.5's arithmetic, and 0 when there is no example. That 0 is this member's answer to the question that
   header hands back to the caller: an offset the run never computed leaves the cursor where §4.10.20 starts
   every control, at the beginning. */
static uint32_t tcs_offset_operand(JSContext *ctx, JSValueConst v)
{
    double d = 0;

    if (!idl_number_of(ctx, IDL_UNSIGNED_LONG, v, &d)) return 0;
    DCHECK(d >= 0 && d <= (double)UINT32_MAX,
           "§3.2.4's `unsigned long` conversion produced a value outside the type's range — the declaration "
           "ran the modulo before the body saw it");
    return (uint32_t)d;
}

/* §4.10.20's three offset GETTERS. Each has the same three steps and differs only in which offset it answers,
   which is why they are one body: "If this element is an input element, and X does not apply to this element,
   return null. If there is no selection, return the code unit offset ... to the character that immediately
   follows the text entry cursor. Return the code unit offset ... to the ... of the selection."
   THE SECOND AND THIRD STEPS COLLAPSE and it is not an approximation: a text entry cursor is a selection whose
   start and end are equal, so "immediately follows the cursor" and "the start of the selection" name the same
   stored number in both cases. */
static JSValue js_tcs_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    int m = TCS_MEMBER(magic);
    const char *member = m == TCS_M_START ? "selectionStart"
                       : m == TCS_M_END   ? "selectionEnd" : "selectionDirection";
    TcsControl c;

    if (!tcs_receiver(ctx, this_val, magic, member, &c)) return JS_EXCEPTION;
    /* "If this element is an input element, and X does not apply to this element, return null." A textarea
       never asks — the standard's condition names input elements, and every textarea has these members. */
    if (!c.is_textarea && !tcs_offsets_apply(c.st)) return JS_NULL;
    if (m == TCS_M_DIRECTION) {
        JSValueConst d = tcs_slot_direction(c.el);

        /* "Return this element's selection direction", whose initial value is "none". */
        return JS_IsUndefined(d) ? JS_NewString(ctx, TCS_DIR_NONE) : JS_DupValue(ctx, d);
    }
    DCHECK(m == TCS_M_START || m == TCS_M_END,
           "§4.10.20's shared offset getter was installed for a member that is not one of its three");
    return JS_NewUint32(ctx, tcs_slot_offset(ctx, c.el, m == TCS_M_START ? TCS_START : TCS_END));
}

/* §4.10.20's `selectionStart` SETTER: "If this element is an input element, and selectionStart does not apply
   to this element, throw an \"InvalidStateError\" DOMException. Let end be the value of this element's
   selectionEnd attribute. If end is less than the given value, set end to the given value. Set the selection
   range with the given value, end, and the value of this element's selectionDirection attribute." */
static JSValue js_tcs_set_start(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    TcsControl c;
    uint32_t start, end;

    if (!tcs_receiver(ctx, this_val, magic, "selectionStart", &c)) return JS_EXCEPTION;
    if (!c.is_textarea && !tcs_offsets_apply(c.st))
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "`selectionStart` does not apply to an `input` in this state");
    start = tcs_offset_operand(ctx, val);
    end = tcs_slot_offset(ctx, c.el, TCS_END);
    if (end < start) end = start;
    tcs_set_selection_range(ctx, this_val, &c, tcs_off(start), tcs_off(end), tcs_slot_direction(c.el));
    return JS_UNDEFINED;
}

/* §4.10.20's `selectionEnd` SETTER: the same shape with no clamp of its own — "Set the selection range with the
   value of this element's selectionStart attribute, the given value, and ... selectionDirection". The
   asymmetry is the standard's: set the selection range's own step 3 collapses an end below the start, so the
   end setter needs no adjustment where the start setter does. */
static JSValue js_tcs_set_end(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    TcsControl c;

    if (!tcs_receiver(ctx, this_val, magic, "selectionEnd", &c)) return JS_EXCEPTION;
    if (!c.is_textarea && !tcs_offsets_apply(c.st))
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "`selectionEnd` does not apply to an `input` in this state");
    tcs_set_selection_range(ctx, this_val, &c, tcs_off(tcs_slot_offset(ctx, c.el, TCS_START)),
                            tcs_off(tcs_offset_operand(ctx, val)), tcs_slot_direction(c.el));
    return JS_UNDEFINED;
}

/* §4.10.20's `selectionDirection` SETTER: "Set the selection range with ... selectionStart, ... selectionEnd,
   and the given value."
   THE IDL SAYS `DOMString?` ON HTMLInputElement AND `DOMString` ON HTMLTextAreaElement, and both are declared
   here as IDL_DOMSTRING. That is not the nullable row being ignored: §3.2.20 maps null to the IDL null and
   IDL_DOMSTRING maps it through ToString to "null", and set the selection range's step 4 sends EVERYTHING that
   is not identical to "forward" or "backward" to "none" — so the two conversions reach the same stored
   direction by different routes, and there is no operand for which they differ. */
static JSValue js_tcs_set_direction(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    TcsControl c;

    if (!tcs_receiver(ctx, this_val, magic, "selectionDirection", &c)) return JS_EXCEPTION;
    if (!c.is_textarea && !tcs_offsets_apply(c.st))
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "`selectionDirection` does not apply to an `input` in this state");
    tcs_set_selection_range(ctx, this_val, &c, tcs_off(tcs_slot_offset(ctx, c.el, TCS_START)),
                            tcs_off(tcs_slot_offset(ctx, c.el, TCS_END)), val);
    return JS_UNDEFINED;
}

/* §4.10.20's `select()`: "If this element is an input element, and either select() does not apply to this
   element or the corresponding control has no selectable text, return. Set the selection range with 0 and
   infinity." */
static JSValue js_tcs_select(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    TcsControl c;

    (void)argc; (void)argv;
    if (!tcs_receiver(ctx, this_val, magic, "select", &c)) return JS_EXCEPTION;
    if (!c.is_textarea && (!tcs_select_applies(c.st) || !tcs_has_selectable_text(c.st))) return JS_UNDEFINED;
    tcs_set_selection_range(ctx, this_val, &c, tcs_off(0), tcs_off_infinity(), JS_UNINITIALIZED);
    return JS_UNDEFINED;
}

/* §4.10.20's `setSelectionRange(start, end, direction)`: "If this element is an input element, and
   setSelectionRange() does not apply to this element, throw an \"InvalidStateError\" DOMException. Set the
   selection range with start, end, and direction." */
static JSValue js_tcs_set_selection_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                          int magic)
{
    TcsControl c;

    if (!tcs_receiver(ctx, this_val, magic, "setSelectionRange", &c)) return JS_EXCEPTION;
    if (!c.is_textarea && !tcs_offsets_apply(c.st))
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "`setSelectionRange()` does not apply to an `input` in this state");
    /* The third argument is `optional DOMString direction` with NO default, so an absent one is step 4's "the
       direction argument was not given" and an explicit `undefined` is the same case — which is what the
       declaration's idl_optional_from produces and why this asks argc rather than converting a hole. */
    tcs_set_selection_range(ctx, this_val, &c, tcs_off(tcs_offset_operand(ctx, argv[0])),
                            tcs_off(tcs_offset_operand(ctx, argv[1])),
                            argc >= 3 ? argv[2] : JS_UNINITIALIZED);
    return JS_UNDEFINED;
}

/* ---- §4.10.20's `setRangeText()` --------------------------------------------------------------------------- */

/* THE SPLICE: the relevant value with the code units in [start, end) replaced by `replacement`. §4.10.20 states
   it as two steps ("delete the sequence of code units ... starting with the code unit at the startth position
   and ending with the code unit at the (end-1)th position", then "Insert the value of the first argument into
   the text of the relevant value ... immediately before the startth code unit"), which over one string is one
   operation and is written as one so the intermediate state cannot be observed by a reader either.
   IT IS DONE IN UTF-16 because §4.10.20 counts code units — see tcs_relevant_length.
   TAINT SURVIVES IT. When either side is unknown the result is unknown, derived by the member's own name with
   the REAL splice over the operands' examples as its example, which is what carries
   `input.setRangeText(location.hash)` through to §4.10.22.4's entry list as the source it came from. OWNED. */
static JSValue tcs_splice(JSContext *ctx, JSValueConst value, uint32_t start, uint32_t end,
                          JSValueConst replacement, uint32_t *new_len)
{
    JSValue vex = concolic_is(value) ? concolic_example(ctx, value) : JS_DupValue(ctx, value);
    JSValue rex = concolic_is(replacement) ? concolic_example(ctx, replacement)
                                           : JS_DupValue(ctx, replacement);
    const uint16_t *v = NULL, *r = NULL;
    size_t vn = 0, rn = 0, on;
    uint16_t *out;
    JSValue spliced;

    if (JS_IsString(vex)) {
        v = JS_ToCStringLenUTF16(ctx, &vn, vex);
        CHECK(v != NULL, "setRangeText: the relevant value could not be read in code units");
    }
    if (JS_IsString(rex)) {
        r = JS_ToCStringLenUTF16(ctx, &rn, rex);
        CHECK(r != NULL, "setRangeText: the replacement could not be read in code units");
    }
    DCHECK(start <= end && end <= vn,
           "§4.10.20's setRangeText reached its splice with offsets outside the relevant value — its own steps "
           "clamp both to the length before this runs");
    on = vn - (end - start) + rn;
    out = malloc((on + 1) * sizeof *out);
    CHECK(out != NULL, "setRangeText: OOM building the spliced value");
    /* THE THREE COPIES ARE GUARDED ON THEIR LENGTHS because `v` and `r` are NULL where the operand carried no
       string to measure (an unknown with no example yet), and `memcpy(dst, NULL, 0)` and the pointer
       arithmetic `v + end` on a null pointer are both undefined behaviour in C even at length zero. */
    if (start) memcpy(out, v, start * sizeof *out);
    if (rn) memcpy(out + start, r, rn * sizeof *out);
    if (vn - end) memcpy(out + start + rn, v + end, (vn - end) * sizeof *out);
    spliced = JS_NewStringUTF16(ctx, out, on);
    free(out);
    if (v) JS_FreeCStringUTF16(ctx, v);
    if (r) JS_FreeCStringUTF16(ctx, r);
    JS_FreeValue(ctx, vex);
    JS_FreeValue(ctx, rex);
    CHECK(!JS_IsException(spliced), "setRangeText: the spliced value could not be allocated");
    /* STEP 11's "new length", answered HERE and nowhere else. It is the length of the replacement AS THIS
       SPLICE MEASURED IT, and a second reading of the same argument beside the caller's arithmetic is two
       answers to one question — which for an unknown operand is not even the same question twice, since a
       fresh `concolic_example` is a fresh read of a value the flow may have narrowed in between. */
    DCHECK(rn <= UINT32_MAX, "§4.10.20's `new length` is wider than the offsets it is added to");
    *new_len = (uint32_t)rn;
    if (!concolic_is(value) && !concolic_is(replacement)) return spliced;
    return concolic_builtin_hook(ctx, concolic_is(replacement) ? replacement : value, "setRangeText", spliced);
}

/* "Set the element's relevant value" — the write half of the splice, which is NOT the `value` IDL setter:
   §4.10.20's steps set the dirty value flag themselves and never invoke the value sanitization algorithm, and
   routing through the setter would also run its own final step and move the cursor to the end, which
   setRangeText's last step is about to contradict. */
static void tcs_set_relevant_value(JSContext *ctx, JSValueConst wrap, const TcsControl *c, JSValueConst v)
{
    if (c->is_textarea) html_form_textarea_set_raw_value(ctx, wrap, v);
    else                input_value_set_relevant(ctx, wrap, v);
}

/* §4.10.20's `setRangeText(replacement [, start, end [, selectionMode ] ])`, in full. */
static JSValue js_tcs_set_range_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                     int magic)
{
    TcsControl c;
    uint32_t start, end, len, sel_start, sel_end, new_len, new_end;
    JSValue value, spliced;
    const char *mode = NULL;
    bool one_arg;

    if (!tcs_receiver(ctx, this_val, magic, "setRangeText", &c)) return JS_EXCEPTION;
    /* Step 1. */
    if (!c.is_textarea && !tcs_offsets_apply(c.st))
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "`setRangeText()` does not apply to an `input` in this state");
    /* WEB IDL §3.6 "Overload resolution" BY ARGUMENT COUNT, which is what step 3 then reads back ("If the
       method has only one argument..."). The IDL declares `setRangeText(DOMString replacement)` and
       `setRangeText(DOMString replacement, unsigned long start, unsigned long end, optional SelectionMode
       selectionMode = "preserve")`, and the declaration states that split so the MACHINE resolves it: an
       argument count of 2 is refused by §3.6 step 5 before this body runs (the surviving longer entry requires
       three), and positions 1 and 2 are read as REQUIRED at every count that reaches the longer entry.
       THE COUNT REACHING THIS BODY IS THE PAGE'S, AND THAT IS AN ASSUMPTION WITH A TELL. The argument machine
       EXTENDS the count it hands a body past what the page passed for any position carrying a declared
       default (§3.6 step 16.1), and step 3 above is the one algorithm that reads the count itself — so this
       member declares no default at position 3, and the assert below is what fires if one is ever added
       rather than the one-argument form silently becoming the four-argument one. */
    DCHECK(argc != 2, "§3.6 step 5 refuses `setRangeText()` at an argument count of 2 before any body runs — "
                      "the longer entry requires three and the shorter one was removed at step 4");
    DCHECK(argc <= 1 || !JS_IsUndefined(argv[1]),
           "§4.10.20's setRangeText reached its body at an argument count past the overload split with no "
           "`start` — position 1 is REQUIRED in the entry §3.6 leaves at that count, so an `undefined` there "
           "means the count was extended by a declared default and no longer states what the page passed, "
           "which is the one thing step 3's \"if the method has only one argument\" reads");
    one_arg = argc <= 1;
    len = tcs_relevant_length(ctx, this_val, &c);
    /* Step 2: "Set this element's dirty value flag to true." For an `input` the flag and the element's own
       value are one fact (input_value.c's `iv_dirty` states why), so the write below IS this step; a textarea
       keeps them apart and html_form.c's raw-value write sets both. */
    /* Step 3. */
    sel_start = tcs_slot_offset(ctx, c.el, TCS_START);
    sel_end = tcs_slot_offset(ctx, c.el, TCS_END);
    if (one_arg) {
        start = sel_start;
        end = sel_end;
    } else {
        start = tcs_offset_operand(ctx, argv[1]);
        end = tcs_offset_operand(ctx, argv[2]);
    }
    /* Step 4: "If start is greater than end, then throw an \"IndexSizeError\" DOMException." It is asked
       BEFORE the two clamps below, so `setRangeText("x", 9, 3)` throws even on a control whose value is
       shorter than both — the order is the standard's and the two orders disagree. */
    if (start > end)
        return JS_ThrowDOMException(ctx, "IndexSizeError",
                                    "`setRangeText()` was given a start greater than its end");
    /* Steps 5 and 6. */
    if (start > len) start = len;
    if (end > len) end = len;
    /* Steps 7 and 8 read the CURRENT selection, which steps 5 and 6 did not touch — they are already in hand
       above, taken before the clamps for the same reason the standard takes them from the attributes. */
    /* Steps 9, 10 and 11, as one splice — see tcs_splice, which also answers step 11's `new length`. */
    value = tcs_relevant_value(ctx, this_val, &c);
    spliced = tcs_splice(ctx, value, start, end, argv[0], &new_len);
    JS_FreeValue(ctx, value);
    tcs_set_relevant_value(ctx, this_val, &c, spliced);
    JS_FreeValue(ctx, spliced);
    /* Step 12: "Let new end be the sum of start and new length." */
    new_end = start + new_len;
    /* Step 13's four sub-lists. */
    if (!one_arg && argc >= 4 && !JS_IsUndefined(argv[3])) {
        mode = JS_ToCString(ctx, argv[3]);
        CHECK(mode != NULL, "setRangeText: the selection mode could not be read");
    }
    if (mode && strcmp(mode, "select") == 0) {
        sel_start = start;
        sel_end = new_end;
    } else if (mode && strcmp(mode, "start") == 0) {
        sel_start = sel_end = start;
    } else if (mode && strcmp(mode, "end") == 0) {
        sel_start = sel_end = new_end;
    } else {
        /* "preserve", and the one-argument form, which the standard gives the SAME sub-list. `old length` is
           end minus start and `delta` is new length minus old length, both computed in the signed width the
           standard's arithmetic needs — a shorter replacement makes delta negative and the two increments
           below then DECREASE the offsets, which is exactly what its parenthetical says. */
        int64_t old_length = (int64_t)end - (int64_t)start;
        int64_t delta = (int64_t)new_len - old_length;

        DCHECK(mode == NULL || strcmp(mode, "preserve") == 0,
               "§4.10.20's SelectionMode enumeration has four values and setRangeText was handed a fifth — the "
               "position is declared IDL_ENUM and §3.2.18's membership test runs before this body");
        if (sel_start > end) sel_start = (uint32_t)((int64_t)sel_start + delta);
        else if (sel_start > start) sel_start = start;
        if (sel_end > end) sel_end = (uint32_t)((int64_t)sel_end + delta);
        else if (sel_end > start) sel_end = new_end;
    }
    if (mode) JS_FreeCString(ctx, mode);
    /* Step 14: "Set the selection range with selection start and selection end." No direction argument, so it
       is step 4's "the direction argument was not given" and the direction resets to "none". */
    tcs_set_selection_range(ctx, this_val, &c, tcs_off(sel_start), tcs_off(sel_end), JS_UNINITIALIZED);
    return JS_UNDEFINED;
}

/* ---- declaration and install ------------------------------------------------------------------------------ */

/* TWELVE IDS AND NOT SIX. The magic carries the declaring interface (see its enum above) and the magic is baked
   into the declaration, so each of the six members is declared once per interface — which is what makes
   `HTMLInputElement.prototype.setRangeText.call(aTextarea)` the TypeError Web IDL §3.7.5 requires instead of a
   splice performed on the wrong control's value. */
static int g_id_start_in = -1, g_id_start_ta = -1;
static int g_id_end_in = -1, g_id_end_ta = -1;
static int g_id_dir_in = -1, g_id_dir_ta = -1;
static int g_id_select_in = -1, g_id_select_ta = -1;
static int g_id_ssr_in = -1, g_id_ssr_ta = -1;
static int g_id_srt_in = -1, g_id_srt_ta = -1;

IDL_ENUM_VALUES(TCS_SELECTION_MODE, "select", "start", "end", "preserve");

void text_control_selection_declare(JSContext *ctx)
{
    /* `setSelectionRange(unsigned long start, unsigned long end, optional DOMString direction)` — note the
       first two are NOT nullable in the IDL even though set the selection range's own prose takes "an integer
       or null": the null arrives from the two offset SETTERS, which pass an attribute value, and never across
       this member's boundary. */
    static const IdlArgType SSR_ARGS[3] = { IDL_UNSIGNED_LONG, IDL_UNSIGNED_LONG, IDL_DOMSTRING };
    /* `setRangeText(DOMString replacement, unsigned long start, unsigned long end, optional SelectionMode
       selectionMode = "preserve")` — the longer of the two overload entries, declared once with everything
       after the first position optional so the ARITY the page called with reaches the body, which is where
       §3.6's count-based choice between the two entries is made (core/html/document_write.c's `open` is the
       same shape and its comment says the same thing). */
    static const IdlArgType SRT_ARGS[4] = { IDL_DOMSTRING, IDL_UNSIGNED_LONG, IDL_UNSIGNED_LONG, IDL_ENUM };

    DCHECK(g_id_select_task < 0,
           "text_control_selection_declare ran twice — one instance is one agent");
    g_id_select_task = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_tcs_select_def);
    DCHECK(g_id_select_task >= 0, "§4.10.20's `select` task machine could not be registered");

    /* The three attribute setters, per interface. Both interfaces declare `unsigned long` offsets; only
       HTMLInputElement's are nullable, and js_tcs_set_direction's comment states why that row converts
       identically here. */
    g_id_start_in = idl_setter_id(ctx, IDL_UNSIGNED_LONG, false, js_tcs_set_start, TCS_M_START);
    g_id_start_ta = idl_setter_id(ctx, IDL_UNSIGNED_LONG, false, js_tcs_set_start,
                                  TCS_M_START | TCS_TEXTAREA);
    g_id_end_in = idl_setter_id(ctx, IDL_UNSIGNED_LONG, false, js_tcs_set_end, TCS_M_END);
    g_id_end_ta = idl_setter_id(ctx, IDL_UNSIGNED_LONG, false, js_tcs_set_end, TCS_M_END | TCS_TEXTAREA);
    g_id_dir_in = idl_setter_id(ctx, IDL_DOMSTRING, false, js_tcs_set_direction, TCS_M_DIRECTION);
    g_id_dir_ta = idl_setter_id(ctx, IDL_DOMSTRING, false, js_tcs_set_direction,
                                TCS_M_DIRECTION | TCS_TEXTAREA);

    g_id_select_in = idl_method_id(ctx, NULL, 0, js_tcs_select, TCS_M_SELECT);
    g_id_select_ta = idl_method_id(ctx, NULL, 0, js_tcs_select, TCS_M_SELECT | TCS_TEXTAREA);

    g_id_ssr_in = idl_method_id(ctx, SSR_ARGS, 3, js_tcs_set_selection_range, TCS_M_SET_RANGE);
    idl_optional_from(2);
    g_id_ssr_ta = idl_method_id(ctx, SSR_ARGS, 3, js_tcs_set_selection_range, TCS_M_SET_RANGE | TCS_TEXTAREA);
    idl_optional_from(2);

    /* §3.6's LENGTH-DIFFERING SPLIT, whose two entries share their type at the split (position 0 is DOMString
       in both), which is the shape idl_overload_length_split_at exists for. The shorter entry's last position
       is 0 and its only argument is required, so `idl_optional_from(1)`; the longer entry requires `start` and
       `end` and makes only `selectionMode` optional, so its own first optional index is 3. Without both,
       `setRangeText("x", undefined, 2)` would read position 1 as an ABSENT optional against the SHORTER
       entry's number and hand the body nothing where §3.2.4 owes it ToNumber(undefined) — and an argument
       count of 2 would be a call with a hole instead of §3.6 step 5's TypeError.
       §3.7.7 Operations' `length` is the minimum over the effective overload set, which these two numbers make
       1 — `setRangeText.length` is the shorter entry's single required argument. */
    g_id_srt_in = idl_method_id(ctx, SRT_ARGS, 4, js_tcs_set_range_text, TCS_M_SET_RANGE_TEXT);
    idl_optional_from(1);
    idl_overload_length_split_at(0);
    idl_overload_split_optional_from(3);
    idl_arg_enum(3, TCS_SELECTION_MODE);   /* §3.2.18's values for the `selectionMode` position */
    g_id_srt_ta = idl_method_id(ctx, SRT_ARGS, 4, js_tcs_set_range_text,
                                TCS_M_SET_RANGE_TEXT | TCS_TEXTAREA);
    idl_optional_from(1);
    idl_overload_length_split_at(0);
    idl_overload_split_optional_from(3);
    idl_arg_enum(3, TCS_SELECTION_MODE);
}

void text_control_selection_install(JSContext *ctx, JSValueConst input_proto, JSValueConst textarea_proto)
{
    DCHECK(g_id_start_in >= 0, "§4.10.20's members were installed before they were declared");
    DCHECK(JS_IsObject(input_proto) && JS_IsObject(textarea_proto),
           "§4.10.20's members were installed without both of the prototypes that declare them");
    idl_install_accessor(ctx, input_proto, "selectionStart", js_tcs_get, TCS_M_START, g_id_start_in);
    idl_install_accessor(ctx, textarea_proto, "selectionStart", js_tcs_get,
                         TCS_M_START | TCS_TEXTAREA, g_id_start_ta);
    idl_install_accessor(ctx, input_proto, "selectionEnd", js_tcs_get, TCS_M_END, g_id_end_in);
    idl_install_accessor(ctx, textarea_proto, "selectionEnd", js_tcs_get,
                         TCS_M_END | TCS_TEXTAREA, g_id_end_ta);
    idl_install_accessor(ctx, input_proto, "selectionDirection", js_tcs_get, TCS_M_DIRECTION, g_id_dir_in);
    idl_install_accessor(ctx, textarea_proto, "selectionDirection", js_tcs_get,
                         TCS_M_DIRECTION | TCS_TEXTAREA, g_id_dir_ta);
    idl_install_method(ctx, input_proto, "select", g_id_select_in);
    idl_install_method(ctx, textarea_proto, "select", g_id_select_ta);
    idl_install_method(ctx, input_proto, "setSelectionRange", g_id_ssr_in);
    idl_install_method(ctx, textarea_proto, "setSelectionRange", g_id_ssr_ta);
    idl_install_method(ctx, input_proto, "setRangeText", g_id_srt_in);
    idl_install_method(ctx, textarea_proto, "setRangeText", g_id_srt_ta);
}

void text_control_selection_free(JSRuntime *rt)
{
    (void)rt;
    g_id_select_task = -1;
    g_id_start_in = g_id_start_ta = g_id_end_in = g_id_end_ta = g_id_dir_in = g_id_dir_ta = -1;
    g_id_select_in = g_id_select_ta = g_id_ssr_in = g_id_ssr_ta = g_id_srt_in = g_id_srt_ta = -1;
}
