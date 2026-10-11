/* CSS 2.1 §17.5.3 Table height algorithms, over core/layout/table_grid.h's grid and core/layout/table_box.h's
   rows. See table_height.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/css/css_property_applies.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/replaced_element.h"
#include "core/layout/table_border_collapse.h"
#include "core/layout/table_box.h"
#include "core/layout/table_grid.h"
#include "core/layout/table_height.h"
#include "core/layout/table_wrapper.h"
#include "core/layout/used_value.h"

static char *th_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

/* ---- §17.6 Borders' two models, asked once at the entry ---------------------------------------------------
   The vertical twin of core/layout/table_width.c's split, asked here too so a table whose height is asked
   without its width still meets it. The cell spacing in "the row heights plus any cell spacing or borders" is
   §17.6.1's vertical `border-spacing`, which §17.6.2 gives no meaning ("Borders are centered on the grid lines
   between the cells"). Row heights need no arm: `th_cell_vertical_edges_pair` answers either model. */
static CssPx th_used_spacing(CssPx vertical, bool collapsing)
{
    return collapsing ? css_px(0.0) : vertical;
}

/* ---- §17.6.1's cell spacing on the block axis -------------------------------------------------------------
   The twin of `tw_spacing_total`: between rows the property, and at the top and bottom "the table's padding for
   that side, plus the relevant border spacing distance", so `nrows + 1` times, padding excluded. A table with
   no rows has no cell borders to separate, so zero. */
static CssPx th_spacing_total(CssPx spacing, size_t nrows)
{
    if (nrows == 0) return css_px(0.0);
    return css_px_scale(spacing, (double) (nrows + 1));
}

/* ---- One cell's vertical padding and border --------------------------------------------------------------
   See table_height.h. The sides are reported separately because §17.5.3's cell baseline needs them apart ("the
   baseline is the bottom of content edge of the cell box" is the box height less the bottom pair). */
static void th_cell_vertical_edges_pair(lxb_dom_element_t *cell, CssPx *top, CssPx *bottom)
{
    static const char *const PADDINGS[2] = { "padding-top", "padding-bottom" };
    static const char *const BORDERS[2] = { "border-top-width", "border-bottom-width" };
    CssPx side[2];
    TableCollapsedEdges charge;
    lxb_dom_element_t *table;
    bool collapsing;
    char nbuf[160];
    int i;

    /* A cell box is its border box only because vertical margins do not apply to it (CSS 2.1 §8.3 "Margin
       properties"), asked of the component that owns that line. */
    DCHECK(!css_property_applies(cell, "margin-top") && !css_property_applies(cell, "margin-bottom"),
           "CSS 2.1 §17.5.3 Table height algorithms took a cell box to be its BORDER box, and CSS 2.1 §8.3 "
           "\"Margin properties\"' Applies-to line now says the vertical margins DO apply to this box — so a "
           "cell's contribution to its row has a term this walk is not adding, and §17.5's own \"the edges "
           "coincide with the border edges of cells\" no longer places the row where this component puts it");
    /* `border-collapse` applies to "'table' and 'inline-table' elements", so the table's model covers both
       sides. */
    table = table_box_table_of(cell);
    collapsing = table_border_collapse_selected(table);
    charge.top = css_px(0.0);
    charge.right = css_px(0.0);
    charge.bottom = css_px(0.0);
    charge.left = css_px(0.0);
    if (collapsing) charge = table_collapsed_cell_edges(cell);
    for (i = 0; i < 2; i++) {
        CssLength pad = css_computed_length(cell, PADDINGS[i]);
        CssLength bor;

        if (pad.kind != CSS_LENGTH_ABSOLUTE)
            DFAILF("%s: a table cell's `%s` is not an absolute length, and CSS 2.1 §17.5.3 Table height "
                   "algorithms cannot resolve it. CSS 2.1 §8.4 \"Padding properties: 'padding-top', "
                   "'padding-right', 'padding-bottom', 'padding-left', and 'padding'\" refers a padding "
                   "percentage to the containing block's WIDTH — \"even for 'padding-top' and "
                   "'padding-bottom'\" — and then names THIS case as the one it does not define: \"If the "
                   "containing block's width depends on this element, then the resulting layout is undefined "
                   "in CSS 2.1.\" For a cell that rectangle is the TABLE BOX's content width, and CSS 2.1 "
                   "§17.5.2.2 Automatic table layout's MCW makes it depend on this very cell — so the "
                   "resolution is a CYCLE and not a missing lookup, and it is the identical cycle "
                   "core/layout/table_column_width.c refuses on the other axis. A KEYWORD cannot arrive here "
                   "at all: CSS 2.1 §8.4's `Value:` line is `<padding-width> | inherit` and its `Computed "
                   "value:` line is \"the percentage as specified or the absolute length\"",
                   box_subject(cell, nbuf, sizeof nbuf), PADDINGS[i]);
        if (collapsing) {
            /* §17.6.2 replaces the cell's own border with half the grid-line resolution: top, then bottom. */
            side[i] = css_px_add(used_value_px(cell, PADDINGS[i]), i == 0 ? charge.top : charge.bottom);
        } else {
            bor = css_computed_length(cell, BORDERS[i]);
            DCHECK(bor.kind == CSS_LENGTH_ABSOLUTE,
                   "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 "
                   "§3.3 \"Line Thickness: the border-width properties\"' `Computed value:` line is `absolute "
                   "length, snapped as a border width` and every arm of that derivation produces one, so a "
                   "percentage or a keyword here is a rule that did not run");
            side[i] = css_px_add(used_value_px(cell, PADDINGS[i]), bor.px);
        }
        DCHECK(side[i].px >= 0.0,
               "CSS 2.1 §17.5.3's cell edges came out NEGATIVE on one side. The padding is `used_value_px`'s, "
               "floored at zero by css-values-4 §9.1's clamp, and the border term is a computed `<line-width>` "
               "core/css/css_computed_value.c asserts non-negative or §17.6.2's half of one, so this is a term "
               "that skipped both");
    }
    *top = side[0];
    *bottom = side[1];
}

