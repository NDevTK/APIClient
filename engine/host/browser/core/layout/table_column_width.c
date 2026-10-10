/* CSS 2.1 §17.5.2.2 Automatic table layout's four column-width steps, over core/layout/table_grid.h's grid.
   See table_column_width.h. */
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

/* The coherence floor: a maximum below a minimum is not a width the column can take (see the header for the
   two crossings). Arithmetic only; the invariant is asserted where the pair leaves this component. */
static CssPx tcw_coherent(CssPx min, CssPx max)
{
    return css_px_max(max, min);
}

/* One cell's horizontal padding and border; see the header.
 *
 * It is not core/layout/used_value.c's `uv_surround`: that resolves a percentage padding against CSS 2.1 §10.1
 * Definition of "containing block"'s width, which for a cell is the table's content width this algorithm is
 * producing. Here a percentage is a cycle and crashes.
 *
 * §10.4 Minimum and maximum widths: 'min-width' and 'max-width' is not read: "In CSS 2.1, the effect of
 * 'min-width' and 'max-width' on tables, inline tables, table cells, table columns, and column groups is
 * undefined."
 *
 * Under §17.6.2 "Borders are centered on the grid lines between the cells", so the cell carries half of the
 * resolved border at each edge (core/layout/table_border_collapse.h). The dispatch is here because three
 * consumers read this one sum. */
CssPx table_cell_border_edges(lxb_dom_element_t *cell)
{
    static const char *const PADDINGS[2] = { "padding-left", "padding-right" };
    static const char *const BORDERS[2] = { "border-left-width", "border-right-width" };
    CssPx edges = css_px(0.0);
    lxb_dom_element_t *table;
    bool collapsing;
    char nbuf[160];
    int i;

    /* The outer size is the border box only because margins do not apply to a cell (CSS 2.1 §8.3 "Margin
       properties"); asked of the component that owns that line. */
    DCHECK(!css_property_applies(cell, "margin-left") && !css_property_applies(cell, "margin-right"),
           "CSS 2.1 §17.5.2.2 Automatic table layout took a cell's OUTER SIZE to be its border box, and CSS 2.1 "
           "§8.3 \"Margin properties\"' Applies-to line now says the horizontal margins DO apply to this box — "
           "so css-sizing-3 §2.2 \"Intrinsic Size Contributions\"' outer size has a term this walk is not "
           "adding and every column the box occupies is reported narrower than its content");
    /* §17.6's `border-collapse` applies to "'table' and 'inline-table' elements", so the model is the
       table's and one ask covers both sides. */
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
        /* §17.6.2 replaces the cell's own border with the grid-line resolution, added below for both sides. */
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
        /* §17.6.2's charge at the two vertical grid lines this cell abuts: half the resolution at each, zero on
           the table's perimeter (core/layout/table_border_collapse.h). */
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

/* §17.5.2.2's step 1 for one cell, in the border box:
     "Calculate the minimum content width (MCW) of each cell: the formatted content may span any number of
      lines but may not overflow the cell box. If the specified 'width' (W) of the cell is greater than MCW, W
      is the minimum cell width. A value of 'auto' means that MCW is the minimum cell width."
     "Also, calculate the \"maximum\" cell width of each cell: formatting the content without breaking lines
      other than where explicit line breaks occur."
   MCW and the maximum are css-sizing-3 §5.1 "Intrinsic Sizes"' pair (§2.1 "Auto Box Sizes": all versus none
   of the soft wrap opportunities taken). core/layout/intrinsic_size.h answers both in the content box. */
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
    /* The specified `width`: css-sizing-3 §3.1.1 "Preferred Size Properties: the width and height
       properties" computes it "as specified, with <length-percentage> values computed", the operand this
       step wants. */
    DCHECK(css_property_applies(cell, "width"),
           "CSS 2.1 §17.5.2.2's step 1 read a `width` on a box that property does not apply to. CSS 2.1 §10.2 "
           "\"Content width: the width property\"' Applies-to line excludes table rows and row groups and no "
           "other table box, so a cell reaching this is a `display` the grid placed as a cell and the cascade "
           "classifies otherwise");
    w = css_computed_length(cell, "width");
    /* A `calc()` takes this arm: css-values-4 §10.1 "Basic Arithmetic: calc()" may mix in a percentage, and
       its basis is this algorithm's own output either way. */
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
    /* Every other computed shape crashes rather than reading as `auto`: `auto` drops the step's floor, so
       misreading a declaration as `auto` reports the column narrower than its content. */
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

        /* css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property": under `border-box` the declared
           length is the border box's, so the content-box operand is that length less padding and border,
           floored at zero. Paraphrased on purpose: the section's editions word the floor differently, and
           engine/citegen.mjs checks quotations against one edition's corpus. */
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
    /* First crossing: W raised the minimum and left the maximum where the content put it. */
    out.max = tcw_coherent(out.min, out.max);
    return out;
}

