/* CSS 2.1 §17.5.2.2 Automatic table layout's COLUMN WIDTHS — the four steps under "Column widths are
 * determined as follows", whose closing sentence is the whole of what this component answers: "This gives a
 * maximum and minimum width for each column."
 *
 * WHY THAT SENTENCE IS THE BOUNDARY AND NOT AN ARBITRARY CUT. §17.5.2.2 is written as three separate pieces
 * with two different subjects. The four steps here take the GRID and the CELLS' CONTENT and produce a pair of
 * numbers per column; the paragraph after them derives CAPMIN from the CAPTIONS, which are not in the table
 * box at all (CSS 2.1 §17.4 Tables in the visual formatting model puts them in the wrapper beside it); and the
 * two rules after THAT — "Column and caption widths influence the final table width as follows" — take those
 * two results plus the table's own `width`, its containing block and §17.6's cell spacing, and produce ONE
 * number. Only the first piece is a function of the cells' content, and it is the only one that needs
 * core/layout/intrinsic_size.h at all. Splitting anywhere else would put a walk over every cell in the same
 * component as a two-line comparison.
 *
 * WHAT IS DELIBERATELY NOT HERE, EACH NAMED WITH ITS OWN SUBJECT.
 *   - §17.5.2.1 Fixed table layout is a DIFFERENT ALGORITHM over the same grid, not a mode of this one: its
 *     column widths come from the column boxes and the FIRST ROW's cells and it reads no other cell's content
 *     ("Cells in subsequent rows do not affect column widths"). Which of the two runs is `table-layout`'s
 *     answer, and that property is not in core/css/css_computed_value.c's modelled set — so a caller of this
 *     entry has decided, and this component does not ask.
 *   - CAPMIN and the final table width, for the reason above.
 *   - §17.5.3 Table height algorithms, which is what CONSUMES a cell's used width. This answers a pair of
 *     CONSTRAINTS per column and never a used width: a used column width is the final table width distributed
 *     back over these, which is the rule one piece further down §17.5.2.2 ("If the used width is greater than
 *     MIN, the extra width should be distributed over the columns").
 *
 * THE NUMBERS ARE BORDER-BOX WIDTHS OF THE CELLS, WHICH IS A CHOICE §17.5.2.2 LEAVES OPEN AND IS RECORDED
 * HERE RATHER THAN RE-DERIVED PER READER. The section never says which box a "column width" is measured in.
 * Two facts decide it together. §17.5.2.2's own final rules add "cell spacing or borders" to the columns
 * SEPARATELY — "the minimum width required by all the columns plus cell spacing or borders (MIN)" — and that
 * phrase is §17.6's two models' term (the separated model's `border-spacing` between cells, the collapsing
 * model's shared borders), not a cell's own padding and border, which sit INSIDE the cell box and would fit
 * nowhere if the column did not hold them. And css-sizing-3 §2.2 "Intrinsic Size Contributions" states the
 * general rule this is an instance of: the contributions "are based on the OUTER SIZE of the box; for this
 * purpose auto margins are treated as zero". A cell's outer size IS its border box, because CSS 2.1 §8.3
 * "Margin properties" does not apply the margins to an internal table box — core/css/css_property_applies.h
 * owns that line and is ASKED rather than restated. So a column width here holds each cell's own padding and
 * border, and whatever §17.6 adds between cells is added by the step that owns §17.6.
 *
 * THE PAIR IS COHERENT: `max` IS NEVER BELOW `min`, and that is a derivation rather than a spec sentence.
 * §17.5.2.2 lets a cell's declared `width` raise its MINIMUM without touching its maximum ("If the specified
 * 'width' (W) of the cell is greater than MCW, W is the minimum cell width"), and its step over spanning cells
 * widens the two sets of columns independently — so both can put a column's minimum above its maximum. A
 * maximum below a minimum is not a width the column can ever take, and the final rules use MAX as an upper
 * operand against the containing block, so the incoherence would travel into the table's own width. The
 * maximum is therefore floored at the minimum where they cross, at the two places they can.
 *
 * NOTHING IS STORED, for core/layout/used_value.h's reason: a layout is per-flow state, so a cached column
 * width is shared state solver/dom_cow.h's delta does not swap and a stale one is another flow's document.
 *
 * EVERY INPUT THIS COMPONENT REFUSES IS REFUSED WITH A `DCHECK`/`DFAIL`, WHICH MEANS THE DIFF THAT FIRST
 * CONSUMES THESE NUMBERS IN RELEASE INHERITS A PROMOTION AND NOT JUST A CALL. Nothing reads this entry yet, so
 * a refused input aborts in dev and cannot reach a release build at all; the moment a used width derived from
 * it is reported through CSSOM VIEW, a compiled-out refusal becomes a FABRICATED RECTANGLE in release —
 * a percentage padding counted as zero, a declared column width dropped — and CLAUDE.md's data-integrity case
 * is then what those sites are, exactly as core/layout/block_flow.c's table-wrapper `CHECK` already is. Make
 * that judgement in the diff that wires this up; it is that diff's line, not this one's. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_COLUMN_WIDTH_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_COLUMN_WIDTH_H

#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/table_grid.h"

/* ONE GRID COLUMN'S TWO CONSTRAINTS, in CSS pixels, as border-box widths of the cells that occupy it.
   They are answered together and never separately, for core/layout/intrinsic_size.h's reason one level up: the
   four steps derive both from the same walk with only the wrap opportunities differing, and the one relation
   between them (`min <= max`) is a statement about the PAIR that a caller holding one of them could not check.
   BOTH ARE `CssPx` AND NOT `double` because a cell's content width can be a function of the environment — a
   `font-size` in `rem` makes a text run's measure one, and CSS 2.1 §10.1's containing block puts the viewport
   under the final table width — so css_length.h's arithmetic carries the union of every operand's facts and a
   page that branches on a measured column reads the domain rather than one arm of it. */
