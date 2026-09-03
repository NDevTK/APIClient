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
 * §17.5's RULE 2 IS ANSWERED HERE AND IT IS THE SAME FACT RULE 6 ALREADY NEEDED, which is why it is one array
 * and not two. "A row group occupies the same grid cells as the rows it contains" is a RUN of the grid rows
 * below, derivable from nothing but which row group each row is inside — core/layout/table_box.h's
 * `TableBoxRow.group`. Rule 6 is stated over that same run — "A cell box cannot extend beyond the last row box
 * of a table or row group" names the GROUP as a bound and not only the table — so the clamp this walk applies
 * to a `rowspan` and the extent rule 2 gives a group box are ONE quantity read twice, and computing them
 * separately would be two producers of one fact, free to disagree about where a group ends while each looked
 * locally right.
 * THE TWO AXES DIVERGE HERE AND THE SECTION SAYS WHY. On the INLINE axis a group needs no run at all: rule 1
 * gives every row the WHOLE grid row, so rule 2's union over a group's rows is that same whole row and a group
 * and a row have identical inline extent. On the BLOCK axis they do not, because that is the axis rows are
 * STACKED on — which is also how §17.5's last normative paragraph reads, since the gaps it names "between the
 * rows, columns, row groups or column groups" are gaps on the axis each of those boxes is stacked on. So the
 * run is the block axis's operand and CSS 2.1 §17.5.3 Table height algorithms' question, and
 * core/layout/table_height.h turns it into an extent. */
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

/* ONE MAXIMAL RUN OF CONSECUTIVE GRID ROWS REPORTING THE SAME ROW GROUP — §17.5's rule 2 ("A row group occupies
   the same grid cells as the rows it contains") and rule 6's bound ("A cell box cannot extend beyond the last
   row box of a table or row group") as ONE quantity, because they are the same one.
   `element` IS NULL FOR THE STRETCH WHOSE ROWS' PARENT IS THE TABLE BOX ITSELF, and that entry is NOT a box.
   §17.2.1 Anonymous table objects generates no anonymous row group — core/layout/table_box.h says so of its own
   `TableBoxRow.group` — so a NULL here is the tree's own shape and never a missing box. Such a stretch still
   BOUNDS a `rowspan`, which is HTML §4.9.12.1 Forming a table's doing rather than an invention: that algorithm
   gathers a run of `tr` children of a `table` into a row group, and §17.5's rule 6 then shortens a cell to it.
   It has an extent and no box to hang it on, which is exactly why `table_grid_row_group_of` refuses to be asked
   for one — see there.
   THE RUNS PARTITION THE GRID ROWS: they are in grid order, `groups[0].first` is 0 when there is any row, each
   entry's `first` is the previous entry's `first + nrows`, and every `nrows` is at least 1. That partition is
   what makes `table_grid_group_of_row` an entry rather than a scan, and it is asserted where it is built. */
typedef struct {
    lxb_dom_element_t *element;
    size_t first;    /* the first grid row of the run */
    size_t nrows;    /* grid rows covered, at least 1 */
} TableGridRowGroup;

/* THE WHOLE GRID of one table box. Cells are in the order §17.5's rules walk them: by grid row, and within a
   row in the order core/layout/table_box.h reports the row's cells, which is their tree order. */
