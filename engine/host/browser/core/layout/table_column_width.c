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
#include "core/layout/table_border_collapse.h"
#include "core/layout/table_box.h"
#include "core/layout/table_column_box.h"
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
 * BOTH OF §17.6 Borders' MODELS ARE ANSWERED HERE, AND THE ASK IS THIS FUNCTION'S. Under CSS 2.1 §17.6.1 The
 * separated borders model "each cell has an individual border" and the sum is the cell's own computed
 * `border-*-width`. Under CSS 2.1 §17.6.2 The collapsing border model it is not: that section centres each
 * border on the grid line between two cells ("Borders are centered on the grid lines between the cells"), so
 * the border at an edge is §17.6.2.1 Border conflict resolution's winner among the boxes meeting there and the
 * cell carries HALF of it — core/layout/table_border_collapse.h owns that whole reading, including why a cell
 * at the table's perimeter carries NONE of the border there. The ask is here rather than at the caller because
 * both of §17.5.2's algorithms and core/layout/used_value.c's cell content width read this ONE sum, and a
 * caller-side dispatch would be that routing decision written three times over a difference none of them can
 * see: both answers are a real width of a real box. */
CssPx table_cell_border_edges(lxb_dom_element_t *cell)
{
    static const char *const PADDINGS[2] = { "padding-left", "padding-right" };
    static const char *const BORDERS[2] = { "border-left-width", "border-right-width" };
    CssPx edges = css_px(0.0);
    lxb_dom_element_t *table;
    bool collapsing;
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
    /* §17.6's `border-collapse` is `Inherited: yes` and its Applies-to line is "'table' and 'inline-table'
       elements", so the model is a fact about the TABLE and one ask covers both of this cell's sides. */
    table = table_box_table_of(cell);
    collapsing = table_border_collapse_selected(table);
    for (i = 0; i < 2; i++) {
        CssLength pad = css_computed_length(cell, PADDINGS[i]);
        CssLength bor;

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
        edges = css_px_add(edges, pad.px);
        /* §17.6.2 replaces this cell's OWN border with the resolution at the grid line, so the two sides are
           added together below rather than one per iteration — there is no per-side answer to read here. */
        if (collapsing) continue;
        bor = css_computed_length(cell, BORDERS[i]);
        DCHECK(bor.kind == CSS_LENGTH_ABSOLUTE,
               "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 "
               "§3.3 \"Line Thickness: the border-width properties\"' `Computed value:` line is `absolute "
               "length, "
               "snapped as a border width` and every arm of that derivation produces one, so a percentage or a "
               "keyword here is a rule that did not run");
        edges = css_px_add(edges, bor.px);
    }
    if (collapsing) {
        /* §17.6.2's charge at the two vertical grid lines this cell abuts — half the resolution at each, and
           zero at one on the table's perimeter, which core/layout/table_border_collapse.h owns whole. */
        TableCollapsedEdges charge = table_collapsed_cell_edges(cell);

        edges = css_px_add(edges, css_px_add(charge.left, charge.right));
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
    edges = table_cell_border_edges(cell);
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

/* §17.5.2.2's STEPS 2 AND 4 EACH READ A DECLARED `width` OFF A COLUMN OR COLUMN-GROUP BOX, and both are
   FLOORS. Step 2 takes a column's minimum as "The minimum is that required by the cell with the largest minimum
   cell width (or the column 'width', whichever is larger)." and its maximum by the matching sentence; step 4 is
   "For each column group element with a 'width' other than 'auto', increase the minimum widths of the columns
   it spans, so that together they are at least as wide as the column group's 'width'." Omitting either reports
   a column NARROWER than the document asks for, and that number travels into a used width and out through
   CSSOM VIEW as a rectangle no reader can distinguish from a measured one.
   THE BOX IS core/layout/table_column_box.h's AND THE PROPERTY IS THIS FILE'S — that component answers WHICH
   box occupies a grid column and reads nothing off it, because §17.5.2.1 Fixed table layout and CSS 2.1
   §17.6.2.1 Border conflict resolution ask the same walk for different properties of the same boxes.
   WHICH BOX THE NUMBER IS A WIDTH OF is settled by §17.5's own closing paragraph rather than assumed: "In the
   separated borders model, the edges coincide with the border edges of cells." A column's edges are therefore
   its cells' BORDER edges, which is the box `TableColumnWidth` is already measured in (see the header), so the
   declared width floors the pair directly and needs no conversion.
   THE COLLAPSING MODEL MOVES THOSE EDGES AND THE FLOOR IS STILL APPLIED AS WRITTEN, WHICH IS RECORDED RATHER
   THAN DERIVED. The same paragraph's first sentence gives them elsewhere — "The edges of the rows, columns, row
   groups and column groups in the collapsing borders model coincide with the hypothetical grid lines on which
   the borders of the cells are centered." — and a cell's border box under CSS 2.1 §17.6.2 The collapsing border
   model reaches exactly those grid lines at every INTERIOR one, since it carries half of a border centred
   there. The two differ only at the table's two OUTERMOST lines, by the half-border §17.6.2 gives to the TABLE
   box ("The left border width of the table is half of the first cell's collapsed left border"), and CSS 2.1
   states no rule reconciling a declared column width with that half. So the floor is taken on the same pair in
   both models, and the difference is a length the table box already carries rather than one dropped here. */
typedef struct {
    bool  declared;   /* the computed `width` is other than `auto` — step 4's own antecedent */
    CssPx px;         /* meaningless unless `declared` */
} TcwDeclaredWidth;

static TcwDeclaredWidth tcw_declared_width(lxb_dom_element_t *box)
{
    TcwDeclaredWidth out;
    CssLength w;
    char nbuf[160];

    out.declared = false;
    out.px = css_px(0.0);
    DCHECK(css_property_applies(box, "width"),
           "CSS 2.1 §17.5.2.2 Automatic table layout is reading a declared `width` off a column or column-group "
           "box and CSS 2.1 §10.2 \"Content width: the 'width' property\"' Applies-to line now excludes it. "
           "That line is \"all elements but non-replaced inline elements, table rows, and row groups\", which "
           "names the ROW boxes and not the column ones, so a disagreement here is that component and this "
           "step having been carried apart — and the cost is silent, because a column whose floor is dropped "
           "is a column that is merely narrow");
    w = css_computed_length(box, "width");
    if (w.kind == CSS_LENGTH_KEYWORD && strcmp(w.keyword, "auto") == 0) return out;
    /* EVERY REFUSAL BELOW LEAVES `declared` FALSE, AND THAT IS THIS DIFF'S LINE RATHER THAN DECORATION.
       core/layout/used_value.c already routes a table's used width through this component in RELEASE, where a
       `DFAILF` compiles to nothing — so a refusal that fell through to the assignment at the end would report
       `declared` with a `px` that is not the width (a `CssLength`'s `px` is the ABSOLUTE arm's field; the
       percentage arm carries `pct`), and a fabricated column floor is indistinguishable downstream from a
       measured one. Falling out with `declared` false drops the floor instead, which is the same release
       behaviour every declared column width had before this walk existed: narrower than the document, and
       never a number the document does not contain. The header's own note that these sites become CLAUDE.md's
       data-integrity case once wired is about the SEVERITY of the whole family, padding included, and is not
       split here for one member of it. */
    if (w.kind == CSS_LENGTH_PERCENTAGE || w.kind == CSS_LENGTH_CALCULATED) {
        DFAILF("%s: a column or column-group box declares a PERCENTAGE `width` and CSS 2.1 §17.5.2.2 Automatic "
               "table layout cannot resolve it. The section refers it to the one number this algorithm is being "
               "run to produce — \"A percentage value for a column width is relative to the table width.\" — and "
               "CSS 2.1 §10.2 \"Content width: the 'width' property\" names exactly this case as one it does "
               "not define: \"If the containing block's width depends on this element's width, then the "
               "resulting layout is undefined in CSS 2.1.\" So the resolution is a CYCLE and not a missing "
               "lookup, and §17.5.2.2 offers only \"If the table has 'width: auto', a percentage represents a "
               "constraint on the column's width, which a UA should try to satisfy.\" BUILD the fixed point the "
               "way this file's percentage-padding crash already names — run the four steps with the "
               "percentage terms at zero, derive the table width from them, resolve against it and re-run — "
               "and RECORD that as the reading taken, because §17.5.2.2 is non-normative from \"The remainder "
               "of this section is non-normative.\" onward and offers no other. A `calc()` reaches this same "
               "crash and for the same reason whenever a percentage is inside it",
               box_subject(box, nbuf, sizeof nbuf));
        return out;
    }
    if (w.kind != CSS_LENGTH_ABSOLUTE) {
        DFAILF("%s: a column or column-group box's computed `width` is a keyword that is neither `auto` nor a "
               "length. CSS 2.1 §10.2 \"Content width: the 'width' property\"' Computed value line is \"the "
               "percentage or 'auto' as specified or the absolute length\", so every arm of that derivation "
               "produces one of three things and this is a fourth — a rule that did not run, not a value the "
               "cascade can hand back",
               box_subject(box, nbuf, sizeof nbuf));
        return out;
    }
    DCHECK(w.px.px >= 0.0,
           "a column or column-group box's computed `width` is NEGATIVE, which CSS 2.1 §10.2 \"Content width: "
           "the 'width' property\" states outright is not a value: \"Negative values for 'width' are illegal.\" "
           "lexbor drops such a declaration, so this is arithmetic that lost a sign rather than a document");
    out.declared = true;
    out.px = w.px;
    return out;
}

/* §17.5.2.2's STEP 4 DEFICIT, and step 3's own sentence is the only guidance either step gives. Step 4 says
   what the columns must reach and nothing about how they get there; step 3, which is the same shape over a
   spanning CELL, says "If possible, widen all spanned columns by approximately the same amount." That names a
   FAMILY of distributions and not one, and §17.5.2.2 is non-normative from "The remainder of this section is
   non-normative." onward, so an even split is RECORDED as the reading taken rather than crashed on — a crash
   here would refuse a document every reading is conforming for. It is the same split step 3 above already
   takes, which is what keeps one table from being widened two ways. */
static void tcw_raise_run_minimum(TableColumnWidth *cols, size_t from, size_t to, CssPx want)
{
    CssPx sum = css_px(0.0);
    size_t c;

    DCHECK(to > from, "CSS 2.1 §17.5.2.2's step 4 was asked to widen an EMPTY run of columns. A column group is "
                      "only reached by the walk below at a column it occupies, so an empty run is that walk "
                      "having advanced past its own start");
    for (c = from; c < to; c++) sum = css_px_add(sum, cols[c].min);
    if (want.px <= sum.px) return;
    {
        CssPx share = css_px_scale(css_px_sub(want, sum), 1.0 / (double) (to - from));

        for (c = from; c < to; c++) cols[c].min = css_px_add(cols[c].min, share);
    }
}

/* THE COLUMN BOXES A DOCUMENT DECLARES CAN OUTNUMBER THE GRID COLUMNS ITS CELLS FILL, AND THE DIFFERENCE IS
   NOT THIS COMPONENT'S TO ABSORB. HTML §4.9.12.1 Forming a table increases the table's own x_width by every
   column group's span before a single row is read, so `<colgroup span="5">` over two-cell rows is a FIVE-column
   table there, while core/layout/table_grid.h grows `ncols` only to cover the cells it places and reports two.
   The difference is invisible while every extra column is `width: auto` — such a column floors nothing and
   contributes nothing to MIN or MAX — and is a WRONG table width the moment one of them declares a width, so
   that is exactly where this stops. */
static void tcw_require_columns_within_grid(const TableColumnBoxMap *map, size_t ngrid)
{
    size_t i;
    char nbuf[160];

    for (i = ngrid; i < map->ncols; i++) {
        lxb_dom_element_t *box = (map->cols[i].column != NULL) ? map->cols[i].column : map->cols[i].column_group;

        if (box == NULL) continue;
        if (!tcw_declared_width(box).declared) continue;
        DFAILF("%s: CSS 2.1 §17.5.2.2 Automatic table layout found a column or column-group box declaring a "
               "`width` for grid column %zu of a table core/layout/table_grid.h reports as %zu columns wide. "
               "That box's floor covers a column no cell of this table occupies, so the floor is DROPPED and "
               "the table comes out narrower than a browser lays it out — CSS 2.1 §17.5's rule 3 gives the box "
               "those grid columns (\"A column box occupies one or more columns of grid cells. Column boxes are "
               "placed next to each other in the order they occur.\") whether or not a cell ever reaches them. "
               "BUILD HTML §4.9.12.1 Forming a table's COLUMN-GROUP CONTRIBUTION TO x_width in "
               "core/layout/table_grid.c, whose `ncols` today is only ever grown to `col + colspan` for a cell "
               "it places: that algorithm walks the `colgroup` children first and increases x_width by each "
               "span before it reaches a row, so the grid's column count must start at "
               "core/layout/table_column_box.h's `noccupied` rather than at zero",
               box_subject(box, nbuf, sizeof nbuf), i, ngrid);
    }
}

size_t table_column_widths(lxb_dom_element_t *table, const TableGrid *grid, TableColumnWidth **out)
{
    TableColumnWidth *cols;
    TableColumnBoxMap boxes;
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
    /* NO BORDER-MODEL ASSERT STANDS HERE ANY LONGER, AND ITS REMOVAL IS THE POINT RATHER THAN A RELAXATION.
       What stood here refused CSS 2.1 §17.6.2 The collapsing border model on the ground that
       `table_cell_border_edges` sums each cell's own computed `border-*-width` — and that function now answers
       BOTH of §17.6 Borders' models at its own site, so these four steps are stated over a border box that is
       right in either one and there is nothing left for a caller to have got wrong. A predicate kept past the
       thing it selected against is a fallback with no second arm to fall back to. */
    *out = NULL;
    /* §17.5's rules 3 and 4, BUILT BEFORE THE ZERO-COLUMN RETURN BELOW AND NOT AFTER IT. A table whose rows
       generate no cell still has the column boxes its own child list declares, and `<table><colgroup span="3"
       style="width: 100px"></table>` is exactly the shape whose floor the early return would drop in silence —
       the guard is about columns the GRID does not have, so a grid with none of them is its sharpest case. */
    table_column_boxes_build(table, grid->ncols, &boxes);
    tcw_require_columns_within_grid(&boxes, grid->ncols);
    if (grid->ncols == 0) {
        table_column_boxes_release(&boxes);
        return 0;
    }
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
       per-column form would re-scan every cell for each column. The column `width` half of the sentence is the
       loop AFTER this one, and it is after rather than merged into it because the two halves have different
       operands — this one walks CELLS and that one walks COLUMNS — while `css_px_max` makes their order
       immaterial to the answer.
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
    /* ---- §17.5.2.2's STEP 2, SECOND HALF: THE COLUMN'S OWN `width` -------------------------------------
       "The minimum is that required by the cell with the largest minimum cell width (or the column 'width',
       whichever is larger)." and "The maximum is that required by the cell with the largest maximum cell width
       (or the column 'width', whichever is larger)." The parenthesis is the SAME on both lines, so one declared
       width floors BOTH ends of the pair — which is what separates it from step 4's group width below, and from
       a cell's own declared `width`, each of which the section writes over the minimum alone.
       A COLUMN THE GRID HAS AND NO COLUMN BOX OCCUPIES IS THE ORDINARY CASE and is not asked: a `<col>` styling
       the first of three columns leaves the other two with a NULL entry, which core/layout/table_column_box.h
       states is the positive answer "no column box occupies this one" rather than a value to default past. */
    for (i = 0; i < grid->ncols; i++) {
        TcwDeclaredWidth w;

        if (boxes.cols[i].column == NULL) continue;
        w = tcw_declared_width(boxes.cols[i].column);
        if (!w.declared) continue;
        cols[i].min = css_px_max(cols[i].min, w.px);
        cols[i].max = css_px_max(cols[i].max, w.px);
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
    /* ---- §17.5.2.2's STEP 4 ---------------------------------------------------------------------------
       "For each column group element with a 'width' other than 'auto', increase the minimum widths of the
       columns it spans, so that together they are at least as wide as the column group's 'width'."
       THE MINIMA ONLY, AND THAT ASYMMETRY IS THE SECTION'S. Step 2 floors both ends from a column's `width` and
       step 3 widens both from a spanning cell; step 4 names the minimum alone and says nothing of the maximum,
       so raising the maximum here would be this engine adding a term. The coherence floor below is what then
       lifts a maximum that the widened minimum crossed, which is the same crossing the header already records
       for step 3 and is handled in the one place rather than at each step.
       THE GROUP'S RUN IS READ OFF THE MAPPING RATHER THAN RE-WALKED, which is rule 4 held as a fact instead of
       restated as a loop: "A column group box occupies the same grid cells as the columns it contains", so the
       columns a group spans are exactly the consecutive entries naming it and a run scan finds them without
       asking the DOM a second time. A run truncated by the grid's own column count cannot carry a declared
       width — `tcw_require_columns_within_grid` stopped this walk above if one did — so a short run here is a
       group whose `width` is `auto`, which floors nothing. */
    for (i = 0; i < grid->ncols; ) {
        lxb_dom_element_t *group = boxes.cols[i].column_group;
        size_t end = i + 1;
        TcwDeclaredWidth w;

        if (group == NULL) { i = end; continue; }
        while (end < grid->ncols && boxes.cols[end].column_group == group) end++;
        w = tcw_declared_width(group);
        if (w.declared) tcw_raise_run_minimum(cols, i, end, w.px);
        i = end;
    }
    /* ---- "This gives a maximum and minimum width for each column." ------------------------------------- */
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
    table_column_boxes_release(&boxes);
    *out = cols;
    return grid->ncols;
}