CssPx table_cell_vertical_edges(lxb_dom_element_t *cell)
{
    CssPx top, bottom;

    DCHECK(cell != NULL, "a cell's vertical edges were asked for with no element");
    th_cell_vertical_edges_pair(cell, &top, &bottom);
    return css_px_add(top, bottom);
}

/* ---- §17.5.3's cell box height ------------------------------------------------------------------------------
   "In CSS 2.1, the height of a cell box is the minimum height required by the content": CSS 2.1 §10.6.3's
   content height over the cell's used width, plus its vertical edges. The width arrives through
   core/layout/block_flow.h's §10.1 resolution, which core/layout/used_value.c answers for a cell from §17.5.2.
   The declared `height` is not read: "it does not increase the height of the cell box", so it is a term of
   the row's maximum only. §10.7's clamp is not applied ("the effect of 'min-height' and 'max-height' on …
   table cells … is undefined"). */
static CssPx th_cell_box_height(lxb_dom_element_t *cell)
{
    ReplacedElement rep = replaced_element_of(cell);
    char nbuf[160];

    if (rep.replaced)
        DFAILF("%s: CSS 2.1 §17.5.3 Table height algorithms gives a cell box's height as \"the minimum height "
               "required by the content\", which for a block container is CSS 2.1 §10.6.3's walk — and this "
               "cell is a REPLACED element, whose height is §10.6.2's over its own natural dimensions and not "
               "a walk over children it does not have. CSS 2.1 §17.2 The CSS table model is what puts it here "
               "at all: \"Replaced elements with these 'display' values are treated as their given display "
               "types during layout.\" BUILD the arm: the cell box's height is then §10.6.2's used height "
               "(core/layout/replaced_element.h's natural dimensions, or CSS 2.1 §10.6.2's 300x150 default) "
               "plus this cell's own vertical padding and border, and every other term of §17.5.3's row "
               "maximum is unchanged. It is NOT reachable through core/layout/used_value.c's replaced arm, "
               "which crashes for a replaced TABLE BOX one classification up and never sees a cell",
               box_subject(cell, nbuf, sizeof nbuf));
    return css_px_add(block_flow_auto_height(cell), table_cell_vertical_edges(cell));
}

/* ---- §17.5.3's four cell alignments -------------------------------------------------------------------------
   "In the context of tables, values for 'vertical-align' have the following meanings: baseline / top / bottom
   / middle", with "sub, super, text-top, text-bottom, <length>, <percentage> … These values do not apply to
   cells; the cell is aligned at the baseline instead." Read through css-inline-3's longhands in §4.2's mapping
   order (see table_height.h); everything else is the baseline catch-all. */
typedef enum {
    TH_ALIGN_BASELINE = 0,
    TH_ALIGN_TOP,
    TH_ALIGN_BOTTOM,
    TH_ALIGN_MIDDLE
} ThAlign;

static ThAlign th_cell_align(lxb_dom_element_t *cell)
{
    CssLength shift = css_computed_length(cell, "baseline-shift");
    char *ab;
    bool middle;

    if (css_length_is_keyword(shift, CSS_KEYWORD_TOP)) return TH_ALIGN_TOP;
    if (css_length_is_keyword(shift, CSS_KEYWORD_BOTTOM)) return TH_ALIGN_BOTTOM;
    ab = th_computed(cell, "alignment-baseline");
    middle = strcmp(ab, "middle") == 0;
    free(ab);
    return middle ? TH_ALIGN_MIDDLE : TH_ALIGN_BASELINE;
}

/* §17.5.3's cell baseline: "The baseline of a cell is the baseline of the first in-flow line box in the cell,
   or the first in-flow table-row in the cell, whichever comes first. If there is no such line box or
   table-row, the baseline is the bottom of content edge of the cell box." Measured from the cell box's top
   border edge, the frame of the row's "maximum distance between the top of the cell box and the baseline".
   A nested table reached first crashes inside core/layout/block_flow.c, so `true` is the first arm and `false`
   the third. */
static CssPx th_cell_baseline(lxb_dom_element_t *cell, CssPx box_height)
{
    CssPx top, bottom, inner = css_px(0.0);

    th_cell_vertical_edges_pair(cell, &top, &bottom);
    if (block_flow_first_line_box_baseline(cell, &inner)) return css_px_add(top, inner);
    return css_px_sub(box_height, bottom);
}

