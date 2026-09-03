/* CSS 2.1 §17.5 Visual layout of table contents' RULES 3 AND 4 — WHICH COLUMN AND COLUMN-GROUP BOX OCCUPIES
 * EACH GRID COLUMN. Rule 3: "A column box occupies one or more columns of grid cells. Column boxes are placed
 * next to each other in the order they occur." Rule 4: "A column group box occupies the same grid cells as the
 * columns it contains."
 *
 * WHY IT IS A COMPONENT AND NOT A LOOKUP AT ITS CALLER. Three separate algorithms put the SAME question to
 * these boxes and each of them reads a DIFFERENT property off the answer, so the walk that finds the box is the
 * only part they share. CSS 2.1 §17.5.2.2 Automatic table layout reads a declared `width` off a column in its
 * step 2 and off a column group in its step 4; §17.5.2.1 Fixed table layout reads the same `width` in its own
 * step 1; and CSS 2.1 §17.6.2.1 Border conflict resolution reads a BORDER off both at every cell edge, since
 * the borders that meet at an edge come from "cells, rows, row groups, columns, column groups, and the table
 * itself". A per-caller walk would be three walks over one child list, free to disagree about which grid
 * columns a `<colgroup span=3>` covers — and that disagreement is invisible, because every answer is a real box
 * of the real document.
 *
 * WHY IT IS NOT IN core/layout/table_grid.h, WHICH IS WHERE THAT HEADER SAYS THESE BOXES BELONG. That sentence
 * is right about the SUBJECT and wrong about the FILE, and it says so itself in the same paragraph: the boxes
 * "are not rendered (exactly as if they had 'display: none')" (CSS 2.1 §17.2 The CSS table model), so nothing
 * here is a placement in the sense that file's cell walk is — it is a MAPPING FROM THE COLUMN INDICES THAT WALK
 * ALREADY NUMBERS to the elements that carry properties for them. Putting it there would put a property-bearing
 * element lookup inside the component whose whole output is a rectangle per cell.
 *
 * HOW MANY COLUMNS A BOX OCCUPIES IS THE DOCUMENT LANGUAGE'S ANSWER, EXACTLY AS A CELL'S SPAN IS. Rule 3 says
 * "one or more" and never how many, and §17.5's rule 5 states the general licence for both: "Although CSS 2.1
 * does not define how the number of spanned rows or columns is determined, a user agent may have special
 * knowledge about the source document". HTML §4.9.12.1 Forming a table is that knowledge for the column boxes
 * as core/layout/table_grid.h already takes it for the cell boxes, and its two arms are what this component
 * runs: a `colgroup` WITH `col` children takes its width from those children ("Let the last span columns in the
 * table correspond to the current column col element"), and one WITHOUT them takes its own `span`. Both arms
 * end in the same clamp — "If span is greater than 1000, let it be 1000 instead" — and the same fallback:
 * "Otherwise, if the col element has no span attribute, or if trying to parse the attribute's value resulted in
 * an error or zero, then let span be 1."
 * THE ATTRIBUTE IS READ FROM AN HTML `col` OR `colgroup` AND FROM NOTHING ELSE, which is core/layout/
 * table_grid.h's narrowing for `colspan` and is the same reasoning: §17.5's sentence is that a UA may have
 * special knowledge of the source document, not that it may guess, so a `<div style="display: table-column">`
 * occupies exactly one grid column because no document language gave it a span.
 *
 * `direction` IS NOT READ AND RULE 3's OWN CLAUSE IS WHY. "The first column box may be either on the left or on
 * the right, depending on the value of the 'direction' property of the table" is a statement about where column
 * 0 is PAINTED, and core/layout/table_grid.h numbers its columns in §17.5's own LOGICAL order for exactly that
 * reason — rule 5's cell placement carries the mirrored `ltr`/`rtl` reading in the same way. Column boxes in
 * document order therefore map onto logical columns 0, 1, 2 … under both values, and a component that read
 * `direction` here would be reversing the mapping a second time.
 *
 * WHAT IS DELIBERATELY NOT HERE. The `width` and the borders themselves: this answers WHICH BOX, and each
 * algorithm reads its own property off it and states its own crash when that property is one CSS 2.1 declines
 * to resolve. A component that read `width` here would have to pick one of §17.5.2.1's and §17.5.2.2's two
 * different uses of it. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_COLUMN_BOX_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_COLUMN_BOX_H

#include <stddef.h>

#include <lexbor/dom/dom.h>

/* THE TWO BOXES OCCUPYING ONE GRID COLUMN. Either may be NULL and both commonly are — a table with no `<col>`
   at all is the ordinary shape — and NULL is a POSITIVE STATEMENT that no box of that kind occupies this
   column, never a value a consumer may default past. The pair is answered together because rule 4 makes them
   ONE fact: a column group occupies the grid cells of the columns it contains, so `column_group` is the group
   `column` sits in whenever both are present, and a consumer holding only one of them could not check that. */
typedef struct {
    lxb_dom_element_t *column;        /* the 'table-column' box occupying this grid column, or NULL */
    lxb_dom_element_t *column_group;  /* the 'table-column-group' box occupying it, or NULL */
} TableColumnBoxes;

/* ONE TABLE'S WHOLE MAPPING. `cols` holds `ncols` entries, indexed by the grid column indices
   core/layout/table_grid.h numbers.
   `ncols` IS THE GREATER OF THE GRID'S COLUMN COUNT AND `noccupied`, AND THAT IS NOT A CONVENIENCE. The two
   counts can differ in both directions and each direction is a real document: a `<col>` styling the first
   column of three-cell rows occupies fewer columns than the grid has, and a `<colgroup span="5">` above
   two-cell rows occupies more. The second is the one a consumer must be able to SEE — HTML §4.9.12.1 Forming a
   table increases x_width by every column group's span, so such a table is five columns wide in HTML and the
   grid's cell walk reports two — and it is invisible unless the entries past the grid's count are addressable.
   So they are, and a consumer states its own invariant over them rather than being handed a truncated map that
   agrees with the grid by construction. */
typedef struct {
    TableColumnBoxes *cols;
    size_t ncols;       /* entries in `cols` */
    size_t noccupied;   /* grid columns the column and column-group boxes together occupy — HTML §4.9.12.1's
                           contribution to x_width from the table's column groups, before any row is read */
} TableColumnBoxMap;

/* Builds `table`'s mapping over a grid of `ngrid` columns (core/layout/table_grid.h's `ncols`). The caller
   releases the result with `table_column_boxes_release`.
   A TABLE WITH NEITHER COLUMNS NOR COLUMN BOXES IS A REAL TABLE: `ncols` and `noccupied` are zero and `cols` is
   NULL, which is what `<table><tr><td></table>`'s single column answers for its one entry and what
   `<table></table>` answers for none. */
void table_column_boxes_build(lxb_dom_element_t *table, size_t ngrid, TableColumnBoxMap *out);

/* Releases what `table_column_boxes_build` stored. A zero-entry map holds NULL and is accepted. */
void table_column_boxes_release(TableColumnBoxMap *map);

#endif
