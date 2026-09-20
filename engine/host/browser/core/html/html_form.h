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
                       JSValueConst textarea_proto, JSValueConst option_proto,
                       JSValueConst button_proto);
void html_form_free(JSRuntime *rt);
/* `document.forms` — a Document member, so document.c installs it on its prototype. */

/* ---- §4.10.11 The textarea element's VALUE, for the one other component that operates on it ----------------
 *
 * §4.10.20's text control selection APIs "must operate on the element's API VALUE" for a textarea, so that
 * algorithm has a second caller and stops being this file's private business. It is exported rather than
 * re-spelled there for the reason §4.10.5.4's `input_value_get` is exported: two answers to "what is this
 * control's value" is the defect that lets one algorithm read a value another one wrote differently. */

/* Whether this node is a `textarea` element. Exported so a component that installs on both text-control
   prototypes can perform Web IDL §3.7.6 Attributes' and §3.7.7 Operations' brand check without a fourth
   private copy of the local-name test. */
bool html_form_is_textarea(const lxb_dom_node_t *n);

/* §4.10.11's API VALUE: "the element's raw value, with newlines normalized" (Infra §4.7 Strings), which is what
   the `value` IDL attribute returns on getting and what §4.10.20 measures its offsets into. OWNED. An unknown
   raw value is its own API value — see the algorithm for why the alternative costs identity and provenance. */
JSValue html_form_textarea_api_value(JSContext *ctx, JSValueConst wrap);

/* SET THE ELEMENT'S RAW VALUE, and with it §4.10.18.1's dirty value flag — the write half §4.10.20's
   `setRangeText()` performs, whose steps splice the relevant value and set the flag and which never run the
   `value` IDL setter (that one additionally moves the text entry cursor, which setRangeText's own last step is
   about to contradict). */
void html_form_textarea_set_raw_value(JSContext *ctx, JSValueConst wrap, JSValueConst val);

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

/* ---- HTML §4.10.2 "Categories"' LISTED and LABELABLE elements, as the INTERFACES that declare the two
 * members those categories give a control: §4.10.18.3's `form` and §4.10.4's `labels` -----------------------
 *
 * The table and both getters are in html_form.c, beside the form owner and the label relation they answer
 * from. What crosses this header is the ROW LIST and one install taking every prototype by name, because the
 * PROTOTYPES belong to core/html/html_element.c — the same split every other §4.10 install here makes: that
 * file owns the element-interface table, this one owns the algorithms.
 *
 * `cat` IS A BIT SET AND NOT TWO TABLES because §4.10.2 states two OVERLAPPING subsets of one element list,
 * and two tables are two places for `output` to be in one of them and not the other. */
#define HTML_FORM_CAT_LISTED     0x1u
#define HTML_FORM_CAT_LABELABLE  0x2u
/* THE ROW ORDER IS THIS ENUM AND NOTHING ELSE: the table is written with ARRAY DESIGNATORS keyed by these, so
   a row cannot drift out of the position its getter's magic names and no assertion is needed to say so. */
enum { FC_BUTTON = 0, FC_FIELDSET, FC_INPUT, FC_OBJECT, FC_OUTPUT, FC_SELECT, FC_TEXTAREA, FC_COUNT };
typedef struct { const char *iface; const char *tag; unsigned cat; } HtmlFormControlIface;
extern const HtmlFormControlIface HTML_FORM_CONTROL_IFACES[FC_COUNT];

