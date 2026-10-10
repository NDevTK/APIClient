/* CSS 2.1 §17.5 Visual layout of table contents' rules 3 and 4 over one table's child list. See
   table_column_box.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/html/integer_microsyntax.h"
#include "core/layout/box_subject.h"
#include "core/layout/box_tree.h"
#include "core/layout/table_box.h"
#include "core/layout/table_column_box.h"

/* HTML §4.9.12.1 Forming a table's clamp, identical in both column arms: "If span is greater than 1000, let
   it be 1000 instead." It is the section's own step, not an engine bound. */
#define TCB_SPAN_MAX ((size_t) 1000)

/* The one place this file reads a computed `display`. The four walks below iterate core/layout/box_tree.h's
   sequence, which applies css-display-3 §2.5 "Box Generation: the none and contents keywords"' splice, so a
   `contents` child never reaches here; the assert holds that contract. A local `->next` walk that skipped a
   `contents` child would lose the column boxes inside it ("its children and pseudo-elements still generate
   boxes and text sequences as normal") and undercount `noccupied`. The check is here once rather than at each
   walk because both passes must take the identical branch on the identical child list. */
static TableBoxKind tcb_kind_of(lxb_dom_element_t *el)
{
    char *display = css_computed_value(el, "display");
    TableBoxKind kind;
    char nbuf[160];

    DCHECK(display != NULL, "the cascade produced no computed `display` — the UA layer answers `inline` for "
                            "every element it does not name, so this cannot be unset");
    DCHECKF(strcmp(display, "contents") != 0,
            "%s: css-display-3 §2.5 \"Box Generation: the none and contents keywords\" replaces this child by "
            "its contents, and core/layout/box_tree.h's sequence therefore never yields one — so this "
            "classification cannot be handed one unless a walk in this file stepped a DOM child list instead of "
            "that sequence. The four walks below are the whole population; find the one that did",
            box_subject(el, nbuf, sizeof nbuf));
    kind = table_box_kind(display);
    free(display);
    return kind;
}

/* An HTML element's local name, ASCII-lowercase already for an HTML-namespace element. */
static bool tcb_html_local_name_is(lxb_dom_element_t *el, const char *name)
{
    size_t len = 0;
    const lxb_char_t *ln;

    if (el == NULL || lxb_dom_interface_node(el)->ns != LXB_NS_HTML) return false;
    ln = lxb_dom_element_local_name(el, &len);
    return ln != NULL && strlen(name) == len && memcmp(ln, name, len) == 0;
}

/* How many grid columns one column or column-group box occupies: HTML §4.9.12.1 Forming a table's `span`
   steps, shared by both arms. Unlike `colspan` (core/layout/table_grid.c), zero falls back: "if the col element
   has no span attribute, or if trying to parse the attribute's value resulted in an error or zero, then let
   span be 1". An overflowing digit run has parsed (core/html/integer_microsyntax.h) and clamps to 1000.
   Read only from an HTML `col` or `colgroup`; see the header. */
static size_t tcb_span_attr(lxb_dom_element_t *box)
{
    size_t len = 0;
    const lxb_char_t *v;
    HtmlInteger n;

    if (!tcb_html_local_name_is(box, "col") && !tcb_html_local_name_is(box, "colgroup")) return 1;
    v = lxb_dom_element_get_attribute(box, (const lxb_char_t *) "span", 4, &len);
    if (v == NULL) return 1;
    if (!html_parse_non_negative_integer((const char *) v, len, &n)) return 1;
    if (n.overflow || n.value > (long long) TCB_SPAN_MAX) return TCB_SPAN_MAX;
    if (n.value == 0) return 1;
    return (size_t) n.value;
}

/* Records that `column` and `group` occupy the `span` grid columns starting at `*x`, and advances `*x`.
   The write is clipped to the array and the count is not, so `noccupied` reports what the boxes occupy even
   past the grid's column count. */
static void tcb_occupy(TableColumnBoxMap *map, size_t *x, size_t span,
                       lxb_dom_element_t *column, lxb_dom_element_t *group)
{
    size_t i;

    for (i = 0; i < span; i++) {
        size_t c = *x + i;

        if (c < map->ncols) {
            /* Rules 3 and 4 move the index forward only, so no grid column is written twice. */
            DCHECK(map->cols[c].column == NULL && map->cols[c].column_group == NULL,
                   "CSS 2.1 §17.5 Visual layout of table contents' rules 3 and 4 assigned TWO column or "
                   "column-group boxes to one grid column. Rule 3 places column boxes \"next to each other in "
                   "the order they occur\" and rule 4 gives a column group \"the same grid cells as the "
                   "columns it contains\", so the running index only ever moves forward — a repeat is this "
                   "walk having advanced by something other than the span it just wrote");
            map->cols[c].column = column;
            map->cols[c].column_group = group;
        }
    }
    *x += span;
}

