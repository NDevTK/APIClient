/* CSS 2.1 §17.5 Visual layout of table contents' GRID — which grid cell each cell box is anchored at and how
 * many it covers. It is the operand §17.5.2 Table width algorithms: the 'table-layout' property is stated
 * over, because a COLUMN's width is taken across the cells that occupy it and nothing before this says which
 * those are.
 *
 * WHY IT IS A SEPARATE COMPONENT FROM core/layout/table_box.h. That one runs §17.2.1 Anonymous table objects'
 * box generation and answers WHICH BOXES EXIST — the rows, and the cells inside each row, in tree order. This
 * one answers WHERE THEY SIT, which §17.5's own rules make a different question with a different input: a
 * cell's column is not its position in its row's child list, because a cell anchored in an EARLIER row can
 * still be covering this row's leftmost slots. §17.5's rule 5 says so in the sentence that defines the
 * placement — "the part of the cell in the first column it occupies must not overlap with any other cell box
 * (i.e., a row-spanning cell starting in a prior row)". A component that answered both would be one walk whose
 * output changes meaning halfway.
 *
 * THE SPANS COME FROM HTML AND §17.5 SAYS THEY MUST. Its rule 5 declines to define them and hands the question
 * over by name: "Although CSS 2.1 does not define how the number of spanned rows or columns is determined, a
 * user agent may have special knowledge about the source document; a future update of CSS may provide a way to
 * express this knowledge in CSS syntax." HTML §4.9.11 Attributes common to td and th elements is that
 * knowledge — `colspan` "must be a valid non-negative integer greater than zero and less than or equal to
 * 1000", `rowspan` "a valid non-negative integer less than or equal to 65534", and for rowspan "the value zero
 * means that the cell is to span all the remaining rows in the row group". The PARSING and the CLAMPING are
 * HTML §4.9.12.1 Forming a table's, whose steps this component runs rather than restates.
 * THEY ARE READ FROM A `td` OR `th` AND FROM NOTHING ELSE, which is the narrow reading and the correct one:
 * §4.9.11 gives those attributes meaning on those two elements, so reading `colspan` off a `<div style=
 * "display: table-cell">` would be inventing a span the document language does not define — §17.5's own
 * sentence is that a UA may have special knowledge, not that it may guess. Such a cell spans 1×1, and so does
 * §17.2.1's ANONYMOUS cell, which has no element to carry an attribute at all.
 *
 * §17.5's RULE 6 IS A CLAMP AND IT IS THE SAME CLAMP THAT MAKES `rowspan=0` WORK. "A cell box cannot extend
 * beyond the last row box of a table or row group; the user agents must shorten it until it fits." So a
 * rowspan is shortened to the rows remaining in its own row group, and HTML's "span all the remaining rows in
 * the row group" is that shortening applied to an unbounded request rather than a second rule. A run of rows
 * that core/layout/table_box.h reports with no row group of their own is one group for this purpose, which is
 * what HTML §4.9.12.1 forms for a run of `tr` children of a `table`.
 *
 * THE OVERLAP CASE IS A CHOICE THE SPEC OFFERS AND NOT A GAP, so it is recorded rather than crashed on. §17.5
 * rule 5: "If this position would cause a column-spanning cell to overlap a row-spanning cell from a prior
 * row, CSS does not define the results: implementations may either overlap the cells (as is done in many HTML
 * implementations) or may shift the later cell to the right to avoid such overlap." This component takes the
 * FIRST reading, which is HTML §4.9.12.1's — that algorithm assigns the slots and calls the collision "a table
 * model error" while still defining what covers what — and reports it per cell so a consumer that needs the
 * other reading has the fact rather than having to re-derive it. Crashing here would refuse a document both
 * readings are conforming for.
 *
 * WHAT IS NOT HERE. §17.5's rules 3 and 4 place COLUMN and COLUMN-GROUP boxes ("A column box occupies one or
 * more columns of grid cells… A column group box occupies the same grid cells as the columns it contains"),
 * and this component does not, because §17.2 The CSS table model says those boxes "are not rendered (exactly
 * as if they had 'display: none')" and what §17.5.2 wants from them is a DECLARED WIDTH on a column, which is
 * a property lookup over the same column indices this grid already numbers. Rule 3's `direction` clause — "The
 * first column box may be either on the left or on the right" — is likewise a mapping from these indices to
 * positions and belongs with the widths, not with the assignment; the indices here are in the spec's own
 * LOGICAL order, so a caller reading them right-to-left needs no second grid. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_GRID_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_GRID_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

/* ONE CELL BOX'S RECTANGLE OF GRID CELLS. §17.5: "Each cell is thus a rectangular box, one or more grid cells
   wide and high." `element` is NULL for §17.2.1's anonymous 'table-cell' box, exactly as
   core/layout/table_box.h reports it, and such a cell is always 1×1. */
typedef struct {
    lxb_dom_element_t *element;
    size_t row;        /* the grid row it is anchored at — §17.5: "The top row of this rectangle is in the row
                          specified by the cell's parent" */
    size_t col;        /* the leftmost grid column it occupies, in §17.5's own logical order */
    size_t rowspan;    /* grid rows covered, at least 1, already shortened by rule 6 */
    size_t colspan;    /* grid columns covered, at least 1 */
    /* §17.5 rule 5's undefined case, RECORDED: at least one slot this cell covers was already covered by a
       cell anchored in a prior row. Never true of the cell's own anchor slot — the placement skips occupied
       slots — so it is always a column-spanning cell reaching into a row-spanning one, which is the exact
       shape the section describes. */
    bool overlaps;
} TableGridCell;

/* THE WHOLE GRID of one table box. Cells are in the order §17.5's rules walk them: by grid row, and within a
   row in the order core/layout/table_box.h reports the row's cells, which is their tree order. */
typedef struct {
    TableGridCell *cells;
    size_t ncells;
    size_t nrows;       /* §17.5's rule 1: "the table occupies exactly as many grid rows as there are row
                           elements" — so this is table_box_rows' own count and is not re-derived */
    size_t ncols;       /* the widest any row reached, which is the table's column count */
    bool any_overlap;   /* whether rule 5's undefined case arose at all, so a consumer can ask once */
} TableGrid;

/* Builds `table`'s grid. `table` must generate a TABLE box (core/layout/table_box.h's `table_box_kind`), and
   the caller releases the result with `table_grid_release`.
   A TABLE WITH NO ROWS IS A REAL GRID AND NOT A FAILURE: `nrows` and `ncells` are zero, `ncols` is zero, and
   §17.5.3's height over it is the sum of no row heights, which is a number. */
void table_grid_build(lxb_dom_element_t *table, TableGrid *out);

/* Releases what `table_grid_build` stored. A zero-cell grid holds NULL and is accepted. */
void table_grid_release(TableGrid *grid);

#endif
