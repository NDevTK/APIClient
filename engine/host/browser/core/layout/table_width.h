/* CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property — THE USED WIDTH OF A TABLE BOX AND OF
 * EVERY COLUMN IN IT, which is the number §17.4 Tables in the visual formatting model sends the table wrapper
 * box to ("The width of the table wrapper box is the border-edge width of the table box inside it, as described
 * by section 17.5.2") and the number §17.5.3 Table height algorithms measures a cell's content height over.
 *
 * WHY IT IS A COMPONENT SEPARATE FROM core/layout/table_column_width.h. That one answers §17.5.2.2 Automatic
 * table layout's four steps and stops at the section's own sentence, "This gives a maximum and minimum width
 * for each column" — a pair of CONSTRAINTS per column, derived from the cells' content, with no table in it.
 * This one is the rest of §17.5.2 and has a different subject at every step: it chooses between the section's
 * TWO ALGORITHMS on `table-layout`, it derives CAPMIN from the CAPTION boxes (which §17.4 puts in the WRAPPER,
 * beside the table box rather than inside it), it adds §17.6.1 The separated borders model's cell spacing, it
 * compares against the table's own `width` and its containing block, and it distributes the answer back over
 * the columns. Only the first piece is a walk over every cell; folding the two together would put that walk in
 * the same function as a two-line comparison, and would give the fixed algorithm — which reads no cell outside
 * the first row — a dependency on a measurement it must not make.
 *
 * THE TWO ALGORITHMS ARE A DISPATCH AND NOT A MODE, and §17.5.2 makes the difference between them normative in
 * exactly one direction: "user agents may use any algorithm they wish to do so and are free to prefer rendering
 * speed over precision, except when the fixed layout algorithm is selected". So a table whose `table-layout` is
 * `fixed` MUST get §17.5.2.1 Fixed table layout, while §17.5.2.2 is offered as one automatic algorithm among
 * others — its own words are "The remainder of this section is non-normative" and "they can use any other
 * algorithm even if it results in different behavior". EVERY PLACE THAT LICENCE IS TAKEN IS RECORDED AT THE
 * SITE rather than crashed on: a crash where the standard offers a choice refuses a document both readings are
 * conforming for, which is a different failure from a crash where the answer would be WRONG.
 *
 * `table-layout: fixed` WITH `width: auto` TAKES THE AUTOMATIC ALGORITHM, and that is §17.5.2.1's own sentence
 * rather than a fallback this component invented: "A value of 'auto' (for both 'display: table' and 'display:
 * inline-table') means use the automatic table layout algorithm." The section then offers an alternative in the
 * same paragraph — "a UA may (but does not have to) use the algorithm of 10.3.3 to compute a width and apply
 * fixed table layout even if the specified width is 'auto'" — and this component DECLINES it, which the
 * parenthesis explicitly permits. The consequence is worth knowing rather than hidden: a `table-layout: fixed`
 * table with no declared width is laid out from ALL of its cells' content, not from its first row's.
 *
 * WHAT THE NUMBERS ARE MEASURED IN, because §17.5.2 never says and two readers must not answer differently.
 * A COLUMN width here is a BORDER-BOX width of the cells that occupy it — core/layout/table_column_width.h
 * records that choice and its reasons, and the distribution below only ever adds to those numbers, so nothing
 * changes box on the way through. THE TABLE's width is §17.6.1's, in that section's own sentence: "The width of
 * the table is the distance from the left inner padding edge to the right inner padding edge (including the
 * border spacing but excluding padding and border)" — so it is the table box's CONTENT width and it CONTAINS
 * the cell spacing, which is why the spacing is a term of the sum below rather than something a consumer adds.
 *
 * AND THE DECLARED `width` OF AN HTML `<table>` IS A BORDER-BOX NUMBER, WHICH IS §17.6.1's OTHER SENTENCE AND
 * NOT A `box-sizing` THIS ENGINE INVENTS: "However, in HTML and XHTML1, the width of the <table> element is the
 * distance from the left border edge to the right border edge." That rule is about the ELEMENT and not about
 * its `display`, so it covers `inline-table` on a `<table>` and does not cover `display: table` on a `<div>`.
 * It is applied HERE, as a rule of §17.5.2's own, rather than as a UA `box-sizing: border-box` declaration:
 * §17.6.1's note says that spelling is CSS3's ("In CSS3 this peculiar requirement will be defined in terms of
 * UA style sheet rules and the 'box-sizing' property"), and adding the declaration would change what
 * `getComputedStyle(table).boxSizing` reports, which is an observable this section says nothing about.
 *
 * §17.6.2 The collapsing border model IS LAID OUT AND NOT REFUSED, AND IT COSTS THIS COMPONENT TWO TERMS.
 * Under §17.6.2 a cell's used border is not its own computed `border-*-width` — that section centres each
 * border on the grid line between two cells and its own row-width equation charges the two OUTERMOST at half —
 * and `border-spacing` has no meaning there at all. Both changes are made where they belong rather than as a
 * mode of this algorithm: core/layout/table_border_collapse.h owns §17.6.2.1 Border conflict resolution and
 * what a cell is charged by it, core/layout/table_column_width.h's `table_cell_border_edges` takes that arm so
 * the four steps below are stated over the same border box in either model, and the two terms THIS component
 * owns are the SPACING (zero under §17.6.2) and the TABLE BOX'S OWN EDGES (no padding, and §17.6.2's own two
 * half-borders instead of the computed `border-*-width`). Every other line of §17.5.2 is model-independent,
 * which is why there is no second algorithm here and no `if` inside one.
 *
 * NOTHING IS STORED BETWEEN CALLS, for core/layout/block_flow.h's reason: a layout is per-flow state, so a
 * cached table width is shared state solver/dom_cow.h's delta does not swap and a stale one is another flow's
 * document. The array this entry allocates is the CALLER's for the duration of one question. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_WIDTH_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TABLE_WIDTH_H

#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/table_grid.h"

/* §17.5.2's ANSWER, which is ONE fact in three shapes and never three facts: `content` is the sum of `columns`
   and `spacing` taken between and around them, and the entry below asserts that at its boundary. They are
   returned together because a consumer holding one of them cannot derive the others — a caller with the
   columns alone does not know the spacing term, and a caller with the table width alone cannot place a single
   cell.
   `spacing` IS A FIELD RATHER THAN A CONSUMER'S SECOND READ OF `border-spacing`, and the reason is the one
   case where the arithmetic cannot recover it: a table with NO columns takes no spacing at all, so `content`
   less the columns is zero there whatever the property says, and a consumer inverting the sum would answer a
   spacing of zero for a table that declares one. A second read of the property would also be a second place
   for §17.6.1's `Computed value: two absolute lengths` to be resolved, free to disagree with the one the
   columns were laid out under. */
