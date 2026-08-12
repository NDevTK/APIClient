/* CONSTRUCTING THE ENTRY LIST — HTML §4.10.22.4.
 *
 * WHY THIS IS ITS OWN FILE. Two standards' members converge on this ONE algorithm — HTML's "submit a form"
 * (§4.10.21.3 step 16) and XHR §5's `new FormData(form, submitter)` — and a third thing, §4.13.7.3's ENTRY
 * CONSTRUCTION for a form-associated custom element, is a step INSIDE it. Written into either caller it would
 * have had to be written into both, and the two would have drifted the way every duplicated walk in this tree
 * has: one of them would have grown the `select` branch and the other would not.
 *
 * IT IS A SUSPENSION POINT, and that is the fact that shapes everything here. Step 7 fires a `formdata` event
 * at the form, and a page's handler runs with a live handle on the list being built:
 *
 *     form.addEventListener('formdata', e => e.formData.append('csrf', token));
 *
 * so the entries that handler appends ARE part of the request. Running that dispatch from a C body would be the
 * drive-to-completion this engine aborts on, which is why `form.submit()` — a plain C body until this landed on
 * the claim that "nothing on this path reaches the page's code" — is a machine now too.
 *
 * THE ENTRY LIST IS A JS VALUE. Step 6 makes "a new FormData object ASSOCIATED WITH entry list", so the list
 * and the FormData are one object by the time anything can see either; this builds into the FormData from step
 * 4 onward. §State-isolation requires exactly that of it — the list lives on the running flow's step state, so
 * it forks per flow and parks to the IDB cold tier with the flow that is building it, which a malloc'd C list
 * on the side could not do and which matters here more than usual: a `formdata` handler that forks leaves two
 * arms each appending their own entries.
 *
 * WHAT IS HONESTLY ABSENT, BY NAME:
 *   - `<input type=file>`'s SELECTED FILES are always empty, and that is not a gap: this engine has no file
 *     picker and no user, so step 5.8's "if there are no selected files" branch is the whole of what it can
 *     ever take. That branch is implemented, so a named file control still contributes its entry. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/events/event_target.h"
#include "core/file/blob.h"
#include "core/html/form_data.h"
#include "core/html/form_data_event.h"
#include "core/html/form_entry_list.h"
#include "core/html/directionality.h"
#include "core/html/element_internals.h"
#include "core/html/html_form.h"
#include "core/dom/node.h"
#include "solver/concolic.h"

/* This sub-sequence's own cursor. It is a phase and not a STAGE for the reason every other `*_run` helper's is:
   the machine that performs it rests at ONE of its own stages for the whole sub-sequence, and this says where
   inside that stage the sub-sequence resumed. */
enum { FEL_START = 0, FEL_WALK, FEL_FIRE, FEL_DONE };

/* ---- §4.10.22.4 steps 1, 2 and 8: the form's "constructing entry list" ------------------------------------
 *
 * A boolean ON THE FORM, held as an own slot on its wrapper under a Symbol this component minted and never
 * published — so it is per-flow for free (a slot written as a property write is captured by the COW delta) and
 * two arms that submit the same form each have their own. It exists to make the algorithm non-reentrant: a
 * `formdata` handler that calls `new FormData(theSameForm)` gets step 1's null, which XHR §5 step 1.3 turns
 * into an InvalidStateError. */
static JSValue g_building_key = JS_UNDEFINED;
static JSAtom  g_atom_building = JS_ATOM_NULL;

void form_entry_list_declare(JSContext *ctx)
{
    DCHECK(JS_IsUndefined(g_building_key), "form_entry_list_declare ran twice — one instance is one agent");
    g_building_key = JS_NewSymbol(ctx, "constructingEntryList", false);
    CHECK(!JS_IsException(g_building_key), "the constructing-entry-list slot key allocation failed");
    g_atom_building = JS_ValueToAtom(ctx, g_building_key);
    CHECK(g_atom_building != JS_ATOM_NULL, "the constructing-entry-list slot key could not be interned");
}

void form_entry_list_free(JSContext *ctx)
{
    /* The AGENT's, so it is released with the agent — a Symbol nobody frees is a live GC object the runtime's
       own walk counts as a leak. */
    JS_FreeAtom(ctx, g_atom_building);
    g_atom_building = JS_ATOM_NULL;
    JS_FreeValue(ctx, g_building_key);
    g_building_key = JS_UNDEFINED;
}

