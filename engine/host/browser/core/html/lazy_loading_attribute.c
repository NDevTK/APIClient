/* HTML §2.5.7 "Lazy loading attributes" — the attribute's keyword/state table, in ONE place.
 *
 * WHY IT IS A COMPONENT. The same argument core/html/referrer_policy_attribute.c and
 * core/html/cors_settings_attribute.c make: `loading` is spelled the same on `<img>`, `<iframe>` and `<video>`,
 * §2.5.7 defines it once for all three, and a table on one of them is the first of three copies. This one had
 * already grown two of them before this file existed, and neither was §2.3.3's algorithm: one compared the raw
 * attribute with a case-insensitive equality against "lazy" and treated everything else as Eager, which is the
 * bare-strcasecmp shape core/html/enumerated_attribute.c's header names, and the other did the same thing again
 * three files away.
 *
 * WHAT THE RAW COMPARISON GOT WRONG IS NOT THE MATCH, IT IS THE ANSWER'S TYPE. Comparing against "lazy" and
 * answering `lazy ? "lazy" : "eager"` happens to give §2.6.1's reflected value for both of §2.5.7's states,
 * because both of them have a keyword and the defaults are Eager — so the defect it left was not a wrong string
 * but an implementation of §2.6.1's getter that could not be reused, and that had to be re-derived (and got the
 * ASCII-versus-locale fold wrong, since `strcasecmp` folds by the current locale and a Turkish locale does not
 * fold `LAZY` to `lazy`) at every element that gained the attribute. */
#include "core/html/lazy_loading_attribute.h"

/* §2.5.7's table, in the section's own order. */
static const EnumeratedKeyword LAZY_LOADING_KEYWORDS[] = {
    { "lazy",  LAZY_LOADING_LAZY },
    { "eager", LAZY_LOADING_EAGER },
    { NULL,    0 }
};

const EnumeratedAttribute LAZY_LOADING_ATTRIBUTE = {
    LAZY_LOADING_KEYWORDS, LAZY_LOADING_EAGER, LAZY_LOADING_EAGER, LAZY_LOADING_EAGER
};

bool lazy_loading_attribute_is_lazy(const lxb_dom_element_t *el)
{
    return enumerated_attribute_state(el, "loading", LAZY_LOADING_ATTRIBUTE.keywords,
                                      LAZY_LOADING_ATTRIBUTE.missing, LAZY_LOADING_ATTRIBUTE.empty,
                                      LAZY_LOADING_ATTRIBUTE.invalid) == LAZY_LOADING_LAZY;
}