typedef struct {
    CssPx min;   /* §17.5.2.2's "minimum column width" */
    CssPx max;   /* §17.5.2.2's "maximum column width" */
} TableColumnWidth;

/* §17.5.2.2's FOUR STEPS over `grid`, which must be `table`'s own (core/layout/table_grid.h). Answers
   `grid->ncols` and stores a newly allocated array of that many at `*out`, WHICH THE CALLER OWNS AND FREES; a
   count of zero stores NULL.
   ZERO COLUMNS IS A REAL TABLE AND NOT A FAILURE — `<table></table>`, or one whose only children are a caption
   and a row group with no cells — and the sum of no column widths is a number, which is what the final rules
   then compare against the table's own `width` and its containing block.
   EVERY INPUT THIS COMPONENT CANNOT READ CRASHES BY NAME rather than being left out of a maximum, because a
   term omitted from a MINIMUM makes the column narrower than the content it must hold, and that number then
   travels into a used width and out through CSSOM VIEW as a rectangle no reader can distinguish from a
   measured one. */
size_t table_column_widths(lxb_dom_element_t *table, const TableGrid *grid, TableColumnWidth **out);

/* ONE CELL'S HORIZONTAL PADDING AND BORDER, in CSS pixels — the difference between the content box
   core/layout/intrinsic_size.h answers in and the BORDER box every column width in §17.5.2 is measured in (the
   choice the header above records). It is EXPORTED because CSS 2.1 §17.5.2 Table width algorithms: the
   'table-layout' property has TWO algorithms and both need it: §17.5.2.2 Automatic table layout's step 1
   converts a cell's intrinsic sizes, and §17.5.2.1 Fixed table layout's step 2 converts the DECLARED `width` of
   "a cell in the first row" into the same box. Two spellings of one four-term sum are two places for the terms
   to come to disagree, and the disagreement would be invisible: both answers are real widths of real boxes.
   IT IS NOT core/layout/used_value.h's SURROUND and must not become a call to it — see table_column_width.c for
   the cycle that makes them two different questions over the same four properties.
   THE SEPARATED BORDER MODEL IS THE CALLER'S TO ESTABLISH. Under CSS 2.1 §17.6.2 The collapsing border model a
   cell's used border is not its own computed `border-*-width`, so this sum double-counts the shared halves;
   §17.5.2's entry refuses that model by name before any of this runs, and `table_column_widths` asserts it. */
CssPx table_cell_border_edges(lxb_dom_element_t *cell);

/* CSS 2.1 §17.5 Visual layout of table contents' RULES 3 AND 4 ARE NOT HERE — they are
   core/layout/table_column_box.h, which answers WHICH column and column-group box occupies each grid column
   and reads no property off either. What stood here was a narrower question over the same walk (the FIRST such
   box whose computed `width` is other than `auto`) and it went out with the crash it existed to raise: it could
   answer only whether SOME column declared a width, which is enough to refuse a document and not enough to lay
   one out, and CSS 2.1 §17.6.2.1 Border conflict resolution needs the same walk to name a box PER EDGE. */

#endif
