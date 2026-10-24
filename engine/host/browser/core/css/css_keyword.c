/* A CSS keyword as an ID — see core/css/css_keyword.h. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "core/css/css_keyword.h"

static const char *const CSS_KEYWORD_NAMES[CSS_KEYWORD_COUNT] = {
    [CSS_KEYWORD_INVALID] = NULL,
#define CSS_KEYWORD_NAME_ROW(id, name) [CSS_KEYWORD_##id] = name,
    CSS_KEYWORD_LIST(CSS_KEYWORD_NAME_ROW)
#undef CSS_KEYWORD_NAME_ROW
};

static char css_keyword_ascii_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

CssKeyword css_keyword_of(const char *ident, size_t n)
{
    unsigned k;
    size_t i;

    DCHECK(ident != NULL, "a keyword was looked up with no identifier");
    for (k = CSS_KEYWORD_INVALID + 1; k < CSS_KEYWORD_COUNT; k++) {
        const char *name = CSS_KEYWORD_NAMES[k];

        if (strlen(name) != n) continue;
        for (i = 0; i < n && css_keyword_ascii_lower(ident[i]) == name[i]; i++) {}
        if (i == n) return (CssKeyword)k;
    }
    return CSS_KEYWORD_INVALID;
}

const char *css_keyword_name(CssKeyword k)
{
    DCHECKF(k > CSS_KEYWORD_INVALID && k < CSS_KEYWORD_COUNT, "keyword id %u names no keyword", (unsigned)k);
    return CSS_KEYWORD_NAMES[k];
}

/* The sets another set's grammar is composed of, each its own function so a composed set reads them rather
   than asking css_keyword_set_has again. Their productions are cited at their rows below. */
static bool kw_size(CssKeyword k)
{
    return k == CSS_KEYWORD_AUTO || k == CSS_KEYWORD_STRETCH || k == CSS_KEYWORD_MIN_CONTENT ||
           k == CSS_KEYWORD_MAX_CONTENT || k == CSS_KEYWORD_FIT_CONTENT;
}

static bool kw_absolute_size(CssKeyword k)
{
    return k == CSS_KEYWORD_XX_SMALL || k == CSS_KEYWORD_X_SMALL || k == CSS_KEYWORD_SMALL ||
           k == CSS_KEYWORD_MEDIUM || k == CSS_KEYWORD_LARGE || k == CSS_KEYWORD_X_LARGE ||
           k == CSS_KEYWORD_XX_LARGE || k == CSS_KEYWORD_XXX_LARGE;
}

static bool kw_display_outside(CssKeyword k)
{
    return k == CSS_KEYWORD_BLOCK || k == CSS_KEYWORD_INLINE || k == CSS_KEYWORD_RUN_IN;
}

static bool kw_display_inside(CssKeyword k)
{
    return k == CSS_KEYWORD_FLOW || k == CSS_KEYWORD_FLOW_ROOT || k == CSS_KEYWORD_TABLE ||
           k == CSS_KEYWORD_FLEX || k == CSS_KEYWORD_GRID || k == CSS_KEYWORD_RUBY;
}

static bool kw_display_internal(CssKeyword k)
{
    return k == CSS_KEYWORD_TABLE_ROW_GROUP || k == CSS_KEYWORD_TABLE_HEADER_GROUP ||
           k == CSS_KEYWORD_TABLE_FOOTER_GROUP || k == CSS_KEYWORD_TABLE_ROW || k == CSS_KEYWORD_TABLE_CELL ||
           k == CSS_KEYWORD_TABLE_COLUMN_GROUP || k == CSS_KEYWORD_TABLE_COLUMN ||
           k == CSS_KEYWORD_TABLE_CAPTION || k == CSS_KEYWORD_RUBY_BASE || k == CSS_KEYWORD_RUBY_TEXT ||
           k == CSS_KEYWORD_RUBY_BASE_CONTAINER || k == CSS_KEYWORD_RUBY_TEXT_CONTAINER;
}