typedef struct {
    /* The USED width of each grid column, in §17.5's own logical column order (core/layout/table_grid.h), as a
       BORDER-BOX width of the cells that occupy it. `ncols` entries, or NULL when `ncols` is zero. */
    CssPx  *columns;
    size_t  ncols;
    /* §17.6.1's "width of the table": the table BOX's used content width, INCLUDING the border spacing and
       excluding its own padding and border. This is CSS 2.1's `width` for the table box, so a consumer wanting
       §17.4's wrapper width adds the table box's own horizontal SURROUND to it — which is what
       core/layout/used_value.h's border edge already does for every other box.
       WHAT THAT SURROUND IS DEPENDS ON THE BORDER MODEL, AND A CONSUMER READING THE PROPERTIES DIRECTLY IS
       WRONG UNDER §17.6.2 The collapsing border model — which is why the conversion goes through
       core/layout/used_value.h and never through `css_computed_length` at a caller. A collapsed table box has
       NO padding at all ("Also, in this model, a table does not have padding (but does have margins)") and its
       border widths are not its computed `border-*-width` but §17.6.2's own halves ("The left border width of
       the table is half of the first cell's collapsed left border"), which
       core/layout/table_border_collapse.h's `table_collapsed_table_edges` answers whole.
       THIS WAS A NAMED RESIDUAL AND IT IS BUILT; the reading is kept rather than deleted so that nobody
       re-derives the four-property version. core/layout/used_value.c's surround now asks §17.2 The CSS table
       model which box it is being asked about and takes §17.6.2's halves for a table box in that model, so
       every border-edge conversion in this tree — the one `used_value_border_edge_px` performs over this
       field, and the leading pair core/layout/flow_position.c measures a row's origin from — reads the SAME
       number `tw_table_edges` subtracts a declared border-box width by, one file over. WHAT IT LOOKED LIKE
       WHILE IT WAS OPEN, since that is the shape a regression would take again: for
       `<table style="border-collapse:collapse;border:10px solid">` whose cells declare no border of their own,
       the resolved border at each outermost grid line is 10 and §17.6.2 gives the table box a border width of
       5 on each side, so its border box is this content width plus 10 — and a consumer reading
       `border-left-width` and `border-right-width` reported it plus 20. */
    CssPx   content;
    /* §17.6.1 The separated borders model's HORIZONTAL `border-spacing`, which that section defines as "The
       'border-spacing' property specifies the distance between the borders of adjoining cells" and which
       `content` holds `ncols + 1` of — one between each adjoining pair and one at each end, since §17.6.1
       also says "The distance between the table border and the borders of the cells on the edge of the table
       is the table's padding for that side plus the relevant border-spacing distance". The VERTICAL half is
       §17.5.3 Table height algorithms' and is not here.
       IT IS THIS ALGORITHM'S TERM AND NOT THE PROPERTY, WHICH IS A DIFFERENCE ONLY §17.6.2 The collapsing
       border model MAKES: there it is ZERO whatever `border-spacing` computed to, because that model has no
       distance between adjoining cell borders at all ("Borders are centered on the grid lines between the
       cells"). A consumer adds this field and never re-reads the property, so a collapsed table adds nothing
       — and the property still has a computed value, since it is `Inherited: yes` and a descendant table may
       be in the other model. */
    CssPx   spacing;
} TableUsedWidths;

