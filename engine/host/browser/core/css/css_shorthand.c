/* CSS Cascade §Shorthand Properties — see css_shorthand.h for why the expansion is here and not in lexbor. */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/css/css_shorthand.h"

static char *css_sh_strdup(const char *s)
{
    char *out = strdup(s);
    CHECK(out != NULL, "cssom: OOM expanding a shorthand — a dropped component would read as undeclared");
    return out;
}

/* One component value of a shorthand, copied out of the serialized declaration. OWNED. */
static char *css_sh_dupn(const char *s, size_t n)
{
    char *out = malloc(n + 1);

    CHECK(out != NULL, "cssom: OOM copying a shorthand component — a dropped one would read as undeclared, "
                       "which is the property's INITIAL value and a different number");
    memcpy(out, s, n);
    out[n] = '\0';
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

/* THE COMPONENT VALUES of a shorthand's value. `max` is the grammar's own multiplier, so a value carrying more
   components than the grammar admits is INVALID rather than truncated — reported as -1, which every caller
   turns into the dropped declaration.
   A FUNCTION IS ONE COMPONENT VALUE however many spaces its arguments carry, which is CSS Syntax §4's function
   token and not a nicety: `border: 1px solid rgb(1, 2, 3)` is THREE components, and a split on whitespace
   alone reports five and drops the declaration as over-long. So the depth is counted. */
static int css_words(const char *v, const char **w, size_t *len, int max)
{
    int n = 0;

    while (*v) {
        const char *s;
        int depth = 0;

        while (*v && isspace((unsigned char)*v)) v++;
        if (!*v) break;
        s = v;
        while (*v && (depth > 0 || !isspace((unsigned char)*v))) {
            if (*v == '(') depth++;
            else if (*v == ')' && depth > 0) depth--;
            v++;
        }
        if (n == max) return -1;
        w[n] = s;
        len[n] = (size_t)(v - s);
        n++;
    }
    return n;
}

#define CSS_SH_N(a) (sizeof(a) / sizeof((a)[0]))

/* The CANONICAL spelling of the keyword `w` in `set`, or NULL when it is none of them — which is how each of
   the grammars below both VALIDATES a component and lower-cases it for CSSOM to serialize back. */
static const char *css_sh_keyword(const char *const *set, unsigned n, const char *w, size_t len)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (css_word_is(w, len, set[i])) return set[i];
    return NULL;
}

/* css-overflow §3.1's value grammar for `overflow-x`/`overflow-y`, plus the LEGACY ALIAS the same section
   requires ("User agents must also support the overlay keyword as a legacy value alias of auto"). The alias is
   a SPECIFIED value in its own right and is mapped where the section's other value-to-value rule lives — the
   computed value — so this component keeps it. */
static const char *const OVERFLOW_KEYWORDS[] = {
    "visible", "hidden", "clip", "scroll", "auto", "overlay"
};

/* css-backgrounds-3 §3.2's `<line-style>`, entire and in the spec's own order, and §3.3's three `<line-width>`
   keywords. THESE GRAMMARS HAVE TO BE HERE, unlike `<margin-width>`: lexbor's property registry carries no
   `border-width` and no `border-style` (it has `border`, the four `border-<side>` shorthands and the four
   `border-*-color` longhands, and nothing else of the border), so those two declarations reach the cascade as
   a `__CUSTOM` holding the name and the RAW TOKENS with nothing having validated them. CSS Syntax DROPS an
   invalid declaration, and dropping it is what these two lists decide — a `border-style: nope` that flowed
   through would reach the computed value as a keyword no grammar admits. */
static const char *const LINE_STYLE_KEYWORDS[] = {
    "none", "hidden", "dotted", "dashed", "solid", "double", "groove", "ridge", "inset", "outset",
};
static const char *const LINE_WIDTH_KEYWORDS[] = { "thin", "medium", "thick" };

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

/* The four sides in the order every four-side rule in CSS states them, which is also the order of SIDE_OF's
   columns and the order `border-width: 1px 2px 3px 4px` assigns. */
static const char *const SIDES[] = { "top", "right", "bottom", "left" };

/* `margin`/`padding` name their longhands `<shorthand>-<side>`. */
static int box_side_index(const char *shorthand, const char *longhand)
{
    size_t n = strlen(shorthand);
    unsigned i;

    if (strncmp(longhand, shorthand, n) != 0 || longhand[n] != '-') return -1;
    for (i = 0; i < 4; i++)
        if (strcmp(longhand + n + 1, SIDES[i]) == 0) return (int)i;
    return -1;
}

/* The border group names its longhands `border-<side>-<part>` — the side in the MIDDLE, which is why the
   function above does not answer for them and why `border-width`'s rotation and `border-top`'s triple are
   both indexed through this one. -1 when `longhand` is not `border-<side>-<part>` for this `part`. */
