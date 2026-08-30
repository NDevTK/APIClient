/* CSSOM §9's FIRST CONJUNCT — every property definition's "Applies to:" line. See css_property_applies.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_property_applies.h"
#include "core/layout/replaced_element.h"

static bool css_pa_in(const char *const *set, unsigned n, const char *v)
{
    unsigned i;

    if (v == NULL) return false;
    for (i = 0; i < n; i++)
        if (strcmp(set[i], v) == 0) return true;
    return false;
}

/* WHAT STOOD HERE WAS css-display Appendix B's list of HTML elements that "aren't rendered purely by CSS box
   concepts", tested as a stand-in for CSS 2.1 §3.1's "replaced element" and CRASHING for an inline member
   rather than answering. Both halves are gone: the question is HTML §15.4 "Replaced elements"' and it is
   answered by core/layout/replaced_element.h, which reads the element's actual state rather than its tag. The
   list is not kept beside it — a superset predicate is exactly the fallback a caller reaches for when the real
   one crashes, and it disagreed with §15.4 in both directions (it carried `br`, `wbr`, `meter`, `progress`,
   `select` and `textarea`, which §15.4's own sentence does not list; and it called every `img` replaced, which
   §15.4.2's third rule contradicts for a broken image with an `alt`). */

/* css-display §2.4's `<display-internal>` TABLE keywords, which is the set every "Applies to:" line below
   names a subset of. `table` and `inline-table` are NOT among them — they are a `<display-inside>` and a
   `<display-legacy>` — which is exactly the distinction §8.3's line turns on. */
static const char *const CSS_TABLE_INTERNAL[] = {
    "table-row-group", "table-header-group", "table-footer-group", "table-row", "table-cell",
    "table-column-group", "table-column", "table-caption",
};

#define CSS_PA_N(a) (sizeof(a) / sizeof((a)[0]))

/* §10.2's "table rows, and row groups", and §10.5's "table columns, and column groups" — the two halves of the
   table exclusion that differ between `width` and `height`, and they differ because a row's width and a
   column's height are the TABLE's to decide (CSS 2.1 §17.5). */
static const char *const CSS_TABLE_ROWS[] = {
    "table-row", "table-row-group", "table-header-group", "table-footer-group",
};
static const char *const CSS_TABLE_COLUMNS[] = {
    "table-column", "table-column-group",
};

/* §8.4's exclusion, verbatim: every internal table box EXCEPT the cell and the caption, both of which take
   padding. */
static const char *const CSS_PADDING_EXCLUDED[] = {
    "table-row-group", "table-header-group", "table-footer-group", "table-row",
    "table-column-group", "table-column",
};

static const char *const CSS_SIZE_PROPERTIES[] = { "width", "height" };
static const char *const CSS_MARGIN_PROPERTIES[] = {
    "margin-top", "margin-right", "margin-bottom", "margin-left",
};
static const char *const CSS_PADDING_PROPERTIES[] = {
    "padding-top", "padding-right", "padding-bottom", "padding-left",
};
static const char *const CSS_INSET_PROPERTIES[] = { "top", "right", "bottom", "left" };

