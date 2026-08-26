/* Every property definition's numeric productions — see css_property_numeric.h. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "core/css/css_property_numeric.h"

/* One production as a bit, spelled short so the table below is a list of GRAMMARS and not of macro names. */
#define N_(p)  CSS_NUMERIC_BIT(CSS_MATH_PROD_##p)
#define S_NONE CSS_NUMERIC_NONE
#define S_WHOL CSS_NUMERIC_WHOLE
#define S_COMP CSS_NUMERIC_COMPONENT

typedef struct {
    const char     *name;
    CssNumericShape shape;
    unsigned        productions;
} CssNumericRow;

/* EVERY PROPERTY THIS ENGINE CAN PUT IN A DECLARATION BLOCK, IN ASCENDING NAME ORDER (asserted below, because a
   sorted table is what makes a missing row visible at a glance and a duplicate impossible to miss).
   THE SET IS lexbor's property registry UNION the longhands core/css/css_shorthand.h's expansion writes, and
   both halves are load-bearing. The registry is what decides which declarations lexbor TYPES — only a typed
   declaration can be found INVALID and so reach the re-judging this table exists for; an untyped one is handed
   on as raw tokens and was never lexbor's to refuse. The expansion's longhands are the other half because a
   `border-top-width` never appears in any source text this engine parses and still ends up in a block, so the
   consumer that asserts a collected declaration matches its property's production would have nothing to ask.
   A NAME IN NEITHER HALF CRASHES rather than defaulting — see the header.

   THE GRAMMARS, with the section each row's production is read out of. Where two levels of a spec disagree the
   row follows the one lexbor's own grammar implements, because the row's job is to decide what lexbor could not
   judge rather than to be a second opinion about what it did:

     css-sizing-3 §3.1.1 "Preferred Size Properties: the width and height properties",
                  §3.1.2 "Minimum Size Properties", §3.1.3 "Maximum Size Properties"
                                         `auto | <length-percentage [0,∞]> | min-content | max-content | ...`
     css-box-4 §3.1 "Page-relative (Physical) Margin Properties"        `<length-percentage> | auto`
     css-box-4 §4.1 "Page-relative (Physical) Padding Properties"       `<length-percentage [0,∞]>`
     css-position-3 §3.1 "Box Insets: the top, right, bottom, left, inset-block-start, inset-inline-start,
                  inset-block-end, and inset-inline-end properties"     `auto | <length-percentage>`
     css-logical-1 §4.3 "Flow-Relative Offsets" makes the four `inset-*` longhands `<'top'>`, which is why they
                  carry the inset row's production rather than one of their own.
     css-fonts-4 §2.5 "Font size: the font-size property"
                                    `<absolute-size> | <relative-size> | <length-percentage [0,∞]> | math`
     css-fonts-4 §2.2 "Font weight: the font-weight property" — `<font-weight-absolute> | bolder | lighter`,
                  and §2.2's `<font-weight-absolute> = [ normal | bold | <number [1,1000]> ]`, so a NUMBER.
     css-fonts-4 §2.3.1 "Font width: the font-stretch legacy name alias" — §2.3's `font-width` is
                  `normal | <percentage [0,∞]> | <keywords>`, so a PERCENTAGE and never a length.
     css-fonts-4 §2.4 "Font style: the font-style property" — `normal | italic | left | right |
                  oblique <angle [-90deg,90deg]>?`: an ANGLE, and one that stands AFTER a keyword.
     css-fonts-4 §2.6 "Relative sizing: the font-size-adjust property"  `none | <number [0,∞]>`
     css-fonts-4 §2.7 "Shorthand font property: the font property"      — its `<'font-size'>` and its
                  `[ / <'line-height'> ]?` are where a math function can stand in it.
     css-fonts-4 §6.12 "Low-level font feature settings control: the font-feature-settings property" —
                  `<feature-tag-value> = <opentype-tag> [ <integer [0,∞]> | on | off ]?`
     css-fonts-4 §8.2 "Low-level font variation settings control: the font-variation-settings property" —
                  `normal | [ <opentype-tag> <number> ]#`
     css-backgrounds-3 §3.3 "Line Thickness: the border-width properties" — `<line-width>`, which §3.3 defines
                  as `<length [0,∞]> | thin | medium | thick`. NO PERCENTAGE: this is the row that must not be
                  written as `<length-percentage>` for convenience, and the reason the header gives.
     css-backgrounds-3 §6 "Border Images" — `border-image-slice` `[<number [0,∞]> | <percentage [0,∞]>]{1,4} &&
                  fill?`, `border-image-width` `[ <length-percentage [0,∞]> | <number [0,∞]> | auto ]{1,4}`,
                  `border-image-outset` `[ <length [0,∞]> | <number [0,∞]> ]{1,4}` — three neighbours whose
                  numeric branches differ from each other in every position.
     css-text-3 §7.1 "Word Spacing: the word-spacing property" and §7.2 "Tracking: the letter-spacing property"
                  are both `normal | <length>`. css-text-4 widens both to `<length-percentage>`; lexbor
                  implements the level 3 grammar and these rows follow it, so a `calc(1em + 1%)` there is a
                  dropped declaration exactly as it is in the engines that ship level 3.
     css-text-3 §4.2 "Tab Character Size: the tab-size property"        `<number [0,∞]> | <length [0,∞]>`
     css-text-3 §8.1 "First Line Indentation: the text-indent property" —
                  `[ <length-percentage> ] && hanging? && each-line?`, a COMPONENT because of the `&&`.
     css-inline-3 §5.1 "Line Spacing: the line-height property"
                                    `normal | <number [0,∞]> | <length-percentage [0,∞]>` — BOTH, and the
                  two do not subsume one another in CSS Typed OM 1 §4.3.2 "Numeric Value Typing".
     css-inline-3 §4.2.3 "Post-Alignment Shift: the baseline-shift longhand"
                                    `<length-percentage> | sub | super | top | center | bottom`
     css-color-4 §3.3 "Transparency: the opacity property" — `<opacity-value>`, which that section defines as
                  `<number> | <percentage>`: two productions, and `calc(50%)` matches only the second.
     css-flexbox-1 §7.2.1 "The flex-grow property" and §7.2.2 "The flex-shrink property"  `<number [0,∞]>`
     css-flexbox-1 §7.2.3 "The flex-basis property" — `content | <'width'>`, so `width`'s production.
     css-flexbox-1 §7.1 "The flex Shorthand" — `none | [ <'flex-grow'> <'flex-shrink'>? || <'flex-basis'> ]`
     css-display-3 §3 "Display Order: the order property"               `<integer>`
     css-writing-modes-4 §9.1 "Horizontal-in-Vertical Composition: the text-combine-upright property" —
                  `none | all | [ digits <integer [2,4]>? ]`
     css-page-floats-3 §5.1 "The float-defer property" `<integer> | last | none`, §7 "The float-offset
                  property" `<length-percentage>`
     CSS 2.1 §9.9.1 "Specifying the stack level: the 'z-index' property" `auto | <integer> | inherit`
     css-inline-3 §4.2 "Transverse Box Alignment: the vertical-align property" —
                  `[ first | last ] || <'alignment-baseline'> || <'baseline-shift'>`, a SHORTHAND, so its
                  numeric production sits in the `<'baseline-shift'>` COMPONENT (§4.2.3 "Post-Alignment Shift:
                  the baseline-shift longhand" is `<length-percentage> | sub | super | top | center | bottom`)
                  and `vertical-align: first calc(1em)` is a value only that longhand's grammar can judge.
                  CSS 2.2 §10.8 "Line height calculations: the 'line-height' and 'vertical-align' properties"
                  defines it as a longhand whose whole value is `... | <percentage> | <length>` instead — and
                  that is NOT the grammar parsed here: lexbor's `vertical-align` state machine is §4.2's `||`
                  and stores into an alignment and a shift sub-value, its registry carries all three longhands,
                  and core/css/css_shorthand.c expands the row. A `S_WHOL` here would send a two-component
                  value to the whole-value math test, which one lone math function is the only thing that can
                  pass — silently dropping every `vertical-align` that names a term beside its length.

   EVERY OTHER ROW IS `CSS_NUMERIC_NONE`, AND THAT IS A CLAIM RATHER THAN AN OMISSION: its grammar names no numeric
   production anywhere, so a math function written there is invalid CSS and the declaration is dropped by CSS
   Syntax 3 §5.5.6 "Consume a declaration"'s last step. The rows that look like they might not be are the ones
   worth having read the grammar for: `border-*-color` and `background-color` are `<color>`, which no math
   function's type can match; `overflow-wrap` and its `word-wrap` legacy alias are keyword-only; `flex-flow` is
   `<'flex-direction'> || <'flex-wrap'>`, two keyword grammars; `text-decoration` is three keyword-and-colour
   longhands; and `border-style`/`border-color` are `{1,4}` repetitions of keyword and colour grammars. */
