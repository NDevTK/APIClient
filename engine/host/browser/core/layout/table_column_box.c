/* CSS 2.1 §17.5 Visual layout of table contents' rules 3 and 4 over one table's child list. See
   table_column_box.h for the contract, for why the walk is a component rather than a lookup at each of the
   three algorithms that need it, and for why `direction` is not read here. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/html/integer_microsyntax.h"
#include "core/layout/box_subject.h"
#include "core/layout/table_box.h"
#include "core/layout/table_column_box.h"

/* HTML §4.9.12.1 Forming a table's clamp, stated once in each of its two column arms and identical in both:
   "If span is greater than 1000, let it be 1000 instead." It is the section's own step and not a bound in
   CLAUDE.md's sense — a page may write any digits it likes and this is what the algorithm does with them. */
#define TCB_SPAN_MAX ((size_t) 1000)

/* THE ONE PLACE THIS FILE READS A COMPUTED `display`, WHICH IS WHERE ITS BOX-TREE-SHAPE ASSUMPTION LIVES, and
   the refusal below is here rather than at the four walks that call it for the reason the two passes are
   written to share a branch at all: the banner over pass one says they "take the identical branch on the
   identical child list", and a test written four times is that invariant held by hand.
   A `contents` CHILD IS A WRONG ANSWER HERE AND NOT AN UNBUILT ONE, WHICH IS THE WHOLE REASON IT CRASHES.
   Every other value this mapping does not name is a child that generates no column box, so skipping it is
   rules 3 and 4 running: css-display-3 §2.5 "Box Generation: the none and contents keywords" gives `none`
   "The element and its descendants generate no boxes or text sequences", so nothing inside one can be a column
   box and the skip is exact. `contents` is the opposite — "The element itself does not generate any boxes,
   but its children and pseudo-elements still generate boxes and text sequences as normal" — so the boxes
   rules 3 and 4 must count may be INSIDE the skipped child, and skipping it undercounts `noccupied` and leaves
   a grid column mapped to no box at all.
   THE WALK STEPPED PAST IT RATHER THAN REFUSING, AND THAT IS THE DEFECT SHAPE WORTH KEEPING: a walk that
   cannot express the splice must REFUSE a spliced child rather than step past it, because a step past is a
   plausible box tree and a refusal is a named gap. table_column_box.h's own argument for being a component
   says why the plausible answer is the expensive one — three algorithms read a different property off this
   mapping, and a disagreement between them "is invisible, because every answer is a real box of the real
   document" — and a silently short mapping is that same invisibility with no second walk needed to produce
   it.
   IT IS NOT THIS COMPONENT'S SPLICE TO BUILD, which is why the message names the box-tree step rather than a
   local descent: core/layout/block_flow.c's child classification, core/layout/flex_item.c's flex-item
   classification, core/layout/line_box.c's line walk and core/layout/table_box.c's anonymous-table walk each
   meet this value at their own child walk and each names the same absent construction, so a fifth copy here
   would be one box-tree rule with five answers about which children a box has.
   RETIREMENT: this paragraph goes when the splice is built and this walk iterates it, because the contrast
   between `none` and `contents` is then a property of the sequence handed to this walk rather than an argument
   about a value it reads for itself. */