/* ---- §17.5.3's MIN ------------------------------------------------------------------------------------------
   The alignment procedure as two maxima; see table_height.h. `cells` are those anchored in this row and one row
   high; row-spanning cells go through the constraint pass in `table_heights`. */
typedef struct {
    lxb_dom_element_t *element;
    CssPx              height;
    ThAlign            align;
} ThRowCell;

/* Whether MIN over `cells` measures baselines: with at most one baseline cell, MIN is `max(H)` by the
   procedure itself (one cell's `A + (H - A)` is its `H`), so no baseline is measured. `table_heights_ask`
   asks the baselines under the same test before `th_row_min` reads them. */
static bool th_row_reads_baselines(const ThRowCell *cells, size_t n)
{
    size_t i, nbaseline = 0;

    for (i = 0; i < n; i++)
        if (cells[i].align == TH_ALIGN_BASELINE) nbaseline++;
    return nbaseline > 1;
}

static CssPx th_row_min(const ThRowCell *cells, size_t n)
{
    CssPx tallest = css_px(0.0), above = css_px(0.0), below = css_px(0.0);
    size_t i;

    for (i = 0; i < n; i++) tallest = css_px_max(tallest, cells[i].height);
    if (!th_row_reads_baselines(cells, n)) return tallest;
    for (i = 0; i < n; i++) {
        CssPx a;

        if (cells[i].align != TH_ALIGN_BASELINE) continue;
        a = th_cell_baseline(cells[i].element, cells[i].height);
        DCHECK(a.px >= -1e-9 && a.px <= cells[i].height.px + 1e-9,
               "CSS 2.1 §17.5.3 Table height algorithms put a cell's BASELINE outside its own cell box. The "
               "section measures it from the top of the cell box — either to \"the baseline of the first "
               "in-flow line box\", which core/layout/block_flow.h reports as an offset inside this box's "
               "content edge, or to \"the bottom of content edge of the cell box\", which is the box height "
               "less its bottom padding and border. Both are inside the border box by construction, so a "
               "baseline outside it is a frame conversion that added the wrong edge — and the row height built "
               "on it would be a real distance measured from the wrong origin");
        above = css_px_max(above, a);
        below = css_px_max(below, css_px_sub(cells[i].height, a));
    }
    /* Step 1 establishes the row's baseline at `above` and step 3's provisional height reaches `above + below`;
       steps 2 and 3 then raise it to any taller cell of the other three alignments, which `tallest` holds. */
    return css_px_max(css_px_add(above, below), tallest);
}

/* ---- §17.5.3's three terms ---------------------------------------------------------------------------------
   "the maximum of the row's computed 'height', the computed 'height' of each cell in the row, and the minimum
   height (MIN) required by the cells": one of the first two terms, off one element.
   `auto` contributes nothing ("the row height used for layout is MIN"). A percentage contributes nothing too, a
   recorded choice ("CSS 2.1 does not define how the height of table cells and table rows is calculated when
   their height is specified using percentage values"): it is `auto`'s arm, and resolving against the table's
   height would be a cycle. An anonymous row or cell (no element) has the initial `height: auto`. */
static CssPx th_declared_term(lxb_dom_element_t *el)
{
    CssLength h;

    if (el == NULL) return css_px(0.0);
    h = css_computed_length(el, "height");
    if (h.kind == CSS_LENGTH_ABSOLUTE) return css_px_max(h.px, css_px(0.0));
    return css_px(0.0);
}

/* ---- The table's own declared height, which §17.5.3 makes a minimum ---------------------------------------
   "Any other value is treated as a minimum height." `auto` is css-sizing-3 §3.2.1's behaves-as-auto, per its
   note that CSS2 conditions on a computed `auto` "should be interpreted as meaning behaves as auto".
   A resolving percentage uses CSS 2.1 §10.5 "Content height: the 'height' property" ("calculated with respect
   to the height of the generated box's containing block"), and §17.4: "Percentages on 'width' and 'height' on
   the table are relative to the table wrapper box's containing block, not the table wrapper box itself." That
   is §10.1's walk from the table's parent, which never asks this table for a height; `used_value_px(table,
   "height")` would recurse into `table_heights` and must not be used. (CSS 2 §9.2.1.1 "Anonymous block boxes"
   reaches the same ancestor: "the closest non-anonymous ancestor box is used instead".)
   The behaves-as-auto test above and the definiteness of that basis are complementary by construction (on
   §10.1's second case the definiteness gate is that predicate; elsewhere one tag decides both), so the assert
   below is a two-sided check. */
