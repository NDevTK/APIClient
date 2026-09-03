/* CSS 2.1 §17.6.2 The collapsing border model and §17.6.2.1 Border conflict resolution — THE USED BORDER AT
 * EVERY GRID LINE OF A TABLE, and the two things a layout reads off it: how much of a border each CELL BOX is
 * charged, and what the TABLE BOX's own four border widths are.
 *
 * WHY IT IS A COMPONENT AND NOT AN ARM AT EACH CONSUMER. §17.6.2.1 states one resolution over the borders of
 * six kinds of box — "cells, rows, row groups, columns, column groups, and the table itself" — and THREE
 * separate algorithms need the answer in three different shapes: CSS 2.1 §17.5.2.2 Automatic table layout wants
 * a cell's horizontal charge, CSS 2.1 §17.5.3 Table height algorithms wants its vertical charge, and §17.6.2's
 * own four sentences about the table's border widths want a maximum over a whole edge of the grid. Each is a
 * different reduction of ONE per-grid-line quantity, and a copy of the resolution at each consumer would be
 * three walks free to disagree about which boxes meet at an edge — a disagreement nothing downstream could
 * see, because every answer is a real border width of a real box.
 *
 * ---- WHAT §17.6.2's ROW-WIDTH EQUATION ACTUALLY CHARGES TO A CELL --------------------------------------------
 * The section states the whole geometry as one equation "which holds for every row of the table":
 *   row-width = (0.5 * border-width_0) + padding-left_1 + width_1 + padding-right_1 + border-width_1 +
 *               padding-left_2 +...+ padding-right_n + (0.5 * border-width_n)
 * with "border-width_i refers to the border between cells i and i + 1". So an INTERIOR grid line is charged
 * ONCE IN FULL and the two OUTERMOST are charged at half.
 *
 * THE TWO HALVES AT THE ENDS ARE THE TABLE BOX'S OWN BORDER AND NOT A CELL'S, which the very next paragraph
 * says outright: "The left border width of the table is half of the first cell's collapsed left border, and the
 * right border width of the table is half of the last cell's collapsed right border." Read together, the
 * equation's `row-width` is the table box's BORDER-BOX width, its two 0.5 terms are the table's own left and
 * right border widths, and everything between them is the table's CONTENT width. So this component charges a
 * cell HALF the resolved border at each grid line it abuts INSIDE the table, and NOTHING at a grid line on the
 * table's perimeter — the outer half of a perimeter border is outside the table box entirely, which is what
 * §17.6.2 means by "any excess spills into the margin area of the table".
 * THAT READING IS WHAT MAKES EVERY SUM DOWNSTREAM COME OUT RIGHT WITH NO SECOND ARM. A grid column's used
 * width is the border box of the cells occupying it (core/layout/table_column_width.h's recorded choice), so
 * under this charge the used widths of all the columns sum to 0.5*V_1 + V_1 + … + 0.5*V_1 … — each interior
 * grid line's border counted once, from the halves its two neighbours each carry, and neither perimeter line
 * counted at all. That IS the table's content width, which is the same relation §17.6.1 The separated borders
 * model's spacing satisfies, so core/layout/table_width.c's own check that its two answers describe one
 * rectangle holds unchanged under both models. It is also why a SPANNING cell needs no extra term: the two
 * columns a `colspan=2` cell covers each carry half of the grid line running under it, and the pair sum to the
 * whole, exactly as the `border-spacing` term they replace did.
 * A TABLE DOES NOT HAVE PADDING IN THIS MODEL — "Note that in this model, the width of the table includes half
 * the table border. Also, in this model, a table does not have padding (but does have margins)." — so a
 * consumer adding the table's own edges adds the four border widths below and NOTHING else.
 *
 * ---- ONLY THE WIDTH IS RESOLVED, AND THAT IS A PROOF RATHER THAN A NARROWING ---------------------------------
 * §17.6.2.1's four rules pick ONE border out of the set meeting at an edge — its width, its style and its
 * colour — and a LAYOUT needs only the width. Rules 3 and 4 cannot change it:
 *   Rule 1 CAN: "Borders with the 'border-style' of 'hidden' take precedence over all other conflicting
 *     borders. Any border with this value suppresses all borders at this location." One `hidden` anywhere in
 *     the set makes the used width ZERO however wide the others are, so this rule is implemented explicitly.
 *   Rule 2 cannot: a `none` border's COMPUTED width is already 0 (CSS 2.1 §8.5.1's own `Computed value:` line,
 *     "absolute length; '0' if the border style is 'none' or 'hidden'", which core/css/css_computed_value.c
 *     applies at the computed value), so discarding the `none` candidates and taking the maximum over the rest
 *     is the same number as taking the maximum over all of them. "Only if the border properties of all the
 *     elements meeting at this edge are 'none' will the border be omitted" is that same maximum over zeroes.
 *   Rule 3's WIDTH half is the answer: "narrow borders are discarded in favor of wider ones". Its STYLE half —
 *     "If several have the same 'border-width' then styles are preferred in this order: 'double', 'solid',
 *     'dashed', 'dotted', 'ridge', 'outset', 'groove', and the lowest: 'inset'" — selects among borders of
 *     EQUAL width and therefore leaves the width alone.
 *   Rule 4 likewise: it is stated "If border styles differ only in color", so every candidate it chooses
 *     between already has the same width and style.
 * So the used WIDTH at an edge is: zero if any box meeting there has a `border-*-style` of `hidden`, and
 * otherwise the maximum of the computed `border-*-width` of every box meeting there. That is exact, not
 * approximate, and it is the whole of what §17.5.2 and §17.5.3 can consume.
 *
 * NAMED RESIDUAL — WHICH BORDER IS PAINTED. WHAT IS NOT COVERED: rules 3's style order and rule 4's origin
 * order (cell, row, row group, column, column group, table; then further-to-the-left under `direction: ltr`
 * and further-to-the-top), which decide the STYLE and COLOUR drawn at an edge. WHAT THE NEXT DIFF BUILDS: the
 * resolution answering the winning box rather than a width — the same candidate walk below, ordered by those
 * two rules instead of reduced by a maximum — for the painting component, which does not exist in this tree at
 * all (nothing here draws a border; core/layout/ answers geometry). HOW ITS ABSENCE WOULD SHOW: two borders of
 * equal width and different style meeting at one grid line would be indistinguishable to a consumer, so
 * §17.6.2.1's own worked example — a `5px dashed blue` cell against a `5px solid green` cell — could not be
 * told from its mirror image; no width, and therefore no rectangle this directory produces, changes.
 *
 * ---- WHICH BOXES "MEET AT THAT EDGE", WHICH §17.6.2.1 NAMES AND DOES NOT PLACE ------------------------------
 * The section lists the six kinds and leaves the geometry to the reader, so the derivation is RECORDED here
 * rather than crashed on — it follows from each box's own extent under CSS 2.1 §17.5 Visual layout of table
 * contents, and there is no other reading that puts a border anywhere a box has one.
 *   A VERTICAL grid line, taken over ONE grid row: the cells immediately to its left and right contribute their
 *     `border-right-width` and `border-left-width`. A COLUMN box contributes its `border-left-width` only where
 *     the line is the FIRST of the grid columns it occupies and its `border-right-width` only where the line is
 *     just past its LAST — a `<col span="3">`'s interior grid lines run THROUGH it, and a box has no border
 *     there. A COLUMN GROUP box the same, over its own run (§17.5's rule 4: "A column group box occupies the
 *     same grid cells as the columns it contains"). A ROW, a ROW GROUP and the TABLE contribute only at the
 *     table's two OUTERMOST vertical lines, which are the only vertical lines their boxes have a border on.
 *   A HORIZONTAL grid line, taken over ONE grid column: the mirror image — the cells above and below, the ROW
 *     boxes above and below (§17.5's rule 1 gives each row exactly one grid row, so every horizontal line
 *     inside the table is a row's own top or bottom edge), the ROW GROUP boxes at the boundaries of their runs,
 *     and the COLUMN, COLUMN GROUP and TABLE boxes only at the table's top and bottom lines.
 * A BOX NO ELEMENT NAMES CONTRIBUTES NOTHING, and that is a real answer rather than a skipped lookup: §17.2.1
 * Anonymous table objects generates rows and cells no element names, CSS 2 §9.2.1.1 Anonymous block boxes gives
 * such a box the initial value of every non-inherited property, and `border-style`'s initial value is `none`
 * with a computed width of 0. It cannot contribute a `hidden` either.
 *
 * A CELL'S EDGE MAY CROSS SEVERAL GRID LINES AND THE CHARGE IS THE MAXIMUM OVER THEM — a RECORDED CHOICE,
 * because §17.6.2's equation is stated per row over "the number of cells in the row" and says nothing about a
 * cell whose top edge runs past several columns' worth of resolved borders. The cell box is a RECTANGLE, so it
 * is charged the widest of the borders its edge meets; any smaller charge would let a wider segment's border
 * cross into the cell's own content.
 *
 * WHAT IS DELIBERATELY NOT HERE. The CHOICE OF MODEL is `border-collapse`'s and is asked once, at the entry of
 * each algorithm, through `table_border_collapse_selected` below — this file answers what the collapsing model
 * says and never whether a table is in it. And nothing here reads `border-spacing`: §17.6.2 gives that property
 * no meaning at all, which its consumers state as a zero term rather than asking this component for one. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_BORDER_COLLAPSE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_BORDER_COLLAPSE_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* §17.6's `border-collapse` read as the model it selects — "The value 'separate' selects the separated borders
   border model. The value 'collapse' selects the collapsing borders model." A third keyword crashes here: §17.6
   gives the property `Computed value: as specified` over a keyword-only `collapse | separate | inherit`
   grammar, so nothing between the declaration and here has a rule that could produce one.
   IT IS ASKED OF THE TABLE AND NOT OF A CELL. The property is `Inherited: yes` and its Applies-to line is
   "'table' and 'inline-table' elements", so one table is in one model and a per-cell ask would be the same
   question answered in as many places as there are cells. */
