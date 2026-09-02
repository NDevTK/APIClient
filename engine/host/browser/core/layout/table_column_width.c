/* CSS 2.1 §17.5.2.2 Automatic table layout's four column-width steps, over core/layout/table_grid.h's grid.
   See table_column_width.h for the contract, for why the boundary is §17.5.2.2's own "This gives a maximum and
   minimum width for each column", and for why the numbers are BORDER-BOX widths of the cells. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/css/css_property_applies.h"
#include "core/layout/box_subject.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/table_box.h"
#include "core/layout/table_column_width.h"
#include "core/layout/table_grid.h"

/* THE COHERENCE FLOOR, stated once because it is applied at the two places the pair can cross and both are
   the SAME fact: a maximum below a minimum is not a width the column can ever take. See the header for the two
   §17.5.2.2 steps that produce the crossing — a declared cell `width` raising a minimum past a max-content,
   and the two independent widenings over a spanning cell. It is arithmetic and asserts nothing, so it names no
   site; the invariant it establishes is asserted once, at the boundary where the pair leaves this component. */
static CssPx tcw_coherent(CssPx min, CssPx max)
{
    return css_px_max(max, min);
}

/* ONE CELL'S HORIZONTAL PADDING AND BORDER, in CSS pixels — the difference between the content box
   core/layout/intrinsic_size.h answers in and the border box a column width is measured in.
 *
 * IT IS NOT core/layout/used_value.c's `uv_surround` AND MUST NOT BECOME A CALL TO IT, which is a division of
 * labour rather than a second copy: that one resolves a PERCENTAGE padding through `used_value_px`, against
 * CSS 2.1 §10.1 Definition of "containing block"'s rectangle — whose width for a cell is the table's content
 * width, which is what §17.5.2.2 is being run to produce. Asking it here would be this algorithm asking for its
 * own output. So the percentage is a CYCLE and crashes, and the two functions answer two different questions
 * over the same four properties.
 *
 * §10.4 Minimum and maximum widths: 'min-width' and 'max-width' IS NOT READ AND THAT IS THE SECTION'S OWN
 * SENTENCE: "In CSS 2.1, the effect of 'min-width' and 'max-width' on tables, inline tables, table cells,
 * table columns, and column groups is undefined." A clamp applied here would be this engine deciding a case
 * the standard declines to.
 *
 * NAMED RESIDUAL — §17.6.1 The separated borders model ONLY. WHAT IS NOT COVERED: CSS 2.1 §17.6.2 The
 * collapsing border model, in which a cell's used border is not its own computed `border-*-width` — the
 * section resolves the borders of adjacent cells into one and divides it between them, so the sum taken here
 * double-counts the shared halves. The border read below is right under `border-collapse: separate`, which is
 * that property's initial value — so it is what an undeclared document is in, and this component does not yet
 * ASK which model is in force. WHAT THE NEXT DIFF BUILDS: the ask itself — read `border-collapse` here and
 * take §17.6.2's arm when it answers `collapse`. Both properties are now modelled, so nothing is missing
 * underneath: they are in core/css/css_computed_value.c's `css_computed_models` set, and they landed in
 * DIFFERENT arms rather than the one arm an earlier draft of this residual named. `border-collapse` is
 * as-specified; `border-spacing` is NOT, and could not have been — CSS 2.1 §17.6.1 The separated borders
 * model states its `Computed value: two absolute lengths`, a pair, which no as-specified row can hold. That
 * clause was spec-wrong when it was written and a reader who acted on it would have built the wrong shape,
 * which is why it is corrected here rather than deleted. HOW ITS ABSENCE WOULD SHOW: a table
 * declaring `border-collapse: collapse` with a non-zero cell border reports every column WIDER than a browser
 * does, by one border width per shared edge, and the table that grows out of those columns is wider than the
 * one Chrome lays out for the same document. */