static bool th_declared_minimum(lxb_dom_element_t *table, CssPx *out)
{
    CssLength h;
    /* Initialised so the release arm under the assert is defined: a zero basis resolves to zero, which
       §17.5.3 reads like `auto` (the row sum is never negative). */
    CssPx basis = css_px(0.0);
    bool definite;
    char nbuf[160];

    *out = css_px(0.0);
    if (used_value_height_behaves_as_auto(table)) return false;
    h = css_computed_length(table, "height");
    if (h.kind == CSS_LENGTH_ABSOLUTE) {
        *out = css_px_max(h.px, css_px(0.0));
        return true;
    }
    definite = used_value_containing_block_height(table, &basis);
    DCHECKF(definite,
            "%s: a table box whose `height` is a PERCENTAGE that css-sizing-3 §3.2.1 \"“Behaving as "
            "auto”\" says does NOT behave as auto, standing over a containing block that CSS 2.1 §10.1 "
            "\"Definition of 'containing block'\"' walk gives NO height. The two answers are complementary "
            "BY CONSTRUCTION rather than by agreement, so this is the construction having come apart and not "
            "a page: on §10.1's SECOND case the height entry's own last gate IS the behaves-as-auto "
            "predicate asked of the containing block, and every other case is one tag read twice — the "
            "initial containing block and the viewport are definite and never behave as auto, and "
            "CSS 2 §17.4 Tables in the visual formatting model's wrapper is the reverse of both, which "
            "a table box never reaches because §10.1's walk starts at its parent. READ WHICH ARM "
            "`uv_cb` TOOK before reading either predicate: a tag the two classify differently is the "
            "finding, and a second test beside them here would only hide which one moved",
            box_subject(table, nbuf, sizeof nbuf));
    /* §10.5's resolution, then css-values-4 §9.1 "Numeric Functions"' clamp, as used_value.c's percentage arms
       do: "Negative values for 'height' are illegal", a negative literal is dropped (css-values-4 §5.1 "Range
       Restrictions and Range Definition Notation"), so a negative here is a math function's result. */
    *out = css_px_max(css_length_resolve_pct(h, basis), css_px(0.0));
    return true;
}

bool table_heights_ask(lxb_dom_element_t *table, const TableGrid *grid)
{
    ThRowCell *bucket;
    size_t r, i, n;
    bool ready = true;

    DCHECK(table != NULL && grid != NULL, "CSS 2.1 §17.5.3's inputs were asked with no table or no grid");
    if (grid->ncells == 0) return true;
    bucket = calloc(grid->ncells, sizeof(*bucket));
    CHECK(bucket != NULL, "out of memory collecting a row's cells for CSS 2.1 §17.5.3");
    /* Every cell box height `th_cell_box_height` reads, in the order `table_heights` reads them. A replaced
       cell is left to that function's own refusal. */
    for (i = 0; i < grid->ncells && ready; i++) {
        lxb_dom_element_t *cell = grid->cells[i].element;
        CssPx unused;

        if (cell == NULL || replaced_element_of(cell).replaced) continue;
        ready = block_flow_auto_height_ask(cell, &unused);
    }
    /* The baselines `th_row_min` measures, row by row under `th_row_reads_baselines`. */
    for (r = 0; r < grid->nrows && ready; r++) {
        n = 0;
        for (i = 0; i < grid->ncells; i++) {
            const TableGridCell *c = &grid->cells[i];

            if (c->row != r || c->rowspan != 1) continue;
            bucket[n].element = c->element;
            bucket[n].align = c->element == NULL ? TH_ALIGN_BASELINE : th_cell_align(c->element);
            n++;
        }
        if (!th_row_reads_baselines(bucket, n)) continue;
        for (i = 0; i < n && ready; i++) {
            CssPx unused;
            bool has;

            if (bucket[i].align != TH_ALIGN_BASELINE || bucket[i].element == NULL) continue;
            ready = block_flow_first_line_box_baseline_ask(bucket[i].element, &unused, &has);
        }
    }
    free(bucket);
    return ready;
}

