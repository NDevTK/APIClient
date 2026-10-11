/* CSS 2.1 §17.6.2 The collapsing border model and §17.6.2.1 Border conflict resolution.
   See table_border_collapse.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/box_subject.h"
#include "core/layout/table_border_collapse.h"
#include "core/layout/table_box.h"
#include "core/layout/table_column_box.h"
#include "core/layout/table_grid.h"

/* The four sides in CSS's four-side order, which is also `TableCollapsedEdges`' field order. */
enum { TBC_TOP = 0, TBC_RIGHT = 1, TBC_BOTTOM = 2, TBC_LEFT = 3 };
static const char *const TBC_SIDES[4] = { "top", "right", "bottom", "left" };

/* The boxes §17.6.2.1's resolution is stated over, gathered once for one table box. Internal: both entries
   are routes, so no consumer owns a gathering. The grid places cells, the rows give each horizontal line's row
   and row group, and the column map gives each vertical line's column and column-group box.
   `rows` is not a second `TableGrid.rows`: only `TableBoxRow.group` says which row group a row is in, and the
   gathering asserts the two agree element by element. */
typedef struct {
    lxb_dom_element_t *table;
    const TableGrid   *grid;      /* borrowed */
    TableBoxRow       *rows;      /* owned; `grid->nrows` entries, in §17.2's display order */
    size_t             nrows;
    TableColumnBoxMap  columns;   /* owned */
} TbcBoxes;

bool table_border_collapse_selected(lxb_dom_element_t *table)
{
    DCHECK(table != NULL, "CSS 2.1 §17.6 Borders' border model was asked for of no element");
    return css_computed_keyword_is(table, "border-collapse", CSS_KEYWORD_COLLAPSE);
}

/* ---- One grid line's candidates ---------------------------------------------------------------------------
   §17.6.2.1's "borders at every edge of every cell … specified by border properties on a variety of elements
   that meet at that edge", reduced as the header shows: rule 1's `hidden` as a latch, rule 3's width as a
   maximum. */
typedef struct {
    bool  hidden;   /* rule 1: "Any border with this value suppresses all borders at this location." */
    CssPx width;    /* rule 3's maximum over every candidate, `none` included at its computed width of zero */
} TbcEdge;

static TbcEdge tbc_edge_empty(void)
{
    TbcEdge e;

    e.hidden = false;
    e.width = css_px(0.0);
    return e;
}

/* One box meeting the edge. A NULL element (an anonymous box, or an empty slot) contributes nothing: its
   `border-style` is the initial `none`, width zero, never `hidden`. */
static void tbc_meet(TbcEdge *e, lxb_dom_element_t *el, int side)
{
    char name[32];
    char *style;
    CssLength w;

    DCHECK(e != NULL, "a §17.6.2.1 candidate was added to no edge");
    DCHECK(side >= TBC_TOP && side <= TBC_LEFT,
           "CSS 2.1 §17.6.2.1 Border conflict resolution was asked for a border side that is not one of the "
           "four — TBC_SIDES and this range are one list and have come apart");
    if (el == NULL) return;
    snprintf(name, sizeof name, "border-%s-style", TBC_SIDES[side]);
    style = css_computed_value(el, name);
    DCHECK(style != NULL,
           "the cascade produced no computed value for a `border-*-style`, which is in lexbor's registry with "
           "an initial value of `none`, so the last layer always answers");
    if (strcmp(style, "hidden") == 0) e->hidden = true;
    free(style);
    snprintf(name, sizeof name, "border-%s-width", TBC_SIDES[side]);
    w = css_computed_length(el, name);
    DCHECK(w.kind == CSS_LENGTH_ABSOLUTE,
           "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 §3.3 "
           "\"Line Thickness: the border-width properties\"' `Computed value:` line is `absolute length, "
           "snapped as a border width` and every arm of that derivation produces one, so a percentage or a "
           "keyword here is a rule that did not run");
    e->width = css_px_max(e->width, w.px);
}