static int border_side_index(const char *longhand, const char *part)
{
    static const char PFX[] = "border-";
    const char *p = longhand;
    unsigned i;

    if (strncmp(p, PFX, sizeof PFX - 1) != 0) return -1;
    p += sizeof PFX - 1;
    for (i = 0; i < 4; i++) {
        size_t n = strlen(SIDES[i]);

        if (strncmp(p, SIDES[i], n) == 0 && p[n] == '-' && strcmp(p + n + 1, part) == 0) return (int)i;
    }
    return -1;
}

/* css-backgrounds-3 §3.4's THREE TERMS, in the order its `<line-width> || <line-style> || <color>` states
   them, with the INITIAL VALUE each longhand takes when the shorthand's value omits it — §3.3's `medium`,
   §3.2's `none` and §3.1's `currentcolor`. The initial is load-bearing rather than decorative: CSS Cascade
   §Shorthand Properties says a shorthand "sets all of its longhand sub-properties, exactly as if expanded in
   place" and that "each missing sub-property is assigned its initial value", so `border: solid` sets
   `border-top-width` to `medium` — and answering NULL for it would let a `border-width: 5px` from an earlier
   declaration survive a later `border: solid` the cascade says overrode it. */
static const char *const BORDER_PARTS[] = { "width", "style", "color" };
static const char *const BORDER_PART_INITIAL[] = { "medium", "none", "currentcolor" };

/* Which of §3.4's three terms `longhand` is, and on which side. -1 when it is not a border longhand. */
static int border_part_index(const char *longhand, int *pside)
{
    unsigned i;

    for (i = 0; i < CSS_SH_N(BORDER_PARTS); i++) {
        int side = border_side_index(longhand, BORDER_PARTS[i]);

        if (side >= 0) { *pside = side; return (int)i; }
    }
    return -1;
}

/* WHICH TERM OF §3.4's `||` a component value is. The two keyword grammars are disjoint sets of idents and are
   asked first; a component that begins a NUMBER token is the `<line-width>`; everything else is the `<color>`.
   That last step is reading lexbor's answer rather than guessing at one — this branch is only ever reached for
   `border` and `border-<side>`, which ARE in its registry, so the declaration has already been parsed into a
   typed {width, style, color} and serialized back in this canonical order, and lexbor's own parser makes the
   identical split (a DIMENSION or NUMBER token goes to the width, an ident it does not recognise as one of the
   two keyword sets goes to the color). Re-implementing `<color>` here would be a second copy of a grammar
   core/css/css_color.h owns. */
static int border_term_of(const char *w, size_t n)
{
    if (css_sh_keyword(LINE_WIDTH_KEYWORDS, CSS_SH_N(LINE_WIDTH_KEYWORDS), w, n)) return 0;
    if (css_sh_keyword(LINE_STYLE_KEYWORDS, CSS_SH_N(LINE_STYLE_KEYWORDS), w, n)) return 1;
    if (n > 0 && (isdigit((unsigned char)w[0]) || w[0] == '.' || w[0] == '+' || w[0] == '-')) return 0;
    return 2;
}

/* §3.4's expansion: the component values in ANY ORDER, each term at most once, omitted terms initial. */
static char *border_triple_component(const char *value, int part)
{
    const char *w[3], *found[3] = { NULL, NULL, NULL };
    size_t wl[3], flen[3] = { 0, 0, 0 };
    int n, i;

    n = css_words(value, w, wl, 3);
    if (n < 1) return NULL;
    for (i = 0; i < n; i++) {
        int term = border_term_of(w[i], wl[i]);

        if (found[term] != NULL) return NULL;   /* `||` admits each term at most once: an invalid declaration */
        found[term] = w[i];
        flen[term] = wl[i];
    }
    if (found[part] != NULL) return css_sh_dupn(found[part], flen[part]);
    return css_sh_strdup(BORDER_PART_INITIAL[part]);
}

/* CSS 2.1 §8.3's four-side rotation applied to a shorthand whose components are `<line-width>` or
   `<line-style>` — css-backgrounds-3 §3.3 and §3.2 state the identical sentence for `border-width` and
   `border-style`, which is why this is SIDE_OF again and not a second table. The difference from `margin` and
   `padding` is VALIDATION: nothing has checked these components, so each is put through its grammar and a
   component outside it drops the whole declaration. §3.3's range is part of that grammar — `<line-width> =
   <length [0,∞]> | thin | medium | thick`, and "Negative values are invalid" — so a leading minus drops the
   declaration here rather than reaching the computed value as a width no border can have. */
