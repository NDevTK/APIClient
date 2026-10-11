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
        return k == CSS_KEYWORD_AUTO || k == CSS_KEYWORD_STRETCH || k == CSS_KEYWORD_MIN_CONTENT ||
               k == CSS_KEYWORD_MAX_CONTENT || k == CSS_KEYWORD_FIT_CONTENT;
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
        return k == CSS_KEYWORD_CONTENT || css_keyword_set_has(CSS_KEYWORDS_SIZE, k);
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
        return css_keyword_set_has(CSS_KEYWORDS_ABSOLUTE_SIZE, k) || k == CSS_KEYWORD_LARGER ||
               k == CSS_KEYWORD_SMALLER || k == CSS_KEYWORD_MATH;
    /* css-fonts-4 §2.5's `<absolute-size>`: `[ xx-small | x-small | small | medium | large | x-large |
       xx-large | xxx-large ]`. */
    case CSS_KEYWORDS_ABSOLUTE_SIZE:
        return k == CSS_KEYWORD_XX_SMALL || k == CSS_KEYWORD_X_SMALL || k == CSS_KEYWORD_SMALL ||
               k == CSS_KEYWORD_MEDIUM || k == CSS_KEYWORD_LARGE || k == CSS_KEYWORD_X_LARGE ||
               k == CSS_KEYWORD_XX_LARGE || k == CSS_KEYWORD_XXX_LARGE;
    /* css-display-3 §2 "Box Layout Modes: the display property"'s productions. `<display-outside> = block |
       inline | run-in`; `<display-inside> = flow | flow-root | table | flex | grid | ruby`; `<display-internal>
       = table-row-group | table-header-group | table-footer-group | table-row | table-cell | table-column-group
       | table-column | table-caption | ruby-base | ruby-text | ruby-base-container | ruby-text-container`;
       `<display-box> = contents | none`; `<display-legacy> = inline-block | inline-table | inline-flex |
       inline-grid`. */
    case CSS_KEYWORDS_DISPLAY_OUTSIDE:
        return k == CSS_KEYWORD_BLOCK || k == CSS_KEYWORD_INLINE || k == CSS_KEYWORD_RUN_IN;
    case CSS_KEYWORDS_DISPLAY_INSIDE:
        return k == CSS_KEYWORD_FLOW || k == CSS_KEYWORD_FLOW_ROOT || k == CSS_KEYWORD_TABLE ||
               k == CSS_KEYWORD_FLEX || k == CSS_KEYWORD_GRID || k == CSS_KEYWORD_RUBY;
    case CSS_KEYWORDS_DISPLAY_INTERNAL:
        return k == CSS_KEYWORD_TABLE_ROW_GROUP || k == CSS_KEYWORD_TABLE_HEADER_GROUP ||
               k == CSS_KEYWORD_TABLE_FOOTER_GROUP || k == CSS_KEYWORD_TABLE_ROW || k == CSS_KEYWORD_TABLE_CELL ||
               k == CSS_KEYWORD_TABLE_COLUMN_GROUP || k == CSS_KEYWORD_TABLE_COLUMN ||
               k == CSS_KEYWORD_TABLE_CAPTION || k == CSS_KEYWORD_RUBY_BASE || k == CSS_KEYWORD_RUBY_TEXT ||
               k == CSS_KEYWORD_RUBY_BASE_CONTAINER || k == CSS_KEYWORD_RUBY_TEXT_CONTAINER;
    case CSS_KEYWORDS_DISPLAY_BOX:
        return k == CSS_KEYWORD_CONTENTS || k == CSS_KEYWORD_NONE;
    case CSS_KEYWORDS_DISPLAY_LEGACY:
        return k == CSS_KEYWORD_INLINE_BLOCK || k == CSS_KEYWORD_INLINE_TABLE || k == CSS_KEYWORD_INLINE_FLEX ||
               k == CSS_KEYWORD_INLINE_GRID;
    case CSS_KEYWORDS_DISPLAY:
        return k == CSS_KEYWORD_LIST_ITEM || css_keyword_set_has(CSS_KEYWORDS_DISPLAY_OUTSIDE, k) ||
               css_keyword_set_has(CSS_KEYWORDS_DISPLAY_INSIDE, k) ||
               css_keyword_set_has(CSS_KEYWORDS_DISPLAY_INTERNAL, k) ||
               css_keyword_set_has(CSS_KEYWORDS_DISPLAY_BOX, k) || css_keyword_set_has(CSS_KEYWORDS_DISPLAY_LEGACY, k);
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
    }
    DFAILF("keyword set %u is none of the sets core/css/css_keyword.h declares", (unsigned)set);
    return false;
}
