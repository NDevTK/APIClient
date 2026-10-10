/* CSS 2.1 §17.5.2.2 Automatic table layout's column widths: the four steps under "Column widths are determined
 * as follows", ending "This gives a maximum and minimum width for each column." That sentence is the boundary:
 * only these steps read cell content (via core/layout/intrinsic_size.h); CAPMIN, the final table width and the
 * distribution of a used width back over the columns are core/layout/table_width.h's. §17.5.2.1 Fixed table
 * layout is a different algorithm; the caller has chosen by `table-layout`. §17.5.3 consumes used widths.
 *
 * The numbers are the cells' border-box widths, a choice §17.5.2.2 leaves open: its final rules add "cell
 * spacing or borders" (§17.6's terms) separately, and css-sizing-3 §2.2 "Intrinsic Size Contributions" bases
 * contributions on "the outer size of the box", which for a cell is its border box because CSS 2.1 §8.3
 * "Margin properties" does not apply to internal table boxes (asked of core/css/css_property_applies.h).
 *
 * `max` is never below `min`: a declared cell `width` raises only the minimum, and steps 3 and 4 widen the two
 * independently, so the maximum is floored at the minimum where they can cross. Nothing is stored (layout is
 * per-flow state solver/dom_cow.h does not swap). Refusals are DCHECK/DFAIL although core/layout/table_width.c
 * consumes this in release; `tcw_declared_width`'s release arm drops a floor rather than fabricating one. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_COLUMN_WIDTH_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_COLUMN_WIDTH_H

#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/table_grid.h"

/* One grid column's two constraints, in CSS pixels, as border-box widths of the cells that occupy it. They
   come from one walk and are returned together so the pair's relation (`min <= max`) is checkable. They are
   `CssPx` because a cell's content width can depend on the environment (a `rem` font size, the viewport under
   CSS 2.1 §10.1's containing block), and css_length.h's arithmetic carries that domain. */
typedef struct {
    CssPx min;   /* §17.5.2.2's "minimum column width" */
    CssPx max;   /* §17.5.2.2's "maximum column width" */
} TableColumnWidth;

/* §17.5.2.2's four steps over `grid`, which must be `table`'s own (core/layout/table_grid.h). Answers
   `grid->ncols` and stores a newly allocated array of that many at `*out`, which the caller owns and frees; a
   count of zero stores NULL. Zero columns is a real table (`<table></table>`). An input that cannot be read
   crashes by name rather than being left out, since a term omitted from a minimum narrows the column. */
size_t table_column_widths(lxb_dom_element_t *table, const TableGrid *grid, TableColumnWidth **out);

/* One cell's horizontal padding and border, in CSS pixels: the difference between the content box
   core/layout/intrinsic_size.h answers in and the border box §17.5.2 measures columns in. Shared by
   §17.5.2.2's step 1, §17.5.2.1 Fixed table layout's step 2 (converting a first-row cell's declared `width`)
   and core/layout/used_value.c, so the four-term sum is spelled once. It is not used_value.c's surround; see
   table_column_width.c for the cycle that separates them.
   It answers both of CSS 2.1 §17.6 Borders' models itself, from the cell's table: under §17.6.1 The separated
   borders model the cell's own `border-*-width`; under §17.6.2 The collapsing border model half of §17.6.2.1
   Border conflict resolution's winner at each vertical grid line, zero on the perimeter
   (core/layout/table_border_collapse.h). */
CssPx table_cell_border_edges(lxb_dom_element_t *cell);

/* CSS 2.1 §17.5 Visual layout of table contents' rules 3 and 4 (which box occupies each grid column) are
   core/layout/table_column_box.h. */

#endif
