/* CSS 2 §9.4.1 "Block formatting contexts": a box's position. See flow_position.h for the coordinate space,
   which boxes are placed by which section, and what is still refused. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/frame/viewport.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/flow_placement.h"
#include "core/layout/flow_position.h"
#include "core/layout/layout_job.h"
#include "core/layout/line_box.h"
#include "core/layout/replaced_element.h"
#include "core/layout/table_box.h"
#include "core/layout/table_grid.h"
#include "core/layout/table_height.h"
#include "core/layout/table_width.h"
#include "core/layout/table_wrapper.h"
#include "core/layout/used_value.h"

/* Every crash in this file names the box, not the call site: `flow_border_box_origin` is reached from many
   CSSOM View members, and one `element.scrollWidth` places every box in the document (core/layout/
   scrolling_area.c's descendant walk; core/dom/element_view.c's `ev_scroll_extent`), so a frame list does not
   say which box had no rule. A `__FILE__`/`__LINE__` threaded from callers would arrive through a few
   forwarding functions and still not name it. Every remedy names a computed `display`, `position`, `float` or
   `writing-mode`, so the message names the element and that value via core/layout/box_subject.h (which must
   be total; see there). Messages use `box_subject_computed`, not `fp_computed_is` (whose DCHECK would replace
   the reported defect), and never ask `replaced_element_of` (which aborts for several element kinds). The
   containing-block assert names a node (`box_subject_node`), since its question is the element's parent. */
static bool fp_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = css_computed_value(el, name);
    char nbuf[160];
    bool same;

    DCHECKF(v != NULL,
            "%s, property `%s`: "
            "the cascade produced no computed value for a property this engine models — every one of "
            "them is in lexbor's registry with an initial value, so the last layer always answers",
            box_subject(el, nbuf, sizeof nbuf), name);
    same = strcmp(v, kw) == 0;
    free(v);
    return same;
}

/* §9.4.1's two rules are stated in physical axes ("laid out one after the other, vertically"; "each box's left
   outer edge touches the left edge of the containing block"), those of `horizontal-tb` only. css-writing-modes-4
   §3.2 "Block Flow Direction: the writing-mode property" makes the block flow direction a property, and §7.4
   "Flow-Relative Mappings" restates the rules over flow-relative axes. Another mode is refused rather than
   placed by the wrong rule. */
static void fp_require_horizontal_tb(lxb_dom_element_t *el)
{
    char nbuf[160], wbuf[64];

    if (!fp_computed_is(el, "writing-mode", "horizontal-tb"))
        DFAILF("%s, computed `writing-mode` `%s`: "
              "this box's computed `writing-mode` is not `horizontal-tb`, so its BLOCK FLOW DIRECTION is not "
              "downwards and its inline axis is not horizontal — css-writing-modes-4 §3.2 \"Block Flow "
              "Direction: the writing-mode property\" gives `vertical-rl` and `sideways-rl` a right-to-left "
              "block flow and `vertical-lr` and `sideways-lr` a left-to-right one. CSS 2 §9.4.1's two rules are "
              "written in the PHYSICAL axes of a horizontal-tb mode ('one after the other, vertically' and "
              "'each box's left outer edge touches the left edge of the containing block'), and this component "
              "implements them physically, so placing a vertical-mode box by them would answer a coordinate "
              "from the wrong axis rather than fail. BUILD css-writing-modes-4 §7.4 \"Flow-Relative Mappings\", "
              "which restates §9.4.1 over the block and inline axes, and then this file's stacking and "
              "touching become the flow-relative pair with §6.4 \"Abstract-to-Physical Mappings\" applied once "
              "at the end. core/layout/block_flow.c's own vertical walk is the same subproblem seen from the "
              "height side and lands with it",
              box_subject(el, nbuf, sizeof nbuf),
              box_subject_computed(el, "writing-mode", wbuf, sizeof wbuf));
}

/* "The containing block in which the root element lives is a rectangle called the initial containing block"
   (§10.1): the element whose parent is the Document, the same test core/layout/used_value.c uses as its base
   case. */
