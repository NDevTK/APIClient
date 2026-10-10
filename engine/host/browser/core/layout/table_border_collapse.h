/* CSS 2.1 §17.6.2 The collapsing border model and §17.6.2.1 Border conflict resolution: the used border at every
 * grid line of a table, and the two readings layout needs: each cell box's charge and the table box's own four
 * border widths. One resolution over "cells, rows, row groups, columns, column groups, and the table itself"
 * serves §17.5.2.2 Automatic table layout (horizontal charges), §17.5.3 Table height algorithms (vertical) and
 * §17.6.2's table-border sentences, so the boxes meeting at an edge are decided in one place.
 *
 * What a cell is charged. §17.6.2's equation "which holds for every row of the table":
 *   row-width = (0.5 * border-width_0) + padding-left_1 + width_1 + padding-right_1 + border-width_1 +
 *               padding-left_2 +...+ padding-right_n + (0.5 * border-width_n)
 * charges interior lines once in full and the outermost two at half, and the next paragraph names those halves
 * the table's own: "The left border width of the table is half of the first cell's collapsed left border". So a
 * cell is charged half the resolved border at each interior line it abuts and nothing on the perimeter (the
 * outer half "spills into the margin area of the table"). Column widths (cells' border boxes) then sum to the
 * table's content width, as with §17.6.1's spacing, and a spanning cell needs no extra term. "In this model, a
 * table does not have padding (but does have margins)", so the four widths below are its whole surround. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_BORDER_COLLAPSE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_BORDER_COLLAPSE_H

/* Only the width is resolved, exactly: zero if any box meeting the edge has `border-*-style: hidden` (rule 1,
   "Any border with this value suppresses all borders at this location"), else the maximum computed
   `border-*-width` (rule 3, "narrow borders are discarded in favor of wider ones"). Rule 2 needs no arm: a
   `none` border computes to width 0 (CSS 2.1 §8.5.1: "'0' if the border style is 'none' or 'hidden'"). Rule 3's
   style order and rule 4 ("If border styles differ only in color") choose among equal widths.
   Named residual: which border is painted. Not covered: rule 3's style order and rule 4's origin order, which
   decide the style and colour drawn. Next: the same candidate walk answering the winning box, for a painting
   component (none exists; core/layout/ answers geometry). Absence shows as two equal-width borders of
   different style at one line being indistinguishable; no width or rectangle changes. */

/* Which boxes meet at an edge, which §17.6.2.1 names but does not place; derived from §17.5's box extents. A
   vertical line over one grid row: the cells left and right; a column or column-group box only at the first
   or past the last column of its run (rule 4: "A column group box occupies the same grid cells as the columns
   it contains"); rows, row groups and the table only at the outermost lines. A horizontal line over one grid
   column: the mirror image (rule 1 gives each row one grid row; row groups at their run boundaries; columns,
   groups and the table only at top and bottom). A box no element names (§17.2.1 Anonymous table objects)
   contributes nothing: its `border-style` is the initial `none`. A cell edge crossing several lines is
   charged the maximum over them, a recorded choice: the cell is a rectangle, so a smaller charge would let a
   wider segment cross into its content. The model choice is `table_border_collapse_selected`'s, asked at each
   algorithm's entry; `border-spacing` has no meaning here and is not read. */

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* §17.6's `border-collapse` read as the model it selects: "The value 'separate' selects the separated borders
   border model. The value 'collapse' selects the collapsing borders model." A third keyword crashes; the
   grammar is keyword-only and computed as specified. Asked of the table: the property applies to "'table' and
   'inline-table' elements". */
bool table_border_collapse_selected(lxb_dom_element_t *table);

/* Four border widths in CSS pixels, in CSS's four-side order. */
typedef struct {
    CssPx top;
    CssPx right;
    CssPx bottom;
    CssPx left;
} TableCollapsedEdges;

/* What one cell box is charged: half the resolved border at each interior grid line it abuts, zero on the
   table's perimeter. `cell` must generate a 'table-cell' box CSS 2.1 §17.5's grid places. Zero is a real
   answer (a corner cell's outer sides; an all-`none` table).
   A route: it finds the table box (core/layout/table_box.h), builds §17.5's grid and rules 3 and 4, and reads
   the resolution, per call. Nothing is cached (layout is per-flow state solver/dom_cow.h does not swap). The
   cost: §17.5.2.2's step 1 and §17.5.3's row maximum ask once per cell, so both are quadratic in cells; the
   fix is threading one gathering down from `table_column_widths` and `table_heights`, not a cache here. */
TableCollapsedEdges table_collapsed_cell_edges(lxb_dom_element_t *cell);

/* §17.6.2's four sentences for the table box's border widths:
     "UAs must compute an initial left and right border width for the table by examining the first and last
      cells in the first row of the table. The left border width of the table is half of the first cell's
      collapsed left border, and the right border width of the table is half of the last cell's collapsed right
      border."
     "The top border width of the table is computed by examining all cells who collapse their top borders with
      the top border of the table. The top border width of the table is equal to half of the maximum collapsed
      top border. The bottom border width is computed by examining all cells whose bottom borders collapse with
      the bottom of the table. The bottom border width is equal to half of the maximum collapsed bottom border."
   A table with no rows or no columns answers four zeroes (no cells at an edge). With no padding in this model
   ("a table does not have padding (but does have margins)"), these four are the whole surround. A route, as
   above. */
TableCollapsedEdges table_collapsed_table_edges(lxb_dom_element_t *table);

#endif