/* css-overflow-3 §3.1 "Managing Overflow: the overflow-x, overflow-y, and overflow properties"' line, which is
 * an INCLUSION where every line above is an exclusion: "block containers [CSS2], flex containers
 * [CSS-FLEXBOX-1], grid containers [CSS-GRID-1], and table grid boxes [CSS-TABLES-3]". It is written as the
 * computed `display` keywords that name those four box types, because that is the only fact an Applies-to line
 * ever turns on (see the header) and because each of the four is named by a `display` value in its own spec:
 *   BLOCK CONTAINERS — css-display-3 §2.2 "Inner Display Layout Models: the flow, flow-root, table, flex, grid,
 *     and ruby keywords" is where the term is attached to a value: `flow` "generates a block container box"
 *     unless its outer type makes it an inline box, and `flow-root` "generates a block container box" always.
 *     So the keywords are `block` and `flow-root`, `inline-block` (the `<display-legacy>` spelling of `inline
 *     flow-root`, an INLINE-LEVEL block container and so still one), `list-item` (css-display-3 §2.3
 *     "Generating Marker Boxes: the list-item keyword", whose inner type is flow), and the two internal table
 *     boxes css-display-3 §2.4 "Layout-Internal Display Types: the table-* and ruby-* keywords" gives an inner
 *     type to by name — "table-cell boxes have a flow-root inner display type" and the same sentence for
 *     `table-caption`. Every other `<display-internal>` keyword has none, which is why `table-row` is absent.
 *   FLEX CONTAINERS — css-flexbox-1 §3 "Flex Containers: the flex and inline-flex display values".
 *   GRID CONTAINERS — css-grid-2 §5.1 "Establishing Grid Containers: the grid and inline-grid display values".
 *   TABLE GRID BOXES — css-tables-3 §2.1 "Table Structure": "table grid box: A block-level box containing the
 *     table-internal boxes, excluding its captions", which `table` and `inline-table` generate.
 * `inline` is the keyword this line's absence is actually about: `overflow: auto` on a `<span>` is a
 * declaration the cascade computes and the property does not apply to, so the span establishes no scroll
 * container (core/layout/scroll_container.h) — the one answer a reader of the computed value alone gets wrong. */
static const char *const CSS_OVERFLOW_APPLIES[] = {
    "block", "inline-block", "flow-root", "list-item", "table-cell", "table-caption",
    "flex", "inline-flex", "grid", "inline-grid", "table", "inline-table",
};
static const char *const CSS_OVERFLOW_PROPERTIES[] = { "overflow-x", "overflow-y" };