/* Rules 1 and 3 over the accumulated set. Rule 2 ("Only if the border properties of all the elements meeting
   at this edge are 'none' will the border be omitted") is the same maximum over zeroes. */
static CssPx tbc_used(const TbcEdge *e)
{
    return e->hidden ? css_px(0.0) : e->width;
}

/* ---- Which box occupies what ------------------------------------------------------------------------------ */

/* The cell box covering grid slot (`row`, `col`), or NULL where none reaches it (§17.5's rule 5 does not
   require a full grid). The first match wins: for an overlap, core/layout/table_grid.h's recorded reading
   (`TableGridCell.overlaps`) makes the first-reached cell the one the grid says is there. */
static lxb_dom_element_t *tbc_cell_at(const TableGrid *g, size_t row, size_t col)
{
    size_t i;

    for (i = 0; i < g->ncells; i++) {
        const TableGridCell *k = &g->cells[i];

        if (row >= k->row && row < k->row + k->rowspan && col >= k->col && col < k->col + k->colspan)
            return k->element;
    }
    return NULL;
}

/* The column box (`group` false) or column-group box (`group` true) occupying grid column `col`, or NULL. */
static lxb_dom_element_t *tbc_column_at(const TableColumnBoxMap *m, size_t col, bool group)
{
    if (col >= m->ncols) return NULL;
    return group ? m->cols[col].column_group : m->cols[col].column;
}

/* Whether grid column `col` is the first (resp. last) of its column or column-group box's run, where that box
   has a left (resp. right) border. Rule 3 places column boxes "next to each other in the order they occur", so
   runs are contiguous. */
static bool tbc_column_starts(const TableColumnBoxMap *m, size_t col, bool group)
{
    lxb_dom_element_t *here = tbc_column_at(m, col, group);

    return here != NULL && (col == 0 || tbc_column_at(m, col - 1, group) != here);
}

static bool tbc_column_ends(const TableColumnBoxMap *m, size_t col, bool group)
{
    lxb_dom_element_t *here = tbc_column_at(m, col, group);

    return here != NULL && tbc_column_at(m, col + 1, group) != here;
}

/* The same two questions for a row group over grid rows, from core/layout/table_box.h's per-row `group`. */
static bool tbc_group_starts(const TbcBoxes *b, size_t row)
{
    lxb_dom_element_t *here = b->rows[row].group;

    return here != NULL && (row == 0 || b->rows[row - 1].group != here);
}

static bool tbc_group_ends(const TbcBoxes *b, size_t row)
{
    lxb_dom_element_t *here = b->rows[row].group;

    return here != NULL && (row + 1 >= b->nrows || b->rows[row + 1].group != here);
}

/* ---- The two resolutions ---------------------------------------------------------------------------------
   `line` is a grid line, 0 to the count inclusive: `n` columns have `n + 1` vertical lines, the outermost two
   on the table's perimeter. See the header for the membership. */
static CssPx tbc_vertical(const TbcBoxes *b, size_t line, size_t row)
{
    TbcEdge e = tbc_edge_empty();
    size_t ncols = b->grid->ncols;
    int k;

    if (line > 0) tbc_meet(&e, tbc_cell_at(b->grid, row, line - 1), TBC_RIGHT);
    if (line < ncols) tbc_meet(&e, tbc_cell_at(b->grid, row, line), TBC_LEFT);
    for (k = 0; k < 2; k++) {
        bool group = (k == 1);

        if (line > 0 && tbc_column_ends(&b->columns, line - 1, group))
            tbc_meet(&e, tbc_column_at(&b->columns, line - 1, group), TBC_RIGHT);
        if (line < ncols && tbc_column_starts(&b->columns, line, group))
            tbc_meet(&e, tbc_column_at(&b->columns, line, group), TBC_LEFT);
    }
    /* A row, a row group and the table have a vertical border only at the outermost two lines. */
    if (line == 0 || line == ncols) {
        int side = (line == 0) ? TBC_LEFT : TBC_RIGHT;

        if (row < b->nrows) {
            tbc_meet(&e, b->rows[row].element, side);
            tbc_meet(&e, b->rows[row].group, side);
        }
        tbc_meet(&e, b->table, side);
    }
    return tbc_used(&e);
}