static char *border_four_side_component(const char *value, int side, bool widths)
{
    const char *w[4], *kw;
    size_t wl[4];
    int n = css_words(value, w, wl, 4), i, comp;

    if (n < 1) return NULL;
    for (i = 0; i < n; i++) {
        if (widths) {
            char *probe;
            bool ok;

            if (css_sh_keyword(LINE_WIDTH_KEYWORDS, CSS_SH_N(LINE_WIDTH_KEYWORDS), w[i], wl[i])) continue;
            if (wl[i] > 0 && w[i][0] == '-') return NULL;
            probe = css_sh_dupn(w[i], wl[i]);
            ok = css_length_is_length(probe);
            free(probe);
            if (!ok) return NULL;
        } else if (!css_sh_keyword(LINE_STYLE_KEYWORDS, CSS_SH_N(LINE_STYLE_KEYWORDS), w[i], wl[i])) {
            return NULL;
        }
    }
    comp = SIDE_OF[n][side];
    /* A KEYWORD component answers with its canonical lower-case spelling, which is what CSSOM serializes back —
       the same reason `overflow`'s expansion maps through its grammar. A `<length>` is copied verbatim, the
       way `margin`'s is: core/css/css_length.h owns the unit's case-folding and every other question about it. */
    kw = widths ? css_sh_keyword(LINE_WIDTH_KEYWORDS, CSS_SH_N(LINE_WIDTH_KEYWORDS), w[comp], wl[comp])
                : css_sh_keyword(LINE_STYLE_KEYWORDS, CSS_SH_N(LINE_STYLE_KEYWORDS), w[comp], wl[comp]);
    return kw ? css_sh_strdup(kw) : css_sh_dupn(w[comp], wl[comp]);
}

char *css_shorthand_component(const char *shorthand, const char *value, const char *longhand)
{
    const char *w[4], *kw[2];
    size_t wl[4];
    int n, axis, side = -1, part, sh_side;

    DCHECK(shorthand != NULL && value != NULL && longhand != NULL,
           "the shorthand expansion was asked about a NULL property name or value — a declaration always "
           "carries both by the time lexbor has serialized it");
    /* THE SHORTHANDS THIS COMPONENT EXPANDS. Every other name — a longhand, a custom property, a shorthand
       nothing here reads — sets no longhand through this path, which is what css_shorthand_complete_for is for:
       it is the assertion that the caller's longhand is one whose shorthands are all in this function. */

    /* ---- css-backgrounds-3 §3.2, §3.3 and §3.4's BORDER SHORTHANDS ---------------------------------------- */
    part = border_part_index(longhand, &side);
    if (part >= 0) {
        bool triple = strcmp(shorthand, "border") == 0;

        sh_side = -1;
        if (!triple) {
            unsigned i;

            for (i = 0; i < 4; i++) {
                char name[32];

                snprintf(name, sizeof name, "border-%s", SIDES[i]);
                if (strcmp(shorthand, name) == 0) { triple = true; sh_side = (int)i; break; }
            }
        }
        /* `border-top` sets the TOP side's three longhands and no other side's; `border` sets all four. */
        if (triple && (sh_side < 0 || sh_side == side)) {
            /* CSS Cascade §Shorthand Properties: "If a shorthand is specified as one of the CSS-wide keywords,
               it sets all of its sub-properties to that keyword". It is the ENTIRE value when present, so it
               is answered before the shorthand's own grammar is tried — `border: initial` is not a
               `<line-width> || <line-style> || <color>` at all, and lexbor parses it into the one slot it has
               room for, which would otherwise read as a `<line-style>` on its way past. The keyword is handed
               on so CSS Cascade §7's DEFAULTING step is what resolves it, which is where the engine's missing
               inheritance crashes (css_computed_value.h). */
            if (css_wide_keyword(value)) return css_sh_strdup(value);
            return border_triple_component(value, part);
        }
        if (part == 0 && strcmp(shorthand, "border-width") == 0) {
            if (css_wide_keyword(value)) return css_sh_strdup(value);
            return border_four_side_component(value, side, true);
        }
        if (part == 1 && strcmp(shorthand, "border-style") == 0) {
            if (css_wide_keyword(value)) return css_sh_strdup(value);
            return border_four_side_component(value, side, false);
        }
        return NULL;   /* a border shorthand this component does not expand, or one that does not set this side */
    }
    if (strcmp(shorthand, "margin") == 0 || strcmp(shorthand, "padding") == 0) {
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
        return css_sh_dupn(w[comp], wl[comp]);
    }
    if (strcmp(shorthand, "overflow") != 0) return NULL;
    if (strcmp(longhand, "overflow-x") == 0)      axis = 0;
    else if (strcmp(longhand, "overflow-y") == 0) axis = 1;
    else return NULL;
    /* css-overflow §3.1: `overflow: <'overflow-block'>{1,2}` "sets the specified values of overflow-x and
       overflow-y in that order. If the second value is omitted, it is copied from the first." */
    n = css_words(value, w, wl, 2);
    if (n < 1) return NULL;
    kw[0] = css_sh_keyword(OVERFLOW_KEYWORDS, CSS_SH_N(OVERFLOW_KEYWORDS), w[0], wl[0]);
    kw[1] = (n > 1) ? css_sh_keyword(OVERFLOW_KEYWORDS, CSS_SH_N(OVERFLOW_KEYWORDS), w[1], wl[1]) : kw[0];
    if (!kw[0] || !kw[1]) return NULL;   /* a value outside the grammar: the declaration is invalid */
    return css_sh_strdup(kw[axis]);
}

