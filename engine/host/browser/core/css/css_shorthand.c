/* CSS Cascade §Shorthand Properties — see css_shorthand.h for why the expansion is here and not in lexbor. */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_background_shorthand.h"
#include "core/css/css_color.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_font_shorthand.h"
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

bool css_shorthand_number(const char *w, size_t n, double *out)
{
    char buf[64];
    char *end = NULL;
    size_t i;

    DCHECK(w != NULL && out != NULL,
           "css-values-4 §5.3's `<number>` production was asked about a NULL span, or with nowhere to report "
           "the value — a component value is a span inside the declaration it was split out of, and an absent "
           "pointer is a caller that lost it");
    /* §5.3's OWN SPELLING IS THE FILTER, and it runs BEFORE the conversion rather than after it: `strtod` is a
       C production, so it accepts `0x10`, `inf` and `nan` — three spans §5.3 does not admit and each of which
       would otherwise reach a property's `Value:` line as a number nobody wrote. What §5.3 permits is decimal
       digits, ONE dot, an `e`/`E` exponent and a leading sign, and the whole-span conversion below is what
       refuses the malformed arrangements of those characters (`1e`, `1.2.3`, `+`). */
    if (n == 0 || n >= sizeof buf) return false;
    for (i = 0; i < n; i++) {
        char c = w[i];

        if (!(c >= '0' && c <= '9') && c != '.' && c != 'e' && c != 'E' && c != '+' && c != '-') return false;
    }
    memcpy(buf, w, n);
    buf[n] = '\0';
    *out = strtod(buf, &end);
    return end != NULL && *end == '\0';
}

/* css-overflow §3.1's value grammar for `overflow-x`/`overflow-y`, plus the LEGACY ALIAS the same section
   requires ("User agents must also support the overlay keyword as a legacy value alias of auto"). The alias is
   a SPECIFIED value in its own right and is mapped where the section's other value-to-value rule lives — the
   computed value — so this component keeps it. */
static const char *const OVERFLOW_KEYWORDS[] = {
    "visible", "hidden", "clip", "scroll", "auto", "overlay"
};

/* css-flexbox-1 §5.1 "Flex Flow Direction: the flex-direction property" and §5.2 "Flex Line Wrapping: the
   flex-wrap property", each entire and in its own `Value:` line's order. THE TWO SETS ARE DISJOINT and that is
   what makes §5.3 "Flex Direction and Wrap: the flex-flow shorthand"'s `||` decidable one word at a time: a
   word is a `<'flex-direction'>` or a `<'flex-wrap'>` or the declaration is invalid, and no word can be both,
   so the expansion below needs no backtracking and no ORDER. A `||` whose terms shared a keyword would need
   both — which is why this is asserted by construction here rather than assumed at the call. */
static const char *const FLEX_DIRECTION_KEYWORDS[] = { "row", "row-reverse", "column", "column-reverse" };
static const char *const FLEX_WRAP_KEYWORDS[] = { "nowrap", "wrap", "wrap-reverse" };

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

/* CSS 2.1 §17 Tables' FOUR TABLE PROPERTIES, whose grammars are here for exactly the reason the two border
   lists above are, and the reason is the same sentence about the same registry: lexbor carries NONE of these
   four, so `caption-side: bottom` reaches the cascade as a `__CUSTOM` holding the name and the RAW TOKENS with
   nothing having validated them and nothing having lower-cased them. A `caption-side: BOTTOM` would then fail
   to compare equal to the keyword it is, and a `table-layout: nope` would reach the computed value as a
   keyword no grammar admits — which is the state every one of the four is in until this row exists.
   THE THREE KEYWORD-VALUED ONES SHARE A TABLE AND `border-spacing` DOES NOT, and the split is their own
   `Value:` lines rather than a convenience: CSS 2.1 §17.4.1 Caption position and alignment gives
   `caption-side` `top | bottom`, CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property gives
   `table-layout` `auto | fixed`, and CSS 2.1 §17.6 Borders gives `border-collapse` `collapse | separate` —
   three two-keyword choices with no multiplier — while CSS 2.1 §17.6.1 The separated borders model gives
   `border-spacing` `<length> <length>?`, which is a multiplier over a production and a member of no set.
   `inherit` is on all four `Value:` lines and is NOT in these sets: CSS Cascade §7.3's keywords are a value
   for every property in CSS and are handled once, ahead of every property's own grammar, below. */
static const char *const CAPTION_SIDE_KEYWORDS[] = { "top", "bottom" };
static const char *const TABLE_LAYOUT_KEYWORDS[] = { "auto", "fixed" };
static const char *const BORDER_COLLAPSE_KEYWORDS[] = { "collapse", "separate" };

static const struct { const char *name; const char *const *kw; unsigned n; } TABLE_KEYWORD_LONGHANDS[] = {
    { "caption-side", CAPTION_SIDE_KEYWORDS, CSS_SH_N(CAPTION_SIDE_KEYWORDS) },
    { "table-layout", TABLE_LAYOUT_KEYWORDS, CSS_SH_N(TABLE_LAYOUT_KEYWORDS) },
    { "border-collapse", BORDER_COLLAPSE_KEYWORDS, CSS_SH_N(BORDER_COLLAPSE_KEYWORDS) },
};

