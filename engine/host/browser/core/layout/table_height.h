/* CSS 2.1 §17.5.3 Table height algorithms: the table box's height and its rows' heights.
 *
 * The table height is a sum: "A value of 'auto' means that the height is the sum of the row heights plus any
 * cell spacing or borders. Any other value is treated as a minimum height." Distributing a surplus is declined
 * ("CSS 2.1 does not define how extra space is distributed when the 'height' property causes the table to be
 * taller than it otherwise would be"); the recorded choice leaves it below the last row, inside the content box.
 *
 * A row's height is "the maximum of the row's computed 'height', the computed 'height' of each cell in the row,
 * and the minimum height (MIN) required by the cells"; `auto` contributes nothing ("the row height used for
 * layout is MIN"). MIN "depends on cell box heights and cell box alignment (much like the calculation of a line
 * box height)", and the section's procedure reduces to
 *     max( max(A) + max(H - A) over the baseline cells,  max(H) over every other cell )
 * with `A` a cell box's top-to-baseline distance and `H` its height. The first term exceeds `max(H)` when two
 * baseline cells differ in `A` (20px top padding on one, 20px bottom on the other), so `max(H)` is only a floor.
 * With at most one baseline cell the first term is `H₁`, so MIN is exactly `max(H)` with no baseline read. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_HEIGHT_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_HEIGHT_H

/* A cell's alignment is read through css-inline-3's longhands: css-inline-3 §4.2 "Transverse Box Alignment: the
   vertical-align property" makes `vertical-align` a shorthand, mapping for cells "top (baseline-shift: top) ...
   to start, bottom (baseline-shift: bottom) to end, and otherwise middle (alignment-baseline: middle) to
   center"; core/css/css_shorthand.c expands it and core/css/css_computed_value.c models both longhands.
   §17.5.3's catch-all ("These values do not apply to cells; the cell is aligned at the baseline instead")
   covers the rest.
   §17.5.3's declined cases are recorded choices: a percentage row or cell `height` contributes nothing (`auto`'s
   arm); "the meaning of 'height' on row groups" is read by nothing; and for row-spanning cells only "the sum of
   the row heights involved must be great enough to encompass the cell spanning the rows" is enforced, with the
   growth placed in the last spanned row. §10.7 "Minimum and maximum heights: 'min-height' and 'max-height'" is
   not read ("the effect … on tables, inline tables, table cells, table rows, and row groups is undefined").
   Under §17.6.2 The collapsing border model the spacing term is zero ("Borders are centered on the grid lines
   between the cells"), and cell edges come through `table_cell_vertical_edges`
   (core/layout/table_border_collapse.h). Nothing is stored; the allocated array is the caller's. */

#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/table_grid.h"

/* §17.5.3's answer, one fact in three shapes, the vertical twin of core/layout/table_width.h's
   `TableUsedWidths`: `content` is the sum of `rows` and `spacing` between and around them, asserted at the
   entry's boundary. `spacing` is a field for the width answer's reasons: a table with no rows takes none, and a
   second read of `border-spacing` could disagree with the one the rows were laid out under. */
typedef struct {
    /* The used height of each grid row, in core/layout/table_grid.h's row order, as a border-box height of
       its cells (§17.5: in the separated model "the edges coincide with the border edges of cells"). `nrows`
       entries, or NULL when `nrows` is 0. */
    CssPx  *rows;
    size_t  nrows;
    /* The table box's used content height, including the vertical border spacing and excluding its own
       padding and border: "the sum of the row heights plus any cell spacing or borders", floored by a declared
       `height` ("a minimum height"). A declared height is an ordinary content-box length: §17.6.1's HTML
       border-edge exception is about the width only.
       Converting to a border box under §17.6.2 goes through core/layout/used_value.c, which takes
       core/layout/table_border_collapse.h's `table_collapsed_table_edges` halves; adding the computed
       `border-top-width` and `border-bottom-width` would count the full borders §17.6.2 halves. */
    CssPx   content;
    /* §17.6.1's vertical `border-spacing` ("the first gives the horizontal spacing and the second the
       vertical spacing"), which `content` holds `nrows + 1` of: "the distance between the table border and
       the borders of the cells on the edge of the table is the table's padding for that side, plus the
       relevant border spacing distance" covers top and bottom. The horizontal half is §17.5.2's.
       It is this algorithm's term, not the property: zero under §17.6.2 The collapsing border model. */
    CssPx   spacing;
} TableUsedHeights;

/* §17.5.3 over `table` and `grid`, which must be that table's own (core/layout/table_grid.h). `table` must
   generate a table box (`table` or `inline-table`, core/layout/table_box.h's `table_box_kind`); a
   table-internal box crashes. The result is the caller's, released with `table_heights_release`. A table with
   no rows is real; its height is its declared `height` or zero. An input §17.5.3 cannot resolve crashes by
   name, since every term is a floor under a row. */
