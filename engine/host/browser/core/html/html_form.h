/* FORMS — HTML §4.10: a form's controls, their VALUE state, and submission. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_FORM_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_FORM_H
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "core/html/enumerated_attribute.h"   /* §2.3.3's definitions for the enumerated attributes below */

/* Install §4.10's members on the interfaces that DECLARE them. The html layer owns the per-tag prototypes and
   hands them over; this file owns the algorithms. */
/* Declared once per AGENT; html_form_install then names the cached ids for each realm's prototypes. */
void html_form_declare(JSContext *ctx);
void html_form_install(JSContext *ctx, JSValueConst form_proto, JSValueConst input_proto,
                       JSValueConst textarea_proto, JSValueConst option_proto);
void html_form_free(JSRuntime *rt);
/* `document.forms` — a Document member, so document.c installs it on its prototype. */

/* ---- §4.10.18.3 THE FORM OWNER -----------------------------------------------------------------------------
 *
 * A form-associated element's relationship with a form element. It is STORED state and not a lookup: the spec
 * initialises it to null, RESETS it at named moments, and a page observes the difference (an element whose
 * `form` attribute names nothing keeps a null owner even while it sits inside a form, until something resets
 * it). Held on the element's own wrapper, so it forks per flow and parks with the flow that changed it.
 *
 * AND AN ELEMENT NO RESET HAS EVER RUN FOR IS NOT AN ELEMENT WITH NO OWNER. HTML's TREE BUILDER associates a
 * parsed control with its "form element pointer" as it builds, and this engine's Lexbor parse routes through
 * none of DOM §4.2.3's insertion steps — so a parsed `<form><input name=q>` has had no reset at all, and an
 * absent slot read as a null owner would empty `form.elements` and every entry list for every parsed document.
 * The absent slot therefore means exactly what it says — the reset that the insertion should have run has not
 * run — and html_form_owner_of answers by RUNNING it, through the same steps 3-5 the reset itself uses. One
 * derivation, not a second rule: a STORED owner always wins, so an element whose reset produced null
 * (`<my-control form=nothing>`) keeps null. */

/* THE ELEMENT'S FORM OWNER — the form element's wrapper, or JS_NULL. OWNED. */
JSValue html_form_owner_of(JSContext *ctx, JSValueConst wrap);

/* COULD this element be form-associated at all — asked of the TAG, with NO WRAPPER. §4.10.2's categories are
   all built-in tags plus custom elements, so this drops nothing they contain; what it buys is that a tree walk
   over a page's own markup does not materialise a wrapper per node just to find out that a `<div>` has no form
   owner. Whether it IS one is only knowable through its definition and its per-flow owner slot, which is what
   html_form_owner_of answers. */
bool html_form_maybe_associated(lxb_dom_node_t *n);

/* "RESET THE FORM OWNER of element", steps 1-5. `*pchanged` (may be NULL) says whether step 3-5 left a
   DIFFERENT owner than the element had, which is what §4.13.3's "doing so changes the form owner" reads.
   Returns the new owner (or JS_NULL), OWNED.
   NOTE THE INPUT: step 4 reads the element's `form` CONTENT ATTRIBUTE, and one of the sites that triggers a
   reset is the write of that very attribute — which, per DOM §4.9, notifies BEFORE the value is stored. So the
   attribute's value is an ARGUMENT rather than something read back off the element at a moment when the
   element does not yet hold it: `form_attr` is the value it WILL have, NULL for absent/removed, and
   html_form_reset_owner is the form for every other trigger, which reads the element's own. */
JSValue html_form_reset_owner(JSContext *ctx, JSValueConst wrap, bool *pchanged);
JSValue html_form_reset_owner_with_attr(JSContext *ctx, JSValueConst wrap, const char *form_attr,
                                        size_t form_attr_len, bool *pchanged);