/* §17.5.2 over `table` and `grid`, which must be that table's own (core/layout/table_grid.h). `table` must
   generate a TABLE box — `table` or `inline-table` (core/layout/table_box.h's `table_box_kind`) — and a
   table-internal box crashes rather than being answered, because a CELL's used width is a column's and a ROW's
   is the table's, which are different questions with different answers.
   THE RESULT IS THE CALLER'S and is released with `table_widths_release`.
   A TABLE WITH NO COLUMNS IS A REAL TABLE AND NOT A FAILURE — `<table></table>`, or one whose only child is a
   caption — and its width is then the greater of its own declared `width`, CAPMIN and zero, which is a number.
   EVERY INPUT §17.5.2 CANNOT RESOLVE CRASHES BY NAME rather than being dropped from a maximum: every term here
   is a FLOOR under the table's width, so a term left out reports a table narrower than the document asks for,
   and that number travels into §17.4's wrapper, into core/layout/flow_position.h's coordinates and out through
   CSSOM VIEW as a rectangle no reader can distinguish from a measured one. */
void table_widths(lxb_dom_element_t *table, const TableGrid *grid, TableUsedWidths *out);

/* ONE CELL'S USED BORDER-BOX WIDTH, taken out of the answer above — the number CSS 2.1 §17.5 Visual layout of
   table contents leaves implicit and every consumer of a cell's geometry needs: "Each cell is thus a
   rectangular box, one or more grid cells wide and high", so a cell's width is the used width of the columns
   its rectangle covers. `cell` must be a cell of the grid `widths` was computed over
   (core/layout/table_grid.h's `table_grid_cell_of`), and the answer is in the same BORDER box the columns are.
   THE SPANNING CASE IS A CHOICE CSS 2.1 DECLINES TO MAKE AND IT IS RECORDED HERE RATHER THAN CRASHED ON. A
   cell covering N columns gets those N used widths PLUS N-1 of `spacing`, because §17.6.1 The separated
   borders model defines `border-spacing` as "the distance between the borders of adjoining cells" and the
   columns inside one cell's rectangle have no adjoining cell borders between them — the spacing there is
   inside the cell, not between two of them. §17.5.2.2 Automatic table layout's step 3 states the FLOOR without
   that term ("For each cell that spans more than one column, increase the minimum widths of the columns it
   spans so that together they are at least as wide as the cell"), which is what makes this a reading rather
   than a derivation: under it a spanning cell ends at least as wide as its own minimum and up to N-1 spacings
   WIDER, never narrower than its content. The other reading — charging the cell only its columns — makes a
   spanning cell narrower than the row it sits in by exactly those N-1 spacings, so the two disagree about
   whether the table's own width covers its cells, and only this one keeps §17.6.1's sum whole.
   UNDER §17.6.2 The collapsing border model THAT TERM IS ZERO AND THE ANSWER IS STILL EXACT, which is the
   reading rather than a case this falls through. The N-1 grid lines running under a spanning cell are each
   charged HALF to the column on either side of them (core/layout/table_border_collapse.h), so the N used
   widths this loop sums already contain every one of those borders whole — which is precisely the length the
   `border-spacing` term supplies in the other model, arriving through the columns instead of beside them.
   IT IS NOT A USED `width` IN CSS 2.1's SENSE and a caller must not report it as one: css-sizing-3 §3.3 "Box
   Edges for Sizing: the box-sizing property" decides which box a used value is exposed in, and under the
   initial `content-box` that is the content box — this number less the cell's own padding and border, which
   core/layout/table_column_width.h's `table_cell_border_edges` is the ONE spelling of. */
CssPx table_cell_used_border_box(const TableUsedWidths *widths, const TableGridCell *cell);

/* Releases what `table_widths` stored. A zero-column answer holds NULL and is accepted. */
void table_widths_release(TableUsedWidths *w);

#endif
