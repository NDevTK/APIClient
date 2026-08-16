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

/* CSS 2.1 §8.3 and §8.4's FOUR-SIDE ROTATION, which `margin` and `padding` state in identical words: "If there
   is only one component value, it applies to all sides. If there are two values, the top and bottom … are set
   to the first value and the right and left … to the second. If there are three values, the top is set to the
   first value, the left and right are set to the second, and the bottom is set to the third. If there are four
   values, they apply to the top, right, bottom, and left, respectively." The table is that sentence: one row
   per component count, four columns in the longhands' own order.
   THE COMPONENT IS COPIED VERBATIM AND NOT RE-VALIDATED, which is the difference between this and the
   `overflow` expansion above. `overflow`'s components are KEYWORDS and the expansion has to answer with the
   canonical lower-case spelling, so it maps each word through the grammar. A `<margin-width>` is a length, a
   percentage or `auto`, and lexbor has ALREADY parsed and validated the whole declaration into its typed
   `lxb_css_property_margin_t` and serialized it back canonically — an invalid one never arrives, because
   css_style_declaration.c drops lexbor's `__UNDEF` before this is called. Re-parsing the number here would be
   a second copy of the length grammar with nothing to keep it in step. */
static const int SIDE_OF[5][4] = {
    { -1, -1, -1, -1 },   /* 0 components: not a declaration */
    {  0,  0,  0,  0 },   /* 1: all four sides */
    {  0,  1,  0,  1 },   /* 2: top/bottom, then right/left */
    {  0,  1,  2,  1 },   /* 3: top, then right/left, then bottom */
    {  0,  1,  2,  3 },   /* 4: top, right, bottom, left */
};

static int box_side_index(const char *shorthand, const char *longhand)
{
    static const char *const SIDES[] = { "top", "right", "bottom", "left" };
    size_t n = strlen(shorthand);
    unsigned i;

    if (strncmp(longhand, shorthand, n) != 0 || longhand[n] != '-') return -1;
    for (i = 0; i < 4; i++)
        if (strcmp(longhand + n + 1, SIDES[i]) == 0) return (int)i;
    return -1;
}

char *css_shorthand_component(const char *shorthand, const char *value, const char *longhand)
{
    const char *w[4], *kw[2];
    size_t wl[4];
    int n, axis, side;

    DCHECK(shorthand != NULL && value != NULL && longhand != NULL,
           "the shorthand expansion was asked about a NULL property name or value — a declaration always "
           "carries both by the time lexbor has serialized it");
    /* THE SHORTHANDS THIS COMPONENT EXPANDS. Every other name — a longhand, a custom property, a shorthand
       nothing here reads — sets no longhand through this path, which is what css_shorthand_complete_for is for:
       it is the assertion that the caller's longhand is one whose shorthands are all in this function. */
    if (strcmp(shorthand, "margin") == 0 || strcmp(shorthand, "padding") == 0) {
        char *out;
        int comp;

        side = box_side_index(shorthand, longhand);
        if (side < 0) return NULL;   /* this shorthand does not name that longhand */
        n = css_words(value, w, wl, 4);
        DCHECK(n >= 1 && n <= 4,
               "a `margin`/`padding` declaration serialized to a component count outside the {1,4} multiplier "
               "CSS 2.1 §8.3 and §8.4 give it — lexbor parses the shorthand into a FOUR-SLOT typed value and "
               "serializes only the slots that were set, so neither an empty value nor a fifth word can come "
               "out of a declaration its own parser accepted");
        if (n < 1) return NULL;      /* in release, an invalid declaration simply sets no longhand */
        comp = SIDE_OF[n][side];
        out = malloc(wl[comp] + 1);
        CHECK(out != NULL, "cssom: OOM expanding a box shorthand — a dropped component would read as "
                           "undeclared, which is the property's INITIAL value and a different number");
        memcpy(out, w[comp], wl[comp]);
        out[wl[comp]] = '\0';
        return out;
    }
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
         display, float, position, box-sizing — NO shorthand sets any of the four;
         the four margins — `margin` is the only shorthand that sets them (CSS 2.1 §8.3; the logical
           `margin-block`/`margin-inline` set the logical longhands, which are different properties);
         the four paddings — `padding`, likewise (§8.4);
         width, height, min-width, max-width, min-height, max-height — NO shorthand sets any of them. CSS 2.1
           has none, and css-sizing adds none: `flex` sets `flex-basis`, not `width`, and `aspect-ratio` is a
           longhand of its own.
       A name absent from this list is not "probably fine": it is a question nobody has answered, and the
       answer decides whether a `margin: 0` two lines up was read or ignored. */
    static const char *const RECORDED[] = {
        "overflow-x", "overflow-y", "display", "float", "position", "box-sizing",
        "margin-top", "margin-right", "margin-bottom", "margin-left",
        "padding-top", "padding-right", "padding-bottom", "padding-left",
        "width", "height", "min-width", "max-width", "min-height", "max-height",
    };
    unsigned i;

    DCHECK(longhand != NULL, "the shorthand-completeness question was asked about a NULL property name");
    for (i = 0; i < sizeof(RECORDED) / sizeof(RECORDED[0]); i++)
        if (strcmp(RECORDED[i], longhand) == 0) return true;
    return false;
}
