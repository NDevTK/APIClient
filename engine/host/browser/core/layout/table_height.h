/* CSS 2.1 §17.5.3 Table height algorithms — THE TABLE BOX'S HEIGHT, ITS ROWS' HEIGHTS, AND THE ONE PLACE THIS
 * ENGINE DECIDES WHAT CSS 2.1 DECLINES TO.
 *
 * IT IS A SUM AND NOT A DISTRIBUTION, WHICH IS THE HALF A CAREFUL READING GETS BACKWARDS. §17.5.3's own first
 * sentence pair: "A value of 'auto' means that the height is the sum of the row heights plus any cell spacing
 * or borders. Any other value is treated as a minimum height." A declared height therefore FLOORS the sum; it
 * does not divide anything. The section then says outright that the other direction is not defined at all —
 * "CSS 2.1 does not define how extra space is distributed when the 'height' property causes the table to be
 * taller than it otherwise would be" — so a component that spread the surplus over the rows would be answering
 * a question the standard declines, and one that crashed on it would refuse a document every reading is
 * conforming for. THE CHOICE IS RECORDED AT THE SITE and is the same shape core/layout/table_width.h records
 * its own §17.5.2.2 licences in: the surplus is left BELOW the last row, inside the table box's content edge.
 *
 * A ROW'S HEIGHT IS A MAXIMUM OF THREE TERMS AND ONLY THE THIRD IS CONTENT, which is the other half a careful
 * reading gets backwards. §17.5.3: "it is the maximum of the row's computed 'height', the computed 'height' of
 * each cell in the row, and the minimum height (MIN) required by the cells." So a `<tr style="height:200px">`
 * of one-line cells is 200 tall by the FIRST term, with nothing in it measured; and `A 'height' value of 'auto'
 * for a 'table-row' means the row height used for layout is MIN` is why an undeclared row contributes nothing
 * to its own maximum rather than contributing a zero that would have to be maxed away.
 *
 * WHAT MIN IS, AND WHY IT IS NOT `max(cell heights)`. §17.5.3 does not write MIN as a formula; it says "MIN
 * depends on cell box heights and cell box ALIGNMENT (much like the calculation of a line box height)" and then
 * gives the alignment procedure in full — baseline cells first, which "will establish the baseline of the row";
 * then `top` cells; then "The row now has a top, possibly a baseline, and a provisional height, which is the
 * distance from the top to the lowest bottom of the cells positioned so far"; then "If any of the remaining
 * cells, those aligned at the bottom or the middle, have a height that is larger than the current height of the
 * row, the height of the row will be increased to the maximum of those cells". Reduced over those four steps
 * MIN is
 *     max( max(A) + max(H - A) over the BASELINE cells,  max(H) over every other cell )
 * with `A` the distance from a cell box's top to its own baseline and `H` its box height — and the first term
 * EXCEEDS `max(H)` exactly when two baseline cells have different `A`. Two one-line cells, one with 20px of
 * top padding and one with 20px of bottom padding, are the smallest document that shows it: `max(H)` is
 * `20 + line-height` and the row is really `40 + line-height` tall. So `max(H)` is a FLOOR under MIN and never
 * MIN, and reporting a floor here is the defect this project refuses at the field level — a rectangle no
 * reader can tell from a measured one, travelling out through §17.4's wrapper and CSSOM VIEW.
 *   THE ≤1 CASE IS EXACT AND IS NOT A NARROWING. With no baseline cell the first term is absent; with exactly
 *   one, `max(A) + max(H - A)` is `A₁ + (H₁ - A₁)` = `H₁`, which `max(H)` already covers. So a row with at most
 *   one baseline-aligned cell has `MIN = max(H)` by the procedure itself, and this component takes that arm
 *   without measuring a single baseline.
 *
 * WHICH ALIGNMENT A CELL HAS IS READ THROUGH css-inline-3's LONGHANDS AND NOT THROUGH `vertical-align`, and
 * that is this fork's cascade rather than a substitution invented here. css-inline-3 §4.2 "Transverse Box
 * Alignment: the vertical-align property" makes `vertical-align` a SHORTHAND — `[ first | last] ||
 * <'alignment-baseline'> || <'baseline-shift'>`, `Computed value: see individual properties` — and states the
 * table-cell mapping in its own note: "vertical-align can also affect the alignment of table cells when
 * align-content is normal. Specifically, top (baseline-shift: top) maps it to start, bottom (baseline-shift:
 * bottom) to end, and otherwise middle (alignment-baseline: middle) to center." core/css/css_shorthand.c
 * expands the shorthand into those three longhands and core/css/css_computed_value.c models `baseline-shift`
 * and `alignment-baseline`, so the four cases §17.5.3 names are readable exactly as written — and the section's
 * own catch-all is what makes the remaining values collapse rather than needing a rule: "sub, super, text-top,
 * text-bottom, <length>, <percentage> … These values do not apply to cells; the cell is aligned at the baseline
 * instead."
 *
 * EVERY CASE §17.5.3 DECLINES IS TAKEN AS A CHOICE AND RECORDED; EVERY CASE WHERE THE ANSWER WOULD BE WRONG
 * CRASHES. The section declines three things by name and this component answers all three at the site:
 *   - "CSS 2.1 does not define how the height of table cells and table rows is calculated when their height is
 *     specified using percentage values" — a percentage `height` on a row or a cell contributes nothing to the
 *     row's maximum, which is `auto`'s own arm.
 *   - "CSS 2.1 does not define the meaning of 'height' on row groups" — a row group's declared height is read
 *     by nothing here.
 *   - "CSS 2.1 does not specify how cells that span more than one row affect row height calculations EXCEPT
 *     that the sum of the row heights involved must be great enough to encompass the cell spanning the rows" —
 *     which is a CONSTRAINT and not a decline, so it is satisfied, and the freedom it leaves (which of the
 *     spanned rows grows) is the recorded choice.
 * §10.7 "Minimum and maximum heights: 'min-height' and 'max-height'" is not read at all, and that is its own
 * sentence rather than an omission here: "In CSS 2.1, the effect of 'min-height' and 'max-height' on tables,
 * inline tables, table cells, table rows, and row groups is undefined."
 *
 * §17.6.2 The collapsing border model IS REFUSED BY NAME AT THIS COMPONENT'S ENTRY, exactly as §17.5.2's is and
 * for the vertical twin of its reasons: §17.5.3's "plus any cell spacing or borders" is §17.6.1's vertical
 * `border-spacing` in the separated model and the table's own resolved borders in the collapsing one, and a
 * cell box's height here is measured over that cell's OWN computed `border-*-width`, which §17.6.2 replaces
 * with a resolution across the grid line. Running one model's arithmetic over the other reports every row and
 * the whole table taller than a browser lays them out.
 *
 * NOTHING IS STORED BETWEEN CALLS, for core/layout/block_flow.h's reason: a layout is per-flow state, so a
 * cached table height is shared state solver/dom_cow.h's delta does not swap and a stale one is another flow's
 * document. The array this entry allocates is the CALLER's for the duration of one question. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_HEIGHT_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_HEIGHT_H

#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/table_grid.h"

/* §17.5.3's ANSWER, which is ONE fact in three shapes and never three facts — the vertical twin of
   core/layout/table_width.h's `TableUsedWidths`, field for field, because the two sections are the same shape:
   `content` is the sum of `rows` and `spacing` taken between and around them, and the entry below asserts that
   at its boundary. They are returned together because a consumer holding one of them cannot derive the others.
   `spacing` IS A FIELD RATHER THAN A CONSUMER'S SECOND READ OF `border-spacing`, for the width answer's own two
   reasons: a table with NO rows takes no spacing at all, so a consumer inverting the sum would answer a spacing
   of zero for a table that declares one; and a second read would be a second place for §17.6.1's `Computed
   value: two absolute lengths` to be resolved, free to disagree with the one the rows were laid out under. */