/* HTML §4.10.4 "The label element"'s `labels`: every `label` element in the element's tree whose LABELED
   CONTROL is this element, in tree order — as a STATIC NodeList, the same named gap querySelectorAll carries.
   Here rather than in the label element's own file because there is no label component: the algorithm is the
   form layer's, and it is one predicate with §4.10.4's other direction (a label's own `control`).
   THE CITATION WAS §4.10.19 AND THAT SECTION IS "Attributes common to form controls", which says nothing about
   labels; the callers are the members of every LABELABLE element (§4.10.2 Categories lists the eight), of which
   §4.10.13's `progress` and §4.10.14's `meter` are built. */
JSValue html_form_labels_of(JSContext *ctx, JSValueConst wrap);

/* HTML §4.10.19's "a form control is disabled": the element carries a `disabled` content attribute, or it is a
   descendant of a `fieldset` whose `disabled` attribute is set and it is not inside that fieldset's first
   legend child. §4.13.5 step 10.2's condition. */
bool html_form_control_is_disabled(JSContext *ctx, JSValueConst wrap);

/* HTML §4.10.5.1's "an input element can be MUTABLE" — "Except where otherwise specified, an input element is
   always mutable", and the standard specifies otherwise exactly twice: "when an input element is disabled, it
   is not mutable", and §4.10.5.3.6's `readonly` — "when specified, the element is not mutable", in the states
   that attribute APPLIES to. Those are the text-entry states and nothing else ("Only text controls can be made
   read-only, since for other controls ... there is no useful distinction between being read-only and being
   disabled, so the readonly attribute does not apply"), which is why File Upload and Color read as mutable with
   the attribute sitting on them.
   It is here rather than beside one of its readers because it has several — §4.10.5.4's showPicker() step 1
   throws an InvalidStateError on it, "show the picker, if applicable" step 2 returns on it, and §4.10.5.1.17's
   drag-and-drop selection is only allowed while it holds — and a second copy is the one-fact-two-answers defect
   that lets a readonly date control open a picker in one algorithm and not the other. */
bool html_form_input_is_mutable(JSContext *ctx, JSValueConst wrap);

/* ---- §4.10.2's CATEGORIES, and what the entry list asks of them --------------------------------------------
 *
 * §4.10.2 lists the form-associated categories by ELEMENT, and every consumer of one asks the same question of
 * the same list — so the list is stated once here and nothing re-spells it. `wrap` is the element's wrapper
 * because a FORM-ASSOCIATED CUSTOM ELEMENT is in every one of these categories and only its definition can say
 * so. */

/* §4.10.5.1's STATES OF THE `type` ATTRIBUTE — the enumerated attribute's twenty-one keywords, resolved ONCE.
   Every consumer of an input's type asks the same question of the same table, and asked as a chain of string
   comparisons per consumer the table is written out again each time: §4.10.21's constraint attributes apply per
   state, §3.2.6's auto-directionality is a list of states, §4.10.2's buttons are four of them. The TEXT state is
   both the missing-value and the invalid-value default, so an absent or unrecognised keyword IS that state and
   no consumer needs a case for it. */
typedef enum {
    INPUT_STATE_NONE = 0,        /* the element is not an `input` */
    INPUT_STATE_HIDDEN, INPUT_STATE_TEXT, INPUT_STATE_SEARCH, INPUT_STATE_TEL, INPUT_STATE_URL,
    INPUT_STATE_EMAIL, INPUT_STATE_PASSWORD, INPUT_STATE_DATE, INPUT_STATE_MONTH, INPUT_STATE_WEEK,
    INPUT_STATE_TIME, INPUT_STATE_DATETIME_LOCAL, INPUT_STATE_NUMBER, INPUT_STATE_RANGE, INPUT_STATE_COLOR,
    INPUT_STATE_CHECKBOX, INPUT_STATE_RADIO, INPUT_STATE_FILE, INPUT_STATE_SUBMIT, INPUT_STATE_IMAGE,
    INPUT_STATE_RESET, INPUT_STATE_BUTTON,
} HtmlInputState;
HtmlInputState html_form_input_state(const lxb_dom_node_t *n);

/* §4.10.5.1's `type` AS §2.3.3 DEFINES IT, exported for the consumer that needs the DEFINITION rather than this
   element's state: §2.6.1's `input.type` is a limited-to-only-known-values reflection, so it must hand back the
   CANONICAL KEYWORD of the state, which the enum above cannot spell. Sharing the definition is what stops the
   reflection becoming a twenty-third spelling of the keyword list. */