bool table_border_collapse_selected(lxb_dom_element_t *table);

/* FOUR BORDER WIDTHS IN CSS PIXELS, in the order every four-side rule in CSS states them. */
typedef struct {
    CssPx top;
    CssPx right;
    CssPx bottom;
    CssPx left;
} TableCollapsedEdges;

/* WHAT ONE CELL BOX IS CHARGED — half the resolved border at each edge that is an interior grid line, and zero
   at each edge on the table's perimeter (see the header's reading of §17.6.2's row-width equation). `cell` must
   generate a 'table-cell' box that CSS 2.1 §17.5's grid places.
   ZERO IS A REAL ANSWER AT EVERY EDGE and never a missing lookup: a cell at the table's corner is charged
   nothing on two of its sides because the table box carries those borders, and a cell in a table all of whose
   boxes declare `border-style: none` is charged nothing anywhere.
   IT IS A ROUTE AND THE BOXES ARE GATHERED PER CALL, exactly as core/layout/used_value.c's
   `uv_table_cell_used_width` is a route: the table box is found (core/layout/table_box.h), §17.5's grid and
   §17.5's rules 3 and 4 are built over it, and the resolution is read out. NOTHING IS CACHED, for
   core/layout/block_flow.h's reason — a layout is per-flow state, so a cached one is shared state
   solver/dom_cow.h's delta does not swap and a stale one is another flow's document. THE COST IS REAL AND IS
   NAMED HERE RATHER THAN HIDDEN: §17.5.2.2's step 1 and §17.5.3's row maximum each ask this once per cell, so
   a table in the collapsing model gathers its boxes once per cell and both passes are quadratic in the cell
   count. The fix is to thread ONE gathering down from `table_column_widths` and `table_heights`, which already
   hold the grid, and it is not to cache one here. */