void table_heights(lxb_dom_element_t *table, const TableGrid *grid, TableUsedHeights *out)
{
    CssBorderSpacing spacing;
    TableBoxRow *rows = NULL;
    ThRowCell *bucket = NULL;
    CssPx used_spacing, declared = css_px(0.0), sum, check_sum, slack;
    CssDisplay display;
    bool is_table_box, has_declared, collapsing;
    size_t nrows, r, i;
    char nbuf[160];

    DCHECK(table != NULL && grid != NULL && out != NULL,
           "CSS 2.1 §17.5.3's table height was asked for with no element, no grid or nowhere to put it. The "
           "grid is the OPERAND every row below is stated over, and nothing before core/layout/table_grid.h "
           "says which cells are anchored in which row or how many rows each covers");
    DCHECK(grid->cells != NULL || grid->ncells == 0,
           "CSS 2.1 §17.5.3 was handed a grid with no cell array and a non-zero cell count — `table_grid_build` "
           "stores NULL only for a table whose rows generate no cell, so the two have been carried apart since "
           "it answered");
    display = css_computed_display(table);
    is_table_box = table_box_kind_generates_table_box(table_box_kind(display));
    if (!is_table_box)
        DFAILF("%s: CSS 2.1 §17.5.3 Table height algorithms is stated over a TABLE box — \"The height of a "
               "table is given by the 'height' property for the 'table' or 'inline-table' element\" — and this "
               "box's computed `display` (printed above) is a different one of CSS 2.1 §17.2 The CSS table "
               "model's box types. The height of a table-internal box is NOT this question and must not be "
               "answered from it: a ROW's is one entry of `TableUsedHeights.rows`, a CELL's is the rows its "
               "rectangle covers (`table_cell_used_border_box_height` below), a ROW GROUP's is the case "
               "§17.5.3 declines outright (\"CSS 2.1 does not define the meaning of 'height' on row groups\"), "
               "and a CAPTION is not in the table box at all — §17.4 Tables in the visual formatting model "
               "renders it as a normal block box in the WRAPPER, so §10.6.3 owns its height. ROUTE the caller "
               "to the table box above this one (core/layout/table_box.h's `table_box_table_of`) and ask it "
               "for the rows the caller wants",
               box_subject(table, nbuf, sizeof nbuf));
    /* §17.6's model, asked once for the spacing; each cell's edges ask the same table in
       `th_cell_vertical_edges_pair`. */
    collapsing = table_border_collapse_selected(table);
    /* §17.6.1's `Computed value: two absolute lengths`, answered whole by core/css/css_computed_value.h. Only
       the vertical half is a height term; the horizontal half is CSS 2.1 §17.5.2 Table width algorithms: the
       'table-layout' property's. */
    spacing = css_computed_border_spacing(table);
    DCHECK(spacing.vertical.px >= 0.0,
           "CSS 2.1 §17.6.1 The separated borders model states \"Lengths may not be negative\" of "
           "`border-spacing`, and core/css/css_computed_value.c asserts that where it computes the pair — so a "
           "negative reaching here is arithmetic that lost a sign rather than a document");
    nrows = table_box_rows(table, &rows);
    DCHECK(nrows == grid->nrows,
           "CSS 2.1 §17.5 Visual layout of table contents' rule 1 makes the grid's row count the table's own "
           "row count — \"the table occupies exactly as many grid rows as there are row elements\" — and "
           "core/layout/table_grid.h states outright that it takes that number from `table_box_rows` rather "
           "than re-deriving it. Two different counts here means the two walks ran over different trees, and "
           "every row index below would name a different row in each");
    out->nrows = nrows;
    used_spacing = th_used_spacing(spacing.vertical, collapsing);
    out->spacing = used_spacing;
    out->rows = nrows == 0 ? NULL : calloc(nrows, sizeof(*out->rows));
    CHECK(nrows == 0 || out->rows != NULL, "out of memory allocating CSS 2.1 §17.5.3's row heights");
    bucket = grid->ncells == 0 ? NULL : calloc(grid->ncells, sizeof(*bucket));
    CHECK(grid->ncells == 0 || bucket != NULL, "out of memory collecting a row's cells for CSS 2.1 §17.5.3");
    for (r = 0; r < nrows; r++) {
        CssPx term = th_declared_term(rows[r].element);
        size_t n = 0;

        /* "each cell in the row" means the cells anchored here (`TableGridCell.row`, "the row specified by the
           cell's parent") and one row high; a cell reaching down from above goes through the constraint pass. */
        for (i = 0; i < grid->ncells; i++) {
            const TableGridCell *c = &grid->cells[i];

            if (c->row != r || c->rowspan != 1) continue;
            DCHECK(n < grid->ncells, "more cells were collected for one row than the grid holds");
            bucket[n].element = c->element;
            bucket[n].height = c->element == NULL ? css_px(0.0) : th_cell_box_height(c->element);
            /* An anonymous cell has initial values (CSS 2 §9.2.1.1): `vertical-align: baseline` (css-inline-3
               §4.2's initial value) and an empty, zero-height box. */
            bucket[n].align = c->element == NULL ? TH_ALIGN_BASELINE : th_cell_align(c->element);
            if (c->element != NULL) term = css_px_max(term, th_declared_term(c->element));
            n++;
        }
        out->rows[r] = css_px_max(term, th_row_min(bucket, n));
        DCHECK(out->rows[r].px >= 0.0,
               "CSS 2.1 §17.5.3 Table height algorithms gave a grid row a NEGATIVE used height. Every term is "
               "a declared length floored at zero, a content height core/layout/block_flow.c floors at zero, a "
               "padding, a border width, or a maximum of those — so a negative here is arithmetic that lost a "
               "sign, and the rectangle it would put through CSSOM VIEW is one no reader can tell from a "
               "measured one");
    }
    /* ---- §17.5.3's spanning constraint --------------------------------------------------------------------------
       "CSS 2.1 does not specify how cells that span more than one row affect row height calculations except
       that the sum of the row heights involved must be great enough to encompass the cell spanning the rows."
       A floor to satisfy; the recorded choice puts the whole deficit on the last spanned row, so rows above
       stay where unspanned rows put them. One sweep in grid order suffices: steps only increase rows and each
       re-reads the array, and the boundary assert re-checks every cell. A spanning cell's declared `height` is
       folded in here because the row maximum is over cells in one row. */
    for (i = 0; i < grid->ncells; i++) {
        const TableGridCell *c = &grid->cells[i];
        CssPx need, have;
        size_t k;

        if (c->rowspan == 1) continue;
        DCHECK(c->row + c->rowspan <= nrows,
               "CSS 2.1 §17.5 Visual layout of table contents' grid placed a cell reaching past the last row "
               "the table has. Rule 1 makes the row count the number of row elements — \"the table occupies "
               "exactly as many grid rows as there are row elements\" — and the section's own last placement "
               "rule shortens a `rowspan` that would overflow it: \"A cell box cannot extend beyond the last "
               "row box of a table or row group; the user agents must shorten it until it fits.\" So a cell "
               "past the end is that shortening having been lost between the placement and this read");
        /* Rule 6 binds a cell to its row group too, checked against core/layout/table_grid.h's runs. The lookup is
           under the dev guard, not in the DCHECK condition, because it is a walk with an abort in it. */
#if APICLIENT_DEV
        {
            const TableGridRowGroup *run = table_grid_group_of_row(grid, c->row);

            DCHECK(c->row + c->rowspan <= run->first + run->nrows,
                   "CSS 2.1 §17.5 Visual layout of table contents' rule 6 binds a cell to its ROW GROUP and "
                   "not only to the table — \"A cell box cannot extend beyond the last row box of a table or "
                   "row group; the user agents must shorten it until it fits\" — and this cell reaches past "
                   "the last grid row of the run its anchor row is in. core/layout/table_grid.c applies that "
                   "clamp against the same runs this reads, so a cell past its group's end is that clamp "
                   "having been computed against a different partition of the rows, and the rows that would "
                   "encompass it below belong to a row group it is not in");
        }
#endif
        need = c->element == NULL ? css_px(0.0)
                                  : css_px_max(th_cell_box_height(c->element), th_declared_term(c->element));
        have = css_px_scale(used_spacing, (double) (c->rowspan - 1));
        for (k = 0; k < c->rowspan; k++) have = css_px_add(have, out->rows[c->row + k]);
        if (need.px > have.px)
            out->rows[c->row + c->rowspan - 1] =
                css_px_add(out->rows[c->row + c->rowspan - 1], css_px_sub(need, have));
    }
    sum = th_spacing_total(used_spacing, nrows);
    for (r = 0; r < nrows; r++) sum = css_px_add(sum, out->rows[r]);
    /* ---- §17.5's last paragraph over the row groups, asserted against the sum above -------------------------
       The gaps "between the rows, columns, row groups or column groups" are one set, so one spacing stands
       between two groups and §17.5.3's sum needs no group term. Each run's top must be one spacing below the
       previous run's bottom, and the last bottom plus one spacing must be the sum; this catches a second
       spacing at a group boundary, otherwise indistinguishable from a taller table. Skipped for no rows (no
       cell borders, no runs). The whole block is dev-only because the comparison sum exists only for it. */
#if APICLIENT_DEV
    if (nrows != 0) {
        CssPx run_bottom = css_px(0.0), run_top, run_expect, run_slack;

        for (i = 0; i < grid->ngroups; i++) {
            run_top = table_row_used_block_offset(out, grid->groups[i].first);
            run_expect = i == 0 ? used_spacing : css_px_add(run_bottom, used_spacing);
            run_slack = css_px_sub(run_top, run_expect);
            DCHECK(run_slack.px <= 1e-9 * (1.0 + (run_top.px < 0.0 ? -run_top.px : run_top.px)) &&
                       run_slack.px >= -1e-9 * (1.0 + (run_top.px < 0.0 ? -run_top.px : run_top.px)),
                   "CSS 2.1 §17.5 Visual layout of table contents' row groups do not TILE the block axis. "
                   "§17.5's last paragraph names the gaps \"between the rows, columns, row groups or column "
                   "groups\" as ONE set corresponding to 'border-spacing', so exactly one vertical spacing "
                   "stands between the bottom of one row group and the top of the next — the same single "
                   "spacing that stands between the two rows meeting there — and one stands between the "
                   "table box's content edge and the first. This run starts somewhere else, so either the "
                   "runs core/layout/table_grid.h reports no longer partition the rows this answer was "
                   "computed over, or a spacing is being counted twice at a group boundary. The second is "
                   "invisible in every document, since a row group box one spacing too tall looks exactly "
                   "like a table one spacing taller");
            run_bottom = css_px_add(run_top, table_row_group_used_extent(out, &grid->groups[i]));
        }
        run_slack = css_px_sub(css_px_add(run_bottom, used_spacing), sum);
        DCHECK(run_slack.px <= 1e-9 * (1.0 + (sum.px < 0.0 ? -sum.px : sum.px)) &&
                   run_slack.px >= -1e-9 * (1.0 + (sum.px < 0.0 ? -sum.px : sum.px)),
               "CSS 2.1 §17.5 Visual layout of table contents' row groups do not add up to the table CSS 2.1 "
               "§17.5.3 Table height algorithms answers. The last run's bottom edge plus the one spacing "
               "§17.6.1 The separated borders model puts between the table border and the cells on the edge "
               "of the table must BE §17.5.3's sum, \"the sum of the row heights plus any cell spacing or "
               "borders\" — which has no row-group term at all, because §17.5's last paragraph makes every "
               "gap a group could contribute a gap between two rows that the sum already holds. The tiling "
               "above pins where each run STARTS; this pins where the last one ENDS, and the two together "
               "are the whole of §17.5's placement of the row groups on this axis");
    }
#endif
    has_declared = th_declared_minimum(table, &declared);
    /* The `auto` height is the sum and any other value "is treated as a minimum height", so the greater of
       the two. The surplus stands below the last row (table_height.h): every row stays where a reader measuring
       one row predicts; spreading it is equally conforming. */
    out->content = has_declared ? css_px_max(sum, declared) : sum;
    check_sum = th_spacing_total(used_spacing, out->nrows);
    for (r = 0; r < out->nrows; r++) check_sum = css_px_add(check_sum, out->rows[r]);
    slack = css_px_sub(out->content, check_sum);
    /* §17.5.3's sum, asserted as an inequality (a declared minimum may exceed the rows). A content height
       shorter than its rows would place rows outside the box sized from `content`. */
    DCHECK(slack.px >= -1e-9 * (1.0 + (out->content.px < 0.0 ? -out->content.px : out->content.px)),
           "CSS 2.1 §17.5.3 Table height algorithms makes a table's height \"the sum of the row heights plus "
           "any cell spacing or borders\", floored by a declared minimum — and this answer's content height is "
           "SMALLER than that sum. A consumer placing rows from the array and sizing the box from `content` "
           "would lay them out in two different rectangles, with every row past the first outside the box");
    for (i = 0; i < grid->ncells; i++) {
        const TableGridCell *c = &grid->cells[i];
        CssPx need, have;
        size_t k;

        if (c->rowspan == 1 || c->element == NULL) continue;
        need = css_px_max(th_cell_box_height(c->element), th_declared_term(c->element));
        have = css_px_scale(used_spacing, (double) (c->rowspan - 1));
        for (k = 0; k < c->rowspan; k++) have = css_px_add(have, out->rows[c->row + k]);
        DCHECK(have.px >= need.px - 1e-9 * (1.0 + (need.px < 0.0 ? -need.px : need.px)),
               "CSS 2.1 §17.5.3 Table height algorithms states the ONE thing it does require of a row-spanning "
               "cell — \"the sum of the row heights involved must be great enough to encompass the cell "
               "spanning the rows\" — and the rows this cell covers do not encompass it. The pass above only "
               "ever increases a row and re-reads the array per cell, so a violation here is two cells' "
               "requirements having been satisfied against different copies of the heights");
    }
    free(bucket);
    table_box_rows_free(rows, nrows);
    DCHECK(out->content.px >= 0.0,
           "CSS 2.1 §17.5.3 gave a table a NEGATIVE used height. Every operand is a non-negative row height, a "
           "non-negative spacing counted a non-negative number of times, or a declared length floored at zero");
}