extern const EnumeratedAttribute HTML_INPUT_TYPE_ATTRIBUTE;

/* §4.10.19.6's FOUR FORM SUBMISSION ATTRIBUTES, as TWO keyword tables and FOUR definitions — which is the shape
   the section itself states: "The method and formmethod content attributes are enumerated attributes with the
   following keywords and states ... The method attribute's missing value default and invalid value default are
   both the GET state. The formmethod attribute has no missing value default, and its invalid value default is
   the GET state", and the same sentence again for `enctype`/`formenctype`. So the pairs differ ONLY in the
   missing value default, and the one without it is why core/html/enumerated_attribute.h has a no-state value at
   all: `document.createElement('input').formMethod` is the empty string, not "get".
   `method` and `enctype` ARE THE ELEMENT'S OWN STATES and are what §4.10.22.3's submission reads; the
   `form`-prefixed two are the submitter's override, which is why the section defines "the method of an element"
   over both. */
extern const EnumeratedAttribute HTML_FORM_METHOD_ATTRIBUTE;
extern const EnumeratedAttribute HTML_FORM_FORMMETHOD_ATTRIBUTE;
extern const EnumeratedAttribute HTML_FORM_ENCTYPE_ATTRIBUTE;
extern const EnumeratedAttribute HTML_FORM_FORMENCTYPE_ATTRIBUTE;

/* §4.10.3's `autocomplete` ON A `form` ELEMENT — "on"/"off", both defaults the On state. It is NOT the
   `autocomplete` of §4.10.19.7, which is a control's autofill detail tokens and whose IDL getter returns "the
   element's IDL-exposed autofill value" rather than reflecting anything; one name, two attributes, and only
   this one is an enumerated attribute. */
extern const EnumeratedAttribute HTML_FORM_AUTOCOMPLETE_ATTRIBUTE;

/* §4.10.7's PLACEHOLDER LABEL OPTION: with `required` specified and a display size of 1, the FIRST option in
   the select's list of options, when its value is the empty string and its parent is the select itself. It is
   here rather than beside the constraint that reads it because the list of options and the display size are
   §4.10.7's and this file owns them. The option's wrapper, or JS_NULL. OWNED. */
JSValue html_form_placeholder_label_option(JSContext *ctx, JSValueConst select);

/* §4.10.2 SUBMITTABLE: button, input, select, textarea, and form-associated custom elements. §4.10.22.4 step 3
   walks exactly these. */
bool html_form_is_submittable(JSContext *ctx, JSValueConst wrap);
/* §4.10.2: which submittable elements are BUTTONS — a `button` element, or an `input` whose `type` is in the
   Submit Button, Image Button, Reset Button or Button state. §4.10.22.4 step 5.1's third condition. */
bool html_form_is_button(JSContext *ctx, JSValueConst wrap);
/* §4.10.2: which buttons are SUBMIT buttons — `input type=submit`, `input type=image`, and a `button` whose
   `type` is in the Submit Button state or in the Auto state with no `command`/`commandfor` and whose parent is
   not a `select`. XHR §5's constructor throws a TypeError for a submitter that is not one. */
bool html_form_is_submit_button(JSContext *ctx, JSValueConst wrap);

/* §4.10.22.4 step 3's CONTROLS: every submittable element whose form owner is `form`, in tree order, as a JS
   Array of wrappers. OWNED. */
JSValue html_form_submittable_controls(JSContext *ctx, JSValueConst form);

/* §4.10.22.4 STEP 5'S BRANCH CHAIN, AS THE ONE QUESTION IT IS. The chain asks a mixture of tag and `type`-state
   questions, and every one of them is a Lexbor read — so the classification happens HERE, where §4.10's element
   knowledge already lives, and the entry-list algorithm branches on the answer rather than re-deriving it from
   the tree. */
