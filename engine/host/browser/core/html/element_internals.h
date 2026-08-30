/* ELEMENT INTERNALS — HTML §4.13.7: `attachInternals()`, the ElementInternals interface, the form-associated
   half, the CustomStateSet, and the ARIA content-attribute map. See element_internals.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_ELEMENT_INTERNALS_H
#define ENGINE_HOST_BROWSER_CORE_HTML_ELEMENT_INTERNALS_H
#include <stdbool.h>
#include <stdint.h>
#include "quickjs.h"

/* Declared ONCE PER AGENT — the classes, the slot keys and the member pool entries — from the same place
   html_form_declare runs, because `attachInternals` is an HTMLElement member and that is where HTMLElement's
   members are declared. Declares this component's per-realm prototypes into core/realm.h's list. */
void element_internals_declare(JSContext *ctx);
/* §4.13.7 "Element internals"'s `ElementInternals attachInternals()` onto HTMLElement.prototype, for ONE
   realm. */
void element_internals_install_html_members(JSContext *ctx, JSValueConst html_proto);
/* The interface OBJECTS — `ElementInternals`, `CustomStateSet`, `ValidityState`. */
void element_internals_install(JSContext *ctx, JSValueConst global);
void element_internals_free(JSRuntime *rt);

/* §4.13.5 "Upgrades" STEP 11's TWO REACTIONS, for the upgrade that has just made an element custom. It is here
   rather than in custom_elements.c because step 11.1's "reset the form owner" and step 11.2's "is disabled"
   are this component's and the form layer's questions, and the enqueue is the one line that joins them. A
   no-op unless the element is a form-associated custom element, which is step 11's own condition. */
void element_internals_upgrade_form_steps(JSContext *ctx, JSValueConst wrap);

/* §4.10.18.3's RESET, at the three moments this engine can see one — the element's own insertion, its own
   removal, and the write of its own `form` content attribute — plus §4.13.3's "doing so changes the form
   owner, its formAssociatedCallback is called". `form_attr` is the value the `form` attribute WILL have when
   the trigger IS that write (NULL for a removal), and ELEMENT_INTERNALS_ATTR_UNCHANGED for every other
   trigger, which reads the element's own. A no-op unless the element is a form-associated custom element. */
#define ELEMENT_INTERNALS_ATTR_UNCHANGED ((const char *)-1)
void element_internals_reset_form_owner(JSContext *ctx, JSValueConst wrap,
                                        const char *form_attr, size_t form_attr_len);

/* §4.13.7.3's SUBMISSION VALUE for a form-associated custom element — `setFormValue`'s first argument, which
   HTML §4.13.7.3's entry construction algorithm turns into the element's entries. A `(File or USVString or
   FormData)?`, so the caller distinguishes the FormData arm (an entry LIST appended wholesale) from the single
   value; JS_NULL when the element has never been given one. OWNED. */
JSValue element_internals_submission_value(JSContext *ctx, JSValueConst wrap);

/* §4.10.21.1's VALIDITY FLAGS OF A FORM-ASSOCIATED CUSTOM ELEMENT, in the CV_* bits constraint_validation.h
   declares — the state §4.13.7.3's `setValidity` wrote, read by "statically validate the constraints" so a
   FACE answers the same question every other control does. `el` is the ELEMENT, not its ElementInternals.
   "Does it suffer a custom error" is the CV_CUSTOM_ERROR bit of this answer and needs no second entry. What is
   not derivable from the bits is §4.10.21.1's validation MESSAGE — the string setValidity was given — and
   nothing reads that yet, so no accessor for it is exported. */
uint32_t element_internals_validity_flags(JSContext *ctx, JSValueConst el);

#endif
