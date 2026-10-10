/* CSS 2.1 §17.5 Visual layout of table contents' rules 3 and 4: which column and column-group box occupies
 * each grid column. Rule 3: "A column box occupies one or more columns of grid cells. Column boxes are placed
 * next to each other in the order they occur." Rule 4: "A column group box occupies the same grid cells as the
 * columns it contains."
 *
 * Three algorithms read different properties off this one mapping (§17.5.2.2 Automatic table layout and
 * §17.5.2.1 Fixed table layout read `width`; CSS 2.1 §17.6.2.1 Border conflict resolution reads borders), so the
 * walk is one component they share and cannot disagree over. It answers which box only; each algorithm reads
 * its own property. It is separate from core/layout/table_grid.h because these boxes "are not rendered (exactly
 * as if they had 'display: none')" (CSS 2.1 §17.2 The CSS table model): it maps grid column indices to elements.
 *
 * Spans are the document language's (rule 5: "a user agent may have special knowledge about the source
 * document"): HTML §4.9.12.1 Forming a table's `span` steps, read only from HTML `col` and `colgroup`, so a
 * `display: table-column` div occupies one column. `direction` is not read: table_grid.h numbers columns in
 * logical order, so rule 3's left-or-right clause is about painting. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_COLUMN_BOX_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_COLUMN_BOX_H

#include <stddef.h>

#include <lexbor/dom/dom.h>

/* The two boxes occupying one grid column. Either may be NULL, which positively states that no box of that
   kind occupies it. They are answered together because rule 4 makes `column_group` the group `column` sits in
   whenever both are present. */
typedef struct {
    lxb_dom_element_t *column;        /* the 'table-column' box occupying this grid column, or NULL */
    lxb_dom_element_t *column_group;  /* the 'table-column-group' box occupying it, or NULL */
} TableColumnBoxes;

/* One table's whole mapping. `cols` holds `ncols` entries, indexed by core/layout/table_grid.h's grid column
   indices. `ncols` is the greater of the grid's column count and `noccupied`: a `<colgroup span="5">` over
   two-cell rows occupies more columns than the cell walk reports (HTML §4.9.12.1 Forming a table grows
   x_width by every group's span), and the entries past the grid's count stay addressable so a consumer can
   state its own invariant over them. */
typedef struct {
    TableColumnBoxes *cols;
    size_t ncols;       /* entries in `cols` */
    size_t noccupied;   /* grid columns the column and column-group boxes together occupy — HTML §4.9.12.1's
                           contribution to x_width from the table's column groups, before any row is read */
} TableColumnBoxMap;

/* Builds `table`'s mapping over a grid of `ngrid` columns (core/layout/table_grid.h's `ncols`). The caller
   releases the result with `table_column_boxes_release`. A table with neither columns nor column boxes
   (`<table></table>`) answers `ncols` and `noccupied` zero and `cols` NULL. */
void table_column_boxes_build(lxb_dom_element_t *table, size_t ngrid, TableColumnBoxMap *out);

/* Releases what `table_column_boxes_build` stored. A zero-entry map holds NULL and is accepted. */
void table_column_boxes_release(TableColumnBoxMap *map);

#endif