typedef enum {
    FORM_FIELD_OTHER = 0,      /* step 5.11's "otherwise": an entry with the element's value */
    FORM_FIELD_IMAGE_BUTTON,   /* step 5.2  — `input` in the Image Button state */
    FORM_FIELD_FACE,           /* step 5.3  — a form-associated custom element */
    FORM_FIELD_SELECT,         /* step 5.6  — one entry per selected, enabled option */
    FORM_FIELD_CHECKBOX,       /* step 5.7  — `input` in the Checkbox or Radio Button state */
    FORM_FIELD_FILE,           /* step 5.8  — `input` in the File Upload state */
    FORM_FIELD_CHARSET,        /* step 5.9  — `input type=hidden` named `_charset_` */
} FormFieldKind;
FormFieldKind html_form_field_kind(JSContext *ctx, JSValueConst wrap);

/* The `name` CONTENT ATTRIBUTE — step 5.4's read, and step 5.2.2's. BORROWED from the attribute store; NULL
   with `*plen` 0 when it is absent. */
const char *html_form_control_name(JSValueConst wrap, size_t *plen);

/* Step 5.7's value: "if the field element has a `value` attribute specified, then let value be the value of
   that attribute; otherwise, let value be the string `on`" — which IS §4.10.5.4's DEFAULT/ON mode word for
   word, so this asks input_value.c for that mode rather than spelling the same two lines again. OWNED. */
JSValue html_form_checkbox_value(JSContext *ctx, JSValueConst wrap);

/* §4.10.7's LIST OF OPTIONS for a `select`, narrowed to step 5.6's condition — selectedness true and not
   disabled — with §4.10.7's SELECTEDNESS SETTING ALGORITHM already applied. A JS Array of option wrappers in
   tree order. OWNED. */
JSValue html_form_selected_options(JSContext *ctx, JSValueConst select);

/* Step 5.1's first condition: "field has a `datalist` element ancestor". */
bool html_form_has_datalist_ancestor(JSValueConst wrap);

/* Step 5.12's CONDITION: the element has a `dirname` attribute whose value is not the empty string AND it is an
   auto-directionality form-associated element (§3.2.6's list: an `input` in the Hidden, Text, Search, Telephone,
   URL, Email, Password, Submit Button, Reset Button or Button state, or a `textarea`). */
bool html_form_needs_dirname_entry(JSValueConst wrap);

/* §3.2.6's AUTO-DIRECTIONALITY FORM-ASSOCIATED ELEMENTS — an `input` in the Hidden, Text, Search, Telephone,
   URL, Email, Password, Submit Button, Reset Button or Button state, or a `textarea`. The list §3.2.6's auto
   directionality and §4.10.22.4 step 5.12 both read, stated once. */
bool html_form_is_auto_directionality_face(const lxb_dom_node_t *n);

/* §3.2.6's one type-specific Undefined case: an `input` in the TELEPHONE state is 'ltr' regardless of what
   contains it. */
bool html_form_is_telephone_input(const lxb_dom_node_t *n);

/* §4.10.18.1's VALUE, and §4.10.5.1.15's CHECKEDNESS, for a caller that is not the IDL accessor — the entry
   list reads both off controls it did not receive as a receiver. `html_form_control_value` is OWNED and may be
   a CONCOLIC: the value slot holds a JSValue rather than bytes exactly so that `input.value = location.hash`
   survives into the submission.
   FOR AN `input` IT IS input_value.c'S — §4.10.5.4's mode-dependent value, already through §4.10.5.1's value
   sanitization algorithm. That is why it is ONE function and not a read each consumer does for itself: the
   entry list, §4.10.21's constraint validation and §3.2.6's auto-directionality all read a control's value, and
   an unsanitized one made all three wrong together (`<input type=url value=" http://x ">` reported a
   typeMismatch no browser reports). */
JSValue html_form_control_value(JSContext *ctx, JSValueConst wrap);
bool    html_form_control_checked(JSContext *ctx, JSValueConst wrap);

/* Is this an HTMLFormElement — the narrowing predicate a class-id brand cannot express (§16.5a's gap: every
   node wrapper is one class). XHR §5's `optional HTMLFormElement form` declares it. */
bool html_form_is_form_element(JSValueConst v);

#endif