static const CssNumericRow CSS_NUMERIC[] = {
    { "align-content",             S_NONE,      0 },
    { "align-items",               S_NONE,      0 },
    { "align-self",                S_NONE,      0 },
    { "alignment-baseline",        S_NONE,      0 },
    { "background-color",          S_NONE,      0 },
    { "baseline-shift",            S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "baseline-source",           S_NONE,      0 },
    { "border",                    S_COMP, N_(LENGTH) },
    { "border-bottom",             S_COMP, N_(LENGTH) },
    { "border-bottom-color",       S_NONE,      0 },
    { "border-bottom-style",       S_NONE,      0 },
    { "border-bottom-width",       S_WHOL,     N_(LENGTH) },
    { "border-color",              S_NONE,      0 },
    { "border-image-outset",       S_COMP, N_(NUMBER) | N_(LENGTH) },
    { "border-image-repeat",       S_NONE,      0 },
    { "border-image-slice",        S_COMP, N_(NUMBER) | N_(PERCENTAGE) },
    { "border-image-source",       S_NONE,      0 },
    { "border-image-width",        S_COMP, N_(NUMBER) | N_(LENGTH_PERCENTAGE) },
    { "border-left",               S_COMP, N_(LENGTH) },
    { "border-left-color",         S_NONE,      0 },
    { "border-left-style",         S_NONE,      0 },
    { "border-left-width",         S_WHOL,     N_(LENGTH) },
    { "border-right",              S_COMP, N_(LENGTH) },
    { "border-right-color",        S_NONE,      0 },
    { "border-right-style",        S_NONE,      0 },
    { "border-right-width",        S_WHOL,     N_(LENGTH) },
    { "border-style",              S_NONE,      0 },
    { "border-top",                S_COMP, N_(LENGTH) },
    { "border-top-color",          S_NONE,      0 },
    { "border-top-style",          S_NONE,      0 },
    { "border-top-width",          S_WHOL,     N_(LENGTH) },
    { "border-width",              S_COMP, N_(LENGTH) },
    { "bottom",                    S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "box-sizing",                S_NONE,      0 },
    { "clear",                     S_NONE,      0 },
    { "color",                     S_NONE,      0 },
    { "direction",                 S_NONE,      0 },
    { "display",                   S_NONE,      0 },
    { "dominant-baseline",         S_NONE,      0 },
    { "flex",                      S_COMP, N_(NUMBER) | N_(LENGTH_PERCENTAGE) },
    { "flex-basis",                S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "flex-direction",            S_NONE,      0 },
    { "flex-flow",                 S_NONE,      0 },
    { "flex-grow",                 S_WHOL,     N_(NUMBER) },
    { "flex-shrink",               S_WHOL,     N_(NUMBER) },
    { "flex-wrap",                 S_NONE,      0 },
    { "float",                     S_NONE,      0 },
    { "float-defer",               S_WHOL,     N_(INTEGER) },
    { "float-offset",              S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "float-reference",           S_NONE,      0 },
    { "font",                      S_COMP, N_(NUMBER) | N_(LENGTH_PERCENTAGE) },
    { "font-family",               S_NONE,      0 },
    { "font-feature-settings",     S_COMP, N_(INTEGER) },
    { "font-kerning",              S_NONE,      0 },
    { "font-language-override",    S_NONE,      0 },
    { "font-optical-sizing",       S_NONE,      0 },
    { "font-size",                 S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "font-size-adjust",          S_WHOL,     N_(NUMBER) },
    { "font-stretch",              S_WHOL,     N_(PERCENTAGE) },
    { "font-style",                S_COMP, N_(ANGLE) },
    { "font-variant-alternates",   S_NONE,      0 },
    { "font-variant-caps",         S_NONE,      0 },
    { "font-variant-east-asian",   S_NONE,      0 },
    { "font-variant-emoji",        S_NONE,      0 },
    { "font-variant-ligatures",    S_NONE,      0 },
    { "font-variant-numeric",      S_NONE,      0 },
    { "font-variant-position",     S_NONE,      0 },
    { "font-variation-settings",   S_COMP, N_(NUMBER) },
    { "font-weight",               S_WHOL,     N_(NUMBER) },
    { "hanging-punctuation",       S_NONE,      0 },
    { "height",                    S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "hyphens",                   S_NONE,      0 },
    { "inset-block-end",           S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "inset-block-start",         S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "inset-inline-end",          S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "inset-inline-start",        S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "justify-content",           S_NONE,      0 },
    { "left",                      S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "letter-spacing",            S_WHOL,     N_(LENGTH) },
    { "line-break",                S_NONE,      0 },
    { "line-height",               S_WHOL,     N_(NUMBER) | N_(LENGTH_PERCENTAGE) },
    { "margin",                    S_COMP, N_(LENGTH_PERCENTAGE) },
    { "margin-bottom",             S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "margin-left",               S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "margin-right",              S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "margin-top",                S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "max-height",                S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "max-width",                 S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "min-height",                S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "min-width",                 S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "opacity",                   S_WHOL,     N_(NUMBER) | N_(PERCENTAGE) },
    { "order",                     S_WHOL,     N_(INTEGER) },
    { "overflow",                  S_NONE,      0 },
    { "overflow-block",            S_NONE,      0 },
    { "overflow-inline",           S_NONE,      0 },
    { "overflow-wrap",             S_NONE,      0 },
    { "overflow-x",                S_NONE,      0 },
    { "overflow-y",                S_NONE,      0 },
    { "padding",                   S_COMP, N_(LENGTH_PERCENTAGE) },
    { "padding-bottom",            S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "padding-left",              S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "padding-right",             S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "padding-top",               S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "position",                  S_NONE,      0 },
    { "right",                     S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "tab-size",                  S_WHOL,     N_(NUMBER) | N_(LENGTH) },
    { "text-align",                S_NONE,      0 },
    { "text-align-all",            S_NONE,      0 },
    { "text-align-last",           S_NONE,      0 },
    { "text-combine-upright",      S_COMP, N_(INTEGER) },
    { "text-decoration",           S_NONE,      0 },
    { "text-decoration-color",     S_NONE,      0 },
    { "text-decoration-line",      S_NONE,      0 },
    { "text-decoration-style",     S_NONE,      0 },
    { "text-indent",               S_COMP, N_(LENGTH_PERCENTAGE) },
    { "text-justify",              S_NONE,      0 },
    { "text-orientation",          S_NONE,      0 },
    { "text-overflow",             S_NONE,      0 },
    { "text-transform",            S_NONE,      0 },
    { "top",                       S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "unicode-bidi",              S_NONE,      0 },
    { "vertical-align",            S_COMP,     N_(LENGTH_PERCENTAGE) },
    { "visibility",                S_NONE,      0 },
    { "white-space",               S_NONE,      0 },
    { "width",                     S_WHOL,     N_(LENGTH_PERCENTAGE) },
    { "word-break",                S_NONE,      0 },
    { "word-spacing",              S_WHOL,     N_(LENGTH) },
    { "word-wrap",                 S_NONE,      0 },
    { "wrap-flow",                 S_NONE,      0 },
    { "wrap-through",              S_NONE,      0 },
    { "writing-mode",              S_NONE,      0 },
    { "z-index",                   S_WHOL,     N_(INTEGER) },
};

