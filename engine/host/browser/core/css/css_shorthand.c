/* CSS Cascade §Shorthand Properties — see css_shorthand.h for why the expansion is here and not in lexbor. */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_color.h"
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

/* css-backgrounds-3 §3.4's OTHER HALF, which its own prose states outright: "The border shorthand also resets
   border-image to its initial value." That is not a footnote here — CSSOM §6.6 re-forms a shorthand only when
   EVERY longhand it sets is present in the block, so a `border` that did not set these five would be a
   shorthand no declaration block could ever put back together, and `border: 1px solid red` would read back out
   of `cssText` as three separate four-side shorthands. "Resets" means the value is the same whatever the
   border's own value is; the declaration still has to be VALID first, which §3.4's own grammar decides. */
static const char *const BORDER_IMAGE_LONGHANDS[] = {
    "border-image-source", "border-image-slice", "border-image-width",
    "border-image-outset", "border-image-repeat",
};
/* css-backgrounds-3 §6.1 through §6.5's `Initial:` lines, in that same order. */
static const char *const BORDER_IMAGE_INITIAL[] = { "none", "100%", "1", "0", "stretch" };

static int border_image_index(const char *longhand)
{
    unsigned i;

    for (i = 0; i < sizeof(BORDER_IMAGE_LONGHANDS) / sizeof(BORDER_IMAGE_LONGHANDS[0]); i++)
        if (strcmp(BORDER_IMAGE_LONGHANDS[i], longhand) == 0) return (int)i;
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
/* THE FOUR-SIDE SHORTHAND OF EACH PART, indexed the same way — §3.3's `border-width`, §3.2's `border-style` and
   §3.1's `border-color`, each `<its component>{1,4}` over the same rotation. css_shorthand_init asserts that
   each one's recorded longhand list IS the four `border-<side>-<part>`, so the three tables cannot drift. */
static const char *const BORDER_PART_SHORTHAND[] = { "border-width", "border-style", "border-color" };

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

/* WHICH GRAMMAR a `border-<part>`'s four components are written in — css-backgrounds-3 §3.3's `<line-width>`,
   §3.2's `<line-style>` and §3.1's `<color>`. The three rotations are one algorithm over three component
   grammars, which is why this is an argument and not three functions. */
typedef enum { CSS_BORDER_PART_WIDTH = 0, CSS_BORDER_PART_STYLE = 1, CSS_BORDER_PART_COLOR = 2 } CssBorderPart;

/* Is `w` a valid component of that grammar? A component outside it drops the whole declaration, which is CSS
   Syntax's invalid declaration and the reason this is a validation and not a split.
   §3.1's `<color>` is asked of core/css/css_color.h, which is THE agent's `<color>` — the same parse HTML's
   colour well and the canvas go through. Asking it here rather than reimplementing it is what the note this
   replaces asked for: `border-color` was the one row this component recorded and did NOT expand, so
   `border-color: red` set no longhand at all, reached the cascade as nothing, and made every `border-*-color`
   answer FALSE to css_shorthand_complete_for. */
static bool border_part_component_valid(CssBorderPart part, const char *w, size_t n)
{
    char *probe;
    bool ok;

    if (part == CSS_BORDER_PART_STYLE)
        return css_sh_keyword(LINE_STYLE_KEYWORDS, CSS_SH_N(LINE_STYLE_KEYWORDS), w, n) != NULL;
    if (part == CSS_BORDER_PART_COLOR) {
        CssColor c;

        return css_color_parse(w, n, &c);
    }
    if (css_sh_keyword(LINE_WIDTH_KEYWORDS, CSS_SH_N(LINE_WIDTH_KEYWORDS), w, n)) return true;
    if (n > 0 && w[0] == '-') return false;   /* §3.3: "Negative values are invalid" */
    probe = css_sh_dupn(w, n);
    ok = css_length_is_length(probe);
    free(probe);
    return ok;
}

/* CSS 2.1 §8.3's four-side rotation applied to a shorthand whose components are `<line-width>`, `<line-style>`
   or `<color>` — css-backgrounds-3 §3.3, §3.2 and §3.1 state the identical sentence for `border-width`,
   `border-style` and `border-color`, which is why this is SIDE_OF again and not a second table. The difference
   from `margin` and `padding` is VALIDATION: nothing has checked these components, so each is put through its
   grammar and a component outside it drops the whole declaration. */
static char *border_four_side_component(const char *value, int side, CssBorderPart part)
{
    const char *w[4], *kw;
    size_t wl[4];
    int n = css_words(value, w, wl, 4), i, comp;

    if (n < 1) return NULL;
    for (i = 0; i < n; i++)
        if (!border_part_component_valid(part, w[i], wl[i])) return NULL;
    comp = SIDE_OF[n][side];
    /* A KEYWORD component answers with its canonical lower-case spelling, which is what CSSOM serializes back —
       the same reason `overflow`'s expansion maps through its grammar. A `<length>` is copied verbatim, the
       way `margin`'s is: core/css/css_length.h owns the unit's case-folding and every other question about it.
       A `<color>` is copied verbatim too, and for the same reason one property along: this is the SPECIFIED
       value, and which of `red` / `#f00` / `rgb(255 0 0)` was written is what §6.7.1 serializes back. */
    kw = (part == CSS_BORDER_PART_WIDTH)
             ? css_sh_keyword(LINE_WIDTH_KEYWORDS, CSS_SH_N(LINE_WIDTH_KEYWORDS), w[comp], wl[comp])
             : (part == CSS_BORDER_PART_STYLE)
                   ? css_sh_keyword(LINE_STYLE_KEYWORDS, CSS_SH_N(LINE_STYLE_KEYWORDS), w[comp], wl[comp])
                   : NULL;
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
    /* §3.4's reset of `border-image`, ahead of the side/part split because these five longhands carry the side
       nowhere in their names. A CSS-wide keyword is the whole value and is handed on the same way the triple's
       is; otherwise the triple's grammar runs as the VALIDITY test, because an invalid `border` declaration is
       dropped and sets nothing at all — including these. */
    if (strcmp(shorthand, "border") == 0) {
        int img = border_image_index(longhand);

        if (img >= 0) {
            char *valid;

            if (css_wide_keyword(value)) return css_sh_strdup(value);
            valid = border_triple_component(value, 0);
            if (!valid) return NULL;
            free(valid);
            return css_sh_strdup(BORDER_IMAGE_INITIAL[img]);
        }
    }
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
        /* `border-width`, `border-style` and `border-color` each rotate over the FOUR SIDES of their own part
           and set no other part's longhand — which the part index says outright, so the three rows are one
           branch rather than three. */
        if (strcmp(shorthand, BORDER_PART_SHORTHAND[part]) == 0) {
            if (css_wide_keyword(value)) return css_sh_strdup(value);
            return border_four_side_component(value, side, (CssBorderPart)part);
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

/* ---- CSSOM §6.6's REVERSE DIRECTION -----------------------------------------------------------------------
 *
 * THE FORWARD DIRECTION ABOVE ANSWERS ONE QUESTION AT A TIME — "what does this declaration give that longhand"
 * — and the cascade never needs another, because it resolves one property for one element. CSSOM §6.6's
 * SERIALIZE A CSS DECLARATION BLOCK asks the mirror question and cannot be written without it: given the whole
 * set of declarations, which shorthand covers a group of them, and what value would that shorthand carry.
 * Its declaration loop is "if property maps to one or more shorthand properties, let shorthands be an array of
 * those shorthand properties, IN PREFERRED ORDER", and its shorthand loop then needs the shorthand's OWN
 * longhand list to ask whether all of them are present.
 *
 * SO THE TABLE IS THE COMPONENT, AND THE TWO DIRECTIONS ARE TWO READINGS OF IT. The reverse reading is derived
 * by scanning the rows rather than typed a second time, which is the only way "margin-top's shorthands" and
 * "margin's longhands" cannot come apart — and the forward reading is tied to it by css_shorthand_init's round
 * trip, which runs EVERY row's fixture through the expansion and back, so a longhand added to a row with no
 * branch to expand it crashes at init.
 *
 * EVERY ROW IN THE TABLE IS EXPANDED, and `border-color` was the one that was not. Putting four `border-*-color`
 * declarations lexbor TYPED back together needs no grammar of this component's — the values are already
 * canonical — while taking `border-color: red green` APART needs the `<color>` production, so the row sat in the
 * table with an `expands: false` flag beside it. What that flag actually bought was a `border-color: red` that
 * set NO longhand: the cascade read every `border-*-color` as undeclared, and css_shorthand_complete_for
 * answered FALSE for the four of them so every consumer that asserts completeness crashed on them. The `<color>`
 * production is core/css/css_color.h's — the agent's ONE `<color>`, the same parse the colour well and the
 * canvas go through — so it is ASKED, the row expands like every other, and the flag is deleted.
 *
 * WHAT IS DELIBERATELY NOT IN THE TABLE AT ALL: every CSS shorthand this engine has no grammar for — `flex`,
 * `flex-flow`, `text-decoration`, `background`, `font`. css_shorthand_is_shorthand answers FALSE for each, and
 * the block serialization then emits their longhands separately, which is what it does today. */

/* HOW A SHORTHAND'S VALUE IS PUT BACK TOGETHER — CSSOM §6.7.2's "serialize a CSS value ... with a list", which
   is "serialize a CSS value from a hypothetical declaration of the property shorthand with its value
   representing the combined values of the declarations in list", under §6.7.2's two syntactic rules: reorder
   `||` terms into the property's canonical order, and OMIT any component that can be dropped or shortened
   without changing the meaning. Each kind is one grammar's answer to those two rules, run backwards. */
typedef enum {
    CSS_SH_FOUR_SIDE,   /* CSS 2.1 §8.3's rotation reversed: top/right/bottom/left with the tail dropped */
    CSS_SH_TWO_AXIS,    /* css-overflow §3.1's `{1,2}`: the second omitted when it equals the first */
    CSS_SH_TRIPLE,      /* css-backgrounds-3 §3.4's `||`, each term omitted when it is that longhand's initial */
    CSS_SH_BORDER       /* §3.4's `border`: one triple common to all four sides, over an untouched border-image */
} CssShKind;

typedef struct {
    const char *name;
    const char *const *longhands;
    unsigned n;
    CssShKind kind;
    /* ONE FIXTURE VALUE, in the canonical form the round trip must reproduce — expand it into every longhand,
       consolidate the results back, and the string must come out unchanged. Every row carries one, because
       css_shorthand_component answers for every row's every longhand: a row that did not was a shorthand whose
       declarations set NOTHING, and the flag that used to record that is deleted with the row that had it. */
    const char *probe;
} CssShorthandRow;

static const char *const LH_MARGIN[]  = { "margin-top", "margin-right", "margin-bottom", "margin-left" };
static const char *const LH_PADDING[] = { "padding-top", "padding-right", "padding-bottom", "padding-left" };
static const char *const LH_OVERFLOW[] = { "overflow-x", "overflow-y" };
static const char *const LH_BORDER_WIDTH[] = {
    "border-top-width", "border-right-width", "border-bottom-width", "border-left-width"
};
static const char *const LH_BORDER_STYLE[] = {
    "border-top-style", "border-right-style", "border-bottom-style", "border-left-style"
};
static const char *const LH_BORDER_COLOR[] = {
    "border-top-color", "border-right-color", "border-bottom-color", "border-left-color"
};
static const char *const LH_BORDER_TOP[]    = { "border-top-width", "border-top-style", "border-top-color" };
static const char *const LH_BORDER_RIGHT[]  = { "border-right-width", "border-right-style", "border-right-color" };
static const char *const LH_BORDER_BOTTOM[] = { "border-bottom-width", "border-bottom-style", "border-bottom-color" };
static const char *const LH_BORDER_LEFT[]   = { "border-left-width", "border-left-style", "border-left-color" };
/* §3.4's `border` — the twelve side longhands and the five `border-image` ones it resets. Grouped by PART
   rather than by side, because CSS_SH_BORDER reads them positionally: 0-3 widths, 4-7 styles, 8-11 colors,
   12-16 image. §6.6.1's getPropertyValue walks the same list "in canonical order" and is order-insensitive
   there — it requires every one of them and stops at the first that is missing. */
static const char *const LH_BORDER[] = {
    "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
    "border-top-style", "border-right-style", "border-bottom-style", "border-left-style",
    "border-top-color", "border-right-color", "border-bottom-color", "border-left-color",
    "border-image-source", "border-image-slice", "border-image-width",
    "border-image-outset", "border-image-repeat",
};

/* IN ASCENDING NAME ORDER, asserted by css_shorthand_init — the preferred-order sort's first step is
   lexicographic, and a table that is already sorted is what makes that step a scan instead of a comparison
   nobody would notice going wrong. */
static const CssShorthandRow SHORTHANDS[] = {
    { "border",        LH_BORDER,        17, CSS_SH_BORDER,    "1px solid red" },
    { "border-bottom", LH_BORDER_BOTTOM,  3, CSS_SH_TRIPLE,    "1px solid red" },
    { "border-color",  LH_BORDER_COLOR,   4, CSS_SH_FOUR_SIDE, "red green" },
    { "border-left",   LH_BORDER_LEFT,    3, CSS_SH_TRIPLE,    "1px solid red" },
    { "border-right",  LH_BORDER_RIGHT,   3, CSS_SH_TRIPLE,    "1px solid red" },
    { "border-style",  LH_BORDER_STYLE,   4, CSS_SH_FOUR_SIDE, "solid" },
    { "border-top",    LH_BORDER_TOP,     3, CSS_SH_TRIPLE,    "1px solid red" },
    { "border-width",  LH_BORDER_WIDTH,   4, CSS_SH_FOUR_SIDE, "1px" },
    { "margin",        LH_MARGIN,         4, CSS_SH_FOUR_SIDE, "1px 2px 3px 4px" },
    { "overflow",      LH_OVERFLOW,       2, CSS_SH_TWO_AXIS,  "hidden auto" },
    { "padding",       LH_PADDING,        4, CSS_SH_FOUR_SIDE, "1px 2px" },
};

static const CssShorthandRow *css_sh_row(const char *name)
{
    unsigned i;

    for (i = 0; i < CSS_SH_N(SHORTHANDS); i++)
        if (strcmp(SHORTHANDS[i].name, name) == 0) return &SHORTHANDS[i];
    return NULL;
}

const char *const *css_shorthand_longhands(const char *shorthand, unsigned *pn)
{
    const CssShorthandRow *row;

    DCHECK(shorthand != NULL && pn != NULL,
           "the shorthand's longhand list was asked for with no name or nowhere to report the count");
    row = css_sh_row(shorthand);
    *pn = row ? row->n : 0;
    return row ? row->longhands : NULL;
}

bool css_shorthand_is_shorthand(const char *name)
{
    DCHECK(name != NULL, "the shorthand question was asked about a NULL property name");
    return css_sh_row(name) != NULL;
}

/* CSSOM §6.6's PREFERRED ORDER is a definition, not a UA preference, and it is FOUR steps applied in its own
   order — each a stable rearrangement of the one before, so the last is the primary key and the first is the
   tie-break. Written out step by step rather than collapsed into one comparator, because the collapsed form is
   where the tie-break silently inverts. */
static bool css_sh_prefixed(const char *s) { return s[0] == '-'; }

static bool css_sh_prefixed_not_webkit(const char *s)
{
    static const char WK[] = "-webkit-";

    return s[0] == '-' && strncmp(s, WK, sizeof WK - 1) != 0;
}

static void css_sh_move_last(const char **v, unsigned n, bool (*pred)(const char *))
{
    unsigned i, at = 0;
    const char *keep[CSS_SHORTHAND_MAX_OF], *moved[CSS_SHORTHAND_MAX_OF];
    unsigned nk = 0, nm = 0;

    for (i = 0; i < n; i++) {
        if (pred(v[i])) moved[nm++] = v[i];
        else keep[nk++] = v[i];
    }
    for (i = 0; i < nk; i++) v[at++] = keep[i];
    for (i = 0; i < nm; i++) v[at++] = moved[i];
}

unsigned css_shorthand_shorthands_of(const char *longhand, const char **out, unsigned max)
{
    unsigned n = 0, i, j;

    DCHECK(longhand != NULL && out != NULL,
           "the longhand's shorthand set was asked for with no name or nowhere to write it");
    DCHECK(max >= CSS_SHORTHAND_MAX_OF,
           "a caller offered less room than a longhand's shorthand set can need — CSS_SHORTHAND_MAX_OF is the "
           "table's own maximum and css_shorthand_init asserts the table against it");
    for (i = 0; i < CSS_SH_N(SHORTHANDS); i++) {
        for (j = 0; j < SHORTHANDS[i].n; j++) {
            if (strcmp(SHORTHANDS[i].longhands[j], longhand) != 0) continue;
            CHECK(n < max, "cssom: a longhand maps to more shorthands than the caller made room for — the "
                           "next write would be past the end of its array");
            out[n++] = SHORTHANDS[i].name;
            break;
        }
    }
    CHECK(n <= CSS_SHORTHAND_MAX_OF,
          "cssom: a longhand maps to more shorthands than CSS_SHORTHAND_MAX_OF — the preferred-order steps "
          "below rearrange through arrays sized by that constant, so raise it with the row that broke it");
    /* Step 1, lexicographic: the table is already in ascending name order and is scanned in that order, which
       css_shorthand_init asserts — so the scan above IS this step, and a table that stopped being sorted
       crashes there rather than silently reordering `border-top` ahead of `border-color` here. */
    /* Steps 2 and 3: the prefixed spellings last, then the non-`-webkit-` prefixed ones after them. */
    css_sh_move_last(out, n, css_sh_prefixed);
    css_sh_move_last(out, n, css_sh_prefixed_not_webkit);
    /* Step 4, the primary key: the greatest number of longhands first, stably. `border` (17) therefore
       precedes `border-width` (4), which precedes `border-top` (3) — which is what makes a block holding all
       twelve side longhands serialize as `border-width; border-style; border-color` rather than as four
       `border-<side>` declarations, and what makes one holding all seventeen serialize as `border`. */
    for (i = 1; i < n; i++) {
        const char *cur = out[i];
        unsigned cn = css_sh_row(cur)->n;

        for (j = i; j > 0 && css_sh_row(out[j - 1])->n < cn; j--) out[j] = out[j - 1];
        out[j] = cur;
    }
    return n;
}

/* CSS Logical §2: "any pair of flow-relative properties and physical properties (ignoring shorthand
   properties) related by setting equivalent styles on the various sides or dimensions of a box, forms a
   logical property group ... paired properties share a computed value ... determined by cascading the
   declarations of both properties together as one". THAT is why §6.6's serialization refuses to re-form a
   shorthand across one: `margin: 10px` written where `margin-top` and `margin-inline-start` were declared in
   a particular order would move one of them past the other, and the pair's shared computed value is decided by
   which came last.
   ONLY THE GROUPS THIS COMPONENT'S SHORTHANDS REACH ARE RECORDED, which is the complete answer for the
   question asked: a declaration whose group is not recorded reads as group 0, and group 0 equals no group, so
   it never blocks a consolidation it has no business blocking. The flow-relative spellings are here even
   though lexbor's registry does not carry them — `margin-inline-start` reaches a declaration block as a
   `__CUSTOM` and is exactly the declaration that has to block one. */
enum { CSS_LG_NONE = 0, CSS_LG_MARGIN, CSS_LG_PADDING, CSS_LG_BORDER_WIDTH, CSS_LG_BORDER_STYLE,
       CSS_LG_BORDER_COLOR, CSS_LG_OVERFLOW };

static const struct { const char *name; unsigned group; bool physical; } LOGICAL_GROUP[] = {
    { "margin-top", CSS_LG_MARGIN, true }, { "margin-right", CSS_LG_MARGIN, true },
    { "margin-bottom", CSS_LG_MARGIN, true }, { "margin-left", CSS_LG_MARGIN, true },
    { "margin-block-start", CSS_LG_MARGIN, false }, { "margin-block-end", CSS_LG_MARGIN, false },
    { "margin-inline-start", CSS_LG_MARGIN, false }, { "margin-inline-end", CSS_LG_MARGIN, false },

    { "padding-top", CSS_LG_PADDING, true }, { "padding-right", CSS_LG_PADDING, true },
    { "padding-bottom", CSS_LG_PADDING, true }, { "padding-left", CSS_LG_PADDING, true },
    { "padding-block-start", CSS_LG_PADDING, false }, { "padding-block-end", CSS_LG_PADDING, false },
    { "padding-inline-start", CSS_LG_PADDING, false }, { "padding-inline-end", CSS_LG_PADDING, false },

    { "border-top-width", CSS_LG_BORDER_WIDTH, true }, { "border-right-width", CSS_LG_BORDER_WIDTH, true },
    { "border-bottom-width", CSS_LG_BORDER_WIDTH, true }, { "border-left-width", CSS_LG_BORDER_WIDTH, true },
    { "border-block-start-width", CSS_LG_BORDER_WIDTH, false },
    { "border-block-end-width", CSS_LG_BORDER_WIDTH, false },
    { "border-inline-start-width", CSS_LG_BORDER_WIDTH, false },
    { "border-inline-end-width", CSS_LG_BORDER_WIDTH, false },

    { "border-top-style", CSS_LG_BORDER_STYLE, true }, { "border-right-style", CSS_LG_BORDER_STYLE, true },
    { "border-bottom-style", CSS_LG_BORDER_STYLE, true }, { "border-left-style", CSS_LG_BORDER_STYLE, true },
    { "border-block-start-style", CSS_LG_BORDER_STYLE, false },
    { "border-block-end-style", CSS_LG_BORDER_STYLE, false },
    { "border-inline-start-style", CSS_LG_BORDER_STYLE, false },
    { "border-inline-end-style", CSS_LG_BORDER_STYLE, false },

    { "border-top-color", CSS_LG_BORDER_COLOR, true }, { "border-right-color", CSS_LG_BORDER_COLOR, true },
    { "border-bottom-color", CSS_LG_BORDER_COLOR, true }, { "border-left-color", CSS_LG_BORDER_COLOR, true },
    { "border-block-start-color", CSS_LG_BORDER_COLOR, false },
    { "border-block-end-color", CSS_LG_BORDER_COLOR, false },
    { "border-inline-start-color", CSS_LG_BORDER_COLOR, false },
    { "border-inline-end-color", CSS_LG_BORDER_COLOR, false },

    /* css-overflow §3.1's property definition table says it in one line — "Logical property group: overflow" —
       for all four of `overflow-x`, `overflow-y`, `overflow-block` and `overflow-inline`. */
    { "overflow-x", CSS_LG_OVERFLOW, true }, { "overflow-y", CSS_LG_OVERFLOW, true },
    { "overflow-block", CSS_LG_OVERFLOW, false }, { "overflow-inline", CSS_LG_OVERFLOW, false },
};

unsigned css_shorthand_logical_group(const char *longhand, bool *pphysical)
{
    unsigned i;

    DCHECK(longhand != NULL && pphysical != NULL,
           "the logical-property-group question was asked with no name or nowhere to report the mapping logic");
    *pphysical = true;
    for (i = 0; i < CSS_SH_N(LOGICAL_GROUP); i++) {
        if (strcmp(LOGICAL_GROUP[i].name, longhand) != 0) continue;
        *pphysical = LOGICAL_GROUP[i].physical;
        return LOGICAL_GROUP[i].group;
    }
    return CSS_LG_NONE;
}

/* ---- §6.7.2's serialize a CSS value, over a list of longhands --------------------------------------------- */

static char *css_sh_join(const char *const *parts, unsigned n)
{
    size_t total = 1;
    unsigned i;
    char *out;
    size_t at = 0;

    for (i = 0; i < n; i++) total += strlen(parts[i]) + 1;
    out = malloc(total);
    CHECK(out != NULL, "cssom: OOM serializing a shorthand value — a dropped one reads as the empty string, "
                       "which is how §6.6 is told the shorthand could not represent the declarations");
    for (i = 0; i < n; i++) {
        size_t l = strlen(parts[i]);

        if (i) out[at++] = ' ';
        memcpy(out + at, parts[i], l);
        at += l;
    }
    out[at] = '\0';
    return out;
}

/* CSS Cascade §7.3's keywords are a value for EVERY property, and a shorthand carrying one "sets all of its
   sub-properties to that keyword" — so a list in which every longhand carries the SAME keyword is exactly what
   such a declaration produced and serializes back as the keyword. Any other mixture is a list no single
   declaration could have produced, and §6.7.2's answer for that is the empty string.
   NOTE THAT THIS ENGINE NEVER STORES THE KEYWORD `initial` FOR AN OMITTED SUB-PROPERTY — border_triple_component
   answers with the initial VALUE (`medium`, `none`, `currentcolor`) — so there is no shorthand here that has to
   tolerate a mixture containing it. Returns 1 when it answered, 0 when no longhand carries a keyword at all,
   and -1 for the mixture. */
static int css_sh_wide_value(const char *const *v, unsigned n, char **out)
{
    unsigned i;
    bool any = false;

    for (i = 0; i < n; i++)
        if (css_wide_keyword(v[i])) { any = true; break; }
    if (!any) return 0;
    for (i = 1; i < n; i++)
        if (strcmp(v[i], v[0]) != 0) return -1;
    if (!css_wide_keyword(v[0])) return -1;
    *out = css_sh_strdup(v[0]);
    return 1;
}

/* CSS 2.1 §8.3's rotation run backwards. The three tests are the exact inverse of SIDE_OF's rows: the fourth
   component is written only when left differs from right, the third only when bottom differs from top or a
   fourth is being written, the second only when right differs from top or a third is. */
static char *css_sh_four_side_value(const char *const *v)
{
    const char *parts[4];
    unsigned n = 1;
    bool show_left = strcmp(v[1], v[3]) != 0;
    bool show_bottom = strcmp(v[0], v[2]) != 0 || show_left;
    bool show_right = strcmp(v[0], v[1]) != 0 || show_bottom;

    parts[0] = v[0];
    if (show_right) parts[n++] = v[1];
    if (show_bottom) parts[n++] = v[2];
    if (show_left) parts[n++] = v[3];
    return css_sh_join(parts, n);
}

/* css-overflow §3.1's `{1,2}`: "if the second value is omitted, it is copied from the first". */
static char *css_sh_two_axis_value(const char *const *v)
{
    if (strcmp(v[0], v[1]) == 0) return css_sh_strdup(v[0]);
    return css_sh_join(v, 2);
}

/* css-backgrounds-3 §3.4's `<line-width> || <line-style> || <color>`, written in the canonical order of the
   grammar with each term omitted when it holds that longhand's initial value — §6.7.2's "if component values
   can be omitted ... without changing the meaning of the value, omit them", and BORDER_PART_INITIAL is the same
   list the forward expansion fills an omitted term from. Every term initial leaves nothing to write, and the
   empty string is §6.7.2's own answer for a shorthand that cannot represent the list. */
static char *css_sh_triple_value(const char *width, const char *style, const char *color)
{
    const char *v[3], *parts[3];
    unsigned n = 0, i;

    v[0] = width; v[1] = style; v[2] = color;
    for (i = 0; i < 3; i++)
        if (strcmp(v[i], BORDER_PART_INITIAL[i]) != 0) parts[n++] = v[i];
    if (n == 0) return NULL;
    return css_sh_join(parts, n);
}

/* §3.4's `border`, which "cannot set different values on the four borders": a list whose four widths, four
   styles or four colors disagree is one no `border` declaration could have produced. And the five
   `border-image` longhands it resets must still hold what it reset them to — a `border-image` set afterwards
   is precisely the case WPT's border-shorthand-serialization.html pins, where `rule.style.border` is the empty
   string although all twelve side longhands are present. */
static char *css_sh_border_value(const char *const *v)
{
    unsigned i;

    for (i = 0; i < CSS_SH_N(BORDER_IMAGE_INITIAL); i++)
        if (strcmp(v[12 + i], BORDER_IMAGE_INITIAL[i]) != 0) return NULL;
    for (i = 1; i < 4; i++)
        if (strcmp(v[i], v[0]) != 0 || strcmp(v[4 + i], v[4]) != 0 || strcmp(v[8 + i], v[8]) != 0) return NULL;
    return css_sh_triple_value(v[0], v[4], v[8]);
}

char *css_shorthand_serialize_value(const char *shorthand, const char *const *values)
{
    const CssShorthandRow *row;
    char *wide = NULL;
    unsigned i;
    int w;

    DCHECK(shorthand != NULL && values != NULL,
           "§6.7.2's serialize-a-CSS-value was asked for with no shorthand or no values");
    row = css_sh_row(shorthand);
    DCHECK(row != NULL,
           "§6.7.2's serialize-a-CSS-value was asked for a name this component does not record as a shorthand. "
           "The caller reached here through css_shorthand_shorthands_of, which only ever names rows of the "
           "table above, so the two have come apart");
    if (!row) return NULL;
    for (i = 0; i < row->n; i++)
        DCHECK(values[i] != NULL,
               "§6.7.2's serialize-a-CSS-value was handed a list with a longhand missing. §6.6's loop asks "
               "whether ALL of a shorthand's longhands are present BEFORE it builds the list, so a hole here "
               "is that test having been skipped — and a hole read as a value is the shorthand claiming a "
               "declaration the block never made");
    w = css_sh_wide_value(values, row->n, &wide);
    if (w != 0) return w > 0 ? wide : NULL;
    switch (row->kind) {
    case CSS_SH_FOUR_SIDE: return css_sh_four_side_value(values);
    case CSS_SH_TWO_AXIS:  return css_sh_two_axis_value(values);
    case CSS_SH_TRIPLE:    return css_sh_triple_value(values[0], values[1], values[2]);
    default:
        DCHECK(row->kind == CSS_SH_BORDER,
               "a shorthand row carries a value-serialization kind this switch does not implement");
        return css_sh_border_value(values);
    }
}

void css_shorthand_init(void)
{
#if APICLIENT_DEV
    unsigned i, j, k;

    for (i = 0; i < CSS_SH_N(SHORTHANDS); i++) {
        const CssShorthandRow *row = &SHORTHANDS[i];
        const char *sh[CSS_SHORTHAND_MAX_OF];
        char *values[CSS_SHORTHAND_MAX_LONGHANDS];
        char *back;
        unsigned back_n;

        DCHECK(i == 0 || strcmp(SHORTHANDS[i - 1].name, row->name) < 0,
               "css_shorthand.c's shorthand table is not in ascending name order, and CSSOM §6.6's preferred "
               "order takes its lexicographic tie-break FROM that order — so a row inserted out of place "
               "silently reorders two shorthands of equal length and changes which one a block serializes as");
        DCHECK(row->n >= 2 && row->n <= CSS_SHORTHAND_MAX_LONGHANDS,
               "a shorthand row names fewer than two longhands or more than CSS_SHORTHAND_MAX_LONGHANDS. The "
               "upper bound is what every caller sizes its value array to; raise the constant with the row");
        DCHECK(row->probe != NULL,
               "a shorthand row carries no fixture value, so nothing exercises its expansion. Every row in this "
               "table is one css_shorthand_component takes apart — a row that is not is a shorthand whose "
               "declarations set no longhand at all, which reads as the property's INITIAL value everywhere");
        for (j = 0; j < row->n; j++) {
            DCHECK(strcmp(row->longhands[j], row->name) != 0,
                   "a shorthand names ITSELF among its longhands");
            DCHECK(!css_shorthand_is_shorthand(row->longhands[j]),
                   "a name is recorded BOTH as a shorthand and as a longhand of another one. §6.6's loop asks "
                   "each declaration which shorthands map to it and would then ask the same name for its own "
                   "longhands — CSS Logical's own definition of a property group excludes shorthands for the "
                   "same reason");
            for (k = 0; k < j; k++)
                DCHECK(strcmp(row->longhands[k], row->longhands[j]) != 0,
                       "a shorthand names one longhand twice, so its value list has two entries for one "
                       "declaration and the positional serializations read the wrong slot");
            back_n = css_shorthand_shorthands_of(row->longhands[j], sh, CSS_SHORTHAND_MAX_OF);
            DCHECK(back_n >= 1,
                   "a longhand named by a shorthand row does not find that row from the other direction");
        }
        for (j = 0; j < row->n; j++) {
            values[j] = css_shorthand_component(row->name, row->probe, row->longhands[j]);
            DCHECK(values[j] != NULL,
                   "a shorthand this table says it EXPANDS gave one of its own longhands no value. The reverse "
                   "reading gained a longhand that the forward one has no branch for, and nothing else would "
                   "have said so: the cascade would read that longhand as undeclared, which is its INITIAL "
                   "value and a different number, and §6.6 would stop re-forming the shorthand entirely");
        }
        back = css_shorthand_serialize_value(row->name, (const char *const *)values);
        DCHECK(back != NULL && strcmp(back, row->probe) == 0,
               "a shorthand's fixture value did not survive being expanded into its longhands and consolidated "
               "back. §6.7.2's serialization is the exact inverse of the expansion above, so a round trip that "
               "changes the string means one of the two is wrong — and the visible symptom is a `cssText` that "
               "differs from what the page wrote for no reason a reader could see");
        free(back);
        for (j = 0; j < row->n; j++) free(values[j]);
    }
    /* THE THREE PART TABLES ARE ONE INDEX, and css_shorthand_component reads all three by it: the part index
       `border_part_index` answers with names the component grammar (BORDER_PARTS), the initial value the
       triple falls back to (BORDER_PART_INITIAL) and the four-side shorthand of that part
       (BORDER_PART_SHORTHAND). A row reordered in one and not the others would expand `border-color` through
       the `<line-width>` grammar and drop every colour. */
    for (i = 0; i < CSS_SH_N(BORDER_PARTS); i++) {
        const CssShorthandRow *row = css_sh_row(BORDER_PART_SHORTHAND[i]);
        unsigned s;

        DCHECK(CSS_SH_N(BORDER_PART_INITIAL) == CSS_SH_N(BORDER_PARTS) &&
                   CSS_SH_N(BORDER_PART_SHORTHAND) == CSS_SH_N(BORDER_PARTS),
               "the three border-part tables have different lengths, so one of them is indexed past its end by "
               "a part index the other two answer for");
        DCHECK(row != NULL && row->n == 4,
               "a border part's four-side shorthand is not a recorded four-longhand row");
        for (s = 0; s < 4; s++) {
            char name[40];

            snprintf(name, sizeof name, "border-%s-%s", SIDES[s], BORDER_PARTS[i]);
            DCHECK(strcmp(row->longhands[s], name) == 0,
                   "a border part's four-side shorthand does not name that part's four side longhands in side "
                   "order — the rotation writes each component into the slot SIDE_OF names, so a list in another "
                   "order assigns `border-color: red green` to the wrong sides");
        }
    }
#endif
}

bool css_shorthand_complete_for(const char *longhand)
{
    /* THE LONGHANDS WHOSE SHORTHAND SET IS RECORDED IN FULL, and what makes each complete:
         overflow-x, overflow-y — `overflow` is the only shorthand in CSS that sets them (css-overflow §3.1;
           the logical `overflow-block`/`overflow-inline` are longhands of the same group, not shorthands);
         display, float, position, box-sizing — NO shorthand sets any of the four;
         the four margins — `margin` is the only shorthand that sets them (CSS 2.1 §8.3; the logical
           `margin-block`/`margin-inline` set the logical longhands, which are different properties);
         the four paddings — `padding`, likewise (§8.4);
         width, height, min-width, max-width, min-height, max-height — NO shorthand sets any of them. CSS 2.1
           has none, and css-sizing adds none: `flex` sets `flex-basis`, not `width`, and `aspect-ratio` is a
           longhand of its own;
         the twelve border side longhands — THREE shorthands set each and all three are in the table above:
           `border-width` (or `border-style`, or `border-color`), `border-<side>` and `border`, which is
           css-backgrounds-3 §3.3, §3.2, §3.1 and §3.4. `border-image` sets no width, style or color longhand
           (its own `border-image-width` is a different property), and the logical `border-block`/
           `border-inline` group sets the logical longhands, which lexbor's registry does not carry and which
           are different properties for the same reason `margin-block` is.
       THE OTHER HALF OF THE QUESTION — is every one of those shorthands one the expansion above actually takes
       APART — is no longer asked HERE, because it is now a property of the TABLE rather than of a longhand:
       css_shorthand_init runs every row's fixture through the expansion and back and crashes on a row that does
       not answer for one of its own longhands. It used to be a per-call read of an `expands` flag whose only
       FALSE was `border-color`, and the flag is gone with the gap.
       A name absent from this list is not "probably fine": it is a question nobody has answered, and the
       answer decides whether a `margin: 0` two lines up was read or ignored. */
    static const char *const RECORDED[] = {
        "overflow-x", "overflow-y", "display", "float", "position", "box-sizing",
        "margin-top", "margin-right", "margin-bottom", "margin-left",
        "padding-top", "padding-right", "padding-bottom", "padding-left",
        "width", "height", "min-width", "max-width", "min-height", "max-height",
        "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
        "border-top-style", "border-right-style", "border-bottom-style", "border-left-style",
        "border-top-color", "border-right-color", "border-bottom-color", "border-left-color",
    };
    unsigned i;

    DCHECK(longhand != NULL, "the shorthand-completeness question was asked about a NULL property name");
    for (i = 0; i < sizeof(RECORDED) / sizeof(RECORDED[0]); i++) {
        if (strcmp(RECORDED[i], longhand) != 0) continue;
        DCHECK(!css_shorthand_is_shorthand(longhand),
               "a SHORTHAND is listed among the longhands whose shorthand set is recorded — the two are asked "
               "different questions and a name cannot be both");
        return true;
    }
    return false;
}