void table_column_boxes_build(lxb_dom_element_t *table, size_t ngrid, TableColumnBoxMap *out)
{
    /* Every member below is a node: a table and a column group are not list items (css-display-3 §2.3
       "Generating Marker Boxes: the list-item keyword" pairs `list-item` with `flow` or `flow-root`). */
    BoxTreeChild tc;
    size_t occupied = 0, x = 0;

    DCHECK(table != NULL && out != NULL,
           "CSS 2.1 §17.5's column and column-group boxes were asked for of no element, or with nowhere to put "
           "the mapping");
    out->cols = NULL;
    out->ncols = 0;
    out->noccupied = 0;

    /* ---- Pass one: how many grid columns the boxes occupy ------------------------------------------------
       The array cannot be sized until `noccupied` is known, so the child list is walked twice; both passes take
       the identical branch on it, so the spans are re-read rather than stashed. */
    for (tc = box_tree_first_child(table); box_tree_child_exists(tc); tc = box_tree_next_sibling(table, tc)) {
        lxb_dom_node_t *c = box_tree_child_node(tc);
        lxb_dom_element_t *el;
        TableBoxKind kind;

        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        el = lxb_dom_interface_element(c);
        kind = tcb_kind_of(el);
        if (kind == TABLE_BOX_COLUMN) {
            occupied += tcb_span_attr(el);
        } else if (kind == TABLE_BOX_COLUMN_GROUP) {
            BoxTreeChild mc;
            size_t inner = 0;

            for (mc = box_tree_first_child(el); box_tree_child_exists(mc); mc = box_tree_next_sibling(el, mc)) {
                lxb_dom_node_t *m = box_tree_child_node(mc);

                if (m->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
                if (tcb_kind_of(lxb_dom_interface_element(m)) == TABLE_BOX_COLUMN)
                    inner += tcb_span_attr(lxb_dom_interface_element(m));
            }
            /* §4.9.12.1's two arms: a group WITH column children takes their total, one WITHOUT takes its own
               `span`. `inner` is zero exactly when the group has no column child. */
            occupied += (inner != 0) ? inner : tcb_span_attr(el);
        }
    }
    out->noccupied = occupied;
    out->ncols = (ngrid > occupied) ? ngrid : occupied;
    if (out->ncols == 0) return;
    out->cols = (TableColumnBoxes *) calloc(out->ncols, sizeof *out->cols);
    CHECK(out->cols != NULL,
          "CSS 2.1 §17.5's rules 3 and 4 could not allocate one column-box pair per grid column of the table "
          "being laid out");

    /* ---- Pass two: which boxes occupy which columns ------------------------------------------------------ */
    for (tc = box_tree_first_child(table); box_tree_child_exists(tc); tc = box_tree_next_sibling(table, tc)) {
        lxb_dom_node_t *c = box_tree_child_node(tc);
        lxb_dom_element_t *el;
        TableBoxKind kind;

        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        el = lxb_dom_interface_element(c);
        kind = tcb_kind_of(el);
        if (kind == TABLE_BOX_COLUMN) {
            /* A column box directly in the table box. HTML's tree construction always implies a `colgroup`, so
               this is a `display: table-column` element outside any group, which §17.2.1 Anonymous table
               objects does not wrap; it occupies its columns with no group over them. */
            tcb_occupy(out, &x, tcb_span_attr(el), el, NULL);
        } else if (kind == TABLE_BOX_COLUMN_GROUP) {
            BoxTreeChild mc;
            size_t before = x;

            for (mc = box_tree_first_child(el); box_tree_child_exists(mc); mc = box_tree_next_sibling(el, mc)) {
                lxb_dom_node_t *m = box_tree_child_node(mc);
                lxb_dom_element_t *col;

                if (m->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
                col = lxb_dom_interface_element(m);
                if (tcb_kind_of(col) != TABLE_BOX_COLUMN) continue;
                tcb_occupy(out, &x, tcb_span_attr(col), col, el);
            }
            if (x == before) tcb_occupy(out, &x, tcb_span_attr(el), NULL, el);
        }
    }
    DCHECK(x == out->noccupied,
           "CSS 2.1 §17.5's rules 3 and 4 occupied a different number of grid columns on the second pass than "
           "the first pass counted. Both passes take the same branch on the same child list and re-read the "
           "same `span` attributes, so a disagreement is the DOM having changed under the two — which cannot "
           "happen inside one layout — or one arm having been edited without the other");
}

void table_column_boxes_release(TableColumnBoxMap *map)
{
    DCHECK(map != NULL, "CSS 2.1 §17.5's column-box mapping was released through no mapping");
    DCHECK(map->cols != NULL || map->ncols == 0,
           "CSS 2.1 §17.5's column-box mapping holds no array and a non-zero entry count — "
           "`table_column_boxes_build` stores NULL only for a table with neither grid columns nor column "
           "boxes, so the two have been carried apart since it answered");
    free(map->cols);
    map->cols = NULL;
    map->ncols = 0;
    map->noccupied = 0;
}
