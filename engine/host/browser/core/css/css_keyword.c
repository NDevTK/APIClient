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
    /* css-box-4 §4.1 "Page-relative (Physical) Padding Properties…" (`<length-percentage [0,∞]>`), and
       every length whose own arm consumes its keywords before the parse: css-backgrounds-3 §3.3's
       `<line-width>` and css-fonts-4 §2.5's sizes compute to "an absolute length". */
    case CSS_KEYWORDS_NONE:
        return false;
    /* css-box-4 §3.1 "Page-relative (Physical) Margin Properties…": `<length-percentage> | auto`. */
    case CSS_KEYWORDS_MARGIN:
    /* css-position-3 §3.1 "Box Insets: the top, right, bottom, left, inset-block-start, inset-inline-start,
       inset-block-end, and inset-inline-end properties": `auto | <length-percentage>`. */
    case CSS_KEYWORDS_INSET:
        return k == CSS_KEYWORD_AUTO;
    /* css-sizing-3 §3.1.1 "Preferred Size Properties…" and §3.1.2 "Minimum Size Properties…": `auto |
       <box-size>`, where §3.2 "Sizing Values: the <length-percentage>, auto | none, stretch, min-content,
       max-content, and fit-content values" defines `<box-size> = <length-percentage [0,∞]> | stretch |
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
    }
    DFAILF("keyword set %u is none of the sets core/css/css_keyword.h declares", (unsigned)set);
    return false;
}
