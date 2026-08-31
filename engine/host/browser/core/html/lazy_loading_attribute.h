/* HTML §2.5.7 "Lazy loading attributes". See lazy_loading_attribute.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_LAZY_LOADING_ATTRIBUTE_H
#define ENGINE_HOST_BROWSER_CORE_HTML_LAZY_LOADING_ATTRIBUTE_H

#include "core/html/enumerated_attribute.h"

/* §2.5.7's two states. Both have a keyword, so unlike §2.5.4's No CORS there is no state this attribute can be
   in that §2.6.1's getter answers with the empty string — `img.loading` is always "lazy" or "eager". */
enum { LAZY_LOADING_EAGER = 0, LAZY_LOADING_LAZY };

/* §2.5.7's attribute as §2.3.3 defines it: "The attribute's missing value default and invalid value default are
   both the Eager state". It declares no empty value default, which §2.3.3 reduces to running step 4 — so the
   empty position carries the invalid one, exactly as core/html/enumerated_attribute.h's contract states. */
extern const EnumeratedAttribute LAZY_LOADING_ATTRIBUTE;

/* §2.5.7's own question — "if element's lazy loading attribute is in the Lazy state" — asked of an element, so
   that a consumer deciding whether to defer a fetch does not restate the keyword table to do it. */
bool lazy_loading_attribute_is_lazy(const lxb_dom_element_t *el);

#endif