void table_heights(lxb_dom_element_t *table, const TableGrid *grid, TableUsedHeights *out);

/* The walks `table_heights` reads over the same `table` and `grid`, asked without a drive: each cell's
   content height and the cell baselines a row's MIN measures. False ("not yet") when one was deferred, with
   the question suspended on the current job; true when `table_heights` will find every one answered. */
bool table_heights_ask(lxb_dom_element_t *table, const TableGrid *grid);

/* One cell's used border-box height from the answer above, the twin of core/layout/table_width.h's
   `table_cell_used_border_box`: a cell's box fills the rows it covers (§17.5, and §17.5.3: "Cell boxes that are
   smaller than the height of the row receive extra top or bottom padding."). `cell` must be a cell of the grid
   `heights` was computed over (core/layout/table_grid.h's `table_grid_cell_of`).
   A cell spanning N rows also gets N-1 of `spacing`, the width answer's reading (no adjoining cell borders
   inside one cell). Under §17.6.2 the term is zero and the answer still exact, since each interior line is
   charged half to each neighbouring row. Both sides of §17.5.3's spanning constraint are then in one box.
   It is not a used `height`: under `content-box` (css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing
   property") the content box is this less `table_cell_vertical_edges`. */
CssPx table_cell_used_border_box_height(const TableUsedHeights *heights, const TableGridCell *cell);

/* One row group box's block-axis extent: CSS 2.1 §17.5 Visual layout of table contents' rule 2 as a distance.
   On the inline axis a group equals a row (core/layout/used_value.c); here a group covers a run of rows
   (core/layout/table_grid.h's `TableGridRowGroup`). §17.5's last paragraph places the edges: "in the
   collapsing borders model coincide with the hypothetical grid lines on which the borders of the cells are
   centered. (And thus, in this model, the rows together exactly cover the table, leaving no gaps; ditto for the
   columns.) In the separated borders model, the edges coincide with the border edges of cells. (And thus, in
   this model, there may be gaps between the rows, columns, row groups or column groups, corresponding to the
   'border-spacing' property.)" So the extent is the N row heights plus N-1 `TableUsedHeights.spacing`, which
   is zero under §17.6.2: one arithmetic for both models. Between two groups stands one spacing, the same one
   between their rows; `table_heights` asserts each run's top against the previous run's bottom.
   It is an extent, not a used `height` ("CSS 2.1 does not define the meaning of 'height' on row groups"); it
   answers `getBoundingClientRect`, not `getComputedStyle(tbody).height`. `group` may be NULL, for an empty
   `<tbody>`; the answer is zero (reasoned at the site). The run must be from the grid `table_heights` used. */
CssPx table_row_group_used_extent(const TableUsedHeights *heights, const TableGridRowGroup *group);

/* Where one grid line lies on the block axis: the distance from the table box's content edge to the top of
   grid row `row`. A row box (rule 1), a row group (rule 2, at its run's first row) and a cell (anchored at "the
   row specified by the cell's parent") all start here, so the prefix sum is spelled once.
   It is `row` row heights plus `row + 1` of `TableUsedHeights.spacing` (§17.6.1: "the table's padding for that
   side, plus the relevant border spacing distance", measured past the padding). Under §17.6.2 the term is
   zero and row 0 starts at the content edge, which is where that model puts the first grid line (the table box
   takes "half of the maximum collapsed top border"). The table's own border, padding and origin are
   `used_value_leading_edge_px`'s and core/layout/flow_position.h's.
   The domain is the `nrows + 1` lines: at `nrows` it is where a row after the last would begin, which an empty
   trailing `<tbody>` stands on. Callers indexing a row hold the stricter range. Past `nrows` is a crash. */
CssPx table_row_used_block_offset(const TableUsedHeights *heights, size_t row);

/* One cell's vertical padding and border, in CSS pixels: the mirror of core/layout/table_column_width.h's
   `table_cell_border_edges`, between the content box §10.6.3 answers in and the border box rows are measured
   in. It lives beside §17.5.3, its operand.
   A percentage padding crashes: CSS 2.1 §8.4 refers it to the containing block's width "even for 'padding-top'
   and 'padding-bottom'", for a cell the table's content width, which §17.5.2.2's MCW makes depend on the cell;
   §8.4 says "if the containing block's width depends on this element, then the resulting layout is undefined
   in CSS 2.1". An absolute padding is the used one (`used_value_px`), so a negative `calc()` floors at zero
   (css-values-4 §9.1 "Numeric Functions").
   Both of §17.6's models are answered here, from the cell's table: the cell's own `border-*-width` under
   §17.6.1; under §17.6.2 half of §17.6.2.1's winner at each horizontal line, zero on the perimeter
   (core/layout/table_border_collapse.h). */
CssPx table_cell_vertical_edges(lxb_dom_element_t *cell);

/* Releases what `table_heights` stored. A zero-row answer holds NULL and is accepted. */
void table_heights_release(TableUsedHeights *h);

#endif