static CssPx tbc_horizontal(const TbcBoxes *b, size_t line, size_t col)
{
    TbcEdge e = tbc_edge_empty();
    size_t nrows = b->nrows;

    if (line > 0) {
        tbc_meet(&e, tbc_cell_at(b->grid, line - 1, col), TBC_BOTTOM);
        /* Rule 1 gives each row one grid row, so an interior horizontal line is the bottom of the row above
           and the top of the row below. */
        tbc_meet(&e, b->rows[line - 1].element, TBC_BOTTOM);
        if (tbc_group_ends(b, line - 1)) tbc_meet(&e, b->rows[line - 1].group, TBC_BOTTOM);
    }
    if (line < nrows) {
        tbc_meet(&e, tbc_cell_at(b->grid, line, col), TBC_TOP);
        tbc_meet(&e, b->rows[line].element, TBC_TOP);
        if (tbc_group_starts(b, line)) tbc_meet(&e, b->rows[line].group, TBC_TOP);
    }
    /* A column, a column group and the table have a horizontal border only at the top and bottom. */
    if (line == 0 || line == nrows) {
        int side = (line == 0) ? TBC_TOP : TBC_BOTTOM;

        tbc_meet(&e, tbc_column_at(&b->columns, col, false), side);
        tbc_meet(&e, tbc_column_at(&b->columns, col, true), side);
        tbc_meet(&e, b->table, side);
    }
    return tbc_used(&e);
}

/* ---- The context ----------------------------------------------------------------------------------------- */

static void tbc_gather(lxb_dom_element_t *table, const TableGrid *grid, TbcBoxes *out)
{
    char nbuf[160];

    DCHECK(table != NULL && grid != NULL && out != NULL,
           "CSS 2.1 §17.6.2 The collapsing border model's boxes were gathered with no table, no grid or "
           "nowhere to put them. The grid is the OPERAND the resolution is stated over — §17.6.2.1 Border "
           "conflict resolution is written per EDGE, and nothing before core/layout/table_grid.h says which "
           "cells adjoin which grid line");
    DCHECK(grid->cells != NULL || grid->ncells == 0,
           "CSS 2.1 §17.6.2 was handed a grid with no cell array and a non-zero cell count — `table_grid_build` "
           "stores NULL only for a table whose rows generate no cell, so the two have been carried apart since "
           "it answered");
    if (!table_border_collapse_selected(table))
        DFAILF("%s: CSS 2.1 §17.6.2 The collapsing border model's border resolution was gathered for a table "
               "whose `border-collapse` selects CSS 2.1 §17.6.1 The separated borders model, in which the two "
               "models' answers are not merely different numbers but different SHAPES: §17.6.1 states that "
               "\"each cell has an individual border\" and that \"Rows, columns, row groups, and column groups "
               "cannot have borders (i.e., user agents must ignore the border properties for those "
               "elements)\", so four of the six kinds of box §17.6.2.1 resolves over declare nothing at all "
               "there. ASK `table_border_collapse_selected` at the algorithm's entry and take §17.6.1's arm — "
               "each cell's own computed `border-*-width` plus `border-spacing` — rather than routing here",
               box_subject(table, nbuf, sizeof nbuf));
    out->table = table;
    out->grid = grid;
    out->rows = NULL;
    /* The grid places the rows (§17.5's rule 1), but only `table_box_rows` gives each row's group, so both
       are read and asserted to agree: a mismatch would resolve a border from the wrong, still real, box. */
    out->nrows = table_box_rows(table, &out->rows);
    DCHECK(out->nrows == grid->nrows,
           "CSS 2.1 §17.5 Visual layout of table contents' rule 1 makes the grid's row count the table's own "
           "row count — \"the table occupies exactly as many grid rows as there are row elements\" — and "
           "core/layout/table_grid.h states outright that it takes that number from `table_box_rows` rather "
           "than re-deriving it. Two different counts here means the two walks ran over different trees, and "
           "every row index below would name a different row in each");
    {
        size_t r;

        for (r = 0; r < out->nrows; r++)
            DCHECK(grid->rows[r] == out->rows[r].element,
                   "CSS 2.1 §17.5 Visual layout of table contents' rule 1 places one row box per grid row "
                   "(\"Each row box occupies one row of grid cells\"), and core/layout/table_grid.h's own "
                   "placement and core/layout/table_box.h's box generation name DIFFERENT elements at one grid "
                   "row. The grid states it takes its row order from `table_box_rows`, so the two are one walk "
                   "and cannot disagree — and CSS 2.1 §17.6.2.1 Border conflict resolution would resolve a "
                   "horizontal grid line's border out of one row's boxes and that row's GROUP out of another's");
    }
    table_column_boxes_build(table, grid->ncols, &out->columns);
}

