/* CSS Paged Media Level 3's membership tables. See css_page.h for why the lists are closed and why they are
 * one component. Every name below is copied from the spec's own text and grouped the way the spec groups it,
 * so the two can be read side by side. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "core/css/css_page.h"
#include "core/css/css_shorthand.h"

/* §4.3's margin at-rule productions, in the spec's own order — the sixteen page-margin boxes §5 defines. */
static const char *const MARGIN_AT_RULES[] = {
    "top-left-corner", "top-left", "top-center", "top-right", "top-right-corner",
    "bottom-left-corner", "bottom-left", "bottom-center", "bottom-right", "bottom-right-corner",
    "left-top", "left-middle", "left-bottom",
    "right-top", "right-middle", "right-bottom",
};

/* Appendix A's two lists share almost all of their names, so what is shared is written ONCE. The spec prints
   them as two full lists; writing them that way here would be two copies of sixty names that a single typo
   could make disagree, and the disagreement would be one property silently missing from one context. */
static const char *const PAGE_AND_MARGIN_PROPERTIES[] = {
    /* bidi properties */
    "direction",
    /* background properties */
    "background-color", "background-image", "background-repeat", "background-attachment",
    "background-position", "background",
    /* border properties */
    "border-top-width", "border-right-width", "border-bottom-width", "border-left-width", "border-width",
    "border-top-color", "border-right-color", "border-bottom-color", "border-left-color", "border-color",
    "border-top-style", "border-right-style", "border-bottom-style", "border-left-style", "border-style",
    "border-top", "border-right", "border-bottom", "border-left", "border",
    /* counter properties */
    "counter-reset", "counter-increment",
    "color",
    /* font properties */
    "font-family", "font-size", "font-style", "font-variant", "font-weight", "font",
    /* height properties */
    "height", "min-height", "max-height", "line-height",
    /* margin properties */
    "margin-top", "margin-right", "margin-bottom", "margin-left", "margin",
    /* outline properties */
    "outline-width", "outline-style", "outline-color", "outline",
    /* padding properties */
    "padding-top", "padding-right", "padding-bottom", "padding-left", "padding",
    "quotes",
    /* text properties */
    "letter-spacing", "text-align", "text-decoration", "text-indent", "text-transform", "white-space",
    "word-spacing",
    "visibility",
    /* width properties */
    "width", "min-width", "max-width",
};

/* THE PAGE CONTEXT'S OWN, and every one of them is a css-page-3 DESCRIPTOR rather than a CSS 2.1 property —
   which is why §6 introduces them separately ("this specification additionally defines the size property that
   only applies in the page context") and why CSSOM §6.4.7's CSSPageDescriptors lists them beside the margins.
   `size` is §7's, `page-orientation` §7.1.2's, `marks` and `bleed` §9's; each is defined `For: @page`. */
static const char *const PAGE_ONLY_PROPERTIES[] = {
    "size", "page-orientation", "marks", "bleed",
};

/* THE MARGIN CONTEXT'S OWN — the five names Appendix A's second list carries and its first does not. Each is a
   property of a BOX that holds generated content, which the page box is not: §5's margin boxes are the ones
   with `content`, a stacking position (`z-index`), an alignment inside their box (`vertical-align`) and an
   overflow, and `unicode-bidi` goes with the inline content they can hold. */
static const char *const MARGIN_ONLY_PROPERTIES[] = {
    "unicode-bidi", "content", "overflow", "vertical-align", "z-index",
};

#define IN(table, name) css_page_in(table, sizeof(table) / sizeof((table)[0]), name)

static bool css_page_in(const char *const *table, size_t n, const char *name)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (strcmp(table[i], name) == 0) return true;
    return false;
}

bool css_page_margin_at_rule(const char *name)
{
    DCHECK(name != NULL, "an at-rule was asked whether it is a §4.3 margin at-rule without giving its name");
    return IN(MARGIN_AT_RULES, name);
}

/* The lists themselves, asked of one name. */
static bool css_page_named(CssPageContext context, const char *name)
{
    if (IN(PAGE_AND_MARGIN_PROPERTIES, name)) return true;
    if (context == CSS_PAGE_CONTEXT_MARGIN) return IN(MARGIN_ONLY_PROPERTIES, name);
    DCHECK(context == CSS_PAGE_CONTEXT_PAGE,
           "a property was asked about a page context that is neither §4.3's page context nor its margin "
           "context — those are the two the spec states lists for, and a third would have no list to read");
    return IN(PAGE_ONLY_PROPERTIES, name);
}

bool css_page_property_applies(CssPageContext context, const char *name)
{
    const char *of[CSS_SHORTHAND_MAX_OF];
    unsigned n, i;

    DCHECK(name != NULL, "a page context was asked about a property with no name");
    DCHECK(!(name[0] == '-' && name[1] == '-'),
           "a CUSTOM PROPERTY was asked whether it applies in a page context. Appendix A is a list of CSS 2.1 "
           "properties and a custom property is not one, so this table can only ever answer false for it — "
           "which would drop a declaration CSS Variables §2 makes valid wherever a declaration is. The caller "
           "decides that before asking");
    if (css_page_named(context, name)) return true;
    /* A LONGHAND OF AN ADMITTED SHORTHAND IS ADMITTED WITH IT, and this is not a convenience — it is the only
       way the lists can be read against a CSSOM §6.6 declaration block at all. Appendix A names CSS 2.1
       PROPERTIES, and a CSS 2.1 shorthand's longhand set has grown since: `border` now also resets
       css-backgrounds-3's five `border-image-*` longhands, and `overflow` is two axes. A block holds LONGHANDS
       — "an ordered collection of CSS properties", one per property, which is what the expansion produces —
       so admitting `border` while refusing what `border` SETS would delete most of a declaration the spec
       admits, and delete it invisibly, because a block's text is round-tripped through §6.6's serialization
       between the parse and this question. The set is READ from core/css/css_shorthand.h's own table rather
       than restated here, so a shorthand that grows a longhand cannot leave a second list behind. */
    n = css_shorthand_shorthands_of(name, of, CSS_SHORTHAND_MAX_OF);
    for (i = 0; i < n; i++)
        if (css_page_named(context, of[i])) return true;
    return false;
}