static CssPx tcw_cell_edges(lxb_dom_element_t *cell)
{
    static const char *const PADDINGS[2] = { "padding-left", "padding-right" };
    static const char *const BORDERS[2] = { "border-left-width", "border-right-width" };
    CssPx edges = css_px(0.0);
    char nbuf[160];
    int i;

    /* css-sizing-3 §2.2 "Intrinsic Size Contributions"' outer size is the box's margins as well, and the
       header's claim that a cell's outer size IS its border box rests entirely on the margins not applying —
       so it is ASKED of the component that owns CSS 2.1 §8.3 "Margin properties"' Applies-to line rather than
       restated here, and a day the two disagree is a day this walk stops rather than under-reporting a
       column by two margins. */
    DCHECK(!css_property_applies(cell, "margin-left") && !css_property_applies(cell, "margin-right"),
           "CSS 2.1 §17.5.2.2 Automatic table layout took a cell's OUTER SIZE to be its border box, and CSS 2.1 "
           "§8.3 \"Margin properties\"' Applies-to line now says the horizontal margins DO apply to this box — "
           "so css-sizing-3 §2.2 \"Intrinsic Size Contributions\"' outer size has a term this walk is not "
           "adding and every column the box occupies is reported narrower than its content");
    for (i = 0; i < 2; i++) {
        CssLength pad = css_computed_length(cell, PADDINGS[i]);
        CssLength bor = css_computed_length(cell, BORDERS[i]);

        if (pad.kind != CSS_LENGTH_ABSOLUTE)
            DFAILF("%s: a table cell's `%s` is not an absolute length, and CSS 2.1 §17.5.2.2 Automatic table "
                   "layout cannot resolve it. CSS 2.1 §8.4 refers a padding percentage to the containing "
                   "block's width and then names THIS case as the one it does not define: \"If the containing "
                   "block's width depends on this element, then the resulting layout is undefined in CSS "
                   "2.1.\" For a cell that rectangle is the TABLE BOX's content width, which is what this "
                   "algorithm is being run to produce — so the resolution is a CYCLE and not a missing lookup. "
                   "§17.5.2.2 states its own inputs and does "
                   "not name one: \"Input to the automatic table layout must only include the width of the "
                   "containing block and the content of, and any CSS properties set on, the table and any of "
                   "its descendants.\" BUILD the fixed point the way CSS 2.1 leaves open and this engine must "
                   "not guess at: resolve the percentage against the width the algorithm settles on, which "
                   "means running the four steps with the percentage terms at zero, deriving the table width, "
                   "and re-running them — and RECORD that as the reading taken, because §17.5.2.2 is "
                   "non-normative from \"The remainder of this section is non-normative\" onward and offers no "
                   "other. A `calc()` or a keyword here is a different defect: `padding-*`'s `Computed value:` "
                   "line is \"the percentage as specified or the absolute length\", so neither is a value the "
                   "cascade can hand back",
                   box_subject(cell, nbuf, sizeof nbuf), PADDINGS[i]);
        DCHECK(bor.kind == CSS_LENGTH_ABSOLUTE,
               "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 "
               "§3.3 \"Line Thickness: the border-width properties\"' `Computed value:` line is `absolute "
               "length, "
               "snapped as a border width` and every arm of that derivation produces one, so a percentage or a "
               "keyword here is a rule that did not run");
        edges = css_px_add(edges, css_px_add(pad.px, bor.px));
    }
    DCHECK(edges.px >= 0.0,
           "CSS 2.1 §17.5.2.2's cell edges came out NEGATIVE. CSS 2.1 §8.4 states outright that \"unlike "
           "margin properties, values for padding values cannot be negative\" — the spec's own wording, "
           "\"padding values\" included — and css-backgrounds-3 §3.3's <line-width> is a non-negative "
           "<length>, so lexbor drops either declaration; a negative here is arithmetic that lost a sign "
           "rather than a document");
    return edges;
}

