/* CSS Fonts Module Level 4's `@font-feature-values` membership table. See css_font_feature_values.h for why
 * the list is closed, why it is a component of its own, and why a name in it is DROPPED outside the enclosing
 * rule rather than crashed on. Every name below is copied from CSS Fonts 4 §6.9.1 "Basic syntax"'s own
 * `<font-feature-value-type>` production, in that production's order, so the two can be read side by side. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "core/css/css_font_feature_values.h"

/* CSS Fonts 4 §6.9.1's `<font-feature-value-type>` alternatives, in the spec's own order — the seven feature
   value blocks it defines, each `@<name> { <declaration-list> }`.
   THE ORDER IS THE PRODUCTION'S AND NOT ASCII, deliberately: this table is READ against the spec's one line and
   a lookup over seven names does not need a sort. Where a table's order is load-bearing for the lookup (the
   recognized-at-rule registry, which binary-searches) the order is asserted at the lookup; here it is not, so
   asserting an order the spec does not write would be a second fact able to disagree with the source it was
   transcribed from. */
static const char *const FEATURE_VALUE_AT_RULES[] = {
    "stylistic", "historical-forms", "styleset", "character-variant",
    "swash", "ornaments", "annotation",
};

bool css_font_feature_value_at_rule(const char *name)
{
    const size_t n = sizeof FEATURE_VALUE_AT_RULES / sizeof FEATURE_VALUE_AT_RULES[0];
    size_t i;

    DCHECK(name != NULL,
           "an at-rule was asked whether it is one of CSS Fonts 4 §6.9.1 \"Basic syntax\"'s feature value "
           "blocks without giving its name. The name is the only thing that can decide it — CSS Fonts 4 "
           "§6.9.1's production is a list of at-keywords and nothing else — so a null here is a caller that "
           "lost the name rather than an at-rule that never had one");
    for (i = 0; i < n; i++)
        if (strcmp(FEATURE_VALUE_AT_RULES[i], name) == 0) return true;
    return false;
}