/* The row `longhand` names in the table above, or -1. */
static int table_keyword_longhand_index(const char *longhand)
{
    unsigned i;

    for (i = 0; i < CSS_SH_N(TABLE_KEYWORD_LONGHANDS); i++)
        if (strcmp(TABLE_KEYWORD_LONGHANDS[i].name, longhand) == 0) return (int)i;
    return -1;
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

/* ---- css-inline-3 §4.2 "Transverse Box Alignment: the vertical-align property" -----------------------------
 *
 * `vertical-align` IS A SHORTHAND IN THIS ENGINE, AND THAT IS A FACT ABOUT THE VENDORED PARSER RATHER THAN A
 * CHOICE. CSS 2.2 §10.8 "Line height calculations: the 'line-height' and 'vertical-align' properties" defines
 * it as a longhand whose value is `baseline | sub | super | top | text-top | middle | bottom | text-bottom |
 * <percentage> | <length>`; css-inline-3 §4.2 redefines it as `[ first | last ] || <'alignment-baseline'> ||
 * <'baseline-shift'>`, a shorthand over three longhands the module states separately (§4.2.1 "Alignment
 * Baseline Source: the baseline-source longhand", §4.2.2 "Alignment Baseline Type: the alignment-baseline
 * longhand", §4.2.3 "Post-Alignment Shift: the baseline-shift longhand"). Lexbor's property registry carries
 * ALL FOUR names, its `vertical-align` state machine is §4.2's `||` and stores into an `alignment` and a
 * `shift` sub-value, and its serializer writes those two back in the grammar's own order — so §4.2 is the
 * grammar a declaration in this engine has actually been through, and CSS 2.2's ten-value list is a production
 * nothing here parses. Every CSS 2.1 value still reaches a longhand: `baseline`, `middle`, `text-top` and
 * `text-bottom` are §4.2.2's, and `sub`, `super`, `top`, `bottom`, a `<length>` and a `<percentage>` are
 * §4.2.3's.
 *
 * WHICH MEANS THE COMPUTED VALUE IS THE LONGHANDS' AND NOT THIS PROPERTY'S, and the two specs disagree about
 * it in the one way that matters. CSS 2.2 §10.8 gives `vertical-align` "for <percentage> and <length> the
 * absolute length, otherwise as specified" over a `Percentages:` line of "refer to the 'line-height' of the
 * element itself"; §4.2.3 gives `baseline-shift` "the specified keyword or a computed <length-percentage>
 * value" over "refer to the USED VALUE of line-height". A used value is not a value any cascade step holds, so
 * §4.2.3's percentage cannot resolve at computed-value time and CSS 2.2's sentence describes a resolution this
 * engine must not perform — which is exactly why recording the shorthand comes first: the three longhands are
 * where the `Computed value:` lines that CAN be honoured are written.
 *
 * THE THREE TERM SETS ARE DISJOINT, which is what makes §4.2's `||` splittable by the component value alone
 * rather than by position. `baseline` belongs only to §4.2.2; `top`, `center` and `bottom` belong only to
 * §4.2.3 — its LINE-RELATIVE shift values, which §4.2.3's own note flags as the three it is least sure of
 * ("the line-relative shift values don't fit perfectly in the dichotomy between alignment-baseline and
 * baseline-shift"), so a future edition moving them is the change that would break this split and it will
 * break it LOUDLY; and `first`/`last` belong only to the source term. A word in two of them would
 * make the shorthand ambiguous and the split a guess — so the disjointness is asserted rather than assumed,
 * beside the round trip, in css_shorthand_init. */
static const char *const BASELINE_SOURCE_KEYWORDS[] = { "first", "last" };
/* §4.2.2's `Value:` line entire and in its own order. The SVG legacy spellings §4.2.2.1 lists are deliberately
   absent: lexbor's `lxb_css_alignment_baseline_type_t` carries these eight and no others, so a declaration
   naming one of the legacy values never reaches this component as a `vertical-align` at all. */
static const char *const ALIGNMENT_BASELINE_KEYWORDS[] = {
    "baseline", "text-bottom", "alphabetic", "ideographic", "middle", "central", "mathematical", "text-top"
};
/* §4.2.3's keyword arm; its `<length-percentage>` arm is core/css/css_length.h's production, asked below. */
static const char *const BASELINE_SHIFT_KEYWORDS[] = { "sub", "super", "top", "center", "bottom" };
/* Each longhand's own `Initial:` line — §4.2.1's `auto`, §4.2.2's `baseline`, §4.2.3's `0` — which is what an
   OMITTED term of the `||` is set to. §4.2 states the first of them outright ("if first or last is specified,
   it sets baseline-source, WHICH IS OTHERWISE RESET TO AUTO"), and CSS Cascade 5 §3 "Shorthand Properties"
   states the rule the other two follow: "each “missing” sub-property is assigned its initial value".
   THE SHIFT'S ZERO CARRIES ITS UNIT, AND THAT IS THE SAME LENGTH §4.2.3 WRITES WITHOUT ONE. css-values-4 §6
   "Distance Units: the <length> type": "for zero lengths the unit identifier is optional (i.e. can be
   syntactically represented as the <number> 0)" — so `0` and `0px` are two spellings of one value, and this
   list must be written in the spelling its READERS produce. It has two: the expansion fills an omitted term
   from it, and §6.6.1's shorthand step compares it against each longhand's RESOLVED value — which for a
   length is always core/css/css_length.c's serialization, and that always writes the unit. A bare `0` here
   would match neither, so `vertical-align` would report an all-initial element as `0px` rather than as its
   own `Initial:` line, which is the one case whole_initial exists for. */
static const char *const VERTICAL_ALIGN_INITIAL[] = { "auto", "baseline", "0px" };

static const char *const LH_VERTICAL_ALIGN[] = { "baseline-source", "alignment-baseline", "baseline-shift" };

/* ---- css-text-4 §7.1 "Text Alignment: the text-align shorthand" ------------------------------------------
   ITS TWO LONGHANDS, in the order §7.1's own sentence names them: "this shorthand property sets the
   text-align-all and text-align-last properties". §7.1's `Computed value:` line is "see individual
   properties", so the SHORTHAND has no computed value of its own and a consumer that wants the alignment of a
   line box asks §7.3's `text-align-all` (or §7.4's `text-align-last` for the last line) — which is exactly why
   this row has to exist before either of those can be modelled: without it a `text-align: center` sets NEITHER
   longhand, both read their initial values, and there is a real number to show for a declaration nothing
   looked at. */
static const char *const LH_TEXT_ALIGN[] = { "text-align-all", "text-align-last" };

/* §7.3 "Default Text Alignment: the text-align-all property"'s `Value:` line —
   `start | end | left | right | center | <string> | justify | match-parent` — LESS the `<string>`, and that
   omission is lexbor's grammar rather than a choice made here. §7.1 says of the string "the string must be a
   single character; otherwise the declaration is invalid and must be ignored", and §7.2 "Character-based
   Alignment in a Table Column" is the whole of what it does — a table-cell alignment character. Lexbor's
   property registry admits no `<string>` arm for `text-align`, `text-align-all` or `text-align-last` (its
   value enums are the seven keywords below, plus `justify-all` for the shorthand and `auto` for the last-line
   longhand), so such a declaration is dropped by the parser and never reaches this expansion at all.
   THE SET IS THE `text-align-all` GRAMMAR AND NOT THE SHORTHAND'S: §7.1 adds `justify-all`, which is a value
   of the shorthand alone and is handled where the assignment is. */
static const char *const TEXT_ALIGN_KEYWORDS[] = {
    "start", "end", "left", "right", "center", "justify", "match-parent"
};

static int text_align_longhand_index(const char *longhand)
{
    unsigned i;

    for (i = 0; i < CSS_SH_N(LH_TEXT_ALIGN); i++)
        if (strcmp(LH_TEXT_ALIGN[i], longhand) == 0) return (int)i;
    return -1;
}

/* §7.1's ASSIGNMENT, which is one sentence plus two named exceptions and nothing else:
     "Values other than justify-all or match-parent are assigned to text-align-all and RESET text-align-last to
      auto";
     `justify-all` "sets both text-align-all and text-align-last to justify";
     `match-parent`, "when specified on the text-align shorthand, sets both text-align-all and text-align-last
      to match-parent".
   THE RESET IS THE PART THAT MAKES THIS A SHORTHAND RATHER THAN AN ALIAS, and it is why the second longhand's
   answer is `auto` rather than NULL: a `text-align: center` after a `text-align-last: right` must undo the
   second declaration, and a component that answered nothing for it would leave it standing. */
static char *text_align_component(const char *value, int term)
{
    const char *w[1], *kw;
    size_t wl[1];

    if (css_words(value, w, wl, 1) != 1) return NULL;
    if (css_word_is(w[0], wl[0], "justify-all")) return css_sh_strdup("justify");
    kw = css_sh_keyword(TEXT_ALIGN_KEYWORDS, CSS_SH_N(TEXT_ALIGN_KEYWORDS), w[0], wl[0]);
    if (kw == NULL) return NULL;
    if (strcmp(kw, "match-parent") == 0) return css_sh_strdup(kw);
    return css_sh_strdup(term == 0 ? kw : "auto");
}

/* WHICH OF §4.2's THREE TERMS the component value `w` is — 0 the source, 1 the alignment baseline, 2 the
   shift — or -1 for a value outside the shorthand's grammar, which drops the declaration. `*canon` is set to
   the keyword's CANONICAL (lower-case) spelling, or to NULL for the `<length-percentage>` arm, which has no
   canonical spelling of this component's to give: core/css/css_length.h owns the unit's case and every other
   question about it, so that arm is copied verbatim exactly as a `margin`'s component is. */
static int vertical_align_term_of(const char *w, size_t n, const char **canon)
{
    char *probe;
    bool len;

    *canon = css_sh_keyword(BASELINE_SOURCE_KEYWORDS, CSS_SH_N(BASELINE_SOURCE_KEYWORDS), w, n);
    if (*canon) return 0;
    *canon = css_sh_keyword(ALIGNMENT_BASELINE_KEYWORDS, CSS_SH_N(ALIGNMENT_BASELINE_KEYWORDS), w, n);
    if (*canon) return 1;
    *canon = css_sh_keyword(BASELINE_SHIFT_KEYWORDS, CSS_SH_N(BASELINE_SHIFT_KEYWORDS), w, n);
    if (*canon) return 2;
    probe = css_sh_dupn(w, n);
    /* §4.2.3's `<length-percentage>` carries NO range restriction — "raise (positive value) or lower (negative
       value)" — so a leading `-` is a value and not the invalid declaration it is for a `<line-width>`. */
    len = css_length_is_length_percentage(probe);
    free(probe);
    return len ? 2 : -1;
}

/* §4.2's expansion: the component values in ANY ORDER, each term at most once, omitted terms initial — the same
   `||` shape §3.4's border triple has, over a different partition of the value space. */
static char *vertical_align_component(const char *value, int part)
{
    const char *w[3], *found[3] = { NULL, NULL, NULL }, *canon[3] = { NULL, NULL, NULL };
    size_t wl[3], flen[3] = { 0, 0, 0 };
    int n, i;

    n = css_words(value, w, wl, 3);
    if (n < 1) return NULL;
    for (i = 0; i < n; i++) {
        const char *c;
        int term = vertical_align_term_of(w[i], wl[i], &c);

        if (term < 0) return NULL;              /* outside §4.2's grammar: an invalid declaration */
        if (found[term] != NULL) return NULL;   /* `||` admits each term at most once */
        found[term] = w[i];
        flen[term] = wl[i];
        canon[term] = c;
    }
    if (found[part] == NULL) return css_sh_strdup(VERTICAL_ALIGN_INITIAL[part]);
    return canon[part] ? css_sh_strdup(canon[part]) : css_sh_dupn(found[part], flen[part]);
}

/* Which of §4.2's three longhands `longhand` is, or -1 for a name it does not set. The list is read rather than
   restated so the index the expansion writes and the index the table's canonical order names are ONE. */
static int vertical_align_longhand_index(const char *longhand)
{
    unsigned i;

    for (i = 0; i < CSS_SH_N(LH_VERTICAL_ALIGN); i++)
        if (strcmp(LH_VERTICAL_ALIGN[i], longhand) == 0) return (int)i;
    return -1;
}

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
       value, and which of `red` / `#f00` / `rgb(255 0 0)` was written is what §6.7.2 "Serializing CSS Values"
       serializes back. */
    kw = (part == CSS_BORDER_PART_WIDTH)
             ? css_sh_keyword(LINE_WIDTH_KEYWORDS, CSS_SH_N(LINE_WIDTH_KEYWORDS), w[comp], wl[comp])
             : (part == CSS_BORDER_PART_STYLE)
                   ? css_sh_keyword(LINE_STYLE_KEYWORDS, CSS_SH_N(LINE_STYLE_KEYWORDS), w[comp], wl[comp])
                   : NULL;
    return kw ? css_sh_strdup(kw) : css_sh_dupn(w[comp], wl[comp]);
}

/* css-flexbox-1 §5.3 "Flex Direction and Wrap: the flex-flow shorthand"' EXPANSION — `Value: <'flex-direction'>
   || <'flex-wrap'>`, so at most two words, each of which must be a keyword of one of the two disjoint sets
   above, and each longhand not named takes its own `Initial:` line (§5.1's `row`, §5.2's `nowrap`). A repeated
   term is a value the `||` does not admit — css-values-4 §2.2 "Component Value Combinators" separates OPTIONS
   and not occurrences ("A double bar (||) separates two or more options: one or more of them must occur, in
   any order"), and neither §5.3 nor §2.3 "Component Value Multipliers" gives either term a multiplier, so a
   term may appear at most once.
   IT VALIDATES AS WELL AS SPLITS, for the reason `border-style`'s does: §5.3's own `Value:` line is the only
   thing standing between a `flex-flow: nope` and a computed value carrying a keyword no grammar admits, and
   CSS Syntax drops an invalid declaration whole — which for a shorthand means it sets NEITHER longhand. */
static char *flex_flow_component(const char *value, const char *longhand)
{
    const char *w[3], *dir = NULL, *wrap = NULL;
    size_t wl[3];
    int n = css_words(value, w, wl, 3), i;

    if (n < 1 || n > 2) return NULL;
    for (i = 0; i < n; i++) {
        const char *d = css_sh_keyword(FLEX_DIRECTION_KEYWORDS, CSS_SH_N(FLEX_DIRECTION_KEYWORDS), w[i], wl[i]);
        const char *p = css_sh_keyword(FLEX_WRAP_KEYWORDS, CSS_SH_N(FLEX_WRAP_KEYWORDS), w[i], wl[i]);

        if (d != NULL) { if (dir != NULL) return NULL; dir = d; continue; }
        if (p != NULL) { if (wrap != NULL) return NULL; wrap = p; continue; }
        return NULL;
    }
    if (strcmp(longhand, "flex-direction") == 0) return css_sh_strdup(dir != NULL ? dir : "row");
    if (strcmp(longhand, "flex-wrap") == 0) return css_sh_strdup(wrap != NULL ? wrap : "nowrap");
    return NULL;
}

/* css-flexbox-1 §7.1 "The flex Shorthand"'s THREE LONGHANDS in its `Value:` line's own order, and beside each
   the value §7.1 gives it WHEN THE SHORTHAND OMITS IT. THAT SECOND LIST IS NOT THE `Initial:` LINES and is the
   whole reason `flex` is a kind of its own rather than a CSS_SH_ALL_OF row — §7.1's own note says so outright:
   "The initial values of the flex longhands are equivalent to flex: 0 1 auto. This differs from their defaults
   when omitted in the flex shorthand (effectively 1 1 0px) so that the flex shorthand can better accommodate
   the most common cases." The three sentences it is read off are §7.1's component definitions: "When omitted,
   it is set to 1" for the `<'flex-grow'>` component, the same sentence for `<'flex-shrink'>`, and "When
   omitted from the flex shorthand, its specified value is 0" for `<'flex-basis'>`.
   SO THE ROW CARRIES NO `initial` LIST. That field is documented as each longhand's own `Initial:` line and is
   read by CSS_SH_ALL_OF's serialization to decide which terms can be dropped; filling it with THESE three
   would be one field answering two questions, and the omission rule would then drop a `flex-grow: 0` — the
   longhand's initial — and re-read it as the 1 §7.1 substitutes. */
static const char *const LH_FLEX[] = { "flex-grow", "flex-shrink", "flex-basis" };
static const char *const FLEX_OMITTED[] = { "1", "1", "0" };