bool css_property_applies(lxb_dom_element_t *el, const char *name)
{
    char *display;
    bool is_size, is_margin, is_padding, is_overflow, applies;

    DCHECK(el != NULL && name != NULL,
           "an Applies-to line was asked for with no element or no property name");
    /* §9.3.2: "Applies to: positioned elements". The whole line, and it names no display type, so it is
       answered before `display` is read. */
    if (css_pa_in(CSS_INSET_PROPERTIES, CSS_PA_N(CSS_INSET_PROPERTIES), name)) {
        char *pos = css_computed_value(el, "position");
        bool positioned = pos != NULL && strcmp(pos, "static") != 0;

        DCHECK(pos != NULL, "the cascade produced no computed `position` — css-position §2 gives it an initial "
                            "value of `static`, so the cascade's last layer always answers");
        free(pos);
        return positioned;
    }
    is_size = css_pa_in(CSS_SIZE_PROPERTIES, CSS_PA_N(CSS_SIZE_PROPERTIES), name);
    is_margin = css_pa_in(CSS_MARGIN_PROPERTIES, CSS_PA_N(CSS_MARGIN_PROPERTIES), name);
    is_padding = css_pa_in(CSS_PADDING_PROPERTIES, CSS_PA_N(CSS_PADDING_PROPERTIES), name);
    is_overflow = css_pa_in(CSS_OVERFLOW_PROPERTIES, CSS_PA_N(CSS_OVERFLOW_PROPERTIES), name);
    if (!is_size && !is_margin && !is_padding && !is_overflow) {
        DFAIL("A caller asked whether a property APPLIES to an element, and this component has not recorded "
              "that property's `Applies to:` line. The four groups it carries are the physical box-model "
              "lengths, the physical insets, the two sizes and the two overflow longhands; what CSSOM §9's own "
              "table also routes here and this "
              "does not answer is (1) the LOGICAL box-model properties — `inline-size`, `block-size`, "
              "`margin-block-start`, `padding-inline-end` and the rest — whose Applies-to lines are their "
              "physical twins' but which need css-writing-modes §6's mapping from the element's computed "
              "`writing-mode` and `direction` to say WHICH twin, a mapping this engine does not have and "
              "lexbor's property registry carries no logical longhand for; and (2) `transform-origin`, whose "
              "line is `transformable elements` (css-transforms-1 §4 \"The transform-origin Property\"). THE "
              "DEFINITION OF THAT TERM IS NO LONGER MISSING and this line used to imply it was: "
              "core/css/css_transform.h states css-transforms-1 §2 \"Terminology\"'s transformable element out "
              "of the computed `display` and HTML §15.4's replaced-ness, exactly as the lines below are "
              "stated. What recording the row here would then reach is §9's OTHER conjunct — a USED value for "
              "`transform-origin`, which core/layout/used_value.h does not carry (it is not one of the ten "
              "physical box-model lengths) and which css-transforms-1 §5 \"Transform reference box\" resolves "
              "a percentage against. BUILD that used value, then record the line beside the others");
        return false;
    }
    display = css_computed_value(el, "display");
    DCHECK(display != NULL, "the cascade produced no computed `display` — the UA layer answers `inline` for "
                            "every element it does not name, so this cannot be unset");
    /* "…but NON-REPLACED INLINE elements". An inline box is css-display's `inline` outer type with a `flow`
       inner one, which is the computed `display` of `inline` and NOT `inline-block` (that is a
       `<display-legacy>` for `inline flow-root`, and CSS 2.1 §10.3.9 gives it a used width of its own). */
    if (is_size && strcmp(display, "inline") == 0) {
        /* THE EXCLUSION IS "NON-REPLACED inline elements", so the word `non-replaced` is a second conjunct and
           this is where it is asked. It used to CRASH here for every element css-display Appendix B listed,
           on the ground that the answer needed an intrinsic width no component could supply; both halves have
           been built. HTML §15.4 "Replaced elements" is what decides which element is replaced right now —
           core/layout/replaced_element.h — and CSS 2.1 §10.3.2's arms are what turn that into a used width,
           including its own 300 x 150 default for an object with no natural dimensions at all.
           THE TWO ANSWERS STILL DIFFER AND BOTH ARE COMMON, which is why this is a branch and not a `true`: an
           inline `<img>` whose image is loading resolves `width` to a used value, and the same `<img>` after
           the request breaks with an `alt` present is §15.4.2's THIRD rule — a non-replaced phrasing element —
           whose `width` does not apply and whose resolved value CSSOM §9 makes the computed `auto`. */
        bool replaced = replaced_element_of(el).replaced;

        free(display);
        return replaced;
    }
    /* css-overflow-3 §3.1's line is an INCLUSION, so it is tested before the exclusion lines below and not
       written as their complement — `inline`, `table-row` and every other keyword outside the set is excluded
       by the line saying nothing about it rather than by a second list naming it. */
    if (is_overflow) {
        applies = css_pa_in(CSS_OVERFLOW_APPLIES, CSS_PA_N(CSS_OVERFLOW_APPLIES), display);
        free(display);
        return applies;
    }
    if (is_size) {
        bool horizontal = strcmp(name, "width") == 0;

        applies = !css_pa_in(horizontal ? CSS_TABLE_ROWS : CSS_TABLE_COLUMNS,
                             horizontal ? CSS_PA_N(CSS_TABLE_ROWS) : CSS_PA_N(CSS_TABLE_COLUMNS), display);
    } else if (is_margin) {
        /* §8.3: "all elements except elements with table display types other than table-caption, table and
           inline-table". `table-caption` is an internal table box that DOES take margins, so it is subtracted
           back out of the internal set rather than left out of it — the two lists are the spec's and disagree
           on exactly that one keyword and on `table-cell`. */
        applies = !css_pa_in(CSS_TABLE_INTERNAL, CSS_PA_N(CSS_TABLE_INTERNAL), display) ||
                  strcmp(display, "table-caption") == 0;
    } else {
        applies = !css_pa_in(CSS_PADDING_EXCLUDED, CSS_PA_N(CSS_PADDING_EXCLUDED), display);
    }
    free(display);
    return applies;
}