bool css_shorthand_validates_longhand(const char *longhand)
{
    int side, part;

    DCHECK(longhand != NULL, "the longhand-grammar question was asked about a NULL property name");
    part = border_part_index(longhand, &side);
    (void)side;
    /* The four widths and the four styles. The four `border-*-color` longhands ARE in lexbor's registry — it
       carries them and no other border longhand — so their declarations are typed, validated and serialized
       by its own parser, and second-guessing that here would be a second `<color>` grammar. */
    return part == 0 || part == 1;
}

char *css_shorthand_longhand_value(const char *longhand, const char *value)
{
    const char *w[1], *kw;
    size_t wl[1];
    int side, part = border_part_index(longhand, &side);
    int n;

    (void)side;
    DCHECK(value != NULL, "a longhand declaration was validated with no value");
    DCHECK(css_shorthand_validates_longhand(longhand),
           "a longhand this component does not own the grammar of was routed through it — the predicate and "
           "this function are one list and have come apart, and the failure is silent in the direction that "
           "matters: a property lexbor DOES type would have its own parser's answer replaced by this one");
    /* CSS Cascade §7.3's keywords are a value for EVERY property, so they precede the property's own grammar
       and are handed on for §7's DEFAULTING step to resolve (css_computed_value.h says where that crashes). */
    if (css_wide_keyword(value)) return css_sh_strdup(value);
    /* `<line-width>` and `<line-style>` are each ONE component value — no multiplier — so a second one is an
       invalid declaration and css_words reports it by refusing to write past `max`. */
    n = css_words(value, w, wl, 1);
    if (n != 1) return NULL;
    if (part == 1) {
        kw = css_sh_keyword(LINE_STYLE_KEYWORDS, CSS_SH_N(LINE_STYLE_KEYWORDS), w[0], wl[0]);
        return kw ? css_sh_strdup(kw) : NULL;
    }
    kw = css_sh_keyword(LINE_WIDTH_KEYWORDS, CSS_SH_N(LINE_WIDTH_KEYWORDS), w[0], wl[0]);
    if (kw) return css_sh_strdup(kw);
    if (wl[0] > 0 && w[0][0] == '-') return NULL;   /* §3.3: "Negative values are invalid" */
    {
        char *probe = css_sh_dupn(w[0], wl[0]);

        if (css_length_is_length(probe)) return probe;
        free(probe);
        return NULL;
    }
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
           longhand of its own;
         the four border-*-width and the four border-*-style — THREE shorthands set each of them and all three
           are above: `border-width` (or `border-style`), `border-<side>` and `border`, which is
           css-backgrounds-3 §3.3, §3.2 and §3.4. `border-image` sets no width or style longhand (its own
           `border-image-width` is a different property), and the logical `border-block`/`border-inline` group
           sets the logical longhands, which lexbor's registry does not carry and which are different
           properties for the same reason `margin-block` is.
       WHAT IS DELIBERATELY NOT HERE: the four `border-*-color`. `border` and `border-<side>` above DO answer
       for them — the component is in hand, and returning NULL would be a wrong answer to a question this
       function claims to answer — but `border-color` is a shorthand nothing expands, because validating its
       raw components means the `<color>` grammar (core/css/css_color.h) and that is the next subproblem, not
       this one. So the set is INCOMPLETE and this says so, which is exactly what stops a consumer trusting it.
       A name absent from this list is not "probably fine": it is a question nobody has answered, and the
       answer decides whether a `margin: 0` two lines up was read or ignored. */
    static const char *const RECORDED[] = {
        "overflow-x", "overflow-y", "display", "float", "position", "box-sizing",
        "margin-top", "margin-right", "margin-bottom", "margin-left",
        "padding-top", "padding-right", "padding-bottom", "padding-left",
        "width", "height", "min-width", "max-width", "min-height", "max-height",
        "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
        "border-top-style", "border-right-style", "border-bottom-style", "border-left-style",
    };
    unsigned i;

    DCHECK(longhand != NULL, "the shorthand-completeness question was asked about a NULL property name");
    for (i = 0; i < sizeof(RECORDED) / sizeof(RECORDED[0]); i++)
        if (strcmp(RECORDED[i], longhand) == 0) return true;
    return false;
}