static bool fp_is_root(const lxb_dom_node_t *n)
{
    return n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

/* The initial containing block's width, from the element's own document, never a running realm (an iframe's
   ICB is 300 CSS pixels wide, its parent's 1280). Having a box implies a presented document, so the viewport
   is asserted, not checked. */
static CssPx fp_icb_width(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    char nbuf[160];
    JSContext *dctx;

    DCHECKF(n->owner_document != NULL,
           "%s: "
           "the initial containing block was asked for an element whose node has no owner document — every node "
           "this engine mints belongs to the document that created it",
           box_subject(el, nbuf, sizeof nbuf));
    dctx = document_active_realm_of(lxb_dom_interface_node(n->owner_document));
    DCHECKF(dctx != NULL && viewport_exists(dctx),
           "%s (realm %s): "
           "CSS 2 §9.4.1's placement was asked for an element whose document is not being presented, so there "
           "is no viewport and §10.1's INITIAL CONTAINING BLOCK does not exist. The caller's own first step is "
           "the has-a-box predicate (core/dom/element_view.h), which is defined over exactly that question — so "
           "the two answers have come apart",
           box_subject(el, nbuf, sizeof nbuf), dctx == NULL ? "absent" : "present but presenting no viewport");
    return viewport_icb_width(dctx);
}

/* §9.4.1's horizontal rule, one rule with two answers: "each box's left outer edge touches the left edge of the
   containing block (for right-to-left formatting, right edges touch)". The containing block's computed
   `direction` decides (core/layout/used_value.h, with §10.1's "the 'direction' property of the initial
   containing block is the same as for the root element"). In `ltr` the border box begins one `margin-left` in;
   in `rtl` at the block's width less `margin-right` less the border box's width. Where §10.3.3's equation holds
   they coincide; an over-constrained `rtl` box differs exactly where the spec says. */
static CssPx fp_left_offset(lxb_dom_element_t *el, CssPx cb_width)
{
    if (!used_value_containing_block_is_rtl(el)) return used_value_px(el, "margin-left");
    return css_px_sub(css_px_sub(cb_width, used_value_px(el, "margin-right")),
                      used_value_border_edge_px(el, false));
}

/* CSS 2 §8.1's edges between a box's border, padding and content boxes on the leading side of one axis are
 * `used_value_leading_border_px` and `used_value_leading_edge_px`, read from core/layout/used_value.h rather
 * than composed from computed values here. CSS 2.1 §17 says some boxes lack them: §17.6.1 The separated
 * borders model gives row, row group, column and column group boxes no border, §8.4 "Padding properties:
 * 'padding-top', 'padding-right', 'padding-bottom', 'padding-left', and 'padding'" gives them no padding, and
 * §17.6.2 The collapsing border model says "in this model, a table does not have padding (but does have
 * margins)" with half-borders. Those entries route by box kind and border model, once for every caller. */

/* CSS 2.1 §17.5 Visual layout of table contents' placement of row, row group and cell boxes: rules 1, 2 and 5
 * with §17.6.1's spacing, not §9.4.1's rules ("Internal table elements do not have margins.").
 *
 * The edges are §17.5's last paragraph: "In the separated borders model, the edges coincide with the border
 * edges of cells. (And thus, in this model, there may be gaps between the rows, columns, row groups or column
 * groups, corresponding to the 'border-spacing' property.)" §17.6.1: "The distance between the table border
 * and the borders of the cells on the edge of the table is the table's padding for that side, plus the
 * relevant border spacing distance", so the first cell border is one spacing in from the content edge.
 * Spacings come from table_width.h's and table_height.h's answers, never the property, and are zero under
 * §17.6.2, where edges "coincide with the hypothetical grid lines on which the borders of the cells are
 * centered"; one arithmetic serves both models. The table box's origin is the wrapper's: §17.4 gives the
 * wrapper the element's margins and initial border and padding, and the table box the reverse, so the table
 * box's border edge is the wrapper's content edge (asserted below) moved down past its top captions. */

/* The one table layout every table placement reads, gathered once so columns (§17.5.2 Table width algorithms:
   the 'table-layout' property) and rows (§17.5.3 Table height algorithms) are built over the grid the rectangle
   came from. Built inside one placement and discarded with it: unlike core/layout/flow_placement.h, which
   holds answers under a pass with two ends and solver/dom_cow.h's number asserted across it, a table's layout
   has no such span, so nothing is kept. */
typedef struct {
    lxb_dom_element_t *table;
    FlowPoint          origin;    /* the TABLE box's border-box origin, inside §17.4's wrapper */
    TableGrid          grid;
    TableUsedWidths    widths;
    TableUsedHeights   heights;
} FpTableFrame;

/* `el` is the box being placed, not the table: the refusals below name it. */
static void fp_table_frame_build(lxb_dom_element_t *el, FpTableFrame *out)
{
    out->table = table_box_table_of(el);
    out->origin = flow_table_box_origin(out->table);
    table_grid_build(out->table, &out->grid);
    table_widths(out->table, &out->grid, &out->widths);
    table_heights(out->table, &out->grid, &out->heights);
}

static void fp_table_frame_release(FpTableFrame *f)
{
    table_heights_release(&f->heights);
    table_widths_release(&f->widths);
    table_grid_release(&f->grid);
}

/* Which end of the table §17.5's logical column 0 is painted at: rule 3, "The first column box may be either on
   the left or on the right, depending on the value of the 'direction' property of the table.", and rule 5,
   "(This constraint holds if the 'direction' property of the table is 'ltr'; if the 'direction' is 'rtl',
   interchange \"left\" and \"right\" in the previous two sentences.)"
   It is the table's own computed `direction`, not the containing block's
   (`used_value_containing_block_is_rtl` answers §9.4.1's question). `direction` is inherited
   (css-writing-modes-4 §2.1 "Specifying Directionality: the direction property"), so the two differ only where
   a document sets it on the table. */
static bool fp_table_direction_is_rtl(lxb_dom_element_t *table)
{
    char *d = css_computed_value(table, "direction");
    char nbuf[160], vbuf[64];
    bool rtl, ltr;

    DCHECKF(d != NULL,
            "%s: the cascade produced no computed `direction` for a TABLE box whose grid a CSS 2.1 §17.5 "
            "Visual layout of table contents placement is being read out of — the property is in lexbor's "
            "registry with an initial value, so the last layer always answers",
            box_subject(table, nbuf, sizeof nbuf));
    rtl = strcmp(d, "rtl") == 0;
    ltr = strcmp(d, "ltr") == 0;
    free(d);
    DCHECKF(rtl || ltr,
            "%s, computed `direction` `%s`: a computed `direction` that is neither `ltr` nor `rtl`. "
            "css-writing-modes-4 §2.1 \"Specifying Directionality: the direction property\" gives the property "
            "the `Value:` line `ltr | rtl` and nothing else, and its `Computed value:` line is `specified "
            "value` — so a third spelling is a declaration that reached the cascade without its grammar, and "
            "CSS 2.1 §17.5's rules 3 "
            "and 5 have no third answer for which end of the table logical column 0 is painted at",
            box_subject(table, nbuf, sizeof nbuf),
            box_subject_computed(table, "direction", vbuf, sizeof vbuf));
    return rtl;
}

/* One rectangle of grid cells turned into the border-box corner of the box occupying it: §17.5's rules 1, 2
 * and 5 as one arithmetic. A row is the rectangle `[0, ncols)` of its grid row ("one row of grid cells"); a cell
 * is core/layout/table_grid.h's rectangle ("one or more grid cells wide and high").
 *
 * Columns are numbered in logical order, and rules 3 and 5 put column 0 left under `ltr`, right under `rtl`
 * (`fp_table_direction_is_rtl`); rows always stack "from top to bottom in the order they occur in the source
 * document". The `rtl` arm mirrors the column index, summing the columns after the rectangle, rather than
 * subtracting from the table's `content` width: that would depend on `content` matching `columns`, which holds
 * only to rounding and fails for a zero-column table. Both arms then read `columns` and `spacing` alike, and a
 * row's x is identical under both values.
 *
 * The block coordinate is the anchor row's ("The top row of this rectangle is in the row specified by the
 * cell's parent"), since a cell's top border edge is its row's. */
static FlowPoint fp_table_grid_origin(const FpTableFrame *f, size_t row, size_t col, size_t colspan)
{
    FlowPoint p = { { 0.0, CSS_ENV_NONE, NULL }, { 0.0, CSS_ENV_NONE, NULL } };
    CssPx x, y;
    size_t j;

    DCHECK(row <= f->heights.nrows,
           "CSS 2.1 §17.5's rules 1, 2 and 5 placed a box at a grid LINE CSS 2.1 §17.5.3 Table height "
           "algorithms answered no row for. There are `nrows` rows and `nrows + 1` lines; rules 1 and 5 name a "
           "ROW and are strictly inside that range, and rule 2's union over ZERO rows names the last line for "
           "a trailing empty row group. `table_heights` answers one height per grid row of the grid it was "
           "given and asserts that count against the grid's own, so an index above the lines is those two "
           "arrays having been carried apart");
    DCHECK(col + colspan <= f->widths.ncols && col + colspan >= col,
           "CSS 2.1 §17.5's rules 1 and 5 placed a box at a rectangle reaching past the grid's own column "
           "count, or at one whose span wrapped. core/layout/table_grid.h grows `ncols` to `col + colspan` for "
           "every cell it places and gives a row every column of its grid row, and core/layout/table_width.c "
           "asserts its own column count against the grid's — so a rectangle outside it is a grid and a "
           "placement that were carried apart, and the prefix sum below would read a used width that is not "
           "this table's");
    /* §17.5.3's prefix sum, asked of core/layout/table_height.h's `table_row_used_block_offset` (row heights
       and §17.5.3's spacing term, zero under §17.6.2), not re-spelled here. */
    y = css_px_add(css_px_add(f->origin.y, used_value_leading_edge_px(f->table, true)),
                   table_row_used_block_offset(&f->heights, row));
    /* The same on the inline axis, over the columns rules 3 and 5 put before this rectangle (under `rtl`, the
       ones logically after it). */
    x = css_px_add(css_px_add(f->origin.x, used_value_leading_edge_px(f->table, false)), f->widths.spacing);
    if (fp_table_direction_is_rtl(f->table))
        for (j = col + colspan; j < f->widths.ncols; j++)
            x = css_px_add(css_px_add(x, f->widths.columns[j]), f->widths.spacing);
    else
        for (j = 0; j < col; j++)
            x = css_px_add(css_px_add(x, f->widths.columns[j]), f->widths.spacing);
    p.x = x;
    p.y = y;
    return p;
}

/* §17.5's rule 1, "Each row box occupies one row of grid cells": every column of the row's grid row. */
static FlowPoint fp_table_row_origin(lxb_dom_element_t *el)
{
    FpTableFrame f;
    FlowPoint p = { { 0.0, CSS_ENV_NONE, NULL }, { 0.0, CSS_ENV_NONE, NULL } };
    size_t row = 0;
    char nbuf[160], tbuf[160];

    fp_table_frame_build(el, &f);
    if (!table_grid_row_of(&f.grid, el, &row)) {
        /* The frame is released before the crash (it owns no element, so `f.table` survives): `DFAILF` compiles
           out in release and this arm falls through to the zero point. */
        fp_table_frame_release(&f);
        DFAILF("%s: CSS 2.1 §17.5 Visual layout of table contents' rule 1 placed NO grid row for this ROW box "
               "in %s's grid, which is the table box CSS 2.1 §17.2's own nesting puts it inside. The two walks "
               "have come apart: core/layout/table_box.h reached this table by climbing the internal boxes "
               "above the row, and core/layout/table_grid.h reached the rows by descending the same table's row "
               "groups, so a box that is in one and not the other is a row this table's own box generation does "
               "not report as a row of it. There is no position to answer with — the grid in hand is not this "
               "row's",
               box_subject(el, nbuf, sizeof nbuf), box_subject(f.table, tbuf, sizeof tbuf));
        return p;
    }
    p = fp_table_grid_origin(&f, row, 0, f.widths.ncols);
    fp_table_frame_release(&f);
    return p;
}

/* §17.5's rule 5, "Each cell is thus a rectangular box, one or more grid cells wide and high", using
   core/layout/table_grid.h's rectangle. */
static FlowPoint fp_table_cell_origin(lxb_dom_element_t *el)
{
    FpTableFrame f;
    FlowPoint p = { { 0.0, CSS_ENV_NONE, NULL }, { 0.0, CSS_ENV_NONE, NULL } };
    const TableGridCell *cell;
    char nbuf[160], tbuf[160];

    fp_table_frame_build(el, &f);
    cell = table_grid_cell_of(&f.grid, el);
    if (cell == NULL) {
        /* Released before the crash, as in `fp_table_row_origin`. */
        fp_table_frame_release(&f);
        DFAILF("%s: CSS 2.1 §17.5 Visual layout of table contents' rule 5 placed NO cell for this CELL box in "
               "%s's grid, which is the table box CSS 2.1 §17.2's own nesting puts it inside. The two walks "
               "have come apart: core/layout/table_box.h reached this table by climbing the internal boxes "
               "above the cell, and core/layout/table_grid.h reached the cells by descending the same table's "
               "rows, so a box that is in one and not the other hangs under a row §17.2.1 Anonymous table "
               "objects' box generation does not report as a row of this table. There is no position to answer "
               "with — the grid in hand is not this cell's",
               box_subject(el, nbuf, sizeof nbuf), box_subject(f.table, tbuf, sizeof tbuf));
        return p;
    }
    p = fp_table_grid_origin(&f, cell->row, cell->col, cell->colspan);
    fp_table_frame_release(&f);
    return p;
}

/* §17.5's rule 2, "A row group occupies the same grid cells as the rows it contains": every column of the
 * group's run of grid rows. A group and a row share their inline extent and x (rule 1 gives a row every column),
 * which is why core/layout/used_value.c answers both widths from one function.
 *
 * Rule 2's empty union is a recorded choice, made here. `<tbody></tbody>` has no cell, so §17.5's last
 * paragraph names it no edges. core/layout/table_grid.h's `table_grid_empty_row_group_line` gives the grid line
 * it stands on (a fact); this chooses that the line is the distance a row starting there would have
 * (`table_row_used_block_offset`), the next row's top border edge. The alternative, the previous row's bottom
 * border edge, is one `border-spacing` higher under §17.6.1 and identical under §17.6.2. This one keeps the
 * group in place when a row is added to it. A browser may differ only in `getBoundingClientRect().top` on an
 * empty `<tbody>` with a vertical `border-spacing`.
 * No refusal arm: `table_grid_row_group_of` asserts the wrong-table walk and `table_grid_empty_row_group_line`
 * asserts the group is this table's. */
static FlowPoint fp_table_row_group_origin(lxb_dom_element_t *el)
{
    FpTableFrame f;
    FlowPoint p;
    const TableGridRowGroup *run;
    size_t line;

    fp_table_frame_build(el, &f);
    run = table_grid_row_group_of(&f.grid, el);
    line = (run != NULL) ? run->first : table_grid_empty_row_group_line(&f.grid, el);
    p = fp_table_grid_origin(&f, line, 0, f.widths.ncols);
    fp_table_frame_release(&f);
    return p;
}

/* Whether a box is on a line box, asked here before §9.4.1's rules, which are written about block-level boxes
 * in a block formatting context. The UA sheet gives `inline-block` to `input`, `button`, `select`, `textarea`
 * and `marquee` and table-internal types to a table's children, none of which §9.4.1's two rules place. The
 * invariant is asserted here, at the child's own position question, rather than relying on block_flow.c
 * refusing the child at its parent's height walk.
 *
 * `none` and `contents` are a DCHECK (in `fp_require_placeable`): the caller has established a box exists. */
static bool fp_is_on_a_line_box(lxb_dom_element_t *el)
{
    /* The question is the box's level. CSS 2.1 §9.2.2 "Inline-level elements and inline boxes" puts both kinds on
       the line: a non-replaced `display: inline` element "generates an inline box", and inline-level boxes
       "such as replaced inline-level elements, inline-block elements, and inline-table elements" are atomic,
       participating "as a single opaque box". core/layout/line_box.h owns the fragment shape (two edge items,
       or one atomic item).
       An `inline-block` comes through here: line_box.h hangs its margin box from its own baseline, which
       §10.8.1 "Leading and half-leading" puts inside the box ("The baseline of an 'inline-block' is the
       baseline of its last line box in the normal flow, unless it has either no in-flow line boxes or if its
       'overflow' property has a computed value other than 'visible', in which case the baseline is the bottom
       margin edge"; `lb_atomic_extent`). A replaced `inline-block` is the same (CSS 2.2 §10.3.10: "Exactly as
       inline replaced elements."), sized by used_value.c's replaced arm first.
       `inline-table`, `inline-flex` and `inline-grid` leave through `fp_require_placeable`, each needing a
       module's number. A replaced element with `display: block` is block-level (§9.2.1) and takes the
       induction, with §10.3.4's and §10.6.2's extents. */
    return fp_computed_is(el, "display", "inline") || fp_computed_is(el, "display", "inline-block");
}

/* CSS 2 §9.4.2 "Inline formatting contexts"' placement of a box on a line ("boxes are laid out horizontally,
   one after the other, beginning at the top of a containing block"), for a non-replaced inline box's fragments
   and an atomic inline-level box's one fragment (a replaced element, or either kind of `inline-block`, CSS 2.2
   §10.3.9 and §10.3.10). core/layout/line_box.h owns the difference.
   The frame is the establishing box's content box: its origin (this function, one level up) plus CSS 2 §8.1's
   leading border and padding, as §10.1's second case composes it below. CSSOM View §6's `getClientRects` asks
   core/layout/line_box.h for the whole list via core/dom/element_view.c. */
bool flow_inline_fragment_rects_ask(lxb_dom_element_t *el, FlowRect **out, size_t *count)
{
    lxb_dom_element_t *style = NULL;
    LineBoxFragment *frags = NULL;
    FlowRect *rects;
    FlowPoint o;
    CssPx left, top;
    char ebuf[160], sbuf[160];
    size_t n, i;

    DCHECK(el != NULL && out != NULL,
           "CSS 2 §9.4.2's fragment rectangles were asked for with no element or nowhere to report them");
    if (!line_box_inline_fragments_ask(el, &style, &frags, &n)) return false;
    DCHECKF(n >= 1 && frags != NULL && style != NULL,
           "%s, establishing box %s, %zu fragment(s) reported%s: "
           "CSS 2 §9.4.2's fragments were reported as none for an inline-level box that generates one. That "
           "entry's own asserts make a zero count impossible — an inline box's two edge items and an atomic "
           "inline-level box's one item are alike content the fill partitions — so this is that contract "
           "having been broken between the two files",
           box_subject(el, ebuf, sizeof ebuf), box_subject(style, sbuf, sizeof sbuf), n,
           frags == NULL ? " and no fragment array" : "");
    /* §10.1's second case, as the block-level arm composes it: the establishing box's origin (where a float, an
       out-of-flow ancestor or a vertical mode crashes by its own section) plus §8.1's leading border and
       padding, the gap between its border edge and the content edge line_box.h measures from. */
    if (!flow_border_box_origin_ask(style, &o)) {
        free(frags);
        return false;
    }
    left = css_px_add(o.x, used_value_leading_edge_px(style, false));
    top = css_px_add(o.y, used_value_leading_edge_px(style, true));
    rects = malloc(n * sizeof *rects);
    CHECK(rects != NULL, "out of memory placing CSS 2 §9.4.2's box fragments — one entry per fragment of one "
                         "inline box, so a failure here is the physical floor");
    for (i = 0; i < n; i++) {
        rects[i].x = css_px_add(left, frags[i].inline_start);
        rects[i].y = css_px_add(top, frags[i].block_start);
        rects[i].width = css_px_sub(frags[i].inline_end, frags[i].inline_start);
        rects[i].height = css_px_sub(frags[i].block_end, frags[i].block_start);
    }
    free(frags);
    *out = rects;
    *count = n;
    return true;
}

size_t flow_inline_fragment_rects(lxb_dom_element_t *el, FlowRect **out)
{
    LayoutJobDrive d;
    size_t n = 0;

    layout_job_drive_begin(&d);
    while (!flow_inline_fragment_rects_ask(el, out, &n)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return n;
}

/* The origin of a box §9.4.2 places: its first fragment's corner, which is also an atomic inline-level box's
   only one ("single opaque box"), so callers need not know which they hold. */
static bool fp_line_box_origin(lxb_dom_element_t *el, FlowPoint *out)
{
    FlowRect *rects = NULL;
    size_t n = 0;

    if (!flow_inline_fragment_rects_ask(el, &rects, &n)) return false;
    out->x = rects[0].x;
    out->y = rects[0].y;
    free(rects);
    return true;
}

/* CSS 2.1 §17.4 "Tables in the visual formatting model"'s caption: "rendered as normal block boxes inside the
   table wrapper box", so §9.4.1 places it on the wrapper's stack (core/layout/block_flow.c walks that list) and
   §10.1's second case composes the wrapper's origin with the stack position. The wrapper is the table element's
   principal box and has no border or padding (§17.4 puts them on the table box), so its origin is the table
   element's and its content edge adds nothing. */
static bool fp_caption_origin(lxb_dom_element_t *el, FlowPoint *out)
{
    lxb_dom_element_t *table = used_value_stacking_container(el);
    FlowPoint o;
    CssPx top;

    if (!flow_border_box_origin_ask(table, &o) || !block_flow_child_top_ask(el, &top)) return false;
    out->x = css_px_add(o.x, fp_left_offset(el, used_value_containing_block_width(el)));
    out->y = css_px_add(o.y, top);
    return true;
}

static void fp_require_placeable(lxb_dom_element_t *el)
{
    /* Rows, cells and row groups are placed by §17.5's rules 1, 2 and 5 above, and captions by §17.4's wrapper
       stack (`fp_caption_origin`); none is listed. Left: column and column-group boxes (no mapping from element
       to grid column). */
    static const char *const TABLE_INTERNAL[] = {
        "table-column-group", "table-column",
    };
    char *d = css_computed_value(el, "display");
    char nbuf[160];
    bool table_internal = false, inline_level;
    unsigned i;

    /* No guard under this assert: `css_computed_value(el, "display")` cannot return NULL (its `computed_display`
       dereferences the specified value first, and every return is that string or a CHECKed `css_cv_strdup`),
       so a guard would be dead and only make the assert look recoverable. */
    DCHECKF(d != NULL, "%s: the cascade produced no computed `display` for an element whose box is being placed",
            box_subject(el, nbuf, sizeof nbuf));
    DCHECKF(strcmp(d, "none") != 0 && strcmp(d, "contents") != 0,
           "%s: "
           "CSS 2 §9.4.1's placement was asked for an element that GENERATES NO BOX — css-display-3 §2.5 "
           "\"Box Generation: the none and contents keywords\" gives `contents` no box of its own and `none` "
           "no box at all (\"The element and its descendants generate no boxes or text sequences.\"), and "
           "core/dom/element_view.h's one box "
           "predicate reads exactly those two values. The caller's own step establishes the box exists before "
           "asking where it is, so this is that predicate and this test disagreeing",
           box_subject(el, nbuf, sizeof nbuf));
    for (i = 0; i < sizeof(TABLE_INTERNAL) / sizeof(TABLE_INTERNAL[0]); i++)
        if (strcmp(d, TABLE_INTERNAL[i]) == 0) table_internal = true;
    /* `inline` and `inline-block` are placed by §9.4.2 (`fp_line_box_origin`) before this is asked. What is left
       is atomic inline-level boxes (CSS 2.1 §9.2.2 "Inline-level elements and inline boxes") whose module
       outside CSS 2.1 §10 owns a number the line needs. */
    inline_level = strcmp(d, "inline-table") == 0 || strcmp(d, "inline-flex") == 0 ||
                   strcmp(d, "inline-grid") == 0;
    /* The string is released before either crash and neither reads it: `DFAIL` compiles out in release, so a
       `free` beside one would be a use-after-free or double free there. The messages name the `display`
       through `box_subject`, which reads and frees its own copy; folding `d` in would reintroduce the bug. */
    free(d);
    if (table_internal)
        DFAILF("%s: "
              "this box is TABLE-INTERNAL, and CSS 2.1 §17.5 'Visual layout of table contents' positions it "
              "rather than §9.4.1: a row group, a column and a column group are laid out inside the TABLE's "
              "own grid of rows and columns, so a row group's position is the accumulated heights of the "
              "rows above its first, and a column's is the accumulated widths of the columns before it — "
              "neither is a distance §9.4.1's two rules can produce. "
              "A ROW, A CELL AND A ROW GROUP ARE NO LONGER AMONG THEM AND THAT IS THE SHAPE OF EVERY ANSWER "
              "BELOW: §17.5's rules 1, 2 and 5 PLACE all three (\"Each row box occupies one row of grid "
              "cells\", \"A row group occupies the same grid cells as the rows it contains\" and \"Each cell "
              "is thus a rectangular box, one or more grid cells wide and high\"), core/layout/table_grid.h "
              "reports each placement back per element, and `fp_table_grid_origin` above turns ONE RECTANGLE OF "
              "GRID CELLS into this coordinate — the block-axis distance asked of "
              "core/layout/table_height.h's `table_row_used_block_offset` and the inline-axis prefix sum over "
              "`TableUsedWidths.columns` with §17.6.1 The separated borders model's spacing between and before "
              "them, measured from the table box's content edge, with rules 3 and 5's `direction` deciding "
              "which end logical column 0 is painted at. So the OPERANDS and the ARITHMETIC are both built and "
              "what each box below still lacks is a NAME FOR ITS OWN RECTANGLE IN THE GRID, which is a "
              "different missing thing per box and not one blocker. "
              "THE CELL'S INSTRUCTION THAT STOOD HERE IS RETIRED WITH THE CODE IT GUARDED, and the reading is "
              "kept rather than deleted because it named the answer's SHAPE wrongly and a reader re-deriving it "
              "would build a second placement: it said a cell's PHYSICAL x is the prefix sum under `ltr` and "
              "\"the table's content width less that sum and less the cell's own width\" under `rtl`. Those "
              "two are the same point, and the second spelling makes every `rtl` coordinate depend on "
              "core/layout/table_width.h's `content` agreeing with its `columns` — an identity that component "
              "asserts only to within its own distribution's rounding and which legitimately FAILS for a "
              "zero-column table. `fp_table_grid_origin` performs rule 5's interchange on the COLUMN INDEX "
              "instead, so both arms read `columns` and `spacing` and neither reads `content`. "
              "THE ROW GROUP'S INSTRUCTION THAT STOOD HERE IS RETIRED WITH THE CODE IT GUARDED, and its last "
              "sentence was right about the size of the work and wrong about where the decision lived: it said "
              "rule 2's EMPTY union had no reading for a POSITION \"because a zero-extent box still has to sit "
              "somewhere and the grid names nowhere\", and the grid names it now — an empty `<tbody>` stands on "
              "the grid LINE §17.2 The CSS table model's display order counts the rows before it to "
              "(core/layout/table_grid.h's `table_grid_empty_row_group_line`, out of "
              "core/layout/table_box.h's `table_box_row_groups`), and WHICH DISTANCE that line becomes is the "
              "choice `fp_table_row_group_origin` above records with its alternative. A reader re-deriving the "
              "retired sentence would build a second placement for a box that has one. "
              "A COLUMN AND A COLUMN GROUP NEED A MAPPING THAT DOES NOT EXIST IN EITHER DIRECTION YET, and this "
              "is the one place below where the operand is genuinely absent rather than merely unrouted. "
              "core/layout/table_column_box.h answers §17.5's rules 3 and 4 from the GRID COLUMN's side — "
              "`TableColumnBoxMap.cols[i]` names the column and column-group boxes occupying column `i` — and a "
              "box holding an ELEMENT needs the reverse, which is that component's entry to add and not this "
              "file's scan. Note its own `ncols` can EXCEED the grid's, since HTML §4.9.12.1 Forming a table "
              "counts a `<colgroup span>` into the table's width, so the reverse answer can name a column no "
              "cell occupies and the horizontal sum above has no width for it",
              box_subject(el, nbuf, sizeof nbuf));
    if (inline_level)
        DFAILF("%s: "
              "CSS 2.1 §9.2.2 'Inline-level elements and inline boxes' makes this an ATOMIC INLINE-LEVEL box — "
              "an `inline-table`, or css-display-3 §2's `inline flex` and `inline grid` — boxes CSS 2.1 §9.2.2 "
              "calls atomic \"because they participate in their inline formatting context as a single opaque "
              "box\". So §9.4.2 places it, not §9.4.1, and its position is a position ON A LINE BOX exactly as "
              "a non-replaced inline box's is. "
              "THE PLACEMENT IS BUILT AND SO IS THE `inline-block`, WHICH IS WHY NEITHER IS STILL NAMED HERE: "
              "`line_box_inline_fragments` (core/layout/line_box.h) delimits an atomic inline's fragment by its "
              "own item index, hangs its margin box from the line's baseline SPLIT at the box's own baseline "
              "(`lb_atomic_extent`), and `fp_line_box_origin` above turns that rectangle into this coordinate — "
              "for a `display: inline` box, for a REPLACED element, and for both halves of the `inline-block` "
              "CSS 2.2 §10.3.9 and §10.3.10 divide between. Following an earlier form of this line would have "
              "built that a second time. "
              "WHAT IS LEFT IS NOT A PLACEMENT AT ALL: each of the three boxes above needs a NUMBER from a "
              "module outside CSS 2.1 §10, without which there is nothing for §9.4.2's fill to put on a line. "
              "AN `inline-table` HAS CSS 2.1 §17.2 The CSS table model's box structure — the row groups, rows "
              "and cells §17.2.1 Anonymous table objects generates, which core/layout/table_box.h answers — so "
              "what it still needs is NOT that inline size — CSS 2.1 §17.5.2 Table width algorithms: the "
              "'table-layout' property is core/layout/table_width.h and core/layout/used_value.c routes a "
              "table box's `width` to it, so `used_value_margin_edge_px` answers an `inline-table`'s margin-box "
              "inline size like any other box's — but the baseline CSS 2.2 §10.8.1 \"Leading and half-leading\" states "
              "for it (\"The baseline of an 'inline-table' is the baseline of the first row of the table\"), "
              "which is a position inside that structure and not a rule that can be built beside it. "
              "AN `inline-flex` AND AN `inline-grid` ARE ALREADY CLASSIFIED — `uv_box_kind`'s "
              "`UV_BOX_INLINE_FLEX_GRID` — so an `auto` width on one CRASHES by its own module's name rather "
              "than answering with §10.3.3's constraint equation over a box no part of §10.3 describes. WHAT "
              "THAT CLASSIFICATION LEFT IS THE MODULE'S OWN INTRINSIC MAIN SIZES, which §10.3.9's "
              "shrink-to-fit reads as its two terms: css-flexbox-1 §9.9.1 \"Flex Container Intrinsic Main "
              "Sizes\" and css-grid-1 §5.2 \"Sizing Grid Containers\", whose max-content size is \"the sum "
              "of the grid container's track sizes (including gutters) in the appropriate axis\". Each module "
              "owns that box's BASELINE too and §10.8.1 names neither of them — css-flexbox-1 §8.5 \"Flex "
              "Container Baselines\" derives one from the items on the container's startmost flex line and "
              "css-grid-1 §10.6 \"Grid Container Baselines\" does the same — so both numbers fall out of the "
              "module's layout and neither is this file's to invent. BUILD the module this box's `display` "
              "names as a second producer of `IntrinsicInlineSizes`. An `inline-table` is NOT waiting on that "
              "and this line used to say it was: its inline size is answered, and what it needs is §10.8.1's "
              "own sentence above, which is a position inside CSS 2.1 §17.5.3 Table height algorithms' rows. "
              "Then this box reaches the line and this arm deletes",
              box_subject(el, nbuf, sizeof nbuf));
    /* What is left is block-level (CSS 2.1 §9.2.1 'Block-level elements and block boxes'), which §9.4.1's rules
       describe. A `table` stays here: §17.4 'Tables in the visual formatting model' makes the wrapper
       block-level for `display: table`, so its origin is §9.4.1's and §17.5 owns what is inside it. */
}

/* ---- CSS 2.1 §10.3.7's and §10.6.4's static position, as a coordinate --------------------------------------
 * core/layout/block_flow.h answers where §9.4.1's stack would have put the hypothetical box, in its own
 * containing block's frame; the sections want that distance from §10.1's third or fourth rectangle, so the
 * static position is a difference of two origins.
 *
 * Only the vertical rule is a stack; the horizontal rule ("each box's left outer edge touches the left edge of
 * the containing block") puts the margin edge at the block's content edge whatever the siblings did. `trailing`
 * is §10.3.7's `right` with its sign, for an `rtl` containing block only, never on the block axis.
 *
 * Named residual: the hypothetical box's inline size is not solved. Exact while its margin edge touches the
 * containing block's edge (no width needed; `rtl` uses the block's `used_value_content_px`). Not covered: an
 * `auto` horizontal margin, which §10.3.3's rules 2 and 4 turn into slack that centres the box. Next: §10.3.3's
 * solve over the hypothetical box with its in-flow `width`. Absence shows as an absolutely positioned box with
 * `margin-left: auto` and no declared `left` sitting at the leading content edge, where a browser centres it. */

/* `flow_padding_box_origin` without a drive: false ("not yet") as `flow_border_box_origin_ask` answers it. */
static bool fp_padding_box_origin_ask(lxb_dom_element_t *el, FlowPoint *out)
{
    FlowPoint p;

    if (!flow_border_box_origin_ask(el, &p)) return false;
    p.x = css_px_add(p.x, used_value_leading_border_px(el, false));
    p.y = css_px_add(p.y, used_value_leading_border_px(el, true));
    *out = p;
    return true;
}

/* §10.1's third and fourth cases as a coordinate: the leading edge, on the axis asked, of the rectangle an
   absolutely positioned box's offsets are stated against, in the ICB's space. The extent is the caller's
   (`used_value_containing_block_width`, or §10.6.4's block-axis twin). False ("not yet") when the fourth
   case's ancestor origin was deferred; `*out` is then unwritten. */
static bool fp_abs_cb_leading_ask(lxb_dom_element_t *el, bool vertical, bool for_static_position, CssPx *out)
{
    lxb_dom_element_t *anc = NULL;
    lxb_dom_node_t *n;
    JSContext *dctx;
    char nbuf[160];

    switch (used_value_abs_containing_block(el, &anc)) {
    case USED_VALUE_ABS_CB_INITIAL:
        /* §10.1: the initial containing block is "anchored at the canvas origin", this component's origin. */
        *out = css_px(0.0);
        return true;
    case USED_VALUE_ABS_CB_VIEWPORT:
        /* css-position-3 §2.1 "Containing Blocks of Positioned Boxes": a fixed box's containing block is "the
           layout viewport … as a result, fixed boxes do not move when the document is scrolled", and CSSOM
           View §4's `scrollX` is "relative to the initial containing block origin, of the left of the
           viewport". So the viewport's leading edge is the scroll position.
           For a static position §10.3.7 says instead "the containing block of fixed positioned elements is the
           initial containing block instead of the viewport, and all scrollable boxes should be assumed to be
           scrolled to their origin": the ICB has "the dimensions of the viewport", so that is
           `for_static_position` returning the origin. Its second half holds already:
           `flow_padding_box_origin` subtracts no ancestor scroll offset. */
        if (for_static_position) {
            *out = css_px(0.0);
            return true;
        }
        n = lxb_dom_interface_node(el);
        DCHECKF(n->owner_document != NULL,
               "%s: §10.1's third case was asked for an element whose node has no owner document — every node "
               "this engine mints belongs to the document that created it",
               box_subject(el, nbuf, sizeof nbuf));
        dctx = document_active_realm_of(lxb_dom_interface_node(n->owner_document));
        DCHECKF(dctx != NULL && viewport_exists(dctx),
               "%s: §10.1's third case is the VIEWPORT and this element's document is presented in none. "
               "core/dom/element_view.h's has-a-box predicate is where an element outside a rendered tree "
               "leaves, and the two answers have come apart",
               box_subject(el, nbuf, sizeof nbuf));
        *out = css_px(viewport_window_scroll(dctx, vertical));
        return true;
    case USED_VALUE_ABS_CB_PADDING_EDGE: {
        /* §10.1's fourth case: "the containing block is formed by the padding edge of the ancestor", which
           `flow_padding_box_origin` is (so an unplaceable ancestor crashes there by its own section). */
        FlowPoint o;

        DCHECKF(anc != NULL,
               "%s: §10.1's FOURTH case names \"the padding edge of the ancestor\" and `uv_cb` reported that "
               "case with no ancestor. The tag and the element are written together out of one walk, so a "
               "NULL here is that walk having produced a case it has no box for",
               box_subject(el, nbuf, sizeof nbuf));
        if (!fp_padding_box_origin_ask(anc, &o)) return false;
        *out = vertical ? o.y : o.x;
        return true;
    }
    }
    DFAILF("%s: §10.1 gives an absolutely positioned box one of three rectangles and this is a fourth",
           box_subject(el, nbuf, sizeof nbuf));
    *out = css_px(0.0);
    return true;
}

bool flow_static_position_ask(lxb_dom_element_t *el, bool vertical, bool trailing, CssPx *out)
{
    lxb_dom_element_t *spcb;
    FlowPoint o;
    CssPx edge, cb_edge, top;

    DCHECK(el != NULL, "CSS 2.1 §10.3.7's static position was asked for with no element");
    /* The root's hypothetical static box is in the initial containing block, §10.1's first case, which is
       also its containing block as an absolutely positioned box: the static position is that rectangle's own
       edge on every arm. */
    if (fp_is_root(lxb_dom_interface_node(el))) {
        *out = css_px(0.0);
        return true;
    }
    DCHECK(!vertical || !trailing,
           "CSS 2.1 §10.6.4 names `top` in BOTH of the entries that use a static position — its "
           "all-three-`auto` case (\"set `top` to the static position and apply rule number three below\") and "
           "its rule 2 — and states no static position for `bottom` anywhere. A trailing static position on "
           "the block axis is §10.3.7's `rtl` arm having been carried across an axis it is not written for");
    spcb = block_flow_static_position_containing_block(el);
    if (!flow_border_box_origin_ask(spcb, &o)) return false;
    if (!fp_abs_cb_leading_ask(el, vertical, true, &cb_edge)) return false;
    if (vertical) {
        if (!block_flow_static_child_top_ask(el, &top)) return false;
        /* §9.4.1's vertical rule from the walk that implements it, measured from the static-position containing
           block's top content edge, so its §8.1 leading border and padding are added here, as §10.1's second
           case does for an in-flow box. */
        edge = css_px_add(css_px_add(o.y, used_value_leading_edge_px(spcb, true)), top);
    } else if (!trailing) {
        /* §9.4.1's horizontal rule: "each box's left outer edge touches the left edge of the containing block".
           The outer edge is §8.1's margin edge, what §10.3.7 asks for, so it is the block's left content edge. */
        edge = css_px_add(o.x, used_value_leading_edge_px(spcb, false));
    } else {
        /* "(for right-to-left formatting, right edges touch)": the right margin edge is at the block's right
           content edge, its left one plus its content width. */
        edge = css_px_add(css_px_add(o.x, used_value_leading_edge_px(spcb, false)),
                          used_value_content_px(spcb, false));
    }
    if (!trailing) {
        *out = css_px_sub(edge, cb_edge);
        return true;
    }
    /* §10.3.7's `right` is measured from the containing block's right edge, positive leftwards, so the
       subtraction is reversed and the rectangle's extent (the ancestor's padding box under §10.1's fourth
       case, `used_value_containing_block_width`) is added to reach that edge. */
    *out = css_px_sub(css_px_add(cb_edge, used_value_containing_block_width(el)), edge);
    return true;
}

CssPx flow_static_position(lxb_dom_element_t *el, bool vertical, bool trailing)
{
    LayoutJobDrive d;
    CssPx v;

    layout_job_drive_begin(&d);
    while (!flow_static_position_ask(el, vertical, trailing, &v)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return v;
}

/* CSS 2 §9.3.2 "Box offsets: `top`, `right`, `bottom`, `left`"' placement of an absolutely positioned box:
   §10.3.7's equation puts its leading border edge at the containing block's leading edge plus the used offset
   plus the used margin. No ancestor border or padding is added, because §10.1's fourth case is already the
   padding edge (`fp_abs_cb_leading_ask`); that is the difference from §10.1's second case below.
   The static positions §10.3.7's and §10.6.4's solve will read are asked first, so a deferred stack walk
   answers false ("not yet") before anything is computed and the solve then finds them answered. */
static bool fp_abs_origin_ask(lxb_dom_element_t *el, FlowPoint *out)
{
    CssPx x0, y0, unused;
    bool vertical, trailing;

    for (vertical = false;; vertical = true) {
        if (used_value_abs_static_position_read(el, vertical, &trailing) &&
            !flow_static_position_ask(el, vertical, trailing, &unused))
            return false;
        if (vertical) break;
    }
    if (!fp_abs_cb_leading_ask(el, false, false, &x0) || !fp_abs_cb_leading_ask(el, true, false, &y0))
        return false;
    out->x = css_px_add(css_px_add(x0, used_value_abs_offset_px(el, false)), used_value_px(el, "margin-left"));
    out->y = css_px_add(css_px_add(y0, used_value_abs_offset_px(el, true)), used_value_px(el, "margin-top"));
    return true;
}

/* CSS 2.2 §9.4.3 "Relative positioning"'s translation on one axis, or zero where the section does not apply.
   §9.4.3 is stated over a box that "has been laid out according to the normal flow or floated" and is then
   "shifted relative to this position"; a box whose `position` is not `relative` is not shifted by it. The
   composition and `fp_origin_agrees` below must add the same term, so it is asked in one place. */
static CssPx fp_relative_offset(lxb_dom_element_t *el, bool vertical)
{
    if (!css_computed_keyword_is(el, "position", CSS_KEYWORD_RELATIVE)) return css_px(0.0);
    return used_value_rel_offset_px(el, vertical);
}

/* §10.1's second case re-derived from the record, for an assertion only (the check core/layout/
   flow_placement.h's §10.1 section is written around). It reads the containing block's recorded point, the
   box's stack position from core/layout/block_flow.c's record, the edges from the cascade, and §9.4.3's
   translation from core/layout/used_value.h, so it can disagree with the recorded point. A disagreement means
   the record answered for a box it does not hold, or a computed value moved inside the pass.
   It peeks rather than asks (`flow_placement_origin_peek` counts and recurses nowhere), keeping it O(1):
   asking would trigger the ancestor's check, exponential in depth. It answers true where it cannot speak (no
   pass, another arm's point, an ancestor or stack position not held), which `derived_from` being NULL
   records. */
/* Two lengths' examples compared, for an assertion only (as in core/layout/flow_placement.c): a public equality
   over `CssPx` would let a caller branch on a concolic value's example and lose the other world's arm. Inside a
   `DCHECK` there is no arm to lose. NaN equals itself here: two runs of one derivation both giving NaN
   agree. */
static bool fp_same_example(CssPx a, CssPx b)
{
    return a.px == b.px || (a.px != a.px && b.px != b.px);
}

static bool fp_origin_agrees(lxb_dom_element_t *el, const lxb_dom_element_t *cb, FlowPoint rec)
{
    FlowPoint o;
    CssPx     top, x, y;

    if (cb == NULL) return true;
    if (!flow_placement_origin_peek(cb, &o, NULL)) return true;
    if (!flow_placement_peek(el, &top)) return true;
    x = css_px_add(css_px_add(o.x, used_value_leading_edge_px((lxb_dom_element_t *)cb, false)),
                   fp_left_offset(el, used_value_containing_block_width(el)));
    y = css_px_add(css_px_add(o.y, used_value_leading_edge_px((lxb_dom_element_t *)cb, true)), top);
    /* CSS 2.2 §9.4.3's term is part of the equation: the recorded point includes this box's relative
       translation (see `flow_border_box_origin`). The ancestor's needs no term: `o` is its recorded point,
       which carries it. */
    x = css_px_add(x, fp_relative_offset(el, false));
    y = css_px_add(y, fp_relative_offset(el, true));
    return fp_same_example(x, rec.x) && fp_same_example(y, rec.y);
}

/* CSS 2 §8.1 "Box dimensions"' border box, placed through §10.1's cases. `*cb_out` is the containing block for
   §10.1's second case and stays NULL for every other arm, which is all the record needs to know about how a
   point was produced (core/layout/flow_placement.h). Answers false ("not yet") when the second case's climb
   or §9.4.1's walk deferred a question; `*out` is then unwritten. The other arms complete in place. */
static bool fp_border_box_origin_compute(lxb_dom_element_t *el, const lxb_dom_element_t **cb_out,
                                         FlowPoint *out)
{
    FlowPoint p = { { 0.0, CSS_ENV_NONE, NULL }, { 0.0, CSS_ENV_NONE, NULL } };
    lxb_dom_node_t *n;
    lxb_dom_element_t *cb;
    FlowPoint o;
    CssPx top;
    /* `vbuf` holds a computed value at two sites and a node name at the third, so it is sized for the wider. */
    char nbuf[160], vbuf[160];

    DCHECK(el != NULL, "a border box's position was asked for with no element");
    n = lxb_dom_interface_node(el);

    /* §9.4.1 places boxes in normal flow; §9.3's positioning scheme sends other boxes to their own sections
       first. */
    /* CSS 2 §9.3.1 takes an absolutely positioned box out of normal flow: its position is §9.3.2's offsets over
       §10.1's third or fourth case, solved by §10.3.7 and §10.6.4, whose `auto` cases use the static
       position, so this consumes §9.4.1's flow layout. */
    if (css_computed_keyword_is(el, "position", CSS_KEYWORD_ABSOLUTE) || css_computed_keyword_is(el, "position", CSS_KEYWORD_FIXED))
        return fp_abs_origin_ask(el, out);
    /* CSS 2 §9.5.1's placement for the root, the one float whose containing block (the initial containing
       block) holds no other box: "a floating box's outer top may not be higher than the top of its containing
       block" and "must be placed as high as possible", and "a left-floating box must be put as far to the left
       as possible, a right-floating box as far to the right as possible", so its outer edges are the ICB's. */
    if (block_flow_is_floated(el) && fp_is_root(n)) {
        p.y = used_value_px(el, "margin-top");
        if (fp_computed_is(el, "float", "left")) {
            p.x = used_value_px(el, "margin-left");
        } else {
            DCHECKF(fp_computed_is(el, "float", "right"),
                    "%s, computed `float` `%s`: CSS 2 §9.5.1's `Value:` line is `left | right | none`",
                    box_subject(el, nbuf, sizeof nbuf), box_subject_computed(el, "float", vbuf, sizeof vbuf));
            p.x = css_px_sub(css_px_sub(fp_icb_width(el), used_value_px(el, "margin-right")),
                             used_value_border_edge_px(el, false));
        }
        *out = p;
        return true;
    }
    if (block_flow_is_floated(el))
        DFAILF("%s, computed `float` `%s`: "
              "CSS 2 §9.5 'Floats' positions a FLOATING box, and §9.4.1's rule does not: a float is shifted to "
              "the left or right edge of its containing block and then down past any earlier float it would "
              "overlap, under §9.5.1's nine constraints. Its own used width is a SHRINK-TO-FIT (§10.3.5) and "
              "core/layout/used_value.c COMPUTES ONE NOW, over core/layout/intrinsic_size.h's measurement of "
              "the box's content — so the extent is no longer the blocker and the POSITION is the whole of what "
              "is left. BUILD §9.5.1's nine constraints over the line boxes the float interacts with. "
              "THAT CLAUSE IS TRUE ABOUT THE EXTENT AND INCOMPLETE ABOUT THE POSITION, AND THE CORRECTION IS "
              "WRITTEN HERE BECAUSE A LANE DISPATCHED AT IT WILL OTHERWISE RE-DERIVE IT: §9.5.1's rules 4, 5 "
              "and 6 are a LOWER BOUND on the float's outer top stated over boxes it does not own — its "
              "containing block's top, \"the outer top of any block or floated box generated by an element "
              "earlier in the source document\", and \"the top of any line-box containing a box generated by an "
              "element earlier in the source document\" — so the whole of what this placement needs first is "
              "WHERE §9.4.1's AND §9.4.2's FLOW STANDS AT THE FLOAT'S OWN PLACE, which CSS 2.2 §9.5 "
              "\"Floats\" states in its own words as \"if there is a line box, the outer top of the floated box "
              "is aligned with the top of the current line box\". ONE OF THE TWO ROUTES TO THAT NUMBER NOW "
              "ANSWERS, WHICH IS WHAT THE ORDERING NOTE THAT STOOD HERE WAS FOR AND IS RECORDED RATHER THAN "
              "DELETED BECAUSE A READER WILL RE-DERIVE THE OLD STATE. It said EVERY route refused — a container "
              "with inline content in core/layout/line_box.c and one with a block-level box on its stack in "
              "core/layout/block_flow.c — and therefore that §9.5.1's placement was not the next member of the "
              "decomposition, since a diff building it would land code no run could execute. THE BLOCK-LEVEL "
              "HALF OF THAT IS SPENT: core/layout/block_flow.c no longer refuses §9.4.1's stack for a container "
              "holding a float, because CSS 2.2 §9.5 says the stack is unchanged by one (\"non-positioned block "
              "boxes created before and after the float box flow vertically as if the float did not exist\"), so "
              "§9.4.1's positions and §10.6.3's height are ANSWERED beside a float and §9.5.1's rules 4, 5 and 6 "
              "have the lower bound they are stated over. WHAT IS STILL REFUSED IS THE LINE-BOX HALF and it is "
              "refused in TWO places in that one file, not one: `lb_child` for a float inside the run, and a "
              "CONTEXT check for every other float that reaches these lines. So the remaining order is §9.5's "
              "shortening BEFORE §9.5.1's placement for a float whose containing block has inline content, and "
              "§9.5.1 FIRST for one whose containing block holds only block-level boxes — two entry points rather "
              "than one blocked chain. RETIREMENT: this paragraph goes when a float's own flow position is "
              "ANSWERED rather than refused, since the ordering it corrects is then visible in the tree",
              box_subject(el, nbuf, sizeof nbuf), box_subject_computed(el, "float", vbuf, sizeof vbuf));
    fp_require_horizontal_tb(el);
    /* §9.4's two normal-flow contexts are alternatives chosen by level, so a box on a line leaves through
       §9.4.2 here, like floats and out-of-flow boxes above. */
    if (fp_is_on_a_line_box(el)) return fp_line_box_origin(el, out);
    /* CSS 2.1 §17.5 Visual layout of table contents places row, cell and row group boxes, which must not fall
       through to §9.4.1's rules; all go through `fp_table_grid_origin`. The three row group displays are
       spelled as strings, like the tests above, rather than routed through `table_box_kind_is_row_group`'s
       enum. The other three table-internal boxes are refused in `fp_require_placeable`. */
    if (fp_computed_is(el, "display", "table-row")) { *out = fp_table_row_origin(el); return true; }
    if (fp_computed_is(el, "display", "table-cell")) { *out = fp_table_cell_origin(el); return true; }
    if (fp_computed_is(el, "display", "table-row-group") ||
        fp_computed_is(el, "display", "table-header-group") ||
        fp_computed_is(el, "display", "table-footer-group"))
        return (*out = fp_table_row_group_origin(el), true);
    if (fp_computed_is(el, "display", "table-caption")) return fp_caption_origin(el, out);
    fp_require_placeable(el);

    /* §10.1's first case, §9.4.1's base case: the root's containing block is the ICB, "anchored at the canvas
       origin", so both rules reduce to its own margins. With no preceding sibling it begins at the ICB's top,
       and "margins of the root element's box do not collapse" (§8.3.1 'Collapsing margins'), so the vertical
       distance is its own used `margin-top`. */
    if (fp_is_root(n)) {
        p.x = fp_left_offset(el, fp_icb_width(el));
        /* §9.4.1's vertical rule at the top of the containing block, with §8.3.1's root-element exception. */
        p.y = used_value_px(el, "margin-top");
        *out = p;
        return true;
    }

    /* §10.1's second case, §9.4.1's inductive step: the containing block is "the content edge of the nearest
       block container ancestor box", so the origin is that box's origin plus its leading border and padding
       (§8.1) plus §9.4.1's offsets: horizontally the same touching rule over the containing block's width,
       vertically core/layout/block_flow.h's stack (preceding in-flow siblings' used heights with §8.3.1's
       collapsing). It terminates at the root, whose containing block (the ICB) is no element's box. */
    cb = used_value_containing_block(el);
    DCHECKF(cb != NULL,
           "%s, parent %s: "
           "CSS 2.1 §10.1 answered NULL — its first case, the initial containing block — for an element that is "
           "not the root. The two tests are the same test (a node whose parent is the Document), so they have "
           "come apart",
           box_subject(el, nbuf, sizeof nbuf), box_subject_node(n->parent, vbuf, sizeof vbuf));
    if (!flow_border_box_origin_ask(cb, &o) || !block_flow_child_top_ask(el, &top)) return false;
    p.x = css_px_add(css_px_add(o.x, used_value_leading_edge_px(cb, false)),
                     fp_left_offset(el, used_value_containing_block_width(el)));
    p.y = css_px_add(css_px_add(o.y, used_value_leading_edge_px(cb, true)), top);
    /* The one arm with a second route to its number, reported through `*cb_out`; the others leave it NULL. */
    *cb_out = cb;
    *out = p;
    return true;
}

/* CSS 2 §8.1 "Box dimensions"' border-box origin, asked of the pass first. The cost removed is a recursion:
   §10.1's second case climbs the containing-block chain per ask, quadratic in depth over a document; within a
   pass a climb stops at the first ancestor already derived (core/layout/flow_placement.h explains why
   remembering suits a recursion but not a walk). Outside a pass nothing is held, so `offsetTop` between two
   renders climbs afresh. The census counts an ask at the attempt that answers it: a "not yet" attempt is
   asked again. */
bool flow_border_box_origin_ask(lxb_dom_element_t *el, FlowPoint *out)
{
    FlowPoint                p;
    const lxb_dom_element_t *cb = NULL;

    DCHECK(el != NULL && out != NULL, "a border box's position was asked for with no element or no out");
    if (flow_placement_origin_peek(el, &p, &cb)) {
        (void)flow_placement_origin_ask(el, &p, &cb);
        DCHECKF(fp_origin_agrees(el, cb, p),
                "CSS 2 §10.1's second case puts this box's border-box origin at its containing block's "
                "origin plus that block's leading border and padding plus §9.4.1's stack position, and CSS "
                "2.2 §9.4.3 \"Relative positioning\" then SHIFTS that point by this box's own used `left` and "
                "`top`; the point this pass recorded for it no longer satisfies that equation — recorded "
                "(%g, %g). The FOUR operands come from four places: the containing block's own recorded "
                "point, core/layout/block_flow.c's placement record, the CASCADE, and "
                "core/layout/used_value.h's §9.4.3 pair. So this is one of two things "
                "and they take opposite work. EITHER the record answered for a box it does not hold — read "
                "core/layout/flow_placement.c's probe, which is the only code that decides which entry a key "
                "reaches — OR a computed value moved INSIDE the pass, which is the axis the pass's tree "
                "version cannot see and which solver/dom_cow.c's attribute chokepoint exists to make "
                "impossible. If that chokepoint did not fire, the change arrived by a route it does not own "
                "and the route is the finding",
                p.x.px, p.y.px);
        *out = p;
        return true;
    }
    if (!fp_border_box_origin_compute(el, &cb, &p)) return false;
    (void)flow_placement_origin_ask(el, &p, NULL);
    /* CSS 2.2 §9.4.3 "Relative positioning", applied once here because it shifts any placement: "Once a box has
       been laid out according to the normal flow or floated, it may be shifted relative to this position."
       The record carries the shifted point, so descendants inherit the translation through §10.1's second
       case, and a `position: absolute` child of a relative parent moves too (`fp_abs_cb_leading_ask` reads that
       parent's `flow_padding_box_origin`).
       "Offsetting a box (B1) in this way has no effect on the box (B2) that follows" holds because
       `block_flow_child_top_ask` derives from used heights and §8.3.1 and calls nothing in this file; a stack walk
       that read a border-box origin would break it without this file changing.
       Table-internal boxes take the term too: §9.3.1 says "The effect of 'position:relative' on
       table-row-group, table-header-group, table-footer-group, table-row, table-column-group, table-column,
       table-cell, and table-caption elements is undefined", which permits it, as browsers do. */
    p.x = css_px_add(p.x, fp_relative_offset(el, false));
    p.y = css_px_add(p.y, fp_relative_offset(el, true));
    flow_placement_origin_record(el, p, cb);
    *out = p;
    return true;
}

FlowPoint flow_border_box_origin(lxb_dom_element_t *el)
{
    LayoutJobDrive d;
    FlowPoint p;

    layout_job_drive_begin(&d);
    while (!flow_border_box_origin_ask(el, &p)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return p;
}

/* CSS 2 §8.1 "Box dimensions"' padding box origin: the border box origin moved inward by the leading border on
   each axis, derived from `flow_border_box_origin` so an unplaceable box crashes there first. */
FlowPoint flow_table_box_origin(lxb_dom_element_t *table)
{
    FlowPoint p;

    DCHECK(!table_wrapper_owns_property("border-top-width") && !table_wrapper_owns_property("padding-top") &&
               !table_wrapper_owns_property("padding-left") && table_wrapper_owns_property("margin-top"),
           "CSS 2.1 §17.4 Tables in the visual formatting model's declaration split is what puts the table "
           "box's border edge on the table wrapper box's content edge: the wrapper takes the table "
           "element's `margin-*` and the INITIAL value of its border and padding, and the table box takes the "
           "border and padding and the initial `margin-*`. This arithmetic reads the table element's own border "
           "and padding as the TABLE BOX's and adds nothing for the wrapper — so if that list has moved, the "
           "two boxes no longer coincide and every box below is offset by a border nobody accounted for");
    /* §17.4's wrapper is what §9.4.1 stacks, so the table element's origin is an ordinary recursion, and the
       table box sits below the wrapper's top captions on the wrapper's own stack. */
    p = flow_border_box_origin(table);
    p.y = css_px_add(p.y, block_flow_table_box_top(table));
    return p;
}

FlowPoint flow_padding_box_origin(lxb_dom_element_t *el)
{
    LayoutJobDrive d;
    FlowPoint p;

    layout_job_drive_begin(&d);
    while (!fp_padding_box_origin_ask(el, &p)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return p;
}