static void tbc_scatter(TbcBoxes *b)
{
    DCHECK(b != NULL, "CSS 2.1 §17.6.2's gathered boxes were released through no answer");
    table_box_rows_free(b->rows, b->nrows);
    table_column_boxes_release(&b->columns);
    b->rows = NULL;
    b->nrows = 0;
    b->table = NULL;
    b->grid = NULL;
}

/* ---- What the consumers read ------------------------------------------------------------------------------ */

static TableCollapsedEdges tbc_cell_charge(const TbcBoxes *b, const TableGridCell *cell)
{
    TableCollapsedEdges out;
    size_t ncols, rend, cend, i;

    DCHECK(b != NULL && cell != NULL,
           "CSS 2.1 §17.6.2's charge was asked for with no gathered boxes or no grid cell — a cell's charge is "
           "the resolution at the grid lines it abuts, and neither operand has a substitute");
    DCHECK(cell->rowspan >= 1 && cell->colspan >= 1,
           "CSS 2.1 §17.5 Visual layout of table contents' grid reported a cell covering NO grid cell, and "
           "that section states the floor in the sentence that defines a cell's rectangle: \"Each cell is thus "
           "a rectangular box, one or more grid cells wide and high\"");
    ncols = b->grid->ncols;
    rend = cell->row + cell->rowspan;
    cend = cell->col + cell->colspan;
    DCHECK(rend <= b->nrows && cend <= ncols,
           "CSS 2.1 §17.6.2's charge was asked for a cell whose rectangle leaves the grid it was placed in. "
           "§17.5's rule 6 shortens a cell until it fits (\"A cell box cannot extend beyond the last row box "
           "of a table or row group; the user agents must shorten it until it fits\") and "
           "core/layout/table_grid.h grows `ncols` to `col + colspan` for every cell it places, so a rectangle "
           "past either end is a grid whose cell array and counts were carried apart");
    out.top = css_px(0.0);
    out.right = css_px(0.0);
    out.bottom = css_px(0.0);
    out.left = css_px(0.0);
    /* Perimeter lines are the table's (the equation's two `0.5 * border-width` terms), so a cell at the grid's
       edge keeps zero on that side. */
    for (i = cell->row; i < rend; i++) {
        if (cell->col > 0) out.left = css_px_max(out.left, tbc_vertical(b, cell->col, i));
        if (cend < ncols) out.right = css_px_max(out.right, tbc_vertical(b, cend, i));
    }
    for (i = cell->col; i < cend; i++) {
        if (cell->row > 0) out.top = css_px_max(out.top, tbc_horizontal(b, cell->row, i));
        if (rend < b->nrows) out.bottom = css_px_max(out.bottom, tbc_horizontal(b, rend, i));
    }
    /* "borders are centered on the grid lines between the cells": each of the two cells at an interior line
       carries half, and the halves sum to the equation's `border-width_i`. */
    out.top = css_px_scale(out.top, 0.5);
    out.right = css_px_scale(out.right, 0.5);
    out.bottom = css_px_scale(out.bottom, 0.5);
    out.left = css_px_scale(out.left, 0.5);
    return out;
}