static bool fel_flag_of(JSContext *ctx, JSValueConst form)
{
    JSValue v;
    bool b;

    if (!JS_IsObject(form)) return false;
    if (JS_GetOwnSlot(ctx, &v, form, g_atom_building) <= 0) return false;
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

bool form_entry_list_constructing(JSContext *ctx, JSValueConst form)
{
    DCHECK(g_atom_building != JS_ATOM_NULL,
           "§4.10.21.3 step 2 asked before form_entry_list_declare minted the form's flag key");
    return fel_flag_of(ctx, form);
}

/* CONFIGURABLE AND WRITABLE, for the reason the form-owner slot is: this is written twice per construction and
   a slot defined with no flags makes the second write a silent no-op. */
static void fel_flag_set(JSContext *ctx, JSValueConst form, bool on)
{
    if (!JS_IsObject(form)) return;
    JS_DefinePropertyValue(ctx, (JSValue)form, g_atom_building, JS_NewBool(ctx, on),
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
}

/* ---- "create an entry", HTML §4.10.22.4 -------------------------------------------------------------------
 *
 * Step 2 converts a STRING value into a scalar value string, and step 3's Blob arm leaves a File as itself. A
 * CONCOLIC takes neither: it is the value the interpreter computed, carrying the source it came from, and a
 * conversion here would flatten `input.value = location.hash` into whichever text its shape happens to print.
 * `value` is CONSUMED. */
static JSValue fel_entry_value(JSContext *ctx, JSValue value)
{
    JSValue s;

    if (concolic_is(value)) return value;
    if (blob_file_name_of(value) != NULL) return value;   /* already a File — §5's create step is skipped */
    s = JS_ToString(ctx, value);
    JS_FreeValue(ctx, value);
    if (JS_IsException(s)) return s;
    return JS_ToScalarValueString(ctx, s);
}

static void fel_append(JSContext *ctx, JSValueConst entries, const char *name, size_t nlen, JSValue value)
{
    form_data_append_entry(ctx, entries, name, nlen, fel_entry_value(ctx, value));
}

/* §4.13.7.3's ENTRY CONSTRUCTION ALGORITHM, which step 5.3 performs for a form-associated custom element. It
   is the whole submission half of `setFormValue`, and the reason a FACE can carry entries the `name` attribute
   never names: a submission value that is an entry LIST is appended wholesale and the element's own name is
   not consulted at all. */
static void fel_face_entries(JSContext *ctx, JSValueConst entries, JSValueConst field)
{
    JSValue sub = element_internals_submission_value(ctx, field);
    const char *name;
    size_t nlen = 0;

    /* Step 1: the submission value is a list of entries. */
    if (form_data_is(sub)) {
        form_data_append_all(ctx, entries, sub);
        JS_FreeValue(ctx, sub);
        return;
    }
    /* Step 2, then step 3: an unnamed element, or a null submission value, contributes nothing. */
    name = html_form_control_name(field, &nlen);
    if (!name || !nlen || JS_IsNull(sub) || JS_IsUndefined(sub)) {
        JS_FreeValue(ctx, sub);
        return;
    }
    fel_append(ctx, entries, name, nlen, sub);
}

/* Step 5.2: an IMAGE BUTTON that IS the submitter contributes its selected coordinate as two entries, named
   `name.x` and `name.y` — or bare `x` and `y` when it has no name. THE COORDINATE IS (0, 0), and that is the
   real answer rather than a placeholder: §4.10.5.1.19 says the selected coordinate "is initially (0, 0)" and
   the only thing that ever moves it is a user activating the control while explicitly selecting one, which a
   headless engine has no source for. */
static void fel_image_button_entries(JSContext *ctx, JSValueConst entries, JSValueConst field)
{
    size_t nlen = 0;
    const char *name = html_form_control_name(field, &nlen);
    char *key;

    if (!name) nlen = 0;
    key = malloc(nlen + 3);
    CHECK(key != NULL, "forms: OOM building an image button's coordinate entry names");
    if (nlen) { memcpy(key, name, nlen); key[nlen] = '.'; nlen++; }
    key[nlen] = 'x';
    fel_append(ctx, entries, key, nlen + 1, JS_NewStringLen(ctx, "0", 1));
    key[nlen] = 'y';
    fel_append(ctx, entries, key, nlen + 1, JS_NewStringLen(ctx, "0", 1));
    free(key);
}

/* Step 5.8: the FILE UPLOAD state with no selected files — "create an entry with name and a new File object
   with an empty name, application/octet-stream as type, and an empty body". */
static void fel_file_entry(JSContext *ctx, JSValueConst entries, const char *name, size_t nlen)
{
    static const char OCTET[] = "application/octet-stream";
    JSValue f = file_new(ctx, "", 0, OCTET, sizeof(OCTET) - 1, "", 0, 0);

    if (JS_IsException(f)) { JS_FreeValue(ctx, f); return; }
    form_data_append_entry(ctx, entries, name, nlen, f);
}

/* ONE CONTROL — step 5's body for a single field. Nothing in it reaches the page's code: every read is a
   content attribute, an engine-held value slot or a custom element's stored submission value, so the whole of
   it runs between two yields rather than needing a stage of its own. */
static void fel_one_field(JSContext *ctx, JSValueConst entries, JSValueConst field, JSValueConst submitter,
                          const char *encoding)
{
    FormFieldKind kind;
    const char *name;
    size_t nlen = 0;

    /* Step 5.1's five skip conditions. */
    if (html_form_has_datalist_ancestor(field)) return;
    if (html_form_control_is_disabled(ctx, field)) return;
    if (html_form_is_button(ctx, field) && JS_VALUE_GET_PTR(field) != JS_VALUE_GET_PTR(submitter)) return;
    kind = html_form_field_kind(ctx, field);
    if (kind == FORM_FIELD_CHECKBOX && !html_form_control_checked(ctx, field)) return;

    /* Step 5.2, whose own "if the field element is not submitter, then continue" step 5.1 has already made. */
    if (kind == FORM_FIELD_IMAGE_BUTTON) { fel_image_button_entries(ctx, entries, field); return; }
    /* Step 5.3, BEFORE the name check: a FACE decides its own entry names, so an unnamed one can still
       contribute. */
    if (kind == FORM_FIELD_FACE) { fel_face_entries(ctx, entries, field); return; }

    /* Step 5.4. */
    name = html_form_control_name(field, &nlen);
    if (!name || !nlen) return;

    switch (kind) {
    case FORM_FIELD_SELECT: {
        /* Step 5.6: one entry per option whose selectedness is true and that is not disabled. */
        JSValue opts = html_form_selected_options(ctx, field);
        uint32_t n = 0, k;
        JSValue lv = JS_GetPropertyStr(ctx, opts, "length");

        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        for (k = 0; k < n; k++) {
            JSValue opt = JS_GetPropertyUint32(ctx, opts, k);
            fel_append(ctx, entries, name, nlen, html_form_control_value(ctx, opt));
            JS_FreeValue(ctx, opt);
        }
        JS_FreeValue(ctx, opts);
        break;
    }
    case FORM_FIELD_CHECKBOX:
        fel_append(ctx, entries, name, nlen, html_form_checkbox_value(ctx, field));   /* step 5.7 */
        break;
    case FORM_FIELD_FILE:
        fel_file_entry(ctx, entries, name, nlen);                                     /* step 5.8 */
        break;
    case FORM_FIELD_CHARSET:
        /* Step 5.9: `_charset_` carries the NAME OF THE ENCODING the submission picked, which is the one thing
           §4.10.22.5's answer is ever observable through. */
        fel_append(ctx, entries, name, nlen, JS_NewString(ctx, encoding));
        break;
    default:
        DCHECK(kind == FORM_FIELD_OTHER, "a form field was classified as a kind step 5's chain does not have");
        fel_append(ctx, entries, name, nlen, html_form_control_value(ctx, field));     /* step 5.11 */
        break;
    }

    /* STEP 5.12 — the `dirname` entry, AFTER the control's own and not before it. The order is the spec's and
       it is observable: a server reading a multipart body in order sees `comment` then `comment.dir`, and a
       form whose two entries arrive the other way round is a different request. §3.2.6's directionality is
       what it carries, which is the whole reason that algorithm exists in a headless engine. */
    if (html_form_needs_dirname_entry(field)) {
        lxb_dom_node_t *n = node_of(field);
        size_t dlen = 0;
        const char *dirname = (const char *)lxb_dom_element_get_attribute(lxb_dom_interface_element(n),
                                                                          (const lxb_char_t *)"dirname", 7,
                                                                          &dlen);
        int dir = directionality_of(ctx, lxb_dom_interface_element(n));

        DCHECK(dirname != NULL && dlen != 0,
               "step 5.12's condition held for a control whose `dirname` attribute is absent or empty — the "
               "condition and the read are two spellings of one attribute and they disagreed");
        fel_append(ctx, entries, dirname, dlen, JS_NewString(ctx, directionality_name(dir)));
    }
}

/* ---- the algorithm ---------------------------------------------------------------------------------------- */

void form_entry_list_init(FormEntryListRun *r)
{
    int k;

    r->phase = FEL_START;
    r->fphase = 0;
    r->flag_set = 0;
    r->i = 0;
    r->form = JS_UNDEFINED;
    r->controls = JS_UNDEFINED;
    r->entries = JS_UNDEFINED;
    r->ev = JS_UNDEFINED;
    for (k = 0; k < 4; k++) r->cb[k] = JS_UNDEFINED;
}

void form_entry_list_visit(JSContext *ctx, FormEntryListRun *r, JSStepVisit *v)
{
    int k;

    v->val(ctx, &r->form);
    v->val(ctx, &r->controls);
    v->val(ctx, &r->entries);
    v->val(ctx, &r->ev);
    for (k = 0; k < 4; k++)
        v->val(ctx, &r->cb[k]);
}

void form_entry_list_release(JSContext *ctx, FormEntryListRun *r)
{
    int k;

    /* Step 8 for a run that never reached it. A flow abandoned inside step 5 — outranked and dropped, or
       unwound by a throw — would otherwise leave the form's flag set forever, and every later submission of
       that form would take step 1's early return and silently do nothing. */
    if (r->flag_set) {
        fel_flag_set(ctx, r->form, false);
        r->flag_set = 0;
    }
    JS_FreeValue(ctx, r->form);
    JS_FreeValue(ctx, r->controls);
    JS_FreeValue(ctx, r->entries);
    JS_FreeValue(ctx, r->ev);
    r->form = r->controls = r->entries = r->ev = JS_UNDEFINED;
    for (k = 0; k < 4; k++) {
        JS_FreeValue(ctx, r->cb[k]);
        r->cb[k] = JS_UNDEFINED;
    }
}

int form_entry_list_run(JSContext *ctx, FormEntryListRun *r, JSValueConst form, JSValueConst submitter,
                        const char *encoding, JSValue in, JSValue *pout, JSValue **out_cb, int *out_argc)
{
    int rc;

    DCHECK(encoding != NULL, "§4.10.22.4 was run with no encoding — its own default is UTF-8, named by the "
                             "caller rather than invented here");
    if (r->phase == FEL_START) {
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        DCHECK(g_atom_building != JS_ATOM_NULL,
               "§4.10.22.4 ran before form_entry_list_declare minted the form's flag key");
        if (fel_flag_of(ctx, form)) {   /* step 1 */
            *pout = JS_NULL;
            r->phase = FEL_DONE;
            return 0;
        }
        r->form = JS_DupValue(ctx, form);
        fel_flag_set(ctx, form, true);   /* step 2 */
        r->flag_set = 1;
        r->controls = html_form_submittable_controls(ctx, form);   /* step 3 */
        r->entries = form_data_new(ctx, NULL);                     /* steps 4 and 6, as one object */
        if (JS_IsException(r->entries)) { r->entries = JS_UNDEFINED; return -1; }
        r->i = 0;
        r->phase = FEL_WALK;
    }
    if (r->phase == FEL_WALK) {
        uint32_t n = 0;
        JSValue lv = JS_GetPropertyStr(ctx, r->controls, "length");

        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        if (r->i < n) {
            JSValue field = JS_GetPropertyUint32(ctx, r->controls, r->i++);
            fel_one_field(ctx, r->entries, field, submitter, encoding);
            JS_FreeValue(ctx, field);
            /* Step 5 walks a list of the PAGE's size, so it offers the scheduler a yield per control — a form
               with ten thousand controls is fair BFS work and not one uninterruptible opcode. */
            return JS_STEP_YIELD;
        }
        r->phase = FEL_FIRE;
    }
    DCHECK(r->phase == FEL_FIRE, "§4.10.22.4 resumed at a phase it never parks in");
    /* Step 7: "fire an event named formdata at form using FormDataEvent, with the formData attribute
       initialized to form data and the bubbles attribute initialized to true" — the ONE §2.9 dispatch, as a
       REQUEST, so the handlers run as ordinary preemptible page code and this resumes after every one of them
       has returned WITH whatever they appended already in the list. */
    if (JS_IsUndefined(r->ev)) {
        r->ev = form_data_event_new(ctx, r->entries);
        if (JS_IsException(r->ev)) { r->ev = JS_UNDEFINED; JS_FreeValue(ctx, in); return -1; }
    }
    rc = event_target_fire_run(ctx, &r->fphase, STEP_CB(r->cb), r->form, r->ev, in, NULL, out_cb, out_argc);
    if (rc > 0) return rc;
    if (rc < 0) return -1;
    fel_flag_set(ctx, r->form, false);   /* step 8 */
    r->flag_set = 0;
    /* Step 9: "return a CLONE of entry list" — so a caller holding the returned list cannot reach back into
       the FormData the page's handler still has a reference to, and vice versa. */
    *pout = form_data_clone(ctx, r->entries);
    r->phase = FEL_DONE;
    return JS_IsException(*pout) ? -1 : 0;
}