/* See table_height.h for the reading the N-1 spacings encode and for why this is a BORDER-box number. */
CssPx table_cell_used_border_box_height(const TableUsedHeights *heights, const TableGridCell *cell)
{
    CssPx h;
    size_t r;

    DCHECK(heights != NULL && cell != NULL,
           "CSS 2.1 §17.5's cell height was asked for with no table height answer or no grid cell — a cell box "
           "fills the rows its rectangle covers, so neither operand has a substitute");
    DCHECK(heights->rows != NULL || heights->nrows == 0,
           "CSS 2.1 §17.5.3's answer was indexed for a cell while holding no row array and a non-zero row "
           "count — `table_heights` stores NULL only for a table with no rows, so the two have been carried "
           "apart since it answered");
    DCHECK(cell->rowspan >= 1,
           "CSS 2.1 §17.5 Visual layout of table contents' grid reported a cell covering NO row, and that "
           "section states the floor in the sentence that defines a cell's rectangle: \"Each cell is thus a "
           "rectangular box, one or more grid cells wide and high\". core/layout/table_grid.h answers that "
           "field and its own contract puts the same floor of one on it, so a zero here is that guarantee "
           "having been lost between the placement and this read");
    DCHECK(cell->row + cell->rowspan <= heights->nrows,
           "CSS 2.1 §17.5's grid placed a cell past the last row CSS 2.1 §17.5.3 Table height algorithms "
           "answered a height for. Rule 6 clips a `rowspan` to the table, so the two walks have come apart and "
           "the rows in hand belong to a grid this cell is not in");
    h = css_px_scale(heights->spacing, (double) (cell->rowspan - 1));
    for (r = 0; r < cell->rowspan; r++) h = css_px_add(h, heights->rows[cell->row + r]);
    DCHECK(h.px >= 0.0,
           "CSS 2.1 §17.5's cell height came out NEGATIVE over rows CSS 2.1 §17.5.3 asserts are each "
           "non-negative and a `border-spacing` §17.6.1 forbids to be negative");
    return h;
}