static TableBoxKind tcb_kind_of(lxb_dom_element_t *el)
{
    char *display = css_computed_value(el, "display");
    TableBoxKind kind;
    char nbuf[160];

    DCHECK(display != NULL, "the cascade produced no computed `display` — the UA layer answers `inline` for "
                            "every element it does not name, so this cannot be unset");
    if (strcmp(display, "contents") == 0) {
        free(display);
        DFAILF("%s: CSS 2.1 §17.5 \"Visual layout of table contents\"' rules 3 and 4 count this child's "
               "column and column-group boxes off a child list it is not a member of yet. Its computed "
               "`display` is `contents`, and css-display-3 §2.5 \"Box Generation: the none and contents "
               "keywords\" states both halves of what that means: \"The element itself does not generate any "
               "boxes, but its children and pseudo-elements still generate boxes and text sequences as "
               "normal\", and \"For the purposes of box generation and layout, the element must be treated as "
               "if it had been replaced in the element tree by its contents (including both its "
               "source-document children and its pseudo-elements, such as ::before and ::after "
               "pseudo-elements, which are generated before/after the element's children as normal).\" So "
               "the boxes rules 3 and 4 must place are this child's OWN children, spliced into this list at "
               "its position: a `contents` column group hands the table its `col` children directly, which "
               "is rule 3's group-less column box this walk already has an arm for, and a `contents` child "
               "of a column group hands that group its own. BUILD THE SPLICE §2.5 STATES AS THE THING "
               "EVERY WALK OVER A BOX'S CHILDREN ITERATES: core/layout/block_flow.c, "
               "core/layout/flex_item.c, core/layout/line_box.c and core/layout/table_box.c each meet this "
               "value at their own child walk and each names the same construction, so it is one box-tree "
               "step and not five",
               box_subject(el, nbuf, sizeof nbuf));
        return TABLE_BOX_NOT_A_TABLE_BOX;
    }
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

/* HOW MANY GRID COLUMNS ONE COLUMN OR COLUMN-GROUP BOX OCCUPIES — HTML §4.9.12.1 Forming a table's `span`
   steps, which both arms of that section state in the same shape.
   ZERO IS THE FALLBACK AND NOT THE VALUE, which is the one place these steps differ from the `colspan` steps
   core/layout/table_grid.c runs: §4.9.12.1 writes the column arms as "Otherwise, if the col element has no
   span attribute, or if trying to parse the attribute's value resulted in an error or zero, then let span be
   1", so `<col span="0">` occupies ONE column. Reading it as zero would delete the box from the mapping
   entirely and every property the three consuming algorithms take off it with it.
   THE OVERFLOW ARM IS THE CLAMP AND NOT AN ERROR, for core/html/integer_microsyntax.h's stated reason: a digit
   run too large for a `long long` has PARSED, to something above any ceiling here, and §4.9.12.1 answers that
   with 1000. Treating it as a parse failure would give `span="99999999999999999999"` the value 1.
   THE ATTRIBUTE IS READ FROM AN HTML `col` OR `colgroup` AND FROM NOTHING ELSE — see the header. */
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
   THE WRITE IS CLIPPED TO THE ARRAY AND THE COUNT IS NOT, which is the whole of how the header's two counts
   stay apart: `noccupied` must report what the boxes OCCUPY even where the grid has fewer columns, because a
   consumer cannot state an invariant about an overrun it was never shown. */
static void tcb_occupy(TableColumnBoxMap *map, size_t *x, size_t span,
                       lxb_dom_element_t *column, lxb_dom_element_t *group)
{
    size_t i;

    for (i = 0; i < span; i++) {
        size_t c = *x + i;

        if (c < map->ncols) {
            /* Rule 3 places column boxes "next to each other in the order they occur" and rule 4 gives a group
               "the same grid cells as the columns it contains", so no grid column is reached twice by this
               walk. A second write would mean the two rules had been made to disagree about one index. */
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
    lxb_dom_node_t *n, *c;
    size_t occupied = 0, x = 0;

    DCHECK(table != NULL && out != NULL,
           "CSS 2.1 §17.5's column and column-group boxes were asked for of no element, or with nowhere to put "
           "the mapping");
    out->cols = NULL;
    out->ncols = 0;
    out->noccupied = 0;

    /* ---- PASS ONE: how many grid columns the boxes occupy -------------------------------------------------
       The array cannot be sized until `noccupied` is known and `noccupied` is not a function of the grid, so
       the child list is walked twice rather than grown — the two passes take the identical branch on the
       identical child list, which is why the spans are re-read rather than stashed. */
    n = lxb_dom_interface_node(table);
    for (c = n->first_child; c != NULL; c = c->next) {
        lxb_dom_element_t *el;
        TableBoxKind kind;

        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        el = lxb_dom_interface_element(c);
        kind = tcb_kind_of(el);
        if (kind == TABLE_BOX_COLUMN) {
            occupied += tcb_span_attr(el);
        } else if (kind == TABLE_BOX_COLUMN_GROUP) {
            lxb_dom_node_t *m;
            size_t inner = 0;

            for (m = c->first_child; m != NULL; m = m->next) {
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

    /* ---- PASS TWO: which boxes occupy which columns ------------------------------------------------------ */
    for (c = n->first_child; c != NULL; c = c->next) {
        lxb_dom_element_t *el;
        TableBoxKind kind;

        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        el = lxb_dom_interface_element(c);
        kind = tcb_kind_of(el);
        if (kind == TABLE_BOX_COLUMN) {
            /* A column box that is a direct child of the table box. HTML's tree construction never leaves a
               `col` there — it implies a `colgroup` around it — so this is CSS's own case: an element whose
               computed `display` is `table-column` outside any column group, which §17.2.1 Anonymous table
               objects does NOT wrap (its rules generate missing table, row-group, row and cell boxes and no
               column group), so the box occupies its columns with no group over them. */
            tcb_occupy(out, &x, tcb_span_attr(el), el, NULL);
        } else if (kind == TABLE_BOX_COLUMN_GROUP) {
            lxb_dom_node_t *m;
            size_t before = x;

            for (m = c->first_child; m != NULL; m = m->next) {
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