typedef struct {
    /* THE TABLE BOX THIS GRID IS OF, stored rather than left to the caller to remember, because it is what
       makes "this element is not in this grid" and "this element is in this grid and occupies nothing"
       DIFFERENT ANSWERS. `table_grid_row_group_of` is where that distinction is load-bearing: an EMPTY
       `<tbody>` is a real row group box of this table that rule 2 places at no grid cell, and without this
       field it would be indistinguishable from a caller that walked to the wrong table box. */
    lxb_dom_element_t *table;
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
    /* §17.5's RULE 2 AND RULE 6's BOUND, as the runs above. In grid order, partitioning rows `0..nrows-1`;
       NULL and zero for a table with no rows. `ngroups` is this array's length for `nrows`'s own reason — the
       runs ARE the partition, so a second count could disagree with it and nothing downstream could tell. */
    TableGridRowGroup *groups;
    size_t ngroups;
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

/* WHICH RUN OF GRID ROWS one ROW GROUP BOX occupies — §17.5's rule 2 read back the way a consumer holding an
   element has to read it, and NULL when rule 2's union over that group's rows is EMPTY.
   NULL HAS EXACTLY ONE CAUSE HERE, WHICH IS THE WHOLE REASON `TableGrid.table` EXISTS. `table_grid_cell_of`'s
   NULL and `table_grid_row_of`'s FALSE both fold in "the caller walked to the wrong table box", and their
   consumers crash on that. This entry cannot: an EMPTY `<tbody>` is a real row group box of this table holding
   no row, so a NULL that also meant "wrong table" would be a walk error wearing the shape of a legitimate
   answer, and the consumer that ATE it would report a real extent for a group of some other table. So the
   wrong-table case is asserted away here, where the walk fact lives, and the NULL that survives is rule 2's
   union over zero rows and nothing else. WHAT THAT EXTENT IS is not this file's answer — it is
   core/layout/table_height.h's `table_row_group_used_extent`, which records the choice CSS 2.1 declines.
   `group` IS NOT `const` WHERE THE TWO ENTRIES ABOVE ARE, and the difference is not an oversight: those two
   SCAN stored pointers and need nothing of the element, while this one asserts core/layout/table_box.h's own
   upward walk (`table_box_table_of`) against `TableGrid.table`, and that walk takes the element as the tree
   holds it. A cast at this boundary would hide which of the two shapes this entry is.
   §17.2.1's TABLE-BOX-PARENTED STRETCH CANNOT BE ASKED FOR, exactly as the anonymous cell and the anonymous row
   cannot: §17.2.1 Anonymous table objects generates no anonymous row group, so the NULL-element run above names
   no BOX, and `group` must not be NULL — a NULL would match the first such stretch and answer a question about
   a box that does not exist. */
const TableGridRowGroup *table_grid_row_group_of(const TableGrid *grid, lxb_dom_element_t *group);

/* WHICH GRID LINE an EMPTY ROW GROUP BOX stands on — the other arm of the entry above, and the ONE question
   rule 2's union over zero rows leaves an answer for. `group` must be a row group box of this grid's table for
   which the entry above answered NULL; a group holding rows is a caller's crash here, because that group has a
   RUN and its first grid row is `first` of it.
   IT IS A GRID LINE AND NOT A GRID ROW, and the difference is the whole content of the answer. There are
   `nrows + 1` lines and the empty group stands on the one its first row WOULD occupy — the count of grid rows
   §17.2 The CSS table model's display order places BEFORE it — which is `nrows` itself for a trailing empty
   group. That count is a FACT and not a choice: rule 1 fills the table with the row boxes "from top to bottom
   in the order they occur in the source document", so the rows before this group are the rows before it. WHAT
   IS A CHOICE is the DISTANCE that line is turned into, and it is recorded where the box is placed
   (core/layout/flow_position.c) rather than here, because §17.6.1 The separated borders model leaves one
   `border-spacing` of gap either side of the line and §17.5 names no edges for a box occupying no grid cell.
   IT IS AN ENTRY HERE AND NOT A WALK AT THE CONSUMER for the reason every entry above is: an EMPTY row group
   box is invisible in this grid's own arrays — it has no run, no row and no cell — so answering it means
   re-running core/layout/table_box.h's box generation, and a consumer that did so would be free to disagree
   with this grid about §17.2's display order while both looked locally right. That disagreement is silent: it
   moves the box and nothing else. */
size_t table_grid_empty_row_group_line(const TableGrid *grid, lxb_dom_element_t *group);

/* WHICH RUN a given GRID ROW is in — the same partition read from the other side, which is the side §17.5's
   RULE 6 is stated from: "A cell box cannot extend beyond the last row box of a table or row group", asked of
   a cell that knows its anchor row and not its group. NEVER NULL for `row < grid->nrows`, because the runs
   partition the rows; a `row` at or past `nrows` is a caller's crash and aborts here rather than answering.
   IT IS AN ENTRY AND NOT A SCAN AT THE CALLER because the PARTITION is this component's invariant: a scan
   written at a consumer would be free to fall off the end, or to take the first run whose `first` is not
   greater than `row`, and both look right on every table whose runs happen to be one row long. */
const TableGridRowGroup *table_grid_group_of_row(const TableGrid *grid, size_t row);

/* Releases what `table_grid_build` stored. A zero-cell grid holds NULL and is accepted. */
void table_grid_release(TableGrid *grid);

#endif
