/* HTML §2.5.5 "Referrer policy attributes". See referrer_policy_attribute.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_REFERRER_POLICY_ATTRIBUTE_H
#define ENGINE_HOST_BROWSER_CORE_HTML_REFERRER_POLICY_ATTRIBUTE_H

#include <lexbor/dom/dom.h>

/* The state of this element's `referrerpolicy` content attribute, AS ITS CANONICAL KEYWORD — which is what
   every consumer of a referrer policy attribute is defined to take: HTML §7.1.6's determine-the-iframe-element-
   referrer-policy returns "the state's corresponding keyword", and §2.6.1's reflected `referrerPolicy` getter
   returns the canonical keyword of the state.
   NEVER NULL, and never owned — the returned bytes are a row of this component's own keyword table, so there is
   nothing to free and nothing that can outlive the program. A referrer policy attribute's missing value default
   and invalid value default are both the EMPTY STRING STATE, whose keyword is the empty string, so an element
   with no attribute and an element with a nonsense one both answer "" — a positive statement of a real state
   (Referrer Policy §3.9 "The empty string"), not an absence a caller has to fill in. */
const char *referrer_policy_attribute_keyword(const lxb_dom_element_t *el);

#endif
