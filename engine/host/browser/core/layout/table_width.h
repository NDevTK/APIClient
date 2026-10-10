/* CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property: the used width of a table box and of
 * each of its columns. §17.4 sizes the wrapper from it ("the border-edge width of the table box inside it, as
 * described by section 17.5.2") and §17.5.3 Table height algorithms measures cells over it.
 *
 * core/layout/table_column_width.h answers §17.5.2.2's per-column pair from the cells' content; this is the
 * rest: the choice between the two algorithms, CAPMIN from the wrapper's captions, §17.6.1's cell spacing, the
 * comparison against the table's `width` and containing block, and the distribution back over the columns.
 * §17.5.2.1 Fixed table layout reads no cell outside the first row, so it must not depend on that measurement.
 *
 * Only the fixed algorithm is normative ("except when the fixed layout algorithm is selected"); §17.5.2.2 says
 * "The remainder of this section is non-normative". Every place that licence is taken is recorded at its site
 * rather than crashed on. `table-layout: fixed` with `width: auto` takes the automatic algorithm: "A value of
 * 'auto' (for both 'display: table' and 'display: inline-table') means use the automatic table layout
 * algorithm." The optional §10.3.3 route the same paragraph offers is declined, so such a table is laid out
 * from all its cells. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_WIDTH_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_WIDTH_H

/* Units. A column width is a border-box width of its cells (core/layout/table_column_width.h's choice). The
   table's width is §17.6.1's "distance from the left inner padding edge to the right inner padding edge
   (including the border spacing but excluding padding and border)": the table box's content width, spacing
   included. A declared `width` on an HTML `<table>` element (not a `display: table` div) is a border-box number
   ("in HTML and XHTML1, the width of the <table> element is the distance from the left border edge to the
   right border edge"), applied here rather than as a UA `box-sizing`, which would change what
   `getComputedStyle(table).boxSizing` reports.
   §17.6.2 The collapsing border model is laid out through two terms this component owns: the spacing (zero)
   and the table box's own edges (no padding, §17.6.2's two half-borders); cell borders come through
   `table_cell_border_edges` (core/layout/table_border_collapse.h). Nothing is stored between calls; the
   allocated array is the caller's for one question. */

#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/table_grid.h"

/* §17.5.2's answer: one fact in three shapes. `content` is the sum of `columns` and `spacing` taken between and
   around them, asserted at the entry's boundary. `spacing` is a field because a table with no columns takes
   none, so it cannot be recovered from `content`, and a second read of `border-spacing` could disagree with
   the one the columns were laid out under. */
typedef struct {
    /* The used width of each grid column, in §17.5's logical column order (core/layout/table_grid.h), as a
       border-box width of its cells. `ncols` entries, or NULL when `ncols` is zero. */
    CssPx  *columns;
    size_t  ncols;
    /* §17.6.1's "width of the table": the table box's used content width, including the border spacing and
       excluding its own padding and border. §17.4's wrapper width adds the table box's horizontal surround,
       which depends on the border model, so it goes through core/layout/used_value.h (which takes §17.6.2's
       halves for a collapsed table: "a table does not have padding (but does have margins)", "The left border
       width of the table is half of the first cell's collapsed left border"), never through the computed
       `border-*-width`. For `<table style="border-collapse:collapse;border:10px solid">` with borderless cells
       the border box is this width plus 10, not plus 20. */
    CssPx   content;
    /* §17.6.1 The separated borders model's horizontal `border-spacing` ("the distance between the borders of
       adjoining cells"), which `content` holds `ncols + 1` of, since "The distance between the table border
       and the borders of the cells on the edge of the table is the table's padding for that side plus the
       relevant border-spacing distance". The vertical half is §17.5.3's.
       It is this algorithm's term, not the property: zero under §17.6.2 ("Borders are centered on the grid
       lines between the cells") whatever `border-spacing` computed to. Consumers add this field. */
    CssPx   spacing;
} TableUsedWidths;

/* §17.5.2 over `table` and `grid`, which must be that table's own (core/layout/table_grid.h). `table` must
   generate a table box (`table` or `inline-table`, core/layout/table_box.h's `table_box_kind`); a
   table-internal box crashes, since a cell's width is a column's and a row's is the grid row's.
   The result is the caller's and is released with `table_widths_release`. A table with no columns is real;
   its width is the greater of its declared `width`, CAPMIN and zero. An input §17.5.2 cannot resolve crashes
   by name, since every term is a floor under the table's width. */
void table_widths(lxb_dom_element_t *table, const TableGrid *grid, TableUsedWidths *out);

/* css-sizing-3 §5.1 "Intrinsic Sizes"' pair for a table box over `table` and its own `grid`: a producer of
   `IntrinsicInlineSizes` beside core/layout/intrinsic_size.h and core/layout/flex_intrinsic_size.h. `table`
   must generate a table box, as for `table_widths`.
   It is §17.5.2.2's `width: auto` rule evaluated at §5.1's two containing blocks: "the used width is the
   greater of the table's containing block width, CAPMIN, and MIN. However, if either CAPMIN or the maximum
   width required by the columns plus cell spacing or borders (MAX) is less than that of the containing block,
   use max(MAX, CAPMIN)." At zero the "However" cannot fire, giving max(CAPMIN, MIN); at infinity it always
   fires, giving max(MAX, CAPMIN). §17.5.2.1 Fixed table layout has no arm: §5.1 assumes "an auto preferred
   size", and the fixed algorithm hands a `width: auto` table to the automatic one.
   It must not read the containing block, so it is not `table_widths` read out: inside a float or inline-block,
   CSS 2.2 §10.3.5's shrink-to-fit asks the ancestor's `intrinsic_inline_sizes`, which asks this table again
   (`<div style="float:left"><table>…</table></div>` would recurse without end). Its operands stay
   containing-block-free (table_column_width.c's percentage arms crash; CAPMIN resolves cyclic percentages to
   zero under css-sizing-3 §5.2.1). The answer is the table box's content width, like `TableUsedWidths.content`;
   the caller's §3.3 conversion over the table box's surround yields §17.4's wrapper width. */
IntrinsicInlineSizes table_intrinsic_inline_sizes(lxb_dom_element_t *table, const TableGrid *grid);

/* One cell's used border-box width from the answer above: "Each cell is thus a rectangular box, one or more
   grid cells wide and high", so it is the used width of the columns it covers. `cell` must be a cell of the
   grid `widths` was computed over (core/layout/table_grid.h's `table_grid_cell_of`).
   A cell spanning N columns also gets N-1 of `spacing`, a recorded reading CSS 2.1 leaves open: §17.6.1's
   spacing is "the distance between the borders of adjoining cells" and there are none inside one cell, and
   this keeps §17.6.1's sum whole (charging columns only would make the cell narrower than its row). Under
   §17.6.2 the term is zero and the answer is still exact: each interior grid line's border is charged half to
   each neighbouring column, so the summed columns hold it whole.
   It is not a used `width`: css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property" exposes the
   content box under `content-box`, which is this less core/layout/table_column_width.h's
   `table_cell_border_edges`. */
CssPx table_cell_used_border_box(const TableUsedWidths *widths, const TableGridCell *cell);

/* Releases what `table_widths` stored. A zero-column answer holds NULL and is accepted. */
void table_widths_release(TableUsedWidths *w);

#endif