/* See table_height.h for §17.5's last paragraph in both border models, for why ONE arithmetic is right in each
   of them, and for why this is an EXTENT and never a used `height`. */
CssPx table_row_group_used_extent(const TableUsedHeights *heights, const TableGridRowGroup *group)
{
    CssPx h;
    size_t r;

    DCHECK(heights != NULL,
           "CSS 2.1 §17.5's rule 2 was asked for a row group's extent with no table height answer. Rule 2 gives "
           "the box \"the same grid cells as the rows it contains\" and nothing else — the grid rows are an "
           "index and the DISTANCE is CSS 2.1 §17.5.3 Table height algorithms', so there is no substitute "
           "operand");
    DCHECK(heights->rows != NULL || heights->nrows == 0,
           "CSS 2.1 §17.5.3's answer was indexed for a row group while holding no row array and a non-zero row "
           "count — `table_heights` stores NULL only for a table with no rows, so the two have been carried "
           "apart since it answered");
    /* An empty row group (`<tbody></tbody>`, which `table_grid_row_group_of` answers NULL for) is a recorded
       choice CSS 2.1 leaves open: §17.5's last paragraph names no edges for a box in no grid cell. The extent
       is zero, from rule 2: on this axis an extent is a function of the rows held. (On the inline axis
       used_value.c gives an empty group the whole grid row, consistently, since rule 1 makes that extent
       independent of the row count.) The alternative, no rectangle at all, would be a crash on a conforming
       document. Browsers may differ; only `getBoundingClientRect().height` on the empty `<tbody>` shows it.
       Its position is core/layout/table_grid.h's `table_grid_empty_row_group_line`, turned into a distance at
       the placement (core/layout/flow_position.c). */
    if (group == NULL) return css_px(0.0);
    DCHECK(group->nrows >= 1,
           "CSS 2.1 §17.5's rule 2 reported a row group run covering NO grid row. core/layout/table_grid.h "
           "builds a run only around a row it holds and asserts that floor where it builds them, so a zero "
           "here is not the EMPTY row group — that group has no run at all and arrives as NULL above — but "
           "that array having been carried apart since it answered");
    DCHECK(group->first + group->nrows <= heights->nrows,
           "CSS 2.1 §17.5's rule 2 placed a row group past the last row CSS 2.1 §17.5.3 Table height "
           "algorithms answered a height for. The runs partition the grid rows of the grid `table_heights` was "
           "given, and that entry answers one height per grid row of it — so the run in hand belongs to a grid "
           "these heights are not of, and every row it names is another table's");
    /* The N-1 spacings inside the group: under §17.6.1 its edges are its first and last rows' cell border edges,
       so gaps to neighbouring groups are outside. Under §17.6.2 `spacing` is zero: "the rows together exactly
       cover the table, leaving no gaps". */
    h = css_px_scale(heights->spacing, (double) (group->nrows - 1));
    for (r = 0; r < group->nrows; r++) h = css_px_add(h, heights->rows[group->first + r]);
    DCHECK(h.px >= 0.0,
           "CSS 2.1 §17.5's rule 2 gave a row group a NEGATIVE extent over rows CSS 2.1 §17.5.3 asserts are "
           "each non-negative and a `border-spacing` §17.6.1 The separated borders model forbids to be "
           "negative (\"Lengths may not be negative\")");
    return h;
}