typedef struct {
    /* The USED height of each grid row, in core/layout/table_grid.h's own row order, as a BORDER-BOX height of
       the cells that occupy it — the same box §17.5's own last paragraph puts a row's edges at in the separated
       model ("the edges coincide with the border edges of cells"). `nrows` entries, or NULL when `nrows` is 0. */
    CssPx  *rows;
    size_t  nrows;
    /* The table BOX's used CONTENT height, INCLUDING the vertical border spacing and excluding its own padding
       and border — §17.5.3's "the sum of the row heights plus any cell spacing or borders", floored by the
       table's own declared `height` because that section makes a declared value "a minimum height".
       IT IS THE CONTENT BOX ON THIS AXIS AND THAT IS NOT AN ASSUMPTION: §17.6.1 The separated borders model's
       border-edge exception — "in HTML and XHTML1, the width of the <table> element is the distance from the
       left border edge to the right border edge" — is written about the WIDTH and about nothing else, so the
       block axis has no such peculiarity and a declared `height` is an ordinary content-box length under the
       initial `box-sizing`. */
    CssPx   content;
    /* §17.6.1's VERTICAL `border-spacing` — "the first gives the horizontal spacing and the second the vertical
       spacing" — which `content` holds `nrows + 1` of, one between each adjoining pair of rows and one at each
       end, since §17.6.1's "the distance between the table border and the borders of the cells on the edge of
       the table is the table's padding for that side, plus the relevant border spacing distance" is written
       "for that side" and so covers the top and the bottom. The HORIZONTAL half is §17.5.2's and is not here. */
    CssPx   spacing;
} TableUsedHeights;