#undef N_
#undef S_NONE
#undef S_WHOL
#undef S_COMP

#define CSS_NUMERIC_N (sizeof(CSS_NUMERIC) / sizeof(CSS_NUMERIC[0]))

void css_property_numeric_init(void)
{
    unsigned i;

    /* THE SORT IS THE SEARCH'S PRECONDITION AND IT IS ASSERTED RATHER THAN TRUSTED. A binary search over a
       table that stopped being sorted does not crash and does not scan the rest — it MISSES a row that is
       present, and this component answers a miss with the DFAIL below. So the day someone inserts a row in the
       wrong place, the symptom would be "a property outside this component's audit" naming a property that is
       plainly in the table two lines above it, which is a diagnosis pointing at the wrong defect. A strict
       ordering also makes a DUPLICATE row impossible, which matters more here than in a linear table: two rows
       for one property are two grammars, and which of them the search lands on is an implementation detail. */
    for (i = 1; i < CSS_NUMERIC_N; i++)
        DCHECK(strcmp(CSS_NUMERIC[i - 1].name, CSS_NUMERIC[i].name) < 0,
               "this component's property table is not in strictly ascending name order. It is read by binary "
               "search, so an out-of-order row is a property the search cannot reach and a duplicate is a "
               "second grammar for one property — and both report as an unaudited property rather than as the "
               "transcription error they are");
}