static TableCollapsedEdges tbc_table_borders(const TbcBoxes *b)
{
    TableCollapsedEdges out;
    size_t ncols, c;

    DCHECK(b != NULL, "CSS 2.1 §17.6.2's table border widths were asked for with no gathered boxes");
    out.top = css_px(0.0);
    out.right = css_px(0.0);
    out.bottom = css_px(0.0);
    out.left = css_px(0.0);
    ncols = b->grid->ncols;
    /* Each sentence is stated over the cells at a grid edge, so no rows or no columns gives zeroes. */
    if (b->nrows == 0 || ncols == 0) return out;
    /* "by examining the first and last cells in the first row of the table": both read at grid row 0, and
       "If subsequent rows have larger collapsed left and right borders, then any excess spills into the margin
       area of the table." */
    out.left = css_px_scale(tbc_vertical(b, 0, 0), 0.5);
    out.right = css_px_scale(tbc_vertical(b, ncols, 0), 0.5);
    /* "The top border width of the table is computed by examining all cells who collapse their top borders
       with the top border of the table … equal to half of the maximum collapsed top border", likewise the
       bottom: every grid column of line 0 (resp. the last line). */
    for (c = 0; c < ncols; c++) {
        out.top = css_px_max(out.top, tbc_horizontal(b, 0, c));
        out.bottom = css_px_max(out.bottom, tbc_horizontal(b, b->nrows, c));
    }
    out.top = css_px_scale(out.top, 0.5);
    out.bottom = css_px_scale(out.bottom, 0.5);
    return out;
}

/* ---- The two routes ---------------------------------------------------------------------------------------
   See the header. Both find the table box by climbing CSS 2.1 §17.2 The CSS table model's nesting
   (core/layout/table_box.h), as core/layout/used_value.c's cell route does. */

TableCollapsedEdges table_collapsed_cell_edges(lxb_dom_element_t *cell)
{
    lxb_dom_element_t *table;
    TableCollapsedEdges charge;
    TbcBoxes boxes;
    TableGrid grid;
    const TableGridCell *placed;
    char nbuf[160], tbuf[160];

    DCHECK(cell != NULL, "CSS 2.1 §17.6.2's charge was asked for of no cell element");
    table = table_box_table_of(cell);
    table_grid_build(table, &grid);
    placed = table_grid_cell_of(&grid, cell);
    if (placed == NULL)
        DFAILF("%s: CSS 2.1 §17.5 Visual layout of table contents placed NO cell for this box in %s's grid, "
               "which is the table box CSS 2.1 §17.2 The CSS table model's own nesting puts it inside — so "
               "CSS 2.1 §17.6.2 The collapsing border model has no grid line to resolve a border at. The two "
               "walks have come apart: core/layout/table_box.h reached this table by climbing the internal "
               "boxes above the cell, and core/layout/table_grid.h reached the cells by descending the same "
               "table's rows, so a box that is in one and not the other is a row this cell hangs under that "
               "CSS 2.1 §17.2.1 Anonymous table objects' box generation does not report as a row of this table",
               box_subject(cell, nbuf, sizeof nbuf), box_subject(table, tbuf, sizeof tbuf));
    tbc_gather(table, &grid, &boxes);
    charge = tbc_cell_charge(&boxes, placed);
    tbc_scatter(&boxes);
    table_grid_release(&grid);
    return charge;
}

TableCollapsedEdges table_collapsed_table_edges(lxb_dom_element_t *table)
{
    TableCollapsedEdges own;
    TbcBoxes boxes;
    TableGrid grid;

    DCHECK(table != NULL, "CSS 2.1 §17.6.2's table border widths were asked for of no element");
    table_grid_build(table, &grid);
    tbc_gather(table, &grid, &boxes);
    own = tbc_table_borders(&boxes);
    tbc_scatter(&boxes);
    table_grid_release(&grid);
    return own;
}