/* §17.5.3 over `table` and `grid`, which must be that table's own (core/layout/table_grid.h). `table` must
   generate a TABLE box — `table` or `inline-table` (core/layout/table_box.h's `table_box_kind`) — and a
   table-internal box crashes rather than being answered, because a CELL's used height is a ROW's and a ROW's is
   this answer's element, which are different questions with different answers.
   THE RESULT IS THE CALLER'S and is released with `table_heights_release`.
   A TABLE WITH NO ROWS IS A REAL TABLE AND NOT A FAILURE — `<table></table>`, or one whose only child is a
   caption — and its height is then its own declared `height` or zero, which is a number.
   EVERY INPUT §17.5.3 CANNOT RESOLVE CRASHES BY NAME rather than being dropped from a maximum: every term here
   is a FLOOR under a row, so a term left out reports a table SHORTER than the document asks for, and that
   number travels into §17.4's wrapper, into core/layout/flow_position.h's coordinates and out through CSSOM
   VIEW as a rectangle no reader can distinguish from a measured one. */
void table_heights(lxb_dom_element_t *table, const TableGrid *grid, TableUsedHeights *out);

/* ONE CELL'S USED BORDER-BOX HEIGHT, taken out of the answer above — the vertical twin of
   core/layout/table_width.h's `table_cell_used_border_box`, and a DERIVATION from §17.5 Visual layout of table
   contents rather than a second algorithm. §17.5: "Each cell is thus a rectangular box, one or more grid cells
   wide and high", and that section's own last paragraph puts the row edges at the cell border edges in the
   separated model — so a cell's box fills the rows its rectangle covers. §17.5.3 states the same thing from the
   cell's side: "Cell boxes that are smaller than the height of the row receive extra top or bottom padding."
   `cell` must be a cell of the grid `heights` was computed over (core/layout/table_grid.h's
   `table_grid_cell_of`).
   THE SPANNING CASE TAKES THE SAME READING THE WIDTH ANSWER TAKES, for the same §17.6.1 reason: a cell covering
   N rows gets those N used heights PLUS N-1 of `spacing`, because `border-spacing` is "the distance that
   separates adjoining cell borders" and the rows inside one cell's rectangle have no adjoining cell borders
   between them — that spacing is inside the cell, not between two of them. It is also what makes §17.5.3's own
   spanning CONSTRAINT ("the sum of the row heights involved must be great enough to encompass the cell spanning
   the rows") a statement this component can assert, since both sides are then measured in the same box.
   IT IS NOT A USED `height` IN CSS 2.1's SENSE and a caller must not report it as one: css-sizing-3 §3.3 "Box
   Edges for Sizing: the box-sizing property" decides which box a used value is exposed in, and under the
   initial `content-box` that is the content box — this number less `table_cell_vertical_edges` below. */
CssPx table_cell_used_border_box_height(const TableUsedHeights *heights, const TableGridCell *cell);

/* ONE CELL'S VERTICAL PADDING AND BORDER, in CSS pixels — the exact mirror of core/layout/table_column_width.h's
   `table_cell_border_edges`, on the other axis, and exported for the same reason: it is the difference between
   the CONTENT box §10.6.3's walk answers in and the BORDER box every row height here is measured in, and two
   spellings of one four-term sum are two places for the terms to come to disagree.
   IT LIVES HERE AND NOT BESIDE ITS HORIZONTAL TWIN because that one is an operand of §17.5.2's two column
   algorithms and this one is an operand of §17.5.3's row maximum; they are the same four properties read for
   two different sections, and the file a reader opens to check either is the file that states the section.
   A PERCENTAGE PADDING CRASHES, and that is CSS 2.1 §8.4's own undefined case rather than a lookup this
   component declines: §8.4 refers a padding percentage — "even for 'padding-top' and 'padding-bottom'" — to the
   containing block's WIDTH, which for a cell is the table box's content width, and then names this exact shape
   as the one it does not define, "if the containing block's width depends on this element, then the resulting
   layout is undefined in CSS 2.1". §17.5.2.2 Automatic table layout's MCW makes that dependence real for every
   cell.
   THE SEPARATED BORDER MODEL IS THE CALLER'S TO ESTABLISH, exactly as it is for the horizontal twin: under
   §17.6.2 a cell's used border is not its own computed `border-*-width`, so this sum double-counts the shared
   halves. `table_heights` refuses that model by name before any of this runs. */
CssPx table_cell_vertical_edges(lxb_dom_element_t *cell);

/* Releases what `table_heights` stored. A zero-row answer holds NULL and is accepted. */
void table_heights_release(TableUsedHeights *h);

#endif