/* §17.5.2.2's STEP 1, for one cell, in the border box the header records:
     "Calculate the minimum content width (MCW) of each cell: the formatted content may span any number of
      lines but may not overflow the cell box. If the specified 'width' (W) of the cell is greater than MCW, W
      is the minimum cell width. A value of 'auto' means that MCW is the minimum cell width."
     "Also, calculate the \"maximum\" cell width of each cell: formatting the content without breaking lines
      other than where explicit line breaks occur."
   MCW AND THE MAXIMUM ARE css-sizing-3 §5.1 "Intrinsic Sizes"' PAIR and are not a second measurement: §5.1's
   own §2.1 "Auto Box Sizes" defines the min-content size as the one taken "if ALL soft wrap opportunities
   within the box were taken" and the max-content size as the one "if NONE of the soft wrap opportunities
   within the box were taken", which is the same two sentences CSS 2.1 writes here in its own vocabulary.
   core/layout/intrinsic_size.h answers both in the CONTENT box, and the caller converts once. */
static TableColumnWidth tcw_cell_widths(lxb_dom_element_t *cell)
{
    IntrinsicInlineSizes sizes;
    TableColumnWidth out;
    CssPx edges, min_content;
    CssLength w;
    char nbuf[160];

    DCHECK(cell != NULL, "CSS 2.1 §17.5.2.2's step 1 was asked for of no cell element");
    edges = tcw_cell_edges(cell);
    sizes = intrinsic_inline_sizes(cell);
    min_content = sizes.min_content;
    /* §17.5.2.2 says the SPECIFIED `width`, and css-sizing-3 §3.1.1 "Preferred Size Properties: the width and
       height properties" makes the computed value "as specified, with <length-percentage> values computed" —
       so the computed value IS the specified one with its lengths absolutized, which is the operand this step
       wants and the only one the cascade can hand over. */
    DCHECK(css_property_applies(cell, "width"),
           "CSS 2.1 §17.5.2.2's step 1 read a `width` on a box that property does not apply to. CSS 2.1 §10.2 "
           "\"Content width: the width property\"' Applies-to line excludes table rows and row groups and no "
           "other table box, so a cell reaching this is a `display` the grid placed as a cell and the cascade "
           "classifies otherwise");
    w = css_computed_length(cell, "width");
    /* A `calc()` takes this arm and not the one below it: css-values-4 §10.1 "Basic Arithmetic: calc()" lets
       one mix a length with a percentage, so the value is a percentage as far as WHAT IT RESOLVES AGAINST is
       concerned, and that basis is this algorithm's own output either way. */
    if (w.kind == CSS_LENGTH_PERCENTAGE || w.kind == CSS_LENGTH_CALCULATED)
        DFAILF("%s: a table cell declares a PERCENTAGE `width`, and CSS 2.1 §17.5.2.2 Automatic table layout "
               "resolves it against a width it has not produced yet. The section states the basis in its own "
               "words — \"A percentage value for a column width is relative to the table width\" — and then "
               "says what it is under `width: auto`: \"If the table has 'width: auto', a percentage represents "
               "a CONSTRAINT on the column's width, which a UA should try to satisfy. (Obviously, this is not "
               "always possible: if the column's width is '110%%', the constraint cannot be satisfied.)\" That "
               "is a constraint with no algorithm attached, so treating the percentage as absent would silently "
               "drop an author's declaration and treating it as zero would report a narrower minimum than the "
               "content needs. BUILD it as the second pass §17.5.2.2's opening sentence already provides for "
               "(\"which generally requires no more than two passes\"): run these four steps with the "
               "percentage terms absent, let the final rules settle the table width, resolve every percentage "
               "against it and re-run — and record that as the reading, since the section is non-normative "
               "here and names no other",
               box_subject(cell, nbuf, sizeof nbuf));
    /* EVERY OTHER SHAPE `width` CAN COMPUTE TO CRASHES RATHER THAN BEING TREATED AS `auto`, and the difference
       is the direction of the error: `auto` means the step's floor is absent, so silently taking one of these
       for it drops a declaration that RAISES a minimum and reports the column narrower than its content. */
    if (w.kind != CSS_LENGTH_ABSOLUTE &&
        !(w.kind == CSS_LENGTH_KEYWORD && strcmp(w.keyword, "auto") == 0))
        DFAILF("%s: CSS 2.1 §17.5.2.2 Automatic table layout's step 1 compares a cell's minimum content width "
               "against \"the specified 'width' (W) of the cell\", and this cell's computed `width` is neither "
               "an absolute length nor `auto`. css-sizing-3 §3.1.1 \"Preferred Size Properties: the width and "
               "height properties\" is what widened that property past CSS 2.1's two arms — its value grammar "
               "carries the intrinsic sizing keywords beside the length, and each names a DIFFERENT rule from "
               "the comparison this step states, which is why none of them may be quietly read as `auto`. "
               "BUILD each keyword's own arm beside the length one. A `calc()` does NOT "
               "reach here — it takes the percentage crash above, because what it resolves against is this "
               "algorithm's own output whether or not a percentage survived the function",
               box_subject(cell, nbuf, sizeof nbuf));
    if (w.kind == CSS_LENGTH_ABSOLUTE) {
        CssPx declared = w.px;
        char *box_sizing;
        bool border_box;

        /* css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property" — under `border-box` the declared
           length is the BORDER box's, so the content-box operand §17.5.2.2's comparison is stated in is that
           length less this box's padding and border, floored at zero because an inner size cannot be negative.
           IT IS PARAPHRASED AND NOT QUOTED DELIBERATELY: the two Editor's Draft editions of §3.3 word that
           flooring differently — one as a clause of the subtraction sentence, one as a sentence of its own —
           so any one-line quotation is a quotation of ONE edition, and engine/citegen.mjs checks quotations
           against the committed corpus of whichever edition engine/specindex holds. The citation is what is
           stable across both; do not "restore" a quotation here. */
        box_sizing = css_computed_value(cell, "box-sizing");
        DCHECK(box_sizing != NULL,
               "the cascade produced no computed `box-sizing` — css-sizing-3 §3.3 \"Box Edges for Sizing: the "
               "box-sizing property\" gives it an initial value of `content-box`, so the cascade's last layer "
               "always answers");
        border_box = strcmp(box_sizing, "border-box") == 0;
        free(box_sizing);
        if (border_box) declared = css_px_max(css_px_sub(declared, edges), css_px(0.0));
        /* "If the specified 'width' (W) of the cell is greater than MCW, W is the minimum cell width." */
        min_content = css_px_max(min_content, declared);
    }
    out.min = css_px_add(min_content, edges);
    out.max = css_px_add(sizes.max_content, edges);
    /* The first of the two crossings the header records: W raised the minimum and left the maximum where the
       content put it. */
    out.max = tcw_coherent(out.min, out.max);
    return out;
}

