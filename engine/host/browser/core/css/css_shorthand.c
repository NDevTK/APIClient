/* CSS Cascade §Shorthand Properties — see css_shorthand.h for why the expansion is here and not in lexbor. */
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_shorthand.h"

static char *css_sh_strdup(const char *s)
{
    char *out = strdup(s);
    CHECK(out != NULL, "cssom: OOM expanding a shorthand — a dropped component would read as undeclared");
    return out;
}

/* A CSS keyword comparison. CSS Syntax §4 makes an ident ASCII case-insensitive, and lexbor serializes a
   token as the author wrote it, so `HIDDEN` and `hidden` arrive here as different bytes and are one value.
   `kw` is the CANONICAL (lower-case) spelling, which is what CSSOM serializes back out. */
static bool css_word_is(const char *w, size_t n, const char *kw)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (kw[i] == '\0' || (char)tolower((unsigned char)w[i]) != kw[i]) return false;
    return kw[n] == '\0';
}

/* THE COMPONENT WORDS of a shorthand's value. `max` is the grammar's own multiplier, so a value carrying more
   words than the grammar admits is INVALID rather than truncated — reported as -1, which every caller turns
   into the dropped declaration. */
static int css_words(const char *v, const char **w, size_t *len, int max)
{
    int n = 0;

    while (*v) {
        const char *s;

        while (*v && isspace((unsigned char)*v)) v++;
        if (!*v) break;
        s = v;
        while (*v && !isspace((unsigned char)*v)) v++;
        if (n == max) return -1;
        w[n] = s;
        len[n] = (size_t)(v - s);
        n++;
    }
    return n;
}

/* css-overflow §3.1's value grammar for `overflow-x`/`overflow-y`, plus the LEGACY ALIAS the same section
   requires ("User agents must also support the overlay keyword as a legacy value alias of auto"). The alias is
   a SPECIFIED value in its own right and is mapped where the section's other value-to-value rule lives — the
   computed value — so this component keeps it. */
static const char *const OVERFLOW_KEYWORDS[] = {
    "visible", "hidden", "clip", "scroll", "auto", "overlay"
};

static const char *overflow_keyword(const char *w, size_t n)
{
    unsigned i;

    for (i = 0; i < sizeof(OVERFLOW_KEYWORDS) / sizeof(OVERFLOW_KEYWORDS[0]); i++)
        if (css_word_is(w, n, OVERFLOW_KEYWORDS[i])) return OVERFLOW_KEYWORDS[i];
    return NULL;
}

char *css_shorthand_component(const char *shorthand, const char *value, const char *longhand)
{
    const char *w[2], *kw[2];
    size_t wl[2];
    int n, axis;

    DCHECK(shorthand != NULL && value != NULL && longhand != NULL,
           "the shorthand expansion was asked about a NULL property name or value — a declaration always "
           "carries both by the time lexbor has serialized it");
    /* THE ONE SHORTHAND THIS COMPONENT EXPANDS. Every other name — a longhand, a custom property, a shorthand
       nothing here reads — sets no longhand through this path, which is what css_shorthand_complete_for is for:
       it is the assertion that the caller's longhand is one whose shorthands are all in this function. */
    if (strcmp(shorthand, "overflow") != 0) return NULL;
    if (strcmp(longhand, "overflow-x") == 0)      axis = 0;
    else if (strcmp(longhand, "overflow-y") == 0) axis = 1;
    else return NULL;
    /* css-overflow §3.1: `overflow: <'overflow-block'>{1,2}` "sets the specified values of overflow-x and
       overflow-y in that order. If the second value is omitted, it is copied from the first." */
    n = css_words(value, w, wl, 2);
    if (n < 1) return NULL;
    kw[0] = overflow_keyword(w[0], wl[0]);
    kw[1] = (n > 1) ? overflow_keyword(w[1], wl[1]) : kw[0];
    if (!kw[0] || !kw[1]) return NULL;   /* a value outside the grammar: the declaration is invalid */
    return css_sh_strdup(kw[axis]);
}

bool css_shorthand_complete_for(const char *longhand)
{
    /* THE LONGHANDS WHOSE COMPLETE SHORTHAND SET IS ABOVE, and what makes each complete:
         overflow-x, overflow-y — `overflow` is the only shorthand in CSS that sets them (css-overflow §3.1;
           the logical `overflow-block`/`overflow-inline` are longhands of the same group, not shorthands);
         display, float, position — NO shorthand sets any of the three.
       A name absent from this list is not "probably fine": it is a question nobody has answered, and the
       answer decides whether a `margin: 0` two lines up was read or ignored. */
    static const char *const RECORDED[] = { "overflow-x", "overflow-y", "display", "float", "position" };
    unsigned i;

    DCHECK(longhand != NULL, "the shorthand-completeness question was asked about a NULL property name");
    for (i = 0; i < sizeof(RECORDED) / sizeof(RECORDED[0]); i++)
        if (strcmp(RECORDED[i], longhand) == 0) return true;
    return false;
}
