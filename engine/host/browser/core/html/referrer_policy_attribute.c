/* HTML §2.5.5 "Referrer policy attributes" — the attribute's keyword/state table, in ONE place.
 *
 * §2.5.5 IS THREE SENTENCES AND EACH IS A MECHANISM: "A referrer policy attribute is an ENUMERATED ATTRIBUTE"
 * (so §2.3.3's determine-the-state decides it, ASCII case-insensitively — core/html/enumerated_attribute.c);
 * "each REFERRER POLICY, INCLUDING THE EMPTY STRING, is a keyword for this attribute, mapping to a state of the
 * same name" (so the keyword list is not HTML's at all, it is Referrer Policy §3 "Referrer Policies"' — and the
 * empty string is a KEYWORD, not merely a default); "the attribute's MISSING VALUE DEFAULT and INVALID VALUE
 * DEFAULT are both the empty string state".
 *
 * WHY THE ELEMENT DOES NOT OWN THIS TABLE. The attribute is spelled the same on `<a>`, `<area>`, `<iframe>`,
 * `<img>`, `<link>` and `<script>`, and §2.5.5 defines it once for all of them; a table on the iframe component
 * would be the first of six, and the sixth would be the one that had not heard about a policy the list gained.
 * The element-specific part is not the table, it is WHICH ELEMENTS HAVE THE ATTRIBUTE AT ALL — which is what
 * HTML §7.1.6's `iframe`-only condition is about, and it lives with the iframe (core/html/html_iframe.c).
 *
 * THE EMPTY STRING IS NOT AN ABSENCE AND MUST NOT BE READ AS ONE. Referrer Policy §3.9 "The empty string" is a
 * real state — it means "no policy has been set at this level, so a lower-precedence signal decides", and HTML
 * §2.5.5's own note gives that precedence order (a `noreferrer` link type, then this attribute, then a `meta`
 * with `name=referrer`, then the `Referrer-Policy` header). A consumer that treated "" as "the attribute did
 * not answer" and substituted a default of its own would be the defaulted-field defect exactly: a plausible
 * datum standing where a measurement belongs. */
#include "check.h"
#include "core/html/enumerated_attribute.h"
#include "core/html/referrer_policy_attribute.h"

/* Referrer Policy §3 "Referrer Policies": "A referrer policy is the empty string, "no-referrer",
   "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin", "origin-when-cross-origin",
   "strict-origin-when-cross-origin", or "unsafe-url"" — the same nine the `ReferrerPolicy` IDL enum lists, in
   the same order. Each maps to a state OF THE SAME NAME, so one keyword per state and §2.3.3's first canonical-
   keyword arm applies to every one of them. */
enum {
    RP_EMPTY_STRING = 0,
    RP_NO_REFERRER,
    RP_NO_REFERRER_WHEN_DOWNGRADE,
    RP_SAME_ORIGIN,
    RP_ORIGIN,
    RP_STRICT_ORIGIN,
    RP_ORIGIN_WHEN_CROSS_ORIGIN,
    RP_STRICT_ORIGIN_WHEN_CROSS_ORIGIN,
    RP_UNSAFE_URL
};

/* The terminator is the row whose keyword is NULL, and the EMPTY-STRING KEYWORD IS NOT IT: `""` is a non-NULL
   pointer to a NUL byte, so the walk passes over it and stops one row later. That distinction is the whole
   reason this table can hold §2.5.5's empty-string keyword at all. */
static const EnumeratedKeyword RP_KEYWORDS[] = {
    { "",                                RP_EMPTY_STRING },
    { "no-referrer",                     RP_NO_REFERRER },
    { "no-referrer-when-downgrade",      RP_NO_REFERRER_WHEN_DOWNGRADE },
    { "same-origin",                     RP_SAME_ORIGIN },
    { "origin",                          RP_ORIGIN },
    { "strict-origin",                   RP_STRICT_ORIGIN },
    { "origin-when-cross-origin",        RP_ORIGIN_WHEN_CROSS_ORIGIN },
    { "strict-origin-when-cross-origin", RP_STRICT_ORIGIN_WHEN_CROSS_ORIGIN },
    { "unsafe-url",                      RP_UNSAFE_URL },
    { NULL,                              0 }
};

/* §2.5.5's two sentences about defaults, as the one definition §2.6.1's getter looks up. All three positions
   are the empty string state, for the two different reasons the reader below spells out. */
const EnumeratedAttribute REFERRER_POLICY_ATTRIBUTE = {
    RP_KEYWORDS, RP_EMPTY_STRING, RP_EMPTY_STRING, RP_EMPTY_STRING
};

const char *referrer_policy_attribute_keyword(const lxb_dom_element_t *el)
{
    int state;

    DCHECK(el != NULL,
           "§2.5.5's referrer policy attribute was asked of no element — every caller reaches this having "
           "already decided the element is one that carries the attribute (§7.1.6 asks whether the embedder is "
           "an `iframe`), so a NULL here is a caller that skipped its own condition");
    /* §2.3.3's determine the state. ALL THREE SPECIAL STATES ARE THE EMPTY STRING STATE, and they are for two
       different reasons that happen to coincide: §2.5.5 declares the MISSING and INVALID value defaults to be
       it, and it declares NO empty value default at all — which §2.3.3 reduces to "step 3 is skipped and step 4
       runs", i.e. passing the invalid one in the empty position. Step 3 is unreachable here regardless, because
       the empty string is a KEYWORD and step 2 has already matched it. */
    state = enumerated_attribute_state(el, "referrerpolicy", REFERRER_POLICY_ATTRIBUTE.keywords,
                                       REFERRER_POLICY_ATTRIBUTE.missing, REFERRER_POLICY_ATTRIBUTE.empty,
                                       REFERRER_POLICY_ATTRIBUTE.invalid);
    return enumerated_attribute_canonical_keyword(REFERRER_POLICY_ATTRIBUTE.keywords, state);
}