/* §17.5.2.2's STEPS 2 AND 4 EACH READ A `width` OFF A BOX THIS ENGINE DOES NOT PLACE, and this is the probe
   that stops the walk exactly where its answer would be WRONG rather than merely narrow.
   Step 2 takes a column's minimum as "that required by the cell with the largest minimum cell width (or the
   column 'width', whichever is larger)" and its maximum the same way; step 4 is "For each column group element
   with a 'width' other than 'auto', increase the minimum widths of the columns it spans, so that together they
   are at least as wide as the column group's 'width'." Both are FLOORS, so omitting one reports a column
   NARROWER than the document asks for — a number that then travels into a used width and out through CSSOM
   VIEW as a rectangle indistinguishable from a measured one.
   A COLUMN BOX WITH `width: auto` CONTRIBUTES NOTHING TO EITHER FLOOR, so its presence is not a defect and
   this probe does not stop for one: §17.5.2.2's step 4 excludes it in its own antecedent, "For each column
   group element with a 'width' other than 'auto'", and step 2's parenthesis then compares a cell against
   nothing. That is why the crash is conditioned on the VALUE and not on the box — a `<colgroup>` wrapping the
   columns of an ordinary table is the common shape and its answer here is complete. */
static TableBoxKind tcw_kind_of(lxb_dom_element_t *el)
{
    char *display = css_computed_value(el, "display");
    TableBoxKind kind;

    DCHECK(display != NULL, "the cascade produced no computed `display` — the UA layer answers `inline` for "
                            "every element it does not name, so this cannot be unset");
    kind = table_box_kind(display);
    free(display);
    return kind;
}

