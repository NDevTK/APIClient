/* CSS 2.1 §17.5 Visual layout of table contents' grid: which grid cell each cell box is anchored at and how many
 * it covers, the operand of §17.5.2 Table width algorithms: the 'table-layout' property. It is separate from
 * core/layout/table_box.h (which boxes exist) because placement needs other input: rule 5's "must not overlap
 * with any other cell box (i.e., a row-spanning cell starting in a prior row)".
 *
 * Spans come from HTML, as rule 5 allows ("a user agent may have special knowledge about the source
 * document"): HTML §4.9.11 Attributes common to td and th elements (`colspan` up to 1000, `rowspan` up to
 * 65534, and rowspan zero "means that the cell is to span all the remaining rows in the row group"), parsed and
 * clamped by HTML §4.9.12.1 Forming a table's steps, and read only from `td` and `th`; a `display: table-cell`
 * div or an anonymous cell spans 1×1. Rule 6 ("A cell box cannot extend beyond the last row box of a table or
 * row group; the user agents must shorten it until it fits") is the clamp that also implements `rowspan=0`;
 * table-parented rows form one group for it, as §4.9.12.1 forms one.
 *
 * Rule 5's overlap case ("CSS does not define the results: implementations may either overlap the cells … or
 * may shift the later cell to the right") takes HTML §4.9.12.1's overlapping reading, recorded per cell. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_GRID_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_GRID_H

/* Rule 1 ("Each row box occupies one row of grid cells. Together, the row boxes fill the table from top to
   bottom in the order they occur in the source document") is placed here by the walk that anchors cells.
   Rules 3 and 4 (column and column-group boxes) are a mapping onto these column indices, since those boxes "are
   not rendered (exactly as if they had 'display: none')", and are core/layout/table_column_box.h's. Indices are
   logical; rule 3's `direction` clause maps them to positions.
   Rule 2 ("A row group occupies the same grid cells as the rows it contains") is the same run of rows rule 6
   clamps against, so both read one array. Inline-axis extent needs no run (every row spans the whole grid
   row); the block-axis extent is core/layout/table_height.h's. */

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

/* One cell box's rectangle of grid cells. §17.5: "Each cell is thus a rectangular box, one or more grid cells
   wide and high." `element` is NULL for §17.2.1's anonymous 'table-cell' box, which is always 1×1. */
typedef struct {
    lxb_dom_element_t *element;
    size_t row;        /* the grid row it is anchored at — §17.5: "The top row of this rectangle is in the row
                          specified by the cell's parent" */
    size_t col;        /* the leftmost grid column it occupies, in §17.5's own logical order */
    size_t rowspan;    /* grid rows covered, at least 1, already shortened by rule 6 */
    size_t colspan;    /* grid columns covered, at least 1 */
    /* §17.5 rule 5's undefined case, recorded: a slot this cell covers was already covered by a cell from a
       prior row. Never the anchor slot (the placement skips occupied slots), so it is always a column-spanning
       cell reaching into a row-spanning one. */
    bool overlaps;
} TableGridCell;

/* One maximal run of consecutive grid rows reporting the same row group: rule 2's extent and rule 6's bound.
   `element` is NULL for a stretch of table-parented rows, which is not a box (§17.2.1 generates no anonymous row
   group) but still bounds a `rowspan`, as HTML §4.9.12.1 gathers such rows into a group;
   `table_grid_row_group_of` therefore refuses NULL.
   The runs partition the grid rows: in grid order, `groups[0].first` is 0 when there is any row, each `first`
   is the previous `first + nrows`, and every `nrows` is at least 1, asserted where built. */
typedef struct {
    lxb_dom_element_t *element;
    size_t first;    /* the first grid row of the run */
    size_t nrows;    /* grid rows covered, at least 1 */
} TableGridRowGroup;

