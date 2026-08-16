/* CSSOM §9's FIRST CONJUNCT — every property definition's "Applies to:" line. See css_property_applies.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_property_applies.h"

static bool css_pa_in(const char *const *set, unsigned n, const char *v)
{
    unsigned i;

    if (v == NULL) return false;
    for (i = 0; i < n; i++)
        if (strcmp(set[i], v) == 0) return true;
    return false;
}

/* css-display Appendix B's own list of HTML elements that "aren't rendered purely by CSS box concepts", copied
   from the spec rather than re-derived: it is the list `display: contents` has to say something extra about,
   and it is the superset every candidate for CSS 2.1 §3.1's "replaced element" is drawn from. The header says
   why membership is not the same question and why an inline member crashes instead of being classified. */
static const char *const CSS_UNUSUAL_ELEMENTS[] = {
    "audio", "br", "canvas", "embed", "frame", "frameset", "iframe", "img", "input", "meter",
    "object", "progress", "select", "textarea", "video", "wbr",
};

bool css_element_may_be_replaced(lxb_dom_element_t *el)
{
    size_t n = 0;
    const lxb_char_t *tag;
    unsigned i;

    DCHECK(el != NULL, "the replaced-element question was asked about no element");
    tag = lxb_dom_element_local_name(el, &n);
    DCHECK(tag != NULL, "an element with no local name — lexbor gives every element one");
    for (i = 0; i < sizeof(CSS_UNUSUAL_ELEMENTS) / sizeof(CSS_UNUSUAL_ELEMENTS[0]); i++)
        if (strlen(CSS_UNUSUAL_ELEMENTS[i]) == n && memcmp(CSS_UNUSUAL_ELEMENTS[i], tag, n) == 0)
            return true;
    return false;
}

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

bool css_property_applies(lxb_dom_element_t *el, const char *name)
{
    char *display;
    bool is_size, is_margin, is_padding, applies;

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
    if (!is_size && !is_margin && !is_padding) {
        DFAIL("CSSOM §9 asked whether a property APPLIES to an element, and this component has not recorded "
              "that property's `Applies to:` line. The three groups it carries are the physical box-model "
              "lengths, the physical insets and the two sizes; what §9's own table also routes here and this "
              "does not answer is (1) the LOGICAL box-model properties — `inline-size`, `block-size`, "
              "`margin-block-start`, `padding-inline-end` and the rest — whose Applies-to lines are their "
              "physical twins' but which need css-writing-modes §6's mapping from the element's computed "
              "`writing-mode` and `direction` to say WHICH twin, a mapping this engine does not have and "
              "lexbor's property registry carries no logical longhand for; and (2) `transform-origin`, whose "
              "line is `transformable elements` (css-transforms §3), a definition over the element's box type "
              "and its `transform-box`. RECORD the line beside the others, from the property's own spec");
        return false;
    }
    display = css_computed_value(el, "display");
    DCHECK(display != NULL, "the cascade produced no computed `display` — the UA layer answers `inline` for "
                            "every element it does not name, so this cannot be unset");
    /* "…but NON-REPLACED INLINE elements". An inline box is css-display's `inline` outer type with a `flow`
       inner one, which is the computed `display` of `inline` and NOT `inline-block` (that is a
       `<display-legacy>` for `inline flow-root`, and CSS 2.1 §10.3.9 gives it a used width of its own). */
    if (is_size && strcmp(display, "inline") == 0) {
        if (css_element_may_be_replaced(el)) {
            free(display);
            DFAIL("CSS 2.1 §10.2 and §10.5 exclude NON-REPLACED inline elements from `width` and `height`, and "
                  "this element generates an inline box AND is one css-display Appendix B lists as not "
                  "rendered purely by CSS box concepts. The two answers differ and both are reachable: as "
                  "NON-replaced the property does not apply and CSSOM §9 resolves it to the computed value "
                  "(`auto`), while as REPLACED it applies and §10.3.2 derives the used width from an INTRINSIC "
                  "WIDTH — the decoded image's, the embedded document's — which this engine has no decoder and "
                  "no child navigable layout to ask for. BUILD the replaced-element predicate from HTML's own "
                  "rendering rules (an `img` is replaced only WHEN IT REPRESENTS AN IMAGE, which is a fact "
                  "about the load state and not about the tag), then §10.3.2's intrinsic-dimension arms, whose "
                  "default when there is no intrinsic size the spec itself states as 300 x 150 — the same "
                  "number core/frame/viewport.c already derives a child navigable's viewport from");
            return false;
        }
        free(display);
        return false;   /* a non-replaced inline element: §9 resolves `width`/`height` to the computed value */
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
