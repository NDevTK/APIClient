/* FORMS — HTML §4.10: a form's controls, their VALUE state, and submission. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_FORM_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_FORM_H
#include <stdbool.h>
#include "quickjs.h"

/* Install §4.10's members on the interfaces that DECLARE them. The html layer owns the per-tag prototypes and
   hands them over; this file owns the algorithms. */
/* Declared once per AGENT; html_form_install then names the cached ids for each realm's prototypes. */
void html_form_declare(JSContext *ctx);
void html_form_install(JSContext *ctx, JSValueConst form_proto, JSValueConst input_proto,
                       JSValueConst textarea_proto, JSValueConst option_proto);
void html_form_free(JSContext *ctx);
/* `document.forms` — a Document member, so document.c installs it on its prototype. */

/* ---- §4.10.18.3 THE FORM OWNER -----------------------------------------------------------------------------
 *
 * A form-associated element's relationship with a form element. It is STORED state and not a lookup: the spec
 * initialises it to null, RESETS it at named moments, and a page observes the difference (an element whose
 * `form` attribute names nothing keeps a null owner even while it sits inside a form, until something resets
 * it). Held on the element's own wrapper, so it forks per flow and parks with the flow that changed it.
 *
 * The only form-associated elements this engine has are FORM-ASSOCIATED CUSTOM ELEMENTS: the built-in controls
 * reach their form through `form.elements`' tree walk, which needs no stored owner and has no
 * `formAssociatedCallback` to fire. */

/* THE ELEMENT'S FORM OWNER — the form element's wrapper, or JS_NULL. OWNED. */
JSValue html_form_owner_of(JSContext *ctx, JSValueConst wrap);

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

/* HTML §4.10.19's "labels": every `label` element in the element's tree whose `for` attribute is the element's
   ID, plus every ancestor label, in tree order — as a STATIC NodeList, the same named gap querySelectorAll
   carries. Here rather than in the label element's own file because there is no label component: the algorithm
   is the form layer's, and `ElementInternals.labels` is its one caller. */
JSValue html_form_labels_of(JSContext *ctx, JSValueConst wrap);

/* HTML §4.10.19's "a form control is disabled": the element carries a `disabled` content attribute, or it is a
   descendant of a `fieldset` whose `disabled` attribute is set and it is not inside that fieldset's first
   legend child. §4.13.5 step 10.2's condition. */
bool html_form_control_is_disabled(JSContext *ctx, JSValueConst wrap);

#endif