/* The whole grid of one table box. Cells are by grid row, then in core/layout/table_box.h's (tree) order. */
typedef struct {
    /* The table box this grid is of, stored so "not in this grid" and "in this grid, occupying nothing" (an
       empty `<tbody>`) are different answers; see `table_grid_row_group_of`. */
    lxb_dom_element_t *table;
    TableGridCell *cells;
    size_t ncells;
    /* §17.5's rule 1 as a placement: `rows[y]` is the row element whose box occupies grid row `y`, in
       core/layout/table_box.h's display order. NULL is §17.2.1's anonymous 'table-row' box. `nrows` is the
       array's length ("the table occupies exactly as many grid rows as there are row elements"), so there is
       no second count. NULL when `nrows` is zero. */
    lxb_dom_element_t **rows;
    size_t nrows;
    /* Rule 2 and rule 6's bound, as the runs above: grid order, partitioning rows `0..nrows-1`; NULL and zero
       for a table with no rows. `ngroups` is the array's length. */
    TableGridRowGroup *groups;
    size_t ngroups;
    size_t ncols;       /* the widest any row reached, which is the table's column count */
    bool any_overlap;   /* whether rule 5's undefined case arose at all, so a consumer can ask once */
} TableGrid;

/* Builds `table`'s grid. `table` must generate a table box (core/layout/table_box.h's `table_box_kind`); the
   caller releases the result with `table_grid_release`. A table with no rows is a real grid (all counts zero). */
void table_grid_build(lxb_dom_element_t *table, TableGrid *out);

/* The rectangle one cell element was placed at, or NULL when this grid placed no cell for it (it is not a cell
   of this table, or no rule placed it; both are the consumer's crash). An entry so the one-match invariant,
   a fact about §17.5's placement, is held here. `cell` must not be NULL: an anonymous cell cannot be named,
   and NULL would match the first one. */
const TableGridCell *table_grid_cell_of(const TableGrid *grid, const lxb_dom_element_t *cell);

/* The grid row one row element's box occupies (rule 1 read back). Answers true and stores the index at `*out`;
   false, leaving `*out` untouched, when the element is not a row of this table (a caller that reached the wrong
   table box). A scan through cells would miss `<tr></tr>`, which has a real height and position.
   `*out` is not written on false so an ignored answer is an uninitialised read `-Wall` and sanitizers catch,
   not a sentinel quietly used as a number. `row` must not be NULL (an anonymous row cannot be named). */
bool table_grid_row_of(const TableGrid *grid, const lxb_dom_element_t *row, size_t *out);

/* The run of grid rows one row group box occupies (rule 2 read back), or NULL when that union is empty (an
   empty `<tbody>`). NULL has exactly that one cause: the wrong-table case is asserted here against
   `TableGrid.table`, since a consumer could not tell it from a real empty group. The empty group's extent is
   core/layout/table_height.h's `table_row_group_used_extent`.
   `group` is not `const` because the assert runs core/layout/table_box.h's `table_box_table_of`, which takes
   the element as the tree holds it. `group` must not be NULL: the table-parented stretch names no box. */
const TableGridRowGroup *table_grid_row_group_of(const TableGrid *grid, lxb_dom_element_t *group);

/* The grid line an empty row group box stands on. `group` must be a row group of this grid's table for which
   `table_grid_row_group_of` answered NULL; a group with rows is a caller's crash. There are `nrows + 1` lines,
   and the group stands on the count of rows §17.2's display order places before it (`nrows` for a trailing
   one), a fact from rule 1. The distance that line becomes is a choice recorded at placement
   (core/layout/flow_position.c), since §17.6.1 leaves a `border-spacing` gap either side and §17.5 names no
   edges for such a box. An entry because the empty group is in none of this grid's arrays, so answering means
   re-running core/layout/table_box.h's generation, which must agree with this grid's order. */
size_t table_grid_empty_row_group_line(const TableGrid *grid, lxb_dom_element_t *group);

/* The run a given grid row is in: the partition read from rule 6's side ("A cell box cannot extend beyond the
   last row box of a table or row group"). Never NULL for `row < grid->nrows`; a larger `row` aborts. An entry
   because the partition is this component's invariant. */
const TableGridRowGroup *table_grid_group_of_row(const TableGrid *grid, size_t row);

/* Releases what `table_grid_build` stored. A zero-cell grid holds NULL and is accepted. */
void table_grid_release(TableGrid *grid);

#endif