/* ONE column or column-group box, checked. Split out so the two nesting levels below are one call each rather
   than one loop with a level flag in it — the flag was the only thing making the walk hard to read, and
   §17.2's box types nest no deeper than a column inside a column group. */
static void tcw_check_column_box(lxb_dom_element_t *box)
{
    CssLength cw = css_computed_length(box, "width");
    char nbuf[160];

    if (cw.kind == CSS_LENGTH_KEYWORD && strcmp(cw.keyword, "auto") == 0) return;
    DFAILF("%s: CSS 2.1 §17.5.2.2 Automatic table layout reads a DECLARED `width` off this box and "
                       "core/layout/table_grid.h does not place it. Two of the four steps need it and both are "
                       "FLOORS, so leaving it out reports every column it covers narrower than the document "
                       "asks for: step 2 takes a column's minimum as \"that required by the cell with the "
                       "largest minimum cell width (or the column 'width', whichever is larger)\" and its "
                       "maximum by the same sentence, and step 4 is \"For each column group element with a "
                       "'width' other than 'auto', increase the minimum widths of the columns it spans, so "
                       "that together they are at least as wide as the column group's 'width'.\" BUILD CSS 2.1 "
                       "§17.5 Visual layout of table contents' rules 3 and 4 — \"A column box occupies one or "
                       "more columns of grid cells\" and \"A column group box occupies the same grid cells as "
                       "the columns it contains\" — as a COLUMN-INDEX assignment beside core/layout/"
                       "table_grid.h's cell placement, which is where that header already says they belong "
                       "and which numbers the same columns this component indexes; the boxes themselves are "
                       "not rendered (§17.2 The CSS table model: \"are not rendered (exactly as if they had "
                       "'display: none')\"), so nothing else is owed. A `width: auto` on such a box is NOT "
           "this crash and never was — step 4's own antecedent excludes it and step 2's "
           "parenthesis then compares against nothing",
           box_subject(box, nbuf, sizeof nbuf));
}

static void tcw_require_no_declared_column_width(lxb_dom_element_t *table)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(table), *c;

    for (c = n->first_child; c != NULL; c = c->next) {
        lxb_dom_element_t *el;
        TableBoxKind kind;

        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        el = lxb_dom_interface_element(c);
        kind = tcw_kind_of(el);
        if (kind == TABLE_BOX_COLUMN) {
            tcw_check_column_box(el);
        } else if (kind == TABLE_BOX_COLUMN_GROUP) {
            lxb_dom_node_t *m;

            /* CSS 2.1 §17.5 Visual layout of table contents' rule 4: "A column group box occupies the same
               grid cells as the columns it contains", so the group's own `width` and each column's are two
               separate floors and both are checked. */
            tcw_check_column_box(el);
            for (m = c->first_child; m != NULL; m = m->next) {
                lxb_dom_element_t *col;

                if (m->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
                col = lxb_dom_interface_element(m);
                if (tcw_kind_of(col) == TABLE_BOX_COLUMN) tcw_check_column_box(col);
            }
        }
    }
}