/* INSTALL §4.10.18.3's `form` and §4.10.4's `labels` on the prototypes that DECLARE them. Each prototype is a
   NAMED parameter and each member is installed at its own unconditional call, which is not a matter of style:
   the Web IDL gap audit resolves which object a member landed on by following the install's TARGET, and it can
   follow a named parameter the caller bound from a literal interface name. Written instead as a loop over the
   row list above — `for (i…) install(ctx, html_iface_proto(ctx, ROWS[i].iface), i)` — every one of these twelve
   members is reported UNPROVEN, the audit saying of each that
   `proto is 71 tagged prototypes and this install is under a condition that does not name which`.
   That is an ABSTENTION and not a check: it retires a true ABSENT row and puts nothing in its place.
   MEASURED, both shapes, at this revision.
   The ROW LIST still crosses this header because §4.10.2's categories and the brand check are this file's, and
   because core/html/html_element.c CHECKS each row's `tag` against HTML §3.2.2 "Elements in the DOM" once per
   agent, in its declare path beside the same join asserted for the reflection sets — that assertion is the only
   thing holding this table and that one together. The `cat` bits and this parameter list are TWO HALVES of one
   statement of the categories, held together in both directions by two different instruments: a getter reached
   through a row without its bit ABORTS at the getter, and a row whose bit is set with no install is exactly
   what the IDL gap audit reports as ABSENT. */
void html_form_install_control_members(JSContext *ctx, JSValueConst button_proto, JSValueConst fieldset_proto,
                                       JSValueConst input_proto, JSValueConst object_proto,
                                       JSValueConst output_proto, JSValueConst select_proto,
                                       JSValueConst textarea_proto);

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

/* §4.10.6 The button element's `type` AS §2.3.3 DEFINES IT — `submit`, `reset` and `button`, with the AUTO
   state (which no keyword names) as both the missing and the invalid value default. Exported for the same
   reason `input`'s is, plus one this element makes sharper: §4.10.6 defines "a submit button" over this
   attribute's STATE and its `type` getter over the state's CANONICAL KEYWORD, so a second reading of the
   keyword list is two answers about one element. There was one — three case-insensitive comparisons inside
   html_form_is_submit_button — and the getter's own assert is what now holds the two together. */
extern const EnumeratedAttribute HTML_BUTTON_TYPE_ATTRIBUTE;

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
   `autocomplete` of §4.10.19.7 Autofill, which is a control's autofill detail tokens and whose IDL getter
   §4.10.19.7.2 Processing model defines as "The autocomplete IDL attribute, on getting, must return the
   element's IDL-exposed autofill value" rather than as a reflection of anything; one name, two attributes, and
   only this one is an enumerated attribute. */
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

/* §4.10.22.4 step 5.7's value: "if the field element has a `value` attribute specified, then let value be the value of
   that attribute; otherwise, let value be the string `on`" — which IS §4.10.5.4's DEFAULT/ON mode word for
   word, so this asks input_value.c for that mode rather than spelling the same two lines again. OWNED. */
JSValue html_form_checkbox_value(JSContext *ctx, JSValueConst wrap);

/* §4.10.7's LIST OF OPTIONS for a `select`, narrowed to §4.10.22.4 step 5.6's condition — selectedness true and not
   disabled — with §4.10.7's SELECTEDNESS SETTING ALGORITHM already applied. A JS Array of option wrappers in
   tree order. OWNED. */
JSValue html_form_selected_options(JSContext *ctx, JSValueConst select);

/* §4.10.7's SELECTEDNESS SETTING ALGORITHM as the WRITE it is — its two steps set the SELECTEDNESS of the
   options in a select's list of options, which is §4.10.10's state and lives in core/html/html_option.c. It is
   here because its subject is the SELECT: the list of options is this file's walk and the `multiple` attribute
   and the display size are this file's reads. Idempotent, and a write that changes nothing records nothing, so
   §4.10.10's readers may run it at every read rather than each carrying a copy of its outcome. */
void html_form_selectedness_setting_algorithm(JSContext *ctx, lxb_dom_node_t *select);

/* THE SELECT WHOSE LIST OF OPTIONS HOLDS THIS OPTION, or NULL — what §4.10.10's "cause the element to ask for a
   reset" resolves. NOT the nearest `select` ancestor: §4.10.7's walk stops descending at a nested `optgroup`, a
   `datalist`, an `hr` and an `option`, so an option under one of those has a select ancestor and is in no list
   of options, and the candidate's own list is what decides. */
lxb_dom_node_t *html_form_select_of_option(JSContext *ctx, lxb_dom_node_t *opt);

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