/* §7.2.3 "The flex-basis property"' `Value:` line, `content | <'width'>`, with §7.2.3's own keyword first and
   `<'width'>` spelled out after it. `<'width'>` is css-sizing-3 §3.1.1 "Preferred Size Properties: the width
   and height properties"' `auto | <box-size>`, and §3.2 "Sizing Values: the <length-percentage [0,∞]>, auto |
   none, stretch, min-content, max-content, and fit-content values" is where `<box-size>` is defined: "Values
   other than auto and none are grouped under the <box-size> production:
   <box-size> = <length-percentage> | stretch | min-content | max-content | fit-content". THE LEVEL-3 KEYWORDS
   ARE `flex-basis`'s BY REFERENCE AND NOT BY ANALOGY, which §3.2 states in a note of its own — "The flex-basis
   property hereby also gains these new keywords, as its values are defined by reference to <'width'>" — so a
   set that stopped at CSS 2.1's `auto` would drop three keywords a page may legitimately write.
   `none` IS NOT HERE. §3.1.1's `Value:` line is `auto | <box-size>` and the `none` in §3.2's TITLE belongs to
   the MAXIMUM size properties (§3.1.3), whose line is `none | <box-size>` — so reading the title as a value
   set would give `flex-basis` a keyword `width` itself does not have. */
static const char *const FLEX_BASIS_KEYWORDS[] = {
    "content", "auto", "stretch", "min-content", "max-content", "fit-content",
};

/* Which of css-flexbox-1 §7.1 "The flex Shorthand"'s three longhands `longhand` is, or -1. Read off the list above so the index the expansion
   writes and the index the table's canonical order names are ONE. */
static int flex_longhand_index(const char *longhand)
{
    unsigned i;

    for (i = 0; i < CSS_SH_N(LH_FLEX); i++)
        if (strcmp(LH_FLEX[i], longhand) == 0) return (int)i;
    return -1;
}

/* One of css-flexbox-1 §7.1 "The flex Shorthand"'s three specified values, as a span into the declaration or at one of the literals above. */
typedef struct { const char *p; size_t n; } CssShSpan;

static void flex_span(CssShSpan *o, const char *p, size_t n) { o->p = p; o->n = n; }

/* Is this component value a `<'flex-grow'>` / `<'flex-shrink'>` — §7.2.1 "The flex-grow property" and §7.2.2
   "The flex-shrink property" both give `<number [0,∞]>`, and both say "Negative values are not allowed." */
static bool flex_is_factor(const char *w, size_t n)
{
    double v;

    return css_shorthand_number(w, n, &v) && v >= 0.0;
}

/* Take this component value as the `<'flex-basis'>`, canonicalizing a keyword the way `overflow`'s expansion
   does. FALSE for a value outside §7.2.3's grammar, which drops the whole declaration.
   `zero_ok` IS §7.1's OWN DISAMBIGUATION RULE AND NOT A TOLERANCE: "A unitless zero that is not already
   preceded by two flex factors must be interpreted as a flex factor. To avoid misinterpretation or invalid
   declarations, authors must specify a zero <'flex-basis'> component with a unit or precede it by two flex
   factors." So a bare number reaching this entry is a `<'flex-basis'>` in exactly one arrangement — third,
   after both factors — and is a zero even there. It is asked HERE rather than at the arrangement because the
   arrangement is what knows how many factors precede it, and the value is what knows whether it is a zero.
   IT MUST BE ASKED BEFORE THE `<length-percentage>` TEST, because css_length_is_length_percentage answers TRUE
   for a unitless NON-ZERO as well — css-values-4 §6 "Distance Units: the <length> type" permits the unit to be
   omitted only for zero, and that entry leaves the refusal to the parse that later asserts it. Asking it in
   the other order would make `flex: 1 1 5` a declaration whose `flex-basis` computed value crashes the length
   parser rather than one the cascade drops. */
static bool flex_take_basis(CssShSpan *o, const char *w, size_t n, bool zero_ok)
{
    const char *kw = css_sh_keyword(FLEX_BASIS_KEYWORDS, CSS_SH_N(FLEX_BASIS_KEYWORDS), w, n);
    char probe[64];
    double v;

    if (kw != NULL) { flex_span(o, kw, strlen(kw)); return true; }
    if (css_shorthand_number(w, n, &v)) {
        if (!zero_ok || v != 0.0) return false;
        flex_span(o, w, n);
        return true;
    }
    /* css-sizing-3 §3.2's `<length-percentage [0,∞]>`: "Negative values are invalid." The test is the LITERAL
       sign and not the value, exactly as `border-<part>`'s is one grammar up: a `calc(1rem - 2rem)` is a VALID
       declaration whose used value is clamped, so refusing it here would drop a declaration CSS admits. */
    if (n == 0 || w[0] == '-') return false;
    /* AND WHAT REMAINS MUST BEGIN LIKE A DIMENSION OR BE A FUNCTION, which css-values-4 §5.4 "Numbers with
       Units: dimension values" states outright: "When written literally, a dimension is a number immediately
       followed by a unit identifier, which is an identifier." So a literal `<length-percentage>` starts with
       §5.3's `<number>`, and the only other spelling the production admits is a math function — a name and a
       `(`, which the entry below type-checks through core/css/css_math.h.
       IT IS ASKED HERE BECAUSE `css_length_is_length_percentage` IS DELIBERATELY WIDER THAN §6, and its own
       comment says so: it hands the span to `strtod`, a C production, and leaves the refusal of what CSS does
       not admit to the parse that later ASSERTS it. That is the right split for a value lexbor has already
       typed and the wrong one where this grammar is the only check there is — `inf` and `nan` are spans
       `strtod` reads as numbers and CSS reads as plain identifiers, so admitting one would turn a page's own
       invalid declaration into a `flex-basis` whose computed value ABORTS the length parser, which is this
       engine's assertion mechanism fired by a stranger's bytes rather than by its own logic. */
    if (!(w[0] >= '0' && w[0] <= '9') && w[0] != '.' && w[0] != '+' && memchr(w, '(', n) == NULL) return false;
    if (n >= sizeof probe) return false;
    memcpy(probe, w, n);
    probe[n] = '\0';
    if (!css_length_is_length_percentage(probe)) return false;
    flex_span(o, w, n);
    return true;
}

/* css-flexbox-1 §7.1 "The flex Shorthand"'s EXPANSION, answered once for all three longhands:
   `none | [ <'flex-grow'> <'flex-shrink'>? || <'flex-basis'> ]`, with each omitted component at its
   FLEX_OMITTED default. FALSE for a value outside the grammar, which is an invalid declaration and sets no
   longhand at all.
   THE `||` HAS AN ORDERED PAIR INSIDE IT, which is what makes this a different algorithm from `flex-flow`'s
   one-word-at-a-time assignment and from `border-<side>`'s any-order triple. css-values-4 §2.2 "Component
   Value Combinators" gives the combinator — "A double bar (||) separates two or more options: one or more of
   them must occur, in any order" — so its two terms give four arrangements: the factors alone, the basis
   alone, and each order of the two. The factors WITHIN their term are positional: grow first, shrink second,
   never reversed.
   EACH ARRANGEMENT IS TRIED WITH THE FACTORS ASSIGNED GREEDILY AND THAT IS §7.1's RULE RATHER THAN A
   PREFERENCE. A bare `<number>` is not a `<'flex-basis'>` at all (css-values-4 §6 "Distance Units: the
   <length> type" admits a unitless length only for zero), so the only span the two terms can both claim is a
   literal zero — and §7.1's own sentence
   awards it to the factor unless two factors already precede it. Trying the factor arrangement first IS that
   sentence, which is why an arrangement that fails afterwards is not retried the other way round: a span the
   factor arm claimed is one the basis arm is forbidden to take. */
