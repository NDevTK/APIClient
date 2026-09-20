/* css-values-5 "Appendix A: Arbitrary Substitution Functions" / "Substitution in Shorthand Properties" — see
 * css_pending_substitution.h for the encoding, for why it is a string in this engine, and for what is
 * deliberately not covered. This file knows no property, no element and no cascade: it is the ENCODING and
 * nothing else, so one fixture with no DOM exercises all of it. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_pending_substitution.h"
#include "core/css/css_var.h"

/* U+0001 START OF HEADING. Its whole job is to be a byte no shorthand NAME contains — the names come from
   core/css/css_shorthand.c's own table — so the split below is exact whatever the page's value holds. */
#define CSS_PENDING_MARK '\x01'

static char *cps_dupn(const char *s, size_t n)
{
    char *out = malloc(n + 1);

    CHECK(out != NULL, "css-values-5's pending-substitution value: OOM");
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

char *css_pending_make(const char *shorthand, const char *original)
{
    size_t sn, on;
    char *out;

    DCHECK(shorthand != NULL && original != NULL,
           "css-values-5 \"Substitution in Shorthand Properties\": a pending-substitution value was asked for "
           "with no shorthand name or no original value. It carries BOTH — the name so the computed-value "
           "step knows whose grammar to re-parse with, the value because that step re-parses the shorthand AS "
           "WRITTEN — so either one absent is a producer that dropped half of what it was told");
    DCHECK(strchr(shorthand, CSS_PENDING_MARK) == NULL,
           "css-values-5 \"Substitution in Shorthand Properties\": a SHORTHAND NAME carrying this encoding's "
           "own separator. The names are core/css/css_shorthand.c's table rows, which are C string literals "
           "spelled from the standards' property names, so this is a caller passing something that is not one "
           "— and the split would then take the name's own mark for the separator and hand the resolution "
           "step a truncated property name");
    sn = strlen(shorthand);
    on = strlen(original);
    out = malloc(sn + on + 3);
    CHECK(out != NULL, "css-values-5's pending-substitution value: OOM");
    out[0] = CSS_PENDING_MARK;
    memcpy(out + 1, shorthand, sn);
    out[sn + 1] = CSS_PENDING_MARK;
    memcpy(out + sn + 2, original, on);
    out[sn + on + 2] = '\0';
    return out;
}

bool css_pending_is(const char *value)
{
    return value != NULL && value[0] == CSS_PENDING_MARK && strchr(value + 1, CSS_PENDING_MARK) != NULL;
}

bool css_pending_split(const char *value, char **shorthand, const char **original)
{
    const char *sep;

    DCHECK(shorthand != NULL && original != NULL,
           "css-values-5 \"Substitution in Shorthand Properties\": a pending-substitution value was decoded "
           "with nowhere to put one of its two halves — a caller wanting only one of them is a caller that "
           "will re-derive the other by scanning again, which is the second idea of where the separator is "
           "that this entry exists to prevent");
    if (!css_pending_is(value)) return false;
    sep = strchr(value + 1, CSS_PENDING_MARK);
    /* `css_pending_is` is the SAME scan, so the separator is found or the predicate above answered TRUE about
       a string that has none — two readings of one encoding having come apart inside one file. */
    DCHECK(sep != NULL,
           "css-values-5 \"Substitution in Shorthand Properties\": a value this file's own predicate called a "
           "pending-substitution value has no separator in it");
    *shorthand = cps_dupn(value + 1, (size_t)(sep - (value + 1)));
    *original = sep + 1;
    return true;
}

bool css_pending_serialize(const char *const *values, unsigned n, char **out)
{
    unsigned i;
    bool any = false, all = true;

    DCHECK(values != NULL && out != NULL && n > 0,
           "css-values-5 \"Substitution in Shorthand Properties\"' serialization rule was asked about no "
           "longhand values — the rule is a quantifier over a shorthand's components, and a shorthand with no "
           "components is not one core/css/css_shorthand.c records");
    *out = NULL;
    for (i = 0; i < n; i++) {
        bool pending = css_pending_is(values[i]);

        /* THE `Otherwise` ARM HAS TWO DISJUNCTS AND THIS IS THE SECOND: "or contain arbitrary substitution
           functions of their own that have not yet been substituted". A longhand reaches this list carrying
           one only by having been DECLARED as a longhand — `margin-top: var(--x)` beside three literal
           margins — which is a different population from the pending values above and takes the same answer.
           Leaving it out would make that shorthand serialize to `var(--x) 1px 1px 1px`, a string that is
           neither the standard's answer nor a value any grammar here produced: `css_shorthand_component`
           never returns an unsubstituted function, so the only way one arrives is the declaration itself.
           THE SCAN IS core/css/css_var.h's OWN, the same one that decided this declaration was admitted at
           parse time and the same one the substitution step performs, so no two of the three can come to
           disagree about what a function token is. */
        any = any || pending || css_var_references(values[i]);
        /* "from the same original shorthand value" is a BYTE COMPARISON and not a re-decode, because every
           longhand of one declaration is filled in from ONE `css_pending_make` call carrying one shorthand
           name and one original value. So two of a shorthand's longhands can only differ here by having been
           set by two DIFFERENT declarations — which is exactly the case the rule's `Otherwise` arm is about,
           and differing bytes is the evidence for it. */
        all = all && pending && strcmp(values[i], values[0]) == 0;
    }
    if (!any) return false;
    if (all) {
        char *name = NULL;
        const char *original = NULL;

        (void)css_pending_split(values[0], &name, &original);
        free(name);                  /* the rule wants the ORIGINAL VALUE; the name is the other half */
        *out = cps_dupn(original, strlen(original));
    }
    return true;
}