size_t table_column_widths(lxb_dom_element_t *table, const TableGrid *grid, TableColumnWidth **out)
{
    TableColumnWidth *cols;
    size_t i, k;

    DCHECK(table != NULL, "CSS 2.1 §17.5.2.2's column widths were asked for of no element");
    DCHECK(grid != NULL && out != NULL,
           "CSS 2.1 §17.5.2.2's column widths were asked for with no grid or nowhere to put them. The grid is "
           "the OPERAND the section is stated over — a column's width is taken across the cells that occupy "
           "it, and nothing before core/layout/table_grid.h says which those are");
    DCHECK(grid->cells != NULL || grid->ncells == 0,
           "CSS 2.1 §17.5.2.2 was handed a grid with no cell array and a non-zero cell count — "
           "`table_grid_build` stores NULL only for a table whose rows generate no cell, so the two have been "
           "carried apart since it answered");
    *out = NULL;
    if (grid->ncols == 0) return 0;
    tcw_require_no_declared_column_width(table);
    cols = (TableColumnWidth *) calloc(grid->ncols, sizeof *cols);
    CHECK(cols != NULL,
          "CSS 2.1 §17.5.2.2's column widths could not allocate one pair per grid column of the table being "
          "laid out");
    for (i = 0; i < grid->ncols; i++) {
        cols[i].min = css_px(0.0);
        cols[i].max = css_px(0.0);
    }
    /* ---- §17.5.2.2's STEP 2 ---------------------------------------------------------------------------
       "For each column, determine a maximum and minimum column width from the cells that span only that
       column." It is written per COLUMN and run here per CELL over the same set, which is the same reduction
       with the loops exchanged: every cell is examined once and lands in exactly the column it spans, where a
       per-column form would re-scan every cell for each column. The column `width` half of the sentence is
       the probe above's subject and is settled before this loop runs.
       §17.5's RULE 5 OVERLAP CASE NEEDS NO ARM HERE. core/layout/table_grid.h records `overlaps` for a
       column-spanning cell that reaches into a row-spanning one and states which of §17.5's two conforming
       readings it took; under that reading the cell OCCUPIES the columns it covers, so its width is an input
       to them exactly as any other cell's is, and a second reading would change which columns it covers rather
       than what it contributes to them. */
    for (k = 0; k < grid->ncells; k++) {
        const TableGridCell *cell = &grid->cells[k];
        TableColumnWidth cw;

        if (cell->colspan != 1) continue;
        if (cell->element == NULL)
            DFAIL("CSS 2.1 §17.5.2.2 Automatic table layout's step 1 reached §17.2.1 Anonymous table objects' "
                  "ANONYMOUS 'table-cell' box, whose content is a RANGE OF DOM SIBLINGS and not an element — "
                  "and css-sizing-3 §5.1 \"Intrinsic Sizes\" is answered of an element "
                  "(core/layout/intrinsic_size.h). Skipping it is not available: the cell holds real content, "
                  "so its minimum is a FLOOR under the column and leaving it out reports that column narrower "
                  "than the document. BUILD the range form of §5.1's pair — core/layout/table_box.h already "
                  "carries the anonymous cell's `[first, end)` over its row's child list and "
                  "core/layout/block_flow.c already delimits exactly such runs for §9.2.1.1 \"Anonymous block "
                  "boxes\", so what is missing is an intrinsic-size entry taking a run rather than an element, "
                  "and core/layout/table_grid.h must then carry the range beside the element it now stores as "
                  "NULL");
        DCHECK(cell->col < grid->ncols,
               "CSS 2.1 §17.5.2.2's step 2 found a cell anchored outside the grid's own column count. "
               "core/layout/table_grid.h grows `ncols` to cover every cell it places, so a column index at or "
               "past it is a grid whose cell array and column count were carried apart");
        cw = tcw_cell_widths(cell->element);
        cols[cell->col].min = css_px_max(cols[cell->col].min, cw.min);
        cols[cell->col].max = css_px_max(cols[cell->col].max, cw.max);
    }
    /* ---- §17.5.2.2's STEP 3 ---------------------------------------------------------------------------
       "For each cell that spans more than one column, increase the minimum widths of the columns it spans so
       that together, they are at least as wide as the cell. Do the same for the maximum widths. If possible,
       widen all spanned columns by approximately the same amount."
       THE EQUAL SPLIT IS THE SECTION'S OWN SUGGESTION AND IS RECORDED RATHER THAN CRASHED ON: "approximately
       the same amount" names a family of distributions and not one, and the section is non-normative from
       "The remainder of this section is non-normative" onward, so a crash here would refuse a document every
       reading is conforming for. The deficit is split evenly across the spanned columns.
       THE ORDER IS THE GRID'S OWN AND IS ALSO A RECORDED CHOICE. Two spanning cells over overlapping column
       runs do not commute — widening for the first changes the sum the second measures — and §17.5.2.2 says
       nothing about the order. core/layout/table_grid.h reports cells by grid row and, within a row, in the
       order the row's cells occur in its child list; this walk takes them in that order, so the answer is a
       function of the document rather than of an allocation. */
    for (k = 0; k < grid->ncells; k++) {
        const TableGridCell *cell = &grid->cells[k];
        TableColumnWidth cw;
        CssPx sum_min, sum_max;
        size_t c;

        if (cell->colspan == 1) continue;
        if (cell->element == NULL)
            DFAIL("CSS 2.1 §17.5.2.2 Automatic table layout's step 3 reached an ANONYMOUS 'table-cell' box. "
                  "§17.2.1 Anonymous table objects generates one around content a row did not wrap and such a "
                  "cell is always 1x1 (core/layout/table_grid.h), so a colspan above one on an element-less "
                  "cell is that grid having placed a span it read from no element");
        DCHECK(cell->col + cell->colspan <= grid->ncols,
               "CSS 2.1 §17.5.2.2's step 3 found a cell spanning past the grid's own column count. "
               "core/layout/table_grid.h grows `ncols` to `col + colspan` for every cell it places, so this is "
               "a grid whose cell array and column count were carried apart");
        cw = tcw_cell_widths(cell->element);
        sum_min = css_px(0.0);
        sum_max = css_px(0.0);
        for (c = cell->col; c < cell->col + cell->colspan; c++) {
            sum_min = css_px_add(sum_min, cols[c].min);
            sum_max = css_px_add(sum_max, cols[c].max);
        }
        if (cw.min.px > sum_min.px) {
            CssPx share = css_px_scale(css_px_sub(cw.min, sum_min), 1.0 / (double) cell->colspan);

            for (c = cell->col; c < cell->col + cell->colspan; c++)
                cols[c].min = css_px_add(cols[c].min, share);
        }
        if (cw.max.px > sum_max.px) {
            CssPx share = css_px_scale(css_px_sub(cw.max, sum_max), 1.0 / (double) cell->colspan);

            for (c = cell->col; c < cell->col + cell->colspan; c++)
                cols[c].max = css_px_add(cols[c].max, share);
        }
    }
    /* ---- §17.5.2.2's STEP 4 is the probe above's other subject, and there is nothing left for it to do here:
       a column group with `width: auto` floors nothing, and any other value stopped this walk before a column
       was measured. ---------------------------------------------------------------------------------------
       ---- "This gives a maximum and minimum width for each column." ------------------------------------- */
    for (i = 0; i < grid->ncols; i++) {
        /* The second of the two crossings the header records: step 3 widens the minima and the maxima over
           the same columns from two independent sums, so a column can come out of it with a minimum above a
           maximum that no cell of its own ever raised. */
        cols[i].max = tcw_coherent(cols[i].min, cols[i].max);
        DCHECK(cols[i].min.px >= 0.0 && cols[i].max.px >= cols[i].min.px,
               "CSS 2.1 §17.5.2.2's pair left this component INCOHERENT — a negative minimum, or a maximum "
               "below the minimum. Every term summed into either is a non-negative intrinsic size, padding or "
               "border width, and the maximum is floored at the minimum on the line above, so this is "
               "arithmetic that lost a sign rather than a document. It is asserted at the BOUNDARY because the "
               "final rules of §17.5.2.2 Automatic table layout use MIN and MAX as two ends of one comparison "
               "against the containing block, and a pair that crossed would put the table's own width on the "
               "wrong side of it with nothing downstream to say so");
    }
    *out = cols;
    return grid->ncols;
}