static bool flex_triple(const char *value, CssShSpan out[3])
{
    const char *w[3];
    size_t wl[3];
    int n = css_words(value, w, wl, 3);
    unsigned i;

    for (i = 0; i < CSS_SH_N(FLEX_OMITTED); i++)
        flex_span(&out[i], FLEX_OMITTED[i], strlen(FLEX_OMITTED[i]));
    if (n < 1) return false;   /* -1 is a value carrying more components than the grammar admits */
    /* css-flexbox-1 §7.1 "The flex Shorthand"'s OTHER TOP-LEVEL ARM, ahead of the bracketed one it is an
       alternative to: "The keyword none expands to 0 0 auto." It is written as the three values rather than
       kept as a keyword because css-flexbox-1 §7.1 "The flex Shorthand"'s `Computed value:` line is "see
       individual properties", so what the cascade carries is each longhand's own value and `none` is not one
       of them. */
    if (n == 1 && css_word_is(w[0], wl[0], "none")) {
        flex_span(&out[0], "0", 1);
        flex_span(&out[1], "0", 1);
        flex_span(&out[2], "auto", 4);
        return true;
    }
    switch (n) {
    case 1:
        if (flex_is_factor(w[0], wl[0])) { flex_span(&out[0], w[0], wl[0]); return true; }
        return flex_take_basis(&out[2], w[0], wl[0], false);
    case 2:
        if (flex_is_factor(w[0], wl[0]) && flex_is_factor(w[1], wl[1])) {
            flex_span(&out[0], w[0], wl[0]);
            flex_span(&out[1], w[1], wl[1]);
            return true;
        }
        if (flex_is_factor(w[0], wl[0])) {
            flex_span(&out[0], w[0], wl[0]);
            return flex_take_basis(&out[2], w[1], wl[1], false);
        }
        if (flex_is_factor(w[1], wl[1])) {
            flex_span(&out[0], w[1], wl[1]);
            return flex_take_basis(&out[2], w[0], wl[0], false);
        }
        return false;
    default:
        DCHECK(n == 3, "css_words answered a component count outside the 1..3 its own `max` argument permits");
        if (flex_is_factor(w[0], wl[0]) && flex_is_factor(w[1], wl[1])) {
            flex_span(&out[0], w[0], wl[0]);
            flex_span(&out[1], w[1], wl[1]);
            /* THE ONE PLACE A UNITLESS ZERO IS A BASIS — two flex factors precede it, which is §7.1's own
               condition and the arrangement its "or precede it by two flex factors" instructs authors to use. */
            return flex_take_basis(&out[2], w[2], wl[2], true);
        }
        if (flex_is_factor(w[1], wl[1]) && flex_is_factor(w[2], wl[2])) {
            flex_span(&out[0], w[1], wl[1]);
            flex_span(&out[1], w[2], wl[2]);
            return flex_take_basis(&out[2], w[0], wl[0], false);
        }
        return false;
    }
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

    /* ---- css-fonts-4 §2.7's `font` ------------------------------------------------------------------------
       The whole grammar — including CSS Cascade §7.3's keywords, which for this shorthand reach the RESET-ONLY
       group as well and so cannot be answered here — belongs to the component that owns §2.7. */
    if (strcmp(shorthand, "font") == 0) return css_font_shorthand_component(value, longhand);

    /* ---- css-backgrounds-3 §2.10's `background` ------------------------------------------------------------
       The one shorthand whose value is a comma-separated LIST OF LAYERS, so one declaration answers each
       longhand with a list of its own; core/css/css_background_shorthand.h owns §2.10 and this table owns the
       row, exactly as CSS_SH_FONT is the seam for §2.7. */
    if (strcmp(shorthand, "background") == 0) return css_background_shorthand_component(value, longhand);

    /* ---- css-inline-3 §4.2's `vertical-align` -------------------------------------------------------------- */
    if (strcmp(shorthand, "vertical-align") == 0) {
        int term = vertical_align_longhand_index(longhand);

        if (term < 0) return NULL;
        /* CSS Cascade §Shorthand Properties: "if a shorthand is specified as one of the CSS-wide keywords, it
           sets all of its sub-properties to that keyword" — the ENTIRE value, so it precedes §4.2's own
           grammar, in which `inherit` is no term at all. §7's DEFAULTING step is what resolves it. */
        if (css_wide_keyword(value)) return css_sh_strdup(value);
        return vertical_align_component(value, term);
    }

    /* ---- css-text-4 §7.1's `text-align` ------------------------------------------------------------------- */
    if (strcmp(shorthand, "text-align") == 0) {
        int term = text_align_longhand_index(longhand);

        if (term < 0) return NULL;
        /* CSS Cascade §Shorthand Properties: "if a shorthand is specified as one of the CSS-wide keywords, it
           sets all of its sub-properties to that keyword" — the ENTIRE value, ahead of §7.1's own grammar, in
           which `inherit` is no value at all. §7's DEFAULTING step is what resolves it, and for THESE two that
           matters more than for most: both are `Inherited: yes`, so §7.3's and §7.4's own inheritance and an
           explicit `inherit` reach the same answer through different steps and must not be conflated here. */
        if (css_wide_keyword(value)) return css_sh_strdup(value);
        return text_align_component(value, term);
    }

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
        bool is_margin = strcmp(shorthand, "margin") == 0;
        int comp, k;

        side = box_side_index(shorthand, longhand);
        if (side < 0) return NULL;   /* this shorthand does not name that longhand */
        /* CSS Cascade §Shorthand Properties: a CSS-wide keyword "sets all of its sub-properties to that
           keyword", so it is the ENTIRE value and precedes the shorthand's own grammar — the same order the
           border arms above state, and required here for the same reason now that the grammar below refuses
           what is not a `<margin-width>`: `margin: inherit` is not one, and §7's DEFAULTING step is what
           resolves it one property along. */
        if (css_wide_keyword(value)) return css_sh_strdup(value);
        n = css_words(value, w, wl, 4);
        /* THE COUNT IS NO LONGER LEXBOR'S TO GUARANTEE, so it is a refusal and not an assertion. This used to
           DCHECK that the count was within CSS 2.1 §8.3 and §8.4's `{1,4}` on the grounds that lexbor parses
           the shorthand into a four-slot typed value and can serialize nothing else — which was true while
           every value reaching here had been through lexbor's own grammar. A declaration lexbor REFUSED now
           reaches here too (a `margin: calc(1rem) 2px`, whose only defect in its eyes is a math function it has
           never heard of), and its value is the RAW SOURCE SPAN with no slot count behind it. A fifth component
           there is a page's own invalid declaration, which CSS Syntax 3 §5.5.6 "Consume a declaration" drops. */
        if (n < 1) return NULL;
        /* AND SO IS THE GRAMMAR. Nothing validated a refused declaration's components, so each is put through
           the longhand's own production before any of them sets anything — css-box-4 §3.1 makes a margin
           `<length-percentage> | auto` and §4.1 makes a padding `<length-percentage [0,∞]>`, which is why
           `auto` is admitted for one and not the other. It runs for a lexbor-typed value too, where it is a
           re-check that cannot fail, rather than being made conditional on which door the value came through:
           a validation that only some callers get is the shape whose gaps are invisible.
           §10.12 "Range Checking" is deliberately not applied — a negative `calc()` is a VALID declaration that
           computes to the clamped value, so refusing one here would drop `padding: calc(1rem - 2rem)`. */
        for (k = 0; k < n; k++) {
            char *probe = css_sh_dupn(w[k], wl[k]);
            bool ok = css_length_is_length_percentage(probe) ||
                      (is_margin && strcmp(probe, "auto") == 0);

            free(probe);
            if (!ok) return NULL;
        }
        comp = SIDE_OF[n][side];
        return css_sh_dupn(w[comp], wl[comp]);
    }
    /* ---- css-flexbox-1 §5.3 "Flex Direction and Wrap: the flex-flow shorthand"'s `flex-flow` -------------- */
    /* csscascade-5 §3 "Shorthand Properties": "if a shorthand is specified as one of the CSS-wide keywords, it
       sets all of its sub-properties to that keyword" — the ENTIRE value, so it precedes the shorthand's own
       grammar, in which `inherit` is no term at all. CSS Cascade §7's DEFAULTING step is what resolves it. */
    if (strcmp(shorthand, "flex-flow") == 0) {
        if (strcmp(longhand, "flex-direction") != 0 && strcmp(longhand, "flex-wrap") != 0) return NULL;
        if (css_wide_keyword(value)) return css_sh_strdup(value);
        return flex_flow_component(value, longhand);
    }
    /* ---- css-flexbox-1 §7.1 "The flex Shorthand"'s `flex` --------------------------------------------------
       csscascade-5 §3 "Shorthand Properties"' CSS-wide keyword arm again, and it matters more here than for
       most: §7.1's own grammar has a `none` in it, so a reader could take `initial` for one more keyword of the
       same kind. It is not — a CSS-wide keyword is the ENTIRE value for every property in CSS and sets all
       three longhands to itself, where `none` is one production of this property's own `Value:` line and
       expands to three DIFFERENT values. */
    if (strcmp(shorthand, "flex") == 0) {
        CssShSpan t[3];
        int term = flex_longhand_index(longhand);

        if (term < 0) return NULL;
        if (css_wide_keyword(value)) return css_sh_strdup(value);
        if (!flex_triple(value, t)) return NULL;
        return css_sh_dupn(t[term].p, t[term].n);
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

/* CSS 2.1 §17's four table longhands' OWN value grammars, over the table above. The answer is the SPECIFIED
   value — canonicalized where the grammar is a keyword, verbatim where it is a length, which is the same split
   the border longhands take one function down — or NULL for a value the grammar does not admit, which is a
   declaration CSS Syntax drops. Only ever reached for a name `css_shorthand_validates_longhand` answers TRUE
   for, and CSS Cascade §7.3's keywords have already been taken by the caller. */
static char *table_longhand_value(const char *longhand, const char *value)
{
    const char *w[2], *kw;
    size_t wl[2];
    int idx = table_keyword_longhand_index(longhand), n, i;
    char *out;
    size_t len;

    if (idx >= 0) {
        /* Each of the three `Value:` lines is a two-keyword choice carrying NO multiplier, so a second
           component value is an invalid declaration and css_words reports it by refusing to write past `max`. */
        if (css_words(value, w, wl, 1) != 1) return NULL;
        kw = css_sh_keyword(TABLE_KEYWORD_LONGHANDS[idx].kw, TABLE_KEYWORD_LONGHANDS[idx].n, w[0], wl[0]);
        return kw ? css_sh_strdup(kw) : NULL;
    }
    DCHECK(strcmp(longhand, "border-spacing") == 0,
           "a longhand outside CSS 2.1 §17 Tables' four reached the table-property grammar. This function and "
           "`css_shorthand_validates_longhand`'s §17 arm are one list and have come apart, and the failure is "
           "silent in the direction that matters: a property with a grammar of its own would have that grammar "
           "replaced by `<length> <length>?`");
    /* CSS 2.1 §17.6.1 The separated borders model's `<length> <length>?`, and its own two sentences about what
       the components mean: "If one length is specified, it gives both the horizontal and vertical spacing. If
       two are specified, the first gives the horizontal spacing and the second the vertical spacing." Which of
       the two a reader gets is the COMPUTED value's question and not this one — this is the specified value,
       so a one-length declaration stays one length and core/css/css_computed_value.h's entry is what doubles
       it. A THIRD component is an invalid declaration, which css_words reports by refusing to write past 2. */
    n = css_words(value, w, wl, 2);
    if (n < 1) return NULL;
    for (i = 0; i < n; i++) {
        char *probe;
        bool ok;

        /* §17.6.1: "Lengths may not be negative." css_length_is_length answers the PRODUCTION and not the
           range, so the sign is tested here exactly as css-backgrounds-3 §3.3's is for a border width. */
        if (wl[i] > 0 && w[i][0] == '-') return NULL;
        probe = css_sh_dupn(w[i], wl[i]);
        ok = css_length_is_length(probe);
        free(probe);
        if (!ok) return NULL;
    }
    /* Rebuilt from the components rather than copied from `value`, so the specified value carries ONE space
       between two lengths however the author spaced the declaration — the same canonicalization every keyword
       grammar here performs, applied to the one production that has no keyword to canonicalize to. */
    len = wl[0] + ((n > 1) ? wl[1] + 1 : 0);
    out = malloc(len + 1);
    CHECK(out != NULL, "cssom: OOM copying a `border-spacing` — a dropped one would read as undeclared, which "
                       "is CSS 2.1 §17.6.1's initial `0` and a different distance");
    memcpy(out, w[0], wl[0]);
    if (n > 1) {
        out[wl[0]] = ' ';
        memcpy(out + wl[0] + 1, w[1], wl[1]);
    }
    out[len] = '\0';
    return out;
}

bool css_shorthand_validates_longhand(const char *longhand)
{
    int side, part;

    DCHECK(longhand != NULL, "the longhand-grammar question was asked about a NULL property name");
    /* CSS 2.1 §17 Tables' four, whose grammars are above and whose registry gap is the one the border
       longhands have — see the table for the sentence, which is the same sentence. */
    if (table_keyword_longhand_index(longhand) >= 0 || strcmp(longhand, "border-spacing") == 0) return true;
    /* SEVEN OF css-backgrounds-3 §2.10's EIGHT, for the same reason and with the same split: lexbor's registry
       carries `background-color` and none of the other seven, so §2.10's component owns their grammars and
       lexbor's parser owns the colour's. The question is FORWARDED rather than answered here, because the list
       and the grammar behind it are one statement in one file. */
    if (css_background_shorthand_validates_longhand(longhand)) return true;
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
    if (css_background_shorthand_validates_longhand(longhand))
        return css_background_shorthand_longhand_value(longhand, value);
    DCHECK(css_shorthand_validates_longhand(longhand),
           "a longhand this component does not own the grammar of was routed through it — the predicate and "
           "this function are one list and have come apart, and the failure is silent in the direction that "
           "matters: a property lexbor DOES type would have its own parser's answer replaced by this one");
    /* CSS Cascade §7.3's keywords are a value for EVERY property, so they precede the property's own grammar
       and are handed on for §7's DEFAULTING step to resolve (css_computed_value.h says where that crashes). */
    if (css_wide_keyword(value)) return css_sh_strdup(value);
    /* CSS 2.1 §17 Tables' four, AFTER §7.3's keywords and before the border arm, because `border-spacing`'s
       grammar needs two component slots and the border longhands' needs one. */
    if (table_keyword_longhand_index(longhand) >= 0 || strcmp(longhand, "border-spacing") == 0)
        return table_longhand_value(longhand, value);
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
 * THE FORWARD DIRECTION ABOVE ANSWERS ONE QUESTION AT A TIME — what does this declaration give that longhand
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
 * WHAT IS DELIBERATELY NOT IN THE TABLE AT ALL: every CSS shorthand this engine has no grammar for.
 * css_shorthand_is_shorthand answers FALSE for each, and the block serialization then emits their longhands
 * separately — css-fonts-4 §6.11 "Overall shorthand for font rendering: the font-variant property" is one such,
 * and css_shorthand_complete_for records the seven longhands it leaves incomplete rather than claiming them.
 *
 * AND A ROW'S COST IS ITS GRAMMAR'S SHAPE, WHICH IS WHY THE TWO FLEX SHORTHANDS ARE TWO KINDS. css-flexbox-1
 * §5.3's `flex-flow` is `<'flex-direction'> || <'flex-wrap'>` over two DISJOINT keyword sets, which is one
 * word-to-term assignment with no ordering and no backtracking — CSS_SH_ALL_OF's rule exactly. §7.1 "The flex
 * Shorthand"'s `flex` is `none | [ <'flex-grow'> <'flex-shrink'>? || <'flex-basis'> ]`, which is not: it puts
 * an ORDERED PAIR inside the `||`, and §7.1 gives its omitted components defaults that are NOT their
 * longhands' `Initial:` lines — "When omitted, it is set to 1" for both factors against a `flex-grow` initial
 * of 0, and "When omitted from the flex shorthand, its specified value is 0" for the basis against an initial
 * of `auto`. So neither direction of CSS_SH_ALL_OF is its rule: the expansion cannot fill an omitted term from
 * this table's `initial` list, and the serialization cannot omit a term for holding one. CSS_SH_FLEX is that
 * pair of rules and nothing else; the reason it is a KIND rather than a wider CSS_SH_ALL_OF is that widening
 * one would make the `initial` field mean two things at once, and the field a row states its `Initial:` lines
 * in is the field the omission rule reads.
 *
 * AND TWO ROWS' GRAMMARS ARE NOT HERE. css-fonts-4 §2.7's `font` is a positional SEQUENCE with an unordered
 * optional prefix, an infix `/` and a trailing comma-list, over six component grammars nothing else in this
 * engine has; css-backgrounds-3 §2.10's `background` is a comma-separated LIST OF LAYERS whose final layer has
 * one more term than the others, with an infix inside one term and two terms over one keyword set that ORDER
 * tells apart. Both are a different shape from the four kinds above, which are each arithmetic over ONE
 * component grammar applied to ONE value. core/css/css_font_shorthand.h owns §2.7 and
 * core/css/css_background_shorthand.h owns §2.10; this table owns the rows, and CSS_SH_FONT and
 * CSS_SH_BACKGROUND are the seams. */

/* HOW A SHORTHAND'S VALUE IS PUT BACK TOGETHER — CSSOM §6.7.2's "serialize a CSS value ... with a list", which
   is "serialize a CSS value from a hypothetical declaration of the property shorthand with its value
   representing the combined values of the declarations in list", under §6.7.2's two syntactic rules: reorder
   `||` terms into the property's canonical order, and OMIT any component that can be dropped or shortened
   without changing the meaning. Each kind is one grammar's answer to those two rules, run backwards. */
typedef enum {
    CSS_SH_FOUR_SIDE,   /* CSS 2.1 §8.3's rotation reversed: top/right/bottom/left with the tail dropped */
    CSS_SH_TWO_AXIS,    /* css-overflow §3.1's `{1,2}`: the second omitted when it equals the first */
    CSS_SH_ALL_OF,      /* an n-term `||`, each term omitted when it holds that longhand's initial. It was
                           spelled TRIPLE while every row of it had three terms; css-flexbox-1 §5.3's
                           `flex-flow` is the same rule over TWO, and the row already carries its own `n`, so
                           the count is read from the row rather than written into the kind's name. */
    CSS_SH_FLEX,        /* css-flexbox-1 §7.1's `flex`: an ordered factor pair inside a `||`, whose omitted
                           components take §7.1's own defaults rather than their longhands' `Initial:` lines */
    CSS_SH_BORDER,      /* §3.4's `border`: one triple common to all four sides, over an untouched border-image */
    CSS_SH_FONT,        /* css-fonts-4 §2.7's `font`, whose grammar is core/css/css_font_shorthand.h's */
    CSS_SH_TEXT_ALIGN,  /* css-text-4 §7.1's one keyword redistributed over two longhands, plus its two pairs */
    CSS_SH_BACKGROUND   /* css-backgrounds-3 §2.10's `background`, whose grammar is a LIST OF LAYERS and lives
                           in core/css/css_background_shorthand.h */
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
    /* CSS_SH_ALL_OF ONLY — each longhand's own `Initial:` line, in the row's canonical order. §6.7.2's rule is
       "If component values can be omitted or replaced with a shorter representation without changing the
       meaning of the value, omit/replace them.", and a
       term holding its initial is exactly one that can be, so this list is the omission's DATA and belongs to
       the row rather than to the algorithm. It is the SAME list the forward expansion fills an omitted term
       from, which is what keeps the two directions one statement. NULL for every other kind, asserted. */
    const char *const *initial;
    /* CSS_SH_ALL_OF ONLY — the SHORTHAND'S OWN `Initial:` line, for the case where every term is omitted and
       the omission would leave the empty string, which matches no production of any of these grammars.
       IT IS NULL WHERE THE PROPERTY DEFINITION STATES NONE, and that is the whole of the difference between
       the two rows that reach the case: css-inline-3 §4.2 gives `vertical-align` an `Initial:` line of
       `baseline`, so an all-initial list has a value it can be written as; css-backgrounds-3 §3.4 gives
       `border-<side>` "see individual properties", so it has none and §6.7.2's "cannot exactly represent" arm
       is the answer instead. css_shorthand_init asserts that a stated one EXPANDS BACK to this row's own
       initials, so the two cannot state different faces of one value. */
    const char *whole_initial;
} CssShorthandRow;

static const char *const LH_MARGIN[]  = { "margin-top", "margin-right", "margin-bottom", "margin-left" };
static const char *const LH_PADDING[] = { "padding-top", "padding-right", "padding-bottom", "padding-left" };
static const char *const LH_OVERFLOW[] = { "overflow-x", "overflow-y" };
/* css-flexbox-1 §5.3 "Flex Direction and Wrap: the flex-flow shorthand", in its `Value:` line's own order
   (`<'flex-direction'> || <'flex-wrap'>`), with each longhand's own `Initial:` line beside it — §5.1's `row`
   and §5.2's `nowrap`. §5.3's own `Initial:` line is "see individual properties", so it states none for the
   all-terms-omitted case and CSSOM §6.7.2's "cannot exactly represent the values" arm is the answer, exactly
   as it is for `border-<side>`. */
static const char *const LH_FLEX_FLOW[] = { "flex-direction", "flex-wrap" };
static const char *const FLEX_FLOW_INITIAL[] = { "row", "nowrap" };
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
    /* css-backgrounds-3 §2.10. The fixture names EVERY term of one `<final-bg-layer>`, each away from its own
       initial and in the grammar's canonical order, so the round trip exercises the whole partition at once —
       an `<image>`, a two-component `<bg-position>`, the `/` infix and its `<bg-size>`, a `<repeat-style>`, an
       `<attachment>`, the ONE-value `<visual-box>` form (§2.10: "if one <visual-box> value is present then it
       sets both background-origin and background-clip to that value") and the colour only the final layer may
       carry. `10em auto` and the one-value box are the two places the two directions could disagree and not be
       noticed: the first is §2.9.1's always-two-values serialization, and the second is the rule that a single
       box is not the same fact as two equal ones. */
    { "background",    CSS_BACKGROUND_SHORTHAND_LONGHANDS, CSS_BACKGROUND_SHORTHAND_N, CSS_SH_BACKGROUND,
      "url(chess.png) 40% 50% / 10em auto round fixed border-box gray", NULL, NULL },
    { "border",        LH_BORDER,        17, CSS_SH_BORDER,    "1px solid red", NULL, NULL },
    { "border-bottom", LH_BORDER_BOTTOM,  3, CSS_SH_ALL_OF,    "1px solid red", BORDER_PART_INITIAL, NULL },
    { "border-color",  LH_BORDER_COLOR,   4, CSS_SH_FOUR_SIDE, "red green", NULL, NULL },
    { "border-left",   LH_BORDER_LEFT,    3, CSS_SH_ALL_OF,    "1px solid red", BORDER_PART_INITIAL, NULL },
    { "border-right",  LH_BORDER_RIGHT,   3, CSS_SH_ALL_OF,    "1px solid red", BORDER_PART_INITIAL, NULL },
    { "border-style",  LH_BORDER_STYLE,   4, CSS_SH_FOUR_SIDE, "solid", NULL, NULL },
    { "border-top",    LH_BORDER_TOP,     3, CSS_SH_ALL_OF,    "1px solid red", BORDER_PART_INITIAL, NULL },
    { "border-width",  LH_BORDER_WIDTH,   4, CSS_SH_FOUR_SIDE, "1px", NULL, NULL },
    /* css-flexbox-1 §7.1. The fixture names ALL THREE components with each away from its own OMITTED default
       — 2 against a default grow of 1, 3 against a default shrink of 1, `10px` against a default basis of 0 —
       which is the condition under which every one of them is written out, so the round trip exercises the
       whole `||` in the one arrangement that shows all of it. The arrangements one fixture cannot reach (the
       basis BEFORE the factors, `none`, and §7.1's unitless-zero rule) are asserted separately by
       css_shorthand_init, because each of them is a DIFFERENT sentence of §7.1 rather than another value of
       the same one. THE FIXTURE IS NOT `0 1 auto`: that is §7.1's `Initial:` line, and it is the one triple
       whose every component is at its LONGHAND's initial rather than at its omitted default, so a round trip
       over it would exercise the omission rule at none of its three terms. */
    { "flex",          LH_FLEX,           3, CSS_SH_FLEX,      "2 3 10px", NULL, NULL },
    /* css-flexbox-1 §5.3. The fixture names BOTH terms of the `||`, each away from its own initial, so the
       round trip exercises the whole partition — the assignment of a word to the term whose keyword set
       contains it, and the omission rule over two initials rather than three. */
    { "flex-flow",     LH_FLEX_FLOW,      2, CSS_SH_ALL_OF,    "column wrap", FLEX_FLOW_INITIAL, NULL },
    /* §2.7's own longhand list, taken from the component that owns the grammar so the ORDER the serialization
       reads positionally and the order the expansion writes are ONE statement in ONE file. The fixture is the
       shortest value §2.7's grammar admits — its `<'font-size'>` and `<'font-family'>#` are both required and
       everything else is optional — so the round trip exercises all nineteen longhands (the seven set, and the
       twelve reset to their initial values) without depending on how any one component canonicalizes. */
    { "font", CSS_FONT_SHORTHAND_LONGHANDS, CSS_FONT_SHORTHAND_N, CSS_SH_FONT, "12px sans-serif", NULL, NULL },
    { "margin",        LH_MARGIN,         4, CSS_SH_FOUR_SIDE, "1px 2px 3px 4px", NULL, NULL },
    { "overflow",      LH_OVERFLOW,       2, CSS_SH_TWO_AXIS,  "hidden auto", NULL, NULL },
    { "padding",       LH_PADDING,        4, CSS_SH_FOUR_SIDE, "1px 2px", NULL, NULL },
    /* css-text-4 §7.1. The fixture is the case §7.1's own sentence is about — a value "other than justify-all
       or match-parent", which is assigned to `text-align-all` and RESETS `text-align-last` to `auto` — so the
       round trip exercises the reset, which is the half an alias would not have. The two exception values are
       each their own fixed pair and are exercised by the serialization's own two equality tests. */
    { "text-align",    LH_TEXT_ALIGN,     2, CSS_SH_TEXT_ALIGN, "center", NULL, NULL },
    /* css-inline-3 §4.2. The fixture names all three terms, in the grammar's canonical order and none of them
       at its initial, so the round trip exercises every arm of the partition at once — a `[first|last]`, an
       `<'alignment-baseline'>` and an `<'baseline-shift'>` keyword. */
    { "vertical-align", LH_VERTICAL_ALIGN, 3, CSS_SH_ALL_OF, "first middle super",
      VERTICAL_ALIGN_INITIAL, "baseline" },
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

/* A THREE-TERM `||`, written in the canonical order of the grammar with each term omitted when it holds that
   longhand's initial value — §6.7.2's "If component values can be omitted or replaced with a shorter
   representation without changing the meaning of the value, omit/replace them." `initial` is the same list the forward expansion fills an omitted term from, which is
   what keeps the two directions one statement; it is the ROW's, because the rule is one algorithm over two
   grammars (css-backgrounds-3 §3.4's `<line-width> || <line-style> || <color>` and css-inline-3 §4.2's
   `[first|last] || <'alignment-baseline'> || <'baseline-shift'>`).
   EVERY TERM INITIAL LEAVES NOTHING TO WRITE, and the empty string matches no production of either grammar —
   so the answer is the SHORTHAND's own `Initial:` line where its definition states one and §6.7.2's "cannot
   exactly represent the values" arm where it does not. See the row's `whole_initial` for why that is the
   difference between the two rows rather than a preference. */
static char *css_sh_all_of_value(const char *const *v, const char *const *initial, unsigned terms,
                                 const char *whole_initial)
{
    const char *parts[4];
    unsigned n = 0, i;

    DCHECK(terms >= 1 && terms <= CSS_SH_N(parts),
           "§6.7.2's `||` serialization was asked for a term count this function has no room for. It is a "
           "FIXED array because every row of this kind is one property definition's own `Value:` line and none "
           "of them is longer than that — a row that is has a grammar to state here, not a larger buffer");
    for (i = 0; i < terms; i++)
        if (strcmp(v[i], initial[i]) != 0) parts[n++] = v[i];
    if (n == 0) return whole_initial ? css_sh_strdup(whole_initial) : NULL;
    return css_sh_join(parts, n);
}

/* css-flexbox-1 §7.1's `Value:` line RUN BACKWARDS under CSSOM §6.7.2's two rules — the canonical order is the
   grammar's, which is this row's own longhand order, and a component is omitted when it "can be omitted or
   replaced with a shorter representation without changing the meaning of the value".
   WHAT DECIDES THAT HERE IS §7.1's OMITTED DEFAULTS AND NOT THE LONGHANDS' `Initial:` LINES, which is the same
   sentence that makes this a kind of its own: writing `flex: 0 1 auto` as the empty string would MEAN
   `1 1 0`, and dropping a `flex-shrink: 1` is meaning-preserving precisely because 1 is what §7.1 substitutes
   for an absent shrink.
   THE GRAMMAR CONSTRAINS THE OMISSIONS AS WELL AS THE VALUES, in two ways a per-term test alone would miss.
   The factors are an ORDERED PAIR — `<'flex-grow'> <'flex-shrink'>?` — so a shrink that is written needs the
   grow written before it; and the whole value must be SOMETHING, so an all-default triple keeps its grow (a
   `flex` of `1 1 0` is written `1`, never the empty string, which matches no production).
   AND §7.1's UNITLESS-ZERO RULE IS THE THIRD CONSTRAINT, on the basis rather than on the factors. A basis that
   is a literal `<number>` re-reads as a flex factor unless two factors precede it, so a NON-ZERO one is a
   triple no `flex` declaration could have produced at all — §6.7.2's "cannot exactly represent the values of
   all the properties in list", reported by NULL, the way `border`'s disagreeing sides are — and a zero one
   forces both factors to be written even where each holds its own default. `0` itself never reaches either
   case, because it IS the basis's omitted default and is dropped one line up. */
static char *css_sh_flex_value(const char *const *v)
{
    const char *parts[3];
    unsigned n = 0;
    bool show_basis = strcmp(v[2], FLEX_OMITTED[2]) != 0;
    bool show_shrink = strcmp(v[1], FLEX_OMITTED[1]) != 0;
    bool show_grow;

    if (show_basis) {
        size_t bn = strlen(v[2]);
        CssShSpan probe;
        double num;
        /* §7.1's unitless-zero rule decides the ARRANGEMENT before it decides the value: a `<number>` basis is
           only ever grammatical after two flex factors, so writing one commits both. */
        bool numeric = css_shorthand_number(v[2], bn, &num);

        if (numeric) show_shrink = true;
        /* AND THE GRAMMAR IS ASKED RATHER THAN RESTATED, which is what stops the two directions parting: the
           question "would a `flex` declaration carrying these bytes read them back as this basis" is the
           EXPANSION'S OWN predicate, run in the arrangement the line below is about to write. A `flex-basis`
           the longhand carries and no `flex` value could produce — a negative length, a non-zero bare number,
           a keyword outside §7.2.3's set — is CSSOM §6.7.2's "cannot exactly represent the values of all the
           properties in list", reported by NULL exactly as `border`'s disagreeing sides are. */
        if (!flex_take_basis(&probe, v[2], bn, numeric)) return NULL;
    }
    show_grow = show_shrink || !show_basis || strcmp(v[0], FLEX_OMITTED[0]) != 0;
    if (show_grow) parts[n++] = v[0];
    if (show_shrink) parts[n++] = v[1];
    if (show_basis) parts[n++] = v[2];
    DCHECK(n >= 1,
           "css-flexbox-1 §7.1's serialization omitted every component, and the empty string matches no "
           "production of its `Value:` line. The grow term is what stands when the other two are at their "
           "omitted defaults, so reaching this means that rule was dropped rather than that a value was");
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
    {
        /* The three parts of ONE side, gathered out of the twelve — `border` has already been established to
           set every side alike, so any side's triple is the shorthand's. §3.4 gives it an `Initial:` line of
           "see individual properties" exactly as `border-<side>` has, so an all-initial list has no value to
           be written as here either. */
        const char *const triple[3] = { v[0], v[4], v[8] };

        return css_sh_all_of_value(triple, BORDER_PART_INITIAL, 3, NULL);
    }
}

/* css-text-4 §7.1's ASSIGNMENT RUN BACKWARDS, and it is a function rather than a search because
   `text-align-last` alone tells the three cases apart. `auto` is the reset EVERY value other than the two
   named exceptions performs, so a pair ending in it was written as `text-align-all`'s own value; a pair of
   `justify` is the only one `justify-all` produces; a pair of `match-parent` is the only one `match-parent`
   produces.
   ANY OTHER PAIR IS ONE NO `text-align` DECLARATION COULD HAVE MADE — a `text-align-last: center` set on its
   own beside a `text-align-all: left` — and CSSOM §6.7.2's "cannot exactly represent the values" arm is the
   answer, which the block serialization emits as the empty string. That is the same shape as `border`'s
   disagreeing sides and is reported the same way, by NULL. */
static char *css_sh_text_align_value(const char *const *v)
{
    if (strcmp(v[1], "auto") == 0) return css_sh_strdup(v[0]);
    if (strcmp(v[0], "justify") == 0 && strcmp(v[1], "justify") == 0) return css_sh_strdup("justify-all");
    if (strcmp(v[0], "match-parent") == 0 && strcmp(v[1], "match-parent") == 0)
        return css_sh_strdup("match-parent");
    return NULL;
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
    case CSS_SH_ALL_OF:    return css_sh_all_of_value(values, row->initial, row->n, row->whole_initial);
    case CSS_SH_FLEX:      return css_sh_flex_value(values);
    case CSS_SH_FONT:      return css_font_shorthand_value(values);
    case CSS_SH_TEXT_ALIGN: return css_sh_text_align_value(values);
    case CSS_SH_BACKGROUND: return css_background_shorthand_value(values);
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
        DCHECK((row->kind == CSS_SH_ALL_OF) == (row->initial != NULL),
               "a shorthand row carries a per-longhand INITIAL list without being the `||` kind that reads it, "
               "or is that kind and carries none. §6.7.2's omission rule needs exactly that list to "
               "know which terms can be dropped, and a NULL one would be read past the null pointer rather "
               "than reported");
        DCHECK(row->whole_initial == NULL || row->initial != NULL,
               "a shorthand row states its OWN `Initial:` line without the per-longhand initials the all-"
               "initial case is detected by — the second is what decides that the first is ever reached");
        if (row->whole_initial != NULL)
            for (j = 0; j < row->n; j++) {
                char *from_whole = css_shorthand_component(row->name, row->whole_initial, row->longhands[j]);

                DCHECK(from_whole != NULL && strcmp(from_whole, row->initial[j]) == 0,
                       "a shorthand's own `Initial:` line does not EXPAND to its longhands' own initial values, "
                       "so the two faces of one value disagree. The serialization writes that line whenever "
                       "every term is initial and the cascade writes those initials whenever the shorthand is "
                       "not declared, and a page reading the shorthand back would then see a value no "
                       "declaration of it could have produced");
                free(from_whole);
            }
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
    /* css-flexbox-1 §7.1's ARRANGEMENTS AND SENTENCES ONE FIXTURE CANNOT REACH. The row's round trip exercises
       the grammar in the arrangement where every component is written; each case below is a DIFFERENT sentence
       of §7.1, and each of them fails SILENTLY — the declaration is not dropped, it sets a wrong `flex-basis`
       or a wrong factor on a page that will simply lay out at the wrong width. That is the one failure mode a
       crash cannot find later, so it is pinned here.
       THE EXPECTED TRIPLES ARE READ OFF §7.1's OWN TEXT and not off this implementation: `none` "expands to
       0 0 auto"; a value of one `<number>` is a flex factor with the other two components omitted, so §7.1.1
       "Basic Values of flex" states `flex: <number [1,∞]>` as "Equivalent to flex: <number [1,∞]> 1 0"; the
       `||` admits the basis before the factors as well as after; and "A unitless zero that is not already
       preceded by two flex factors must be interpreted as a flex factor", which makes `1 1 0` a basis of 0 and
       `0 1 1` a declaration no arrangement matches. */
    {
        static const struct { const char *value; const char *want[3]; } FLEX_CASES[] = {
            /* §7.1's `none` arm, whose three values are its own sentence rather than the omitted defaults. */
            { "none",       { "0", "0", "auto" } },
            /* §7.1.1's `flex: <number>`, and the omitted-default pair that is not the longhands' initials. */
            { "2",          { "2", "1", "0" } },
            /* §7.1's `Initial:` line, which IS each longhand's own initial — the one triple where the two
               lists agree, and the reason a reader can mistake them for one list. */
            { "0 1 auto",   { "0", "1", "auto" } },
            /* The `||` in the other order: a `<'flex-basis'>` ahead of the ordered factor pair, with one
               factor and with both. The factors stay grow-then-shrink inside their own term. */
            { "10px 2",     { "2", "1", "10px" } },
            { "10px 2 3",   { "2", "3", "10px" } },
            /* The unitless zero AS A BASIS, which is only ever the third component of three — and a zero
               written any other way, which the serialization has to keep both factors in front of. */
            { "1 1 0",      { "1", "1", "0" } },
            { "1 1 0.0",    { "1", "1", "0.0" } },
            /* And as a FACTOR everywhere else — including where a basis would have been grammatical. */
            { "0 auto",     { "0", "1", "auto" } },
            { "1 0",        { "1", "0", "0" } },
            /* A zero WITH A UNIT is the spelling §7.1 tells authors to write, and it is a basis anywhere. */
            { "0px",        { "1", "1", "0px" } },
            /* A keyword `<'flex-basis'>` is answered in its canonical spelling, as every keyword here is,
               and css-sizing-3 §3.2's level-3 additions are in the set by reference from §7.2.3. */
            { "MIN-CONTENT", { "1", "1", "min-content" } },
            { "stretch",    { "1", "1", "stretch" } },
            /* A math function is ONE component value however many spaces its arguments carry. */
            { "calc(10px + 2em)", { "1", "1", "calc(10px + 2em)" } },
        };
        /* Triples the three LONGHANDS can each hold and no `flex` declaration produces — CSSOM §6.7.2's
           "cannot exactly represent the values of all the properties in list", which the block serialization
           reads as the empty string and answers by moving on. Each is a `<'flex-basis'>` the grammar would
           read back as something other than a basis, which is the one way this shorthand reaches that arm. */
        static const char *const FLEX_UNREPRESENTABLE[][3] = {
            { "1", "1", "5" },        /* a bare non-zero re-reads as a flex factor */
            { "1", "1", "-3px" },     /* css-sizing-3 §3.2: "Negative values are invalid" */
            { "2", "3", "inf" },      /* a C number that is not a CSS one */
        };
        /* Values §7.1's grammar does not admit. Each is a DROPPED declaration setting no longhand at all, and
           each is one this file would have got wrong in a different way: a unitless non-zero basis (which
           css_length_is_length_percentage answers TRUE for), a third flex factor, a negative factor, a
           negative length, and a fourth component. */
        static const char *const FLEX_INVALID[] = {
            "0 1 1", "1 1 1", "1 2 3 4", "-1 auto", "1 -1", "1 1 -10px", "1 1 5", "nope", "1 nope", "none 1",
            /* AND THE THREE SPANS `strtod` READS AS NUMBERS AND CSS DOES NOT, which are the reason
               css_shorthand_number filters the span and flex_take_basis asks css-values-4 §5.4's first
               character. Each of them would otherwise reach a computed value as a `<number>` or a
               `<length>` that no production of either grammar admits. */
            "0x10 auto", "inf", "nan auto",
        };

        for (i = 0; i < CSS_SH_N(FLEX_CASES); i++) {
            char *back;
            char *got[3];

            for (j = 0; j < CSS_SH_N(got); j++) {
                got[j] = css_shorthand_component("flex", FLEX_CASES[i].value, LH_FLEX[j]);
                DCHECKF(got[j] != NULL && strcmp(got[j], FLEX_CASES[i].want[j]) == 0,
                        "css-flexbox-1 §7.1's expansion of `flex: %s` gave `%s` the value `%s` where the "
                        "section states `%s`. This is the failure that does not crash: the declaration is "
                        "ACCEPTED and one of its three longhands carries a value the page never wrote, so "
                        "every flex item under it is laid out at a size §9.9.3's cap and floor derive from a "
                        "number nobody chose",
                        FLEX_CASES[i].value, LH_FLEX[j], got[j] ? got[j] : "(dropped)",
                        FLEX_CASES[i].want[j]);
            }
            /* AND THE SERIALIZATION AGREES WITH THE EXPANSION ON EVERY ONE OF THEM, which is a stronger
               statement than the row's own round trip: that one starts from a value already in the omitted
               form, and these start from values §6.7.2 must SHORTEN. Re-expanding the shortened form is what
               says the shortening preserved the meaning, which is the whole of §6.7.2's condition. */
            back = css_shorthand_serialize_value("flex", (const char *const *)got);
            DCHECKF(back != NULL,
                    "css-flexbox-1 §7.1's serialization reported that `flex: %s`'s own three longhand values "
                    "cannot be written as a `flex` declaration — but they came OUT of one, so §6.7.2's "
                    "\"cannot exactly represent\" arm has claimed a triple this grammar produces",
                    FLEX_CASES[i].value);
            for (j = 0; j < CSS_SH_N(got); j++) {
                char *again = back ? css_shorthand_component("flex", back, LH_FLEX[j]) : NULL;

                DCHECKF(again != NULL && strcmp(again, FLEX_CASES[i].want[j]) == 0,
                        "css-flexbox-1 §7.1's serialization shortened `flex: %s` to `%s`, and re-expanding "
                        "that gives `%s` the value `%s` rather than `%s`. CSSOM §6.7.2 permits an omission "
                        "only \"without changing the meaning of the value\", so a component was dropped that "
                        "the grammar reads back as something else — §7.1's unitless-zero rule is where that "
                        "happens",
                        FLEX_CASES[i].value, back ? back : "(nothing)", LH_FLEX[j],
                        again ? again : "(dropped)", FLEX_CASES[i].want[j]);
                free(again);
            }
            free(back);
            for (j = 0; j < CSS_SH_N(got); j++) free(got[j]);
        }
        for (i = 0; i < CSS_SH_N(FLEX_UNREPRESENTABLE); i++) {
            char *back = css_shorthand_serialize_value("flex", FLEX_UNREPRESENTABLE[i]);

            DCHECKF(back == NULL,
                    "css-flexbox-1 §7.1's serialization wrote `%s` for a `flex-basis` of `%s`, and no `flex` "
                    "declaration carrying those bytes reads them back as that basis. CSSOM §6.7.2 permits a "
                    "shortening only \"without changing the meaning of the value\", so this is the "
                    "\"cannot exactly represent\" arm being skipped — and the visible symptom is a `cssText` "
                    "the page never wrote and this engine cannot re-parse",
                    back ? back : "(nothing)", FLEX_UNREPRESENTABLE[i][2]);
            free(back);
        }
        for (i = 0; i < CSS_SH_N(FLEX_INVALID); i++)
            for (j = 0; j < CSS_SH_N(LH_FLEX); j++) {
                char *got = css_shorthand_component("flex", FLEX_INVALID[i], LH_FLEX[j]);

                DCHECKF(got == NULL,
                        "css-flexbox-1 §7.1's expansion accepted `flex: %s` and gave `%s` the value `%s`. CSS "
                        "Syntax drops a declaration outside a property's grammar WHOLE, so a value this "
                        "section does not admit must set none of the three longhands — accepting one is a "
                        "number on the page that no declaration of it could have produced",
                        FLEX_INVALID[i], LH_FLEX[j], got);
                free(got);
            }
    }
    /* css-inline-3 §4.2's THREE TERM SETS ARE DISJOINT, which is the property that lets its `||` be split by
       the component value alone. Asserted by asking the partition itself about every keyword of every set: a
       word that answered a term other than its own would be one the header's claim is false of, and the
       symptom would be a `vertical-align: middle` setting `baseline-shift` — a silent wrong longhand rather
       than a dropped declaration. The `<length-percentage>` arm needs no row here: it is the fallback the
       partition reaches only after all three keyword sets have refused. */
    {
        static const char *const *const VA_SETS[] = {
            BASELINE_SOURCE_KEYWORDS, ALIGNMENT_BASELINE_KEYWORDS, BASELINE_SHIFT_KEYWORDS
        };
        static const unsigned VA_SET_N[] = {
            CSS_SH_N(BASELINE_SOURCE_KEYWORDS), CSS_SH_N(ALIGNMENT_BASELINE_KEYWORDS),
            CSS_SH_N(BASELINE_SHIFT_KEYWORDS)
        };

        for (i = 0; i < CSS_SH_N(VA_SETS); i++)
            for (j = 0; j < VA_SET_N[i]; j++) {
                const char *canon = NULL;
                const char *kw = VA_SETS[i][j];
                /* The partition is run HERE and not inside the assert: it writes through `canon`, and a
                   DCHECK's condition must be side-effect-free (check.h) so that the compiled-out build runs
                   the same program as the dev one. */
                int term = vertical_align_term_of(kw, strlen(kw), &canon);

                DCHECK(term == (int)i && canon != NULL && strcmp(canon, kw) == 0,
                       "a keyword of one of css-inline-3 §4.2's three term sets is classified as another "
                       "term's, so the sets are not disjoint and the shorthand's `||` cannot be split by the "
                       "component value. Whichever set is scanned FIRST would take the shared word and the "
                       "other longhand would silently read its initial value");
            }
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
         color — NO shorthand sets it;
         visibility — NO shorthand in CSS sets it. css-display-3 §4 "Invisibility: the visibility property"
           declares it as a standalone property with its own `Value:` line (`visible | hidden | collapse`) and
           its own `Computed value:` line, and the module states no shorthand over it: §2's `display` decides
           whether a box is GENERATED and is a different property, which §4's own parenthesis says outright
           ("Set the display property to none to suppress box generation altogether"). It shares a keyword
           with `overflow` (`hidden`) and a name with nothing;
         flex-direction, flex-wrap — `flex-flow` is the only shorthand that sets either (css-flexbox-1 §5.3,
           whose `Value:` line is exactly those two longhands), and it is in the table above. §7.1's `flex`
           sets the three flexibility longhands and neither of these two.
           THIS PARAGRAPH STOOD FOR A WHILE WITH NEITHER NAME IN THE LIST BELOW, which is the quietest shape
           this defect has and is worth recording rather than tidying away: the ARGUMENT was written and the
           two entries were not, so the reasoning read as done while the predicate answered FALSE — and
           css_computed_value.c asserts this predicate before it derives anything, so every read of either
           property aborted at a message about an UNEXPANDED SHORTHAND for a shorthand that was in fact
           expanded. A list a paragraph argues for is a list a reader will believe without checking;
         flex-grow, flex-shrink, flex-basis — css-flexbox-1 §7.1 "The flex Shorthand"'s `flex` is the ONLY
           shorthand in CSS that sets any of the three, and it is in the table above. §7.1's `Value:` line is
           `none | [ <'flex-grow'> <'flex-shrink'>? || <'flex-basis'> ]`, which is exactly these three and
           nothing else, while §7.2.1, §7.2.2 and §7.2.3 declare each as a standalone longhand with its own
           `Value:`, `Initial:` and `Computed value:` lines. No module states a second container over them:
           §5.3's `flex-flow` sets `flex-direction` and `flex-wrap` and none of these, §5.4's `order` is a
           sibling property about painting and ordering rather than about flexibility, and CSS Box Alignment 3's
           `place-*` shorthands are over `align-*`/`justify-*`, which are different properties again. All three
           ARE in lexbor's property registry, so — like `text-align` and unlike the border longhands — their own
           declarations were typed and validated all along and it was the SHORTHAND that set nothing: a
           `flex: 1` reached the cascade as a declaration of a property no consumer reads, and §9.9.3 "Flex Item
           Intrinsic Size Contributions"' cap and floor would have read a `flex-grow` of 0 and a `flex-shrink`
           of 1 off every item on every page that writes the shorthand;
         THE EIGHT css-backgrounds-3 §2.10 NAMES — `background` is the ONLY shorthand in CSS that sets any of
           them, and it is in the table above. §2.10 states the whole of the relation in its own words ("given
           a valid declaration, for each layer the shorthand first sets the corresponding value of each of
           background-image, background-position, background-size, background-repeat, background-origin,
           background-clip and background-attachment to that property's initial value … finally
           background-color is set to the specified color"), and each of the eight is declared as a standalone
           longhand by its own section (§2.2 through §2.9). No module states a second container over them:
           §5.7 "Border Image Shorthand: the border-image property" sets the five `border-image-*` longhands
           and none of these, and CSS Logical states no flow-relative group over any of the eight, so there is
           no `background-*-block`/`-inline` pair to be a second spelling of one.
           `background-color` IS THE ONE THIS ROW WAS BUILT FOR, and it is the quietest shape the missing-row
           defect has: it is in CSSOM §9's unconditional used-value list, so `getComputedStyle(el)
           .backgroundColor` is a USED value and every used-value consumer asserts this predicate first — which
           means a `background: red` two lines above the read was not merely unexpanded, it was UNREACHABLE,
           and the answer would have been `transparent` with a real colour to show for it;
         outline-color and caret-color are NOT here, and each names one missing shorthand: `outline-color`
           needs css-ui-4 §3.1 "Outlines Shorthand: the outline property" and `caret-color` needs its §5.2.4
           "Insertion caret shorthand: caret". Neither is in the table above, and neither longhand is in
           lexbor's property registry either, so a row alone would not finish them — the cascade has no value
           to read for a property nothing types, which is the one way this pair differs from §2.10's eight;
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
       answer decides whether a `margin: 0` two lines up was read or ignored.
       `white-space` is a LONGHAND here on CSS 2.1 §16.6's statement of it, and no shorthand in this table sets
       it; css-text-4's decomposition into `white-space-collapse`/`text-wrap-mode` names two properties lexbor's
       registry does not carry, so there is no expansion for this file to take apart.
       `direction` and `writing-mode` — NO shorthand in CSS sets either. css-writing-modes-4 declares both as
       standalone properties (§2.1 "Specifying Directionality: the direction property", §3.2 "Block Flow
       Direction: the writing-mode property"), each with its own `Value:` line and its own `Computed value:`
       line, and the module states no shorthand over them — its own §3.2.1 "Obsolete SVG1.1 writing-mode Values"
       is about VALUES of `writing-mode` and not about a second property that sets it. The `text-orientation`
       §5.1 declares is a sibling longhand, not a container.
       `baseline-source`, `alignment-baseline` and `baseline-shift` — css-inline-3 §4.2's `vertical-align` is
       the only shorthand that sets any of the three, and it is in the table above. §4.2 says so in its own
       words for the first ("if first or last is specified, it sets baseline-source, which is otherwise reset
       to auto") and declares the other two as its remaining components; no other module states a shorthand
       over them, and CSS Box Alignment 3's `place-*` shorthands are over `align-*`/`justify-*`, which are
       different properties. THE ROW THAT MADE THIS ANSWERABLE IS THE SHORTHAND'S, and until it existed a
       `vertical-align: middle` set NO longhand at all — the declaration reached the cascade as one lexbor had
       typed and nothing had taken apart, so all three read as undeclared, with their initial values to show
       for it and nothing to say the declaration was never looked at.

       `transform` — NO shorthand in CSS sets it. css-transforms-1 §3 "The transform Property" declares it as a
       standalone property with its own `Value:` line (`none | <transform-list>`), and the module states no
       shorthand over it: its §4 `transform-origin` and §5 `transform-box` are SIBLING longhands, not
       containers, and css-transforms-2's `translate`, `rotate`, `scale` and `perspective` are separate
       PROPERTIES that contribute to the same rendering — §3's own "Any computed value other than none for the
       transform affects containing block and stacking context" is stated over this property alone — rather
       than longhands of this one. `transition` and `animation` name a property, they do not set it.

       THE FONT LONGHANDS, and the SEVEN that are deliberately absent. css-fonts-4 §2.7's `font` is the only
       shorthand in CSS that sets `font-size`, `line-height`, `font-family`, `font-style`, `font-weight` or
       `font-stretch` — §2.3.1 makes `font-width` a legacy name ALIAS of that last one rather than a second
       property — and it is the only one that resets `font-feature-settings`, `font-kerning`,
       `font-language-override`, `font-optical-sizing`, `font-size-adjust` or `font-variation-settings`. It is
       in the table above, so those twelve are complete.
       `font-variant-caps` AND THE SIX OTHER `font-variant-*` LONGHANDS ARE NOT, and the reason is one row this
       table does not have: css-fonts-4 §6.11's `font-variant` is a second shorthand that sets every one of
       them, and it is not here. Recording them would assert a completeness that is false in the direction that
       matters — a `font-variant: small-caps` two lines above a `font-variant-caps` read would be invisible.

       `text-align-all` and `text-align-last` — css-text-4 §7.1's `text-align` is the ONLY shorthand in CSS
       that sets either, and it is in the table above. §7.1 states the whole of the relation in its own words
       ("this shorthand property sets the text-align-all and text-align-last properties"), while §7.3 "Default
       Text Alignment: the text-align-all property" and §7.4 "Last Line Alignment: the text-align-last
       property" declare each as a standalone longhand with its own `Value:` and `Computed value:` lines. No
       module states a second container over them: §7.5's `text-justify` selects the METHOD a `justify`
       alignment uses and is a sibling property, and §7.6's `text-group-align` aligns a block within its
       container rather than content within a line.
       THE ROW IS WHAT MADE THIS ANSWERABLE, exactly as `vertical-align`'s did one paragraph up. `text-align`
       IS in lexbor's property registry, so a `text-align: center` declaration was TYPED and serialized all
       along and simply set no longhand — which is the quietest shape this defect has: nothing was dropped and
       nothing was unknown, the value was just filed under a name no consumer reads. CSS 2.2 §9.4.2's "their
       horizontal distribution within the line box is determined by the 'text-align' property" would have read
       `start` for every line box in every document, with a real coordinate to show for it.

       CSS 2.1 §17 Tables' `caption-side`, `table-layout`, `border-collapse` and `border-spacing` — NO
       shorthand in CSS sets any of the four. §17.4.1 Caption position and alignment, §17.5.2 Table width
       algorithms: the 'table-layout' property, §17.6 Borders and §17.6.1 The separated borders model each
       declare one of them as a standalone property with its own `Value:` line, and CSS 2.1 states no container
       over any of them. THE ONE THAT LOOKS LIKE A COUNTEREXAMPLE IS `border`, AND IT IS NOT ONE: css-
       backgrounds-3 §3.4 "Border Shorthand Property: the border property" sets the twelve `border-*-width`,
       `border-*-style` and `border-*-color` longhands — the same twelve this list already records three
       shorthands for — and neither `border-collapse` nor `border-spacing` is among them. The two share a
       PREFIX with that family and nothing else: they are §17.6's two properties about which BORDER MODEL a
       table is in and how far apart separated cell borders sit, not about any one box's own border edge.
       All four are absent from lexbor's property registry, so their grammars are this component's (the §17
       table above) and a declaration reaching the cascade has been through one. */
    static const char *const RECORDED[] = {
        "overflow-x", "overflow-y", "display", "float", "position", "box-sizing", "color", "white-space",
        "direction", "writing-mode", "transform", "visibility",
        "flex-direction", "flex-wrap", "flex-grow", "flex-shrink", "flex-basis",
        "background-image", "background-position", "background-size", "background-repeat",
        "background-attachment", "background-origin", "background-clip", "background-color",
        "baseline-source", "alignment-baseline", "baseline-shift",
        "text-align-all", "text-align-last",
        "font-size", "line-height", "font-family", "font-style", "font-weight", "font-stretch",
        "font-feature-settings", "font-kerning", "font-language-override", "font-optical-sizing",
        "font-size-adjust", "font-variation-settings",
        "margin-top", "margin-right", "margin-bottom", "margin-left",
        "padding-top", "padding-right", "padding-bottom", "padding-left",
        "width", "height", "min-width", "max-width", "min-height", "max-height",
        "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
        "border-top-style", "border-right-style", "border-bottom-style", "border-left-style",
        "border-top-color", "border-right-color", "border-bottom-color", "border-left-color",
        "caption-side", "table-layout", "border-collapse", "border-spacing",
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
