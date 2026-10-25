/* CSS Properties and Values API 1 §2.1 "Determining the Registration": the custom property registration a
 * document holds for one custom property name. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_REGISTRATION_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_REGISTRATION_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* §2.1's struct. `syntax` is the syntax string (`*` for the universal syntax definition), and a NULL
   `initial_value` is the guaranteed-invalid value. Both strings are OWNED. */
typedef struct {
    char *syntax;
    bool  inherits;
    char *initial_value;
} CssPropertyRegistration;

/* One registration a style sheet's `@property` rule states: the custom property name it registers and the
   registration, both owned. core/css/css_rule.h's cascade walk collects them in document order. */
typedef struct {
    char                   *name;
    CssPropertyRegistration reg;
} CssPropertyRegistrationEntry;

/* The registration of `name` in `el`'s node document: the last valid `@property` rule naming it, in document
   order over the document's applying style sheets, read from the document's author sheet set the layout job's
   cascade record holds. False when there is none, and `*out` is then untouched; on true the caller frees
   `*out` with css_property_registration_free. */
bool css_property_registration_of(lxb_dom_element_t *el, const char *name, CssPropertyRegistration *out);
void css_property_registration_free(CssPropertyRegistration *r);
void css_property_registration_entry_free(CssPropertyRegistrationEntry *e);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_REGISTRATION_H */