/* §17.5.2.2's steps 2 and 4 read a declared `width` off a column or column-group box, and both are floors.
   Step 2: "The minimum is that required by the cell with the largest minimum cell width (or the column
   'width', whichever is larger)." and likewise the maximum. Step 4: "For each column group element with a
   'width' other than 'auto', increase the minimum widths of the columns it spans, so that together they are at
   least as wide as the column group's 'width'." core/layout/table_column_box.h finds the box; this file reads
   the property.
   The declared width floors the border-box pair directly: "In the separated borders model, the edges coincide
   with the border edges of cells." In the collapsing model "The edges of the rows, columns, row groups and
   column groups in the collapsing borders model coincide with the hypothetical grid lines on which the borders
   of the cells are centered", which a cell's border box reaches at every interior line; the outermost lines
   differ by the half-border §17.6.2 gives the table box, for which CSS 2.1 states no reconciliation, so the
   same floor is taken in both models. */
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
    /* Every refusal below leaves `declared` false. core/layout/used_value.c reaches this in release, where
       `DFAILF` compiles out; falling through would report `px` from the wrong `CssLength` arm (a percentage
       carries `pct`). Dropping the floor makes the column narrower but never invents a number. */
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

/* §17.5.2.2's step 4 deficit. Step 4 says what the columns must reach, not how; step 3's same-shaped rule
   says "If possible, widen all spanned columns by approximately the same amount." The section is
   non-normative there, so an even split is the recorded reading, the same one step 3 takes. */
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

/* Column boxes can outnumber the grid columns cells fill: HTML §4.9.12.1 Forming a table grows x_width by
   every column group's span, so `<colgroup span="5">` over two-cell rows is five columns wide there, while
   core/layout/table_grid.h reports two. An extra column with `width: auto` contributes nothing; one declaring
   a width would make the table width wrong, so that is refused. */
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
    /* No border-model assert: `table_cell_border_edges` answers both of §17.6's models, so these steps are
       right in either. */
    *out = NULL;
    /* §17.5's rules 3 and 4 are built before the zero-column return, so a cell-less table's declared column
       boxes (`<table><colgroup span="3" style="width: 100px"></table>`) still reach the guard. */
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
    /* ---- §17.5.2.2's step 2 ---------------------------------------------------------------------------
       "For each column, determine a maximum and minimum column width from the cells that span only that
       column." Run per cell rather than per column: each single-column cell lands in its one column. The
       column `width` half is the next loop; `css_px_max` makes the order immaterial.
       §17.5's rule 5 overlap needs no arm: under the reading core/layout/table_grid.h records for `overlaps`,
       the cell occupies the columns it covers and contributes to them like any other. */
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
    /* ---- §17.5.2.2's step 2, second half: the column's own `width` -------------------------------------
       The parenthesis "(or the column 'width', whichever is larger)" is on both the minimum and maximum line,
       so a column's declared width floors both ends, unlike step 4's group width and a cell's own `width`.
       A grid column no column box occupies has a NULL entry, the positive answer "none". */
    for (i = 0; i < grid->ncols; i++) {
        TcwDeclaredWidth w;

        if (boxes.cols[i].column == NULL) continue;
        w = tcw_declared_width(boxes.cols[i].column);
        if (!w.declared) continue;
        cols[i].min = css_px_max(cols[i].min, w.px);
        cols[i].max = css_px_max(cols[i].max, w.px);
    }
    /* ---- §17.5.2.2's step 3 ---------------------------------------------------------------------------
       "For each cell that spans more than one column, increase the minimum widths of the columns it spans so
       that together, they are at least as wide as the cell. Do the same for the maximum widths. If possible,
       widen all spanned columns by approximately the same amount."
       The even split and the order are recorded readings (the section is non-normative here). Overlapping
       spans do not commute, so cells are taken in core/layout/table_grid.h's order (by row, then child-list
       order), a function of the document. */
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
    /* ---- §17.5.2.2's step 4 ---------------------------------------------------------------------------
       "For each column group element with a 'width' other than 'auto', increase the minimum widths of the
       columns it spans, so that together they are at least as wide as the column group's 'width'."
       Minima only, as the section says; the coherence floor below lifts a crossed maximum. A group's run is
       the consecutive mapping entries naming it (rule 4: "A column group box occupies the same grid cells as
       the columns it contains"). A run cut short by the grid's count has `width: auto`, since
       `tcw_require_columns_within_grid` refused any declared one. */
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
        /* Second crossing: steps 3 and 4 widen minima and maxima from independent sums. */
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