/* The table's binary search, shared so the two questions below cannot disagree about which names it holds. */
static const CssNumericRow *css_numeric_find(const char *name)
{
    unsigned lo = 0, hi = CSS_NUMERIC_N;

    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = strcmp(name, CSS_NUMERIC[mid].name);

        if (c == 0) return &CSS_NUMERIC[mid];
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

bool css_property_numeric_audited(const char *name)
{
    DCHECK(name != NULL, "the numeric-production table was asked whether it records nothing");
    return css_numeric_find(name) != NULL;
}

CssNumericShape css_property_numeric(const char *name, unsigned *productions)
{
    const CssNumericRow *row;

    DCHECK(name != NULL, "a property's numeric productions were asked for with no property name");
    DCHECK(productions != NULL,
           "a property's numeric productions were asked for with nowhere to write the set. The SHAPE alone "
           "does not decide whether a math function may be written — a CSS_NUMERIC_WHOLE `border-top-width` "
           "and a CSS_NUMERIC_WHOLE `font-size` differ only in the mask — so a caller that passed NULL is one "
           "that meant to ask a question this component does not answer");
    row = css_numeric_find(name);
    if (row != NULL) {
        *productions = row->productions;
        DCHECK((row->shape == CSS_NUMERIC_NONE) == (row->productions == 0),
               "a row states that a property's grammar names no numeric production and then names one, or "
               "states that it names some and lists none. The two halves are ONE fact written twice, which "
               "is the only way this table can be internally wrong, and a caller reading the mask past a "
               "NONE shape (or the shape past an empty mask) would take the opposite arm in silence");
        return row->shape;
    }
    *productions = 0;
    DFAIL("a property outside this component's audit was asked which numeric productions its grammar names. "
          "The audited set is lexbor's property registry UNION the longhands core/css/css_shorthand.h's "
          "expansion writes — which is every property whose value ANYTHING in this engine validates, and so "
          "the only set for which this question has an answer. This is one of exactly three things, and none "
          "may be answered with a default. EITHER lexbor's registry has grown a property (a vendored-source "
          "upgrade) or css_shorthand.c has grown a longhand, in which case READ THAT PROPERTY'S DEFINITION and "
          "add its row: which numeric productions its `Value:` line names, and whether they are the whole "
          "value or one component of it. OR a caller reached here with a name that is not a property at all — "
          "a custom property, whose `--` prefix its caller must test first, because CSS Properties and Values "
          "API 1 owns that grammar and this table does not. OR a caller is asking about a property lexbor "
          "never typed (`border-radius`, `gap`, `translate` — the registry stops well short of CSS), whose "
          "value reached the cascade as raw tokens with no grammar consulted: that caller wants "
          "`css_property_numeric_audited` and is asserting an invariant about a value nobody validated. "
          "Defaulting to NONE would call an unaudited property one that admits no math function, which is "
          "indistinguishable from a real answer and would drop every `calc()` written for it with nothing to "
          "say so");
    return CSS_NUMERIC_NONE;
}