static bool kw_display_box(CssKeyword k)
{
    return k == CSS_KEYWORD_CONTENTS || k == CSS_KEYWORD_NONE;
}

static bool kw_display_legacy(CssKeyword k)
{
    return k == CSS_KEYWORD_INLINE_BLOCK || k == CSS_KEYWORD_INLINE_TABLE || k == CSS_KEYWORD_INLINE_FLEX ||
           k == CSS_KEYWORD_INLINE_GRID;
}

static bool kw_content_position(CssKeyword k)
{
    return k == CSS_KEYWORD_CENTER || k == CSS_KEYWORD_START || k == CSS_KEYWORD_END ||
           k == CSS_KEYWORD_FLEX_START || k == CSS_KEYWORD_FLEX_END;
}

/* Each set is the keyword arm of the `Computed value:` line of the properties core/css/css_property_id.h
   gives it, read off the `Value:` line beside it. */
bool css_keyword_set_has(CssKeywordSet set, CssKeyword k)
{
    switch (set) {
    case CSS_KEYWORDS_UNTYPED:
        DFAILF("keyword %u was tested against a TEXT-shaped property's keyword set, which is not typed yet; "
               "give that row a set and its computed value a typed arm",
               (unsigned)k);
        return false;
    /* css-box-4 §4.1 "Page-relative (Physical) Padding Properties…" (`<length-percentage [0,∞]>`). */
    case CSS_KEYWORDS_NONE:
        return false;
    /* css-box-4 §3.1 "Page-relative (Physical) Margin Properties…": `<length-percentage> | auto`. */
    case CSS_KEYWORDS_MARGIN:
    /* css-position-3 §3.1 "Box Insets: the top, right, bottom, left, inset-block-start, inset-inline-start,
       inset-block-end, and inset-inline-end properties": `auto | <length-percentage>`. */
    case CSS_KEYWORDS_INSET:
        return k == CSS_KEYWORD_AUTO;
    /* css-sizing-3 §3.1.1 "Preferred Size Properties…" and §3.1.2 "Minimum Size Properties…": `auto |
       <box-size>`, where css-sizing-3 §3.2 "Sizing Values: the <length-percentage>, auto | none, stretch,
       min-content, max-content, and fit-content values" defines `<box-size> = <length-percentage [0,∞]> | stretch |
       min-content | max-content | fit-content`. */
    case CSS_KEYWORDS_SIZE:
        return kw_size(k);
    /* css-sizing-3 §3.1.3 "Maximum Size Properties…": `none | <box-size>`. */
    case CSS_KEYWORDS_MAX_SIZE:
        return k == CSS_KEYWORD_NONE || k == CSS_KEYWORD_STRETCH || k == CSS_KEYWORD_MIN_CONTENT ||
               k == CSS_KEYWORD_MAX_CONTENT || k == CSS_KEYWORD_FIT_CONTENT;
    /* css-inline-3 §4.2.3 "Post-Alignment Shift: the baseline-shift longhand": `<length-percentage> | sub |
       super | top | center | bottom`. */
    case CSS_KEYWORDS_BASELINE_SHIFT:
        return k == CSS_KEYWORD_SUB || k == CSS_KEYWORD_SUPER || k == CSS_KEYWORD_TOP ||
               k == CSS_KEYWORD_CENTER || k == CSS_KEYWORD_BOTTOM;
    /* css-flexbox-1 §7.2.3 "The flex-basis property": `content | <'width'>`. */
    case CSS_KEYWORDS_FLEX_BASIS:
        return k == CSS_KEYWORD_CONTENT || kw_size(k);
    /* css-position-3 §2 "Choosing A Positioning Scheme: position property": `static | relative | absolute |
       sticky | fixed`. */
    case CSS_KEYWORDS_POSITION:
        return k == CSS_KEYWORD_STATIC || k == CSS_KEYWORD_RELATIVE || k == CSS_KEYWORD_ABSOLUTE ||
               k == CSS_KEYWORD_STICKY || k == CSS_KEYWORD_FIXED;
    /* css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property": `content-box | border-box`. */
    case CSS_KEYWORDS_BOX_SIZING:
        return k == CSS_KEYWORD_CONTENT_BOX || k == CSS_KEYWORD_BORDER_BOX;
    /* css-page-floats-3 §4 "The clear property": `inline-start | inline-end | block-start | block-end | left |
       right | top | bottom | both-inline | both-block | both | none`. */
    case CSS_KEYWORDS_CLEAR:
        return k == CSS_KEYWORD_INLINE_START || k == CSS_KEYWORD_INLINE_END || k == CSS_KEYWORD_BLOCK_START ||
               k == CSS_KEYWORD_BLOCK_END || k == CSS_KEYWORD_LEFT || k == CSS_KEYWORD_RIGHT ||
               k == CSS_KEYWORD_TOP || k == CSS_KEYWORD_BOTTOM || k == CSS_KEYWORD_BOTH_INLINE ||
               k == CSS_KEYWORD_BOTH_BLOCK || k == CSS_KEYWORD_BOTH || k == CSS_KEYWORD_NONE;
    /* css-backgrounds-3 §3.3 "Line Thickness: the border-width properties": `<line-width> = <length [0,∞]> |
       thin | medium | thick`. */
    case CSS_KEYWORDS_LINE_WIDTH:
        return k == CSS_KEYWORD_THIN || k == CSS_KEYWORD_MEDIUM || k == CSS_KEYWORD_THICK;
    /* css-fonts-4 §2.5 "Font size: the font-size property": `<absolute-size> | <relative-size> |
       <length-percentage [0,∞]> | math`, where `<relative-size>` is `[ larger | smaller ]`. */
    case CSS_KEYWORDS_FONT_SIZE:
        return kw_absolute_size(k) || k == CSS_KEYWORD_LARGER ||
               k == CSS_KEYWORD_SMALLER || k == CSS_KEYWORD_MATH;
    /* css-fonts-4 §2.5's `<absolute-size>`: `[ xx-small | x-small | small | medium | large | x-large |
       xx-large | xxx-large ]`. */
    case CSS_KEYWORDS_ABSOLUTE_SIZE:
        return kw_absolute_size(k);
    /* css-display-3 §2 "Box Layout Modes: the display property"'s productions. `<display-outside> = block |
       inline | run-in`; `<display-inside> = flow | flow-root | table | flex | grid | ruby`; `<display-internal>
       = table-row-group | table-header-group | table-footer-group | table-row | table-cell | table-column-group
       | table-column | table-caption | ruby-base | ruby-text | ruby-base-container | ruby-text-container`;
       `<display-box> = contents | none`; `<display-legacy> = inline-block | inline-table | inline-flex |
       inline-grid`. */
    case CSS_KEYWORDS_DISPLAY_OUTSIDE:
        return kw_display_outside(k);
    case CSS_KEYWORDS_DISPLAY_INSIDE:
        return kw_display_inside(k);
    case CSS_KEYWORDS_DISPLAY_INTERNAL:
        return kw_display_internal(k);
    case CSS_KEYWORDS_DISPLAY_BOX:
        return kw_display_box(k);
    case CSS_KEYWORDS_DISPLAY_LEGACY:
        return kw_display_legacy(k);
    case CSS_KEYWORDS_DISPLAY:
        return k == CSS_KEYWORD_LIST_ITEM || kw_display_outside(k) ||
               kw_display_inside(k) ||
               kw_display_internal(k) ||
               kw_display_box(k) || kw_display_legacy(k);
    /* css-flexbox-1 §5.1 "Flex Flow Direction: the flex-direction property": `row | row-reverse | column |
       column-reverse`. */
    case CSS_KEYWORDS_FLEX_DIRECTION:
        return k == CSS_KEYWORD_ROW || k == CSS_KEYWORD_ROW_REVERSE || k == CSS_KEYWORD_COLUMN ||
               k == CSS_KEYWORD_COLUMN_REVERSE;
    /* css-flexbox-1 §5.2 "Flex Line Wrapping: the flex-wrap property": `nowrap | wrap | wrap-reverse`. */
    case CSS_KEYWORDS_FLEX_WRAP:
        return k == CSS_KEYWORD_NOWRAP || k == CSS_KEYWORD_WRAP || k == CSS_KEYWORD_WRAP_REVERSE;
    /* css-writing-modes-4 §2.1 "Specifying Directionality: the direction property": `ltr | rtl`. */
    case CSS_KEYWORDS_DIRECTION:
        return k == CSS_KEYWORD_LTR || k == CSS_KEYWORD_RTL;
    /* css-writing-modes-4 §3.2 "Block Flow Direction: the writing-mode property": `horizontal-tb | vertical-rl
       | vertical-lr | sideways-rl | sideways-lr`. */
    case CSS_KEYWORDS_WRITING_MODE:
        return k == CSS_KEYWORD_HORIZONTAL_TB || k == CSS_KEYWORD_VERTICAL_RL || k == CSS_KEYWORD_VERTICAL_LR ||
               k == CSS_KEYWORD_SIDEWAYS_RL || k == CSS_KEYWORD_SIDEWAYS_LR;
    /* css-tables-3 §3.5.1 "The Table-Layout property": `auto | fixed`. */
    case CSS_KEYWORDS_TABLE_LAYOUT:
        return k == CSS_KEYWORD_AUTO || k == CSS_KEYWORD_FIXED;
    /* css-tables-3 §3.5.2 "The Border-Collapse property": `separate | collapse`. */
    case CSS_KEYWORDS_BORDER_COLLAPSE:
        return k == CSS_KEYWORD_SEPARATE || k == CSS_KEYWORD_COLLAPSE;
    /* css-tables-3 §3.5.3 "The Caption-Side property": `top | bottom`. */
    case CSS_KEYWORDS_CAPTION_SIDE:
        return k == CSS_KEYWORD_TOP || k == CSS_KEYWORD_BOTTOM;
    /* css-display-3 §4 "Invisibility: the visibility property": `visible | hidden | collapse`. */
    case CSS_KEYWORDS_VISIBILITY:
        return k == CSS_KEYWORD_VISIBLE || k == CSS_KEYWORD_HIDDEN || k == CSS_KEYWORD_COLLAPSE;
    /* css-backgrounds-3 §3.2 "Line Patterns: the border-style properties": `<line-style> = none | hidden |
       dotted | dashed | solid | double | groove | ridge | inset | outset`. */
    case CSS_KEYWORDS_LINE_STYLE:
        return k == CSS_KEYWORD_NONE || k == CSS_KEYWORD_HIDDEN || k == CSS_KEYWORD_DOTTED ||
               k == CSS_KEYWORD_DASHED || k == CSS_KEYWORD_SOLID || k == CSS_KEYWORD_DOUBLE ||
               k == CSS_KEYWORD_GROOVE || k == CSS_KEYWORD_RIDGE || k == CSS_KEYWORD_INSET ||
               k == CSS_KEYWORD_OUTSET;
    /* css-ui-4 §6.2 "Exclusion from Hit-testing: the pointer-events property": `auto | none`. */
    case CSS_KEYWORDS_POINTER_EVENTS:
        return k == CSS_KEYWORD_AUTO || k == CSS_KEYWORD_NONE;
    /* css-lists-3 §3.5 "Positioning Markers: The list-style-position property": `inside | outside`. */
    case CSS_KEYWORDS_LIST_STYLE_POSITION:
        return k == CSS_KEYWORD_INSIDE || k == CSS_KEYWORD_OUTSIDE;
    /* css-inline-3 §4.2.1 "Alignment Baseline Source: the baseline-source longhand": `auto | first | last`. */
    case CSS_KEYWORDS_BASELINE_SOURCE:
        return k == CSS_KEYWORD_AUTO || k == CSS_KEYWORD_FIRST || k == CSS_KEYWORD_LAST;
    /* css-fonts-4 §6.3 "Kerning: the font-kerning property": `auto | normal | none`. */
    case CSS_KEYWORDS_FONT_KERNING:
        return k == CSS_KEYWORD_AUTO || k == CSS_KEYWORD_NORMAL || k == CSS_KEYWORD_NONE;
    /* css-inline-3 §4.2.2 "Alignment Baseline Type: the alignment-baseline longhand": `baseline |
       <baseline-metric>`, `<baseline-metric> = text-bottom | alphabetic | ideographic | middle | central |
       mathematical | hanging | text-top`. NAMED RESIDUAL: lexbor's longhand parser has no `hanging`, so a
       declared `alignment-baseline: hanging` is dropped; the next diff is that keyword in lexbor's state
       machine; its absence shows as such a declaration computing to the cascade's next value. */
    case CSS_KEYWORDS_ALIGNMENT_BASELINE:
        return k == CSS_KEYWORD_BASELINE || k == CSS_KEYWORD_TEXT_BOTTOM || k == CSS_KEYWORD_ALPHABETIC ||
               k == CSS_KEYWORD_IDEOGRAPHIC || k == CSS_KEYWORD_MIDDLE || k == CSS_KEYWORD_CENTRAL ||
               k == CSS_KEYWORD_MATHEMATICAL || k == CSS_KEYWORD_HANGING || k == CSS_KEYWORD_TEXT_TOP;
    /* css-overflow-3 §3.1 "Managing Overflow: the overflow-x, overflow-y, and overflow properties": `visible |
       hidden | clip | scroll | auto`. `overlay` is a legacy value alias of `auto`, converted at parse time
       (css-cascade-5 §4.1.1 "Value Aliasing"), so it is in no computed value and in no set. */
    case CSS_KEYWORDS_OVERFLOW:
        return k == CSS_KEYWORD_VISIBLE || k == CSS_KEYWORD_HIDDEN || k == CSS_KEYWORD_CLIP ||
               k == CSS_KEYWORD_SCROLL || k == CSS_KEYWORD_AUTO;
    /* css-align-3 §4.1 "Positional Alignment: the center, start, end, self-start, self-end, flex-start,
       flex-end, left, and right keywords": `<self-position> = center | start | end | self-start | self-end |
       flex-start | flex-end` and `<content-position> = center | start | end | flex-start | flex-end`. They are
       productions, no row's set; core/css/css_align.c composes them into the six properties' grammars. */
    case CSS_KEYWORDS_SELF_POSITION:
        return kw_content_position(k) || k == CSS_KEYWORD_SELF_START ||
               k == CSS_KEYWORD_SELF_END;
    case CSS_KEYWORDS_CONTENT_POSITION:
        return kw_content_position(k);
    /* css-align-3 §4.3 "Distributed Alignment: the stretch, space-between, space-around, and space-evenly
       keywords": `<content-distribution> = space-between | space-around | space-evenly | stretch`. */
    case CSS_KEYWORDS_CONTENT_DISTRIBUTION:
        return k == CSS_KEYWORD_SPACE_BETWEEN || k == CSS_KEYWORD_SPACE_AROUND || k == CSS_KEYWORD_SPACE_EVENLY ||
               k == CSS_KEYWORD_STRETCH;
    /* css-align-3 §4.4 "Overflow Alignment: the safe and unsafe keywords and scroll safety limits":
       `<overflow-position> = unsafe | safe`. */
    case CSS_KEYWORDS_OVERFLOW_POSITION:
        return k == CSS_KEYWORD_UNSAFE || k == CSS_KEYWORD_SAFE;
    }
    DFAILF("keyword set %u is none of the sets core/css/css_keyword.h declares", (unsigned)set);
    return false;
}