TableCollapsedEdges table_collapsed_cell_edges(lxb_dom_element_t *cell);

/* §17.6.2's OWN FOUR SENTENCES for the TABLE box's border widths, whole:
     "UAs must compute an initial left and right border width for the table by examining the first and last
      cells in the first row of the table. The left border width of the table is half of the first cell's
      collapsed left border, and the right border width of the table is half of the last cell's collapsed right
      border."
     "The top border width of the table is computed by examining all cells who collapse their top borders with
      the top border of the table. The top border width of the table is equal to half of the maximum collapsed
      top border. The bottom border width is computed by examining all cells whose bottom borders collapse with
      the bottom of the table. The bottom border width is equal to half of the maximum collapsed bottom border."
   A TABLE WITH NO ROWS OR NO COLUMNS ANSWERS FOUR ZEROES, which is a derivation from those sentences rather
   than a floor chosen here: each is stated over the cells at an edge of the grid, and such a table has none.
   THE TABLE'S PADDING IS NOT PART OF THIS AND HAS NO SECOND ENTRY, because §17.6.2 states that "a table does
   not have padding (but does have margins)" — a consumer that adds a padding under this model is adding a
   length the section says the box does not have. So these four ARE a collapsed table box's whole surround, and
   a consumer converting between its content box and its border box adds exactly them.
   A ROUTE, on the same terms as the entry above. */
TableCollapsedEdges table_collapsed_table_edges(lxb_dom_element_t *table);

#endif