/* See table_height.h for why this is one distance for three boxes, the leading spacing in each of CSS 2.1
   §17.6's two border models, and why it is measured from the table box's content edge. */
CssPx table_row_used_block_offset(const TableUsedHeights *heights, size_t row)
{
    CssPx y;
    size_t j;

    DCHECK(heights != NULL,
           "CSS 2.1 §17.5 Visual layout of table contents was asked where a grid row starts with no table "
           "height answer. The distance is the row heights above it and CSS 2.1 §17.5.3 Table height "
           "algorithms' cell-spacing term between them, and neither has a substitute operand");
    DCHECK(heights->rows != NULL || heights->nrows == 0,
           "CSS 2.1 §17.5.3's answer was indexed for a grid row's block-axis offset while holding no row array "
           "and a non-zero row count — `table_heights` stores NULL only for a table with no rows, so the two "
           "have been carried apart since it answered");
    DCHECK(row <= heights->nrows,
           "CSS 2.1 §17.5 Visual layout of table contents was asked where a grid LINE is for a line past the "
           "last one rule 1 places — \"the table occupies exactly as many grid rows as there are row "
           "elements\", so there are `nrows` rows and `nrows + 1` lines and this index is above both. Rules 1 "
           "and 5 index a ROW and their callers hold the stricter range at their own sites; what this entry "
           "admits at `nrows` is rule 2's union over ZERO rows, whose box stands on the line its first row "
           "would occupy. A caller wanting the distance to the BOTTOM of the last row asks for this at that "
           "row and adds its height, which is the one arithmetic that stays right when the row count is zero");
    /* One spacing before the first row and one between each adjoining pair above `row`; zero under §17.6.2,
       where grid row 0 starts at the table box's content edge. */
    y = heights->spacing;
    for (j = 0; j < row; j++) y = css_px_add(css_px_add(y, heights->rows[j]), heights->spacing);
    DCHECK(y.px >= 0.0,
           "CSS 2.1 §17.5 Visual layout of table contents placed a grid row at a NEGATIVE distance inside the "
           "table box. Every term is a row height CSS 2.1 §17.5.3 Table height algorithms asserts is "
           "non-negative or a `border-spacing` §17.6.1 The separated borders model forbids to be negative "
           "(\"Lengths may not be negative\"), so a negative here is arithmetic that lost a sign — and the box "
           "it would place sits above the table's own content edge");
    return y;
}

void table_heights_release(TableUsedHeights *h)
{
    DCHECK(h != NULL, "CSS 2.1 §17.5.3's answer was released with nothing to release");
    DCHECK(h->rows != NULL || h->nrows == 0,
           "CSS 2.1 §17.5.3's answer was released holding no row array and a non-zero row count");
    free(h->rows);
    h->rows = NULL;
    h->nrows = 0;
}
