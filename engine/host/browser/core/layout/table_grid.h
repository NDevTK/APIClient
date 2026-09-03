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
 * THE ROW AXIS IS HERE AND THE COLUMN AXIS IS NOT, AND THE TWO ARE NOT AN INCONSISTENCY. §17.5's rule 1 is a
 * PLACEMENT this walk performs — "Each row box occupies one row of grid cells. Together, the row boxes fill the
 * table from top to bottom in the order they occur in the source document (i.e., the table occupies exactly as
 * many grid rows as there are row elements)" — so a row's grid row IS this component's own output, produced by
 * the same walk over core/layout/table_box.h's rows that anchors the cells, and asking anywhere else for it
 * would be a second walk free to number the rows differently. §17.5's rules 3 and 4 place COLUMN and
 * COLUMN-GROUP boxes ("A column box occupies one or more columns of grid cells… A column group box occupies the
 * same grid cells as the columns it contains") and this component does not, because §17.2 The CSS table model
 * says those boxes "are not rendered (exactly as if they had 'display: none')": there is no cell walk that
 * numbers them, only a MAPPING onto the column indices this one already numbers, and that mapping is
 * core/layout/table_column_box.h's. Rule 3's `direction` clause — "The first column box may be either on the
 * left or on the right" — is likewise a mapping from these indices to positions and belongs with the widths,
 * not with the assignment; the indices here are in the spec's own LOGICAL order, so a caller reading them
 * right-to-left needs no second grid.
 *
 * §17.5's RULE 2 IS NOT ANSWERED HERE EITHER, AND ITS ABSENCE IS THE ROW AXIS'S OWN GAP RATHER THAN THE COLUMN
 * AXIS'S. "A row group occupies the same grid cells as the rows it contains" is a run of the grid rows below,
 * derivable from nothing but which row group each row is inside — a fact core/layout/table_box.h's
 * `TableBoxRow.group` already carries and this component currently drops. It is not stored because storing it
 * with no consumer would be a field written and read nowhere; the diff that places a ROW GROUP box adds the
 * field and the run entry beside it. Its absence is not silent and is not this file's to report: a row group
 * box has no origin without it, so core/layout/flow_position.c aborts for one by name. */
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
    /* §17.5's RULE 1, WHICH IS A PLACEMENT AND NOT A COUNT: "Each row box occupies one row of grid cells."
       `rows[y]` is the ROW ELEMENT whose box occupies grid row `y`, in the order core/layout/table_box.h
       reports the rows, which §17.2's display order makes the top-to-bottom order rule 1 fills the table in.
       A NULL entry is §17.2.1 Anonymous table objects' anonymous 'table-row' box, exactly as `TableGridCell`'s
       own `element` is NULL for an anonymous cell — a positive statement that the section generated that row
       and no element names it, never a row that is missing.
       `nrows` IS THIS ARRAY'S LENGTH AND THAT IS THE WHOLE OF WHY IT IS ONE FIELD AND NOT TWO. Rule 1's own
       parenthesis is "the table occupies exactly as many grid rows as there are row elements", so the count and
       the array are the same fact; a second count could disagree with the array and nothing downstream could
       tell. NULL when `nrows` is zero. */
    lxb_dom_element_t **rows;
    size_t nrows;
    size_t ncols;       /* the widest any row reached, which is the table's column count */
    bool any_overlap;   /* whether rule 5's undefined case arose at all, so a consumer can ask once */
} TableGrid;

/* Builds `table`'s grid. `table` must generate a TABLE box (core/layout/table_box.h's `table_box_kind`), and
   the caller releases the result with `table_grid_release`.
   A TABLE WITH NO ROWS IS A REAL GRID AND NOT A FAILURE: `nrows` and `ncells` are zero, `ncols` is zero, and
   §17.5.3's height over it is the sum of no row heights, which is a number. */
void table_grid_build(lxb_dom_element_t *table, TableGrid *out);

/* WHICH RECTANGLE OF GRID CELLS one CELL ELEMENT was placed at — the grid read back the way a consumer holding
   an element rather than a walk has to read it, and NULL when this grid placed no cell for that element.
   IT IS AN ENTRY AND NOT A SCAN AT THE CALLER because the NULL is a REAL ANSWER with two distinct causes and
   only this component can tell a consumer which invariant to state about it: the element is not a cell of THIS
   table (a caller that walked to the wrong table box), or it is a cell no rule placed. Both are a consumer's
   crash and neither is this component's, so the answer is the honest NULL rather than an abort here — but a
   scan written at each consumer would be one loop per consumer over a field whose ONE-MATCH invariant is a
   fact about §17.5's placement, which is this file's and not theirs.
   §17.2.1's ANONYMOUS 'table-cell' BOX CANNOT BE ASKED FOR, and that is the section's own doing rather than a
   narrowing: such a box has no element to name it (the `element` field above is NULL for it), so `cell` must
   not be NULL and this entry asserts that instead of matching the first anonymous cell in the grid. */
const TableGridCell *table_grid_cell_of(const TableGrid *grid, const lxb_dom_element_t *cell);

/* WHICH GRID ROW one ROW ELEMENT's box occupies — §17.5's rule 1 read back the way a consumer holding an
   element rather than a walk has to read it. Answers true and stores that row's index at `*out`; answers FALSE
   and leaves `*out` untouched when this grid placed no row for that element.
   IT IS AN ENTRY AND NOT A SCAN AT THE CALLER for `table_grid_cell_of`'s reason, and the shape of the scan it
   replaces is why the distinction is not cosmetic here. A consumer that had only the cell entry could reach a
   row ONLY through the cells anchored in it, and `<tr></tr>` has none — so such a scan answers "no such row"
   for a row that really is in this grid, with a real height (CSS 2.1 §17.5.3 Table height algorithms' maximum
   over no cell) and a real position. That is not a slower lookup, it is a WRONG one, and it is wrong exactly
   for the shape a caller is least likely to have in front of it.
   FALSE IS A REAL ANSWER WITH ONE CAUSE and it is the consumer's crash rather than this component's: the
   element is not a row of THIS table, so the caller reached the wrong table box. `table_grid_build` walks
   core/layout/table_box.h's rows and stores every one of them, so a row of this table is always found.
   `*out` IS NOT WRITTEN ON FALSE, DELIBERATELY, and a sentinel index would have been the weaker choice: a
   caller that ignores the answer then does arithmetic on an UNINITIALISED index, which `-Wall` and a sanitizer
   both name, where a sentinel past the end reads as an ordinary number the caller can quietly add to.
   §17.2.1's ANONYMOUS 'table-row' BOX CANNOT BE ASKED FOR, exactly as its anonymous cell cannot: it has no
   element to name it, so `row` must not be NULL and this entry asserts that instead of matching the first
   anonymous row in the grid. */
bool table_grid_row_of(const TableGrid *grid, const lxb_dom_element_t *row, size_t *out);

/* Releases what `table_grid_build` stored. A zero-cell grid holds NULL and is accepted. */
void table_grid_release(TableGrid *grid);

#endif
